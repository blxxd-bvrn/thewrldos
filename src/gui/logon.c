/* The welcome (sign-in) screen and logon skins.
 *
 * Two layouts share one loop: the sign-in screen (the default) and the
 * XP-style classic screen. A skin (.wrldlogon in LOGON_SKIN_DIR) chooses the
 * layout and its colours, text and an optional background picture; Logon Skin
 * Maker writes skins and Logon Skin Themer chooses one. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "audio.h"
#include "fs.h"
#include "usb.h"
#include "logon.h"

/* ---------------- colour roles ---------------- */
static const struct { const char *key, *signin, *classic; } roles[LC_COUNT] = {
    [LC_BG_TOP]        = { "bg_top", "Background (top)", "Top and bottom bars (dark)" },
    [LC_BG_BOTTOM]     = { "bg_bottom", "Background (bottom)", "Top and bottom bars (light)" },
    [LC_GLOW1]         = { "glow1", "Glow (left and bottom)", "Centre glow" },
    [LC_GLOW2]         = { "glow2", "Glow (centre)", "Edge colour" },
    [LC_GLOW3]         = { "glow3", "Glow (right) and top tab", "Top line" },
    [LC_ACCENT]        = { "accent", "Power icon", "Bottom line" },
    [LC_TEXT]          = { "text", "Text", "Text" },
    [LC_SUBTEXT]       = { "subtext", "Title and dim text", "Hints" },
    [LC_PANEL_TOP]     = { "panel_top", "Panel (top)", "Highlight (left)" },
    [LC_PANEL_BOTTOM]  = { "panel_bottom", "Panel (bottom)", "Highlight (right)" },
    [LC_PANEL_TEXT]    = { "panel_text", "Panel text", NULL },
    [LC_BUTTON_TOP]    = { "button_top", "Button (top)", "Picture frame (pointed at)" },
    [LC_BUTTON_BOTTOM] = { "button_bottom", "Button (bottom)", "Picture frame" },
    [LC_BUTTON_TEXT]   = { "button_text", "Button text", NULL },
};
const char *logon_color_key(int r) { return r >= 0 && r < LC_COUNT ? roles[r].key : ""; }
const char *logon_color_label(int r, int layout) {
    if (r < 0 || r >= LC_COUNT) return NULL;
    return layout == LOGON_LAYOUT_CLASSIC ? roles[r].classic : roles[r].signin;
}
const char *logon_layout_name(int l) { return l == LOGON_LAYOUT_CLASSIC ? "Classic (XP)" : "Sign-in screen"; }

void logon_skin_defaults(logon_skin_t *s, int layout) {
    memset(s, 0, sizeof *s);
    s->layout = layout == LOGON_LAYOUT_CLASSIC ? LOGON_LAYOUT_CLASSIC : LOGON_LAYOUT_SIGNIN;
    s->show_clock = s->show_guest = s->show_help = s->show_globe = true;
    uint32_t *c = s->col;
    if (s->layout == LOGON_LAYOUT_CLASSIC) {
        strlcpy(s->name, "Classic", sizeof s->name);
        strlcpy(s->title, "welcome", sizeof s->title);
        strlcpy(s->subtitle, "To begin, click your user name", sizeof s->subtitle);
        c[LC_BG_TOP] = HEX(0x00309C); c[LC_BG_BOTTOM] = HEX(0x003CB4);
        c[LC_GLOW1] = HEX(0x7AA0F0); c[LC_GLOW2] = HEX(0x5A7EDC); c[LC_GLOW3] = HEX(0xB8D0FF);
        c[LC_ACCENT] = HEX(0xE88A30); c[LC_TEXT] = WHITE; c[LC_SUBTEXT] = HEX(0xD8E4F8);
        c[LC_PANEL_TOP] = HEX(0x00309C); c[LC_PANEL_BOTTOM] = HEX(0x5A7EDC); c[LC_PANEL_TEXT] = WHITE;
        c[LC_BUTTON_TOP] = HEX(0xFFC830); c[LC_BUTTON_BOTTOM] = WHITE; c[LC_BUTTON_TEXT] = HEX(0x0A2A8C);
    } else {
        strlcpy(s->name, "Sign In", sizeof s->name);
        strlcpy(s->title, "welcome to", sizeof s->title);
        strlcpy(s->subtitle, "Click on your name to sign in", sizeof s->subtitle);
        strlcpy(s->brand, OS_NAME, sizeof s->brand);
        c[LC_BG_TOP] = HEX(0x0A1CA6); c[LC_BG_BOTTOM] = HEX(0x1A36C4);
        c[LC_GLOW1] = HEX(0x3A62F2); c[LC_GLOW2] = HEX(0x8A72C4); c[LC_GLOW3] = HEX(0x7C9ACC);
        c[LC_ACCENT] = HEX(0xD8442A); c[LC_TEXT] = WHITE; c[LC_SUBTEXT] = HEX(0xC4D2FF);
        c[LC_PANEL_TOP] = HEX(0xDCEBFF); c[LC_PANEL_BOTTOM] = HEX(0x86ACF0); c[LC_PANEL_TEXT] = HEX(0x08123C);
        c[LC_BUTTON_TOP] = WHITE; c[LC_BUTTON_BOTTOM] = HEX(0xC4D6F6); c[LC_BUTTON_TEXT] = HEX(0x0A2A8C);
    }
    strlcpy(s->file, s->name, sizeof s->file);
}

/* ---------------- skin files ---------------- */
#define MAX_SKINS 32
static logon_skin_t skins[MAX_SKINS];
static int nskins;
static bool skins_loaded;
static const char *const builtin_skins[] = { "Sign In", "XP Classic", "Midnight", "Sunrise", "Olive Classic" };

