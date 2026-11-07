/* Read/write FAT32 driver for installed systems: format, mount, long file
 * names, whole-file create/replace, recursive delete, rename/move. The FAT is
 * cached in memory and written back on fat32_sync(). */
#include "kernel.h"
#include "disk.h"

typedef struct PACKED {
    uint8_t jmp[3]; char oem[8];
    uint16_t bps; uint8_t spc; uint16_t reserved; uint8_t nfats; uint16_t root_entries;
    uint16_t total16; uint8_t media; uint16_t fatsz16; uint16_t spt, heads; uint32_t hidden, total32;
    uint32_t fatsz32; uint16_t flags, ver; uint32_t root_clus; uint16_t fsinfo, backup; uint8_t r[12];
    uint8_t drv, r1, sig; uint32_t serial; char label[11]; char fstype[8];
} bpb32_t;
typedef struct PACKED { uint8_t name[11]; uint8_t attr, ntres, ctime_tenth; uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus_lo; uint32_t size; } de_t;

#define A_RO 0x01
#define A_HIDDEN 0x02
#define A_SYSTEM 0x04
#define A_LABEL 0x08
#define A_DIR 0x10
#define A_ARCH 0x20
#define A_LFN 0x0F
#define EOC 0x0FFFFFFF

struct fatvol {
    blkdev_t *dev;
    uint32_t lba, sectors, spc, csz, reserved, nfats, fatsz, root_clus, nclus, data_lba, fsinfo;
    uint32_t *fat;
    uint8_t *dirty;
    uint32_t free_count, hint, serial;
    char label[12];
    uint8_t *tmp;      /* one cluster */
};

/* ---------------- FAT table ---------------- */
static uint32_t fget(fatvol_t *v, uint32_t c) { return c < v->nclus + 2 ? v->fat[c] & 0x0FFFFFFF : EOC; }
static void fset(fatvol_t *v, uint32_t c, uint32_t val) {
    if (c >= v->nclus + 2) return;
    v->fat[c] = (v->fat[c] & 0xF0000000) | (val & 0x0FFFFFFF);
    v->dirty[c / 128] = 1;
}
static bool is_eoc(uint32_t c) { return c >= 0x0FFFFFF8 || c < 2; }
static uint32_t alloc_clus(fatvol_t *v, uint32_t prev) {
    for (uint32_t k = 0; k < v->nclus; k++) {
        uint32_t c = 2 + (v->hint - 2 + k) % v->nclus;
        if (!fget(v, c)) {
            fset(v, c, EOC);
            if (prev) fset(v, prev, c);
            v->hint = c + 1 >= v->nclus + 2 ? 2 : c + 1;
            if (v->free_count) v->free_count--;
            return c;
        }
    }
    return 0;
}
static void free_chain(fatvol_t *v, uint32_t c) {
    for (uint32_t n = 0; !is_eoc(c) && n < v->nclus; n++) {
        uint32_t nx = fget(v, c);
        fset(v, c, 0); v->free_count++;
        if (c < v->hint) v->hint = c;
        c = nx;
    }
}
static uint32_t clus_lba(fatvol_t *v, uint32_t c) { return v->data_lba + (c - 2) * v->spc; }
void fat32_sync(fatvol_t *v) {
    if (!v) return;
    for (uint32_t s = 0; s < v->fatsz;) {
        if (!v->dirty[s]) { s++; continue; }
        uint32_t e = s; while (e < v->fatsz && v->dirty[e] && e - s < 128) e++;
        for (uint32_t f = 0; f < v->nfats; f++) blk_write(v->dev, v->lba + v->reserved + f * v->fatsz + s, e - s, (uint8_t *)v->fat + s * 512);
        memset(v->dirty + s, 0, e - s);
        s = e;
    }
    uint8_t fs[512];
    if (blk_read(v->dev, v->lba + v->fsinfo, 1, fs) && *(uint32_t *)fs == 0x41615252) {
        *(uint32_t *)(fs + 488) = v->free_count; *(uint32_t *)(fs + 492) = v->hint;
        blk_write(v->dev, v->lba + v->fsinfo, 1, fs);
    }
}

