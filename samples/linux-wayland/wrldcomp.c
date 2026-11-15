/* wrldcomp: a small Wayland compositor for THE WRLD OS.
 *
 * Wayland programs (GTK 4, and later WebKitGTK) talk to it over a Unix socket
 * in $XDG_RUNTIME_DIR; every xdg_toplevel becomes a desktop window through
 * /dev/wrld/window. The pixels a client attaches (wl_shm buffers in memfds)
 * are copied at commit, its subsurfaces and popups are composited into the
 * toplevel's window, and the window's mouse, keyboard, resize and close events
 * come back as wl_pointer, wl_keyboard and xdg_toplevel events.
 *
 * Globals: wl_compositor 5, wl_subcompositor, wl_shm (libwayland's), wl_seat 7
 * (pointer, keyboard), wl_output 3, wl_data_device_manager 3 (no clipboard
 * contents yet) and xdg_wm_base 3 (toplevels, popups with positioners).
 * A client that sets a window geometry draws its own decorations: its window
 * is frameless, cropped to the geometry plus a small margin, and its title bar
 * drags and resizes the desktop window. Others get the system frame.
 *
 *   wrldcomp [program [args...]]   runs the program with WAYLAND_DISPLAY set and
 *                                  exits when it does */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"
#include "../linux-gnome/wrldwin.h"

#define MARGIN 8                            /* of a client's shadow kept around its geometry, for resizing */

static struct wl_display *display;
static struct wl_event_loop *loop;
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static int verbose;
#define LOG(...) do { if (verbose) { fprintf(stderr, "wrldcomp: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------------- objects ---------------- */
enum { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_SUB, ROLE_CURSOR };
struct window;
struct xdg;
struct surface {
    struct wl_resource *res;
    struct wl_resource *pend_buf; int pend_attached, pdx, pdy;
    struct wl_list pend_frames, frames;     /* wl_callback resources (link via wl_resource_get_link) */
    uint32_t *pix; int w, h;                /* the committed picture, copied out of the buffer */
    int role;
    struct xdg *xdg;
    struct sub *sub;                        /* when this surface is a subsurface */
    struct wl_list children;                /* struct sub */
    int mapped;
};
struct sub { struct wl_resource *res; struct surface *s, *parent; int x, y, px, py; struct wl_list link; };
struct positioner { int w, h, ax, ay, aw, ah, anchor, gravity, adjust, ox, oy; };
struct xdg {
    struct wl_resource *res, *toplevel, *popup;
    struct surface *s;
    uint32_t serial; int acked, configured;
    int gx, gy, gw, gh, has_geom;
    struct window *win;                     /* toplevel: its desktop window; popup: the window it is drawn in */
    struct xdg *parent;
    int px, py;                             /* popup: position relative to the parent's geometry */
    struct positioner pos;
    int grab;
    struct wl_list link;                    /* popups of a window, bottom first */
    char title[100];
    int min_w, min_h;
};
struct window {
    int fd; uint32_t *px; int w, h;         /* the desktop window and its mapped pixels */
    int frameless, mx, my;                  /* margins around the geometry (frameless) */
    struct xdg *top;
    struct wl_event_source *src;
    struct wl_list popups;
    int want_w, want_h;                     /* a size the user dragged the window to */
    int dirty;
};

/* ---------------- seat ---------------- */
static struct wl_list pointers, keyboards;  /* wl_resource links */
static struct surface *ptr_focus, *kbd_focus;
static uint32_t mods_now;
static char *keymap_text; static size_t keymap_len;
static uint32_t next_serial(void) { return wl_display_next_serial(display); }
static void send_to_client(struct wl_list *list, struct wl_client *c, void (*fn)(struct wl_resource *, void *), void *arg) {
    struct wl_resource *r;
    wl_resource_for_each(r, list) if (wl_resource_get_client(r) == c) fn(r, arg);
}

