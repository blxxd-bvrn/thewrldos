/* Desktop shell: wallpaper, icons, taskbar, start menu, tray, boot & welcome screens */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "raster.h"
#include "pkg.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "fs.h"
#include "net.h"
#include "usb.h"
#include "animation.h"

int wallpaper_id = 0;
int wallpaper_fit = 0;                        /* 0 crop, 1 scale, 2 stretch */
uint32_t desktop_color = HEX(0x3A6EA5);
char user_name[32] = "User";
char user_password[32];                        /* hash; empty = no password */
char computer_name[32] = "WRLD-PC";
int user_picture = 0;
int user_picture_count = 71;                  /* the photographs in C:\WRLD\Avatars */
bool show_clock = true, show_seconds = false, clock_24h = false;
static canvas_t *wallpaper;
static int tb_y(void);
static bool startmenu_open;
static int sm_hot = -1, tb_hot = -1;
int startmenu_animation = MENU_ANIM_SLIDE;
bool sidebar_slide = true, sidebar_tile_fade = true;
static animation_t sm_motion;
static int sm_progress;
static bool sm_ignore_up;
static uint32_t shell_anim_frame;
static canvas_t *sm_under;

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
    CUSTOM_WALLPAPER,                                    /* "Your picture": Custom desktop... in Personalization */
};
const char *wallpaper_names[] = { "Blue shoal", "Home planet", "We carry the world", "Bubble city",
                                  "Kelp forest", "Northern lights", "Aurora sky", "City lights",
                                  "Summer leaves", "Your picture", "(None)" };
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
static int wall_gen;                                     /* bumped when the custom picture changes */
/* a picture chosen with "Custom desktop...": copied to CUSTOM_WALLPAPER (so it stays when the
 * original moves) and shown; false with a reason if it is not a picture this OS can read */
bool wallpaper_set_custom(const char *path, char *err, int cap) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir || !n->size || !n->data) { snprintf(err, cap, "The file could not be read."); return false; }
    canvas_t *test = image_decode(n->data, n->size, err, cap);
    if (!test) return false;
    canvas_free(test);
    vfs_mkdir_p("C:\\WRLD\\Custom");
    if (!vfs_write_file(CUSTOM_WALLPAPER, n->data, n->size)) { snprintf(err, cap, "There is no room to keep a copy of the picture."); return false; }
    if (wall_src && wall_src_id == WALLPAPER_CUSTOM) { canvas_free(wall_src); wall_src = NULL; wall_src_id = -1; }
    wall_out_id = -1;
    wallpaper_id = WALLPAPER_CUSTOM;
    wall_gen++;
    shell_wallpaper_changed();
    return true;
}
bool wallpaper_custom_exists(void) { vnode_t *n = vfs_resolve(CUSTOM_WALLPAPER, NULL); return n && !n->dir && n->size; }
void wallpaper_render(canvas_t *c, int id) {
    if (id < 0 || id >= wallpaper_count - 1) { gfx_clear(c, desktop_color); return; }
    if (!c) return;
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
    static int last_id = -1, last_w, last_h, last_fit = -1, last_gen; static uint32_t last_col;
    if (wallpaper && (wallpaper->w != SCREEN_W || wallpaper->h != SCREEN_H)) { canvas_free(wallpaper); wallpaper = NULL; last_id = -1; }
    if (last_id == wallpaper_id && last_fit == wallpaper_fit && last_w == SCREEN_W && last_h == SCREEN_H && last_gen == wall_gen && (wallpaper_id != wallpaper_count - 1 || last_col == desktop_color)) return;
    /* a plain colour (chosen, or a picture that cannot be read or does not fit in memory) needs
     * no full-screen canvas: desktop_paint fills the colour itself */
    bool picture = wallpaper_id >= 0 && wallpaper_id < wallpaper_count - 1 && wall_source(wallpaper_id);
    if (!picture) { canvas_free(wallpaper); wallpaper = NULL; canvas_free(wall_out); wall_out = NULL; }
    else {
        if (!wallpaper) wallpaper = canvas_new(SCREEN_W, SCREEN_H);
        wallpaper_render(wallpaper, wallpaper_id);
    }
    last_gen = wall_gen; last_id = wallpaper_id; last_col = desktop_color; last_fit = wallpaper_fit; last_w = SCREEN_W; last_h = SCREEN_H;
    wm_need_redraw = true;
}

/* ---------------- account pictures ----------------
 * The 71 photographs in C:\WRLD\Avatars are decoded once into 96x96 cached
 * thumbnails; draw_user_picture() masks one to a circle at the size asked
 * for, so the log-on tile, the Start menu and the account page all use it. */
#define AVATAR_CACHE 128
static canvas_t *avatar_cache[72];                      /* the 71 photographs and the custom picture */
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
    if (id < 0 || id > USER_PICTURE_CUSTOM) id = 0;
    if (avatar_cache[id]) return avatar_cache[id];
    char path[80];
    if (id == USER_PICTURE_CUSTOM) strlcpy(path, CUSTOM_AVATAR, sizeof path);
    else snprintf(path, sizeof path, "C:\\WRLD\\Avatars\\Avatar %02d.jpg", id + 1);
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
/* "Custom avatar...": the picture is copied to CUSTOM_AVATAR and becomes the account picture */
bool user_picture_set_custom(const char *path, char *err, int cap) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir || !n->size || !n->data) { snprintf(err, cap, "The file could not be read."); return false; }
    canvas_t *test = image_decode(n->data, n->size, err, cap);
    if (!test) return false;
    bool tiny = test->w < 2 || test->h < 2;
    canvas_free(test);
    if (tiny) { snprintf(err, cap, "The picture is too small."); return false; }
    vfs_mkdir_p("C:\\WRLD\\Custom");
    if (!vfs_write_file(CUSTOM_AVATAR, n->data, n->size)) { snprintf(err, cap, "There is no room to keep a copy of the picture."); return false; }
    if (avatar_cache[USER_PICTURE_CUSTOM]) { canvas_free(avatar_cache[USER_PICTURE_CUSTOM]); avatar_cache[USER_PICTURE_CUSTOM] = NULL; }
    user_picture = USER_PICTURE_CUSTOM;
    wm_need_redraw = true;
    return true;
}
bool user_picture_custom_exists(void) { vnode_t *n = vfs_resolve(CUSTOM_AVATAR, NULL); return n && !n->dir && n->size; }
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
#define SIDE_TILE_HEAD 20
#define SIDE_TILE_GAP 6
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
static animation_t side_motion, side_fade[ST_N];
static int side_progress, side_alpha[ST_N];
static bool side_seen_open;
static canvas_t *side_canvas, *side_under;

