/* User address spaces for Linux processes: 4-level page tables whose slot 0 is
 * the kernel's identity map, a sorted list of mappings (VMAs), and demand
 * paging: a page is allocated, zeroed or filled from its file, on first touch. */
#ifdef __x86_64__
#include "lx.h"

#define PTE_P   0x001ull
#define PTE_W   0x002ull
#define PTE_U   0x004ull
#define PTE_NX  (1ull << 63)
#define ADDR(e) ((e) & 0x000FFFFFFFFFF000ull)

extern uint64_t boot_pml4[];

uvm_t *uvm_new(void) {
    uvm_t *vm = kzalloc(sizeof *vm);
    if (!vm) return NULL;
    vm->pml4 = pmm_alloc();
    if (!vm->pml4) { kfree(vm); return NULL; }
    vm->pml4[0] = boot_pml4[0];                 /* the kernel's 0..512 GiB (identity map of the low 4 GiB) */
    return vm;
}
uintptr_t uvm_cr3(uvm_t *vm) { return (uintptr_t)vm->pml4; }

static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }

/* the PTE for va, creating the tables on the way if asked */
static uint64_t *pte_of(uvm_t *vm, uint64_t va, bool create) {
    uint64_t *t = vm->pml4;
    int shifts[3] = { 39, 30, 21 };
    for (int l = 0; l < 3; l++) {
        uint64_t *e = &t[(va >> shifts[l]) & 511];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t *nt = pmm_alloc();
            if (!nt) return NULL;
            *e = (uint64_t)(uintptr_t)nt | PTE_P | PTE_W | PTE_U;
        }
        t = (uint64_t *)(uintptr_t)ADDR(*e);
    }
    return &t[(va >> 12) & 511];
}
uint64_t *lx_pte_lookup(uvm_t *vm, uint64_t va) { uint64_t *e = pte_of(vm, va, false); return e && (*e & PTE_P) ? e : NULL; }
static uint64_t pte_flags(int prot) {
    uint64_t f = PTE_P | PTE_U;
    if (prot & VM_WRITE) f |= PTE_W;
    if (!(prot & VM_EXEC)) f |= PTE_NX;
    return f;
}

static void free_range_pages(uvm_t *vm, uint64_t start, uint64_t end) {
    for (uint64_t va = start; va < end; va += PAGE) {
        uint64_t *pte = pte_of(vm, va, false);
        if (!pte) { va = (va | 0x1FFFFF); continue; }          /* no page table: skip its 2 MiB */
        if (*pte & PTE_P) { pmm_free((void *)(uintptr_t)ADDR(*pte)); vm->pages--; *pte = 0; invlpg(va); }
    }
}
static void free_tables(uint64_t *t, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_P)) continue;
        if (level > 1) free_tables((uint64_t *)(uintptr_t)ADDR(t[i]), level - 1);
        else pmm_free((void *)(uintptr_t)ADDR(t[i]));
    }
    pmm_free(t);
}
void uvm_free(uvm_t *vm) {
    if (!vm) return;
    for (int i = 1; i < 256; i++) if (vm->pml4[i] & PTE_P) free_tables((uint64_t *)(uintptr_t)ADDR(vm->pml4[i]), 3);
    pmm_free(vm->pml4);
    while (vm->vmas) { vma_t *n = vm->vmas->next; kfree(vm->vmas); vm->vmas = n; }
    kfree(vm);
}

