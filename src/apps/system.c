/* System windows: Task Manager, About, Properties; and the BMP writer Paint uses.
 * The accessories are in src/apps/accessories, the games in src/apps/games. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

void file_dialog(const char *title, const char *initial, bool save, const char *ext, void (*cb)(void *, const char *), void *ctx);

static void free_data(window_t *w) { kfree(w->data); }

/* ======================= BMP ======================= */
/* bmp_load is in Rust (rust/src/imgdec.rs) */
uint8_t *bmp_save(canvas_t *c, uint32_t *size) {
    uint32_t stride = (c->w * 3 + 3) & ~3u, img = stride * c->h, total = 54 + img;
    uint8_t *d = kzalloc(total);
    d[0] = 'B'; d[1] = 'M'; *(uint32_t *)(d + 2) = total; *(uint32_t *)(d + 10) = 54; *(uint32_t *)(d + 14) = 40;
    *(int32_t *)(d + 18) = c->w; *(int32_t *)(d + 22) = c->h; *(uint16_t *)(d + 26) = 1; *(uint16_t *)(d + 28) = 24; *(uint32_t *)(d + 34) = img;
    for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) {
        uint32_t p = c->px[y * c->stride + x]; uint8_t *o = d + 54 + stride * (c->h - 1 - y) + x * 3;
        o[0] = C_B(p); o[1] = C_G(p); o[2] = C_R(p);
    }
    *size = total;
    return d;
}

