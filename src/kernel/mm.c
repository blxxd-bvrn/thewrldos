/* Physical memory discovery, paging and the kernel heap.
 * x86: 4 MiB pages identity-map all 4 GiB. x64: the boot stub's tables identity-map
 * the first 4 GiB with 2 MiB pages; Win32 processes get private address spaces. */
#include "kernel.h"
#include "bootinfo.h"

extern uint8_t kernel_end[];
#ifndef __x86_64__
static uint32_t page_dir[1024] __attribute__((aligned(4096)));
#endif
static uintptr_t heap_start, heap_end;
static uint32_t heap_used; static uint64_t phys_total;

/* ---------- heap: address-ordered block list with coalescing ---------- */
typedef struct block {
    uint32_t size;          /* total size including header */
    uint32_t magic;         /* 0xF2EE / 0xA110C */
    struct block *next, *prev;
#ifdef __x86_64__
    uint64_t pad;           /* keep blocks (and what we hand out) 16-byte aligned */
#endif
} block_t;
#define HDR sizeof(block_t)
#define MAGIC_FREE 0xF2EEF2EE
#define MAGIC_USED 0xA110CA7E
static block_t *heap_first;
static block_t *heap_rover;

static void heap_init(uintptr_t start, uintptr_t end) {
    start = (start + 15) & ~(uintptr_t)15;
    heap_start = start; heap_end = end;
    heap_first = (block_t *)start;
    heap_first->size = end - start;
    heap_first->magic = MAGIC_FREE;
    heap_first->next = heap_first->prev = NULL;
    heap_rover = heap_first;
}

void *kmalloc(size_t n) {
    if (!n) n = 1;
    if (n > 0x7FFF0000u) { klog("kmalloc: refusing %u MB", (uint32_t)(n >> 20)); return NULL; }
    uint32_t need = (n + HDR + 15) & ~15u;
    uintptr_t f = irq_save();
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
    klog("kmalloc: out of memory (%u bytes)", (uint32_t)n);
    return NULL;
}
void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    if (b->magic != MAGIC_USED) { klog("kfree: bad pointer %p (from %p)", p, __builtin_return_address(0)); return; }
    uintptr_t f = irq_save();
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
    uintptr_t a = ((uintptr_t)raw + 8 + align - 1) & ~(uintptr_t)(align - 1);
    ((uintptr_t *)a)[-1] = (uintptr_t)raw;
    return (void *)a;
}
size_t kmalloc_usable(void *p) {
    if (!p) return 0;
    block_t *b = (block_t *)((uint8_t *)p - HDR);
    return b->magic == MAGIC_USED ? b->size - HDR : 0;
}
void kfree_aligned(void *p) { if (p) kfree((void *)((uintptr_t *)p)[-1]); }
uint32_t mm_total(void) { return heap_end - heap_start; }
uint32_t mm_used(void) { return heap_used; }
uint64_t mm_phys_total(void) { return phys_total; }

/* ---------- paging ---------- */
#ifdef __x86_64__
extern uint64_t boot_pml4[512], boot_pdpt[512], boot_pd[2048];
uintptr_t paging_kernel_root(void) { return (uintptr_t)boot_pml4; }
static void flush_tlb(void) { uintptr_t cr3; __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3) :: "memory"); }
/* device registers (MMIO) must not be cached: set PCD|PWT on the 2 MiB page */
void paging_set_uncached(uintptr_t addr) {
    if (addr >= 0x100000000ull) return;
    boot_pd[addr >> 21] |= 0x18;
    flush_tlb();
}
/* The framebuffer is written far more than it is read: mark its page as
 * write-combining (PAT entry 1) so full-screen composites leave the CPU in
 * bursts instead of one uncached store at a time. */
void paging_set_wc(uintptr_t addr, uint32_t size) {
    if (addr >= 0x100000000ull) return;
    uint64_t end = (uint64_t)addr + size;
    if (end > 0x100000000ull) end = 0x100000000ull;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    uint64_t pat = ((uint64_t)hi << 32) | lo;
    pat = (pat & ~0xFF00ull) | 0x0100ull;      /* PA1 = write-combining */
    __asm__ volatile("wrmsr" :: "c"(0x277), "a"((uint32_t)pat), "d"((uint32_t)(pat >> 32)));
    for (uint64_t a = addr & ~0x1FFFFFull; a < end; a += 0x200000)
        boot_pd[a >> 21] = (boot_pd[a >> 21] & ~0x18ull) | (1ull << 12);
    flush_tlb();
}
/* Private address spaces for Win32 programs. Each has its own PML4 and PDPT;
 * page directories are shared with the kernel (identity 0..4 GiB) until a 2 MiB
 * page inside one is replaced, then that directory is copied. Tables are
 * remembered so the space can be freed. */