/* ---------------- compositing ---------------- */
static void blend(struct window *w, const struct surface *s, int ox, int oy) {
    if (!s->pix) return;
    for (int y = 0; y < s->h; y++) {
        int dy = oy + y; if (dy < 0 || dy >= w->h) continue;
        uint32_t *d = w->px + (size_t)dy * w->w; const uint32_t *p = s->pix + (size_t)y * s->w;
        for (int x = 0; x < s->w; x++) {
            int dx = ox + x; if (dx < 0 || dx >= w->w) continue;
            uint32_t c = p[x], a = c >> 24;
            if (a == 255) d[dx] = c;
            else if (a) {                    /* premultiplied ARGB over the window */
                uint32_t b = d[dx], ia = 255 - a;
                uint32_t r = ((c >> 16) & 255) + (((b >> 16) & 255) * ia) / 255, g = ((c >> 8) & 255) + (((b >> 8) & 255) * ia) / 255, bl = (c & 255) + ((b & 255) * ia) / 255;
                uint32_t na = a + ((b >> 24) * ia) / 255;
                d[dx] = (na << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (bl > 255 ? 255 : bl);
            }
        }
    }
}
static void draw_tree(struct window *w, struct surface *s, int ox, int oy) {
    blend(w, s, ox, oy);
    struct sub *c;
    wl_list_for_each(c, &s->children, link) draw_tree(w, c->s, ox + c->x, oy + c->y);
}
static int map_window(struct window *w) {
    if (w->px && w->px != MAP_FAILED) munmap(w->px, (size_t)w->w * w->h * 4);
    w->px = mmap(NULL, (size_t)w->w * w->h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
    return w->px != MAP_FAILED;
}
static void window_events(struct window *w);
static int on_window_fd(int fd, uint32_t mask, void *data) { (void)fd; (void)mask; window_events(data); return 0; }
/* the size of the desktop window for the toplevel's current picture */
static void window_size(struct xdg *x, int *ww, int *wh, int *mx, int *my) {
    struct surface *s = x->s;
    if (x->has_geom) {
        *mx = x->gx < MARGIN ? x->gx : MARGIN; *my = x->gy < MARGIN ? x->gy : MARGIN;
        if (*mx < 0) *mx = 0;
        if (*my < 0) *my = 0;
        *ww = x->gw + 2 * *mx; *wh = x->gh + 2 * *my;
    } else { *mx = *my = 0; *ww = s->w; *wh = s->h; }
    if (*ww < 1) *ww = 1;
    if (*wh < 1) *wh = 1;
}
static void composite(struct window *w) {
    struct xdg *t = w->top;
    if (!t || !t->s->pix) return;
    int ww, wh, mx, my;
    window_size(t, &ww, &wh, &mx, &my);
    if (w->fd < 0) {
        w->fd = open("/dev/wrld/window", O_RDWR | O_CLOEXEC);
        if (w->fd < 0) { perror("wrldcomp: /dev/wrld/window"); return; }
        struct wrldwin_create c = { ww, wh, WRLDWIN_RESIZABLE | (t->has_geom ? WRLDWIN_FRAMELESS : 0), "" };
        snprintf(c.title, sizeof c.title, "%s", t->title[0] ? t->title : "Wayland program");
        if (ioctl(w->fd, WRLDWIN_CREATE, &c)) { perror("wrldcomp: create window"); close(w->fd); w->fd = -1; return; }
        w->w = ww; w->h = wh; w->frameless = t->has_geom;
        if (!map_window(w)) { perror("wrldcomp: mmap"); return; }
        w->src = wl_event_loop_add_fd(loop, w->fd, WL_EVENT_READABLE, on_window_fd, w);
        LOG("window %dx%d for '%s'%s", ww, wh, c.title, w->frameless ? " (client decorations)" : "");
    } else if (ww != w->w || wh != w->h) {
        struct wrldwin_size sz = { ww, wh };
        if (ioctl(w->fd, WRLDWIN_SETSIZE, &sz)) return;
        w->w = ww; w->h = wh;
        if (!map_window(w)) return;
    }
    w->mx = mx; w->my = my;
    /* outside the geometry stays see-through (shadows), inside starts white */
    for (int y = 0; y < w->h; y++) for (int x = 0; x < w->w; x++) {
        int in = !w->frameless || (x >= mx && y >= my && x < w->w - mx && y < w->h - my);
        w->px[(size_t)y * w->w + x] = in ? 0xFFFFFFFFu : 0;
    }
    int ox = t->has_geom ? mx - t->gx : 0, oy = t->has_geom ? my - t->gy : 0;
    draw_tree(w, t->s, ox, oy);
    if (w->frameless)                        /* faint shadow pixels around the geometry: see-through, but still reach the client */
        for (int y = 0; y < w->h; y++) for (int x = 0; x < w->w; x++) {
            int in = x >= mx && y >= my && x < w->w - mx && y < w->h - my;
            uint32_t *p = &w->px[(size_t)y * w->w + x];
            if (!in && (*p >> 24) < 160) *p = 0;
        }
    struct xdg *p;
    wl_list_for_each(p, &w->popups, link)
        if (p->s->pix) draw_tree(w, p->s, ox + t->gx + p->px - (p->has_geom ? p->gx : 0), oy + t->gy + p->py - (p->has_geom ? p->gy : 0));
    ioctl(w->fd, WRLDWIN_PRESENT, 0);
    w->dirty = 0;
}

/* ---------------- frame callbacks: every 16 ms ---------------- */
static struct wl_list all_surfaces_frames;   /* not used; frames live on surfaces */
static struct wl_event_source *frame_timer;
static struct wl_list surfaces;              /* struct surface_link */
struct surface_link { struct surface *s; struct wl_list link; };
static int frame_tick(void *data) {
    (void)data;
    uint32_t t = now_ms();
    struct surface_link *l;
    wl_list_for_each(l, &surfaces, link) {
        struct wl_resource *cb, *tmp;
        wl_resource_for_each_safe(cb, tmp, &l->s->frames) { wl_callback_send_done(cb, t); wl_resource_destroy(cb); }
    }
    wl_event_source_timer_update(frame_timer, 16);
    return 0;
}

/* ---------------- wl_surface ---------------- */
static void window_of(struct surface *s, struct window **out) {
    *out = NULL;
    while (s && s->role == ROLE_SUB && s->sub) s = s->sub->parent;
    if (s && s->xdg) *out = s->xdg->win;
}
static void s_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void s_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c; struct surface *s = wl_resource_get_user_data(r);
    s->pend_buf = buf; s->pend_attached = 1; s->pdx = x; s->pdy = y;
}
static void s_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static void cb_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(cb, NULL, NULL, cb_unlink);
    wl_list_insert(s->pend_frames.prev, wl_resource_get_link(cb));
}
static void s_set_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) { (void)c; (void)r; (void)reg; }
static void xdg_send_configure(struct xdg *x, int w, int h);
static void place_popup(struct xdg *p);
static void s_commit(struct wl_client *c, struct wl_resource *r) {
    (void)c; struct surface *s = wl_resource_get_user_data(r);
    if (s->pend_attached) {
        s->pend_attached = 0;
        struct wl_shm_buffer *sb = s->pend_buf ? wl_shm_buffer_get(s->pend_buf) : NULL;
        if (sb) {
            int w = wl_shm_buffer_get_width(sb), h = wl_shm_buffer_get_height(sb), stride = wl_shm_buffer_get_stride(sb);
            uint32_t fmt = wl_shm_buffer_get_format(sb);
            if (w != s->w || h != s->h || !s->pix) { free(s->pix); s->pix = malloc((size_t)w * h * 4); s->w = w; s->h = h; }
            if (s->pix) {
                wl_shm_buffer_begin_access(sb);
                const uint8_t *src = wl_shm_buffer_get_data(sb);
                for (int y = 0; y < h; y++) {
                    memcpy(s->pix + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 4);
                    if (fmt == WL_SHM_FORMAT_XRGB8888) for (int x = 0; x < w; x++) s->pix[(size_t)y * w + x] |= 0xFF000000u;
                }
                wl_shm_buffer_end_access(sb);
            }
        } else if (!s->pend_buf) { free(s->pix); s->pix = NULL; s->w = s->h = 0; }
        if (s->pend_buf) wl_buffer_send_release(s->pend_buf);   /* copied: the client may reuse it now */
        s->pend_buf = NULL;
    }
    wl_list_insert_list(s->frames.prev, &s->pend_frames);
    wl_list_init(&s->pend_frames);
    if (s->sub) { s->sub->x = s->sub->px; s->sub->y = s->sub->py; }
    /* the initial commit of an xdg surface: answer with a configure */
    if (s->xdg && !s->xdg->configured && (s->xdg->toplevel || s->xdg->popup)) {
        s->xdg->configured = 1;
        if (s->xdg->toplevel) xdg_send_configure(s->xdg, 0, 0);
        else place_popup(s->xdg);
    }
    struct window *w; window_of(s, &w);
    if (w) composite(w);
}
static void s_set_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c; (void)r; (void)t; }
static void s_set_scale(struct wl_client *c, struct wl_resource *r, int32_t sc) { (void)c; (void)r; (void)sc; }
static void s_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; (void)r; (void)x; (void)y; }
static const struct wl_surface_interface surface_impl = {
    s_destroy, s_attach, s_damage, s_frame, s_set_region, s_set_region, s_commit, s_set_transform, s_set_scale, s_damage, s_offset,
};
static void surface_gone(struct wl_resource *r) {
    struct surface *s = wl_resource_get_user_data(r);
    if (ptr_focus == s) ptr_focus = NULL;
    if (kbd_focus == s) kbd_focus = NULL;
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frames) wl_resource_destroy(cb);
    wl_resource_for_each_safe(cb, tmp, &s->pend_frames) wl_resource_destroy(cb);
    struct sub *c, *ct;
    wl_list_for_each_safe(c, ct, &s->children, link) { wl_list_remove(&c->link); wl_list_init(&c->link); c->parent = NULL; }
    if (s->sub) { if (s->sub->parent) wl_list_remove(&s->sub->link); wl_list_init(&s->sub->link); s->sub->s = NULL; }
    if (s->xdg) s->xdg->s = NULL;
    struct surface_link *l, *lt;
    wl_list_for_each_safe(l, lt, &surfaces, link) if (l->s == s) { wl_list_remove(&l->link); free(l); }
    free(s->pix); free(s);
}

