/* The WRLD Browser on WebKit: WebKitGTK 6 in THE WRLD OS window frame, through the
 * wrldcomp Wayland compositor. Tabs, a toolbar with an address bar that also
 * searches, a bookmarks bar, a status bar, find in page, zoom, downloads into My
 * Documents\Downloads and cookies kept in the user's profile.
 *   C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/wrldweb [--proxy URL] [address]
 * The OS starts it from the desktop (src/apps/webkit.c) and passes later addresses
 * through the file named by WRLD_REQUEST, one per line ("" = a new tab). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define APP_NAME "The WRLD Browser"
#define HOME_URI "wrld:home"
#define SEARCH_URI "https://www.google.com/search?q="

static GtkWidget *win, *notebook, *address, *back_btn, *fwd_btn, *reload_btn, *status, *findbar, *find_entry;
static WebKitNetworkSession *session;
static WebKitSettings *wsettings;
static char *downloads_dir;
static const char *request_file;

static const struct { const char *name, *uri; } bookmarks[] = {
    { "Google", "https://www.google.com/" }, { "Wikipedia", "https://en.wikipedia.org/" }, { "YouTube", "https://www.youtube.com/" },
    { "Hacker News", "https://news.ycombinator.com/" }, { "DuckDuckGo", "https://duckduckgo.com/" }, { "GitHub", "https://github.com/" },
    { "BBC News", "https://www.bbc.com/news" }, { "Weather", "https://wttr.in/" },
};

static const char *start_page =
    "<!doctype html><html><head><meta charset=utf-8><title>New Tab</title><style>"
    "body{margin:0;font-family:sans-serif;background:linear-gradient(#3a6ea5,#8fb3dd 60%,#eef3fa);min-height:100vh;color:#123}"
    ".wrap{max-width:720px;margin:0 auto;padding:70px 20px}h1{color:#fff;font-weight:300;font-size:40px;margin:0 0 6px;text-shadow:0 1px 3px #0005}"
    "p.sub{color:#e8f0ff;margin:0 0 28px}form{display:flex;background:#fff;border-radius:24px;box-shadow:0 2px 10px #0003;overflow:hidden}"
    "input{flex:1;border:0;padding:13px 20px;font-size:17px;outline:none}button{border:0;background:#2a5fb0;color:#fff;padding:0 22px;font-size:15px;cursor:pointer}"
    ".tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin-top:34px}"
    ".tiles a{display:block;background:#ffffffd9;border-radius:10px;padding:16px 10px;text-align:center;text-decoration:none;color:#123;box-shadow:0 1px 4px #0002}"
    ".tiles a:hover{background:#fff}.tiles b{display:block;font-size:22px;margin-bottom:6px;color:#2a5fb0}"
    ".foot{margin-top:40px;font-size:12px;color:#456}</style></head><body><div class=wrap>"
    "<h1>The WRLD Browser</h1><p class=sub>Powered by WebKit on THE WRLD OS</p>"
    "<form action='https://www.google.com/search'><input name=q placeholder='Search the web or type an address' autofocus><button>Search</button></form>"
    "<div class=tiles>"
    "<a href='https://www.google.com/'><b>G</b>Google</a><a href='https://en.wikipedia.org/'><b>W</b>Wikipedia</a>"
    "<a href='https://www.youtube.com/'><b>YT</b>YouTube</a><a href='https://news.ycombinator.com/'><b>HN</b>Hacker News</a>"
    "<a href='https://duckduckgo.com/'><b>D</b>DuckDuckGo</a><a href='https://github.com/'><b>GH</b>GitHub</a>"
    "<a href='https://www.bbc.com/news'><b>N</b>BBC News</a><a href='https://wttr.in/'><b>Wx</b>Weather</a></div>"
    "<p class=foot>Ctrl+T new tab &middot; Ctrl+L address bar &middot; Ctrl+F find &middot; Ctrl+plus / minus zoom</p></div></body></html>";

static const char *css =
    "window.wrld { background: #ECE9D8; }"
    ".wrld-toolbar { background-image: linear-gradient(#FEFEFC, #ECE9D8); border-bottom: 1px solid #ACA899; padding: 3px 4px; }"
    ".wrld-toolbar button { min-height: 26px; min-width: 26px; padding: 2px 6px; }"
    ".wrld-bookmarks { background: #F6F5F0; border-bottom: 1px solid #D5D2C5; padding: 1px 4px; }"
    ".wrld-bookmarks button { min-height: 22px; padding: 1px 8px; font-size: 9pt; }"
    ".wrld-status { background: #ECE9D8; border-top: 1px solid #ACA899; padding: 2px 8px; font-size: 9pt; color: #333; }"
    "entry.wrld-address { min-height: 26px; }"
    "notebook > header { background: #DCD9CB; }"
    "notebook > header tab { min-height: 24px; padding: 2px 6px; }"
    "notebook > header tab label { font-size: 9pt; }";

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

/* ---------------- tabs ---------------- */
static WebKitWebView *view_at(int i) {
    GtkWidget *p = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
    return p ? WEBKIT_WEB_VIEW(p) : NULL;
}
static WebKitWebView *current(void) { return view_at(gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook))); }
static void set_status(const char *s) { gtk_label_set_text(GTK_LABEL(status), s && *s ? s : "Done"); }