/* ---------------- cluster chains ---------------- */
static uint32_t chain_len(fatvol_t *v, uint32_t c) { uint32_t n = 0; while (!is_eoc(c) && n <= v->nclus) { n++; c = fget(v, c); } return n; }
static bool read_chain(fatvol_t *v, uint32_t c, uint8_t *buf, uint32_t bytes) {
    uint32_t off = 0;
    while (off < bytes && !is_eoc(c)) {
        uint32_t run = 1;
        while (fget(v, c + run - 1) == c + run && off + run * v->csz < bytes && run < 64) run++;
        uint32_t want = MIN(run * v->csz, bytes - off);
        uint32_t full = want / 512, tail = want % 512;
        if (full && !blk_read(v->dev, clus_lba(v, c), full, buf + off)) return false;
        if (tail) { if (!blk_read(v->dev, clus_lba(v, c) + full, 1, v->tmp)) return false; memcpy(buf + off + full * 512, v->tmp, tail); }
        off += want;
        uint32_t last = c + run - 1;
        c = fget(v, last);
    }
    return off >= bytes;
}
/* allocate a fresh chain for `bytes` of data and write it; returns first cluster (0 for empty) */
static uint32_t write_new_chain(fatvol_t *v, const uint8_t *data, uint32_t bytes, bool *ok) {
    *ok = true;
    if (!bytes) return 0;
    uint32_t n = (bytes + v->csz - 1) / v->csz;
    if (n > v->free_count) { *ok = false; return 0; }
    uint32_t first = 0, prev = 0, run_start = 0, run_len = 0, off = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = alloc_clus(v, prev);
        if (!c) { *ok = false; if (first) free_chain(v, first); return 0; }
        if (!first) first = c;
        if (run_len && c == run_start + run_len && run_len < 128 / v->spc) run_len++;
        else {
            if (run_len) {
                uint32_t want = MIN(run_len * v->csz, bytes - off), full = want / 512, tail = want % 512;
                if (full) blk_write(v->dev, clus_lba(v, run_start), full, data + off);
                if (tail) { memset(v->tmp, 0, 512); memcpy(v->tmp, data + off + full * 512, tail); blk_write(v->dev, clus_lba(v, run_start) + full, 1, v->tmp); }
                off += want;
            }
            run_start = c; run_len = 1;
        }
        prev = c;
    }
    uint32_t want = bytes - off, full = want / 512, tail = want % 512;
    if (full && !blk_write(v->dev, clus_lba(v, run_start), full, data + off)) *ok = false;
    if (tail) { memset(v->tmp, 0, 512); memcpy(v->tmp, data + off + full * 512, tail); if (!blk_write(v->dev, clus_lba(v, run_start) + full, 1, v->tmp)) *ok = false; }
    return first;
}

