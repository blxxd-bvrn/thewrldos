/* NTFS 3.1 for THE WRLD OS: format, mount, read and write.
 *
 * On disk: 4 KiB clusters, 1 KiB MFT records and 4 KiB index blocks, both with
 * update-sequence fixups. Files keep $STANDARD_INFORMATION, $FILE_NAME (Win32
 * namespace), a resident $SECURITY_DESCRIPTOR and an unnamed $DATA (resident
 * when small, otherwise a run list). Directories keep a $I30 index of
 * $FILE_NAME keys: $INDEX_ROOT, plus $INDEX_ALLOCATION and $BITMAP when the
 * entries do not fit in the record.
 *
 * In memory the driver keeps the directory tree, the cluster bitmap and the MFT
 * bitmap. Writes go straight to the file's record and clusters; a changed
 * directory's index is rebuilt (as a balanced B-tree) by ntfs_sync().
 * Limits: no compression, encryption, attribute lists or alternate streams,
 * and the MFT does not grow (16384 records on volumes of 512 MB or more). */
#include "kernel.h"
#include "fs.h"
#include "disk.h"

#define CLUSTER 4096u
#define SPC (CLUSTER / 512)
#define RECSZ 1024u
#define IDXSZ 4096u
#define FIRST_USER_REC 24
#define AT_SI 0x10
#define AT_FN 0x30
#define AT_SD 0x50
#define AT_VOLNAME 0x60
#define AT_VOLINFO 0x70
#define AT_DATA 0x80
#define AT_IROOT 0x90
#define AT_IALLOC 0xA0
#define AT_BITMAP 0xB0
#define AT_END 0xFFFFFFFFu
#define MREF(rec, seq) ((uint64_t)(rec) | ((uint64_t)(seq) << 48))

typedef struct { uint64_t lcn, len; } run_t;           /* lcn == ~0: sparse */
typedef struct nnode {
    uint32_t rec; uint16_t seq;
    uint8_t dir, sys, dirty, resident;
    uint32_t fattr;                                     /* FILE_ATTRIBUTE_* */
    uint64_t ctime, mtime;
    uint64_t size, alloc;                               /* $DATA sizes (files) */
    run_t *runs; int nruns;                             /* file data / directory index blocks */
    char name[256]; uint16_t n16[255]; int nlen;
    struct nnode *parent, *child, *next;
} nnode_t;

struct ntfsvol {
    blkdev_t *dev; uint32_t lba;
    uint64_t total_sectors, nclus;
    run_t mft_runs[16]; int nmft_runs; uint32_t nrec;
    run_t mftbm_runs[4]; int nmftbm_runs; uint8_t *mftbm; uint32_t mftbm_bytes; bool mftbm_dirty;
    run_t bm_runs[8]; int nbm_runs; uint8_t *bm; uint32_t bm_bytes; bool bm_dirty;
    uint64_t free_clusters, hint;
    uint64_t serial64; char label[33];
    nnode_t *root;
    uint16_t usn;
    uint8_t rec[RECSZ], rec2[RECSZ];
};
typedef struct ntfsvol ntfsvol_t;

/* ---------------- helpers ---------------- */
static uint16_t upcase16(uint16_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {                      /* Latin Extended-A: pairs */
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) return (c & 1) ? c - 1 : c;
        return c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}
