/* Native software composition for window snapshots. Kept independent of the
 * framebuffer/font code so clipping and alpha can also be checked on the host. */
#include "gfx.h"

void blit_scaled_alpha(canvas_t *dst, rect_t r, canvas_t *src, int alpha) {
    if (!src || !src->px || src->w <= 0 || src->h <= 0 || r.w <= 0 || r.h <= 0 || alpha <= 0) return;
    alpha = MIN(alpha, 255);
    int x0 = MAX(r.x, dst->clip.x), y0 = MAX(r.y, dst->clip.y);
    int x1 = MIN(r.x + r.w, dst->clip.x + dst->clip.w), y1 = MIN(r.y + r.h, dst->clip.y + dst->clip.h);
    if (x1 <= x0 || y1 <= y0) return;
    /* Quotient/remainder stepping is exact even after left-edge clipping.
     * Rounded fixed-point steps can sample the wrong column at integer edges. */
    int step = src->w / r.w, rem = src->w % r.w;
    int64_t first = (int64_t)(x0 - r.x) * src->w;
    int sx0 = (int)(first / r.w), error0 = (int)(first % r.w);
    for (int y = y0; y < y1; y++) {
        const uint32_t *s = src->px + (int)((int64_t)(y - r.y) * src->h / r.h) * src->stride;
        uint32_t *d = dst->px + y * dst->stride;
        int sx = sx0, error = error0;
        for (int x = x0; x < x1; x++) {
            uint32_t col = s[sx];
            sx += step; error += rem;
            if (error >= r.w) { sx++; error -= r.w; }
            int a = (C_A(col) * alpha + 127) / 255;
            if (a >= 255) d[x] = col;
            else if (a) {
                uint32_t rb = ((col & 0xFF00FF) * a + (d[x] & 0xFF00FF) * (255 - a)) >> 8;
                uint32_t g = ((col & 0x00FF00) * a + (d[x] & 0x00FF00) * (255 - a)) >> 8;
                d[x] = 0xFF000000u | (rb & 0xFF00FF) | (g & 0x00FF00);
            }
        }
    }
}
