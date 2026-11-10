/* Unicode text layout: the bidirectional algorithm (UAX #9) and script
 * itemisation, the two things a line of text needs before it can be shaped.
 *
 * The result is one embedding level per code point: even levels run left to
 * right, odd levels right to left. Text is then cut into runs at level and
 * script boundaries, each run is shaped by the font, and the runs (and the
 * glyphs of a right-to-left run) are put back in visual order. */
#ifndef TEXT_H
#define TEXT_H

#include "types.h"

/* UAX #9 paragraph level: 0 left-to-right, 1 right-to-left. */
int text_bidi_base(const uint32_t *cps, int n);

/* Fills `levels` (one per code point). `base` is 0 or 1 from the function
 * above, or -1 to derive it from the first strong character. */
void text_bidi_levels(const uint32_t *cps, int n, uint8_t *levels, int base);

/* The mirrored form of a bracket in right-to-left text, or `cp` itself. */
uint32_t text_mirror(uint32_t cp);

/* OpenType script tag for a code point ('arab', 'hebr', 'latn', ...), or 0
 * for characters that belong to no script (digits, punctuation, spaces). */
uint32_t text_script(uint32_t cp);

#endif