/* ---------------- directories ---------------- */
typedef struct { uint8_t *buf; uint32_t bytes, clus; } dir_t;
static bool dir_load(fatvol_t *v, uint32_t clus, dir_t *d) {
    if (!clus) clus = v->root_clus;
    d->clus = clus;
    uint32_t n = chain_len(v, clus);
    if (!n || n > 4096) return false;
    d->bytes = n * v->csz;
    d->buf = kmalloc(d->bytes);
    if (!read_chain(v, clus, d->buf, d->bytes)) { kfree(d->buf); return false; }
    return true;
}
static bool dir_store(fatvol_t *v, dir_t *d) {
    uint32_t c = d->clus, prev = 0;
    for (uint32_t off = 0; off < d->bytes; off += v->csz) {
        if (is_eoc(c)) { c = alloc_clus(v, prev); if (!c) return false; }
        if (!blk_write(v->dev, clus_lba(v, c), v->spc, d->buf + off)) return false;
        prev = c; c = fget(v, c);
    }
    return true;
}
static void dir_grow(fatvol_t *v, dir_t *d) {
    d->buf = krealloc(d->buf, d->bytes + v->csz);
    memset(d->buf + d->bytes, 0, v->csz);
    d->bytes += v->csz;
}
static void dir_free(dir_t *d) { kfree(d->buf); d->buf = NULL; }
static uint32_t de_clus(const de_t *e) { return ((uint32_t)e->clus_hi << 16) | e->clus_lo; }
static void de_set_clus(de_t *e, uint32_t c) { e->clus_hi = c >> 16; e->clus_lo = c & 0xFFFF; }
static uint8_t lfn_sum(const uint8_t *n) { uint8_t s = 0; for (int i = 0; i < 11; i++) s = ((s & 1) << 7) + (s >> 1) + n[i]; return s; }
static const int lfn_offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void sfn_to_str(const de_t *e, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) { char c = e->name[i]; if (i == 0 && (uint8_t)c == 0x05) c = (char)0xE5; out[k++] = (e->ntres & 0x08) ? tolower(c) : c; }
    if (e->name[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && e->name[i] != ' '; i++) out[k++] = (e->ntres & 0x10) ? tolower(e->name[i]) : e->name[i]; }
    out[k] = 0;
}
/* iterate entries: calls cb(entry index, first index of its LFN run, long name) */
typedef bool (*dir_cb)(de_t *e, int idx, int first, const char *name, void *ctx);
static void dir_iter(dir_t *d, dir_cb cb, void *ctx) {
    char lfn[256]; int lfn_first = -1; uint8_t lfn_ck = 0; bool lfn_ok = false;
    int n = d->bytes / 32;
    for (int i = 0; i < n; i++) {
        de_t *e = (de_t *)(d->buf + i * 32);
        if (!e->name[0]) break;
        if (e->name[0] == 0xE5) { lfn_ok = false; continue; }
        if (e->attr == A_LFN) {
            const uint8_t *r = (const uint8_t *)e;
            int seq = r[0] & 0x1F;
            if (r[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_first = i; lfn_ck = r[13]; lfn_ok = true; }
            if (!lfn_ok || r[13] != lfn_ck || seq < 1 || seq > 19) { lfn_ok = false; continue; }
            for (int k = 0; k < 13; k++) {
                uint16_t ch = r[lfn_offs[k]] | (r[lfn_offs[k] + 1] << 8);
                int pos = (seq - 1) * 13 + k;
                if (ch == 0 || ch == 0xFFFF || pos >= 255) break;
                lfn[pos] = ch < 128 ? (char)ch : '_';
            }
            continue;
        }
        if (e->attr & A_LABEL) { lfn_ok = false; continue; }
        char name[256];
        int first = i;
        if (lfn_ok && lfn_sum(e->name) == lfn_ck && lfn[0]) { strlcpy(name, lfn, sizeof name); first = lfn_first; }
        else sfn_to_str(e, name);
        lfn_ok = false;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!cb(e, i, first, name, ctx)) return;
    }
}
typedef struct { const char *name; int idx, first; } find_ctx_t;
static bool find_cb(de_t *e, int idx, int first, const char *name, void *ctx) {
    find_ctx_t *f = ctx;
    if (!strcasecmp(name, f->name)) { f->idx = idx; f->first = first; return false; }
    return true;
}
static de_t *dir_find(dir_t *d, const char *name, int *first) {
    find_ctx_t f = { name, -1, -1 };
    dir_iter(d, find_cb, &f);
    if (f.idx < 0) return NULL;
    if (first) *first = f.first;
    return (de_t *)(d->buf + f.idx * 32);
}
static void dos_now(uint16_t *date, uint16_t *time) {
    rtc_time_t t; rtc_now(&t);
    *date = ((MAX(t.year, 1980) - 1980) << 9) | (t.month << 5) | t.day;
    *time = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
}
static void de_stamp(de_t *e) { uint16_t d, t; dos_now(&d, &t); e->mdate = d; e->mtime = t; }
static bool sfn_taken(dir_t *d, const uint8_t *s) {
    for (uint32_t i = 0; i < d->bytes / 32; i++) {
        de_t *e = (de_t *)(d->buf + i * 32);
        if (!e->name[0]) break;
        if (e->name[0] == 0xE5 || e->attr == A_LFN) continue;
        if (!memcmp(e->name, s, 11)) return true;
    }
    return false;
}
/* builds an 8.3 alias; returns true if a long-name entry is required */
static bool make_sfn(dir_t *d, const char *name, uint8_t sfn[11]) {
    static const char *bad = "\"*+,/:;<=>?[\\]|";
    memset(sfn, ' ', 11);
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    bool lossy = false, lower = false;
    int nb = 0, ne = 0;
    for (const char *p = name; *p && (!dot || p < dot); p++) {
        char c = *p;
        if (c == ' ' || c == '.') { lossy = true; continue; }
        if ((uint8_t)c >= 128 || strchr(bad, c)) { c = '_'; lossy = true; }
        if (c >= 'a' && c <= 'z') lower = true;
        if (nb < 8) sfn[nb++] = toupper(c); else lossy = true;
    }
    if (dot) for (const char *p = dot + 1; *p; p++) {
        char c = *p;
        if (c == ' ' || c == '.') { lossy = true; continue; }
        if ((uint8_t)c >= 128 || strchr(bad, c)) { c = '_'; lossy = true; }
        if (c >= 'a' && c <= 'z') lower = true;
        if (ne < 3) sfn[8 + ne++] = toupper(c); else lossy = true;
    }
    if (!nb) { sfn[0] = '_'; nb = 1; lossy = true; }
    if (!lossy && !sfn_taken(d, sfn)) return lower;
    for (int n = 1; n < 1000000; n++) {
        char tail[12]; snprintf(tail, sizeof tail, "~%d", n);
        int tl = strlen(tail), keep = MIN(nb, 8 - tl);
        uint8_t t[11]; memcpy(t, sfn, 11);
        for (int i = 0; i < tl; i++) t[keep + i] = tail[i];
        for (int i = keep + tl; i < 8; i++) t[i] = ' ';
        if (!sfn_taken(d, t)) { memcpy(sfn, t, 11); return true; }
    }
    return true;
}
/* adds a new entry and returns it (pointer into d->buf, valid until the next grow) */
static de_t *dir_add(fatvol_t *v, dir_t *d, const char *name, uint8_t attr, uint32_t clus, uint32_t size) {
    uint8_t sfn[11];
    bool need_lfn = make_sfn(d, name, sfn);
    int len = strlen(name);
    if (len > 255) return NULL;
    int nl = need_lfn ? (len + 12) / 13 : 0, need = nl + 1;
    int start = -1;
    for (;;) {
        int n = d->bytes / 32, run = 0;
        for (int i = 0; i < n; i++) {
            uint8_t b = d->buf[i * 32];
            if (b == 0 || b == 0xE5) { if (++run == need) { start = i - need + 1; break; } }
            else run = 0;
        }
        if (start >= 0) break;
        if (d->bytes >= 2 * 1024 * 1024) return NULL;
        dir_grow(v, d);
    }
    uint8_t ck = lfn_sum(sfn);
    for (int k = 0; k < nl; k++) {
        uint8_t *r = d->buf + (start + k) * 32;
        int seq = nl - k;
        memset(r, 0, 32);
        r[0] = seq | (k == 0 ? 0x40 : 0); r[11] = A_LFN; r[13] = ck;
        for (int j = 0; j < 13; j++) {
            int pos = (seq - 1) * 13 + j;
            uint16_t ch = pos < len ? (uint8_t)name[pos] : pos == len ? 0 : 0xFFFF;
            r[lfn_offs[j]] = ch & 0xFF; r[lfn_offs[j] + 1] = ch >> 8;
        }
    }
    de_t *e = (de_t *)(d->buf + (start + nl) * 32);
    memset(e, 0, 32);
    memcpy(e->name, sfn, 11);
    e->attr = attr; de_set_clus(e, clus); e->size = size;
    de_stamp(e); e->cdate = e->adate = e->mdate; e->ctime = e->mtime;
    return e;
}
static void dir_remove(dir_t *d, int first, int idx) { for (int i = first; i <= idx; i++) d->buf[i * 32] = 0xE5; }

