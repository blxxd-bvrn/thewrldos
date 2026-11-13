/* Visual styles: Luna (Blue / Olive Green / Silver), Classic, plus user hue rotation */
#include "kernel.h"
#include "theme.h"
#include "wm.h"
#include "fs.h"

theme_t T;
int theme_id = THEME_LUNA_BLUE, theme_hue = 0;
int theme_sat, theme_light;
bool dark_mode;

/* ---------- advanced colour overrides ----------
 * A small binary file in the system config directory holds the user's
 * per-role colours.  A zero entry means "use the style's colour"; non-zero
 * entries are applied after dark mode and the hue adjustment, so they win. */
#define OVERRIDE_FILE "C:\\WRLD\\System32\\config\\appearance.bin"
#define OVERRIDE_MAGIC 0x57434F4Cu                /* "WCOL" */
typedef struct { const char *name; uint16_t off; } role_t;
static const role_t roles[] = {
    { "Window face", offsetof(theme_t, face) },
    { "Window background", offsetof(theme_t, window_bg) },
    { "Text", offsetof(theme_t, text) },
    { "Dim text", offsetof(theme_t, gray_text) },
    { "Hyperlink", offsetof(theme_t, link) },
    { "Headings and links", offsetof(theme_t, link_head) },
    { "Selection", offsetof(theme_t, select) },
    { "Selected text", offsetof(theme_t, select_text) },
    { "Button top", offsetof(theme_t, btn_top) },
    { "Button bottom", offsetof(theme_t, btn_bot) },
    { "Button border", offsetof(theme_t, btn_border) },
    { "Button hover top", offsetof(theme_t, btn_hot_a) },
    { "Button hover bottom", offsetof(theme_t, btn_hot_b) },
    { "Scrollbar track", offsetof(theme_t, sb_track) },
    { "Scrollbar thumb top", offsetof(theme_t, sb_thumb_top) },
    { "Scrollbar thumb bottom", offsetof(theme_t, sb_thumb_bot) },
    { "Scrollbar border", offsetof(theme_t, sb_border) },
    { "Check and radio", offsetof(theme_t, check) },
    { "Menu background", offsetof(theme_t, menu_bg) },
    { "Menu border", offsetof(theme_t, menu_border) },
    { "Taskbar top", offsetof(theme_t, tb_top) },
    { "Taskbar bottom", offsetof(theme_t, tb_bot) },
    { "Taskbar text", offsetof(theme_t, tb_text) },
    { "Title bar top", offsetof(theme_t, title_top) },
    { "Title bar bottom", offsetof(theme_t, title_bot) },
    { "Title text", offsetof(theme_t, title_text) },
    { "Panel top", offsetof(theme_t, pane_top) },
    { "Panel bottom", offsetof(theme_t, pane_bot) },
    { "Panel links", offsetof(theme_t, pane_link) },
    { "Desktop colour", offsetof(theme_t, desktop_bg) },
};
#define NROLES ((int)ARRAY_LEN(roles))
static uint32_t role_override[NROLES];
int theme_role_count(void) { return NROLES; }
const char *theme_role_name(int i) { return i >= 0 && i < NROLES ? roles[i].name : ""; }
uint32_t theme_role_color(int i) { return i >= 0 && i < NROLES ? role_override[i] : 0; }
bool theme_role_is_set(int i) { return i >= 0 && i < NROLES && role_override[i] != 0; }
uint32_t theme_role_effective(int i) { return i >= 0 && i < NROLES ? (role_override[i] ? role_override[i] : *(uint32_t *)((uint8_t *)&T + roles[i].off)) : 0; }
void theme_role_set(int i, uint32_t color) { if (i >= 0 && i < NROLES) { role_override[i] = color ? (0xFF000000u | (color & 0xFFFFFFu)) : 0; } }
void theme_role_clear(int i) { if (i >= 0 && i < NROLES) role_override[i] = 0; }
void theme_role_clear_all(void) { memset(role_override, 0, sizeof role_override); }
void theme_overrides_load(void) {
    uint32_t len = 0;
    uint8_t *b = vfs_read_file(OVERRIDE_FILE, &len);
    if (!b || len < 12) { kfree(b); return; }
    uint32_t magic = *(uint32_t *)b, ver = *(uint32_t *)(b + 4), n = *(uint32_t *)(b + 8);
    if (magic != OVERRIDE_MAGIC || ver != 1 || n > (uint32_t)NROLES || len < 12 + n * 4) { kfree(b); return; }
    memset(role_override, 0, sizeof role_override);
    memcpy(role_override, b + 12, n * 4);
    kfree(b);
}
void theme_overrides_save(void) {
    uint8_t buf[12 + NROLES * 4];
    *(uint32_t *)buf = OVERRIDE_MAGIC; *(uint32_t *)(buf + 4) = 1; *(uint32_t *)(buf + 8) = NROLES;
    memcpy(buf + 12, role_override, sizeof role_override);
    vfs_write_file(OVERRIDE_FILE, buf, sizeof buf);
}

uint32_t theme_text_on(uint32_t bg) {
    int luma = (C_R(bg) * 299 + C_G(bg) * 587 + C_B(bg) * 114) / 1000;
    return luma >= 145 ? HEX(0x17202A) : WHITE;
}

/* A palette overlay, not a seventh visual style. Keep each style's accent,
 * title gradients, Start button and glass settings, with dark neutral client
 * surfaces. Hue/tone and the user's explicit role overrides still run last. */
