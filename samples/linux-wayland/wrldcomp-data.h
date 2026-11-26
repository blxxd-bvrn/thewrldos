/* The clipboard and drag and drop for wrldcomp (included by wrldcomp.c): wl_data_device.
 *  - Copy in a program (its selection) goes to THE WRLD OS's clipboard, so it can be pasted in
 *    Notepad, WordPad, text boxes or the Command Prompt; what is copied there is offered back as
 *    the selection, so Ctrl+V in the browser pastes it.
 *  - Drag and drop inside a program (text, links, pictures, HTML5 drag and drop): the drag's
 *    source is offered to the surface under the pointer, which receives it on the drop.
 *  - Files dropped on the window from Explorer or the desktop (WRLDWIN_EV_DROP) arrive as a
 *    text/uri-list drop, which a web page takes as files (an upload) and the browser as links. */
struct dsrc { struct wl_resource *res; struct wl_array mimes; uint32_t actions; };
struct doffer {
    struct wl_resource *res;
    struct dsrc *src;                        /* the program's own source, or NULL: THE WRLD OS's data */
    char *text; size_t len;                  /* that data (a file list); NULL: the clipboard, read when asked */
    int for_drag;
};
static struct wl_list devices;               /* wl_data_device resources */
static struct dsrc *sel_src;                 /* the program's current selection (its copy) */
static struct wl_resource *sel_res;          /* its resource, to notice when it goes */
static uint32_t clip_seen = (uint32_t)-1;    /* the OS clipboard generation the programs were last given */
static int clip_fd = -1;                     /* /dev/wrld/window, for the clipboard requests */
static struct dsrc *drag_src;
static int drag_active;
static struct wl_client *drag_client;
static struct surface *drag_focus;
static struct doffer *drag_offer;
static struct wl_resource *drag_dev;         /* the device the current drag offer went to */

static struct wl_event_source *drop_timer;
static int os_drop_pending;                  /* a drop from THE WRLD OS waits for the program to accept */
static uint32_t os_drop_action;
static char os_drop_mime;
static void os_drop_check(void);
static int clipfd(void) { if (clip_fd < 0) clip_fd = open("/dev/wrld/window", O_RDWR | O_CLOEXEC); return clip_fd; }
static int is_text_mime(const char *m) {
    return !strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "text/plain") || !strcmp(m, "UTF8_STRING") || !strcmp(m, "TEXT") || !strcmp(m, "STRING");
}

/* ---- data sources (a program's copy or drag) ---- */
static void ds_offer(struct wl_client *c, struct wl_resource *r, const char *m) {
    (void)c; struct dsrc *s = wl_resource_get_user_data(r);
    char **slot = wl_array_add(&s->mimes, sizeof(char *)); if (slot) *slot = strdup(m);
}
static void ds_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ds_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; struct dsrc *s = wl_resource_get_user_data(r); s->actions = a; }
static const struct wl_data_source_interface data_source_impl = { ds_offer, ds_destroy, ds_actions };
static void source_gone(struct wl_resource *r) {
    struct dsrc *s = wl_resource_get_user_data(r);
    if (sel_src == s) { sel_src = NULL; sel_res = NULL; }
    if (drag_src == s) drag_src = NULL;
    if (drag_offer && drag_offer->src == s) drag_offer->src = NULL;
    char **m; wl_array_for_each(m, &s->mimes) free(*m);
    wl_array_release(&s->mimes);
    free(s);
}
static void dm_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct dsrc *s = calloc(1, sizeof *s);
    wl_array_init(&s->mimes);
    s->res = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &data_source_impl, s, source_gone);
}