/* ---------------- paths ---------------- */
static int split(const char *path, char parts[][128], int max) {
    int n = 0;
    while (*path && n < max) {
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;
        int k = 0;
        while (*path && *path != '\\' && *path != '/') { if (k < 127) parts[n][k++] = *path; path++; }
        parts[n][k] = 0;
        if (k && strcmp(parts[n], ".")) n++;
    }
    return n;
}
/* walks `count` components; returns the cluster of the final directory (root_clus for root), 0 on failure */
static uint32_t walk_dirs(fatvol_t *v, char parts[][128], int count, bool create) {
    uint32_t clus = v->root_clus;
    for (int i = 0; i < count; i++) {
        dir_t d;
        if (!dir_load(v, clus, &d)) return 0;
        de_t *e = dir_find(&d, parts[i], NULL);
        if (e) {
            if (!(e->attr & A_DIR)) { dir_free(&d); return 0; }
            uint32_t c = de_clus(e);
            dir_free(&d);
            clus = c ? c : v->root_clus;
            continue;
        }
        if (!create) { dir_free(&d); return 0; }
        uint32_t nc = alloc_clus(v, 0);
        if (!nc) { dir_free(&d); return 0; }
        memset(v->tmp, 0, v->csz);
        de_t *dot = (de_t *)v->tmp, *dotdot = dot + 1;
        memset(dot->name, ' ', 11); dot->name[0] = '.'; dot->attr = A_DIR; de_set_clus(dot, nc);
        memset(dotdot->name, ' ', 11); dotdot->name[0] = dotdot->name[1] = '.'; dotdot->attr = A_DIR; de_set_clus(dotdot, clus == v->root_clus ? 0 : clus);
        de_stamp(dot); dotdot->mdate = dot->mdate; dotdot->mtime = dot->mtime;
        blk_write(v->dev, clus_lba(v, nc), v->spc, v->tmp);
        if (!dir_add(v, &d, parts[i], A_DIR, nc, 0) || !dir_store(v, &d)) { dir_free(&d); return 0; }
        dir_free(&d);
        clus = nc;
    }
    return clus;
}