/* ======================= Task Manager ======================= */
typedef struct { int tab, sel, scroll; uint8_t cpu[120], mem[120]; uint32_t last_idle, last_tick; int cpu_now; } tm_t;
static void tm_sample(window_t *w) {
    tm_t *t = w->data;
    uint32_t now = timer_ticks(), idle = idle_ticks;
    uint32_t dt = now - t->last_tick, di = idle - t->last_idle;
    int pct = dt ? 100 - (int)(di * 100 / dt) : 0;
    t->cpu_now = CLAMP(pct, 0, 100);
    memmove(t->cpu, t->cpu + 1, 119); t->cpu[119] = t->cpu_now;
    memmove(t->mem, t->mem + 1, 119); t->mem[119] = mm_used() * 100 / mm_total();
    t->last_idle = idle; t->last_tick = now;
    w->dirty = true;
}
static void graph(canvas_t *c, rect_t r, uint8_t *data, uint32_t col) {
    fill_rect(c, r.x, r.y, r.w, r.h, BLACK);
    for (int x = 0; x < r.w; x += 12) vline(c, r.x + x, r.y, r.h, HEX(0x008040));
    for (int y = 0; y < r.h; y += 12) hline(c, r.x, r.y + y, r.w, HEX(0x008040));
    int px = -1, py = 0;
    for (int i = 0; i < 120; i++) {
        int x = r.x + i * (r.w - 1) / 119, y = r.y + r.h - 1 - data[i] * (r.h - 2) / 100;
        if (px >= 0) draw_line(c, px, py, x, y, col);
        px = x; py = y;
    }
}
static void tm_paint(window_t *w) {
    tm_t *t = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    const char *tabs[] = { "Applications", "Processes", "Performance" };
    int tx = 6;
    for (int i = 0; i < 3; i++) { int tw = text_width(FONT_UI, tabs[i]) + 20; if (ui_tab(w, R(tx, 6, tw, 22), tabs[i], t->tab == i)) { t->tab = i; t->sel = -1; } tx += tw + 1; }
    rect_t body = R(6, 28, c->w - 12, c->h - 28 - 56);
    if (T.classic) bevel_deep(c, body.x, body.y, body.w, body.h, true); else draw_rect(c, body.x, body.y, body.w, body.h, T.face_dark);
    fill_rect(c, body.x + 1, body.y + 1, body.w - 2, body.h - 2, T.classic ? T.face : T.window_bg);
    rect_t lr = R(body.x + 10, body.y + 10, body.w - 20, body.h - 54);
    if (t->tab == 0) {
        draw_sunken_box(c, lr, T.window_bg);
        fill_rect(c, lr.x + 1, lr.y + 1, lr.w - 2, 20, T.face); draw_text(c, FONT_UI, lr.x + 8, lr.y + 4, "Task", T.text); draw_text(c, FONT_UI, lr.x + lr.w - 100, lr.y + 4, "Status", T.text);
        int n = 0; window_t *list[64];
        for (int i = 0; i < wm_window_count(); i++) { window_t *x = wm_window_at(i); if (x != w && !(x->flags & (WF_POPUP | WF_HIDDEN))) list[n++] = x; }
        for (int i = 0; i < n; i++) {
            rect_t r = R(lr.x + 2, lr.y + 22 + i * 20, lr.w - 4, 20);
            if (ui_click_area(w, r)) t->sel = i;
            if (w->ui.dblclick && t->sel == i) wm_focus(list[i]);
            if (t->sel == i) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
            draw_icon(c, list[i]->icon >= 0 ? list[i]->icon : ICON_EXE, 16, r.x + 4, r.y + 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 26, r.y + 3, r.w - 140, list[i]->title, t->sel == i ? T.select_text : T.text);
            draw_text(c, FONT_UI, lr.x + lr.w - 100, r.y + 3, list[i]->state == WS_MINIMIZED ? "Minimized" : "Running", t->sel == i ? T.select_text : T.text);
        }
        bool has = t->sel >= 0 && t->sel < n;
        if (ui_button_ex(w, R(body.x + body.w - 260, body.y + body.h - 36, 80, 24), "End Task", has, false) && has) wm_close(list[t->sel]);
        if (ui_button_ex(w, R(body.x + body.w - 172, body.y + body.h - 36, 80, 24), "Switch To", has, false) && has) wm_focus(list[t->sel]);
        if (ui_button(w, R(body.x + body.w - 84, body.y + body.h - 36, 74, 24), "New Task...")) shell_show_run();
    } else if (t->tab == 1) {
        draw_sunken_box(c, lr, T.window_bg);
        static const char *kinds[] = { "System", "Shell", "Win32", "App" };
        const char *hd[] = { "Image Name", "PID", "Type", "CPU time", "Mem Usage" }; int hx[] = { 8, 190, 240, 310, 400 };
        fill_rect(c, lr.x + 1, lr.y + 1, lr.w - 2, 20, T.face);
        for (int k = 0; k < 5; k++) draw_text(c, FONT_UI, lr.x + hx[k], lr.y + 4, hd[k], T.text);
        int i = 0; task_t *selt = NULL;
        for (task_t *k = task_list(); k; k = k->next, i++) {
            rect_t r = R(lr.x + 2, lr.y + 22 + i * 20, lr.w - 4, 20);
            if (r.y + 20 > lr.y + lr.h) break;
            if (ui_click_area(w, r)) t->sel = i;
            bool s = t->sel == i; if (s) selt = k;
            if (s) fill_rect(c, r.x, r.y, r.w, r.h, T.select);
            uint32_t col = s ? T.select_text : T.text; char b[48];
            draw_text_ellipsis(c, FONT_UI, lr.x + hx[0], r.y + 3, 175, k->name, col);
            snprintf(b, sizeof b, "%d", k->pid); draw_text(c, FONT_UI, lr.x + hx[1], r.y + 3, b, col);
            draw_text(c, FONT_UI, lr.x + hx[2], r.y + 3, kinds[k->kind & 3], col);
            uint32_t cs = k->pid == 0 ? 0 : k->cpu_ms / 1000; snprintf(b, sizeof b, "%u:%02u:%02u", cs / 3600, (cs / 60) % 60, cs % 60); draw_text(c, FONT_UI, lr.x + hx[3], r.y + 3, b, col);
            snprintf(b, sizeof b, "%u K", k->mem_bytes / 1024); draw_text(c, FONT_UI, lr.x + hx[4], r.y + 3, b, col);
        }
        bool can = selt && selt->pid != 0;
        if (ui_button_ex(w, R(body.x + body.w - 104, body.y + body.h - 36, 94, 24), "End Process", can, false) && can) { task_kill(selt->pid); t->sel = -1; }
    } else {
        char b[64];
        int gw = (body.w - 40) / 4;
        draw_group(c, R(body.x + 10, body.y + 8, gw, 120), "CPU Usage");
        fill_rect(c, body.x + 20, body.y + 28, gw - 20, 90, BLACK);
        int bh = t->cpu_now * 70 / 100;
        for (int y = 0; y < 70; y += 3) fill_rect(c, body.x + 30, body.y + 34 + y, gw - 40, 2, (70 - y) <= bh ? HEX(0x00FF00) : HEX(0x004000));
        snprintf(b, sizeof b, "%d %%", t->cpu_now); draw_text_centered(c, FONT_UI, R(body.x + 20, body.y + 104, gw - 20, 14), b, HEX(0x00FF00));
        draw_group(c, R(body.x + 20 + gw, body.y + 8, body.w - gw - 30, 120), "CPU Usage History");
        graph(c, R(body.x + 30 + gw, body.y + 28, body.w - gw - 50, 90), t->cpu, HEX(0x00FF00));
        draw_group(c, R(body.x + 10, body.y + 136, gw, 120), "Heap Usage");
        fill_rect(c, body.x + 20, body.y + 156, gw - 20, 90, BLACK);
        snprintf(b, sizeof b, "%u MB", mm_used() / (1024 * 1024)); draw_text_centered(c, FONT_UI, R(body.x + 20, body.y + 230, gw - 20, 14), b, HEX(0x00FF00));
        int mh = t->mem[119] * 70 / 100;
        for (int y = 0; y < 70; y += 3) fill_rect(c, body.x + 30, body.y + 160 + y, gw - 40, 2, (70 - y) <= mh ? HEX(0xFFFF00) : HEX(0x404000));
        draw_group(c, R(body.x + 20 + gw, body.y + 136, body.w - gw - 30, 120), "Memory Usage History");
        graph(c, R(body.x + 30 + gw, body.y + 156, body.w - gw - 50, 90), t->mem, HEX(0xFFFF00));
        int y = body.y + 266;
        snprintf(b, sizeof b, "Physical memory: %llu MB", (unsigned long long)(mm_phys_total() / (1024 * 1024))); draw_text(c, FONT_UI, body.x + 14, y, b, T.text);
        snprintf(b, sizeof b, "Kernel heap: %u / %u KB", mm_used() / 1024, mm_total() / 1024); draw_text(c, FONT_UI, body.x + 14, y + 18, b, T.text);
        uint32_t up = timer_ticks() / 1000; snprintf(b, sizeof b, "Up time: %u:%02u:%02u   Tasks: %d   Open windows: %d", up / 3600, (up / 60) % 60, up % 60, task_count(), wm_window_count());
        draw_text(c, FONT_UI, body.x + 260, y, b, T.text);
    }
    char st[96]; snprintf(st, sizeof st, "Processes: %d     CPU Usage: %d%%     Heap: %u MB / %u MB", task_count(), t->cpu_now, mm_used() >> 20, mm_total() >> 20);
    hline(c, 0, c->h - 22, c->w, T.face_dark);
    draw_text(c, FONT_UI, 8, c->h - 18, st, T.text);
}
static window_t *tm_win;
static void tm_destroy(window_t *w) { kfree(w->data); tm_win = NULL; }
void taskmgr_open(void) {
    if (tm_win && wm_alive(tm_win)) { wm_focus(tm_win); return; }
    window_t *w = wm_create(OS_NAME " Task Manager", -1, -1, 540, 470, WF_RESIZABLE);
    tm_t *t = kzalloc(sizeof *t); t->sel = -1; t->tab = 1; t->last_idle = idle_ticks; t->last_tick = timer_ticks();
    w->data = t; w->paint = tm_paint; w->destroy = tm_destroy; w->icon = ICON_TASKMGR; w->min_w = 460; w->min_h = 400;
    wm_set_timer(w, 1000, tm_sample);
    tm_win = w;
}

