/* Downloads the way Internet Explorer asks (included by wrldweb.c):
 *  1. "File Download - Security Warning": the file's name, type and size and the server it comes
 *     from, with Run, Save and Cancel, and the warning about files from the Internet.
 *  2. Then the "File Download" progress window: the server and the file, a progress bar, the time
 *     left, where it goes and how fast, "Close this dialog box when download completes", and
 *     Open / Open Folder / Cancel (Close once it is done).
 * Run keeps the file in C:\WRLD\Temp\Downloads and has THE WRLD OS open it when it is complete;
 * Save asks where (My Documents\Downloads to start with). Open and Open Folder go through
 * /dev/wrld/window (WRLDWIN_OPEN), as a double-click in Explorer would. */
#include <sys/ioctl.h>
#include <fcntl.h>
#include "../linux-gnome/wrldwin.h"

typedef struct {
    WebKitDownload *dl;
    char *name, *host, *type_desc, *dest;
    guint64 size;
    int mode;                                /* 0 asking, 1 run, 2 save */
    gboolean done, failed;
    GtkWidget *win, *bar, *info, *left, *to, *rate, *chk, *open_btn, *folder_btn, *cancel_btn, *anim, *head;
    gint64 t0;
    guint tick;
    int frame;
} dl_t;

/* ---- small helpers ---- */
static void os_open(const char *linux_path) {
    int fd = open("/dev/wrld/window", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    char b[512]; snprintf(b, sizeof b, "%s", linux_path);
    ioctl(fd, WRLDWIN_OPEN, b);
    close(fd);
}
static char *wrld_path(const char *p) {                       /* /mnt/c/X/Y -> C:\X\Y */
    if (p && !strncmp(p, "/mnt/", 5) && p[5] && (p[6] == '/' || !p[6])) {
        char *s = g_strdup_printf("%c:%s", g_ascii_toupper(p[5]), p[6] ? p + 6 : "\\");
        for (char *q = s; *q; q++) if (*q == '/') *q = '\\';
        return s;
    }
    return g_strdup(p ? p : "");
}
static char *fmt_size(double b) {
    if (b >= 1024.0 * 1024 * 1024) return g_strdup_printf("%.2fGB", b / (1024.0 * 1024 * 1024));
    if (b >= 1024.0 * 1024) return g_strdup_printf("%.2fMB", b / (1024.0 * 1024));
    if (b >= 1024.0) return g_strdup_printf("%.0fKB", b / 1024.0);
    return g_strdup_printf("%.0f bytes", b);
}
static char *fmt_time(double s) {
    if (s < 1) s = 1;
    if (s < 60) return g_strdup_printf("%.0f sec", s);
    if (s < 3600) return g_strdup_printf("%d min %d sec", (int)s / 60, (int)s % 60);
    return g_strdup_printf("%d hr %d min", (int)s / 3600, ((int)s % 3600) / 60);
}
/* what Windows calls the file type */
static char *type_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return g_strdup("File");
    static const struct { const char *ext, *desc; } known[] = {
        { "exe", "Application" }, { "msi", "Windows Installer Package" }, { "zip", "Compressed (zipped) Folder" },
        { "pdf", "PDF Document" }, { "txt", "Text Document" }, { "htm", "HTML Document" }, { "html", "HTML Document" },
        { "jpg", "JPEG Image" }, { "jpeg", "JPEG Image" }, { "png", "PNG Image" }, { "gif", "GIF Image" }, { "bmp", "Bitmap Image" },
        { "mp3", "MP3 Format Sound" }, { "wav", "Wave Sound" }, { "mp4", "MP4 Video" }, { "iso", "Disc Image File" },
        { "dmg", "Apple Disk Image" }, { "deb", "Debian Package" }, { "rpm", "RPM Package" }, { "doc", "Microsoft Word Document" },
        { "docx", "Microsoft Word Document" }, { "rtf", "Rich Text Document" }, { "sh", "Shell Script" },
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(known); i++) if (!g_ascii_strcasecmp(dot + 1, known[i].ext)) return g_strdup(known[i].desc);
    char *up = g_ascii_strup(dot + 1, -1), *s = g_strdup_printf("%s File", up);
    g_free(up); return s;
}