static int name_cmp(const uint16_t *a, int al, const uint16_t *b, int bl) {   /* COLLATION_FILENAME */
    for (int i = 0; i < al && i < bl; i++) {
        uint16_t x = upcase16(a[i]), y = upcase16(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    if (al != bl) return al < bl ? -1 : 1;
    for (int i = 0; i < al; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static uint64_t ntfs_now(void) {
    rtc_time_t t; rtc_read(&t);
    int y = t.year, m = t.month;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    int64_t secs = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    return (uint64_t)(secs + 11644473600ll) * 10000000ull;
}
static void set_name(nnode_t *n, const char *name) {
    strlcpy(n->name, name, sizeof n->name);
    n->nlen = 0;
    for (const uint8_t *p = (const uint8_t *)name; *p && n->nlen < 255; p++) n->n16[n->nlen++] = *p;   /* Latin-1 -> UTF-16 */
}
static bool sec_io(ntfsvol_t *v, uint64_t sector, uint32_t count, void *buf, bool wr) {
    if (sector + count > v->total_sectors + 1) return false;
    return wr ? blk_write(v->dev, v->lba + (uint32_t)sector, count, buf) : blk_read(v->dev, v->lba + (uint32_t)sector, count, buf);
}
/* byte offset inside a run list -> volume sector */
static bool run_sector(const run_t *r, int n, uint64_t off, uint64_t *sector) {
    uint64_t vcn = off / CLUSTER;
    for (int i = 0; i < n; i++) {
        if (vcn < r[i].len) { if (r[i].lcn == ~0ull) return false; *sector = (r[i].lcn + vcn) * SPC + (off % CLUSTER) / 512; return true; }
        vcn -= r[i].len;
    }
    return false;
}
static bool runs_io(ntfsvol_t *v, const run_t *r, int n, uint64_t off, uint32_t len, void *buf, bool wr) {
    uint8_t *b = buf;
    while (len) {
        uint64_t s; if (!run_sector(r, n, off, &s)) return false;
        uint32_t chunk = MIN(len, CLUSTER - (uint32_t)(off % CLUSTER));     /* stay inside one cluster */
        if (!sec_io(v, s, (chunk + 511) / 512, b, wr)) return false;
        b += chunk; off += chunk; len -= chunk;
    }
    return true;
}

/* ---------------- update sequence fixups ---------------- */
static void fixup_apply(ntfsvol_t *v, uint8_t *b, uint32_t size) {      /* before writing */
    uint16_t ofs = *(uint16_t *)(b + 4), cnt = *(uint16_t *)(b + 6);
    if (++v->usn == 0 || v->usn == 0xFFFF) v->usn = 1;
    uint16_t *usa = (uint16_t *)(b + ofs);
    usa[0] = v->usn;
    for (uint32_t i = 1; i < cnt && i * 512 <= size; i++) { uint16_t *end = (uint16_t *)(b + i * 512 - 2); usa[i] = *end; *end = v->usn; }
}
static bool fixup_undo(uint8_t *b, uint32_t size) {                     /* after reading */
    uint16_t ofs = *(uint16_t *)(b + 4), cnt = *(uint16_t *)(b + 6);
    if (ofs + cnt * 2 > size) return false;
    uint16_t *usa = (uint16_t *)(b + ofs);
    for (uint32_t i = 1; i < cnt && i * 512 <= size; i++) { uint16_t *end = (uint16_t *)(b + i * 512 - 2); if (*end != usa[0]) return false; *end = usa[i]; }
    return true;
}

/* ---------------- MFT records ---------------- */
static bool read_rec(ntfsvol_t *v, uint32_t n, uint8_t *b) {
    if (n >= v->nrec || !runs_io(v, v->mft_runs, v->nmft_runs, (uint64_t)n * RECSZ, RECSZ, b, false)) return false;
    return !memcmp(b, "FILE", 4) && fixup_undo(b, RECSZ);
}
static bool write_rec(ntfsvol_t *v, uint32_t n, const uint8_t *b) {
    uint8_t tmp[RECSZ]; memcpy(tmp, b, RECSZ);
    fixup_apply(v, tmp, RECSZ);
    return runs_io(v, v->mft_runs, v->nmft_runs, (uint64_t)n * RECSZ, RECSZ, tmp, true);
}
static void rec_init(uint8_t *b, uint32_t n, uint16_t seq, uint16_t flags) {
    memset(b, 0, RECSZ);
    memcpy(b, "FILE", 4);
    *(uint16_t *)(b + 0x04) = 0x30; *(uint16_t *)(b + 0x06) = 3;     /* update sequence array */
    *(uint16_t *)(b + 0x10) = seq ? seq : 1;
    *(uint16_t *)(b + 0x12) = 1;                                     /* hard links */
    *(uint16_t *)(b + 0x14) = 0x38;                                  /* first attribute */
    *(uint16_t *)(b + 0x16) = flags;                                 /* 1 in use, 2 directory */
    *(uint32_t *)(b + 0x1C) = RECSZ;
    *(uint32_t *)(b + 0x2C) = n;
    *(uint32_t *)(b + 0x38) = AT_END;
    *(uint32_t *)(b + 0x18) = 0x38 + 8;
}
static uint32_t rec_used(uint8_t *b) { return *(uint32_t *)(b + 0x18); }
/* append an attribute (before the end marker); returns its offset or 0 if it does not fit */
static uint32_t rec_add(uint8_t *b, uint32_t type, const uint16_t *name, int nlen, uint32_t len) {
    uint32_t at = rec_used(b) - 8;
    len = (len + 7) & ~7u;
    if (at + len + 8 > RECSZ) return 0;
    memset(b + at, 0, len);
    *(uint32_t *)(b + at) = type; *(uint32_t *)(b + at + 4) = len;
    *(uint16_t *)(b + at + 0x0E) = (*(uint16_t *)(b + 0x28))++;     /* attribute instance */
    (void)name; (void)nlen;
    *(uint32_t *)(b + at + len) = AT_END;
    *(uint32_t *)(b + 0x18) = at + len + 8;
    return at;
}
static bool add_resident(uint8_t *b, uint32_t type, const uint16_t *name, int nlen, const void *val, uint32_t vlen, uint8_t rflags) {
    uint32_t hdr = 0x18 + ((nlen * 2 + 7) & ~7u);
    uint32_t at = rec_add(b, type, name, nlen, hdr + vlen);
    if (!at) return false;
    b[at + 9] = nlen; *(uint16_t *)(b + at + 10) = 0x18;
    if (nlen) memcpy(b + at + 0x18, name, nlen * 2);
    *(uint32_t *)(b + at + 0x10) = vlen; *(uint16_t *)(b + at + 0x14) = hdr; b[at + 0x16] = rflags;
    if (vlen) memcpy(b + at + hdr, val, vlen);
    return true;
}
static int encode_runs(const run_t *r, int n, uint8_t *out, int cap) {
    int o = 0; int64_t prev = 0;
    for (int i = 0; i < n; i++) {
        uint8_t lb = 0, ob = 0; uint64_t len = r[i].len;
        while (lb < 8 && (len >> (8 * lb))) lb++;
        if (!lb) lb = 1;
        bool sparse = r[i].lcn == ~0ull;
        int64_t d = sparse ? 0 : (int64_t)r[i].lcn - prev;
        if (!sparse) { ob = 1; while (ob < 8 && !(d >= -(1ll << (8 * ob - 1)) && d < (1ll << (8 * ob - 1)))) ob++; }
        if (o + 1 + lb + ob + 1 > cap) return -1;
        out[o++] = lb | (ob << 4);
        for (int k = 0; k < lb; k++) out[o++] = len >> (8 * k);
        for (int k = 0; k < ob; k++) out[o++] = (uint64_t)d >> (8 * k);
        if (!sparse) prev = r[i].lcn;
    }
    out[o++] = 0;
    return o;
}
static bool add_nonresident(uint8_t *b, uint32_t type, const uint16_t *name, int nlen, const run_t *r, int nr, uint64_t size) {
    uint8_t map[512]; int ml = encode_runs(r, nr, map, sizeof map);
    if (ml < 0) return false;
    uint64_t clus = 0; for (int i = 0; i < nr; i++) clus += r[i].len;
    uint32_t nameoff = 0x40, mapoff = (nameoff + nlen * 2 + 7) & ~7u;
    uint32_t at = rec_add(b, type, name, nlen, mapoff + ml);
    if (!at) return false;
    b[at + 8] = 1; b[at + 9] = nlen; *(uint16_t *)(b + at + 10) = nameoff;
    if (nlen) memcpy(b + at + nameoff, name, nlen * 2);
    *(uint64_t *)(b + at + 0x10) = 0;                            /* lowest VCN */
    *(uint64_t *)(b + at + 0x18) = clus ? clus - 1 : 0;          /* highest VCN */
    *(uint16_t *)(b + at + 0x20) = mapoff;
    *(uint64_t *)(b + at + 0x28) = clus * CLUSTER;               /* allocated */
    *(uint64_t *)(b + at + 0x30) = size;                         /* data */
    *(uint64_t *)(b + at + 0x38) = size;                         /* initialised */
    memcpy(b + at + mapoff, map, ml);
    return true;
}
/* find an attribute by type (and name, NULL = unnamed) */
static uint8_t *find_attr(uint8_t *b, uint32_t type, const char *aname) {
    uint32_t off = *(uint16_t *)(b + 0x14);
    while (off + 8 <= RECSZ) {
        uint32_t t = *(uint32_t *)(b + off), len = *(uint32_t *)(b + off + 4);
        if (t == AT_END || len < 0x18 || off + len > RECSZ) break;
        if (t == type) {
            int nl = b[off + 9]; uint16_t no = *(uint16_t *)(b + off + 10);
            bool match = aname ? (nl == (int)strlen(aname)) : nl == 0;
            for (int i = 0; match && aname && i < nl; i++) if (*(uint16_t *)(b + off + no + 2 * i) != (uint8_t)aname[i]) match = false;
            if (match) return b + off;
        }
        off += len;
    }
    return NULL;
}
static int decode_runs(const uint8_t *a, run_t *out, int max) {
    const uint8_t *p = a + *(uint16_t *)(a + 0x20), *end = a + *(uint32_t *)(a + 4);
    int n = 0; int64_t lcn = 0;
    while (p < end && *p && n < max) {
        int lb = *p & 15, ob = *p >> 4; p++;
        uint64_t len = 0; for (int k = 0; k < lb; k++) len |= (uint64_t)p[k] << (8 * k);
        p += lb;
        if (ob) {
            int64_t d = 0; for (int k = 0; k < ob; k++) d |= (int64_t)p[k] << (8 * k);
            if ((p[ob - 1] & 0x80) && ob < 8) d |= (int64_t)(~0ull << (8 * ob));
            lcn += d; out[n].lcn = lcn;
        } else out[n].lcn = ~0ull;
        out[n].len = len; n++;
        p += ob;
    }
    return n;
}
static void *resident_value(uint8_t *a, uint32_t *len) { *len = *(uint32_t *)(a + 0x10); return a + *(uint16_t *)(a + 0x14); }

/* ---------------- clusters ---------------- */
static bool bm_get(ntfsvol_t *v, uint64_t c) { return v->bm[c >> 3] & (1 << (c & 7)); }
static void bm_set(ntfsvol_t *v, uint64_t c, bool on) {
    if (on == bm_get(v, c)) return;
    if (on) { v->bm[c >> 3] |= 1 << (c & 7); v->free_clusters--; } else { v->bm[c >> 3] &= ~(1 << (c & 7)); v->free_clusters++; }
    v->bm_dirty = true;
}
static void free_runs(ntfsvol_t *v, run_t *r, int n) {
    for (int i = 0; i < n; i++) if (r[i].lcn != ~0ull) for (uint64_t k = 0; k < r[i].len; k++) if (r[i].lcn + k < v->nclus) bm_set(v, r[i].lcn + k, false);
}
/* allocate n clusters: one contiguous run if possible, else up to max runs */
static int alloc_runs(ntfsvol_t *v, uint64_t n, run_t *out, int max) {
    if (!n) return 0;
    if (n > v->free_clusters) return -1;
    for (int pass = 0; pass < 2; pass++) {                       /* contiguous, from the hint, then from 0 */
        uint64_t start = pass ? 0 : v->hint, run = 0;
        for (uint64_t c = start; c < v->nclus; c++) {
            if (bm_get(v, c)) { run = 0; continue; }
            if (++run == n) {
                uint64_t s = c + 1 - n;
                for (uint64_t k = 0; k < n; k++) bm_set(v, s + k, true);
                out[0].lcn = s; out[0].len = n; v->hint = c + 1;
                return 1;
            }
        }
    }
    int k = 0; uint64_t need = n;                                /* fragmented */
    for (uint64_t c = 0; c < v->nclus && need; c++) {
        if (bm_get(v, c)) continue;
        if (k && out[k - 1].lcn + out[k - 1].len == c) out[k - 1].len++;
        else { if (k == max) { free_runs(v, out, k); return -1; } out[k].lcn = c; out[k].len = 1; k++; }
        bm_set(v, c, true); need--;
    }
    return need ? (free_runs(v, out, k), -1) : k;
}

/* ---------------- tree ---------------- */
static nnode_t *child_named(nnode_t *dir, const char *name, int len) {
    for (nnode_t *c = dir->child; c; c = c->next) {
        if ((int)strlen(c->name) != len) continue;
        bool eq = true;
        for (int i = 0; i < len && eq; i++) if (upcase16((uint8_t)c->name[i]) != upcase16((uint8_t)name[i])) eq = false;
        if (eq) return c;
    }
    return NULL;
}
static nnode_t *lookup(ntfsvol_t *v, const char *path, bool parent_only, const char **leaf) {
    nnode_t *cur = v->root;
    const char *s = path;
    while (*s == '\\' || *s == '/') s++;
    for (;;) {
        const char *e = s; while (*e && *e != '\\' && *e != '/') e++;
        const char *nx = e; while (*nx == '\\' || *nx == '/') nx++;
        if (parent_only && !*nx) { if (leaf) *leaf = s; return cur; }
        if (e == s) return cur;
        nnode_t *c = child_named(cur, s, e - s);
        if (!c || c->sys) return NULL;
        cur = c; s = nx;
        if (!*s) return cur;
        if (!cur->dir) return NULL;
    }
}
static void link_child(nnode_t *dir, nnode_t *c) { c->parent = dir; c->next = dir->child; dir->child = c; }
static void unlink_child(nnode_t *c) {
    if (!c->parent) return;
    for (nnode_t **pp = &c->parent->child; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    c->parent = NULL; c->next = NULL;
}
static int alloc_rec(ntfsvol_t *v) {
    for (uint32_t i = FIRST_USER_REC; i < v->nrec; i++) if (!(v->mftbm[i >> 3] & (1 << (i & 7)))) {
        v->mftbm[i >> 3] |= 1 << (i & 7); v->mftbm_dirty = true; return i;
    }
    return -1;
}
static uint16_t next_seq(ntfsvol_t *v, uint32_t n) {
    uint8_t b[RECSZ];
    if (runs_io(v, v->mft_runs, v->nmft_runs, (uint64_t)n * RECSZ, RECSZ, b, false) && !memcmp(b, "FILE", 4)) {
        uint16_t s = *(uint16_t *)(b + 0x10) + 1;
        return s ? s : 1;
    }
    return 1;
}

/* the $FILE_NAME value a node has in its parent */
static uint32_t make_fn(nnode_t *n, uint8_t *out) {
    nnode_t *p = n->parent ? n->parent : n;
    memset(out, 0, 0x42);
    *(uint64_t *)(out + 0x00) = MREF(p->rec, p->seq);
    *(uint64_t *)(out + 0x08) = n->ctime; *(uint64_t *)(out + 0x10) = n->mtime;
    *(uint64_t *)(out + 0x18) = n->mtime; *(uint64_t *)(out + 0x20) = n->mtime;
    if (!n->dir) { *(uint64_t *)(out + 0x28) = n->alloc; *(uint64_t *)(out + 0x30) = n->size; }
    *(uint32_t *)(out + 0x38) = n->fattr | (n->dir ? 0x10000000u : 0);
    out[0x40] = n->nlen; out[0x41] = n->sys ? 3 : 1;             /* Win32&DOS for $-files, else Win32 */
    memcpy(out + 0x42, n->n16, n->nlen * 2);
    return 0x42 + n->nlen * 2;
}
static const uint8_t default_sd[80] = {
    0x01, 0x00, 0x04, 0x80, 48, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0,
    /* DACL: Everyone, full control, inherited by files and folders */
    0x02, 0x00, 28, 0, 1, 0, 0, 0,  0x00, 0x03, 20, 0, 0xFF, 0x01, 0x1F, 0x00,  1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    /* owner and group: BUILTIN\Administrators */
    1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 0x02, 0, 0,
    1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 0x02, 0, 0 };
/* $STANDARD_INFORMATION + $FILE_NAME + $SECURITY_DESCRIPTOR at the start of a record */
static bool rec_common_sd(uint8_t *b, nnode_t *n, bool sd) {
    uint8_t si[0x30] = { 0 };
    *(uint64_t *)(si + 0) = n->ctime; *(uint64_t *)(si + 8) = n->mtime; *(uint64_t *)(si + 16) = n->mtime; *(uint64_t *)(si + 24) = n->mtime;
    *(uint32_t *)(si + 32) = n->fattr;
    uint8_t fn[0x42 + 510]; uint32_t fl = make_fn(n, fn);
    return add_resident(b, AT_SI, NULL, 0, si, sizeof si, 0) && add_resident(b, AT_FN, NULL, 0, fn, fl, 1) &&
           (!sd || add_resident(b, AT_SD, NULL, 0, default_sd, sizeof default_sd, 0));
}
static bool rec_common(uint8_t *b, nnode_t *n) { return rec_common_sd(b, n, true); }
/* bytes a record has left after its header, SI, FN and SD (and the end marker) */
static int rec_room(nnode_t *n, bool sd) { return (int)RECSZ - 0x38 - 0x48 - (int)((0x18 + 0x42 + n->nlen * 2 + 7) & ~7u) - (sd ? 0x68 : 0) - 8; }
static const uint16_t I30[4] = { '$', 'I', '3', '0' };

/* ---------------- directory indexes ---------------- */
typedef struct { uint8_t *key; uint32_t klen; uint64_t ref; int64_t child; } ient_t;   /* child: VCN or -1 */
static uint32_t ient_size(const ient_t *e) { return ((0x10 + e->klen + 7) & ~7u) + (e->child >= 0 ? 8 : 0); }
static uint32_t put_entry(uint8_t *o, const ient_t *e, bool last, int64_t last_child) {
    if (last) {
        uint32_t len = 0x10 + (last_child >= 0 ? 8 : 0);
        memset(o, 0, len);
        *(uint16_t *)(o + 8) = len; *(uint16_t *)(o + 12) = 2 | (last_child >= 0 ? 1 : 0);
        if (last_child >= 0) *(uint64_t *)(o + len - 8) = last_child;
        return len;
    }
    uint32_t len = ient_size(e);
    memset(o, 0, len);
    *(uint64_t *)o = e->ref; *(uint16_t *)(o + 8) = len; *(uint16_t *)(o + 10) = e->klen;
    *(uint16_t *)(o + 12) = e->child >= 0 ? 1 : 0;
    memcpy(o + 0x10, e->key, e->klen);
    if (e->child >= 0) *(uint64_t *)(o + len - 8) = e->child;
    return len;
}
static int ient_cmp(const void *a, const void *b) {
    const ient_t *x = a, *y = b;
    return name_cmp((const uint16_t *)(x->key + 0x42), x->key[0x40], (const uint16_t *)(y->key + 0x42), y->key[0x40]);
}
static void sort_ients(ient_t *e, int n) {           /* insertion sort: directories are small */
    for (int i = 1; i < n; i++) { ient_t t = e[i]; int j = i - 1; while (j >= 0 && ient_cmp(&e[j], &t) > 0) { e[j + 1] = e[j]; j--; } e[j + 1] = t; }
}
#define BLK_ENTRIES (IDXSZ - 0x40 - 0x18)        /* room for entries in a block, keeping space for the end entry */
#define ROOT_RESERVE (0x20 + 0x20 + 0x60 + 0x30)   /* $INDEX_ROOT headers, $INDEX_ALLOCATION and $BITMAP */
static bool rebuild_dir(ntfsvol_t *v, nnode_t *d) {
    int n = 0; for (nnode_t *c = d->child; c; c = c->next) n++;
    if (d == v->root) n++;                                         /* the root lists itself as "." */
    ient_t *lv = kzalloc(sizeof(ient_t) * (n + 1));
    uint8_t *keys = kmalloc((size_t)(n + 1) * 0x250);
    int k = 0;
    for (nnode_t *c = d->child; c; c = c->next, k++) { lv[k].key = keys + k * 0x250; lv[k].klen = make_fn(c, lv[k].key); lv[k].ref = MREF(c->rec, c->seq); lv[k].child = -1; }
    if (d == v->root) { lv[k].key = keys + k * 0x250; lv[k].klen = make_fn(d, lv[k].key); lv[k].ref = MREF(5, d->seq); lv[k].child = -1; k++; }
    sort_ients(lv, n);
    bool sd = rec_room(d, true) - ROOT_RESERVE >= 0x40;       /* very long folder names: no room for a descriptor */
    int room = rec_room(d, sd) - ROOT_RESERVE;
    if (room < 0x18) room = 0x18;
    /* build the B-tree bottom up; blocks[] receives the index blocks in VCN order */
    uint8_t *blocks = NULL; int nblocks = 0, capblocks = 0;
    int64_t level_end = -1;                                        /* child pointer of the level's end entry */
    int cnt = n;
    for (;;) {
        uint32_t total = 0x10 + (level_end >= 0 ? 8 : 0);
        for (int i = 0; i < cnt; i++) total += ient_size(&lv[i]);
        if ((int)total <= room) break;
        /* pack this level into blocks; the entry after each full block moves up */
        ient_t *up = kzalloc(sizeof(ient_t) * (cnt + 1)); int nup = 0;
        int i = 0;
        for (;;) {
            if (nblocks == capblocks) { capblocks = capblocks ? capblocks * 2 : 8; blocks = krealloc(blocks, (size_t)capblocks * IDXSZ); }
            uint8_t *blk = blocks + (size_t)nblocks * IDXSZ; memset(blk, 0, IDXSZ);
            uint32_t used = 0; int first = i;
            while (i < cnt && used + ient_size(&lv[i]) <= BLK_ENTRIES - 8) { used += ient_size(&lv[i]); i++; }
            if (i == first && i < cnt) { used += ient_size(&lv[i]); i++; }    /* always progress */
            bool final = i >= cnt;
            int64_t endchild = final ? level_end : lv[i].child;            /* the separator's own child */
            uint8_t *o = blk + 0x40; bool internal = endchild >= 0;
            for (int j = first; j < (final ? cnt : i); j++) { o += put_entry(o, &lv[j], false, 0); if (lv[j].child >= 0) internal = true; }
            o += put_entry(o, NULL, true, endchild);
            memcpy(blk, "INDX", 4);
            *(uint16_t *)(blk + 4) = 0x28; *(uint16_t *)(blk + 6) = 1 + IDXSZ / 512;
            *(uint64_t *)(blk + 0x10) = nblocks;                           /* VCN (one cluster per block) */
            *(uint32_t *)(blk + 0x18) = 0x28; *(uint32_t *)(blk + 0x1C) = (uint32_t)(o - blk) - 0x18;
            *(uint32_t *)(blk + 0x20) = IDXSZ - 0x18; blk[0x24] = internal ? 1 : 0;
            int vcn = nblocks++;
            if (final) { level_end = vcn; break; }
            up[nup] = lv[i]; up[nup].child = vcn; nup++; i++;             /* separator moves up */
        }
        kfree(lv); lv = up; cnt = nup;
    }
    /* index blocks on disk: free the old ones, allocate new */
    free_runs(v, d->runs, d->nruns); kfree(d->runs); d->runs = NULL; d->nruns = 0;
    bool ok = true;
    if (nblocks) {
        run_t r[16]; int nr = alloc_runs(v, nblocks, r, 16);
        if (nr < 0) ok = false;
        else {
            d->runs = kmalloc(sizeof(run_t) * nr); memcpy(d->runs, r, sizeof(run_t) * nr); d->nruns = nr;
            for (int b = 0; b < nblocks && ok; b++) {
                uint8_t *blk = blocks + (size_t)b * IDXSZ;
                fixup_apply(v, blk, IDXSZ);
                ok = runs_io(v, d->runs, d->nruns, (uint64_t)b * IDXSZ, IDXSZ, blk, true);
            }
        }
    }
    /* the directory record */
    uint8_t *b = v->rec;
    rec_init(b, d->rec, d->seq, 3);
    ok = ok && rec_common_sd(b, d, sd);
    uint8_t root[RECSZ]; memset(root, 0, sizeof root);
    *(uint32_t *)(root + 0) = AT_FN; *(uint32_t *)(root + 4) = 1;             /* COLLATION_FILENAME */
    *(uint32_t *)(root + 8) = IDXSZ; root[12] = IDXSZ / CLUSTER;
    uint8_t *o = root + 0x20;
    for (int i = 0; i < cnt; i++) o += put_entry(o, &lv[i], false, 0);
    o += put_entry(o, NULL, true, level_end);
    uint32_t ilen = (uint32_t)(o - (root + 0x10));
    *(uint32_t *)(root + 0x10) = 0x10; *(uint32_t *)(root + 0x14) = ilen; *(uint32_t *)(root + 0x18) = ilen;
    root[0x1C] = level_end >= 0 ? 1 : 0;
    ok = ok && add_resident(b, AT_IROOT, I30, 4, root, 0x10 + ilen, 0);
    if (ok && nblocks) {
        uint8_t bmp[64] = { 0 }; uint32_t bl = ((nblocks + 63) / 64) * 8;
        if (bl > sizeof bmp) ok = false;
        for (int i = 0; i < nblocks && ok; i++) bmp[i >> 3] |= 1 << (i & 7);
        ok = ok && add_nonresident(b, AT_IALLOC, I30, 4, d->runs, d->nruns, (uint64_t)nblocks * IDXSZ) &&
             add_resident(b, AT_BITMAP, I30, 4, bmp, bl, 0);
    }
    if (ok) ok = write_rec(v, d->rec, b);
    kfree(lv); kfree(keys); kfree(blocks);
    if (!ok) klog("ntfs: could not rebuild the index of %s (%d entries)", d->name, n);
    d->dirty = 0;
    return ok;
}

/* ---------------- file records ---------------- */
static bool write_file_rec(ntfsvol_t *v, nnode_t *f, const void *data, uint32_t len) {
    uint8_t *b = v->rec;
    rec_init(b, f->rec, f->seq, 1);
    if (!rec_common(b, f)) return false;
    if (f->resident) return add_resident(b, AT_DATA, NULL, 0, data, len, 0) && write_rec(v, f->rec, b);
    return add_nonresident(b, AT_DATA, NULL, 0, f->runs, f->nruns, f->size) && write_rec(v, f->rec, b);
}
/* rename / move: rewrite the $FILE_NAME, keeping $DATA or the index as they are */
static bool rewrite_names(ntfsvol_t *v, nnode_t *n) {
    if (n->dir) { n->dirty = 1; return true; }
    uint8_t *old = v->rec2;
    if (!read_rec(v, n->rec, old)) return false;
    uint8_t *da = find_attr(old, AT_DATA, NULL);
    if (!da) return false;
    uint32_t dlen = *(uint32_t *)(da + 4);
    uint8_t *b = v->rec;
    rec_init(b, n->rec, n->seq, 1);
    if (!rec_common(b, n)) return false;
    uint32_t at = rec_add(b, AT_DATA, NULL, 0, dlen);
    if (!at) return false;
    uint16_t inst = *(uint16_t *)(b + at + 0x0E);
    memcpy(b + at, da, dlen); *(uint16_t *)(b + at + 0x0E) = inst;
    return write_rec(v, n->rec, b);
}
static nnode_t *new_node(ntfsvol_t *v, nnode_t *parent, const char *name, bool dir) {
    int r = alloc_rec(v);
    if (r < 0) { klog("ntfs: the MFT is full"); return NULL; }
    nnode_t *n = kzalloc(sizeof *n);
    n->rec = r; n->seq = next_seq(v, r); n->dir = dir;
    n->ctime = n->mtime = ntfs_now();
    n->fattr = dir ? 0 : 0x20;                                  /* ARCHIVE */
    set_name(n, name);
    link_child(parent, n);
    parent->dirty = 1;
    if (dir) n->dirty = 1;
    return n;
}

/* ---------------- public API ---------------- */
bool ntfs_exists(ntfsvol_t *v, const char *path) { return lookup(v, path, false, NULL) != NULL; }
bool ntfs_mkdir(ntfsvol_t *v, const char *path) {
    nnode_t *cur = v->root;
    const char *s = path;
    while (*s) {
        while (*s == '\\' || *s == '/') s++;
        if (!*s) break;
        const char *e = s; while (*e && *e != '\\' && *e != '/') e++;
        char name[256]; int l = MIN((int)(e - s), 255); memcpy(name, s, l); name[l] = 0;
        nnode_t *c = child_named(cur, s, e - s);
        if (c && !c->dir) return false;
        if (!c) { c = new_node(v, cur, name, true); if (!c) return false; }
        cur = c; s = e;
    }
    return true;
}
static void free_node_tree(nnode_t *n) {
    while (n->child) { nnode_t *c = n->child; n->child = c->next; free_node_tree(c); }
    kfree(n->runs); kfree(n);
}
static bool del_rec(ntfsvol_t *v, nnode_t *n) {
    uint8_t *b = v->rec;
    if (read_rec(v, n->rec, b)) {
        *(uint16_t *)(b + 0x16) &= ~3;                          /* not in use */
        uint16_t s = *(uint16_t *)(b + 0x10) + 1; *(uint16_t *)(b + 0x10) = s ? s : 1;
        write_rec(v, n->rec, b);
    }
    v->mftbm[n->rec >> 3] &= ~(1 << (n->rec & 7)); v->mftbm_dirty = true;
    free_runs(v, n->runs, n->nruns);
    return true;
}
static void delete_tree(ntfsvol_t *v, nnode_t *n) {
    for (nnode_t *c = n->child; c; c = c->next) delete_tree(v, c);
    del_rec(v, n);
}
bool ntfs_delete(ntfsvol_t *v, const char *path) {
    nnode_t *n = lookup(v, path, false, NULL);
    if (!n || n == v->root || n->sys) return false;
    delete_tree(v, n);
    nnode_t *p = n->parent; unlink_child(n); p->dirty = 1;
    free_node_tree(n);
    return true;
}
bool ntfs_write(ntfsvol_t *v, const char *path, const void *data, uint32_t len, bool readonly) {
    const char *leaf;
    char dirpath[512]; strlcpy(dirpath, path, sizeof dirpath);
    nnode_t *dir = lookup(v, path, true, &leaf);
    if (!dir) {                                                 /* create missing folders */
        char *slash = strrchr(dirpath, '\\'); char *s2 = strrchr(dirpath, '/'); if (s2 > slash) slash = s2;
        if (slash) { *slash = 0; if (!ntfs_mkdir(v, dirpath)) return false; }
        dir = lookup(v, path, true, &leaf);
        if (!dir) return false;
    }
    if (!dir->dir || !*leaf) return false;
    nnode_t *f = child_named(dir, leaf, strlen(leaf));
    if (f && (f->dir || f->sys)) return false;
    if (!f) { f = new_node(v, dir, leaf, false); if (!f) return false; }
    free_runs(v, f->runs, f->nruns); kfree(f->runs); f->runs = NULL; f->nruns = 0;
    f->mtime = ntfs_now(); f->size = len;
    f->fattr = 0x20 | (readonly ? 1 : 0);
    f->resident = (int)len + 0x18 <= rec_room(f, true) && len <= 700;
    bool ok = true;
    if (f->resident) f->alloc = (len + 7) & ~7u;
    else {
        uint64_t nc = (len + CLUSTER - 1) / CLUSTER;
        run_t r[48]; int nr = alloc_runs(v, nc, r, 48);
        if (nr < 0) { klog("ntfs: disk full writing %s", path); return false; }
        f->runs = kmalloc(sizeof(run_t) * nr); memcpy(f->runs, r, sizeof(run_t) * nr); f->nruns = nr;
        f->alloc = nc * CLUSTER;
        /* whole clusters; the tail cluster padded with zeros */
        uint32_t full = len / CLUSTER * CLUSTER;
        uint64_t off = 0;
        for (int i = 0; i < nr && ok; i++) {
            uint64_t bytes = MIN(r[i].len * CLUSTER, (uint64_t)full - MIN(off, (uint64_t)full));
            if (bytes) ok = sec_io(v, r[i].lcn * SPC, (uint32_t)(bytes / 512), (uint8_t *)data + off, true);
            off += r[i].len * CLUSTER;
        }
        if (ok && full < len) {
            uint8_t *t = kzalloc(CLUSTER); memcpy(t, (const uint8_t *)data + full, len - full);
            uint64_t s; ok = run_sector(f->runs, f->nruns, full, &s) && sec_io(v, s, SPC, t, true);
            kfree(t);
        }
    }
    ok = ok && write_file_rec(v, f, data, len);
    dir->dirty = 1;                                             /* sizes in the index changed */
    if (!ok) klog("ntfs: could not write %s", path);
    return ok;
}
bool ntfs_rename(ntfsvol_t *v, const char *from, const char *to) {
    nnode_t *n = lookup(v, from, false, NULL);
    if (!n || n == v->root || n->sys || lookup(v, to, false, NULL)) return false;
    const char *leaf; nnode_t *nd = lookup(v, to, true, &leaf);
    if (!nd || !nd->dir || !*leaf) return false;
    for (nnode_t *a = nd; a; a = a->parent) if (a == n) return false;
    nnode_t *op = n->parent;
    unlink_child(n); op->dirty = 1;
    set_name(n, leaf); link_child(nd, n); nd->dirty = 1;
    return rewrite_names(v, n);
}
static uint8_t *read_node_data(ntfsvol_t *v, nnode_t *n, uint32_t *len) {
    if (n->dir) return NULL;
    uint32_t sz = (uint32_t)n->size;
    uint8_t *buf = kmalloc(sz + 1);
    if (!buf) return NULL;
    if (n->resident) {
        uint8_t *b = v->rec2;
        uint8_t *a = read_rec(v, n->rec, b) ? find_attr(b, AT_DATA, NULL) : NULL;
        uint32_t vl; uint8_t *val = a ? resident_value(a, &vl) : NULL;
        if (!val) { kfree(buf); return NULL; }
        memcpy(buf, val, MIN(vl, sz));
    } else {
        uint64_t off = 0;
        for (int i = 0; i < n->nruns && off < sz; i++) {
            uint64_t bytes = MIN(n->runs[i].len * CLUSTER, (uint64_t)sz - off);
            if (n->runs[i].lcn == ~0ull) memset(buf + off, 0, bytes);
            else {
                uint32_t secs = (uint32_t)((bytes + 511) / 512);
                if (bytes % 512) {                              /* tail: read via a bounce buffer */
                    uint8_t *t = kmalloc((size_t)secs * 512);
                    if (!sec_io(v, n->runs[i].lcn * SPC, secs, t, false)) { kfree(t); kfree(buf); return NULL; }
                    memcpy(buf + off, t, bytes); kfree(t);
                } else if (!sec_io(v, n->runs[i].lcn * SPC, secs, buf + off, false)) { kfree(buf); return NULL; }
            }
            off += bytes;
        }
    }
    *len = sz;
    return buf;
}
uint8_t *ntfs_read(ntfsvol_t *v, const char *path, uint32_t *len) {
    nnode_t *n = lookup(v, path, false, NULL);
    return n ? read_node_data(v, n, len) : NULL;
}
extern void (*vol_load_progress)(uint64_t done, uint64_t total);
static uint64_t load_done, load_total; static uint32_t load_tick;
static int load_into(ntfsvol_t *v, nnode_t *d, vnode_t *vd) {
    int files = 0;
    for (nnode_t *c = d->child; c; c = c->next) {
        if (c->sys) continue;
        vnode_t *n = vfs_lookup_child(vd, c->name);
        if (n && n->dir != c->dir) n = NULL;
        if (!n) n = vfs_new_node(vd, c->name, c->dir);
        if (c->dir) { files += load_into(v, c, n); continue; }
        uint32_t l = 0; uint8_t *data = read_node_data(v, c, &l);
        if (!data) { klog("ntfs: read failed for %s", c->name); data = kmalloc(1); l = 0; }
        kfree(n->data); n->data = data; n->size = l; n->cap = l + 1;
        n->readonly = (c->fattr & 1) != 0;
        files++;
        load_done += c->alloc;
        if (vol_load_progress && timer_ticks() - load_tick >= 50) { load_tick = timer_ticks(); vol_load_progress(load_done, load_total); }
    }
    return files;
}
int ntfs_load_tree(ntfsvol_t *v, vnode_t *root) {
    load_done = 0; load_tick = timer_ticks();
    load_total = (v->nclus - v->free_clusters) * CLUSTER;
    int f = load_into(v, v->root, root);
    if (vol_load_progress) vol_load_progress(load_total, load_total);
    vfs_generation++;
    return f;
}
static void sync_dirs(ntfsvol_t *v, nnode_t *d) {
    for (nnode_t *c = d->child; c; c = c->next) if (c->dir) sync_dirs(v, c);
    if (d->dirty) rebuild_dir(v, d);
}
void ntfs_sync(ntfsvol_t *v) {
    sync_dirs(v, v->root);
    if (v->mftbm_dirty) { runs_io(v, v->mftbm_runs, v->nmftbm_runs, 0, (v->mftbm_bytes + 511) & ~511u, v->mftbm, true); v->mftbm_dirty = false; }
    if (v->bm_dirty) { runs_io(v, v->bm_runs, v->nbm_runs, 0, (v->bm_bytes + 511) & ~511u, v->bm, true); v->bm_dirty = false; }
}
uint32_t ntfs_serial(ntfsvol_t *v) { return (uint32_t)v->serial64; }
const char *ntfs_label(ntfsvol_t *v) { return v->label; }
uint64_t ntfs_free_bytes(ntfsvol_t *v) { return v->free_clusters * CLUSTER; }
uint64_t ntfs_total_bytes(ntfsvol_t *v) { return v->nclus * CLUSTER; }
void ntfs_unmount(ntfsvol_t *v) {
    if (!v) return;
    if (v->root) free_node_tree(v->root);
    kfree(v->bm); kfree(v->mftbm); kfree(v);
}

/* ---------------- mount ---------------- */
ntfsvol_t *ntfs_mount(blkdev_t *dev, uint32_t lba, uint32_t sectors) {
    uint8_t bs[512];
    if (!blk_read(dev, lba, 1, bs) || memcmp(bs + 3, "NTFS    ", 8) || bs[510] != 0x55 || bs[511] != 0xAA) return NULL;
    if (*(uint16_t *)(bs + 0x0B) != 512 || bs[0x0D] != SPC || (int8_t)bs[0x40] != -10 || bs[0x44] != 1) {
        klog("ntfs: unsupported geometry (%u bytes/sector, %u sectors/cluster)", *(uint16_t *)(bs + 0x0B), bs[0x0D]);
        return NULL;
    }
    ntfsvol_t *v = kzalloc(sizeof *v);
    v->dev = dev; v->lba = lba;
    v->total_sectors = *(uint64_t *)(bs + 0x28);
    if (sectors && v->total_sectors > sectors) v->total_sectors = sectors;
    v->nclus = v->total_sectors / SPC;
    v->serial64 = *(uint64_t *)(bs + 0x48);
    /* bootstrap the MFT run list from record 0 */
    v->mft_runs[0].lcn = *(uint64_t *)(bs + 0x30); v->mft_runs[0].len = 4; v->nmft_runs = 1; v->nrec = 16;
    uint8_t *b = v->rec;
    uint8_t *a;
    if (!read_rec(v, 0, b) || !(a = find_attr(b, AT_DATA, NULL)) || !b[(a - b) + 8]) goto fail;
    v->nmft_runs = decode_runs(a, v->mft_runs, 16);
    v->nrec = (uint32_t)(*(uint64_t *)(a + 0x30) / RECSZ);
    if (!(a = find_attr(b, AT_BITMAP, NULL)) || !a[8]) goto fail;
    v->nmftbm_runs = decode_runs(a, v->mftbm_runs, 4);
    v->mftbm_bytes = (uint32_t)*(uint64_t *)(a + 0x30);
    v->mftbm = kzalloc((v->mftbm_bytes + 1023) & ~511u);
    if (!runs_io(v, v->mftbm_runs, v->nmftbm_runs, 0, (v->mftbm_bytes + 511) & ~511u, v->mftbm, false)) goto fail;
    /* cluster bitmap */
    if (!read_rec(v, 6, b) || !(a = find_attr(b, AT_DATA, NULL)) || !a[8]) goto fail;
    v->nbm_runs = decode_runs(a, v->bm_runs, 8);
    v->bm_bytes = (uint32_t)*(uint64_t *)(a + 0x30);
    v->bm = kzalloc((v->bm_bytes + 1023) & ~511u);
    if (!runs_io(v, v->bm_runs, v->nbm_runs, 0, (v->bm_bytes + 511) & ~511u, v->bm, false)) goto fail;
    for (uint64_t c = 0; c < v->nclus; c++) if (!bm_get(v, c)) v->free_clusters++;
    /* volume label */
    if (read_rec(v, 3, b) && (a = find_attr(b, AT_VOLNAME, NULL))) {
        uint32_t vl; uint16_t *nm = resident_value(a, &vl);
        int k = 0; for (uint32_t i = 0; i < vl / 2 && k < 32; i++) v->label[k++] = nm[i] < 256 ? (char)nm[i] : '?';
        v->label[k] = 0;
    }
    /* every record in use -> a node */
    nnode_t **by = kzalloc(sizeof(nnode_t *) * v->nrec);
    uint64_t *parent = kzalloc(sizeof(uint64_t) * v->nrec);
    for (uint32_t r = 0; r < v->nrec; r++) {
        if (!(v->mftbm[r >> 3] & (1 << (r & 7)))) continue;
        if (!read_rec(v, r, b)) continue;
        uint16_t fl = *(uint16_t *)(b + 0x16);
        if (!(fl & 1) || *(uint64_t *)(b + 0x20)) continue;      /* free, or an extension record */
        uint8_t *fn = NULL;
        for (uint32_t off = *(uint16_t *)(b + 0x14); off + 0x18 <= RECSZ;) {     /* best $FILE_NAME: not DOS-only */
            uint32_t t = *(uint32_t *)(b + off), len = *(uint32_t *)(b + off + 4);
            if (t == AT_END || len < 0x18) break;
            if (t == AT_FN) { uint32_t vl; uint8_t *val = resident_value(b + off, &vl); if (val[0x41] != 2 && (!fn || val[0x41] == 1)) fn = val; }
            off += len;
        }
        if (!fn && r != 5) continue;
        nnode_t *n = kzalloc(sizeof *n);
        n->rec = r; n->seq = *(uint16_t *)(b + 0x10); n->dir = (fl & 2) != 0; n->sys = r < 16;
        if (fn) {
            parent[r] = *(uint64_t *)fn & 0xFFFFFFFFFFFFull;
            n->nlen = fn[0x40];
            memcpy(n->n16, fn + 0x42, n->nlen * 2);
            for (int i = 0; i < n->nlen; i++) n->name[i] = n->n16[i] < 256 ? (char)n->n16[i] : '?';
            n->name[n->nlen] = 0;
        }
        if ((a = find_attr(b, AT_SI, NULL))) { uint32_t vl; uint8_t *si = resident_value(a, &vl); n->ctime = *(uint64_t *)si; n->mtime = *(uint64_t *)(si + 8); n->fattr = *(uint32_t *)(si + 32); }
        if (!n->dir && (a = find_attr(b, AT_DATA, NULL))) {
            if (a[8]) {
                run_t tmp[64]; int nr = decode_runs(a, tmp, 64);
                n->runs = kmalloc(sizeof(run_t) * (nr ? nr : 1)); memcpy(n->runs, tmp, sizeof(run_t) * nr); n->nruns = nr;
                n->size = *(uint64_t *)(a + 0x30); n->alloc = *(uint64_t *)(a + 0x28);
            } else { uint32_t vl; resident_value(a, &vl); n->size = vl; n->alloc = (vl + 7) & ~7u; n->resident = 1; }
        }
        if (n->dir && (a = find_attr(b, AT_IALLOC, "$I30")) && a[8]) {
            run_t tmp[16]; int nr = decode_runs(a, tmp, 16);
            n->runs = kmalloc(sizeof(run_t) * (nr ? nr : 1)); memcpy(n->runs, tmp, sizeof(run_t) * nr); n->nruns = nr;
        }
        by[r] = n;
    }
    v->root = by[5];
    if (!v->root) { kfree(by); kfree(parent); goto fail; }
    set_name(v->root, ".");
    for (uint32_t r = 0; r < v->nrec; r++) {
        nnode_t *n = by[r];
        if (!n || r == 5) continue;
        uint64_t p = parent[r];
        if (p < v->nrec && by[p] && by[p]->dir) { link_child(by[p], n); if (by[p]->sys && p != 5) n->sys = 1; }
        else { klog("ntfs: record %u has no parent directory", r); free_node_tree(n); }
    }
    kfree(by); kfree(parent);
    klog("ntfs: mounted \"%s\" serial %08x, %u records, %u MB free of %u MB", v->label, (uint32_t)v->serial64, v->nrec,
         (uint32_t)(v->free_clusters * CLUSTER >> 20), (uint32_t)(v->nclus * CLUSTER >> 20));
    return v;
fail:
    klog("ntfs: the volume at sector %u could not be mounted", lba);
    ntfs_unmount(v);
    return NULL;
}

/* ---------------- format ---------------- */
static const struct { const char *name; uint32_t type, display, collation, flags; uint64_t minsz, maxsz; } attrdefs[] = {
    { "$STANDARD_INFORMATION", 0x10, 0, 0, 0x40, 0x30, 0x48 }, { "$ATTRIBUTE_LIST", 0x20, 0, 0, 0x80, 0, ~0ull },
    { "$FILE_NAME", 0x30, 0, 0, 0x42, 0x44, 0x242 }, { "$OBJECT_ID", 0x40, 0, 0, 0x40, 0, 0x100 },
    { "$SECURITY_DESCRIPTOR", 0x50, 0, 0, 0x80, 0, ~0ull }, { "$VOLUME_NAME", 0x60, 0, 0, 0x40, 2, 0x100 },
    { "$VOLUME_INFORMATION", 0x70, 0, 0, 0x40, 0xC, 0xC }, { "$DATA", 0x80, 0, 0, 0, 0, ~0ull },
    { "$INDEX_ROOT", 0x90, 0, 0, 0x40, 0, ~0ull }, { "$INDEX_ALLOCATION", 0xA0, 0, 0, 0x80, 0, ~0ull },
    { "$BITMAP", 0xB0, 0, 0, 0x80, 0, ~0ull }, { "$REPARSE_POINT", 0xC0, 0, 0, 0x80, 0, 0x4000 },
    { "$EA_INFORMATION", 0xD0, 0, 0, 0x40, 8, 8 }, { "$EA", 0xE0, 0, 0, 0, 0, 0x10000 },
    { "$LOGGED_UTILITY_STREAM", 0x100, 0, 0, 0x80, 0, 0x10000 },
};
static bool zero_clusters(ntfsvol_t *v, uint64_t lcn, uint64_t n, uint8_t fill) {
    uint8_t *z = kmalloc(64 * 1024); memset(z, fill, 64 * 1024);
    bool ok = true;
    for (uint64_t c = 0; c < n && ok; c += 16) ok = sec_io(v, (lcn + c) * SPC, (uint32_t)(MIN(16, n - c) * SPC), z, true);
    kfree(z);
    return ok;
}
static bool write_clusters(ntfsvol_t *v, uint64_t lcn, const void *data, uint32_t len) {
    uint32_t secs = (len + 511) / 512;
    uint8_t *t = kzalloc((size_t)((len + CLUSTER - 1) / CLUSTER) * CLUSTER);
    memcpy(t, data, len);
    bool ok = sec_io(v, lcn * SPC, secs, t, true);
    kfree(t);
    return ok;
}
bool ntfs_format(blkdev_t *dev, uint32_t lba, uint32_t sectors, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap) {
    if (sectors < 64 * 2048) { snprintf(err, errcap, "The partition is too small for NTFS (needs 64 MB)."); return false; }
    ntfsvol_t *v = kzalloc(sizeof *v);
    v->dev = dev; v->lba = lba;
    v->total_sectors = sectors - 1;                             /* the last sector holds the backup boot sector */
    v->nclus = v->total_sectors / SPC;
    v->serial64 = ((uint64_t)rand_next() << 32) | serial;
    v->bm_bytes = (uint32_t)((v->nclus + 63) / 64 * 8);
    v->bm = kzalloc((v->bm_bytes + 1023) & ~511u);
    v->free_clusters = v->nclus;
    for (uint64_t c = v->nclus; c < (uint64_t)v->bm_bytes * 8; c++) v->bm[c >> 3] |= 1 << (c & 7);   /* past the end: in use */
    if (cb) cb(2, "Creating the file system");
    if (full) {                                                 /* full format: write every cluster */
        for (uint64_t c = 0; c < v->nclus; c += 256) {
            if (!zero_clusters(v, c, MIN(256, v->nclus - c), 0)) { snprintf(err, errcap, "Setup could not write to the disk (cluster %u).", (uint32_t)c); goto fail; }
            if (cb && !(c % 4096)) cb(2 + (int)(c * 80 / v->nclus), "Formatting");
        }
    }
    /* layout */
    v->nrec = v->nclus >= 131072 ? 16384 : 4096;
    uint64_t mft_clus = (uint64_t)v->nrec * RECSZ / CLUSTER;
    uint64_t mft_lcn = 16;
    uint32_t mftbm_bytes = (v->nrec / 8 + 7) & ~7u;
    uint64_t mftbm_lcn = mft_lcn + mft_clus;
    uint64_t log_clus = v->nclus >= 262144 ? 4096 : 1024;       /* 16 MB / 4 MB */
    uint64_t log_lcn = mftbm_lcn + 1;
    uint64_t attrdef_lcn = log_lcn + log_clus;
    uint64_t bm_clus = (v->bm_bytes + CLUSTER - 1) / CLUSTER, bm_lcn = attrdef_lcn + 1;
    uint64_t upcase_lcn = bm_lcn + bm_clus;                     /* 32 clusters */
    uint64_t mirr_lcn = v->nclus / 2;
    if (upcase_lcn + 32 >= mirr_lcn) { snprintf(err, errcap, "The partition is too small for NTFS."); goto fail; }
    #define USE(l, n) do { for (uint64_t _c = 0; _c < (n); _c++) bm_set(v, (l) + _c, true); } while (0)
    USE(0, 2); USE(mft_lcn, mft_clus); USE(mftbm_lcn, 1); USE(log_lcn, log_clus); USE(attrdef_lcn, 1);
    USE(bm_lcn, bm_clus); USE(upcase_lcn, 32); USE(mirr_lcn, 1);
    v->hint = upcase_lcn + 32;
    v->mft_runs[0] = (run_t){ mft_lcn, mft_clus }; v->nmft_runs = 1;
    v->mftbm_runs[0] = (run_t){ mftbm_lcn, 1 }; v->nmftbm_runs = 1;
    v->bm_runs[0] = (run_t){ bm_lcn, bm_clus }; v->nbm_runs = 1;
    v->mftbm_bytes = mftbm_bytes;
    v->mftbm = kzalloc(CLUSTER);
    for (int r = 0; r < 16; r++) v->mftbm[r >> 3] |= 1 << (r & 7);
    /* empty records for the whole MFT */
    if (cb) cb(84, "Creating the master file table");
    {
        uint8_t *chunk = kmalloc(64 * 1024);
        for (uint32_t r = 0; r < v->nrec; r += 64) {
            for (int k = 0; k < 64; k++) { uint8_t *b = chunk + k * RECSZ; rec_init(b, r + k, 1, 0); *(uint16_t *)(b + 0x10) = 0; fixup_apply(v, b, RECSZ); }
            if (!runs_io(v, v->mft_runs, 1, (uint64_t)r * RECSZ, 64 * 1024, chunk, true)) { kfree(chunk); snprintf(err, errcap, "Setup could not write the master file table."); goto fail; }
        }
        kfree(chunk);
    }
    /* system file contents */
    if (!zero_clusters(v, log_lcn, log_clus, 0xFF)) { snprintf(err, errcap, "Setup could not write $LogFile."); goto fail; }   /* empty log */
    uint8_t *ad = kzalloc(CLUSTER);
    for (unsigned i = 0; i < ARRAY_LEN(attrdefs); i++) {
        uint8_t *e = ad + i * 160;
        for (int k = 0; attrdefs[i].name[k]; k++) *(uint16_t *)(e + 2 * k) = (uint8_t)attrdefs[i].name[k];
        *(uint32_t *)(e + 0x80) = attrdefs[i].type; *(uint32_t *)(e + 0x84) = attrdefs[i].display;
        *(uint32_t *)(e + 0x88) = attrdefs[i].type == AT_FN ? 1 : attrdefs[i].collation;
        *(uint32_t *)(e + 0x8C) = attrdefs[i].flags; *(uint64_t *)(e + 0x90) = attrdefs[i].minsz; *(uint64_t *)(e + 0x98) = attrdefs[i].maxsz;
    }
    uint32_t ad_len = ARRAY_LEN(attrdefs) * 160 + 160;
    write_clusters(v, attrdef_lcn, ad, CLUSTER); kfree(ad);
    uint16_t *up = kmalloc(65536 * 2);
    for (uint32_t c = 0; c < 65536; c++) up[c] = upcase16(c);
    write_clusters(v, upcase_lcn, up, 65536 * 2); kfree(up);
    /* system file records */
    static const char *names[16] = { "$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".", "$Bitmap", "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend" };
    uint64_t now = ntfs_now();
    nnode_t *root = kzalloc(sizeof *root);
    root->rec = 5; root->seq = 5; root->dir = 1; root->sys = 1; root->fattr = 0x06; root->ctime = root->mtime = now; set_name(root, ".");
    v->root = root;
    nnode_t *sys[12] = { 0 };
    for (int r = 0; r < 12; r++) {
        if (r == 5) { sys[r] = root; continue; }
        nnode_t *n = kzalloc(sizeof *n);
        n->rec = r; n->seq = r ? r : 1; n->sys = 1; n->fattr = 0x06; n->ctime = n->mtime = now; n->dir = r == 11;
        set_name(n, names[r]); link_child(root, n); sys[r] = n;
    }
    struct { int rec; run_t run; uint64_t size; } nr[] = {
        { 0, { mft_lcn, mft_clus }, mft_clus * CLUSTER }, { 1, { mirr_lcn, 1 }, 4 * RECSZ }, { 2, { log_lcn, log_clus }, log_clus * CLUSTER },
        { 4, { attrdef_lcn, 1 }, ad_len }, { 6, { bm_lcn, bm_clus }, v->bm_bytes }, { 7, { 0, 2 }, 8192 }, { 10, { upcase_lcn, 32 }, 131072 } };
    for (unsigned i = 0; i < ARRAY_LEN(nr); i++) { nnode_t *n = sys[nr[i].rec]; n->size = nr[i].size; n->alloc = nr[i].run.len * CLUSTER; }
    bool ok = true;
    for (int r = 0; r < 12 && ok; r++) {
        if (r == 5 || r == 11) continue;                        /* directories: written by rebuild_dir */
        nnode_t *n = sys[r];
        uint8_t *b = v->rec;
        rec_init(b, r, n->seq, 1);
        ok = rec_common(b, n);
        for (unsigned i = 0; i < ARRAY_LEN(nr) && ok; i++) if (nr[i].rec == r) {
            ok = add_nonresident(b, AT_DATA, NULL, 0, &nr[i].run, 1, nr[i].size);
            if (r == 0) { run_t mb = { mftbm_lcn, 1 }; ok = ok && add_nonresident(b, AT_BITMAP, NULL, 0, &mb, 1, mftbm_bytes); }
        }
        if (r == 3) {                                           /* $Volume */
            uint16_t ln[32]; int ll = 0; for (; label[ll] && ll < 32; ll++) ln[ll] = (uint8_t)label[ll];
            uint8_t vi[12] = { 0 }; vi[8] = 3; vi[9] = 1;     /* NTFS 3.1 */
            ok = ok && add_resident(b, AT_VOLNAME, NULL, 0, ln, ll * 2, 0) && add_resident(b, AT_VOLINFO, NULL, 0, vi, 12, 0) &&
                 add_resident(b, AT_DATA, NULL, 0, NULL, 0, 0);
        }
        if (r == 8) {                                           /* $BadClus: empty $DATA and a sparse $Bad */
            static const uint16_t bad[4] = { '$', 'B', 'a', 'd' };
            run_t sp = { ~0ull, v->nclus };
            ok = ok && add_resident(b, AT_DATA, NULL, 0, NULL, 0, 0) && add_nonresident(b, AT_DATA, bad, 4, &sp, 1, v->nclus * CLUSTER);
            if (ok) { uint8_t *a = find_attr(b, AT_DATA, "$Bad"); if (a) *(uint64_t *)(a + 0x38) = 0; }
        }
        if (r == 9) {                                           /* $Secure: empty $SDS, $SDH and $SII indexes */
            static const uint16_t sds[4] = { '$', 'S', 'D', 'S' }, sdh[4] = { '$', 'S', 'D', 'H' }, sii[4] = { '$', 'S', 'I', 'I' };
            uint8_t ir[0x30] = { 0 };
            *(uint32_t *)(ir + 8) = IDXSZ; ir[12] = 1;
            *(uint32_t *)(ir + 0x10) = 0x10; *(uint32_t *)(ir + 0x14) = 0x20; *(uint32_t *)(ir + 0x18) = 0x20;
            *(uint16_t *)(ir + 0x20 + 8) = 0x10; *(uint16_t *)(ir + 0x20 + 12) = 2;   /* end entry */
            ok = ok && add_resident(b, AT_DATA, sds, 4, NULL, 0, 0);
            *(uint32_t *)(ir + 4) = 0x12; ok = ok && add_resident(b, AT_IROOT, sdh, 4, ir, 0x30, 0);   /* COLLATION_NTOFS_SECURITY_HASH */
            *(uint32_t *)(ir + 4) = 0x10; ok = ok && add_resident(b, AT_IROOT, sii, 4, ir, 0x30, 0);   /* COLLATION_NTOFS_ULONG */
            *(uint16_t *)(b + 0x16) = 1 | 4;                    /* in use, view index */
        }
        if (r == 2 || r == 4 || r == 7 || r == 10 || r == 1 || r == 6) {}
        ok = ok && write_rec(v, r, b);
    }
    for (int r = 12; r < 16 && ok; r++) { uint8_t *b = v->rec; rec_init(b, r, r, 0); ok = write_rec(v, r, b); }
    sys[11]->dirty = 1; root->dirty = 1;
    if (ok) ok = rebuild_dir(v, sys[11]) && rebuild_dir(v, root);
    if (!ok) { snprintf(err, errcap, "Setup could not create the NTFS system files."); goto fail; }
    /* $MFTMirr: the first four records */
    uint8_t mirr[4 * RECSZ];
    ok = runs_io(v, v->mft_runs, 1, 0, sizeof mirr, mirr, false) && write_clusters(v, mirr_lcn, mirr, sizeof mirr);
    /* boot sector (and its backup in the partition's last sector) */
    uint8_t bs[512] = { 0 };
    bs[0] = 0xEB; bs[1] = 0x52; bs[2] = 0x90; memcpy(bs + 3, "NTFS    ", 8);
    *(uint16_t *)(bs + 0x0B) = 512; bs[0x0D] = SPC; bs[0x15] = 0xF8;
    *(uint16_t *)(bs + 0x18) = 63; *(uint16_t *)(bs + 0x1A) = 255; *(uint32_t *)(bs + 0x1C) = lba;
    bs[0x24] = 0x80; bs[0x26] = 0x80;
    *(uint64_t *)(bs + 0x28) = v->total_sectors; *(uint64_t *)(bs + 0x30) = mft_lcn; *(uint64_t *)(bs + 0x38) = mirr_lcn;
    bs[0x40] = (uint8_t)-10; bs[0x44] = 1; *(uint64_t *)(bs + 0x48) = v->serial64;
    static const char msg[] = "This is an NTFS volume of THE WRLD OS; it is started by GRUB.";
    memcpy(bs + 0x54, msg, sizeof msg);
    bs[510] = 0x55; bs[511] = 0xAA;
    uint8_t *boot = kzalloc(8192); memcpy(boot, bs, 512);
    ok = ok && write_clusters(v, 0, boot, 8192) && blk_write(dev, lba + sectors - 1, 1, bs);
    kfree(boot);
    v->mftbm_dirty = v->bm_dirty = true;
    ntfs_sync(v);                                               /* bitmaps */
    if (!ok) { snprintf(err, errcap, "Setup could not write the boot sector."); goto fail; }
    if (cb) cb(100, "Done");
    klog("ntfs: formatted %u MB, %u clusters of 4 KB, %u MFT records", (uint32_t)(v->nclus * CLUSTER >> 20), (uint32_t)v->nclus, v->nrec);
    ntfs_unmount(v);
    return true;
fail:
    ntfs_unmount(v);
    return false;
}
