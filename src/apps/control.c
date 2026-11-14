/* Control Panel (category style): Category / Large icons / Small icons views, breadcrumbs, applets */
#include "kernel.h"
#include "devmgr.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "net.h"
#include "audio.h"

static void cp_refresh_theme(void *arg);

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
    { "admin", "Administrative Tools", "Task Manager, devices, storage and shell restart", ICON_TASKMGR, CAT_SYSTEM },
    { "devices", "Device Manager", "See every device and the driver that runs it", ICON_CP_DEVICES, CAT_HARDWARE },
    { "::taskmgr", "Task Manager", "See running programs and performance", ICON_TASKMGR, CAT_SYSTEM },
    { "network", "Network and Sharing Center", "View network status", ICON_CP_NETWORK, CAT_NETWORK },
    { "mouse", "Mouse", "Change mouse settings", ICON_CP_MOUSE, CAT_HARDWARE },
    { "keyboard", "Keyboard", "Test and change keyboard settings", ICON_CP_KEYBOARD, CAT_HARDWARE },
    { "sound", "Sound", "Test the PC speaker", ICON_CP_SOUND, CAT_HARDWARE },
    { "programs", "Programs and Features", "Uninstall or view programs", ICON_CP_PROGRAMS, CAT_PROGRAMS },
    { "defaults", "Default Programs", "File type associations", ICON_EXE, CAT_PROGRAMS },
    { "users", "User Accounts", "Change your name and picture", ICON_CP_USERS, CAT_USERS },
    { "personalization", "Personalization", "Theme, colour hue and background", ICON_CP_APPEARANCE, CAT_APPEARANCE },
    { "advanced", "Advanced Appearance", "Individual colours, scrollbars, buttons and cursors", ICON_CP_APPEARANCE, CAT_APPEARANCE },
    { "display", "Display", "Screen resolution and effects", ICON_CP_DISPLAY, CAT_APPEARANCE },
    { "effects", "Visual Effects", "Animations and 3D acceleration", ICON_CP_DISPLAY, CAT_APPEARANCE },
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
    int hue, theme, sat, light; int wall; int style_page; uint32_t style_gen; int res_sel;
    char uname[32];
    int dbl, speed; bool swap; uint32_t dbl_test_t; bool dbl_open;
    int tz; char keytest[64];
    int prog_sel, prog_scroll;
    int dev_view, dev_tick; uint32_t dev_closed; char dev_sel[112]; int scroll_max;   /* Device Manager: tab, collapsed classes, selection */
    int wall_page;
    int adv_sel, adv_scroll, adv_last, adv_cursor, adv_cursor_scroll, adv_r, adv_g, adv_b;
    bool adv_init;
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
    if (!strcmp(page, "advanced")) {
        c->adv_sel = 0; c->adv_scroll = 0; c->adv_last = -1; c->adv_init = false;
        c->adv_cursor = cursor_set_current(); c->adv_cursor_scroll = 0;
    }
    const applet_t *a = find_applet(page);
    wm_set_title(c->win, a ? a->name : !strncmp(page, "cat:", 4) ? cats[atoi(page + 4)].name : c->view ? "All Control Panel Items" : "Control Panel");
    c->win->icon = a ? a->icon : ICON_CONTROL_PANEL;
    c->hue = theme_hue; c->sat = theme_sat; c->light = theme_light; c->theme = theme_id; c->wall = wallpaper_id; c->res_sel = 0;
    strlcpy(c->uname, user_name, sizeof c->uname);
    c->dbl = wm_dblclick_ms; c->speed = mouse_speed; c->swap = mouse_swap_buttons;
    c->tz = tz_offset_minutes;
    c->win->dirty = true;
}

#define TOP_H 40
#define NAV_W 200
#define W7_HEAD (T.link_head)
#define W7_GREEN (dark_mode ? HEX(0x7CE07C) : HEX(0x1C7A1C))
#define W7_LINK (T.link)
#define W7_TEXT (mix(T.text, T.window_bg, 55))

/* ---------------- chrome ---------------- */
static void round_nav_button(window_t *w, canvas_t *s, int x, int y, bool back, bool enabled, bool *clicked) {
    rect_t r = R(x, y, 26, 26);
    bool hot = ui_hover(w, r) && enabled;
    *clicked = ui_click_area(w, r) && enabled;
    uint32_t a = enabled ? lighten(T.select, hot ? 85 : 55) : T.face_light, b = enabled ? T.select : T.face;
    fill_circle(s, x + 13, y + 13, 13, enabled ? darken(T.select, 70) : T.face_dark);
    for (int j = -12; j <= 12; j++) { int dx = 0; while ((dx + 1) * (dx + 1) + j * j <= 144) dx++; hline(s, x + 13 - dx, y + 13 + j, 2 * dx + 1, mix(a, b, (j + 12) * 255 / 24)); }
    blend_rect(s, x + 6, y + 3, 14, 6, WHITE, 60);
    int cx = x + 13, cy = y + 13;
    for (int i = 0; i < 6; i++) { int xx = back ? cx - 5 + i : cx + 5 - i; vline(s, xx, cy - i, 2 * i + 1, WHITE); }
    fill_rect(s, back ? cx : cx - 6, cy - 2, 7, 5, WHITE);
}
static void draw_chrome(window_t *w, cp_t *c) {
    canvas_t *s = w->surf;
    gradient_v(s, 0, 0, s->w, TOP_H, T.face_light, T.face);
    hline(s, 0, TOP_H - 1, s->w, T.face_dark);
    bool b1, b2;
    round_nav_button(w, s, 8, 7, true, c->hpos > 0, &b1);
    round_nav_button(w, s, 38, 7, false, c->hpos < c->hlen - 1, &b2);
    if (b1) { c->hpos--; cp_go(c, c->hist[c->hpos], false); }
    if (b2) { c->hpos++; cp_go(c, c->hist[c->hpos], false); }
    /* breadcrumb */
    int bx = 72, bw = s->w - bx - 210;
    rect_t br = R(bx, 8, bw, 24);
    fill_rect(s, br.x, br.y, br.w, br.h, T.window_bg); draw_rect(s, br.x, br.y, br.w, br.h, T.face_dark);
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
        draw_arrow(s, x + 2, 20, 3, T.gray_text); x += 10;
        rect_t r = R(x - 2, 10, text_width(FONT_UI, crumbs[i]) + 6, 20);
        bool hot = ui_hover(w, r);
        if (hot) { fill_rect(s, r.x, r.y, r.w, r.h, T.face_light); draw_rect(s, r.x, r.y, r.w, r.h, T.face_dark); }
        if (ui_click_area(w, r) && strcmp(targets[i], c->page)) { cp_go(c, targets[i], true); return; }
        draw_text(s, FONT_UI, x + 1, 13, crumbs[i], T.text);
        x += r.w + 2;
    }
    /* search box */
    rect_t sr = R(s->w - 200, 8, 190, 24);
    ui_textbox(w, sr, c->search, sizeof c->search);
    if (!c->search[0]) draw_text(s, FONT_UI, sr.x + 6, sr.y + 5, "Search Control Panel", T.gray_text);
    draw_icon(s, ICON_SEARCH, 16, sr.x + sr.w - 20, sr.y + 4);
}
static void nav_pane(window_t *w, cp_t *c, int cat) {
    canvas_t *s = w->surf;
    /* Control Panel has a quiet navigation rail, not Explorer's saturated XP
     * task-pane backdrop: blue links must remain readable in light mode too. */
    gradient_v(s, 0, TOP_H, NAV_W, s->h - TOP_H, mix(T.face, T.window_bg, 100), T.face);
    vline(s, NAV_W - 1, TOP_H, s->h - TOP_H, T.face_dark);
    int y = TOP_H + 16;
    if (ui_link(w, 16, y, FONT_UI, "Control Panel Home", W7_LINK)) { cp_go(c, "", true); return; }
    y += 30;
    for (int i = 0; i < CAT_N; i++) {
        bool cur = i == cat;
        int f = cur ? FONT_UI_BOLD : FONT_UI;
        int th = text_wrapped_height(f, NAV_W - 30, cats[i].name, 2);
        if (cur) draw_text_wrapped(s, f, 16, y, NAV_W - 30, cats[i].name, T.text, 2);
        else if (ui_link_wrapped(w, 16, y, NAV_W - 30, f, cats[i].name, W7_LINK)) { char p[16]; snprintf(p, sizeof p, "cat:%d", i); cp_go(c, p, true); return; }
        y += MAX(22, th + 8);
    }
    y += 14;
    hline(s, 16, y, NAV_W - 32, T.face_dark); y += 12;
    draw_text(s, FONT_UI, 16, y, "See also", T.gray_text); y += 20;
    if (ui_link(w, 16, y, FONT_UI, "Task Manager", W7_LINK)) taskmgr_open();
    y += 20;
    if (ui_link(w, 16, y, FONT_UI, "Command Prompt", W7_LINK)) terminal_open(NULL, NULL);
}