/* ---- the dialogs' pictures, drawn with Cairo like the browser's own icons ---- */
typedef void (*paint_fn)(cairo_t *c, int s, const char *arg);
static GtkWidget *picture(paint_fn fn, int size, const char *arg) {
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *c = cairo_create(sf); fn(c, size, arg); cairo_destroy(c); cairo_surface_flush(sf);
    GBytes *b = g_bytes_new(cairo_image_surface_get_data(sf), (gsize)cairo_image_surface_get_stride(sf) * size);
    GdkTexture *t = gdk_memory_texture_new(size, size, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, b, cairo_image_surface_get_stride(sf));
    g_bytes_unref(b); cairo_surface_destroy(sf);
    GtkWidget *im = gtk_image_new_from_paintable(GDK_PAINTABLE(t));
    gtk_image_set_pixel_size(GTK_IMAGE(im), size);
    g_object_unref(t);
    return im;
}
static void paint_file(cairo_t *c, int s, const char *ext) {          /* a page with a folded corner and the extension */
    double k = s / 32.0;
    cairo_scale(c, k, k);
    cairo_move_to(c, 6, 2); cairo_line_to(c, 20, 2); cairo_line_to(c, 26, 8); cairo_line_to(c, 26, 30); cairo_line_to(c, 6, 30); cairo_close_path(c);
    cairo_set_source_rgb(c, 1, 1, 1); cairo_fill_preserve(c);
    cairo_set_source_rgb(c, 0.45, 0.5, 0.58); cairo_set_line_width(c, 1); cairo_stroke(c);
    cairo_move_to(c, 20, 2); cairo_line_to(c, 20, 8); cairo_line_to(c, 26, 8);
    cairo_set_source_rgb(c, 0.85, 0.88, 0.92); cairo_fill_preserve(c); cairo_set_source_rgb(c, 0.45, 0.5, 0.58); cairo_stroke(c);
    cairo_rectangle(c, 3, 17, 20, 10); cairo_set_source_rgb(c, 0.95, 0.72, 0.16); cairo_fill(c);
    if (ext && *ext) {
        char e[5] = { 0 }; for (int i = 0; i < 4 && ext[i]; i++) e[i] = g_ascii_toupper(ext[i]);
        cairo_select_font_face(c, "Liberation Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(c, strlen(e) > 3 ? 6 : 7.5);
        cairo_text_extents_t te; cairo_text_extents(c, e, &te);
        cairo_move_to(c, 13 - te.width / 2 - te.x_bearing, 25); cairo_set_source_rgb(c, 0.2, 0.15, 0.05); cairo_show_text(c, e);
    }
}
static void paint_folder(cairo_t *c, int s, const char *u) {
    (void)u; double k = s / 32.0; cairo_scale(c, k, k);
    cairo_move_to(c, 2, 8); cairo_line_to(c, 11, 8); cairo_line_to(c, 14, 11); cairo_line_to(c, 29, 11); cairo_line_to(c, 29, 27); cairo_line_to(c, 2, 27); cairo_close_path(c);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 8, 0, 27);
    cairo_pattern_add_color_stop_rgb(g, 0, 1.0, 0.86, 0.45); cairo_pattern_add_color_stop_rgb(g, 1, 0.93, 0.68, 0.18);
    cairo_set_source(c, g); cairo_fill_preserve(c); cairo_pattern_destroy(g);
    cairo_set_source_rgb(c, 0.72, 0.52, 0.12); cairo_set_line_width(c, 1); cairo_stroke(c);
    cairo_rectangle(c, 6, 5, 18, 14); cairo_set_source_rgb(c, 1, 1, 1); cairo_fill_preserve(c); cairo_set_source_rgb(c, 0.6, 0.6, 0.6); cairo_stroke(c);
    cairo_move_to(c, 2, 14); cairo_line_to(c, 29, 14); cairo_line_to(c, 27, 27); cairo_line_to(c, 4, 27); cairo_close_path(c);
    cairo_set_source_rgb(c, 0.98, 0.79, 0.33); cairo_fill(c);
}
static void paint_shield(cairo_t *c, int s, const char *u) {           /* the yellow-and-blue security shield */
    (void)u; double k = s / 32.0; cairo_scale(c, k, k);
    cairo_move_to(c, 16, 2); cairo_curve_to(c, 22, 5, 26, 5, 28, 5); cairo_curve_to(c, 28, 18, 24, 26, 16, 30);
    cairo_curve_to(c, 8, 26, 4, 18, 4, 5); cairo_curve_to(c, 6, 5, 10, 5, 16, 2); cairo_close_path(c);
    cairo_save(c); cairo_clip_preserve(c);
    cairo_set_source_rgb(c, 0.98, 0.78, 0.12); cairo_paint(c);
    cairo_set_source_rgb(c, 0.16, 0.42, 0.85); cairo_rectangle(c, 16, 0, 16, 16); cairo_fill(c); cairo_rectangle(c, 0, 16, 16, 16); cairo_fill(c);
    cairo_restore(c);
    cairo_set_source_rgb(c, 0.35, 0.3, 0.1); cairo_set_line_width(c, 1); cairo_stroke(c);
    cairo_set_source_rgb(c, 1, 1, 1); cairo_rectangle(c, 14.5, 9, 3, 10); cairo_fill(c); cairo_rectangle(c, 14.5, 21, 3, 3); cairo_fill(c);
}
static void paint_page(cairo_t *c, int s, const char *u) { (void)u; paint_file(c, s, NULL); }

