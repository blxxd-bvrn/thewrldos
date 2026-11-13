/* Desktop shell: wallpaper, icons, taskbar, start menu, tray, boot & welcome screens */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"
#include "net.h"
#include "usb.h"

int wallpaper_id = 0;
int wallpaper_fit = 0;                        /* 0 crop, 1 scale, 2 stretch */
uint32_t desktop_color = HEX(0x3A6EA5);
char user_name[32] = "User";
char computer_name[32] = "WRLD-PC";
int user_picture = 0;
int user_picture_count = 71;                  /* the photographs in C:\WRLD\Avatars */
bool show_clock = true, show_seconds = false, clock_24h = false;
static canvas_t *wallpaper;
static int tb_y(void);
static bool startmenu_open, allprog_open;
static int sm_hot = -1, ap_hot = -1, tb_hot = -1;

/* ---------------- wallpapers ----------------
 * The photographs live in C:\WRLD\Wallpapers and are decoded by the same
 * decoder the browser uses. The screen-sized copy is rebuilt only when the
 * choice or the resolution changes; the decoded source is kept for the theme
 * thumbnails, which are painted on every frame. */
static int isin(int a) {   /* a: 0..1023 = 0..2pi, returns -256..256 */
    a &= 1023;
    int neg = a >= 512; if (neg) a -= 512;
    int x = a * 314 / 512;                   /* 0..314 ~ 0..pi*100 */
    int num = 16 * x * (314 - x), den = 5 * 314 * 314 - 4 * x * (314 - x);
    int v = num * 256 / (den ? den : 1);
    return neg ? -v : v;
}
int ext_isin(int a) { return isin(a); }

static const char *wallpaper_files[] = {
    "C:\\WRLD\\Wallpapers\\Blue shoal.jpg",
    "C:\\WRLD\\Wallpapers\\Home planet.jpg",
    "C:\\WRLD\\Wallpapers\\We carry the world.jpg",
    "C:\\WRLD\\Wallpapers\\Bubble city.jpg",
    "C:\\WRLD\\Wallpapers\\Kelp forest.jpg",
    "C:\\WRLD\\Wallpapers\\Northern lights.jpg",
    "C:\\WRLD\\Wallpapers\\Aurora sky.jpg",
    "C:\\WRLD\\Wallpapers\\City lights.jpg",
    "C:\\WRLD\\Wallpapers\\Summer leaves.jpg",
};
const char *wallpaper_names[] = { "Blue shoal", "Home planet", "We carry the world", "Bubble city",
                                  "Kelp forest", "Northern lights", "Aurora sky", "City lights",
                                  "Summer leaves", "(None)" };
int wallpaper_count = ARRAY_LEN(wallpaper_names);

static uint32_t wall_mix(uint32_t a, uint32_t b, int w)
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return RGB(ar + (((br - ar) * w) >> 8), ag + (((bg - ag) * w) >> 8), ab + (((bb - ab) * w) >> 8));
}
/* fit 0: crop to the screen's shape, 1: scale the whole image inside it with
 * bars, 2: stretch it to both edges; all three sample bilinearly */
static void wall_scale(canvas_t *dst, canvas_t *src, int fit)
{
    int W = dst->w, H = dst->h, sw = src->w, sh = src->h;
    if (W <= 0 || H <= 0 || sw < 2 || sh < 2) { gfx_clear(dst, HEX(0x202020)); return; }
    int sx0 = 0, sy0 = 0, cw = sw, ch = sh, dx = 0, dy = 0, dw = W, dh = H;
    if (fit == 1) {                                      /* scale: fit inside */
        gfx_clear(dst, BLACK);
        if ((int64_t)sw * H > (int64_t)sh * W) { dw = W; dh = (int)((int64_t)sh * W / sw); }
        else { dh = H; dw = (int)((int64_t)sw * H / sh); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        dx = (W - dw) / 2; dy = (H - dh) / 2;
    } else if (fit != 2) {                               /* crop: cover the screen */
        if ((int64_t)sw * H > (int64_t)sh * W) { ch = sh; cw = (int)((int64_t)sh * W / H); sx0 = (sw - cw) / 2; }
        else { cw = sw; ch = (int)((int64_t)sw * H / W); sy0 = (sh - ch) / 2; }
        if (cw < 2) cw = 2;
        if (ch < 2) ch = 2;
    }
    for (int y = 0; y < dh; y++) {
        uint32_t fy = (uint32_t)(((uint64_t)y * ch << 8) / dh);
        int ry = (int)(fy >> 8), wy = fy & 255;
        if (ry >= ch - 1) { ry = ch - 2; wy = 255; }
        int sy = sy0 + ry;
        const uint32_t *r0 = src->px + sy * src->stride;
        const uint32_t *r1 = r0 + src->stride;
        for (int x = 0; x < dw; x++) {
            uint32_t fx = (uint32_t)(((uint64_t)x * cw << 8) / dw);
            int rx = (int)(fx >> 8), wx = fx & 255;
            if (rx >= cw - 1) { rx = cw - 2; wx = 255; }
            int sx = sx0 + rx;
            uint32_t top = wall_mix(r0[sx], r0[sx + 1], wx);
            uint32_t bot = wall_mix(r1[sx], r1[sx + 1], wx);
            dst->px[(dy + y) * dst->stride + dx + x] = wall_mix(top, bot, wy);
        }
    }
}
static canvas_t *wall_src; static int wall_src_id = -1;
static canvas_t *wall_out; static int wall_out_id = -1, wall_out_fit = -1;
static canvas_t *wall_source(int id)
{
    if (wall_src && wall_src_id == id) return wall_src;
    if (wall_src) { canvas_free(wall_src); wall_src = NULL; wall_src_id = -1; }
    vnode_t *n = vfs_resolve(wallpaper_files[id], NULL);
    if (n && !n->dir && n->size) {
        char err[64];
        wall_src = image_decode(n->data, n->size, err, sizeof err);
        if (wall_src) wall_src_id = id;
    }
    return wall_src;
}
void wallpaper_render(canvas_t *c, int id) {
    if (id < 0 || id >= wallpaper_count - 1) { gfx_clear(c, desktop_color); return; }
    if (wall_out && wall_out_id == id && wall_out_fit == wallpaper_fit && wall_out->w == c->w && wall_out->h == c->h) {
        memcpy(c->px, wall_out->px, (size_t)c->w * c->h * 4);
        return;
    }
    canvas_t *src = wall_source(id);
    if (!src) { gfx_clear(c, desktop_color); return; }
    wall_scale(c, src, wallpaper_fit);
    /* remember this size: the theme thumbnails re-render the same 120x80 view
     * on every frame */
    if (!wall_out || wall_out->w != c->w || wall_out->h != c->h) {
        if (wall_out) canvas_free(wall_out);
        wall_out = canvas_new(c->w, c->h);
    }
    if (wall_out) { memcpy(wall_out->px, c->px, (size_t)c->w * c->h * 4); wall_out_id = id; wall_out_fit = wallpaper_fit; }
}
void shell_wallpaper_changed(void) {
    static int last_id = -1, last_w, last_h, last_fit = -1; static uint32_t last_col;
    if (wallpaper && (wallpaper->w != SCREEN_W || wallpaper->h != SCREEN_H)) { canvas_free(wallpaper); wallpaper = NULL; last_id = -1; }
    if (!wallpaper) wallpaper = canvas_new(SCREEN_W, SCREEN_H);
    if (last_id == wallpaper_id && last_fit == wallpaper_fit && last_w == SCREEN_W && last_h == SCREEN_H && (wallpaper_id != wallpaper_count - 1 || last_col == desktop_color)) return;
    wallpaper_render(wallpaper, wallpaper_id);
    last_id = wallpaper_id; last_col = desktop_color; last_fit = wallpaper_fit; last_w = SCREEN_W; last_h = SCREEN_H;
    wm_need_redraw = true;
}

/* ---------------- account pictures ----------------
 * The 71 photographs in C:\WRLD\Avatars are decoded once into 96x96 cached
 * thumbnails; draw_user_picture() masks one to a circle at the size asked
 * for, so the log-on tile, the Start menu and the account page all use it. */
#define AVATAR_CACHE 128
static canvas_t *avatar_cache[71];
static void avatar_scale(canvas_t *dst, canvas_t *src)
{
    int size = dst->w, sw = src->w, sh = src->h;
    int side = MIN(sw, sh), sx0 = (sw - side) / 2, sy0 = (sh - side) / 2;
    for (int y = 0; y < size; y++) {
        uint32_t fy = (uint32_t)(((uint64_t)y * side << 8) / size);
        int ry = (int)(fy >> 8), wy = fy & 255;
        if (ry >= side - 1) { ry = side - 2; wy = 255; }
        const uint32_t *r0 = src->px + (sy0 + ry) * src->stride + sx0;
        const uint32_t *r1 = r0 + src->stride;
        for (int x = 0; x < size; x++) {
            uint32_t fx = (uint32_t)(((uint64_t)x * side << 8) / size);
            int rx = (int)(fx >> 8), wx = fx & 255;
            if (rx >= side - 1) { rx = side - 2; wx = 255; }
            uint32_t top = wall_mix(r0[rx], r0[rx + 1], wx);
            uint32_t bot = wall_mix(r1[rx], r1[rx + 1], wx);
            dst->px[y * dst->stride + x] = wall_mix(top, bot, wy);
        }
    }
}
static canvas_t *avatar_thumb(int id)
{
    if (id < 0 || id >= user_picture_count) id = 0;
    if (avatar_cache[id]) return avatar_cache[id];
    char path[80];
    snprintf(path, sizeof path, "C:\\WRLD\\Avatars\\Avatar %02d.jpg", id + 1);
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir || !n->size) return NULL;
    char err[64];
    canvas_t *src = image_decode(n->data, n->size, err, sizeof err);
    if (!src) return NULL;
    canvas_t *dst = src->w >= 2 && src->h >= 2 ? canvas_new(AVATAR_CACHE, AVATAR_CACHE) : NULL;
    if (dst) { avatar_scale(dst, src); avatar_cache[id] = dst; }
    canvas_free(src);
    return dst;
}
void draw_user_picture(canvas_t *c, int id, int size, int x, int y)
{
    if (size <= 0) return;
    canvas_t *a = avatar_thumb(id);
    if (!a) { fill_round_rect(c, x, y, size, size, MAX(2, size / 8), HEX(0xA8B8C8)); return; }
    /* a rounded-box cutout, the way Windows XP framed account pictures */
    int rad = MAX(2, size / 8);
    for (int yy = 0; yy < size; yy++) for (int xx = 0; xx < size; xx++) {
        int cx = xx < rad ? rad : (xx >= size - rad ? size - 1 - rad : xx);
        int cy = yy < rad ? rad : (yy >= size - rad ? size - 1 - rad : yy);
        int dx = xx - cx, dy = yy - cy;
        if (dx * dx + dy * dy > rad * rad) continue;
        int sx = xx * AVATAR_CACHE / size, sy = yy * AVATAR_CACHE / size;
        put_pixel(c, x + xx, y + yy, a->px[sy * a->stride + sx]);
    }
}

