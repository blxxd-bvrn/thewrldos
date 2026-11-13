#ifndef ANIMATION_H
#define ANIMATION_H
#include "types.h"

/* Time-based, reversible transitions. No sleeps, floating point or frame-count
 * assumptions; unsigned elapsed time also works across the timer's wrap. */
typedef struct { int from, to; uint32_t start, duration; } animation_t;

static inline bool animation_running(const animation_t *a, uint32_t now) {
    return a->from != a->to && now - a->start < a->duration;
}
static inline int animation_value(const animation_t *a, uint32_t now) {
    uint32_t elapsed = now - a->start;
    if (!a->duration || elapsed >= a->duration) return a->to;
    int t = (int)((uint64_t)elapsed * 1024 / a->duration);
    int ease = t * t / 1024 * (3072 - 2 * t) / 1024;   /* smoothstep */
    return a->from + (int)((int64_t)(a->to - a->from) * ease / 1024);
}
static inline void animation_set(animation_t *a, int value) {
    a->from = a->to = value; a->start = a->duration = 0;
}
static inline void animation_start(animation_t *a, int to, uint32_t now, uint32_t ms) {
    a->from = animation_value(a, now); a->to = to; a->start = now; a->duration = ms;
}
static inline int animation_lerp(int from, int to, int progress) {
    return from + (int)((int64_t)(to - from) * progress / 255);
}
#endif
