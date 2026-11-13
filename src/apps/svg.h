/* SVG rendering for The WRLD Browser (svg.c) */
#ifndef SVG_H
#define SVG_H
#include "browser.h"
void svg_intrinsic(node_t *svg, int *w, int *h);                 /* width/height, or from the viewBox */
void svg_draw(canvas_t *c, node_t *svg, int x, int y, int w, int h, uint32_t current_color);
canvas_t *svg_decode(const uint8_t *data, uint32_t len);         /* an .svg file as a picture */
bool svg_sniff(const uint8_t *data, uint32_t len);               /* looks like SVG */
#endif
