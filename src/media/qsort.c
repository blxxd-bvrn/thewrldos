/* qsort for the decoders (the C library shim has none): a heapsort, so no
 * recursion and no extra memory. */
#include <stdlib.h>
#include <string.h>

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n) { while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; } }
static void sift(unsigned char *base, size_t start, size_t end, size_t size, int (*cmp)(const void *, const void *)) {
    size_t root = start;
    while (root * 2 + 1 <= end) {
        size_t child = root * 2 + 1, best = root;
        if (cmp(base + best * size, base + child * size) < 0) best = child;
        if (child + 1 <= end && cmp(base + best * size, base + (child + 1) * size) < 0) best = child + 1;
        if (best == root) return;
        swap_bytes(base + root * size, base + best * size, size);
        root = best;
    }
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2 || !size) return;
    unsigned char *b = base;
    for (size_t s = (n - 2) / 2 + 1; s-- > 0;) sift(b, s, n - 1, size, cmp);
    for (size_t end = n - 1; end > 0; end--) {
        swap_bytes(b, b + end * size, size);
        sift(b, 0, end - 1, size, cmp);
    }
}
