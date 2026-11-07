/* In-memory hierarchical filesystem with drive letters */
#include "kernel.h"
#include "fs.h"

static vnode_t *drives[26];
static char drive_labels[26][32];
static int drive_kinds[26];
uint32_t vfs_generation;
void (*vfs_hook)(int op, vnode_t *n, const char *oldpath);
#define HOOK(op, n, old) do { if (vfs_hook) vfs_hook(op, n, old); } while (0)

const char *vfs_ext(const char *name) { const char *d = strrchr(name, '.'); return d && d != name ? d + 1 : ""; }

vnode_t *vfs_new_node(vnode_t *parent, const char *name, bool dir) {
    vnode_t *n = kzalloc(sizeof(vnode_t));
    strlcpy(n->name, name, sizeof n->name);
    n->dir = dir;
    rtc_now(&n->mtime);
    if (parent) {
        n->parent = parent; n->drive = parent->drive;
        /* append to keep insertion order */
        vnode_t **pp = &parent->child; while (*pp) pp = &(*pp)->next; *pp = n;
        HOOK(VH_CREATE, n, NULL);
    }
    vfs_generation++;
    return n;
}
void vfs_init(void) {
    drives['C' - 'A'] = vfs_new_node(NULL, "C:", true);
    drives['C' - 'A']->drive = 'C';
    strcpy(drive_labels['C' - 'A'], "Local Disk");
    drive_kinds['C' - 'A'] = 1;
    vfs_mkdir_p("C:\\WRLD\\System32");
    vfs_mkdir_p("C:\\Program Files");
    vfs_mkdir_p(USER_DESKTOP);
    vfs_mkdir_p(USER_DOCS);
    vfs_mkdir_p("C:\\tmp");
}
vnode_t *vfs_root(int d) { d = toupper(d); return (d >= 'A' && d <= 'Z') ? drives[d - 'A'] : NULL; }
char vfs_drive_letter(vnode_t *n) { while (n && n->parent) n = n->parent; return n ? n->drive : 'C'; }
const char *vfs_drive_label(char l) { l = toupper(l); return (l >= 'A' && l <= 'Z') ? drive_labels[l - 'A'] : ""; }
int vfs_drive_kind(char l) { l = toupper(l); return (l >= 'A' && l <= 'Z' && drives[l - 'A']) ? drive_kinds[l - 'A'] : 0; }
static void set_drive(vnode_t *n, int d) { n->drive = d; for (vnode_t *c = n->child; c; c = c->next) set_drive(c, d); }
char vfs_mount(vnode_t *root, const char *label, int kind) {
    for (int i = 'D' - 'A'; i < 26; i++)
        if (!drives[i]) {
            drives[i] = root; root->parent = NULL;
            snprintf(root->name, sizeof root->name, "%c:", 'A' + i);
            set_drive(root, 'A' + i);
            strlcpy(drive_labels[i], label, 32); drive_kinds[i] = kind;
            vfs_generation++;
            return 'A' + i;
        }
    return 0;
}
static void free_tree(vnode_t *n) {
    vnode_t *c = n->child;
    while (c) { vnode_t *nx = c->next; free_tree(c); c = nx; }
    kfree(n->data); kfree(n);
}
bool vfs_unmount(char l) {
    l = toupper(l);
    if (l <= 'C' || l > 'Z' || !drives[l - 'A']) return false;
    free_tree(drives[l - 'A']); drives[l - 'A'] = NULL; vfs_generation++;
    return true;
}

vnode_t *vfs_lookup_child(vnode_t *dir, const char *name) {
    if (!dir || !dir->dir) return NULL;
    for (vnode_t *c = dir->child; c; c = c->next) if (!strcasecmp(c->name, name)) return c;
    return NULL;
}
void path_normalize(char *p) { for (; *p; p++) if (*p == '/') *p = '\\'; }
const char *path_basename(const char *p) {
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}
void path_dirname(const char *p, char *out, int cap) {
    strlcpy(out, p, cap); path_normalize(out);
    char *s = strrchr(out, '\\');
    if (!s) { out[0] = 0; return; }
    if (s == out + 2 && out[1] == ':') s[1] = 0; else *s = 0;
}
void path_join(char *out, int cap, const char *a, const char *b) {
    strlcpy(out, a, cap);
    int l = strlen(out);
    if (l && out[l - 1] != '\\' && out[l - 1] != '/') strlcat(out, "\\", cap);
    strlcat(out, b, cap);
}

