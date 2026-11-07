/* Physical memory discovery, 4 MiB-page paging and kernel heap */
#include "kernel.h"

typedef struct PACKED {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
} mb_info_t;
typedef struct PACKED { uint32_t size; uint64_t addr, len; uint32_t type; } mb_mmap_t;

extern uint8_t kernel_end[];
static uint32_t page_dir[1024] __attribute__((aligned(4096)));
static uint32_t heap_start, heap_end, heap_used, phys_total;

/* ---------- heap: address-ordered block list with coalescing ---------- */
typedef struct block {
    uint32_t size;          /* total size including header */
    uint32_t magic;         /* 0xF2EE / 0xA110C */
    struct block *next, *prev;
} block_t;
#define HDR sizeof(block_t)
#define MAGIC_FREE 0xF2EEF2EE
#define MAGIC_USED 0xA110CA7E
static block_t *heap_first;
static block_t *heap_rover;

static void heap_init(uint32_t start, uint32_t end) {
    start = (start + 15) & ~15u;
    heap_start = start; heap_end = end;
    heap_first = (block_t *)start;
    heap_first->size = end - start;
    heap_first->magic = MAGIC_FREE;
    heap_first->next = heap_first->prev = NULL;
    heap_rover = heap_first;
}

void *kmalloc(size_t n) {
    if (!n) n = 1;
    uint32_t need = (n + HDR + 15) & ~15u;
    uint32_t f = irq_save();
    block_t *b = heap_rover, *start = heap_rover;
    do {
        if (b->magic == MAGIC_FREE && b->size >= need) {
            if (b->size - need >= HDR + 64) {
                block_t *nb = (block_t *)((uint8_t *)b + need);
                nb->size = b->size - need; nb->magic = MAGIC_FREE;
                nb->next = b->next; nb->prev = b;
                if (b->next) b->next->prev = nb;
                b->next = nb; b->size = need;
            }
            b->magic = MAGIC_USED;
            heap_used += b->size;
            heap_rover = b->next ? b->next : heap_first;
            task_t *t = task_current(); if (t) t->mem_bytes += b->size;
            irq_restore(f);
            return (uint8_t *)b + HDR;
        }
        b = b->next ? b->next : heap_first;
    } while (b != start);
    irq_restore(f);
    klog("kmalloc: out of memory (%u bytes)", n);
    return NULL;
}
void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC_USED) { klog("kfree: bad pointer %p", p); return; }
    uint32_t f = irq_save();
    b->magic = MAGIC_FREE;
    heap_used -= b->size;
    task_t *t = task_current(); if (t && t->mem_bytes >= b->size) t->mem_bytes -= b->size;
    if (b->next && b->next->magic == MAGIC_FREE) {
        block_t *n = b->next;
        if (heap_rover == n) heap_rover = b;
        b->size += n->size; b->next = n->next; if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->magic == MAGIC_FREE) {
        block_t *pv = b->prev;
        if (heap_rover == b) heap_rover = pv;
        pv->size += b->size; pv->next = b->next; if (b->next) b->next->prev = pv;
    }
    irq_restore(f);
}
void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    size_t old = b->size - HDR;
    if (old >= n) return p;
    void *q = kmalloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    kfree(p);
    return q;
}
void *kmalloc_aligned(size_t n, size_t align) {
    uint8_t *raw = kmalloc(n + align + 8);
    if (!raw) return NULL;
    uint32_t a = ((uint32_t)raw + 8 + align - 1) & ~(align - 1);
    ((uint32_t *)a)[-1] = (uint32_t)raw;
    return (void *)a;
}
size_t kmalloc_usable(void *p) {
    if (!p) return 0;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    return b->magic == MAGIC_USED ? b->size - HDR : 0;
}
void kfree_aligned(void *p) { if (p) kfree((void *)((uint32_t *)p)[-1]); }
uint32_t mm_total(void) { return heap_end - heap_start; }
uint32_t mm_used(void) { return heap_used; }
uint32_t mm_phys_total(void) { return phys_total; }

