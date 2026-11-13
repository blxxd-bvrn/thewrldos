/* The platform layer under WPE WebKit, tested on THE WRLD OS's Linux layer:
 * GLib's main loop, threads and processes, GIO files, Fontconfig + FreeType
 * fonts and Cairo drawing. The picture is written to C:\cairo-test.png. */
#include <glib.h>
#include <gio/gio.h>
#include <cairo.h>
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static int passed, failed;
static void check(const char *what, gboolean ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); if (ok) passed++; else failed++; }

typedef struct { GMainLoop *loop; int ticks, idles; GAsyncQueue *q; } ctx_t;
static gboolean on_tick(gpointer p) { ctx_t *c = p; c->ticks++; return c->ticks < 5; }
static gboolean on_idle(gpointer p) { ctx_t *c = p; c->idles++; return c->idles < 3; }
static gboolean on_done(gpointer p) { g_main_loop_quit(((ctx_t *)p)->loop); return FALSE; }
static gpointer worker(gpointer p) { ctx_t *c = p; for (int i = 1; i <= 3; i++) { g_usleep(10000); g_async_queue_push(c->q, GINT_TO_POINTER(i)); } return NULL; }

int main(void) {
    printf("THE WRLD OS - platform test (GLib %d.%d, Cairo %s)\n", glib_major_version, glib_minor_version, cairo_version_string());
    /* GLib main loop: timeouts, idles, a worker thread and an async queue */
    ctx_t c = { g_main_loop_new(NULL, FALSE), 0, 0, g_async_queue_new() };
    g_timeout_add(20, on_tick, &c);
    g_idle_add(on_idle, &c);
    GThread *t = g_thread_new("worker", worker, &c);
    g_timeout_add(300, on_done, &c);
    g_main_loop_run(c.loop);
    int sum = 0; for (int i = 0; i < 3; i++) sum += GPOINTER_TO_INT(g_async_queue_timeout_pop(c.q, 1000000));
    g_thread_join(t);
    check("GLib main loop: 5 timeouts and 3 idles", c.ticks == 5 && c.idles == 3);
    check("GThread and GAsyncQueue", sum == 6);
    /* a child process, the way WebKit starts its web and network processes */
    gchar *out = NULL; gint status = -1; GError *err = NULL;
    gchar *argv[] = { "/usr/local/bin/hello", NULL };
    gboolean ok = g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL, &out, NULL, &status, &err);
    /* hello exits with 7 on purpose, so the exit status is checked too */
    check("g_spawn_sync runs a child, reads its output and exit status", ok && out && strlen(out) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7);
    if (!ok || !out || !*out) printf("      spawn: ok=%d status=%d out=%d bytes error=%s\n", ok, status, out ? (int)strlen(out) : -1, err ? err->message : "none");
    g_free(out); g_clear_error(&err);
    /* GIO files */
    GFile *f = g_file_new_for_path("/tmp/gio-test.txt");
    ok = g_file_replace_contents(f, "hello from GIO", 14, NULL, FALSE, 0, NULL, NULL, &err);
    gchar *data = NULL; gsize len = 0;
    ok = ok && g_file_load_contents(f, NULL, &data, &len, NULL, &err);
    check("GIO file write and read", ok && len == 14 && !memcmp(data, "hello from GIO", 14));
    g_free(data); g_file_delete(f, NULL, NULL); g_object_unref(f); g_clear_error(&err);
    /* Fontconfig: find THE WRLD OS's fonts */
    FcInit();
    FcPattern *pat = FcNameParse((const FcChar8 *)"Liberation Sans:bold");
    FcConfigSubstitute(NULL, pat, FcMatchPattern); FcDefaultSubstitute(pat);
    FcResult res; FcPattern *m = FcFontMatch(NULL, pat, &res);
    FcChar8 *file = NULL;
    if (m) FcPatternGetString(m, FC_FILE, 0, &file);
    check("Fontconfig matches Liberation Sans Bold", file && strstr((const char *)file, "LiberationSans-Bold"));
    printf("      font: %s\n", file ? (const char *)file : "(none)");
    /* Cairo: gradients, paths and FreeType text into a PNG */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 480, 200);
    cairo_t *cr = cairo_create(s);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 480, 200);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.05, 0.24, 0.55); cairo_pattern_add_color_stop_rgb(g, 1, 0.17, 0.48, 0.90);
    cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);
    cairo_arc(cr, 90, 100, 60, 0, 6.2832); cairo_set_source_rgb(cr, 0.18, 0.62, 0.27); cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9); cairo_set_line_width(cr, 6);
    cairo_move_to(cr, 60, 100); cairo_line_to(cr, 82, 124); cairo_line_to(cr, 122, 76); cairo_stroke(cr);
    if (m) {
        cairo_font_face_t *ff = cairo_ft_font_face_create_for_pattern(m);
        cairo_set_font_face(cr, ff); cairo_set_font_size(cr, 30);
        cairo_set_source_rgb(cr, 1, 1, 1); cairo_move_to(cr, 175, 92); cairo_show_text(cr, "Cairo on");
        cairo_move_to(cr, 175, 132); cairo_show_text(cr, "THE WRLD OS");
        cairo_font_face_destroy(ff);
    }
    cairo_status_t st = cairo_surface_write_to_png(s, "/mnt/c/cairo-test.png");
    check("Cairo draws text and writes C:\\cairo-test.png", st == CAIRO_STATUS_SUCCESS && cairo_status(cr) == CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr); cairo_surface_destroy(s);
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