/* ---------------- public API ---------------- */
bool fat32_mkdir(fatvol_t *v, const char *path) {
    char parts[32][128]; int n = split(path, parts, 32);
    return walk_dirs(v, parts, n, true) != 0;
}
bool fat32_exists(fatvol_t *v, const char *path) {
    char parts[32][128]; int n = split(path, parts, 32);
    if (!n) return true;
    uint32_t pc = walk_dirs(v, parts, n - 1, false);
    if (!pc) return false;
    dir_t d; if (!dir_load(v, pc, &d)) return false;
    bool r = dir_find(&d, parts[n - 1], NULL) != NULL;
    dir_free(&d);
    return r;
}
bool fat32_write(fatvol_t *v, const char *path, const void *data, uint32_t len, bool readonly) {
    char parts[32][128]; int n = split(path, parts, 32);
    if (!n) return false;
    uint32_t pc = walk_dirs(v, parts, n - 1, true);
    if (!pc) return false;
    dir_t d; if (!dir_load(v, pc, &d)) return false;
    int first;
    de_t *e = dir_find(&d, parts[n - 1], &first);
    if (e && (e->attr & A_DIR)) { dir_free(&d); return false; }
    if (e) { free_chain(v, de_clus(e)); de_set_clus(e, 0); e->size = 0; }
    bool ok;
    uint32_t c = write_new_chain(v, data, len, &ok);
    if (!ok) { dir_free(&d); return false; }
    if (!e) e = dir_add(v, &d, parts[n - 1], A_ARCH, 0, 0);
    if (!e) { free_chain(v, c); dir_free(&d); return false; }
    de_set_clus(e, c); e->size = len;
    e->attr = (e->attr & ~A_RO) | A_ARCH | (readonly ? A_RO : 0);
    de_stamp(e); e->adate = e->mdate;
    bool r = dir_store(v, &d);
    dir_free(&d);
    return r;
}
uint8_t *fat32_read(fatvol_t *v, const char *path, uint32_t *len) {
    char parts[32][128]; int n = split(path, parts, 32);
    if (!n) return NULL;
    uint32_t pc = walk_dirs(v, parts, n - 1, false);
    if (!pc) return NULL;
    dir_t d; if (!dir_load(v, pc, &d)) return NULL;
    de_t *e = dir_find(&d, parts[n - 1], NULL);
    uint8_t *buf = NULL;
    if (e && !(e->attr & A_DIR)) {
        buf = kmalloc(e->size + 1);
        if (e->size && !read_chain(v, de_clus(e), buf, e->size)) { kfree(buf); buf = NULL; }
        else { buf[e->size] = 0; if (len) *len = e->size; }
    }
    dir_free(&d);
    return buf;
}
static bool del_cb(de_t *e, int idx, int first, const char *name, void *ctx);
static void delete_tree(fatvol_t *v, uint32_t clus) {
    dir_t d;
    if (!clus || !dir_load(v, clus, &d)) return;
    dir_iter(&d, del_cb, v);
    dir_free(&d);
}
static bool del_cb(de_t *e, int idx, int first, const char *name, void *ctx) {
    fatvol_t *v = ctx;
    if (e->attr & A_DIR) delete_tree(v, de_clus(e));
    free_chain(v, de_clus(e));
    return true;
}
bool fat32_delete(fatvol_t *v, const char *path) {
    char parts[32][128]; int n = split(path, parts, 32);
    if (!n) return false;
    uint32_t pc = walk_dirs(v, parts, n - 1, false);
    if (!pc) return false;
    dir_t d; if (!dir_load(v, pc, &d)) return false;
    int first;
    de_t *e = dir_find(&d, parts[n - 1], &first);
    if (!e) { dir_free(&d); return false; }
    int idx = ((uint8_t *)e - d.buf) / 32;
    if (e->attr & A_DIR) delete_tree(v, de_clus(e));
    free_chain(v, de_clus(e));
    dir_remove(&d, first, idx);
    bool r = dir_store(v, &d);
    dir_free(&d);
    return r;
}
bool fat32_rename(fatvol_t *v, const char *from, const char *to) {
    char pa[32][128], pb[32][128];
    int na = split(from, pa, 32), nb = split(to, pb, 32);
    if (!na || !nb) return false;
    uint32_t ca = walk_dirs(v, pa, na - 1, false);
    if (!ca) return false;
    dir_t d; if (!dir_load(v, ca, &d)) return false;
    int first;
    de_t *e = dir_find(&d, pa[na - 1], &first);
    if (!e) { dir_free(&d); return false; }
    de_t saved = *e;
    int idx = ((uint8_t *)e - d.buf) / 32;
    dir_remove(&d, first, idx);
    if (!dir_store(v, &d)) { dir_free(&d); return false; }
    dir_free(&d);
    uint32_t cb = walk_dirs(v, pb, nb - 1, true);
    if (!cb || !dir_load(v, cb, &d)) return false;
    int f2; de_t *old = dir_find(&d, pb[nb - 1], &f2);
    if (old) {   /* replace an existing target */
        int i2 = ((uint8_t *)old - d.buf) / 32;
        if (old->attr & A_DIR) delete_tree(v, de_clus(old));
        free_chain(v, de_clus(old));
        dir_remove(&d, f2, i2);
    }
    de_t *ne = dir_add(v, &d, pb[nb - 1], saved.attr, de_clus(&saved), saved.size);
    if (!ne) { dir_free(&d); return false; }
    ne->mdate = saved.mdate; ne->mtime = saved.mtime; ne->cdate = saved.cdate; ne->ctime = saved.ctime;
    bool r = dir_store(v, &d);
    dir_free(&d);
    if (r && (saved.attr & A_DIR) && ca != cb && de_clus(&saved)) {
        dir_t sub;
        if (dir_load(v, de_clus(&saved), &sub)) {
            de_t *dd = (de_t *)sub.buf + 1;
            if (dd->name[0] == '.' && dd->name[1] == '.') { de_set_clus(dd, cb == v->root_clus ? 0 : cb); dir_store(v, &sub); }
            dir_free(&sub);
        }
    }
    return r;
}

