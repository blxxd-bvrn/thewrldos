/* Logon Skin Maker and Logon Skin Themer.
 *
 * The Maker edits a skin (layout, text, background picture, colours, options)
 * with a live preview and saves it as a .wrldlogon file; the Themer shows every
 * skin as a thumbnail and chooses the one the welcome screen uses. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "fs.h"
#include "logon.h"

#define WALLPAPER_DIR "C:\\WRLD\\Wallpapers"

static uint32_t skin_hash(const logon_skin_t *s, uint32_t extra) {
    uint32_t h = 2166136261u ^ extra;
    const uint8_t *p = (const uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) { h ^= p[i]; h *= 16777619u; }
    h ^= (uint32_t)user_picture * 2654435761u;
    for (const char *q = user_name; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
    return h;
}
/* the virtual screen a preview is drawn at: the real one's shape, at most 800 wide */
static void preview_size(int *vw, int *vh) {
    *vw = MIN(800, SCREEN_W); *vh = *vw * SCREEN_H / MAX(SCREEN_W, 1);
    if (*vh < 480) { *vh = 480; *vw = *vh * SCREEN_W / MAX(SCREEN_H, 1); }
}
static logon_skin_t fs_skin;
static void fullscreen_cb(void *arg) { (void)arg; logon_fullscreen_preview(&fs_skin); }
static void show_fullscreen(const logon_skin_t *s) { fs_skin = *s; wm_defer(fullscreen_cb, NULL); }

/* ======================= Logon Skin Maker ======================= */
typedef struct {
    logon_skin_t sk;
    int tab;                        /* 0 general, 1 colours */
    int base_sel;
    int role, role_scroll;
    int r, g, b, role_shown;
    int wp_sel;                     /* background picture dropdown */
    char *wp_names[40]; int nwp;
    canvas_t *prev; uint32_t prev_key;
    bool dirty;
    char status[96];
} mk_t;

static const uint32_t palette[16] = {
    0x0A1CA6, 0x1A36C4, 0x3A62F2, 0x86ACF0, 0xDCEBFF, 0x8A72C4, 0x7C9ACC, 0x00309C,
    0x1E6B2E, 0x6E8A3A, 0xD8442A, 0xE88A30, 0xFFC830, 0x20202A, 0x08123C, 0xFFFFFF,
};

