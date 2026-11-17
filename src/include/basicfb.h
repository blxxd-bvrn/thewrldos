/* WRLD Basic Display: native firmware-framebuffer presentation, not GPU 3D.
 * No firmware calls, device registers or framebuffer reads occur here. */
#ifndef BASICFB_H
#define BASICFB_H
#include "types.h"

#define BASICFB_MAX_BYTES (256u * 1024 * 1024)
typedef struct {
    uint32_t width, height, pitch, bpp;
    uint8_t red_pos, red_bits, green_pos, green_bits, blue_pos, blue_bits;
} basicfb_mode_t;
typedef void (*basicfb_damage_fn)(void *, int, int, int, int);
typedef struct {
    basicfb_mode_t mode;
    uint8_t *pixels;
    uint32_t *shadow;                    /* optional caller-owned canonical ARGB history */
    bool ready, have_frame, streaming;
    volatile uint32_t invalidations;     /* IRQ/emergency writes can interrupt a present */
    uint64_t frames, bytes_written, bytes_last;
    uint32_t regions_last, tiles_last;
    basicfb_damage_fn damage;
    void *context;
    uint32_t rgb_lut[3][256];
} basicfb_t;

bool basicfb_mode_valid(const basicfb_mode_t *, const char **reason);
bool basicfb_bind(basicfb_t *, void *pixels, const basicfb_mode_t *, uint32_t *shadow,
                  size_t shadow_bytes, bool write_combining, basicfb_damage_fn, void *);
void basicfb_invalidate(basicfb_t *);
/* Source is opaque canonical ARGB; stride is in pixels. Writes are clipped.
 * write()'s source starts at the requested destination rectangle's origin. */
void basicfb_write(basicfb_t *, int x, int y, int w, int h, const uint32_t *, uint32_t stride);
void basicfb_present(basicfb_t *, const uint32_t *, uint32_t stride);
#endif