static GtkWidget *label(const char *text, gboolean bold, gboolean wrap) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (bold) gtk_widget_add_css_class(l, "dl-bold");
    if (wrap) { gtk_label_set_wrap(GTK_LABEL(l), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(l), 52); }
    return l;
}
static GtkWidget *dl_window(const char *title, int w, int h, gboolean modal) {
    GtkWidget *d = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(win));
    gtk_window_set_modal(GTK_WINDOW(d), modal);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(d), FALSE);          /* THE WRLD OS draws the frame */
    gtk_window_set_default_size(GTK_WINDOW(d), w, h);
    gtk_widget_add_css_class(d, "dl-dialog");
    return d;
}

/* ---- the progress window ---- */
static void dl_free(dl_t *d) {
    if (d->tick) g_source_remove(d->tick);
    g_free(d->name); g_free(d->host); g_free(d->type_desc); g_free(d->dest);
    g_object_unref(d->dl);
    g_free(d);
}
static void dl_close(dl_t *d) {
    if (d->win) { GtkWidget *w = d->win; d->win = NULL; gtk_window_destroy(GTK_WINDOW(w)); }
    if (d->done || d->failed) dl_free(d);
}
static gboolean on_dl_win_close(GtkWindow *w, gpointer p) {
    dl_t *d = p; (void)w;
    if (!d->done && !d->failed) webkit_download_cancel(d->dl);   /* closing it while it runs cancels, as in IE */
    d->win = NULL;
    if (d->done || d->failed) dl_free(d);
    return FALSE;
}
static void on_dl_cancel(GtkButton *b, gpointer p) {
    dl_t *d = p; (void)b;
    if (d->done || d->failed) { dl_close(d); return; }
    webkit_download_cancel(d->dl);
}
static void on_dl_open(GtkButton *b, gpointer p) { dl_t *d = p; (void)b; if (d->dest) os_open(d->dest); dl_close(d); }
static void on_dl_folder(GtkButton *b, gpointer p) { dl_t *d = p; (void)b; if (d->dest) { char *dir = g_path_get_dirname(d->dest); os_open(dir); g_free(dir); } dl_close(d); }
/* the page flying from the globe to the folder */
static void anim_draw(GtkDrawingArea *a, cairo_t *c, int w, int h, gpointer p) {
    dl_t *d = p; (void)a;
    double t = d->done ? 1.0 : (d->frame % 24) / 23.0;
    double x = 44 + t * (w - 44 - 72), y = 18 - sin(t * G_PI) * 12;
    if (!d->done) {
        cairo_save(c); cairo_translate(c, x, y + h / 2.0 - 16); cairo_scale(c, 0.55, 0.55);
        paint_page(c, 32, NULL); cairo_restore(c);
    }
}
static gboolean dl_tick(gpointer p) {
    dl_t *d = p;
    d->frame++;
    if (d->anim) gtk_widget_queue_draw(d->anim);
    if (!d->win || d->done || d->failed) return G_SOURCE_CONTINUE;
    guint64 got = webkit_download_get_received_data_length(d->dl);
    double el = (g_get_monotonic_time() - d->t0) / 1e6, rate = el > 0.2 ? got / el : 0;
    guint64 total = d->size ? d->size : (guint64)(webkit_download_get_estimated_progress(d->dl) > 0 ? got / webkit_download_get_estimated_progress(d->dl) : 0);
    double frac = total ? (double)got / total : 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->bar), frac);
    if (!total) gtk_progress_bar_pulse(GTK_PROGRESS_BAR(d->bar));
    char *g = fmt_size(got), *t = total ? fmt_size(total) : g_strdup("an unknown size");
    char *left = (total && rate > 0) ? fmt_time((total - got) / rate) : g_strdup("Not known");
    char *s1 = g_strdup_printf("%s (%s of %s copied)", left, g, t);
    gtk_label_set_text(GTK_LABEL(d->left), s1);
    char *r = fmt_size(rate), *s2 = g_strdup_printf("%s/Sec", r);
    gtk_label_set_text(GTK_LABEL(d->rate), s2);
    char *title = g_strdup_printf("%d%% of %s Completed", (int)(frac * 100), d->name);
    gtk_window_set_title(GTK_WINDOW(d->win), title);
    gtk_label_set_text(GTK_LABEL(d->head), "Saving:");
    g_free(g); g_free(t); g_free(left); g_free(s1); g_free(r); g_free(s2); g_free(title);
    return G_SOURCE_CONTINUE;
}
static GtkWidget *row(GtkWidget *grid, int y, const char *key, GtkWidget **val) {
    GtkWidget *k = label(key, FALSE, FALSE); gtk_widget_set_size_request(k, 118, -1);
    *val = label("", FALSE, FALSE); gtk_label_set_ellipsize(GTK_LABEL(*val), PANGO_ELLIPSIZE_MIDDLE); gtk_widget_set_hexpand(*val, TRUE);
    gtk_grid_attach(GTK_GRID(grid), k, 0, y, 1, 1); gtk_grid_attach(GTK_GRID(grid), *val, 1, y, 1, 1);
    return k;
}
static void dl_progress_window(dl_t *d) {
    d->win = dl_window("File Download", 430, 300, FALSE);
    g_signal_connect(d->win, "close-request", G_CALLBACK(on_dl_win_close), d);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(v, "dl-body");
    /* globe, flying page, folder */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(top), icon_image(IC_GLOBE, 32));
    d->anim = gtk_drawing_area_new(); gtk_widget_set_hexpand(d->anim, TRUE); gtk_widget_set_size_request(d->anim, -1, 40);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->anim), anim_draw, d, NULL);
    gtk_box_append(GTK_BOX(top), d->anim);
    gtk_box_append(GTK_BOX(top), picture(paint_folder, 32, NULL));
    gtk_box_append(GTK_BOX(v), top);
    d->head = label("Getting File Information:", FALSE, FALSE);
    gtk_box_append(GTK_BOX(v), d->head);
    char *what = g_strdup_printf("%s from %s", d->name, d->host);
    d->info = label(what, FALSE, FALSE); g_free(what);
    gtk_label_set_ellipsize(GTK_LABEL(d->info), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(v), d->info);
    d->bar = gtk_progress_bar_new(); gtk_widget_add_css_class(d->bar, "dl-bar");
    gtk_box_append(GTK_BOX(v), d->bar);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    row(grid, 0, "Estimated time left:", &d->left);
    row(grid, 1, "Download to:", &d->to);
    row(grid, 2, "Transfer rate:", &d->rate);
    char *wp = wrld_path(d->dest); gtk_label_set_text(GTK_LABEL(d->to), wp); g_free(wp);
    gtk_box_append(GTK_BOX(v), grid);
    d->chk = gtk_check_button_new_with_label("Close this dialog box when download completes");
    gtk_box_append(GTK_BOX(v), d->chk);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END); gtk_widget_set_margin_top(btns, 6);
    d->open_btn = gtk_button_new_with_label("Open"); d->folder_btn = gtk_button_new_with_label("Open Folder"); d->cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_sensitive(d->open_btn, FALSE); gtk_widget_set_sensitive(d->folder_btn, FALSE);
    g_signal_connect(d->open_btn, "clicked", G_CALLBACK(on_dl_open), d);
    g_signal_connect(d->folder_btn, "clicked", G_CALLBACK(on_dl_folder), d);
    g_signal_connect(d->cancel_btn, "clicked", G_CALLBACK(on_dl_cancel), d);
    gtk_box_append(GTK_BOX(btns), d->open_btn); gtk_box_append(GTK_BOX(btns), d->folder_btn); gtk_box_append(GTK_BOX(btns), d->cancel_btn);
    gtk_box_append(GTK_BOX(v), btns);
    gtk_window_set_child(GTK_WINDOW(d->win), v);
    gtk_window_present(GTK_WINDOW(d->win));
    d->t0 = g_get_monotonic_time();
    d->tick = g_timeout_add(250, dl_tick, d);
}

