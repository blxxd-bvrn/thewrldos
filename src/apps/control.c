/* Control Panel (Windows 7 style): Category / Large icons / Small icons views, breadcrumbs, applets */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "net.h"
#include "audio.h"

typedef struct { const char *id, *name, *desc; int icon; int cat; } applet_t;
enum { CAT_SYSTEM, CAT_NETWORK, CAT_HARDWARE, CAT_PROGRAMS, CAT_USERS, CAT_APPEARANCE, CAT_CLOCK, CAT_EASE, CAT_N };
static const struct { const char *name; int icon; const char *links[3]; const char *targets[3]; } cats[CAT_N] = {
    { "System and Security", ICON_CP_SYSTEM, { "Review your computer's status", "View system information", "Open Task Manager" }, { "system", "system", "::taskmgr" } },
    { "Network and Internet", ICON_CP_NETWORK, { "View network status and tasks", NULL }, { "network" } },
    { "Hardware and Sound", ICON_CP_HARDWARE, { "View devices", "Mouse", "Sound" }, { "devices", "mouse", "sound" } },
    { "Programs", ICON_CP_PROGRAMS, { "Uninstall a program", "Default programs" }, { "programs", "defaults" } },
    { "User Accounts", ICON_CP_USERS, { "Change account name", "Change your picture" }, { "users", "users" } },
    { "Appearance and Personalization", ICON_CP_APPEARANCE, { "Change the theme", "Change desktop background", "Adjust screen resolution" }, { "personalization", "personalization", "display" } },
    { "Clock, Language, and Region", ICON_CP_CLOCK, { "Change date, time, or time zone" }, { "datetime" } },
    { "Ease of Access", ICON_CP_EASE, { "Let " OS_NAME " suggest settings", "Optimize visual display" }, { "ease", "ease" } },
};
static const applet_t applets[] = {
    { "system", "System", "View basic information about your computer", ICON_CP_SYSTEM, CAT_SYSTEM },
    { "devices", "Device Manager", "View hardware detected by the kernel", ICON_CP_DEVICES, CAT_HARDWARE },
    { "::taskmgr", "Task Manager", "See running programs and performance", ICON_TASKMGR, CAT_SYSTEM },
    { "network", "Network and Sharing Center", "View network status", ICON_CP_NETWORK, CAT_NETWORK },
    { "mouse", "Mouse", "Change mouse settings", ICON_CP_MOUSE, CAT_HARDWARE },
    { "keyboard", "Keyboard", "Test and change keyboard settings", ICON_CP_KEYBOARD, CAT_HARDWARE },
    { "sound", "Sound", "Test the PC speaker", ICON_CP_SOUND, CAT_HARDWARE },
    { "programs", "Programs and Features", "Uninstall or view programs", ICON_CP_PROGRAMS, CAT_PROGRAMS },
    { "defaults", "Default Programs", "File type associations", ICON_EXE, CAT_PROGRAMS },
    { "users", "User Accounts", "Change your name and picture", ICON_CP_USERS, CAT_USERS },
    { "personalization", "Personalization", "Theme, colour hue and background", ICON_CP_APPEARANCE, CAT_APPEARANCE },
    { "display", "Display", "Screen resolution and effects", ICON_CP_DISPLAY, CAT_APPEARANCE },
    { "taskbar", "Taskbar and Start Menu", "Customise the taskbar", ICON_CP_TASKBAR, CAT_APPEARANCE },
    { "fonts", "Fonts", "View installed fonts", ICON_CP_FONTS, CAT_APPEARANCE },
    { "datetime", "Date and Time", "Set the time zone and clock", ICON_CP_DATETIME, CAT_CLOCK },
    { "ease", "Ease of Access Center", "Make the computer easier to use", ICON_CP_EASE, CAT_EASE },
};
#define NAPPLETS ((int)ARRAY_LEN(applets))

typedef struct {
    char page[32];            /* "" home, "cat:N", or applet id */
    char hist[20][32]; int hpos, hlen;
    int view;                 /* 0 category, 1 large icons, 2 small icons */
    int scroll;
    char search[64];
    /* applet state */
    int hue, theme; int wall; int style_page; uint32_t style_gen; int res_sel;
    char uname[32];
    int dbl, speed; bool swap; uint32_t dbl_test_t; bool dbl_open;
    int tz; char keytest[64];
    int prog_sel, prog_scroll;
    bool anim;
    window_t *win;
} cp_t;

static const applet_t *find_applet(const char *id) { for (int i = 0; i < NAPPLETS; i++) if (!strcmp(applets[i].id, id)) return &applets[i]; return NULL; }
static void cp_go(cp_t *c, const char *page, bool push) {
    if (!strcmp(page, "::taskmgr")) { taskmgr_open(); return; }
    if (push) {
        if (c->hlen) c->hpos++;
        if (c->hpos >= 20) { memmove(c->hist, c->hist + 1, 19 * 32); c->hpos = 19; }
        strlcpy(c->hist[c->hpos], page, 32); c->hlen = c->hpos + 1;
    }
    strlcpy(c->page, page, sizeof c->page);
    c->scroll = 0;
    const applet_t *a = find_applet(page);
    wm_set_title(c->win, a ? a->name : !strncmp(page, "cat:", 4) ? cats[atoi(page + 4)].name : c->view ? "All Control Panel Items" : "Control Panel");
    c->win->icon = a ? a->icon : ICON_CONTROL_PANEL;
    c->hue = theme_hue; c->theme = theme_id; c->wall = wallpaper_id; c->res_sel = 0;
    strlcpy(c->uname, user_name, sizeof c->uname);
    c->dbl = wm_dblclick_ms; c->speed = mouse_speed; c->swap = mouse_swap_buttons;
    c->tz = tz_offset_minutes; c->anim = wm_animations;
    c->win->dirty = true;
}

#define TOP_H 40
#define NAV_W 200
static const uint32_t W7_HEAD = 0xFF1E3287, W7_GREEN = 0xFF1C7A1C, W7_LINK = 0xFF0066CC, W7_TEXT = 0xFF333333;

