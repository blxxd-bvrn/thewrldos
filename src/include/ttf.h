/* A small TrueType engine: sfnt tables, Unicode cmaps, outlines with
 * composites, quadratic rasterisation with 8x8 anti-aliasing.
 *
 * It has no library dependencies and no global state: the caller supplies the
 * font file and an allocator, which is what lets the kernel and a host test
 * share the same source. OpenType/CFF outlines ('OTTO') are not supported;
 * neither are hinting instructions, which the rasteriser ignores.
 *
 * Coordinates follow the font: y grows upwards, and a size of `px` means an em
 * of `px` pixels, the same meaning as a CSS font-size. */
#ifndef TTF_H
#define TTF_H

#ifdef TTF_STANDALONE
#include <stdint.h>
#include <stddef.h>
#else
#include "types.h"
#endif

typedef struct ttf_face ttf_face_t;

typedef void *(*ttf_alloc_fn)(void *user, size_t bytes);
typedef void  (*ttf_free_fn)(void *user, void *p);

typedef struct {
    int      width, height;   /* coverage bitmap size in pixels (0 for a blank glyph) */
    int      xoff, yoff;      /* bitmap's top-left, from the pen and from the baseline */
    int      advance;         /* how far the pen moves, in pixels */
    uint8_t *coverage;        /* width*height bytes: 0 blank, 255 fully covered */
} ttf_glyph_t;

/* Parses `data` (an sfnt or the first face of a collection) and keeps a
 * pointer to it: the buffer must outlive the face. Returns NULL for a file
 * this engine cannot read. */
ttf_face_t *ttf_open(const void *data, size_t size,
                     ttf_alloc_fn alloc, ttf_free_fn free_fn, void *user);
void ttf_close(ttf_face_t *face);

int ttf_units_per_em(const ttf_face_t *face);
int ttf_ascender(const ttf_face_t *face);     /* font units, y up */
int ttf_descender(const ttf_face_t *face);
int ttf_line_gap(const ttf_face_t *face);
int ttf_num_glyphs(const ttf_face_t *face);

/* The glyph for a Unicode code point, 0 (".notdef") when the font has none. */
int ttf_glyph_index(const ttf_face_t *face, uint32_t codepoint);
int ttf_advance(const ttf_face_t *face, int glyph);          /* font units */
int ttf_kerning(const ttf_face_t *face, int left, int right); /* font units */

/* Rasterises one glyph. `slant` shears the outline by that percentage of the
 * em for each em above the baseline (0 upright, about 20 for an oblique);
 * pass 0 when the face already is italic. The coverage buffer belongs to the
 * caller: release it with ttf_glyph_release(). Returns 0, or -1 on failure. */
int ttf_render(const ttf_face_t *face, int glyph, int px, int slant,
               ttf_glyph_t *out);
void ttf_glyph_release(const ttf_face_t *face, ttf_glyph_t *glyph);

/* ---------------- layout ---------------- */

/* One glyph after shaping, in font units (y grows upwards, like the outlines).
 * `advance` is how far the pen moves; `x_offset`/`y_offset` move this glyph
 * from the pen, which is how marks sit over their base. `cluster` is the index
 * of the first code point this glyph came from. */
typedef struct {
    int gid;
    int advance;
    int x_offset, y_offset;
    int cluster;
} ttf_shaped_t;

/* Shapes one run of text - a single script, one direction - with the font's
 * own GSUB and GPOS tables: ligatures, contextual substitutions, Arabic
 * joining forms and mark attachment. `script` is an OpenType script tag
 * ('arab', 'hebr', 'latn', ...) or 0 for the default script. `rtl` selects the
 * Arabic joining features and reverses advances for the caller to lay out.
 *
 * The result belongs to the face's allocator: release it with
 * ttf_shape_free(). Returns NULL when the text has no glyphs. */
ttf_shaped_t *ttf_shape(const ttf_face_t *face, const uint32_t *cps, int n,
                        uint32_t script, int rtl, int *out_n);
void ttf_shape_free(const ttf_face_t *face, ttf_shaped_t *shaped);

#endif
