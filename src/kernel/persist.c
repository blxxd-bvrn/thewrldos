/* Installed-system persistence. When THE WRLD OS boots from its hard disk the
 * FAT32 system partition is loaded into the C: drive, and every change to C:
 * is written back: directories, deletes and renames immediately, file
 * contents by a background write-back task (and on shutdown/restart).
 * Also keeps the user's settings in C:\WRLD\wrld.ini. */
#include "kernel.h"
#include "disk.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "audio.h"

vol_t *system_volume;
int system_disk_index = -1, system_part_index = -1;
static bool persist_on, fat_changed;
static vnode_t **dirty;
static int ndirty, capdirty;

static bool on_sys(vnode_t *n) { return persist_on && n && vfs_drive_letter(n) == 'C'; }
static void rel_path(vnode_t *n, char *buf, int cap) {
    char full[512]; vfs_path(n, full, sizeof full);
    strlcpy(buf, full[0] && full[1] == ':' ? full + 2 : full, cap);
}
static void mark_dirty(vnode_t *n) {
    if (n->dirty) return;
    if (ndirty == capdirty) { capdirty = capdirty ? capdirty * 2 : 64; dirty = krealloc(dirty, capdirty * sizeof *dirty); }
    n->dirty = true; dirty[ndirty++] = n;
}
static void mark_tree(vnode_t *n) {
    if (n->dir) {
        char p[512]; rel_path(n, p, sizeof p);
        if (vol_mkdir(system_volume, p)) fat_changed = true;
        for (vnode_t *c = n->child; c; c = c->next) mark_tree(c);
    } else mark_dirty(n);
}
static bool is_under(vnode_t *n, vnode_t *top) { for (; n; n = n->parent) if (n == top) return true; return false; }
static void purge(vnode_t *top) {
    int k = 0;
    for (int i = 0; i < ndirty; i++) { if (is_under(dirty[i], top)) dirty[i]->dirty = false; else dirty[k++] = dirty[i]; }
    ndirty = k;
}
static void hook(int op, vnode_t *n, const char *old) {
    if (!persist_on || !system_volume) return;
    char p[512];
    switch (op) {
    case VH_CREATE:
        if (!on_sys(n)) return;
        if (n->dir) { rel_path(n, p, sizeof p); vol_mkdir(system_volume, p); fat_changed = true; }
        else mark_dirty(n);
        break;
    case VH_WRITE:
        if (on_sys(n)) mark_dirty(n);
        break;
    case VH_DELETE:
        if (!on_sys(n)) return;
        purge(n);
        rel_path(n, p, sizeof p);
        vol_delete(system_volume, p); fat_changed = true;
        break;
    case VH_RENAME: {
        bool was = old && toupper(old[0]) == 'C' && old[1] == ':';
        bool now = on_sys(n);
        if (was && now) {
            rel_path(n, p, sizeof p);
            if (!vol_rename(system_volume, old + 2, p)) mark_tree(n);
        } else if (was) { vol_delete(system_volume, old + 2); purge(n); }
        else if (now) mark_tree(n);
        fat_changed = true;
        break;
    }
    }
}

void persist_flush(void) {
    if (!system_volume) return;
    settings_save();
    int n = ndirty;
    for (int i = 0; i < ndirty; i++) {
        vnode_t *f = dirty[i];
        f->dirty = false;
        if (f->dir || vfs_drive_letter(f) != 'C') continue;
        char p[512]; rel_path(f, p, sizeof p);
        if (!vol_write(system_volume, p, f->data, f->size, f->readonly)) klog("persist: could not write %s", p);
    }
    ndirty = 0;
    if (n || fat_changed) { vol_sync(system_volume); fat_changed = false; }
    if (n) klog("persist: wrote %d file(s) to the system disk", n);
}
int persist_pending(void) { return ndirty; }
static void flush_task(void *arg) {
    for (;;) {
        task_sleep(2000);
        if (ndirty || fat_changed) persist_flush();
        else settings_save();
    }
}
void persist_start(void) {
    vfs_hook = hook;
    persist_on = true;
    if (system_volume) task_create("persistd", flush_task, NULL, 16384);
}