/* ---------------- chrome ---------------- */
static void round_nav_button(window_t *w, canvas_t *s, int x, int y, bool back, bool enabled, bool *clicked) {
    rect_t r = R(x, y, 26, 26);
    bool hot = ui_hover(w, r) && enabled;
    *clicked = ui_click_area(w, r) && enabled;
    uint32_t a = enabled ? (hot ? HEX(0x6CB8FF) : HEX(0x4C9BE8)) : HEX(0xC8D0DA), b = enabled ? (hot ? HEX(0x2272D8) : HEX(0x1A5BB8)) : HEX(0xA8B0BA);
    fill_circle(s, x + 13, y + 13, 13, enabled ? HEX(0x1B4A8A) : HEX(0x9AA4B0));
    for (int j = -12; j <= 12; j++) { int dx = 0; while ((dx + 1) * (dx + 1) + j * j <= 144) dx++; hline(s, x + 13 - dx, y + 13 + j, 2 * dx + 1, mix(a, b, (j + 12) * 255 / 24)); }
    blend_rect(s, x + 6, y + 3, 14, 6, WHITE, 60);
    int cx = x + 13, cy = y + 13;
    for (int i = 0; i < 6; i++) { int xx = back ? cx - 5 + i : cx + 5 - i; vline(s, xx, cy - i, 2 * i + 1, WHITE); }
    fill_rect(s, back ? cx : cx - 6, cy - 2, 7, 5, WHITE);
}
static void draw_chrome(window_t *w, cp_t *c) {
    canvas_t *s = w->surf;
    gradient_v(s, 0, 0, s->w, TOP_H, HEX(0xF3F7FC), HEX(0xDAE5F3));
    hline(s, 0, TOP_H - 1, s->w, HEX(0xA0AFC3));
    bool b1, b2;
    round_nav_button(w, s, 8, 7, true, c->hpos > 0, &b1);
    round_nav_button(w, s, 38, 7, false, c->hpos < c->hlen - 1, &b2);
    if (b1) { c->hpos--; cp_go(c, c->hist[c->hpos], false); }
    if (b2) { c->hpos++; cp_go(c, c->hist[c->hpos], false); }
    /* breadcrumb */
    int bx = 72, bw = s->w - bx - 210;
    rect_t br = R(bx, 8, bw, 24);
    fill_rect(s, br.x, br.y, br.w, br.h, WHITE); draw_rect(s, br.x, br.y, br.w, br.h, HEX(0x8E9EB8));
    draw_icon(s, ICON_CONTROL_PANEL, 16, bx + 4, 12);
    int x = bx + 24;
    const char *crumbs[3]; const char *targets[3]; int n = 0;
    char catpage[16];
    crumbs[n] = c->view && !c->page[0] ? "All Control Panel Items" : "Control Panel"; targets[n++] = "";
    const applet_t *a = find_applet(c->page);
    int cat = !strncmp(c->page, "cat:", 4) ? atoi(c->page + 4) : a ? a->cat : -1;
    if (cat >= 0 && !c->view) { snprintf(catpage, sizeof catpage, "cat:%d", cat); crumbs[n] = cats[cat].name; targets[n++] = catpage; }
    if (a) { crumbs[n] = a->name; targets[n++] = a->id; }
    for (int i = 0; i < n; i++) {
        draw_arrow(s, x + 2, 20, 3, HEX(0x505A68)); x += 10;
        rect_t r = R(x - 2, 10, text_width(FONT_UI, crumbs[i]) + 6, 20);
        bool hot = ui_hover(w, r);
        if (hot) { fill_rect(s, r.x, r.y, r.w, r.h, HEX(0xE5F1FB)); draw_rect(s, r.x, r.y, r.w, r.h, HEX(0x7DA2CE)); }
        if (ui_click_area(w, r) && strcmp(targets[i], c->page)) { cp_go(c, targets[i], true); return; }
        draw_text(s, FONT_UI, x + 1, 13, crumbs[i], BLACK);
        x += r.w + 2;
    }
    /* search box */
    rect_t sr = R(s->w - 200, 8, 190, 24);
    ui_textbox(w, sr, c->search, sizeof c->search);
    if (!c->search[0]) draw_text(s, FONT_UI, sr.x + 6, sr.y + 5, "Search Control Panel", HEX(0x8A8A8A));
    draw_icon(s, ICON_SEARCH, 16, sr.x + sr.w - 20, sr.y + 4);
}
static void nav_pane(window_t *w, cp_t *c, int cat) {
    canvas_t *s = w->surf;
    gradient_v(s, 0, TOP_H, NAV_W, s->h - TOP_H, HEX(0xE3ECF7), HEX(0xF8FBFE));
    vline(s, NAV_W - 1, TOP_H, s->h - TOP_H, HEX(0xD2DCE8));
    int y = TOP_H + 16;
    if (ui_link(w, 16, y, FONT_UI, "Control Panel Home", W7_LINK)) { cp_go(c, "", true); return; }
    y += 30;
    for (int i = 0; i < CAT_N; i++) {
        bool cur = i == cat;
        if (cur) draw_text_wrapped(s, FONT_UI_BOLD, 16, y, NAV_W - 30, cats[i].name, BLACK, 1);
        else if (ui_link(w, 16, y, FONT_UI, cats[i].name, W7_LINK)) { char p[16]; snprintf(p, sizeof p, "cat:%d", i); cp_go(c, p, true); return; }
        y += (text_width(FONT_UI, cats[i].name) > NAV_W - 30 ? 30 : 22);
    }
    y += 14;
    hline(s, 16, y, NAV_W - 32, HEX(0xC8D4E4)); y += 12;
    draw_text(s, FONT_UI, 16, y, "See also", HEX(0x6A7A8E)); y += 20;
    if (ui_link(w, 16, y, FONT_UI, "Task Manager", W7_LINK)) taskmgr_open();
    y += 20;
    if (ui_link(w, 16, y, FONT_UI, "Command Prompt", W7_LINK)) terminal_open(NULL, NULL);
}

/* ---------------- home views ---------------- */
static void home_category(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    int colw = (s->w - 60) / 2;
    for (int i = 0; i < CAT_N; i++) {
        int col = i < 4 ? 0 : 1, row = i % 4;
        int x = ox + 24 + col * colw, y = oy + 70 + row * 100;
        draw_icon(s, cats[i].icon, 48, x, y);
        if (ui_link(w, x + 60, y + 2, FONT_LARGE, cats[i].name, W7_GREEN)) { char p[16]; snprintf(p, sizeof p, "cat:%d", i); cp_go(c, p, true); return; }
        for (int k = 0; k < 3 && cats[i].links[k]; k++)
            if (ui_link(w, x + 60, y + 30 + k * 18, FONT_UI, cats[i].links[k], W7_LINK)) { cp_go(c, cats[i].targets[k], true); return; }
    }
}
static void home_icons(window_t *w, cp_t *c, int ox, int oy, bool large) {
    canvas_t *s = w->surf;
    const applet_t *list[NAPPLETS]; int n = 0;
    for (int i = 0; i < NAPPLETS; i++) if (!c->search[0] || strstr(applets[i].name, c->search) || strstr(applets[i].desc, c->search)) list[n++] = &applets[i];
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && strcmp(list[b - 1]->name, list[b]->name) > 0; b--) { const applet_t *t = list[b]; list[b] = list[b - 1]; list[b - 1] = t; }
    int cw2 = large ? 250 : 200, chh = large ? 48 : 26, cols = MAX(1, (s->w - 40) / cw2);
    for (int i = 0; i < n; i++) {
        int x = ox + 24 + (i % cols) * cw2, y = oy + 60 + (i / cols) * (chh + 8);
        rect_t r = R(x - 4, y - 3, cw2 - 10, chh + 6);
        if (ui_hover(w, r)) { fill_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0xEAF4FD)); draw_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0xB8D6FB)); }
        if (ui_click_area(w, r)) { cp_go(c, list[i]->id, true); return; }
        if (large) { draw_icon(s, list[i]->icon, 32, x, y + 6); draw_text(s, FONT_UI, x + 42, y + 10, list[i]->name, W7_LINK); draw_text_ellipsis(s, FONT_SMALL, x + 42, y + 26, cw2 - 60, list[i]->desc, HEX(0x6A6A6A)); }
        else { draw_icon(s, list[i]->icon, 16, x, y + 4); draw_text(s, FONT_UI, x + 24, y + 5, list[i]->name, W7_LINK); }
    }
    if (!n) draw_text(s, FONT_UI, ox + 24, oy + 60, "No items match your search.", HEX(0x6A6A6A));
}
static void category_page(window_t *w, cp_t *c, int cat, int ox, int oy) {
    canvas_t *s = w->surf;
    int y = oy + 20;
    for (int i = 0; i < NAPPLETS; i++) {
        if (applets[i].cat != cat) continue;
        draw_icon(s, applets[i].icon, 48, ox + 20, y);
        if (ui_link(w, ox + 80, y + 4, FONT_LARGE, applets[i].name, W7_GREEN)) { cp_go(c, applets[i].id, true); return; }
        draw_text(s, FONT_UI, ox + 80, y + 30, applets[i].desc, W7_TEXT);
        y += 72;
        hline(s, ox + 20, y - 12, s->w - ox - 40, HEX(0xE6ECF2));
    }
}