static void apply_dark(theme_t *t) {
    uint32_t accent = t->select, title = t->title_mid ? t->title_mid : t->title_top;
    uint32_t shell = t->tb_top ? t->tb_top : accent;
    t->face = mix(HEX(0x232529), accent, 12);
    t->face_dark = mix(HEX(0x111317), accent, 10); t->face_light = mix(HEX(0x41454C), accent, 18);
    t->window_bg = mix(HEX(0x191B1F), accent, 6); t->text = HEX(0xE9EBEF); t->gray_text = HEX(0xABB2BE);
    t->select = darken(accent, 100); t->select_text = WHITE;
    t->hot = darken(accent, 80); t->hot_text = WHITE;
    t->title_hi = mix(t->title_hi ? t->title_hi : title, t->face_light, 140);
    t->title_top = mix(t->title_top, t->face, 165); t->title_mid = mix(title, t->face_dark, 175);
    t->title_bot = mix(t->title_bot, t->face_dark, 185);
    t->ititle_hi = t->face_light; t->ititle_top = mix(t->face, title, 18);
    t->ititle_mid = t->face; t->ititle_bot = t->face_dark;
    t->title_text = WHITE; t->title_shadow = BLACK; t->ititle_text = HEX(0xBFC6D0);
    t->frame_out = darken(title, 210); t->frame_mid = t->title_mid; t->frame_in = mix(title, t->face_light, 140);
    t->iframe_out = t->face_dark; t->iframe_mid = t->face; t->iframe_in = t->face_light;
    t->cap_top = mix(t->cap_top ? t->cap_top : title, t->face_light, 180);
    t->cap_bot = mix(title, t->face_dark, 175); t->cap_border = mix(t->face_light, WHITE, 85);
    t->cap_glyph = WHITE; t->close_top = darken(t->close_top ? t->close_top : HEX(0xD06A5A), 35);
    t->close_bot = darken(t->close_bot ? t->close_bot : HEX(0xA32A1C), 30);
    t->tb_hi = mix(shell, t->face_light, 140); t->tb_top = mix(shell, t->face_dark, 180);
    t->tb_mid = mix(shell, HEX(0x101216), 195); t->tb_bot = mix(shell, HEX(0x0B0D10), 210); t->tb_text = t->text;
    t->tray_hi = t->tb_hi; t->tray_top = mix(t->tray_top ? t->tray_top : shell, t->face, 185);
    t->tray_bot = t->tb_bot; t->tray_border = t->frame_out; t->tray_text = t->text;
    t->start_hi = darken(t->start_hi ? t->start_hi : shell, 45);
    t->start_top = darken(t->start_top ? t->start_top : shell, 90);
    t->start_bot = darken(t->start_bot ? t->start_bot : shell, 115); t->start_text = WHITE; t->start_shadow = BLACK;
    t->tbtn_top = mix(shell, t->face, 175); t->tbtn_bot = mix(shell, t->face_dark, 195); t->tbtn_border = t->frame_out;
    t->tbtn_act_top = mix(accent, t->face, 110); t->tbtn_act_bot = mix(accent, t->face_dark, 135);
    t->tbtn_text = t->text; t->tbtn_hot = mix(accent, t->face_light, 160);
    t->sm_head_top = mix(t->sm_head_top, t->face, 175); t->sm_head_bot = mix(t->sm_head_bot, t->face_dark, 190);
    t->sm_head_text = t->text; t->sm_border = mix(title, t->face_light, 160);
    t->sm_left = t->window_bg; t->sm_left_text = t->text;
    t->sm_right = mix(t->face, shell, 20); t->sm_right_line = t->face_light; t->sm_right_text = t->text;
    t->sm_foot_top = t->sm_head_top; t->sm_foot_bot = t->sm_head_bot;
    t->sm_accent = lighten(t->sm_accent ? t->sm_accent : accent, 35); t->sm_foot_text = t->text;
    t->btn_border = t->face_dark; t->btn_top = mix(t->face_light, accent, 12); t->btn_bot = t->face;
    t->btn_hot_a = mix(t->face_light, accent, 65); t->btn_hot_b = mix(t->face, accent, 85); t->btn_focus = lighten(accent, 100);
    t->sb_track = t->window_bg; t->sb_thumb_top = mix(t->face_light, accent, 45); t->sb_thumb_bot = t->face;
    t->sb_border = t->face_dark; t->check = lighten(t->check ? t->check : accent, 125);
    t->pane_top = mix(t->face, title, 12); t->pane_bot = t->window_bg;
    t->pane_head_top = mix(t->face_light, title, 20); t->pane_head_bot = mix(t->face, title, 35);
    t->pane_head_text = t->text; t->pane_body = mix(t->window_bg, accent, 10);
    t->pane_link = lighten(t->pane_link, 150); t->link = lighten(t->link, 150); t->link_head = lighten(t->link_head, 155);
    t->menu_bg = t->face; t->menu_border = t->face_light;
    t->glass = mix(t->glass ? t->glass : title, HEX(0x0F131B), 175);
    t->iglass = mix(t->iglass ? t->iglass : title, t->face, 200);
    /* Do not turn Aero into an opaque slab or give opaque Plex a blur. */
    if (t->renderer == RENDER_AERO) { t->glass_alpha = MAX(t->glass_alpha, 135); t->iglass_alpha = MAX(t->iglass_alpha, 105); }
}

/* built-in fallback (safe mode, or when the style files are missing) */
static void make_classic(theme_t *t) {
    memset(t, 0, sizeof *t);
    t->classic = true; t->renderer = RENDER_CLASSIC;
    t->title_h = 22; t->border = 4; t->cap_btn = 16; t->menubar_h = 19; t->taskbar_h = 28;
    t->face = HEX(0xD4D0C8); t->face_dark = HEX(0x808080); t->face_light = WHITE;
    t->window_bg = WHITE; t->text = BLACK; t->gray_text = HEX(0x808080);
    t->title_top = HEX(0x0A246A); t->title_bot = HEX(0xA6CAF0);
    t->ititle_top = HEX(0x808080); t->ititle_bot = HEX(0xC0C0C0);
    t->title_text = WHITE; t->ititle_text = HEX(0xD4D0C8);
    t->select = HEX(0x0A246A); t->select_text = WHITE; t->hot = HEX(0x0A246A); t->hot_text = WHITE;
    t->tb_text = BLACK; t->tray_text = BLACK; t->tbtn_text = BLACK; t->start_text = BLACK;
    t->sm_head_top = HEX(0x000080); t->sm_head_bot = HEX(0x1084D0); t->sm_head_text = WHITE;
    t->sm_left = HEX(0xD4D0C8); t->sm_right = HEX(0xD4D0C8);
    t->pane_top = HEX(0xD4D0C8); t->pane_bot = HEX(0xD4D0C8); t->pane_head_top = HEX(0x0A246A); t->pane_head_bot = HEX(0x0A246A);
    t->pane_head_text = WHITE; t->pane_body = HEX(0xFFFFFF); t->pane_link = HEX(0x0000CC);
    t->link = HEX(0x0000CC); t->link_head = HEX(0x006000);
    t->desktop_bg = HEX(0x3A6EA5);
    t->menu_bg = HEX(0xD4D0C8); t->menu_border = HEX(0x808080);
    t->btn_focus = BLACK;
}