/* Accepts: C:\a\b, C:/a, /a/b (current drive), a/b relative, .., ., ~ */
vnode_t *vfs_resolve(const char *path, const char *cwd) {
    if (!path) return NULL;
    char buf[512];
    vnode_t *cur;
    if (path[0] == '~') { snprintf(buf, sizeof buf, "%s%s", USER_HOME, path + 1); path = buf; }
    if (isalpha(path[0]) && path[1] == ':') { cur = vfs_root(path[0]); path += 2; }
    else if (path[0] == '/' || path[0] == '\\') { cur = cwd ? vfs_root(isalpha(cwd[0]) && cwd[1] == ':' ? cwd[0] : 'C') : vfs_root('C'); }
    else { cur = cwd ? vfs_resolve(cwd, NULL) : vfs_root('C'); }
    if (!cur) return NULL;
    char part[128];
    while (*path) {
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        int n = 0;
        while (*path && *path != '/' && *path != '\\' && n < 127) part[n++] = *path++;
        part[n] = 0;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (cur->parent) cur = cur->parent; continue; }
        cur = vfs_lookup_child(cur, part);
        if (!cur) return NULL;
    }
    return cur;
}
void vfs_path(vnode_t *n, char *buf, int cap) {
    if (!n) { strlcpy(buf, "", cap); return; }
    if (!n->parent) { snprintf(buf, cap, "%s\\", n->name); return; }
    const char *parts[64]; int k = 0;
    for (vnode_t *p = n; p && k < 64; p = p->parent) parts[k++] = p->name;
    buf[0] = 0;
    for (int i = k - 1; i >= 0; i--) { strlcat(buf, parts[i], cap); if (i) strlcat(buf, "\\", cap); }
}
vnode_t *vfs_create(const char *path, const char *cwd, bool dir) {
    vnode_t *e = vfs_resolve(path, cwd);
    if (e) return e;
    char full[512], parent[512];
    if (!(isalpha(path[0]) && path[1] == ':') && cwd && path[0] != '/' && path[0] != '\\') path_join(full, sizeof full, cwd, path);
    else strlcpy(full, path, sizeof full);
    path_normalize(full);
    path_dirname(full, parent, sizeof parent);
    vnode_t *p = parent[0] ? vfs_resolve(parent, cwd) : vfs_resolve(cwd ? cwd : "C:\\", NULL);
    if (!p || !p->dir || p->readonly) return NULL;
    const char *base = path_basename(full);
    if (!*base) return NULL;
    return vfs_new_node(p, base, dir);
}
vnode_t *vfs_mkdir_p(const char *path) {
    char buf[512]; strlcpy(buf, path, sizeof buf); path_normalize(buf);
    vnode_t *cur = vfs_root(buf[0]);
    char *p = buf + 2;
    while (*p) {
        while (*p == '\\') p++;
        if (!*p) break;
        char *e = p; while (*e && *e != '\\') e++;
        char save = *e; *e = 0;
        vnode_t *n = vfs_lookup_child(cur, p);
        if (!n) n = vfs_new_node(cur, p, true);
        cur = n; *e = save; p = e;
    }
    return cur;
}
bool vfs_write_node(vnode_t *n, const void *data, uint32_t len) {
    if (!n || n->dir || n->readonly) return false;
    if (n->data && n->size == len && !memcmp(n->data, data, len)) return true;   /* unchanged */
    if (len > n->cap || !n->data) { kfree(n->data); n->cap = len + 64; n->data = kmalloc(n->cap); }
    memcpy(n->data, data, len); n->size = len;
    rtc_now(&n->mtime); vfs_generation++;
    HOOK(VH_WRITE, n, NULL);
    return true;
}
bool vfs_append_node(vnode_t *n, const void *data, uint32_t len) {
    if (!n || n->dir || n->readonly) return false;
    if (n->size + len > n->cap) { n->cap = (n->size + len) * 2 + 64; n->data = krealloc(n->data, n->cap); }
    memcpy(n->data + n->size, data, len); n->size += len;
    rtc_now(&n->mtime); vfs_generation++;
    HOOK(VH_WRITE, n, NULL);
    return true;
}
vnode_t *vfs_write_file(const char *path, const void *data, uint32_t len) {
    vnode_t *n = vfs_create(path, NULL, false);
    if (!n || n->dir) return NULL;
    return vfs_write_node(n, data, len) ? n : NULL;
}
uint8_t *vfs_read_file(const char *path, uint32_t *size) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir) return NULL;
    uint8_t *b = kmalloc(n->size + 1);
    memcpy(b, n->data, n->size); b[n->size] = 0;
    if (size) *size = n->size;
    return b;
}
static void unlink_node(vnode_t *n) {
    if (!n->parent) return;
    for (vnode_t **pp = &n->parent->child; *pp; pp = &(*pp)->next) if (*pp == n) { *pp = n->next; break; }
    n->next = NULL;
}
bool vfs_delete(vnode_t *n) {
    if (!n || !n->parent || n->readonly || (n->parent && n->parent->readonly)) return false;
    HOOK(VH_DELETE, n, NULL);
    unlink_node(n);
    free_tree(n);
    vfs_generation++;
    return true;
}
bool vfs_rename(vnode_t *n, const char *nn) {
    if (!n || !n->parent || n->readonly || !*nn) return false;
    if (vfs_lookup_child(n->parent, nn) && strcasecmp(n->name, nn)) return false;
    char old[512]; vfs_path(n, old, sizeof old);
    strlcpy(n->name, nn, sizeof n->name); vfs_generation++;
    HOOK(VH_RENAME, n, old);
    return true;
}
bool vfs_move(vnode_t *n, vnode_t *nd) {
    if (!n || !nd || !nd->dir || nd->readonly || n->readonly) return false;
    for (vnode_t *p = nd; p; p = p->parent) if (p == n) return false;
    char old[512]; vfs_path(n, old, sizeof old);
    unlink_node(n);
    n->parent = nd;
    vnode_t **pp = &nd->child; while (*pp) pp = &(*pp)->next; *pp = n;
    set_drive(n, nd->drive);
    vfs_generation++;
    HOOK(VH_RENAME, n, old);
    return true;
}
vnode_t *vfs_copy(vnode_t *n, vnode_t *dd, const char *newname) {
    if (!n || !dd || !dd->dir || dd->readonly) return NULL;
    vnode_t *c = vfs_new_node(dd, newname ? newname : n->name, n->dir);
    if (!n->dir) { if (n->size) vfs_write_node(c, n->data, n->size); }
    else for (vnode_t *k = n->child; k; k = k->next) vfs_copy(k, c, NULL);
    return c;
}
int vfs_count_children(vnode_t *d) { int n = 0; for (vnode_t *c = d ? d->child : NULL; c; c = c->next) n++; return n; }
uint32_t vfs_tree_size(vnode_t *n) { uint32_t s = n->size; for (vnode_t *c = n->child; c; c = c->next) s += vfs_tree_size(c); return s; }
void vfs_sort_children(vnode_t *d) {
    /* folders first, then alphabetical (insertion sort on linked list) */
    vnode_t *sorted = NULL;
    vnode_t *c = d->child;
    while (c) {
        vnode_t *nx = c->next;
        vnode_t **pp = &sorted;
        while (*pp && ((*pp)->dir > c->dir || ((*pp)->dir == c->dir && strcasecmp((*pp)->name, c->name) < 0))) pp = &(*pp)->next;
        c->next = *pp; *pp = c;
        c = nx;
    }
    d->child = sorted;
}