/* ---------------- applets ---------------- */
static void heading(canvas_t *s, int x, int y, const char *t, const char *sub) {
    draw_text(s, FONT_LARGE, x, y, t, W7_HEAD);
    if (sub) draw_text_wrapped(s, FONT_UI, x, y + 30, s->w - x - 30, sub, W7_TEXT, 2);
}
void style_create_from(int base) {
    char path[200], name[48];
    for (int k = 1; k < 100; k++) {
        snprintf(name, sizeof name, "My Style %d", k);
        snprintf(path, sizeof path, STYLE_DIR "\\%s.wrldstyle", name);
        if (!vfs_resolve(path, NULL)) break;
    }
    char body[1400];
    snprintf(body, sizeof body,
        "; %s - a THE WRLD OS visual style based on \"%s\".\r\n"
        "; Every value not listed here comes from the Base style. Remove the ';' in\r\n"
        "; front of a line and change the colour, save, then pick the style in\r\n"
        "; Control Panel > Personalization (or double-click this file).\r\n\r\n"
        "[Style]\r\nName=%s\r\nBase=%s\r\nDescription=My own visual style.\r\n\r\n"
        "[Colors]\r\n; title_top=#2E82F8\r\n; title_mid=#0A5BE6\r\n; title_bot=#0049D0\r\n; tb_top=#2A66E0\r\n; tb_bot=#1C47B8\r\n"
        "; start_top=#3C9C3C\r\n; start_bot=#2D7A2D\r\n; select=#316AC5\r\n; desktop_bg=#3A6EA5\r\n"
        "; glass=#74B8FC\r\n; glass_alpha=105\r\n",
        name, style_file(base), name, style_file(base));
    vfs_mkdir_p(STYLE_DIR);
    if (vfs_write_file(path, body, strlen(body))) { styles_load(); notepad_open(path); }
}
void cp_theme_thumb(canvas_t *s, int x, int y, int id, int hue, bool sel) {
    theme_t save = T;
    theme_compute(id, hue, &T);
    canvas_t *pre = canvas_new(120, 80);
    gfx_clear(pre, id == THEME_OLIVE ? HEX(0x6E7F48) : id == THEME_SILVER ? HEX(0x8A90A8) : HEX(0x3A6EA5));
    wallpaper_render(pre, id == THEME_CLASSIC ? 5 : 0);
    draw_window_frame(pre, R(10, 8, 100, 56), "Window", -1, true, 0, -1, -1, 0);
    fill_rect(pre, 10 + T.border, 8 + T.title_h, 100 - 2 * T.border, 56 - T.title_h - T.border, T.face);
    if (T.classic) fill_rect(pre, 0, 70, 120, 10, T.face); else gradient_v(pre, 0, 70, 120, 10, T.tb_top, T.tb_bot);
    if (!T.classic) fill_round_rect(pre, 0, 70, 26, 10, 3, T.start_top); else { fill_rect(pre, 2, 71, 22, 8, T.face); bevel(pre, 2, 71, 22, 8, true); }
    T = save;
    if (sel) { fill_rect(s, x - 4, y - 4, 128, 88, HEX(0xCCE4F8)); draw_rect(s, x - 4, y - 4, 128, 88, HEX(0x3399FF)); }
    blit(s, x, y, pre, 0, 0, 120, 80);
    draw_rect(s, x, y, 120, 80, HEX(0x808080));
    canvas_free(pre);
}
static void page_personalization(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Change the visuals on your computer", "Pick a visual style, rotate its colour hue and choose a background. Changes apply at once.");
    /* pick up new or edited .wrldstyle files */
    if (c->style_gen != vfs_generation) { c->style_gen = vfs_generation; styles_load(); theme_compute(theme_id, theme_hue, &T); wm_theme_changed(); }
    int y = oy + 78;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Visual styles", BLACK);
    if (ui_link(w, s->w - 330, y, FONT_UI, "Create a style...", W7_LINK)) style_create_from(theme_id);
    if (ui_link(w, s->w - 210, y, FONT_UI, "Open the Themes folder", W7_LINK)) explorer_open(STYLE_DIR);
    y += 22;
    int n = THEME_COUNT, per = 8, pages = (n + per - 1) / per;
    if (c->style_page >= pages) c->style_page = 0;
    int hovered = -1;
    for (int k = 0; k < per && c->style_page * per + k < n; k++) {
        int i = c->style_page * per + k;
        int x = ox + 24 + (k % 4) * 140, yy = y + (k / 4) * 104;
        cp_theme_thumb(s, x, yy, i, i == theme_id ? theme_hue : 0, theme_id == i);
        if (ui_hover(w, R(x - 4, yy - 4, 128, 104))) hovered = i;
        if (ui_click_area(w, R(x - 4, yy - 4, 128, 104))) {
            theme_apply(i, i == theme_id ? theme_hue : 0); c->hue = theme_hue;
            const char *wp = style_wallpaper(i);
            for (int j = 0; wp[0] && j < wallpaper_count; j++) if (!strcasecmp(wallpaper_names[j], wp)) { wallpaper_id = j; shell_wallpaper_changed(); }
            wm_redraw_all();
        }
        draw_text_centered(s, FONT_UI, R(x - 4, yy + 83, 128, 16), style_name(i), BLACK);
    }
    if (pages > 1) {
        char pb[32]; snprintf(pb, sizeof pb, "Page %d of %d", c->style_page + 1, pages);
        draw_text(s, FONT_UI, ox + 24, y + 210, pb, W7_TEXT);
        if (ui_link(w, ox + 110, y + 210, FONT_UI, "Next page", W7_LINK)) c->style_page = (c->style_page + 1) % pages;
    }
    y += MIN(2, (n + 3) / 4) * 104 + (pages > 1 ? 18 : 0);
    draw_text_ellipsis(s, FONT_UI, ox + 20, y - 4, s->w - ox - 40, style_desc(hovered >= 0 ? hovered : theme_id), W7_TEXT);
    y += 16;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Colour hue", BLACK);
    char hb[40]; snprintf(hb, sizeof hb, "%d degrees", c->hue);
    draw_text(s, FONT_UI, ox + 120, y, hb, W7_TEXT);
    y += 20;
    theme_t base; theme_compute(theme_id, 0, &base);
    uint32_t basec = base.renderer == RENDER_AERO || base.renderer == RENDER_PLEX ? base.glass : base.classic ? base.title_top : base.title_mid;
    for (int i = 0; i < 360; i += 2) fill_rect(s, ox + 25 + i * 300 / 360, y, 2, 10, hue_shift(basec | 0xFF000000, i));
    if (ui_slider(w, R(ox + 20, y + 12, 310, 24), &c->hue, 0, 359)) { theme_apply(theme_id, c->hue); wm_redraw_all(); }
    if (ui_button(w, R(ox + 350, y + 10, 90, 24), "Reset hue")) { c->hue = 0; theme_apply(theme_id, 0); wm_redraw_all(); }
    static const int presets[] = { 0, 30, 120, 160, 200, 280, 320 };
    static const char *pnames[] = { "Default", "Violet", "Rose", "Ruby", "Amber", "Emerald", "Teal" };
    for (int i = 0; i < 7; i++) {
        int x = ox + 450 + i * 26;
        fill_round_rect(s, x, y + 10, 22, 22, 4, HEX(0x606060));
        fill_round_rect(s, x + 1, y + 11, 20, 20, 3, hue_shift(basec | 0xFF000000, presets[i]));
        if (ui_click_area(w, R(x, y + 10, 22, 22))) { c->hue = presets[i]; theme_apply(theme_id, presets[i]); wm_redraw_all(); }
        if (ui_hover(w, R(x, y + 10, 22, 22))) draw_text(s, FONT_SMALL, ox + 450, y + 36, pnames[i], W7_TEXT);
    }
    y += 50;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Desktop background", BLACK); y += 22;
    static canvas_t *thumbs[8];
    for (int i = 0; i < wallpaper_count; i++) {
        int x = ox + 24 + i * 100;
        if (!thumbs[i]) {
            canvas_t *big = canvas_new(SCREEN_W / 4, SCREEN_H / 4);
            wallpaper_render(big, i);
            thumbs[i] = big;
        }
        bool sel = wallpaper_id == i;
        if (sel) { fill_rect(s, x - 3, y - 3, 92, 70, HEX(0xCCE4F8)); draw_rect(s, x - 3, y - 3, 92, 70, HEX(0x3399FF)); }
        for (int j = 0; j < 64; j++) for (int k = 0; k < 86; k++) put_pixel(s, x + k, y + j, i == wallpaper_count - 1 ? desktop_color : thumbs[i]->px[(j * thumbs[i]->h / 64) * thumbs[i]->w + k * thumbs[i]->w / 86]);
        draw_rect(s, x, y, 86, 64, HEX(0x808080));
        draw_text_centered(s, FONT_UI, R(x - 3, y + 67, 92, 16), wallpaper_names[i], BLACK);
        if (ui_click_area(w, R(x - 3, y - 3, 92, 86))) { wallpaper_id = i; shell_wallpaper_changed(); wm_need_redraw = true; }
    }
    y += 92;
    if (wallpaper_id == wallpaper_count - 1) {
        draw_text(s, FONT_UI, ox + 20, y + 4, "Desktop colour:", BLACK);
        static const uint32_t cols[] = { 0x3A6EA5, 0x008080, 0x000000, 0x6E7F48, 0x5A4A7A, 0xA05030, 0x404858, 0xC0C0C0 };
        for (int i = 0; i < 8; i++) {
            rect_t r = R(ox + 120 + i * 28, y, 22, 22);
            fill_rect(s, r.x, r.y, r.w, r.h, HEX(cols[i])); draw_rect(s, r.x, r.y, r.w, r.h, (desktop_color & 0xFFFFFF) == cols[i] ? HEX(0x3399FF) : HEX(0x808080));
            if (ui_click_area(w, r)) { desktop_color = HEX(cols[i]); shell_wallpaper_changed(); }
        }
    }
}
static const struct { int w, h; } modes[] = { { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 }, { 1280, 1024 }, { 1360, 768 },
    { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 } };