/* ---------------- Longhorn-style sidebar ----------------
 * A panel on the right edge of the desktop with removable tiles: clock, app
 * list, search, shortcuts, quick launch, usage meter and memo. It follows the
 * active visual style; the toggle arrow lives at the right end of the taskbar. */
#define SIDE_W 200
#define SIDE_HDR 62
#define SIDE_MEMO "C:\\WRLD\\Sidebar memo.txt"
enum { ST_CLOCK, ST_APPS, ST_SEARCH, ST_SHORTCUTS, ST_QUICK, ST_USAGE, ST_MEMO, ST_N };
#define TBIT(t) (1 << (t))
bool sidebar_open = false;
int sidebar_tiles = TBIT(ST_CLOCK) | TBIT(ST_APPS) | TBIT(ST_SEARCH) | TBIT(ST_QUICK) | TBIT(ST_USAGE);
static int side_hot = -1;
static char side_query[48];
static bool side_search_focus;
#define SIDE_RESULTS 5
typedef struct { char label[64], path[192], action[32]; int icon, kind; } side_res_t;
static side_res_t side_res[SIDE_RESULTS]; static int side_res_n;
static char side_last_query[48]; static uint32_t side_res_gen;
static int side_cpu, side_ram;
static uint32_t side_last_tick, side_last_idle;
static char side_memo[192]; static uint32_t side_memo_gen = 0xFFFFFFFF;

static const char *side_tile_name[ST_N] = { "Clock & Date", "Apps", "Search", "Shortcuts", "Quick Launch", "Usage Meter", "Memo" };
static const int side_tile_icon[ST_N] = { ICON_CP_CLOCK, ICON_CP_PROGRAMS, ICON_SEARCH, ICON_FOLDER, ICON_BROWSER, ICON_TASKMGR, ICON_NOTEPAD };
static const struct { const char *name, *action; int icon; } side_short[] = {
    { "My Documents", USER_DOCS, ICON_MY_DOCUMENTS },
    { "My Computer", "::computer", ICON_COMPUTER },
    { "Control Panel", "::control", ICON_CONTROL_PANEL },
};
static const struct { int icon; const char *app; } side_quick[] = {
    { ICON_BROWSER, "xplorer" }, { ICON_EXPLORER, "explorer" }, { ICON_TERMINAL, "cmd" }, { ICON_NOTEPAD, "notepad" },
};
#define SIDE_SHORTCUTS ((int)ARRAY_LEN(side_short))
#define SIDE_QUICK ((int)ARRAY_LEN(side_quick))

static int side_body_h(int t) {
    switch (t) {
    case ST_CLOCK: return 48;
    case ST_APPS: return 5 * 16;
    case ST_SEARCH: return 34 + SIDE_RESULTS * 16;
    case ST_SHORTCUTS: return SIDE_SHORTCUTS * 16;
    case ST_QUICK: return 30;
    case ST_USAGE: return 44;
    default: return 34;
    }
}
static void side_colors(uint32_t *top, uint32_t *bot, uint32_t *text, uint32_t *dim, uint32_t *hdr1, uint32_t *hdr2) {
    if (T.classic) { *top = *bot = T.face; *text = BLACK; *dim = HEX(0x404040); *hdr1 = T.face_light; *hdr2 = T.face; }
    else {
        /* the taskbar's own Luna blue, so the panel belongs to the same
         * surface rather than reading as a dark slab */
        *top = T.tb_hi; *bot = T.tb_mid;
        *text = WHITE; *dim = mix(WHITE, *bot, 70);
        *hdr1 = lighten(T.sm_head_top, 12); *hdr2 = T.sm_head_bot;
    }
}
static uint32_t side_bg_at(int y, int h) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    return mix(top, bot, MIN(255, y * 255 / MAX(1, h)));
}
/* the welcome picture: full-bleed photo fading into the panel, not cut to the
 * account-picture rounded box used elsewhere */
