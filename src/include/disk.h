#ifndef DISK_H
#define DISK_H
#include "kernel.h"
#include "fs.h"

/* ---- block devices (ATA/IDE, PIO) ---- */
typedef struct {
    int index;                 /* Disk 0, 1, ... */
    int bus, slave;            /* 0 primary / 1 secondary, 0 master / 1 slave */
    uint16_t io, ctl;
    bool lba48;
    bool dma;                  /* bus-master IDE DMA available */
    uint32_t sectors;          /* 512-byte sectors */
    char model[41];
} blkdev_t;

void ata_init(void);
int blk_count(void);
blkdev_t *blk_get(int i);
bool blk_read(blkdev_t *d, uint32_t lba, uint32_t count, void *buf);
bool blk_write(blkdev_t *d, uint32_t lba, uint32_t count, const void *buf);
bool blk_zero(blkdev_t *d, uint32_t lba, uint32_t count);

/* ---- MBR partition tables ---- */
typedef struct { uint8_t type, boot; uint32_t lba, sectors; } part_t;
int mbr_read(blkdev_t *d, part_t parts[4]);     /* returns 1 if a valid MBR is present */
bool mbr_write(blkdev_t *d, const part_t parts[4], const uint8_t *bootcode /* 440 bytes or NULL */);
const char *part_type_name(uint8_t type);

/* ---- writable FAT32 volumes ---- */
typedef struct fatvol fatvol_t;
typedef void (*progress_fn)(int pct, const char *what);
bool fat32_format(blkdev_t *d, uint32_t lba, uint32_t sectors, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap);
fatvol_t *fat32_mount(blkdev_t *d, uint32_t lba, uint32_t sectors);
void fat32_unmount(fatvol_t *v);
uint32_t fat32_serial(fatvol_t *v);
const char *fat32_label(fatvol_t *v);
uint64_t fat32_free_bytes(fatvol_t *v);
uint64_t fat32_total_bytes(fatvol_t *v);
blkdev_t *fat32_dev(fatvol_t *v);
/* paths are relative to the volume root, '\\' or '/' separated */
bool fat32_exists(fatvol_t *v, const char *path);
bool fat32_mkdir(fatvol_t *v, const char *path);                 /* mkdir -p */
bool fat32_write(fatvol_t *v, const char *path, const void *data, uint32_t len, bool readonly);  /* create/replace */
bool fat32_delete(fatvol_t *v, const char *path);                /* recursive */
bool fat32_rename(fatvol_t *v, const char *from, const char *to);
uint8_t *fat32_read(fatvol_t *v, const char *path, uint32_t *len);
int fat32_load_tree(fatvol_t *v, vnode_t *root);                 /* materialise into the VFS; returns files */
void fat32_sync(fatvol_t *v);
/* ---- volumes: FAT32 or NTFS behind one interface (vol.c, ntfs.c) ---- */
typedef struct vol vol_t;
enum { FS_FAT32 = 1, FS_NTFS = 2 };
extern void (*vol_load_progress)(uint64_t done, uint64_t total);   /* called while a volume's files are loaded */
vol_t *vol_mount(blkdev_t *d, uint32_t lba, uint32_t sectors);       /* FAT32 or NTFS */
void vol_unmount(vol_t *v);
bool vol_format(int kind, blkdev_t *d, uint32_t lba, uint32_t sectors, const char *label, bool full, uint32_t serial, progress_fn cb, char *err, int errcap);
int vol_kind(vol_t *v);
const char *vol_fs_name(vol_t *v);
uint8_t vol_part_type(int kind);                                      /* MBR partition type */
uint32_t vol_serial(vol_t *v);
const char *vol_label(vol_t *v);
uint64_t vol_free_bytes(vol_t *v);
uint64_t vol_total_bytes(vol_t *v);
bool vol_exists(vol_t *v, const char *path);
bool vol_mkdir(vol_t *v, const char *path);
bool vol_write(vol_t *v, const char *path, const void *data, uint32_t len, bool readonly);
bool vol_delete(vol_t *v, const char *path);
bool vol_rename(vol_t *v, const char *from, const char *to);
uint8_t *vol_read(vol_t *v, const char *path, uint32_t *len);
int vol_load_tree(vol_t *v, vnode_t *root);
void vol_sync(vol_t *v);

/* ---- persistence: installed systems keep C: on a FAT32 partition ---- */
extern vol_t *system_volume;        /* NULL when running live from CD/RAM */
extern int system_disk_index, system_part_index;
bool persist_boot_from_disk(uint32_t serial, char *err, int errcap);
void persist_start(void);           /* enable write-back + background flush task */
void persist_flush(void);           /* write everything dirty now (shutdown/restart) */
int persist_pending(void);
void persist_status(char *buf, int cap);   /* "Disk 0, Partition 1: ..." or live session */
void settings_load(void);
void settings_save(void);
const char *settings_get(const char *key);
void settings_set(const char *key, const char *val);
#define SETTINGS_FILE "C:\\WRLD\\wrld.ini"

/* ---- setup ---- */
extern bool boot_setup_mode;        /* "setup" on the kernel command line */
extern uint32_t boot_root_serial;   /* "root=XXXXXXXX" */
void setup_text_mode(bool from_desktop);   /* blue text-mode installer */
bool setup_oobe_pending(void);
void setup_oobe(void);              /* first-boot "Finishing setup" wizard */
void setup_launch_from_desktop(void);
#endif
