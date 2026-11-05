#ifndef FS_H
#define FS_H
#include "kernel.h"

typedef struct vnode {
    char name[96];
    bool dir;
    bool readonly;
    uint8_t *data;
    uint32_t size, cap;
    struct vnode *parent, *child, *next;
    rtc_time_t mtime;
    int drive;
} vnode_t;

void vfs_init(void);
vnode_t *vfs_root(int drive);                        /* 'C' etc. */
char vfs_drive_letter(vnode_t *n);
const char *vfs_drive_label(char letter);
int vfs_drive_kind(char letter);                     /* 0 none, 1 hdd, 2 disk image */
char vfs_mount(vnode_t *root, const char *label, int kind);   /* returns letter */
bool vfs_unmount(char letter);
vnode_t *vfs_resolve(const char *path, const char *cwd);
vnode_t *vfs_lookup_child(vnode_t *dir, const char *name);
vnode_t *vfs_new_node(vnode_t *parent, const char *name, bool dir);
vnode_t *vfs_create(const char *path, const char *cwd, bool dir);
vnode_t *vfs_mkdir_p(const char *path);
bool vfs_write_node(vnode_t *n, const void *data, uint32_t len);
bool vfs_append_node(vnode_t *n, const void *data, uint32_t len);
vnode_t *vfs_write_file(const char *path, const void *data, uint32_t len);
uint8_t *vfs_read_file(const char *path, uint32_t *size);
bool vfs_delete(vnode_t *n);
bool vfs_rename(vnode_t *n, const char *newname);
bool vfs_move(vnode_t *n, vnode_t *newdir);
vnode_t *vfs_copy(vnode_t *n, vnode_t *destdir, const char *newname);
void vfs_path(vnode_t *n, char *buf, int cap);
int vfs_count_children(vnode_t *dir);
void vfs_sort_children(vnode_t *dir);
const char *vfs_ext(const char *name);
uint32_t vfs_tree_size(vnode_t *n);
extern uint32_t vfs_generation;    /* bumps on every change */

void path_join(char *out, int cap, const char *a, const char *b);
void path_normalize(char *p);
const char *path_basename(const char *p);
void path_dirname(const char *p, char *out, int cap);

/* initrd + disk formats */
void tar_extract(const uint8_t *data, uint32_t size, vnode_t *dest);
vnode_t *fat_mount_image(const uint8_t *img, uint32_t size, char *label_out, int label_cap, char *err, int errcap);
uint8_t *dmg_extract(const uint8_t *file, uint32_t size, uint32_t *out_size, char *info, int infocap);
char dmg_mount(vnode_t *file, char *msg, int msgcap);
int inflate_zlib(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap, uint32_t *outlen);
int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t dstcap, uint32_t *outlen);

#define USER_HOME "C:\\Documents and Settings\\User"
#define USER_DESKTOP USER_HOME "\\Desktop"
#define USER_DOCS USER_HOME "\\My Documents"
#endif
