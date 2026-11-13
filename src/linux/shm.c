/* Shared memory for Linux programs: memfd_create, shm_open (/dev/shm) and
 * MAP_SHARED | MAP_ANONYMOUS.
 *
 * A shared object is a reference-counted array of physical pages. Mapping it
 * MAP_SHARED puts those same pages in the process's page tables (marked
 * PTE_SHARED, so the address space never frees them), and fork() keeps the
 * mapping shared; each mapping, open descriptor and /dev/shm name holds a
 * reference, and the pages are freed with the last one. This is what lets a
 * browser engine's processes hand rendered tiles and IPC buffers to each other. */
#ifdef __x86_64__
#include "kernel.h"
#include "lx.h"

static shmobj_t *named;                         /* /dev/shm/<name> objects */

shmobj_t *shm_new(const char *name) {
    shmobj_t *o = kzalloc(sizeof *o);
    if (!o) return NULL;
    o->refs = 1;
    if (name) strlcpy(o->name, name, sizeof o->name);
    return o;
}
void shm_ref(shmobj_t *o) { if (o) o->refs++; }
void shm_unref(shmobj_t *o) {
    if (!o || --o->refs > 0) return;
    for (uint64_t i = 0; i < o->npages; i++) if (o->pages[i]) pmm_free(o->pages[i]);
    kfree(o->pages);
    kfree(o);
}
/* resize to `size` bytes; pages past the end are kept until the object goes, because
 * another process may still have them mapped */
int shm_resize(shmobj_t *o, uint64_t size) {
    if (size > (1ull << 36)) return -EFBIG;
    if ((o->seals & SHM_SEAL_SHRINK) && size < o->size) return -EPERM;
    if ((o->seals & SHM_SEAL_GROW) && size > o->size) return -EPERM;
    uint64_t need = PALIGN(size) / PAGE;
    if (need > o->npages) {
        uint8_t **np = krealloc(o->pages, sizeof(uint8_t *) * need);
        if (!np) return -ENOMEM;
        memset(np + o->npages, 0, sizeof(uint8_t *) * (need - o->npages));
        o->pages = np; o->npages = need;
    }
    if (size < o->size) {                             /* bytes past the new end read as zero later */
        uint64_t first = PALIGN(size) / PAGE;
        if (size & (PAGE - 1)) { uint8_t *pg = o->pages[size / PAGE]; if (pg) memset(pg + (size & (PAGE - 1)), 0, PAGE - (size & (PAGE - 1))); }
        for (uint64_t i = first; i < o->npages && i < PALIGN(o->size) / PAGE; i++) if (o->pages[i]) memset(o->pages[i], 0, PAGE);
    }
    o->size = size;
    return 0;
}
uint8_t *shm_page(shmobj_t *o, uint64_t idx, bool create) {
    if (idx >= o->npages) return NULL;
    if (!o->pages[idx] && create) o->pages[idx] = pmm_alloc();    /* zeroed */
    return o->pages[idx];
}
/* read or write through the descriptor (read(2), write(2), pread, pwrite) */
int64_t shm_rw(shmobj_t *o, uint8_t *buf, uint64_t n, uint64_t off, bool write) {
    if (write) {
        if (o->seals & SHM_SEAL_WRITE) return -EPERM;
        if (off + n > o->size) { int r = shm_resize(o, off + n); if (r) return r; }
    } else {
        if (off >= o->size) return 0;
        n = MIN(n, o->size - off);
    }
    uint64_t done = 0;
    while (done < n) {
        uint64_t at = off + done, k = MIN(n - done, PAGE - (at & (PAGE - 1)));
        uint8_t *pg = shm_page(o, at / PAGE, write);
        if (write) { if (!pg) return done ? (int64_t)done : -ENOMEM; memcpy(pg + (at & (PAGE - 1)), buf + done, k); }
        else if (pg) memcpy(buf + done, pg + (at & (PAGE - 1)), k);
        else memset(buf + done, 0, k);
        done += k;
    }
    return (int64_t)done;
}

/* ---- /dev/shm ---- */
shmobj_t *shm_lookup(const char *name) {
    for (shmobj_t *o = named; o; o = o->next) if (!strcmp(o->name, name)) { shm_ref(o); return o; }
    return NULL;
}
shmobj_t *shm_create_named(const char *name) {
    shmobj_t *o = shm_new(name);
    if (!o) return NULL;
    o->is_named = true;
    o->next = named; named = o;                       /* the name holds the first reference */
    shm_ref(o);                                        /* and the caller one more */
    return o;
}
int shm_unlink_name(const char *name) {
    for (shmobj_t **pp = &named; *pp; pp = &(*pp)->next) {
        shmobj_t *o = *pp;
        if (strcmp(o->name, name)) continue;
        *pp = o->next; o->is_named = false;
        shm_unref(o);
        return 0;
    }
    return -ENOENT;
}
#endif