bool persist_boot_from_disk(uint32_t serial, char *err, int errcap) {
    for (int d = 0; d < blk_count(); d++) {
        blkdev_t *dev = blk_get(d);
        part_t parts[4];
        if (!mbr_read(dev, parts)) continue;
        for (int i = 0; i < 4; i++) {
            if (parts[i].type != 0x0B && parts[i].type != 0x0C && parts[i].type != 0x07) continue;
            vol_t *v = vol_mount(dev, parts[i].lba, parts[i].sectors);
            if (!v) continue;
            if ((serial && vol_serial(v) == serial) || (!serial && vol_exists(v, "boot\\wrld.elf"))) {
                system_volume = v; system_disk_index = d; system_part_index = i;
                /* systems installed before the system folder was renamed */
                if (vol_exists(v, "Windows") && !vol_exists(v, "WRLD")) {
                    bool ok = vol_rename(v, "Windows", "WRLD");
                    const char *oldst = "WRLD\\Resources\\Themes\\Windows Classic.wrldstyle";
                    uint32_t sl; uint8_t *st = ok ? vol_read(v, oldst, &sl) : NULL;
                    if (st) {                                 /* rename the style and its Name= line */
                        char *nb = kmalloc(sl + 16); uint32_t o = 0;
                        for (uint32_t k = 0; k < sl; k++) {
                            if (k + 20 <= sl && !memcmp(st + k, "Name=Windows Classic", 20)) { memcpy(nb + o, "Name=WRLD Classic", 17); o += 17; k += 19; }
                            else nb[o++] = st[k];
                        }
                        if (vol_write(v, "WRLD\\Resources\\Themes\\WRLD Classic.wrldstyle", nb, o, false)) vol_delete(v, oldst);
                        kfree(nb); kfree(st);
                    }
                    vol_sync(v);
                    klog("persist: renamed C:\\Windows to C:\\WRLD (%s)", ok ? "ok" : "failed");
                }
                /* systems installed before the browser became XplorerLite */
                {
                    const char *ol = "Documents and Settings\\User\\Desktop\\The WRLD Xplorer.lnk";
                    const char *nl = "Documents and Settings\\User\\Desktop\\XplorerLite.lnk";
                    uint32_t ln = 0; uint8_t *lk = vol_exists(v, ol) ? vol_read(v, ol, &ln) : NULL;
                    if (lk) {
                        if (vol_exists(v, nl) || vol_write(v, nl, lk, ln, false)) vol_delete(v, ol);
                        kfree(lk); vol_sync(v);
                        klog("persist: desktop shortcut renamed to XplorerLite");
                    }
                }
                int files = vol_load_tree(v, vfs_root('C'));
                klog("persist: system disk %d partition %d, %d files loaded", d, i + 1, files);
                return files >= 0;
            }
            vol_unmount(v);
        }
    }
    snprintf(err, errcap, "THE WRLD OS could not find its system partition (volume serial %08X).\n"
             "Check that the hard disk is connected, or boot from the CD and run Setup again.", serial);
    return false;
}

/* ---------------- settings (C:\WRLD\wrld.ini) ---------------- */
typedef struct { char k[24]; char v[64]; } kv_t;
static kv_t kv[40];
static int nkv;
static char last_saved[2048];
const char *settings_get(const char *key) { for (int i = 0; i < nkv; i++) if (!strcasecmp(kv[i].k, key)) return kv[i].v; return NULL; }
void settings_set(const char *key, const char *val) {
    for (int i = 0; i < nkv; i++) if (!strcasecmp(kv[i].k, key)) { strlcpy(kv[i].v, val, sizeof kv[i].v); return; }
    if (nkv < 40) { strlcpy(kv[nkv].k, key, sizeof kv[nkv].k); strlcpy(kv[nkv].v, val, sizeof kv[nkv].v); nkv++; }
}
/* account pictures are saved by name, so adding icons never changes them */
static const struct { const char *name; int icon; } pictures[] = {
    { "flower", ICON_USER_FLOWER }, { "planet", ICON_USER_PLANET }, { "fish", ICON_USER_FISH }, { "mountain", ICON_USER_MOUNTAIN } };