typedef struct { uint64_t *pml4; void *tables[192]; int ntables; } vmspace_t;
static vmspace_t spaces[64];
static uint64_t *new_table(vmspace_t *v, const uint64_t *copy) {
    if (v->ntables >= 192) return NULL;
    uint64_t *t = kmalloc_aligned(4096, 4096);
    if (!t) return NULL;
    if (copy) memcpy(t, copy, 4096); else memset(t, 0, 4096);
    v->tables[v->ntables++] = t;
    return t;
}
static vmspace_t *space_of(uintptr_t root) { for (int i = 0; i < 64; i++) if (spaces[i].pml4 && (uintptr_t)spaces[i].pml4 == root) return &spaces[i]; return NULL; }
static bool owned(vmspace_t *v, const void *t) { for (int i = 0; i < v->ntables; i++) if (v->tables[i] == t) return true; return false; }
uintptr_t vm_space_new(void) {
    vmspace_t *v = NULL;
    for (int i = 0; i < 64; i++) if (!spaces[i].pml4) { v = &spaces[i]; break; }
    if (!v) return 0;
    v->ntables = 0;
    uint64_t *pml4 = new_table(v, boot_pml4);
    uint64_t *pdpt = new_table(v, boot_pdpt);
    if (!pml4 || !pdpt) { for (int i = 0; i < v->ntables; i++) kfree_aligned(v->tables[i]); return 0; }
    pml4[0] = (uintptr_t)pdpt | 7;
    v->pml4 = pml4;
    return (uintptr_t)pml4;
}
bool vm_map_2m(uintptr_t root, uint64_t va, uintptr_t pa) {
    vmspace_t *v = space_of(root);
    if (!v || (va >> 39) != 0 || (va & 0x1FFFFF) || (pa & 0x1FFFFF)) return false;   /* first 512 GiB only */
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(v->pml4[0] & ~0xFFFull);
    int gi = (va >> 30) & 511, di = (va >> 21) & 511;
    uint64_t *pd = (pdpt[gi] & 1) ? (uint64_t *)(uintptr_t)(pdpt[gi] & ~0xFFFull) : NULL;
    if (!pd || !owned(v, pd)) {                         /* shared kernel directory, or none: make a private one */
        pd = new_table(v, pd);
        if (!pd) return false;
        pdpt[gi] = (uintptr_t)pd | 7;
    }
    pd[di] = pa | 0x87;                                  /* present, writable, user, 2 MiB */
    if (root == ({ uintptr_t c; __asm__ volatile("mov %%cr3,%0" : "=r"(c)); c; })) flush_tlb();
    return true;
}
/* map one 4 KiB page; a 2 MiB page that was there (the kernel's identity map) is replaced */
bool vm_map_4k(uintptr_t root, uint64_t va, uintptr_t pa) {
    vmspace_t *v = space_of(root);
    if (!v || (va >> 39) != 0 || (va & 0xFFF) || (pa & 0xFFF)) return false;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(v->pml4[0] & ~0xFFFull);
    int gi = (va >> 30) & 511, di = (va >> 21) & 511, ti = (va >> 12) & 511;
    uint64_t *pd = (pdpt[gi] & 1) ? (uint64_t *)(uintptr_t)(pdpt[gi] & ~0xFFFull) : NULL;
    if (!pd || !owned(v, pd)) {
        pd = new_table(v, pd);
        if (!pd) return false;
        pdpt[gi] = (uintptr_t)pd | 7;
    }
    uint64_t *pt = ((pd[di] & 1) && !(pd[di] & 0x80)) ? (uint64_t *)(uintptr_t)(pd[di] & ~0xFFFull) : NULL;
    if (!pt || !owned(v, pt)) {
        pt = new_table(v, NULL);
        if (!pt) return false;
        pd[di] = (uintptr_t)pt | 7;
    }
    pt[ti] = pa | 7;                                     /* present, writable, user */
    if (root == ({ uintptr_t c; __asm__ volatile("mov %%cr3,%0" : "=r"(c)); c; })) flush_tlb();
    return true;
}
void vm_space_free(uintptr_t root) {
    vmspace_t *v = space_of(root);
    if (!v) return;
    for (int i = 0; i < v->ntables; i++) kfree_aligned(v->tables[i]);
    v->ntables = 0; v->pml4 = NULL;
}
static void paging_init(void) { }                      /* done by the boot stub */
#else
uint32_t *paging_kernel_dir(void) { return page_dir; }
uintptr_t paging_kernel_root(void) { return (uintptr_t)page_dir; }
uint32_t *paging_new_dir(void) {
    uint32_t *d = kmalloc_aligned(4096, 4096);
    memcpy(d, page_dir, 4096);
    return d;
}
/* device registers (MMIO) must not be cached: set PCD|PWT on the 4 MiB page */
void paging_set_uncached(uintptr_t addr) {
    page_dir[addr >> 22] |= 0x18;
    uint32_t cr3; __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(cr3) :: "memory");
}
/* write-combining needs the PAT/PAT MSR; the 32-bit build keeps the default */
void paging_set_wc(uintptr_t addr, uint32_t size) { (void)addr; (void)size; }
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
#endif