static int res_prev_w, res_prev_h, res_countdown;
static window_t *res_box;
static void res_revert(void) {
    if (res_prev_w) gfx_set_resolution(res_prev_w, res_prev_h);
    res_prev_w = 0;
}
static void res_answer(void *ctx, int r) {
    if (!res_prev_w) return;                 /* already reverted by the timer */
    if (r != IDYES) res_revert();
    res_prev_w = 0; res_box = NULL;
}
static void res_tick(window_t *w) {
    if (!res_box || !wm_alive(res_box)) { w->timer = NULL; return; }
    if (--res_countdown <= 0) { wm_destroy(res_box); res_box = NULL; res_revert(); w->timer = NULL; return; }
    char t[64]; snprintf(t, sizeof t, "Display Settings (%d)", res_countdown);
    wm_set_title(res_box, t);
}
static void page_display(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Change the appearance of your display", NULL);
    /* monitor preview */
    int mx = ox + 30, my = oy + 64;
    fill_round_rect(s, mx, my, 150, 104, 6, HEX(0x30343C)); fill_rect(s, mx + 8, my + 8, 134, 84, HEX(0x3A6EA5));
    fill_rect(s, mx + 60, my + 104, 30, 10, HEX(0x50545C)); fill_round_rect(s, mx + 40, my + 112, 70, 8, 3, HEX(0x40444C));
    char b[160];
    snprintf(b, sizeof b, "%d x %d", SCREEN_W, SCREEN_H); draw_text_centered(s, FONT_UI_BOLD, R(mx + 8, my + 8, 134, 84), b, WHITE);
    int x = ox + 210, y = oy + 64;
    draw_text(s, FONT_UI, x, y, "Display adapter:", HEX(0x505050)); draw_text_ellipsis(s, FONT_UI, x + 110, y, s->w - x - 130, video_driver_name(), BLACK); y += 22;
    int avail[ARRAY_LEN(modes)], na = 0, cur = 0;
    for (unsigned i = 0; i < ARRAY_LEN(modes); i++) if (video_mode_ok(modes[i].w, modes[i].h)) { if (modes[i].w == SCREEN_W && modes[i].h == SCREEN_H) cur = na; avail[na++] = i; }
    if (!c->res_sel && na) c->res_sel = cur + 1;          /* slider position + 1 (0 = not yet set) */
    if (video_can_set_mode() && na > 1) {
        draw_text(s, FONT_UI, x, y + 4, "Screen resolution:", HEX(0x505050));
        int sw = MAX(120, MIN(300, s->w - x - 140));
        draw_text(s, FONT_SMALL, x + 110, y + 26, "Less", HEX(0x606060)); draw_text(s, FONT_SMALL, x + 110 + sw - 24, y + 26, "More", HEX(0x606060));
        int pos = c->res_sel - 1;
        if (ui_slider(w, R(x + 110, y, sw, 24), &pos, 0, na - 1)) c->res_sel = pos + 1;
        int m = avail[CLAMP(c->res_sel - 1, 0, na - 1)];
        snprintf(b, sizeof b, "%d by %d pixels", modes[m].w, modes[m].h);
        draw_text_centered(s, FONT_UI_BOLD, R(x + 110, y + 40, sw, 16), b, BLACK);
        y += 66;
        bool changed = modes[m].w != SCREEN_W || modes[m].h != SCREEN_H;
        if (ui_button_ex(w, R(x + 110, y, 90, 24), "Apply", changed, false) && changed) {
            int pw = SCREEN_W, ph = SCREEN_H;
            if (gfx_set_resolution(modes[m].w, modes[m].h)) {
                res_prev_w = pw; res_prev_h = ph; res_countdown = 15;
                res_box = msgbox("Display Settings (15)", "Your screen resolution has changed. Do you want to keep these settings?\n\nIf you do nothing, the previous settings come back in 15 seconds.",
                                 MBI_QUESTION, MBX_YESNO, res_answer, NULL);
                wm_set_timer(w, 1000, res_tick);
            } else msgbox("Display Settings", "The display adapter could not switch to that resolution.", MBI_ERROR, MBX_OK, NULL, NULL);
        }
        y += 36;
    } else {
        draw_text_wrapped(s, FONT_UI, x, y, s->w - x - 30, "This display adapter is driven through the boot framebuffer, so the resolution is fixed. "
            "Supported adapters for changing it: QEMU/Bochs standard VGA, VirtualBox VBoxVGA and VMSVGA, VMware SVGA II.", W7_TEXT, 2);
        y += 60;
    }
    snprintf(b, sizeof b, "Colour depth: %u-bit true colour", fb_info.bpp);
    draw_text(s, FONT_UI, x, y, b, W7_TEXT); y += 30;
    ui_checkbox(w, x, y, "Animate windows when minimizing and maximizing", &wm_animations);
    y += 28;
    if (ui_link(w, x, y, FONT_UI, "Personalize visual style and colours", W7_LINK)) cp_go(c, "personalization", true);
}
static void page_taskbar(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Taskbar and Start Menu Properties", NULL);
    int y = oy + 70;
    draw_group(s, R(ox + 20, y, 420, 130), "Notification area"); y += 26;
    if (ui_checkbox(w, ox + 36, y, "Show the clock", &show_clock)) wm_need_redraw = true; y += 26;
    if (ui_checkbox(w, ox + 36, y, "Show seconds", &show_seconds)) wm_need_redraw = true; y += 26;
    if (ui_checkbox(w, ox + 36, y, "Use 24-hour clock", &clock_24h)) wm_need_redraw = true; y += 50;
    draw_group(s, R(ox + 20, y, 420, 90), "Start menu"); y += 26;
    draw_text(s, FONT_UI, ox + 36, y, "Start menu style follows the visual style:", BLACK); y += 20;
    draw_text(s, FONT_UI, ox + 36, y, "Luna styles use the two-column menu; Classic uses the cascading menu.", W7_TEXT);
}
static void page_datetime(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Date and Time", NULL);
    rtc_time_t t; rtc_now(&t);
    int cx = ox + 110, cy = oy + 160, r = 80;
    fill_circle(s, cx, cy, r + 4, HEX(0x5A7090)); fill_circle(s, cx, cy, r, WHITE);
    extern int ext_isin(int a);
    for (int i = 0; i < 12; i++) {
        int a = i * 1024 / 12;
        int sx = ext_isin(a), sy = -ext_isin(a + 256);
        fill_circle(s, cx + sx * (r - 10) / 256, cy + sy * (r - 10) / 256, i % 3 ? 2 : 4, HEX(0x1E3287));
    }
    int ha = ((t.hour % 12) * 60 + t.minute) * 1024 / 720, ma = t.minute * 1024 / 60, sa = t.second * 1024 / 60;
    draw_thick_line(s, cx, cy, cx + ext_isin(ha) * (r - 35) / 256, cy - ext_isin(ha + 256) * (r - 35) / 256, 5, HEX(0x202020));
    draw_thick_line(s, cx, cy, cx + ext_isin(ma) * (r - 18) / 256, cy - ext_isin(ma + 256) * (r - 18) / 256, 3, HEX(0x202020));
    draw_line(s, cx, cy, cx + ext_isin(sa) * (r - 12) / 256, cy - ext_isin(sa + 256) * (r - 12) / 256, HEX(0xD02020));
    fill_circle(s, cx, cy, 4, HEX(0x202020));
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mons[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char b[96];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.minute, t.second); draw_text(s, FONT_XL, ox + 230, oy + 90, b, BLACK);
    snprintf(b, sizeof b, "%s, %s %d, %d", days[t.weekday], mons[(t.month + 11) % 12], t.day, t.year); draw_text(s, FONT_UI, ox + 232, oy + 128, b, BLACK);
    draw_text(s, FONT_UI_BOLD, ox + 232, oy + 170, "Time zone", BLACK);
    snprintf(b, sizeof b, "UTC%c%02d:%02d", c->tz < 0 ? '-' : '+', ABSDIFF(c->tz, 0) / 60, ABSDIFF(c->tz, 0) % 60); draw_text(s, FONT_UI, ox + 320, oy + 170, b, BLACK);
    int hours = c->tz / 30;
    if (ui_slider(w, R(ox + 230, oy + 190, 300, 24), &hours, -24, 28)) { c->tz = hours * 30; tz_offset_minutes = c->tz; wm_need_redraw = true; }
    draw_text(s, FONT_SMALL, ox + 230, oy + 216, "UTC-12", W7_TEXT); draw_text(s, FONT_SMALL, ox + 500, oy + 216, "UTC+14", W7_TEXT);
    ui_checkbox(w, ox + 232, oy + 240, "Use 24-hour clock in the taskbar", &clock_24h);
    draw_text_wrapped(s, FONT_UI, ox + 30, oy + 280, s->w - ox - 60, "The hardware clock (CMOS RTC) is read directly by the kernel and treated as UTC; the time zone offset is applied for display.", W7_TEXT, 2);
}
static void page_system(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "View basic information about your computer", NULL);
    int y = oy + 64;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Edition", HEX(0x1E3287)); hline(s, ox + 80, y + 8, s->w - ox - 120, HEX(0xD8E0EA)); y += 24;
    draw_text(s, FONT_UI, ox + 40, y, OS_NAME " " OS_EDITION, BLACK); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Version " OS_VERSION " (Build " OS_BUILD ")", BLACK); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "An original operating system with an XP-style shell", W7_TEXT);
    draw_icon(s, ICON_LOGO, 48, s->w - 90, oy + 64);
    y += 30;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "System", HEX(0x1E3287)); hline(s, ox + 80, y + 8, s->w - ox - 120, HEX(0xD8E0EA)); y += 24;
    char v[16], br[52], b[160]; cpu_get_info(v, br);
    const char *lab[] = { "Computer name:", "Installed on:", "Processor:", "Installed memory:", "Kernel heap:", "System type:", "Uptime:", "Tasks:" };
    char vals[8][96];
    strlcpy(vals[6], computer_name, 96);
    persist_status(vals[7], 96);
    snprintf(vals[0], 96, "%s (%s)", br, v);
    snprintf(vals[1], 96, "%u MB", mm_phys_total() / (1024 * 1024));
    snprintf(vals[2], 96, "%u KB used of %u MB", mm_used() / 1024, mm_total() / (1024 * 1024));
    snprintf(vals[3], 96, "32-bit operating system (i686 protected mode, 4 MiB paging)");
    uint32_t up = timer_ticks() / 1000; snprintf(vals[4], 96, "%u:%02u:%02u", up / 3600, (up / 60) % 60, up % 60);
    snprintf(vals[5], 96, "%d running (cooperative scheduler)", task_count());
    static const int order[8] = { 6, 7, 0, 1, 2, 3, 4, 5 };
    for (int k = 0; k < 8; k++) { int i = order[k]; draw_text(s, FONT_UI, ox + 40, y, lab[k], HEX(0x505050)); draw_text_ellipsis(s, FONT_UI, ox + 170, y, s->w - ox - 200, vals[i], BLACK); y += 20; }
    y += 14;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Compatibility", HEX(0x1E3287)); hline(s, ox + 110, y + 8, s->w - ox - 150, HEX(0xD8E0EA)); y += 24;
    snprintf(b, sizeof b, "Win32 PE32 (.exe): %d kernel32, %d msvcrt, %d user32, %d gdi32 functions", 0, 0, 0, 0);
    extern const int kernel32_count, msvcrt_count, user32_count, gdi32_count;
    snprintf(b, sizeof b, "Win32 PE32 (.exe): %d kernel32 / %d msvcrt / %d user32 / %d gdi32 functions", kernel32_count, msvcrt_count, user32_count, gdi32_count);
    draw_text(s, FONT_UI, ox + 40, y, b, BLACK); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Shell scripts (.sh): built-in POSIX-style interpreter", BLACK); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Apple disk images (.dmg): UDIF raw/zlib, FAT volumes, script .app bundles", BLACK); y += 26;
    if (ui_link(w, ox + 40, y, FONT_UI, "Device Manager", W7_LINK)) cp_go(c, "devices", true);
    if (!system_volume && ui_link(w, ox + 310, y, FONT_UI, "Install " OS_NAME " to a hard disk", W7_LINK)) setup_launch_from_desktop();
    if (ui_link(w, ox + 160, y, FONT_UI, "Kernel log (dmesg)", W7_LINK)) { vfs_write_file("C:\\tmp\\dmesg.log", klog_buffer(), strlen(klog_buffer())); notepad_open("C:\\tmp\\dmesg.log"); }
}
static void page_devices(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Device Manager", "Hardware enumerated from PCI configuration space and the kernel's built-in drivers.");
    pci_dev_t d[32]; int n = pci_scan(d, 32);
    int y = oy + 80 - c->scroll;
    draw_icon(s, ICON_COMPUTER, 16, ox + 20, y); draw_text(s, FONT_UI_BOLD, ox + 40, y + 1, computer_name, BLACK); y += 22;
    const struct { const char *g; int icon; } builtin[] = { { "Keyboards: Standard PS/2 Keyboard (IRQ 1)", ICON_CP_KEYBOARD }, { "Mice: PS/2 IntelliMouse-compatible (IRQ 12)", ICON_CP_MOUSE },
        { "System timer: Intel 8253/8254 PIT @ 1000 Hz (IRQ 0)", ICON_CP_DATETIME }, { "System CMOS/real time clock", ICON_CP_CLOCK }, { "Ports: Communications Port (COM1)", ICON_CP_DEVICES }, { "Sound: PC speaker", ICON_CP_SOUND } };
    for (unsigned i = 0; i < ARRAY_LEN(builtin); i++) { draw_icon(s, builtin[i].icon, 16, ox + 40, y); draw_text(s, FONT_UI, ox + 62, y + 1, builtin[i].g, BLACK); y += 20; }
    y += 6;
    for (int i = 0; i < n; i++) {
        char b[160];
        snprintf(b, sizeof b, "%s - %s [%04x:%04x] at %02x:%02x.%x", pci_class_name(d[i].cls, d[i].sub), pci_vendor_name(d[i].vendor), d[i].vendor, d[i].device, d[i].bus, d[i].dev, d[i].fn);
        int ic = d[i].cls == 3 ? ICON_CP_DISPLAY : d[i].cls == 2 ? ICON_CP_NETWORK : d[i].cls == 1 ? ICON_DRIVE : d[i].cls == 4 ? ICON_CP_SOUND : ICON_CP_DEVICES;
        draw_icon(s, ic, 16, ox + 40, y); draw_text(s, FONT_UI, ox + 62, y + 1, b, BLACK); y += 20;
    }
}
static void page_network(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "View your basic network information", NULL);
    bool ok = net_ready();
    draw_icon(s, ICON_COMPUTER, 48, ox + 40, oy + 80); draw_text(s, FONT_UI, ox + 30, oy + 134, computer_name, BLACK);
    if (ok) for (int x = ox + 100; x < ox + 240; x += 2) hline(s, x, oy + 104, 1, HEX(0x2E8E2E));
    else { for (int x = ox + 100; x < ox + 240; x += 8) hline(s, x, oy + 104, 4, HEX(0xC03030)); draw_icon(s, ICON_ERROR, 16, ox + 162, oy + 96); }
    draw_icon(s, ICON_CP_NETWORK, 48, ox + 250, oy + 80); draw_text(s, FONT_UI, ox + 254, oy + 134, "Internet", BLACK);
    int y = oy + 170;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Local Area Connection", HEX(0x1E3287)); hline(s, ox + 160, y + 8, s->w - ox - 190, HEX(0xD8E0EA)); y += 24;
    char a[16], m[16], g[16], d[16], b[160];
    ip_to_str(netif.ip, a); ip_to_str(netif.mask, m); ip_to_str(netif.gw, g); ip_to_str(netif.dns, d);
    const char *lab[] = { "Status:", "Adapter:", "Physical address:", "IPv4 address:", "Subnet mask:", "Default gateway:", "DNS server:", "Activity:" };
    char vals[8][96];
    strlcpy(vals[0], net_status_text(), 96);
    strlcpy(vals[1], netif.driver[0] ? netif.driver : "None", 96);
    snprintf(vals[2], 96, "%02X-%02X-%02X-%02X-%02X-%02X", netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
    strlcpy(vals[3], netif.configured ? a : "-", 96); strlcpy(vals[4], netif.configured ? m : "-", 96);
    strlcpy(vals[5], netif.configured ? g : "-", 96); strlcpy(vals[6], netif.configured ? d : "-", 96);
    snprintf(vals[7], 96, "%u KB sent, %u KB received", netif.tx_bytes / 1024, netif.rx_bytes / 1024);
    for (int i = 0; i < 8; i++) { if (!netif.present && i > 1) break; draw_text(s, FONT_UI, ox + 40, y, lab[i], HEX(0x505050)); draw_text(s, FONT_UI, ox + 170, y, vals[i], BLACK); y += 20; }
    y += 10;
    if (netif.present && ui_button(w, R(ox + 40, y, 150, 26), "Renew address (DHCP)")) dhcp_renew();
    if (ui_link(w, ox + 210, y + 5, FONT_UI, "Open The WRLD Xplorer", W7_LINK)) browser_open(NULL);
    y += 40;
    snprintf(b, sizeof b, "Secure websites are checked against %d trusted root certificates (add your own in C:\\Windows\\Certificates).", tls_trust_anchor_count());
    draw_text_wrapped(s, FONT_UI, ox + 30, y, s->w - ox - 60, netif.present ? b : "No supported network adapter was found. THE WRLD OS supports Intel PRO/1000 (e1000) adapters, the default in QEMU, VirtualBox and VMware.", W7_TEXT, 2);
}
static void page_mouse(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Mouse Properties", NULL);
    int y = oy + 70;
    draw_group(s, R(ox + 20, y, 460, 110), "Double-click speed");
    draw_text(s, FONT_UI, ox + 36, y + 30, "Slow", BLACK); draw_text(s, FONT_UI, ox + 300, y + 30, "Fast", BLACK);
    int v = 900 - c->dbl;
    if (ui_slider(w, R(ox + 70, y + 26, 220, 24), &v, 100, 750)) { c->dbl = 900 - v; wm_dblclick_ms = c->dbl; }
    rect_t tr = R(ox + 380, y + 22, 48, 48);
    draw_icon(s, c->dbl_open ? ICON_FOLDER_OPEN : ICON_FOLDER, 48, tr.x, tr.y);
    if (ui_hover(w, tr) && w->ui.dblclick) c->dbl_open = !c->dbl_open;
    draw_text(s, FONT_SMALL, ox + 36, y + 70, "Double-click the folder to test the setting.", W7_TEXT);
    y += 126;
    draw_group(s, R(ox + 20, y, 460, 80), "Pointer speed");
    draw_text(s, FONT_UI, ox + 36, y + 30, "Slow", BLACK); draw_text(s, FONT_UI, ox + 300, y + 30, "Fast", BLACK);
    if (ui_slider(w, R(ox + 70, y + 26, 220, 24), &c->speed, 1, 10)) mouse_speed = c->speed;
    y += 96;
    if (ui_checkbox(w, ox + 24, y, "Switch primary and secondary buttons", &c->swap)) mouse_swap_buttons = c->swap;
}
static void page_keyboard(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Keyboard Properties", "Click in the box and type to test the PS/2 keyboard driver.");
    draw_text(s, FONT_UI, ox + 20, oy + 80, "Test area:", BLACK);
    ui_textbox(w, R(ox + 100, oy + 76, 300, 22), c->keytest, sizeof c->keytest);
    draw_text(s, FONT_UI_BOLD, ox + 20, oy + 120, "Keyboard shortcuts", BLACK);
    const char *k[] = { "Win or Ctrl+Esc - open the Start menu", "Alt+Tab - switch windows", "Alt+F4 - close the active window", "Ctrl+Alt+Del / Ctrl+Shift+Esc - Task Manager", "F2 - rename, F5 - refresh, Delete - recycle", "Ctrl+C in Command Prompt - interrupt" };
    for (int i = 0; i < 6; i++) draw_text(s, FONT_UI, ox + 30, oy + 142 + i * 18, k[i], W7_TEXT);
}
static void page_sound(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Sound", "Change the volume, and choose the sounds " OS_NAME " plays when it starts and shuts down.");
    int y = oy + 64; char b[160];
    draw_icon(s, ICON_CP_SOUND, 48, ox + 30, y);
    draw_text(s, FONT_UI, ox + 96, y + 2, "Playback device:", HEX(0x505050));
    draw_text_ellipsis(s, FONT_UI_BOLD, ox + 210, y + 2, s->w - ox - 230, audio_device_name(), BLACK);
    if (!audio_present()) {
        draw_text_wrapped(s, FONT_UI, ox + 96, y + 22, s->w - ox - 120, "No supported sound card was found. " OS_NAME " supports Intel High Definition Audio and AC'97 "
            "(QEMU: -device intel-hda -device hda-output, or -device AC97; VirtualBox: 'Intel HD Audio' or 'ICH AC97').", W7_TEXT, 2);
        y += 80;
    } else {
        y += 26;
        draw_text(s, FONT_UI, ox + 96, y + 4, "Volume:", HEX(0x505050));
        int vol = audio_volume;
        if (ui_slider(w, R(ox + 210, y, 220, 24), &vol, 0, 100)) audio_volume = vol;
        snprintf(b, sizeof b, "%d%%", audio_volume); draw_text(s, FONT_UI, ox + 440, y + 4, b, BLACK);
        y += 30;
        ui_checkbox(w, ox + 210, y, "Mute", &audio_muted);
        y += 40;
        draw_text(s, FONT_UI_BOLD, ox + 20, y, "System sounds", HEX(0x1E3287)); hline(s, ox + 125, y + 8, s->w - ox - 155, HEX(0xD8E0EA)); y += 26;
        ui_checkbox(w, ox + 40, y, "Play the startup and shutdown sounds", &system_sounds); y += 30;
        static const struct { const char *label, *file; } snd[] = { { "Startup", SOUND_STARTUP }, { "Shutdown", SOUND_SHUTDOWN } };
        for (int i = 0; i < 2; i++) {
            draw_icon(s, ICON_CP_SOUND, 16, ox + 40, y + 4);
            draw_text(s, FONT_UI, ox + 62, y + 5, snd[i].label, BLACK);
            draw_text_ellipsis(s, FONT_UI, ox + 140, y + 5, s->w - ox - 260, path_basename(snd[i].file), W7_TEXT);
            if (ui_button(w, R(s->w - 110, y, 80, 24), "Test")) { char err[64]; audio_stop_all(); if (!audio_play_file(snd[i].file, err, sizeof err)) msgbox("Sound", err, MBI_ERROR, MBX_OK, NULL, NULL); }
            y += 30;
        }
        if (ui_button(w, R(s->w - 110, y, 80, 24), "Stop")) audio_stop_all();
        y += 40;
    }
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "PC speaker", HEX(0x1E3287)); hline(s, ox + 105, y + 8, s->w - ox - 135, HEX(0xD8E0EA)); y += 26;
    static const struct { const char *n; int f; } tones[] = { { "Beep", 880 }, { "Asterisk", 1200 }, { "Critical Stop", 330 } };
    for (int i = 0; i < 3; i++) if (ui_button(w, R(ox + 40 + i * 110, y, 100, 24), tones[i].n)) speaker_beep(tones[i].f, 150);
}
static void page_programs(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Uninstall or change a program", "Built-in programs are part of the system. Programs installed under C:\\Program Files can be removed.");
    const char *names[64]; int icons[64]; const char *pub[64]; vnode_t *nodes[64]; int n = 0;
    for (int i = 0; i < app_count && n < 64; i++) { names[n] = app_table[i].title; icons[n] = app_table[i].icon; pub[n] = "THE WRLD OS (built-in)"; nodes[n] = NULL; n++; }
    vnode_t *pf = vfs_resolve("C:\\Program Files", NULL);
    for (vnode_t *k = pf ? pf->child : NULL; k && n < 64; k = k->next) { names[n] = k->name; icons[n] = file_icon(k); pub[n] = "Installed by user"; nodes[n] = k; n++; }
    vnode_t *sm = vfs_resolve("C:\\Samples", NULL);
    for (vnode_t *k = sm ? sm->child : NULL; k && n < 64; k = k->next) { names[n] = k->name; icons[n] = file_icon(k); pub[n] = "Sample program"; nodes[n] = k; n++; }
    int y = oy + 76;
    gradient_v(s, ox + 20, y, s->w - ox - 40, 22, WHITE, HEX(0xEEF2F7)); draw_rect(s, ox + 20, y, s->w - ox - 40, 22, HEX(0xD5DFEA));
    draw_text(s, FONT_UI, ox + 30, y + 4, "Name", BLACK); draw_text(s, FONT_UI, ox + 320, y + 4, "Publisher", BLACK); draw_text(s, FONT_UI, ox + 480, y + 4, "Size", BLACK);
    y += 24;
    for (int i = 0; i < n && y < s->h - 60; i++) {
        rect_t r = R(ox + 20, y, s->w - ox - 40, 22);
        if (ui_click_area(w, r)) c->prog_sel = i;
        if (c->prog_sel == i) fill_rect(s, r.x, r.y, r.w, r.h, HEX(0xCCE8FF));
        else if (ui_hover(w, r)) fill_rect(s, r.x, r.y, r.w, r.h, HEX(0xE5F3FF));
        draw_icon(s, icons[i], 16, ox + 26, y + 3);
        draw_text_ellipsis(s, FONT_UI, ox + 48, y + 4, 260, names[i], BLACK);
        draw_text(s, FONT_UI, ox + 320, y + 4, pub[i], W7_TEXT);
        if (nodes[i]) { char b[32]; format_size(b, sizeof b, vfs_tree_size(nodes[i])); draw_text(s, FONT_UI, ox + 480, y + 4, b, W7_TEXT); }
        y += 22;
    }
    int sel = c->prog_sel;
    bool can = sel >= 0 && sel < n && nodes[sel] && !strcmp(pub[sel], "Installed by user");
    if (ui_button_ex(w, R(ox + 20, s->h - 44, 100, 26), "Uninstall", can, false) && can) { vfs_delete(nodes[sel]); c->prog_sel = -1; notify_balloon("Programs and Features", "The program was uninstalled."); }
    if (sel >= 0 && sel < n && nodes[sel] && ui_button(w, R(ox + 130, s->h - 44, 100, 26), "Run")) { char p[256]; vfs_path(nodes[sel], p, sizeof p); shell_open(p, NULL); }
}
static void page_defaults(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Associate a file type with a program", NULL);
    static const struct { const char *ext, *desc, *prog; int icon; } assoc[] = {
        { ".exe", "Win32 application (PE32, i386)", "WRLD Win32 subsystem", ICON_EXE },
        { ".sh", "Shell script", "WRLD shell (sh)", ICON_SCRIPT },
        { ".dmg", "Apple disk image (UDIF)", "Disk Image Mounter", ICON_DMG },
        { ".app", "macOS application bundle", "Bundle launcher (script executables)", ICON_APP_BUNDLE },
        { ".txt / .log / .ini / .c", "Text document", "Notepad", ICON_TEXT_FILE },
        { ".bmp", "Bitmap image", "Image Viewer / Paint", ICON_IMAGE_FILE },
        { ".lnk", "Shortcut", "Shell", ICON_EXE },
        { "Mach-O / ELF", "Native macOS / Linux binaries", "Identified, not executed", ICON_SYS_FILE },
    };
    int y = oy + 64;
    for (unsigned i = 0; i < ARRAY_LEN(assoc); i++) {
        if (i % 2) fill_rect(s, ox + 20, y - 2, s->w - ox - 40, 34, HEX(0xF4F8FC));
        draw_icon(s, assoc[i].icon, 32, ox + 26, y);
        draw_text(s, FONT_UI_BOLD, ox + 68, y + 2, assoc[i].ext, BLACK);
        draw_text(s, FONT_UI, ox + 68, y + 17, assoc[i].desc, W7_TEXT);
        draw_text(s, FONT_UI, ox + 380, y + 9, assoc[i].prog, BLACK);
        y += 36;
    }
}
static void page_users(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Make changes to your user account", NULL);
    fill_round_rect(s, ox + 28, oy + 68, 56, 56, 5, HEX(0xA8C4E8)); draw_icon(s, user_picture, 48, ox + 32, oy + 72);
    draw_text(s, FONT_LARGE, ox + 100, oy + 72, user_name, BLACK);
    draw_text(s, FONT_UI, ox + 100, oy + 98, "Administrator", W7_TEXT);
    draw_text(s, FONT_UI_BOLD, ox + 28, oy + 150, "Account name", BLACK);
    bool enter = ui_textbox(w, R(ox + 28, oy + 170, 220, 22), c->uname, sizeof c->uname);
    if ((ui_button(w, R(ox + 258, oy + 169, 100, 24), "Change name") || enter) && c->uname[0]) { strlcpy(user_name, c->uname, sizeof user_name); wm_need_redraw = true; }
    draw_text(s, FONT_UI_BOLD, ox + 28, oy + 214, "Picture", BLACK);
    static const int pics[] = { ICON_USER_FLOWER, ICON_USER_PLANET, ICON_USER_FISH, ICON_USER_MOUNTAIN };
    for (int i = 0; i < 4; i++) {
        rect_t r = R(ox + 28 + i * 70, oy + 236, 56, 56);
        fill_round_rect(s, r.x, r.y, r.w, r.h, 5, user_picture == pics[i] ? HEX(0x3399FF) : HEX(0xD8D8D8));
        draw_icon(s, pics[i], 48, r.x + 4, r.y + 4);
        if (ui_click_area(w, r)) { user_picture = pics[i]; wm_need_redraw = true; }
    }
}
static void page_fonts(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Preview fonts", "Fonts are rasterised with anti-aliasing from the Liberation family (SIL Open Font License).");
    static const struct { int f; const char *n; } fl[] = { { FONT_UI, "Sans 12 (UI)" }, { FONT_UI_BOLD, "Sans Bold 12" }, { FONT_TITLE, "Sans Bold 13 (title bars)" }, { FONT_LARGE, "Sans 18" }, { FONT_XL, "Sans 24" }, { FONT_MONO, "Mono 13 (console)" }, { FONT_START, "Sans Bold Italic 17" } };
    int y = oy + 76;
    for (unsigned i = 0; i < ARRAY_LEN(fl); i++) { draw_text(s, FONT_SMALL, ox + 24, y, fl[i].n, W7_TEXT); draw_text(s, fl[i].f, ox + 200, y - 2, "The quick brown fox jumps over the lazy dog", BLACK); y += font_height(fl[i].f) + 12; }
}
static void page_ease(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Make your computer easier to use", NULL);
    int y = oy + 70;
    draw_icon(s, ICON_CP_EASE, 48, ox + 24, y);
    draw_text(s, FONT_UI_BOLD, ox + 84, y + 4, "Quick access to common tools", BLACK);
    if (ui_button(w, R(ox + 84, y + 26, 190, 26), "Use high-contrast Classic style")) { theme_apply(THEME_CLASSIC, 0); wm_redraw_all(); }
    if (ui_button(w, R(ox + 284, y + 26, 160, 26), "Restore Luna (Blue)")) { theme_apply(THEME_LUNA_BLUE, 0); wm_redraw_all(); }
    y += 80;
    if (ui_link(w, ox + 84, y, FONT_UI, "Make the mouse easier to use (slower pointer, slower double-click)", W7_LINK)) { mouse_speed = 3; wm_dblclick_ms = 700; }
    y += 24;
    if (ui_link(w, ox + 84, y, FONT_UI, "Turn off unnecessary animations", W7_LINK)) wm_animations = false;
    y += 24;
    if (ui_link(w, ox + 84, y, FONT_UI, "Show seconds and 24-hour time in the taskbar", W7_LINK)) { show_seconds = true; clock_24h = true; wm_need_redraw = true; }
}