/* ---------------- wl_region (contents ignored: every surface takes input everywhere) ---------------- */
static void r_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void r_box(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; (void)r; (void)x; (void)y; (void)w; (void)h; }
static const struct wl_region_interface region_impl = { r_destroy, r_box, r_box };

/* ---------------- wl_compositor ---------------- */
static void c_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct surface *s = calloc(1, sizeof *s);
    if (!s) { wl_client_post_no_memory(c); return; }
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_list_init(&s->pend_frames); wl_list_init(&s->frames); wl_list_init(&s->children);
    wl_resource_set_implementation(s->res, &surface_impl, s, surface_gone);
    struct surface_link *l = calloc(1, sizeof *l); l->s = s; wl_list_insert(&surfaces, &l->link);
}
static void c_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(reg, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = { c_create_surface, c_create_region };
static void bind_compositor(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, v, id);
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---------------- subsurfaces ---------------- */
static void sub_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void sub_pos(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; struct sub *b = wl_resource_get_user_data(r); b->px = x; b->py = y; }
static void sub_place(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib) { (void)c; (void)r; (void)sib; }
static void sub_mode(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static const struct wl_subsurface_interface subsurface_impl = { sub_destroy, sub_pos, sub_place, sub_place, sub_mode, sub_mode };
static void sub_gone(struct wl_resource *r) {
    struct sub *b = wl_resource_get_user_data(r);
    if (b->parent) wl_list_remove(&b->link);
    if (b->s) { b->s->sub = NULL; b->s->role = ROLE_NONE; }
    free(b);
}
static void sc_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void sc_get(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf, struct wl_resource *parent) {
    struct sub *b = calloc(1, sizeof *b);
    b->s = wl_resource_get_user_data(surf); b->parent = wl_resource_get_user_data(parent);
    b->res = wl_resource_create(c, &wl_subsurface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(b->res, &subsurface_impl, b, sub_gone);
    b->s->sub = b; b->s->role = ROLE_SUB;
    wl_list_insert(b->parent->children.prev, &b->link);
}
static const struct wl_subcompositor_interface subcompositor_impl = { sc_destroy, sc_get };
static void bind_subcompositor(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, v, id);
    wl_resource_set_implementation(r, &subcompositor_impl, NULL, NULL);
}

/* ---------------- xdg_wm_base ---------------- */
static void xdg_send_configure(struct xdg *x, int w, int h) {
    struct wl_array states; wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof *st); if (st) *st = XDG_TOPLEVEL_STATE_ACTIVATED;
    xdg_toplevel_send_configure(x->toplevel, w, h, &states);
    wl_array_release(&states);
    x->serial = next_serial();
    xdg_surface_send_configure(x->res, x->serial);
}
static void close_popups(struct window *w, struct xdg *from);
static void window_gone(struct window *w) {
    if (!w) return;
    if (w->src) wl_event_source_remove(w->src);
    if (w->px && w->px != MAP_FAILED) munmap(w->px, (size_t)w->w * w->h * 4);
    if (w->fd >= 0) close(w->fd);
    struct xdg *p, *pt;
    wl_list_for_each_safe(p, pt, &w->popups, link) { wl_list_remove(&p->link); wl_list_init(&p->link); p->win = NULL; }
    free(w);
}
/* toplevel */
static void tl_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void tl_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p) { (void)c; (void)r; (void)p; }
static void tl_title(struct wl_client *c, struct wl_resource *r, const char *t) {
    (void)c; struct xdg *x = wl_resource_get_user_data(r);
    snprintf(x->title, sizeof x->title, "%s", t ? t : "");
    if (x->win && x->win->fd >= 0) { char b[100]; snprintf(b, sizeof b, "%s", x->title); ioctl(x->win->fd, WRLDWIN_TITLE, b); }
}
static void tl_app_id(struct wl_client *c, struct wl_resource *r, const char *a) { (void)c; (void)r; (void)a; }
static void tl_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) { (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y; }
static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) {
    (void)c; (void)seat; (void)serial; struct xdg *x = wl_resource_get_user_data(r);
    if (x->win && x->win->fd >= 0) ioctl(x->win->fd, WRLDWIN_ACTION, WRLDWIN_DO_MOVE);
}
static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)c; (void)seat; (void)serial; struct xdg *x = wl_resource_get_user_data(r);
    /* xdg edges: top 1, bottom 2, left 4, right 8; the window device: left 1, right 2, top 4, bottom 8 */
    int e = ((edges & 1) ? 4 : 0) | ((edges & 2) ? 8 : 0) | ((edges & 4) ? 1 : 0) | ((edges & 8) ? 2 : 0);
    if (x->win && x->win->fd >= 0 && e) ioctl(x->win->fd, WRLDWIN_ACTION, (unsigned long)WRLDWIN_DO_RESIZE(e));
}
static void tl_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; struct xdg *x = wl_resource_get_user_data(r); x->min_w = w; x->min_h = h; }
static void tl_size_max(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void tl_max(struct wl_client *c, struct wl_resource *r) { (void)c; struct xdg *x = wl_resource_get_user_data(r); if (x->win && x->win->fd >= 0) ioctl(x->win->fd, WRLDWIN_ACTION, WRLDWIN_DO_MAXIMIZE); }
static void tl_unmax(struct wl_client *c, struct wl_resource *r) { (void)c; struct xdg *x = wl_resource_get_user_data(r); if (x->win && x->win->fd >= 0) ioctl(x->win->fd, WRLDWIN_ACTION, WRLDWIN_DO_RESTORE); }
static void tl_full(struct wl_client *c, struct wl_resource *r, struct wl_resource *o) { (void)o; tl_max(c, r); }
static void tl_min(struct wl_client *c, struct wl_resource *r) { (void)c; struct xdg *x = wl_resource_get_user_data(r); if (x->win && x->win->fd >= 0) ioctl(x->win->fd, WRLDWIN_ACTION, WRLDWIN_DO_MINIMIZE); }
static const struct xdg_toplevel_interface toplevel_impl = {
    tl_destroy, tl_parent, tl_title, tl_app_id, tl_menu, tl_move, tl_resize, tl_size_max, tl_size, tl_max, tl_unmax, tl_full, tl_unmax, tl_min,
};
static void toplevel_gone(struct wl_resource *r) {
    struct xdg *x = wl_resource_get_user_data(r);
    x->toplevel = NULL;
    window_gone(x->win); x->win = NULL;
}
/* popup placement: the anchor point on the anchor rectangle, the gravity
 * from there, the offset, then slid back inside the window */
static void place_popup(struct xdg *p) {
    struct positioner *q = &p->pos;
    int ax = q->ax, ay = q->ay;
    switch (q->anchor) {
    case 1: ax += q->aw / 2; break;                                   /* top */
    case 2: ax += q->aw / 2; ay += q->ah; break;                      /* bottom */
    case 3: ay += q->ah / 2; break;                                   /* left */
    case 4: ax += q->aw; ay += q->ah / 2; break;                      /* right */
    case 5: break;                                                    /* top left */
    case 6: ay += q->ah; break;                                       /* bottom left */
    case 7: ax += q->aw; break;                                       /* top right */
    case 8: ax += q->aw; ay += q->ah; break;                          /* bottom right */
    default: ax += q->aw / 2; ay += q->ah / 2; break;
    }
    int x = ax - q->w / 2, y = ay - q->h / 2;
    switch (q->gravity) {
    case 1: y = ay - q->h; break;
    case 2: y = ay; break;
    case 3: x = ax - q->w; break;
    case 4: x = ax; break;
    case 5: x = ax - q->w; y = ay - q->h; break;
    case 6: x = ax - q->w; y = ay; break;
    case 7: x = ax; y = ay - q->h; break;
    case 8: x = ax; y = ay; break;
    }
    x += q->ox; y += q->oy;
    /* relative to the parent's geometry; convert to the toplevel's and keep it inside the window */
    int tx = x, ty = y;
    for (struct xdg *a = p->parent; a && a->popup; a = a->parent) { tx += a->px; ty += a->py; }
    struct window *w = p->win;
    if (w && w->top) {
        int maxw = w->top->has_geom ? w->top->gw : w->w, maxh = w->top->has_geom ? w->top->gh : w->h;
        if (tx + q->w > maxw) tx = maxw - q->w;
        if (ty + q->h > maxh) ty = maxh - q->h;
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
    }
    int dx = tx, dy = ty;                                             /* the slid position, in parent terms */
    for (struct xdg *a = p->parent; a && a->popup; a = a->parent) { dx -= a->px; dy -= a->py; }
    p->px = tx; p->py = ty;                                           /* stored relative to the toplevel */
    xdg_popup_send_configure(p->popup, dx, dy, q->w, q->h);
    p->serial = next_serial();
    xdg_surface_send_configure(p->res, p->serial);
}
static void pp_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void pp_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial) { (void)c; (void)seat; (void)serial; struct xdg *x = wl_resource_get_user_data(r); x->grab = 1; }
static void pp_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos, uint32_t token) {
    (void)c; struct xdg *x = wl_resource_get_user_data(r);
    x->pos = *(struct positioner *)wl_resource_get_user_data(pos);
    xdg_popup_send_repositioned(r, token);
    place_popup(x);
}
static const struct xdg_popup_interface popup_impl = { pp_destroy, pp_grab, pp_reposition };
static void popup_gone(struct wl_resource *r) {
    struct xdg *x = wl_resource_get_user_data(r);
    x->popup = NULL;
    struct window *w = x->win;
    wl_list_remove(&x->link); wl_list_init(&x->link);
    x->win = NULL;
    if (w) composite(w);
}
static void close_popups(struct window *w, struct xdg *from) {
    (void)from;
    struct xdg *p, *pt;
    wl_list_for_each_reverse_safe(p, pt, &w->popups, link) if (p->popup) xdg_popup_send_popup_done(p->popup);
}
/* xdg_surface */
static void xs_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void xs_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct xdg *x = wl_resource_get_user_data(r);
    x->toplevel = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(x->toplevel, &toplevel_impl, x, toplevel_gone);
    x->s->role = ROLE_TOPLEVEL;
    struct window *w = calloc(1, sizeof *w);
    w->fd = -1; w->top = x; wl_list_init(&w->popups);
    x->win = w;
}
static void xs_popup(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *parent, struct wl_resource *pos) {
    struct xdg *x = wl_resource_get_user_data(r);
    x->popup = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(x->popup, &popup_impl, x, popup_gone);
    x->s->role = ROLE_POPUP;
    x->parent = parent ? wl_resource_get_user_data(parent) : NULL;
    x->pos = *(struct positioner *)wl_resource_get_user_data(pos);
    struct xdg *top = x->parent; while (top && top->popup) top = top->parent;
    x->win = top ? top->win : NULL;
    if (x->win) wl_list_insert(x->win->popups.prev, &x->link); else wl_list_init(&x->link);
}
static void xs_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)c; struct xdg *d = wl_resource_get_user_data(r);
    d->gx = x; d->gy = y; d->gw = w; d->gh = h; d->has_geom = w > 0 && h > 0;
}
static void xs_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial) { (void)c; struct xdg *d = wl_resource_get_user_data(r); if (serial == d->serial) d->acked = 1; }
static const struct xdg_surface_interface xdg_surface_impl = { xs_destroy, xs_toplevel, xs_popup, xs_geometry, xs_ack };
static void xdg_surface_gone(struct wl_resource *r) {
    struct xdg *x = wl_resource_get_user_data(r);
    if (x->s) x->s->xdg = NULL;
    if (x->toplevel) { wl_resource_set_user_data(x->toplevel, NULL); }
    free(x);
}
/* positioner */
static void po_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void po_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->w = w; p->h = h; }
static void po_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->ax = x; p->ay = y; p->aw = w; p->ah = h; }
static void po_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->anchor = (int)a; }
static void po_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->gravity = (int)g; }
static void po_adjust(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->adjust = (int)a; }
static void po_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c; struct positioner *p = wl_resource_get_user_data(r); p->ox = x; p->oy = y; }
static void po_reactive(struct wl_client *c, struct wl_resource *r) { (void)c; (void)r; }
static void po_psize(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { (void)c; (void)r; (void)w; (void)h; }
static void po_pconf(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static const struct xdg_positioner_interface positioner_impl = { po_destroy, po_size, po_rect, po_anchor, po_gravity, po_adjust, po_offset, po_reactive, po_psize, po_pconf };
static void free_data(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
/* wm_base */
static void wm_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void wm_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &positioner_impl, calloc(1, sizeof(struct positioner)), free_data);
}
static void wm_get_xdg(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf) {
    struct xdg *x = calloc(1, sizeof *x);
    x->s = wl_resource_get_user_data(surf); x->s->xdg = x;
    wl_list_init(&x->link);
    x->res = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(x->res, &xdg_surface_impl, x, xdg_surface_gone);
}
static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t s) { (void)c; (void)r; (void)s; }
static const struct xdg_wm_base_interface wm_base_impl = { wm_destroy, wm_positioner, wm_get_xdg, wm_pong };
static void bind_wm_base(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, v, id);
    wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

