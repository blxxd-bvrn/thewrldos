/* Vendor-neutral framebuffer conversion and damage presentation. The firmware
 * already owns scanout; this does not pretend to initialize an Intel/AMD/NVIDIA
 * GPU engine. RAM history avoids expensive device reads and unchanged writes. */
#include "basicfb.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

static uint32_t mask(unsigned pos, unsigned bits) { return ((1u << bits) - 1u) << pos; }
static uint32_t channel(uint32_t v, unsigned bits) { return (v * ((1u << bits) - 1) + 127) / 255; }
bool basicfb_mode_valid(const basicfb_mode_t *m, const char **reason) {
    const char *why = NULL;
    if (!m || !m->width || !m->height || m->width > 8192 || m->height > 8192) why = "invalid framebuffer dimensions";
    else if (m->bpp != 15 && m->bpp != 16 && m->bpp != 24 && m->bpp != 32) why = "unsupported framebuffer pixel depth";
    else if ((uint64_t)m->width * ((m->bpp + 7) / 8) > m->pitch ||
             (uint64_t)m->pitch * m->height > BASICFB_MAX_BYTES) why = "invalid framebuffer pitch or size";
    else if (!m->red_bits || !m->green_bits || !m->blue_bits || m->red_bits > 16 || m->green_bits > 16 || m->blue_bits > 16 ||
             m->red_pos + m->red_bits > m->bpp || m->green_pos + m->green_bits > m->bpp || m->blue_pos + m->blue_bits > m->bpp) why = "invalid firmware RGB masks";
    else {
        uint32_t r = mask(m->red_pos, m->red_bits), g = mask(m->green_pos, m->green_bits), b = mask(m->blue_pos, m->blue_bits);
        if ((r & g) || (r & b) || (g & b)) why = "overlapping firmware RGB masks";
    }
    if (reason) *reason = why;
    return !why;
}
bool basicfb_bind(basicfb_t *f, void *pixels, const basicfb_mode_t *m, uint32_t *shadow,
                  size_t shadow_bytes, bool wc, basicfb_damage_fn damage, void *context) {
    if (!f || !pixels || !basicfb_mode_valid(m, NULL)) return false;
    memset(f, 0, sizeof *f); f->mode = *m; f->pixels = pixels; f->ready = true;
    if (shadow && shadow_bytes >= (size_t)m->width * m->height * 4) f->shadow = shadow;
#ifdef __x86_64__
    f->streaming = wc;
#else
    (void)wc;                            /* x86 remains x87/no-SSE compatible */
#endif
    f->damage = damage; f->context = context;
    for (unsigned v = 0; v < 256; v++) {
        f->rgb_lut[0][v] = channel(v, m->red_bits) << m->red_pos;
        f->rgb_lut[1][v] = channel(v, m->green_bits) << m->green_pos;
        f->rgb_lut[2][v] = channel(v, m->blue_bits) << m->blue_pos;
    }
    return true;
}
void basicfb_invalidate(basicfb_t *f) { if (f) { f->invalidations++; f->have_frame = false; } }

static void copy_bytes(uint8_t *d, const uint8_t *s, size_t n, bool stream) {
#ifdef __x86_64__
    if (stream && n >= 256) {
        while (n && ((uintptr_t)d & 63)) { *d++ = *s++; n--; }
        while (n >= 64) {
            __asm__ volatile("movdqu 0(%0),%%xmm0; movdqu 16(%0),%%xmm1; movdqu 32(%0),%%xmm2; movdqu 48(%0),%%xmm3;"
                             "movntdq %%xmm0,0(%1); movntdq %%xmm1,16(%1); movntdq %%xmm2,32(%1); movntdq %%xmm3,48(%1)"
                             :: "r"(s), "r"(d) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
            s += 64; d += 64; n -= 64;
        }
    }
    size_t words = n / 8;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(words) :: "memory");
    n &= 7;
#else
    (void)stream;
    size_t words = n / 4;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) :: "memory");
    n &= 3;