/* the initrd's files now live in the VFS: free the archive */
void mm_release_initrd(void) {
    if (!initrd_data) return;
    klog("memory: %u KB initrd archive freed after unpacking", initrd_size / 1024);
    kfree(initrd_data);
    initrd_data = NULL; initrd_size = 0;
}
uint8_t *initrd_data;
uint32_t initrd_size;
bool boot_verbose, boot_safe_mode, boot_force_lapic;

void mm_init(void) {
    /* 4..16 MiB is reserved for Win32 process images; the kernel itself is loaded at 16 MiB */
    uintptr_t lowest = ((uintptr_t)kernel_end + 0xFFF) & ~(uintptr_t)0xFFF;
    if (lowest < 16 * 1024 * 1024) lowest = 16 * 1024 * 1024;
    uint32_t top = 64 * 1024 * 1024;
    if (bootinfo.have_mem) phys_total = (uint64_t)(bootinfo.mem_upper + 1024) * 1024;
    if (bootinfo.mmap_count) {
        uint64_t best = 0, ramsz = 0;
        for (int i = 0; i < bootinfo.mmap_count; i++) {
            const boot_mmap_t *e = &bootinfo.mmap[i];
            if (e->type == 1 && e->addr >= 0x100000) {
                uint64_t end = e->addr + e->len;
                ramsz += e->len;
                if (e->addr < 0xF0000000ull) { if (end > 0xF0000000ull) end = 0xF0000000ull; if (end - e->addr > best) { best = end - e->addr; top = (uint32_t)end; } }
            }
        }
        if (ramsz) phys_total = ramsz;
        if (top > phys_total) phys_total = top;
    } else if (bootinfo.have_mem) top = (bootinfo.mem_upper + 1024) * 1024;

    /* initrd module: copy it above the reserved window */
    uint32_t mod_start = 0, mod_end = 0;
    if (bootinfo.module_count) {
        uint64_t s = bootinfo.modules[0].start, e = bootinfo.modules[0].end;
        if (e > s && e <= 0xFFFFFFFFull) {
            mod_start = (uint32_t)s; mod_end = (uint32_t)e;
            if (mod_end > lowest) lowest = (mod_end + 0xFFF) & ~0xFFFu;
        } else if (e > s) {
            klog("initrd: module at %llx is above the 4 GiB identity map, ignored", (unsigned long long)s);
        }
    }
    if (bootinfo.cmdline) {
        const char *cmd = bootinfo.cmdline;
        if (strstr(cmd, "verbose")) boot_verbose = true;
        if (strstr(cmd, "safe")) boot_safe_mode = true;
        if (strstr(cmd, "lapic")) boot_force_lapic = true;
        extern bool boot_setup_mode; extern uint32_t boot_root_serial;
        if (strstr(cmd, "setup")) boot_setup_mode = true;
        const char *r = strstr(cmd, "root=");
        if (r) boot_root_serial = strtoul(r + 5, NULL, 16);
    }
    /* Stay inside the one available region that holds the kernel and initrd.
     * EFI memory maps have reserved holes (ACPI, runtime services) that a
     * linear heap growing towards the top of RAM must not walk into.  On a
     * BIOS map this normally selects the same range as before. */
    if (bootinfo.mmap_count) {
        for (int i = 0; i < bootinfo.mmap_count; i++) {
            const boot_mmap_t *e = &bootinfo.mmap[i];
            if (e->type != 1 || e->addr > lowest || lowest >= e->addr + e->len) continue;
            uint64_t end = e->addr + e->len;
            if (end > 0xF0000000ull) end = 0xF0000000ull;
            if (end > lowest) top = (uint32_t)end;
            break;
        }
    }
    paging_init();
    heap_init(lowest, top);
    /* GRUB may load the initrd in low memory, where the ISA DMA buffer and the Win32
     * program window also live: copy it to the heap now; mm_release_initrd() frees
     * the copy once its files are unpacked */
    if (mod_end > mod_start) {
        initrd_size = mod_end - mod_start;
        initrd_data = kmalloc(initrd_size);
        if (initrd_data) memcpy(initrd_data, (void *)(uintptr_t)mod_start, initrd_size);
    }
    (void)kernel_end;
}