/* ---------------- ustar initrd ---------------- */
static uint32_t octal(const char *s, int n) { uint32_t v = 0; for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0'); return v; }
void tar_extract(const uint8_t *data, uint32_t size, vnode_t *dest) {
    uint32_t off = 0; int files = 0;
    char destpath[256]; vfs_path(dest, destpath, sizeof destpath);
    while (off + 512 <= size) {
        const char *h = (const char *)data + off;
        if (!h[0]) break;
        char name[256];
        if (!memcmp(h + 257, "ustar", 5) && h[345]) { snprintf(name, sizeof name, "%.155s/%.100s", h + 345, h); }
        else snprintf(name, sizeof name, "%.100s", h);
        uint32_t fsize = octal(h + 124, 12);
        char type = h[156];
        char *nm = name; if (nm[0] == '.' && nm[1] == '/') nm += 2;
        int l = strlen(nm); if (l && nm[l - 1] == '/') nm[--l] = 0;
        if (l) {
            char full[512]; path_join(full, sizeof full, destpath, nm); path_normalize(full);
            if (type == '5') vfs_mkdir_p(full);
            else if (type == '0' || type == 0) {
                char dir[512]; path_dirname(full, dir, sizeof dir); vfs_mkdir_p(dir);
                vfs_write_file(full, data + off + 512, fsize); files++;
            }
        }
        off += 512 + ((fsize + 511) & ~511u);
    }
    klog("initrd: extracted %d files", files);
}