/* ---- the download itself ---- */
static void dl_start(dl_t *d, const char *path) {
    g_free(d->dest); d->dest = g_strdup(path);
    char *dir = g_path_get_dirname(path); g_mkdir_with_parents(dir, 0755); g_free(dir);
    webkit_download_set_destination(d->dl, path);
    dl_progress_window(d);
    char *s = g_strdup_printf("Downloading %s...", d->name); set_status(s); g_free(s);
}
static char *unique_path(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, NULL);
    for (int i = 2; g_file_test(path, G_FILE_TEST_EXISTS) && i < 100; i++) {   /* keep earlier files */
        g_free(path); char *b2 = g_strdup_printf("(%d) %s", i, name); path = g_build_filename(dir, b2, NULL); g_free(b2);
    }
    return path;
}
static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer p) {
    dl_t *d = p;
    GFile *f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) { webkit_download_cancel(d->dl); return; }       /* Save As was cancelled */
    char *path = g_file_get_path(f);
    dl_start(d, path);
    g_free(path); g_object_unref(f);
}
static void on_warn_choice(GtkButton *b, gpointer p) {
    dl_t *d = p;
    int choice = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "choice"));
    GtkWidget *w = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_WINDOW);
    g_object_set_data(G_OBJECT(w), "answered", GINT_TO_POINTER(1));
    gtk_window_destroy(GTK_WINDOW(w));
    if (choice == 0) { webkit_download_cancel(d->dl); return; }
    d->mode = choice;
    if (choice == 1) {                                        /* Run: a temporary copy THE WRLD OS opens when done */
        char *path = unique_path("/mnt/c/WRLD/Temp/Downloads", d->name);
        dl_start(d, path); g_free(path);
        return;
    }
    GtkFileDialog *fd = gtk_file_dialog_new();                /* Save: where? */
    gtk_file_dialog_set_title(fd, "Save As");
    gtk_file_dialog_set_initial_name(fd, d->name);
    g_mkdir_with_parents(downloads_dir, 0755);
    GFile *dir = g_file_new_for_path(downloads_dir);
    gtk_file_dialog_set_initial_folder(fd, dir); g_object_unref(dir);
    gtk_file_dialog_save(fd, GTK_WINDOW(win), NULL, on_save_chosen, d);
    g_object_unref(fd);
}
static gboolean on_warn_close(GtkWindow *w, gpointer p) {
    dl_t *d = p;
    if (!g_object_get_data(G_OBJECT(w), "answered")) webkit_download_cancel(d->dl);   /* the close box: Cancel */
    return FALSE;
}
static void on_risk(GtkButton *b, gpointer p) {
    (void)b; GtkWidget *more = p;
    gtk_widget_set_visible(more, !gtk_widget_get_visible(more));
}
static GtkWidget *warn_button(const char *text, int choice, dl_t *d) {
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_set_size_request(b, 84, -1);
    g_object_set_data(G_OBJECT(b), "choice", GINT_TO_POINTER(choice));
    g_signal_connect(b, "clicked", G_CALLBACK(on_warn_choice), d);
    return b;
}
static void dl_security_warning(dl_t *d) {
    GtkWidget *w = dl_window("File Download - Security Warning", 460, 250, TRUE);
    g_signal_connect(w, "close-request", G_CALLBACK(on_warn_close), d);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(v, "dl-body");
    gtk_box_append(GTK_BOX(v), label("Do you want to run or save this file?", TRUE, FALSE));
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    const char *dot = strrchr(d->name, '.');
    gtk_box_append(GTK_BOX(h), picture(paint_file, 32, dot ? dot + 1 : NULL));
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 4); gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    char *sz = d->size ? fmt_size(d->size) : NULL;
    char *type = sz ? g_strdup_printf("%s, %s", d->type_desc, sz) : g_strdup(d->type_desc);
    const char *keys[] = { "Name:", "Type:", "From:" }, *vals[] = { d->name, type, d->host };
    for (int i = 0; i < 3; i++) {
        GtkWidget *k = label(keys[i], FALSE, FALSE); gtk_widget_set_halign(k, GTK_ALIGN_END);
        GtkWidget *val = label(vals[i], i != 1, FALSE); gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_MIDDLE);
        if (i == 0) gtk_widget_add_css_class(val, "dl-link");
        gtk_grid_attach(GTK_GRID(grid), k, 0, i, 1, 1); gtk_grid_attach(GTK_GRID(grid), val, 1, i, 1, 1);
    }
    g_free(sz); g_free(type);
    gtk_box_append(GTK_BOX(h), grid);
    gtk_box_append(GTK_BOX(v), h);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    GtkWidget *run = warn_button("Run", 1, d);
    gtk_box_append(GTK_BOX(btns), run); gtk_box_append(GTK_BOX(btns), warn_button("Save", 2, d)); gtk_box_append(GTK_BOX(btns), warn_button("Cancel", 0, d));
    gtk_box_append(GTK_BOX(v), btns);
    /* the warning pane */
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(pane, "dl-warn");
    gtk_box_append(GTK_BOX(pane), picture(paint_shield, 32, NULL));
    GtkWidget *pv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(pv), label("While files from the Internet can be useful, this file type can potentially harm your computer. "
                                      "If you do not trust the source, do not run or save this software.", FALSE, TRUE));
    GtkWidget *more = label("Programs and scripts run with your permissions and can change or delete your files. Only run a file "
                            "from a site you know, and save it first if you want to check it before you open it.", FALSE, TRUE);
    gtk_widget_set_visible(more, FALSE);
    GtkWidget *risk = gtk_button_new_with_label("What's the risk?");
    gtk_widget_add_css_class(risk, "dl-linkbtn"); gtk_widget_set_halign(risk, GTK_ALIGN_START);
    g_signal_connect(risk, "clicked", G_CALLBACK(on_risk), more);
    gtk_box_append(GTK_BOX(pv), risk); gtk_box_append(GTK_BOX(pv), more);
    gtk_box_append(GTK_BOX(pane), pv);
    gtk_box_append(GTK_BOX(v), pane);
    gtk_window_set_child(GTK_WINDOW(w), v);
    gtk_window_set_default_widget(GTK_WINDOW(w), run);
    gtk_window_present(GTK_WINDOW(w));
}
static gboolean on_dl_decide(WebKitDownload *dl, char *name, gpointer p) {
    dl_t *d = p; (void)dl;
    char *base = g_path_get_basename(name && *name ? name : "download");
    d->name = base;
    d->type_desc = type_of(base);
    WebKitURIResponse *r = webkit_download_get_response(d->dl);
    const char *uri = r ? webkit_uri_response_get_uri(r) : webkit_uri_request_get_uri(webkit_download_get_request(d->dl));
    GUri *gu = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    d->host = g_strdup(gu && g_uri_get_host(gu) ? g_uri_get_host(gu) : "this computer");
    if (gu) g_uri_unref(gu);
    d->size = r ? webkit_uri_response_get_content_length(r) : 0;
    dl_security_warning(d);
    return TRUE;                                              /* the destination comes when the user answers */
}
static void on_dl_finished(WebKitDownload *dl, gpointer p) {
    dl_t *d = p; (void)dl;
    if (d->failed) return;
    d->done = TRUE;
    double el = (g_get_monotonic_time() - d->t0) / 1e6;
    guint64 got = webkit_download_get_received_data_length(d->dl);
    char *s = g_strdup_printf("Saved %s", d->name); set_status(s); g_free(s);
    if (d->mode == 1 && d->dest) os_open(d->dest);           /* Run */
    if (!d->win) { dl_free(d); return; }
    if (d->mode == 1 || gtk_check_button_get_active(GTK_CHECK_BUTTON(d->chk))) { dl_close(d); return; }
    gtk_window_set_title(GTK_WINDOW(d->win), "Download complete");
    gtk_label_set_text(GTK_LABEL(d->head), "Download Complete");
    gtk_widget_add_css_class(d->head, "dl-bold");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->bar), 1.0);
    char *g = fmt_size(got), *t = fmt_time(el), *l = g_strdup_printf("%s in %s", g, t);
    gtk_label_set_text(GTK_LABEL(d->left), l);
    gtk_label_set_text(GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(gtk_widget_get_parent(d->left)), 0, 0)), "Downloaded:");
    char *r = fmt_size(el > 0 ? got / el : got), *rs = g_strdup_printf("%s/Sec", r);
    gtk_label_set_text(GTK_LABEL(d->rate), rs);
    g_free(g); g_free(t); g_free(l); g_free(r); g_free(rs);
    gtk_widget_set_sensitive(d->open_btn, TRUE); gtk_widget_set_sensitive(d->folder_btn, TRUE);
    gtk_button_set_label(GTK_BUTTON(d->cancel_btn), "Close");
    gtk_widget_queue_draw(d->anim);
}
static void on_dl_failed2(WebKitDownload *dl, GError *e, gpointer p) {
    dl_t *d = p; (void)dl;
    d->failed = TRUE;
    gboolean cancelled = e && g_error_matches(e, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER);
    char *s = cancelled ? g_strdup("Download cancelled") : g_strdup_printf("Download failed: %s", e ? e->message : "error");
    set_status(s);
    if (d->win && !cancelled) {
        gtk_window_set_title(GTK_WINDOW(d->win), "File Download");
        gtk_label_set_text(GTK_LABEL(d->head), "The download could not be completed:");
        gtk_label_set_text(GTK_LABEL(d->info), e ? e->message : "error");
        gtk_button_set_label(GTK_BUTTON(d->cancel_btn), "Close");
    } else if (d->win) dl_close(d);
    else dl_free(d);
    g_free(s);
}
static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer u) {
    (void)s; (void)u;
    dl_t *d = g_new0(dl_t, 1);
    d->dl = g_object_ref(dl);
    g_signal_connect(dl, "decide-destination", G_CALLBACK(on_dl_decide), d);
    g_signal_connect(dl, "finished", G_CALLBACK(on_dl_finished), d);
    g_signal_connect(dl, "failed", G_CALLBACK(on_dl_failed2), d);
}
static const char dl_css[] =
    ".hang-bar { background: #FFFFE1; border-bottom: 1px solid #ACA899; padding: 4px 8px; font-family: 'Liberation Sans'; font-size: 9pt; }"
    ".dl-dialog { background: #ECE9D8; }"
    ".dl-body { padding: 12px 14px 10px 14px; font-family: 'Liberation Sans'; font-size: 9pt; color: #000; }"
    ".dl-bold { font-weight: bold; }"
    ".dl-link { color: #0B3CA0; }"
    ".dl-warn { border-top: 1px solid #ACA899; padding-top: 10px; margin-top: 4px; }"
    "button.dl-linkbtn { background: none; border: none; box-shadow: none; padding: 0; min-height: 0; color: #0B3CA0; }"
    "button.dl-linkbtn label { text-decoration: underline; }"
    "progressbar.dl-bar trough { min-height: 14px; background: #fff; border: 1px solid #7A7A7A; border-radius: 2px; }"
    "progressbar.dl-bar progress { min-height: 12px; background: #2ECC40; border-radius: 1px; }";
static void dl_init(void) {
    GtkCssProvider *cp = gtk_css_provider_new();
    gtk_css_provider_load_from_string(cp, dl_css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(cp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(cp);
}