/* ---- offers ---- */
static void write_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t k = write(fd, p, n); if (k <= 0) break; p += k; n -= (size_t)k; }
}
static void of_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *m) {
    (void)c; (void)serial; struct doffer *o = wl_resource_get_user_data(r);
    LOG("offer accept %s", m ? m : "(none)");
    if (o == drag_offer && !o->src) { os_drop_mime = m != NULL; os_drop_check(); }
    if (o->src && o->for_drag) wl_data_source_send_target(o->src->res, m);
}
static void of_receive(struct wl_client *c, struct wl_resource *r, const char *m, int32_t fd) {
    (void)c; struct doffer *o = wl_resource_get_user_data(r);
    if (o->src) { wl_data_source_send_send(o->src->res, m, fd); close(fd); return; }   /* the program's own data */
    LOG("receive %s: %s", m, o->text ? "files" : "clipboard");
    if (o->text) write_all(fd, o->text, o->len);
    else if (clipfd() >= 0) {                                 /* the OS clipboard, as it is now */
        size_t cap = 1 << 20; char *b = malloc(cap);
        struct wrldwin_clip q = { (uint64_t)(uintptr_t)b, (uint32_t)cap, 0 };
        long n = b ? ioctl(clip_fd, WRLDWIN_CLIP_GET, &q) : -1;
        if (n > 0) write_all(fd, b, (size_t)(n < (long)cap ? n : (long)cap));
        free(b);
    }
    close(fd);
}
static void drag_end(void);
static void of_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void of_finish(struct wl_client *c, struct wl_resource *r) {
    (void)c; struct doffer *o = wl_resource_get_user_data(r);
    if (o->src && wl_resource_get_version(o->src->res) >= 3) wl_data_source_send_dnd_finished(o->src->res);
    if (o == drag_offer) drag_end();
}
static void of_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions, uint32_t preferred) {
    (void)c; struct doffer *o = wl_resource_get_user_data(r);
    LOG("offer actions %u preferred %u", actions, preferred);
    uint32_t avail = o->src ? (o->src->actions ? o->src->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    uint32_t pick = (preferred & avail & actions) ? preferred : (avail & actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                  : (avail & actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE : 0;
    if (wl_resource_get_version(r) >= 3) wl_data_offer_send_action(r, pick);
    if (o->src && wl_resource_get_version(o->src->res) >= 3) wl_data_source_send_action(o->src->res, pick);
    if (o == drag_offer && !o->src) { os_drop_action = pick; os_drop_check(); }
}
static const struct wl_data_offer_interface data_offer_impl = { of_accept, of_receive, of_destroy, of_finish, of_actions };
static void offer_gone(struct wl_resource *r) {
    struct doffer *o = wl_resource_get_user_data(r);
    if (o == drag_offer) { drag_offer = NULL; }
    free(o->text); free(o);
}
/* a new offer on device `dev` with the source's types (or THE WRLD OS's: text, or a file list) */
static struct doffer *offer_new(struct wl_resource *dev, struct dsrc *src, const char *text, size_t len, int uris) {
    struct doffer *o = calloc(1, sizeof *o);
    o->src = src;
    if (text) { o->text = malloc(len + 1); memcpy(o->text, text, len); o->text[len] = 0; o->len = len; }
    o->res = wl_resource_create(wl_resource_get_client(dev), &wl_data_offer_interface, wl_resource_get_version(dev), 0);
    wl_resource_set_implementation(o->res, &data_offer_impl, o, offer_gone);
    wl_data_device_send_data_offer(dev, o->res);
    if (src) { char **m; wl_array_for_each(m, &src->mimes) wl_data_offer_send_offer(o->res, *m); }
    else if (uris) { wl_data_offer_send_offer(o->res, "text/uri-list"); }
    else { wl_data_offer_send_offer(o->res, "text/plain;charset=utf-8"); wl_data_offer_send_offer(o->res, "text/plain"); wl_data_offer_send_offer(o->res, "UTF8_STRING"); wl_data_offer_send_offer(o->res, "STRING"); wl_data_offer_send_offer(o->res, "TEXT"); }
    return o;
}

/* ---- the selection ---- */
/* the current selection to every device of client c: the program's own copy, or THE WRLD OS's
 * clipboard when there is text on it */
static void selection_to(struct wl_client *c) {
    struct wrldwin_clip q = { 0, 0, 0 };
    long n = clipfd() >= 0 ? ioctl(clip_fd, WRLDWIN_CLIP_GET, &q) : -1;
    clip_seen = q.gen;
    struct wl_resource *dev;
    wl_resource_for_each(dev, &devices) {
        if (wl_resource_get_client(dev) != c) continue;
        if (sel_src) { struct doffer *o = offer_new(dev, sel_src, NULL, 0, 0); wl_data_device_send_selection(dev, o->res); }
        else if (n > 0) { struct doffer *o = offer_new(dev, NULL, NULL, 0, 0); wl_data_device_send_selection(dev, o->res); }
        else wl_data_device_send_selection(dev, NULL);
    }
}
/* reading a program's copy into THE WRLD OS's clipboard */
struct sel_read { int fd; char *buf; size_t len, cap; struct wl_event_source *src; struct dsrc *from; };
static int on_sel_data(int fd, uint32_t mask, void *data) {
    struct sel_read *r = data; (void)mask;
    if (r->len + 65536 > r->cap) { r->cap = r->cap * 2 + 65536; r->buf = realloc(r->buf, r->cap); }
    ssize_t k = read(fd, r->buf + r->len, r->cap - r->len - 1);
    if (k > 0 && r->len + (size_t)k < (4u << 20)) { r->len += (size_t)k; return 0; }
    if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    r->buf[r->len] = 0;
    if (r->len && sel_src == r->from && clipfd() >= 0) {      /* still the selection: it becomes the clipboard */
        struct wrldwin_clip q = { (uint64_t)(uintptr_t)r->buf, (uint32_t)r->len, 0 };
        ioctl(clip_fd, WRLDWIN_CLIP_SET, &q);
        struct wrldwin_clip g = { 0, 0, 0 }; ioctl(clip_fd, WRLDWIN_CLIP_GET, &g); clip_seen = g.gen;
        LOG("clipboard: %zu bytes from the program", r->len);
    }
    wl_event_source_remove(r->src); close(r->fd); free(r->buf); free(r);
    return 0;
}
static void dd_sel(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t serial) {
    (void)c; (void)r; (void)serial;
    sel_src = s ? wl_resource_get_user_data(s) : NULL; sel_res = s;
    if (!sel_src) return;
    const char *mime = NULL; char **m;
    wl_array_for_each(m, &sel_src->mimes) if (!mime && !strcmp(*m, "text/plain;charset=utf-8")) mime = *m;
    wl_array_for_each(m, &sel_src->mimes) if (!mime && is_text_mime(*m)) mime = *m;
    if (!mime) return;                                       /* not text (a picture): it stays in the program */
    int p[2]; if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) return;
    wl_data_source_send_send(sel_src->res, mime, p[1]); close(p[1]);
    struct sel_read *rd = calloc(1, sizeof *rd); rd->fd = p[0]; rd->from = sel_src;
    rd->src = wl_event_loop_add_fd(wl_display_get_event_loop(display), p[0], WL_EVENT_READABLE | WL_EVENT_HANGUP, on_sel_data, rd);
    wl_display_flush_clients(display);
}
/* the OS clipboard changed (something was copied on the desktop): the program with the keyboard hears */
static struct wl_event_source *clip_timer;
static int clip_tick(void *data) {
    (void)data;
    struct wrldwin_clip q = { 0, 0, 0 };
    if (clipfd() >= 0 && ioctl(clip_fd, WRLDWIN_CLIP_GET, &q) >= 0 && q.gen != clip_seen) {
        clip_seen = q.gen;
        sel_src = NULL; sel_res = NULL;                       /* the desktop's copy is newer than the program's */
        if (kbd_focus) selection_to(wl_resource_get_client(kbd_focus->res));
    }
    wl_event_source_timer_update(clip_timer, 500);
    return 0;
}

/* ---- drag and drop ---- */
static void drag_leave_focus(void) {
    if (drag_dev && drag_focus) wl_data_device_send_leave(drag_dev);
    drag_dev = NULL; drag_focus = NULL;
}
static void drag_end(void) {
    drag_active = 0; drag_src = NULL; drag_client = NULL;
    drag_leave_focus();
    drag_offer = NULL;
}
static struct wl_resource *device_of(struct wl_client *c) {
    struct wl_resource *dev;
    wl_resource_for_each(dev, &devices) if (wl_resource_get_client(dev) == c) return dev;
    return NULL;
}
/* the drag moved over (lx, ly) of surface s */
static void drag_motion(struct surface *s, int lx, int ly) {
    if (s != drag_focus) {
        drag_leave_focus();
        if (!s) return;
        struct wl_resource *dev = device_of(wl_resource_get_client(s->res));
        if (!dev) return;
        drag_offer = drag_src ? offer_new(dev, drag_src, NULL, 0, 0) : NULL;
        if (drag_offer) drag_offer->for_drag = 1;
        if (drag_offer && wl_resource_get_version(drag_offer->res) >= 3)
            wl_data_offer_send_source_actions(drag_offer->res, drag_src->actions ? drag_src->actions : WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_data_device_send_enter(dev, next_serial(), s->res, wl_fixed_from_int(lx), wl_fixed_from_int(ly), drag_offer ? drag_offer->res : NULL);
        drag_dev = dev; drag_focus = s;
        return;
    }
    if (drag_dev) wl_data_device_send_motion(drag_dev, now_ms(), wl_fixed_from_int(lx), wl_fixed_from_int(ly));
}
static void drag_drop(void) {
    if (drag_dev && drag_offer) {
        wl_data_device_send_drop(drag_dev);
        if (drag_src && wl_resource_get_version(drag_src->res) >= 3) wl_data_source_send_dnd_drop_performed(drag_src->res);
        drag_active = 0;                                     /* the offer's finish ends it */
        return;
    }
    if (drag_src) wl_data_source_send_cancelled(drag_src->res);
    drag_end();
}
static void dd_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, struct wl_resource *origin, struct wl_resource *icon, uint32_t serial) {
    (void)r; (void)origin; (void)icon; (void)serial;
    drag_src = s ? wl_resource_get_user_data(s) : NULL;
    drag_active = 1; drag_client = c; drag_focus = NULL; drag_dev = NULL; drag_offer = NULL;
    pointer_focus(NULL, 0, 0);                               /* the pointer belongs to the drag now */
}
static int os_drop_x, os_drop_y, os_drop_ticks;
/* while the program decides, the pointer "moves" in place every 100 ms, as it does under a hand:
 * web pages accept a drop in their dragover handler, which runs on pointer motion */
static int drop_later(void *d) {
    (void)d;
    if (!os_drop_pending || !drag_dev || !drag_offer) { os_drop_pending = 0; return 0; }
    if ((os_drop_mime && os_drop_action) || ++os_drop_ticks > 30) {
        wl_data_device_send_drop(drag_dev);
        os_drop_pending = 0;
        return 0;
    }
    wl_data_device_send_motion(drag_dev, now_ms(), wl_fixed_from_int(os_drop_x), wl_fixed_from_int(os_drop_y));
    wl_event_source_timer_update(drop_timer, 100);
    return 0;
}
/* the program said what it takes: once it accepts the files with an action, the drop goes */
static void os_drop_check(void) {
    if (os_drop_pending && os_drop_mime && os_drop_action && drop_timer) wl_event_source_timer_update(drop_timer, 1);
}
/* files dropped on window w at (x, y) from THE WRLD OS: a drop of a text/uri-list */
static void os_drop(struct window *w, int x, int y) {
    char *list = malloc(65536); if (!list) return;
    struct wrldwin_clip q = { (uint64_t)(uintptr_t)list, 65535, 0 };
    long n = ioctl(w->fd, WRLDWIN_DROP_GET, &q);
    if (n <= 0) { free(list); return; }
    if (n > 65535) n = 65535;
    int lx = 0, ly = 0;
    struct surface *s = surface_at(w, x, y, &lx, &ly);
    struct wl_resource *dev = s ? device_of(wl_resource_get_client(s->res)) : NULL;
    if (!dev) { free(list); return; }
    drag_leave_focus();
    drag_offer = offer_new(dev, NULL, list, (size_t)n, 1);
    drag_offer->for_drag = 1;
    free(list);
    if (wl_resource_get_version(drag_offer->res) >= 3) wl_data_offer_send_source_actions(drag_offer->res, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_send_enter(dev, next_serial(), s->res, wl_fixed_from_int(lx), wl_fixed_from_int(ly), drag_offer->res);
    wl_data_device_send_motion(dev, now_ms(), wl_fixed_from_int(lx), wl_fixed_from_int(ly));
    drag_dev = dev; drag_focus = s;
    /* the drop itself a moment later, once the program has looked at the offer and said what it
     * accepts (sent at once, GTK had no drop to finish) */
    if (!drop_timer) drop_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), drop_later, NULL);
    os_drop_pending = 1; os_drop_mime = 0; os_drop_action = 0; os_drop_ticks = 0;
    os_drop_x = lx; os_drop_y = ly;
    wl_event_source_timer_update(drop_timer, 100);           /* dropped after 3 s at the latest: some programs never say */
    LOG("drop: %ld bytes of files: %.*s", n, (int)(n > 300 ? 300 : n), drag_offer->text);
}

/* ---- the device ---- */
static void dd_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_data_device_interface data_device_impl = { dd_drag, dd_sel, dd_release };
static void device_gone(struct wl_resource *r) { if (r == drag_dev) { drag_dev = NULL; drag_focus = NULL; } wl_list_remove(wl_resource_get_link(r)); }
static void dm_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat; struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(d, &data_device_impl, NULL, device_gone);
    wl_list_insert(&devices, wl_resource_get_link(d));
    if (kbd_focus && wl_resource_get_client(kbd_focus->res) == c) selection_to(c);
}
static const struct wl_data_device_manager_interface data_manager_impl = { dm_source, dm_device };
static void bind_data_manager(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
    wl_resource_set_implementation(r, &data_manager_impl, NULL, NULL);
}
static void data_init(struct wl_event_loop *loop) {
    wl_list_init(&devices);
    clip_timer = wl_event_loop_add_timer(loop, clip_tick, NULL);
    wl_event_source_timer_update(clip_timer, 500);
}
