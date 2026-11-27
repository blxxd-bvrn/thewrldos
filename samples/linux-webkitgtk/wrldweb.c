/* The WRLD Browser on WebKit: WebKitGTK 6 in THE WRLD OS window frame, through the
 * wrldcomp Wayland compositor, dressed like a 2007 desktop browser: round Back and
 * Forward buttons, the address bar with Refresh and Stop beside it and a search box,
 * a menu bar, tabs sharing their row with the Favorites buttons and the command bar
 * (Home, Page, Tools), and a status bar with the zone and the zoom. Also find in page,
 * zoom, downloads into My Documents\Downloads and cookies kept in the user's profile.
 *   C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/wrldweb [--proxy URL] [address]
 * The OS starts it from the desktop (src/apps/webkit.c) and passes later addresses
 * through the file named by WRLD_REQUEST, one per line ("" = a new tab). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define APP_NAME "The WRLD Browser"
#define HOME_URI "wrld:home"
#define HOME() (home_uri ? home_uri : HOME_URI)
#define SEARCH_URI "wrld://web?q="                /* The WRLD Web: wrldweb-search.h */

static GtkWidget *win, *notebook, *address, *search, *back_btn, *fwd_btn, *refresh_btn, *stop_btn, *status, *zone_label, *zone_icon,
                 *zoom_label, *findbar, *find_entry;
static WebKitNetworkSession *session;
static WebKitSettings *wsettings;
static WebKitWebContext *wctx;          /* the web context: caches sized to this computer */
static char *downloads_dir;
static const char *request_file;
static char *home_uri;                  /* Internet Options: the home page (NULL: wrld:home) */
static gboolean cli_proxy;               /* a proxy given on the command line wins over Internet Options */

static const struct { const char *name, *uri; } favorites[] = {
    { "The WRLD Web", "wrld://web" }, { "Wikipedia", "https://en.wikipedia.org/" }, { "YouTube", "https://www.youtube.com/" },
    { "Hacker News", "https://news.ycombinator.com/" }, { "DuckDuckGo", "https://duckduckgo.com/" }, { "GitHub", "https://github.com/" },
    { "BBC News", "https://www.bbc.com/news" }, { "Weather", "https://wttr.in/" },
};

/* the home page, in the style of a 2007 portal: a blue band, a search box with its verticals */
static const char *start_page =
    "<!doctype html><html><head><meta charset=utf-8><title>WRLD Home</title><style>"
    "body{margin:0;font-family:'Liberation Sans',Arial,sans-serif;background:#fff;color:#222}"
    ".band{height:96px;background:linear-gradient(#0b3f78,#1f6fb8 55%,#0b4d93);position:relative;overflow:hidden;border-bottom:1px solid #8fb6de}"
    ".band:after{content:'';position:absolute;left:0;right:0;bottom:0;height:30px;background:linear-gradient(#ffffff00,#ffffff55)}"
    ".band .swoosh{position:absolute;left:30%;top:-60px;width:900px;height:220px;border-radius:50%;border-top:2px solid #ffffff40;transform:rotate(-8deg)}"
    ".logo{position:absolute;left:22px;top:20px;display:flex;align-items:center;color:#fff;font-size:24px;text-shadow:0 1px 2px #0006}"
    ".globe{width:46px;height:46px;border-radius:50%;margin-right:12px;background:radial-gradient(circle at 35% 30%,#bfe6ff,#3d8ee0 45%,#0b3d7a);"
    "box-shadow:0 0 0 3px #ffffff90,0 2px 6px #0007;position:relative;overflow:hidden}"
    ".globe:before{content:'';position:absolute;left:8px;top:12px;width:18px;height:14px;background:#5fbf3f;border-radius:50% 40% 60% 40%}"
    ".globe:after{content:'';position:absolute;left:24px;top:26px;width:14px;height:10px;background:#5fbf3f;border-radius:40% 60% 40% 50%}"
    ".opts{text-align:right;padding:12px 20px;font-size:13px}.opts a{color:#222;text-decoration:none;margin-left:14px}"
    ".main{text-align:center;margin-top:60px}h1{font-weight:normal;font-size:30px;margin:0 0 18px}"
    "form{display:inline-flex;border:1px solid #7f9db9}input{width:360px;border:0;padding:5px 6px;font-size:15px;outline:none}"
    "button{border:0;border-left:1px solid #7f9db9;background:linear-gradient(#fff,#cfe3f7);width:30px;cursor:pointer;display:flex;align-items:center;justify-content:center}"
    ".verts{margin-top:8px;font-size:13px}.verts a{margin:0 11px;color:#2f2fa0;text-decoration:none}.verts a.on{font-weight:bold;color:#22228a}"
    ".fav{margin:46px auto 0;max-width:560px;font-size:14px;color:#333}.fav a{color:#0645ad;margin:0 8px;line-height:24px}"
    ".foot{margin-top:70px;font-size:12px;color:#888}.foot a{color:#888}</style></head><body>"
    "<div class=band><div class=swoosh></div><div class=logo><div class=globe></div>The WRLD Home</div></div>"
    "<div class=opts><a href='wrld://web'>The WRLD Web</a></div>"
    "<div class=main><h1>WRLD Search</h1>"
    "<form id=f action='wrld://web'><input name=q id=q autofocus><input type=hidden name=tbm id=t disabled><button title=Search><svg width=16 height=16 viewBox='0 0 16 16'><circle cx=6.5 cy=6.5 r=4.2 fill=#dcefff stroke=#1d4f8c stroke-width=1.8 /><path d='M9.6 9.6 14 14' stroke=#1d4f8c stroke-width=2.6 stroke-linecap=round /></svg></button></form>"
    "<div class=verts><a href=# class=on data-t=''>Web</a><a href=# data-t=isch>Images</a><a href=# data-t=nws>News</a><a href=# data-t=vid>Videos</a></div>"
    "<div class=fav>Favorites: <a href='https://en.wikipedia.org/'>Wikipedia</a><a href='https://www.youtube.com/'>YouTube</a>"
    "<a href='https://news.ycombinator.com/'>Hacker News</a><a href='https://www.bbc.com/news'>BBC News</a><a href='https://github.com/'>GitHub</a>"
    "<a href='https://wttr.in/'>Weather</a></div>"
    "<div class=foot>The WRLD Browser &middot; WebKit on THE WRLD OS</div></div>"
    "<script>document.querySelectorAll('.verts a').forEach(a=>a.onclick=e=>{e.preventDefault();"
    "document.querySelectorAll('.verts a').forEach(b=>b.className='');a.className='on';var t=document.getElementById('t');"
    "t.value=a.dataset.t;t.disabled=!a.dataset.t;if(document.getElementById('q').value)document.getElementById('f').submit();});</script>"
    "</body></html>";