/* ---------------- home views ---------------- */
static void home_category(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    int avail = s->w - 60;
    int colw = MIN(avail / 2, 400);
    int x0 = ox + 24 + (avail - 2 * colw) / 2;
    for (int i = 0; i < CAT_N; i++) {
        int col = i < 4 ? 0 : 1, row = i % 4;
        int x = x0 + col * colw, y = oy + 70 + row * 100;
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
        if (ui_hover(w, r)) { fill_round_rect(s, r.x, r.y, r.w, r.h, 3, T.face_light); draw_round_rect(s, r.x, r.y, r.w, r.h, 3, T.face_dark); }
        if (ui_click_area(w, r)) { cp_go(c, list[i]->id, true); return; }
        if (large) { draw_icon(s, list[i]->icon, 32, x, y + 6); draw_text(s, FONT_UI, x + 42, y + 10, list[i]->name, W7_LINK); draw_text_ellipsis(s, FONT_SMALL, x + 42, y + 26, cw2 - 60, list[i]->desc, T.gray_text); }
        else { draw_icon(s, list[i]->icon, 16, x, y + 4); draw_text(s, FONT_UI, x + 24, y + 5, list[i]->name, W7_LINK); }
    }
    if (!n) draw_text(s, FONT_UI, ox + 24, oy + 60, "No items match your search.", T.gray_text);
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
        hline(s, ox + 20, y - 12, s->w - ox - 40, T.face_dark);
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
    int wp = id == THEME_CLASSIC ? wallpaper_count - 1 : 0;
    for (int j = 0; style_wallpaper(id)[0] && j < wallpaper_count; j++) if (!strcasecmp(wallpaper_names[j], style_wallpaper(id))) wp = j;
    wallpaper_render(pre, wp);
    draw_window_frame(pre, R(10, 8, 100, 56), "Window", -1, true, 0, -1, -1, 0);
    fill_rect(pre, 10 + T.border, 8 + T.title_h, 100 - 2 * T.border, 56 - T.title_h - T.border, T.face);
    if (T.classic) fill_rect(pre, 0, 70, 120, 10, T.face); else gradient_v(pre, 0, 70, 120, 10, T.tb_top, T.tb_bot);
    if (!T.classic) fill_round_rect(pre, 0, 70, 26, 10, 3, T.start_top); else { fill_rect(pre, 2, 71, 22, 8, T.face); bevel(pre, 2, 71, 22, 8, true); }
    T = save;
    if (sel) { fill_rect(s, x - 4, y - 4, 128, 88, mix(T.window_bg, T.select, 55)); draw_rect(s, x - 4, y - 4, 128, 88, T.select); }
    blit(s, x, y, pre, 0, 0, 120, 80);
    draw_rect(s, x, y, 120, 80, HEX(0x808080));
    canvas_free(pre);
}
static void page_personalization(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Change the visuals on your computer", "Pick a visual style, adjust its hue, saturation and lightness, and choose a background.");
    /* pick up new or edited .wrldstyle files */
    if (c->style_gen != vfs_generation) { c->style_gen = vfs_generation; styles_load(); theme_compute(theme_id, theme_hue, &T); wm_theme_changed(); }
    if (ui_link(w, ox + 20, oy + 56, FONT_UI, "Advanced appearance: colours and cursors...", W7_LINK)) cp_go(c, "advanced", true);
    {   extern void logonthemer_open(void);
        int lx = ox + 40 + text_width(FONT_UI, "Advanced appearance: colours and cursors...");
        if (ui_link(w, lx, oy + 56, FONT_UI, "Welcome screen (logon skins)...", W7_LINK)) logonthemer_open(); }
    int y = oy + 78;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Visual styles", T.text);
    {
        bool dm = dark_mode;
        int dmx = ox + 20 + text_width(FONT_UI_BOLD, "Visual styles") + 24;
        if (ui_checkbox(w, dmx, y - 1, "Dark mode", &dm) && dm != dark_mode) {
            dark_mode = dm; theme_apply(theme_id, theme_hue);
            wm_defer(cp_refresh_theme, NULL);      /* repaint after this paint pass */
            settings_save();
        }
    }
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
        draw_text_centered(s, FONT_UI, R(x - 4, yy + 83, 128, 16), style_name(i), T.text);
    }
    if (pages > 1) {
        char pb[32]; snprintf(pb, sizeof pb, "Page %d of %d", c->style_page + 1, pages);
        draw_text(s, FONT_UI, ox + 24, y + 210, pb, W7_TEXT);
        if (ui_link(w, ox + 110, y + 210, FONT_UI, "Next page", W7_LINK)) c->style_page = (c->style_page + 1) % pages;
    }
    y += MIN(2, (n + 3) / 4) * 104 + (pages > 1 ? 18 : 0);
    draw_text_ellipsis(s, FONT_UI, ox + 20, y - 4, s->w - ox - 40, style_desc(hovered >= 0 ? hovered : theme_id), W7_TEXT);
    y += 16;
    int cw = s->w - ox - 40;                     /* usable width */
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Colour hue", T.text);
    char hb[40]; snprintf(hb, sizeof hb, "%d degrees", c->hue);
    draw_text(s, FONT_UI, ox + 110, y, hb, W7_TEXT);
    theme_t base;
    { int s0 = theme_sat, l0 = theme_light; theme_sat = theme_light = 0; theme_compute(theme_id, 0, &base); theme_sat = s0; theme_light = l0; }
    uint32_t basec = base.renderer == RENDER_AERO || base.renderer == RENDER_PLEX ? base.glass : base.classic ? base.title_top : base.title_mid;
    static const int presets[] = { 0, 30, 120, 160, 200, 280, 320 };
    static const char *pnames[] = { "Default", "Violet", "Rose", "Ruby", "Amber", "Emerald", "Teal" };
    int px0 = ox + 200;
    for (int i = 0; i < 7; i++) {
        int x = px0 + i * 22;
        fill_round_rect(s, x, y - 2, 18, 18, 3, HEX(0x606060));
        fill_round_rect(s, x + 1, y - 1, 16, 16, 2, hue_shift(basec | 0xFF000000, presets[i]));
        if (ui_click_area(w, R(x, y - 2, 18, 18))) { c->hue = presets[i]; theme_apply(theme_id, presets[i]); wm_redraw_all(); }
        if (ui_hover(w, R(x, y - 2, 18, 18))) draw_text(s, FONT_SMALL, px0 + 7 * 22 + 6, y + 1, pnames[i], W7_TEXT);
    }
    if (ui_link(w, ox + 20 + cw - text_width(FONT_UI, "Reset colours"), y, FONT_UI, "Reset colours", W7_LINK)) {
        c->hue = c->sat = c->light = 0; theme_sat = theme_light = 0; theme_apply(theme_id, 0); wm_redraw_all();
    }
    y += 20;
    int hw = MIN(cw, 560);
    for (int i = 0; i < hw - 10; i += 2) fill_rect(s, ox + 25 + i, y, 2, 8, hue_shift(basec | 0xFF000000, i * 360 / (hw - 10)));
    if (ui_slider(w, R(ox + 20, y + 10, hw, 24), &c->hue, 0, 359)) { theme_apply(theme_id, c->hue); wm_redraw_all(); }
    y += 42;
    /* saturation and lightness, side by side */
    {
        char vb[24];
        int half = (MIN(cw, 560) - 20) / 2, x2 = ox + 20 + half + 20;
        draw_text(s, FONT_UI_BOLD, ox + 20, y, "Saturation", T.text);
        snprintf(vb, sizeof vb, "%+d%%", c->sat); draw_text(s, FONT_UI, ox + 105, y, c->sat ? vb : "normal", W7_TEXT);
        draw_text(s, FONT_UI_BOLD, x2, y, "Lightness", T.text);
        snprintf(vb, sizeof vb, "%+d%%", c->light); draw_text(s, FONT_UI, x2 + 80, y, c->light ? vb : "normal", W7_TEXT);
        uint32_t bc = hue_shift(basec | 0xFF000000, c->hue);
        for (int i = 0; i < half - 10; i += 2) {
            fill_rect(s, ox + 25 + i, y + 20, 2, 8, color_adjust(bc, -100 + i * 200 / (half - 10), 0));
            fill_rect(s, x2 + 5 + i, y + 20, 2, 8, color_adjust(bc, 0, -60 + i * 120 / (half - 10)));
        }
        bool ch = ui_slider(w, R(ox + 20, y + 30, half, 24), &c->sat, -100, 100);
        ch |= ui_slider(w, R(x2, y + 30, half, 24), &c->light, -60, 60);
        if (ch) { theme_set_tone(c->sat, c->light); wm_redraw_all(); }
        y += 64;
    }
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Desktop background", T.text);
    static canvas_t *thumbs[12];
    static const char *fit_names[] = { "Crop", "Scale", "Stretch" };
    int fx = ox + 20 + text_width(FONT_UI_BOLD, "Desktop background") + 18;
    draw_text(s, FONT_UI, fx, y + 1, "Fit:", T.gray_text);
    for (int i = 0; i < 3; i++)
        if (ui_radio(w, fx + 32 + i * 82, y - 1, fit_names[i], wallpaper_fit == i) && wallpaper_fit != i) {
            wallpaper_fit = i;
            for (int k = 0; k < 12; k++) if (thumbs[k]) { canvas_free(thumbs[k]); thumbs[k] = NULL; }
            shell_wallpaper_changed();
            wm_need_redraw = true;
        }
    y += 22;
    int wper = 6, wpages = (wallpaper_count + wper - 1) / wper;
    if (wpages > 1) {
        if (ui_link(w, ox + 110, y + 88, FONT_UI, "Next page", W7_LINK)) c->wall_page = (c->wall_page + 1) % wpages;
        char pb[32]; snprintf(pb, sizeof pb, "Page %d of %d", c->wall_page + 1, wpages);
        draw_text(s, FONT_UI, ox + 24, y + 88, pb, W7_TEXT);
    }
    if (c->wall_page >= wpages) c->wall_page = 0;
    for (int k = 0; k < wper && c->wall_page * wper + k < wallpaper_count; k++) {
        int i = c->wall_page * wper + k;
        int x = ox + 24 + k * 100;
        if (!thumbs[i]) {
            canvas_t *big = canvas_new(SCREEN_W / 4, SCREEN_H / 4);
            wallpaper_render(big, i);
            thumbs[i] = big;
        }
        bool sel = wallpaper_id == i;
        if (sel) { fill_rect(s, x - 3, y - 3, 92, 70, mix(T.window_bg, T.select, 55)); draw_rect(s, x - 3, y - 3, 92, 70, T.select); }
        for (int j = 0; j < 64; j++) for (int q = 0; q < 86; q++) put_pixel(s, x + q, y + j, i == wallpaper_count - 1 ? desktop_color : thumbs[i]->px[(j * thumbs[i]->h / 64) * thumbs[i]->w + q * thumbs[i]->w / 86]);
        draw_rect(s, x, y, 86, 64, HEX(0x808080));
        draw_text_centered(s, FONT_UI, R(x - 3, y + 67, 92, 16), wallpaper_names[i], T.text);
        if (ui_click_area(w, R(x - 3, y - 3, 92, 86))) { wallpaper_id = i; shell_wallpaper_changed(); wm_need_redraw = true; }
    }
    y += 92 + (wpages > 1 ? 20 : 0);
    if (wallpaper_id == wallpaper_count - 1) {
        draw_text(s, FONT_UI, ox + 20, y + 4, "Desktop colour:", T.text);
        static const uint32_t cols[] = { 0x3A6EA5, 0x008080, 0x000000, 0x6E7F48, 0x5A4A7A, 0xA05030, 0x404858, 0xC0C0C0 };
        for (int i = 0; i < 8; i++) {
            rect_t r = R(ox + 120 + i * 28, y, 22, 22);
            fill_rect(s, r.x, r.y, r.w, r.h, HEX(cols[i])); draw_rect(s, r.x, r.y, r.w, r.h, (desktop_color & 0xFFFFFF) == cols[i] ? HEX(0x3399FF) : HEX(0x808080));
            if (ui_click_area(w, r)) { desktop_color = HEX(cols[i]); shell_wallpaper_changed(); }
        }
    }
}
static void page_advanced(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Advanced appearance", "Change individual colours and choose a cursor set. Changes are saved for this user.");
    int n = theme_role_count();
    if (c->adv_sel < 0 || c->adv_sel >= n) c->adv_sel = 0;
    int lx = ox + 20, ly = oy + 64, lw = 240, lh = MIN(390, s->h - ly - 24);
    draw_sunken_box(s, R(lx, ly, lw, lh), T.window_bg);
    const char *names[40];
    for (int i = 0; i < n && i < 40; i++) names[i] = theme_role_name(i);
    ui_list(w, R(lx + 1, ly + 1, lw - 2, lh - 2), names, n, &c->adv_sel, &c->adv_scroll);
    if (!c->adv_init || c->adv_sel != c->adv_last) {
        c->adv_last = c->adv_sel; c->adv_init = true;
        uint32_t col = theme_role_effective(c->adv_sel);
        c->adv_r = C_R(col); c->adv_g = C_G(col); c->adv_b = C_B(col);
    }
    int px = lx + lw + 24, pw = s->w - px - 20;
    if (pw < 220) { px = lx; pw = lw; }
    int sel = CLAMP(c->adv_sel, 0, n - 1);
    uint32_t col = theme_role_effective(sel);
    draw_text(s, FONT_LARGE, px, ly, theme_role_name(sel), T.text);
    draw_text(s, FONT_UI, px, ly + 24, theme_role_is_set(sel) ? "Custom colour" : "Automatic (from the current style)", T.gray_text);
    fill_rect(s, px, ly + 46, 120, 38, col);
    draw_rect(s, px, ly + 46, 120, 38, T.face_dark);
    char hx[16]; snprintf(hx, sizeof hx, "#%06X", col & 0xFFFFFF);
    draw_text(s, FONT_UI, px + 132, ly + 56, hx, T.text);
    int sy = ly + 100, changed = 0;
    const char *lab[] = { "Red", "Green", "Blue" };
    int *vv[] = { &c->adv_r, &c->adv_g, &c->adv_b };
    for (int i = 0; i < 3; i++) {
        draw_text(s, FONT_UI, px, sy + 4, lab[i], T.text);
        int v = *vv[i];
        if (ui_slider(w, R(px + 58, sy, MIN(230, pw - 58), 22), &v, 0, 255)) { *vv[i] = v; changed = 1; }
        char b[8]; snprintf(b, sizeof b, "%d", *vv[i]); draw_text(s, FONT_UI, px + pw - 32, sy + 4, b, T.text);
        sy += 30;
    }
    if (changed) {
        theme_role_set(sel, RGB(c->adv_r, c->adv_g, c->adv_b));
        theme_apply(theme_id, theme_hue);
        if (!w->ui.down || w->ui.released) theme_overrides_save();
        wm_defer(cp_refresh_theme, NULL);
    }
    sy += 4;
    if (ui_button(w, R(px, sy, 90, 24), "Automatic")) {
        theme_role_clear(sel); theme_apply(theme_id, theme_hue); theme_overrides_save();
        c->adv_init = false; wm_defer(cp_refresh_theme, NULL);
    }
    if (ui_button(w, R(px + 100, sy, 90, 24), "Reset all")) {
        theme_role_clear_all(); theme_apply(theme_id, theme_hue); theme_overrides_save();
        c->adv_init = false; wm_defer(cp_refresh_theme, NULL);
    }
    sy += 42;
    draw_text(s, FONT_UI_BOLD, px, sy, "Cursor set", T.link_head);
    sy += 20;
    int nc = cursor_set_count();
    const char *cn[16];
    for (int i = 0; i < nc && i < 16; i++) cn[i] = cursor_set_name(i);
    if (c->adv_cursor < 0 || c->adv_cursor >= nc) c->adv_cursor = cursor_set_current();
    draw_sunken_box(s, R(px, sy, MIN(240, pw), MIN(90, MAX(26, nc * 18 + 4))), T.window_bg);
    ui_list(w, R(px + 1, sy + 1, MIN(240, pw) - 2, MIN(90, MAX(26, nc * 18 + 4)) - 2), cn, nc, &c->adv_cursor, &c->adv_cursor_scroll);
    if (c->adv_cursor >= 0 && c->adv_cursor < nc && c->adv_cursor != cursor_set_current()) {
        cursor_set_load(cn[c->adv_cursor]);
        settings_save();
        wm_defer(cp_refresh_theme, NULL);
    }
    sy += MIN(90, MAX(26, nc * 18 + 4)) + 8;
    draw_text_wrapped(s, FONT_SMALL, px, sy, pw, "Put Windows .cur files in C:\\WRLD\\Cursors\\<set>\\ named arrow, ibeam, hand, sizens, sizewe, sizenwse, sizenesw, busy, move and cross. Missing files keep the built-in cursor.", T.gray_text, 3);
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
    draw_text(s, FONT_UI, x, y, "Display adapter:", T.gray_text); draw_text_ellipsis(s, FONT_UI, x + 110, y, s->w - x - 130, video_driver_name(), T.text); y += 22;
    int avail[ARRAY_LEN(modes)], na = 0, cur = 0;
    for (unsigned i = 0; i < ARRAY_LEN(modes); i++) if (video_mode_ok(modes[i].w, modes[i].h)) { if (modes[i].w == SCREEN_W && modes[i].h == SCREEN_H) cur = na; avail[na++] = i; }
    if (!c->res_sel && na) c->res_sel = cur + 1;          /* slider position + 1 (0 = not yet set) */
    if (video_can_set_mode() && na > 1) {
        draw_text(s, FONT_UI, x, y + 4, "Screen resolution:", T.gray_text);
        int lx = x + 130;
        int sw = MAX(120, MIN(300, s->w - lx - 30));
        draw_text(s, FONT_SMALL, lx, y + 26, "Less", W7_TEXT); draw_text(s, FONT_SMALL, lx + sw - 24, y + 26, "More", W7_TEXT);
        int pos = c->res_sel - 1;
        if (ui_slider(w, R(lx, y, sw, 24), &pos, 0, na - 1)) c->res_sel = pos + 1;
        int m = avail[CLAMP(c->res_sel - 1, 0, na - 1)];
        snprintf(b, sizeof b, "%d by %d pixels", modes[m].w, modes[m].h);
        draw_text_centered(s, FONT_UI_BOLD, R(lx, y + 40, sw, 16), b, T.text);
        y += 66;
        bool changed = modes[m].w != SCREEN_W || modes[m].h != SCREEN_H;
        if (ui_button_ex(w, R(lx, y, 90, 24), "Apply", changed, false) && changed) {
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
    if (ui_link(w, x, y, FONT_UI, "Visual effects and animations...", W7_LINK)) cp_go(c, "effects", true);
    y += 28;
    if (ui_link(w, x, y, FONT_UI, "Personalize visual style and colours", W7_LINK)) cp_go(c, "personalization", true);
}
static void page_effects(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Choose your visual effects", "Animations follow your visual style and never pause mouse or keyboard handling.");
    int x = ox + 24, y = oy + 88, width = s->w - x - 28;
    if (ui_checkbox(w, x, y, "Enable visual animations", &wm_animations)) { wm_effects_changed(); settings_save(); }
    y += 42;
    static const char *windows[] = { "None (instant)", "Border scale (Windows XP)", "Aero fade (Vista / 7)" };
    static const char *menus[] = { "None (instant)", "Slide", "Fade" };
    draw_text(s, FONT_UI_BOLD, x, y + 4, "Windows", T.text);
    ui_dropdown(w, R(x + 130, y, MIN(280, width - 130), 24), windows, WINDOW_ANIM_COUNT, &wm_animation_style);
    draw_text_wrapped(s, FONT_UI, x, y + 34, width, "Applied when opening, closing, minimizing, restoring and maximizing windows. Border scale moves the outline; Aero fades the whole window.", W7_TEXT, 2);
    y += 94;
    hline(s, x, y - 12, width, T.face_dark);
    draw_text(s, FONT_UI_BOLD, x, y + 4, "Start menu", T.text);
    ui_dropdown(w, R(x + 130, y, MIN(280, width - 130), 24), menus, MENU_ANIM_COUNT, &startmenu_animation);
    y += 54;
    draw_text(s, FONT_UI_BOLD, x, y, "Sidebar", T.text); y += 26;
    bool changed = ui_checkbox(w, x + 8, y, "Slide the sidebar in and out", &sidebar_slide); y += 28;
    changed |= ui_checkbox(w, x + 8, y, "Fade sidebar tiles when opening, adding or removing", &sidebar_tile_fade);
    if (changed) { wm_effects_changed(); settings_save(); }
    y += 40;
    hline(s, x, y - 12, width, T.face_dark);
    {   /* the 3D engine */
        extern bool accel3d;
        draw_text(s, FONT_UI_BOLD, x, y, "3D", T.text); y += 26;
        if (ui_checkbox(w, x + 8, y, "Enable 3D acceleration (WRLD 3D engine)", &accel3d)) { wm_effects_changed(); settings_save(); }
        y += 24;
        y += draw_text_wrapped(s, FONT_UI, x + 30, y, width - 30, accel3d
            ? "On: Alt+Tab shows your windows in Flip 3D, and Windows programs can draw 3D with OpenGL and Direct3D 9. The engine runs on the processor, since THE WRLD OS has no graphics card driver."
            : "Off: Alt+Tab switches windows directly, and 3D programs show a message instead.", W7_TEXT, 2) + 6;
        if (ui_button(w, R(x + 30, y, 150, 26), "Test 3D (WRLD 3D)")) demo3d_open();
        y += 44;
    }
    int bw = MIN(140, (width - 20) / 3);
    if (ui_button(w, R(x, y, bw, 26), "Preview window")) notepad_open(NULL);
    if (ui_button(w, R(x + bw + 10, y, bw, 26), "Preview Start menu")) startmenu_toggle();
    if (ui_button(w, R(x + 2 * (bw + 10), y, bw, 26), "Toggle sidebar")) sidebar_open = !sidebar_open;
    y += 44;
    draw_text_wrapped(s, FONT_UI, x, y, width, wm_animations ? "Settings are saved automatically. Choose None for individual effects, or clear Enable visual animations to turn all of them off." : "All visual animations are off. Your individual choices are kept for when you enable them again.", W7_TEXT, 2);
    (void)c;
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
    draw_text(s, FONT_UI, ox + 36, y, "Start menu style follows the visual style:", T.text); y += 20;
    draw_text(s, FONT_UI, ox + 36, y, "Luna styles use the two-column menu; Classic uses the cascading menu.", W7_TEXT);
    if (ui_link(w, ox + 36, y + 48, FONT_UI, "Change Start menu and sidebar animations...", W7_LINK)) cp_go(c, "effects", true);
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
        fill_circle(s, cx + sx * (r - 10) / 256, cy + sy * (r - 10) / 256, i % 3 ? 2 : 4, T.link_head);
    }
    int ha = ((t.hour % 12) * 60 + t.minute) * 1024 / 720, ma = t.minute * 1024 / 60, sa = t.second * 1024 / 60;
    draw_thick_line(s, cx, cy, cx + ext_isin(ha) * (r - 35) / 256, cy - ext_isin(ha + 256) * (r - 35) / 256, 5, HEX(0x202020));
    draw_thick_line(s, cx, cy, cx + ext_isin(ma) * (r - 18) / 256, cy - ext_isin(ma + 256) * (r - 18) / 256, 3, HEX(0x202020));
    draw_line(s, cx, cy, cx + ext_isin(sa) * (r - 12) / 256, cy - ext_isin(sa + 256) * (r - 12) / 256, HEX(0xD02020));
    fill_circle(s, cx, cy, 4, HEX(0x202020));
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mons[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char b[96];
    snprintf(b, sizeof b, "%02d:%02d:%02d", t.hour, t.minute, t.second); draw_text(s, FONT_XL, ox + 230, oy + 90, b, T.text);
    snprintf(b, sizeof b, "%s, %s %d, %d", days[t.weekday], mons[(t.month + 11) % 12], t.day, t.year); draw_text(s, FONT_UI, ox + 232, oy + 128, b, T.text);
    draw_text(s, FONT_UI_BOLD, ox + 232, oy + 170, "Time zone", T.text);
    snprintf(b, sizeof b, "UTC%c%02d:%02d", c->tz < 0 ? '-' : '+', ABSDIFF(c->tz, 0) / 60, ABSDIFF(c->tz, 0) % 60); draw_text(s, FONT_UI, ox + 320, oy + 170, b, T.text);
    int hours = c->tz / 30;
    if (ui_slider(w, R(ox + 230, oy + 190, 300, 24), &hours, -24, 28)) { c->tz = hours * 30; tz_offset_minutes = c->tz; wm_need_redraw = true; }
    draw_text(s, FONT_SMALL, ox + 230, oy + 216, "UTC-12", W7_TEXT); draw_text(s, FONT_SMALL, ox + 500, oy + 216, "UTC+14", W7_TEXT);
    ui_checkbox(w, ox + 232, oy + 240, "Use 24-hour clock in the taskbar", &clock_24h);
    draw_text_wrapped(s, FONT_UI, ox + 30, oy + 280, s->w - ox - 60, "The hardware clock (CMOS RTC) is read directly by the kernel and treated as UTC; the time zone offset is applied for display.", W7_TEXT, 2);
}
static void admin_restart_cb(void *ctx, int r) { (void)ctx; if (r == IDYES) shellmanager_restart(); }
static void page_admin(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Administrative Tools", "System tools for this computer. Restarting the shell closes all open windows but not the system.");
    struct tool { const char *name, *desc; int icon, action; };
    static const struct tool tools[] = {
        { "Task Manager", "Processes, CPU and memory", ICON_TASKMGR, 1 },
        { "Device Manager", "Hardware and driver status", ICON_CP_DEVICES, 2 },
        { "System Information", "Edition, processor and memory", ICON_CP_SYSTEM, 3 },
        { "Network and Sharing Center", "Adapters and addresses", ICON_CP_NETWORK, 4 },
        { "Sound", "Playback devices and volume", ICON_CP_SOUND, 5 },
        { "Programs and Features", "Installed and built-in programs", ICON_CP_PROGRAMS, 6 },
        { "Command Prompt", "A shell with administration commands", ICON_TERMINAL, 7 },
        { "Restart Shell Manager", "Recover the desktop, taskbar and window manager", ICON_RESTART, 8 },
        { "WRLD Security Center", "Firewall, password, web protection and system health", ICON_CP_SYSTEM, 9 },
    };
    int act = 0, y = oy + 72;
    for (unsigned i = 0; i < ARRAY_LEN(tools); i++) {
        int col = i % 2, row = i / 2;
        int x = ox + 20 + col * ((s->w - ox - 40) / 2), yy = y + row * 58;
        rect_t r = R(x - 4, yy - 4, (s->w - ox - 40) / 2 - 8, 50);
        if (ui_hover(w, r)) fill_round_rect(s, r.x, r.y, r.w, r.h, 4, T.face_light);
        if (ui_click_area(w, r)) act = tools[i].action;
        draw_icon(s, tools[i].icon, 32, x, yy + 2);
        draw_text(s, FONT_UI_BOLD, x + 42, yy + 2, tools[i].name, T.text);
        draw_text_ellipsis(s, FONT_SMALL, x + 42, yy + 20, r.w - 46, tools[i].desc, T.gray_text);
    }
    int sy = y + 5 * 58 + 10;
    draw_text(s, FONT_UI_BOLD, ox + 20, sy, "Storage devices", T.link_head);
    hline(s, ox + 150, sy + 8, s->w - ox - 180, T.face_dark); sy += 24;
    for (int i = 0; i < blk_count() && i < 8; i++) {
        char b[160]; blk_describe(blk_get(i), b, sizeof b);
        draw_text_ellipsis(s, FONT_UI, ox + 32, sy, s->w - ox - 60, b, T.text);
        sy += 18;
    }
    switch (act) {
    case 1: taskmgr_open(); break;
    case 2: cp_go(c, "devices", true); break;
    case 3: cp_go(c, "system", true); break;
    case 4: cp_go(c, "network", true); break;
    case 5: cp_go(c, "sound", true); break;
    case 6: cp_go(c, "programs", true); break;
    case 7: terminal_open(NULL, NULL); break;
    case 8: msgbox("Restart Shell Manager", "This restarts the desktop, taskbar, sidebar and window manager. All open windows will be closed, but the computer keeps running.", MBI_WARNING, MBX_YESNO, admin_restart_cb, NULL); break;
    case 9: security_open(); break;
    }
}
static void page_system(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "View basic information about your computer", NULL);
    int y = oy + 64;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Edition", T.link_head); hline(s, ox + 80, y + 8, s->w - ox - 120, T.face_dark); y += 24;
    draw_text(s, FONT_UI, ox + 40, y, OS_NAME " " OS_EDITION, T.text); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Version " OS_VERSION " (Build " OS_BUILD ")", T.text); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "An original operating system with an XP-style shell", W7_TEXT);
    draw_icon(s, ICON_LOGO, 48, s->w - 90, oy + 64);
    y += 30;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "System", T.link_head); hline(s, ox + 80, y + 8, s->w - ox - 120, T.face_dark); y += 24;
    char v[16], br[52], b[160]; cpu_get_info(v, br);
    const char *lab[] = { "Computer name:", "Installed on:", "Processor:", "Installed memory:", "Kernel heap:", "System type:", "Uptime:", "Tasks:" };
    char vals[8][96];
    strlcpy(vals[6], computer_name, 96);
    persist_status(vals[7], 96);
    snprintf(vals[0], 96, "%s (%s)", br, v);
    snprintf(vals[1], 96, "%llu MB", (unsigned long long)(mm_phys_total() / (1024 * 1024)));
    snprintf(vals[2], 96, "%u KB used of %u MB", mm_used() / 1024, mm_total() / (1024 * 1024));
    snprintf(vals[3], 96, OS_ARCH_DESC);
    uint32_t up = timer_ticks() / 1000; snprintf(vals[4], 96, "%u:%02u:%02u", up / 3600, (up / 60) % 60, up % 60);
    snprintf(vals[5], 96, "%d running (cooperative scheduler)", task_count());
    static const int order[8] = { 6, 7, 0, 1, 2, 3, 4, 5 };
    for (int k = 0; k < 8; k++) { int i = order[k]; draw_text(s, FONT_UI, ox + 40, y, lab[k], T.gray_text); draw_text_ellipsis(s, FONT_UI, ox + 170, y, s->w - ox - 200, vals[i], T.text); y += 20; }
    y += 14;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Compatibility", T.link_head); hline(s, ox + 110, y + 8, s->w - ox - 150, T.face_dark); y += 24;
    snprintf(b, sizeof b, "Win32 " OS_EXE_DESC " (.exe): %d kernel32, %d msvcrt, %d user32, %d gdi32 functions", 0, 0, 0, 0);
    extern const int kernel32_count, msvcrt_count, user32_count, gdi32_count;
    snprintf(b, sizeof b, "Win32 " OS_EXE_DESC " (.exe): %d kernel32 / %d msvcrt / %d user32 / %d gdi32 functions", kernel32_count, msvcrt_count, user32_count, gdi32_count);
    draw_text(s, FONT_UI, ox + 40, y, b, T.text); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Shell scripts (.sh): built-in POSIX-style interpreter", T.text); y += 18;
    draw_text(s, FONT_UI, ox + 40, y, "Apple disk images (.dmg): UDIF raw/zlib, FAT volumes, script .app bundles", T.text); y += 26;
    if (ui_link(w, ox + 40, y, FONT_UI, "Device Manager", W7_LINK)) cp_go(c, "devices", true);
    if (!system_volume && ui_link(w, ox + 310, y, FONT_UI, "Install " OS_NAME " to a hard disk", W7_LINK)) setup_launch_from_desktop();
    if (ui_link(w, ox + 160, y, FONT_UI, "Kernel log (dmesg)", W7_LINK)) { vfs_write_file("C:\\tmp\\dmesg.log", klog_buffer(), strlen(klog_buffer())); notepad_open("C:\\tmp\\dmesg.log"); }
}
/* ---------------- Device Manager ---------------- */
static int devclass_icon(int cls) {
    static const int ic[DC_COUNT] = { ICON_COMPUTER, ICON_CP_SYSTEM, ICON_CP_DISPLAY, ICON_DRIVE, ICON_DRIVE, ICON_CP_NETWORK, ICON_CP_SOUND,
                                      ICON_CP_DEVICES, ICON_CP_DEVICES, ICON_CP_KEYBOARD, ICON_CP_MOUSE, ICON_CP_DEVICES, ICON_CP_HARDWARE, ICON_QUESTION };
    return cls >= 0 && cls < DC_COUNT ? ic[cls] : ICON_CP_DEVICES;
}
static void dev_key(const device_t *d, char *out, int cap) { snprintf(out, cap, "%s|%s", d->hwid, d->location); }
static const char *dev_status_text(const device_t *d) {
    switch (d->status) {
    case DS_WORKING: return "Working";
    case DS_BUILTIN: return "Built in (no driver needed)";
    case DS_NOT_STARTED: return "Driver available, device not in use";
    case DS_FAILED: return "The driver could not start this device";
    default: return "No driver";
    }
}
/* a 9x9 expand/collapse box, as in a tree view */
static void tree_box(canvas_t *s, int x, int y, bool open) {
    fill_rect(s, x, y, 9, 9, T.window_bg); draw_rect(s, x, y, 9, 9, T.face_dark);
    hline(s, x + 2, y + 4, 5, T.text);
    if (!open) vline(s, x + 4, y + 2, 5, T.text);
}
static void page_devices(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    char sum[160]; devmgr_summary(sum, sizeof sum);
    heading(s, ox + 20, oy + 16, "Device Manager", sum);
    int y = oy + 70;
    if (ui_button(w, R(ox + 20, y, 180, 24), "Scan for hardware changes")) { devmgr_scan(); w->dirty = true; }
    if (ui_tab(w, R(ox + 216, y, 130, 24), "Devices by type", c->dev_view == 0)) { c->dev_view = 0; c->scroll = 0; w->dirty = true; }
    if (ui_tab(w, R(ox + 346, y, 130, 24), "Built-in drivers", c->dev_view == 1)) { c->dev_view = 1; c->scroll = 0; w->dirty = true; }
    int top = y + 34, panel_h = c->dev_view == 0 ? 118 : 0, bottom = s->h - panel_h - 8;
    rect_t area = R(ox + 20, top, s->w - ox - 40, bottom - top);
    fill_rect(s, area.x, area.y, area.w, area.h, T.window_bg); draw_rect(s, area.x, area.y, area.w, area.h, T.face_dark);
    rect_t old = canvas_push_clip(s, R(area.x + 1, area.y + 1, area.w - 2, area.h - 2));
    int n = devmgr_count(), row = 20, ry = area.y + 6 - c->scroll, content = 0;
    if (c->dev_view == 1) {
        /* every driver in the kernel, and how many devices it is running */
        const driver_t *const *drv; int nd = devmgr_drivers(&drv);
        int cx[4] = { area.x + 10, area.x + 110, area.x + 180, area.x + 250 };
        static const char *hd[4] = { "Driver", "Bus", "In use", "Description" };
        for (int k = 0; k < 4; k++) draw_text(s, FONT_UI_BOLD, cx[k], ry, hd[k], T.link_head);
        ry += row; content += row;
        for (int cls = 0; cls < DC_COUNT; cls++)
            for (int k = 0; k < nd; k++) {
                if (drv[k]->cls != cls) continue;
                int used = 0;
                for (int i = 0; i < n; i++) { device_t *d = devmgr_get(i); if (d->drv == drv[k] && d->status == DS_WORKING) used++; }
                char cnt[12]; if (used) snprintf(cnt, sizeof cnt, "%d", used); else strcpy(cnt, "-");
                uint32_t col = used ? T.text : T.gray_text;
                draw_icon(s, devclass_icon(cls), 16, cx[0] - 2, ry - 1);
                draw_text(s, FONT_UI, cx[0] + 18, ry, drv[k]->name, col); draw_text(s, FONT_UI, cx[1] + 18, ry, drv[k]->bus, col);
                draw_text(s, FONT_UI, cx[2] + 18, ry, cnt, col);
                draw_text_ellipsis(s, FONT_UI, cx[3] + 18, ry, area.x + area.w - cx[3] - 28, drv[k]->desc, col);
                ry += row; content += row;
            }
        char foot[120]; snprintf(foot, sizeof foot, "%d drivers are built into %s. Greyed drivers have no matching device in this computer.", nd, OS_NAME);
        ry += 6; draw_text(s, FONT_SMALL, cx[0], ry, foot, W7_TEXT); content += row + 6;
    } else {
        draw_icon(s, ICON_COMPUTER, 16, area.x + 8, ry); draw_text(s, FONT_UI_BOLD, area.x + 30, ry + 1, computer_name, T.text);
        ry += row; content += row;
        for (int cls = 0; cls < DC_COUNT; cls++) {
            int members = 0; bool trouble = false;
            for (int i = 0; i < n; i++) { device_t *d = devmgr_get(i); if (d->cls == cls) { members++; if (d->status == DS_NO_DRIVER || d->status == DS_FAILED) trouble = true; } }
            if (!members) continue;
            bool open = !(c->dev_closed & (1u << cls));
            rect_t hr = R(area.x + 4, ry - 1, area.w - 8, row);
            if (ui_click_area(w, hr)) { c->dev_closed ^= 1u << cls; w->dirty = true; }
            tree_box(s, area.x + 26, ry + 3, open);
            draw_icon(s, devclass_icon(cls), 16, area.x + 42, ry);
            draw_text(s, FONT_UI, area.x + 64, ry + 1, devclass_name(cls), T.text);
            if (trouble && !open) draw_icon(s, ICON_WARNING, 16, area.x + 68 + text_width(FONT_UI, devclass_name(cls)), ry);
            ry += row; content += row;
            if (!open) continue;
            for (int i = 0; i < n; i++) {
                device_t *d = devmgr_get(i);
                if (d->cls != cls) continue;
                char key[112]; dev_key(d, key, sizeof key);
                bool sel = !strcmp(key, c->dev_sel);
                rect_t dr = R(area.x + 60, ry - 1, area.w - 68, row);
                if (ui_click_area(w, dr)) { strlcpy(c->dev_sel, key, sizeof c->dev_sel); sel = true; w->dirty = true; }
                int tw = MIN(text_width(FONT_UI, d->name), area.w - 110);
                if (sel) fill_rect(s, area.x + 84, ry - 1, tw + 8, row - 1, T.select);
                int icon = d->status == DS_NO_DRIVER ? ICON_QUESTION : d->status == DS_FAILED ? ICON_WARNING : devclass_icon(cls);
                draw_icon(s, icon, 16, area.x + 66, ry);
                uint32_t col = sel ? T.select_text : d->status == DS_NOT_STARTED ? T.gray_text : T.text;
                draw_text_ellipsis(s, FONT_UI, area.x + 88, ry + 1, area.w - 110, d->name, col);
                if (d->is_new) draw_text(s, FONT_SMALL, area.x + 96 + tw, ry + 2, "new", W7_GREEN);
                ry += row; content += row;
            }
        }
    }
    s->clip = old;
    c->scroll_max = MAX(0, content + 12 - area.h);
    if (c->scroll > c->scroll_max) { c->scroll = c->scroll_max; w->dirty = true; }
    if (c->scroll_max) {                                   /* a thin position bar */
        int bh = MAX(24, area.h * area.h / (content + 12)), by = area.y + 2 + (area.h - 4 - bh) * c->scroll / c->scroll_max;
        fill_rect(s, area.x + area.w - 7, by, 5, bh, T.sb_thumb_top);
    }
    if (c->dev_view != 0) return;
    /* the selected device */
    int py = bottom + 8, px = ox + 20;
    device_t *d = NULL;
    for (int i = 0; i < n && !d; i++) { char key[112]; dev_key(devmgr_get(i), key, sizeof key); if (!strcmp(key, c->dev_sel)) d = devmgr_get(i); }
    if (!d) { draw_text(s, FONT_UI, px, py + 8, "Click a device to see its driver, status and hardware ID.", W7_TEXT); return; }
    draw_icon(s, d->status == DS_NO_DRIVER ? ICON_QUESTION : d->status == DS_FAILED ? ICON_WARNING : devclass_icon(d->cls), 32, px, py + 2);
    draw_text_ellipsis(s, FONT_UI_BOLD, px + 42, py, s->w - px - 60, d->name, T.text);
    char line[220];
    uint32_t stcol = d->status == DS_WORKING ? W7_GREEN : (d->status == DS_NO_DRIVER || d->status == DS_FAILED) ? (dark_mode ? HEX(0xFF9C80) : HEX(0xB03020)) : W7_TEXT;
    draw_text(s, FONT_UI, px + 42, py + 18, "Status:", T.gray_text); draw_text(s, FONT_UI, px + 130, py + 18, dev_status_text(d), stcol);
    if (d->drv) snprintf(line, sizeof line, "%s  -  %s", d->drv->name, d->drv->desc); else strcpy(line, "None");
    draw_text(s, FONT_UI, px + 42, py + 36, "Driver:", T.gray_text); draw_text_ellipsis(s, FONT_UI, px + 130, py + 36, s->w - px - 150, line, T.text);
    draw_text(s, FONT_UI, px + 42, py + 54, "Hardware ID:", T.gray_text); draw_text(s, FONT_UI, px + 130, py + 54, d->hwid, T.text);
    if (d->location[0]) { int hx = px + 140 + text_width(FONT_UI, d->hwid); draw_text_ellipsis(s, FONT_UI, hx, py + 54, s->w - hx - 20, d->location, T.gray_text); }
    draw_text_wrapped(s, FONT_UI, px + 42, py + 74, s->w - px - 70, d->detail, W7_TEXT, 2);
}
static void page_network(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "View your basic network information", NULL);
    bool ok = net_ready();
    draw_icon(s, ICON_COMPUTER, 48, ox + 40, oy + 80); draw_text(s, FONT_UI, ox + 30, oy + 134, computer_name, T.text);
    if (ok) for (int x = ox + 100; x < ox + 240; x += 2) hline(s, x, oy + 104, 1, HEX(0x2E8E2E));
    else { for (int x = ox + 100; x < ox + 240; x += 8) hline(s, x, oy + 104, 4, HEX(0xC03030)); draw_icon(s, ICON_ERROR, 16, ox + 162, oy + 96); }
    draw_icon(s, ICON_CP_NETWORK, 48, ox + 250, oy + 80); draw_text(s, FONT_UI, ox + 254, oy + 134, "Internet", T.text);
    int y = oy + 170;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Local Area Connection", T.link_head); hline(s, ox + 160, y + 8, s->w - ox - 190, T.face_dark); y += 24;
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
    for (int i = 0; i < 8; i++) { if (!netif.present && i > 1) break; draw_text(s, FONT_UI, ox + 40, y, lab[i], T.gray_text); draw_text(s, FONT_UI, ox + 170, y, vals[i], T.text); y += 20; }
    y += 10;
    if (netif.present && ui_button(w, R(ox + 40, y, 150, 26), "Renew address (DHCP)")) dhcp_renew();
    if (ui_link(w, ox + 210, y + 5, FONT_UI, "Open The WRLD Browser", W7_LINK)) browser_open(NULL);
    y += 40;
    snprintf(b, sizeof b, "Secure websites are checked against %d trusted root certificates (add your own in C:\\WRLD\\Certificates).", tls_trust_anchor_count());
    draw_text_wrapped(s, FONT_UI, ox + 30, y, s->w - ox - 60, netif.present ? b : "No supported network adapter was found. THE WRLD OS supports Intel PRO/1000, e1000e, I217-I219 and I210/I211/I350, Realtek RTL8111/8168/8125 and RTL8139, AMD PCnet, VMware VMXNET3 and VirtIO adapters. Wireless adapters are not supported.", W7_TEXT, 2);
}
static void page_mouse(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Mouse Properties", NULL);
    int y = oy + 70;
    draw_group(s, R(ox + 20, y, 460, 110), "Double-click speed");
    draw_text(s, FONT_UI, ox + 36, y + 30, "Slow", T.text); draw_text(s, FONT_UI, ox + 300, y + 30, "Fast", T.text);
    int v = 900 - c->dbl;
    if (ui_slider(w, R(ox + 70, y + 26, 220, 24), &v, 100, 750)) { c->dbl = 900 - v; wm_dblclick_ms = c->dbl; }
    rect_t tr = R(ox + 380, y + 22, 48, 48);
    draw_icon(s, c->dbl_open ? ICON_FOLDER_OPEN : ICON_FOLDER, 48, tr.x, tr.y);
    if (ui_hover(w, tr) && w->ui.dblclick) c->dbl_open = !c->dbl_open;
    draw_text(s, FONT_SMALL, ox + 36, y + 70, "Double-click the folder to test the setting.", W7_TEXT);
    y += 126;
    draw_group(s, R(ox + 20, y, 460, 80), "Pointer speed");
    draw_text(s, FONT_UI, ox + 36, y + 30, "Slow", T.text); draw_text(s, FONT_UI, ox + 300, y + 30, "Fast", T.text);
    if (ui_slider(w, R(ox + 70, y + 26, 220, 24), &c->speed, 1, 10)) mouse_speed = c->speed;
    y += 96;
    if (ui_checkbox(w, ox + 24, y, "Switch primary and secondary buttons", &c->swap)) mouse_swap_buttons = c->swap;
}
static void page_keyboard(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Keyboard Properties", "Click in the box and type to test the PS/2 keyboard driver.");
    draw_text(s, FONT_UI, ox + 20, oy + 80, "Test area:", T.text);
    ui_textbox(w, R(ox + 100, oy + 76, 300, 22), c->keytest, sizeof c->keytest);
    draw_text(s, FONT_UI_BOLD, ox + 20, oy + 120, "Keyboard shortcuts", T.text);
    const char *k[] = { "Win or Ctrl+Esc - open the Start menu", "Alt+Tab - switch windows", "Alt+F4 - close the active window", "Ctrl+Alt+Del / Ctrl+Shift+Esc - Task Manager", "F2 - rename, F5 - refresh, Delete - recycle", "Ctrl+C in Command Prompt - interrupt" };
    for (int i = 0; i < 6; i++) draw_text(s, FONT_UI, ox + 30, oy + 142 + i * 18, k[i], W7_TEXT);
}
static void page_sound(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Sound", "Change the volume, and choose the sounds " OS_NAME " plays when it starts and shuts down.");
    int y = oy + 64; char b[160];
    draw_icon(s, ICON_CP_SOUND, 48, ox + 30, y);
    draw_text(s, FONT_UI, ox + 96, y + 2, "Playback devices:", T.gray_text);
    if (ui_button(w, R(s->w - 118, y, 88, 24), "Scan again")) { audio_rescan(); devmgr_scan(); w->dirty = true; }
    int nd = audio_present() ? audio_device_count() : 0;
    if (nd > 8) nd = 8;
    const char *items[8];
    for (int i = 0; i < nd; i++) items[i] = audio_device_entry(i);
    int ih = font_height(FONT_UI) + 4, scroll = 0, sel = audio_device_active();
    rect_t lr = R(ox + 96, y + 28, s->w - ox - 130, nd * ih + 4);
    if (nd) ui_list(w, lr, items, nd, &sel, &scroll);
    if (nd && sel != audio_device_active()) audio_device_select(sel);
    int cy = lr.y + lr.h + 4;
    const char *ctl = audio_controller_name();
    if (ctl[0]) {
        draw_text(s, FONT_UI, ox + 96, cy, "Controller:", T.gray_text);
        draw_text(s, FONT_UI, ox + 210, cy, ctl, T.gray_text);
        cy += 22;
    }
    if (!audio_present()) {
        draw_text_wrapped(s, FONT_UI, ox + 96, y + 30, s->w - ox - 120, audio_summary(), W7_TEXT, 2);
        draw_text_wrapped(s, FONT_SMALL, ox + 96, y + 60, s->w - ox - 120, "Supported: Intel High Definition Audio, AC'97, Ensoniq AudioPCI, Sound Blaster 16 and USB audio class 1/2. "
            "Use Scan again after connecting a device, or see Device Manager for the hardware IDs.", W7_TEXT, 2);
        y += 110;
    } else {
        y = cy + 6;
        draw_text(s, FONT_UI, ox + 96, y + 4, "Volume:", T.gray_text);
        int vol = audio_volume;
        if (ui_slider(w, R(ox + 210, y, 220, 24), &vol, 0, 100)) audio_volume = vol;
        snprintf(b, sizeof b, "%d%%", audio_volume); draw_text(s, FONT_UI, ox + 440, y + 4, b, T.text);
        y += 30;
        ui_checkbox(w, ox + 210, y, "Mute", &audio_muted);
        y += 40;
        draw_text(s, FONT_UI_BOLD, ox + 20, y, "System sounds", T.link_head); hline(s, ox + 125, y + 8, s->w - ox - 155, T.face_dark); y += 26;
        ui_checkbox(w, ox + 40, y, "Play the startup and shutdown sounds", &system_sounds); y += 30;
        static const struct { const char *label, *file; } snd[] = { { "Startup", SOUND_STARTUP }, { "Shutdown", SOUND_SHUTDOWN } };
        for (int i = 0; i < 2; i++) {
            draw_icon(s, ICON_CP_SOUND, 16, ox + 40, y + 4);
            draw_text(s, FONT_UI, ox + 62, y + 5, snd[i].label, T.text);
            draw_text_ellipsis(s, FONT_UI, ox + 140, y + 5, s->w - ox - 260, path_basename(snd[i].file), W7_TEXT);
            if (ui_button(w, R(s->w - 110, y, 80, 24), "Test")) { char err[64]; audio_stop_all(); if (!audio_play_file(snd[i].file, err, sizeof err)) msgbox("Sound", err, MBI_ERROR, MBX_OK, NULL, NULL); }
            y += 30;
        }
        if (ui_button(w, R(s->w - 110, y, 80, 24), "Stop")) audio_stop_all();
        y += 40;
    }
    y += 10;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "Detected audio hardware", T.link_head); hline(s, ox + 190, y + 8, s->w - ox - 220, T.face_dark); y += 22;
    if (devmgr_count() == 0) devmgr_scan();
    int dn = 0;
    for (int i = 0; i < devmgr_count() && dn < 8; i++) {
        device_t *d = devmgr_get(i);
        if (!d || d->cls != DC_SOUND) continue;
        draw_text_ellipsis(s, FONT_UI, ox + 30, y, s->w - ox - 260, d->name, T.text);
        draw_text(s, FONT_SMALL, s->w - 210, y + 2, dev_status_text(d), d->status == DS_WORKING || d->status == DS_BUILTIN ? W7_TEXT : (dark_mode ? HEX(0xFFC080) : HEX(0x9C4A00)));
        y += 18; dn++;
    }
    if (!dn) draw_text(s, FONT_UI, ox + 30, y, "No audio-class hardware found.", T.gray_text);
    else y += 4;
    draw_text(s, FONT_UI_BOLD, ox + 20, y, "PC speaker", T.link_head); hline(s, ox + 105, y + 8, s->w - ox - 135, T.face_dark); y += 26;
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
    gradient_v(s, ox + 20, y, s->w - ox - 40, 22, T.window_bg, T.face); draw_rect(s, ox + 20, y, s->w - ox - 40, 22, T.face_dark);
    draw_text(s, FONT_UI, ox + 30, y + 4, "Name", T.text); draw_text(s, FONT_UI, ox + 320, y + 4, "Publisher", T.text); draw_text(s, FONT_UI, ox + 480, y + 4, "Size", T.text);
    y += 24;
    for (int i = 0; i < n && y < s->h - 60; i++) {
        rect_t r = R(ox + 20, y, s->w - ox - 40, 22);
        if (ui_click_area(w, r)) c->prog_sel = i;
        if (c->prog_sel == i) fill_rect(s, r.x, r.y, r.w, r.h, mix(T.window_bg, T.select, dark_mode ? 100 : 40));
        else if (ui_hover(w, r)) fill_rect(s, r.x, r.y, r.w, r.h, T.face_light);
        draw_icon(s, icons[i], 16, ox + 26, y + 3);
        draw_text_ellipsis(s, FONT_UI, ox + 48, y + 4, 260, names[i], T.text);
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
        { ".exe", ARCH_BITS == 64 ? "Win32 application (PE32+, x64)" : "Win32 application (PE32, i386)", "WRLD Win32 subsystem", ICON_EXE },
        { ".sh", "Shell script", "WRLD shell (sh)", ICON_SCRIPT },
        { ".dmg", "Apple disk image (UDIF)", "Disk Image Mounter", ICON_DMG },
        { ".app", "macOS application bundle", "Bundle launcher (script executables)", ICON_APP_BUNDLE },
        { ".txt / .log / .ini / .c", "Text document", "Notepad", ICON_TEXT_FILE },
        { ".bmp", "Bitmap image", "Image Viewer / Paint", ICON_IMAGE_FILE },
        { ".mp3 / .wav", "MP3 audio / Wave sound", "THE WRLD Media Player", ICON_MUSIC_FILE },
        { ".lnk", "Shortcut", "Shell", ICON_EXE },
        { "Mach-O / ELF", "Native macOS / Linux binaries", "Identified, not executed", ICON_SYS_FILE },
    };
    int y = oy + 64;
    for (unsigned i = 0; i < ARRAY_LEN(assoc); i++) {
        if (i % 2) fill_rect(s, ox + 20, y - 2, s->w - ox - 40, 34, T.face);
        draw_icon(s, assoc[i].icon, 32, ox + 26, y);
        draw_text(s, FONT_UI_BOLD, ox + 68, y + 2, assoc[i].ext, T.text);
        draw_text(s, FONT_UI, ox + 68, y + 17, assoc[i].desc, W7_TEXT);
        draw_text(s, FONT_UI, ox + 380, y + 9, assoc[i].prog, T.text);
        y += 36;
    }
}
static void page_users(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Make changes to your user account", NULL);
    draw_text(s, FONT_UI_BOLD, ox + 28, oy + 214, "Picture", T.text);
    int per = 12, pitch = 42, sz = 36;
    for (int i = 0; i < user_picture_count; i++) {
        int col = i % per, row = i / per;
        rect_t r = R(ox + 28 + col * pitch, oy + 236 + row * pitch, sz + 4, sz + 4);
        if (ui_click_area(w, r)) user_picture = i;
        fill_round_rect(s, r.x, r.y, r.w, r.h, 4, user_picture == i ? T.select : T.face_dark);
        draw_user_picture(s, i, sz, r.x + 2, r.y + 2);
    }
    /* the summary is drawn after the grid so a new pick shows in this paint */
    fill_round_rect(s, ox + 28, oy + 68, 56, 56, 5, mix(T.face, T.select, 70)); draw_user_picture(s, user_picture, 48, ox + 32, oy + 72);
    draw_text(s, FONT_LARGE, ox + 100, oy + 72, user_name, T.text);
    draw_text(s, FONT_UI, ox + 100, oy + 98, "Administrator", W7_TEXT);
    draw_text(s, FONT_UI_BOLD, ox + 28, oy + 150, "Account name", T.text);
    bool enter = ui_textbox(w, R(ox + 28, oy + 170, 220, 22), c->uname, sizeof c->uname);
    if ((ui_button(w, R(ox + 258, oy + 169, 100, 24), "Change name") || enter) && c->uname[0]) { strlcpy(user_name, c->uname, sizeof user_name); wm_need_redraw = true; }
}
static void page_fonts(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Preview fonts", "Text is shaped by the font's own OpenType tables (Liberation family, SIL Open Font License; Noto Naskh Arabic for Arabic).");
    static const struct { int f; const char *n; } fl[] = { { FONT_UI, "Sans 12 (UI)" }, { FONT_UI_BOLD, "Sans Bold 12" }, { FONT_TITLE, "Sans Bold 13 (title bars)" }, { FONT_LARGE, "Sans 18" }, { FONT_XL, "Sans 24" }, { FONT_MONO, "Mono 13 (console)" }, { FONT_START, "Sans Bold Italic 17" } };
    int y = oy + 96;
    for (unsigned i = 0; i < ARRAY_LEN(fl); i++) { draw_text(s, FONT_SMALL, ox + 24, y, fl[i].n, W7_TEXT); draw_text(s, fl[i].f, ox + 200, y - 2, "The quick brown fox jumps over the lazy dog", T.text); y += font_height(fl[i].f) + 12; }
    y += 4;
    draw_text(s, FONT_SMALL, ox + 24, y, "Arabic (right to left)", W7_TEXT);
    draw_text(s, FONT_LARGE, ox + 200, y - 2, "العربية: مرحبًا بالعالم", T.text);
    y += font_height(FONT_LARGE) + 12;
    draw_text(s, FONT_SMALL, ox + 24, y, "Hebrew (right to left)", W7_TEXT);
    draw_text(s, FONT_LARGE, ox + 200, y - 2, "שלום עולם", T.text);
    y += font_height(FONT_LARGE) + 12;
    draw_text(s, FONT_SMALL, ox + 24, y, "Bidirectional", W7_TEXT);
    draw_text(s, FONT_UI, ox + 200, y - 1, "WRLD 7 — العربية 123 abc", T.text);
}
static void page_ease(window_t *w, cp_t *c, int ox, int oy) {
    canvas_t *s = w->surf;
    heading(s, ox + 20, oy + 16, "Make your computer easier to use", NULL);
    int y = oy + 70;
    draw_icon(s, ICON_CP_EASE, 48, ox + 24, y);
    draw_text(s, FONT_UI_BOLD, ox + 84, y + 4, "Quick access to common tools", T.text);
    if (ui_button(w, R(ox + 84, y + 26, 190, 26), "Use high-contrast Classic style")) { theme_apply(THEME_CLASSIC, 0); wm_redraw_all(); }
    if (ui_button(w, R(ox + 284, y + 26, 160, 26), "Restore Luna (Blue)")) { theme_apply(THEME_LUNA_BLUE, 0); wm_redraw_all(); }
    y += 80;
    if (ui_link(w, ox + 84, y, FONT_UI, "Make the mouse easier to use (slower pointer, slower double-click)", W7_LINK)) { mouse_speed = 3; wm_dblclick_ms = 700; }
    y += 24;
    if (ui_link(w, ox + 84, y, FONT_UI, "Turn off unnecessary animations", W7_LINK)) { wm_animations = false; wm_effects_changed(); settings_save(); }
    if (ui_link(w, ox + 84, y + 24, FONT_UI, "Choose individual visual effects...", W7_LINK)) cp_go(c, "effects", true);
    y += 24;
    y += 24;
    if (ui_link(w, ox + 84, y, FONT_UI, "Show seconds and 24-hour time in the taskbar", W7_LINK)) { show_seconds = true; clock_24h = true; wm_need_redraw = true; }
}