static void update_window_title(WebKitWebView *v) {
    const char *t = webkit_web_view_get_title(v);
    char *s = g_strdup_printf("%s - " APP_NAME, t && *t ? t : "New Tab");
    gtk_window_set_title(GTK_WINDOW(win), s);
    g_free(s);
}
static void update_toolbar(WebKitWebView *v) {
    if (!v || v != current()) return;
    gtk_widget_set_sensitive(back_btn, webkit_web_view_can_go_back(v));
    gtk_widget_set_sensitive(fwd_btn, webkit_web_view_can_go_forward(v));
    gtk_button_set_icon_name(GTK_BUTTON(reload_btn), webkit_web_view_is_loading(v) ? "media-playback-stop-symbolic" : "view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reload_btn, webkit_web_view_is_loading(v) ? "Stop (Esc)" : "Reload (F5)");
    if (!gtk_widget_has_focus(address) || !gtk_editable_get_text(GTK_EDITABLE(address))[0]) {
        const char *u = webkit_web_view_get_uri(v);
        gtk_editable_set_text(GTK_EDITABLE(address), u && strcmp(u, HOME_URI) && strcmp(u, "about:blank") ? u : "");
    }
    double p = webkit_web_view_get_estimated_load_progress(v);
    gtk_entry_set_progress_fraction(GTK_ENTRY(address), webkit_web_view_is_loading(v) && p < 1.0 ? p : 0.0);
    const char *u = webkit_web_view_get_uri(v);
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(address), GTK_ENTRY_ICON_PRIMARY,   /* a padlock for secure pages */
                                      u && g_str_has_prefix(u, "https://") ? "changes-prevent-symbolic" : NULL);
    update_window_title(v);
}
static GtkWidget *tab_label_of(WebKitWebView *v) { return g_object_get_data(G_OBJECT(v), "wrld-label"); }

static void on_title(WebKitWebView *v, GParamSpec *ps, gpointer d) {
    (void)ps; (void)d;
    const char *t = webkit_web_view_get_title(v);
    GtkWidget *l = tab_label_of(v);
    if (l) gtk_label_set_text(GTK_LABEL(l), t && *t ? t : "New Tab");
    if (v == current()) update_window_title(v);
}
static void on_changed(WebKitWebView *v, GParamSpec *ps, gpointer d) { (void)ps; (void)d; update_toolbar(v); }
static void on_load(WebKitWebView *v, WebKitLoadEvent e, gpointer d) {
    (void)d;
    if (v == current()) set_status(e == WEBKIT_LOAD_FINISHED ? "Done" : "Loading...");
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
        ? WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, "settings", wsettings, "website-policies", pol, NULL))
        : WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session, "settings", wsettings, "website-policies", pol, NULL));
    g_object_unref(pol);
    gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 16); gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *x = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(x), FALSE);
    gtk_widget_set_tooltip_text(x, "Close tab (Ctrl+W)");
    g_signal_connect(x, "clicked", G_CALLBACK(on_close_clicked), v);
    gtk_box_append(GTK_BOX(box), label); gtk_box_append(GTK_BOX(box), x);
    g_object_set_data(G_OBJECT(v), "wrld-label", label);
    int at = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook)) + 1;
    int i = gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), GTK_WIDGET(v), box, at);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), GTK_WIDGET(v), TRUE);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(v, "notify::uri", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_changed), NULL);
    g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_changed), NULL);
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

/* ---------------- the toolbar ---------------- */
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
static void on_go(GtkEntry *e, gpointer d) {
    (void)d;
    char *u = guess_uri(gtk_editable_get_text(GTK_EDITABLE(e)));
    WebKitWebView *v = current();
    load(v, u); g_free(u);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}