/* ======================= About ======================= */
static void about_paint(window_t *w) {
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    gradient_h(c, 0, 0, c->w, 78, HEX(0x0A3CA8), HEX(0x5A8EF0));
    draw_globe(c, 10, 7, 64, (int)(timer_ticks() / 40));
    draw_wordmark(c, 84, 6, WORDMARK_MEDIUM, WHITE);
    draw_text(c, FONT_UI, 86, 58, OS_EDITION, HEX(0xE0E8FF));
    hline(c, 0, 78, c->w, HEX(0xF09A3C));
    int y = 92;
    const char *lines[] = { "Version " OS_VERSION " (Build " OS_BUILD ")", OS_KERNEL_DESC,
        "Drivers: PS/2 keyboard & mouse, RTC, PCI, VBE framebuffer, PC speaker", "Win32 subsystem for " OS_EXE_DESC " .exe programs", "POSIX-style shell for .sh scripts",
        "UDIF .dmg disk images with FAT volumes and .app bundles", "", "Fonts: Liberation family (SIL Open Font License).", "Icons and wallpapers are original procedural artwork." };
    for (unsigned i = 0; i < ARRAY_LEN(lines); i++) { draw_text(c, FONT_UI, 20, y, lines[i], T.text); y += 17; }
    char b[96]; snprintf(b, sizeof b, "Physical memory available to " OS_NAME ": %u KB", mm_phys_total() / 1024);
    draw_text(c, FONT_UI, 20, y + 6, b, T.gray_text);
    snprintf(b, sizeof b, "This product is licensed to: %s", user_name);
    draw_text(c, FONT_UI, 20, y + 24, b, T.gray_text);
    char st[120]; persist_status(st, sizeof st);
    snprintf(b, sizeof b, "%s: %s", system_volume ? "Installed on" : "Running as", st);
    draw_text_ellipsis(c, FONT_UI, 20, y + 42, c->w - 40, b, T.gray_text);
    if (ui_button_ex(w, R(c->w - 90, c->h - 34, 76, 24), "OK", true, true)) wm_destroy(w);
}
static void about_tick(window_t *w) { w->dirty = true; }
void about_open(void) {
    window_t *w = wm_create("About " OS_NAME, -1, -1, 470, 352, WF_NO_MAX | WF_NO_MIN);
    w->paint = about_paint; w->icon = ICON_ABOUT;
    wm_set_timer(w, 50, about_tick);
    wm_center(w);
}

