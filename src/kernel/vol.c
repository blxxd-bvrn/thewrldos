/* Volumes: one interface over the FAT32 and NTFS drivers, used by Setup and by
 * persistence (the installed system's C: drive). */
#include "kernel.h"
#include "disk.h"

typedef struct ntfsvol ntfsvol_t;
ntfsvol_t *ntfs_mount(blkdev_t *dev, uint32_t lba, uint32_t sectors);
void ntfs_unmount(ntfsvol_t *v);
bool ntfs_format(blkdev_t *dev, uint32_t lba, uint32_t sectors, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap);
uint32_t ntfs_serial(ntfsvol_t *v);
const char *ntfs_label(ntfsvol_t *v);
uint64_t ntfs_free_bytes(ntfsvol_t *v);
uint64_t ntfs_total_bytes(ntfsvol_t *v);
bool ntfs_exists(ntfsvol_t *v, const char *path);
bool ntfs_mkdir(ntfsvol_t *v, const char *path);
bool ntfs_write(ntfsvol_t *v, const char *path, const void *data, uint32_t len, bool readonly);
bool ntfs_delete(ntfsvol_t *v, const char *path);
bool ntfs_rename(ntfsvol_t *v, const char *from, const char *to);
uint8_t *ntfs_read(ntfsvol_t *v, const char *path, uint32_t *len);
int ntfs_load_tree(ntfsvol_t *v, vnode_t *root);
void ntfs_sync(ntfsvol_t *v);

struct vol { int kind; void *fs; };
void (*vol_load_progress)(uint64_t done, uint64_t total);

vol_t *vol_mount(blkdev_t *d, uint32_t lba, uint32_t sectors) {
    void *fs = fat32_mount(d, lba, sectors);
    int kind = FS_FAT32;
    if (!fs) { fs = ntfs_mount(d, lba, sectors); kind = FS_NTFS; }
    if (!fs) return NULL;
    vol_t *v = kzalloc(sizeof *v); v->kind = kind; v->fs = fs;
    return v;
}
void vol_unmount(vol_t *v) {
    if (!v) return;
    if (v->kind == FS_NTFS) ntfs_unmount(v->fs); else fat32_unmount(v->fs);
    kfree(v);
}
bool vol_format(int kind, blkdev_t *d, uint32_t lba, uint32_t sectors, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap) {
    if (kind == FS_NTFS) return ntfs_format(d, lba, sectors, label, full, serial, cb, err, errcap);
    return fat32_format(d, lba, sectors, label, full, serial, cb, err, errcap);
}
int vol_kind(vol_t *v) { return v->kind; }
const char *vol_fs_name(vol_t *v) { return v->kind == FS_NTFS ? "NTFS" : "FAT32"; }
uint8_t vol_part_type(int kind) { return kind == FS_NTFS ? 0x07 : 0x0C; }
#define DISPATCH(fat, ntfs) (v->kind == FS_NTFS ? (ntfs) : (fat))
uint32_t vol_serial(vol_t *v) { return DISPATCH(fat32_serial(v->fs), ntfs_serial(v->fs)); }
const char *vol_label(vol_t *v) { return DISPATCH(fat32_label(v->fs), ntfs_label(v->fs)); }
uint64_t vol_free_bytes(vol_t *v) { return DISPATCH(fat32_free_bytes(v->fs), ntfs_free_bytes(v->fs)); }
uint64_t vol_total_bytes(vol_t *v) { return DISPATCH(fat32_total_bytes(v->fs), ntfs_total_bytes(v->fs)); }
bool vol_exists(vol_t *v, const char *p) { return DISPATCH(fat32_exists(v->fs, p), ntfs_exists(v->fs, p)); }
bool vol_mkdir(vol_t *v, const char *p) { return DISPATCH(fat32_mkdir(v->fs, p), ntfs_mkdir(v->fs, p)); }
bool vol_write(vol_t *v, const char *p, const void *d, uint32_t n, bool ro) { return DISPATCH(fat32_write(v->fs, p, d, n, ro), ntfs_write(v->fs, p, d, n, ro)); }
bool vol_delete(vol_t *v, const char *p) { return DISPATCH(fat32_delete(v->fs, p), ntfs_delete(v->fs, p)); }
bool vol_rename(vol_t *v, const char *a, const char *b) { return DISPATCH(fat32_rename(v->fs, a, b), ntfs_rename(v->fs, a, b)); }
uint8_t *vol_read(vol_t *v, const char *p, uint32_t *n) { return DISPATCH(fat32_read(v->fs, p, n), ntfs_read(v->fs, p, n)); }
int vol_load_tree(vol_t *v, vnode_t *root) { return DISPATCH(fat32_load_tree(v->fs, root), ntfs_load_tree(v->fs, root)); }
void vol_sync(vol_t *v) { if (v->kind == FS_NTFS) ntfs_sync(v->fs); else fat32_sync(v->fs); }