static uint32_t parse_col(const char *v) {
    while (*v == ' ') v++;
    if (*v == '#') v++;
    return 0xFF000000u | (strtoul(v, NULL, 16) & 0xFFFFFF);
}
static bool parse_bool(const char *v) { return *v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T'; }

/* the value of `key` in an INI text, or NULL */
static bool ini_find(const char *txt, const char *key, char *out, int cap) {
    int kl = strlen(key);
    for (const char *line = txt; *line;) {
        const char *e = line; while (*e && *e != '\n') e++;
        const char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, key, kl)) {
            const char *q = p + kl; while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') {
                q++; while (*q == ' ' || *q == '\t') q++;
                int n = 0; while (q < e && *q != '\r' && n < cap - 1) out[n++] = *q++;
                while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
                out[n] = 0;
                return true;
            }
        }
        line = *e ? e + 1 : e;
    }
    return false;
}
static bool parse_skin(const char *file, logon_skin_t *s) {
    char path[200]; snprintf(path, sizeof path, LOGON_SKIN_DIR "\\%s." LOGON_SKIN_EXT, file);
    uint32_t len;
    char *raw = (char *)vfs_read_file(path, &len);
    if (!raw) return false;
    char *txt = kmalloc(len + 1);
    if (!txt) { kfree(raw); return false; }
    memcpy(txt, raw, len); txt[len] = 0; kfree(raw);
    char v[200];
    int layout = ini_find(txt, "Layout", v, sizeof v) && !strcasecmp(v, "classic") ? LOGON_LAYOUT_CLASSIC : LOGON_LAYOUT_SIGNIN;
    logon_skin_defaults(s, layout);
    strlcpy(s->file, file, sizeof s->file);
    strlcpy(s->name, file, sizeof s->name);
    if (ini_find(txt, "Name", v, sizeof v) && *v) strlcpy(s->name, v, sizeof s->name);
    if (ini_find(txt, "Description", v, sizeof v)) strlcpy(s->desc, v, sizeof s->desc);
    if (ini_find(txt, "Title", v, sizeof v)) strlcpy(s->title, v, sizeof s->title);
    if (ini_find(txt, "Subtitle", v, sizeof v)) strlcpy(s->subtitle, v, sizeof s->subtitle);
    if (ini_find(txt, "Brand", v, sizeof v)) strlcpy(s->brand, v, sizeof s->brand);
    if (ini_find(txt, "Image", v, sizeof v)) strlcpy(s->image, v, sizeof s->image);
    if (ini_find(txt, "ShowClock", v, sizeof v)) s->show_clock = parse_bool(v);
    if (ini_find(txt, "ShowGuest", v, sizeof v)) s->show_guest = parse_bool(v);
    if (ini_find(txt, "ShowHelp", v, sizeof v)) s->show_help = parse_bool(v);
    if (ini_find(txt, "ShowGlobe", v, sizeof v)) s->show_globe = parse_bool(v);
    for (int i = 0; i < LC_COUNT; i++) if (ini_find(txt, roles[i].key, v, sizeof v) && *v) s->col[i] = parse_col(v);
    kfree(txt);
    return true;
}
static void add_skin(const char *file, bool builtin) {
    if (nskins >= MAX_SKINS) return;
    for (int i = 0; i < nskins; i++) if (!strcasecmp(skins[i].file, file)) return;
    logon_skin_t *s = &skins[nskins];
    if (!parse_skin(file, s)) {
        if (!builtin) return;
        /* a shipped skin whose file was removed: the two layouts still work */
        logon_skin_defaults(s, !strcasecmp(file, "XP Classic") ? LOGON_LAYOUT_CLASSIC : LOGON_LAYOUT_SIGNIN);
        if (strcasecmp(file, "Sign In") && strcasecmp(file, "XP Classic")) return;
        strlcpy(s->file, file, sizeof s->file); strlcpy(s->name, file, sizeof s->name);
    }
    s->builtin = builtin;
    nskins++;
}
void logon_skins_load(void) {
    nskins = 0;
    skins_loaded = true;
    for (unsigned i = 0; i < ARRAY_LEN(builtin_skins); i++) add_skin(builtin_skins[i], true);
    vnode_t *d = vfs_resolve(LOGON_SKIN_DIR, NULL);
    if (d) {
        vfs_sort_children(d);
        for (vnode_t *n = d->child; n; n = n->next) {
            if (n->dir || strcasecmp(vfs_ext(n->name), LOGON_SKIN_EXT)) continue;
            char f[64]; strlcpy(f, n->name, sizeof f);
            char *dot = strrchr(f, '.'); if (dot) *dot = 0;
            add_skin(f, false);
        }
    }
}
int logon_skin_count(void) { if (!skins_loaded) logon_skins_load(); return nskins; }
const logon_skin_t *logon_skin_get(int i) { if (!skins_loaded) logon_skins_load(); return &skins[CLAMP(i, 0, nskins - 1)]; }
int logon_skin_find(const char *name) {
    if (!name || !*name) return -1;
    int n = logon_skin_count();
    for (int i = 0; i < n; i++) if (!strcasecmp(skins[i].file, name) || !strcasecmp(skins[i].name, name)) return i;
    return -1;
}
int logon_skin_current_index(void) {
    int i = logon_skin_find(settings_get("logon_skin"));
    if (i < 0) i = logon_skin_find(LOGON_DEFAULT_SKIN);
    return i < 0 ? 0 : i;
}
const logon_skin_t *logon_skin_current(void) { return logon_skin_get(logon_skin_current_index()); }
void logon_skin_set_current(const char *file) { settings_set("logon_skin", file); settings_save(); }

static void sanitize_file(const char *name, char *out, int cap) {
    int n = 0;
    for (const char *p = name; *p && n < cap - 1; p++) {
        char ch = *p;
        if (strchr("\\/:*?\"<>|", ch) || (uint8_t)ch < 32) ch = '_';
        out[n++] = ch;
    }
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;
    out[n] = 0;
    if (!n) strlcpy(out, "My Skin", cap);
}
bool logon_skin_save(const logon_skin_t *s) {
    char file[64]; sanitize_file(s->file[0] ? s->file : s->name, file, sizeof file);
    char *t = kmalloc(4096);
    if (!t) return false;
    int o = snprintf(t, 4096,
        "; THE WRLD OS logon skin, made with Logon Skin Maker. You can also edit it here.\r\n"
        "; Layout is signin or classic. Colours are #RRGGBB. Image is a PNG, JPEG, GIF or BMP\r\n"
        "; path (leave it empty for the drawn background).\r\n\r\n"
        "[Skin]\r\nName=%s\r\nDescription=%s\r\nLayout=%s\r\n\r\n"
        "[Text]\r\nTitle=%s\r\nSubtitle=%s\r\nBrand=%s\r\n\r\n"
        "[Background]\r\nImage=%s\r\n\r\n"
        "[Options]\r\nShowClock=%d\r\nShowGuest=%d\r\nShowHelp=%d\r\nShowGlobe=%d\r\n\r\n[Colors]\r\n",
        s->name, s->desc, s->layout == LOGON_LAYOUT_CLASSIC ? "classic" : "signin",
        s->title, s->subtitle, s->brand, s->image,
        s->show_clock, s->show_guest, s->show_help, s->show_globe);
    for (int i = 0; i < LC_COUNT && o < 4000; i++) o += snprintf(t + o, 4096 - o, "%s=#%06X\r\n", roles[i].key, s->col[i] & 0xFFFFFF);
    vfs_mkdir_p(LOGON_SKIN_DIR);
    char path[200]; snprintf(path, sizeof path, LOGON_SKIN_DIR "\\%s." LOGON_SKIN_EXT, file);
    bool ok = vfs_write_file(path, t, strlen(t)) != NULL;
    kfree(t);
    if (ok) logon_skins_load();
    return ok;
}
bool logon_skin_delete(const char *file) {
    char path[200]; snprintf(path, sizeof path, LOGON_SKIN_DIR "\\%s." LOGON_SKIN_EXT, file);
    vnode_t *n = vfs_resolve(path, NULL);
    bool ok = n && vfs_delete(n);
    logon_skins_load();
    return ok;
}

/* ---------------- backgrounds ---------------- */
/* Backgrounds are cached: the screen redraws on every key and pointer move. */
typedef struct { canvas_t *c; uint32_t key; } bgcache_t;
static bgcache_t bgc[3];
static int bgc_next;

