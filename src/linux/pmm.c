/* Physical pages for Linux processes: 4 KiB pages cut from 2 MiB chunks of
 * the kernel heap (which is identity-mapped, so a page's address is both its
 * physical and its kernel-visible address). Freed pages go on a list and are
 * reused; chunks are not given back to the heap. */
#ifdef __x86_64__
#include "lx.h"

#define CHUNK (2u << 20)
typedef struct freepg { struct freepg *next; } freepg_t;
static freepg_t *free_list;
static uint32_t used, total;

void *pmm_alloc(void) {
    if (!free_list) {
        uint8_t *c = kmalloc_aligned(CHUNK, 4096);
        if (!c) {
            c = kmalloc_aligned(256 * 1024, 4096);      /* the heap is fragmented or nearly full */
            if (!c) return NULL;
            for (uint32_t o = 0; o < 256 * 1024; o += 4096) { freepg_t *f = (freepg_t *)(c + o); f->next = free_list; free_list = f; total++; }
        } else {
            for (uint32_t o = 0; o < CHUNK; o += 4096) { freepg_t *f = (freepg_t *)(c + o); f->next = free_list; free_list = f; total++; }
        }
    }
    freepg_t *p = free_list;
    free_list = p->next;
    used++;
    memset(p, 0, 4096);
    return p;
}
void pmm_free(void *p) {
    if (!p) return;
    freepg_t *f = p; f->next = free_list; free_list = f;
    used--;
}
uint32_t pmm_used_pages(void) { return used; }
#endif