static void mk_scan_wallpapers(mk_t *m) {
    for (int i = 0; i < m->nwp; i++) kfree(m->wp_names[i]);
    m->nwp = 0;
    m->wp_names[m->nwp++] = strdup("(none - drawn background)");
    vnode_t *d = vfs_resolve(WALLPAPER_DIR, NULL);
    if (d) {
        vfs_sort_children(d);
        for (vnode_t *n = d->child; n && m->nwp < 40; n = n->next) {
            const char *e = vfs_ext(n->name);
            if (n->dir || (strcasecmp(e, "png") && strcasecmp(e, "jpg") && strcasecmp(e, "jpeg") && strcasecmp(e, "bmp") && strcasecmp(e, "gif"))) continue;
            m->wp_names[m->nwp++] = strdup(n->name);
        }
    }
    m->wp_sel = 0;
    for (int i = 1; i < m->nwp; i++) {
        char p[200]; snprintf(p, sizeof p, WALLPAPER_DIR "\\%s", m->wp_names[i]);
        if (!strcasecmp(p, m->sk.image)) m->wp_sel = i;
    }
}
static void mk_load(mk_t *m, const logon_skin_t *from, bool copy) {
    m->sk = *from;
    m->sk.builtin = false;
    if (copy) {
        char n[48]; snprintf(n, sizeof n, "My %s", from->name);
        strlcpy(m->sk.name, n, sizeof m->sk.name);
        strlcpy(m->sk.file, n, sizeof m->sk.file);
    }
    m->role_shown = -1;
    mk_scan_wallpapers(m);
}
static void mk_save_as_cb(void *ctx, int r) { (void)ctx; (void)r; }
static bool mk_save(mk_t *m, bool use) {
    if (!m->sk.name[0]) { msgbox("Logon Skin Maker", "Give the skin a name first.", MBI_WARNING, MBX_OK, NULL, NULL); return false; }
    strlcpy(m->sk.file, m->sk.name, sizeof m->sk.file);
    int i = logon_skin_find(m->sk.file);
    if (i >= 0 && logon_skin_get(i)->builtin && !strcasecmp(logon_skin_get(i)->file, m->sk.file)) {
        msgbox("Logon Skin Maker", "That name belongs to a skin that comes with " OS_NAME ". Choose another name, so the original stays as it is.",
               MBI_WARNING, MBX_OK, mk_save_as_cb, NULL);
        return false;
    }
    if (!logon_skin_save(&m->sk)) { msgbox("Logon Skin Maker", "The skin could not be saved.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    int j = logon_skin_find(m->sk.name);
    if (use && j >= 0) logon_skin_set_current(logon_skin_get(j)->file);
    snprintf(m->status, sizeof m->status, use ? "Saved \"%s\" and set it as the welcome screen." : "Saved \"%s\".", m->sk.name);
    m->dirty = false;
    return true;
}
static void mk_hex_cb(void *ctx, const char *text) {
    window_t *w = ctx;
    if (!text || !wm_alive(w)) return;
    mk_t *m = w->data;
    const char *p = text; while (*p == ' ' || *p == '#') p++;
    if (!isxdigit(*p)) return;
    m->sk.col[m->role] = 0xFF000000u | (strtoul(p, NULL, 16) & 0xFFFFFF);
    m->role_shown = -1; m->dirty = true;
    w->dirty = true;
}
static void mk_general(window_t *w, mk_t *m, int x, int y, int cw) {
    canvas_t *s = w->surf;
    int n = logon_skin_count();
    const char *names[32];
    for (int i = 0; i < n && i < 32; i++) names[i] = logon_skin_get(i)->name;
    draw_text(s, FONT_UI, x, y + 4, "Start from:", T.text);
    ui_dropdown(w, R(x + 80, y, cw - 80 - 66, 22), names, MIN(n, 32), &m->base_sel);
    if (ui_button(w, R(x + cw - 60, y, 60, 22), "Load")) { mk_load(m, logon_skin_get(m->base_sel), true); m->dirty = true; }
    y += 34;
    draw_text(s, FONT_UI, x, y + 4, "Name:", T.text);
    if (ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.name, sizeof m->sk.name)) {}
    y += 28;
    draw_text(s, FONT_UI, x, y + 4, "Description:", T.text);
    ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.desc, sizeof m->sk.desc);
    y += 34;
    draw_group(s, R(x, y, cw, 50), "Layout");
    if (ui_radio(w, x + 12, y + 20, "Sign-in screen", m->sk.layout == LOGON_LAYOUT_SIGNIN) && m->sk.layout != LOGON_LAYOUT_SIGNIN) {
        logon_skin_t d; logon_skin_defaults(&d, LOGON_LAYOUT_SIGNIN);
        memcpy(m->sk.col, d.col, sizeof d.col);
        strlcpy(m->sk.title, d.title, sizeof m->sk.title); strlcpy(m->sk.subtitle, d.subtitle, sizeof m->sk.subtitle);
        strlcpy(m->sk.brand, d.brand, sizeof m->sk.brand);
        m->sk.layout = LOGON_LAYOUT_SIGNIN; m->role_shown = -1;
    }
    if (ui_radio(w, x + 150, y + 20, "Classic (XP)", m->sk.layout == LOGON_LAYOUT_CLASSIC) && m->sk.layout != LOGON_LAYOUT_CLASSIC) {
        logon_skin_t d; logon_skin_defaults(&d, LOGON_LAYOUT_CLASSIC);
        memcpy(m->sk.col, d.col, sizeof d.col);
        strlcpy(m->sk.title, d.title, sizeof m->sk.title); strlcpy(m->sk.subtitle, d.subtitle, sizeof m->sk.subtitle);
        m->sk.layout = LOGON_LAYOUT_CLASSIC; m->role_shown = -1;
    }
    y += 62;
    bool signin = m->sk.layout == LOGON_LAYOUT_SIGNIN;
    draw_text(s, FONT_UI, x, y + 4, signin ? "Title:" : "Signing in:", T.text);
    ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.title, sizeof m->sk.title);
    y += 28;
    draw_text(s, FONT_UI, x, y + 4, signin ? "Subtitle:" : "Prompt:", T.text);
    ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.subtitle, sizeof m->sk.subtitle);
    y += 28;
    if (signin) {
        draw_text(s, FONT_UI, x, y + 4, "Corner text:", T.text);
        ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.brand, sizeof m->sk.brand);
        y += 28;
    }
    y += 6;
    draw_text(s, FONT_UI, x, y + 4, "Picture:", T.text);
    int ws = m->wp_sel;
    ui_dropdown(w, R(x + 80, y, cw - 80, 22), (const char **)m->wp_names, m->nwp, &m->wp_sel);
    if (ws != m->wp_sel) {
        if (m->wp_sel <= 0) m->sk.image[0] = 0;
        else snprintf(m->sk.image, sizeof m->sk.image, WALLPAPER_DIR "\\%s", m->wp_names[m->wp_sel]);
    }
    y += 26;
    draw_text(s, FONT_SMALL, x + 80, y, "Or type the path of any PNG, JPEG, GIF or BMP:", T.gray_text);
    y += 14;
    ui_textbox(w, R(x + 80, y, cw - 80, 22), m->sk.image, sizeof m->sk.image);
    y += 34;
    draw_group(s, R(x, y, cw, signin ? 74 : 50), "Show");
    ui_checkbox(w, x + 12, y + 20, "Globe", &m->sk.show_globe);
    if (signin) {
        ui_checkbox(w, x + 150, y + 20, "Clock and date", &m->sk.show_clock);
        ui_checkbox(w, x + 12, y + 44, "Sign in as Guest", &m->sk.show_guest);
        ui_checkbox(w, x + 150, y + 44, "Help and Settings", &m->sk.show_help);
    }
}
static void mk_colours(window_t *w, mk_t *m, int x, int y, int cw, int ch) {
    canvas_t *s = w->surf;
    const char *items[LC_COUNT]; int map[LC_COUNT], n = 0;
    for (int i = 0; i < LC_COUNT; i++) {
        const char *l = logon_color_label(i, m->sk.layout);
        if (l) { items[n] = l; map[n++] = i; }
    }
    int sel = 0;
    for (int i = 0; i < n; i++) if (map[i] == m->role) sel = i;
    int lh = MIN(ch - 230, n * (font_height(FONT_UI) + 4) + 6);
    ui_list(w, R(x, y, cw, lh), items, n, &sel, &m->role_scroll);
    /* swatches beside the names */
    rect_t old = canvas_push_clip(s, R(x + 2, y + 2, cw - 4, lh - 4));
    int ih = font_height(FONT_UI) + 4, vis = (lh - 4) / ih;
    for (int i = m->role_scroll; i < n && i < m->role_scroll + vis + 1; i++) {
        int yy = y + 2 + (i - m->role_scroll) * ih;
        int sx = x + cw - (n > vis ? 18 : 0) - 34;
        fill_rect(s, sx, yy + 2, 26, ih - 4, m->sk.col[map[i]]);
        draw_rect(s, sx, yy + 2, 26, ih - 4, T.face_dark);
    }
    s->clip = old;
    m->role = map[CLAMP(sel, 0, n - 1)];
    if (m->role_shown != m->role) {
        m->role_shown = m->role;
        uint32_t c = m->sk.col[m->role];
        m->r = C_R(c); m->g = C_G(c); m->b = C_B(c);
    }
    y += lh + 12;
    uint32_t c = m->sk.col[m->role];
    fill_rect(s, x, y, 70, 40, c); draw_rect(s, x, y, 70, 40, T.face_dark);
    char hx[16]; snprintf(hx, sizeof hx, "#%06X", c & 0xFFFFFF);
    draw_text(s, FONT_UI_BOLD, x + 82, y + 4, logon_color_label(m->role, m->sk.layout), T.text);
    draw_text(s, FONT_UI, x + 82, y + 22, hx, T.gray_text);
    if (ui_button(w, R(x + cw - 76, y + 16, 76, 22), "Hex...")) input_box("Colour", "Type a colour as #RRGGBB:", hx, mk_hex_cb, w);
    y += 50;
    const char *lab[3] = { "Red", "Green", "Blue" };
    int *vv[3] = { &m->r, &m->g, &m->b };
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        draw_text(s, FONT_UI, x, y + 4, lab[i], T.text);
        int v = *vv[i];
        if (ui_slider(w, R(x + 46, y, cw - 46 - 34, 22), &v, 0, 255)) { *vv[i] = v; changed = true; }
        char b[8]; snprintf(b, sizeof b, "%d", *vv[i]);
        draw_text(s, FONT_UI, x + cw - 28, y + 4, b, T.text);
        y += 28;
    }
    if (changed) { m->sk.col[m->role] = RGB(m->r, m->g, m->b); m->dirty = true; }
    y += 4;
    draw_text(s, FONT_UI, x, y, "Quick colours:", T.gray_text);
    y += 16;
    int sw = (cw - 7 * 4) / 8;
    for (int i = 0; i < 16; i++) {
        rect_t r = R(x + (i % 8) * (sw + 4), y + (i / 8) * 22, sw, 18);
        if (ui_click_area(w, r)) { m->sk.col[m->role] = HEX(palette[i]); m->role_shown = -1; m->dirty = true; }
        fill_rect(s, r.x, r.y, r.w, r.h, HEX(palette[i]));
        draw_rect(s, r.x, r.y, r.w, r.h, ui_hover(w, r) ? T.select : T.face_dark);
    }
}
static void mk_paint(window_t *w) {
    mk_t *m = w->data;
    canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    int lw = 330, pad = 12;
    /* left: the settings */
    fill_rect(s, 0, 0, lw + pad * 2, s->h, T.window_bg);
    vline(s, lw + pad * 2, 0, s->h, T.face_dark);
    if (ui_tab(w, R(pad, pad, 90, 24), "General", m->tab == 0)) m->tab = 0;
    if (ui_tab(w, R(pad + 92, pad, 90, 24), "Colours", m->tab == 1)) m->tab = 1;
    hline(s, pad, pad + 24, lw, T.face_dark);
    logon_skin_t before = m->sk;
    if (m->tab == 0) mk_general(w, m, pad, pad + 36, lw);
    else mk_colours(w, m, pad, pad + 36, lw, s->h - pad - 36);
    if (memcmp(&before, &m->sk, sizeof before)) m->dirty = true;

    /* right: the preview */
    int px = lw + pad * 3, pw = s->w - px - pad;
    draw_text(s, FONT_LARGE, px, pad, "Preview", T.text);
    char sub[96]; snprintf(sub, sizeof sub, "%s  -  %s%s", m->sk.name[0] ? m->sk.name : "(no name)", logon_layout_name(m->sk.layout), m->dirty ? "  (not saved)" : "");
    draw_text_ellipsis(s, FONT_UI, px, pad + 26, pw, sub, T.gray_text);
    int vw, vh; preview_size(&vw, &vh);
    int bh = s->h - pad * 2 - 46 - 80;
    int ww = pw, hh = ww * vh / vw;
    if (hh > bh) { hh = bh; ww = hh * vw / vh; }
    rect_t pr = R(px + (pw - ww) / 2, pad + 46, MAX(ww, 1), MAX(hh, 1));
    uint32_t key = skin_hash(&m->sk, (uint32_t)(pr.w * 7919 + pr.h));
    if (!m->prev || m->prev_key != key || m->prev->w != pr.w || m->prev->h != pr.h) {
        if (m->prev && (m->prev->w != pr.w || m->prev->h != pr.h)) { canvas_free(m->prev); m->prev = NULL; }
        if (!m->prev) m->prev = canvas_new(pr.w, pr.h);
        if (m->prev) logon_preview(m->prev, R(0, 0, pr.w, pr.h), &m->sk, vw, vh);
        m->prev_key = key;
    }
    fill_rect(s, pr.x - 1, pr.y - 1, pr.w + 2, pr.h + 2, BLACK);
    if (m->prev) blit(s, pr.x, pr.y, m->prev, 0, 0, pr.w, pr.h);
    int by = pr.y + pr.h + 14;
    if (ui_button(w, R(px, by, 130, 26), "Preview full screen")) show_fullscreen(&m->sk);
    if (ui_button(w, R(px + 138, by, 80, 26), "New")) { logon_skin_t d; logon_skin_defaults(&d, m->sk.layout); strlcpy(d.name, "My Skin", sizeof d.name); mk_load(m, &d, false); m->dirty = true; m->status[0] = 0; }
    if (ui_button(w, R(px + pw - 196, by, 80, 26), "Save") && mk_save(m, false)) w->dirty = true;
    if (ui_button_ex(w, R(px + pw - 110, by, 110, 26), "Save and use", true, true) && mk_save(m, true)) w->dirty = true;
    if (m->status[0]) draw_text_ellipsis(s, FONT_UI, px, by + 36, pw, m->status, T.gray_text);
    else draw_text_ellipsis(s, FONT_UI, px, by + 36, pw, "Skins are saved in " LOGON_SKIN_DIR ".", T.gray_text);
}
static void mk_destroy(window_t *w) {
    mk_t *m = w->data;
    for (int i = 0; i < m->nwp; i++) kfree(m->wp_names[i]);
    if (m->prev) canvas_free(m->prev);
    kfree(m);
}
void logonmaker_open(const char *skin) {
    window_t *w = wm_create("Logon Skin Maker", -1, -1, MIN(940, SCREEN_W - 40), MIN(600, SCREEN_H - 70), WF_RESIZABLE);
    mk_t *m = kzalloc(sizeof *m);
    int i = skin ? logon_skin_find(skin) : -1;
    if (skin && i < 0) {
        /* a path to a .wrldlogon somewhere else: Maker edits skins in the skin folder */
        char f[96]; strlcpy(f, path_basename(skin), sizeof f);
        char *dot = strrchr(f, '.'); if (dot) *dot = 0;
        i = logon_skin_find(f);
    }
    if (i >= 0) {
        const logon_skin_t *s = logon_skin_get(i);
        mk_load(m, s, s->builtin);
        m->base_sel = i;
    } else {
        m->base_sel = logon_skin_current_index();
        mk_load(m, logon_skin_get(m->base_sel), true);
    }
    m->role = LC_BG_TOP;
    w->data = m; w->paint = mk_paint; w->destroy = mk_destroy;
    w->icon = ICON_CP_APPEARANCE; w->min_w = 720; w->min_h = 520;
    wm_center(w);
}