static const char *css =
    "* { font-family: 'Liberation Sans', sans-serif; font-size: 9pt; }"
    "window.wrld { background: #f4f7fb; }"
    /* the navigation bar: a light blue band */
    ".ie-nav { background-image: linear-gradient(#e6eef8, #cfdced 60%, #c3d3e8); border-bottom: 1px solid #a6b9d2; padding: 4px 6px 4px 4px; }"
    ".ie-nav button.round { padding: 0; margin: 0; min-width: 0; min-height: 0; border: none; background: none; box-shadow: none; }"
    ".ie-nav button.round:disabled image { -gtk-icon-filter: grayscale(1) opacity(0.55); }"
    ".ie-nav button.round:hover image { -gtk-icon-filter: brightness(1.15); }"
    ".ie-nav button.round:active image { -gtk-icon-filter: brightness(0.9); }"
    ".ie-field { background: #fff; border: 1px solid #7f9db9; border-radius: 0; padding: 0; min-height: 22px; }"
    ".ie-field entry, entry.ie-entry { background: #fff; border: none; border-radius: 0; box-shadow: none; outline: none; min-height: 20px; padding: 0 4px; }"
    ".ie-field button { padding: 1px 4px; margin: 0; min-height: 0; min-width: 0; border: none; border-left: 1px solid #c5d3e4; border-radius: 0; background-image: linear-gradient(#fdfeff, #e3ecf7); box-shadow: none; }"
    ".ie-field button:hover { background-image: linear-gradient(#ffffff, #cfe2f8); }"
    ".ie-field button:disabled image { -gtk-icon-filter: grayscale(1) opacity(0.4); }"
    "entry.ie-entry progress { background: #c9e3c1; border: none; border-radius: 0; }"
    /* the menu bar */
    ".ie-menu { background-image: linear-gradient(#fbfcfe, #eef2f8); border-bottom: 1px solid #c9d4e3; padding: 0 2px; }"
    ".ie-menu > item { padding: 2px 7px; border-radius: 0; }"
    ".ie-menu > item:hover, .ie-menu > item:selected { background: #c1d2ee; box-shadow: inset 0 0 0 1px #6c8cc6; }"
    "popover.menu contents { background: #fafbfd; border: 1px solid #979797; border-radius: 0; padding: 2px; }"
    "popover.menu modelbutton { padding: 3px 18px; border-radius: 0; }"
    "popover.menu modelbutton:hover { background: #d8e6f8; box-shadow: inset 0 0 0 1px #a8c4e8; }"
    /* the tab row: Favorites buttons, tabs, the command bar */
    "notebook { background: #ffffff; }"
    "notebook > header { background-image: linear-gradient(#fafcff, #e4ecf7); border-bottom: 1px solid #a6b9d2; padding: 2px 4px 0 4px; box-shadow: none; }"
    "notebook > header > tabs { margin: 0; }"
    "notebook > header tab { min-height: 20px; padding: 2px 6px; margin: 0 1px; border: 1px solid #a3b6d0; border-bottom: none; border-radius: 3px 3px 0 0;"
    "  background-image: linear-gradient(#eef3fa, #d7e2f1); box-shadow: none; outline: none; }"
    "notebook > header tab:hover { background-image: linear-gradient(#f6fbff, #dbeafb); }"
    "notebook > header tab:checked { background-image: linear-gradient(#ffffff, #f2f6fb); border-color: #8aa2c4; }"
    "notebook > header tab button { padding: 0; margin: 0 0 0 3px; min-width: 0; min-height: 0; border: none; background: none; box-shadow: none; }"
    "notebook > header tab button:hover { background: #e9a7a0; border-radius: 2px; }"
    "notebook > header.top > tabs > tab:checked { box-shadow: none; }"
    "notebook > header > tabs > arrow { min-width: 14px; }"
    ".ie-cmd button, .ie-fav button { padding: 2px 4px; margin: 0 1px 2px 1px; min-height: 0; min-width: 0; border: 1px solid transparent; border-radius: 2px; background: none; box-shadow: none; }"
    ".ie-cmd button:hover, .ie-fav button:hover { border-color: #93b1d8; background-image: linear-gradient(#fbfdff, #dbe8f8); }"
    ".ie-cmd menubutton > button { padding: 2px 4px; }"
    ".ie-cmd menubutton > button arrow { min-width: 9px; min-height: 9px; -gtk-icon-size: 9px; }"
    ".ie-cmd button.ie-newtab { padding: 0; margin: 0 8px 0 2px; min-height: 18px; border: 1px solid #b8c8de; border-bottom: none; border-radius: 3px 3px 0 0;"
    "  background-image: linear-gradient(#f8fbff, #e1eaf5); }"
    ".ie-cmd button.ie-newtab:hover { background-image: linear-gradient(#ffffff, #d9e8fa); border-color: #8fb0dc; }"
    /* find bar and status bar */
    ".ie-find { background-image: linear-gradient(#fbfcfe, #e9eff7); border-bottom: 1px solid #c9d4e3; padding: 2px 6px; }"
    ".ie-find button { padding: 1px 6px; min-height: 0; }"
    ".ie-status { background-image: linear-gradient(#f7f9fc, #e3e9f2); border-top: 1px solid #a6b9d2; padding: 1px 4px; color: #222; }"
    ".ie-status .sep { border-left: 1px solid #b7c4d6; margin: 1px 4px; }";

/* WebKit only accepts composited frames when GTK says it could use GL, although the
 * shared-memory frames it then receives never touch GL (GTK itself draws with Cairo
 * here, GSK_RENDERER=cairo). Without this, a page that must be composited, such as one
 * playing a video, crashes the browser. This definition in the program takes the place
 * of GTK's for WebKit's question. */
__attribute__((visibility("default"), noinline)) gboolean gdk_display_prepare_gl(GdkDisplay *d, GError **e) {
    /* only WebKit gets the yes: GTK asks too, and believing it has GL changes how it sets
     * itself up (its fonts come out wrong); everyone else gets GTK's own answer */
    Dl_info caller;
    if (dladdr(__builtin_return_address(0), &caller) && caller.dli_fname && strstr(caller.dli_fname, "libwebkitgtk")) return TRUE;
    static gboolean (*real)(GdkDisplay *, GError **);
    if (!real) real = (gboolean (*)(GdkDisplay *, GError **))dlsym(RTLD_NEXT, "gdk_display_prepare_gl");
    return real ? real(d, e) : FALSE;
}