/* ---------------- wl_seat ---------------- */
static void res_unlink(struct wl_resource *r) { wl_list_remove(wl_resource_get_link(r)); }
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial, struct wl_resource *surf, int32_t x, int32_t y) {
    (void)c; (void)r; (void)serial; (void)x; (void)y;
    if (surf) { struct surface *s = wl_resource_get_user_data(surf); if (s->role == ROLE_NONE) s->role = ROLE_CURSOR; }
}
static void res_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_pointer_interface pointer_impl = { ptr_set_cursor, res_release };
static const struct wl_keyboard_interface keyboard_impl = { res_release };
static void send_keymap(struct wl_resource *k) {
    if (!keymap_text) return;
    int fd = memfd_create("wrld-keymap", MFD_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, keymap_text, keymap_len) != (ssize_t)keymap_len) { close(fd); return; }
    wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)keymap_len);
    close(fd);
    if (wl_resource_get_version(k) >= 4) wl_keyboard_send_repeat_info(k, 30, 400);
}
static void seat_ptr(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &pointer_impl, NULL, res_unlink);
    wl_list_insert(&pointers, wl_resource_get_link(p));
}
static void seat_kbd(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(k, &keyboard_impl, NULL, res_unlink);
    wl_list_insert(&keyboards, wl_resource_get_link(k));
    send_keymap(k);
}
static void seat_touch(struct wl_client *c, struct wl_resource *r, uint32_t id) { (void)c; (void)r; (void)id; }
static const struct wl_seat_interface seat_impl = { seat_ptr, seat_kbd, seat_touch, res_release };
static void bind_seat(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (v >= 2) wl_seat_send_name(r, "seat0");
}

