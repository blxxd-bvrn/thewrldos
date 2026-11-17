/* Large-page PAT selectors shared by x86/x64 and host tests. PA1 is selected by
 * PWT, NOT by the large-page PAT bit (which selects PA4). */
#ifndef FBMAP_H
#define FBMAP_H
#include "types.h"
#define FBMAP_PAT_MASK 0x1018ull
#define FBMAP_WC 0x8ull
#define FBMAP_UC 0x18ull
static inline uint64_t fbmap_cache_bits(uint64_t entry, bool wc) {
    return (entry & ~FBMAP_PAT_MASK) | (wc ? FBMAP_WC : FBMAP_UC);
}
typedef struct { uint64_t base; uint32_t offset, pages; } mmio_plan_t;
static inline bool mmio_plan(uint64_t physical, uint32_t size, uint32_t limit, unsigned bits, mmio_plan_t *out) {
    uint64_t end = physical + size;
    if (!physical || !size || size > limit || end < physical || bits < 32 || bits > 52 || ((end - 1) >> bits)) return false;
    uint32_t offset = physical & 0x1FFFFF, pages = ((uint64_t)offset + size + 0x1FFFFF) >> 21;
    if (!pages || pages > 512) return false;
    if (out) *out = (mmio_plan_t){ physical & ~0x1FFFFFull, offset, pages };
    return true;
}
#endif