/* ---------------- icons, drawn with Cairo in the browser's own style ---------------- */
enum { IC_BACK, IC_FWD, IC_REFRESH, IC_STOP, IC_SEARCH, IC_STAR, IC_ADDFAV, IC_HOME, IC_PAGE, IC_TOOLS, IC_GLOBE, IC_LOCK, IC_DOC, IC_CLOSE, IC_FIND, IC_COUNT };
static void rgb(cairo_t *c, unsigned v, double a) { cairo_set_source_rgba(c, ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0, a); }
static void star_path(cairo_t *c, double cx, double cy, double r) {
    for (int i = 0; i < 10; i++) {
        double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45 : r;
        if (i == 0) cairo_move_to(c, cx + rr * cos(a), cy + rr * sin(a)); else cairo_line_to(c, cx + rr * cos(a), cy + rr * sin(a));
    }
    cairo_close_path(c);
}
static void draw_star(cairo_t *c, double cx, double cy, double r) {
    star_path(c, cx, cy, r);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, cy - r, 0, cy + r);
    cairo_pattern_add_color_stop_rgb(g, 0, 1, 0.95, 0.55); cairo_pattern_add_color_stop_rgb(g, 1, 0.98, 0.70, 0.05);
    cairo_set_source(c, g); cairo_fill_preserve(c); cairo_pattern_destroy(g);
    rgb(c, 0xb87a00, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
}
static void draw_arrow_head(cairo_t *c, double x, double y, double ang, double s) {
    cairo_move_to(c, x + s * cos(ang), y + s * sin(ang));
    cairo_line_to(c, x + s * cos(ang + 2.4), y + s * sin(ang + 2.4));
    cairo_line_to(c, x + s * cos(ang - 2.4), y + s * sin(ang - 2.4));
    cairo_close_path(c); cairo_fill(c);
}
static void draw_icon(cairo_t *c, int kind, double s) {
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(c, CAIRO_LINE_JOIN_ROUND);
    double m = s / 2;
    switch (kind) {
    case IC_BACK: case IC_FWD: {                                     /* a glossy blue ball with a white arrow */
        double r = m - 1;
        cairo_arc(c, m, m, r, 0, 2 * M_PI);
        cairo_pattern_t *g = cairo_pattern_create_radial(m, m * 1.25, r * 0.2, m, m, r);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.45, 0.78, 1.0); cairo_pattern_add_color_stop_rgb(g, 0.7, 0.11, 0.42, 0.80); cairo_pattern_add_color_stop_rgb(g, 1, 0.05, 0.25, 0.55);
        cairo_set_source(c, g); cairo_fill_preserve(c); cairo_pattern_destroy(g);
        rgb(c, 0x23446f, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
        cairo_save(c); cairo_arc(c, m, m, r - 1, 0, 2 * M_PI); cairo_clip(c);   /* the shine on the top half */
        cairo_pattern_t *h = cairo_pattern_create_linear(0, 0, 0, m);
        cairo_pattern_add_color_stop_rgba(h, 0, 1, 1, 1, 0.75); cairo_pattern_add_color_stop_rgba(h, 1, 1, 1, 1, 0.08);
        cairo_set_source(c, h); cairo_rectangle(c, 0, 0, s, m); cairo_fill(c); cairo_pattern_destroy(h); cairo_restore(c);
        double d = kind == IC_BACK ? -1 : 1, w = s * 0.24;
        cairo_set_line_width(c, s * 0.12); rgb(c, 0xffffff, 1);
        cairo_move_to(c, m - d * w, m); cairo_line_to(c, m + d * w * 0.2, m); cairo_stroke(c);            /* the shaft, tail to head */
        cairo_move_to(c, m + d * w * 1.3, m); cairo_line_to(c, m + d * w * 0.05, m - w * 1.05); cairo_line_to(c, m + d * w * 0.05, m + w * 1.05); cairo_close_path(c); cairo_fill(c);
        break;
    }
    case IC_REFRESH: {                                               /* two green arrows round a circle */
        cairo_set_line_width(c, s * 0.14); rgb(c, 0x2f9a2f, 1);
        double r = s * 0.32;
        cairo_arc(c, m, m, r, M_PI * 1.15, M_PI * 1.95); cairo_stroke(c);
        draw_arrow_head(c, m + r * cos(M_PI * 1.95), m + r * sin(M_PI * 1.95), M_PI * 0.45, s * 0.2);
        rgb(c, 0x1f7fd0, 1);
        cairo_arc(c, m, m, r, M_PI * 0.15, M_PI * 0.95); cairo_stroke(c);
        draw_arrow_head(c, m + r * cos(M_PI * 0.95), m + r * sin(M_PI * 0.95), M_PI * 1.45, s * 0.2);
        break;
    }
    case IC_STOP: case IC_CLOSE:                                     /* a red cross (stop), a grey one (close tab) */
        cairo_set_line_width(c, kind == IC_STOP ? s * 0.17 : s * 0.13); rgb(c, kind == IC_STOP ? 0xd23a2a : 0x6b7a90, 1);
        cairo_move_to(c, s * 0.27, s * 0.27); cairo_line_to(c, s * 0.73, s * 0.73);
        cairo_move_to(c, s * 0.73, s * 0.27); cairo_line_to(c, s * 0.27, s * 0.73); cairo_stroke(c);
        break;
    case IC_SEARCH: case IC_FIND:                                    /* a magnifying glass */
        cairo_arc(c, s * 0.42, s * 0.42, s * 0.24, 0, 2 * M_PI); rgb(c, 0xdcefff, 1); cairo_fill_preserve(c);
        cairo_set_line_width(c, s * 0.1); rgb(c, 0x1d4f8c, 1); cairo_stroke(c);
        cairo_set_line_width(c, s * 0.16); cairo_move_to(c, s * 0.6, s * 0.6); cairo_line_to(c, s * 0.84, s * 0.84); cairo_stroke(c);
        break;
    case IC_STAR: draw_star(c, m, m, s * 0.45); break;
    case IC_ADDFAV:
        draw_star(c, s * 0.42, s * 0.45, s * 0.4);
        cairo_set_line_width(c, s * 0.13); rgb(c, 0x2c9a2c, 1);
        cairo_move_to(c, s * 0.76, s * 0.55); cairo_line_to(c, s * 0.76, s * 0.95);
        cairo_move_to(c, s * 0.56, s * 0.75); cairo_line_to(c, s * 0.96, s * 0.75); cairo_stroke(c);
        break;
    case IC_HOME:                                                    /* a house: red roof, pale walls, blue door */
        cairo_move_to(c, s * 0.08, s * 0.5); cairo_line_to(c, m, s * 0.1); cairo_line_to(c, s * 0.92, s * 0.5); cairo_close_path(c);
        rgb(c, 0xd0482a, 1); cairo_fill_preserve(c); rgb(c, 0x7a2a16, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
        cairo_rectangle(c, s * 0.2, s * 0.48, s * 0.6, s * 0.42); rgb(c, 0xf6f1e3, 1); cairo_fill_preserve(c); rgb(c, 0x7f7462, 1); cairo_stroke(c);
        cairo_rectangle(c, s * 0.42, s * 0.62, s * 0.17, s * 0.28); rgb(c, 0x3a6fb8, 1); cairo_fill(c);
        break;
    case IC_PAGE: case IC_DOC: {                                     /* a page with a folded corner */
        double x0 = s * 0.2, x1 = s * 0.8, y0 = s * 0.08, y1 = s * 0.92, f = s * 0.2;
        cairo_move_to(c, x0, y0); cairo_line_to(c, x1 - f, y0); cairo_line_to(c, x1, y0 + f); cairo_line_to(c, x1, y1); cairo_line_to(c, x0, y1); cairo_close_path(c);
        rgb(c, 0xffffff, 1); cairo_fill_preserve(c); rgb(c, 0x6d7f99, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
        cairo_move_to(c, x1 - f, y0); cairo_line_to(c, x1 - f, y0 + f); cairo_line_to(c, x1, y0 + f); cairo_stroke(c);
        if (kind == IC_PAGE) { rgb(c, 0x8aa2c4, 1); for (int i = 0; i < 4; i++) { cairo_rectangle(c, x0 + 2, s * 0.38 + i * s * 0.13, (x1 - x0) - 4, 1); cairo_fill(c); } }
        else { cairo_arc(c, m, s * 0.58, s * 0.17, 0, 2 * M_PI); rgb(c, 0x3d8ee0, 1); cairo_fill(c); }
        break;
    }
    case IC_TOOLS: {                                                 /* a grey cog */
        cairo_save(c); cairo_translate(c, m, m);
        for (int i = 0; i < 8; i++) { cairo_rotate(c, M_PI / 4); cairo_rectangle(c, -s * 0.08, -s * 0.46, s * 0.16, s * 0.2); }
        cairo_arc(c, 0, 0, s * 0.32, 0, 2 * M_PI);
        rgb(c, 0x6f7c8e, 1); cairo_fill(c);
        cairo_arc(c, 0, 0, s * 0.13, 0, 2 * M_PI); rgb(c, 0xf2f5f9, 1); cairo_fill(c);
        cairo_restore(c);
        break;
    }
    case IC_GLOBE: {                                                 /* the zone: a small globe */
        double r = m - 1;
        cairo_arc(c, m, m, r, 0, 2 * M_PI);
        cairo_pattern_t *g = cairo_pattern_create_radial(m * 0.7, m * 0.6, 1, m, m, r);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.75, 0.9, 1); cairo_pattern_add_color_stop_rgb(g, 1, 0.12, 0.42, 0.78);
        cairo_set_source(c, g); cairo_fill(c); cairo_pattern_destroy(g);
        rgb(c, 0x4caf3a, 1);
        cairo_save(c); cairo_arc(c, m, m, r, 0, 2 * M_PI); cairo_clip(c);
        cairo_arc(c, s * 0.35, s * 0.38, s * 0.2, 0, 2 * M_PI); cairo_fill(c);
        cairo_arc(c, s * 0.68, s * 0.7, s * 0.17, 0, 2 * M_PI); cairo_fill(c);
        cairo_restore(c);
        cairo_arc(c, m, m, r, 0, 2 * M_PI); rgb(c, 0x1b4b85, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
        break;
    }
    case IC_LOCK:                                                    /* a gold padlock */
        cairo_set_line_width(c, s * 0.12); rgb(c, 0x8a6a1a, 1);
        cairo_arc(c, m, s * 0.42, s * 0.2, M_PI, 2 * M_PI); cairo_line_to(c, m + s * 0.2, s * 0.5);
        cairo_move_to(c, m - s * 0.2, s * 0.5); cairo_line_to(c, m - s * 0.2, s * 0.42); cairo_stroke(c);
        cairo_rectangle(c, s * 0.2, s * 0.48, s * 0.6, s * 0.42); rgb(c, 0xf0c040, 1); cairo_fill_preserve(c);
        rgb(c, 0x8a6a1a, 1); cairo_set_line_width(c, 1); cairo_stroke(c);
        break;
    }
}
static GdkTexture *icon_cache[IC_COUNT][3];
static GdkTexture *icon_texture(int kind, int size) {
    int slot = size <= 16 ? 0 : size <= 24 ? 1 : 2;
    if (icon_cache[kind][slot]) return icon_cache[kind][slot];
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *c = cairo_create(sf);
    draw_icon(c, kind, size);
    cairo_destroy(c); cairo_surface_flush(sf);
    GBytes *b = g_bytes_new(cairo_image_surface_get_data(sf), (gsize)cairo_image_surface_get_stride(sf) * size);
    icon_cache[kind][slot] = gdk_memory_texture_new(size, size, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, b, cairo_image_surface_get_stride(sf));
    g_bytes_unref(b); cairo_surface_destroy(sf);
    return icon_cache[kind][slot];
}
static GtkWidget *icon_image(int kind, int size) {
    GtkWidget *im = gtk_image_new_from_paintable(GDK_PAINTABLE(icon_texture(kind, size)));
    gtk_image_set_pixel_size(GTK_IMAGE(im), size);
    return im;
}
/* a flat button with an icon and an optional label */
static GtkWidget *icon_button(int kind, int size, const char *label, const char *tip, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new();
    if (label) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
        gtk_box_append(GTK_BOX(box), icon_image(kind, size)); gtk_box_append(GTK_BOX(box), gtk_label_new(label));
        gtk_button_set_child(GTK_BUTTON(b), box);
    } else gtk_button_set_child(GTK_BUTTON(b), icon_image(kind, size));
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}
/* a command-bar menu: icon, label and a drop-down arrow */
static GtkWidget *menu_button(int kind, const char *label, GMenuModel *model, const char *tip) {
    GtkWidget *mb = gtk_menu_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_append(GTK_BOX(box), icon_image(kind, 16)); gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("pan-down-symbolic"));
    gtk_menu_button_set_child(GTK_MENU_BUTTON(mb), box);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), model);
    gtk_widget_set_tooltip_text(mb, tip);
    return mb;
}

/* ---------------- tabs ---------------- */
static WebKitWebView *view_at(int i) {
    GtkWidget *p = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
    return p ? WEBKIT_WEB_VIEW(p) : NULL;
}
static WebKitWebView *current(void) { return view_at(gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook))); }
static void set_status(const char *s) { gtk_label_set_text(GTK_LABEL(status), s && *s ? s : "Done"); }
static GtkWidget *tab_label_of(WebKitWebView *v) { return g_object_get_data(G_OBJECT(v), "wrld-label"); }
static GtkWidget *tab_icon_of(WebKitWebView *v) { return g_object_get_data(G_OBJECT(v), "wrld-icon"); }