static uint32_t skin_key(const logon_skin_t *s, int w, int h) {
    uint32_t k = 2166136261u;
#define MIX(v) do { k ^= (uint32_t)(v); k *= 16777619u; } while (0)
    MIX(s->layout); MIX(w); MIX(h);
    for (int i = 0; i < LC_COUNT; i++) MIX(s->col[i]);
    for (const char *p = s->image; *p; p++) MIX(*p);
#undef MIX
    return k;
}

typedef struct { int cx, cy, rx, ry; uint32_t col; int a; } blob_t;
static uint32_t signin_sample(const logon_skin_t *s, int W, int H, int x, int y, const blob_t *b, int nb) {
    int t = CLAMP((y * 3 * 255 / MAX(H, 1) + x * 255 / MAX(W, 1)) / 4, 0, 255);
    uint32_t c = mix(s->col[LC_BG_TOP], s->col[LC_BG_BOTTOM], t);
    for (int i = 0; i < nb; i++) {
        int64_t dx = x - b[i].cx, dy = y - b[i].cy;
        int64_t d = dx * dx * 65536 / ((int64_t)b[i].rx * b[i].rx) + dy * dy * 65536 / ((int64_t)b[i].ry * b[i].ry);
        if (d >= 65536) continue;
        int64_t f = 65536 - d;
        f = f * f >> 16;                                 /* soft edge */
        c = mix(c, b[i].col, (int)(f * b[i].a >> 16));
    }
    return c;
}
static void signin_bg(canvas_t *c, const logon_skin_t *s) {
    int W = c->w, H = c->h;
    blob_t b[6] = {
        { W * 10 / 100, H * 18 / 100, W * 44 / 100, H * 56 / 100, s->col[LC_GLOW1], 175 },
        { W * 12 / 100, H * 78 / 100, W * 26 / 100, H * 30 / 100, darken(s->col[LC_BG_TOP], 30), 120 },
        { W * 46 / 100, H * 106 / 100, W * 42 / 100, H * 40 / 100, s->col[LC_GLOW1], 150 },
        { W * 62 / 100, H * 40 / 100, W * 26 / 100, H * 30 / 100, s->col[LC_GLOW2], 175 },
        { W * 102 / 100, H * 62 / 100, W * 24 / 100, H * 44 / 100, s->col[LC_GLOW3], 200 },
        { W * 84 / 100, H * 4 / 100, W * 30 / 100, H * 22 / 100, s->col[LC_GLOW2], 70 },
    };
    /* the glows are smooth, so sample every 8th pixel and interpolate */
    const int G = 8;
    int gw = W / G + 2, gh = H / G + 2;
    uint32_t *g = kmalloc((size_t)gw * gh * 4);
    if (!g) { gradient_v(c, 0, 0, W, H, s->col[LC_BG_TOP], s->col[LC_BG_BOTTOM]); return; }
    for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) g[j * gw + i] = signin_sample(s, W, H, i * G, j * G, b, 6);
    for (int y = 0; y < H; y++) {
        int j = y / G, fy = (y % G) * 256 / G;
        uint32_t *row = c->px + y * c->stride;
        for (int x = 0; x < W; x++) {
            int i = x / G, fx = (x % G) * 256 / G;
            uint32_t a = g[j * gw + i], bb = g[j * gw + i + 1], cc = g[(j + 1) * gw + i], d = g[(j + 1) * gw + i + 1];
            int r = ((C_R(a) * (256 - fx) + C_R(bb) * fx) * (256 - fy) + (C_R(cc) * (256 - fx) + C_R(d) * fx) * fy) >> 16;
            int gg = ((C_G(a) * (256 - fx) + C_G(bb) * fx) * (256 - fy) + (C_G(cc) * (256 - fx) + C_G(d) * fx) * fy) >> 16;
            int bl = ((C_B(a) * (256 - fx) + C_B(bb) * fx) * (256 - fy) + (C_B(cc) * (256 - fx) + C_B(d) * fx) * fy) >> 16;
            /* a little ordered dither keeps the wide gradients from banding */
            int dth = ((x ^ y) & 1) - ((x >> 1 ^ y) & 1);
            row[x] = RGB(CLAMP(r + dth, 0, 255), CLAMP(gg + dth, 0, 255), CLAMP(bl + dth, 0, 255));
        }
    }
    kfree(g);
}
#define CLASSIC_BAR 76
static void classic_bg(canvas_t *c, const logon_skin_t *s) {
    int W = c->w, H = c->h, top = CLASSIC_BAR, bot = CLASSIC_BAR;
    const uint32_t *k = s->col;
    gradient_v(c, 0, 0, W, top, k[LC_BG_TOP], k[LC_BG_BOTTOM]);
    gradient_h(c, 0, top - 2, W / 2, 2, k[LC_GLOW2], k[LC_GLOW3]);
    gradient_h(c, W / 2, top - 2, W - W / 2, 2, k[LC_GLOW3], k[LC_GLOW2]);
    for (int y = top; y < H - bot; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - W / 4, dy = y - H / 3;
            int d = (dx * dx + dy * dy) / 900;
            c->px[y * c->stride + x] = mix(k[LC_GLOW1], k[LC_GLOW2], MIN(255, d));
        }
    gradient_v(c, 0, H - bot, W, bot, k[LC_BG_BOTTOM], k[LC_BG_TOP]);
    gradient_h(c, 0, H - bot, W / 2, 2, k[LC_ACCENT], lighten(k[LC_ACCENT], 90));
    gradient_h(c, W / 2, H - bot, W - W / 2, 2, lighten(k[LC_ACCENT], 90), k[LC_ACCENT]);
}
/* a picture, scaled to cover the whole canvas */
static bool image_bg(canvas_t *c, const char *path) {
    uint32_t n; uint8_t *d = vfs_read_file(path, &n);
    if (!d) return false;
    char err[64];
    canvas_t *im = image_decode(d, n, err, sizeof err);
    kfree(d);
    if (!im) { klog("logon: %s: %s", path, err); return false; }
    int w = c->w, h = im->h * c->w / MAX(im->w, 1);
    if (h < c->h) { h = c->h; w = im->w * c->h / MAX(im->h, 1); }
    rect_t old = canvas_push_clip(c, R(0, 0, c->w, c->h));
    blit_scaled_alpha(c, R((c->w - w) / 2, (c->h - h) / 2, w, h), im, 255);
    c->clip = old;
    canvas_free(im);
    return true;
}
void logon_background(canvas_t *c, const logon_skin_t *s) {
    uint32_t key = skin_key(s, c->w, c->h);
    for (int i = 0; i < 3; i++)
        if (bgc[i].c && bgc[i].key == key && bgc[i].c->w == c->w && bgc[i].c->h == c->h) {
            for (int y = 0; y < c->h; y++) memcpy(c->px + y * c->stride, bgc[i].c->px + y * bgc[i].c->stride, (size_t)c->w * 4);
            return;
        }
    canvas_t *n = canvas_new(c->w, c->h);
    canvas_t *dst = n ? n : c;
    if (s->layout == LOGON_LAYOUT_CLASSIC) classic_bg(dst, s); else signin_bg(dst, s);
    if (s->image[0]) image_bg(dst, s->image);
    if (!n) return;
    bgcache_t *e = &bgc[bgc_next]; bgc_next = (bgc_next + 1) % 3;
    if (e->c) canvas_free(e->c);
    e->c = n; e->key = key;
    for (int y = 0; y < c->h; y++) memcpy(c->px + y * c->stride, n->px + y * n->stride, (size_t)c->w * 4);
}

