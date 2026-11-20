/* Images for the browser, wallpapers, the sign-in screen and Image Viewer: the format is
 * recognised here; PNG, GIF, JPEG and BMP are decoded in Rust (rust/src/imgdec.rs), SVG in
 * svg.c. Output canvases hold 0xAARRGGBB pixels; alpha is kept for blending. */
#include "kernel.h"
#include "gfx.h"
#include "fs.h"
#include "apps.h"

#define MAX_PIXELS (16 * 1024 * 1024)
static canvas_t *new_img(int w, int h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (uint64_t)w * h > MAX_PIXELS) return NULL;
    return canvas_new(w, h);
}

/* The decoders are in Rust (rust/src/imgdec.rs, rust/src/kernel/imgk.rs); they decode into
 * a canvas made here, within the size limits above. */
void *img_canvas_new(int w, int h, uint32_t **px) { canvas_t *c = new_img(w, h); if (!c) return NULL; *px = c->px; return c; }
void *img_decode_rs(int kind, const uint8_t *d, uint32_t n, char *err, int ecap);

canvas_t *image_decode(const uint8_t *d, uint32_t n, char *err, int ecap) {
    err[0] = 0;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return img_decode_rs(1, d, n, err, ecap);
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return img_decode_rs(2, d, n, err, ecap);
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return img_decode_rs(3, d, n, err, ecap);
    { extern bool svg_sniff(const uint8_t *, uint32_t); extern canvas_t *svg_decode(const uint8_t *, uint32_t);
      if (svg_sniff(d, n)) { canvas_t *c = svg_decode(d, n); if (!c) snprintf(err, ecap, "unreadable SVG"); return c; } }
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') { canvas_t *c = bmp_load(d, n); if (!c) snprintf(err, ecap, "unsupported BMP"); return c; }
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) snprintf(err, ecap, "WebP images are not supported");
    else if (n >= 5 && (!memcmp(d, "<svg", 4) || !memcmp(d, "<?xml", 5))) snprintf(err, ecap, "SVG images are not supported");
    else snprintf(err, ecap, "unknown image format");
    return NULL;
}