/* ---------------- loading into the VFS ---------------- */
typedef struct { fatvol_t *v; vnode_t *parent; int files, depth; } load_ctx_t;
extern void (*vol_load_progress)(uint64_t done, uint64_t total);
static uint64_t load_done, load_total;
static uint32_t load_last_tick;
static bool load_cb(de_t *e, int idx, int first, const char *name, void *ctx) {
    load_ctx_t *L = ctx;
    fatvol_t *v = L->v;
    vnode_t *n = vfs_lookup_child(L->parent, name);
    bool dir = (e->attr & A_DIR) != 0;
    if (n && n->dir != dir) n = NULL;
    if (!n) n = vfs_new_node(L->parent, name, dir);
    n->mtime.year = 1980 + (e->mdate >> 9); n->mtime.month = (e->mdate >> 5) & 15; n->mtime.day = e->mdate & 31;
    n->mtime.hour = e->mtime >> 11; n->mtime.minute = (e->mtime >> 5) & 63; n->mtime.second = (e->mtime & 31) * 2;
    if (dir) {
        if (L->depth < 24 && de_clus(e)) {
            dir_t d;
            if (dir_load(v, de_clus(e), &d)) {
                load_ctx_t sub = { v, n, 0, L->depth + 1 };
                dir_iter(&d, load_cb, &sub);
                L->files += sub.files;
                dir_free(&d);
            }
        }
    } else {
        uint8_t *buf = kmalloc(e->size + 1);
        if (e->size && !read_chain(v, de_clus(e), buf, e->size)) { klog("fat32: read failed for %s", name); e->size = 0; }
        kfree(n->data);
        n->data = buf; n->size = e->size; n->cap = e->size + 1;
        n->readonly = (e->attr & A_RO) != 0;
        L->files++;
        load_done += (e->size + v->csz - 1) / v->csz * v->csz;
        if (vol_load_progress && timer_ticks() - load_last_tick >= 50) { load_last_tick = timer_ticks(); vol_load_progress(load_done, load_total); }
    }
    return true;
}
int fat32_load_tree(fatvol_t *v, vnode_t *root) {
    dir_t d;
    if (!dir_load(v, v->root_clus, &d)) return -1;
    load_ctx_t L = { v, root, 0, 0 };
    load_done = 0; load_total = (uint64_t)(v->nclus - v->free_count) * v->csz; load_last_tick = timer_ticks();
    dir_iter(&d, load_cb, &L);
    if (vol_load_progress) vol_load_progress(load_total, load_total);
    dir_free(&d);
    vfs_generation++;
    return L.files;
}