/* ---------------- shared pieces ---------------- */
#define DIVIDER_PNG "C:\\WRLD\\Resources\\Logon Divider.png"
static canvas_t *divider_img;
static bool divider_tried;
static void draw_divider(canvas_t *c, int x, int y0, int h) {
    if (!divider_tried) {
        divider_tried = true;
        uint32_t n; uint8_t *d = vfs_read_file(DIVIDER_PNG, &n);
        if (d) {
            char err[64];
            divider_img = image_decode(d, n, err, sizeof err);
            if (!divider_img) klog("logon: %s: %s", DIVIDER_PNG, err);
            kfree(d);
        }
    }
    if (!divider_img || divider_img->h <= 0 || h <= 0) {
        for (int y = 0; y < h; y++) {
            int dd = ABSDIFF(2 * y, h) * 255 / MAX(h, 1);
            blend_pixel(c, x, y0 + y, WHITE, 216 - 216 * MIN(255, dd) / 255);
        }
        return;
    }
    canvas_t *im = divider_img;
    for (int y = 0; y < h; y++) {
        int sy = (int)((int64_t)y * im->h / h);
        for (int i = 0; i < im->w; i++) {
            uint32_t p = im->px[sy * im->stride + i];
            int a = p >> 24;
            if (a) blend_pixel(c, x - im->w / 2 + i, y0 + y, p | 0xFF000000u, a);
        }
    }
}
static void underline(canvas_t *c, int font, int x, int y, const char *s, int n, uint32_t col) {
    hline(c, x, y + font_ascent(font) + 2, text_width_n(font, s, n), col);
}
/* a rounded "pill" button with the first letter underlined, as access keys were shown */
static void pill(canvas_t *c, rect_t r, const char *label, const logon_skin_t *s, bool hot) {
    uint32_t top = s->col[LC_BUTTON_TOP], bot = s->col[LC_BUTTON_BOTTOM], tx = s->col[LC_BUTTON_TEXT];
    if (hot) { top = lighten(top, 30); bot = mix(bot, s->col[LC_GLOW1], 60); }
    int rad = r.h / 2;
    fill_round_rect_blend(c, r.x + 1, r.y + 2, r.w, r.h, rad, BLACK, 50);
    fill_round_rect(c, r.x, r.y, r.w, r.h, rad, mix(tx, bot, 110));
    gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad - 1, top, bot);
    fill_round_rect_blend(c, r.x + 3, r.y + 2, r.w - 6, (r.h - 4) / 2, rad - 2, WHITE, 70);
    int tw = text_width(FONT_UI_BOLD, label);
    int x = r.x + (r.w - tw) / 2, y = r.y + (r.h - font_height(FONT_UI_BOLD)) / 2;
    draw_text(c, FONT_UI_BOLD, x, y, label, tx);
    underline(c, FONT_UI_BOLD, x, y, label, 1, tx);
}
/* the account picture in a white frame with a soft shadow */
static void framed_picture(canvas_t *c, int x, int y, int size, uint32_t frame, int alpha) {
    fill_round_rect_blend(c, x + 3, y + 4, size + 4, size + 4, 5, BLACK, 70 * alpha / 255);
    fill_round_rect_blend(c, x, y, size + 4, size + 4, 5, frame, alpha);
    if (alpha >= 250) draw_user_picture(c, user_picture, size, x + 2, y + 2);
    else {
        /* dimmed: draw it into a scratch canvas and blend */
        canvas_t *t = canvas_new(size, size);
        if (!t) return;
        for (int i = 0; i < size * size; i++) t->px[i] = 0;
        draw_user_picture(t, user_picture, size, 0, 0);
        for (int i = 0; i < size * size; i++) if (t->px[i]) t->px[i] = (t->px[i] & 0xFFFFFF) | ((uint32_t)alpha << 24);
        blit_alpha(c, x + 2, y + 2, t, 255);
        canvas_free(t);
    }
}
static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                              "August", "September", "October", "November", "December" };
static void format_clock(bool preview, char *tm, int tcap, char *dt, int dcap) {
    rtc_time_t t;
    if (preview) { memset(&t, 0, sizeof t); t.hour = 10; t.minute = 32; t.year = 2003; t.month = 3; t.day = 25; }
    else rtc_now(&t);
    if (clock_24h) snprintf(tm, tcap, "%02d:%02d", t.hour, t.minute);
    else snprintf(tm, tcap, "%d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour < 12 ? "AM" : "PM");
    snprintf(dt, dcap, "%s %d, %d", month_names[CLAMP(t.month, 1, 12) - 1], t.day, t.year);
}

/* ---------------- the sign-in layout ---------------- */
static void glass_panel(canvas_t *c, rect_t r, int fade, const logon_skin_t *s) {
    uint32_t top = s->col[LC_PANEL_TOP], bot = s->col[LC_PANEL_BOTTOM];
    int rad = 8;
    for (int j = 0; j < r.h; j++) {
        uint32_t col = mix(top, bot, j * 255 / MAX(1, r.h - 1));
        for (int i = 0; i < r.w; i++) {
            int a = 235;
            if (i > r.w - fade) a = a * (r.w - i) / MAX(1, fade);
            /* round the left corners */
            if (i < rad && (j < rad || j >= r.h - rad)) {
                int dx = rad - i, dy = j < rad ? rad - j : j - (r.h - rad - 1);
                if (dx * dx + dy * dy > rad * rad) continue;
            }
            blend_pixel(c, r.x + i, r.y + j, col, a);
        }
    }
    /* thin white edge, fading out with the panel */
    for (int i = rad; i < r.w; i++) {
        int a = 200; if (i > r.w - fade) a = a * (r.w - i) / MAX(1, fade);
        blend_pixel(c, r.x + i, r.y, WHITE, a);
        blend_pixel(c, r.x + i, r.y + r.h - 1, WHITE, a * 3 / 4);
    }
    for (int j = rad; j < r.h - rad; j++) blend_pixel(c, r.x, r.y + j, WHITE, 200);
    for (int k = 0; k < rad; k++) {     /* corner arcs */
        int dy = rad; while (dy > 0 && (rad - k) * (rad - k) + (rad - dy + 1) * (rad - dy + 1) <= rad * rad) dy--;
        blend_pixel(c, r.x + k, r.y + dy, WHITE, 200);
        blend_pixel(c, r.x + k, r.y + r.h - 1 - dy, WHITE, 160);
    }
    /* a glossy band across the top half */
    for (int j = 1; j < r.h / 2; j++) for (int i = 1; i < r.w - fade; i++) {
        if (i < rad && j < rad) { int dx = rad - i, dy = rad - j; if (dx * dx + dy * dy > rad * rad) continue; }
        blend_pixel(c, r.x + i, r.y + j, WHITE, 40 - 40 * j / (r.h / 2));
    }
}
static void power_glyph(canvas_t *c, int cx, int cy, uint32_t col, uint32_t bg) {
    draw_circle(c, cx, cy, 5, col);
    fill_rect(c, cx - 2, cy - 6, 5, 4, bg);              /* gap at the top for the stroke */
    vline(c, cx, cy - 7, 6, col);
}
static void signin_render(canvas_t *c, const logon_skin_t *s, logon_state_t *st) {
    int W = c->w, H = c->h;
    const uint32_t *k = s->col;
    logon_background(c, s);

    /* top right: the Power Off / Help and Settings tab */
    const char *pl = "Power Off", *hl = "Help and Settings";
    int hw = s->show_help ? text_width(FONT_UI_BOLD, hl) : 0, pw = text_width(FONT_UI_BOLD, pl);
    int hx = W - 14 - hw;                                /* help label */
    int px = s->show_help ? hx - 22 - 30 - pw : W - 14 - pw;
    int tab_x = px - 22 - 26;
    for (int y = 0; y < 30; y++) {
        int lx = tab_x + (30 - y) * (30 - y) / 30;       /* curved, slanting left edge */
        uint32_t col = mix(mix(k[LC_GLOW3], WHITE, 70), mix(k[LC_BG_BOTTOM], k[LC_GLOW3], 140), y * 255 / 29);
        for (int x = lx; x < W; x++) blend_pixel(c, x, y, col, x < lx + 2 ? 120 : 215);
    }
    for (int x = tab_x; x < W; x++) { blend_pixel(c, x, 30, WHITE, 140); blend_pixel(c, x, 31, BLACK, 50); }
    st->hit[HIT_POWER] = R(px - 22, 2, pw + 26, 26);
    fill_circle(c, px - 12, 14, 8, mix(k[LC_ACCENT], WHITE, st->hot == HIT_POWER ? 60 : 0));
    draw_circle(c, px - 12, 14, 8, darken(k[LC_ACCENT], 60));
    power_glyph(c, px - 12, 15, WHITE, mix(k[LC_ACCENT], WHITE, st->hot == HIT_POWER ? 60 : 0));
    uint32_t lc = st->hot == HIT_POWER ? mix(k[LC_TEXT], HEX(0xFFE08A), 160) : k[LC_TEXT];
    draw_text(c, FONT_UI_BOLD, px, 8, pl, lc); underline(c, FONT_UI_BOLD, px, 8, pl, -1, lc);
    if (s->show_help) {
        st->hit[HIT_HELP] = R(hx - 22, 2, hw + 26, 26);
        fill_circle(c, hx - 12, 14, 8, st->hot == HIT_HELP || st->help ? HEX(0xFFE8A0) : WHITE);
        draw_circle(c, hx - 12, 14, 8, mix(k[LC_BG_TOP], WHITE, 90));
        draw_text(c, FONT_UI_BOLD, hx - 15, 8, "?", k[LC_BG_TOP]);
        lc = st->hot == HIT_HELP ? mix(k[LC_TEXT], HEX(0xFFE08A), 160) : k[LC_TEXT];
        draw_text(c, FONT_UI_BOLD, hx, 8, hl, lc); underline(c, FONT_UI_BOLD, hx, 8, hl, -1, lc);
    }

    /* the clock */
    if (s->show_clock) {
        char tm[24], dt[40]; format_clock(st->preview, tm, sizeof tm, dt, sizeof dt);
        int tw = text_width(FONT_HUGE, tm), x = W - 18 - tw;
        draw_text(c, FONT_HUGE, x + 1, 43, tm, mix(k[LC_BG_TOP], BLACK, 120));
        draw_text(c, FONT_HUGE, x, 42, tm, k[LC_TEXT]);
        draw_text(c, FONT_UI, x, 76, dt, k[LC_TEXT]);
    }

    /* top left: title, logo, subtitle */
    if (s->title[0]) {
        draw_text(c, FONT_DISPLAY_BI, 17, 7, s->title, mix(k[LC_BG_TOP], BLACK, 100));
        draw_text(c, FONT_DISPLAY_BI, 16, 6, s->title, k[LC_SUBTEXT]);
    }
    int lx = 66;
    if (s->show_globe) { draw_globe(c, lx, 42, 64, -36 + st->frame * 2); lx += 70; }
    draw_wordmark(c, lx, 50, WORDMARK_MEDIUM, k[LC_TEXT]);
    if (s->subtitle[0]) draw_text(c, FONT_WEB_I, lx + 2, 98, s->subtitle, k[LC_TEXT]);

    /* the account */
    int ty = MAX(146, H * 19 / 100);
    if (!st->selected) {
        bool hot = st->hot == HIT_USER;
        st->hit[HIT_USER] = R(80, ty - 6, 280, 64);
        framed_picture(c, 88, ty, 48, hot ? WHITE : mix(k[LC_BG_BOTTOM], WHITE, 170), hot ? 255 : 200);
        uint32_t nc = hot ? k[LC_TEXT] : mix(k[LC_BG_BOTTOM], k[LC_TEXT], 170);
        draw_text(c, FONT_UI_BOLD, 148, ty + 6, user_name, nc);
        draw_text(c, FONT_UI, 148, ty + 21, user_password[0] ? "Password protected" : "Click to sign in",
                  hot ? k[LC_SUBTEXT] : mix(k[LC_BG_BOTTOM], k[LC_SUBTEXT], 150));
    } else {
        rect_t pr = R(78, ty - 8, 320, 70);
        glass_panel(c, pr, 70, s);
        st->hit[HIT_USER] = R(pr.x, pr.y, 62, pr.h);
        framed_picture(c, 88, ty, 48, WHITE, 255);
        uint32_t pt = k[LC_PANEL_TEXT];
        draw_text(c, FONT_UI_BOLD, 148, ty - 1, user_name, pt);
        char on[64]; snprintf(on, sizeof on, "on %s", computer_name);
        draw_text(c, FONT_UI, 148, ty + 12, on, mix(pt, k[LC_PANEL_BOTTOM], 60));
        if (st->logging_in) {
            draw_text(c, FONT_UI_BOLD, 148, ty + 32, "Signing in", pt);
            int dx = 148 + text_width(FONT_UI_BOLD, "Signing in") + 8;
            for (int i = 0; i < 3; i++) fill_circle(c, dx + i * 10, ty + 39, 3, ((st->frame / 6) % 3) == i ? pt : mix(pt, k[LC_PANEL_BOTTOM], 190));
        } else if (user_password[0] || st->preview) {
            rect_t b = R(148, ty + 30, 122, 20);
            st->hit[HIT_PW] = b;
            fill_rect(c, b.x, b.y, b.w, b.h, WHITE);
            draw_rect(c, b.x, b.y, b.w, b.h, HEX(0x7F9DB9));
            if (!st->pw_len) {
                draw_text(c, FONT_UI, b.x + 4, b.y + 4, "Enter your password", HEX(0x9A9A9A));
                vline(c, b.x + 3, b.y + 3, b.h - 6, BLACK);
            } else {
                int n = MIN(st->pw_len, 14);
                for (int i = 0; i < n; i++) fill_circle(c, b.x + 8 + i * 7, b.y + b.h / 2, 2, BLACK);
                vline(c, b.x + 6 + n * 7, b.y + 3, b.h - 6, BLACK);
            }
            st->hit[HIT_SIGNIN] = R(280, ty + 26, 80, 28);
            pill(c, st->hit[HIT_SIGNIN], "Sign In", s, st->hot == HIT_SIGNIN);
        } else {
            st->hit[HIT_SIGNIN] = R(148, ty + 28, 80, 28);
            pill(c, st->hit[HIT_SIGNIN], "Sign In", s, st->hot == HIT_SIGNIN);
        }
        if (st->bad_pw) draw_text(c, FONT_UI_BOLD, 148, pr.y + pr.h + 8, "Incorrect password. Please try again.", HEX(0xFFD070));
    }

    /* bottom: guest, brand, notices */
    if (s->show_guest) {
        const char *g = "Sign in as Guest";
        st->hit[HIT_GUEST] = R(22, H - 48, text_width(FONT_UI_BOLD, g) + 26, 28);
        pill(c, st->hit[HIT_GUEST], g, s, st->hot == HIT_GUEST);
    }
    if (st->notice) draw_text(c, FONT_UI_BOLD, 22, H - 76, st->notice, HEX(0xFFD070));
    if (s->brand[0]) {
        int bw = text_width(FONT_DISPLAY_BI, s->brand);
        draw_text(c, FONT_DISPLAY_BI, W - bw - 17, H - 45, s->brand, mix(k[LC_BG_TOP], BLACK, 100));
        draw_text(c, FONT_DISPLAY_BI, W - bw - 18, H - 46, s->brand, k[LC_TEXT]);
    }

    /* the Help and Settings panel */
    if (st->help && s->show_help) {
        rect_t r = R(W - 318, 38, 304, 176);
        fill_round_rect_blend(c, r.x + 3, r.y + 4, r.w, r.h, 8, BLACK, 70);
        fill_round_rect(c, r.x, r.y, r.w, r.h, 8, WHITE);
        gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, k[LC_PANEL_TOP], k[LC_PANEL_BOTTOM]);
        uint32_t pt = k[LC_PANEL_TEXT];
        draw_text(c, FONT_UI_BOLD, r.x + 14, r.y + 12, "Help and Settings", pt);
        int y = r.y + 32;
        y += draw_text_wrapped(c, FONT_UI, r.x + 14, y, r.w - 28,
            "Click your name, type your password and press Enter. To make your own look for this screen, "
            "use Logon Skin Maker after you sign in.", pt, 2) + 10;
        int cur = logon_skin_current_index(), cnt = logon_skin_count();
        char b[96]; snprintf(b, sizeof b, "Logon skin: %s (%d of %d)", logon_skin_get(cur)->name, cur + 1, cnt);
        draw_text_ellipsis(c, FONT_UI_BOLD, r.x + 14, y, r.w - 28, b, pt);
        st->hit[HIT_NEXT_SKIN] = R(r.x + 14, r.y + r.h - 40, 110, 28);
        pill(c, st->hit[HIT_NEXT_SKIN], "Next skin", s, st->hot == HIT_NEXT_SKIN);
        st->hit[HIT_HELP_CLOSE] = R(r.x + r.w - 90, r.y + r.h - 40, 76, 28);
        pill(c, st->hit[HIT_HELP_CLOSE], "Close", s, st->hot == HIT_HELP_CLOSE);
    }
}

