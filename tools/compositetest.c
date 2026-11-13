/* Host checks for the exact compositor used by Aero window effects. */
#include "gfx.h"
#include <stdio.h>

static int fails, cases;
static const uint32_t bg = 0xFF294D61, guard = 0x12345678;
static uint32_t over(uint32_t dst, uint32_t src, int alpha) {
    int a = (C_A(src) * CLAMP(alpha, 0, 255) + 127) / 255;
    if (!a) return dst;
    if (a == 255) return src;
    return RGB((C_R(src) * a + C_R(dst) * (255 - a)) >> 8,
               (C_G(src) * a + C_G(dst) * (255 - a)) >> 8,
               (C_B(src) * a + C_B(dst) * (255 - a)) >> 8);
}
static void test(rect_t r, rect_t clip, int alpha) {
    /* Both canvases have padded scanlines; guard words surround the output. */
    uint32_t data[82], pixels[15] = { 0xFFFF0000, 0xFF00FF00, 0x00000000, 0, 0,
                                    0xFF0000FF, 0x80FFFFFF, 0xFFE08040, 0, 0,
                                    0xFFFFFFFF, 0xFF123456, 0x4000FFFF, 0, 0 };
    for (int i = 0; i < 82; i++) data[i] = bg;
    data[0] = data[81] = guard;
    canvas_t dst = { data + 1, 8, 8, 10, clip }, src = { pixels, 3, 3, 5, { 0, 0, 3, 3 } };
    blit_scaled_alpha(&dst, r, &src, alpha);
    cases++;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 10; x++) {
        uint32_t want = bg;
        if (x < 8 && r.w > 0 && r.h > 0 && pt_in(clip, x, y) && pt_in(r, x, y) && alpha > 0) {
            int sx = (int)((int64_t)(x - r.x) * src.w / r.w), sy = (int)((int64_t)(y - r.y) * src.h / r.h);
            want = over(bg, pixels[sy * src.stride + sx], alpha);
        }
        if (data[1 + y * 10 + x] != want) {
            printf("FAIL case %d pixel %d,%d: got %08x wanted %08x\n", cases, x, y, data[1 + y * 10 + x], want);
            fails++; return;
        }
    }
    if (data[0] != guard || data[81] != guard) { puts("FAIL output guard overwritten"); fails++; }
}
int main(void) {
    const rect_t rects[] = { { 0, 0, 3, 3 }, { 1, 1, 6, 6 }, { -2, -1, 6, 6 }, { -3, -3, 9, 9 },
                            { 6, 5, 7, 8 }, { 0, 0, 1, 1 }, { -8, 2, 3, 3 }, { 9, 9, 6, 6 },
                            { 0, 0, 0, 4 }, { 2, 2, 4, 0 }, { 0, 0, -1, 8 }, { 2, 1, 4, 7 } };
    const rect_t clips[] = { { 0, 0, 8, 8 }, { 1, 2, 4, 3 }, { 0, 0, 0, 0 } };
    const int alphas[] = { -1, 0, 1, 64, 128, 254, 255, 300 };
    for (unsigned r = 0; r < ARRAY_LEN(rects); r++)
        for (unsigned c = 0; c < ARRAY_LEN(clips); c++)
            for (unsigned a = 0; a < ARRAY_LEN(alphas); a++) test(rects[r], clips[c], alphas[a]);
    printf("compositor tests: %s (%d cases)\n", fails ? "FAILED" : "passed", cases);
    return fails ? 1 : 0;
}
