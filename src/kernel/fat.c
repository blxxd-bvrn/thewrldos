/* Read-only FAT12/16/32 driver (with VFAT long names). Mounted volumes are
 * materialised into the in-memory VFS. */
#include "kernel.h"
#include "fs.h"

typedef struct PACKED {
    uint8_t jmp[3]; char oem[8];
    uint16_t bps; uint8_t spc; uint16_t reserved; uint8_t nfats; uint16_t root_entries;
    uint16_t total16; uint8_t media; uint16_t fatsz16; uint16_t spt, heads; uint32_t hidden, total32;
    union {
        struct PACKED { uint8_t drv, r1, sig; uint32_t serial; char label[11]; char fstype[8]; } f16;
        struct PACKED { uint32_t fatsz32; uint16_t flags, ver; uint32_t root_clus; uint16_t fsinfo, backup; uint8_t r[12]; uint8_t drv, r1, sig; uint32_t serial; char label[11]; char fstype[8]; } f32;
    };
} bpb_t;
typedef struct PACKED { char name[11]; uint8_t attr, ntres, ctime_tenth; uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus_lo; uint32_t size; } dirent_t;

typedef struct { const uint8_t *img; uint32_t size; int type; uint32_t bps, spc, fat_off, data_off, root_off, root_entries, root_clus, nclus; } fat_t;