/* ---------------- .wrldstyle files ---------------- */
#define F(n, k, sh) { #n, offsetof(theme_t, n), k, sh }
static const struct { const char *name; uint16_t off; char kind; bool shift; } fields[] = {
    F(title_h, 'i', 0), F(border, 'i', 0), F(cap_btn, 'i', 0), F(menubar_h, 'i', 0), F(taskbar_h, 'i', 0),
    F(glass_alpha, 'i', 0), F(iglass_alpha, 'i', 0), F(glass_blur, 'i', 0), F(glass, 'c', 1), F(iglass, 'c', 1),
    F(face, 'c', 0), F(face_dark, 'c', 0), F(face_light, 'c', 0), F(window_bg, 'c', 0), F(text, 'c', 0), F(gray_text, 'c', 0),
    F(select, 'c', 1), F(select_text, 'c', 0), F(hot, 'c', 1), F(hot_text, 'c', 0),
    F(title_hi, 'c', 1), F(title_top, 'c', 1), F(title_mid, 'c', 1), F(title_bot, 'c', 1),
    F(ititle_hi, 'c', 1), F(ititle_top, 'c', 1), F(ititle_mid, 'c', 1), F(ititle_bot, 'c', 1),
    F(title_text, 'c', 0), F(title_shadow, 'c', 1), F(ititle_text, 'c', 1),
    F(frame_out, 'c', 1), F(frame_mid, 'c', 1), F(frame_in, 'c', 1), F(iframe_out, 'c', 1), F(iframe_mid, 'c', 1), F(iframe_in, 'c', 1),
    F(cap_top, 'c', 1), F(cap_bot, 'c', 1), F(cap_border, 'c', 0), F(cap_glyph, 'c', 0), F(close_top, 'c', 0), F(close_bot, 'c', 0),
    F(tb_hi, 'c', 1), F(tb_top, 'c', 1), F(tb_mid, 'c', 1), F(tb_bot, 'c', 1), F(tb_text, 'c', 0),
    F(tray_hi, 'c', 1), F(tray_top, 'c', 1), F(tray_bot, 'c', 1), F(tray_border, 'c', 1), F(tray_text, 'c', 0),
    F(start_hi, 'c', 0), F(start_top, 'c', 0), F(start_bot, 'c', 0), F(start_text, 'c', 0), F(start_shadow, 'c', 0),
    F(tbtn_top, 'c', 1), F(tbtn_bot, 'c', 1), F(tbtn_border, 'c', 1), F(tbtn_act_top, 'c', 1), F(tbtn_act_bot, 'c', 1), F(tbtn_text, 'c', 0), F(tbtn_hot, 'c', 1),
    F(sm_head_top, 'c', 1), F(sm_head_bot, 'c', 1), F(sm_head_text, 'c', 0), F(sm_border, 'c', 1), F(sm_left, 'c', 0), F(sm_left_text, 'c', 0),
    F(sm_right, 'c', 1), F(sm_right_line, 'c', 1), F(sm_right_text, 'c', 0), F(sm_foot_top, 'c', 1), F(sm_foot_bot, 'c', 1), F(sm_accent, 'c', 0), F(sm_foot_text, 'c', 0),
    F(btn_border, 'c', 1), F(btn_top, 'c', 0), F(btn_bot, 'c', 0), F(btn_hot_a, 'c', 0), F(btn_hot_b, 'c', 0), F(btn_focus, 'c', 1),
    F(sb_track, 'c', 0), F(sb_thumb_top, 'c', 1), F(sb_thumb_bot, 'c', 1), F(sb_border, 'c', 1), F(check, 'c', 1),
    F(pane_top, 'c', 1), F(pane_bot, 'c', 1), F(pane_head_top, 'c', 0), F(pane_head_bot, 'c', 1), F(pane_head_text, 'c', 1), F(pane_body, 'c', 1), F(pane_link, 'c', 1),
    F(link, 'c', 0), F(link_head, 'c', 0), F(desktop_bg, 'c', 1), F(menu_bg, 'c', 0), F(menu_border, 'c', 0),
};
typedef struct { char file[64], name[48], desc[160], wallpaper[24]; theme_t base; } style_t;
static style_t styles[24];
static int nstyles;
static const char *builtin_files[THEME_BUILTIN] = { "Luna Blue", "Olive Green", "Silver", "WRLD Classic", "Aero", "Plex" };

