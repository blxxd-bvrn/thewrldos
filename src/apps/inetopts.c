/* Internet Options: the browser's settings in one dialog, as in Windows (General, Security,
 * Privacy, Content, Connections, Programs, Advanced), opened from Control Panel and from the
 * browser's Tools menu. Every control is wired to a real setting:
 *
 *   home_page      the home page ("" = wrld://home)        both engines
 *   javascript     run scripts on pages                      both engines
 *   adblock        the ad blocker                            classic engine
 *   images         show pictures                             both engines
 *   cookies        always / no-third-party / never           WebKit (the classic engine keeps none)
 *   popups         let pages open windows by themselves      WebKit
 *   proxy_on/proxy an HTTP proxy, host:port                  WebKit
 *   smooth_scroll  animated scrolling                        WebKit
 *   browser        webkit / classic                          which engine opens
 *
 * OK and Apply save them and tell a running WebKit browser to pick them up (webkit.c). */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "fs.h"
#include "net.h"
#include "browser.h"

extern bool js_enabled, adblock_enabled, browser_show_images;
extern bool webkit_browser_available(char *why, int cap);
extern void webkit_options_changed(void);
extern bool webkit_clear_data(void);
extern void browser_classic_clear_history(void);
extern bool browser_classic_current_url(char *out, int cap);
extern int tls_trust_anchor_count(void);

#define CERT_DIR "C:\\WRLD\\Certificates"
enum { TAB_GENERAL, TAB_SECURITY, TAB_PRIVACY, TAB_CONTENT, TAB_CONNECTIONS, TAB_PROGRAMS, TAB_ADVANCED, TAB_N };
static const char *tab_names[TAB_N] = { "General", "Security", "Privacy", "Content", "Connections", "Programs", "Advanced" };
static const char *cookie_vals[3] = { "always", "no-third-party", "never" };

typedef struct {
    int tab;
    char home[512];
    bool js, adblock, images, popups, proxy_on, smooth, classic;
    int cookies;                         /* index into cookie_vals */
    char proxy[256];
    bool dirty;                          /* something changed since the last Apply */
    char note[160];                      /* a line under the tabs after an action */
} io_t;
static window_t *io_win;

static bool getb(const char *k, bool def) { const char *v = settings_get(k); return v && *v ? atoi(v) != 0 : def; }
static void load(io_t *o) {
    const char *v;
    strlcpy(o->home, (v = settings_get("home_page")) ? v : "", sizeof o->home);
    if (!o->home[0]) strcpy(o->home, "wrld://home");
    o->js = js_enabled; o->adblock = adblock_enabled; o->images = browser_show_images;
    o->popups = getb("popups", false); o->proxy_on = getb("proxy_on", false); o->smooth = getb("smooth_scroll", false);
    strlcpy(o->proxy, (v = settings_get("proxy")) ? v : "", sizeof o->proxy);
    o->cookies = 1;
    if ((v = settings_get("cookies"))) for (int i = 0; i < 3; i++) if (!strcmp(v, cookie_vals[i])) o->cookies = i;
    v = settings_get("browser"); o->classic = v && !strcasecmp(v, "classic");
    o->dirty = false;
}
static void apply(io_t *o) {
    settings_set("home_page", !strcmp(o->home, "wrld://home") ? "" : o->home);
    js_enabled = o->js; adblock_enabled = o->adblock; browser_show_images = o->images;
    settings_set("popups", o->popups ? "1" : "0");
    settings_set("cookies", cookie_vals[o->cookies]);
    settings_set("proxy_on", o->proxy_on ? "1" : "0");
    settings_set("proxy", o->proxy);
    settings_set("smooth_scroll", o->smooth ? "1" : "0");
    char why[96];
    if (o->classic || webkit_browser_available(why, sizeof why)) settings_set("browser", o->classic ? "classic" : "webkit");
    settings_save();                     /* also writes javascript, adblock and images (persist.c) */
    webkit_options_changed();
    o->dirty = false;
}

static void history_answer(void *ctx, int r) {
    (void)ctx;
    if (r != IDYES || !io_win || !wm_alive(io_win)) return;
    io_t *o = io_win->data;
    browser_classic_clear_history();
    bool wk = webkit_clear_data();
    strlcpy(o->note, wk ? "Browsing history deleted: back and forward lists, cookies, cached files and site data."
                        : "Browsing history deleted: back and forward lists.", sizeof o->note);
    io_win->dirty = true;
}
static void reset_answer(void *ctx, int r) {
    (void)ctx;
    if (r != IDYES || !io_win || !wm_alive(io_win)) return;
    io_t *o = io_win->data;
    strcpy(o->home, "wrld://home"); o->js = true; o->adblock = true; o->images = true;
    o->popups = false; o->cookies = 1; o->proxy_on = false; o->smooth = false;
    apply(o);
    strlcpy(o->note, "Every Internet Options setting is back to its default.", sizeof o->note);
    io_win->dirty = true;
}