/* ======================= Logon Skin Themer ======================= */
#define TH_W 176
#define TH_H 132
typedef struct {
    int sel, scroll;
    canvas_t *thumb[32]; uint32_t thumb_key[32];
    char status[96];
} th_t;
static void th_delete_cb(void *ctx, int r) {
    window_t *w = ctx;
    if (r != IDYES || !wm_alive(w)) return;
    th_t *t = w->data;
    const logon_skin_t *s = logon_skin_get(t->sel);
    char f[64]; strlcpy(f, s->file, sizeof f);
    bool cur = t->sel == logon_skin_current_index();
    logon_skin_delete(f);
    if (cur) logon_skin_set_current(LOGON_DEFAULT_SKIN);
    snprintf(t->status, sizeof t->status, "Deleted \"%s\".", f);
    t->sel = logon_skin_current_index();
    w->dirty = true;
}
static void th_paint(window_t *w) {
    th_t *t = w->data;
    canvas_t *s = w->surf;
    gfx_clear(s, T.window_bg);
    int n = logon_skin_count();
    if (t->sel >= n) t->sel = n - 1;
    int cur = logon_skin_current_index();
    draw_text(s, FONT_LARGE, 16, 12, "Choose a look for the welcome screen", T.text);
    draw_text(s, FONT_UI, 16, 40, "The skin you apply is used the next time you sign in or log off.", T.gray_text);
    int side = 230, gx = 16, gy = 66, gw = s->w - side - 32, gh = s->h - gy - 12;
    draw_sunken_box(s, R(gx, gy, gw, gh), T.window_bg);
    int cols = MAX(1, (gw - 8) / (TH_W + 14)), cellw = (gw - 8) / cols, cellh = TH_H + 34;
    int rows = (n + cols - 1) / cols, vis = MAX(1, (gh - 8) / cellh);
    if (ui_hover(w, R(gx, gy, gw, gh)) && w->ui.wheel) t->scroll = CLAMP(t->scroll + w->ui.wheel, 0, MAX(0, rows - vis));
    t->scroll = CLAMP(t->scroll, 0, MAX(0, rows - vis));
    rect_t old = canvas_push_clip(s, R(gx + 2, gy + 2, gw - 4, gh - 4));
    int vw, vh; preview_size(&vw, &vh);
    for (int i = t->scroll * cols; i < n && i < (t->scroll + vis + 1) * cols; i++) {
        const logon_skin_t *sk = logon_skin_get(i);
        int cx = gx + 4 + (i % cols) * cellw, cy = gy + 4 + (i / cols - t->scroll) * cellh;
        rect_t cell = R(cx + 2, cy + 2, cellw - 4, cellh - 4);
        if (ui_click_area(w, cell)) { t->sel = i; t->status[0] = 0; }
        if (ui_hover(w, cell) && w->ui.dblclick) { logon_skin_set_current(sk->file); snprintf(t->status, sizeof t->status, "\"%s\" will be used at the next sign-in.", sk->name); }
        if (i == t->sel) fill_round_rect(s, cell.x, cell.y, cell.w, cell.h, 4, mix(T.window_bg, T.select, 70));
        else if (ui_hover(w, cell)) fill_round_rect(s, cell.x, cell.y, cell.w, cell.h, 4, mix(T.window_bg, T.select, 30));
        uint32_t key = skin_hash(sk, 1);
        if (!t->thumb[i] || t->thumb_key[i] != key) {
            if (!t->thumb[i]) t->thumb[i] = canvas_new(TH_W, TH_H);
            if (t->thumb[i]) logon_preview(t->thumb[i], R(0, 0, TH_W, TH_H), sk, vw, vh);
            t->thumb_key[i] = key;
        }
        int tx = cx + (cellw - TH_W) / 2, ty = cy + 8;
        fill_rect(s, tx - 1, ty - 1, TH_W + 2, TH_H + 2, i == t->sel ? T.select : T.face_dark);
        if (t->thumb[i]) blit(s, tx, ty, t->thumb[i], 0, 0, TH_W, TH_H);
        char lab[64]; snprintf(lab, sizeof lab, "%s%s", sk->name, i == cur ? " (in use)" : "");
        int lw = MIN(text_width(i == cur ? FONT_UI_BOLD : FONT_UI, lab), cellw - 8);
        draw_text_ellipsis(s, i == cur ? FONT_UI_BOLD : FONT_UI, cx + (cellw - lw) / 2, ty + TH_H + 6, cellw - 8, lab, T.text);
    }
    s->clip = old;
    if (rows > vis) draw_scrollbar(s, R(gx + gw - 18, gy + 1, 17, gh - 2), rows, vis, t->scroll, true);

    /* the details */
    const logon_skin_t *sk = logon_skin_get(t->sel);
    int x = s->w - side - 8, y = gy;
    draw_text_ellipsis(s, FONT_LARGE_BOLD, x, y, side - 8, sk->name, T.text);
    y += 28;
    draw_text(s, FONT_UI, x, y, logon_layout_name(sk->layout), T.gray_text);
    y += 18;
    if (sk->builtin) { draw_text(s, FONT_UI, x, y, "Comes with " OS_NAME, T.gray_text); y += 18; }
    y += 6;
    if (sk->desc[0]) y += draw_text_wrapped(s, FONT_UI, x, y, side - 8, sk->desc, T.text, 2) + 10;
    if (t->sel == cur) { draw_text(s, FONT_UI_BOLD, x, y, "This skin is in use.", T.text); y += 22; }
    if (ui_button_ex(w, R(x, y, side - 8, 26), "Apply", t->sel != cur, true)) {
        logon_skin_set_current(sk->file);
        snprintf(t->status, sizeof t->status, "\"%s\" will be used at the next sign-in.", sk->name);
    }
    y += 32;
    if (ui_button(w, R(x, y, side - 8, 26), "Preview full screen")) show_fullscreen(sk);
    y += 32;
    if (ui_button(w, R(x, y, side - 8, 26), sk->builtin ? "Make a copy in Skin Maker" : "Edit in Skin Maker")) logonmaker_open(sk->file);
    y += 32;
    if (ui_button(w, R(x, y, side - 8, 26), "New skin...")) logonmaker_open(NULL);
    y += 32;
    if (ui_button_ex(w, R(x, y, side - 8, 26), "Delete", !sk->builtin, false)) {
        char q[128]; snprintf(q, sizeof q, "Delete the logon skin \"%s\"?", sk->name);
        msgbox("Logon Skin Themer", q, MBI_QUESTION, MBX_YESNO, th_delete_cb, w);
    }
    y += 32;
    if (ui_button(w, R(x, y, side - 8, 26), "Refresh list")) { logon_skins_load(); t->status[0] = 0; }
    y += 40;
    if (t->status[0]) draw_text_wrapped(s, FONT_UI, x, y, side - 8, t->status, T.text, 2);
}
static void th_destroy(window_t *w) {
    th_t *t = w->data;
    for (int i = 0; i < 32; i++) if (t->thumb[i]) canvas_free(t->thumb[i]);
    kfree(t);
}
static void th_activate(window_t *w, bool active) { if (active) { logon_skins_load(); w->dirty = true; } }
void logonthemer_open(void) {
    logon_skins_load();
    window_t *w = wm_create("Logon Skin Themer", -1, -1, MIN(860, SCREEN_W - 40), MIN(560, SCREEN_H - 70), WF_RESIZABLE);
    th_t *t = kzalloc(sizeof *t);
    t->sel = logon_skin_current_index();
    w->data = t; w->paint = th_paint; w->destroy = th_destroy; w->activate = th_activate;
    w->icon = ICON_CP_APPEARANCE; w->min_w = 640; w->min_h = 420;
    wm_center(w);
}
