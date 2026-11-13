/* /dev/wrld/window: a desktop window for a Linux program.
 *
 * The program opens the device, creates a window with an ioctl, mmaps the
 * window's pixels (a shared object, 32-bit 0xAARRGGBB, what Cairo's ARGB32 and
 * pixman use) and draws into them, then presents a frame with another ioctl.
 * Mouse, keyboard, resize and close events are read from the same descriptor,
 * which works with poll and epoll. This is the display path a browser engine's
 * backend (or a small Wayland compositor) uses on THE WRLD OS.
 *
 * Shared with programs through samples/linux-gnome/wrldwin.h. */
#ifdef __x86_64__
#include "kernel.h"
#include "gfx.h"
#include "wm.h"
#include "lx.h"

#define WIN_CREATE  0x57A1      /* struct { int32 width, height; uint32 flags; char title[100]; } */
#define WIN_PRESENT 0x57A2      /* the buffer holds a new frame */
#define WIN_RESIZE  0x57A3      /* struct { int32 width, height; }: resize the buffer (then mmap again) */
#define WIN_TITLE   0x57A4      /* char title[100] */
#define WIN_INFO    0x57A5      /* struct { int32 width, height, buf_width, buf_height; } */
#define WINF_RESIZABLE 1

enum { WEV_MOTION = 1, WEV_BUTTON_DOWN, WEV_BUTTON_UP, WEV_WHEEL, WEV_KEY_DOWN, WEV_KEY_UP, WEV_RESIZE, WEV_CLOSE, WEV_LEAVE };
typedef struct { uint32_t type; int32_t x, y; uint32_t code; uint32_t ch; uint32_t mods; } wev_t;
#define WQ 256

typedef struct lxwin {
    window_t *w;
    shmobj_t *fb;
    int bw, bh;                                 /* buffer size */
    wev_t q[WQ]; int head, tail;
    bool closed;                                /* the user closed the window */
    uint32_t frames;
} lxwin_t;

static void push(lxwin_t *lw, uint32_t type, int x, int y, uint32_t code, uint32_t ch, uint32_t mods) {
    int nt = (lw->tail + 1) % WQ;
    if (nt == lw->head) {                                    /* full: drop the oldest motion */
        if (type == WEV_MOTION) return;
        lw->head = (lw->head + 1) % WQ;
    }
    if (type == WEV_MOTION && lw->head != lw->tail) {        /* coalesce consecutive moves */
        wev_t *last = &lw->q[(lw->tail + WQ - 1) % WQ];
        if (last->type == WEV_MOTION) { last->x = x; last->y = y; return; }
    }
    wev_t *e = &lw->q[lw->tail];
    e->type = type; e->x = x; e->y = y; e->code = code; e->ch = ch; e->mods = mods;
    lw->tail = nt;
}

/* ---- the desktop side ---- */
static void win_paint(window_t *w) {
    lxwin_t *lw = w->data;
    canvas_t *c = w->surf;
    if (!lw || !lw->fb || !lw->bw) { gfx_clear(c, HEX(0x202020)); return; }
    int cw = MIN(c->w, lw->bw), ch = MIN(c->h, lw->bh);
    uint64_t stride = (uint64_t)lw->bw * 4;
    for (int y = 0; y < ch; y++) {
        uint32_t *dst = c->px + (size_t)y * c->stride;
        uint64_t off = y * stride;
        int x = 0;
        while (x < cw) {
            uint64_t at = off + (uint64_t)x * 4;
            uint8_t *pg = shm_page(lw->fb, at / PAGE, false);
            int run = MIN(cw - x, (int)((PAGE - (at & (PAGE - 1))) / 4));
            if (pg) { const uint32_t *src = (const uint32_t *)(pg + (at & (PAGE - 1))); for (int i = 0; i < run; i++) dst[x + i] = src[i] | 0xFF000000u; }
            else for (int i = 0; i < run; i++) dst[x + i] = 0xFF000000u;
            x += run;
        }
        for (int i = cw; i < c->w; i++) dst[i] = HEX(0x202020);
    }
    for (int y = ch; y < c->h; y++) for (int i = 0; i < c->w; i++) c->px[(size_t)y * c->stride + i] = HEX(0x202020);
}
static void win_mouse(window_t *w, int type, int x, int y, int btn) {
    lxwin_t *lw = w->data; if (!lw) return;
    switch (type) {
    case ME_MOVE: push(lw, WEV_MOTION, x, y, 0, 0, 0); break;
    case ME_DOWN: case ME_DBLCLICK: push(lw, WEV_BUTTON_DOWN, x, y, btn == MB_LEFT ? 1 : btn == MB_RIGHT ? 3 : btn == MB_MIDDLE ? 2 : btn, 0, 0); break;
    case ME_UP: push(lw, WEV_BUTTON_UP, x, y, btn == MB_LEFT ? 1 : btn == MB_RIGHT ? 3 : btn == MB_MIDDLE ? 2 : btn, 0, 0); break;
    case ME_WHEEL: push(lw, WEV_WHEEL, x, y, (uint32_t)btn, 0, 0); break;
    case ME_LEAVE: push(lw, WEV_LEAVE, 0, 0, 0, 0, 0); break;
    }
}
static void win_key(window_t *w, key_event_t *e) {
    lxwin_t *lw = w->data; if (!lw) return;
    push(lw, e->pressed ? WEV_KEY_DOWN : WEV_KEY_UP, 0, 0, (uint32_t)e->key, (uint8_t)e->ch, (uint32_t)e->mods);
}
static void win_resized(window_t *w) {
    lxwin_t *lw = w->data; if (!lw) return;
    push(lw, WEV_RESIZE, w->surf->w, w->surf->h, 0, 0, 0);
}
static bool win_close(window_t *w) {
    lxwin_t *lw = w->data;
    if (!lw || lw->closed) return true;                     /* asked twice: close it anyway */
    lw->closed = true;
    push(lw, WEV_CLOSE, 0, 0, 0, 0, 0);
    return false;                                            /* the program decides; it usually exits */
}
static void win_destroy(window_t *w) { lxwin_t *lw = w->data; if (lw) lw->w = NULL; w->data = NULL; }

