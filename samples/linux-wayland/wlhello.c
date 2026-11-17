/* wlhello: the smallest useful Wayland client, to check wrldcomp. It opens an
 * xdg_toplevel, draws into a wl_shm buffer (a memfd), follows resizes, and
 * shows the pointer and the keys it receives: click to drop squares, type to
 * see the key codes, Esc or the close button to quit.
 *   wrldcomp wlhello */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp; static struct wl_shm *shm; static struct xdg_wm_base *wm; static struct wl_seat *seat;
static struct wl_surface *surf; static struct xdg_surface *xs; static struct xdg_toplevel *top;
static int W = 420, H = 300, running = 1, configured, px = -1, py = -1, keys, last_key, nsq;
static int sq[64][2];
static uint32_t frame_n;

static void draw(void) {
    int stride = W * 4, size = stride * H;
    int fd = memfd_create("wlhello", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) { perror("memfd"); exit(1); }
    uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        p[y * W + x] = 0xFF000000u | ((uint32_t)(40 + x * 120 / W) << 16) | ((uint32_t)(60 + y * 100 / H) << 8) | (uint32_t)(140 + (frame_n & 63));
    for (int i = 0; i < nsq; i++)                        /* clicked squares */
        for (int y = sq[i][1] - 8; y < sq[i][1] + 8; y++) for (int x = sq[i][0] - 8; x < sq[i][0] + 8; x++)
            if (x >= 0 && y >= 0 && x < W && y < H) p[y * W + x] = 0xFFFFD040u;
    if (px >= 0) for (int k = -10; k <= 10; k++) {       /* the pointer */
        if (px + k >= 0 && px + k < W && py < H) p[py * W + px + k] = 0xFFFFFFFFu;
        if (py + k >= 0 && py + k < H && px < W) p[(py + k) * W + px] = 0xFFFFFFFFu;
    }
    for (int i = 0; i < keys && i < 40; i++)              /* one bar per key pressed */
        for (int y = H - 20; y < H - 6; y++) for (int x = 10 + i * 10; x < 17 + i * 10; x++) if (x < W) p[y * W + x] = 0xFF80FF80u;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool); munmap(p, size); close(fd);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    frame_n++;
}
/* pointer and keyboard */
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *f, wl_fixed_t x, wl_fixed_t y) { px = wl_fixed_to_int(x); py = wl_fixed_to_int(y); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *f) { px = py = -1; draw(); }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { px = wl_fixed_to_int(x); py = wl_fixed_to_int(y); draw(); }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    printf("wlhello: button %#x %s at %d,%d\n", b, st ? "down" : "up", px, py); fflush(stdout);
    if (st && nsq < 64) { sq[nsq][0] = px; sq[nsq][1] = py; nsq++; draw(); }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { printf("wlhello: scroll %d\n", wl_fixed_to_int(v)); fflush(stdout); }
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_src(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_disc(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_v120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void p_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener pl = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame, p_src, p_stop, p_disc, p_v120, p_rel };
static void k_map(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) { printf("wlhello: keymap format %u, %u bytes\n", f, sz); fflush(stdout); close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *f, struct wl_array *ks) { printf("wlhello: keyboard focus\n"); fflush(stdout); }
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *f) {}
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) {
    printf("wlhello: key %u %s\n", key, st ? "down" : "up"); fflush(stdout);
    if (st) { keys++; last_key = key; if (key == 1) running = 0; draw(); }
}
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t g) { printf("wlhello: modifiers %#x\n", dep); fflush(stdout); }
static void k_rep(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {}
static const struct wl_keyboard_listener kl = { k_map, k_enter, k_leave, k_key, k_mods, k_rep };
static void seat_caps(void *d, struct wl_seat *s, uint32_t c) {
    if (c & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(s), &pl, NULL);
    if (c & WL_SEAT_CAPABILITY_KEYBOARD) wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kl, NULL);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener sl = { seat_caps, seat_name };
/* shell */
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wml = { wm_ping };
static void xs_conf(void *d, struct xdg_surface *x, uint32_t s) { xdg_surface_ack_configure(x, s); configured = 1; draw(); }
static const struct xdg_surface_listener xsl = { xs_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    if (w > 0 && h > 0 && (w != W || h != H)) { W = w; H = h; printf("wlhello: resized to %dx%d\n", W, H); fflush(stdout); }
}
static void top_close(void *d, struct xdg_toplevel *t) { running = 0; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener tl = { top_conf, top_close, top_bounds, top_caps };
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wml, NULL); }
    else if (!strcmp(iface, "wl_seat")) { seat = wl_registry_bind(r, name, &wl_seat_interface, v < 5 ? v : 5); wl_seat_add_listener(seat, &sl, NULL); }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener rl = { reg_global, reg_remove };

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wlhello: no Wayland display (run it through wrldcomp)\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &rl, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm) { fprintf(stderr, "wlhello: the compositor lacks wl_compositor, wl_shm or xdg_wm_base\n"); return 1; }
    printf("wlhello: connected\n"); fflush(stdout);
    surf = wl_compositor_create_surface(comp);
    xs = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xs, &xsl, NULL);
    top = xdg_surface_get_toplevel(xs); xdg_toplevel_add_listener(top, &tl, NULL);
    xdg_toplevel_set_title(top, "Hello from Wayland");
    wl_surface_commit(surf);
    while (running && wl_display_dispatch(dpy) != -1) {}
    printf("wlhello: done after %u frames, %d keys\n", frame_n, keys);
    wl_display_disconnect(dpy);
    return 0;
}