vma_t *uvm_find(uvm_t *vm, uint64_t addr) {
    for (vma_t *v = vm->vmas; v && v->start <= addr; v = v->next) if (addr < v->end) return v;
    return NULL;
}
static void insert_vma(uvm_t *vm, vma_t *n) {
    vma_t **pp = &vm->vmas;
    while (*pp && (*pp)->start < n->start) pp = &(*pp)->next;
    n->next = *pp; *pp = n;
}
/* split v at addr (strictly inside it); v keeps the lower part */
static void split_vma(vma_t *v, uint64_t addr) {
    vma_t *n = kmalloc(sizeof *n);
    *n = *v;
    n->start = addr;
    if (n->fdata) n->foff += addr - v->start;
    v->end = addr;
    v->next = n;
}
uint64_t uvm_find_free(uvm_t *vm, uint64_t len, uint64_t below) {
    uint64_t end = below;
    for (int guard = 0; guard < 100000; guard++) {
        if (end < USER_LO + len) return 0;
        uint64_t start = end - len;
        vma_t *hit = NULL;
        for (vma_t *v = vm->vmas; v; v = v->next) if (v->start < end && v->end > start) hit = v;   /* highest overlap */
        if (!hit) return start;
        end = hit->start;
    }
    return 0;
}
int uvm_unmap(uvm_t *vm, uint64_t start, uint64_t len) {
    uint64_t end = PALIGN(start + len);
    start &= ~(PAGE - 1);
    if (end <= start) return 0;
    for (vma_t *v = vm->vmas; v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;
        if (v->start < start) { split_vma(v, start); continue; }      /* the next iteration sees the upper part */
        if (v->end > end) split_vma(v, end);
    }
    vma_t **pp = &vm->vmas;
    while (*pp) {
        vma_t *v = *pp;
        if (v->start >= start && v->end <= end) { *pp = v->next; free_range_pages(vm, v->start, v->end); kfree(v); }
        else pp = &v->next;
    }
    return 0;
}
int64_t uvm_map(uvm_t *vm, uint64_t hint, uint64_t len, int prot, int flags, bool fixed,
                const uint8_t *fdata, uint64_t fsize, uint64_t foff, vnode_t *fnode) {
    len = PALIGN(len);
    if (!len) return -EINVAL;
    uint64_t start;
    if (fixed) {
        if ((hint & (PAGE - 1)) || hint < USER_LO || hint + len > USER_HI || hint + len < hint) return -EINVAL;
        uvm_unmap(vm, hint, len);
        start = hint;
    } else {
        start = 0;
        hint &= ~(PAGE - 1);
        if (hint >= USER_LO && hint + len <= USER_HI) {
            bool free_ = true;
            for (vma_t *v = vm->vmas; v; v = v->next) if (v->start < hint + len && v->end > hint) { free_ = false; break; }
            if (free_) start = hint;
        }
        if (!start) start = uvm_find_free(vm, len, MMAP_TOP);
        if (!start) return -ENOMEM;
    }
    vma_t *n = kzalloc(sizeof *n);
    if (!n) return -ENOMEM;
    n->start = start; n->end = start + len; n->prot = prot; n->flags = flags;
    n->fdata = fdata; n->fsize = fsize; n->foff = foff; n->fnode = fnode;
    insert_vma(vm, n);
    return (int64_t)start;
}
int uvm_protect(uvm_t *vm, uint64_t start, uint64_t len, int prot) {
    uint64_t end = PALIGN(start + len);
    if (start & (PAGE - 1)) return -EINVAL;
    /* every page must be mapped */
    for (uint64_t a = start; a < end;) { vma_t *v = uvm_find(vm, a); if (!v) return -ENOMEM; a = v->end; }
    for (vma_t *v = vm->vmas; v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;
        if (v->start < start) { split_vma(v, start); continue; }
        if (v->end > end) split_vma(v, end);
        v->prot = prot;
        for (uint64_t va = v->start; va < v->end; va += PAGE) {
            uint64_t *pte = pte_of(vm, va, false);
            if (!pte) { va |= 0x1FFFFF; continue; }
            if (*pte & PTE_P) { *pte = ADDR(*pte) | pte_flags(prot); invlpg(va); }
        }
    }
    return 0;
}
bool uvm_fault(uvm_t *vm, uint64_t addr, bool write) {
    if (addr < USER_LO || addr >= USER_HI) return false;
    vma_t *v = uvm_find(vm, addr);
    if (!v || !v->prot) return false;
    if (write && !(v->prot & VM_WRITE)) return false;
    uint64_t va = addr & ~(PAGE - 1);
    uint64_t *pte = pte_of(vm, va, true);
    if (!pte) return false;
    if (*pte & PTE_P) return !write || (*pte & PTE_W);         /* another thread got there first */
    uint8_t *pg = pmm_alloc();
    if (!pg) { klog("linux: out of memory for a page at %llx", (unsigned long long)va); return false; }
    if (v->fdata) {
        uint64_t off = v->foff + (va - v->start);
        if (off < v->fsize) memcpy(pg, v->fdata + off, MIN(PAGE, v->fsize - off));
    }
    *pte = (uint64_t)(uintptr_t)pg | pte_flags(v->prot);
    vm->pages++;
    return true;
}
bool uvm_check(uvm_t *vm, uint64_t addr, uint64_t len, bool write) {
    if (!len) return true;
    if (addr < USER_LO || addr + len > USER_HI || addr + len < addr) return false;
    uint64_t end = addr + len;
    while (addr < end) {
        vma_t *v = uvm_find(vm, addr);
        if (!v || !(v->prot & VM_READ) || (write && !(v->prot & VM_WRITE))) return false;
        addr = v->end;
    }
    return true;
}
#endif