/* ---------------- the classic layout ---------------- */
static void classic_render(canvas_t *c, const logon_skin_t *s, logon_state_t *st) {
    int W = c->w, H = c->h;
    const uint32_t *k = s->col;
    logon_background(c, s);
    draw_divider(c, W / 2, CLASSIC_BAR + 40, H - 2 * CLASSIC_BAR - 80);
    int lx = W / 2 - 330;
    if (s->show_globe) draw_globe(c, lx + 88, H / 2 - 150, 96, -36 + st->frame * 2);
    draw_wordmark(c, lx + 190, H / 2 - 126, WORDMARK_MEDIUM, k[LC_TEXT]);
    const char *msg = st->logging_in ? (s->title[0] ? s->title : "welcome") : s->subtitle;
    int f = st->logging_in ? FONT_XL : FONT_LARGE;
    draw_text(c, f, W / 2 - 30 - text_width(f, msg), H / 2 - 10, msg, k[LC_TEXT]);
    if (!st->logging_in) {
        rect_t t = R(W / 2 + 30, H / 2 - 40, 300, 70);
        st->hit[HIT_USER] = t;
        bool hot = st->hot == HIT_USER;
        if (hot) gradient_h(c, t.x, t.y, t.w, t.h, k[LC_PANEL_TOP], k[LC_PANEL_BOTTOM]);
        fill_round_rect(c, t.x + 8, t.y + 8, 54, 54, 5, hot ? k[LC_BUTTON_TOP] : k[LC_BUTTON_BOTTOM]);
        draw_user_picture(c, user_picture, 48, t.x + 11, t.y + 11);
        draw_text(c, FONT_LARGE, t.x + 76, t.y + 24, user_name, k[LC_TEXT]);
    } else {
        for (int i = 0; i < 3; i++) {
            int a = ((st->frame / 6) % 3) == i ? 255 : 90;
            fill_circle(c, W / 2 + 40 + i * 16, H / 2, 4, mix(k[LC_GLOW2], k[LC_TEXT], a));
        }
    }
    if (st->ask_pw) {
        rect_t pr = R(W / 2 + 30, H / 2 + 42, 300, 26);
        st->hit[HIT_PW] = pr;
        draw_text(c, FONT_UI, pr.x, pr.y - 18, "Password:", k[LC_TEXT]);
        fill_rect(c, pr.x, pr.y, pr.w, pr.h, WHITE); draw_rect(c, pr.x, pr.y, pr.w, pr.h, HEX(0x808080));
        char mask[32]; int n = MIN(st->pw_len, (int)sizeof mask - 1);
        for (int i = 0; i < n; i++) mask[i] = '*';
        mask[n] = 0;
        draw_text(c, FONT_UI, pr.x + 6, pr.y + 5, mask, BLACK);
        if (st->bad_pw) draw_text(c, FONT_UI, pr.x, pr.y + 30, "Incorrect password.", HEX(0xFFD070));
        else draw_text(c, FONT_SMALL, pr.x, pr.y + 30, "Press ENTER, or ESC to go back.", k[LC_SUBTEXT]);
    }
    int bx = 30, by = H - 52;
    st->hit[HIT_POWER] = R(bx, by, 180, 32);
    draw_icon(c, ICON_POWER, 32, bx, by);
    draw_text(c, FONT_UI_BOLD, bx + 40, by + 9, "Turn off computer", st->hot == HIT_POWER ? HEX(0xFFE08A) : k[LC_TEXT]);
    const char *hint = "After you log on, you can add or change accounts in Control Panel > User Accounts.";
    if (W >= 760) draw_text(c, FONT_UI, W - text_width(FONT_UI, hint) - 30, H - 44, hint, k[LC_TEXT]);
    if (st->notice) draw_text(c, FONT_UI_BOLD, 30, H - CLASSIC_BAR - 26, st->notice, HEX(0xFFD070));
}

