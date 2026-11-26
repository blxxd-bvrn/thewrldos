/* The WRLD Search: one place to find files, folders, programs and settings.
 * The start page is a sky with the search box in the middle, nine areas of the
 * computer with their most-used places, Recommendations and Your Recent Activities
 * on the right and Top Things To Do along the bottom. A search lists the matching
 * programs, Control Panel settings, and files and folders on every drive, and
 * offers the web; a click opens what was found.
 * Opened from the Start menu, the classic Start menu's Search, the sidebar's
 * Search tile (Enter), the Run box ("search") and the Command Prompt. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"
#include "raster.h"

#define TOP_H 30
#define RIGHT_W 214
#define MAXRES 400

enum { RK_PROGRAM, RK_SETTING, RK_FILE, RK_WEB, RK_N };
static const char *kind_title[RK_N] = { "Programs", "Control Panel", "Files and Folders", "The Internet" };
typedef struct { int kind, icon; char label[96], sub[160], action[192]; } res_t;
typedef struct {
    char q[128], shown[128];                 /* what is typed; what the results are for */
    bool results; int scroll, content_h;
    res_t *res; int nres, count[RK_N]; bool truncated;
    int box_id; bool want_focus;
    char back_q[128];
} srch_t;

/* ---------------- what can be found ---------------- */
/* Control Panel places, with the words people use for them */
static const struct { const char *name, *page, *words; int icon; } settings_list[] = {
    { "Personalization", "personalization", "theme style colour color look appearance hue dark mode", ICON_CP_APPEARANCE },
    { "Desktop background", "personalization", "wallpaper picture background desktop", ICON_CP_APPEARANCE },
    { "Display", "display", "screen resolution monitor size", ICON_CP_DISPLAY },
    { "Visual Effects", "effects", "animations 3d acceleration transparency", ICON_CP_DISPLAY },
    { "Advanced Appearance", "advanced", "colours colors scrollbars buttons cursors fonts", ICON_CP_APPEARANCE },
    { "Network and Sharing Center", "network", "internet ip address wifi ethernet dns proxy connection", ICON_CP_NETWORK },
    { "Sound", "sound", "audio volume speaker music", ICON_CP_SOUND },
    { "Mouse", "mouse", "pointer double click cursor", ICON_CP_MOUSE },
    { "Keyboard", "keyboard", "keys layout typing repeat", ICON_CP_KEYBOARD },
    { "Date and Time", "datetime", "clock time zone calendar", ICON_CP_DATETIME },
    { "User Accounts", "users", "account name picture password login", ICON_CP_USERS },
    { "Programs and Features", "programs", "uninstall remove install packages apps", ICON_CP_PROGRAMS },
    { "Default Programs", "defaults", "file types associations open with", ICON_EXE },
    { "System", "system", "computer information memory processor version about", ICON_CP_SYSTEM },
    { "Device Manager", "devices", "hardware drivers devices usb", ICON_CP_DEVICES },
    { "Administrative Tools", "admin", "storage disks shell restart", ICON_TASKMGR },
    { "Taskbar and Start Menu", "taskbar", "start menu taskbar quick launch sidebar", ICON_CP_TASKBAR },
    { "Fonts", "fonts", "typefaces text", ICON_CP_FONTS },
    { "Ease of Access Center", "ease", "accessibility magnify contrast", ICON_CP_EASE },
};
/* a query matches when every word of it is in the text (case does not matter) */
static bool has_ci(const char *hay, const char *needle, int nl) {
    for (const char *h = hay; *h; h++) {
        int k = 0;
        while (k < nl && h[k] && tolower((unsigned char)h[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}
static bool matches(const char *q, const char *a, const char *b) {
    const char *p = q;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0; while (p[n] && p[n] != ' ') n++;
        if (!has_ci(a, p, n) && !(b && has_ci(b, p, n))) return false;
        p += n;
    }
    return true;
}
static void add(srch_t *s, int kind, int icon, const char *label, const char *sub, const char *action) {
    if (s->nres >= MAXRES) { s->truncated = true; return; }
    res_t *r = &s->res[s->nres++];
    r->kind = kind; r->icon = icon;
    strlcpy(r->label, label, sizeof r->label); strlcpy(r->sub, sub ? sub : "", sizeof r->sub); strlcpy(r->action, action, sizeof r->action);
    s->count[kind]++;
}
static int scan_budget;
static void scan(srch_t *s, vnode_t *d, int depth) {
    if (!d || !d->dir || depth > 24) return;
    for (vnode_t *n = d->child; n && scan_budget > 0; n = n->next) {
        scan_budget--;
        if (n->name[0] == '.') continue;
        char full[192]; vfs_path(n, full, sizeof full);
        if (matches(s->shown, n->name, NULL)) {
            char where[192]; path_dirname(full, where, sizeof where);
            char act[200]; snprintf(act, sizeof act, "open:%s", full);
            add(s, RK_FILE, file_icon(n), n->name, where, act);
            if (s->nres >= MAXRES) return;
        }
        if (n->dir && !(depth == 1 && !strcasecmp(n->name, "Linux") && strlen(s->shown) < 3)) scan(s, n, depth + 1);
    }
}
static void run_search(srch_t *s) {
    strlcpy(s->shown, s->q, sizeof s->shown);
    s->nres = 0; s->truncated = false; s->scroll = 0;
    memset(s->count, 0, sizeof s->count);
    if (!s->res) s->res = kzalloc(sizeof(res_t) * MAXRES);
    if (!s->res) return;
    s->results = true;
    const char *q = s->shown;
    for (int i = 0; i < app_count; i++)
        if (matches(q, app_table[i].title, app_table[i].subtitle)) {
            char act[64]; snprintf(act, sizeof act, "app:%s", app_table[i].id);
            add(s, RK_PROGRAM, app_table[i].icon, app_table[i].title, app_table[i].subtitle, act);
        }
    for (unsigned i = 0; i < ARRAY_LEN(settings_list); i++)
        if (matches(q, settings_list[i].name, settings_list[i].words)) {
            char act[64]; snprintf(act, sizeof act, "cp:%s", settings_list[i].page);
            add(s, RK_SETTING, settings_list[i].icon, settings_list[i].name, "Control Panel", act);
        }
    /* files and folders: your documents first, then every drive */
    scan_budget = 60000;
    scan(s, vfs_resolve(USER_HOME, NULL), 1);
    for (char l = 'A'; l <= 'Z' && scan_budget > 0; l++) {
        vnode_t *root = vfs_root(l);
        if (!root) continue;
        for (vnode_t *n = root->child; n && scan_budget > 0; n = n->next) {
            if (l == 'C' && !strcasecmp(n->name, "Documents and Settings")) continue;   /* already searched */
            scan_budget--;
            char full[192]; vfs_path(n, full, sizeof full);
            if (matches(q, n->name, NULL)) { char act[200]; snprintf(act, sizeof act, "open:%s", full); add(s, RK_FILE, file_icon(n), n->name, full, act); }
            if (n->dir) scan(s, n, 2);
        }
    }
    if (scan_budget <= 0) s->truncated = true;
    char act[192]; snprintf(act, sizeof act, "web:%s", q);
    char lab[160]; snprintf(lab, sizeof lab, "Search the web for \"%s\"", q);
    add(s, RK_WEB, ICON_BROWSER, lab, "Opens The WRLD Browser", act);
    klog("search: \"%s\": %d programs, %d settings, %d files%s", q, s->count[RK_PROGRAM], s->count[RK_SETTING], s->count[RK_FILE], s->truncated ? " (stopped early)" : "");
}

/* ---------------- doing things ---------------- */
static void web_search(const char *q) {
    char u[700] = "https://www.google.com/search?q="; int o = strlen(u);
    for (const char *p = q; *p && o < (int)sizeof u - 4; p++) {
        if (isalnum((unsigned char)*p)) u[o++] = *p; else if (*p == ' ') u[o++] = '+';
        else o += snprintf(u + o, sizeof u - o, "%%%02X", (uint8_t)*p);
    }
    u[o] = 0;
    browser_open(u);
}
void recent_add(const char *path);
static void do_action(const char *a) {
    if (!strncmp(a, "app:", 4)) app_launch(a + 4, NULL);
    else if (!strncmp(a, "cp:", 3)) control_open(a + 3);
    else if (!strncmp(a, "open:", 5)) { if (!shell_open(a + 5, NULL)) msgbox("The WRLD Search", "That item is no longer there.", MBI_WARNING, MBX_OK, NULL, NULL); }
    else if (!strncmp(a, "dir:", 4)) { vfs_mkdir_p(a + 4); explorer_open(a + 4); }
    else if (!strncmp(a, "web:", 4)) web_search(a + 4);
    else if (!strncmp(a, "url:", 4)) browser_open(a + 4);
}

/* ---------------- the look ---------------- */
static const uint32_t C_TITLE = 0xFF0F3A78, C_LINK = 0xFF2A5DB0, C_TEXT = 0xFF203040, C_DIM = 0xFF5A6B80, C_ORANGE = 0xFFE07B39;
/* the sky: a blue gradient with soft clouds and light falling from the upper right */
/* the background picture (C:\WRLD\Search\Background.jpg), scaled to cover the page and kept at
 * that size until the window changes; the drawn sky below stands in if the file is missing */
#define SEARCH_BG "C:\\WRLD\\Search\\Background.jpg"
static canvas_t *bg_src, *bg_fit; static bool bg_tried;
static bool picture(canvas_t *c, rect_t r) {
    if (!bg_tried) {
        bg_tried = true;
        uint32_t len; uint8_t *d = vfs_read_file(SEARCH_BG, &len);
        char err[64];
        if (d) { bg_src = image_decode(d, len, err, sizeof err); kfree(d); }
    }
    if (!bg_src || r.w <= 0 || r.h <= 0) return false;
    if (!bg_fit || bg_fit->w != r.w || bg_fit->h != r.h) {
        if (bg_fit) canvas_free(bg_fit);
        bg_fit = canvas_new(r.w, r.h);
        if (!bg_fit) return false;
        /* cover: the larger of the two scales, centred, the overflow cut off */
        int64_t sw = bg_src->w, sh = bg_src->h;
        int w = r.w, h = (int)(sh * r.w / sw);
        if (h < r.h) { h = r.h; w = (int)(sw * r.h / sh); }
        blit_scaled_alpha(bg_fit, R((r.w - w) / 2, (r.h - h) / 2, w, h), bg_src, 255);
    }
    blit(c, r.x, r.y, bg_fit, 0, 0, r.w, r.h);
    return true;
}
static void sky(canvas_t *c, rect_t r) {
    if (picture(c, r)) return;
    gradient_v3(c, r.x, r.y, r.w, r.h, HEX(0xA9CEF2), HEX(0xD6EAFB), HEX(0xF1F8FF), r.h / 3);
    static const struct { int x, y, w, h, a; } clouds[] = {
        { 8, 4, 30, 10, 70 }, { 22, 2, 26, 8, 60 }, { 40, 10, 36, 14, 55 }, { 60, 3, 30, 12, 70 }, { 2, 28, 40, 12, 40 },
        { 70, 26, 34, 10, 45 }, { 30, 44, 44, 12, 35 }, { -6, 58, 34, 14, 40 }, { 66, 60, 40, 16, 35 },
    };
    for (unsigned i = 0; i < ARRAY_LEN(clouds); i++) {
        int x = r.x + clouds[i].x * r.w / 100, y = r.y + clouds[i].y * r.h / 100, w = clouds[i].w * r.w / 100, h = clouds[i].h * r.h / 100;
        fill_round_rect_blend(c, x, y, w, h, h / 2, WHITE, clouds[i].a);
        fill_round_rect_blend(c, x + w / 6, y - h / 4, w * 2 / 3, h * 3 / 4, h / 3, WHITE, clouds[i].a / 2);
    }
    raster_path_t *p = raster_path_new();
    if (p) {                                                     /* the rays */
        for (int k = 0; k < 3; k++) {
            int x0 = r.x + r.w * (70 + k * 9) / 100;
            raster_reset(p);
            raster_move_to(p, RASTER_FIX(x0), RASTER_FIX(r.y));
            raster_line_to(p, RASTER_FIX(x0 + r.w / 14), RASTER_FIX(r.y));
            raster_line_to(p, RASTER_FIX(x0 - r.w / 5), RASTER_FIX(r.y + r.h));
            raster_line_to(p, RASTER_FIX(x0 - r.w / 5 - r.w / 22), RASTER_FIX(r.y + r.h));
            raster_close(p);
            raster_fill(c, p, 0x30FFFFFF, false);
        }
        raster_path_free(p);
    }
}
/* a glassy panel with a title band */
static int panel(canvas_t *c, rect_t r, const char *title) {
    fill_round_rect_blend(c, r.x, r.y, r.w, r.h, 6, WHITE, 120);
    draw_round_rect(c, r.x, r.y, r.w, r.h, 6, 0xFFB9D3EE);
    if (!title) return r.y + 8;
    gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, 28, 5, HEX(0xEAF4FE), HEX(0xC9E0F7));
    hline(c, r.x + 6, r.y + 29, r.w - 12, 0xFFB9D3EE);
    draw_text(c, FONT_TITLE, r.x + 10, r.y + 7, title, C_TITLE);
    return r.y + 36;
}
/* a line of text where words between asterisks are bold: "Change *desktop wallpaper*" */
static bool rich_link(window_t *w, int x, int y, const char *t) {
    canvas_t *c = w->surf;
    int tw = 0; bool bold = false;
    for (const char *p = t; *p; ) {
        if (*p == '*') { bold = !bold; p++; continue; }
        int n = 0; while (p[n] && p[n] != '*') n++;
        char seg[96]; snprintf(seg, sizeof seg, "%.*s", n, p);
        tw += text_width(bold ? FONT_UI_BOLD : FONT_UI, seg);
        p += n;
    }
    rect_t r = R(x, y, tw, font_height(FONT_UI) + 2);
    bool hot = ui_hover(w, r);
    int cx = x; bold = false;
    for (const char *p = t; *p; ) {
        if (*p == '*') { bold = !bold; p++; continue; }
        int n = 0; while (p[n] && p[n] != '*') n++;
        char seg[96]; snprintf(seg, sizeof seg, "%.*s", n, p);
        int f = bold ? FONT_UI_BOLD : FONT_UI;
        draw_text(c, f, cx, y, seg, bold ? C_TITLE : C_LINK);
        cx += text_width(f, seg);
        p += n;
    }
    if (hot) hline(c, x, y + font_height(FONT_UI), tw, C_LINK);
    return ui_click_area(w, r);
}

/* ---------------- the start page ---------------- */
typedef struct { const char *title; int icon; const char *title_action; const char *links[3]; const char *actions[3]; } area_t;
static const area_t areas[] = {
    { "Documents & Pictures", ICON_MY_DOCUMENTS, "dir:" USER_DOCS, { "my documents", "my music", "my videos" },
      { "dir:" USER_DOCS, "dir:" USER_DOCS "\\My Music", "dir:" USER_DOCS "\\My Videos" } },
    { "Control Panel", ICON_CONTROL_PANEL, "app:control", { "personalization", "sound and mouse", "date and time" },
      { "cp:personalization", "cp:sound", "cp:datetime" } },
    { "Network & Internet", ICON_CP_NETWORK, "cp:network", { "network status", "firewall and web protection", "browse the web" },
      { "cp:network", "app:security", "app:xplorer" } },
    { "Programs", ICON_CP_PROGRAMS, "cp:programs", { "install or remove programs", "default programs", "command prompt" },
      { "cp:programs", "cp:defaults", "app:cmd" } },
    { "My Computer", ICON_COMPUTER, "open:::computer", { "drives and storage", "device manager", "system information" },
      { "open:::computer", "cp:devices", "cp:system" } },
    { "Music & Video", ICON_MEDIA_PLAYER, "app:wmplayer", { "play music", "my music", "sound settings" },
      { "app:wmplayer", "dir:" USER_DOCS "\\My Music", "cp:sound" } },
    { "Accessories", ICON_NOTEPAD, "app:notepad", { "write in wordpad", "draw in paint", "calculator" },
      { "app:wordpad", "app:paint", "app:calc" } },
    { "Games", ICON_GAME, "app:minesweeper", { "minesweeper", "snake", "pong" },
      { "app:minesweeper", "app:snake", "app:pong" } },
    { "Help", ICON_HELP, "app:help", { "help and support", "about the wrld os", "security center" },
      { "app:help", "app:about", "app:security" } },
};
static const struct { const char *text, *action; int icon; } recommendations[] = {
    { "Change the *desktop* picture", "cp:personalization", ICON_CP_APPEARANCE },
    { "Set an *account* password", "app:security", ICON_CP_USERS },
    { "Play *music* or a video", "app:wmplayer", ICON_MEDIA_PLAYER },
    { "Explore the *web*", "app:xplorer", ICON_BROWSER },
};
static const struct { const char *text, *action; } top_things[] = {
    { "Write a *document*", "app:wordpad" }, { "Open *My Documents*", "dir:" USER_DOCS }, { "Change *desktop wallpaper*", "cp:personalization" },
    { "Draw a *picture*", "app:paint" }, { "Listen to *music*", "app:wmplayer" }, { "Check *network* status", "cp:network" },
    { "Install a *program*", "cp:programs" }, { "Open the *Command Prompt*", "app:cmd" }, { "Set the *date and time*", "cp:datetime" },
    { "Change *sound* settings", "cp:sound" }, { "See what is *running*", "app:taskmgr" }, { "Review your *security*", "app:security" },
};
int recent_count(void);
const char *recent_at(int i);
static void start_page(window_t *w, srch_t *s, rect_t body, int top) {
    canvas_t *c = w->surf;
    int lw = body.w - RIGHT_W - 36, lx = body.x + 18;
    /* the nine areas, three by three */
    int colw = lw / 3, rowh = 74, y0 = top;
    for (unsigned i = 0; i < ARRAY_LEN(areas); i++) {
        const area_t *a = &areas[i];
        int x = lx + (i % 3) * colw, y = y0 + (i / 3) * rowh;
        fill_round_rect_blend(c, x, y + 2, 38, 38, 4, WHITE, 140);
        draw_round_rect(c, x, y + 2, 38, 38, 4, 0xFF9FC2E6);
        draw_icon(c, a->icon, 32, x + 3, y + 5);
        int tx = x + 46;
        if (ui_link(w, tx, y, FONT_UI_BOLD, a->title, C_TITLE)) do_action(a->title_action);
        for (int k = 0; k < 3; k++)
            if (ui_link(w, tx, y + 17 + k * 15, FONT_UI, a->links[k], C_LINK)) do_action(a->actions[k]);
    }
    /* Top Things To Do */
    int ty = y0 + 3 * rowh + 6;
    rect_t tp = R(lx - 6, ty, lw + 6, MAX(110, body.y + body.h - ty - 14));
    int py = panel(c, tp, "Top Things To Do");
    int tcol = (tp.w - 20) / 3;
    for (unsigned i = 0; i < ARRAY_LEN(top_things); i++) {
        int x = tp.x + 12 + (i % 3) * tcol, y = py + (i / 3) * 19;
        if (y + 16 > tp.y + tp.h) break;
        fill_circle(c, x + 3, y + 7, 2, C_LINK);
        if (rich_link(w, x + 12, y, top_things[i].text)) do_action(top_things[i].action);
    }
    /* Recommendations */
    int rx = body.x + body.w - RIGHT_W - 14;
    rect_t rp = R(rx, top - 6, RIGHT_W, 36 + 4 * 24 + 6);
    py = panel(c, rp, "Recommendations");
    for (unsigned i = 0; i < ARRAY_LEN(recommendations); i++) {
        draw_icon(c, recommendations[i].icon, 16, rp.x + 10, py + i * 24);
        if (rich_link(w, rp.x + 32, py + i * 24 + 1, recommendations[i].text)) do_action(recommendations[i].action);
    }
    /* Your Recent Activities */
    rect_t ap = R(rx, rp.y + rp.h + 14, RIGHT_W, body.y + body.h - (rp.y + rp.h + 14) - 14);
    py = panel(c, ap, "Your Recent Activities");
    int n = recent_count();
    if (!n) draw_text(c, FONT_UI, ap.x + 12, py, "Files you open appear here.", C_DIM);
    for (int i = 0; i < n && py + 30 < ap.y + ap.h; i++) {
        const char *p = recent_at(i);
        vnode_t *v = vfs_resolve(p, NULL);
        const char *name = path_basename(p);
        if (v) draw_icon(c, file_icon(v), 16, ap.x + 10, py + 2);
        rect_t r = R(ap.x + 32, py, ap.w - 42, 30);
        bool hot = ui_hover(w, r);
        draw_text_ellipsis(c, FONT_UI_BOLD, ap.x + 32, py, ap.w - 42, name, hot ? C_LINK : C_TITLE);
        draw_text_ellipsis(c, FONT_SMALL, ap.x + 32, py + 15, ap.w - 42, v ? file_type_name(v) : "No longer there", C_DIM);
        if (ui_click_area(w, r)) { char a[200]; snprintf(a, sizeof a, "open:%s", p); do_action(a); }
        py += 34;
    }
    (void)s;
}

/* ---------------- the results ---------------- */
static void results_page(window_t *w, srch_t *s, rect_t body, int top) {
    canvas_t *c = w->surf;
    rect_t area = R(body.x + 18, top, body.w - 36, body.y + body.h - top - 12);
    fill_round_rect_blend(c, area.x, area.y, area.w, area.h, 6, WHITE, 165);
    draw_round_rect(c, area.x, area.y, area.w, area.h, 6, 0xFFB9D3EE);
    rect_t old = canvas_push_clip(c, R(area.x + 1, area.y + 1, area.w - 2, area.h - 2));
    if (ui_hover(w, area) && w->ui.wheel) { s->scroll += w->ui.wheel * 40; w->ui.wheel = 0; }
    s->scroll = CLAMP(s->scroll, 0, MAX(0, s->content_h - area.h + 20));
    int y = area.y + 12 - s->scroll;
    int total = s->count[RK_PROGRAM] + s->count[RK_SETTING] + s->count[RK_FILE];
    char head[200];
    snprintf(head, sizeof head, total ? "%d result%s for \"%s\"%s" : "Nothing on this computer matches \"%s\"", total, total == 1 ? "" : "s", s->shown, s->truncated ? " (the search stopped early: try more words)" : "");
    if (!total) snprintf(head, sizeof head, "Nothing on this computer matches \"%s\"", s->shown);
    draw_text(c, FONT_LARGE, area.x + 14, y, head, C_TITLE);
    y += 34;
    for (int k = 0; k < RK_N; k++) {
        if (!s->count[k]) continue;
        char t[64]; snprintf(t, sizeof t, "%s (%d)", kind_title[k], s->count[k]);
        if (k == RK_WEB) snprintf(t, sizeof t, "%s", kind_title[k]);
        draw_text(c, FONT_UI_BOLD, area.x + 14, y, t, C_TITLE);
        hline(c, area.x + 14 + text_width(FONT_UI_BOLD, t) + 8, y + 8, area.w - 40 - text_width(FONT_UI_BOLD, t), 0xFF9FC2E6);
        y += 22;
        for (int i = 0; i < s->nres; i++) {
            res_t *r = &s->res[i];
            if (r->kind != k) continue;
            rect_t row = R(area.x + 10, y, area.w - 20, 22);
            if (y + 22 > area.y && y < area.y + area.h) {
                bool hot = ui_hover(w, row) && ui_hover(w, area);
                if (hot) { fill_round_rect(c, row.x, row.y, row.w, row.h, 3, 0xFFDCEBFB); draw_round_rect(c, row.x, row.y, row.w, row.h, 3, 0xFF9DBFE6); }
                draw_icon(c, r->icon, 16, row.x + 6, row.y + 3);
                int lw = MIN(text_width(FONT_UI, r->label), row.w / 2);
                draw_text_ellipsis(c, FONT_UI, row.x + 28, row.y + 4, row.w / 2, r->label, C_TEXT);
                draw_text_ellipsis(c, FONT_SMALL, row.x + 40 + lw, row.y + 6, row.w - 52 - lw, r->sub, C_DIM);
                if (hot && ui_click_area(w, row)) do_action(r->action);
            }
            y += 23;
        }
        y += 10;
    }
    s->content_h = y + s->scroll - area.y;
    c->clip = old;
}

/* ---------------- the window ---------------- */
static void menu_cb(void *ctx, int id);
static window_t *menu_win;
static void open_menu(window_t *w, int x, const menu_item_t *items, int n) {
    rect_t cr = wm_client_rect(w);
    menu_win = w;
    popup_menu(cr.x + x, cr.y + TOP_H - 2, items, n, menu_cb, w);
}
enum { M_NEW = 1, M_CLOSE, M_CLEAR, M_HOME, M_RESULTS, M_EXPLORER, M_HELP, M_ABOUT };
static void menu_cb(void *ctx, int id) {
    window_t *w = ctx;
    if (w != menu_win || !wm_alive(w)) return;
    srch_t *s = w->data;
    switch (id) {
    case M_NEW: s->q[0] = 0; s->results = false; s->want_focus = true; break;
    case M_CLOSE: wm_close(w); return;
    case M_CLEAR: s->q[0] = 0; s->want_focus = true; break;
    case M_HOME: s->results = false; break;
    case M_RESULTS: if (s->nres) s->results = true; break;
    case M_EXPLORER: explorer_open("::computer"); break;
    case M_HELP: app_launch("help", NULL); break;
    case M_ABOUT: msgbox("About The WRLD Search", "The WRLD Search finds programs, Control Panel settings, and files and folders on every drive.\n\nType words that are in a name: every word must match.", MBI_INFO, MBX_OK, NULL, NULL); break;
    }
    w->dirty = true;
}
static void top_bar(window_t *w, srch_t *s) {
    canvas_t *c = w->surf;
    gradient_v3(c, 0, 0, c->w, TOP_H, HEX(0x2B6FD0), HEX(0x14489C), HEX(0x0E3A84), TOP_H / 2);
    blend_rect(c, 0, 0, c->w, TOP_H / 2, WHITE, 40);
    hline(c, 0, TOP_H - 1, c->w, HEX(0x0A2A5E));
    static const menu_item_t mfile[] = { { "New Search", M_NEW, 0 }, { "-", 0, 0 }, { "Close", M_CLOSE, 0 } };
    static const menu_item_t medit[] = { { "Clear the Search Box", M_CLEAR, 0 } };
    static menu_item_t mview[] = { { "Start Page", M_HOME, 0 }, { "Results", M_RESULTS, 0 }, { "-", 0, 0 }, { "My Computer", M_EXPLORER, 0 } };
    static const menu_item_t mhelp[] = { { "Help and Support", M_HELP, 0 }, { "About The WRLD Search", M_ABOUT, 0 } };
    mview[1].flags = s->nres ? 0 : MI_DISABLED;
    static const char *names[] = { "File", "Edit", "View", "Help" };
    int x = 8;
    for (int i = 0; i < 4; i++) {
        int tw = text_width(FONT_UI, names[i]);
        rect_t r = R(x - 6, 4, tw + 12, TOP_H - 8);
        if (ui_hover(w, r)) fill_round_rect_blend(c, r.x, r.y, r.w, r.h, 3, WHITE, 60);
        draw_text(c, FONT_UI, x, (TOP_H - font_height(FONT_UI)) / 2, names[i], WHITE);
        if (ui_click_area(w, r)) {
            if (i == 0) open_menu(w, r.x, mfile, ARRAY_LEN(mfile));
            else if (i == 1) open_menu(w, r.x, medit, ARRAY_LEN(medit));
            else if (i == 2) open_menu(w, r.x, mview, ARRAY_LEN(mview));
            else open_menu(w, r.x, mhelp, ARRAY_LEN(mhelp));
        }
        x += tw + 22;
    }
    /* back to the start page, and where you are */
    int bx = x + 6, cy = TOP_H / 2;
    rect_t br = R(bx - 9, cy - 9, 18, 18);
    bool can_back = s->results, hot = can_back && ui_hover(w, br);
    fill_circle(c, bx, cy, 9, can_back ? (hot ? HEX(0x6FB4F6) : HEX(0x3D8CE6)) : HEX(0x5D7FAF));
    draw_circle(c, bx, cy, 9, HEX(0xBFD9F7));
    draw_thick_line(c, bx + 4, cy, bx - 3, cy, 2, WHITE);
    draw_thick_line(c, bx - 3, cy, bx, cy - 3, 2, WHITE); draw_thick_line(c, bx - 3, cy, bx, cy + 3, 2, WHITE);
    if (can_back && ui_click_area(w, br)) { s->results = false; w->dirty = true; }
    rect_t ar = R(bx + 16, 6, c->w - bx - 28, TOP_H - 12);
    gradient_round_rect(c, ar.x, ar.y, ar.w, ar.h, ar.h / 2, HEX(0xFDFEFF), HEX(0xD9E8F8));
    char crumb[200];
    if (s->results) snprintf(crumb, sizeof crumb, "The WRLD Search  >  Results for \"%s\"", s->shown);
    else strlcpy(crumb, "The WRLD Search", sizeof crumb);
    draw_text_ellipsis(c, FONT_UI, ar.x + 12, ar.y + (ar.h - font_height(FONT_UI)) / 2, ar.w - 24, crumb, HEX(0x24406A));
}
static void search_paint(window_t *w) {
    srch_t *s = w->data;
    canvas_t *c = w->surf;
    rect_t body = R(0, TOP_H, c->w, c->h - TOP_H);
    sky(c, body);
    top_bar(w, s);
    /* the title: THE WRLD above Search */
    int title_y = body.y + (s->results ? 8 : 22);
    const char *brand = "THE WRLD";
    int sw = text_width(FONT_DISPLAY, "Search");
    int tx = (c->w - sw) / 2;
    if (!s->results) {
        draw_text(c, FONT_SMALL, tx + 4, title_y, brand, HEX(0x40618E));
        draw_text(c, FONT_DISPLAY, tx + 1, title_y + 9, "Search", HEX(0xFFFFFF));
        draw_text(c, FONT_DISPLAY, tx, title_y + 8, "Search", HEX(0x1B4E8E));
        const char *sub = "Find your files, folders, programs and settings, or pick up where you left off.";
        draw_text(c, FONT_UI, (c->w - text_width(FONT_UI, sub)) / 2, title_y + 60, sub, HEX(0x2D4D78));
    }
    /* the search box: orange edge, Go at its end */
    int boxw = MIN(420, c->w - 80), boxy = s->results ? title_y + 2 : title_y + 82;
    rect_t box = R((c->w - boxw) / 2, boxy, boxw, 26);
    fill_rect(c, box.x, box.y, box.w, box.h, WHITE);
    draw_rect(c, box.x, box.y, box.w, box.h, C_ORANGE);
    draw_rect(c, box.x - 1, box.y - 1, box.w + 2, box.h + 2, 0x60E07B39);
    rect_t go = R(box.x + box.w - 36, box.y + 2, 34, box.h - 4);
    s->box_id = w->ui.next_id + 1;
    if (s->want_focus) { w->ui.focus = s->box_id; s->want_focus = false; }
    bool focused = false;
    bool enter = ui_textbox_ex(w, R(box.x + 2, box.y + 2, box.w - 42, box.h - 4), s->q, sizeof s->q, true, C_TEXT, &focused);
    if (!s->q[0] && !focused) draw_text(c, FONT_UI, box.x + 8, box.y + (box.h - font_height(FONT_UI)) / 2, "Type the name of a file, folder, program or setting", HEX(0x8A97A8));
    bool gohot = ui_hover(w, go);
    if (gohot) fill_round_rect(c, go.x, go.y, go.w, go.h, 3, HEX(0xFCE3D2));
    draw_text_centered(c, FONT_UI_BOLD, go, "Go", HEX(0xB85A1C));
    if ((enter || ui_click_area(w, go)) && s->q[0]) { run_search(s); w->dirty = true; }
    int top = box.y + box.h + (s->results ? 14 : 26);
    if (s->results) results_page(w, s, body, top);
    else start_page(w, s, body, top);
}
static void search_destroy(window_t *w) { srch_t *s = w->data; if (s) { kfree(s->res); kfree(s); } w->data = NULL; }
static window_t *search_win;
void wrldsearch_open(const char *query) {
    if (search_win && wm_alive(search_win)) {
        srch_t *s = search_win->data;
        if (query && *query) { strlcpy(s->q, query, sizeof s->q); run_search(s); }
        else s->want_focus = true;
        search_win->dirty = true;
        wm_focus(search_win);
        return;
    }
    window_t *w = wm_create("The WRLD Search", -1, -1, MIN(920, SCREEN_W - 40), MIN(640, SCREEN_H - 70), WF_RESIZABLE);
    if (!w) return;
    srch_t *s = kzalloc(sizeof *s);
    w->data = s; w->paint = search_paint; w->destroy = search_destroy; w->icon = ICON_SEARCH;
    s->want_focus = true;
    search_win = w;
    if (query && *query) { strlcpy(s->q, query, sizeof s->q); run_search(s); }
}