#endif
    while (n--) *d++ = *s++;
}
/* Packed writes are explicitly unaligned-safe, including padded 24-bit rows. */
typedef struct __attribute__((packed)) { uint32_t value; } unaligned32_t;
static void put32(uint8_t *p, uint32_t v) { ((unaligned32_t *)p)->value = v; }
static bool native_rgb(const basicfb_mode_t *m) {
    return m->red_pos == 16 && m->red_bits == 8 && m->green_pos == 8 && m->green_bits == 8 && m->blue_pos == 0 && m->blue_bits == 8;
}
static void row(basicfb_t *f, uint8_t *d, const uint32_t *s, uint32_t n) {
    const basicfb_mode_t *m = &f->mode;
    if (m->bpp == 32 && native_rgb(m)) { copy_bytes(d, (const uint8_t *)s, (size_t)n * 4, f->streaming); return; }
    if (m->bpp == 32 && m->red_pos == 0 && m->red_bits == 8 && m->green_pos == 8 && m->green_bits == 8 && m->blue_pos == 16 && m->blue_bits == 8) {
        while (n--) { uint32_t v = *s++; put32(d, (v & 0xFF00FF00u) | ((v & 255) << 16) | ((v >> 16) & 255)); d += 4; }
        return;
    }
    if (m->bpp == 24 && native_rgb(m)) {
        while (n >= 4) {
            put32(d, (s[0] & 0xFFFFFFu) | (s[1] << 24));
            put32(d + 4, ((s[1] >> 8) & 0xFFFFu) | (s[2] << 16));
            put32(d + 8, ((s[2] >> 16) & 255u) | (s[3] << 8));
            s += 4; d += 12; n -= 4;
        }
        while (n--) { uint32_t v = *s++; *d++ = v; *d++ = v >> 8; *d++ = v >> 16; }
        return;
    }
    uint32_t bytes = (m->bpp + 7) / 8;
    while (n--) {
        uint32_t v = *s++, out = f->rgb_lut[0][(v >> 16) & 255] | f->rgb_lut[1][(v >> 8) & 255] | f->rgb_lut[2][v & 255];
        for (uint32_t i = 0; i < bytes; i++) { *d++ = out; out >>= 8; }
    }
}
static void flush(void) {
#ifdef __x86_64__
    __asm__ volatile("sfence" ::: "memory");
#else
    __sync_synchronize();
#endif
}
static void write_rect(basicfb_t *f, int x, int y, int w, int h, const uint32_t *src, uint32_t stride) {
    uint32_t bytes = (f->mode.bpp + 7) / 8;
    for (int j = 0; j < h; j++) {
        row(f, f->pixels + (size_t)(y + j) * f->mode.pitch + (size_t)x * bytes, src + (size_t)j * stride, w);
        if (f->shadow) memcpy(f->shadow + (size_t)(y + j) * f->mode.width + x, src + (size_t)j * stride, (size_t)w * 4);
    }
    uint64_t n = (uint64_t)w * h * bytes; f->bytes_written += n; f->bytes_last += n; f->regions_last++;
}
void basicfb_write(basicfb_t *f, int x, int y, int w, int h, const uint32_t *src, uint32_t stride) {
    if (!f || !f->ready || !src || w <= 0 || h <= 0 || stride < (uint32_t)w) return;
    int64_t right = (int64_t)x + w, bottom = (int64_t)y + h;
    if (right <= 0 || bottom <= 0 || x >= (int)f->mode.width || y >= (int)f->mode.height) return;
    int sx = x < 0 ? -((int64_t)x) : 0, sy = y < 0 ? -((int64_t)y) : 0;
    if (x < 0) x = 0; if (y < 0) y = 0;
    w = (int)(right > f->mode.width ? f->mode.width : right) - x;
    h = (int)(bottom > f->mode.height ? f->mode.height : bottom) - y;
    f->bytes_last = 0; f->regions_last = f->tiles_last = 0;
    write_rect(f, x, y, w, h, src + (size_t)sy * stride + sx, stride);
    flush();
    if (f->damage) f->damage(f->context, x, y, w, h);
}
static bool same_row(const uint32_t *a, const uint32_t *b, int w) {
    while (w >= 8) {
        if ((a[0]^b[0]) | (a[1]^b[1]) | (a[2]^b[2]) | (a[3]^b[3]) |
            (a[4]^b[4]) | (a[5]^b[5]) | (a[6]^b[6]) | (a[7]^b[7])) return false;
        a += 8; b += 8; w -= 8;
    }
    while (w--) if (*a++ != *b++) return false;
    return true;
}
static bool changed(basicfb_t *f, const uint32_t *src, uint32_t stride, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) if (!same_row(src + (size_t)(y + j) * stride + x,
            f->shadow + (size_t)(y + j) * f->mode.width + x, w)) return true;
    return false;
}
void basicfb_present(basicfb_t *f, const uint32_t *src, uint32_t stride) {
    if (!f || !f->ready || !src || stride < f->mode.width) return;
    int width = f->mode.width, height = f->mode.height;
    uint32_t generation = f->invalidations;
    f->bytes_last = 0; f->regions_last = f->tiles_last = 0; f->frames++;
    int left = width, top = height, right = 0, bottom = 0;
    if (!f->shadow || !f->have_frame) {
        write_rect(f, 0, 0, width, height, src, stride);
        left = top = 0; right = width; bottom = height;
    } else for (int y = 0; y < height; y += 16) {
        int h = height - y < 16 ? height - y : 16, run = -1;
        for (int x = 0; x <= width; x += 32) {
            int w = width - x < 32 ? width - x : 32;
            bool dirty = x < width && changed(f, src, stride, x, y, w, h);
            if (dirty) { f->tiles_last++; if (run < 0) run = x; }
            if ((!dirty || x + w == width) && run >= 0) {
                int end = dirty ? x + w : x;
                write_rect(f, run, y, end - run, h, src + (size_t)y * stride + run, stride);
                if (run < left) left = run; if (y < top) top = y; if (end > right) right = end; if (y + h > bottom) bottom = y + h;
                run = -1;
            }
        }
    }
    if (right > left) { flush(); if (f->damage) f->damage(f->context, left, top, right - left, bottom - top); }
    f->have_frame = f->invalidations == generation;
}
