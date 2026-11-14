/* Boot screen, welcome/logon screen, turn-off & log-off dialogs, Run, message boxes */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "audio.h"
#include "fs.h"
#include "usb.h"

/* ---------------- boot screen ---------------- */
static int boot_frame;
static char boot_msg[96];
static void boot_draw(int pct) {
    canvas_t *c = &screen;
    gfx_clear(c, BLACK);
    int cx = SCREEN_W / 2, cy = SCREEN_H / 2 - 60;
    /* spinning globe above the wordmark */
    draw_globe(c, cx - 80, cy - 200, 160, -36 + boot_frame * 2);
    int ww = wordmark_width(WORDMARK_LARGE);
    draw_wordmark(c, cx - ww / 2, cy - 22, WORDMARK_LARGE, WHITE);
    draw_text(c, FONT_UI, cx - text_width(FONT_UI, OS_EDITION) / 2, cy + 52, OS_EDITION, HEX(0xBBBBBB));
    /* progress well with sweeping blocks */
    rect_t r = R(cx - 62, cy + 100, 124, 14);
    fill_round_rect(c, r.x, r.y, r.w, r.h, 4, HEX(0xB2B2B2));
    fill_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, BLACK);
    rect_t old = canvas_push_clip(c, R(r.x + 3, r.y + 2, r.w - 6, r.h - 4));
    int pos = (boot_frame * 3) % (r.w + 40) - 30;
    for (int k = 0; k < 3; k++) {
        int bx = r.x + pos + k * 10;
        gradient_v3(c, bx, r.y + 3, 8, r.h - 6, HEX(0x2838C8), HEX(0x6CA0FF), HEX(0x2838C8), (r.h - 6) / 2);
    }
    c->clip = old;
    (void)pct;
    if (boot_verbose) {
        /* verbose: show the kernel log */
        const char *log = klog_buffer();
        int lines = 0; for (const char *p = log; *p; p++) if (*p == '\n') lines++;
        const char *p = log; int skip = lines - 18;
        while (skip > 0 && *p) { if (*p == '\n') skip--; p++; }
        int y = SCREEN_H - 18 * 15 - 20;
        while (*p) { const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p); draw_text_n(c, FONT_MONO, 12, y, p, e - p, HEX(0x9AE69A)); y += 15; p = *e ? e + 1 : e; }
    } else draw_text(c, FONT_SMALL, 12, SCREEN_H - 20, boot_msg, HEX(0x707070));
    draw_text(c, FONT_SMALL, SCREEN_W - 230, SCREEN_H - 20, "An original XP-style hobby operating system", HEX(0x707070));
    gfx_present(CURSOR_BUSY, -100, -100);
}
void boot_screen_begin(void) { boot_frame = 0; boot_draw(0); }
void boot_screen_progress(const char *msg, int pct) {
    strlcpy(boot_msg, msg, sizeof boot_msg);
    uint32_t until = timer_ticks() + (boot_verbose ? 60 : 260);
    while ((int32_t)(timer_ticks() - until) < 0) { boot_frame++; boot_draw(pct); task_sleep(16); }
}
/* one frame, no minimum display time: for progress inside long operations */
void boot_screen_status(const char *msg, int pct) {
    strlcpy(boot_msg, msg, sizeof boot_msg);
    boot_frame++; boot_draw(pct);
}
void boot_screen_end(void) {
    for (int i = 0; i < 16; i++) { blend_rect(&screen, 0, 0, SCREEN_W, SCREEN_H, BLACK, 60); gfx_present(CURSOR_BUSY, -100, -100); task_sleep(15); }
}

/* ---------------- welcome screen ---------------- */
/* The welcome screen itself, and logon skins, are in logon.c. The shut-down
 * screen uses the current skin's background. */
void logon_paint_background(canvas_t *c);
static void welcome_bg(canvas_t *c, bool divider) { (void)divider; logon_paint_background(c); }