static uint32_t parse_color(const char *v) {
    while (*v == ' ') v++;
    if (!strncasecmp(v, "none", 4)) return 0;
    if (*v == '#') v++;
    uint32_t c = strtoul(v, NULL, 16);
    return strlen(v) > 6 && isxdigit(v[6]) ? c : (0xFF000000u | c);
}
static bool parse_style(const char *file, theme_t *t, style_t *meta, int depth) {
    char path[160]; snprintf(path, sizeof path, STYLE_DIR "\\%s.wrldstyle", file);
    uint32_t len;
    char *txt = (char *)vfs_read_file(path, &len);
    if (!txt) return false;
    /* "Base=" is applied first, wherever it appears, so the file's own values win */
    for (char *bl = txt; (bl = strstr(bl, "Base")); bl++) {
        if (bl != txt && bl[-1] != '\n') continue;
        char *q = bl + 4; while (*q == ' ') q++;
        if (*q != '=') continue;
        q++; while (*q == ' ') q++;
        char base[64]; int k = 0; while (*q && *q != '\r' && *q != '\n' && *q != ';' && k < 63) base[k++] = *q++;
        while (k && base[k - 1] == ' ') k--;
        base[k] = 0;
        if (depth < 4 && base[0] && strcasecmp(base, file)) parse_style(base, t, NULL, depth + 1);
        break;
    }
    for (char *line = txt; *line;) {
        char *e = line; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        while (*line == ' ' || *line == '\t') line++;
        char *eq = strchr(line, '=');
        if (eq && *line != ';' && *line != '[') {
            *eq = 0; char *k = line, *v = eq + 1;
            for (char *q = eq - 1; q >= k && (*q == ' ' || *q == '\t'); q--) *q = 0;
            while (*v == ' ' || *v == '\t') v++;
            char *sc = strchr(v, ';'); if (sc && v[0] != '#') *sc = 0;
            if (!strcasecmp(k, "Base")) {}
            else if (!strcasecmp(k, "Name")) { if (meta) strlcpy(meta->name, v, sizeof meta->name); }
            else if (!strcasecmp(k, "Description")) { if (meta) strlcpy(meta->desc, v, sizeof meta->desc); }
            else if (!strcasecmp(k, "Wallpaper")) { if (meta) strlcpy(meta->wallpaper, v, sizeof meta->wallpaper); }
            else if (!strcasecmp(k, "Renderer"))
                t->renderer = !strcasecmp(v, "classic") ? RENDER_CLASSIC : !strcasecmp(v, "aero") ? RENDER_AERO : !strcasecmp(v, "plex") ? RENDER_PLEX : RENDER_LUNA;
            else for (unsigned i = 0; i < ARRAY_LEN(fields); i++) if (!strcasecmp(k, fields[i].name)) {
                if (fields[i].kind == 'i') *(int *)((uint8_t *)t + fields[i].off) = atoi(v);
                else *(uint32_t *)((uint8_t *)t + fields[i].off) = parse_color(v);
                break;
            }
        }
        *e = save; line = *e ? e + 1 : e;
    }
    kfree(txt);
    return true;
}
static void sanitize(theme_t *t) {
    t->classic = t->renderer == RENDER_CLASSIC;
    t->title_h = CLAMP(t->title_h, 16, 48); t->border = CLAMP(t->border, 1, 12); t->cap_btn = CLAMP(t->cap_btn, 12, t->title_h);
    t->menubar_h = CLAMP(t->menubar_h, 16, 30); t->taskbar_h = CLAMP(t->taskbar_h, 24, 48);
    t->glass_alpha = CLAMP(t->glass_alpha, 0, 255); t->iglass_alpha = CLAMP(t->iglass_alpha, 0, 255); t->glass_blur = CLAMP(t->glass_blur, 0, 16);
    if (!t->sm_right_text) t->sm_right_text = t->classic ? BLACK : HEX(0x00136B);
    if (!t->sm_left_text) t->sm_left_text = BLACK;
    if (!t->sb_track) t->sb_track = t->classic ? t->face : HEX(0xF4F3EE);
    if (!t->sb_thumb_top) t->sb_thumb_top = t->classic ? t->face_light : HEX(0xD8E4FC);
    if (!t->sb_thumb_bot) t->sb_thumb_bot = t->classic ? t->face : HEX(0xB9CDF5);
    if (!t->sb_border) t->sb_border = t->classic ? t->face_dark : HEX(0xA9BCE8);
    if (!t->check) t->check = t->classic ? BLACK : HEX(0x21A121);
}
static void add_style(const char *file) {
    if (nstyles >= (int)ARRAY_LEN(styles)) return;
    style_t *st = &styles[nstyles];
    memset(st, 0, sizeof *st);
    strlcpy(st->file, file, sizeof st->file);
    strlcpy(st->name, file, sizeof st->name);
    if (!parse_style(file, &st->base, st, 0)) { make_classic(&st->base); klog("theme: %s.wrldstyle missing, using Classic", file); }
    sanitize(&st->base);
    nstyles++;
}
void styles_load(void) {
    nstyles = 0;
    for (int i = 0; i < THEME_BUILTIN; i++) add_style(builtin_files[i]);
    vnode_t *d = vfs_resolve(STYLE_DIR, NULL);
    if (d) {
        vfs_sort_children(d);
        for (vnode_t *n = d->child; n; n = n->next) {
            if (n->dir || strcasecmp(vfs_ext(n->name), "wrldstyle")) continue;
            char f[64]; strlcpy(f, n->name, sizeof f); char *dot = strrchr(f, '.'); if (dot) *dot = 0;
            bool dup = false; for (int i = 0; i < nstyles; i++) if (!strcasecmp(styles[i].file, f)) dup = true;
            if (!dup) add_style(f);
        }
    }
    if (theme_id >= nstyles) theme_id = THEME_LUNA_BLUE;
}
int style_count(void) { if (!nstyles) styles_load(); return nstyles; }
static style_t *st_get(int i) { if (!nstyles) styles_load(); return &styles[CLAMP(i, 0, nstyles - 1)]; }
const char *style_name(int i) { return st_get(i)->name; }
const char *style_file(int i) { return st_get(i)->file; }
const char *style_desc(int i) { return st_get(i)->desc; }
const char *style_wallpaper(int i) { return st_get(i)->wallpaper; }
int style_find(const char *n) {
    if (!n || !*n) return -1;
    char f[160]; strlcpy(f, path_basename(n), sizeof f);
    char *dot = strrchr(f, '.'); if (dot && !strcasecmp(dot, ".wrldstyle")) *dot = 0;
    if (!strcasecmp(f, "Windows Classic")) strlcpy(f, "WRLD Classic", sizeof f);   /* old name */
    int cnt = style_count();
    for (int i = 0; i < cnt; i++) if (!strcasecmp(styles[i].file, f) || !strcasecmp(styles[i].name, f)) return i;
    for (int i = 0; i < cnt; i++) if (!strncasecmp(styles[i].file, f, strlen(f)) || !strncasecmp(styles[i].name, f, strlen(f))) return i;
    return -1;
}

void theme_compute(int id, int hue, theme_t *t) {
    id = CLAMP(id, 0, style_count() - 1);
    hue = ((hue % 360) + 360) % 360;
    *t = styles[id].base;
    t->id = id;
    if (dark_mode) apply_dark(t);
    if (hue || theme_sat || theme_light) for (unsigned i = 0; i < ARRAY_LEN(fields); i++) if (fields[i].shift) {
        uint32_t *c = (uint32_t *)((uint8_t *)t + fields[i].off);
        if (*c) *c = color_adjust(hue_shift(*c, hue), theme_sat, theme_light);
    }
    for (int i = 0; i < NROLES; i++)
        if (role_override[i]) *(uint32_t *)((uint8_t *)t + roles[i].off) = role_override[i];
}
void theme_set_tone(int sat, int light) {
    theme_sat = CLAMP(sat, -100, 100); theme_light = CLAMP(light, -60, 60);
    theme_apply(theme_id, theme_hue);
}
void theme_apply(int id, int hue) {
    theme_id = CLAMP(id, 0, style_count() - 1);
    theme_hue = ((hue % 360) + 360) % 360;
    theme_compute(theme_id, theme_hue, &T);
    wm_theme_changed();
}

/* ---------------- drawing helpers ---------------- */
void draw_arrow(canvas_t *c, int x, int y, int dir, uint32_t col) {
    for (int i = 0; i < 4; i++) {
        switch (dir) {
        case 0: hline(c, x - i, y + i, 2 * i + 1, col); break;
        case 1: hline(c, x - (3 - i), y + i, 2 * (3 - i) + 1, col); break;
        case 2: vline(c, x + i, y - i, 2 * i + 1, col); break;
        case 3: vline(c, x + i, y - (3 - i), 2 * (3 - i) + 1, col); break;
        }
    }
}