void logon_render(canvas_t *c, const logon_skin_t *s, logon_state_t *st) {
    for (int i = 0; i < HIT_COUNT; i++) st->hit[i] = R(0, 0, 0, 0);
    rect_t old = canvas_push_clip(c, R(0, 0, c->w, c->h));
    if (s->layout == LOGON_LAYOUT_CLASSIC) classic_render(c, s, st); else signin_render(c, s, st);
    c->clip = old;
}

/* ---------------- previews ---------------- */
void canvas_shrink_into(canvas_t *dst, rect_t r, canvas_t *src) {
    if (!src || r.w <= 0 || r.h <= 0) return;
    if (r.w >= src->w || r.h >= src->h) { blit_scaled_alpha(dst, r, src, 255); return; }
    for (int y = 0; y < r.h; y++) {
        int dy = r.y + y;
        if (dy < dst->clip.y || dy >= dst->clip.y + dst->clip.h) continue;
        int sy0 = y * src->h / r.h, sy1 = MAX(sy0 + 1, (y + 1) * src->h / r.h);
        for (int x = 0; x < r.w; x++) {
            int dx = r.x + x;
            if (dx < dst->clip.x || dx >= dst->clip.x + dst->clip.w) continue;
            int sx0 = x * src->w / r.w, sx1 = MAX(sx0 + 1, (x + 1) * src->w / r.w);
            uint32_t rr = 0, gg = 0, bb = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *row = src->px + sy * src->stride;
                for (int sx = sx0; sx < sx1; sx++) { uint32_t p = row[sx]; rr += C_R(p); gg += C_G(p); bb += C_B(p); n++; }
            }
            dst->px[dy * dst->stride + dx] = RGB(rr / n, gg / n, bb / n);
        }
    }
}
static canvas_t *preview_canvas;
void logon_preview(canvas_t *dst, rect_t r, const logon_skin_t *s, int vw, int vh) {
    if (!preview_canvas || preview_canvas->w != vw || preview_canvas->h != vh) {
        if (preview_canvas) canvas_free(preview_canvas);
        preview_canvas = canvas_new(vw, vh);
        if (!preview_canvas) return;
    }
    logon_state_t st; memset(&st, 0, sizeof st);
    st.preview = true;
    st.selected = s->layout == LOGON_LAYOUT_SIGNIN;
    logon_render(preview_canvas, s, &st);
    canvas_shrink_into(dst, r, preview_canvas);
}
void logon_fullscreen_preview(const logon_skin_t *s) {
    canvas_t *saved = canvas_new(SCREEN_W, SCREEN_H);
    if (!saved) return;
    memcpy(saved->px, screen.px, (size_t)SCREEN_W * SCREEN_H * 4);
    logon_state_t st; memset(&st, 0, sizeof st);
    st.preview = true; st.selected = s->layout == LOGON_LAYOUT_SIGNIN;
    mouse_state_t ms; key_event_t ke;
    while (keyboard_poll(&ke)) {}
    int prev = 1;
    bool redraw = true;
    for (;;) {
        gui_heartbeat = timer_ticks();
        if (redraw) {
            logon_render(&screen, s, &st);
            const char *m = "Preview - click or press any key to return";
            int tw = text_width(FONT_UI_BOLD, m);
            fill_round_rect_blend(&screen, (SCREEN_W - tw) / 2 - 12, SCREEN_H - 112, tw + 24, 26, 13, BLACK, 140);
            draw_text(&screen, FONT_UI_BOLD, (SCREEN_W - tw) / 2, SCREEN_H - 106, m, WHITE);
            gfx_present(CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
            redraw = false;
        }
        bool done = false;
        while (keyboard_poll(&ke)) if (ke.pressed) done = true;
        if (mouse_poll(&ms)) {
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            if ((ms.buttons & 1) && !(prev & 1)) done = true;
            prev = ms.buttons;
            int nh = HIT_NONE;
            for (int i = HIT_COUNT - 1; i > HIT_NONE; i--) if (st.hit[i].w && pt_in(st.hit[i], ms.x, ms.y)) { nh = i; break; }
            if (nh != st.hot) { st.hot = nh; redraw = true; } else gfx_move_cursor(CURSOR_ARROW, ms.x, ms.y);
        }
        if (done) break;
        task_yield(); hlt();
    }
    memcpy(screen.px, saved->px, (size_t)SCREEN_W * SCREEN_H * 4);
    canvas_free(saved);
    wm_redraw_all();
}

/* ---------------- the welcome screen ---------------- */
/* USB diagnostics: without a working keyboard or mouse this screen is the only
 * thing a user can photograph for a bug report. */
static void usb_overlay(canvas_t *c) {
#ifdef __x86_64__
    int y0 = SCREEN_H - 190;
    if (!usb_has_input()) {
        char line[512];
        snprintf(line, sizeof line, "No USB keyboard or mouse was found. Controllers: %s. Devices: %s.",
                 usb_boot_summary(), usb_device_summary());
        int h = draw_text_wrapped(c, FONT_UI, 30, y0, c->w - 60, line, HEX(0xFFD070), 2);
        int y = y0 + h + 6;
        const char *d = usb_diag_text();
        if (*d) y += draw_text_wrapped(c, FONT_SMALL, 30, y, c->w - 60, d, HEX(0xFFD070), 2) + 4;
        draw_text(c, FONT_UI, 30, y, "A PS/2 keyboard and mouse still work; the serial log has the controllers and devices.", HEX(0xFFD070));
    } else {
        /* Shown whether or not input has arrived: when the pointer moves
         * and clicks by itself, this is the only place that says which
         * interface did it, and a photo of it is the whole bug report. */
        char line[1024];
        snprintf(line, sizeof line,
                 usb_input_active()
                 ? "USB input is working. Controllers: %s. Devices: %s."
                 : "USB keyboard/mouse enumerated, but no input has arrived yet. Controllers: %s. Devices: %s.",
                 usb_boot_summary(), usb_device_summary());
        int h = draw_text_wrapped(c, FONT_UI, 30, y0, c->w - 60, line, HEX(0xFFD070), 2);
        int y = y0 + h + 6;
        const char *d = usb_diag_text();
        if (*d) y += draw_text_wrapped(c, FONT_SMALL, 30, y, c->w - 60, d, HEX(0xFFD070), 2) + 4;
        const char *l = usb_live_text();
        if (*l) {
            snprintf(line, sizeof line, "%s (boot/rep = its protocol, k/m = the report length its descriptor gives, id = its report ID, i = interrupt polls, r = reports from them, g = GET_REPORT answers, lk = key or bt = buttons in the last report, n = its length, rp = its bytes, bp = button presses, ~ = a second interface on the same endpoint, x = reports refused as another interface's format)", l);
            y += draw_text_wrapped(c, FONT_SMALL, 30, y, c->w - 60, line, HEX(0xFFD070), 2) + 4;
        }
        const char *e = usb_event_text();
        if (*e) {
            snprintf(line, sizeof line, "Last input: %s (usage 04 is 'a', 05 is 'b'; b = buttons, then dx,dy)", e);
            y += draw_text_wrapped(c, FONT_SMALL, 30, y, c->w - 60, line, HEX(0xFFD070), 2) + 4;
        }
        draw_text(c, FONT_UI, 30, y, "A PS/2 keyboard and mouse still work; the serial log has the controllers and devices.", HEX(0xFFD070));
    }
    draw_text_wrapped(c, FONT_SMALL, 30, SCREEN_H - 212, c->w - 60, audio_summary(), HEX(0xFFD070), 1);
#else
    (void)c;
#endif
}

/* the classic screen's background, for the shut-down screen and dialogs */
void logon_paint_background(canvas_t *c) { logon_background(c, logon_skin_current()); }

static logon_state_t ls;
static char login_pw[32];
static const logon_skin_t *cur_skin;
static void welcome_paint(void) {
    ls.pw_len = strlen(login_pw);
    logon_render(&screen, cur_skin, &ls);
    usb_overlay(&screen);
}
static void present(void) { gfx_present(ls.hot ? CURSOR_HAND : CURSOR_ARROW, wm_mouse_x, wm_mouse_y); }
static void reset_for_skin(void) {
    cur_skin = logon_skin_current();
    bool signin = cur_skin->layout == LOGON_LAYOUT_SIGNIN;
    bool help = ls.help && signin;
    memset(&ls, 0, sizeof ls);
    login_pw[0] = 0;
    ls.selected = signin && user_password[0];          /* one account: open it straight away */
    ls.help = help;
}
/* the user asked to sign in: true when that is done */
static bool try_sign_in(void) {
    if (!user_password[0]) return true;
    if (cur_skin->layout == LOGON_LAYOUT_CLASSIC && !ls.ask_pw) { ls.ask_pw = true; ls.bad_pw = false; login_pw[0] = 0; return false; }
    if (cur_skin->layout == LOGON_LAYOUT_SIGNIN && !ls.selected) { ls.selected = true; return false; }
    if (password_matches(login_pw)) return true;
    ls.bad_pw = true; login_pw[0] = 0;
    return false;
}
void welcome_screen(void) {
    mouse_state_t ms; key_event_t ke;
    int prev = 0;
    usb_input_reset();
    logon_skins_load();
    ls.help = false;
    reset_for_skin();
    welcome_paint(); present();
    uint32_t next = timer_ticks() + 500, clock_next = timer_ticks() + 1000;
    int shown_active = 0, shown_minute = -1;
    for (;;) {
        bool login = false, redraw = false;
        gui_heartbeat = timer_ticks();
        bool signin = cur_skin->layout == LOGON_LAYOUT_SIGNIN;
        bool typing = signin ? ls.selected && user_password[0] : ls.ask_pw;
        while (keyboard_poll(&ke)) {
            if (!ke.pressed) continue;
            redraw = true;
            ls.notice = NULL;
            if (ke.key == KEY_ESC && ls.help) { ls.help = false; continue; }
            if (typing) {
                if (ke.key == KEY_ESC) {
                    ls.bad_pw = false;
                    if (login_pw[0]) login_pw[0] = 0;
                    else if (!signin) ls.ask_pw = false;
                } else if (ke.key == KEY_BACKSPACE) { int l = strlen(login_pw); if (l) login_pw[l - 1] = 0; }
                else if (ke.key == KEY_ENTER) login = try_sign_in();
                else if (ke.ch >= 32 && ke.ch < 127 && strlen(login_pw) < sizeof login_pw - 1) {
                    int l = strlen(login_pw); login_pw[l] = ke.ch; login_pw[l + 1] = 0; ls.bad_pw = false;
                }
            } else if (ke.key == KEY_ENTER || ke.key == ' ') login = try_sign_in();
            if (login) break;
        }
        if (!login && mouse_poll(&ms)) {
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            int nh = HIT_NONE;
            /* the help panel sits on top of everything else */
            for (int i = HIT_COUNT - 1; i > HIT_NONE; i--) if (ls.hit[i].w && pt_in(ls.hit[i], ms.x, ms.y)) { nh = i; break; }
            bool click = (ms.buttons & 1) && !(prev & 1);
            prev = ms.buttons;
            if (click) {
                ls.notice = NULL;
                switch (nh) {
                case HIT_USER: case HIT_SIGNIN: case HIT_PW:
                    if (nh == HIT_PW) break;
                    login = try_sign_in();
                    break;
                case HIT_POWER:
                    shell_show_shutdown();
                    /* cancelled (or woken from stand by): back to signing in */
                    break;
                case HIT_HELP: ls.help = !ls.help; break;
                case HIT_HELP_CLOSE: ls.help = false; break;
                case HIT_NEXT_SKIN: {
                    int n = logon_skin_count(), i = (logon_skin_current_index() + 1) % MAX(n, 1);
                    logon_skin_set_current(logon_skin_get(i)->file);
                    reset_for_skin();
                    break;
                }
                case HIT_GUEST:
                    if (!user_password[0]) login = true;
                    else ls.notice = "There is no Guest account on this computer. Sign in with your password.";
                    break;
                default: break;
                }
                redraw = true;
            }
            if (nh != ls.hot) { ls.hot = nh; redraw = true; }
            else if (!redraw) gfx_move_cursor(nh && nh != HIT_PW ? CURSOR_HAND : nh == HIT_PW ? CURSOR_IBEAM : CURSOR_ARROW, ms.x, ms.y);
        }
        if (login) break;
        /* the clock, once a minute */
        if (cur_skin->layout == LOGON_LAYOUT_SIGNIN && cur_skin->show_clock && (int32_t)(timer_ticks() - clock_next) >= 0) {
            clock_next = timer_ticks() + 1000;
            rtc_time_t t; rtc_now(&t);
            if (t.minute != shown_minute) { if (shown_minute >= 0) redraw = true; shown_minute = t.minute; }
        }
        /* While no USB report has arrived, the text on this screen is the only
         * place the counters can be read, so keep it up to date; and redraw
         * once when they start working, to take the notice away. */
        int active = usb_input_active();
        bool idle_usb = usb_has_input() && !active;
        if (active != shown_active || (idle_usb && (int32_t)(timer_ticks() - next) >= 0)) {
            shown_active = active;
            next = timer_ticks() + 500;
            redraw = true;
        }
        if (redraw) { welcome_paint(); present(); }
        task_yield(); hlt();
    }
    ls.logging_in = true; ls.hot = HIT_NONE; ls.help = false; ls.bad_pw = false; ls.notice = NULL;
    if (cur_skin->layout == LOGON_LAYOUT_SIGNIN) ls.selected = true;
    for (int f = 0; f < 50; f++) {
        ls.frame = f;
        gui_heartbeat = timer_ticks();
        welcome_paint();
        gfx_present(CURSOR_BUSY, wm_mouse_x, wm_mouse_y);
        task_sleep(16);
    }
    memset(login_pw, 0, sizeof login_pw);
    speaker_beep(0, 0);
}