static void update_zoom_label(WebKitWebView *v) {
    char s[16]; snprintf(s, sizeof s, "%d%%", (int)(webkit_web_view_get_zoom_level(v) * 100 + 0.5));
    gtk_label_set_text(GTK_LABEL(zoom_label), s);
}
static void update_window_title(WebKitWebView *v) {
    const char *t = webkit_web_view_get_title(v);
    char *s = g_strdup_printf("%s - " APP_NAME, t && *t ? t : "New Tab");
    gtk_window_set_title(GTK_WINDOW(win), s);
    g_free(s);
}
/* the page's own icon when it has one, else a blank page */
static GdkPaintable *page_icon(WebKitWebView *v) {
    GdkTexture *fav = webkit_web_view_get_favicon(v);
    return fav ? GDK_PAINTABLE(fav) : GDK_PAINTABLE(icon_texture(IC_DOC, 16));
}
static void update_toolbar(WebKitWebView *v) {
    if (!v || v != current()) return;
    gtk_widget_set_sensitive(back_btn, webkit_web_view_can_go_back(v));
    gtk_widget_set_sensitive(fwd_btn, webkit_web_view_can_go_forward(v));
    gtk_widget_set_sensitive(stop_btn, webkit_web_view_is_loading(v));
    const char *u = webkit_web_view_get_uri(v);
    if (!gtk_widget_has_focus(address) || !gtk_editable_get_text(GTK_EDITABLE(address))[0])
        gtk_editable_set_text(GTK_EDITABLE(address), u && strcmp(u, HOME_URI) && strcmp(u, "about:blank") ? u : "");
    double p = webkit_web_view_get_estimated_load_progress(v);
    gtk_entry_set_progress_fraction(GTK_ENTRY(address), webkit_web_view_is_loading(v) && p < 1.0 ? p : 0.0);
    gboolean secure = u && g_str_has_prefix(u, "https://");
    gtk_entry_set_icon_from_paintable(GTK_ENTRY(address), GTK_ENTRY_ICON_PRIMARY, page_icon(v));
    gtk_entry_set_icon_from_paintable(GTK_ENTRY(address), GTK_ENTRY_ICON_SECONDARY, secure ? GDK_PAINTABLE(icon_texture(IC_LOCK, 16)) : NULL);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(address), GTK_ENTRY_ICON_SECONDARY, secure ? "This page is sent over a secure (encrypted) connection" : NULL);
    gboolean local = !u || g_str_has_prefix(u, "file:") || g_str_has_prefix(u, "wrld:") || g_str_has_prefix(u, "about:") || strstr(u, "://localhost") || strstr(u, "://127.");
    gtk_label_set_text(GTK_LABEL(zone_label), local ? "Local computer" : "Internet");
    update_zoom_label(v);
    update_window_title(v);
}

/* A page that stops answering (its web process busy or stuck): a bar offers to wait or to
 * reload it, as a browser does, instead of the window looking dead */
static GtkWidget *hang_bar;
static void on_responsive(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    gboolean ok = webkit_web_view_get_is_web_process_responsive(v);
    if (!ok) g_printerr("wrldweb: the page %s stopped responding\n", webkit_web_view_get_uri(v) ? webkit_web_view_get_uri(v) : "");
    if (v == current() && hang_bar) gtk_widget_set_visible(hang_bar, !ok);
}
static void on_hang_wait(GtkButton *b, gpointer d) { (void)b; (void)d; gtk_widget_set_visible(hang_bar, FALSE); }
static void on_hang_reload(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    WebKitWebView *v = current();
    gtk_widget_set_visible(hang_bar, FALSE);
    if (!v) return;
    webkit_web_view_terminate_web_process(v);
    webkit_web_view_reload(v);
}
static void on_title(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    const char *t = webkit_web_view_get_title(v);
    GtkWidget *l = tab_label_of(v);
    if (l) gtk_label_set_text(GTK_LABEL(l), t && *t ? t : "New Tab");
    if (v == current()) update_window_title(v);
}
static void on_favicon(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    GtkWidget *im = tab_icon_of(v);
    if (im) gtk_image_set_from_paintable(GTK_IMAGE(im), page_icon(v));
    update_toolbar(v);
}
static void on_changed(WebKitWebView *v, GParamSpec *ps, gpointer d) { (void)ps; (void)d; update_toolbar(v); }
static void on_load(WebKitWebView *v, WebKitLoadEvent e, gpointer d) {
    (void)d;
    if (v == current()) set_status(e == WEBKIT_LOAD_FINISHED ? "Done" : "Waiting for the page...");
    update_toolbar(v);
}
static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent e, char *uri, GError *err, gpointer d) {
    (void)v; (void)e; (void)d;
    char *s = g_strdup_printf("Could not open %s: %s", uri, err ? err->message : "error");
    set_status(s); g_free(s);
    return FALSE;                                   /* WebKit's own error page */
}
static void on_target(WebKitWebView *v, WebKitHitTestResult *h, guint mods, gpointer d) {
    (void)mods; (void)d;
    if (v != current()) return;
    set_status(webkit_hit_test_result_context_is_link(h) ? webkit_hit_test_result_get_link_uri(h) : NULL);
}
static void on_crashed(WebKitWebView *v, WebKitWebProcessTerminationReason r, gpointer d) {
    (void)d;
    webkit_web_view_load_alternate_html(v, r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT
        ? "<h2>This page used too much memory</h2><p>Reload it, or close other tabs first.</p>"
        : "<h2>This page stopped working</h2><p>Press F5 to load it again.</p>", webkit_web_view_get_uri(v), NULL);
}

static WebKitWebView *new_tab(const char *uri, WebKitWebView *related, gboolean show);
#include "wrldweb-adblock.h"
static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d) {
    (void)a; (void)d;
    return GTK_WIDGET(new_tab(NULL, v, TRUE));        /* target=_blank and window.open: a new tab */
}
static gboolean on_policy(WebKitWebView *v, WebKitPolicyDecision *dec, WebKitPolicyDecisionType t, gpointer d) {
    (void)d;
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {   /* middle click: a new tab in the background */
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
        if (webkit_navigation_action_get_mouse_button(a) == 2) {
            new_tab(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)), v, FALSE);
            webkit_policy_decision_ignore(dec);
            return TRUE;
        }
    }
    if (t == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {      /* a page's own file it cannot show is saved; never a frame's */
        WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(dec);
        if (webkit_response_policy_decision_is_main_frame_main_resource(rd) && !webkit_response_policy_decision_is_mime_type_supported(rd)) {
            webkit_policy_decision_download(dec);
            return TRUE;
        }
    }
    return FALSE;
}
static void close_tab(WebKitWebView *v) {
    int i = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), GTK_WIDGET(v));
    if (i < 0) return;
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 1) { gtk_window_destroy(GTK_WINDOW(win)); return; }
    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), i);
}
static void on_close_clicked(GtkButton *b, gpointer v) { (void)b; close_tab(WEBKIT_WEB_VIEW(v)); }
static void on_close_req(WebKitWebView *v, gpointer d) { (void)d; close_tab(v); }

static void load(WebKitWebView *v, const char *uri) {
    if (!uri || !*uri || !strcmp(uri, HOME_URI)) webkit_web_view_load_uri(v, HOME_URI);
    else webkit_web_view_load_uri(v, uri);
}
static WebKitWebView *new_tab(const char *uri, WebKitWebView *related, gboolean show) {
    /* videos may start by themselves, with sound, as on a desktop browser (YouTube expects it) */
    WebKitWebsitePolicies *pol = webkit_website_policies_new_with_policies("autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);
    WebKitWebView *v = related
        ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, "settings", wsettings, "website-policies", pol, "user-content-manager", ucm, NULL))
        : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", wctx, "network-session", session, "settings", wsettings, "website-policies", pol, "user-content-manager", ucm, NULL));
    g_object_unref(pol);
    gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);
    /* the tab: the page's icon, its title, and a close button */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *icon = gtk_image_new_from_paintable(GDK_PAINTABLE(icon_texture(IC_DOC, 16)));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    GtkWidget *label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 18); gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *x = icon_button(IC_CLOSE, 12, NULL, "Close Tab (Ctrl+W)", G_CALLBACK(on_close_clicked), v);
    gtk_box_append(GTK_BOX(box), icon); gtk_box_append(GTK_BOX(box), label); gtk_box_append(GTK_BOX(box), x);
    g_object_set_data(G_OBJECT(v), "wrld-label", label);
    g_object_set_data(G_OBJECT(v), "wrld-icon", icon);
    int at = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook)) + 1;
    int i = gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), GTK_WIDGET(v), box, at);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), GTK_WIDGET(v), TRUE);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(v, "notify::is-web-process-responsive", G_CALLBACK(on_responsive), NULL);
    g_signal_connect(v, "notify::favicon", G_CALLBACK(on_favicon), NULL);
    g_signal_connect(v, "notify::uri", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "notify::zoom-level", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "load-changed", G_CALLBACK(on_load), NULL);
    g_signal_connect(v, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(v, "mouse-target-changed", G_CALLBACK(on_target), NULL);
    g_signal_connect(v, "create", G_CALLBACK(on_create), NULL);
    g_signal_connect(v, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(v, "close", G_CALLBACK(on_close_req), NULL);
    g_signal_connect(v, "web-process-terminated", G_CALLBACK(on_crashed), NULL);
    if (show) gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), i);
    if (uri || !related) load(v, uri);
    return v;
}
static void on_switch(GtkNotebook *nb, GtkWidget *page, guint n, gpointer d) {
    (void)nb; (void)n; (void)d;
    gtk_editable_set_text(GTK_EDITABLE(address), "");
    update_toolbar(WEBKIT_WEB_VIEW(page));
    gtk_widget_grab_focus(page);
}

