/* Visualisations for THE WRLD Media Player (mpviz.c). */
#ifndef MPVIZ_H
#define MPVIZ_H
#include "gfx.h"

#define VIZ_BANDS 32
typedef struct {
    int16_t wave[512];                  /* mono samples being heard */
    float band[VIZ_BANDS];              /* log-spaced spectrum, 0..1 */
    float bass, mid, treble, level;     /* 0..1 */
    bool beat;                          /* a bass hit this frame */
    bool playing;
    uint32_t ms;                        /* a clock for animation */
} viz_audio_t;

int viz_collections(void);
const char *viz_collection(int c);
int viz_presets(int c);
const char *viz_preset(int c, int p);
/* draw preset p of collection c into r; `art` is the album picture (may be NULL) */
void viz_draw(canvas_t *dst, rect_t r, int c, int p, const viz_audio_t *a, canvas_t *art);
void viz_reset(void);                   /* forget trails, e.g. when the preset changes */
const char *viz_current_name(void);     /* for Randomization: the look being shown */
#endif