static void on_back(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_go_back(current()); }
static void on_fwd(GtkButton *b, gpointer d) { (void)b; (void)d; webkit_web_view_go_forward(current()); }
static void on_reload(GtkButton *b, gpointer d) {
    (void)b; (void)d; WebKitWebView *v = current();
    if (webkit_web_view_is_loading(v)) webkit_web_view_stop_loading(v); else webkit_web_view_reload(v);
}
static void on_home(GtkButton *b, gpointer d) { (void)b; (void)d; load(current(), HOME_URI); }
static void on_bookmark(GtkButton *b, gpointer uri) { (void)b; load(current(), uri); }
static void zoom(double f) {
    WebKitWebView *v = current();
    double z = f == 0 ? 1.0 : webkit_web_view_get_zoom_level(v) * f;
    webkit_web_view_set_zoom_level(v, CLAMP(z, 0.3, 3.0));
    char s[48]; snprintf(s, sizeof s, "Zoom: %d%%", (int)(webkit_web_view_get_zoom_level(v) * 100 + 0.5));
    set_status(s);
}

/* find in page */
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

/* the menu */
static void act_new_tab(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; new_tab(HOME_URI, NULL, TRUE); gtk_widget_grab_focus(address); }
static void act_close_tab(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; close_tab(current()); }
static void act_find(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; show_find(TRUE); }
static void act_zoom_in(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(1.1); }
static void act_zoom_out(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(1 / 1.1); }
static void act_zoom_reset(GSimpleAction *a, GVariant *p, gpointer d) { (void)a; (void)p; (void)d; zoom(0); }
static void act_about(GSimpleAction *a, GVariant *p, gpointer d) {
    (void)a; (void)p; (void)d;
    char *html = g_strdup_printf("<!doctype html><title>About</title><body style='font-family:sans-serif;margin:40px'>"
        "<h1 style='font-weight:300'>The WRLD Browser</h1><p>WebKitGTK %u.%u.%u in THE WRLD OS's Linux layer, through the wrldcomp Wayland compositor.</p>"
        "<p>Pages are drawn on the CPU. Downloads go to My Documents\\Downloads; cookies are kept in your profile.</p>"
        "<p>The built-in browser is still there: run <code>start classicbrowser</code> in the Command Prompt.</p>",
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    webkit_web_view_load_html(new_tab(NULL, NULL, TRUE), html, "about:wrld");
    g_free(html);
}

/* keys the browser handles before the page does */
static gboolean on_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType m, gpointer d) {
    (void)k; (void)code; (void)d;
    gboolean ctrl = (m & GDK_CONTROL_MASK) != 0, alt = (m & GDK_ALT_MASK) != 0, shift = (m & GDK_SHIFT_MASK) != 0;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)), cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (ctrl && (key == GDK_KEY_t || key == GDK_KEY_T)) { act_new_tab(NULL, NULL, NULL); return TRUE; }
    if (ctrl && (key == GDK_KEY_w || key == GDK_KEY_W)) { close_tab(current()); return TRUE; }
    if ((ctrl && (key == GDK_KEY_l || key == GDK_KEY_L)) || key == GDK_KEY_F6) { gtk_widget_grab_focus(address); gtk_editable_select_region(GTK_EDITABLE(address), 0, -1); return TRUE; }
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
static gboolean on_dl_dest(WebKitDownload *dl, char *name, gpointer d) {
    (void)d;
    g_mkdir_with_parents(downloads_dir, 0755);
    char *base = g_path_get_basename(name && *name ? name : "download");
    char *path = g_build_filename(downloads_dir, base, NULL);
    for (int i = 2; g_file_test(path, G_FILE_TEST_EXISTS) && i < 100; i++) {   /* keep earlier files */
        g_free(path); char *b2 = g_strdup_printf("(%d) %s", i, base); path = g_build_filename(downloads_dir, b2, NULL); g_free(b2);
    }
    webkit_download_set_destination(dl, path);
    char *s = g_strdup_printf("Downloading %s...", base); set_status(s); g_free(s);
    g_free(path); g_free(base);
    return TRUE;
}
static void on_dl_done(WebKitDownload *dl, gpointer d) {
    (void)d;
    const char *dest = webkit_download_get_destination(dl);
    char *base = dest ? g_path_get_basename(dest) : g_strdup("the file");
    char *s = g_strdup_printf("Saved %s in My Documents\\Downloads", base); set_status(s); g_free(s); g_free(base);
}
static void on_dl_failed(WebKitDownload *dl, GError *e, gpointer d) {
    (void)dl; (void)d; char *s = g_strdup_printf("Download failed: %s", e ? e->message : "error"); set_status(s); g_free(s);
}
static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer d) {
    (void)s; (void)d;
    g_signal_connect(dl, "decide-destination", G_CALLBACK(on_dl_dest), NULL);
    g_signal_connect(dl, "finished", G_CALLBACK(on_dl_done), NULL);
    g_signal_connect(dl, "failed", G_CALLBACK(on_dl_failed), NULL);
}