/* ---------------- the navigation bar ---------------- */
static char *guess_uri(const char *t) {
    while (*t == ' ') t++;
    if (!*t) return g_strdup(HOME_URI);
    if (strstr(t, "://") || g_str_has_prefix(t, "about:") || g_str_has_prefix(t, "data:") || g_str_has_prefix(t, "wrld:")) return g_strdup(t);
    /* no dot, or spaces: a search; a numeric host, localhost or a port: plain http */
    if (strchr(t, ' ') || (!strchr(t, '.') && !g_str_has_prefix(t, "localhost"))) {
        char *q = g_uri_escape_string(t, NULL, TRUE), *u = g_strconcat(SEARCH_URI, q, NULL);
        g_free(q); return u;
    }
    gboolean plain = g_ascii_isdigit(t[0]) || g_str_has_prefix(t, "localhost") || strchr(t, ':');
    return g_strconcat(plain ? "http://" : "https://", t, NULL);
}
static void on_go(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    char *u = guess_uri(gtk_editable_get_text(GTK_EDITABLE(address)));
    WebKitWebView *v = current();
    load(v, u); g_free(u);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}
static void on_search(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(search));
    if (!*t) { gtk_widget_grab_focus(search); return; }
    char *q = g_uri_escape_string(t, NULL, TRUE), *u = g_strconcat(SEARCH_URI, q, NULL);
    load(current(), u); g_free(q); g_free(u);
    gtk_widget_grab_focus(GTK_WIDGET(current()));
}
static void on_back(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_go_back(current()); }
static void on_fwd(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_go_forward(current()); }
static void on_refresh(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_reload(current()); }
static void on_stop(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_stop_loading(current()); }
static void on_home(GtkButton *b, gpointer d) { (void)b; (void)d; load(current(), HOME()); }
static void zoom(double f) {
    WebKitWebView *v = current();
    double z = f == 0 ? 1.0 : webkit_web_view_get_zoom_level(v) * f;
    webkit_web_view_set_zoom_level(v, CLAMP(z, 0.3, 3.0));
    update_zoom_label(v);
}

/* find on this page */
static void find_now(gboolean back) {
    const char *t = gtk_editable_get_text(GTK_EDITABLE(find_entry));
    WebKitFindController *fc = webkit_web_view_get_find_controller(current());
    if (!*t) { webkit_find_controller_search_finish(fc); return; }
    if (g_strcmp0(webkit_find_controller_get_search_text(fc), t)) {
        webkit_find_controller_search(fc, t, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 1000);
    } else if (back) webkit_find_controller_search_previous(fc); else webkit_find_controller_search_next(fc);
}
static void on_find_changed(GtkEditable *e, gpointer d) { (void)e; (void)d; find_now(FALSE); }
static void on_find_next(GtkWidget *w, gpointer d) { (void)w; find_now(GPOINTER_TO_INT(d)); }
static void show_find(gboolean on) {
    gtk_widget_set_visible(findbar, on);
    if (on) { gtk_widget_grab_focus(find_entry); gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1); }
    else { webkit_find_controller_search_finish(webkit_web_view_get_find_controller(current())); gtk_widget_grab_focus(GTK_WIDGET(current())); }
}
static void on_find_close(GtkButton *b, gpointer d) { (void)b; (void)d; show_find(FALSE); }