static void cp_refresh_theme(void *arg) { (void)arg; wm_redraw_all(); }
static void cp_paint(window_t *w) {
    cp_t *c = w->data; canvas_t *s = w->surf;
    gfx_clear(s, T.window_bg);
    draw_chrome(w, c);
    const applet_t *a = find_applet(c->page);
    int cat = !strncmp(c->page, "cat:", 4) ? atoi(c->page + 4) : a ? a->cat : -1;
    int ox = 0, oy = TOP_H;
    if (c->search[0] && !a) { home_icons(w, c, 0, oy, true); return; }
    if (!c->page[0]) {
        draw_text(s, FONT_LARGE, 24, oy + 18, c->view ? "Adjust your computer's settings" : "Adjust your computer's settings", W7_HEAD);
        static const char *views[] = { "Category", "Large icons", "Small icons" };
        draw_text(s, FONT_UI, s->w - 210, oy + 22, "View by:", T.text);
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
    else if (!strcmp(a->id, "advanced")) page_advanced(w, c, ox, oy);
    else if (!strcmp(a->id, "display")) page_display(w, c, ox, oy);
    else if (!strcmp(a->id, "effects")) page_effects(w, c, ox, oy);
    else if (!strcmp(a->id, "taskbar")) page_taskbar(w, c, ox, oy);
    else if (!strcmp(a->id, "datetime")) page_datetime(w, c, ox, oy);
    else if (!strcmp(a->id, "system")) page_system(w, c, ox, oy);
    else if (!strcmp(a->id, "admin")) page_admin(w, c, ox, oy);
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
static void cp_tick(window_t *w) {
    cp_t *c = w->data;
    if (!strcmp(c->page, "datetime") || !strcmp(c->page, "system") || !strcmp(c->page, "network")) w->dirty = true;
    if (!strcmp(c->page, "devices") && ++c->dev_tick % 2 == 0) {     /* a device plugged in or removed shows up by itself */
        int before = devmgr_count(); char a[160], b[160]; devmgr_summary(a, sizeof a);
        devmgr_scan(); devmgr_summary(b, sizeof b);
        if (before != devmgr_count() || strcmp(a, b)) w->dirty = true;
    }
}
static void cp_mouse(window_t *w, int type, int x, int y, int b) { cp_t *c = w->data; if (type == ME_WHEEL && !strcmp(c->page, "devices")) { c->scroll = CLAMP(c->scroll + b * 40, 0, c->scroll_max); w->dirty = true; } }
static void cp_menu(window_t *w, int id) {
    cp_t *c = w->data;
    if (id <= -1000 && !strcmp(c->page, "effects")) { wm_effects_changed(); settings_save(); }
    w->dirty = true;
}
static void cp_destroy(window_t *w) { kfree(w->data); }
void control_open(const char *page) {
    window_t *w = wm_create("Control Panel", -1, -1, MIN(840, SCREEN_W - 40), MIN(660, SCREEN_H - 70), WF_RESIZABLE);
    cp_t *c = kzalloc(sizeof *c);
    c->win = w; c->prog_sel = -1;
    w->data = c; w->paint = cp_paint; w->destroy = cp_destroy; w->mouse = cp_mouse; w->menu_cmd = cp_menu;
    w->icon = ICON_CONTROL_PANEL; w->min_w = 640; w->min_h = 420;
    wm_set_timer(w, 1000, cp_tick);
    cp_go(c, "", true);
    if (page && *page) cp_go(c, page, true);
}