/* ---------------- wrld: pages and requests from the desktop ---------------- */
static void wrld_scheme(WebKitURISchemeRequest *r, gpointer d) {
    (void)d;
    GInputStream *in = g_memory_input_stream_new_from_data(start_page, strlen(start_page), NULL);
    webkit_uri_scheme_request_finish(r, in, strlen(start_page), "text/html");
    g_object_unref(in);
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
        new_tab(**l ? *l : HOME_URI, NULL, TRUE);
    }
    g_strfreev(lines); g_free(text);
    return G_SOURCE_CONTINUE;
}

/* ---------------- the window ---------------- */
static GtkWidget *tool_button(const char *icon, const char *tip, GCallback cb) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}
static void activate(GtkApplication *app, gpointer d) {
    const char *start = d;
    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* where things are kept: the profile on C:, a cache in memory */
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
    const char *docs = g_getenv("WRLD_DOCS");
    downloads_dir = docs ? g_build_filename(docs, "Downloads", NULL) : g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_signal_connect(session, "download-started", G_CALLBACK(on_download), NULL);
    const char *proxy = g_object_get_data(G_OBJECT(app), "wrld-proxy");
    if (proxy) {
        const char *local[] = { "localhost", "127.0.0.0/8", NULL };
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(proxy, local);
        webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }
    webkit_web_context_register_uri_scheme(webkit_web_context_get_default(), "wrld", wrld_scheme, NULL, NULL);
    webkit_security_manager_register_uri_scheme_as_local(webkit_web_context_get_security_manager(webkit_web_context_get_default()), "wrld");
    wsettings = webkit_settings_new();
    webkit_settings_set_hardware_acceleration_policy(wsettings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_smooth_scrolling(wsettings, FALSE);   /* fewer repaints on the CPU */
    webkit_settings_set_enable_developer_extras(wsettings, FALSE);

    win = gtk_application_window_new(app);
    gtk_widget_add_css_class(win, "wrld");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);   /* THE WRLD OS draws the frame */
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_window_set_title(GTK_WINDOW(win), APP_NAME);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* toolbar */
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(tb, "wrld-toolbar");
    back_btn = tool_button("go-previous-symbolic", "Back (Alt+Left)", G_CALLBACK(on_back));
    fwd_btn = tool_button("go-next-symbolic", "Forward (Alt+Right)", G_CALLBACK(on_fwd));
    reload_btn = tool_button("view-refresh-symbolic", "Reload (F5)", G_CALLBACK(on_reload));
    GtkWidget *home = tool_button("user-home-symbolic", "Home (Alt+Home)", G_CALLBACK(on_home));
    address = gtk_entry_new();
    gtk_widget_add_css_class(address, "wrld-address");
    gtk_entry_set_placeholder_text(GTK_ENTRY(address), "Search Google or type an address");
    gtk_widget_set_hexpand(address, TRUE);
    gtk_entry_set_input_purpose(GTK_ENTRY(address), GTK_INPUT_PURPOSE_URL);
    g_signal_connect(address, "activate", G_CALLBACK(on_go), NULL);
    GMenu *menu = g_menu_new();
    static const char *items[][3] = { { "New tab", "win.new-tab", "<Control>t" }, { "Close tab", "win.close-tab", "<Control>w" },
                                      { "Find in page...", "win.find", "<Control>f" } };
    for (unsigned i = 0; i < G_N_ELEMENTS(items); i++) {
        GMenuItem *it = g_menu_item_new(items[i][0], items[i][1]);
        g_menu_item_set_attribute(it, "accel", "s", items[i][2]);     /* shown in the menu; the keys are handled in on_key */
        g_menu_append_item(menu, it); g_object_unref(it);
    }
    GMenu *zm = g_menu_new();
    g_menu_append(zm, "Zoom in", "win.zoom-in"); g_menu_append(zm, "Zoom out", "win.zoom-out"); g_menu_append(zm, "Actual size", "win.zoom-reset");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(zm));
    g_menu_append(menu, "About The WRLD Browser", "win.about");
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
    gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(mb), FALSE);
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(mb, "Menu");
    static const GActionEntry acts[] = {
        { "new-tab", act_new_tab, NULL, NULL, NULL, { 0 } }, { "close-tab", act_close_tab, NULL, NULL, NULL, { 0 } },
        { "find", act_find, NULL, NULL, NULL, { 0 } }, { "zoom-in", act_zoom_in, NULL, NULL, NULL, { 0 } },
        { "zoom-out", act_zoom_out, NULL, NULL, NULL, { 0 } }, { "zoom-reset", act_zoom_reset, NULL, NULL, NULL, { 0 } },
        { "about", act_about, NULL, NULL, NULL, { 0 } },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win), acts, G_N_ELEMENTS(acts), NULL);
    gtk_box_append(GTK_BOX(tb), back_btn); gtk_box_append(GTK_BOX(tb), fwd_btn); gtk_box_append(GTK_BOX(tb), reload_btn);
    gtk_box_append(GTK_BOX(tb), home); gtk_box_append(GTK_BOX(tb), address); gtk_box_append(GTK_BOX(tb), mb);
    gtk_box_append(GTK_BOX(vbox), tb);

    /* bookmarks bar */
    GtkWidget *bm = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bm, "wrld-bookmarks");
    for (unsigned i = 0; i < G_N_ELEMENTS(bookmarks); i++) {
        GtkWidget *b = gtk_button_new_with_label(bookmarks[i].name);
        gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
        gtk_widget_set_tooltip_text(b, bookmarks[i].uri);
        g_signal_connect(b, "clicked", G_CALLBACK(on_bookmark), (gpointer)bookmarks[i].uri);
        gtk_box_append(GTK_BOX(bm), b);
    }
    gtk_box_append(GTK_BOX(vbox), bm);

    /* find bar (hidden until Ctrl+F) */
    findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findbar, "wrld-bookmarks");
    gtk_box_append(GTK_BOX(findbar), gtk_label_new("Find:"));
    find_entry = gtk_entry_new();
    gtk_widget_set_size_request(find_entry, 260, -1);
    g_signal_connect(find_entry, "changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_next), GINT_TO_POINTER(0));
    gtk_box_append(GTK_BOX(findbar), find_entry);
    GtkWidget *fp = tool_button("go-up-symbolic", "Previous (Shift+F3)", G_CALLBACK(on_find_next));
    g_signal_handlers_disconnect_by_func(fp, G_CALLBACK(on_find_next), NULL);
    g_signal_connect(fp, "clicked", G_CALLBACK(on_find_next), GINT_TO_POINTER(1));
    GtkWidget *fn = tool_button("go-down-symbolic", "Next (F3)", G_CALLBACK(on_find_next));
    GtkWidget *fx = tool_button("window-close-symbolic", "Close (Esc)", G_CALLBACK(on_find_close));
    gtk_box_append(GTK_BOX(findbar), fp); gtk_box_append(GTK_BOX(findbar), fn); gtk_box_append(GTK_BOX(findbar), fx);
    gtk_widget_set_visible(findbar, FALSE);
    gtk_box_append(GTK_BOX(vbox), findbar);

    /* tabs */
    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook), FALSE);
    gtk_widget_set_vexpand(notebook, TRUE);
    GtkWidget *plus = tool_button("list-add-symbolic", "New tab (Ctrl+T)", G_CALLBACK(act_new_tab));
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook), plus, GTK_PACK_END);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch), NULL);
    gtk_box_append(GTK_BOX(vbox), notebook);

    /* status bar */
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(sb, "wrld-status");
    status = gtk_label_new("Done");
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_widget_set_hexpand(status, TRUE);
    char ver[48]; snprintf(ver, sizeof ver, "WebKit %u.%u", webkit_get_major_version(), webkit_get_minor_version());
    gtk_box_append(GTK_BOX(sb), status); gtk_box_append(GTK_BOX(sb), gtk_label_new(ver));
    gtk_box_append(GTK_BOX(vbox), sb);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(win, keys);

    gtk_window_set_child(GTK_WINDOW(win), vbox);
    new_tab(start ? start : HOME_URI, NULL, TRUE);
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
    /* GStreamer's ORC writes machine code to a file it maps twice (writable and executable);
     * on THE WRLD OS a shared mapping of a file is a private copy, so use ORC's C versions */
    g_setenv("ORC_CODE", "backup", FALSE);
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
