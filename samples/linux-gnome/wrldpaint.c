/* A Linux program in a desktop window: Cairo draws into /dev/wrld/window's
 * shared pixels, and mouse and keyboard events come back through poll().
 * Click to drop dots, type to write, Esc or the close button to quit. */
#include <cairo.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "wrldwin.h"

typedef struct { double x, y, hue; } dot_t;

static void hsv(double h, double *r, double *g, double *b) {
    double s = 0.7, v = 0.95, c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; } else if (h < 120) { rr = x; gg = c; } else if (h < 180) { gg = c; bb = x; }
    else if (h < 240) { gg = x; bb = c; } else if (h < 300) { rr = x; bb = c; } else { rr = c; bb = x; }
    *r = rr + m; *g = gg + m; *b = bb + m;
}

int main(void) {
    int fd = open("/dev/wrld/window", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/wrld/window"); return 1; }
    int W = 560, H = 380;
    struct wrldwin_create c = { W, H, WRLDWIN_RESIZABLE, "Cairo on THE WRLD OS" };
    if (ioctl(fd, WRLDWIN_CREATE, &c)) { perror("WRLDWIN_CREATE"); return 1; }
    unsigned char *px = mmap(NULL, (size_t)W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { perror("mmap"); return 1; }
    dot_t dots[256]; int ndots = 0;
    char text[64] = ""; int mx = -1, my = -1, frame = 0, running = 1, events = 0;
    printf("wrldpaint: window open (%dx%d); click, type, Esc to quit\n", W, H); fflush(stdout);
    while (running) {
        cairo_surface_t *s = cairo_image_surface_create_for_data(px, CAIRO_FORMAT_ARGB32, W, H, W * 4);
        cairo_t *cr = cairo_create(s);
        cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, W, H);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.06, 0.10, 0.22); cairo_pattern_add_color_stop_rgb(g, 1, 0.10, 0.28, 0.55);
        cairo_set_source(cr, g); cairo_paint(cr); cairo_pattern_destroy(g);
        double a = frame * 0.08;                                  /* a spinning arc */
        cairo_set_line_width(cr, 8); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 0.35, 0.85, 1, 0.9);
        cairo_arc(cr, W - 70, 70, 36, a, a + 4.2); cairo_stroke(cr);
        cairo_select_font_face(cr, "Liberation Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 26); cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, 24, 52); cairo_show_text(cr, "A Linux program in a window");
        cairo_select_font_face(cr, "Liberation Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 15); cairo_set_source_rgb(cr, 0.8, 0.88, 1);
        cairo_move_to(cr, 24, 80); cairo_show_text(cr, "Cairo draws into /dev/wrld/window's shared pixels.");
        cairo_move_to(cr, 24, 100); cairo_show_text(cr, "Click to drop dots, type to write, Esc to quit.");
        for (int i = 0; i < ndots; i++) {
            double r, gg, b; hsv(dots[i].hue, &r, &gg, &b);
            cairo_set_source_rgb(cr, r, gg, b); cairo_arc(cr, dots[i].x, dots[i].y, 12, 0, 2 * M_PI); cairo_fill(cr);
        }
        if (mx >= 0) {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.6); cairo_set_line_width(cr, 1);
            cairo_move_to(cr, mx - 10, my + 0.5); cairo_line_to(cr, mx + 10, my + 0.5);
            cairo_move_to(cr, mx + 0.5, my - 10); cairo_line_to(cr, mx + 0.5, my + 10); cairo_stroke(cr);
        }
        cairo_set_source_rgba(cr, 0, 0, 0, 0.35); cairo_rectangle(cr, 16, H - 70, W - 32, 50); cairo_fill(cr);
        cairo_set_font_size(cr, 22); cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_move_to(cr, 30, H - 37); cairo_show_text(cr, text[0] ? text : "(type here)");
        char st[96]; snprintf(st, sizeof st, "%dx%d   frame %d   %d events   %d dots", W, H, frame, events, ndots);
        cairo_set_font_size(cr, 12); cairo_set_source_rgb(cr, 0.7, 0.8, 0.95); cairo_move_to(cr, 24, H - 6); cairo_show_text(cr, st);
        cairo_destroy(cr); cairo_surface_flush(s); cairo_surface_destroy(s);
        ioctl(fd, WRLDWIN_PRESENT, 0);
        frame++;

        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 40) <= 0) continue;
        struct wrldwin_event ev[32];
        ssize_t n = read(fd, ev, sizeof ev);
        if (n <= 0) break;                                        /* the window is gone */
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
            events++;
            switch (ev[i].type) {
            case WRLDWIN_EV_MOTION: mx = ev[i].x; my = ev[i].y; break;
            case WRLDWIN_EV_LEAVE: mx = my = -1; break;
            case WRLDWIN_EV_BUTTON_DOWN:
                if (ndots < 256) { dots[ndots].x = ev[i].x; dots[ndots].y = ev[i].y; dots[ndots].hue = fmod(ndots * 47.0, 360); ndots++; }
                break;
            case WRLDWIN_EV_KEY_DOWN: {
                size_t l = strlen(text);
                if (ev[i].code == WRLDWIN_KEY_ESC) running = 0;
                else if (ev[i].code == WRLDWIN_KEY_BACKSPACE) { if (l) text[l - 1] = 0; }
                else if (ev[i].ch >= 32 && ev[i].ch < 127 && l < sizeof text - 1) { text[l] = (char)ev[i].ch; text[l + 1] = 0; }
                break;
            }
            case WRLDWIN_EV_RESIZE: {
                struct wrldwin_size sz = { ev[i].x, ev[i].y };
                if (ioctl(fd, WRLDWIN_RESIZE, &sz) == 0) {
                    munmap(px, (size_t)W * H * 4);
                    W = sz.width; H = sz.height;
                    px = mmap(NULL, (size_t)W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (px == MAP_FAILED) return 1;
                }
                break;
            }
            case WRLDWIN_EV_CLOSE: running = 0; break;
            }
        }
    }
    printf("wrldpaint: %d frames, %d events, %d dots, text \"%s\"\n", frame, events, ndots, text);
    close(fd);
    return 0;
}