/* ======================= Properties ======================= */
typedef struct { vnode_t *n; bool ro; char info[200]; } prop_t;
static void prop_paint(window_t *w) {
    prop_t *p = w->data; canvas_t *c = w->surf; vnode_t *n = p->n;
    gfx_clear(c, T.face);
    ui_tab(w, R(6, 6, 70, 22), "General", true);
    rect_t body = R(6, 28, c->w - 12, c->h - 72);
    fill_rect(c, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(c, body.x, body.y, body.w, body.h, T.face_dark);
    draw_icon(c, file_icon(n), 32, 22, 42);
    draw_sunken_box(c, R(80, 46, body.w - 90, 22), T.window_bg);
    draw_text_ellipsis(c, FONT_UI, 86, 50, body.w - 100, !n->parent ? vfs_drive_label(n->drive) : n->name, T.text);
    hline(c, 16, 84, body.w - 20, T.face_dark);
    char path[256], b[160];
    int y = 96;
    #define ROW(label, val) do { draw_text(c, FONT_UI, 20, y, label, T.text); draw_text_wrapped(c, FONT_UI, 110, y, body.w - 110, val, T.text, 1); y += 20; } while (0)
    ROW("Type:", file_type_name(n));
    if (n->parent) { vfs_path(n->parent, path, sizeof path); ROW("Location:", path); }
    uint32_t sz = vfs_tree_size(n); format_size(b, sizeof b, sz);
    char b2[200]; snprintf(b2, sizeof b2, "%s (%u bytes)", b, sz); ROW("Size:", b2);
    if (n->dir) { snprintf(b, sizeof b, "%d items", vfs_count_children(n)); ROW("Contains:", b); }
    hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 10;
    snprintf(b, sizeof b, "%02d/%02d/%04d %02d:%02d:%02d", n->mtime.month, n->mtime.day, n->mtime.year, n->mtime.hour, n->mtime.minute, n->mtime.second); ROW("Modified:", b);
    if (p->info[0]) { hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 10; draw_text(c, FONT_UI, 20, y, "Details:", T.text); y += draw_text_wrapped(c, FONT_UI, 110, y, body.w - 110, p->info, T.text, 1) + 6; }
    hline(c, 16, y + 2, body.w - 20, T.face_dark); y += 12;
    draw_text(c, FONT_UI, 20, y, "Attributes:", T.text);
    bool was = p->ro;
    ui_checkbox(w, 110, y, "Read-only", &p->ro);
    if (was != p->ro && (!n->parent || !n->parent->readonly)) n->readonly = p->ro;
    #undef ROW
    if (ui_button_ex(w, R(c->w - 168, c->h - 36, 76, 24), "OK", true, true)) { wm_destroy(w); return; }
    if (ui_button(w, R(c->w - 86, c->h - 36, 76, 24), "Cancel")) wm_destroy(w);
}
void properties_open(vnode_t *n) {
    if (!n) return;
    prop_t *p = kzalloc(sizeof *p); p->n = n; p->ro = n->readonly;
    const char *e = vfs_ext(n->name);
    if (!n->dir && !strcasecmp(e, "exe")) pe_probe(n, p->info, sizeof p->info);
    else if (!n->dir && !strcasecmp(e, "dmg")) { uint32_t sz; uint8_t *d = dmg_extract(n->data, n->size, &sz, p->info, sizeof p->info); kfree(d); }
    else if (!n->dir) macho_probe(n->data, n->size, p->info, sizeof p->info);
    if (!n->parent) snprintf(p->info, sizeof p->info, "%s volume %c:", vfs_drive_kind(n->drive) == 2 ? "Read-only disk image" : "RAM-backed", n->drive);
    char t[160]; snprintf(t, sizeof t, "%s Properties", n->parent ? n->name : vfs_drive_label(n->drive));
    window_t *w = wm_create(t, -1, -1, 360, 420, WF_NO_MAX | WF_NO_MIN);
    w->data = p; w->paint = prop_paint; w->icon = file_icon(n);
    w->destroy = free_data;
    wm_center(w);
}