rect_t caption_button_rect(rect_t win, int flags, int kind) {
    rect_t none = R(0, 0, 0, 0);
    bool nomin = flags & WF_NO_MIN, nomax = flags & WF_NO_MAX;
    if (T.renderer == RENDER_AERO) {
        int top = win.y + 1, h = 20, cx = win.x + win.w - 6 - 43;
        if (kind == 3) return (flags & WF_NOCLOSE) ? none : R(cx, top, 43, h);
        if (nomin && nomax) return none;
        if (kind == 1) return nomax ? none : R(cx - 26, top, 26, h);
        if (kind == 0) return nomin ? none : R(cx - 26 - (nomax ? 0 : 27), top, 27, h);
        return none;
    }
    int s = T.cap_btn;
    int bx = win.x + win.w - (T.classic ? 6 : 5) - s, by = win.y + (T.classic ? 6 : (T.title_h - s) / 2 + 1);
    if (kind == 3) return (flags & WF_NOCLOSE) ? none : R(bx, by, s, s);
    if (nomin && nomax) return none;
    bx -= s + 2;
    if (kind == 1) return nomax ? none : R(bx, by, s, s);
    bx -= s + (T.classic ? 0 : 2);
    if (kind == 0) return nomin ? none : R(bx, by, s, s);
    return none;
}
void draw_glass(canvas_t *c, int x, int y, int w, int h, uint32_t tint, int alpha, int blur) {
    if (w <= 0 || h <= 0) return;
    if (blur > 0) blur_rect(c, x, y, w, h, blur);
    blend_rect(c, x, y, w, h, tint, alpha);
}
/* caption glyphs: 0 minimize, 1 maximize, 2 restore, 3 close */
static void glyph(canvas_t *c, int cx, int cy, int kind, uint32_t g, int scale) {
    int s = scale;
    switch (kind) {
    case 0: fill_rect(c, cx - 4 * s / 2, cy + 2, 4 * s, 3, g); break;
    case 1: draw_rect(c, cx - 5, cy - 4, 10, 9, g); fill_rect(c, cx - 5, cy - 4, 10, 3, g); break;
    case 2: draw_rect(c, cx - 2, cy - 5, 8, 7, g); hline(c, cx - 2, cy - 4, 8, g); fill_rect(c, cx - 5, cy - 1, 8, 7, g); fill_rect(c, cx - 4, cy + 2, 6, 3, 0); break;
    case 3: for (int i = 0; i < 8; i++) { fill_rect(c, cx - 4 + i, cy - 4 + i, 2, 2, g); fill_rect(c, cx + 3 - i, cy - 4 + i, 2, 2, g); } break;
    }
}
static void cap_button(canvas_t *c, rect_t r, int kind, bool active, bool hot, bool pressed) {
    int x = r.x, y = r.y, s = r.w;
    if (T.classic) {
        fill_rect(c, x, y, s, s - 2, T.face);
        bevel_deep(c, x, y, s, s - 2, !pressed);
        int ox = x + (pressed ? 1 : 0), oy = y + (pressed ? 1 : 0);
        switch (kind) {
        case 0: fill_rect(c, ox + 4, oy + 9, 6, 2, T.text); break;
        case 1: draw_rect(c, ox + 3, oy + 2, 9, 9, T.text); hline(c, ox + 3, oy + 3, 9, T.text); break;
        case 2: draw_rect(c, ox + 5, oy + 2, 6, 5, T.text); hline(c, ox + 5, oy + 3, 6, T.text); fill_rect(c, ox + 3, oy + 6, 6, 5, T.face); draw_rect(c, ox + 3, oy + 6, 6, 5, T.text); hline(c, ox + 3, oy + 7, 6, T.text); break;
        case 3: for (int i = 0; i < 7; i++) { fill_rect(c, ox + 4 + i, oy + 3 + i, 2, 1, T.text); fill_rect(c, ox + 10 - i, oy + 3 + i, 2, 1, T.text); } break;
        }
        return;
    }
    if (T.renderer == RENDER_AERO) {
        bool close = kind == 3;
        uint32_t top = close ? T.close_top : T.cap_top, bot = close ? T.close_bot : T.cap_bot;
        if (hot) { top = close ? lighten(top, 50) : HEX(0xDDF2FF); bot = close ? lighten(bot, 40) : HEX(0x2A8EE6); }
        if (pressed) { top = darken(top, 40); bot = darken(bot, 50); }
        fill_round_rect(c, x, y, r.w, r.h, 3, T.cap_border);
        if (close || hot || pressed) gradient_round_rect(c, x + 1, y + 1, r.w - 2, r.h - 2, 2, top, bot);
        else for (int j = 1; j < r.h - 1; j++) blend_rect(c, x + 1, y + j, r.w - 2, 1, mix(top, bot, j * 255 / r.h), active ? 120 : 80);
        blend_rect(c, x + 2, y + 2, r.w - 4, (r.h - 4) / 2, WHITE, close && !active && !hot ? 40 : 75);
        int cx = x + r.w / 2, cy = y + r.h / 2 - 1;
        glyph(c, cx + 1, cy + 1, kind, HEX(0x202830), 2);
        glyph(c, cx, cy, kind, WHITE, 2);
        return;
    }
    if (T.renderer == RENDER_PLEX) {   /* Longhorn: bare glyphs on the title bar, glow on hover */
        if (hot || pressed) {
            uint32_t g = kind == 3 ? T.close_bot : T.cap_bot;
            fill_round_rect_blend(c, x, y, r.w, r.h, 4, pressed ? darken(g, 40) : g, kind == 3 ? 220 : 150);
            fill_round_rect_blend(c, x + 1, y + 1, r.w - 2, r.h / 2, 3, WHITE, 70);
        }
        int cx = x + r.w / 2, cy = y + r.h / 2 - 1;
        uint32_t gc = active || hot ? T.cap_glyph : T.ititle_text;
        glyph(c, cx + 1, cy + 1, kind, mix(T.title_bot, BLACK, 90), 2);
        glyph(c, cx, cy, kind, gc, 2);
        return;
    }
    uint32_t top = kind == 3 ? T.close_top : T.cap_top, bot = kind == 3 ? T.close_bot : T.cap_bot;
    if (!active) { top = mix(top, dark_mode ? T.face : WHITE, 90); bot = mix(bot, dark_mode ? T.face : WHITE, 90); }
    if (hot) { top = lighten(top, 50); bot = lighten(bot, 40); }
    if (pressed) { uint32_t t2 = top; top = darken(bot, 30); bot = t2; }
    /* XP Luna: a white rounded outline, a light-to-dark blue face and a white
     * caption glyph.  The outer white line and the highlight at the top are
     * what make the button read as the Luna caption set rather than a plain
     * gradient chip. */
    fill_round_rect(c, x, y, s, s, 4, T.cap_border);
    gradient_round_rect(c, x + 1, y + 1, s - 2, s - 2, 3, lighten(top, 25), bot);
    blend_rect(c, x + 2, y + 1, s - 4, 1, WHITE, 90);
    blend_rect(c, x + 2, y + s - 3, s - 4, 1, mix(bot, BLACK, 45), 80);
    uint32_t g = kind == 3 ? WHITE : T.cap_glyph;
    switch (kind) {
    case 0:                                     /* minimize: a white bar near the bottom */
        fill_rect(c, x + 6, y + s - 8, 9, 3, g);
        break;
    case 1:                                     /* maximize: square outline with a filled top */
        draw_rect(c, x + 5, y + 5, s - 10, s - 11, g);
        fill_rect(c, x + 5, y + 5, s - 10, 3, g);
        break;
    case 2:                                     /* restore: two overlapping squares */
        draw_rect(c, x + 8, y + 4, 7, 6, g); hline(c, x + 8, y + 5, 7, g);
        fill_rect(c, x + 4, y + 9, 8, 7, bot); draw_rect(c, x + 4, y + 9, 8, 7, g);
        fill_rect(c, x + 4, y + 9, 8, 2, g);
        break;
    case 3:                                     /* close: a bold white X */
        for (int i = 0; i < 10; i++) { fill_rect(c, x + 5 + i, y + 5 + i, 2, 2, g); fill_rect(c, x + 15 - i, y + 5 + i, 2, 2, g); }
        break;
    }
}