static void cp_paint(window_t *w) {
    cp_t *c = w->data; canvas_t *s = w->surf;
    gfx_clear(s, WHITE);
    draw_chrome(w, c);
    const applet_t *a = find_applet(c->page);
    int cat = !strncmp(c->page, "cat:", 4) ? atoi(c->page + 4) : a ? a->cat : -1;
    int ox = 0, oy = TOP_H;
    if (c->search[0] && !a) { home_icons(w, c, 0, oy, true); return; }
    if (!c->page[0]) {
        draw_text(s, FONT_LARGE, 24, oy + 18, c->view ? "Adjust your computer's settings" : "Adjust your computer's settings", W7_HEAD);
        static const char *views[] = { "Category", "Large icons", "Small icons" };
        draw_text(s, FONT_UI, s->w - 210, oy + 22, "View by:", BLACK);
        int v = c->view;
        rect_t dr = R(s->w - 150, oy + 18, 120, 22);
        ui_dropdown(w, dr, views, 3, &c->view);
        if (v != c->view) cp_go(c, "", false);
        if (c->view == 0) home_category(w, c, 0, oy);
        else home_icons(w, c, 0, oy, c->view == 1);
        return;
    }
    nav_pane(w, c, cat);
    if (strcmp(c->page, a ? a->id : c->page)) return;
    ox = NAV_W;
    rect_t old = canvas_push_clip(s, R(NAV_W, TOP_H, s->w - NAV_W, s->h - TOP_H));
    if (!a) category_page(w, c, cat, ox, oy);
    else if (!strcmp(a->id, "personalization")) page_personalization(w, c, ox, oy);
    else if (!strcmp(a->id, "display")) page_display(w, c, ox, oy);
    else if (!strcmp(a->id, "taskbar")) page_taskbar(w, c, ox, oy);
    else if (!strcmp(a->id, "datetime")) page_datetime(w, c, ox, oy);
    else if (!strcmp(a->id, "system")) page_system(w, c, ox, oy);
    else if (!strcmp(a->id, "devices")) page_devices(w, c, ox, oy);
    else if (!strcmp(a->id, "network")) page_network(w, c, ox, oy);
    else if (!strcmp(a->id, "mouse")) page_mouse(w, c, ox, oy);
    else if (!strcmp(a->id, "keyboard")) page_keyboard(w, c, ox, oy);
    else if (!strcmp(a->id, "sound")) page_sound(w, c, ox, oy);
    else if (!strcmp(a->id, "programs")) page_programs(w, c, ox, oy);
    else if (!strcmp(a->id, "defaults")) page_defaults(w, c, ox, oy);
    else if (!strcmp(a->id, "users")) page_users(w, c, ox, oy);
    else if (!strcmp(a->id, "fonts")) page_fonts(w, c, ox, oy);
    else if (!strcmp(a->id, "ease")) page_ease(w, c, ox, oy);
    s->clip = old;
}
static void cp_tick(window_t *w) { cp_t *c = w->data; if (!strcmp(c->page, "datetime") || !strcmp(c->page, "system") || !strcmp(c->page, "network")) w->dirty = true; }
static void cp_mouse(window_t *w, int type, int x, int y, int b) { cp_t *c = w->data; if (type == ME_WHEEL && !strcmp(c->page, "devices")) { c->scroll = MAX(0, c->scroll + b * 30); w->dirty = true; } }
static void cp_menu(window_t *w, int id) { (void)id; w->dirty = true; }
static void cp_destroy(window_t *w) { kfree(w->data); }
void control_open(const char *page) {
    window_t *w = wm_create("Control Panel", -1, -1, 800, 560, WF_RESIZABLE);
    cp_t *c = kzalloc(sizeof *c);
    c->win = w; c->prog_sel = -1;
    w->data = c; w->paint = cp_paint; w->destroy = cp_destroy; w->mouse = cp_mouse; w->menu_cmd = cp_menu;
    w->icon = ICON_CONTROL_PANEL; w->min_w = 640; w->min_h = 420;
    wm_set_timer(w, 1000, cp_tick);
    cp_go(c, "", true);
    if (page && *page) cp_go(c, page, true);
}