/* ---- the program side ---- */
void *lxwin_new(void) { return kzalloc(sizeof(lxwin_t)); }
void lxwin_release(void *o) {
    lxwin_t *lw = o;
    if (!lw) return;
    if (lw->w && wm_alive(lw->w)) { lw->w->data = NULL; wm_destroy(lw->w); }
    shm_unref(lw->fb);
    kfree(lw);
}
static int set_buffer(lxwin_t *lw, int w, int h) {
    if (w < 1 || h < 1 || w > 4096 || h > 4096) return -EINVAL;
    if (!lw->fb) { lw->fb = shm_new("wrld-window"); if (!lw->fb) return -ENOMEM; }
    int r = shm_resize(lw->fb, (uint64_t)w * h * 4);
    if (r) return r;
    lw->bw = w; lw->bh = h;
    return 0;
}
int64_t lxwin_ioctl(void *o, uint64_t cmd, uint64_t arg, uvm_t *vm) {
    lxwin_t *lw = o;
    switch (cmd) {
    case WIN_CREATE: {
        struct { int32_t w, h; uint32_t flags; char title[100]; } a;
        if (!uvm_check(vm, arg, sizeof a, false)) return -EFAULT;
        memcpy(&a, (void *)(uintptr_t)arg, sizeof a); a.title[99] = 0;
        if (lw->w) return -EEXIST;
        int r = set_buffer(lw, a.w, a.h); if (r) return r;
        window_t *w = wm_create(a.title[0] ? a.title : "Linux program", -1, -1, a.w, a.h, (a.flags & WINF_RESIZABLE) ? WF_RESIZABLE : 0);
        if (!w) return -ENOMEM;
        w->data = lw; w->paint = win_paint; w->mouse = win_mouse; w->key = win_key;
        w->resized = win_resized; w->close = win_close; w->destroy = win_destroy;
        w->icon = ICON_EXE; w->min_w = 120; w->min_h = 80;
        lw->w = w;
        wm_center(w);
        w->dirty = true;
        return 0;
    }
    case WIN_PRESENT: if (lw->w) { lw->w->dirty = true; lw->frames++; } return lw->w ? 0 : -ENODEV;
    case WIN_RESIZE: {
        int32_t s[2];
        if (!uvm_check(vm, arg, 8, false)) return -EFAULT;
        memcpy(s, (void *)(uintptr_t)arg, 8);
        int r = set_buffer(lw, s[0], s[1]);
        if (!r && lw->w) lw->w->dirty = true;
        return r;
    }
    case WIN_TITLE: {
        char t[100];
        if (!uvm_check(vm, arg, sizeof t, false)) return -EFAULT;
        memcpy(t, (void *)(uintptr_t)arg, sizeof t); t[99] = 0;
        if (lw->w) wm_set_title(lw->w, t);
        return lw->w ? 0 : -ENODEV;
    }
    case WIN_INFO: {
        int32_t s[4] = { lw->w ? lw->w->surf->w : 0, lw->w ? lw->w->surf->h : 0, lw->bw, lw->bh };
        if (!uvm_check(vm, arg, sizeof s, true)) return -EFAULT;
        memcpy((void *)(uintptr_t)arg, s, sizeof s);
        return 0;
    }
    }
    return -ENOTTY;
}
void *lxwin_buffer(void *o) { return ((lxwin_t *)o)->fb; }
bool lxwin_ready(void *o) { lxwin_t *lw = o; return lw->head != lw->tail || (!lw->w && lw->bw); }
int64_t lxwin_read(void *o, void *dst, uint64_t n, bool nonblock) {
    lxwin_t *lw = o;
    if (n < sizeof(wev_t)) return -EINVAL;
    while (lw->head == lw->tail) {
        if (lw->bw && !lw->w) return 0;                      /* the window is gone: end of file */
        if (nonblock) return -EAGAIN;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
    uint64_t k = 0;
    while (lw->head != lw->tail && k + sizeof(wev_t) <= n) {
        memcpy((uint8_t *)dst + k, &lw->q[lw->head], sizeof(wev_t));
        lw->head = (lw->head + 1) % WQ;
        k += sizeof(wev_t);
    }
    return (int64_t)k;
}
#endif