static int picture_from_name(const char *s, int def) {
    if (!s) return def;
    if (isdigit(s[0])) { int v = atoi(s); for (unsigned i = 0; i < ARRAY_LEN(pictures); i++) if (pictures[i].icon == v) return v; return def; }  /* older wrld.ini */
    for (unsigned i = 0; i < ARRAY_LEN(pictures); i++) if (!strcasecmp(s, pictures[i].name)) return pictures[i].icon;
    return def;
}
static const char *picture_name(int icon) {
    for (unsigned i = 0; i < ARRAY_LEN(pictures); i++) if (pictures[i].icon == icon) return pictures[i].name;
    return pictures[0].name;
}
void persist_status(char *b, int cap) {
    if (!system_volume) { snprintf(b, cap, "Live session from the CD (in memory; changes are not saved)"); return; }
    snprintf(b, cap, "Disk %d, Partition %d: %s \"%s\", %u MB free of %u MB", system_disk_index, system_part_index + 1,
             vol_fs_name(system_volume), vol_label(system_volume), (uint32_t)(vol_free_bytes(system_volume) >> 20), (uint32_t)(vol_total_bytes(system_volume) >> 20));
}
static int geti(const char *k, int def) { const char *v = settings_get(k); return v ? atoi(v) : def; }
void settings_load(void) {
    uint32_t len;
    char *t = (char *)vfs_read_file(SETTINGS_FILE, &len);
    if (!t) return;
    for (char *line = t; *line;) {
        char *e = line; while (*e && *e != '\n') e++;
        char save = *e; *e = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        char *eq = strchr(line, '=');
        if (eq && line[0] != ';' && line[0] != '[') { *eq = 0; settings_set(line, eq + 1); }
        *e = save; line = *e ? e + 1 : e;
    }
    kfree(t);
    /* apply */
    const char *s;
    if ((s = settings_get("user")) && *s) strlcpy(user_name, s, sizeof user_name);
    if ((s = settings_get("computer")) && *s) strlcpy(computer_name, s, sizeof computer_name);
    user_picture = picture_from_name(settings_get("picture"), user_picture);
    tz_offset_minutes = geti("tz", tz_offset_minutes);
    mouse_speed = geti("mouse_speed", mouse_speed);
    clock_24h = geti("clock24", clock_24h);
    show_seconds = geti("seconds", show_seconds);
    audio_volume = CLAMP(geti("volume", audio_volume), 0, 100);
    system_sounds = geti("sounds", system_sounds);
    const char *rs = settings_get("resolution");
    if (rs && video_can_set_mode()) { int rw = atoi(rs); const char *xx = strchr(rs, 'x'); int rh = xx ? atoi(xx + 1) : 0; if (rw && rh) gfx_set_resolution(rw, rh); }
    const char *th = settings_get("theme");
    if (!boot_safe_mode && th) {
        int id = isdigit(th[0]) ? atoi(th) : style_find(th);    /* numbers: older wrld.ini */
        theme_sat = CLAMP(geti("saturation", 0), -100, 100); theme_light = CLAMP(geti("lightness", 0), -60, 60);
        if (id >= 0 && id < THEME_COUNT) theme_apply(id, geti("hue", 0));
    }
    if (settings_get("wallpaper")) { wallpaper_id = geti("wallpaper", wallpaper_id); if (wallpaper_id < 0 || wallpaper_id >= wallpaper_count) wallpaper_id = 0; shell_wallpaper_changed(); }
    klog("settings: loaded %d values", nkv);
}
static void capture(void) {
    char b[16];
    settings_set("user", user_name); settings_set("computer", computer_name);
    settings_set("picture", picture_name(user_picture));
    settings_set("theme", style_file(theme_id));
    if (video_can_set_mode()) { snprintf(b, sizeof b, "%dx%d", SCREEN_W, SCREEN_H); settings_set("resolution", b); }
    snprintf(b, sizeof b, "%d", theme_hue); settings_set("hue", b);
    snprintf(b, sizeof b, "%d", theme_sat); settings_set("saturation", b);
    snprintf(b, sizeof b, "%d", theme_light); settings_set("lightness", b);
    snprintf(b, sizeof b, "%d", wallpaper_id); settings_set("wallpaper", b);
    snprintf(b, sizeof b, "%d", tz_offset_minutes); settings_set("tz", b);
    snprintf(b, sizeof b, "%d", mouse_speed); settings_set("mouse_speed", b);
    snprintf(b, sizeof b, "%d", clock_24h); settings_set("clock24", b);
    snprintf(b, sizeof b, "%d", show_seconds); settings_set("seconds", b);
    snprintf(b, sizeof b, "%d", audio_volume); settings_set("volume", b);
    snprintf(b, sizeof b, "%d", system_sounds); settings_set("sounds", b);
}
void settings_save(void) {
    capture();
    char text[2048]; int o = snprintf(text, sizeof text, "; THE WRLD OS settings\r\n[WRLD]\r\n");
    for (int i = 0; i < nkv && o < (int)sizeof text - 100; i++) o += snprintf(text + o, sizeof text - o, "%s=%s\r\n", kv[i].k, kv[i].v);
    if (!strcmp(text, last_saved)) return;
    if (vfs_write_file(SETTINGS_FILE, text, strlen(text))) strlcpy(last_saved, text, sizeof last_saved);
}