/* ---------------- menus ---------------- */
#define ACT(name) static void name(GSimpleAction *a, GVariant *p, gpointer d)
ACT(act_new_tab) { (void)a; (void)p; (void)d; new_tab(HOME(), NULL, TRUE); gtk_widget_grab_focus(address); }
ACT(act_close_tab) { (void)a; (void)p; (void)d; close_tab(current()); }
ACT(act_exit) { (void)a; (void)p; (void)d; gtk_window_destroy(GTK_WINDOW(win)); }
ACT(act_find) { (void)a; (void)p; (void)d; show_find(TRUE); }
ACT(act_zoom_in) { (void)a; (void)p; (void)d; zoom(1.1); }
ACT(act_zoom_out) { (void)a; (void)p; (void)d; zoom(1 / 1.1); }
ACT(act_zoom_reset) { (void)a; (void)p; (void)d; zoom(0); }
ACT(act_refresh) { (void)a; (void)p; (void)d; webkit_web_view_reload(current()); }
ACT(act_stop) { (void)a; (void)p; (void)d; webkit_web_view_stop_loading(current()); }
ACT(act_back) { (void)a; (void)p; (void)d; webkit_web_view_go_back(current()); }
ACT(act_forward) { (void)a; (void)p; (void)d; webkit_web_view_go_forward(current()); }
ACT(act_home) { (void)a; (void)p; (void)d; load(current(), HOME()); }
ACT(act_open) { (void)a; (void)d; load(current(), g_variant_get_string(p, NULL)); }
ACT(act_address) { (void)a; (void)p; (void)d; gtk_widget_grab_focus(address); gtk_editable_select_region(GTK_EDITABLE(address), 0, -1); }
ACT(act_add_fav) {
    (void)a; (void)p; (void)d;
    set_status("Favorites are fixed in this version: the page's address is in the address bar to copy.");
}
static void history_cleared(GObject *o, GAsyncResult *r, gpointer d) {
    (void)d;
    gboolean ok = webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(o), r, NULL);
    set_status(ok ? "Browsing history deleted: cookies, cached files and site data." : "The browsing history could not be deleted.");
}
ACT(act_clear) {
    (void)a; (void)p; (void)d;
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(session), WEBKIT_WEBSITE_DATA_ALL, 0, NULL, history_cleared, NULL);
}
ACT(act_about) {
    (void)a; (void)p; (void)d;
    char *html = g_strdup_printf("<!doctype html><title>About The WRLD Browser</title><body style=\"font-family:'Liberation Sans',sans-serif;margin:40px\">"
        "<h1 style='font-weight:normal'>The WRLD Browser</h1><p>WebKitGTK %u.%u.%u in THE WRLD OS's Linux layer, through the wrldcomp Wayland compositor.</p>"
        "<p>Pages are drawn on the CPU. Downloads go to My Documents\\Downloads; cookies are kept in your profile.</p>"
        "<p>The built-in browser is still there: run <code>start classicbrowser</code> in the Command Prompt.</p>",
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    webkit_web_view_load_html(new_tab(NULL, NULL, TRUE), html, "about:wrld");
    g_free(html);
}
static void add_item(GMenu *m, const char *label, const char *action, const char *accel) {
    GMenuItem *it = g_menu_item_new(label, action);
    if (accel) g_menu_item_set_attribute(it, "accel", "s", accel);   /* shown in the menu; the keys are handled in on_key */
    g_menu_append_item(m, it); g_object_unref(it);
}
static GMenu *section(GMenu *parent) { GMenu *s = g_menu_new(); g_menu_append_section(parent, NULL, G_MENU_MODEL(s)); g_object_unref(s); return s; }
static GMenu *favorites_menu(void) {
    GMenu *m = g_menu_new();
    add_item(section(m), "Add to Favorites...", "win.add-fav", "<Control>d");
    GMenu *s = section(m);
    for (unsigned i = 0; i < G_N_ELEMENTS(favorites); i++) {
        GMenuItem *it = g_menu_item_new(favorites[i].name, NULL);
        g_menu_item_set_action_and_target_value(it, "win.open", g_variant_new_string(favorites[i].uri));
        g_menu_append_item(s, it); g_object_unref(it);
    }
    return m;
}
static GMenu *page_menu(void) {
    GMenu *m = g_menu_new(), *s = section(m);
    add_item(s, "New Tab", "win.new-tab", "<Control>t"); add_item(s, "Close Tab", "win.close-tab", "<Control>w");
    add_item(section(m), "Find on This Page...", "win.find", "<Control>f");
    s = section(m);
    add_item(s, "Zoom In", "win.zoom-in", "<Control>plus"); add_item(s, "Zoom Out", "win.zoom-out", "<Control>minus"); add_item(s, "Zoom 100%", "win.zoom-reset", "<Control>0");
    return m;
}
static GMenu *tools_menu(void) {
    GMenu *m = g_menu_new();
    add_item(section(m), "Delete Browsing History...", "win.clear", NULL);
    add_item(section(m), "About The WRLD Browser", "win.about", NULL);
    return m;
}
static GMenu *menubar_model(void) {
    GMenu *bar = g_menu_new(), *m, *s;
    m = g_menu_new(); s = section(m);
    add_item(s, "New Tab", "win.new-tab", "<Control>t"); add_item(s, "Close Tab", "win.close-tab", "<Control>w");
    add_item(section(m), "Exit", "win.exit", NULL);
    g_menu_append_submenu(bar, "File", G_MENU_MODEL(m)); g_object_unref(m);
    m = g_menu_new();
    add_item(section(m), "Find on This Page...", "win.find", "<Control>f");
    g_menu_append_submenu(bar, "Edit", G_MENU_MODEL(m)); g_object_unref(m);
    m = g_menu_new(); s = section(m);
    add_item(s, "Go to Address Bar", "win.address", "<Control>l");
    s = section(m);
    add_item(s, "Back", "win.back", "<Alt>Left"); add_item(s, "Forward", "win.forward", "<Alt>Right"); add_item(s, "Home Page", "win.home", "<Alt>Home");
    s = section(m);
    add_item(s, "Stop", "win.stop", "Escape"); add_item(s, "Refresh", "win.refresh", "F5");
    GMenu *z = g_menu_new();
    add_item(z, "Zoom In", "win.zoom-in", "<Control>plus"); add_item(z, "Zoom Out", "win.zoom-out", "<Control>minus"); add_item(z, "100%", "win.zoom-reset", "<Control>0");
    g_menu_append_submenu(section(m), "Zoom", G_MENU_MODEL(z)); g_object_unref(z);
    g_menu_append_submenu(bar, "View", G_MENU_MODEL(m)); g_object_unref(m);
    m = favorites_menu(); g_menu_append_submenu(bar, "Favorites", G_MENU_MODEL(m)); g_object_unref(m);
    m = tools_menu(); g_menu_append_submenu(bar, "Tools", G_MENU_MODEL(m)); g_object_unref(m);
    m = g_menu_new(); add_item(m, "About The WRLD Browser", "win.about", NULL);
    g_menu_append_submenu(bar, "Help", G_MENU_MODEL(m)); g_object_unref(m);
    return bar;
}

/* keys the browser handles before the page does */
static gboolean on_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType m, gpointer d) {
    (void)k; (void)code; (void)d;
    gboolean ctrl = (m & GDK_CONTROL_MASK) != 0, alt = (m & GDK_ALT_MASK) != 0, shift = (m & GDK_SHIFT_MASK) != 0;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)), cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (ctrl && (key == GDK_KEY_t || key == GDK_KEY_T)) { act_new_tab(NULL, NULL, NULL); return TRUE; }
    if (ctrl && (key == GDK_KEY_w || key == GDK_KEY_W)) { close_tab(current()); return TRUE; }
    if ((ctrl && (key == GDK_KEY_l || key == GDK_KEY_L)) || key == GDK_KEY_F6 || (alt && (key == GDK_KEY_d || key == GDK_KEY_D))) { act_address(NULL, NULL, NULL); return TRUE; }
    if (ctrl && (key == GDK_KEY_e || key == GDK_KEY_E)) { gtk_widget_grab_focus(search); return TRUE; }
    if (ctrl && (key == GDK_KEY_d || key == GDK_KEY_D)) { act_add_fav(NULL, NULL, NULL); return TRUE; }
    if (key == GDK_KEY_F5 || (ctrl && (key == GDK_KEY_r || key == GDK_KEY_R))) { webkit_web_view_reload(current()); return TRUE; }
    if (alt && key == GDK_KEY_Left) { webkit_web_view_go_back(current()); return TRUE; }
    if (alt && key == GDK_KEY_Right) { webkit_web_view_go_forward(current()); return TRUE; }
    if (alt && key == GDK_KEY_Home) { on_home(NULL, NULL); return TRUE; }
    if (ctrl && key == GDK_KEY_Tab) { gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), (cur + (shift ? n - 1 : 1)) % n); return TRUE; }
    if (ctrl && key == GDK_KEY_ISO_Left_Tab) { gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), (cur + n - 1) % n); return TRUE; }
    if (ctrl && (key == GDK_KEY_f || key == GDK_KEY_F)) { show_find(TRUE); return TRUE; }
    if (key == GDK_KEY_F3) { find_now(shift); return TRUE; }
    if (ctrl && (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add)) { zoom(1.1); return TRUE; }
    if (ctrl && (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract)) { zoom(1 / 1.1); return TRUE; }
    if (ctrl && (key == GDK_KEY_0 || key == GDK_KEY_KP_0)) { zoom(0); return TRUE; }
    if (key == GDK_KEY_Escape) {
        if (gtk_widget_get_visible(findbar)) { show_find(FALSE); return TRUE; }
        if (webkit_web_view_is_loading(current())) { webkit_web_view_stop_loading(current()); return TRUE; }
    }
    return FALSE;
}

/* ---------------- downloads ---------------- */
#include "wrldweb-download.h"

/* ---------------- wrld: pages and requests from the desktop ---------------- */
#include "wrldweb-search.h"
static void wrld_scheme(WebKitURISchemeRequest *r, gpointer d) {
    (void)d;
    const char *uri = webkit_uri_scheme_request_get_uri(r);
    if (uri && (g_str_has_prefix(uri, "wrld://web") || g_str_has_prefix(uri, "wrld:web"))) {
        char *q = NULL; const char *qs = strchr(uri, '?');
        if (qs) {
            GHashTable *t = g_uri_parse_params(qs + 1, -1, "&", G_URI_PARAMS_WWW_FORM, NULL);
            if (t) { const char *v = g_hash_table_lookup(t, "q"); q = g_strdup(v); g_hash_table_unref(t); }
        }
        wrld_web(r, q);
        g_free(q);
        return;
    }
    GInputStream *in = g_memory_input_stream_new_from_data(start_page, strlen(start_page), NULL);
    webkit_uri_scheme_request_finish(r, in, strlen(start_page), "text/html");
    g_object_unref(in);
}
/* Internet Options (written by the desktop): home page, scripts, pictures, pop-ups, cookies,
 * proxy and smooth scrolling; read at start and whenever the desktop says "wrld:options" */
static void apply_options(void) {
    const char *f = g_getenv("WRLD_OPTIONS");
    char *text = NULL;
    if (!f || !g_file_get_contents(f, &text, NULL, NULL)) return;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *eq = strchr(*l, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = *l, *v = eq + 1;
        gboolean on = atoi(v) != 0;
        if (!strcmp(k, "home")) { g_free(home_uri); home_uri = *v && strcmp(v, HOME_URI) && strcmp(v, "wrld://home") ? guess_uri(v) : NULL; }
        else if (!strcmp(k, "javascript")) webkit_settings_set_enable_javascript(wsettings, on);
        else if (!strcmp(k, "images")) webkit_settings_set_auto_load_images(wsettings, on);
        else if (!strcmp(k, "popups")) webkit_settings_set_javascript_can_open_windows_automatically(wsettings, on);
        else if (!strcmp(k, "smooth")) webkit_settings_set_enable_smooth_scrolling(wsettings, on);
        else if (!strcmp(k, "adblock")) ab_set(on);
        else if (!strcmp(k, "cookies")) {
            WebKitCookieAcceptPolicy pol = !strcmp(v, "always") ? WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS
                                         : !strcmp(v, "never") ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER : WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
            webkit_cookie_manager_set_accept_policy(webkit_network_session_get_cookie_manager(session), pol);
        } else if (!strcmp(k, "proxy") && !cli_proxy) {
            if (*v) {
                char *u = strstr(v, "://") ? g_strdup(v) : g_strconcat("http://", v, NULL);
                const char *local[] = { "localhost", "127.0.0.0/8", NULL };
                WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(u, local);
                g_free(web_proxy); web_proxy = g_strdup(u);
                webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
                webkit_network_proxy_settings_free(ps);
                g_free(u);
            } else webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_NO_PROXY, NULL);
        }
    }
    g_strfreev(lines); g_free(text);
}
static gboolean poll_requests(gpointer d) {
    (void)d;
    if (!request_file) return G_SOURCE_REMOVE;
    char *text = NULL; gsize len = 0;
    if (!g_file_get_contents(request_file, &text, &len, NULL) || !len) { g_free(text); return G_SOURCE_CONTINUE; }
    g_file_set_contents(request_file, "", 0, NULL);
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        if (l[1] == NULL && !**l) break;            /* the empty piece after the last newline */
        g_strstrip(*l);
        if (!strcmp(*l, "wrld:options")) { apply_options(); if (ab_on) ab_load(); set_status("Internet Options changed."); continue; }
        if (!strcmp(*l, "wrld:clear-data")) { act_clear(NULL, NULL, NULL); continue; }
        new_tab(**l ? *l : HOME(), NULL, TRUE);
    }
    g_strfreev(lines); g_free(text);
    return G_SOURCE_CONTINUE;
}