/* ---------- paging ---------- */
uint32_t *paging_kernel_dir(void) { return page_dir; }
uint32_t *paging_new_dir(void) {
    uint32_t *d = kmalloc_aligned(4096, 4096);
    memcpy(d, page_dir, 4096);
    return d;
}
/* device registers (MMIO) must not be cached: set PCD|PWT on the 4 MiB page */
void paging_set_uncached(uint32_t addr) {
    page_dir[addr >> 22] |= 0x18;
    uint32_t cr3; __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3) :: "memory");
}
void paging_free_dir(uint32_t *d) { if (d && d != page_dir) kfree_aligned(d); }

static void paging_init(void) {
    for (int i = 0; i < 1024; i++) page_dir[i] = (i << 22) | 0x83;  /* present, rw, 4 MiB */
    uint32_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= 0x10;                                                    /* PSE */
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4));
    __asm__ volatile("mov %0,%%cr3" :: "r"(page_dir));
    uint32_t cr0; __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= 0x80000000u;  /* PG */
    cr0 &= ~(1u << 2);   /* EM off: use the x87 FPU */
    cr0 |= (1u << 1) | (1u << 5);
    __asm__ volatile("mov %0,%%cr0" :: "r"(cr0));
    __asm__ volatile("fninit");
}

uint8_t *initrd_data;
uint32_t initrd_size;
bool boot_verbose, boot_safe_mode;

void mm_init(uint32_t mbaddr) {
    mb_info_t *mb = (mb_info_t *)mbaddr;
    /* 4..16 MiB is reserved for Win32 process images; the kernel itself is loaded at 16 MiB */
    uint32_t lowest = ((uint32_t)kernel_end + 0xFFF) & ~0xFFFu;
    if (lowest < 16 * 1024 * 1024) lowest = 16 * 1024 * 1024;
    uint32_t top = 64 * 1024 * 1024;
    if (mb->flags & 1) phys_total = (mb->mem_upper + 1024) * 1024;
    if (mb->flags & (1 << 6)) {
        uint32_t best = 0;
        for (uint32_t p = mb->mmap_addr; p < mb->mmap_addr + mb->mmap_length;) {
            mb_mmap_t *e = (mb_mmap_t *)p;
            if (e->type == 1 && e->addr <= 0x100000 + 0 && 0) {}
            if (e->type == 1 && e->addr >= 0x100000 && e->addr < 0xFFFFFFFFull) {
                uint64_t end = e->addr + e->len;
                if (end > 0xF0000000ull) end = 0xF0000000ull;
                if (end - e->addr > best) { best = end - e->addr; top = (uint32_t)end; }
            }
            p += e->size + 4;
        }
        if (top > phys_total) phys_total = top;
    } else if (mb->flags & 1) top = (mb->mem_upper + 1024) * 1024;

    /* initrd module: copy it above the reserved window */
    uint32_t mod_start = 0, mod_end = 0;
    if ((mb->flags & 8) && mb->mods_count) {
        uint32_t *mod = (uint32_t *)mb->mods_addr;
        mod_start = mod[0]; mod_end = mod[1];
        if (mod_end > lowest) lowest = (mod_end + 0xFFF) & ~0xFFFu;
    }
    if (mb->flags & 4) {
        const char *cmd = (const char *)mb->cmdline;
        if (strstr(cmd, "verbose")) boot_verbose = true;
        if (strstr(cmd, "safe")) boot_safe_mode = true;
        extern bool boot_setup_mode; extern uint32_t boot_root_serial;
        if (strstr(cmd, "setup")) boot_setup_mode = true;
        const char *r = strstr(cmd, "root=");
        if (r) boot_root_serial = strtoul(r + 5, NULL, 16);
    }
    paging_init();
    heap_init(lowest, top);
    if (mod_end > mod_start) {
        initrd_size = mod_end - mod_start;
        initrd_data = kmalloc(initrd_size);
        memcpy(initrd_data, (void *)mod_start, initrd_size);
    }
    (void)kernel_end;
}