static int corner_in(int j, int rad) { if (j >= rad) return 0; int dy = rad - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++; return rad - dx; }
/* glass frame (Aero style) */
static void aero_frame(canvas_t *c, rect_t r, const char *title, int icon, bool active, bool maxed) {
    int x = r.x, y = r.y, w = r.w, h = r.h, th = T.title_h, b = T.border, rad = maxed ? 0 : 7;
    uint32_t tint = active ? T.glass : T.iglass;
    int al = active ? T.glass_alpha : T.iglass_alpha;
    /* keep what is outside the rounded top corners */
    uint32_t keep[2][8 * 8];
    for (int j = 0; j < rad; j++) for (int i = 0; i < rad; i++) {
        int px0 = x + i, px1 = x + w - rad + i, py = y + j;
        keep[0][j * 8 + i] = (px0 >= 0 && px0 < c->w && py >= 0 && py < c->h) ? c->px[py * c->stride + px0] : 0;
        keep[1][j * 8 + i] = (px1 >= 0 && px1 < c->w && py >= 0 && py < c->h) ? c->px[py * c->stride + px1] : 0;
    }
    draw_glass(c, x, y, w, th, tint, al, T.glass_blur);
    draw_glass(c, x, y + th, b, h - th - b, tint, al, T.glass_blur);
    draw_glass(c, x + w - b, y + th, b, h - th - b, tint, al, T.glass_blur);
    draw_glass(c, x, y + h - b, w, b, tint, al, T.glass_blur);
    /* sheen and diagonal reflection streaks */
    for (int j = 0; j < th; j++) {
        if (j < th / 2) blend_rect(c, x, y + j, w, 1, WHITE, (th / 2 - j) * 70 / (th / 2));
        for (int k = -th; k < w; k += 170) {
            int s0 = k + (th - j), len = 26;
            int a0 = MAX(s0, 0), a1 = MIN(s0 + len, w);
            if (a1 > a0) blend_rect(c, x + a0, y + j, a1 - a0, 1, WHITE, 22);
        }
    }
    for (int j = 0; j < rad; j++) for (int i = 0; i < rad; i++) {
        if (i >= corner_in(j, rad)) continue;
        int py = y + j, px0 = x + i, px1 = x + w - 1 - i;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h) continue;
        if (px0 >= c->clip.x && px0 < c->clip.x + c->clip.w) c->px[py * c->stride + px0] = keep[0][j * 8 + i];
        if (px1 >= c->clip.x && px1 < c->clip.x + c->clip.w) c->px[py * c->stride + px1] = keep[1][j * 8 + (rad - 1 - i)];
    }
    /* outer outline and inner highlight */
    uint32_t fo = active ? T.frame_out : T.iframe_out, fi = active ? T.frame_in : T.iframe_in;
    for (int j = 0; j < h; j++) {
        int in = corner_in(j, rad);
        if (j == 0) { hline(c, x + in, y, w - 2 * in, fo); continue; }
        int pin = j < rad ? corner_in(j - 1, rad) : in;
        for (int i = in; i <= MAX(in, pin); i++) { blend_pixel(c, x + i, y + j, fo, 230); blend_pixel(c, x + w - 1 - i, y + j, fo, 230); }
        if (j > 0 && j < h - 1) { blend_pixel(c, x + in + 1, y + j, fi, 110); blend_pixel(c, x + w - 2 - in, y + j, fi, 110); }
    }
    hline(c, x, y + h - 1, w, fo);
    blend_rect(c, x + rad, y + 1, w - 2 * rad, 1, fi, 140);
    /* client edge */
    draw_rect(c, x + b - 1, y + th - 1, w - 2 * b + 2, h - th - b + 2, mix(fo, tint, 60));
    /* title with a soft glow behind it */
    int textx = x + 10;
    if (icon >= 0) { draw_icon(c, icon, 16, x + 8, y + (th - 16) / 2 + 1); textx = x + 30; }
    int tw = text_width(FONT_UI, title), fh = font_height(FONT_UI), ty = y + (th - fh) / 2 + 1;
    rect_t old = canvas_push_clip(c, R(x, y, w - 130, th));
    if (T.title_shadow) for (int k = 7; k >= 1; k--) fill_round_rect_blend(c, textx - 4 - k, ty - k / 2, tw + 8 + 2 * k, fh + k, 6, T.title_shadow, 22);
    draw_text(c, FONT_UI, textx, ty, title, active ? T.title_text : T.ititle_text);
    c->clip = old;
}