/* ---------------- wl_output ---------------- */
static void out_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface output_impl = { out_release };
static void bind_output(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_output_interface, v, id);
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, 340, 270, WL_OUTPUT_SUBPIXEL_UNKNOWN, "THE WRLD OS", "Desktop", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1024, 768, 60000);
    if (v >= 2) { wl_output_send_scale(r, 1); wl_output_send_done(r); }
}

/* ---------------- wl_data_device_manager (no clipboard contents yet) ---------------- */
static void ds_offer(struct wl_client *c, struct wl_resource *r, const char *m) { (void)c; (void)r; (void)m; }
static void ds_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void ds_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) { (void)c; (void)r; (void)a; }
static const struct wl_data_source_interface data_source_impl = { ds_offer, ds_destroy, ds_actions };
static void dd_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, struct wl_resource *o, struct wl_resource *i, uint32_t serial) { (void)c; (void)r; (void)s; (void)o; (void)i; (void)serial; }
static void dd_sel(struct wl_client *c, struct wl_resource *r, struct wl_resource *s, uint32_t serial) { (void)c; (void)r; (void)s; (void)serial; }
static void dd_release(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_data_device_interface data_device_impl = { dd_drag, dd_sel, dd_release };
static void dm_source(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &data_source_impl, NULL, NULL);
}
static void dm_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat) {
    (void)seat; struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(d, &data_device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface data_manager_impl = { dm_source, dm_device };
static void bind_data_manager(struct wl_client *c, void *d, uint32_t v, uint32_t id) {
    (void)d; struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
    wl_resource_set_implementation(r, &data_manager_impl, NULL, NULL);
}

/* ---------------- input ---------------- */
/* THE WRLD OS key -> Linux evdev code (the PC scancode for the main block) */
static const char sc_lower[] = { 0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 10, 0, 'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ' };
static const char sc_upper[] = { 0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 9,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 10, 0, 'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ' };
static int evdev_code(uint32_t key) {
    if (key >= 0x100) {
        static const int special[] = { 1, 14, 15, 28, 29, 42, 54, 56, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,
                                       103, 108, 105, 106, 102, 107, 104, 109, 110, 111, 125, 127 };
        unsigned i = key - 0x100;
        return i < sizeof special / sizeof *special ? special[i] : 0;
    }
    for (int i = 1; i < (int)sizeof sc_lower; i++) if (sc_lower[i] == (char)key || sc_upper[i] == (char)key) return i;
    return 0;
}
static uint32_t xkb_mods(uint32_t m) { return ((m & 1) ? 1 : 0) | ((m & 2) ? 4 : 0) | ((m & 4) ? 8 : 0); }
/* the surface under a window point, topmost first; returns surface-local coordinates */
static struct surface *hit_tree(struct surface *s, int x, int y, int *lx, int *ly) {
    struct sub *c;
    wl_list_for_each_reverse(c, &s->children, link) { struct surface *h = hit_tree(c->s, x - c->x, y - c->y, lx, ly); if (h) return h; }
    if (s->pix && x >= 0 && y >= 0 && x < s->w && y < s->h) { *lx = x; *ly = y; return s; }
    return NULL;
}
static struct surface *surface_at(struct window *w, int x, int y, int *lx, int *ly) {
    struct xdg *t = w->top; if (!t || !t->s) return NULL;
    int ox = t->has_geom ? w->mx - t->gx : 0, oy = t->has_geom ? w->my - t->gy : 0;
    struct xdg *p;
    wl_list_for_each_reverse(p, &w->popups, link) {
        if (!p->s || !p->s->pix) continue;
        int px = ox + t->gx + p->px - (p->has_geom ? p->gx : 0), py = oy + t->gy + p->py - (p->has_geom ? p->gy : 0);
        struct surface *h = hit_tree(p->s, x - px, y - py, lx, ly); if (h) return h;
    }
    return hit_tree(t->s, x - ox, y - oy, lx, ly);
}
static int any_grab(struct window *w) { struct xdg *p; wl_list_for_each(p, &w->popups, link) if (p->grab && p->popup) return 1; return 0; }
struct ptr_arg { struct surface *s; int x, y, button, state, axis; uint32_t serial; };
static void p_enter(struct wl_resource *r, void *a) { struct ptr_arg *q = a; wl_pointer_send_enter(r, q->serial, q->s->res, wl_fixed_from_int(q->x), wl_fixed_from_int(q->y)); if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r); }
static void p_leave(struct wl_resource *r, void *a) { struct ptr_arg *q = a; wl_pointer_send_leave(r, q->serial, q->s->res); if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r); }
static void p_motion(struct wl_resource *r, void *a) { struct ptr_arg *q = a; wl_pointer_send_motion(r, now_ms(), wl_fixed_from_int(q->x), wl_fixed_from_int(q->y)); if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r); }
static void p_button(struct wl_resource *r, void *a) { struct ptr_arg *q = a; wl_pointer_send_button(r, q->serial, now_ms(), (uint32_t)q->button, (uint32_t)q->state); if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r); }
static void p_axis(struct wl_resource *r, void *a) { struct ptr_arg *q = a; wl_pointer_send_axis(r, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(q->axis)); if (wl_resource_get_version(r) >= 5) wl_pointer_send_frame(r); }
static void pointer_focus(struct surface *s, int x, int y) {
    if (s == ptr_focus) return;
    struct ptr_arg a = { 0 };
    if (ptr_focus) { a.s = ptr_focus; a.serial = next_serial(); send_to_client(&pointers, wl_resource_get_client(ptr_focus->res), p_leave, &a); }
    ptr_focus = s;
    if (s) { a.s = s; a.x = x; a.y = y; a.serial = next_serial(); send_to_client(&pointers, wl_resource_get_client(s->res), p_enter, &a); }
}
struct kbd_arg { struct surface *s; uint32_t key, state, serial; };
static void k_enter(struct wl_resource *r, void *a) {
    struct kbd_arg *q = a; struct wl_array keys; wl_array_init(&keys);
    wl_keyboard_send_enter(r, q->serial, q->s->res, &keys); wl_array_release(&keys);
    wl_keyboard_send_modifiers(r, q->serial, xkb_mods(mods_now), 0, 0, 0);
}
static void k_leave(struct wl_resource *r, void *a) { struct kbd_arg *q = a; wl_keyboard_send_leave(r, q->serial, q->s->res); }
static void k_key(struct wl_resource *r, void *a) { struct kbd_arg *q = a; wl_keyboard_send_key(r, q->serial, now_ms(), q->key, q->state); }
static void k_mods(struct wl_resource *r, void *a) { struct kbd_arg *q = a; wl_keyboard_send_modifiers(r, q->serial, xkb_mods(mods_now), 0, 0, 0); }
static struct surface *keyboard_target(struct window *w) {
    struct xdg *p, *best = NULL;
    wl_list_for_each(p, &w->popups, link) if (p->grab && p->popup && p->s) best = p;
    return best ? best->s : w->top ? w->top->s : NULL;
}
static void keyboard_focus(struct surface *s) {
    if (s == kbd_focus) return;
    struct kbd_arg a = { 0 };
    if (kbd_focus) { a.s = kbd_focus; a.serial = next_serial(); send_to_client(&keyboards, wl_resource_get_client(kbd_focus->res), k_leave, &a); }
    kbd_focus = s;
    if (s) { a.s = s; a.serial = next_serial(); send_to_client(&keyboards, wl_resource_get_client(s->res), k_enter, &a); }
}
static void window_events(struct window *w) {
    struct wrldwin_event ev[32];
    ssize_t n = read(w->fd, ev, sizeof ev);
    if (n == 0) {                                           /* the window is gone */
        if (w->top && w->top->toplevel) xdg_toplevel_send_close(w->top->toplevel);
        if (w->src) { wl_event_source_remove(w->src); w->src = NULL; }
        return;
    }
    if (n < 0) return;
    for (int i = 0; i < (int)(n / (ssize_t)sizeof *ev); i++) {
        struct wrldwin_event *e = &ev[i];
        int lx = 0, ly = 0;
        switch (e->type) {
        case WRLDWIN_EV_MOTION: {
            struct surface *s = surface_at(w, e->x, e->y, &lx, &ly);
            pointer_focus(s, lx, ly);
            if (s) { struct ptr_arg a = { s, lx, ly, 0, 0, 0, 0 }; send_to_client(&pointers, wl_resource_get_client(s->res), p_motion, &a); }
            break;
        }
        case WRLDWIN_EV_BUTTON_DOWN: case WRLDWIN_EV_BUTTON_UP: {
            struct surface *s = surface_at(w, e->x, e->y, &lx, &ly);
            /* a click outside an open menu closes the menus */
            if (e->type == WRLDWIN_EV_BUTTON_DOWN && any_grab(w) && (!s || s->role != ROLE_POPUP)) {
                int in_popup = 0;
                for (struct surface *q = s; q; q = q->sub ? q->sub->parent : NULL) if (q->role == ROLE_POPUP) in_popup = 1;
                if (!in_popup) { close_popups(w, NULL); break; }
            }
            keyboard_focus(keyboard_target(w));
            pointer_focus(s, lx, ly);
            if (s) {
                static const int btn[] = { 0, 0x110, 0x112, 0x111 };   /* BTN_LEFT, BTN_MIDDLE, BTN_RIGHT */
                struct ptr_arg a = { s, lx, ly, btn[e->code & 3], e->type == WRLDWIN_EV_BUTTON_DOWN, 0, next_serial() };
                send_to_client(&pointers, wl_resource_get_client(s->res), p_button, &a);
            }
            break;
        }
        case WRLDWIN_EV_WHEEL:
            if (ptr_focus) { struct ptr_arg a = { ptr_focus, 0, 0, 0, 0, (int)e->code * 10, 0 }; send_to_client(&pointers, wl_resource_get_client(ptr_focus->res), p_axis, &a); }
            break;
        case WRLDWIN_EV_LEAVE: pointer_focus(NULL, 0, 0); break;
        case WRLDWIN_EV_KEY_DOWN: case WRLDWIN_EV_KEY_UP: {
            keyboard_focus(keyboard_target(w));
            if (!kbd_focus) break;
            int code = evdev_code(e->code);
            struct kbd_arg a = { kbd_focus, (uint32_t)code, e->type == WRLDWIN_EV_KEY_DOWN, next_serial() };
            if (mods_now != e->mods) { mods_now = e->mods; send_to_client(&keyboards, wl_resource_get_client(kbd_focus->res), k_mods, &a); }
            if (code) send_to_client(&keyboards, wl_resource_get_client(kbd_focus->res), k_key, &a);
            break;
        }
        case WRLDWIN_EV_RESIZE:
            if (w->top && w->top->toplevel) {
                int cw = e->x - 2 * w->mx, ch = e->y - 2 * w->my;
                if (cw < 1 || ch < 1) break;
                w->w = e->x; w->h = e->y;                       /* the window already has the new size */
                struct wrldwin_size sz = { e->x, e->y };
                ioctl(w->fd, WRLDWIN_RESIZE, &sz);
                map_window(w);
                xdg_send_configure(w->top, w->top->has_geom ? cw : e->x, w->top->has_geom ? ch : e->y);
            }
            break;
        case WRLDWIN_EV_CLOSE:
            if (w->top && w->top->toplevel) xdg_toplevel_send_close(w->top->toplevel);
            break;
        }
    }
}