/* a check box that marks the dialog changed */
static void check(window_t *w, io_t *o, int x, int y, const char *label, bool *v) { if (ui_checkbox(w, x, y, label, v)) o->dirty = true; }
static int para(canvas_t *s, int x, int y, int wd, const char *t) { return y + draw_text_wrapped(s, FONT_UI, x, y, wd, t, T.text, 2) + 6; }

static void page_general(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    draw_group(s, R(x, y, wd, 128), "Home page");
    draw_icon(s, ICON_BROWSER, 32, x + 12, y + 22);
    draw_text_wrapped(s, FONT_UI, x + 54, y + 20, wd - 66, "Type the address of the page that opens when you click Home or open a new tab.", T.text, 2);
    if (ui_textbox(w, R(x + 54, y + 56, wd - 66, 22), o->home, sizeof o->home)) o->dirty = true;
    int bx = x + 54, bw = (wd - 66 - 12) / 3;
    char cur[512];
    bool have_cur = browser_classic_current_url(cur, sizeof cur);
    if (ui_button_ex(w, R(bx, y + 88, bw, 24), "Use current", have_cur, false)) { strlcpy(o->home, cur, sizeof o->home); o->dirty = true; }
    if (ui_button(w, R(bx + bw + 6, y + 88, bw, 24), "Use default")) { strcpy(o->home, "wrld://home"); o->dirty = true; }
    if (ui_button(w, R(bx + 2 * (bw + 6), y + 88, bw, 24), "Use blank")) { strcpy(o->home, "about:blank"); o->dirty = true; }
    y += 140;
    draw_group(s, R(x, y, wd, 110), "Browsing history");
    draw_icon(s, ICON_RECYCLE_FULL, 32, x + 12, y + 22);
    draw_text_wrapped(s, FONT_UI, x + 54, y + 20, wd - 66, "Delete the pages you went back and forward through, cookies, saved website data and cached files.", T.text, 2);
    if (ui_button(w, R(x + wd - 100, y + 72, 88, 24), "Delete..."))
        msgbox("Delete Browsing History", "Delete the browsing history, cookies, cached files and website data of The WRLD Browser?", MBI_QUESTION, MBX_YESNO, history_answer, NULL);
    y += 122;
    draw_group(s, R(x, y, wd, 62), "Appearance");
    draw_text_wrapped(s, FONT_UI, x + 12, y + 22, wd - 24, "Colours and fonts follow the visual style chosen in Personalization.", T.gray_text, 2);
}
static void page_security(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    draw_group(s, R(x, y, wd, 96), "Scripting");
    draw_icon(s, ICON_LOCK, 32, x + 12, y + 22);
    check(w, o, x + 54, y + 24, "Enable JavaScript (active scripting)", &o->js);
    draw_text_wrapped(s, FONT_UI, x + 54, y + 48, wd - 66, "Most websites need scripts. Turning them off makes pages safer and lighter, but many will not work.", T.gray_text, 2);
    y += 108;
    draw_group(s, R(x, y, wd, 96), "Secure connections");
    char t[200];
    snprintf(t, sizeof t, "Secure (https) websites are checked against %d trusted root certificates. The connection uses TLS 1.2 or 1.3; sites with an invalid certificate are refused.", tls_trust_anchor_count());
    para(s, x + 12, y + 22, wd - 24, t);
    y += 108;
    draw_group(s, R(x, y, wd, 66), "Protection");
    draw_text_wrapped(s, FONT_UI, x + 12, y + 22, wd - 24, "The firewall and virus protection are set in the Security Center.", T.text, 2);
    if (ui_link(w, x + 12, y + 42, FONT_UI, "Open the Security Center", T.link)) security_open();
}
static void page_privacy(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    static const char *labels[3] = { "Accept all cookies", "Block third-party cookies (recommended)", "Block all cookies" };
    static const char *desc[3] = { "Every website, including advertisers embedded in pages, can save cookies.",
                                   "Sites you visit can save cookies; the advertisers and trackers inside their pages cannot.",
                                   "No website can save cookies. You will not stay signed in to websites." };
    draw_group(s, R(x, y, wd, 150), "Cookies");
    for (int i = 0; i < 3; i++) if (ui_radio(w, x + 14, y + 24 + i * 22, labels[i], o->cookies == i)) { o->cookies = i; o->dirty = true; }
    draw_text_wrapped(s, FONT_UI, x + 14, y + 98, wd - 28, desc[o->cookies], T.gray_text, 2);
    y += 162;
    draw_group(s, R(x, y, wd, 76), "Pop-up Blocker");
    bool block = !o->popups;
    if (ui_checkbox(w, x + 14, y + 24, "Turn on Pop-up Blocker", &block)) { o->popups = !block; o->dirty = true; }
    draw_text_wrapped(s, FONT_UI, x + 14, y + 46, wd - 28, "Windows you open by clicking a link are always allowed.", T.gray_text, 2);
    y += 88;
    draw_text_wrapped(s, FONT_UI, x, y, wd, "Cookies and pop-ups apply to the WebKit engine; the classic engine saves no cookies.", T.gray_text, 2);
}
static void page_content(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    draw_group(s, R(x, y, wd, 108), "Ad blocker");
    check(w, o, x + 14, y + 24, "Block advertisements and trackers", &o->adblock);
    draw_text_wrapped(s, FONT_UI, x + 14, y + 48, wd - 28, "Used by the classic engine. Sites you allow keep their ads; the list is a text file you can edit.", T.gray_text, 2);
    if (ui_button(w, R(x + wd - 132, y + 74, 120, 24), "Allowed sites...")) { adblock_page_allowed("http://x/"); notepad_open(ADBLOCK_LIST); }
    y += 120;
    draw_group(s, R(x, y, wd, 108), "Certificates");
    char t[160]; snprintf(t, sizeof t, "%d trusted root certificates. To trust another, put its .pem, .crt or .cer file in " CERT_DIR ".", tls_trust_anchor_count());
    draw_text_wrapped(s, FONT_UI, x + 14, y + 24, wd - 28, t, T.text, 2);
    if (ui_button(w, R(x + wd - 132, y + 74, 120, 24), "Certificates...")) { vfs_mkdir_p(CERT_DIR); explorer_open(CERT_DIR); }
}
static void page_connections(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    draw_group(s, R(x, y, wd, 80), "Network");
    draw_icon(s, ICON_CP_NETWORK, 32, x + 12, y + 22);
    char t[160]; snprintf(t, sizeof t, "Local Area Connection: %s", net_status_text());
    draw_text_ellipsis(s, FONT_UI, x + 54, y + 24, wd - 66, t, T.text);
    if (ui_link(w, x + 54, y + 46, FONT_UI, "Network and Sharing Center", T.link)) control_open("network");
    if (ui_link(w, x + 54 + text_width(FONT_UI, "Network and Sharing Center") + 16, y + 46, FONT_UI, "Network Connections", T.link)) netconn_open();
    y += 92;
    draw_group(s, R(x, y, wd, 128), "Proxy server");
    check(w, o, x + 14, y + 24, "Use a proxy server for your LAN", &o->proxy_on);
    draw_text(s, FONT_UI, x + 14, y + 54, "Address:", o->proxy_on ? T.text : T.gray_text);
    if (o->proxy_on) { if (ui_textbox(w, R(x + 80, y + 50, wd - 94, 22), o->proxy, sizeof o->proxy)) o->dirty = true; }
    else { draw_sunken_box(s, R(x + 80, y + 50, wd - 94, 22), T.face); draw_text_ellipsis(s, FONT_UI, x + 84, y + 54, wd - 102, o->proxy, T.gray_text); }
    draw_text_wrapped(s, FONT_UI, x + 14, y + 80, wd - 28, "For example proxy.example.com:8080. Local addresses bypass it. Used by the WebKit engine.", T.gray_text, 2);
}
static void page_programs(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    char why[96]; bool can = webkit_browser_available(why, sizeof why) || o->classic;
    draw_group(s, R(x, y, wd, 150), "Browser engine");
    if (can) {
        if (ui_radio(w, x + 14, y + 24, "WebKit (full modern websites)", !o->classic)) { o->classic = false; o->dirty = true; }
        draw_text_wrapped(s, FONT_UI, x + 34, y + 44, wd - 48, "Needs the 64-bit edition and about 2 GB of memory.", T.gray_text, 2);
        if (ui_radio(w, x + 14, y + 74, "Classic (built in, light and quick)", o->classic)) { o->classic = true; o->dirty = true; }
        draw_text_wrapped(s, FONT_UI, x + 34, y + 94, wd - 48, "Simple pages with a small JavaScript engine; runs in 256 MB.", T.gray_text, 2);
    } else {
        char t[200]; snprintf(t, sizeof t, "The WRLD Browser uses its classic engine here: WebKit cannot run (%s).", why);
        draw_text_wrapped(s, FONT_UI, x + 14, y + 24, wd - 28, t, T.text, 2);
    }
    y += 162;
    draw_group(s, R(x, y, wd, 66), "File associations");
    draw_text_wrapped(s, FONT_UI, x + 14, y + 22, wd - 28, "Choose the programs that open web pages and other files.", T.text, 2);
    if (ui_link(w, x + 14, y + 42, FONT_UI, "Default Programs", T.link)) control_open("defaults");
}
static void page_advanced(window_t *w, io_t *o, rect_t b) {
    canvas_t *s = w->surf; int x = b.x + 12, wd = b.w - 24, y = b.y + 12;
    draw_text(s, FONT_UI, x, y, "Settings:", T.text); y += 20;
    rect_t lr = R(x, y, wd, 196);
    draw_sunken_box(s, lr, T.window_bg);
    int ly = y + 8;
    draw_text(s, FONT_UI_BOLD, x + 8, ly, "Browsing", T.text); ly += 22;
    check(w, o, x + 20, ly, "Enable smooth scrolling (WebKit)", &o->smooth); ly += 22;
    check(w, o, x + 20, ly, "Enable JavaScript", &o->js); ly += 26;
    draw_text(s, FONT_UI_BOLD, x + 8, ly, "Multimedia", T.text); ly += 22;
    check(w, o, x + 20, ly, "Show pictures", &o->images); ly += 26;
    draw_text(s, FONT_UI_BOLD, x + 8, ly, "Security", T.text); ly += 22;
    bool block = !o->popups;
    if (ui_checkbox(w, x + 20, ly, "Block pop-up windows", &block)) { o->popups = !block; o->dirty = true; }
    y += 206;
    if (ui_button(w, R(x + wd - 170, y, 170, 24), "Restore advanced settings")) { o->smooth = false; o->js = true; o->images = true; o->popups = false; o->dirty = true; }
    y += 36;
    draw_group(s, R(x, y, wd, 74), "Reset Internet Options");
    draw_text_wrapped(s, FONT_UI, x + 14, y + 22, wd - 130, "Set every option on these tabs back to how it started.", T.text, 2);
    if (ui_button(w, R(x + wd - 100, y + 26, 88, 24), "Reset..."))
        msgbox("Reset Internet Options", "Reset every Internet Options setting to its default? Your browsing history is kept.", MBI_QUESTION, MBX_YESNO, reset_answer, NULL);
}