/* ---------------- mount / format ---------------- */
fatvol_t *fat32_mount(blkdev_t *dev, uint32_t lba, uint32_t sectors) {
    uint8_t s[512];
    if (!blk_read(dev, lba, 1, s) || s[510] != 0x55 || s[511] != 0xAA) return NULL;
    bpb32_t *b = (bpb32_t *)s;
    if (b->bps != 512 || !b->spc || (b->spc & (b->spc - 1)) || !b->nfats || b->nfats > 2 || b->fatsz16 || !b->fatsz32 || b->root_clus < 2) return NULL;
    uint32_t total = b->total32 ? b->total32 : b->total16;
    if (sectors && total > sectors) total = sectors;
    if (b->fatsz32 > 32768) { klog("fat32: FAT too large to cache (%u sectors)", b->fatsz32); return NULL; }
    fatvol_t *v = kzalloc(sizeof *v);
    v->dev = dev; v->lba = lba; v->sectors = total; v->spc = b->spc; v->csz = b->spc * 512;
    v->reserved = b->reserved; v->nfats = b->nfats; v->fatsz = b->fatsz32; v->root_clus = b->root_clus;
    v->fsinfo = b->fsinfo ? b->fsinfo : 1;
    v->data_lba = lba + v->reserved + v->nfats * v->fatsz;
    v->nclus = (total - v->reserved - v->nfats * v->fatsz) / v->spc;
    if (v->nclus + 2 > v->fatsz * 128) v->nclus = v->fatsz * 128 - 2;
    v->serial = b->serial;
    memcpy(v->label, b->label, 11); v->label[11] = 0;
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
    v->fat = kmalloc(v->fatsz * 512);
    v->dirty = kzalloc(v->fatsz);
    v->tmp = kmalloc(v->csz < 512 ? 512 : v->csz);
    if (!blk_read(dev, lba + v->reserved, v->fatsz, v->fat)) { fat32_unmount(v); return NULL; }
    v->free_count = 0;
    for (uint32_t c = 2; c < v->nclus + 2; c++) if (!(v->fat[c] & 0x0FFFFFFF)) v->free_count++;
    v->hint = 2;
    klog("fat32: mounted \"%s\" serial %08x, %u clusters of %u bytes, %u MB free", v->label, v->serial, v->nclus, v->csz,
         (uint32_t)((uint64_t)v->free_count * v->csz / (1024 * 1024)));
    return v;
}
void fat32_unmount(fatvol_t *v) { if (!v) return; kfree(v->fat); kfree(v->dirty); kfree(v->tmp); kfree(v); }
uint32_t fat32_serial(fatvol_t *v) { return v->serial; }
const char *fat32_label(fatvol_t *v) { return v->label; }
uint64_t fat32_free_bytes(fatvol_t *v) { return (uint64_t)v->free_count * v->csz; }
uint64_t fat32_total_bytes(fatvol_t *v) { return (uint64_t)v->nclus * v->csz; }
blkdev_t *fat32_dev(fatvol_t *v) { return v->dev; }