void draw_window_frame(canvas_t *c, rect_t r, const char *title, int icon, bool active, int flags, int hot_btn, int pressed_btn, int state) {
    int x = r.x, y = r.y, w = r.w, h = r.h;
    int th = T.title_h, b = T.border;
    bool maxed = state == WS_MAXIMIZED;
    if (T.renderer == RENDER_AERO) aero_frame(c, r, title, icon, active, maxed);
    else if (T.classic) {
        fill_rect(c, x, y, w, h, T.face);
        bevel_deep(c, x, y, w, h, true);
        int tx = x + 4, ty = y + 4, tw = w - 8, tht = th - 4;
        gradient_h(c, tx, ty, tw, tht - 1, active ? T.title_top : T.ititle_top, active ? T.title_bot : T.ititle_bot);
        int textx = tx + 3;
        if (icon >= 0) { draw_icon(c, icon, 16, tx + 2, ty + (tht - 17) / 2); textx += 19; }
        rect_t old = canvas_push_clip(c, R(tx, ty, tw - 60, tht));
        draw_text(c, FONT_UI_BOLD, textx, ty + (tht - 1 - font_height(FONT_UI_BOLD)) / 2, title, active ? T.title_text : T.ititle_text);
        c->clip = old;
    } else {
        uint32_t fo = active ? T.frame_out : T.iframe_out, fm = active ? T.frame_mid : T.iframe_mid, fi = active ? T.frame_in : T.iframe_in;
        /* body frame */
        fill_rect(c, x, y + th, b, h - th, fm); fill_rect(c, x + w - b, y + th, b, h - th, fm);
        fill_rect(c, x, y + h - b, w, b, fm);
        vline(c, x, y + 8, h - 8, fo); vline(c, x + w - 1, y + 8, h - 8, fo); hline(c, x, y + h - 1, w, fo);
        vline(c, x + b - 1, y + th, h - th - b + 1, fi); vline(c, x + w - b, y + th, h - th - b + 1, fi); hline(c, x + b - 1, y + h - b, w - 2 * b + 2, fi);
        /* title bar with rounded top corners */
        int rad = maxed ? 0 : 7;
        for (int j = 0; j < th; j++) {
            uint32_t col;
            if (j == 0) col = fo;
            else if (j == 1) col = active ? T.title_hi : T.ititle_hi;
            else if (j < 8) col = mix(active ? T.title_top : T.ititle_top, active ? T.title_mid : T.ititle_mid, (j - 2) * 255 / 6);
            else col = mix(active ? T.title_mid : T.ititle_mid, active ? T.title_bot : T.ititle_bot, (j - 8) * 255 / (th - 8));
            int in = corner_in(j, rad);
            hline(c, x + in, y + j, w - 2 * in, col);
            if (T.renderer == RENDER_PLEX && j > 1 && j < th / 2) blend_rect(c, x + in + 1, y + j, w - 2 * in - 2, 1, WHITE, 70 - j * 140 / th);
            if (j > 0 && j < rad + 1) { put_pixel(c, x + in, y + j, fo); put_pixel(c, x + w - 1 - in, y + j, fo); }
            else if (j > 0) { put_pixel(c, x, y + j, fo); put_pixel(c, x + w - 1, y + j, fo); }
        }
        int textx = x + 8;
        if (icon >= 0) { draw_icon(c, icon, 16, x + 6, y + (th - 16) / 2 + 1); textx = x + 27; }
        rect_t old = canvas_push_clip(c, R(x, y, w - 75, th));
        int tf = T.renderer == RENDER_PLEX ? FONT_UI : FONT_TITLE;
        int ty = y + (th - font_height(tf)) / 2 + 1;
        if (T.title_shadow && active) draw_text(c, tf, textx + 1, ty + 1, title, T.title_shadow);
        draw_text(c, tf, textx, ty, title, active ? T.title_text : T.ititle_text);
        c->clip = old;
    }
    /* caption buttons */
    for (int k = 3; k >= 0; k--) {
        if (k == 2) continue;
        rect_t br = caption_button_rect(r, flags, k);
        if (!br.w) {
            /* Classic shows a disabled maximize box next to an enabled minimize box */
            if (k == 1 && T.classic && (flags & WF_NO_MAX) && !(flags & WF_NO_MIN)) cap_button(c, caption_button_rect(r, flags & ~WF_NO_MAX, 1), 1, active, false, false);
            continue;
        }
        int kind = k == 1 && maxed ? 2 : k;
        cap_button(c, br, kind, active, hot_btn == k, pressed_btn == k);
    }
}