static void draw_avatar_banner(canvas_t *c, int id, int x, int y, int w, int h, uint32_t fade) {
    canvas_t *a = avatar_thumb(id);
    if (!a) { fill_rect(c, x, y, w, h, fade); return; }
    int side = AVATAR_CACHE;
    int crop = side * h / MAX(1, w);
    if (crop > side) crop = side;
    if (crop < 2) crop = 2;
    int oy = (side - crop) / 2;
    int fade_from = h * 40 / 100;
    for (int yy = 0; yy < h; yy++) {
        uint32_t fy = (uint32_t)(((uint64_t)yy * crop << 8) / h);
        int ry = (int)(fy >> 8), wy = fy & 255;
        if (ry >= crop - 1) { ry = crop - 2; wy = 255; }
        const uint32_t *r0 = a->px + (oy + ry) * a->stride;
        const uint32_t *r1 = r0 + a->stride;
        int alpha = yy > fade_from ? MIN(255, (yy - fade_from) * 255 / MAX(1, h - fade_from)) : 0;
        for (int xx = 0; xx < w; xx++) {
            uint32_t fx = (uint32_t)(((uint64_t)xx * side << 8) / w);
            int rx = (int)(fx >> 8), wx = fx & 255;
            if (rx >= side - 1) { rx = side - 2; wx = 255; }
            uint32_t col = wall_mix(wall_mix(r0[rx], r0[rx + 1], wx), wall_mix(r1[rx], r1[rx + 1], wx), wy);
            if (alpha) col = mix(col, fade, alpha);
            put_pixel(c, x + xx, y + yy, col);
        }
    }
}
static void side_tile_header(canvas_t *c, int x, int y, int w, int t) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    if (T.classic) { fill_rect(c, x, y, w, 17, T.face); bevel(c, x, y, w, 17, true); }
    else { gradient_round_rect(c, x, y, w, 17, 3, h1, h2); blend_rect(c, x + 2, y + 1, w - 4, 1, WHITE, 45); }
    draw_icon(c, side_tile_icon[t], 12, x + 4, y + 2);
    draw_text(c, T.classic ? FONT_UI_BOLD : FONT_UI_BOLD, x + 20, y + 2, side_tile_name[t], text);
    draw_text(c, FONT_UI, x + w - 11, y + 1, "x", side_hot == 10 + t ? text : dim);
}
static void side_bar(canvas_t *c, int x, int y, int w, int pct, uint32_t col, const char *label) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    draw_text(c, FONT_SMALL, x, y, label, dim);
    int by = y + 12;
    fill_round_rect(c, x, by, w, 5, 2, mix(bot, BLACK, 60));
    fill_round_rect(c, x, by, MAX(2, w * pct / 100), 5, 2, col);
}
static void side_memo_load(void) {
    if (side_memo_gen == vfs_generation) return;
    side_memo_gen = vfs_generation;
    side_memo[0] = 0;
    vnode_t *n = vfs_resolve(SIDE_MEMO, NULL);
    if (n && !n->dir && n->size) {
        int l = MIN((int)n->size, (int)sizeof side_memo - 1);
        memcpy(side_memo, n->data, l); side_memo[l] = 0;
        for (char *p = side_memo; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    }
}
static bool side_match_ci(const char *s, const char *q) {
    int ql = strlen(q);
    if (!ql) return false;
    for (; *s; s++) {
        int k = 0;
        while (k < ql && s[k]) {
            char a = s[k], b = q[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            k++;
        }
        if (k == ql) return true;
    }
    return false;
}
static int side_budget;
static void side_add_result(const char *name, const char *path, int icon, int kind, const char *action) {
    if (side_res_n >= SIDE_RESULTS) return;
    side_res_t *r = &side_res[side_res_n++];
    strlcpy(r->label, name, sizeof r->label); strlcpy(r->path, path, sizeof r->path);
    strlcpy(r->action, action, sizeof r->action); r->icon = icon; r->kind = kind;
}
static void side_scan(const char *path, int depth) {
    if (side_res_n >= SIDE_RESULTS || side_budget <= 0 || depth > 3) return;
    vnode_t *d = vfs_resolve(path, NULL);
    if (!d || !d->dir) return;
    for (vnode_t *n = d->child; n && side_res_n < SIDE_RESULTS && side_budget > 0; n = n->next) {
        side_budget--;
        if (side_match_ci(n->name, side_query)) {
            char full[192]; vfs_path(n, full, sizeof full);
            side_add_result(n->name, full, file_icon(n), 1, "");
        }
        if (n->dir && depth < 3) {
            char full[192]; vfs_path(n, full, sizeof full);
            side_scan(full, depth + 1);
        }
    }
}
static void side_search_run(void) {
    if (!strcmp(side_query, side_last_query) && side_res_gen == vfs_generation) return;
    strlcpy(side_last_query, side_query, sizeof side_last_query);
    side_res_gen = vfs_generation;
    side_res_n = 0;
    if (!side_query[0]) return;
    for (int i = 0; i < app_count && side_res_n < SIDE_RESULTS; i++)
        if (side_match_ci(app_table[i].title, side_query)) side_add_result(app_table[i].title, "", app_table[i].icon, 0, app_table[i].id);
    static const struct { const char *name, *page; int icon; } settings[] = {
        { "Sound settings", "sound", ICON_CP_SOUND }, { "Network settings", "network", ICON_CP_NETWORK },
        { "Display settings", "display", ICON_CP_DISPLAY }, { "Mouse settings", "mouse", ICON_CP_MOUSE },
        { "Keyboard settings", "keyboard", ICON_CP_KEYBOARD }, { "Date and Time", "datetime", ICON_CP_DATETIME },
        { "User Accounts", "users", ICON_CP_USERS }, { "Programs and Features", "programs", ICON_CP_PROGRAMS },
        { "System information", "system", ICON_CP_SYSTEM }, { "Taskbar and Start Menu", "taskbar", ICON_CP_TASKBAR },
    };
    for (unsigned i = 0; i < ARRAY_LEN(settings) && side_res_n < SIDE_RESULTS; i++)
        if (side_match_ci(settings[i].name, side_query)) side_add_result(settings[i].name, "", settings[i].icon, 2, settings[i].page);
    side_budget = 600;
    side_scan(USER_DOCS, 0);
    side_scan("C:\\Samples", 2);
    side_scan("C:\\", 2);
}
static void side_run_result(int i) {
    if (i < 0 || i >= side_res_n) return;
    side_res_t *r = &side_res[i];
    if (r->kind == 0) app_launch(r->action, NULL);
    else if (r->kind == 1) shell_open(r->path, NULL);
    else if (r->kind == 2) control_open(r->action);
    side_search_focus = false;
}
static void side_shortcut(int i) {
    if (i == 0) shell_open(USER_DOCS, NULL);
    else if (i == 1) explorer_open("::computer");
    else if (i == 2) app_launch("control", NULL);
}
static void side_quick_launch(int i) {
    if (i < 0 || i >= SIDE_QUICK) return;
    app_launch(side_quick[i].app, NULL);
}
static void side_add_cb(void *ctx, int id) {
    (void)ctx;
    if (id >= 1 && id <= ST_N) {
        for (int t = 0, n = 1; t < ST_N; t++) {
            if (sidebar_tiles & TBIT(t)) continue;
            if (n == id) { sidebar_tiles |= TBIT(t); wm_need_redraw = true; return; }
            n++;
        }
    }
}
static void side_open_add(int x, int y, int missing) {
    menu_item_t m[ST_N];
    int n = 0;
    for (int t = 0; t < ST_N; t++) if (!(sidebar_tiles & TBIT(t))) { m[n].label = side_tile_name[t]; m[n].id = n + 1; m[n].flags = 0; n++; }
    if (!n) return;
    int h = n * 18;
    popup_menu(x, MAX(0, y - h), m, n, side_add_cb, NULL);
}
static void side_click(int h) {
    if (h == 1) control_open("users");
    else if (h >= 10 && h < 10 + ST_N) sidebar_tiles &= ~TBIT(h - 10);
    else if (h == 400) side_search_focus = true;
    else if (h >= 401 && h <= 401 + SIDE_RESULTS) side_run_result(h - 401);
    else if (h >= 100 && h < 100 + 5) { if (h - 100 < app_count) app_launch(app_table[h - 100].id, NULL); }
    else if (h >= 200 && h < 200 + SIDE_SHORTCUTS) side_shortcut(h - 200);
    else if (h >= 300 && h < 300 + SIDE_QUICK) side_quick_launch(h - 300);
    else if (h == 500) notepad_open(SIDE_MEMO);
    else if (h == 600) side_open_add(SCREEN_W - SIDE_W + 8, tb_y() - 30, 0);
    else side_search_focus = false;
    wm_need_redraw = true;
}
static void paint_sidebar(canvas_t *c) {
    if (!sidebar_open) return;
    side_search_run();
    int x = SCREEN_W - SIDE_W, h = tb_y();
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    if (T.classic) {
        fill_rect(c, x, 0, SIDE_W, h, T.face);
        vline(c, x, 0, h, T.face_dark); vline(c, x + 1, 0, h, WHITE);
    } else {
        gradient_v3(c, x, 0, SIDE_W, h, top, top, bot, h * 3 / 4);
        vline(c, x, 0, h, T.sm_accent); blend_rect(c, x + 1, 0, 1, h, WHITE, 45);
        blend_rect(c, x, 0, SIDE_W, 30, WHITE, 18);
    }
    int ix = x + 6, iw = SIDE_W - 12, by = 6;
    draw_avatar_banner(c, user_picture, ix, by, iw, SIDE_HDR, side_bg_at(by + SIDE_HDR, h));
    draw_rect(c, ix, by, iw, SIDE_HDR, T.classic ? T.face_dark : mix(T.sm_accent, BLACK, 40));
    if (!T.classic) { hline(c, ix, by + SIDE_HDR - 1, iw, T.sm_accent); blend_rect(c, ix, by + SIDE_HDR, iw, 1, WHITE, 50); }
    if (side_hot == 1 && !T.classic) fill_round_rect_blend(c, ix, by, iw, SIDE_HDR, 3, WHITE, 20);
    draw_text(c, FONT_SMALL, ix + 6, by + SIDE_HDR - 31, "welcome", mix(WHITE, T.sm_accent, 80));
    draw_text_ellipsis(c, FONT_LARGE_BOLD, ix + 6, by + SIDE_HDR - 20, iw - 12, user_name, WHITE);
    by += SIDE_HDR + 8;
    for (int t = 0; t < ST_N; t++) {
        if (!(sidebar_tiles & TBIT(t))) continue;
        int bh = side_body_h(t);
        side_tile_header(c, ix, by, iw, t);
        int bx = ix, bw = iw, yy = by + 18;
        if (T.classic) { fill_rect(c, bx, yy, bw, bh, T.face); bevel(c, bx, yy, bw, bh, false); }
        else fill_round_rect_blend(c, bx, yy, bw, bh, 3, WHITE, 28);
        if (t == ST_CLOCK) {
            rtc_time_t now; rtc_now(&now);
            int cx = bx + 24, cy = yy + bh / 2, r = 19;
            fill_circle(c, cx, cy, r, HEX(0xF4F4EC)); draw_circle(c, cx, cy, r, mix(bot, BLACK, 80));
            for (int k = 0; k < 12; k++) {
                int a = k * 1024 / 12;
                int px = cx + isin(a) * (r - 3) / 256, py = cy - isin(a + 256) * (r - 3) / 256;
                put_pixel(c, px, py, HEX(0x404040));
            }
            int ha = (now.hour % 12) * 1024 / 12 + now.minute * 1024 / 720;
            int ma = now.minute * 1024 / 60 + now.second * 1024 / 3600;
            draw_thick_line(c, cx, cy, cx + isin(ha) * (r - 9) / 256, cy - isin(ha + 256) * (r - 9) / 256, 2, BLACK);
            draw_thick_line(c, cx, cy, cx + isin(ma) * (r - 6) / 256, cy - isin(ma + 256) * (r - 6) / 256, 1, HEX(0x203060));
            char tb[32], db[40];
            if (clock_24h) snprintf(tb, sizeof tb, "%02d:%02d", now.hour, now.minute);
            else { int hh = now.hour % 12; if (!hh) hh = 12; snprintf(tb, sizeof tb, "%d:%02d %s", hh, now.minute, now.hour >= 12 ? "PM" : "AM"); }
            snprintf(db, sizeof db, "%02d/%02d/%04d", now.month, now.day, now.year);
            draw_text(c, FONT_LARGE, bx + 50, yy + 7, tb, text);
            draw_text(c, FONT_SMALL, bx + 50, yy + 27, db, dim);
            if (now.second & 1) put_pixel(c, cx, cy, HEX(0xC03030));
        } else if (t == ST_APPS) {
            int n = MIN(app_count, 5);
            for (int i = 0; i < n; i++) {
                int ry = yy + i * 16;
                if (side_hot == 100 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.classic ? T.select : mix(WHITE, bot, 180));
                draw_icon(c, app_table[i].icon, 16, bx + 5, ry);
                draw_text_ellipsis(c, FONT_UI, bx + 25, ry + 1, bw - 30, app_table[i].title, text);
            }
        } else if (t == ST_SEARCH) {
            int fw = bw - 8, fy = yy + 3;
            fill_round_rect(c, bx + 4, fy, fw, 20, 3, T.classic ? WHITE : mix(bot, BLACK, 50));
            draw_rect(c, bx + 4, fy, fw, 20, side_search_focus ? T.sm_accent : (T.classic ? T.face_dark : mix(bot, WHITE, 80)));
            draw_text_ellipsis(c, FONT_UI, bx + 9, fy + 3, fw - 12, side_query[0] ? side_query : "Search everything...", side_query[0] ? text : dim);
            for (int i = 0; i < SIDE_RESULTS; i++) {
                int ry = yy + 27 + i * 16;
                if (side_query[0]) {
                    if (i < side_res_n) {
                        if (side_hot == 401 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.classic ? T.select : mix(WHITE, bot, 180));
                        draw_icon(c, side_res[i].icon, 16, bx + 5, ry);
                        draw_text_ellipsis(c, FONT_UI, bx + 25, ry + 1, bw - 30, side_res[i].label, text);
                    }
                } else if (i == 0) draw_text_ellipsis(c, FONT_SMALL, bx + 6, ry, bw - 10, "Apps, files and settings", dim);
            }
            if (side_query[0] && !side_res_n) draw_text_ellipsis(c, FONT_SMALL, bx + 6, yy + 27, bw - 10, "No matches", dim);
        } else if (t == ST_SHORTCUTS) {
            for (int i = 0; i < SIDE_SHORTCUTS; i++) {
                int ry = yy + i * 16;
                if (side_hot == 200 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.classic ? T.select : mix(WHITE, bot, 180));
                draw_icon(c, side_short[i].icon, 16, bx + 5, ry);
                draw_text(c, FONT_UI, bx + 25, ry + 1, side_short[i].name, text);
            }
        } else if (t == ST_QUICK) {
            for (int i = 0; i < SIDE_QUICK; i++) {
                int qx = bx + 6 + i * 26, qy = yy + 4;
                if (side_hot == 300 + i) fill_round_rect(c, qx - 2, qy - 2, 24, 24, 3, T.classic ? T.select : mix(WHITE, bot, 170));
                draw_icon(c, side_quick[i].icon, 16, qx + 2, qy + 2);
            }
        } else if (t == ST_USAGE) {
            side_bar(c, bx + 6, yy + 4, bw - 12, side_cpu, HEX(0x60D060), "CPU");
            char b[24]; snprintf(b, sizeof b, "CPU %d%%", side_cpu); draw_text(c, FONT_SMALL, bx + bw - 52, yy + 2, b, dim);
            side_bar(c, bx + 6, yy + 23, bw - 12, side_ram, HEX(0x5090E0), "RAM");
            snprintf(b, sizeof b, "%d%%", side_ram); draw_text(c, FONT_SMALL, bx + bw - 30, yy + 21, b, dim);
        } else if (t == ST_MEMO) {
            side_memo_load();
            draw_text_ellipsis(c, FONT_SMALL, bx + 6, yy + 4, bw - 12, side_memo[0] ? side_memo : "Click to write a note", side_memo[0] ? text : dim);
            draw_text_ellipsis(c, FONT_SMALL, bx + 6, yy + 18, bw - 12, side_memo[0] ? "Open memo" : "", dim);
        }
        by += 18 + bh + 4;
    }
    /* add tile */
    if (by + 20 <= h - 2) {
        int hovered = side_hot == 600;
        if (hovered && !T.classic) fill_round_rect_blend(c, ix, by, iw, 18, 3, WHITE, 30);
        draw_text(c, FONT_UI, ix + 8, by + 2, "+ Add tile", hovered ? text : dim);
    }
}
static int side_hit(int x, int y) {
    if (!sidebar_open || x < SCREEN_W - SIDE_W || y >= tb_y()) return -1;
    side_search_run();
    int ix = SCREEN_W - SIDE_W + 6, iw = SIDE_W - 12, by = 6;
    if (y >= by && y < by + SIDE_HDR) return 1;
    by += SIDE_HDR + 8;
    for (int t = 0; t < ST_N; t++) {
        if (!(sidebar_tiles & TBIT(t))) continue;
        int bh = side_body_h(t);
        if (y >= by && y < by + 18) return x >= ix + iw - 14 ? 10 + t : 11 + t;
        if (y >= by + 18 && y < by + 18 + bh) {
            int ry = y - (by + 18);
            if (t == ST_APPS && ry < 5 * 16) return 100 + ry / 16;
            if (t == ST_SEARCH) {
                if (ry < 24) return 400;
                int row = (ry - 27) / 16;
                if (side_query[0] && row >= 0 && row < side_res_n) return 401 + row;
                return 400;
            }
            if (t == ST_SHORTCUTS && ry < SIDE_SHORTCUTS * 16) return 200 + ry / 16;
            if (t == ST_QUICK && ry >= 2 && ry < 28) { int c = (x - (ix + 6)) / 26; if (c >= 0 && c < SIDE_QUICK) return 300 + c; }
            if (t == ST_MEMO) return 500;
        }
        by += 18 + bh + 4;
    }
    if (y >= by && y < by + 18 && x >= ix && x < ix + iw) return 600;
    return -1;
}
static void side_sample(void) {
    uint32_t now = timer_ticks(), idle = idle_ticks;
    if (now == side_last_tick) return;
    if (now - side_last_tick < 500) return;
    uint32_t dt = now - side_last_tick, di = idle - side_last_idle;
    if (side_last_tick) side_cpu = CLAMP(dt ? 100 - (int)(di * 100 / dt) : 0, 0, 100);
    side_last_tick = now; side_last_idle = idle;
    side_ram = mm_total() ? (int)((uint64_t)mm_used() * 100 / mm_total()) : 0;
    if (sidebar_open && (sidebar_tiles & TBIT(ST_USAGE))) wm_need_redraw = true;
}

/* ---------------- desktop icons ---------------- */
typedef struct { char name[96]; int icon; char path[256]; int kind; vnode_t *node; } dicon_t;
#define MAX_DICON 48
static dicon_t dicons[MAX_DICON];
static int ndicons, dsel = -1;
static uint32_t dgen;
void desktop_refresh(void) {
    ndicons = 0;
    const struct { const char *n; int ic; const char *p; } fixed[] = {
        { "My Documents", ICON_MY_DOCUMENTS, USER_DOCS }, { "My Computer", ICON_COMPUTER, "::computer" },
        { "Recycle Bin", ICON_RECYCLE_BIN, "::recycle" } };
    for (int i = 0; i < 3; i++) { strcpy(dicons[i].name, fixed[i].n); dicons[i].icon = fixed[i].ic; strcpy(dicons[i].path, fixed[i].p); dicons[i].node = NULL; }
    ndicons = 3;
    vnode_t *d = vfs_resolve(USER_DESKTOP, NULL);
    if (d) {
        vfs_sort_children(d);
        for (vnode_t *n = d->child; n && ndicons < MAX_DICON; n = n->next) {
            dicon_t *di = &dicons[ndicons++];
            strlcpy(di->name, n->name, sizeof di->name);
            if (!n->dir) { char *dot = strrchr(di->name, '.'); if (dot && (!strcasecmp(dot, ".lnk"))) *dot = 0; }
            di->icon = file_icon(n);
            vfs_path(n, di->path, sizeof di->path);
            di->node = n;
        }
    }
    dgen = vfs_generation;
    wm_need_redraw = true;
}
#define DCELL_W 76
#define DCELL_H 76
static rect_t dicon_rect(int i) {
    int rows = (SCREEN_H - T.taskbar_h - 10) / DCELL_H;
    return R(6 + (i / rows) * DCELL_W, 8 + (i % rows) * DCELL_H, DCELL_W, DCELL_H - 4);
}
static void paint_dicons(canvas_t *c) {
    for (int i = 0; i < ndicons; i++) {
        rect_t r = dicon_rect(i);
        dicon_t *d = &dicons[i];
        bool sel = i == dsel;
        int ix = r.x + (r.w - 32) / 2, iy = r.y + 2;
        if (sel) draw_icon_tinted(c, d->icon, 32, ix, iy, T.select, 110); else draw_icon(c, d->icon, 32, ix, iy);
        if (d->node && !d->node->dir && !strcasecmp(vfs_ext(d->node->name), "lnk")) {
            fill_rect(c, ix, iy + 22, 10, 10, WHITE); draw_rect(c, ix, iy + 22, 10, 10, HEX(0x404040));
            draw_line(c, ix + 3, iy + 29, ix + 7, iy + 25, HEX(0x0040C0)); hline(c, ix + 5, iy + 25, 3, HEX(0x0040C0)); vline(c, ix + 7, iy + 25, 3, HEX(0x0040C0));
        }
        /* label: up to two lines, centred */
        const char *s = d->name;
        int lw = r.w - 4, ly = iy + 36;
        char line1[64], line2[64]; line2[0] = 0;
        int n = text_fit_chars(FONT_UI, s, lw);
        if (n < (int)strlen(s)) {
            int brk = n; for (int k = n; k > 0; k--) if (s[k] == ' ') { brk = k; break; }
            strlcpy(line1, s, MIN(brk + 1, 64));
            const char *rest = s + brk; while (*rest == ' ') rest++;
            strlcpy(line2, rest, 64);
        } else strlcpy(line1, s, 64);
        for (int L = 0; L < 2; L++) {
            const char *t = L ? line2 : line1;
            if (!*t) break;
            int tw = MIN(text_width(FONT_UI, t), lw);
            int tx = r.x + (r.w - tw) / 2, ty = ly + L * 13;
            if (sel) { fill_rect(c, tx - 2, ty - 1, tw + 4, 14, T.select); draw_text_ellipsis(c, FONT_UI, tx, ty, lw, t, WHITE); }
            else { draw_text_ellipsis(c, FONT_UI, tx + 1, ty + 1, lw, t, HEX(0x000000)); draw_text_ellipsis(c, FONT_UI, tx, ty, lw, t, WHITE); }
        }
    }
}
void desktop_paint(canvas_t *c) {
    if (dgen != vfs_generation) desktop_refresh();
    if (wallpaper) memcpy(c->px, wallpaper->px, SCREEN_W * SCREEN_H * 4);
    else gfx_clear(c, desktop_color);
    paint_dicons(c);
    paint_sidebar(c);
}
static void dicon_open(int i) {
    dicon_t *d = &dicons[i];
    if (!strcmp(d->path, "::computer")) explorer_open("::computer");
    else if (!strcmp(d->path, "::recycle")) explorer_open("::recycle");
    else shell_open(d->path, NULL);
}
static void desk_ctx_cb(void *ctx, int id);
static int dicon_at(int x, int y) { for (int i = 0; i < ndicons; i++) if (pt_in(dicon_rect(i), x, y)) return i; return -1; }
static void rename_cb(void *ctx, const char *t) { vnode_t *n = ctx; if (t && *t) vfs_rename(n, t); desktop_refresh(); }
static void delete_cb(void *ctx, int r) {
    vnode_t *n = ctx;
    if (r != IDYES) return;
    extern bool recycle_move(vnode_t *n);
    recycle_move(n); desktop_refresh();
}
static void desk_ctx_cb(void *ctx, int id) {
    int i = (int)(uintptr_t)ctx;
    switch (id) {
    case 1: if (i >= 0) dicon_open(i); break;
    case 2: if (i >= 0 && dicons[i].node) { char m[160]; snprintf(m, sizeof m, "Are you sure you want to send '%s' to the Recycle Bin?", dicons[i].node->name); msgbox("Confirm File Delete", m, MBI_QUESTION, MBX_YESNO, delete_cb, dicons[i].node); } break;
    case 3: if (i >= 0 && dicons[i].node) input_box("Rename", "Type a new name:", dicons[i].node->name, rename_cb, dicons[i].node); break;
    case 4: if (i >= 0 && dicons[i].node) properties_open(dicons[i].node); else if (i == 1) control_open("system"); break;
    case 10: desktop_refresh(); break;
    case 11: { char p[256]; for (int k = 1; k < 100; k++) { snprintf(p, sizeof p, USER_DESKTOP "\\New Folder%s", k == 1 ? "" : ""); if (k > 1) snprintf(p, sizeof p, USER_DESKTOP "\\New Folder (%d)", k); if (!vfs_resolve(p, NULL)) { vfs_create(p, NULL, true); break; } } desktop_refresh(); break; }
    case 12: { char p[256]; for (int k = 1; k < 100; k++) { if (k == 1) snprintf(p, sizeof p, USER_DESKTOP "\\New Text Document.txt"); else snprintf(p, sizeof p, USER_DESKTOP "\\New Text Document (%d).txt", k); if (!vfs_resolve(p, NULL)) { vfs_create(p, NULL, false); break; } } desktop_refresh(); break; }
    case 13: control_open("personalization"); break;
    case 14: { extern void recycle_empty(void); recycle_empty(); break; }
    }
}
void desktop_mouse(int type, int x, int y, int btn) {
    int i = dicon_at(x, y);
    if (type == ME_DOWN) {
        if (dsel != i) { dsel = i; wm_need_redraw = true; }
        if (wm_active) { wm_active = NULL; wm_need_redraw = true; }
        if (btn & MB_RIGHT) {
            if (i >= 0) {
                static menu_item_t m[] = { { "Open", 1, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Rename", 3, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } };
                static menu_item_t rb[] = { { "Open", 1, 0 }, { "Empty Recycle Bin", 14, 0 } };
                if (!strcmp(dicons[i].path, "::recycle")) popup_menu(x, y, rb, 2, desk_ctx_cb, (void *)(uintptr_t)i);
                else if (!dicons[i].node) { static menu_item_t m2[] = { { "Open", 1, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } }; popup_menu(x, y, m2, 3, desk_ctx_cb, (void *)(uintptr_t)i); }
                else popup_menu(x, y, m, 6, desk_ctx_cb, (void *)(uintptr_t)i);
            } else {
                static menu_item_t m[] = { { "Refresh", 10, 0 }, { "-", 0, 0 }, { "New Folder", 11, 0 }, { "New Text Document", 12, 0 }, { "-", 0, 0 }, { "Properties", 13, 0 } };
                popup_menu(x, y, m, 6, desk_ctx_cb, (void *)-1);
            }
        }
    } else if (type == ME_DBLCLICK && i >= 0) dicon_open(i);
}

/* ---------------- taskbar ---------------- */
#define START_W_LUNA 99
#define START_W_CLASSIC 56
static int start_w(void) { return T.classic ? START_W_CLASSIC : T.renderer == RENDER_AERO ? T.taskbar_h + 8 : T.renderer == RENDER_PLEX ? 70 : START_W_LUNA; }
static void fmt_clock(char *b, int n);
static int tray_clock_w(void) {
    if (!show_clock) return 0;
    char b[32]; fmt_clock(b, sizeof b);
    return text_width(FONT_UI, b);
}
/* The tray is only as wide as what it holds: two status icons, the clock and
 * the sidebar toggle, packed together at the right edge instead of being
 * spread across a fixed-width panel with empty space between them. */
static int tray_w(void) { return 88 + tray_clock_w(); }
static void tray_icon_x(int *sx, int *nx) {
    int clock_x = SCREEN_W - 30 - tray_clock_w();
    *nx = clock_x - 26;
    *sx = *nx - 24;
}
static int tb_y(void) { return SCREEN_H - T.taskbar_h; }
static int ql_x(void) { return start_w() + (T.classic ? 6 : 8); }
#define QL_N 4
static const struct { int icon; const char *app; } quicklaunch[QL_N] = { { ICON_DESKTOP, "showdesktop" }, { ICON_BROWSER, "xplorer" }, { ICON_EXPLORER, "explorer" }, { ICON_TERMINAL, "cmd" } };
static int tasks_x(void) { return ql_x() + QL_N * 24 + 10; }
static int task_btns(window_t **out, int max) {
    int n = 0;
    for (int i = 0; i < wm_window_count() && n < max; i++) {
        window_t *w = wm_window_at(i);
        if (w->flags & (WF_DIALOG | WF_POPUP | WF_HIDDEN)) continue;
        out[n++] = w;
    }
    /* order by id so buttons don't jump when focus changes */
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && out[b - 1]->id > out[b]->id; b--) { window_t *t = out[b]; out[b] = out[b - 1]; out[b - 1] = t; }
    return n;
}
static rect_t task_btn_rect(int i, int n) {
    int avail = SCREEN_W - tray_w() - tasks_x() - 8;
    int bw = n ? MIN(160, avail / n) : 160;
    return R(tasks_x() + i * (bw + 2), tb_y() + (T.classic ? 4 : 3), bw, T.taskbar_h - (T.classic ? 6 : 5));
}
static void fmt_clock(char *b, int n) {
    rtc_time_t t; rtc_now(&t);
    int h = t.hour;
    if (clock_24h) { if (show_seconds) snprintf(b, n, "%02d:%02d:%02d", h, t.minute, t.second); else snprintf(b, n, "%02d:%02d", h, t.minute); }
    else {
        const char *ap = h >= 12 ? "PM" : "AM"; h %= 12; if (!h) h = 12;
        if (show_seconds) snprintf(b, n, "%d:%02d:%02d %s", h, t.minute, t.second, ap); else snprintf(b, n, "%d:%02d %s", h, t.minute, ap);
    }
}
/* glossy sphere: the Aero start orb */
/* Aero: the globe alone, sized to the taskbar; a soft glow behind it on hover / press */
static void paint_start_globe(canvas_t *c, int cx, int cy, int size, bool hot, bool down) {
    if (hot || down) {
        int r = size / 2 + 5;
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy; if (d2 > r * r) continue;
            blend_pixel(c, cx + dx, cy + dy, down ? T.start_bot : T.start_hi, (down ? 150 : 110) * (r * r - d2) / (r * r));
        }
    }
    draw_icon(c, ICON_LOGO, size, cx - size / 2, cy - size / 2 + (down ? 1 : 0));   /* the globe, pre-rendered at 32/48 px */
}
static void paint_start_button(canvas_t *c, bool hot, bool down) {
    int y = tb_y();
    if (T.renderer == RENDER_AERO) { int sz = T.taskbar_h >= 56 ? 48 : 32; paint_start_globe(c, start_w() / 2, y + T.taskbar_h / 2, sz, hot, down); return; }
    if (T.renderer == RENDER_PLEX) {   /* Longhorn: a plain globe + "start", highlighted on hover */
        rect_t r = R(2, y + 3, start_w() - 6, T.taskbar_h - 6);
        if (hot || down) { fill_round_rect_blend(c, r.x, r.y, r.w, r.h, 4, down ? T.tray_border : WHITE, down ? 140 : 60); fill_round_rect_blend(c, r.x + 1, r.y + 1, r.w - 2, r.h / 2, 3, WHITE, 40); }
        draw_icon(c, ICON_START_LOGO, 16, r.x + 6, r.y + (r.h - 16) / 2);
        draw_text(c, FONT_LARGE, r.x + 27, r.y + (r.h - font_height(FONT_LARGE)) / 2, "start", T.start_text);
        return;
    }
    if (T.classic) {
        rect_t r = R(2, y + 4, START_W_CLASSIC - 2, T.taskbar_h - 6);
        fill_rect(c, r.x, r.y, r.w, r.h, T.face);
        bevel_deep(c, r.x, r.y, r.w, r.h, !down);
        draw_icon(c, ICON_START_LOGO, 16, r.x + 4, r.y + (r.h - 16) / 2);
        draw_text(c, FONT_UI_BOLD, r.x + 23, r.y + (r.h - font_height(FONT_UI_BOLD)) / 2, "Start", BLACK);
        return;
    }
    int w = START_W_LUNA, h = T.taskbar_h;
    uint32_t hi = T.start_hi, top = T.start_top, bot = T.start_bot;
    if (hot && !down) { hi = lighten(hi, 40); top = lighten(top, 30); bot = lighten(bot, 20); }
    if (down) { top = darken(top, 40); bot = darken(bot, 30); hi = top; }
    for (int j = 0; j < h; j++) {
        int in = 0, rr = 10;
        if (j < rr) { int dy = rr - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
        if (j > h - rr) { int dy = j - (h - rr), dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
        uint32_t col = j < 3 ? mix(hi, top, j * 85) : mix(top, bot, (j - 3) * 255 / (h - 3));
        if (j > h / 2 && j < h - 3) col = mix(col, lighten(top, 20), 40);
        hline(c, 0, y + j, w - in, col);
        if (in || j == 0) put_pixel(c, w - in - 1, y + j, darken(bot, 60));
    }
    draw_icon(c, ICON_START_LOGO, 16, 10, y + 7);
    blend_rect(c, 0, y + h - 2, w - 6, 1, BLACK, 40);
    draw_text(c, FONT_START, 31, y + (h - font_height(FONT_START)) / 2 + 2, "start", T.start_shadow);
    draw_text(c, FONT_START, 30, y + (h - font_height(FONT_START)) / 2 + 1, "start", T.start_text);
}
void taskbar_paint(canvas_t *c) {
    int y = tb_y(), h = T.taskbar_h;
    bool aero = T.renderer == RENDER_AERO, plex = T.renderer == RENDER_PLEX;
    if (T.classic) {
        fill_rect(c, 0, y, SCREEN_W, h, T.face);
        hline(c, 0, y, SCREEN_W, T.face); hline(c, 0, y + 1, SCREEN_W, WHITE);
    } else if (aero) {
        draw_glass(c, 0, y, SCREEN_W, h, T.tb_top, 170, 6);
        for (int j = 0; j < h / 2; j++) blend_rect(c, 0, y + j, SCREEN_W, 1, WHITE, 36 - j * 36 / (h / 2));
        hline(c, 0, y, SCREEN_W, HEX(0x0A1018)); blend_rect(c, 0, y + 1, SCREEN_W, 1, T.tb_hi, 140);
    } else if (plex) {
        gradient_v3(c, 0, y, SCREEN_W, h, T.tb_hi, T.tb_top, T.tb_bot, 3);
        for (int j = 1; j < h / 2; j++) blend_rect(c, 0, y + j, SCREEN_W, 1, WHITE, 50 - j * 100 / h);
        hline(c, 0, y, SCREEN_W, T.tray_border); blend_rect(c, 0, y + 1, SCREEN_W, 1, WHITE, 120);
    } else {
        gradient_v3(c, 0, y, SCREEN_W, h, T.tb_hi, T.tb_top, T.tb_bot, 4);
        hline(c, 0, y, SCREEN_W, darken(T.tb_top, 60));
        hline(c, 0, y + 1, SCREEN_W, lighten(T.tb_hi, 60));
    }
    paint_start_button(c, tb_hot == 1000, startmenu_open);
    /* quick launch */
    int qx = ql_x();
    if (T.classic) { vline(c, qx - 3, y + 4, h - 6, HEX(0x808080)); vline(c, qx - 2, y + 4, h - 6, WHITE); }
    else if (plex) { vline(c, qx - 3, y + 5, h - 9, T.tray_border); blend_rect(c, qx - 2, y + 5, 1, h - 9, WHITE, 110); }
    else if (!aero) { vline(c, qx - 3, y + 3, h - 5, darken(T.tb_top, 50)); vline(c, qx - 2, y + 3, h - 5, lighten(T.tb_top, 60)); }
    for (int i = 0; i < QL_N; i++) {
        rect_t r = R(qx + i * 24, y + (h - 22) / 2, 22, 22);
        if (tb_hot == 2000 + i) { if (T.classic) bevel(c, r.x, r.y, r.w, r.h, true); else { blend_rect(c, r.x, r.y, r.w, r.h, WHITE, 60); draw_rect(c, r.x, r.y, r.w, r.h, lighten(T.tb_top, 120)); } }
        draw_icon(c, quicklaunch[i].icon, 16, r.x + 3, r.y + 3);
    }
    /* task buttons */
    window_t *list[32]; int n = task_btns(list, 32);
    for (int i = 0; i < n; i++) {
        window_t *w = list[i]; rect_t r = task_btn_rect(i, n);
        bool act = w == wm_active && w->state != WS_MINIMIZED;
        bool hot = tb_hot == i;
        if (T.classic) {
            fill_rect(c, r.x, r.y, r.w, r.h, act ? HEX(0xE6E3DD) : T.face);
            bevel_deep(c, r.x, r.y, r.w, r.h, !act);
            draw_icon(c, w->icon >= 0 ? w->icon : ICON_EXE, 16, r.x + 4 + act, r.y + (r.h - 16) / 2 + act);
            draw_text_ellipsis(c, act ? FONT_UI_BOLD : FONT_UI, r.x + 24 + act, r.y + (r.h - 14) / 2 + act, r.w - 30, w->title, BLACK);
        } else if (plex) {   /* flat buttons, only the active/hot one has a frame */
            if (act || hot) {
                fill_round_rect(c, r.x, r.y, r.w, r.h, 3, T.tbtn_border);
                gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 2, act ? T.tbtn_act_top : T.tbtn_hot, act ? T.tbtn_act_bot : T.tbtn_bot);
                fill_round_rect_blend(c, r.x + 2, r.y + 2, r.w - 4, (r.h - 4) / 2, 2, WHITE, 50);
            }
            draw_icon(c, w->icon >= 0 ? w->icon : ICON_EXE, 16, r.x + 6, r.y + (r.h - 16) / 2);
            draw_text_ellipsis(c, act ? FONT_UI_BOLD : FONT_UI, r.x + 26, r.y + (r.h - 14) / 2, r.w - 32, w->title, T.tbtn_text);
        } else if (aero) {
            fill_round_rect_blend(c, r.x, r.y, r.w, r.h, 3, BLACK, 90);
            fill_round_rect_blend(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, act ? T.tbtn_act_top : hot ? T.tbtn_hot : T.tbtn_top, act ? 150 : hot ? 120 : 70);
            fill_round_rect_blend(c, r.x + 2, r.y + 2, r.w - 4, (r.h - 4) / 2, 2, WHITE, act ? 60 : 35);
            if (act) blend_rect(c, r.x + 6, r.y + r.h - 3, r.w - 12, 1, WHITE, 120);
            draw_icon(c, w->icon >= 0 ? w->icon : ICON_EXE, 16, r.x + 6, r.y + (r.h - 16) / 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 27, r.y + (r.h - 14) / 2 + 1, r.w - 33, w->title, BLACK);
            draw_text_ellipsis(c, FONT_UI, r.x + 26, r.y + (r.h - 14) / 2, r.w - 32, w->title, T.tbtn_text);
        } else {
            uint32_t top = act ? T.tbtn_act_top : T.tbtn_top, bot = act ? T.tbtn_act_bot : T.tbtn_bot;
            if (hot && !act) { top = lighten(top, 30); bot = lighten(bot, 30); }
            fill_round_rect(c, r.x, r.y, r.w, r.h, 3, T.tbtn_border);
            gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 2, top, bot);
            if (!act) hline(c, r.x + 2, r.y + 1, r.w - 4, lighten(top, 60));
            else hline(c, r.x + 2, r.y + 1, r.w - 4, darken(top, 40));
            draw_icon(c, w->icon >= 0 ? w->icon : ICON_EXE, 16, r.x + 6, r.y + (r.h - 16) / 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 26, r.y + (r.h - 14) / 2, r.w - 32, w->title, T.tbtn_text);
        }
    }
    /* tray */
    int tw = tray_w(), tx = SCREEN_W - tw;
    if (T.classic) {
        rect_t r = R(tx, y + 4, tw - 3, h - 6);
        fill_rect(c, r.x, r.y, r.w, r.h, T.face); bevel(c, r.x, r.y, r.w, r.h, false);
    } else if (aero || plex) {
        blend_rect(c, tx, y + 4, 1, h - 8, plex ? T.tray_border : BLACK, plex ? 255 : 90); blend_rect(c, tx + 1, y + 4, 1, h - 8, WHITE, plex ? 200 : 50);
    } else {
        gradient_v3(c, tx, y + 1, tw, h - 1, T.tray_hi, T.tray_top, T.tray_bot, 4);
        vline(c, tx, y + 1, h - 1, T.tray_border); vline(c, tx + 1, y + 1, h - 1, lighten(T.tray_hi, 80));
    }
    int sx, nx; tray_icon_x(&sx, &nx);
    draw_icon(c, ICON_CP_SOUND, 16, sx, y + (h - 16) / 2);
    {   /* network tray: a monitor icon with a link/address status dot */
        draw_icon(c, ICON_CP_NETWORK, 16, nx, y + (h - 16) / 2);
        bool up = netif.link && netif.configured;
        uint32_t dot = !netif.present ? HEX(0x808080) : netif.link ? (up ? HEX(0x30C030) : HEX(0xE0B030)) : HEX(0xD03030);
        fill_circle(c, nx + 13, y + (h - 16) / 2 + 13, 2, dot);
    }
    if (show_clock) {
        char b[32]; fmt_clock(b, sizeof b);
        int cw = text_width(FONT_UI, b);
        draw_text(c, FONT_UI, SCREEN_W - 30 - cw, y + (h - font_height(FONT_UI)) / 2, b, T.tray_text);
    }
    {   /* sidebar toggle: an up arrow when closed, down when open */
        int bx = SCREEN_W - 20;
        if (tb_hot == 4000) {
            if (T.classic) bevel(c, bx + 2, y + 4, 16, h - 8, false);
            else fill_round_rect_blend(c, bx + 1, y + 3, 17, h - 6, 3, WHITE, 45);
        }
        draw_arrow(c, bx + 9, y + h / 2 - 2, sidebar_open ? 1 : 0, T.tray_text);
    }
}

/* ---------------- balloon ---------------- */
static char balloon_title[64], balloon_text[200];
static uint32_t balloon_until;
void notify_balloon(const char *title, const char *text) {
    strlcpy(balloon_title, title, sizeof balloon_title); strlcpy(balloon_text, text, sizeof balloon_text);
    balloon_until = timer_ticks() + 6000; wm_need_redraw = true;
}
static rect_t balloon_rect(void) {
    int w = 280, h = 34 + text_wrapped_height(FONT_UI, w - 20, balloon_text, 1);
    return R(SCREEN_W - w - 16, tb_y() - h - 14, w, h);
}
static void paint_balloon(canvas_t *c) {
    if (!balloon_until || (int32_t)(timer_ticks() - balloon_until) > 0) return;
    rect_t r = balloon_rect();
    blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 40);
    fill_round_rect(c, r.x, r.y, r.w, r.h, 8, BLACK);
    fill_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, HEX(0xFFFFE1));
    for (int i = 0; i < 12; i++) { hline(c, r.x + r.w - 40 + i, r.y + r.h - 1 + i, 13 - i, HEX(0xFFFFE1)); put_pixel(c, r.x + r.w - 41 + i, r.y + r.h - 1 + i, BLACK); put_pixel(c, r.x + r.w - 27, r.y + r.h - 1 + i, BLACK); }
    draw_icon(c, ICON_INFO, 16, r.x + 10, r.y + 8);
    draw_text(c, FONT_UI_BOLD, r.x + 32, r.y + 9, balloon_title, BLACK);
    draw_text_wrapped(c, FONT_UI, r.x + 10, r.y + 28, r.w - 20, balloon_text, BLACK, 1);
}