bool fat32_format(blkdev_t *d, uint32_t lba, uint32_t total, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap) {
    if (total < 65536) { snprintf(err, errcap, "The partition is too small for FAT32 (minimum 32 MB)."); return false; }
    uint32_t spc = total < 532480 ? 1 : total < 16777216 ? 8 : total < 33554432 ? 16 : total < 67108864 ? 32 : 64;
    uint32_t reserved = 32, nfats = 2;
    uint32_t t2 = (256 * spc + nfats) / 2;
    uint32_t fatsz = (total - reserved + t2 - 1) / t2;
    uint32_t nclus = (total - reserved - nfats * fatsz) / spc;
    if (fatsz > 32768) { snprintf(err, errcap, "The partition is too large for this version of Setup (maximum about 128 GB)."); return false; }
    uint8_t s[512];
    if (cb) cb(0, "Writing boot sector");
    if (!blk_zero(d, lba, reserved)) { snprintf(err, errcap, "Setup could not write to the disk (sector %u).", lba); return false; }
    memset(s, 0, 512);
    bpb32_t *b = (bpb32_t *)s;
    b->jmp[0] = 0xEB; b->jmp[1] = 0x58; b->jmp[2] = 0x90;
    memcpy(b->oem, "WRLDOS  ", 8);
    b->bps = 512; b->spc = spc; b->reserved = reserved; b->nfats = nfats; b->media = 0xF8;
    b->spt = 63; b->heads = 255; b->hidden = lba; b->total32 = total; b->fatsz32 = fatsz;
    b->root_clus = 2; b->fsinfo = 1; b->backup = 6; b->drv = 0x80; b->sig = 0x29; b->serial = serial;
    char lab[12]; memset(lab, ' ', 11); for (int i = 0; i < 11 && label[i]; i++) lab[i] = toupper(label[i]);
    memcpy(b->label, lab, 11); memcpy(b->fstype, "FAT32   ", 8);
    static const uint8_t stub[] = { 0xFA, 0xF4, 0xEB, 0xFD };   /* cli; hlt; jmp $-1 : boot through the MBR */
    memcpy(s + 0x5A, stub, sizeof stub);
    s[510] = 0x55; s[511] = 0xAA;
    blk_write(d, lba, 1, s); blk_write(d, lba + 6, 1, s);
    memset(s, 0, 512);
    *(uint32_t *)s = 0x41615252; *(uint32_t *)(s + 484) = 0x61417272;
    *(uint32_t *)(s + 488) = nclus - 1; *(uint32_t *)(s + 492) = 3; *(uint32_t *)(s + 508) = 0xAA550000;
    blk_write(d, lba + 1, 1, s); blk_write(d, lba + 7, 1, s);
    /* FATs */
    uint32_t fat_total = nfats * fatsz, done = 0;
    while (done < fat_total) {
        uint32_t n = MIN(fat_total - done, 2048u);
        if (!blk_zero(d, lba + reserved + done, n)) { snprintf(err, errcap, "Setup could not write the file allocation table."); return false; }
        done += n;
        if (cb) cb(5 + (full ? 10 : 85) * done / fat_total, "Creating file allocation tables");
    }
    memset(s, 0, 512);
    ((uint32_t *)s)[0] = 0x0FFFFFF8; ((uint32_t *)s)[1] = 0x0FFFFFFF; ((uint32_t *)s)[2] = 0x0FFFFFFF;
    for (uint32_t f = 0; f < nfats; f++) blk_write(d, lba + reserved + f * fatsz, 1, s);
    /* root directory with the volume label */
    uint32_t data = lba + reserved + nfats * fatsz;
    uint8_t *root = kzalloc(spc * 512);
    de_t *e = (de_t *)root;
    memcpy(e->name, lab, 11); e->attr = A_LABEL; de_stamp(e);
    blk_write(d, data, spc, root);
    kfree(root);
    if (full) {
        uint32_t len = nclus * spc, step = 4096;
        for (uint32_t o = spc; o < len; o += step) {
            if (!blk_zero(d, data + o, MIN(step, len - o))) { snprintf(err, errcap, "Setup found a bad sector at %u and cannot format this partition.", data + o); return false; }
            if (cb) cb(15 + (int)((uint64_t)o * 85 / len), "Formatting");
        }
    }
    if (cb) cb(100, "Format complete");
    klog("fat32: formatted %u MB, %u clusters of %u bytes", total / 2048, nclus, spc * 512);
    return true;
}