void draw_button(canvas_t *c, rect_t r, const char *label, bool hot, bool pressed, bool focus, bool enabled) {
    if (T.classic) {
        fill_rect(c, r.x, r.y, r.w, r.h, T.face);
        if (focus) { draw_rect(c, r.x, r.y, r.w, r.h, T.text); bevel_deep(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, !pressed); }
        else bevel_deep(c, r.x, r.y, r.w, r.h, !pressed);
        int o = pressed ? 1 : 0;
        if (!enabled) draw_text_centered(c, FONT_UI, R(r.x + 1, r.y + 1, r.w, r.h), label, T.face_light);
        draw_text_centered(c, FONT_UI, R(r.x + o, r.y + o, r.w, r.h), label, enabled ? T.text : T.gray_text);
        if (focus) dotted_rect(c, r.x + 4, r.y + 4, r.w - 8, r.h - 8, T.text);
        return;
    }
    fill_round_rect(c, r.x, r.y, r.w, r.h, 3, enabled ? T.btn_border : T.face_dark);
    uint32_t top = T.btn_top, bot = T.btn_bot;
    if (pressed) { top = T.face; bot = T.face_light; }
    gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 2, top, bot);
    if (!pressed && enabled) hline(c, r.x + 2, r.y + r.h - 3, r.w - 4, T.face_dark);
    if (enabled && (hot || focus)) {
        uint32_t a = hot ? T.btn_hot_a : lighten(T.btn_focus, 80), b2 = hot ? T.btn_hot_b : T.btn_focus;
        hline(c, r.x + 2, r.y + 1, r.w - 4, a); hline(c, r.x + 2, r.y + r.h - 2, r.w - 4, b2);
        hline(c, r.x + 1, r.y + 2, r.w - 2, a);
        hline(c, r.x + 1, r.y + r.h - 3, r.w - 2, b2);
        vline(c, r.x + 1, r.y + 2, r.h - 4, mix(a, b2, 128)); vline(c, r.x + r.w - 2, r.y + 2, r.h - 4, mix(a, b2, 128));
        vline(c, r.x + 2, r.y + 2, r.h - 4, mix(a, b2, 128)); vline(c, r.x + r.w - 3, r.y + 2, r.h - 4, mix(a, b2, 128));
    }
    draw_text_centered(c, FONT_UI, R(r.x + (pressed ? 1 : 0), r.y + (pressed ? 1 : 0), r.w, r.h), label, enabled ? T.text : T.gray_text);
}
void draw_sunken_box(canvas_t *c, rect_t r, uint32_t bg) {
    fill_rect(c, r.x, r.y, r.w, r.h, bg);
    if (T.classic) bevel_deep(c, r.x, r.y, r.w, r.h, false);
    else draw_rect(c, r.x, r.y, r.w, r.h, dark_mode ? T.face_dark : HEX(0x7F9DB9));
}
void draw_check(canvas_t *c, int x, int y, bool checked, bool hot) {
    if (T.classic) { fill_rect(c, x, y, 13, 13, T.window_bg); bevel_deep(c, x, y, 13, 13, false); }
    else {
        draw_rect(c, x, y, 13, 13, dark_mode ? T.face_dark : HEX(0x1C5180));
        gradient_v(c, x + 1, y + 1, 11, 11, dark_mode ? T.face_light : HEX(0xDCDCD7), T.window_bg);
        if (hot) { draw_rect(c, x + 1, y + 1, 11, 11, HEX(0xF8B330)); draw_rect(c, x + 2, y + 2, 9, 9, HEX(0xFFE0A0)); }
    }
    if (checked) {
        uint32_t k = T.check;
        for (int i = 0; i < 3; i++) { vline(c, x + 3 + i, y + 5 + i, 3, k); }
        for (int i = 0; i < 4; i++) { vline(c, x + 6 + i, y + 6 - i, 3, k); }
    }
}
void draw_radio(canvas_t *c, int x, int y, bool checked, bool hot) {
    if (T.classic) {
        fill_circle(c, x + 6, y + 6, 6, T.face_dark); fill_circle(c, x + 6, y + 6, 5, T.window_bg);
    } else {
        fill_circle(c, x + 6, y + 6, 6, dark_mode ? T.face_dark : HEX(0x1C5180)); fill_circle(c, x + 6, y + 6, 5, dark_mode ? T.face : HEX(0xE4E3DB)); fill_circle(c, x + 6, y + 6, 4, T.window_bg);
        if (hot) draw_circle(c, x + 6, y + 6, 4, HEX(0xF8B330));
    }
    if (checked) fill_circle(c, x + 6, y + 6, 2, T.check);
}
void draw_scrollbar(canvas_t *c, rect_t r, int total, int visible, int pos, bool vertical) {
    int len = vertical ? r.h : r.w, thick = vertical ? r.w : r.h;
    uint32_t track = T.sb_track;
    fill_rect(c, r.x, r.y, r.w, r.h, track);
    int avail = len - 2 * thick;
    int tl = total > visible && total > 0 ? MAX(12, avail * visible / total) : avail;
    int tp = total > visible ? (avail - tl) * pos / (total - visible) : 0;
    tp = CLAMP(tp, 0, avail - tl);
    rect_t a1 = vertical ? R(r.x, r.y, thick, thick) : R(r.x, r.y, thick, thick);
    rect_t a2 = vertical ? R(r.x, r.y + r.h - thick, thick, thick) : R(r.x + r.w - thick, r.y, thick, thick);
    rect_t th = vertical ? R(r.x, r.y + thick + tp, thick, tl) : R(r.x + thick + tp, r.y, tl, thick);
    rect_t parts[3] = { a1, a2, th };
    for (int i = 0; i < 3; i++) {
        rect_t p = parts[i];
        if (T.classic) { fill_rect(c, p.x, p.y, p.w, p.h, T.face); bevel_deep(c, p.x, p.y, p.w, p.h, true); }
        else { fill_round_rect(c, p.x, p.y, p.w, p.h, 2, T.sb_border); gradient_round_rect(c, p.x + 1, p.y + 1, p.w - 2, p.h - 2, 2, T.sb_thumb_top, T.sb_thumb_bot); }
    }
    uint32_t ac = T.text;
    if (vertical) { draw_arrow(c, a1.x + thick / 2, a1.y + thick / 2 - 2, 0, ac); draw_arrow(c, a2.x + thick / 2, a2.y + thick / 2 - 1, 1, ac); }
    else { draw_arrow(c, a1.x + thick / 2 - 2, a1.y + thick / 2, 2, ac); draw_arrow(c, a2.x + thick / 2 - 1, a2.y + thick / 2, 3, ac); }
    if (!T.classic && tl > 14) {
        for (int i = -2; i <= 2; i += 2) {
            if (vertical) hline(c, th.x + 4, th.y + tl / 2 + i, thick - 8, T.gray_text);
            else vline(c, th.x + tl / 2 + i, th.y + 4, thick - 8, T.gray_text);
        }
    }
}
void draw_progress(canvas_t *c, rect_t r, int pct) {
    draw_sunken_box(c, r, T.window_bg);
    int fw = (r.w - 4) * CLAMP(pct, 0, 100) / 100;
    if (T.classic) { for (int x = 0; x + 8 <= fw; x += 10) fill_rect(c, r.x + 2 + x, r.y + 2, 8, r.h - 4, T.select); return; }
    for (int x = 0; x + 6 <= fw; x += 8) gradient_v3(c, r.x + 2 + x, r.y + 2, 6, r.h - 4, HEX(0xB2EDB2), HEX(0x2FBD2F), HEX(0x8BE38B), (r.h - 4) / 2);
}
void draw_group(canvas_t *c, rect_t r, const char *label) {
    int fh = font_height(FONT_UI);
    if (T.classic) { draw_rect(c, r.x, r.y + fh / 2, r.w, r.h - fh / 2, T.face_dark); draw_rect(c, r.x + 1, r.y + fh / 2 + 1, r.w, r.h - fh / 2, T.face_light); }
    else draw_round_rect(c, r.x, r.y + fh / 2, r.w, r.h - fh / 2, 3, T.face_dark);
    int tw = text_width(FONT_UI, label);
    fill_rect(c, r.x + 7, r.y, tw + 6, fh, T.face);
    draw_text(c, FONT_UI, r.x + 10, r.y, label, T.classic ? T.text : T.link);
}
void draw_tab(canvas_t *c, rect_t r, const char *label, bool active, bool hot) {
    if (!active) { r.y += 2; r.h -= 2; }
    if (T.classic) {
        fill_rect(c, r.x, r.y, r.w, r.h, T.face);
        hline(c, r.x + 2, r.y, r.w - 4, T.face_light); vline(c, r.x, r.y + 2, r.h - 2, T.face_light); put_pixel(c, r.x + 1, r.y + 1, T.face_light);
        vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, T.face_dark); vline(c, r.x + r.w - 2, r.y + 1, r.h - 1, T.face_dark);
    } else {
        fill_rect(c, r.x, r.y, r.w, r.h, T.face_dark);
        gradient_v(c, r.x + 1, r.y + 1, r.w - 2, r.h - 1, T.face_light, active ? T.face : T.window_bg);
        if (active || hot) { hline(c, r.x + 1, r.y, r.w - 2, HEX(0xE68B2C)); hline(c, r.x, r.y + 1, r.w, HEX(0xFFC73C)); hline(c, r.x, r.y + 2, r.w, HEX(0xFFC73C)); }
    }
    draw_text_centered(c, FONT_UI, r, label, T.text);
}
void draw_menu_panel(canvas_t *c, rect_t r) {
    if (T.classic) { fill_rect(c, r.x, r.y, r.w, r.h, T.face); bevel_deep(c, r.x, r.y, r.w, r.h, true); return; }
    blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 50);
    fill_rect(c, r.x, r.y, r.w, r.h, T.menu_bg);
    draw_rect(c, r.x, r.y, r.w, r.h, T.menu_border);
}