static int side_x(void) { return SCREEN_W - (sidebar_slide && wm_animations ? SIDE_W * side_progress / 255 : SIDE_W); }
static bool side_tile_visible(int t) { return side_alpha[t] > 0 || (sidebar_open && (sidebar_tiles & TBIT(t))); }
void shell_effects_changed(void) {
    animation_set(&sm_motion, startmenu_open ? 255 : 0); sm_progress = sm_motion.to;
    animation_set(&side_motion, sidebar_open ? 255 : 0); side_progress = side_motion.to;
    side_seen_open = sidebar_open; sm_ignore_up = false;
    for (int t = 0; t < ST_N; t++) {
        animation_set(&side_fade[t], sidebar_open && (sidebar_tiles & TBIT(t)) ? 255 : 0);
        side_alpha[t] = side_fade[t].to;
    }
    shell_anim_frame = 0; wm_need_redraw = true;
}
void shell_animate(uint32_t now) {
    if (now - shell_anim_frame < 16) return;
    shell_anim_frame = now;
    if (side_seen_open != sidebar_open) {
        side_seen_open = sidebar_open;
        animation_start(&side_motion, sidebar_open ? 255 : 0, now, wm_animations ? (sidebar_slide ? 240 : sidebar_tile_fade ? 180 : 0) : 0);
    }
    int val = animation_value(&side_motion, now);
    if (!wm_animations) { animation_set(&side_motion, sidebar_open ? 255 : 0); val = side_motion.to; }
    if (side_progress != val) { side_progress = val; wm_need_redraw = true; }
    for (int t = 0; t < ST_N; t++) {
        int target = sidebar_open && (sidebar_tiles & TBIT(t)) ? 255 : 0;
        if (side_fade[t].to != target) animation_start(&side_fade[t], target, now, wm_animations && sidebar_tile_fade ? 180 : 0);
        if (!wm_animations || !sidebar_tile_fade) animation_set(&side_fade[t], target);
        val = animation_value(&side_fade[t], now);
        if (side_alpha[t] != val) { side_alpha[t] = val; wm_need_redraw = true; }
    }
    if (!wm_animations || startmenu_animation == MENU_ANIM_NONE) animation_set(&sm_motion, startmenu_open ? 255 : 0);
    val = animation_value(&sm_motion, now);
    if (sm_progress != val) { sm_progress = val; wm_need_redraw = true; }
}

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
    if (T.classic) { *top = *bot = T.face; *text = T.text; *dim = mix(T.text, T.face, 80); *hdr1 = T.face_light; *hdr2 = T.face; }
    else {
        /* the taskbar's own Luna blue, so the panel belongs to the same
         * surface rather than reading as a dark slab */
        *top = T.tb_hi; *bot = T.tb_mid;
        *text = theme_text_on(mix(*top, *bot, 160)); *dim = mix(*text, *bot, 65);
        *hdr1 = T.title_top; *hdr2 = T.title_bot;
    }
}
static uint32_t side_bg_at(int y, int h) {
    if (T.renderer == RENDER_AERO) return mix(T.tb_mid, T.glass, 100);
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
            if (alpha) {
                /* Aero: fade into the glass pixels already on the sidebar so the
                 * photo stays translucent instead of ending on a solid strip. */
                uint32_t under = T.renderer == RENDER_AERO ? get_pixel(c, x + xx, y + yy) : fade;
                col = mix(col, under, alpha);
            }
            put_pixel(c, x + xx, y + yy, col);
        }
    }
}
static void side_tile_header(canvas_t *c, int x, int y, int w, int t, int bh) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    int h = SIDE_TILE_HEAD + bh + 2;
    uint32_t edge = T.classic ? T.face_dark : mix(T.title_bot, BLACK, dark_mode ? 65 : 85);
    if (T.classic) {
        fill_rect(c, x, y, w, h, T.face); bevel(c, x, y, w, h, true);
        bevel(c, x + 2, y + SIDE_TILE_HEAD, w - 4, bh, false);
        text = T.text; dim = T.gray_text;
    }
    else if (T.renderer == RENDER_AERO) {
        /* Distinct glazed tile bodies, not opaque cards over the Aero glass. */
        blend_rect(c, x + 1, y + 1, w - 2, h - 2, T.glass, dark_mode ? 60 : 35);
        blend_rect(c, x + 1, y + SIDE_TILE_HEAD, w - 2, bh + 1, WHITE, dark_mode ? 12 : 30);
        blend_rect(c, x + 1, y + 1, w - 2, SIDE_TILE_HEAD - 1, T.tb_mid, 180);
        text = WHITE; dim = HEX(0xCFDAE8);
    } else {
        blend_rect(c, x + 2, y + 2, w, h, BLACK, 30);
        fill_rect(c, x, y, w, h, T.pane_body);
        gradient_v(c, x + 1, y + 1, w - 2, SIDE_TILE_HEAD - 1, h1, h2);
        text = theme_text_on(mix(h1, h2, 128)); dim = mix(text, mix(h1, h2, 128), 60);
    }
    draw_rect(c, x, y, w, h, edge);
    hline(c, x + 1, y + SIDE_TILE_HEAD - 1, w - 2, edge);
    blend_rect(c, x + 1, y + 1, w - 2, 1, WHITE, dark_mode ? 30 : 90);
    vline(c, x + 1, y + SIDE_TILE_HEAD, bh, T.renderer == RENDER_AERO ? mix(T.glass, WHITE, 70) : T.face_light);
    draw_icon(c, side_tile_icon[t], 12, x + 5, y + 4);
    draw_text_ellipsis(c, FONT_UI_BOLD, x + 22, y + 3, w - 50, side_tile_name[t], text);
    for (int gy = 0; gy < 3; gy++) for (int gx = 0; gx < 2; gx++) fill_rect(c, x + w - 29 + gx * 4, y + 6 + gy * 4, 2, 2, mix(text, h2, 90));   /* grip: drag me */
    if (side_hot == 10 + t) fill_rect(c, x + w - 17, y + 3, 13, 13, mix(h2, text, 65));
    draw_text(c, FONT_UI, x + w - 13, y + 2, "x", side_hot == 10 + t ? text : dim);
}
static void side_bar(canvas_t *c, int x, int y, int w, int pct, uint32_t col, const char *label) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    dim = T.renderer == RENDER_AERO ? HEX(0xCFDAE8) : mix(T.text, T.pane_body, 70);
    draw_text(c, FONT_SMALL, x, y, label, dim);
    int by = y + 12;
    fill_round_rect(c, x, by, w, 5, 2, T.renderer == RENDER_AERO ? mix(bot, BLACK, 60) : T.face_dark);
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
            if (n == id) { sidebar_tiles |= TBIT(t); shell_anim_frame = 0; wm_need_redraw = true; return; }
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
    shell_anim_frame = 0;
    wm_need_redraw = true;
}
/* ---- the tiles' order, and dragging a tile by its title bar to a new place ---- */
static int side_order[ST_N] = { 0, 1, 2, 3, 4, 5, 6 };
static int side_ty[ST_N] = { -1, -1, -1, -1, -1, -1, -1 };      /* where each tile is drawn now */
static struct { bool pending, active, settling; int t, y, y0, off; } side_drag;
static int side_x(void);
static int tb_y(void);
/* during a drag only the sidebar column changes: repaint just that */
static void side_redraw(void) { wm_redraw_rect(R(side_x(), 0, SIDE_W, tb_y())); }
static void side_order_load(void) {
    static bool done;
    if (done) return;
    done = true;
    const char *v = settings_get("side_order");
    if (!v || !*v) return;
    int order[ST_N], n = 0; bool used[ST_N] = { false };
    for (const char *p = v; *p && n < ST_N; ) {
        int t = atoi(p);
        if (t >= 0 && t < ST_N && !used[t]) { order[n++] = t; used[t] = true; }
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
    for (int t = 0; t < ST_N; t++) if (!used[t]) order[n++] = t;   /* tiles a newer version added */
    memcpy(side_order, order, sizeof order);
}
static void side_order_save(void) {
    char b[32]; int o = 0;
    for (int k = 0; k < ST_N; k++) o += snprintf(b + o, sizeof b - o, "%s%d", k ? "," : "", side_order[k]);
    settings_set("side_order", b); settings_save();
}
/* while dragging: the position among the other visible tiles where the tile would land */
static int side_drop_index(void) {
    if (!side_drag.active) return -1;
    int by = 6 + SIDE_HDR + 8, slot = 0;
    int dh = SIDE_TILE_HEAD + side_body_h(side_drag.t) + 2;
    int mid = side_drag.y - side_drag.off + dh / 2;
    for (int k = 0; k < ST_N; k++) {
        int t = side_order[k];
        if (!side_tile_visible(t) || t == side_drag.t) continue;
        int th = SIDE_TILE_HEAD + side_body_h(t) + 2;
        if (mid < by + th / 2) return slot;
        by += th + SIDE_TILE_GAP; slot++;
    }
    return slot;
}
/* let go: the tile moves to the place shown */
static void side_drop(void) {
    int drop = side_drop_index();
    int t = side_drag.t, vis[ST_N], nv = 0;
    for (int k = 0; k < ST_N; k++) if (side_order[k] != t && side_tile_visible(side_order[k])) vis[nv++] = side_order[k];
    /* the visible tiles in their new order, then the hidden ones as they were */
    int out[ST_N], n = 0;
    for (int i = 0; i <= nv; i++) { if (i == drop) out[n++] = t; if (i < nv) out[n++] = vis[i]; }
    for (int k = 0; k < ST_N; k++) { int x = side_order[k]; bool in = false; for (int i = 0; i < n; i++) if (out[i] == x) in = true; if (!in) out[n++] = x; }
    memcpy(side_order, out, sizeof out);
    side_ty[t] = side_drag.y - side_drag.off;                /* it settles from where it was let go */
    side_order_save();
}
/* one tile: its header and its body, at (ix, by), iw wide */
static void side_paint_tile(canvas_t *c, int t, int ix, int by, int iw) {
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    int bh = side_body_h(t);
    side_tile_header(c, ix, by, iw, t, bh);
    int bx = ix + 2, bw = iw - 4, yy = by + SIDE_TILE_HEAD;
    uint32_t body_text = T.renderer == RENDER_AERO ? WHITE : T.text;
    uint32_t body_dim = T.renderer == RENDER_AERO ? HEX(0xCFDAE8) : mix(T.text, T.pane_body, 70);
    text = body_text; dim = body_dim;
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
            if (side_hot == 100 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.select);
            draw_icon(c, app_table[i].icon, 16, bx + 5, ry);
            draw_text_ellipsis(c, FONT_UI, bx + 25, ry + 1, bw - 30, app_table[i].title, side_hot == 100 + i ? T.select_text : text);
        }
    } else if (t == ST_SEARCH) {
        int fw = bw - 8, fy = yy + 3;
        fill_round_rect(c, bx + 4, fy, fw, 20, 3, T.window_bg);
        draw_rect(c, bx + 4, fy, fw, 20, side_search_focus ? T.btn_focus : T.face_dark);
        draw_text_ellipsis(c, FONT_UI, bx + 9, fy + 3, fw - 12, side_query[0] ? side_query : "Search everything...", side_query[0] ? T.text : T.gray_text);
        for (int i = 0; i < SIDE_RESULTS; i++) {
            int ry = yy + 27 + i * 16;
            if (side_query[0]) {
                if (i < side_res_n) {
                    if (side_hot == 401 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.select);
                    draw_icon(c, side_res[i].icon, 16, bx + 5, ry);
                    draw_text_ellipsis(c, FONT_UI, bx + 25, ry + 1, bw - 30, side_res[i].label, side_hot == 401 + i ? T.select_text : text);
                }
            } else if (i == 0) draw_text_ellipsis(c, FONT_SMALL, bx + 6, ry, bw - 10, "Apps, files and settings", dim);
            else if (i == 1) draw_text_ellipsis(c, FONT_SMALL, bx + 6, ry, bw - 10, "Enter: The WRLD Search", dim);
        }
        if (side_query[0] && !side_res_n) draw_text_ellipsis(c, FONT_SMALL, bx + 6, yy + 27, bw - 10, "No matches", dim);
    } else if (t == ST_SHORTCUTS) {
        for (int i = 0; i < SIDE_SHORTCUTS; i++) {
            int ry = yy + i * 16;
            if (side_hot == 200 + i) fill_rect(c, bx + 1, ry, bw - 2, 16, T.select);
            draw_icon(c, side_short[i].icon, 16, bx + 5, ry);
            draw_text(c, FONT_UI, bx + 25, ry + 1, side_short[i].name, side_hot == 200 + i ? T.select_text : text);
        }
    } else if (t == ST_QUICK) {
        for (int i = 0; i < SIDE_QUICK; i++) {
            int qx = bx + 6 + i * 26, qy = yy + 4;
            if (side_hot == 300 + i) fill_round_rect(c, qx - 2, qy - 2, 24, 24, 3, T.select);
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
}
/* While a tile is held the sidebar's background (the Aero glass blur, the gradients, the user banner)
 * cannot change, so it is drawn once and reused for every frame of the drag. */
static canvas_t *side_bg_cache;
static bool side_bg_valid;
static void side_paint_background(canvas_t *c, int h);
static void side_paint_tiles(canvas_t *c, int ix, int iw, int by, int h, uint32_t top, uint32_t bot, uint32_t text, uint32_t dim, uint32_t h1, uint32_t h2);
static void paint_sidebar_panel(canvas_t *c) {
    side_search_run();
    int x = 0, h = c->h;
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    if (!side_drag.pending) side_bg_valid = false;
    if (side_drag.pending && side_bg_valid && side_bg_cache && side_bg_cache->h == h) blit(c, 0, 0, side_bg_cache, 0, 0, SIDE_W, h);
    else {
        side_paint_background(c, h);
        if (side_drag.pending) {
            if (!side_bg_cache) side_bg_cache = canvas_new(SIDE_W, h); else canvas_resize(side_bg_cache, SIDE_W, h);
            if (side_bg_cache) { blit(side_bg_cache, 0, 0, c, 0, 0, SIDE_W, h); side_bg_valid = true; }
        }
    }
    int ix = x + 6, iw = SIDE_W - 12, by = 6;
    uint32_t banner_bg = side_bg_at(by + SIDE_HDR, h), banner_text = theme_text_on(banner_bg);
    draw_text(c, FONT_SMALL, ix + 6, by + SIDE_HDR - 31, "welcome", mix(banner_text, banner_bg, 50));
    draw_text_ellipsis(c, FONT_LARGE_BOLD, ix + 6, by + SIDE_HDR - 20, iw - 12, user_name, banner_text);
    by += SIDE_HDR + 8;
    side_paint_tiles(c, ix, iw, by, h, top, bot, text, dim, h1, h2);
}
static void side_paint_background(canvas_t *c, int h) {
    int x = 0;
    uint32_t top, bot, text, dim, h1, h2; side_colors(&top, &bot, &text, &dim, &h1, &h2);
    if (T.classic) {
        fill_rect(c, x, 0, SIDE_W, h, T.face);
        vline(c, x, 0, h, T.face_dark); vline(c, x + 1, 0, h, T.face_light);
    } else if (T.renderer == RENDER_AERO) {
        /* Aero: blurred wallpaper under a blue glass tint, like the window
         * frames, then a light darkening so the tile text keeps its contrast. */
        int gl = (C_R(T.glass) * 30 + C_G(T.glass) * 59 + C_B(T.glass) * 11) / 100;
        int extra = gl < 100 ? 20 : 65;
        draw_glass(c, x, 0, SIDE_W, h, T.glass, MAX(30, T.glass_alpha - 20), T.glass_blur);
        for (int j = 0; j < h; j++) blend_rect(c, x, j, SIDE_W, 1, T.tb_mid, extra + (gl < 100 ? 15 : 35) * j / MAX(1, h));
        blend_rect(c, x, 0, SIDE_W, 28, WHITE, 18);
        vline(c, x, 0, h, mix(T.frame_out, WHITE, 35));
        blend_rect(c, x + 1, 0, 1, h, WHITE, 80);
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
}
/* While a tile is held the tiles' contents do not change either: each is drawn once, kept, and
 * copied to wherever it is for the rest of the drag (text and icons are the costly part). */
static canvas_t *side_tile_img[ST_N];
static bool side_tile_img_ok[ST_N];
static void side_paint_tile_cached(canvas_t *c, int t, int x, int y, int iw) {
    int th = SIDE_TILE_HEAD + side_body_h(t) + 2;
    if (!side_drag.active) { side_tile_img_ok[t] = false; side_paint_tile(c, t, x, y, iw); return; }
    if (side_tile_img_ok[t] && side_tile_img[t] && side_tile_img[t]->h == th) { blit(c, x, y, side_tile_img[t], 0, 0, iw, th); return; }
    side_paint_tile(c, t, x, y, iw);
    if (x < 0 || y < 0 || x + iw > c->w || y + th > c->h) return;      /* only a tile drawn whole is kept */
    if (!side_tile_img[t]) side_tile_img[t] = canvas_new(iw, th); else canvas_resize(side_tile_img[t], iw, th);
    if (side_tile_img[t]) { blit(side_tile_img[t], 0, 0, c, x, y, iw, th); side_tile_img_ok[t] = true; }
}
static void side_paint_tiles(canvas_t *c, int ix, int iw, int by, int h, uint32_t top, uint32_t bot, uint32_t text, uint32_t dim, uint32_t h1, uint32_t h2) {
    (void)h1; (void)h2;
    if (!side_drag.active) for (int t = 0; t < ST_N; t++) side_tile_img_ok[t] = false;
    /* the tiles in their order; while one is dragged it floats under the pointer, the others
     * slide apart to leave room where it would land, and slide back when it is let go */
    side_order_load();
    int slot = 0, gap_y = -1, drop = side_drop_index();
    int dh = side_drag.active ? SIDE_TILE_HEAD + side_body_h(side_drag.t) + 2 : 0;
    for (int k = 0; k < ST_N; k++) {
        int t = side_order[k];
        if (!side_tile_visible(t)) continue;
        if (side_drag.active && t == side_drag.t) continue;
        if (side_drag.active && slot == drop) { gap_y = by; by += dh + SIDE_TILE_GAP; }
        slot++;
        int bh = side_body_h(t);
        int tileh = SIDE_TILE_HEAD + bh + 2;
        /* ease towards the place in the list */
        if (side_ty[t] < 0 || !side_drag.settling) side_ty[t] = by;
        else if (side_ty[t] != by) { int d = by - side_ty[t]; side_ty[t] += d / 3 ? d / 3 : d; side_redraw(); }
        int ty = side_ty[t];
        if (!side_under) side_under = canvas_new(iw + 2, SIDE_TILE_HEAD + side_body_h(ST_SEARCH) + 5);
        if (side_alpha[t] < 255) blit(side_under, 0, 0, c, ix, ty, iw + 2, tileh + 2);
        if (side_alpha[t] < 255) side_paint_tile(c, t, ix, ty, iw);
        else side_paint_tile_cached(c, t, ix, ty, iw);
        if (side_alpha[t] < 255 && side_under) {
            canvas_t under = *side_under; under.h = tileh + 2;
            blit_alpha(c, ix, ty, &under, 255 - side_alpha[t]);
        }
        by += tileh + SIDE_TILE_GAP;
    }
    if (side_drag.active) {
        if (gap_y < 0) { gap_y = by; by += dh + SIDE_TILE_GAP; }
        /* where it will land: a dashed outline */
        uint32_t lc = T.classic ? T.text : WHITE;
        for (int xx = ix; xx < ix + iw; xx += 6) { hline(c, xx, gap_y, 3, lc); hline(c, xx, gap_y + dh - 1, 3, lc); }
        for (int yy = gap_y; yy < gap_y + dh; yy += 6) { vline(c, ix, yy, 3, lc); vline(c, ix + iw - 1, yy, 3, lc); }
        if (!T.classic) fill_round_rect_blend(c, ix + 1, gap_y + 1, iw - 2, dh - 2, 3, WHITE, 25);
        /* the tile itself, lifted: a deeper shadow, a little to the left */
        int fy = CLAMP(side_drag.y - side_drag.off, 0, h - dh), fx = ix - 4;
        blend_rect(c, fx + 5, fy + 6, iw, dh, BLACK, 70);
        side_paint_tile_cached(c, side_drag.t, fx, fy, iw);
    }
    /* add tile */
    if (by + 20 <= h - 2) {
        side_colors(&top, &bot, &text, &dim, &h1, &h2);
        int hovered = side_hot == 600;
        if (hovered && !T.classic) fill_round_rect_blend(c, ix, by, iw, 18, 3, WHITE, 30);
        draw_text(c, FONT_UI, ix + 8, by + 2, "+ Add tile", hovered ? text : dim);
    }
}
static void paint_sidebar(canvas_t *c) {
    if (!side_progress) return;
    int h = tb_y(), x = side_x();
    if (!side_canvas) side_canvas = canvas_new(SIDE_W, h);
    else canvas_resize(side_canvas, SIDE_W, h);
    gfx_clear(side_canvas, T.face);
    blit(side_canvas, 0, 0, c, x, 0, SIDE_W, h);
    paint_sidebar_panel(side_canvas);
    blit(c, x, 0, side_canvas, 0, 0, SIDE_W, h);
}
static int side_hit(int x, int y) {
    if (!sidebar_open || !side_progress || x < side_x() || y < 0 || y >= tb_y()) return -1;
    side_search_run();
    int ix = side_x() + 6, iw = SIDE_W - 12, by = 6;
    if (x < ix || x >= ix + iw) return -1;
    if (y >= by && y < by + SIDE_HDR) return 1;
    by += SIDE_HDR + 8;
    side_order_load();
    for (int k = 0; k < ST_N; k++) {
        int t = side_order[k];
        if (!side_tile_visible(t)) continue;
        int bh = side_body_h(t);
        bool clickable = (sidebar_tiles & TBIT(t)) && side_alpha[t] >= 128;
        if (y >= by && y < by + SIDE_TILE_HEAD) return !clickable ? -1 : x >= ix + iw - 18 ? 10 + t : 700 + t;   /* close, or the title bar: drag */
        if (clickable && y >= by + SIDE_TILE_HEAD && y < by + SIDE_TILE_HEAD + bh) {
            int ry = y - (by + SIDE_TILE_HEAD);
            if (t == ST_APPS && ry < 5 * 16) return 100 + ry / 16;
            if (t == ST_SEARCH) {
                if (ry < 24) return 400;
                int row = (ry - 27) / 16;
                if (side_query[0] && row >= 0 && row < side_res_n) return 401 + row;
                return 400;
            }
            if (t == ST_SHORTCUTS && ry < SIDE_SHORTCUTS * 16) return 200 + ry / 16;
            if (t == ST_QUICK && ry >= 2 && ry < 28 && x >= ix + 8) { int c = (x - (ix + 8)) / 26; if (c >= 0 && c < SIDE_QUICK) return 300 + c; }
            if (t == ST_MEMO) return 500;
        }
        by += SIDE_TILE_HEAD + bh + 2 + SIDE_TILE_GAP;
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
typedef struct { char name[96]; int icon; char path[256]; int kind; vnode_t *node; int px, py; } dicon_t;   /* px < 0: arranged */
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
    extern void dpos_apply(void); dpos_apply();
}
#define DCELL_W 76
#define DCELL_H 76
/* Icons the user placed stay where they were dropped (C:\WRLD\deskpos.txt: name, x, y per line);
 * the others fill the grid from the top left, around them, as Windows does without Auto Arrange */
#define DESKPOS_FILE "C:\\WRLD\\deskpos.txt"
void dpos_apply(void) {
    for (int i = 0; i < ndicons; i++) dicons[i].px = dicons[i].py = -1;
    uint32_t len = 0; char *t = (char *)vfs_read_file(DESKPOS_FILE, &len);
    if (!t) return;
    char *buf = kmalloc(len + 1); if (!buf) { kfree(t); return; }
    memcpy(buf, t, len); buf[len] = 0; kfree(t);
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = 0;
            for (int i = 0; i < ndicons; i++) if (!strcmp(dicons[i].name, line)) { dicons[i].px = atoi(t1 + 1); dicons[i].py = atoi(t2 + 1); }
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(buf);
}
static void dpos_save(void) {
    char *b = kmalloc(MAX_DICON * 120 + 1); if (!b) return;
    int o = 0;
    for (int i = 0; i < ndicons; i++) if (dicons[i].px >= 0) o += snprintf(b + o, MAX_DICON * 120 + 1 - o, "%s\t%d\t%d\n", dicons[i].name, dicons[i].px, dicons[i].py);
    vfs_write_file(DESKPOS_FILE, b, o);
    kfree(b);
}
static rect_t dicon_rect(int i) {
    if (dicons[i].px >= 0) return R(dicons[i].px, dicons[i].py, DCELL_W, DCELL_H - 4);
    int rows = MAX(1, (SCREEN_H - T.taskbar_h - 10) / DCELL_H);
    /* the i-th arranged icon takes the next grid cell no placed icon covers */
    int slot = 0, k = 0;
    for (;; slot++) {
        rect_t c = R(6 + (slot / rows) * DCELL_W, 8 + (slot % rows) * DCELL_H, DCELL_W, DCELL_H - 4);
        bool taken = false;
        for (int j = 0; j < ndicons && !taken; j++) if (dicons[j].px >= 0 && ABSDIFF(dicons[j].px, c.x) < DCELL_W / 2 && ABSDIFF(dicons[j].py, c.y) < DCELL_H / 2) taken = true;
        if (taken) continue;
        int idx = -1;
        for (int j = 0, n = 0; j < ndicons; j++) if (dicons[j].px < 0) { if (n == k) { idx = j; break; } n++; }
        if (idx == i) return c;
        if (idx < 0 || slot > 400) return c;
        k++;
    }
}
/* the icons named in a drag, dropped on empty desktop at (x, y): placed there, on the grid */
static bool dpos_place(int x, int y, const char *paths) {
    int rows = MAX(1, (SCREEN_H - T.taskbar_h - 10) / DCELL_H), cols = MAX(1, (SCREEN_W - 12) / DCELL_W);
    int col = CLAMP((x - 6) / DCELL_W, 0, cols - 1), row = CLAMP((y - 8) / DCELL_H, 0, rows - 1);
    bool any = false;
    for (const char *p = paths; p && *p; ) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[300]; snprintf(line, sizeof line, "%.*s", (int)MIN(n, sizeof line - 1), p);
        p += n + (e ? 1 : 0);
        for (int i = 0; i < ndicons; i++) {
            bool me = !strcasecmp(dicons[i].path, line) || (!strncmp(line, "::desk:", 7) && !strcmp(dicons[i].name, line + 7));
            if (!me) continue;
            dicons[i].px = 6 + col * DCELL_W; dicons[i].py = 8 + row * DCELL_H;
            if (++row >= rows) { row = 0; col = MIN(col + 1, cols - 1); }
            any = true;
        }
    }
    if (any) { dpos_save(); wm_need_redraw = true; }
    return any;
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
    rect_t cl = c->clip;                      /* a partial repaint copies only its part of the wallpaper */
    if (wallpaper && cl.x == 0 && cl.y == 0 && cl.w == SCREEN_W && cl.h == SCREEN_H && c->stride == SCREEN_W) memcpy(c->px, wallpaper->px, SCREEN_W * SCREEN_H * 4);
    else if (wallpaper) { for (int y = cl.y; y < cl.y + cl.h && y < wallpaper->h; y++) memcpy(c->px + y * c->stride + cl.x, wallpaper->px + y * wallpaper->stride + cl.x, MIN(cl.w, wallpaper->w - cl.x) * 4); }
    else fill_rect(c, cl.x, cl.y, cl.w, cl.h, desktop_color);
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
    case 15: if (i >= 0 && dicons[i].node) extract_open(dicons[i].path); break;
    case 14: { extern void recycle_empty(void); recycle_empty(); break; }
    }
}
/* files dropped on the desktop: on the Recycle Bin they are deleted, on a folder they go in it,
 * anywhere else into the Desktop folder */
static bool dpos_place(int x, int y, const char *paths);
bool desktop_drop(int x, int y, const char *paths, bool copy) {
    extern bool recycle_move(vnode_t *n);
    int i = dicon_at(x, y);
    bool ok = false;
    if (i >= 0) {                                             /* an icon dropped on itself: just moved a little */
        char self[300]; snprintf(self, sizeof self, "::desk:%s", dicons[i].name);
        const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
        if ((n == strlen(dicons[i].path) && !strncasecmp(paths, dicons[i].path, n)) || (n == strlen(self) && !strncmp(paths, self, n))) {
            int k = i; i = -1;
            dicons[k].px = dicons[k].py = -2;                 /* not in the way of its own new place */
            bool moved = dpos_place(x, y, paths);
            if (!moved) dicons[k].px = dicons[k].py = -1;
            return true;
        }
    }
    if (i >= 0 && !strcmp(dicons[i].path, "::recycle")) {
        for (const char *p = paths; p && *p; ) {
            const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
            char line[512]; snprintf(line, sizeof line, "%.*s", (int)MIN(n, sizeof line - 1), p);
            p += n + (e ? 1 : 0);
            vnode_t *f = line[0] ? vfs_resolve(line, NULL) : NULL;
            if (f && recycle_move(f)) ok = true;
        }
    } else if (i >= 0 && dicons[i].node && dicons[i].node->dir) ok = shell_drop_files(dicons[i].path, paths, copy);
    else if (i >= 0 && dicons[i].node) {                      /* on a file or shortcut: open the file with it */
        char first[512]; const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
        snprintf(first, sizeof first, "%.*s", (int)MIN(n, sizeof first - 1), paths);
        char cmd[800]; snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", dicons[i].path, first);
        ok = shell_execute(cmd, NULL);
    } else {
        /* icons of the desktop itself (its files, or the fixed icons) dropped on empty space: moved there */
        bool from_desktop = true;
        for (const char *p = paths; p && *p && from_desktop; ) {
            const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
            char line[300]; snprintf(line, sizeof line, "%.*s", (int)MIN(n, sizeof line - 1), p);
            p += n + (e ? 1 : 0);
            bool found = !strncmp(line, "::desk:", 7);
            for (int k = 0; k < ndicons && !found; k++) if (!strcasecmp(dicons[k].path, line)) found = true;
            if (!found) from_desktop = false;
        }
        if (from_desktop && i < 0) { dpos_place(x, y, paths); return true; }
        ok = shell_drop_files(USER_DESKTOP, paths, copy);
        if (ok) { desktop_refresh(); dpos_place(x, y, paths); }
        return ok;
    }
    desktop_refresh();
    return ok;
}
static int dpress = -1, dpress_x, dpress_y;
void desktop_mouse(int type, int x, int y, int btn) {
    int i = dicon_at(x, y);
    if (type == ME_UP) dpress = -1;
    if (type == ME_MOVE && dpress >= 0 && (btn & MB_LEFT) && (ABSDIFF(x, dpress_x) > 5 || ABSDIFF(y, dpress_y) > 5)) {
        int k = dpress; dpress = -1;
        if (dicons[k].node) wm_drag_files(dicons[k].path, dicons[k].icon);
        else { char p[128]; snprintf(p, sizeof p, "::desk:%s", dicons[k].name); wm_drag_files(p, dicons[k].icon); }
        return;
    }
    if (type == ME_DOWN) {
        dpress = (btn & MB_LEFT) && i >= 0 ? i : -1; dpress_x = x; dpress_y = y;
        if (dsel != i) { dsel = i; wm_need_redraw = true; }
        if (wm_active) { wm_active = NULL; wm_need_redraw = true; }
        if (btn & MB_RIGHT) {
            if (i >= 0) {
                static menu_item_t m[] = { { "Open", 1, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Rename", 3, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } };
                static menu_item_t rb[] = { { "Open", 1, 0 }, { "Empty Recycle Bin", 14, 0 } };
                if (!strcmp(dicons[i].path, "::recycle")) popup_menu(x, y, rb, 2, desk_ctx_cb, (void *)(uintptr_t)i);
                else if (!dicons[i].node) { static menu_item_t m2[] = { { "Open", 1, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } }; popup_menu(x, y, m2, 3, desk_ctx_cb, (void *)(uintptr_t)i); }
                else if (!dicons[i].node->dir && archive_is_known(dicons[i].path)) {
                    static menu_item_t ma[] = { { "Open", 1, 0 }, { "Extract All...", 15, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Rename", 3, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } };
                    popup_menu(x, y, ma, 7, desk_ctx_cb, (void *)(uintptr_t)i);
                }
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
static int start_w(void) { return T.classic ? START_W_CLASSIC : T.renderer == RENDER_AERO ? T.taskbar_h + 8 : T.renderer == RENDER_METRO ? T.taskbar_h + 8 : T.renderer == RENDER_PLEX ? 70 : START_W_LUNA; }
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
rect_t shell_task_button_rect(window_t *w) {
    window_t *list[32]; int n = task_btns(list, 32);
    for (int i = 0; i < n; i++) if (list[i] == w) return task_btn_rect(i, n);
    return R(start_w(), tb_y(), 80, T.taskbar_h);
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
    if (T.renderer == RENDER_METRO) {   /* the globe as a flat line logo */
        int w = start_w(), h = T.taskbar_h;
        if (hot || down) fill_rect(c, 0, y, w, h, down ? T.start_top : lighten(T.tb_top, 40));
        uint32_t ink = hot || down ? WHITE : T.start_top;
        int rad = h >= 36 ? 11 : 8;
        int32_t cx = RASTER_FIX(w) / 2, cy = RASTER_FIX(2 * y + h) / 2, r = RASTER_FIX(rad), k = r * 552 / 1000;
        int32_t mr = r * 45 / 100, mk = mr * 552 / 1000, lat = r / 2, half = r * 866 / 1000;
        raster_path_t *p = raster_path_new();
        if (p) {
            /* outline */
            raster_move_to(p, cx + r, cy);
            raster_cubic_to(p, cx + r, cy + k, cx + k, cy + r, cx, cy + r);
            raster_cubic_to(p, cx - k, cy + r, cx - r, cy + k, cx - r, cy);
            raster_cubic_to(p, cx - r, cy - k, cx - k, cy - r, cx, cy - r);
            raster_cubic_to(p, cx + k, cy - r, cx + r, cy - k, cx + r, cy);
            raster_close(p);
            /* a meridian */
            raster_move_to(p, cx, cy - r);
            raster_cubic_to(p, cx + mk, cy - r, cx + mr, cy - k, cx + mr, cy);
            raster_cubic_to(p, cx + mr, cy + k, cx + mk, cy + r, cx, cy + r);
            raster_cubic_to(p, cx - mk, cy + r, cx - mr, cy + k, cx - mr, cy);
            raster_cubic_to(p, cx - mr, cy - k, cx - mk, cy - r, cx, cy - r);
            /* the equator and two parallels */
            raster_move_to(p, cx - r, cy); raster_line_to(p, cx + r, cy);
            raster_move_to(p, cx - half, cy - lat); raster_line_to(p, cx + half, cy - lat);
            raster_move_to(p, cx - half, cy + lat); raster_line_to(p, cx + half, cy + lat);
            raster_stroke(c, p, 2, ink);
            raster_path_free(p);
        }
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
    bool aero = T.renderer == RENDER_AERO, plex = T.renderer == RENDER_PLEX, metro = T.renderer == RENDER_METRO;
    if (metro) {
        fill_rect(c, 0, y, SCREEN_W, h, T.tb_top);
        hline(c, 0, y, SCREEN_W, T.tray_border);
    } else if (T.classic) {
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
    else if (!aero && !metro) { vline(c, qx - 3, y + 3, h - 5, darken(T.tb_top, 50)); vline(c, qx - 2, y + 3, h - 5, lighten(T.tb_top, 60)); }
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
        } else if (metro) {   /* flat tiles: the active one is lighter with an accent line */
            if (act || hot) fill_rect(c, r.x, r.y, r.w, r.h, act ? T.tbtn_act_top : T.tbtn_hot);
            if (act) draw_rect(c, r.x, r.y, r.w, r.h, T.tbtn_border);
            draw_icon(c, w->icon >= 0 ? w->icon : ICON_EXE, 16, r.x + 6, r.y + (r.h - 16) / 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 26, r.y + (r.h - 14) / 2, r.w - 32, w->title, T.tbtn_text);
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
    } else if (metro) {
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
    uint32_t bg = dark_mode ? T.face : HEX(0xFFFFE1), fg = dark_mode ? T.text : BLACK, bd = dark_mode ? T.menu_border : BLACK;
    blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 40);
    fill_round_rect(c, r.x, r.y, r.w, r.h, 8, bd);
    fill_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, bg);
    for (int i = 0; i < 12; i++) { hline(c, r.x + r.w - 40 + i, r.y + r.h - 1 + i, 13 - i, bg); put_pixel(c, r.x + r.w - 41 + i, r.y + r.h - 1 + i, bd); put_pixel(c, r.x + r.w - 27, r.y + r.h - 1 + i, bd); }
    draw_icon(c, ICON_INFO, 16, r.x + 10, r.y + 8);
    draw_text(c, FONT_UI_BOLD, r.x + 32, r.y + 9, balloon_title, fg);
    draw_text_wrapped(c, FONT_UI, r.x + 10, r.y + 28, r.w - 20, balloon_text, fg, 1);
}

/* ---------------- start menu ---------------- */
typedef struct { const char *label, *sub; int icon; const char *action; bool bold; } sm_item_t;
/* The Start menu is built from folders, as in Windows XP, so a program that adds a shortcut
 * shows up in it:
 *   Start Menu\Pinned\*.lnk     the pinned programs at the top left (first line: what to run,
 *                               an optional second line: the grey line under the name)
 *   Start Menu\Programs\...     All Programs, folders as cascading menus
 * and below the pinned programs, the ones started most often from the menu (sm_mfu in the
 * settings). A .lnk file holds a command: an application name ("notepad"), a path, or an
 * application and its argument ("control ncpa.cpl"). */
#define SM_DIR "C:\\Documents and Settings\\User\\Start Menu"
#define SM_PROGRAMS SM_DIR "\\Programs"
#define SM_PINNED SM_DIR "\\Pinned"
static const struct { const char *file, *target; } sm_seed[] = {
    { "Pinned\\Internet.lnk", "xplorer\nThe WRLD Browser" },
    { "Pinned\\Command Prompt.lnk", "cmd\nShell & scripts" },
    { "Pinned\\File Explorer.lnk", "explorer\nBrowse files" },
    { "Programs\\Accessories\\Calculator.lnk", "calc" },
    { "Programs\\Accessories\\Command Prompt.lnk", "cmd" },
    { "Programs\\Accessories\\File Explorer.lnk", "explorer" },
    { "Programs\\Accessories\\Notepad.lnk", "notepad" },
    { "Programs\\Accessories\\Paint.lnk", "paint" },
    { "Programs\\Accessories\\WordPad.lnk", "wordpad" },
    { "Programs\\Accessories\\Logon Skins\\Logon Skin Maker.lnk", "logonmaker" },
    { "Programs\\Accessories\\Logon Skins\\Logon Skin Themer.lnk", "logonthemer" },
    { "Programs\\Accessories\\System Tools\\Control Panel.lnk", "control" },
    { "Programs\\Accessories\\System Tools\\Internet Options.lnk", "control inetcpl.cpl" },
    { "Programs\\Accessories\\System Tools\\Network Connections.lnk", "control ncpa.cpl" },
    { "Programs\\Accessories\\System Tools\\Task Manager.lnk", "taskmgr" },
    { "Programs\\Accessories\\System Tools\\WRLD Security Center.lnk", "security" },
    { "Programs\\Games\\Minesweeper.lnk", "minesweeper" },
    { "Programs\\Games\\Pong.lnk", "pong" },
    { "Programs\\Games\\Snake.lnk", "snake" },
    { "Programs\\Samples\\Hello Console (hello.exe).lnk", "C:\\Samples\\hello.exe" },
    { "Programs\\Samples\\Hello Win32 (winhello.exe).lnk", "C:\\Samples\\winhello.exe" },
    { "Programs\\Samples\\Mount Sample.dmg.lnk", "C:\\Samples\\Sample.dmg" },
    { "Programs\\Samples\\Samples folder.lnk", "C:\\Samples" },
    { "Programs\\Samples\\Shell demo (demo.sh).lnk", "C:\\Samples\\demo.sh" },
    { "Programs\\About " OS_NAME ".lnk", "about" },
    { "Programs\\THE WRLD Media Player.lnk", "wmplayer" },
    { "Programs\\The WRLD Browser.lnk", "xplorer" },
    { "Programs\\The WRLD Search.lnk", "search" },
    { "Programs\\WRLD 3D.lnk", "wrld3d" },
    /* added later: appended, so they reach existing Start menus once (sm_seeded) */
    { "Programs\\Games\\Solitaire.lnk", "solitaire" },
    { "Programs\\Games\\Orbit Pinball.lnk", "pinball" },
    { "Programs\\Games\\WRLD Sentinel.lnk", "sentinel" },
};
/* the folders, with the shortcuts above, on first use; later additions (a newer version's
 * games) are added once each, recorded in sm_seeded, so a shortcut the user deleted stays gone */
static void sm_ensure(void) {
    static bool done;
    if (done) return;
    done = true;
    const char *v = settings_get("sm_seeded");
    int seeded = v ? atoi(v) : 0;
    bool fresh = !vfs_resolve(SM_PROGRAMS, NULL);
    for (int i = fresh ? 0 : seeded; i < (int)ARRAY_LEN(sm_seed); i++) {
        char p[300], dir[300];
        snprintf(p, sizeof p, SM_DIR "\\%s", sm_seed[i].file);
        if (vfs_resolve(p, NULL)) continue;
        strlcpy(dir, p, sizeof dir); char *bs = strrchr(dir, '\\'); if (bs) *bs = 0;
        vfs_mkdir_p(dir);
        vfs_write_file(p, sm_seed[i].target, strlen(sm_seed[i].target));
    }
    char b[16]; snprintf(b, sizeof b, "%d", (int)ARRAY_LEN(sm_seed));
    if (seeded != (int)ARRAY_LEN(sm_seed)) { settings_set("sm_seeded", b); settings_save(); }
}
/* a shortcut's name without ".lnk", its icon, and its second line */
static void lnk_label(vnode_t *n, char *out, int cap) {
    strlcpy(out, n->name, cap);
    char *dot = strrchr(out, '.');
    if (dot && !strcasecmp(dot, ".lnk")) *dot = 0;
}
static void lnk_line(vnode_t *n, int line, char *out, int cap) {
    out[0] = 0;
    if (!n->data || !n->size) return;
    const char *p = (const char *)n->data, *end = p + n->size;
    for (int i = 0; i < line && p < end; i++) { const char *nl = memchr(p, '\n', end - p); if (!nl) return; p = nl + 1; }
    int k = 0;
    while (p < end && *p != '\n' && *p != '\r' && k < cap - 1) out[k++] = *p++;
    out[k] = 0;
}

/* ---- the left column: pinned, then the most used ---- */
#define SM_LEFT_MAX 14
#define MFU_SHOW 6
static sm_item_t sm_left[SM_LEFT_MAX];
static int sm_left_n, sm_pinned_n;
static char sm_left_text[SM_LEFT_MAX][3][160];          /* label, sub, action */
typedef struct { char label[64], action[200]; int count; } mfu_t;
static mfu_t mfu[16]; static int nmfu; static bool mfu_loaded;
static void mfu_load(void) {
    if (mfu_loaded) return;
    mfu_loaded = true; nmfu = 0;
    const char *v = settings_get("sm_mfu");
    if (!v || !*v) {                                     /* a new user: what XP showed first */
        static const struct { const char *l, *a; } def[] = { { "THE WRLD Media Player", "wmplayer" }, { "Notepad", "notepad" },
            { "Paint", "paint" }, { "Solitaire", "solitaire" }, { "Minesweeper", "minesweeper" }, { "Orbit Pinball", "pinball" } };
        for (unsigned i = 0; i < ARRAY_LEN(def); i++) { strlcpy(mfu[nmfu].label, def[i].l, 64); strlcpy(mfu[nmfu].action, def[i].a, 200); mfu[nmfu++].count = 6 - i; }
        return;
    }
    /* count|label|action;... */
    while (*v && nmfu < (int)ARRAY_LEN(mfu)) {
        const char *e = strchr(v, ';'); if (!e) e = v + strlen(v);
        char f[300]; int l = MIN((int)(e - v), 299); memcpy(f, v, l); f[l] = 0;
        char *a = strchr(f, '|'), *b = a ? strchr(a + 1, '|') : NULL;
        if (a && b) { *a = *b = 0; mfu[nmfu].count = atoi(f); strlcpy(mfu[nmfu].label, a + 1, 64); strlcpy(mfu[nmfu].action, b + 1, 200); nmfu++; }
        v = *e ? e + 1 : e;
    }
}
static void mfu_save(void) {
    char out[2400]; int o = 0; out[0] = 0;
    for (int i = 0; i < nmfu && o < (int)sizeof out - 300; i++) o += snprintf(out + o, sizeof out - o, "%s%d|%s|%s", i ? ";" : "", mfu[i].count, mfu[i].label, mfu[i].action);
    settings_set("sm_mfu", out); settings_save();
}
static void mfu_sort(void) { for (int a = 1; a < nmfu; a++) for (int b = a; b > 0 && mfu[b].count > mfu[b - 1].count; b--) { mfu_t t = mfu[b]; mfu[b] = mfu[b - 1]; mfu[b - 1] = t; } }
/* a program started from the Start menu counts towards the most-used list */
static void mfu_note(const char *label, const char *action) {
    if (!label || !*label || strchr(label, '|') || strchr(label, ';') || strchr(action, '|') || strchr(action, ';')) return;
    mfu_load();
    int i = 0;
    while (i < nmfu && strcmp(mfu[i].action, action)) i++;
    if (i == nmfu) {
        if (nmfu == (int)ARRAY_LEN(mfu)) i = --nmfu;      /* the least used goes */
        strlcpy(mfu[i].label, label, 64); strlcpy(mfu[i].action, action, 200); mfu[i].count = 0; nmfu++;
    }
    mfu[i].count++;
    mfu_sort();
    mfu_save();
}
static void mfu_remove(const char *action) {
    mfu_load();
    for (int i = 0; i < nmfu; i++) if (!strcmp(mfu[i].action, action)) { memmove(&mfu[i], &mfu[i + 1], sizeof mfu[0] * (nmfu - i - 1)); nmfu--; break; }
    mfu_save();
}
static int sm_add_left(const char *label, const char *sub, int icon, const char *action, bool bold) {
    if (sm_left_n >= SM_LEFT_MAX) return -1;
    int i = sm_left_n++;
    if (!label) { sm_left[i] = (sm_item_t){ NULL }; return i; }
    strlcpy(sm_left_text[i][0], label, 160); strlcpy(sm_left_text[i][1], sub ? sub : "", 160); strlcpy(sm_left_text[i][2], action, 160);
    sm_left[i] = (sm_item_t){ sm_left_text[i][0], sub && *sub ? sm_left_text[i][1] : NULL, icon, sm_left_text[i][2], bold };
    return i;
}
static int action_icon(const char *action) {
    if (!strncmp(action, "open:", 5)) { vnode_t *n = vfs_resolve(action + 5, NULL); if (n) return file_icon(n); }
    char t[64]; strlcpy(t, action, sizeof t); char *sp = strchr(t, ' '); if (sp) *sp = 0;
    const app_info_t *a = app_find(t);
    return a ? a->icon : ICON_EXE;
}
static void sm_fit(void);
static void sm_build(void) {
    sm_ensure();
    sm_left_n = sm_pinned_n = 0;
    vnode_t *d = vfs_resolve(SM_PINNED, NULL);
    for (vnode_t *n = d && d->dir ? d->child : NULL; n && sm_pinned_n < 5; n = n->next) {
        if (n->dir) continue;
        char label[96], sub[96], action[300];
        lnk_label(n, label, sizeof label); lnk_line(n, 1, sub, sizeof sub);
        char p[300]; vfs_path(n, p, sizeof p); snprintf(action, sizeof action, "open:%s", p);
        sm_add_left(label, sub, file_icon(n), action, true);
        sm_pinned_n++;
    }
    sm_add_left(NULL, NULL, 0, NULL, false);
    mfu_load();
    int shown = 0;
    for (int i = 0; i < nmfu && shown < MFU_SHOW && sm_left_n < SM_LEFT_MAX; i++) {
        bool pinned = false;                              /* a pinned program is not listed twice */
        for (int k = 0; k < sm_pinned_n; k++) if (!strcasecmp(sm_left[k].label, mfu[i].label)) pinned = true;
        if (pinned) continue;
        sm_add_left(mfu[i].label, NULL, action_icon(mfu[i].action), mfu[i].action, false);
        shown++;
    }
    sm_fit();
}
static const sm_item_t sm_right[] = {
    { "My Documents", NULL, ICON_MY_DOCUMENTS, "explorer:" USER_DOCS, true },
    { "My Computer", NULL, ICON_COMPUTER, "explorer:::computer", true },
    { "The WRLD Search", NULL, ICON_SEARCH, "search", true },
    { NULL },
    { "Control Panel", NULL, ICON_CONTROL_PANEL, "control", false },
    { "Network Connections", NULL, ICON_CP_NETWORK, "control:ncpa.cpl", false },
    { "Personalization", NULL, ICON_CP_APPEARANCE, "control:personalization", false },
    { "WRLD Security Center", NULL, ICON_CP_SYSTEM, "security", false },
    { NULL },
    { "Help and Support", NULL, ICON_HELP, "help", false },
    { "Run...", NULL, ICON_RUN, "run", false },
};
#define SM_LEFT_N sm_left_n
#define SM_RIGHT_N ((int)ARRAY_LEN(sm_right))

/* ---- All Programs: Start Menu\Programs as cascading menus, folders first; at the top level,
 * after a line, the programs Linux packages added (pkg.c) ---- */
#define AP_LEVELS 4
#define AP_MAX 60
typedef struct { char label[72]; char action[300]; int icon; bool folder; vnode_t *dir; } ap_entry_t;
typedef struct { ap_entry_t e[AP_MAX]; int n; int from; } ap_level_t;   /* from: the parent row */
static ap_level_t ap_lv[AP_LEVELS];
static int ap_depth;                                       /* open levels (0: All Programs is closed) */
static int ap_hot_lv = -1, ap_hot = -1;                    /* the lit row, and its level */
static char ap_actions[AP_MAX][16];
static int ap_cmp(const ap_entry_t *a, const ap_entry_t *b) { if (a->folder != b->folder) return a->folder ? -1 : 1; return strcasecmp(a->label, b->label); }
static void ap_fill(int lv, vnode_t *dir) {
    ap_level_t *L = &ap_lv[lv];
    L->n = 0;
    for (vnode_t *n = dir && dir->dir ? dir->child : NULL; n && L->n < AP_MAX; n = n->next) {
        ap_entry_t *e = &L->e[L->n++];
        lnk_label(n, e->label, sizeof e->label);
        e->folder = n->dir; e->dir = n->dir ? n : NULL;
        e->icon = n->dir ? ICON_FOLDER : file_icon(n);
        char p[280]; vfs_path(n, p, sizeof p); snprintf(e->action, sizeof e->action, "open:%s", p);
    }
    for (int a = 1; a < L->n; a++) for (int b = a; b > 0 && ap_cmp(&L->e[b], &L->e[b - 1]) < 0; b--) { ap_entry_t t = L->e[b]; L->e[b] = L->e[b - 1]; L->e[b - 1] = t; }
    if (lv == 0) {
        int m = MIN(pkg_menu_count(), AP_MAX - L->n - 1);
        if (m > 0) {
            ap_entry_t *sep = &L->e[L->n++]; memset(sep, 0, sizeof *sep);
            for (int i = 0; i < m; i++) {
                const pkg_menu_t *pm_ = pkg_menu_get(i);
                ap_entry_t *e = &L->e[L->n++];
                strlcpy(e->label, pm_->name, sizeof e->label); e->folder = false; e->dir = NULL;
                e->icon = pm_->terminal ? ICON_TERMINAL : ICON_PACKAGE;
                snprintf(ap_actions[i], sizeof ap_actions[i], "lxmenu:%d", i); strlcpy(e->action, ap_actions[i], sizeof e->action);
            }
        }
    }
}
static void ap_open(int lv, vnode_t *dir, int from) {     /* open level lv (closing deeper ones) */
    if (lv >= AP_LEVELS) return;
    ap_fill(lv, dir); ap_lv[lv].from = from; ap_depth = lv + 1;
    wm_need_redraw = true;
}
static void ap_close(void) { ap_depth = 0; ap_hot = ap_hot_lv = -1; }
#define allprog_open (ap_depth > 0)
static int ap_width_lv(int lv) {
    int w = 180;
    for (int i = 0; i < ap_lv[lv].n; i++) if (ap_lv[lv].e[i].label[0]) w = MAX(w, 28 + text_width(FONT_UI, ap_lv[lv].e[i].label) + 26);
    return MIN(w, 360);
}
static int ap_row_h(const ap_entry_t *e) { return e->label[0] ? 24 : 7; }
#define SM_W 380
#define SM_H 480
#define SM_HEAD 64
#define SM_FOOT 44

/* The menu is as tall as its columns need (the left one grows with pinned and frequently used
 * programs, and All Programs sits under it), at least SM_H, and never taller than the space above
 * the taskbar: past that, the least used programs are left out of the list. */
static int sm_left_h(void) { int h = 0; for (int i = 0; i < sm_left_n; i++) h += !sm_left[i].label ? 9 : sm_left[i].sub ? 40 : 34; return h; }
static int sm_right_h(void) { int h = 0; for (int i = 0; i < SM_RIGHT_N; i++) h += !sm_right[i].label ? 9 : 26; return h; }
static int sm_needed_h(void) {
    int left = sm_left_h() + 10 + 36;                         /* the line and All Programs under the list */
    return SM_HEAD + 6 + MAX(left, sm_right_h()) + 6 + SM_FOOT;
}
static void sm_fit(void) {
    while (sm_left_n > sm_pinned_n + 1 && sm_needed_h() > tb_y() - 2) sm_left_n--;
    if (sm_left_n == sm_pinned_n + 1 && sm_needed_h() > tb_y() - 2) sm_left_n = sm_pinned_n;   /* no list: no line */
}
static rect_t sm_rect(void) {
    int h = T.classic ? 290 : CLAMP(sm_needed_h(), SM_H, tb_y() - 2);
    int offset = wm_animations && startmenu_animation == MENU_ANIM_SLIDE ? h * (255 - sm_progress) / 255 : 0;
    return R(0, tb_y() - h + offset, T.classic ? 190 : SM_W, h);
}
/* item hit ids: 0..99 left, 100..199 right, 300 all programs, 400 logoff, 401 shutdown,
 * 500.. classic items, 600 + level * 100 + row: All Programs */
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
    { "Settings", NULL, ICON_CONTROL_PANEL, "control" }, { "Search", NULL, ICON_SEARCH, "search" },
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
static rect_t ap_item_rect_lv(int lv, int i);
/* level lv's box: level 0 beside All Programs (or Programs in the classic menu), each deeper
 * level beside the folder row it came from, kept on the screen */
static rect_t ap_rect_lv(int lv) {
    ap_level_t *L = &ap_lv[lv];
    int h = 6; for (int i = 0; i < L->n; i++) h += ap_row_h(&L->e[i]);
    int aw = ap_width_lv(lv);
    int x, ytop;
    if (lv == 0) {
        rect_t m = sm_rect();
        if (T.classic) { rect_t r = cl_item_rect(0); x = m.x + m.w - 2; ytop = r.y; }
        else { rect_t a = sm_item_rect(300); x = a.x + a.w + 6; ytop = a.y + a.h - h; }
    } else {
        rect_t pr = ap_rect_lv(lv - 1), row = ap_item_rect_lv(lv - 1, L->from);
        x = pr.x + pr.w - 3; ytop = row.y - 3;
        if (x + aw > SCREEN_W) x = MAX(0, pr.x - aw + 3);
    }
    if (ytop + h > tb_y()) ytop = tb_y() - h;
    if (ytop < 0) ytop = 0;
    return R(x, ytop, aw, MIN(h, tb_y()));
}
static rect_t ap_item_rect_lv(int lv, int i) {
    rect_t r = ap_rect_lv(lv); int y = r.y + 3;
    for (int k = 0; k < i; k++) y += ap_row_h(&ap_lv[lv].e[k]);
    return R(r.x + 2, y, r.w - 4, ap_row_h(&ap_lv[lv].e[i]));
}
bool startmenu_is_open(void) { return startmenu_open; }
static bool sm_kbd;                                         /* the menu is being used from the keyboard */
static void startmenu_set(bool open) {
    if (open) sm_build();
    startmenu_open = open; ap_close(); sm_hot = -1; sm_ignore_up = false; sm_kbd = false;
    animation_start(&sm_motion, open ? 255 : 0, timer_ticks(), wm_animations && startmenu_animation != MENU_ANIM_NONE ? 200 : 0);
    sm_progress = animation_value(&sm_motion, timer_ticks()); shell_anim_frame = 0; wm_need_redraw = true;
}
void startmenu_toggle(void) { startmenu_set(!startmenu_open); }
static void paint_allprog(canvas_t *c) {
    for (int lv = 0; lv < ap_depth; lv++) {
        rect_t r = ap_rect_lv(lv);
        if (T.classic) { fill_rect(c, r.x, r.y, r.w, r.h, T.face); bevel_deep(c, r.x, r.y, r.w, r.h, true); }
        else { blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 50); fill_rect(c, r.x, r.y, r.w, r.h, T.menu_bg); draw_rect(c, r.x, r.y, r.w, r.h, T.sm_border); vline(c, r.x + 1, r.y + 1, r.h - 2, mix(T.sm_border, T.face_light, 150)); }
        ap_level_t *L = &ap_lv[lv];
        if (!L->n) { draw_text(c, FONT_UI, r.x + 10, r.y + 6, "(Empty)", T.gray_text); continue; }
        for (int i = 0; i < L->n; i++) {
            rect_t ir = ap_item_rect_lv(lv, i);
            if (ir.y + ir.h > r.y + r.h) break;
            ap_entry_t *e = &L->e[i];
            if (!e->label[0]) { hline(c, ir.x + 4, ir.y + 3, ir.w - 8, T.face_dark); continue; }
            bool hot = (ap_hot_lv == lv && ap_hot == i) || (lv + 1 < ap_depth && ap_lv[lv + 1].from == i);
            if (hot) fill_rect(c, ir.x, ir.y, ir.w, ir.h, T.hot);
            draw_icon(c, e->icon, 16, ir.x + 6, ir.y + 4);
            draw_text_ellipsis(c, FONT_UI, ir.x + 28, ir.y + 5, ir.w - 46, e->label, hot ? T.hot_text : T.text);
            if (e->folder) draw_arrow(c, ir.x + ir.w - 10, ir.y + ir.h / 2, 3, hot ? T.hot_text : T.text);
        }
    }
}
static void paint_rotated_text(canvas_t *c, int x, int ybottom, const char *s, int font, uint32_t col) {
    int w = text_width(font, s), h = font_height(font);
    canvas_t *tmp = canvas_new(w, h);
    if (!tmp) return;
    gfx_clear(tmp, 0xFF000000);
    draw_text(tmp, font, 0, 0, s, 0xFFFFFFFF);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int a = C_R(tmp->px[j * w + i]);
        if (a) blend_pixel(c, x + j, ybottom - i, col, a);
    }
    canvas_free(tmp);
}
static void paint_startmenu_body(canvas_t *c) {
    rect_t m = sm_rect();
    if (T.classic) {
        fill_rect(c, m.x, m.y, m.w, m.h, T.face); bevel_deep(c, m.x, m.y, m.w, m.h, true);
        gradient_v(c, m.x + 3, m.y + 3, 21, m.h - 6, T.sm_head_top, T.sm_head_bot);
        paint_rotated_text(c, m.x + 3, m.y + m.h - 8, OS_NAME, FONT_LARGE_BOLD, WHITE);
        for (int i = 0; i < CL_N; i++) {
            rect_t r = cl_item_rect(i);
            if (!classic_items[i].label) { hline(c, r.x + 2, r.y + 3, r.w - 4, T.face_dark); hline(c, r.x + 2, r.y + 4, r.w - 4, T.face_light); continue; }
            bool hot = sm_hot == 500 + i;
            if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
            draw_icon(c, classic_items[i].icon, 24, r.x + 4, r.y + 4);
            char lbl[64]; if (i == 7) snprintf(lbl, sizeof lbl, "Log Off %s...", user_name); else strlcpy(lbl, classic_items[i].label, sizeof lbl);
            draw_text_ellipsis(c, FONT_UI, r.x + 36, r.y + (r.h - 14) / 2, r.w - 36 - (i == 0 ? 16 : 4), lbl, hot ? T.hot_text : T.text);
            if (i == 0) draw_arrow(c, r.x + r.w - 10, r.y + r.h / 2, 3, hot ? T.hot_text : T.text);
        }
        if (allprog_open) paint_allprog(c);
        return;
    }
    /* Luna two-column menu (Aero: the same layout on glass) */
    bool aero = T.renderer == RENDER_AERO, flat = T.renderer == RENDER_METRO;
    int corner = flat ? 0 : 7;
    blend_rect(c, m.x + m.w, m.y + 6, 3, m.h - 6, BLACK, 60);
    if (aero) {
        draw_glass(c, m.x, m.y, m.w, m.h, T.sm_head_bot, 200, 8);
        for (int j = 0; j < 40; j++) blend_rect(c, m.x, m.y + j, m.w, 1, WHITE, 50 - j);
        draw_rect(c, m.x, m.y, m.w, m.h, T.sm_border);
        blend_rect(c, m.x + 1, m.y + 1, m.w - 2, 1, WHITE, 120);
    } else if (flat) fill_rect(c, m.x, m.y, m.w, m.h, T.sm_border);
    else fill_round_rect(c, m.x, m.y, m.w, m.h, 7, T.sm_border);
    /* header */
    for (int j = 0; j < SM_HEAD && !aero; j++) {
        int in = 0, rr = corner; if (j < rr) { int dy = rr - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
        uint32_t col = flat ? T.sm_head_top : j < 6 ? mix(lighten(T.sm_head_top, 80), T.sm_head_top, j * 42) : mix(T.sm_head_top, T.sm_head_bot, (j - 6) * 255 / (SM_HEAD - 6));
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
            draw_text_ellipsis(c, FONT_UI_BOLD, r.x + 43, r.y + 6, r.w - 47, it->label, hot ? T.hot_text : T.sm_left_text);
            draw_text_ellipsis(c, FONT_UI, r.x + 43, r.y + 21, r.w - 47, it->sub, hot ? T.hot_text : mix(T.sm_left_text, T.sm_left, 90));
        } else draw_text_ellipsis(c, FONT_UI, r.x + 43, r.y + (r.h - 14) / 2, r.w - 47, it->label, hot ? T.hot_text : T.sm_left_text);
    }
    /* all programs */
    {
        rect_t r = sm_item_rect(300);
        uint32_t sep = mix(T.sm_left, T.sm_left_text, 50);
        gradient_h(c, r.x + 8, r.y - 5, r.w / 2 - 8, 1, T.sm_left, sep); gradient_h(c, r.x + r.w / 2, r.y - 5, r.w / 2 - 8, 1, sep, T.sm_left);
        bool hot = sm_hot == 300 || allprog_open;
        if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
        int tw = text_width(FONT_UI_BOLD, "All Programs");
        draw_text(c, FONT_UI_BOLD, r.x + (r.w - tw - 24) / 2, r.y + 8, "All Programs", hot ? T.hot_text : T.sm_left_text);
        draw_icon(c, ICON_GO, 16, r.x + (r.w + tw - 24) / 2 + 6, r.y + 7);
    }
    for (int i = 0; i < SM_RIGHT_N; i++) {
        rect_t r = sm_item_rect(100 + i);
        const sm_item_t *it = &sm_right[i];
        if (!it->label) { gradient_h(c, r.x + 8, r.y + 4, r.w - 16, 1, T.sm_right, T.sm_right_line); continue; }
        bool hot = sm_hot == 100 + i;
        if (hot) fill_rect(c, r.x, r.y, r.w, r.h, T.hot);
        draw_icon(c, it->icon, 24, r.x + 5, r.y + 1);
        draw_text_ellipsis(c, it->bold ? FONT_UI_BOLD : FONT_UI, r.x + 35, r.y + 6, r.w - 39, it->label, hot ? T.hot_text : T.sm_right_text);
    }
    /* footer */
    int fy = m.y + m.h - SM_FOOT;
    for (int j = 0; j < SM_FOOT - 1 && !aero; j++) {
        int in = 0, rr = corner, jj = SM_FOOT - 2 - j; if (jj < rr) { int dy = rr - jj, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rr * rr) dx++; in = rr - dx; }
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
static void paint_startmenu(canvas_t *c) {
    if (!sm_progress) return;
    rect_t old = canvas_push_clip(c, R(0, 0, SCREEN_W, tb_y()));
    rect_t m = sm_rect();
    rect_t visible = rect_intersect(R(m.x, m.y, m.w + 3, m.h), c->clip);
    bool fade = wm_animations && startmenu_animation == MENU_ANIM_FADE && sm_progress < 255 && visible.w > 0 && visible.h > 0;
    if (fade) {
        if (!sm_under) sm_under = canvas_new(visible.w, visible.h);
        else canvas_resize(sm_under, visible.w, visible.h);
        blit(sm_under, 0, 0, c, visible.x, visible.y, visible.w, visible.h);
    }
    paint_startmenu_body(c);
    if (fade) blit_alpha(c, visible.x, visible.y, sm_under, 255 - sm_progress);
    c->clip = old;
}
static void run_action(const char *a) {
    startmenu_set(false);
    if (!strcmp(a, "run")) shell_show_run();
    else if (!strcmp(a, "logoff")) { extern void shell_logoff_dialog(void); shell_logoff_dialog(); }
    else if (!strcmp(a, "shutdown")) shell_show_shutdown();
    else if (!strncmp(a, "explorer:", 9)) explorer_open(a + 9);
    else if (!strncmp(a, "control:", 8)) control_open(a + 8);
    else if (!strncmp(a, "open:", 5)) shell_open(a + 5, NULL);
    else if (!strncmp(a, "lxmenu:", 7)) { char err[200]; if (!pkg_menu_launch(atoi(a + 7), err, sizeof err)) msgbox("Linux program", err, MBI_ERROR, MBX_OK, NULL, NULL); }
    else app_launch(a, NULL);
}

/* ---------------- shell input ---------------- */
static int sm_hit(int x, int y) {
    if (y < 0 || y >= tb_y()) return -1;
    for (int lv = ap_depth - 1; lv >= 0; lv--) {             /* the deepest menu is on top */
        rect_t ar = ap_rect_lv(lv);
        if (!pt_in(ar, x, y)) continue;
        for (int i = 0; i < ap_lv[lv].n; i++) if (ap_lv[lv].e[i].label[0] && pt_in(ap_item_rect_lv(lv, i), x, y)) return 600 + lv * 100 + i;
        return -2;
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
/* the item an id names: what it runs and its label (NULL for a folder or nothing) */
static const char *sm_id_action(int h, const char **label) {
    *label = NULL;
    if (h >= 600) { ap_entry_t *e = &ap_lv[(h - 600) / 100].e[(h - 600) % 100]; if (e->folder) return NULL; *label = e->label; return e->action; }
    if (h >= 0 && h < SM_LEFT_N && sm_left[h].label) { *label = sm_left[h].label; return sm_left[h].action; }
    return NULL;
}
/* open what an id names: a program (counted towards the most used), a folder, or a command */
static void sm_activate(int h) {
    if (h >= 600) {
        int lv = (h - 600) / 100, i = (h - 600) % 100;
        ap_entry_t *e = &ap_lv[lv].e[i];
        if (e->folder) { ap_open(lv + 1, e->dir, i); ap_hot_lv = lv + 1; ap_hot = sm_kbd ? 0 : -1; return; }
        char a[300], l[72]; strlcpy(a, e->action, sizeof a); strlcpy(l, e->label, sizeof l);
        mfu_note(l, a); run_action(a); return;
    }
    if (h >= 500) { const char *a = classic_items[h - 500].action; if (!strcmp(a, "allprog")) { ap_open(0, vfs_resolve(SM_PROGRAMS, NULL), 0); if (sm_kbd) { ap_hot_lv = 0; ap_hot = 0; } } else run_action(a); return; }
    if (h == 300) { if (allprog_open) ap_close(); else { ap_open(0, vfs_resolve(SM_PROGRAMS, NULL), 0); if (sm_kbd) { ap_hot_lv = 0; ap_hot = 0; } } wm_need_redraw = true; return; }
    if (h == 400) { run_action("logoff"); return; }
    if (h == 401) { run_action("shutdown"); return; }
    if (h >= 100 && h < 200) { run_action(sm_right[h - 100].action); return; }
    if (h >= 0 && h < SM_LEFT_N && sm_left[h].label) {
        char a[160], l[160]; strlcpy(a, sm_left[h].action, sizeof a); strlcpy(l, sm_left[h].label, sizeof l);
        if (h >= sm_pinned_n) mfu_note(l, a);
        run_action(a);
    }
}
/* right-click on a program: open it, pin it to the top of the menu or unpin it, or take it off
 * the most-used list */
static char smctx_action[300], smctx_label[96]; static int smctx_id;
static void sm_ctx_cb(void *ctx, int id) {
    (void)ctx;
    if (id == 1) { if (startmenu_open) sm_activate(smctx_id); return; }
    if (id == 2) {                                           /* pin: a shortcut in Pinned with the same command */
        char dst[300], body[300] = "";
        snprintf(dst, sizeof dst, SM_PINNED "\\%s.lnk", smctx_label);
        if (!strncmp(smctx_action, "open:", 5)) {
            vnode_t *n = vfs_resolve(smctx_action + 5, NULL);
            if (n && !n->dir && n->data && !strcasecmp(vfs_ext(n->name), "lnk")) { int l = MIN(n->size, (uint32_t)sizeof body - 1); memcpy(body, n->data, l); body[l] = 0; }
            else strlcpy(body, smctx_action + 5, sizeof body);
        } else strlcpy(body, smctx_action, sizeof body);
        vfs_mkdir_p(SM_PINNED);
        vfs_write_file(dst, body, strlen(body));
    } else if (id == 3) {                                    /* unpin */
        vnode_t *n = !strncmp(smctx_action, "open:", 5) ? vfs_resolve(smctx_action + 5, NULL) : NULL;
        if (n && !n->dir) vfs_delete(n);
    } else if (id == 4) mfu_remove(smctx_action);
    if (startmenu_open) { sm_build(); wm_need_redraw = true; }
}
static void sm_context(int h, int x, int y) {
    const char *label, *a = sm_id_action(h, &label);
    if (!a || !strncmp(a, "lxmenu:", 7)) return;
    strlcpy(smctx_action, a, sizeof smctx_action); strlcpy(smctx_label, label, sizeof smctx_label); smctx_id = h;
    bool pinned = h >= 0 && h < sm_pinned_n;
    bool listed = h >= sm_pinned_n && h < SM_LEFT_N;
    menu_item_t items[4]; int n = 0;
    items[n++] = (menu_item_t){ "Open", 1, 0 };
    items[n++] = (menu_item_t){ "-", 0, 0 };
    if (pinned) items[n++] = (menu_item_t){ "Unpin from Start menu", 3, 0 };
    else items[n++] = (menu_item_t){ "Pin to Start menu", 2, 0 };
    if (listed) items[n++] = (menu_item_t){ "Remove from This List", 4, 0 };
    popup_menu(x, y, items, n, sm_ctx_cb, NULL);
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
int shell_cursor(int x, int y) {
    if (side_drag.active) return CURSOR_MOVE;
    if (startmenu_open || (sm_progress && pt_in(sm_rect(), x, y)) || y >= tb_y()) return CURSOR_ARROW;
    if (side_hot >= 700 && side_hot < 700 + ST_N) return CURSOR_MOVE;       /* a tile's title bar */
    return -1;
}
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
    if (sm_ignore_up && type == ME_UP && (btn & MB_LEFT)) { sm_ignore_up = false; return true; }
    if (!startmenu_open && sm_progress && y < tb_y() && pt_in(sm_rect(), x, y)) {
        if (type == ME_DOWN) { animation_set(&sm_motion, 0); sm_progress = 0; sm_ignore_up = true; wm_need_redraw = true; }
        return true;
    }
    if (startmenu_open) {
        int h = sm_hit(x, y);
        if (sm_progress < 255 && h != -1) {
            if (type == ME_DOWN) {
                animation_set(&sm_motion, 255); sm_progress = 255; sm_ignore_up = true; wm_need_redraw = true;
            }
            return true;
        }
        if (type == ME_MOVE) {
            int nh = h >= 0 ? h : -1;
            sm_kbd = false;
            if (h >= 600) {
                int lv = (h - 600) / 100, i = (h - 600) % 100;
                ap_entry_t *e = &ap_lv[lv].e[i];
                if (ap_hot_lv != lv || ap_hot != i) { ap_hot_lv = lv; ap_hot = i; wm_need_redraw = true; }
                if (e->folder) { if (ap_depth <= lv + 1 || ap_lv[lv + 1].from != i) ap_open(lv + 1, e->dir, i); }
                else if (ap_depth > lv + 1) { ap_depth = lv + 1; wm_need_redraw = true; }
                if (sm_hot != nh) { sm_hot = nh; wm_need_redraw = true; }
                return true;
            }
            if (nh != sm_hot) { sm_hot = nh; wm_need_redraw = true; }
            if (ap_hot != -1) { ap_hot = ap_hot_lv = -1; wm_need_redraw = true; }
            if (h == 300 || h == 500) { if (!allprog_open) ap_open(0, vfs_resolve(SM_PROGRAMS, NULL), 0); }
            else if (h >= 0 && allprog_open) { ap_close(); wm_need_redraw = true; }
            return h != -1 || y >= tb_y();
        }
        if (type == ME_DOWN) {
            if (h == -1) {
                startmenu_set(false);
                if (tb_hit(x, y) == 1000) return true;
                return y >= tb_y();
            }
            return true;
        }
        if (type == ME_UP && (btn & MB_LEFT)) { if (h >= 0) sm_activate(h); return true; }
        if (type == ME_UP && (btn & MB_RIGHT)) { if (h >= 0) sm_context(h, x, y); return true; }
        return true;
    }
    if (side_drag.pending) {                                   /* a tile's title bar was pressed */
        if (type == ME_MOVE) {
            side_drag.y = y;
            if (!side_drag.active && ABSDIFF(y, side_drag.y0) > 4) { side_drag.active = side_drag.settling = true; }
            if (side_drag.active) side_redraw();
            return true;
        }
        if (type == ME_UP) {
            if (side_drag.active) side_drop();
            side_drag.pending = side_drag.active = false;
            wm_need_redraw = true;
            return true;
        }
        return true;
    }
    if (side_progress && x >= side_x() && y < tb_y()) {
        /* The sidebar lives on the desktop; a window dragged over it is above
         * it.  Let that window take the click and hover instead of treating
         * the click as a sidebar activation. */
        if (wm_window_at_point(x, y)) {
            if (side_hot != -1) { side_hot = -1; wm_need_redraw = true; }
            if (type == ME_DOWN) side_search_focus = false;
            return false;
        }
        int sh = side_hit(x, y);
        if (type == ME_MOVE) { if (sh != side_hot) { side_hot = sh; wm_need_redraw = true; } return true; }
        if (type == ME_DOWN && (btn & MB_LEFT) && sidebar_open && sh >= 700 && sh < 700 + ST_N) {
            /* the title bar: the tile can be dragged to another place */
            int ty = 6 + SIDE_HDR + 8;
            for (int k = 0; k < ST_N; k++) { int t = side_order[k]; if (t == sh - 700) break; if (side_tile_visible(t)) ty += SIDE_TILE_HEAD + side_body_h(t) + 2 + SIDE_TILE_GAP; }
            side_drag.pending = true; side_drag.active = false; side_drag.t = sh - 700;
            side_drag.y = side_drag.y0 = y; side_drag.off = y - ty;
            return true;
        }
        if (type == ME_DOWN && (btn & MB_LEFT) && sidebar_open) { side_click(sh); return true; }
        return true;
    }
    if (side_hot != -1 && (!sidebar_open || !side_progress || x < side_x() || y >= tb_y())) { side_hot = -1; wm_need_redraw = true; }
    if (side_search_focus && type == ME_DOWN) { side_search_focus = false; wm_need_redraw = true; }
    int h = tb_hit(x, y);
    if (type == ME_MOVE) { int nh = h >= 0 ? h : -1; if (nh != tb_hot) { tb_hot = nh; wm_need_redraw = true; } return h != -1; }
    if (h == -1) return false;
    if (type == ME_DOWN && (btn & MB_LEFT)) {
        if (h == 4000) { sidebar_open = !sidebar_open; side_search_focus = false; shell_anim_frame = 0; wm_need_redraw = true; }
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
/* The Start menu from the keyboard: Up/Down move in a column, Left/Right between the columns
 * and into and out of All Programs and its folders, Enter opens, Esc closes a level or the menu,
 * a letter jumps to the next item starting with it. */
static int sm_col_ids(int col, int *ids) {                  /* the ids of a column, top to bottom */
    int n = 0;
    if (T.classic) { for (int i = 0; i < CL_N; i++) if (classic_items[i].label) ids[n++] = 500 + i; return n; }
    if (col == 0) { for (int i = 0; i < SM_LEFT_N; i++) if (sm_left[i].label) ids[n++] = i; ids[n++] = 300; ids[n++] = 400; }
    else { for (int i = 0; i < SM_RIGHT_N; i++) if (sm_right[i].label) ids[n++] = 100 + i; ids[n++] = 401; }
    return n;
}
static const char *sm_id_label(int h) {
    if (h >= 600) return ap_lv[(h - 600) / 100].e[(h - 600) % 100].label;
    if (h >= 500) return classic_items[h - 500].label;
    if (h >= 400) return h == 400 ? "Log Off" : "Turn Off Computer";
    if (h == 300) return "All Programs";
    if (h >= 100) return sm_right[h - 100].label;
    return h >= 0 && h < SM_LEFT_N ? sm_left[h].label : NULL;
}
static void sm_key(key_event_t *e) {
    sm_kbd = true;
    if (sm_progress < 255) { animation_set(&sm_motion, 255); sm_progress = 255; }
    wm_need_redraw = true;
    if (ap_depth && ap_hot_lv >= 0) {                        /* in All Programs */
        ap_level_t *L = &ap_lv[ap_hot_lv];
        int i = ap_hot, lv = ap_hot_lv;
        switch (e->key) {
        case KEY_DOWN: case KEY_UP: {
            int d = e->key == KEY_DOWN ? 1 : -1;
            for (int k = 0; k < L->n; k++) { i = (i + d + L->n) % L->n; if (L->e[i].label[0]) break; }
            ap_hot = i;
            if (ap_depth > lv + 1) ap_depth = lv + 1;          /* moving closes the folder beside */
            return;
        }
        case KEY_RIGHT: case KEY_ENTER:
            if (i >= 0 && i < L->n) {
                if (L->e[i].folder) { ap_open(lv + 1, L->e[i].dir, i); ap_hot_lv = lv + 1; ap_hot = 0; if (!ap_lv[lv + 1].n) ap_hot = -1; }
                else if (e->key == KEY_ENTER) sm_activate(600 + lv * 100 + i);
            }
            return;
        case KEY_LEFT: case KEY_ESC:
            if (lv > 0) { ap_hot = ap_lv[lv].from; ap_hot_lv = lv - 1; ap_depth = lv; }
            else { ap_close(); sm_hot = T.classic ? 500 : 300; }
            return;
        case KEY_MENU: { rect_t r = ap_item_rect_lv(lv, i); sm_context(600 + lv * 100 + i, r.x + 20, r.y + r.h / 2); return; }
        default:
            if (e->ch > 32) {                                   /* the next item starting with the letter */
                for (int k = 1; k <= L->n; k++) { int j = (i + k) % L->n; if (tolower((unsigned char)L->e[j].label[0]) == tolower((unsigned char)e->ch)) { ap_hot = j; break; } }
            }
            return;
        }
    }
    if (e->key == KEY_ESC) { startmenu_set(false); return; }
    int ids[40], col = sm_hot >= 100 && sm_hot < 200 ? 1 : sm_hot == 401 ? 1 : 0;
    int n = sm_col_ids(col, ids), at = -1;
    for (int k = 0; k < n; k++) if (ids[k] == sm_hot) at = k;
    switch (e->key) {
    case KEY_DOWN: sm_hot = ids[at < 0 ? 0 : (at + 1) % n]; return;
    case KEY_UP: sm_hot = ids[at < 0 ? n - 1 : (at + n - 1) % n]; return;
    case KEY_RIGHT:
        if (sm_hot == 300 || sm_hot == 500) { sm_activate(sm_hot); return; }
        if (!T.classic && col == 0) { int r[40]; sm_col_ids(1, r); sm_hot = r[0]; }
        return;
    case KEY_LEFT: if (!T.classic && col == 1) { int l[40]; sm_col_ids(0, l); sm_hot = l[0]; } return;
    case KEY_ENTER: case ' ': if (sm_hot >= 0) sm_activate(sm_hot); else sm_hot = ids[0]; return;
    case KEY_MENU: if (sm_hot >= 0) { rect_t r = sm_item_rect(sm_hot); sm_context(sm_hot, r.x + 30, r.y + r.h / 2); } return;
    default:
        if (e->ch > 32) {
            for (int k = 1; k <= n; k++) { const char *l = sm_id_label(ids[(at + k + n) % n]); if (l && tolower((unsigned char)l[0]) == tolower((unsigned char)e->ch)) { sm_hot = ids[(at + k + n) % n]; break; } }
        }
        return;
    }
}
/* Windows-key shortcuts: the key alone (pressed and let go) opens the Start menu; with another
 * key: D or M shows the desktop (and gives it the keyboard), E Explorer, R Run, F Search,
 * L the log-on screen... as in Windows */
static bool win_held, win_combo;
static void show_desktop(void) {
    for (int i = 0; i < wm_window_count(); i++) wm_minimize(wm_window_at(i));
    wm_focus(NULL);
    if (dsel < 0 && ndicons) dsel = 0;
    wm_need_redraw = true;
}
bool shell_key(key_event_t *e) {
    if (e->key == KEY_WIN) {
        if (e->pressed) { if (!win_held) { win_held = true; win_combo = false; } return true; }
        bool alone = win_held && !win_combo;
        win_held = false;
        if (alone) startmenu_toggle();
        return true;
    }
    if (!e->pressed) return false;
    if (win_held) {
        win_combo = true;
        switch (tolower((unsigned char)e->ch)) {
        case 'd': case 'm': if (startmenu_open) startmenu_set(false); show_desktop(); break;
        case 'e': app_launch("explorer", NULL); break;
        case 'r': shell_show_run(); break;
        case 'f': case 's': wrldsearch_open(NULL); break;
        case 'u': control_open("ease"); break;
        case 'b': case 't': show_desktop(); break;
        default: if (e->key == KEY_F1) app_launch("help", NULL); break;
        }
        return true;
    }
    if (sidebar_open && side_search_focus) {
        if (e->key == KEY_ESC) { side_search_focus = false; wm_need_redraw = true; return true; }
        if (e->key == KEY_BACKSPACE) { int l = strlen(side_query); if (l) side_query[l - 1] = 0; wm_need_redraw = true; return true; }
        if (e->key == KEY_ENTER) {                    /* Enter: everything that matches, in The WRLD Search */
            if (side_query[0]) wrldsearch_open(side_query);
            side_search_focus = false;
            wm_need_redraw = true; return true;
        }
        if (e->ch >= 32 && e->ch < 127) {
            int l = strlen(side_query);
            if (l < (int)sizeof side_query - 1) { side_query[l] = e->ch; side_query[l + 1] = 0; wm_need_redraw = true; }
            return true;
        }
        return true;
    }
    if (e->key == KEY_ESC && (e->mods & MOD_CTRL)) { startmenu_toggle(); return true; }
    if (startmenu_open) { sm_key(e); return true; }
    if (!wm_active && ndicons) {                       /* the desktop has the keyboard: arrows move between icons */
        int rows = (SCREEN_H - T.taskbar_h - 10) / DCELL_H, ns = dsel;
        if (rows < 1) rows = 1;
        switch (e->key) {
        case KEY_DOWN: ns = dsel < 0 ? 0 : MIN(ndicons - 1, dsel + 1); break;
        case KEY_UP: ns = dsel < 0 ? 0 : MAX(0, dsel - 1); break;
        case KEY_RIGHT: ns = dsel < 0 ? 0 : (dsel + rows < ndicons ? dsel + rows : dsel); break;
        case KEY_LEFT: ns = dsel < 0 ? 0 : (dsel - rows >= 0 ? dsel - rows : dsel); break;
        case KEY_HOME: ns = 0; break;
        case KEY_END: ns = ndicons - 1; break;
        case KEY_TAB: ns = dsel < 0 ? 0 : dsel; break;
        default: ns = -2; break;
        }
        if (ns != -2) { dsel = ns; wm_need_redraw = true; return true; }
        if (e->key == KEY_MENU || (e->key == KEY_F10 && (e->mods & MOD_SHIFT))) {   /* the context menu key */
            rect_t r = dsel >= 0 ? dicon_rect(dsel) : R(SCREEN_W / 2, SCREEN_H / 2, 0, 0);
            desktop_mouse(ME_DOWN, r.x + r.w / 2, r.y + r.h / 2, MB_RIGHT);
            return true;
        }
    }
    if (!wm_active && (e->key == KEY_ENTER) && dsel >= 0) { dicon_open(dsel); return true; }
    if (!wm_active && e->key == KEY_DELETE && dsel >= 0 && dicons[dsel].node) { desk_ctx_cb((void *)(uintptr_t)dsel, 2); return true; }
    if (!wm_active && e->key == KEY_F2 && dsel >= 0 && dicons[dsel].node) { desk_ctx_cb((void *)(uintptr_t)dsel, 3); return true; }
    if (!wm_active && e->key == KEY_F5) { desktop_refresh(); return true; }
    return false;
}
static char last_clock[32];
void shell_tick(void) {
    shell_shutdown_tick();
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
void shell_reset(void) {
    startmenu_open = false; ap_close();
    sm_hot = -1;
    side_hot = -1;
    side_search_focus = false;
    shell_effects_changed();
    shell_init();
    wm_need_redraw = true;
}