/* ---------------- start menu ---------------- */
typedef struct { const char *label, *sub; int icon; const char *action; bool bold; } sm_item_t;
static const sm_item_t sm_left[] = {
    { "Internet", "XplorerLite", ICON_BROWSER, "xplorer", true },
    { "Command Prompt", "Shell & scripts", ICON_TERMINAL, "cmd", true },
    { "File Explorer", "Browse files", ICON_EXPLORER, "explorer", true },
    { NULL },
    { "Notepad", NULL, ICON_NOTEPAD, "notepad", false },
    { "Paint", NULL, ICON_PAINT, "paint", false },
    { "Task Manager", NULL, ICON_TASKMGR, "taskmgr", false },
    { "Snake", NULL, ICON_GAME, "snake", false },
};
static const sm_item_t sm_right[] = {
    { "My Documents", NULL, ICON_MY_DOCUMENTS, "explorer:" USER_DOCS, true },
    { "My Computer", NULL, ICON_COMPUTER, "explorer:::computer", true },
    { NULL },
    { "Control Panel", NULL, ICON_CONTROL_PANEL, "control", false },
    { "Personalization", NULL, ICON_CP_APPEARANCE, "control:personalization", false },
    { "System Information", NULL, ICON_CP_SYSTEM, "control:system", false },
    { NULL },
    { "Help and Support", NULL, ICON_HELP, "help", false },
    { "Run...", NULL, ICON_RUN, "run", false },
};
static const sm_item_t all_programs[] = {
    { "Calculator", NULL, ICON_CALCULATOR, "calc" }, { "Command Prompt", NULL, ICON_TERMINAL, "cmd" }, { "XplorerLite", NULL, ICON_BROWSER, "xplorer" },
    { "Control Panel", NULL, ICON_CONTROL_PANEL, "control" }, { "File Explorer", NULL, ICON_EXPLORER, "explorer" },
    { "Notepad", NULL, ICON_NOTEPAD, "notepad" }, { "Paint", NULL, ICON_PAINT, "paint" },
    { "Snake", NULL, ICON_GAME, "snake" }, { "Task Manager", NULL, ICON_TASKMGR, "taskmgr" },
    { NULL },
    { "Samples (EXE / DMG / SH)", NULL, ICON_FOLDER, "explorer:C:\\Samples" },
    { "Hello Console (hello.exe)", NULL, ICON_EXE, "open:C:\\Samples\\hello.exe" },
    { "Hello Win32 (winhello.exe)", NULL, ICON_EXE, "open:C:\\Samples\\winhello.exe" },
    { "Shell demo (demo.sh)", NULL, ICON_SCRIPT, "open:C:\\Samples\\demo.sh" },
    { "Mount Sample.dmg", NULL, ICON_DMG, "open:C:\\Samples\\Sample.dmg" },
    { NULL },
    { "About " OS_NAME, NULL, ICON_ABOUT, "about" },
};
#define SM_LEFT_N ((int)ARRAY_LEN(sm_left))
#define SM_RIGHT_N ((int)ARRAY_LEN(sm_right))
#define AP_N ((int)ARRAY_LEN(all_programs))
#define SM_W 380
#define SM_H 480
#define SM_HEAD 64
#define SM_FOOT 44

