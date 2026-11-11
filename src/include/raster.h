/* A small software 2D rasteriser: paths, strokes, anti-aliased fills and
 * gradients. This is the kernel's equivalent of the software backend of a
 * graphics library like Skia; it has no dependencies and draws straight into a
 * canvas_t, so the browser and the desktop can use it anywhere.
 *
 * Coordinates passed to move_to/line_to/quad_to/cubic_to are 26.6 fixed point
 * (RASTER_FIX); the rect and round-rect helpers take whole pixels. Fills are
 * clipped to the canvas clip rectangle, and colours may have an alpha byte. */
#ifndef RASTER_H
#define RASTER_H

#include "gfx.h"

#define RASTER_FIX(v) ((int32_t)(v) << 6)

typedef struct raster_path raster_path_t;

raster_path_t *raster_path_new(void);
void raster_path_free(raster_path_t *p);
void raster_reset(raster_path_t *p);

void raster_move_to(raster_path_t *p, int32_t x, int32_t y);
void raster_line_to(raster_path_t *p, int32_t x, int32_t y);
void raster_quad_to(raster_path_t *p, int32_t cx, int32_t cy, int32_t x, int32_t y);
void raster_cubic_to(raster_path_t *p, int32_t c1x, int32_t c1y,
                     int32_t c2x, int32_t c2y, int32_t x, int32_t y);
void raster_close(raster_path_t *p);

/* whole-pixel constructors */
void raster_add_rect(raster_path_t *p, int x, int y, int w, int h);
void raster_add_round_rect(raster_path_t *p, int x, int y, int w, int h, int radius);
void raster_add_circle(raster_path_t *p, int cx, int cy, int radius);

/* Fill with `color`; `evenodd` selects the even-odd rule instead of nonzero.
 * The gradient variants interpolate c0 (at x0,y0) to c1 (at x1,y1); a radial
 * gradient uses (x0,y0) as the centre and (x1,y1) to give the radius. */
void raster_fill(canvas_t *c, const raster_path_t *p, uint32_t color, bool evenodd);
void raster_fill_gradient(canvas_t *c, const raster_path_t *p,
                          int x0, int y0, int x1, int y1,
                          uint32_t c0, uint32_t c1, bool radial, bool evenodd);

/* Round joins and caps, `width` pixels wide. */
void raster_stroke(canvas_t *c, const raster_path_t *p, int width, uint32_t color);

#endif