/* ---------------- start-up ---------------- */
/* the program it started: checked from a timer (THE WRLD OS has no signalfd) */
static pid_t child;
static struct wl_event_source *child_timer;
static int child_check(void *data) {
    (void)data;
    int st; pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0)
        if (p == child) { fprintf(stderr, "wrldcomp: %s exited with status %d\n", "the program", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)); wl_display_terminate(display); return 0; }
    wl_event_source_timer_update(child_timer, 200);
    return 0;
}
static void load_keymap(void) {
    const char *path = getenv("WRLD_KEYMAP") ? getenv("WRLD_KEYMAP") : "/usr/share/wrld/keymap.xkb";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "wrldcomp: no keymap at %s: keyboards will not work\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    keymap_text = calloc(1, (size_t)n + 1);
    if (keymap_text && fread(keymap_text, 1, (size_t)n, f) == (size_t)n) keymap_len = (size_t)n + 1;   /* with the NUL */
    fclose(f);
}
int main(int argc, char **argv) {
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-v")) { verbose = 1; a++; }
    if (!getenv("XDG_RUNTIME_DIR")) { mkdir("/tmp/wrld-runtime", 0700); setenv("XDG_RUNTIME_DIR", "/tmp/wrld-runtime", 1); }
    wl_list_init(&pointers); wl_list_init(&keyboards); wl_list_init(&surfaces);
    (void)all_surfaces_frames;
    load_keymap();
    display = wl_display_create();
    if (!display) { fprintf(stderr, "wrldcomp: cannot create the display\n"); return 1; }
    const char *sock = wl_display_add_socket_auto(display);
    if (!sock) { fprintf(stderr, "wrldcomp: cannot create the socket in %s\n", getenv("XDG_RUNTIME_DIR")); return 1; }
    wl_display_init_shm(display);
    wl_global_create(display, &wl_compositor_interface, 5, NULL, bind_compositor);
    wl_global_create(display, &wl_subcompositor_interface, 1, NULL, bind_subcompositor);
    wl_global_create(display, &xdg_wm_base_interface, 3, NULL, bind_wm_base);
    wl_global_create(display, &wl_seat_interface, 7, NULL, bind_seat);
    wl_global_create(display, &wl_output_interface, 3, NULL, bind_output);
    wl_global_create(display, &wl_data_device_manager_interface, 3, NULL, bind_data_manager);
    loop = wl_display_get_event_loop(display);
    frame_timer = wl_event_loop_add_timer(loop, frame_tick, NULL);
    wl_event_source_timer_update(frame_timer, 16);
    setenv("WAYLAND_DISPLAY", sock, 1);
    /* what GTK and friends need here: no GL or Vulkan (CPU rendering with Cairo), no
     * accessibility bus, portals or D-Bus session, settings kept in memory */
    setenv("GDK_BACKEND", "wayland", 0);
    setenv("GSK_RENDERER", "cairo", 0);
    setenv("GDK_DEBUG", "gl-disable,vulkan-disable", 0);
    setenv("GDK_DISABLE", "gl,vulkan", 0);
    setenv("GTK_A11Y", "none", 0);
    setenv("NO_AT_BRIDGE", "1", 0);
    setenv("GTK_USE_PORTAL", "0", 0);
    setenv("GSETTINGS_BACKEND", "memory", 0);
    setenv("XDG_DATA_DIRS", "/usr/share", 0);
    setenv("HOME", "/home/user", 0);
    fprintf(stderr, "wrldcomp: Wayland display %s/%s\n", getenv("XDG_RUNTIME_DIR"), sock);
    if (a < argc) {
        child_timer = wl_event_loop_add_timer(loop, child_check, NULL);
        wl_event_source_timer_update(child_timer, 200);
        child = fork();
        if (child == 0) { execvp(argv[a], argv + a); fprintf(stderr, "wrldcomp: cannot run %s: %s\n", argv[a], strerror(errno)); _exit(127); }
    }
    wl_display_run(display);
    wl_display_destroy_clients(display);
    wl_display_destroy(display);
    return 0;
}
