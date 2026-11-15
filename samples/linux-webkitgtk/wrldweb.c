/* wrldweb: a small WebKitGTK 6 browser on THE WRLD OS, through the wrldcomp
 * Wayland compositor: back, forward, reload, an address bar and a WebKitWebView.
 *   C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/wrldweb [address]
 * With no address it shows a built-in test page. */
#include <gtk/gtk.h>
#include <webkit/webkit.h>

static const char *test_page =
    "<!doctype html><html><head><meta charset=utf-8><title>WebKit on THE WRLD OS</title><style>"
    "body{font-family:sans-serif;margin:24px;background:linear-gradient(#eef4ff,#fff)}"
    "h1{color:#1c4fa0}.box{display:flex;gap:12px}.box div{flex:1;padding:12px;border-radius:8px;color:#fff}"
    "#js{font-weight:bold}canvas{border:1px solid #888}</style></head><body>"
    "<h1>WebKitGTK is running</h1><p>HTML, CSS, JavaScript and canvas, rendered by WebKit on THE WRLD OS.</p>"
    "<div class=box><div style=background:#e44>Flexbox</div><div style=background:#4a4>CSS</div><div style=background:#44e>Layout</div></div>"
    "<p id=js>JavaScript is not running.</p><canvas id=c width=300 height=80></canvas>"
    "<p><input id=t placeholder='Type here'> <button onclick=\"document.getElementById('js').textContent='You typed: '+document.getElementById('t').value\">Go</button></p>"
    "<script>document.getElementById('js').textContent='JavaScript works: 6 * 7 = '+(6*7)+', '+navigator.userAgent.split(' ').pop();"
    "var g=document.getElementById('c').getContext('2d');for(var i=0;i<10;i++){g.fillStyle='hsl('+i*36+',80%,50%)';g.fillRect(i*30,10+i*5,26,40)}</script>"
    "</body></html>";

static GtkWidget *address;

static void go(GtkEntry *e, WebKitWebView *v) {
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    char *uri = strstr(t, "://") || g_str_has_prefix(t, "about:") || g_str_has_prefix(t, "data:") ? g_strdup(t) : g_strconcat("https://", t, NULL);
    webkit_web_view_load_uri(v, uri);
    g_free(uri);
    gtk_widget_grab_focus(GTK_WIDGET(v));
}
static void uri_changed(WebKitWebView *v, GParamSpec *p, gpointer d) {
    (void)p; (void)d;
    const char *u = webkit_web_view_get_uri(v);
    if (u && !g_str_has_prefix(u, "data:") && strcmp(u, "about:blank")) gtk_editable_set_text(GTK_EDITABLE(address), u);
}
static void title_changed(WebKitWebView *v, GParamSpec *p, GtkWindow *w) {
    (void)p;
    const char *t = webkit_web_view_get_title(v);
    gtk_window_set_title(w, t && *t ? t : "The WRLD Web");
}
static void activate(GtkApplication *app, gpointer d) {
    const char *start = d;
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "The WRLD Web");
    gtk_window_set_default_size(GTK_WINDOW(win), 800, 560);

    WebKitWebView *view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
    GtkWidget *fwd = gtk_button_new_from_icon_name("go-next-symbolic");
    GtkWidget *reload = gtk_button_new_from_icon_name("view-refresh-symbolic");
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(webkit_web_view_go_back), view);
    g_signal_connect_swapped(fwd, "clicked", G_CALLBACK(webkit_web_view_go_forward), view);
    g_signal_connect_swapped(reload, "clicked", G_CALLBACK(webkit_web_view_reload), view);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), back);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), fwd);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), reload);
    address = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(address), "Type an address");
    gtk_widget_set_hexpand(address, TRUE);
    gtk_widget_set_size_request(address, 420, -1);
    g_signal_connect(address, "activate", G_CALLBACK(go), view);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), address);
    gtk_window_set_titlebar(GTK_WINDOW(win), header);

    g_signal_connect(view, "notify::uri", G_CALLBACK(uri_changed), NULL);
    g_signal_connect(view, "notify::title", G_CALLBACK(title_changed), win);
    gtk_window_set_child(GTK_WINDOW(win), GTK_WIDGET(view));
    if (start) webkit_web_view_load_uri(view, start);
    else webkit_web_view_load_html(view, test_page, NULL);
    gtk_window_present(GTK_WINDOW(win));
}
int main(int argc, char **argv) {
    /* THE WRLD OS has no GPU drivers for Linux programs and no bubblewrap sandbox */
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", FALSE);
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);
    g_setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", FALSE);
    g_setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", FALSE);
    GtkApplication *app = gtk_application_new("os.wrld.Web", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), argc > 1 ? argv[1] : NULL);
    int r = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return r;
}