static void io_paint(window_t *w) {
    io_t *o = w->data; canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    int tx = 6;
    for (int i = 0; i < TAB_N; i++) {
        int tw = text_width(FONT_UI, tab_names[i]) + 14;
        if (ui_tab(w, R(tx, 6, tw, 22), tab_names[i], o->tab == i)) { o->tab = i; o->note[0] = 0; w->dirty = true; }
        tx += tw + 1;
    }
    rect_t body = R(6, 28, s->w - 12, s->h - 72);
    fill_rect(s, body.x, body.y, body.w, body.h, T.classic ? T.face : T.window_bg);
    draw_rect(s, body.x, body.y, body.w, body.h, T.face_dark);
    rect_t old = canvas_push_clip(s, body);
    switch (o->tab) {
    case TAB_GENERAL: page_general(w, o, body); break;
    case TAB_SECURITY: page_security(w, o, body); break;
    case TAB_PRIVACY: page_privacy(w, o, body); break;
    case TAB_CONTENT: page_content(w, o, body); break;
    case TAB_CONNECTIONS: page_connections(w, o, body); break;
    case TAB_PROGRAMS: page_programs(w, o, body); break;
    case TAB_ADVANCED: page_advanced(w, o, body); break;
    }
    if (o->note[0]) draw_text_wrapped(s, FONT_UI, body.x + 12, body.y + body.h - 40, body.w - 24, o->note, T.link_head, 2);
    s->clip = old;
    int by = s->h - 36;
    if (!w->data) return;
    if (ui_button_ex(w, R(s->w - 246, by, 76, 24), "OK", true, true)) { apply(o); wm_destroy(w); return; }
    if (ui_button(w, R(s->w - 164, by, 76, 24), "Cancel")) { wm_destroy(w); return; }
    if (ui_button_ex(w, R(s->w - 82, by, 76, 24), "Apply", o->dirty, false)) { apply(o); w->dirty = true; }
}
static void io_destroy(window_t *w) { kfree(w->data); w->data = NULL; if (io_win == w) io_win = NULL; }

void inetopts_open(int tab) {
    if (io_win && wm_alive(io_win)) {
        if (tab >= 0 && tab < TAB_N) ((io_t *)io_win->data)->tab = tab;
        io_win->dirty = true; wm_focus(io_win); return;
    }
    io_t *o = kzalloc(sizeof *o);
    load(o);
    o->tab = tab >= 0 && tab < TAB_N ? tab : 0;
    window_t *w = wm_create("Internet Options", -1, -1, 480, 470, WF_NO_MAX | WF_NO_MIN);
    w->data = o; w->paint = io_paint; w->destroy = io_destroy; w->icon = ICON_BROWSER;
    io_win = w;
    wm_center(w);
}