/* ---------------- turn off / log off ---------------- */
static void desaturate_screen(void) {
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint32_t p = screen.px[i];
        int g = (C_R(p) * 30 + C_G(p) * 59 + C_B(p) * 11) / 100;
        screen.px[i] = mix(p, RGB(g, g, g), 230);
    }
}
static void final_screen(const char *msg, bool reboot) {
    canvas_t *c = &screen;
    /* from here on the desktop loop is gone on purpose */
    gfx_ready_for_watchdog = false;
    welcome_bg(c, false);
    draw_globe(c, SCREEN_W / 2 - 210, SCREEN_H / 2 - 92, 96, -36);
    draw_wordmark(c, SCREEN_W / 2 - 104, SCREEN_H / 2 - 74, WORDMARK_MEDIUM, WHITE);
    draw_text(c, FONT_LARGE, SCREEN_W / 2 - 104, SCREEN_H / 2, msg, WHITE);
    gfx_present(CURSOR_BUSY, -100, -100);
    uint32_t t = timer_ticks() + 1500;
    char err[64];
    if (system_sounds) { audio_stop_all(); audio_play_file(SOUND_SHUTDOWN, err, sizeof err); }
    if (system_volume) {
        task_sleep(600);
        welcome_bg(c, false);
        draw_globe(c, SCREEN_W / 2 - 210, SCREEN_H / 2 - 92, 96, -36);
        draw_wordmark(c, SCREEN_W / 2 - 104, SCREEN_H / 2 - 74, WORDMARK_MEDIUM, WHITE);
        draw_text(c, FONT_LARGE, SCREEN_W / 2 - 104, SCREEN_H / 2, "Saving your settings...", WHITE);
        gfx_present(CURSOR_BUSY, -100, -100);
        persist_flush();
    }
    audio_wait(10000);                                   /* let the shutdown sound finish */
    while ((int32_t)(timer_ticks() - t) < 0) task_sleep(10);
    if (reboot) system_reboot();
    system_poweroff();
    /* poweroff unsupported: classic message */
    gfx_clear(c, BLACK);
    draw_text_centered(c, FONT_XL, R(0, 0, SCREEN_W, SCREEN_H), "It is now safe to turn off your computer.", HEX(0xF0A030));
    gfx_present(CURSOR_ARROW, -100, -100);
    for (;;) hlt();
}
void shell_power_action(void *restart) { final_screen(restart ? "Restarting..." : "Shutting down...", restart != NULL); }
static int dialog_loop(const char *title, int kind) {
    /* kind 0: turn off (3 options), 1: log off (2 options). Returns option index or -1 */
    static canvas_t *saved;
    if (saved && (saved->w != SCREEN_W || saved->h != SCREEN_H)) { canvas_free(saved); saved = NULL; }
    if (!saved) saved = canvas_new(SCREEN_W, SCREEN_H);
    memcpy(saved->px, screen.px, SCREEN_W * SCREEN_H * 4);
    for (int k = 0; k < 6; k++) { memcpy(screen.px, saved->px, SCREEN_W * SCREEN_H * 4); for (int i = 0; i <= k; i++) {} desaturate_screen(); }
    canvas_t *bg = canvas_new(SCREEN_W, SCREEN_H);
    memcpy(bg->px, screen.px, SCREEN_W * SCREEN_H * 4);
    int W = 312, H = 198, x = (SCREEN_W - W) / 2, y = (SCREEN_H - H) / 2;
    int n = kind == 0 ? 3 : 2, hot = -1, prev = 1, result = -2;
    const char *labels0[3] = { "Stand By", "Turn Off", "Restart" }, *labels1[2] = { "Switch User", "Log Off" };
    int icons0[3] = { ICON_LOGOFF, ICON_POWER, ICON_RESTART }, icons1[2] = { ICON_CP_USERS, ICON_LOGOFF };
    mouse_state_t ms; key_event_t ke;
    bool redraw = true;
    while (result == -2) {
        gui_heartbeat = timer_ticks();               /* this loop is the desktop now */
        if (redraw) {
            memcpy(screen.px, bg->px, SCREEN_W * SCREEN_H * 4);
            canvas_t *c = &screen;
            if (T.classic) {
                fill_rect(c, x, y, W, H, T.face); bevel_deep(c, x, y, W, H, true);
                gradient_h(c, x + 3, y + 3, W - 6, 18, T.title_top, T.title_bot);
                draw_text(c, FONT_UI_BOLD, x + 8, y + 5, title, WHITE);
            } else {
                gradient_v(c, x, y, W, 46, HEX(0x00309C), HEX(0x0A3CA8));
                fill_rect(c, x, y + 46, W, H - 46 - 40, HEX(0x5A7EDC));
                gradient_h(c, x, y + 46, W, 2, HEX(0x5A7EDC), HEX(0xB8D0FF));
                gradient_v(c, x, y + H - 40, W, 40, HEX(0x0A3CA8), HEX(0x00309C));
                draw_text(c, FONT_LARGE, x + 12, y + 13, title, WHITE);
                draw_icon(c, ICON_LOGO, 32, x + W - 44, y + 7);
            }
            for (int i = 0; i < n; i++) {
                int bx = x + (W / (n + 1)) * (i + 1) - 16, by = y + 76;
                const char *l = kind ? labels1[i] : labels0[i];
                if (hot == i) fill_round_rect(c, bx - 4, by - 4, 40, 40, 6, T.classic ? HEX(0xC0C0C0) : HEX(0x7A9EEC));
                draw_icon(c, kind ? icons1[i] : icons0[i], 32, bx, by);
                int tw = text_width(FONT_UI, l);
                draw_text(c, FONT_UI, bx + 16 - tw / 2, by + 42, l, T.classic ? BLACK : WHITE);
            }
            rect_t cb = R(x + W - 82, y + H - 32, 70, 23);
            draw_button(c, cb, "Cancel", hot == 9, false, false, true);
            gfx_present(CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
            redraw = false;
        }
        while (keyboard_poll(&ke)) if (ke.pressed && ke.key == KEY_ESC) result = -1;
        if (mouse_poll(&ms)) {
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            int nh = -1;
            for (int i = 0; i < n; i++) { int bx = x + (W / (n + 1)) * (i + 1) - 16, by = y + 76; if (pt_in(R(bx - 10, by - 4, 52, 62), ms.x, ms.y)) nh = i; }
            if (pt_in(R(x + W - 82, y + H - 32, 70, 23), ms.x, ms.y)) nh = 9;
            bool click = (ms.buttons & 1) && !(prev & 1); prev = ms.buttons;
            if (click && nh >= 0) result = nh == 9 ? -1 : nh;
            if (nh != hot) { hot = nh; redraw = true; } else gfx_move_cursor(CURSOR_ARROW, ms.x, ms.y);
        }
        task_yield(); hlt();          /* the USB task polls the mouse and keyboard */
    }
    canvas_free(bg);
    memcpy(screen.px, saved->px, SCREEN_W * SCREEN_H * 4);
    wm_need_redraw = true;
    return result;
}
void shell_show_shutdown(void) {
    int r = dialog_loop(T.classic ? "Shut Down " OS_NAME : "Turn off computer", 0);
    if (r == 0) {   /* stand by: blank the screen until input */
        gfx_clear(&screen, BLACK); gfx_present(CURSOR_ARROW, -100, -100);
        mouse_state_t ms; key_event_t ke;
        task_sleep(300); while (keyboard_poll(&ke)) {} mouse_poll(&ms);
        for (;;) {
            gui_heartbeat = timer_ticks();           /* asleep on purpose, not hung */
            if (keyboard_poll(&ke) || mouse_poll(&ms)) break;
            task_yield(); hlt();                     /* a USB keyboard or mouse must run to wake it */
        }
        wm_redraw_all();
    } else if (r == 1) final_screen("Shutting down...", false);
    else if (r == 2) final_screen("Restarting...", true);
}
static void close_all_apps(void) {
    /* Keep the shell, its supervisor and the kernel service tasks running.
     * Logging off should close windows and user programs, not tear down the
     * desktop or USB/network/persistence. */
    for (int i = wm_window_count() - 1; i >= 0; i--) { window_t *w = wm_window_at(i); if (w) wm_destroy(w); }
    for (task_t *t = task_list(); t;) {
        task_t *nx = t->next;
        const char *n = t->name;
        bool service = t->pid == 0 || t == gui_task || !strcmp(n, "shellwatch") || !strcmp(n, "usbd") ||
                       !strcmp(n, "netd") || !strcmp(n, "devmgr") || !strcmp(n, "persistd");
        if (!service) task_kill(t->pid);
        t = nx;
    }
}
void shell_logoff(void) {
    close_all_apps();
    if (startmenu_is_open()) startmenu_toggle();
    welcome_screen();
    wm_redraw_all();
}
void shell_logoff_dialog(void) {
    int r = dialog_loop(T.classic ? "Log Off " OS_NAME : "Log Off " OS_NAME, 1);
    if (r == 0) { welcome_screen(); wm_redraw_all(); }
    else if (r == 1) wm_defer((wm_deferred_fn)shell_logoff, NULL);
}

/* ---------------- message boxes ---------------- */
typedef struct { char *text; int icon, buttons; msgbox_cb cb; void *ctx; int result; } mbox_t;
static void mbox_finish(window_t *w, int r) {
    mbox_t *m = w->data;
    msgbox_cb cb = m->cb; void *ctx = m->ctx;
    m->cb = NULL;
    wm_destroy(w);
    if (cb) cb(ctx, r);
}
static void mbox_paint(window_t *w) {
    mbox_t *m = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    int tx = 16;
    if (m->icon >= 0) { static const int ic[] = { ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_QUESTION }; draw_icon(c, ic[m->icon], 32, 14, 16); tx = 62; }
    draw_text_wrapped(c, FONT_UI, tx, 20, c->w - tx - 16, m->text, T.text, 2);
    const char *labels[3]; int ids[3], n = 0;
    switch (m->buttons) {
    case MBX_OKCANCEL: labels[0] = "OK"; ids[0] = IDOK; labels[1] = "Cancel"; ids[1] = IDCANCEL; n = 2; break;
    case MBX_YESNO: labels[0] = "Yes"; ids[0] = IDYES; labels[1] = "No"; ids[1] = IDNO; n = 2; break;
    case MBX_YESNOCANCEL: labels[0] = "Yes"; ids[0] = IDYES; labels[1] = "No"; ids[1] = IDNO; labels[2] = "Cancel"; ids[2] = IDCANCEL; n = 3; break;
    default: labels[0] = "OK"; ids[0] = IDOK; n = 1;
    }
    int bw = 75, gap = 8, total = n * bw + (n - 1) * gap, bx = (c->w - total) / 2;
    for (int i = 0; i < n; i++)
        if (ui_button_ex(w, R(bx + i * (bw + gap), c->h - 36, bw, 23), labels[i], true, i == 0)) { mbox_finish(w, ids[i]); return; }
}
static void mbox_key(window_t *w, key_event_t *e) {
    if (!e->pressed) return;
    mbox_t *m = w->data;
    if (e->key == KEY_ESC) mbox_finish(w, m->buttons == MBX_OK ? IDOK : m->buttons == MBX_YESNO ? IDNO : IDCANCEL);
}
static bool mbox_close(window_t *w) { mbox_t *m = w->data; msgbox_cb cb = m->cb; m->cb = NULL; if (cb) cb(m->ctx, m->buttons == MBX_OK ? IDOK : IDCANCEL); return true; }
static void mbox_destroy(window_t *w) { mbox_t *m = w->data; kfree(m->text); kfree(m); }
window_t *msgbox(const char *title, const char *text, int icon, int buttons, msgbox_cb cb, void *ctx) {
    int tw = MIN(440, MAX(200, text_width(FONT_UI, text) + 20));
    int tx = icon >= 0 ? 62 : 16;
    int th = text_wrapped_height(FONT_UI, tw, text, 2);
    int cw = MAX(tw + tx + 16, 260), ch = MAX(th, 32) + 36 + 46;
    window_t *w = wm_create(title, -1, -1, cw, ch, WF_NO_MAX | WF_NO_MIN);
    mbox_t *m = kzalloc(sizeof(mbox_t));
    m->text = strdup(text); m->icon = icon; m->buttons = buttons; m->cb = cb; m->ctx = ctx;
    w->data = m; w->paint = mbox_paint; w->key = mbox_key; w->close = mbox_close; w->destroy = mbox_destroy;
    w->icon = icon == MBI_ERROR ? ICON_ERROR : icon == MBI_WARNING ? ICON_WARNING : ICON_INFO;
    if (wm_active && wm_active != w) { w->owner = wm_active; w->flags |= WF_MODAL; }
    wm_center(w);
    return w;
}

/* ---------------- input box ---------------- */
typedef struct { char prompt[128]; char buf[256]; void (*cb)(void *, const char *); void *ctx; bool first; } ibox_t;
static void ibox_paint(window_t *w) {
    ibox_t *b = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    draw_text(c, FONT_UI, 12, 14, b->prompt, T.text);
    if (b->first) { w->ui.focus = 1; b->first = false; }
    bool enter = ui_textbox(w, R(12, 34, c->w - 24, 22), b->buf, sizeof b->buf);
    bool ok = ui_button_ex(w, R(c->w - 170, c->h - 34, 75, 23), "OK", true, false);
    bool cancel = ui_button(w, R(c->w - 87, c->h - 34, 75, 23), "Cancel");
    if (enter || ok) { void (*cb)(void *, const char *) = b->cb; void *ctx = b->ctx; char tmp[256]; strcpy(tmp, b->buf); wm_destroy(w); if (cb) cb(ctx, tmp); }
    else if (cancel) wm_destroy(w);
}
void input_box(const char *title, const char *prompt, const char *initial, void (*cb)(void *, const char *), void *ctx) {
    window_t *w = wm_create(title, -1, -1, 340, 104, WF_NO_MAX | WF_NO_MIN);
    ibox_t *b = kzalloc(sizeof(ibox_t));
    strlcpy(b->prompt, prompt, sizeof b->prompt); strlcpy(b->buf, initial ? initial : "", sizeof b->buf);
    b->cb = cb; b->ctx = ctx; b->first = true;
    w->data = b; w->paint = ibox_paint; w->icon = ICON_RUN;
    w->destroy = (void (*)(window_t *))0;
    wm_center(w);
}

/* ---------------- Run dialog ---------------- */
static char run_buf[256] = "cmd";
static void run_paint(window_t *w) {
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    draw_icon(c, ICON_RUN, 32, 12, 14);
    draw_text_wrapped(c, FONT_UI, 56, 14, c->w - 70, "Type the name of a program, folder, document, or script, and " OS_NAME " will open it for you.", T.text, 2);
    draw_text(c, FONT_UI, 12, 64, "Open:", T.text);
    if (!w->ui.focus) w->ui.focus = 1;
    bool enter = ui_textbox(w, R(56, 60, c->w - 68, 22), run_buf, sizeof run_buf);
    bool ok = ui_button_ex(w, R(c->w - 250, c->h - 34, 75, 23), "OK", true, false);
    bool cancel = ui_button(w, R(c->w - 167, c->h - 34, 75, 23), "Cancel");
    bool browse = ui_button(w, R(c->w - 84, c->h - 34, 75, 23), "Browse...");
    if (enter || ok) {
        char cmd[256]; strcpy(cmd, run_buf);
        wm_destroy(w);
        if (!shell_execute(cmd, NULL)) {
            char m[300]; snprintf(m, sizeof m, OS_NAME " cannot find '%s'. Make sure you typed the name correctly, and then try again.", cmd);
            msgbox(cmd, m, MBI_ERROR, MBX_OK, NULL, NULL);
        }
    } else if (cancel) wm_destroy(w);
    else if (browse) { wm_destroy(w); explorer_open("C:\\"); }
}
void shell_show_run(void) {
    window_t *w = wm_create("Run", 8, SCREEN_H - T.taskbar_h - 190, 350, 132, WF_NO_MAX | WF_NO_MIN);
    w->paint = run_paint; w->icon = ICON_RUN;
}