/* ---------------- the window ---------------- */
/* an entry in a white box with buttons at its right end (the address bar, the search box) */
static GtkWidget *field(GtkWidget *entry, GtkWidget *b1, GtkWidget *b2) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(box, "ie-field");
    gtk_widget_add_css_class(entry, "ie-entry");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(box), entry);
    if (b1) gtk_box_append(GTK_BOX(box), b1);
    if (b2) gtk_box_append(GTK_BOX(box), b2);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    return box;
}
static void activate(GtkApplication *app, gpointer d) {
    const char *start = d;
    g_object_set(gtk_settings_get_default(), "gtk-font-name", "Liberation Sans 9", NULL);
    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp, css);
    dl_init();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* where things are kept: the profile on C:, a cache in memory */
    /* Memory instead of processor time, within a limit: the "web browser" cache model keeps
     * decoded pictures, pages and the back/forward cache in memory, so going back or scrolling a
     * heavy page does not redo the work; memory-pressure settings put a ceiling on it (a third of
     * the computer's memory, at most 1.5 GB per process) past which WebKit drops caches before it
     * would run the system out. */
    {
        long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
        guint64 total_mb = pages > 0 && psz > 0 ? (guint64)pages * (guint64)psz / (1024 * 1024) : 2048;
        guint limit = (guint)MIN(MAX(total_mb / 3, 384), 1536);
        WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
        webkit_memory_pressure_settings_set_memory_limit(mp, limit);
        /* strict first: the conservative threshold must stay below it, and the default strict one is lower */
        webkit_memory_pressure_settings_set_strict_threshold(mp, 0.75);
        webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.5);
        webkit_memory_pressure_settings_set_poll_interval(mp, 5.0);
        webkit_network_session_set_memory_pressure_settings(mp);
        wctx = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, NULL));
        webkit_memory_pressure_settings_free(mp);
        webkit_web_context_set_cache_model(wctx, WEBKIT_CACHE_MODEL_WEB_BROWSER);
        g_message("wrldweb: %" G_GUINT64_FORMAT " MB of memory, WebKit limited to %u MB a process", total_mb, limit);
    }
    const char *prof = g_getenv("WRLD_PROFILE");
    char *data = prof ? g_strdup(prof) : g_build_filename(g_get_home_dir(), ".wrldweb", NULL);
    g_mkdir_with_parents(data, 0755);
    if (g_file_test(data, G_FILE_TEST_IS_DIR) && access(data, W_OK) == 0) {
        session = webkit_network_session_new(data, "/tmp/wrldweb-cache");
        char *cookies = g_build_filename(data, "cookies.txt", NULL);
        webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(session), cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
        g_free(cookies);
    } else session = webkit_network_session_new_ephemeral();
    g_free(data);
    /* (no favicon database: it is SQLite with shared-memory locking, which a file mapping here cannot give) */
    const char *docs = g_getenv("WRLD_DOCS");
    downloads_dir = docs ? g_build_filename(docs, "Downloads", NULL) : g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_signal_connect(session, "download-started", G_CALLBACK(on_download), NULL);
    const char *proxy = g_object_get_data(G_OBJECT(app), "wrld-proxy");
    cli_proxy = proxy != NULL;
    if (proxy) {
        const char *local[] = { "localhost", "127.0.0.0/8", NULL };
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(proxy, local);
        g_free(web_proxy); web_proxy = g_strdup(proxy);
        webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }
    webkit_web_context_register_uri_scheme(wctx, "wrld", wrld_scheme, NULL, NULL);
    webkit_security_manager_register_uri_scheme_as_local(webkit_web_context_get_security_manager(wctx), "wrld");
    wsettings = webkit_settings_new();
    /* With Mesa (OpenGL ES on the CPU, llvmpipe) every page is composited, as WebKit does by default:
     * a web process sets up its GL display only when acceleration is on, and some pages (YouTube's)
     * switch WebKit to its compositor anyway, which then had no GL and never drew a frame. Without
     * Mesa (the stand-in libEGL) pages are painted without compositing. WRLDWEB_ACCEL=0 or 1 overrides. */
    {
        const char *acc = g_getenv("WRLDWEB_ACCEL");
        gboolean gl = g_file_test("/usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0", G_FILE_TEST_EXISTS);
        gboolean accel = acc ? strcmp(acc, "0") != 0 : gl;
        webkit_settings_set_hardware_acceleration_policy(wsettings, accel ? WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS : WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        g_message("wrldweb: %s", accel ? "pages composited with OpenGL ES on the CPU (Mesa llvmpipe)"
                                       : gl ? "compositing off (WRLDWEB_ACCEL=0)" : "no OpenGL (Mesa not installed): pages painted without compositing");
    }
    webkit_settings_set_enable_smooth_scrolling(wsettings, FALSE);   /* fewer repaints on the CPU */
    webkit_settings_set_enable_developer_extras(wsettings, FALSE);
    ab_init();
    apply_options();

    win = gtk_application_window_new(app);
    gtk_widget_add_css_class(win, "wrld");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);   /* THE WRLD OS draws the frame */
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 660);
    gtk_window_set_title(GTK_WINDOW(win), APP_NAME);
    static const GActionEntry acts[] = {
        { "new-tab", act_new_tab, NULL, NULL, NULL, { 0 } }, { "close-tab", act_close_tab, NULL, NULL, NULL, { 0 } },
        { "exit", act_exit, NULL, NULL, NULL, { 0 } }, { "find", act_find, NULL, NULL, NULL, { 0 } },
        { "zoom-in", act_zoom_in, NULL, NULL, NULL, { 0 } }, { "zoom-out", act_zoom_out, NULL, NULL, NULL, { 0 } },
        { "zoom-reset", act_zoom_reset, NULL, NULL, NULL, { 0 } }, { "refresh", act_refresh, NULL, NULL, NULL, { 0 } },
        { "stop", act_stop, NULL, NULL, NULL, { 0 } }, { "back", act_back, NULL, NULL, NULL, { 0 } },
        { "forward", act_forward, NULL, NULL, NULL, { 0 } }, { "home", act_home, NULL, NULL, NULL, { 0 } },
        { "open", act_open, "s", NULL, NULL, { 0 } }, { "address", act_address, NULL, NULL, NULL, { 0 } },
        { "add-fav", act_add_fav, NULL, NULL, NULL, { 0 } }, { "clear", act_clear, NULL, NULL, NULL, { 0 } },
        { "about", act_about, NULL, NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win), acts, G_N_ELEMENTS(acts), NULL);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* the navigation bar: Back, Forward, the address bar with Refresh and Stop, the search box */
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(nav, "ie-nav");
    back_btn = icon_button(IC_BACK, 28, NULL, "Back (Alt+Left)", G_CALLBACK(on_back), NULL);
    fwd_btn = icon_button(IC_FWD, 24, NULL, "Forward (Alt+Right)", G_CALLBACK(on_fwd), NULL);
    gtk_widget_add_css_class(back_btn, "round"); gtk_widget_add_css_class(fwd_btn, "round");
    gtk_widget_set_valign(fwd_btn, GTK_ALIGN_CENTER);
    address = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(address), "Type an address, or words to search for");
    gtk_entry_set_input_purpose(GTK_ENTRY(address), GTK_INPUT_PURPOSE_URL);
    g_signal_connect(address, "activate", G_CALLBACK(on_go), NULL);
    refresh_btn = icon_button(IC_REFRESH, 16, NULL, "Refresh (F5)", G_CALLBACK(on_refresh), NULL);
    stop_btn = icon_button(IC_STOP, 16, NULL, "Stop (Esc)", G_CALLBACK(on_stop), NULL);
    GtkWidget *addr_box = field(address, refresh_btn, stop_btn);
    gtk_widget_set_hexpand(addr_box, TRUE);
    search = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "The WRLD Web");
    g_signal_connect(search, "activate", G_CALLBACK(on_search), NULL);
    GtkWidget *search_box = field(search, icon_button(IC_SEARCH, 16, NULL, "Search (Ctrl+E)", G_CALLBACK(on_search), NULL), NULL);
    gtk_widget_set_size_request(search_box, 230, -1);
    gtk_widget_set_hexpand(search_box, FALSE);
    gtk_box_append(GTK_BOX(nav), back_btn); gtk_box_append(GTK_BOX(nav), fwd_btn);
    gtk_box_append(GTK_BOX(nav), addr_box); gtk_box_append(GTK_BOX(nav), search_box);
    gtk_box_append(GTK_BOX(vbox), nav);

    /* the menu bar */
    GMenu *bar = menubar_model();
    GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));
    gtk_widget_add_css_class(menubar, "ie-menu");
    g_object_unref(bar);
    gtk_box_append(GTK_BOX(vbox), menubar);

    /* find bar (hidden until Ctrl+F) */
    findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findbar, "ie-find");
    gtk_box_append(GTK_BOX(findbar), icon_image(IC_FIND, 16));
    gtk_box_append(GTK_BOX(findbar), gtk_label_new("Find:"));
    find_entry = gtk_entry_new();
    gtk_widget_set_size_request(find_entry, 260, -1);
    g_signal_connect(find_entry, "changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_next), GINT_TO_POINTER(0));
    gtk_box_append(GTK_BOX(findbar), find_entry);
    GtkWidget *fp = gtk_button_new_with_label("Previous"), *fn = gtk_button_new_with_label("Next");
    g_signal_connect(fp, "clicked", G_CALLBACK(on_find_next), GINT_TO_POINTER(1));
    g_signal_connect(fn, "clicked", G_CALLBACK(on_find_next), GINT_TO_POINTER(0));
    GtkWidget *fx = icon_button(IC_CLOSE, 14, NULL, "Close (Esc)", G_CALLBACK(on_find_close), NULL);
    gtk_box_append(GTK_BOX(findbar), fp); gtk_box_append(GTK_BOX(findbar), fn); gtk_box_append(GTK_BOX(findbar), fx);
    gtk_widget_set_visible(findbar, FALSE);
    gtk_box_append(GTK_BOX(vbox), findbar);

    /* tabs, with the Favorites buttons before them and the command bar after */
    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_widget_set_vexpand(notebook, TRUE);
    GtkWidget *favbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(favbox, "ie-fav");
    GtkWidget *favmb = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(favmb), icon_image(IC_STAR, 20));
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(favmb), FALSE);
    GMenu *fm = favorites_menu(); gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(favmb), G_MENU_MODEL(fm)); g_object_unref(fm);
    gtk_widget_set_tooltip_text(favmb, "Favorites Center");
    GtkWidget *addfav = icon_button(IC_ADDFAV, 20, NULL, "Add to Favorites (Ctrl+D)", G_CALLBACK(act_add_fav), NULL);
    gtk_box_append(GTK_BOX(favbox), favmb); gtk_box_append(GTK_BOX(favbox), addfav);
    gtk_box_append(GTK_BOX(favbox), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), favbox, GTK_PACK_START);
    GtkWidget *cmd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(cmd, "ie-cmd");
    GtkWidget *nt = gtk_button_new();                                       /* the small empty tab that opens a new one */
    gtk_widget_add_css_class(nt, "ie-newtab");
    gtk_widget_set_size_request(nt, 30, 20);
    gtk_widget_set_tooltip_text(nt, "New Tab (Ctrl+T)");
    g_signal_connect(nt, "clicked", G_CALLBACK(act_new_tab), NULL);
    gtk_widget_set_valign(nt, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(cmd), nt);
    gtk_box_append(GTK_BOX(cmd), icon_button(IC_HOME, 16, NULL, "Home (Alt+Home)", G_CALLBACK(on_home), NULL));
    GMenu *pm = page_menu(), *tm = tools_menu();
    gtk_box_append(GTK_BOX(cmd), menu_button(IC_PAGE, "Page", G_MENU_MODEL(pm), "Page"));
    gtk_box_append(GTK_BOX(cmd), menu_button(IC_TOOLS, "Tools", G_MENU_MODEL(tm), "Tools"));
    g_object_unref(pm); g_object_unref(tm);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), cmd, GTK_PACK_END);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch), NULL);
    hang_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hang_bar, "hang-bar");
    GtkWidget *hl = gtk_label_new("This page is not responding. You can wait for it, or reload it.");
    gtk_widget_set_hexpand(hl, TRUE); gtk_label_set_xalign(GTK_LABEL(hl), 0);
    GtkWidget *hw = gtk_button_new_with_label("Wait"), *hr = gtk_button_new_with_label("Reload page");
    g_signal_connect(hw, "clicked", G_CALLBACK(on_hang_wait), NULL);
    g_signal_connect(hr, "clicked", G_CALLBACK(on_hang_reload), NULL);
    gtk_box_append(GTK_BOX(hang_bar), hl); gtk_box_append(GTK_BOX(hang_bar), hw); gtk_box_append(GTK_BOX(hang_bar), hr);
    gtk_widget_set_visible(hang_bar, FALSE);
    gtk_box_append(GTK_BOX(vbox), hang_bar);
    gtk_box_append(GTK_BOX(vbox), notebook);

    /* status bar: the link or progress, the zone, the zoom */
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(sb, "ie-status");
    status = gtk_label_new("Done");
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_widget_set_hexpand(status, TRUE);
    GtkWidget *sep1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *sep2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sep1, "sep"); gtk_widget_add_css_class(sep2, "sep");
    zone_icon = icon_image(IC_GLOBE, 16);
    zone_label = gtk_label_new("Internet");
    gtk_label_set_width_chars(GTK_LABEL(zone_label), 14); gtk_label_set_xalign(GTK_LABEL(zone_label), 0);
    GtkWidget *zoom_btn = gtk_menu_button_new();
    zoom_label = gtk_label_new("100%");
    GtkWidget *zb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_box_append(GTK_BOX(zb), icon_image(IC_SEARCH, 14)); gtk_box_append(GTK_BOX(zb), zoom_label);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(zoom_btn), zb);
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(zoom_btn), FALSE);
    GMenu *zm = g_menu_new();
    add_item(zm, "Zoom In", "win.zoom-in", "<Control>plus"); add_item(zm, "Zoom Out", "win.zoom-out", "<Control>minus"); add_item(zm, "100%", "win.zoom-reset", "<Control>0");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(zoom_btn), G_MENU_MODEL(zm)); g_object_unref(zm);
    gtk_menu_button_set_direction(GTK_MENU_BUTTON(zoom_btn), GTK_ARROW_UP);
    gtk_widget_set_tooltip_text(zoom_btn, "Change the zoom level");
    gtk_box_append(GTK_BOX(sb), status); gtk_box_append(GTK_BOX(sb), sep1);
    gtk_box_append(GTK_BOX(sb), zone_icon); gtk_box_append(GTK_BOX(sb), zone_label);
    gtk_box_append(GTK_BOX(sb), sep2); gtk_box_append(GTK_BOX(sb), zoom_btn);
    gtk_box_append(GTK_BOX(vbox), sb);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(win, keys);

    gtk_window_set_child(GTK_WINDOW(win), vbox);
    new_tab(start ? start : HOME(), NULL, TRUE);
    request_file = g_getenv("WRLD_REQUEST");
    if (request_file) g_timeout_add(400, poll_requests, NULL);
    gtk_window_present(GTK_WINDOW(win));
    if (!start) gtk_widget_grab_focus(address);
}
int main(int argc, char **argv) {
    /* THE WRLD OS has no GPU drivers for Linux programs and no bubblewrap sandbox */
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);
    /* composited pages (a video, some animations) travel in shared memory, drawn on the CPU */
    g_setenv("WEBKIT_DMABUF_RENDERER_FORCE_SHM", "1", FALSE);
    g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", FALSE);
    /* GL for the pages WebKit composites: Mesa's llvmpipe on the CPU, straight away (no GPU to look for) */
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", FALSE);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", FALSE);
    /* GStreamer's ORC writes machine code to a file it maps twice (writable and executable);
     * on THE WRLD OS a shared mapping of a file is a private copy, so use ORC's C versions */
    g_setenv("ORC_CODE", "backup", FALSE);
    /* a log the user can send when something goes wrong: this process's messages and those of
     * WebKit's processes (they inherit it), with GStreamer's warnings (video and sound) */
    if (!g_getenv("WRLDWEB_LOG_CONSOLE")) {
        g_mkdir_with_parents("/mnt/c/WRLD/Temp", 0755);
        int lf = open("/mnt/c/WRLD/Temp/wrldweb.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lf >= 0) { dup2(lf, 2); close(lf); }
    }
    g_setenv("GST_DEBUG", "2", FALSE);
    g_setenv("GST_DEBUG_NO_COLOR", "1", FALSE);
    const char *proxy = g_getenv("WRLD_PROXY"), *start = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--proxy") && i + 1 < argc) proxy = argv[++i];
        else start = argv[i];
    }
    char *start_uri = start ? guess_uri(start) : NULL;
    GtkApplication *app = gtk_application_new("os.wrld.Browser", G_APPLICATION_NON_UNIQUE);
    if (proxy && *proxy) g_object_set_data(G_OBJECT(app), "wrld-proxy", (gpointer)proxy);
    g_signal_connect(app, "activate", G_CALLBACK(activate), start_uri);
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    g_free(start_uri);
    return r;
}