static rect_t sm_rect(void) {
    if (T.classic) return R(0, tb_y() - 290, 190, 290);
    return R(0, tb_y() - SM_H, SM_W, SM_H);
}
/* item hit ids: 0..99 left, 100..199 right, 300 all programs, 400 logoff, 401 shutdown */
static rect_t sm_item_rect(int id) {
    rect_t m = sm_rect();
    if (T.classic) {
        int y = m.y + 3 + (id % 100) * 26;
        if (id >= 100) y += 6;
        return R(m.x + 24, y, m.w - 27, 26);
    }
    int y0 = m.y + SM_HEAD + 6;
    if (id < 100) {
        int y = y0;
        for (int i = 0; i < id; i++) y += !sm_left[i].label ? 9 : sm_left[i].sub ? 40 : 34;
        return R(m.x + 2, y, 188, !sm_left[id].label ? 9 : sm_left[id].sub ? 40 : 34);
    }
    if (id < 200) {
        int y = y0, k = id - 100;
        for (int i = 0; i < k; i++) y += !sm_right[i].label ? 9 : 26;
        return R(m.x + 192, y, m.w - 194, !sm_right[k].label ? 9 : 26);
    }
    if (id == 300) return R(m.x + 2, m.y + m.h - SM_FOOT - 36, 188, 30);
    if (T.renderer == RENDER_PLEX && (id == 400 || id == 401)) return R(m.x + m.w - (id == 400 ? 76 : 42), m.y + m.h - SM_FOOT + 7, 30, 30);
    if (id == 400 || id == 401) {             /* sized from their labels, right-aligned */
        int w1 = MIN(36 + text_width(FONT_UI, "Turn Off Computer"), m.w / 2 - 8);
        int w0 = MIN(36 + text_width(FONT_UI, "Log Off"), m.w - w1 - 20);
        int x1 = m.x + m.w - 6 - w1;
        if (id == 401) return R(x1, m.y + m.h - SM_FOOT + 8, w1, 28);
        return R(x1 - 6 - w0, m.y + m.h - SM_FOOT + 8, w0, 28);
    }
    return R(0, 0, 0, 0);
}
/* classic menu entries */
static const sm_item_t classic_items[] = {
    { "Programs", NULL, ICON_FOLDER, "allprog" }, { "Documents", NULL, ICON_MY_DOCUMENTS, "explorer:" USER_DOCS },
    { "Settings", NULL, ICON_CONTROL_PANEL, "control" }, { "Search", NULL, ICON_SEARCH, "explorer:C:\\" },
    { "Help", NULL, ICON_HELP, "help" }, { "Run...", NULL, ICON_RUN, "run" },
    { NULL }, { "Log Off " "User" "...", NULL, ICON_LOGOFF, "logoff" }, { "Shut Down...", NULL, ICON_POWER, "shutdown" },
};
#define CL_N ((int)ARRAY_LEN(classic_items))
static rect_t cl_item_rect(int i) {
    rect_t m = sm_rect();
    int y = m.y + 3;
    for (int k = 0; k < i; k++) y += classic_items[k].label ? 31 : 8;
    return R(m.x + 24, y, m.w - 27, classic_items[i].label ? 31 : 8);
}
static int ap_width(void) {
    int w = 200;
    for (int i = 0; i < AP_N; i++) if (all_programs[i].label) w = MAX(w, 28 + text_width(FONT_UI, all_programs[i].label) + 16);
    return MIN(w, 420);
}
static rect_t ap_rect(void) {
    int h = 0; for (int i = 0; i < AP_N; i++) h += all_programs[i].label ? 24 : 7;
    h += 6;
    int aw = ap_width();
    rect_t m = sm_rect();
    if (T.classic) { rect_t r = cl_item_rect(0); int y = r.y; if (y + h > tb_y()) y = MAX(0, tb_y() - h); return R(m.x + m.w - 2, y, aw, h); }
    rect_t a = sm_item_rect(300);
    int y = MAX(0, a.y + a.h - h);
    return R(a.x + a.w + 6, y, aw, h);
}
static rect_t ap_item_rect(int i) {
    rect_t r = ap_rect(); int y = r.y + 3;
    for (int k = 0; k < i; k++) y += all_programs[k].label ? 24 : 7;
    return R(r.x + 2, y, r.w - 4, all_programs[i].label ? 24 : 7);
}
bool startmenu_is_open(void) { return startmenu_open; }
void startmenu_toggle(void) { startmenu_open = !startmenu_open; allprog_open = false; sm_hot = -1; wm_need_redraw = true; }
static void paint_allprog(canvas_t *c) {
    rect_t r = ap_rect();
    if (T.classic) { fill_rect(c, r.x, r.y, r.w, r.h, T.face); bevel_deep(c, r.x, r.y, r.w, r.h, true); }
    else { blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 50); fill_rect(c, r.x, r.y, r.w, r.h, WHITE); draw_rect(c, r.x, r.y, r.w, r.h, T.sm_border); vline(c, r.x + 1, r.y + 1, r.h - 2, lighten(T.sm_border, 150)); }
    for (int i = 0; i < AP_N; i++) {
        rect_t ir = ap_item_rect(i);
        if (!all_programs[i].label) { hline(c, ir.x + 4, ir.y + 3, ir.w - 8, HEX(0xC0C0C0)); continue; }
        bool hot = ap_hot == i;
        if (hot) fill_rect(c, ir.x, ir.y, ir.w, ir.h, T.hot);
        draw_icon(c, all_programs[i].icon, 16, ir.x + 6, ir.y + 4);
        draw_text_ellipsis(c, FONT_UI, ir.x + 28, ir.y + 5, ir.w - 32, all_programs[i].label, hot ? WHITE : BLACK);
    }
}
static void paint_rotated_text(canvas_t *c, int x, int ybottom, const char *s, int font, uint32_t col) {
    int w = text_width(font, s), h = font_height(font);
    canvas_t *tmp = canvas_new(w, h);
    gfx_clear(tmp, 0xFF000000);
    draw_text(tmp, font, 0, 0, s, 0xFFFFFFFF);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int a = C_R(tmp->px[j * w + i]);
        if (a) blend_pixel(c, x + j, ybottom - i, col, a);
    }
    canvas_free(tmp);
}
static void paint_startmenu(canvas_t *c) {
    if (!startmenu_open) return;
    rect_t m = sm_rect();
    if (T.classic) {
        fill_rect(c, m.x, m.y, m.w, m.h, T.face); bevel_deep(c, m.x, m.y, m.w, m.h, true);
        gradient_v(c, m.x + 3, m.y + 3, 21, m.h - 6, T.sm_head_top, T.sm_head_bot);
        paint_rotated_text(c, m.x + 3, m.y + m.h - 8, OS_NAME, FONT_LARGE_BOLD, WHITE);
        for (int i = 0; i < CL_N; i++) {
            rect_t r = cl_item_rect(i);
            if (!classic_items[i].label) { hline(c, r.x + 2, r.y + 3, r.w - 4, HEX(0x808080)); hline(c, r.x + 2, r.y + 4, r.w - 4, WHITE); continue; }
            bool hot = sm_hot == 500 + i;
            if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
            draw_icon(c, classic_items[i].icon, 24, r.x + 4, r.y + 4);
            char lbl[64]; if (i == 7) snprintf(lbl, sizeof lbl, "Log Off %s...", user_name); else strlcpy(lbl, classic_items[i].label, sizeof lbl);
            draw_text_ellipsis(c, FONT_UI, r.x + 36, r.y + (r.h - 14) / 2, r.w - 36 - (i == 0 ? 16 : 4), lbl, hot ? WHITE : BLACK);
            if (i == 0) draw_arrow(c, r.x + r.w - 10, r.y + r.h / 2, 3, hot ? WHITE : BLACK);
        }
        if (allprog_open) paint_allprog(c);
        return;
    }
    /* Luna two-column menu (Aero: the same layout on glass) */
    bool aero = T.renderer == RENDER_AERO;
    blend_rect(c, m.x + m.w, m.y + 6, 3, m.h - 6, BLACK, 60);
    if (aero) {
        draw_glass(c, m.x, m.y, m.w, m.h, T.sm_head_bot, 200, 8);
        for (int j = 0; j < 40; j++) blend_rect(c, m.x, m.y + j, m.w, 1, WHITE, 50 - j);
        draw_rect(c, m.x, m.y, m.w, m.h, T.sm_border);
        blend_rect(c, m.x + 1, m.y + 1, m.w - 2, 1, WHITE, 120);
    } else fill_round_rect(c, m.x, m.y, m.w, m.h, 7, T.sm_border);
    /* header */
    for (int j = 0; j < SM_HEAD && !aero; j++) {
        int in = 0, rr = 7; if (j < rr) { int dy = rr - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
        uint32_t col = j < 6 ? mix(lighten(T.sm_head_top, 80), T.sm_head_top, j * 42) : mix(T.sm_head_top, T.sm_head_bot, (j - 6) * 255 / (SM_HEAD - 6));
        hline(c, m.x + in + 1, m.y + j + 1, m.w - 2 * in - 2, col);
    }
    int px = m.x + 8, py = m.y + 8;
    fill_round_rect(c, px - 2, py - 2, 52, 52, 4, WHITE);
    fill_round_rect(c, px - 3, py - 3, 54, 54, 5, mix(WHITE, T.sm_head_top, 60));
    fill_round_rect(c, px - 2, py - 2, 52, 52, 4, WHITE);
    draw_user_picture(c, user_picture, 48, px, py);
    draw_text_ellipsis(c, FONT_LARGE_BOLD, m.x + 69, m.y + 20, m.w - 80, user_name, darken(T.sm_head_bot, 80));
    draw_text_ellipsis(c, FONT_LARGE_BOLD, m.x + 68, m.y + 19, m.w - 80, user_name, T.sm_head_text);
    /* accent line */
    if (!aero) {
        gradient_h(c, m.x + 1, m.y + SM_HEAD, m.w / 2, 2, T.sm_head_bot, T.sm_accent);
        gradient_h(c, m.x + m.w / 2, m.y + SM_HEAD, m.w / 2 - 1, 2, T.sm_accent, T.sm_head_bot);
    }
    /* body */
    int by = m.y + SM_HEAD + 2, bh = m.h - SM_HEAD - 2 - SM_FOOT;
    if (aero) {
        fill_round_rect(c, m.x + 6, by, 186, bh, 4, HEX(0x50607A));
        fill_round_rect(c, m.x + 7, by + 1, 184, bh - 2, 3, T.sm_left);
        blend_rect(c, m.x + 196, by, m.w - 202, bh, T.sm_right, 110);
    } else if (T.renderer == RENDER_PLEX) {
        gradient_v(c, m.x + 1, by, 190, bh, T.sm_left, darken(T.sm_left, 40));
        gradient_v(c, m.x + 191, by, m.w - 192, bh, lighten(T.sm_right, 60), darken(T.sm_right, 30));
        vline(c, m.x + 191, by, bh, T.sm_right_line);
    } else {
        fill_rect(c, m.x + 1, by, 190, bh, T.sm_left);
        fill_rect(c, m.x + 191, by, m.w - 192, bh, T.sm_right);
        vline(c, m.x + 191, by, bh, T.sm_right_line);
    }
    for (int i = 0; i < SM_LEFT_N; i++) {
        rect_t r = sm_item_rect(i);
        const sm_item_t *it = &sm_left[i];
        uint32_t sep = mix(T.sm_left, T.sm_left_text, 50);
        if (!it->label) { gradient_h(c, r.x + 8, r.y + 4, r.w / 2 - 8, 1, T.sm_left, sep); gradient_h(c, r.x + r.w / 2, r.y + 4, r.w / 2 - 8, 1, sep, T.sm_left); continue; }
        bool hot = sm_hot == i;
        if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
        draw_icon(c, it->icon, 32, r.x + 5, r.y + (r.h - 32) / 2);
        if (it->sub) {
            draw_text_ellipsis(c, FONT_UI_BOLD, r.x + 43, r.y + 6, r.w - 47, it->label, hot ? WHITE : T.sm_left_text);
            draw_text_ellipsis(c, FONT_UI, r.x + 43, r.y + 21, r.w - 47, it->sub, hot ? WHITE : mix(T.sm_left_text, T.sm_left, 110));
        } else draw_text_ellipsis(c, FONT_UI, r.x + 43, r.y + (r.h - 14) / 2, r.w - 47, it->label, hot ? WHITE : T.sm_left_text);
    }
    /* all programs */
    {
        rect_t r = sm_item_rect(300);
        uint32_t sep = mix(T.sm_left, T.sm_left_text, 50);
        gradient_h(c, r.x + 8, r.y - 5, r.w / 2 - 8, 1, T.sm_left, sep); gradient_h(c, r.x + r.w / 2, r.y - 5, r.w / 2 - 8, 1, sep, T.sm_left);
        bool hot = sm_hot == 300 || allprog_open;
        if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
        int tw = text_width(FONT_UI_BOLD, "All Programs");
        draw_text(c, FONT_UI_BOLD, r.x + (r.w - tw - 24) / 2, r.y + 8, "All Programs", hot ? WHITE : T.sm_left_text);
        draw_icon(c, ICON_GO, 16, r.x + (r.w + tw - 24) / 2 + 6, r.y + 7);
    }
    for (int i = 0; i < SM_RIGHT_N; i++) {
        rect_t r = sm_item_rect(100 + i);
        const sm_item_t *it = &sm_right[i];
        if (!it->label) { gradient_h(c, r.x + 8, r.y + 4, r.w - 16, 1, T.sm_right, T.sm_right_line); continue; }
        bool hot = sm_hot == 100 + i;
        if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
        draw_icon(c, it->icon, 24, r.x + 5, r.y + 1);
        draw_text_ellipsis(c, it->bold ? FONT_UI_BOLD : FONT_UI, r.x + 35, r.y + 6, r.w - 39, it->label, hot ? WHITE : T.sm_right_text);
    }
    /* footer */
    int fy = m.y + m.h - SM_FOOT;
    for (int j = 0; j < SM_FOOT - 1 && !aero; j++) {
        int in = 0, rr = 7, jj = SM_FOOT - 2 - j; if (jj < rr) { int dy = rr - jj, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
        hline(c, m.x + 1 + in, fy + j, m.w - 2 - 2 * in, mix(T.sm_foot_top, T.sm_foot_bot, j * 255 / SM_FOOT));
    }
    const char *fl[2] = { "Log Off", "Turn Off Computer" }; int fi[2] = { ICON_LOGOFF, ICON_POWER };
    for (int k = 0; k < 2 && T.renderer == RENDER_PLEX; k++) {   /* round buttons */
        rect_t r = sm_item_rect(400 + k);
        int cx = r.x + 15, cy = r.y + 15;
        uint32_t base = k ? HEX(0xD8452A) : HEX(0xE8A030);
        if (sm_hot == 400 + k) base = lighten(base, 50);
        fill_circle(c, cx, cy, 13, darken(base, 80));
        fill_circle(c, cx, cy, 12, base);
        for (int j = -11; j < 0; j++) blend_rect(c, cx - 9, cy + j, 18, 1, WHITE, 90 + j * 6);
        draw_icon(c, fi[k], 16, cx - 8, cy - 8);
    }
    for (int k = 0; k < 2 && T.renderer != RENDER_PLEX; k++) {
        rect_t r = sm_item_rect(400 + k);
        if (sm_hot == 400 + k) { blend_rect(c, r.x, r.y, r.w, r.h, WHITE, 50); draw_rect(c, r.x, r.y, r.w, r.h, lighten(T.sm_foot_top, 100)); }
        draw_icon(c, fi[k], 24, r.x + 2, r.y + 2);
        draw_text_ellipsis(c, FONT_UI, r.x + 29, r.y + 7, r.w - 32, fl[k], T.sm_foot_text);
    }
    if (allprog_open) paint_allprog(c);
}
static void run_action(const char *a) {
    startmenu_open = false; allprog_open = false; wm_need_redraw = true;
    if (!strcmp(a, "run")) shell_show_run();
    else if (!strcmp(a, "logoff")) { extern void shell_logoff_dialog(void); shell_logoff_dialog(); }
    else if (!strcmp(a, "shutdown")) shell_show_shutdown();
    else if (!strncmp(a, "explorer:", 9)) explorer_open(a + 9);
    else if (!strncmp(a, "control:", 8)) control_open(a + 8);
    else if (!strncmp(a, "open:", 5)) shell_open(a + 5, NULL);
    else app_launch(a, NULL);
}

/* ---------------- shell input ---------------- */
static int sm_hit(int x, int y) {
    if (allprog_open) {
        rect_t ar = ap_rect();
        if (pt_in(ar, x, y)) { for (int i = 0; i < AP_N; i++) if (all_programs[i].label && pt_in(ap_item_rect(i), x, y)) return 600 + i; return -2; }
    }
    rect_t m = sm_rect();
    if (!pt_in(m, x, y)) return -1;
    if (T.classic) { for (int i = 0; i < CL_N; i++) if (classic_items[i].label && pt_in(cl_item_rect(i), x, y)) return 500 + i; return -2; }
    for (int i = 0; i < SM_LEFT_N; i++) if (sm_left[i].label && pt_in(sm_item_rect(i), x, y)) return i;
    for (int i = 0; i < SM_RIGHT_N; i++) if (sm_right[i].label && pt_in(sm_item_rect(100 + i), x, y)) return 100 + i;
    if (pt_in(sm_item_rect(300), x, y)) return 300;
    if (pt_in(sm_item_rect(400), x, y)) return 400;
    if (pt_in(sm_item_rect(401), x, y)) return 401;
    return -2;
}
static int tb_hit(int x, int y) {
    if (y < tb_y()) return -1;
    if (x >= SCREEN_W - 20) return 4000;                 /* sidebar toggle */
    if (x < start_w()) return 1000;
    for (int i = 0; i < QL_N; i++) if (pt_in(R(ql_x() + i * 24, tb_y() + (T.taskbar_h - 22) / 2, 22, 22), x, y)) return 2000 + i;
    window_t *list[32]; int n = task_btns(list, 32);
    for (int i = 0; i < n; i++) if (pt_in(task_btn_rect(i, n), x, y)) return i;
    if (x >= SCREEN_W - tray_w()) return 3000;
    return -2;
}
int shell_cursor(int x, int y) { (void)x; if (startmenu_open || y >= tb_y()) return CURSOR_ARROW; return -1; }
static void tb_ctx_cb(void *ctx, int id) {
    window_t *w = ctx;
    if (!wm_alive(w)) return;
    if (id == 1) wm_focus(w); else if (id == 2) wm_minimize(w); else if (id == 3) wm_maximize(w, true); else if (id == 4) wm_close(w);
}
static void tb_menu_cb(void *ctx, int id) {
    (void)ctx;
    if (id == 1) app_launch("taskmgr", NULL);
    else if (id == 2) control_open("taskbar");
    else if (id == 3) { for (int i = 0; i < wm_window_count(); i++) wm_minimize(wm_window_at(i)); }
    else if (id == 4) control_open("datetime");
}
bool shell_mouse(int type, int x, int y, int btn) {
    if (startmenu_open) {
        int h = sm_hit(x, y);
        if (type == ME_MOVE) {
            int nh = h >= 0 ? h : -1;
            if (nh != sm_hot) { sm_hot = nh; wm_need_redraw = true; }
            if (h == 300 || h == 500) { if (!allprog_open) { allprog_open = true; wm_need_redraw = true; } }
            else if (h >= 0 && h < 600 && allprog_open) { allprog_open = false; wm_need_redraw = true; }
            if (h >= 600) { if (ap_hot != h - 600) { ap_hot = h - 600; wm_need_redraw = true; } } else if (ap_hot != -1) { ap_hot = -1; wm_need_redraw = true; }
            return h != -1 || y >= tb_y();
        }
        if (type == ME_DOWN) {
            if (h == -1) {
                startmenu_open = false; allprog_open = false; wm_need_redraw = true;
                if (tb_hit(x, y) == 1000) return true;
                return y >= tb_y();
            }
            return true;
        }
        if (type == ME_UP && (btn & MB_LEFT)) {
            if (h >= 600) run_action(all_programs[h - 600].action);
            else if (h >= 500) { const char *a = classic_items[h - 500].action; if (!strcmp(a, "allprog")) { allprog_open = true; wm_need_redraw = true; } else run_action(a); }
            else if (h == 300) { allprog_open = !allprog_open; wm_need_redraw = true; }
            else if (h == 400) run_action("logoff");
            else if (h == 401) run_action("shutdown");
            else if (h >= 100) run_action(sm_right[h - 100].action);
            else if (h >= 0) run_action(sm_left[h].action);
            return true;
        }
        return true;
    }
    if (sidebar_open && x >= SCREEN_W - SIDE_W && y < tb_y()) {
        int sh = side_hit(x, y);
        if (type == ME_MOVE) { if (sh != side_hot) { side_hot = sh; wm_need_redraw = true; } return true; }
        if (type == ME_DOWN && (btn & MB_LEFT)) { side_click(sh); return true; }
        return true;
    }
    if (side_hot != -1 && (!sidebar_open || x < SCREEN_W - SIDE_W || y >= tb_y())) { side_hot = -1; wm_need_redraw = true; }
    if (side_search_focus && type == ME_DOWN) { side_search_focus = false; wm_need_redraw = true; }
    int h = tb_hit(x, y);
    if (type == ME_MOVE) { int nh = h >= 0 ? h : -1; if (nh != tb_hot) { tb_hot = nh; wm_need_redraw = true; } return h != -1; }
    if (h == -1) return false;
    if (type == ME_DOWN && (btn & MB_LEFT)) {
        if (h == 4000) { sidebar_open = !sidebar_open; side_search_focus = false; wm_need_redraw = true; }
        else if (h == 1000) startmenu_toggle();
        else if (h >= 2000 && h < 3000) {
            const char *a = quicklaunch[h - 2000].app;
            if (!strcmp(a, "showdesktop")) { for (int i = 0; i < wm_window_count(); i++) wm_minimize(wm_window_at(i)); }
            else app_launch(a, NULL);
        } else if (h >= 0 && h < 1000) {
            window_t *list[32]; int n = task_btns(list, 32);
            if (h < n) { window_t *w = list[h]; if (w == wm_active && w->state != WS_MINIMIZED) wm_minimize(w); else wm_focus(w); }
        } else if (h == 3000) {
            int sx, nx; tray_icon_x(&sx, &nx);
            if (x >= sx - 4 && x < nx - 2) control_open("sound");
            else if (x >= nx - 2 && x < nx + 20) control_open("network");
            else {
                rtc_time_t t; rtc_now(&t);
                static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
                static const char *mons[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
                char b[80]; snprintf(b, sizeof b, "%s, %s %d, %d", days[t.weekday], mons[(t.month + 11) % 12], t.day, t.year);
                notify_balloon("Date", b);
            }
        }
        return true;
    }
    if (type == ME_DOWN && (btn & MB_RIGHT)) {
        if (h >= 0 && h < 1000) {
            window_t *list[32]; int n = task_btns(list, 32);
            if (h < n) { static const menu_item_t m[] = { { "Restore", 1, 0 }, { "Minimize", 2, 0 }, { "Maximize", 3, 0 }, { "-", 0, 0 }, { "Close", 4, 0 } }; popup_menu(x, y - 120, m, 5, tb_ctx_cb, list[h]); }
        } else {
            static const menu_item_t m[] = { { "Adjust Date/Time", 4, 0 }, { "-", 0, 0 }, { "Show the Desktop", 3, 0 }, { "Task Manager", 1, 0 }, { "-", 0, 0 }, { "Properties", 2, 0 } };
            popup_menu(x, y - 130, m, 6, tb_menu_cb, NULL);
        }
        return true;
    }
    return true;
}
bool shell_key(key_event_t *e) {
    if (!e->pressed) return false;
    if (sidebar_open && side_search_focus) {
        if (e->key == KEY_ESC) { side_search_focus = false; wm_need_redraw = true; return true; }
        if (e->key == KEY_BACKSPACE) { int l = strlen(side_query); if (l) side_query[l - 1] = 0; wm_need_redraw = true; return true; }
        if (e->key == KEY_ENTER) {
            side_search_run();
            if (side_res_n) side_run_result(0);
            wm_need_redraw = true; return true;
        }
        if (e->ch >= 32 && e->ch < 127) {
            int l = strlen(side_query);
            if (l < (int)sizeof side_query - 1) { side_query[l] = e->ch; side_query[l + 1] = 0; wm_need_redraw = true; }
            return true;
        }
        return true;
    }
    if (e->key == KEY_WIN || (e->key == KEY_ESC && (e->mods & MOD_CTRL))) { startmenu_toggle(); return true; }
    if (startmenu_open) {
        if (e->key == KEY_ESC) { startmenu_open = false; allprog_open = false; wm_need_redraw = true; }
        return true;
    }
    if (!wm_active && (e->key == KEY_ENTER) && dsel >= 0) { dicon_open(dsel); return true; }
    if (!wm_active && e->key == KEY_DELETE && dsel >= 0 && dicons[dsel].node) { desk_ctx_cb((void *)(uintptr_t)dsel, 2); return true; }
    if (!wm_active && e->key == KEY_F2 && dsel >= 0 && dicons[dsel].node) { desk_ctx_cb((void *)(uintptr_t)dsel, 3); return true; }
    if (!wm_active && e->key == KEY_F5) { desktop_refresh(); return true; }
    return false;
}
static char last_clock[32];
void shell_tick(void) {
    side_sample();
    char b[32]; fmt_clock(b, sizeof b);
    { extern uint32_t gui_composes; static uint32_t lastc, lastt;
      uint32_t now = timer_ticks();
      if (!lastt) { lastt = now; lastc = gui_composes; }
      else if (now - lastt >= 1000) {
          uint32_t n = gui_composes - lastc;
          if (n > 20) usb_diag("gui: %u composes/s", n);
          lastc = gui_composes; lastt = now;
      } }
    if (strcmp(b, last_clock)) { strcpy(last_clock, b); wm_need_redraw = true; }
    if (balloon_until && (int32_t)(timer_ticks() - balloon_until) > 0) { balloon_until = 0; wm_need_redraw = true; }
}
void shell_paint_overlays(canvas_t *c) {
    paint_balloon(c);
    paint_startmenu(c);
}
void shell_init(void) {
    shell_wallpaper_changed();
    desktop_refresh();
}