static uint32_t fat_next(fat_t *f, uint32_t c) {
    const uint8_t *fat = f->img + f->fat_off;
    if (f->type == 12) { uint32_t o = c + c / 2; uint16_t v = fat[o] | (fat[o + 1] << 8); v = (c & 1) ? v >> 4 : v & 0xFFF; return v >= 0xFF8 ? 0xFFFFFFFF : v; }
    if (f->type == 16) { uint16_t v = ((const uint16_t *)fat)[c]; return v >= 0xFFF8 ? 0xFFFFFFFF : v; }
    uint32_t v = ((const uint32_t *)fat)[c] & 0x0FFFFFFF; return v >= 0x0FFFFFF8 ? 0xFFFFFFFF : v;
}
static const uint8_t *clus_ptr(fat_t *f, uint32_t c) {
    uint32_t off = f->data_off + (c - 2) * f->spc * f->bps;
    return off < f->size ? f->img + off : NULL;
}
static uint8_t *read_chain(fat_t *f, uint32_t c, uint32_t size, uint32_t *outlen) {
    uint32_t csz = f->spc * f->bps;
    uint32_t cap = size ? size : 0, len = 0;
    if (!size) { /* directory: count chain */
        for (uint32_t k = c, n = 0; k >= 2 && k < 0xFFFFFFF0 && n < 65536; k = fat_next(f, k), n++) cap += csz;
    }
    uint8_t *buf = kmalloc(cap + 1);
    for (uint32_t k = c, n = 0; k >= 2 && k < 0xFFFFFFF0 && len < cap && n < 1000000; k = fat_next(f, k), n++) {
        const uint8_t *p = clus_ptr(f, k); if (!p) break;
        uint32_t cp = MIN(csz, cap - len);
        memcpy(buf + len, p, cp); len += cp;
    }
    *outlen = len;
    return buf;
}
static void dos_time(rtc_time_t *t, uint16_t d, uint16_t tm) {
    t->year = 1980 + (d >> 9); t->month = (d >> 5) & 15; t->day = d & 31;
    t->hour = tm >> 11; t->minute = (tm >> 5) & 63; t->second = (tm & 31) * 2; t->weekday = 0;
}
static void load_dir(fat_t *f, const uint8_t *ents, uint32_t n, vnode_t *parent, int depth) {
    char lfn[260]; int lfn_ok = 0;
    if (depth > 16) return;
    for (uint32_t i = 0; i < n; i++) {
        const dirent_t *e = (const dirent_t *)(ents + i * 32);
        if ((uint8_t)e->name[0] == 0) break;
        if ((uint8_t)e->name[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e->attr == 0x0F) {
            const uint8_t *r = (const uint8_t *)e;
            int seq = r[0] & 0x1F;
            if (r[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; }
            static const int offs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) {
                uint16_t ch = r[offs[k]] | (r[offs[k] + 1] << 8);
                int pos = (seq - 1) * 13 + k;
                if (pos < 259 && ch && ch != 0xFFFF) lfn[pos] = ch < 128 ? ch : '_';
            }
            continue;
        }
        if (e->attr & 0x08) { lfn_ok = 0; continue; }   /* volume label */
        char name[96];
        if (lfn_ok && lfn[0]) strlcpy(name, lfn, sizeof name);
        else {
            int k = 0;
            for (int j = 0; j < 8 && e->name[j] != ' '; j++) name[k++] = (e->ntres & 8) ? tolower(e->name[j]) : e->name[j];
            if (e->name[8] != ' ') { name[k++] = '.'; for (int j = 8; j < 11 && e->name[j] != ' '; j++) name[k++] = (e->ntres & 16) ? tolower(e->name[j]) : e->name[j]; }
            name[k] = 0;
        }
        lfn_ok = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        uint32_t clus = ((uint32_t)e->clus_hi << 16) | e->clus_lo;
        vnode_t *v = vfs_new_node(parent, name, (e->attr & 0x10) != 0);
        dos_time(&v->mtime, e->mdate, e->mtime);
        if (v->dir) {
            uint32_t len; uint8_t *d = read_chain(f, clus, 0, &len);
            load_dir(f, d, len / 32, v, depth + 1);
            kfree(d);
        } else if (e->size && clus >= 2) {
            uint32_t len; v->data = read_chain(f, clus, e->size, &len);
            v->size = len; v->cap = e->size;
        }
        v->readonly = true;
    }
}
vnode_t *fat_mount_image(const uint8_t *img, uint32_t size, char *label, int lcap, char *err, int ecap) {
    if (size < 512) { snprintf(err, ecap, "Image too small"); return NULL; }
    const bpb_t *b = (const bpb_t *)img;
    if (img[510] != 0x55 || img[511] != 0xAA || !b->bps || !b->spc || !b->nfats) { snprintf(err, ecap, "No FAT boot sector found"); return NULL; }
    fat_t f = { img, size, 0, b->bps, b->spc, 0, 0, 0, b->root_entries, 0, 0 };
    uint32_t fatsz = b->fatsz16 ? b->fatsz16 : b->f32.fatsz32;
    uint32_t total = b->total16 ? b->total16 : b->total32;
    uint32_t root_secs = (b->root_entries * 32 + b->bps - 1) / b->bps;
    uint32_t data_sec = b->reserved + b->nfats * fatsz + root_secs;
    f.nclus = (total - data_sec) / b->spc;
    f.type = f.nclus < 4085 ? 12 : f.nclus < 65525 ? 16 : 32;
    f.fat_off = b->reserved * b->bps;
    f.root_off = (b->reserved + b->nfats * fatsz) * b->bps;
    f.data_off = data_sec * b->bps;
    char lab[12];
    memcpy(lab, f.type == 32 ? b->f32.label : b->f16.label, 11); lab[11] = 0;
    for (int i = 10; i >= 0 && lab[i] == ' '; i--) lab[i] = 0;
    if (!lab[0] || !strncmp(lab, "NO NAME", 7)) strcpy(lab, "Untitled");
    strlcpy(label, lab, lcap);
    vnode_t *root = vfs_new_node(NULL, "X:", true);
    if (f.type == 32) {
        uint32_t len; uint8_t *d = read_chain(&f, b->f32.root_clus, 0, &len);
        load_dir(&f, d, len / 32, root, 0); kfree(d);
    } else load_dir(&f, img + f.root_off, b->root_entries, root, 0);
    root->readonly = true;
    klog("fat: mounted FAT%d volume '%s' (%u clusters)", f.type, lab, f.nclus);
    return root;
}
