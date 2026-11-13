/* THE WRLD OS Setup.
 *  1. Graphical Setup: pick a disk and partition from a detailed list, confirm the
 *     exact disk/partition/letter/size, format NTFS or FAT32, copy the system
 *     from the live C: drive, install the GRUB boot loader, restart. The older
 *     blue text-mode setup is kept as a fallback ("setup=text").
 *  2. First boot from the hard disk: a graphical wizard collects the user's
 *     name, computer name, time zone and look, then finishes the installation. */
#include "kernel.h"
#include "devmgr.h"
#include "bootinfo.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"

bool boot_setup_mode;
uint32_t boot_root_serial;

/* Set while either setup front-end owns the screen.  The GUI front-end also
 * uses it to redirect the text progress and message helpers in do_install. */
static bool gui_setup;
static void gui_setup_message(const char *msg);
static void gui_setup_progress(const char *caption, int pct, const char *detail);

/* ======================================================================
 * text-mode screens
 * ====================================================================== */
#define TS_BG   HEX(0x0000A8)
#define TS_FG   HEX(0xC0C0C0)
#define TS_HI   HEX(0xFFFFFF)
#define TS_BAR  HEX(0xA8A8A8)
#define TS_YEL  HEX(0xFFFF54)
#define TS_RED  HEX(0xE02020)
#define LH 20
static int cw = 8;
static const char *ts_title = OS_NAME " " OS_EDITION " Setup";
static int X(int col) { return 24 + col * cw; }
static int Y(int row) { return 10 + row * LH; }
static void ts_status(const char *s, const char *right) {
    int y = SCREEN_H - 26;
    fill_rect(&screen, 0, y, SCREEN_W, 26, TS_BAR);
    draw_text(&screen, FONT_MONO, X(0), y + 5, s, BLACK);
    if (right) {
        int x = SCREEN_W - 24 - text_width(FONT_MONO, right) - 8;
        vline(&screen, x - 10, y + 3, 20, HEX(0x404040));
        draw_text(&screen, FONT_MONO, x, y + 5, right, BLACK);
    }
}
static void ts_begin(const char *status, const char *right) {
    gfx_clear(&screen, TS_BG);
    canvas_reset_clip(&screen);
    draw_text(&screen, FONT_MONO, X(0), Y(0), ts_title, TS_HI);
    int w = text_width(FONT_MONO, ts_title) + 2;
    hline(&screen, X(0), Y(1) - 3, w, TS_HI); hline(&screen, X(0), Y(1) - 1, w, TS_HI);
    ts_status(status, right);
}
static void tx(int col, int row, const char *s, uint32_t c) { draw_text(&screen, FONT_MONO, X(col), Y(row), s, c); }
static int tx_wrap(int col, int row, const char *s, uint32_t c) {
    /* wraps at 76 columns from `col`; returns rows used */
    int maxc = 100 - col, rows = 0;
    while (*s) {
        int n = strlen(s), cut = n;
        if (n > maxc) { cut = maxc; while (cut > 0 && s[cut] != ' ') cut--; if (!cut) cut = maxc; }
        const char *nl = memchr(s, '\n', cut); if (nl) cut = nl - s;
        draw_text_n(&screen, FONT_MONO, X(col), Y(row + rows), s, cut, c);
        rows++; s += cut; while (*s == ' ' || *s == '\n') { if (*s == '\n' && s[1] == '\n') { rows++; s++; } s++; }
    }
    return rows;
}
static void bullet(int col, int row, const char *s, uint32_t c) {
    fill_rect(&screen, X(col) + 2, Y(row) + 7, 5, 5, c);
    tx_wrap(col + 3, row, s, c);
}
static void ts_box(int x, int y, int w, int h, uint32_t col, bool dbl) {
    draw_rect(&screen, x, y, w, h, col);
    if (dbl) draw_rect(&screen, x + 3, y + 3, w - 6, h - 6, col);
}
static void ts_present(void) { gui_heartbeat = timer_ticks(); gfx_present(CURSOR_ARROW, -100, -100); }
static void ts_progress(int row, const char *caption, int pct, uint32_t col) {
    int w = 60 * cw, x = (SCREEN_W - w) / 2, y = Y(row);
    if (caption) draw_text(&screen, FONT_MONO, (SCREEN_W - text_width(FONT_MONO, caption)) / 2, y - LH - 4, caption, TS_HI);
    fill_rect(&screen, x, y, w, 26, TS_BG);
    ts_box(x, y, w, 26, TS_HI, false);
    int fw = (w - 6) * MAX(0, MIN(100, pct)) / 100;
    fill_rect(&screen, x + 3, y + 3, fw, 20, col);
    char b[8]; snprintf(b, sizeof b, "%d%%", pct);
    int tw = text_width(FONT_MONO, b), tx0 = x + (w - tw) / 2;
    draw_text(&screen, FONT_MONO, tx0, y + 5, b, fw > tx0 - x ? TS_BG : TS_HI);
}
static int ts_key(void) {
    key_event_t e; mouse_state_t m;
    for (;;) {
        while (mouse_poll(&m)) {}
        while (keyboard_poll(&e)) if (e.pressed) { int k = e.key ? e.key : (uint8_t)e.ch; return k < 0x100 ? tolower(k) : k; }
        task_yield(); hlt();
    }
}
static int ts_key_timeout(uint32_t ms) {
    key_event_t e; mouse_state_t m;
    uint32_t end = timer_ticks() + ms;
    while ((int32_t)(timer_ticks() - end) < 0) {
        while (mouse_poll(&m)) {}
        while (keyboard_poll(&e)) if (e.pressed) { int k = e.key ? e.key : (uint8_t)e.ch; return k < 0x100 ? tolower(k) : k; }
        task_yield(); hlt();
    }
    return 0;
}

static bool from_desk;
static void quit_setup(void);
/* F3 confirmation box; returns true if the user really wants to quit */
static bool confirm_quit(void) {
    int w = 64 * cw, h = 9 * LH, x = (SCREEN_W - w) / 2, y = Y(12);
    fill_rect(&screen, x, y, w, h, TS_BG);
    ts_box(x + 6, y + 6, w - 12, h - 12, TS_HI, true);
    int c0 = (x - 24) / cw + 3;
    tx_wrap(c0, 13, OS_NAME " is not completely set up on your computer. If you quit Setup now, you will need to run Setup again to install " OS_NAME ".", TS_HI);
    bullet(c0, 17, "To continue Setup, press ENTER.", TS_HI);
    bullet(c0, 18, from_desk ? "To quit Setup, press F3." : "To quit Setup and restart, press F3.", TS_HI);
    ts_status("F3=Quit  ENTER=Continue", NULL);
    ts_present();
    for (;;) { int k = ts_key(); if (k == KEY_F3) return true; if (k == KEY_ENTER) return false; }
}

/* ---------------- disks and partitions ---------------- */
enum { IT_DISK, IT_PART, IT_FREE };
typedef struct { int kind, disk, slot; uint32_t lba, sectors; uint8_t type; char letter; int free_mb; bool fat; int fskind; char fslabel[16]; } item_t;   /* fat: has a usable file system (FAT32 or NTFS) */
static item_t items[40];
static int nitems;
static part_t ptab[BLK_MAX][4];
static bool has_mbr[BLK_MAX];
static bool gpt_disk[BLK_MAX];               /* Setup will replace it with MBR */
static void disk_desc(int d, char *b, int n) {
    blkdev_t *dv = blk_get(d);
    uint64_t mb = (dv->total_sectors ? dv->total_sectors : dv->sectors) / 2048;
    if (mb >= 1024 * 1024)
        snprintf(b, n, "%llu GB Disk %d at Id %d on bus %d on %s [%s]", (unsigned long long)(mb / 1024), d, dv->id, dv->busno, dv->driver,
                 gpt_disk[d] ? "GPT -> MBR" : "MBR");
    else
        snprintf(b, n, "%llu MB Disk %d at Id %d on bus %d on %s [%s]", (unsigned long long)mb, d, dv->id, dv->busno, dv->driver,
                 gpt_disk[d] ? "GPT -> MBR" : "MBR");
}
static void part_desc(item_t *it, char *b, int n) {
    if (it->kind == IT_FREE) { snprintf(b, n, "    Unpartitioned space          %8u MB", it->sectors / 2048); return; }
    char fs[24];
    if (it->fat) snprintf(fs, sizeof fs, "[%s]", it->fskind == FS_NTFS ? "NTFS" : "FAT32");
    else if ((it->type == 0x0C || it->type == 0x07) && !it->fat) snprintf(fs, sizeof fs, "[New (Raw)]");
    else snprintf(fs, sizeof fs, "[%s]", part_type_name(it->type));
    char lab[48]; snprintf(lab, sizeof lab, "%c:  Partition%d %s", it->letter, it->slot + 1, fs);
    if (it->fat) snprintf(b, n, "%-33s%8u MB (%6d MB free)", lab, it->sectors / 2048, it->free_mb);
    else snprintf(b, n, "%-33s%8u MB", lab, it->sectors / 2048);
}
static void build_items(void) {
    nitems = 0;
    char letter = 'C';
    for (int d = 0; d < blk_count() && d < BLK_MAX; d++) {
        blkdev_t *dv = blk_get(d);
        items[nitems++] = (item_t){ .kind = IT_DISK, .disk = d };
        has_mbr[d] = mbr_read(dv, ptab[d]);
        gpt_disk[d] = false;
        for (int i = 0; i < 4; i++) if (ptab[d][i].type == 0xEE) gpt_disk[d] = true;
        if (gpt_disk[d]) memset(ptab[d], 0, sizeof ptab[d]);   /* Setup will write MBR */
        int order[4], no = 0;
        for (int i = 0; i < 4; i++) if (ptab[d][i].type) order[no++] = i;
        for (int i = 1; i < no; i++) for (int j = i; j > 0 && ptab[d][order[j]].lba < ptab[d][order[j - 1]].lba; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
        uint32_t pos = 2048;
        for (int k = 0; k <= no && nitems < 38; k++) {
            uint32_t end = k < no ? ptab[d][order[k]].lba : dv->sectors;
            uint32_t start = (pos + 2047) & ~2047u;
            if (end > start && end - start >= 16 * 2048) items[nitems++] = (item_t){ .kind = IT_FREE, .disk = d, .lba = start, .sectors = (end - start) & ~2047u };
            if (k == no) break;
            part_t *p = &ptab[d][order[k]];
            item_t it = { .kind = IT_PART, .disk = d, .slot = order[k], .lba = p->lba, .sectors = p->sectors, .type = p->type, .letter = letter++ };
            if (p->type == 0x0B || p->type == 0x0C || p->type == 0xEF || p->type == 0x07) {
                vol_t *v = vol_mount(dv, p->lba, p->sectors);
                if (v) { it.fat = true; it.fskind = vol_kind(v); it.free_mb = vol_free_bytes(v) / (1024 * 1024); strlcpy(it.fslabel, vol_label(v), sizeof it.fslabel); vol_unmount(v); }
            }
            items[nitems++] = it;
            pos = p->lba + p->sectors;
        }
    }
}
static int free_slot(int d) { for (int i = 0; i < 4; i++) if (!ptab[d][i].type) return i; return -1; }
static bool write_ptab(int d) { return mbr_write(blk_get(d), ptab[d], NULL); }

/* create-partition prompt; returns slot or -1 */
static int create_partition(item_t *it) {
    int maxmb = it->sectors / 2048, slot = free_slot(it->disk);
    bool capped = false;
    if (bootinfo.is_efi && maxmb > 100 * 1024) { maxmb = 100 * 1024; capped = true; }
    char dd[96]; disk_desc(it->disk, dd, sizeof dd);
    if (slot < 0) {
        ts_begin("ENTER=Continue", NULL);
        tx_wrap(2, 3, "Setup cannot create a new partition on this disk because the partition table already has four entries. Delete a partition first.", TS_FG);
        ts_present(); while (ts_key() != KEY_ENTER) {}
        return -1;
    }
    char num[8]; snprintf(num, sizeof num, "%d", maxmb);
    for (;;) {
        ts_begin("ENTER=Create  ESC=Cancel", NULL);
        tx(2, 3, "You asked Setup to create a new partition on", TS_FG);
        tx(2, 4, dd, TS_FG);
        bullet(4, 6, "To create the new partition, enter a size below and press ENTER.", TS_FG);
        bullet(4, 8, "To go back to the previous screen without creating the partition, press ESC.", TS_FG);
        char b[96];
        snprintf(b, sizeof b, "The minimum size for the new partition is      64 megabytes (MB)."); tx(2, 11, b, TS_FG);
        snprintf(b, sizeof b, "The maximum size for the new partition is %7d megabytes (MB).", maxmb); tx(2, 12, b, TS_FG);
        if (capped) tx_wrap(2, 10, "This is a FAT32 system partition; it is limited to 100 GB. The rest of the disk stays unpartitioned.", TS_FG);
        tx(2, 14, "Create partition of size (in MB):", TS_FG);
        fill_rect(&screen, X(37) - 4, Y(14) - 2, 9 * cw, LH, TS_BAR);
        draw_text(&screen, FONT_MONO, X(37), Y(14), num, BLACK);
        if ((timer_ticks() / 400) & 1) fill_rect(&screen, X(37) + text_width(FONT_MONO, num), Y(14) + 14, cw, 2, BLACK);
        ts_present();
        int k = ts_key_timeout(400);
        if (!k) continue;
        if (k == KEY_ESC) return -1;
        if (k == KEY_BACKSPACE) { int l = strlen(num); if (l) num[l - 1] = 0; }
        else if (k >= '0' && k <= '9' && strlen(num) < 7) { int l = strlen(num); num[l] = k; num[l + 1] = 0; }
        else if (k == KEY_ENTER) {
            int mb = atoi(num);
            if (mb < 64 || mb > maxmb) continue;
            uint32_t secs = (uint32_t)mb * 2048;
            if (secs > it->sectors) secs = it->sectors;
            ptab[it->disk][slot] = (part_t){ .type = 0x0C, .boot = 0, .lba = it->lba, .sectors = secs };
            if (!write_ptab(it->disk)) return -1;
            return slot;
        }
    }
}
static bool delete_partition(item_t *it) {
    char dd[96], pd[96]; disk_desc(it->disk, dd, sizeof dd); part_desc(it, pd, sizeof pd);
    ts_begin("L=Delete  ESC=Cancel", NULL);
    tx(2, 3, "You asked Setup to delete the partition", TS_FG);
    tx(6, 5, pd, TS_HI);
    tx(2, 7, "on", TS_FG); tx(2, 8, dd, TS_FG);
    bullet(4, 11, "To delete this partition, press L.", TS_FG);
    tx(7, 12, "CAUTION: All data on this partition will be lost.", TS_HI);
    bullet(4, 14, "To return to the previous screen without deleting the partition, press ESC.", TS_FG);
    ts_present();
    for (;;) {
        int k = ts_key();
        if (k == KEY_ESC) return false;
        if (k == 'l') { memset(&ptab[it->disk][it->slot], 0, sizeof(part_t)); write_ptab(it->disk); return true; }
    }
}

/* ---------------- copying ---------------- */
typedef struct { vnode_t **v; int n, cap; uint64_t bytes; } flist_t;
static void collect(vnode_t *n, flist_t *l) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->v = krealloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = n;
    if (!n->dir) l->bytes += n->size;
    for (vnode_t *c = n->child; c; c = c->next) collect(c, l);
}
static void rel(vnode_t *n, char *b, int cap) { char f[512]; vfs_path(n, f, sizeof f); strlcpy(b, f + 2, cap); }

static int fmt_pct; static const char *fmt_what;
static void fmt_progress(int pct, const char *what) {
    if (pct == fmt_pct && what == fmt_what) return;
    fmt_pct = pct; fmt_what = what;
    if (gui_setup) { gui_setup_progress("Formatting the partition...", pct, what); return; }
    ts_progress(16, "Setup is formatting...", pct, TS_YEL);
    ts_present();
}
static void message_screen(const char *msg, const char *status) {
    if (gui_setup) { gui_setup_message(msg); return; }
    ts_begin(status, NULL);
    tx_wrap(2, 3, msg, TS_FG);
    ts_present();
    while (ts_key() != KEY_ENTER) {}
}

/* mode: 0 NTFS quick, 1 NTFS full, 2 FAT32 quick, 3 FAT32 full, 4 keep the file system */
static bool do_install(int disk, int slot, int mode) {
    blkdev_t *dv = blk_get(disk);
    part_t *p = &ptab[disk][slot];
    char dd[96]; disk_desc(disk, dd, sizeof dd);
    char err[200];
    uint32_t serial = rand_next() ^ (timer_ticks() << 16) ^ 0x57524C44;
    if (bootinfo.is_efi && mode < 2) {
        message_screen("This computer started Setup in UEFI mode, so the system partition must be FAT32 (it becomes the EFI System Partition).\n\nRun Setup again and choose a FAT32 option.\n\nPress ENTER to return.", "ENTER=Continue");
        return false;
    }
    if (gpt_disk[disk] && !gui_setup) {
        message_screen("This disk uses a GPT partition table. Setup writes an MBR partition table, so it will replace the GPT and erase all partitions on this disk.\n\n"
                       "Turn the computer off now if you need to keep them. Otherwise press ENTER to continue.", "ENTER=Continue");
    }
    /* boot loader files from the running system */
    uint32_t bl, cl;
    char corep[64]; snprintf(corep, sizeof corep, "C:\\boot\\grub\\core%d.img", slot + 1);
    uint8_t *bootimg = vfs_read_file("C:\\boot\\grub\\boot.img", &bl), *core = vfs_read_file(corep, &cl);
    if (!bootimg || bl != 512 || !core || !cl) {
        message_screen("Setup cannot find the boot loader files (C:\\boot\\grub\\boot.img and core images) on the installation media. Setup cannot continue.\n\nPress ENTER to return.", "ENTER=Continue");
        kfree(bootimg); kfree(core); return false;
    }
    uint32_t core_secs = (cl + 511) / 512, first = 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) if (ptab[disk][i].type && ptab[disk][i].lba < first) first = ptab[disk][i].lba;
    if (first < core_secs + 1) {
        snprintf(err, sizeof err, "There is not enough room before the first partition on this disk for the boot loader (needs %u sectors, %u available). Delete the partitions and let Setup create new ones.\n\nPress ENTER to return.", core_secs + 1, first);
        message_screen(err, "ENTER=Continue"); kfree(bootimg); kfree(core); return false;
    }
    /* format */
    if (mode != 4) {
        int kind = mode < 2 ? FS_NTFS : FS_FAT32;
        item_t it = { .kind = IT_PART, .disk = disk, .slot = slot, .lba = p->lba, .sectors = p->sectors, .type = vol_part_type(kind), .letter = 'C' };
        char pd[96]; part_desc(&it, pd, sizeof pd);
        if (gui_setup) {
            char det[200]; snprintf(det, sizeof det, "%s\n%s", pd, dd);
            gui_setup_progress("Formatting the partition...", 0, det);
        } else {
            ts_begin("", NULL);
            tx(2, 3, "Please wait while Setup formats the partition", TS_FG);
            tx(6, 5, pd, TS_HI);
            tx(2, 7, "on", TS_FG); tx(2, 8, dd, TS_FG);
        }
        fmt_pct = -1;
        p->type = vol_part_type(kind); write_ptab(disk);
        if (!vol_format(kind, dv, p->lba, p->sectors, "WRLD OS", mode == 1 || mode == 3, serial, fmt_progress, err, sizeof err)) {
            char m[300]; snprintf(m, sizeof m, "%s\n\nPress ENTER to return.", err); message_screen(m, "ENTER=Continue");
            kfree(bootimg); kfree(core); return false;
        }
    }
    vol_t *v = vol_mount(dv, p->lba, p->sectors);
    if (!v) { message_screen("Setup could not open the file system on the selected partition.\n\nPress ENTER to return.", "ENTER=Continue"); kfree(bootimg); kfree(core); return false; }
    if (bootinfo.is_efi && vol_kind(v) != FS_FAT32) {
        message_screen("This computer started Setup in UEFI mode, so the system partition must be FAT32 (it becomes the EFI System Partition).\n\nRun Setup again and choose a FAT32 option.\n\nPress ENTER to return.", "ENTER=Continue");
        vol_unmount(v); kfree(bootimg); kfree(core); return false;
    }
    serial = vol_serial(v);
    /* copy files */
    flist_t L = { 0 };
    collect(vfs_root('C'), &L);
    if ((uint64_t)L.bytes + 4 * 1024 * 1024 > vol_free_bytes(v)) {
        message_screen("The selected partition does not have enough free space for " OS_NAME ".\n\nPress ENTER to return.", "ENTER=Continue");
        vol_unmount(v); kfree(bootimg); kfree(core); return false;
    }
    uint64_t done = 0; uint32_t last = 0;
    for (int i = 0; i < L.n; i++) {
        vnode_t *n = L.v[i];
        if (!n->parent) continue;
        char path[512]; rel(n, path, sizeof path);
        bool ok = n->dir ? vol_mkdir(v, path) : vol_write(v, path, n->data, n->size, n->readonly);
        if (!ok) {
            char m[600]; snprintf(m, sizeof m, "Setup cannot copy the file %s.\n\nThe disk may be full or damaged. Press ENTER to return.", path);
            message_screen(m, "ENTER=Continue"); vol_unmount(v); kfree(L.v); kfree(bootimg); kfree(core); return false;
        }
        if (!n->dir) done += n->size;
        if (timer_ticks() - last > 30 || i == L.n - 1) {
            last = timer_ticks();
            char st[160]; snprintf(st, sizeof st, "Copying: %s", n->name);
            int pct = (int)(done * 100 / MAX(1, L.bytes));
            if (gui_setup) gui_setup_progress("Copying files to the hard disk...", pct, st);
            else {
                ts_begin("", NULL);
                tx_wrap(2, 3, "Please wait while Setup copies files to the " OS_NAME " installation folders. This might take several minutes to complete.", TS_FG);
                ts_progress(16, "Setup is copying files...", pct, TS_YEL);
                ts_status("", st);
                ts_present();
            }
        }
    }
    kfree(L.v);
    vol_delete(v, "Documents and Settings\\User\\Desktop\\Install " OS_NAME ".lnk");
    /* configuration */
    if (gui_setup) gui_setup_progress("Finishing the installation...", 40, "Installing the boot loader");
    else {
        ts_begin("", NULL);
        tx(2, 3, "Please wait while Setup initializes your " OS_NAME " configuration.", TS_FG);
        ts_progress(16, "Setup is installing the boot loader...", 40, TS_YEL);
        ts_present();
    }
    char cfg[1024];
    snprintf(cfg, sizeof cfg,
        "# THE WRLD OS boot menu - written by Setup\n"
        "set timeout=4\nset default=0\n"
        "insmod all_video\n"
        "if [ \"$grub_platform\" = \"efi\" ]; then insmod efi_gop; fi\n"
        "set gfxmode=1920x1080x32,1680x1050x32,1600x900x32,1440x900x32,1366x768x32,1280x1024x32,1280x800x32,1024x768x32,auto\n"
        "set gfxpayload=keep\n\n"
        "menuentry \"" OS_NAME "\" {\n    multiboot2 /boot/wrld.elf root=%08X\n    boot\n}\n"
        "menuentry \"" OS_NAME " (verbose boot)\" {\n    multiboot2 /boot/wrld.elf root=%08X verbose\n    boot\n}\n"
        "menuentry \"" OS_NAME " (safe mode - Classic theme)\" {\n    multiboot2 /boot/wrld.elf root=%08X safe\n    boot\n}\n",
        serial, serial, serial);
    vol_write(v, "boot\\grub\\grub.cfg", cfg, strlen(cfg), false);
    if (bootinfo.is_efi) {
        uint32_t el = 0;
        uint8_t *efi = vfs_read_file("C:\\boot\\grub\\BOOTX64.EFI", &el);
        if (!efi || !el) {
            message_screen("Setup cannot find the UEFI boot loader (C:\\boot\\grub\\BOOTX64.EFI) on the installation media. Setup cannot continue.\n\nPress ENTER to return.", "ENTER=Continue");
            vol_unmount(v); kfree(bootimg); kfree(core); return false;
        }
        vol_mkdir(v, "EFI");
        vol_mkdir(v, "EFI\\BOOT");
        bool eok = vol_write(v, "EFI\\BOOT\\BOOTX64.EFI", efi, el, false);
        kfree(efi);
        if (!eok) {
            message_screen("Setup could not write the UEFI boot loader to the disk.\n\nPress ENTER to return.", "ENTER=Continue");
            vol_unmount(v); kfree(bootimg); kfree(core); return false;
        }
        /* The partition holds the EFI loader now, so the firmware must see it
         * as an EFI System Partition.  FAT32 still mounts as C: on later boots. */
        p->type = 0xEF;
        write_ptab(disk);
        klog("setup: EFI System Partition on disk %d partition %d", disk, slot + 1);
    }
    rtc_time_t t; rtc_now(&t);
    char ini[256];
    snprintf(ini, sizeof ini, "; THE WRLD OS settings\r\n[WRLD]\r\nsetup=oobe\r\ninstalled=%04d-%02d-%02d %02d:%02d\r\nserial=%08X\r\n", t.year, t.month, t.day, t.hour, t.minute, serial);
    vol_write(v, "WRLD\\wrld.ini", ini, strlen(ini), false);
    vol_sync(v);
    vol_unmount(v);
    /* boot loader: core image in the gap after the MBR, boot code in the MBR */
    uint8_t *cbuf = kzalloc(core_secs * 512); memcpy(cbuf, core, cl);
    bool ok = blk_write(dv, 1, core_secs, cbuf);
    kfree(cbuf);
    for (int i = 0; i < 4; i++) ptab[disk][i].boot = i == slot;
    ok = ok && mbr_write(dv, ptab[disk], bootimg);
    kfree(bootimg); kfree(core);
    if (gui_setup) gui_setup_progress("Finishing the installation...", 100, "The boot loader is installed");
    else { ts_progress(16, "Setup is installing the boot loader...", 100, TS_YEL); ts_present(); }
    if (!ok) { message_screen("Setup could not write the boot loader to the disk.\n\nPress ENTER to return.", "ENTER=Continue"); return false; }
    klog("setup: installed to disk %d partition %d (serial %08x)", disk, slot + 1, serial);
    return true;
}

static void finished_screen(void) {
    uint32_t start = timer_ticks();
    for (;;) {
        uint32_t el = timer_ticks() - start;
        if (el >= 15000) break;
        ts_begin("ENTER=Restart Computer", NULL);
        tx(2, 3, "This portion of Setup has completed successfully.", TS_FG);
        tx(2, 5, "If there is a CD in the drive, you can remove it. (The CD boot menu", TS_FG);
        tx(2, 6, "starts the installed system from the hard disk automatically.)", TS_FG);
        tx(2, 8, "To restart your computer, press ENTER.", TS_FG);
        tx(2, 9, "When your computer restarts, Setup will continue.", TS_FG);
        int w = 60 * cw, x = (SCREEN_W - w) / 2, y = Y(16);
        draw_text_centered(&screen, FONT_MONO, R(0, y - LH - 4, SCREEN_W, LH), "Your computer will reboot in 15 seconds...", TS_HI);
        ts_box(x, y, w, 26, TS_HI, false);
        int fw = (w - 6) * (15000 - el) / 15000;
        fill_rect(&screen, x + 3, y + 3, fw, 20, TS_RED);
        ts_present();
        if (ts_key_timeout(100) == KEY_ENTER) break;
    }
    ts_begin("", NULL);
    tx(2, 3, "Restarting your computer...", TS_FG);
    ts_present();
    task_sleep(300);
    system_reboot();
}

static void quit_setup(void) {
    if (from_desk) return;
    ts_begin("", NULL); tx(2, 3, "Setup was not completed. Restarting your computer...", TS_FG); ts_present();
    task_sleep(800);
    system_reboot();
}

static const char *license_text =
    "THE WRLD OS LICENSE AGREEMENT\n\n"
    "THE WRLD OS is an original, from-scratch hobby operating system with an XP-style user interface. "
    "Its kernel, drivers, window manager, Win32 subsystem, shell and applications were written for this project.\n\n"
    "1. You may install THE WRLD OS on as many computers and virtual machines as you like.\n\n"
    "2. Setup will partition and format the disk you choose. Formatting deletes all data on that partition. "
    "Make sure the disk does not contain anything you want to keep.\n\n"
    "3. THE WRLD OS runs " OS_EXE_DESC " Win32 (.exe) console and GUI programs through its own Win32 subsystem, POSIX-style "
    "shell scripts, and opens macOS UDIF disk images. Programs that need APIs it does not implement will report the missing function.\n\n"
    "4. Fonts: Liberation family, SIL Open Font License. Boot loader: GNU GRUB, GNU General Public License v3; "
    "its source code is available from the GNU project.\n\n"
    "5. THE WRLD OS IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED. IN NO EVENT SHALL THE AUTHORS BE "
    "LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY ARISING FROM THE USE OF THIS SOFTWARE.\n\n"
    "By pressing F8 you agree to these terms.";

bool setup_text_active;
static void setup_text_body(bool from_desktop) {
    from_desk = from_desktop;
    cw = MAX(7, text_width(FONT_MONO, "M"));
    /* loading */
    if (!from_desktop) {
        static const char *load[] = { "Setup is loading files (Kernel Debugger DLL)...", "Setup is loading files (ATA/IDE Disk Driver)...",
            "Setup is loading files (NTFS File System)...", "Setup is loading files (FAT32 File System)...", "Setup is loading files (GRUB Boot Loader)...", "Setup is loading files (Win32 Subsystem)...",
            "Setup is loading files (Luna Visual Styles)...", "Setup is starting " OS_NAME "..." };
        for (unsigned i = 0; i < ARRAY_LEN(load); i++) { ts_begin(load[i], NULL); ts_present(); task_sleep(230); }
    }
    /* welcome */
    for (;;) {
        ts_begin("ENTER=Continue  F3=Quit", NULL);
        tx(2, 3, "Welcome to Setup.", TS_HI);
        tx_wrap(2, 5, "This portion of the Setup program prepares " OS_NAME " to run on your computer.", TS_FG);
        bullet(4, 8, "To set up " OS_NAME " now, press ENTER.", TS_FG);
        bullet(4, 10, "To quit Setup without installing " OS_NAME ", press F3.", TS_FG);
        ts_present();
        int k = ts_key();
        if (k == KEY_ENTER) break;
        if (k == KEY_F3 && confirm_quit()) { quit_setup(); return; }
    }
    if (!blk_count()) {
        ts_begin("F3=Quit", NULL);
        if (blk_failed[0]) {
            char m[600];
            snprintf(m, sizeof m, "Setup found a hard disk, but it did not respond:\n\n    %s\n\n"
                     "Try another disk controller type in the virtual machine's settings (IDE works everywhere; SATA, NVMe and SCSI are also supported). Setup cannot continue.\n\nTo quit Setup, press F3.", blk_failed);
            tx_wrap(2, 3, m, TS_FG);
        } else
            tx_wrap(2, 3, "Setup did not find any hard disk drives installed in your computer.\n\nMake sure the drives are connected (for QEMU, add a disk with -hda or run 'make run-install'). Setup cannot continue.\n\nTo quit Setup, press F3.", TS_FG);
        ts_present();
        while (ts_key() != KEY_F3) {}
        quit_setup(); return;
    }
    /* license */
    for (int top = 0;;) {
        ts_begin("F8=I agree  ESC=I do not agree  PAGE DOWN=Next Page", NULL);
        fill_rect(&screen, X(1), Y(2), SCREEN_W - 2 * X(1), Y(34) - Y(2), HEX(0xA8A8A8));
        /* render license with a scroll offset by drawing into rows and skipping */
        int row = 3 - top;
        const char *s = license_text;
        while (*s) {
            int maxc = 104, n = strlen(s), cut = n;
            const char *nl = strchr(s, '\n'); if (nl && nl - s < cut) cut = nl - s;
            if (cut > maxc) { cut = maxc; while (cut > 0 && s[cut] != ' ') cut--; }
            if (row >= 3 && row < 33) draw_text_n(&screen, FONT_MONO, X(3), Y(row), s, cut, row == 3 - top ? HEX(0x000080) : BLACK);
            row++; s += cut; if (*s == ' ') s++; else if (*s == '\n') s++;
        }
        ts_present();
        int k = ts_key();
        if (k == KEY_F8) break;
        if (k == KEY_PGDN || k == KEY_DOWN) { if (row > 33) top += k == KEY_PGDN ? 20 : 1; }
        if (k == KEY_PGUP || k == KEY_UP) top = MAX(0, top - (k == KEY_PGUP ? 20 : 1));
        if (k == KEY_ESC || k == KEY_F3) { if (confirm_quit()) { quit_setup(); return; } }
    }
    /* partition selection */
    int sel = -1;
    for (;;) {
        build_items();
        if (sel < 0 || sel >= nitems || items[sel].kind == IT_DISK) { sel = -1; for (int i = 0; i < nitems; i++) if (items[i].kind != IT_DISK) { sel = i; break; } }
        for (;;) {
            ts_begin("ENTER=Install  C=Create Partition  D=Delete Partition  F3=Quit", NULL);
            tx(2, 3, "The following list shows the existing partitions and", TS_FG);
            tx(2, 4, "unpartitioned space on this computer.", TS_FG);
            tx(2, 6, "Use the UP and DOWN ARROW keys to select an item in the list.", TS_FG);
            bullet(4, 8, "To set up " OS_NAME " on the selected item, press ENTER.", TS_FG);
            bullet(4, 10, "To create a partition in the unpartitioned space, press C.", TS_FG);
            bullet(4, 12, "To delete the selected partition, press D.", TS_FG);
            int bx = X(2), by = Y(15) - 6, bw = SCREEN_W - 2 * bx, bh = Y(33) - by;
            ts_box(bx, by, bw, bh, TS_FG, false);
            /* virtual rows (a blank line before each disk after the first), scrolled to keep the selection visible */
            static int vrow[BLK_MAX * 10], top;
            int vr = 0;
            for (int i = 0; i < nitems; i++) {
                if (items[i].kind == IT_DISK) { if (i) vr++; vrow[i] = vr; vr += 2; }
                else vrow[i] = vr++;
            }
            const int vis = 16;
            if (sel >= 0) {
                int want = vrow[sel];
                for (int i = sel - 1; i >= 0; i--) if (items[i].kind == IT_DISK) { if (vrow[sel] - vrow[i] < vis) want = vrow[i]; break; }
                if (want < top) top = want;
                if (vrow[sel] >= top + vis) top = vrow[sel] - vis + 1;
            }
            if (top > 0 && vr - top < vis) top = MAX(0, vr - vis);
            int row = 16;
            for (int i = 0; i < nitems; i++) {
                char b[128];
                int r = 16 + vrow[i] - top;
                if (r < 16 || r >= 16 + vis) continue;
                row = r + 1;
                if (items[i].kind == IT_DISK) { disk_desc(items[i].disk, b, sizeof b); tx(4, r, b, TS_FG); continue; }
                part_desc(&items[i], b, sizeof b);
                if (i == sel) { fill_rect(&screen, X(5), Y(r) - 1, bw - 3 * 4 * cw, LH, TS_FG); tx(6, r, b, TS_BG); }
                else tx(6, r, b, TS_FG);
            }
            if (top > 0) tx((SCREEN_W - 48) / cw - 4, 16, "^", TS_FG);
            if (vr - top > vis) tx((SCREEN_W - 48) / cw - 4, 15 + vis, "v", TS_FG);
            if (sel < 0) tx(6, row + 1, "(no usable space on the disks)", TS_FG);
            ts_present();
            int k = ts_key();
            if (k == KEY_UP && sel >= 0) { for (int i = sel - 1; i >= 0; i--) if (items[i].kind != IT_DISK) { sel = i; break; } }
            else if (k == KEY_DOWN && sel >= 0) { for (int i = sel + 1; i < nitems; i++) if (items[i].kind != IT_DISK) { sel = i; break; } }
            else if (k == KEY_F3) { if (confirm_quit()) { quit_setup(); return; } }
            else if (k == 'c' && sel >= 0 && items[sel].kind == IT_FREE) { if (create_partition(&items[sel]) >= 0) break; }
            else if (k == 'd' && sel >= 0 && items[sel].kind == IT_PART) { if (delete_partition(&items[sel])) break; }
            else if (k == KEY_ENTER && sel >= 0) {
                item_t it = items[sel];
                int slot = it.slot;
                bool fresh = false;
                if (!bootinfo.is_efi && blk_get(it.disk)->bsize != 512) {
                    message_screen("This disk uses 4 KB sectors (Advanced Format 4Kn). A computer cannot start from such a disk with a BIOS, so " OS_NAME
                                   " cannot be installed on it.\n\n"
                                   "Select a partition on another disk, or set up the virtual disk with 512-byte sectors.\n\nPress ENTER to continue.", "ENTER=Continue");
                    break;
                }
                if (it.kind == IT_FREE) {
                    slot = free_slot(it.disk);
                    if (slot < 0) { message_screen("The partition table on this disk is full. Delete a partition first.\n\nPress ENTER to continue.", "ENTER=Continue"); break; }
                    uint32_t secs = it.sectors;
                    if (bootinfo.is_efi && secs > 100u * 1024 * 2048) secs = 100u * 1024 * 2048;
                    ptab[it.disk][slot] = (part_t){ .type = 0x0C, .lba = it.lba, .sectors = secs };
                    if (!write_ptab(it.disk)) break;
                    fresh = true;
                }
                if ((fresh ? it.sectors : ptab[it.disk][slot].sectors) < 64 * 2048) {
                    message_screen("This partition is too small for " OS_NAME ". Setup needs at least 64 MB.\n\nPress ENTER to continue.", "ENTER=Continue"); break;
                }
                /* file system choice */
                bool keep_ok = !fresh && it.fat;
                uint32_t psecs = fresh ? it.sectors : ptab[it.disk][slot].sectors;
                bool big = psecs / 2048 > 120000;
                if (bootinfo.is_efi && big) {
                    message_screen("This FAT32 partition is larger than the 120 GB that Setup can mount on this system.\n\n"
                                   "Go back and create a smaller system partition (the installer caps new ones at 100 GB); the rest of the disk may stay unpartitioned.\n\nPress ENTER to continue.", "ENTER=Continue");
                    break;
                }
                static const char *opts[5] = { "Format the partition using the NTFS file system (Quick)", "Format the partition using the NTFS file system",
                                               "Format the partition using the FAT file system (Quick)", "Format the partition using the FAT file system",
                                               "Leave the current file system intact (no changes)" };
                /* UEFI firmware boots from an EFI System Partition, which must be
                 * FAT; NTFS cannot be one.  Only offer formats that can boot.
                 * A FAT32 volume above ~120 GB has more than the 16 MB FAT this
                 * driver caches, so do not offer it on a BIOS install either. */
                int modes[5], nopt = 0;
                if (bootinfo.is_efi) {
                    modes[nopt++] = 2; modes[nopt++] = 3;
                    if (keep_ok && it.fskind == FS_FAT32) modes[nopt++] = 4;
                } else {
                    modes[nopt++] = 0; modes[nopt++] = 1;
                    if (!big) { modes[nopt++] = 2; modes[nopt++] = 3; }
                    if (keep_ok && (it.fskind == FS_NTFS || !big)) modes[nopt++] = 4;
                }
                int o = 0;
                bool back = false;
                for (;;) {
                    ts_begin("ENTER=Continue  ESC=Cancel", NULL);
                    item_t cur = it; cur.kind = IT_PART; cur.slot = slot; cur.type = fresh ? 0x07 : it.type; cur.letter = it.kind == IT_FREE ? 'C' : it.letter;
                    if (fresh) { cur.lba = ptab[it.disk][slot].lba; cur.sectors = ptab[it.disk][slot].sectors; cur.fat = false; }
                    char pd[128], dd[96]; part_desc(&cur, pd, sizeof pd); disk_desc(it.disk, dd, sizeof dd);
                    if (fresh || !it.fat) tx_wrap(2, 3, "The partition you selected is not formatted. Setup will now format the partition.", TS_FG);
                    else tx(2, 3, "Setup will install " OS_NAME " on partition", TS_FG);
                    tx(6, 6, pd, TS_HI); tx(2, 8, "on", TS_FG); tx(2, 9, dd, TS_FG);
                    if (bootinfo.is_efi)
                        tx_wrap(2, 11, "This computer started Setup in UEFI mode, so the system partition must be FAT32 (it becomes the EFI System Partition). Press ESC to choose another partition.", TS_FG);
                    else
                        tx_wrap(2, 11, "Use the UP and DOWN ARROW keys to select the file system you want, and then press ENTER. If you want to select a different partition for " OS_NAME ", press ESC.", TS_FG);
                    ts_box(X(4), Y(15) - 6, 66 * cw, (nopt + 1) * LH + 6, TS_FG, false);
                    for (int i = 0; i < nopt; i++) {
                        if (i == o) { fill_rect(&screen, X(5), Y(15 + i) - 1, 64 * cw, LH, TS_FG); tx(6, 15 + i, opts[modes[i]], TS_BG); }
                        else tx(6, 15 + i, opts[modes[i]], TS_FG);
                    }
                    ts_present();
                    int k2 = ts_key();
                    if (k2 == KEY_UP) o = (o + nopt - 1) % nopt;
                    else if (k2 == KEY_DOWN) o = (o + 1) % nopt;
                    else if (k2 == KEY_ESC) { back = true; break; }
                    else if (k2 == KEY_ENTER) {
                        if (modes[o] != 4 && it.fat && !fresh) {
                            ts_begin("F=Format  ESC=Cancel", NULL);
                            tx(2, 3, "WARNING: Formatting this drive will delete all files on it.", TS_HI);
                            tx(2, 5, "To format the drive, press F. To select a different partition, press ESC.", TS_FG);
                            ts_present();
                            int k3; do k3 = ts_key(); while (k3 != 'f' && k3 != KEY_ESC);
                            if (k3 == KEY_ESC) continue;
                        }
                        break;
                    }
                }
                if (back) break;
                if (do_install(it.disk, slot, modes[o])) { finished_screen(); return; }
                break;
            }
        }
    }
}
/* Setup runs on the GUI task and paints its own progress; while it is in
 * control there is no desktop loop to heartbeat, so the watchdog must not
 * mistake a long format for a hung desktop. */
void setup_text_mode(bool from_desktop) {
    setup_text_active = true;
    setup_text_body(from_desktop);
    setup_text_active = false;
}

/* ======================================================================
 * graphical setup
 *
 * The same wizard look as the first-boot pages: the blue Setup background,
 * a step list on the left and a white window.  It runs its own small event
 * loop, so it works both from the boot menu (before wm_run) and from the
 * live desktop.  The text front-end above stays as a fallback for
 * "setup=text" on the kernel command line.
 * ====================================================================== */
enum { SG_WELCOME, SG_LICENSE, SG_DISK, SG_CONFIRM, SG_INSTALL, SG_DONE, SG_QUIT = -1 };
enum { SGA_NONE, SGA_NEXT, SGA_BACK, SGA_NEW, SGA_DELETE, SGA_INSTALL, SGA_RESTART, SGA_CANCEL };

typedef struct {
    int page, sel, top, fs, lic_top, action;
    bool accepted, confirmed;
} sg_t;
static sg_t SG;
static int SG_fs[5], SG_nfs;
static bool SG_from_desk;
static canvas_t *sbg;
static canvas_t *slic;                 /* the whole licence, wrapped once */
static window_t sw;

static const char *sg_steps[] = { "Welcome", "License agreement", "Where to install", "Confirm", "Installing", "Finished" };
static void page_header(canvas_t *s, const char *title, const char *sub);

static void sg_left(int step) {
    canvas_t *c = &screen;
    int y = 100;
    for (int i = 0; i < 6; i++) {
        uint32_t col = i == step ? WHITE : i < step ? HEX(0xC8D8FF) : HEX(0x8AA4E8);
        fill_circle(c, 30, y + 7, 5, i <= step ? (i == step ? HEX(0xFFB54A) : HEX(0x7CE07C)) : HEX(0x5C78D0));
        draw_text(c, i == step ? FONT_UI_BOLD : FONT_UI, 44, y, sg_steps[i], col);
        y += 30;
    }
    y += 20;
    if (SG.page == SG_INSTALL) {
        draw_text(c, FONT_UI, 22, y, "Writing to the disk.", HEX(0xE0E8FF)); y += 16;
        draw_text(c, FONT_UI, 22, y, "Do not turn off the computer.", HEX(0xE0E8FF));
    } else if (SG.page == SG_DONE) {
        draw_text(c, FONT_UI, 22, y, "Installation complete.", HEX(0xE0E8FF)); y += 16;
        draw_text(c, FONT_UI, 22, y, "Restart to finish.", HEX(0xE0E8FF));
    } else {
        draw_text(c, FONT_UI, 22, y, "Setup changes nothing", HEX(0xE0E8FF)); y += 16;
        draw_text(c, FONT_UI, 22, y, "until you click Install.", HEX(0xE0E8FF));
    }
}
static void sg_bg(void) {
    if (sbg && sbg->w == SCREEN_W && sbg->h == SCREEN_H) return;
    if (sbg) canvas_free(sbg);
    sbg = canvas_new(SCREEN_W, SCREEN_H);
    canvas_t *c = sbg;
    gradient_v(c, 0, 0, SCREEN_W, 64, HEX(0x0A2A8C), HEX(0x163EB0));
    hline(c, 0, 63, SCREEN_W, HEX(0x6A8EEA));
    gradient_v(c, 0, 64, SCREEN_W, SCREEN_H - 128, HEX(0x5C82E0), HEX(0x3A5CC4));
    gradient_v(c, 0, SCREEN_H - 64, SCREEN_W, 64, HEX(0x163EB0), HEX(0x0A2A8C));
    gradient_h(c, 0, SCREEN_H - 64, SCREEN_W, 2, HEX(0xE88A30), HEX(0xFFD08A));
    fill_rect(c, 0, 64, 250, SCREEN_H - 128, HEX(0x2E50B8));
    vline(c, 250, 64, SCREEN_H - 128, HEX(0x7A9AF0));
    draw_globe(c, 18, 6, 64, -36);
    draw_wordmark(c, 92, 12, WORDMARK_SMALL, WHITE);
}
static rect_t sg_rect(void) { return R(292, 92, SCREEN_W - 332, SCREEN_H - 184); }
static void sg_sync(void) {
    rect_t r = sg_rect();
    sw.x = r.x; sw.y = r.y; sw.w = r.w; sw.h = r.h;
    sw.flags = WF_NO_MAX | WF_NO_MIN | WF_DIALOG;
    rect_t cr = wm_client_rect(&sw);
    if (!sw.surf || sw.surf->w != cr.w || sw.surf->h != cr.h) {
        if (sw.surf) canvas_free(sw.surf);
        sw.surf = canvas_new(cr.w, cr.h);
    }
}
static void sg_mb(uint64_t mb, char *b, int n) {
    if (mb >= 1024ull * 1024) {
        uint64_t t = (mb * 10 + 524288) >> 20;
        snprintf(b, n, "%llu.%llu TB", (unsigned long long)(t / 10), (unsigned long long)(t % 10));
    } else if (mb >= 1024) {
        uint64_t t = (mb * 10 + 512) >> 10;
        snprintf(b, n, "%llu.%llu GB", (unsigned long long)(t / 10), (unsigned long long)(t % 10));
    } else snprintf(b, n, "%llu MB", (unsigned long long)mb);
}
static void sg_right(canvas_t *s, int font, int right, int y, const char *t, uint32_t col) {
    draw_text(s, font, right - text_width(font, t), y, t, col);
}
static int sg_item_h(item_t *it) { return it->kind == IT_DISK ? 38 : 26; }
static const char *sg_fs_label(int mode) {
    switch (mode) {
    case 0: return "NTFS (Quick format)";
    case 1: return "NTFS (full format - writes every sector)";
    case 2: return "FAT32 (Quick format)";
    case 3: return "FAT32 (full format - writes every sector)";
    default: return "Keep the existing file system (no formatting)";
    }
}
static void sg_build_fs(item_t *it) {
    SG_nfs = 0;
    bool big = it->sectors / 2048 > 120000;
    if (bootinfo.is_efi) {
        SG_fs[SG_nfs++] = 2; SG_fs[SG_nfs++] = 3;
        if (it->fat && it->fskind == FS_FAT32) SG_fs[SG_nfs++] = 4;
    } else {
        SG_fs[SG_nfs++] = 0; SG_fs[SG_nfs++] = 1;
        if (!big) { SG_fs[SG_nfs++] = 2; SG_fs[SG_nfs++] = 3; }
        if (it->fat && (it->fskind == FS_NTFS || !big)) SG_fs[SG_nfs++] = 4;
    }
    SG.fs = 0;
}
static void sg_select_default(void) {
    SG.sel = -1;
    for (int i = 0; i < nitems; i++) if (items[i].kind != IT_DISK) { SG.sel = i; break; }
    SG.top = 0;
}
static void sg_ensure_visible(int listh) {
    if (SG.sel < 0) return;
    if (SG.sel < SG.top) SG.top = SG.sel;
    for (;;) {
        int h = 0;
        for (int i = SG.top; i < nitems; i++) {
            if (h + sg_item_h(&items[i]) > listh) break;
            h += sg_item_h(&items[i]);
        }
        int last = SG.top;
        h = 0;
        for (int i = SG.top; i < nitems && h + sg_item_h(&items[i]) <= listh; i++) { h += sg_item_h(&items[i]); last = i + 1; }
        if (SG.sel < last) break;
        if (last == SG.top || SG.top >= SG.sel) break;
        SG.top = MIN(SG.sel, SG.top + 1);
    }
}

/* ---------- modal prompts ---------- */
static bool gui_confirm(const char *title, const char *msg, const char *yes, const char *no) {
    static canvas_t *base;
    if (!base || base->w != SCREEN_W || base->h != SCREEN_H) { if (base) canvas_free(base); base = canvas_new(SCREEN_W, SCREEN_H); }
    memcpy(base->px, screen.px, SCREEN_W * SCREEN_H * 4);
    int w = MIN(460, SCREEN_W - 80);
    int h = MIN(120 + text_wrapped_height(FONT_UI, w - 40, msg, 5), SCREEN_H - 80);
    rect_t box = R((SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h);
    rect_t by = R(box.x + box.w - (no ? 196 : 106), box.y + box.h - 38, 90, 26);
    rect_t bn = R(box.x + box.w - 100, box.y + box.h - 38, 90, 26);
    mouse_state_t ms; key_event_t ke;
    int prev = 0;
    while (mouse_poll(&ms)) { wm_mouse_x = ms.x; wm_mouse_y = ms.y; prev = ms.buttons; }
    while (keyboard_poll(&ke)) {}
    for (;;) {
        memcpy(screen.px, base->px, SCREEN_W * SCREEN_H * 4);
        canvas_reset_clip(&screen);
        blend_rect(&screen, 0, 0, SCREEN_W, SCREEN_H, BLACK, 120);
        fill_rect(&screen, box.x + 4, box.y + 4, box.w, box.h, HEX(0x303030));
        fill_rect(&screen, box.x, box.y, box.w, box.h, HEX(0xE8EEF8));
        gradient_v(&screen, box.x + 1, box.y + 1, box.w - 2, 28, HEX(0x163EB0), HEX(0x0A2A8C));
        draw_text(&screen, FONT_UI_BOLD, box.x + 10, box.y + 7, title, WHITE);
        draw_text_wrapped(&screen, FONT_UI, box.x + 20, box.y + 42, w - 40, msg, BLACK, 5);
        bool hy = pt_in(by, wm_mouse_x, wm_mouse_y);
        bool hn = no && pt_in(bn, wm_mouse_x, wm_mouse_y);
        draw_button(&screen, by, yes, hy, false, false, true);
        if (no) draw_button(&screen, bn, no, hn, false, false, true);
        gfx_present(hy || hn ? CURSOR_HAND : CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
        while (mouse_poll(&ms)) {
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            int ch = ms.buttons ^ prev;
            if ((ch & 1) && !(ms.buttons & 1)) {
                if (pt_in(by, ms.x, ms.y)) return true;
                if (no && pt_in(bn, ms.x, ms.y)) return false;
            }
            prev = ms.buttons;
        }
        while (keyboard_poll(&ke)) {
            if (!ke.pressed) continue;
            if (ke.key == KEY_ENTER) return true;
            if (ke.key == KEY_ESC) return false;
        }
        task_yield();
    }
}
static void gui_setup_message(const char *msg) { gui_confirm("Setup", msg, "OK", NULL); }

/* ---------- progress page (also used from do_install) ---------- */
static void gui_setup_progress(const char *caption, int pct, const char *detail) {
    gui_heartbeat = timer_ticks();
    sg_bg();
    sg_sync();
    memcpy(screen.px, sbg->px, SCREEN_W * SCREEN_H * 4);
    canvas_reset_clip(&screen);
    sg_left(SG_INSTALL);
    rect_t r = sg_rect();
    draw_window_frame(&screen, r, OS_NAME " Setup", ICON_LOGO, true, WF_NO_MAX | WF_NO_MIN | WF_NOCLOSE, -1, -1, WS_NORMAL);
    rect_t cr = wm_client_rect(&sw);
    fill_rect(&screen, cr.x, cr.y, cr.w, cr.h, WHITE);
    int x = cr.x + 24, y = cr.y + 24, iw = cr.w - 48;
    draw_text(&screen, FONT_LARGE, x, y, "Installing " OS_NAME, BLACK); y += 36;
    draw_text_ellipsis(&screen, FONT_UI, x, y, iw, caption, HEX(0x303030)); y += 30;
    draw_progress(&screen, R(x, y, iw, 22), pct);
    char b[16]; snprintf(b, sizeof b, "%d%%", CLAMP(pct, 0, 100));
    draw_text_centered(&screen, FONT_UI_BOLD, R(x, y, iw, 22), b, BLACK); y += 40;
    if (detail) draw_text_wrapped(&screen, FONT_UI, x, y, iw, detail, HEX(0x505050), 4);
    draw_text(&screen, FONT_UI, x, cr.y + cr.h - 30, "Do not turn off the computer.", HEX(0x9C1C1C));
    gfx_present(CURSOR_BUSY, wm_mouse_x, wm_mouse_y);
}

/* ---------- pages ---------- */
static void sg_footer(window_t *w, bool back, bool cancel, const char *next, bool next_ok, int next_action) {
    canvas_t *s = w->surf;
    int fy = s->h - 40;
    hline(s, 0, fy, s->w, HEX(0xA0A0A0)); hline(s, 0, fy + 1, s->w, WHITE);
    int x = 16;
    if (cancel) { if (ui_button(w, R(x, fy + 8, 80, 24), "Cancel")) SG.action = SGA_CANCEL; x += 88; }
    if (back) { if (ui_button(w, R(x, fy + 8, 80, 24), "< Back")) SG.action = SGA_BACK; }
    if (next) if (ui_button_ex(w, R(s->w - 106, fy + 8, 90, 24), next, next_ok, true)) SG.action = next_action;
}
static void sg_welcome(window_t *w) {
    canvas_t *s = w->surf;
    page_header(s, "Welcome to " OS_NAME " Setup", "This wizard puts " OS_NAME " on a hard disk.");
    int y = 84, bx = 20, bw = s->w - 40;
    y += draw_text_wrapped(s, FONT_UI, bx, y, bw, "Setup will show every disk, partition, drive letter and size before it changes anything. "
        "Nothing is written until you review the summary and click Install.", BLACK, 5) + 12;
    y += 8;
    draw_text(s, FONT_UI_BOLD, bx, y, "What Setup will do", BLACK); y += 20;
    draw_text_wrapped(s, FONT_UI, bx + 16, y, bw - 16, "Create or choose a partition, format it, copy the system files and install the boot loader.", HEX(0x303030), 4); y += 36;
    draw_text(s, FONT_UI_BOLD, bx, y, "Before you continue", BLACK); y += 20;
    draw_text_wrapped(s, FONT_UI, bx + 16, y, bw - 16, "Formatting deletes every file on the selected partition. Setup asks you to confirm, and the summary shows the exact disk number, partition number, letter and size.", HEX(0x303030), 4); y += 40;
    if (y < s->h - 90) {
        fill_rect(s, bx, y, bw, 44, HEX(0xFFF4D8));
        draw_rect(s, bx, y, bw, 44, HEX(0xE0B040));
        draw_text_wrapped(s, FONT_UI, bx + 10, y + 8, bw - 20, "If this computer already has a system on the disk, stop now and make a backup first.", HEX(0x6A4000), 4);
    }
    sg_footer(w, false, true, "Next >", true, SGA_NEXT);
}
static void sg_license(window_t *w) {
    canvas_t *s = w->surf;
    page_header(s, "License Agreement", "Read the terms, then accept them to continue.");
    rect_t box = R(20, 78, s->w - 40, s->h - 188);
    draw_sunken_box(s, box, WHITE);
    int lw = box.w - 18;
    int lh = text_wrapped_height(FONT_UI, lw, license_text, 4);
    if (!slic || slic->w != lw || slic->h != lh) {
        if (slic) canvas_free(slic);
        slic = canvas_new(lw, MAX(1, lh));
        gfx_clear(slic, WHITE);
        draw_text_wrapped(slic, FONT_UI, 0, 0, lw, license_text, BLACK, 4);
    }
    if (w->ui.wheel) SG.lic_top -= w->ui.wheel * 40;
    SG.lic_top = CLAMP(SG.lic_top, 0, MAX(0, lh - (box.h - 10)));
    int vis = MIN(box.h - 10, lh - SG.lic_top);
    if (vis > 0) blit(s, box.x + 8, box.y + 5, slic, 0, SG.lic_top, lw, vis);
    if (lh > box.h - 10) draw_scrollbar(s, R(box.x + box.w - 18, box.y + 1, 17, box.h - 2), lh, box.h - 10, SG.lic_top, true);
    ui_checkbox(w, 20, box.y + box.h + 10, "I accept the terms in the License Agreement", &SG.accepted);
    sg_footer(w, true, true, "Next >", SG.accepted, SGA_NEXT);
}
static void sg_disk(window_t *w) {
    canvas_t *s = w->surf;
    page_header(s, "Where do you want to install " OS_NAME "?", "Check the disk number, drive letter, file system and size.");
    int bx = 16, by = 74, bw = s->w - 32, listh = s->h - by - 104;
    draw_sunken_box(s, R(bx, by, bw, listh), WHITE);
    if (w->ui.wheel) {
        int n = w->ui.wheel > 0 ? 3 : -3;
        while (n > 0) { for (int i = SG.sel + 1; i < nitems; i++) if (items[i].kind != IT_DISK) { SG.sel = i; break; } n--; }
        while (n < 0) { for (int i = SG.sel - 1; i >= 0; i--) if (items[i].kind != IT_DISK) { SG.sel = i; break; } n++; }
    }
    sg_ensure_visible(listh);
    rect_t old = canvas_push_clip(s, R(bx + 1, by + 1, bw - 2, listh - 2));
    int y = by + 3;
    for (int i = SG.top; i < nitems && y < by + listh; i++) {
        item_t *it = &items[i];
        int h = sg_item_h(it);
        if (it->kind == IT_DISK) {
            blkdev_t *dv = blk_get(it->disk);
            char sz[32], t[32], line[220];
            sg_mb((dv->total_sectors ? dv->total_sectors : dv->sectors) / 2048, sz, sizeof sz);
            snprintf(t, sizeof t, "Disk %d", it->disk);
            snprintf(line, sizeof line, "%s  -  %s  -  %s%s", dv->model, dv->driver,
                     gpt_disk[it->disk] ? "GPT partition table (will be replaced)" : has_mbr[it->disk] ? "MBR partition table" : "no partition table",
                     dv->total_sectors > dv->sectors ? "  -  2 TiB addressable" : "");
            fill_rect(s, bx + 2, y, bw - 4, h - 2, HEX(0xE4ECF8));
            draw_text(s, FONT_UI_BOLD, bx + 10, y + 4, t, HEX(0x1E3287));
            sg_right(s, FONT_UI_BOLD, bx + bw - 10, y + 4, sz, HEX(0x1E3287));
            draw_text_ellipsis(s, FONT_SMALL, bx + 80, y + 21, bw - 96, line, HEX(0x505A68));
            if (ui_click_area(w, R(bx + 2, y, bw - 4, h - 2))) {
                for (int k = i + 1; k < nitems && items[k].kind != IT_DISK; k++) if (items[k].kind != IT_DISK) { SG.sel = k; break; }
            }
        } else {
            bool sel = i == SG.sel;
            rect_t row = R(bx + 2, y, bw - 4, h - 2);
            uint32_t fg = sel ? WHITE : BLACK, fg2 = sel ? HEX(0xE0E8FF) : HEX(0x505050);
            if (sel) fill_rect(s, row.x, row.y, row.w, row.h, T.select);
            else if (ui_hover(w, row)) fill_rect(s, row.x, row.y, row.w, row.h, HEX(0xEAF4FD));
            if (ui_click_area(w, row)) SG.sel = i;
            char lab[80], fs[24], sz[32], fr[40] = "";
            if (it->kind == IT_FREE) {
                snprintf(lab, sizeof lab, "Unpartitioned space");
                snprintf(fs, sizeof fs, "-");
                sg_mb(it->sectors / 2048, sz, sizeof sz);
                snprintf(fr, sizeof fr, "%s available", sz);
            } else {
                snprintf(lab, sizeof lab, "%c:   Partition %d", it->letter, it->slot + 1);
                if (it->fat) snprintf(fs, sizeof fs, "%s", it->fskind == FS_NTFS ? "NTFS" : "FAT32");
                else if (it->type == 0x0C || it->type == 0x07) snprintf(fs, sizeof fs, "Raw");
                else snprintf(fs, sizeof fs, "%s", part_type_name(it->type));
                sg_mb(it->sectors / 2048, sz, sizeof sz);
                if (it->fat) snprintf(fr, sizeof fr, "%d MB free", it->free_mb);
            }
            draw_text_ellipsis(s, FONT_UI, bx + 14, y + 4, bw - 300, lab, fg);
            draw_text(s, FONT_UI, bx + 310, y + 4, fs, fg2);
            sg_right(s, FONT_UI, bx + bw - 132, y + 4, it->kind == IT_FREE ? fr : sz, fg);
            if (it->kind != IT_FREE) sg_right(s, FONT_UI, bx + bw - 10, y + 4, fr, fg2);
        }
        y += h;
    }
    s->clip = old;
    if (nitems > 0) {
        int totalh = 0; for (int i = 0; i < nitems; i++) totalh += sg_item_h(&items[i]);
        if (totalh > listh) {
            int vis = 1, hh = 0;
            for (int i = SG.top; i < nitems && hh + sg_item_h(&items[i]) <= listh; i++) { hh += sg_item_h(&items[i]); vis++; }
            draw_scrollbar(s, R(bx + bw - 18, by + 1, 17, listh - 2), nitems, MAX(1, vis), SG.top, true);
        }
    }
    int sy = by + listh + 8;
    if (SG.sel >= 0 && SG.sel < nitems && items[SG.sel].kind != IT_DISK) {
        item_t *it = &items[SG.sel];
        char b[240];
        if (it->kind == IT_FREE) {
            char sz[32]; sg_mb(it->sectors / 2048, sz, sizeof sz);
            snprintf(b, sizeof b, "Selected: Disk %d, unpartitioned space, %s", it->disk, sz);
        } else {
            char sz[32]; sg_mb(it->sectors / 2048, sz, sizeof sz);
            snprintf(b, sizeof b, "Selected: Disk %d, %c:, Partition %d, %s, %s%s", it->disk, it->letter, it->slot + 1,
                     it->fat ? (it->fskind == FS_NTFS ? "NTFS" : "FAT32") : "not formatted", sz, it->fat ? "" : " (will be formatted)");
        }
        draw_text_ellipsis(s, FONT_UI, bx, sy, bw - 190, b, BLACK);
        if (gpt_disk[it->disk])
            draw_text(s, FONT_SMALL, bx, sy + 18, "This disk uses GPT. Setup replaces it with an MBR table and erases all its partitions.", HEX(0x9C4A00));
        else
            draw_text(s, FONT_SMALL, bx, sy + 18, "Select a partition, or unpartitioned space and click New partition.", HEX(0x505A68));
    } else {
        draw_text(s, FONT_UI, bx, sy, "There is no unpartitioned space on these disks.", HEX(0x9C1C1C));
    }
    int x = s->w - 106 - 100 - 8;
    if (ui_button_ex(w, R(x - 100, s->h - 40 + 8, 92, 24), "New partition", SG.sel >= 0 && items[SG.sel].kind == IT_FREE, false)) SG.action = SGA_NEW;
    if (ui_button_ex(w, R(x, s->h - 40 + 8, 92, 24), "Delete", SG.sel >= 0 && items[SG.sel].kind == IT_PART, false)) SG.action = SGA_DELETE;
    sg_footer(w, false, true, "Next >", SG.sel >= 0 && items[SG.sel].kind != IT_DISK, SGA_NEXT);
}
static void sg_confirm(window_t *w) {
    canvas_t *s = w->surf;
    page_header(s, "Confirm your installation", "This is exactly what Setup is going to do.");
    if (SG.sel < 0 || SG.sel >= nitems || items[SG.sel].kind == IT_DISK) {
        draw_text(s, FONT_UI, 24, 90, "No partition is selected. Go back and choose one.", HEX(0x9C1C1C));
        sg_footer(w, true, true, NULL, false, SGA_NONE);
        return;
    }
    item_t *it = &items[SG.sel];
    blkdev_t *dv = blk_get(it->disk);
    int y = 84, bx = 24, bw = s->w - 48;
    char sz[32], b[260];
    sg_mb(it->sectors / 2048, sz, sizeof sz);
    draw_text(s, FONT_UI_BOLD, bx, y, "Install to", BLACK); y += 20;
    snprintf(b, sizeof b, "Disk %d, Partition %d (%c:)  -  %s", it->disk, it->slot + 1, it->letter, sz);
    draw_text(s, FONT_LARGE, bx + 12, y, b, HEX(0x1E3287)); y += 28;
    snprintf(b, sizeof b, "%s, %s, %s", dv->model, dv->driver, gpt_disk[it->disk] ? "GPT (will become MBR)" : has_mbr[it->disk] ? "MBR" : "no table");
    draw_text_ellipsis(s, FONT_UI, bx + 12, y, bw - 12, b, HEX(0x505050)); y += 26;
    int mode;
    draw_text(s, FONT_UI_BOLD, bx, y, "File system", BLACK); y += 22;
    for (int i = 0; i < SG_nfs; i++) {
        if (ui_radio(w, bx + 12, y, sg_fs_label(SG_fs[i]), SG.fs == i)) SG.fs = i;
        y += 22;
    }
    mode = SG_fs[CLAMP(SG.fs, 0, MAX(0, SG_nfs - 1))];
    y += 4;
    if ((mode == 1 || mode == 3) && it->sectors / 2048 > 120000)
        y += draw_text_wrapped(s, FONT_UI, bx, y, bw, "A full format writes every sector. On this disk it can take hours; Quick format is usually the right choice.", HEX(0x9C4A00), 4) + 8;
    if (gpt_disk[it->disk])
        y += draw_text_wrapped(s, FONT_UI, bx, y, bw, "This disk uses a GPT partition table. Setup writes an MBR table, which erases every existing partition on this disk.", HEX(0x9C1C1C), 4) + 8;
    if (mode != 4)
        y += draw_text_wrapped(s, FONT_UI, bx, y, bw, "Formatting deletes every file on the selected partition. This cannot be undone.", HEX(0x9C1C1C), 4) + 6;
    else
        y += draw_text_wrapped(s, FONT_UI, bx, y, bw, "The existing file system is kept, but Setup copies its own files into it and may overwrite files with the same names.", HEX(0x9C4A00), 4) + 6;
    ui_checkbox(w, bx, y, "I understand and want to continue.", &SG.confirmed);
    sg_footer(w, true, true, "Install", SG.confirmed, SGA_INSTALL);
}
static void sg_done(window_t *w) {
    canvas_t *s = w->surf;
    page_header(s, "Setup is complete", OS_NAME " is installed on your hard disk.");
    int y = 90, bx = 24, bw = s->w - 48;
    draw_text_wrapped(s, FONT_UI, bx, y, bw, "Remove the installation CD or USB drive, then restart the computer so it starts from the hard disk.", BLACK, 5);
    y += 50;
    draw_text_wrapped(s, FONT_UI, bx, y, bw, "Setup has written the boot menu and copied your files. The first start asks for your user name, computer name, time zone and look.", HEX(0x303030), 5);
    y += 60;
    draw_text(s, FONT_UI_BOLD, bx, y, "You can restart now.", BLACK);
    int fy = s->h - 40;
    hline(s, 0, fy, s->w, HEX(0xA0A0A0)); hline(s, 0, fy + 1, s->w, WHITE);
    if (ui_button_ex(w, R(s->w - 116, fy + 8, 100, 24), "Restart now", true, true)) SG.action = SGA_RESTART;
    if (SG_from_desk && ui_button(w, R(s->w - 226, fy + 8, 100, 24), "Close")) SG.action = SGA_CANCEL;
}
static void sg_page(window_t *w) {
    switch (SG.page) {
    case SG_WELCOME: sg_welcome(w); break;
    case SG_LICENSE: sg_license(w); break;
    case SG_DISK: sg_disk(w); break;
    case SG_CONFIRM: sg_confirm(w); break;
    case SG_DONE: sg_done(w); break;
    default: break;
    }
}
static void sg_paint(void) {
    sg_bg(); sg_sync();
    memcpy(screen.px, sbg->px, SCREEN_W * SCREEN_H * 4);
    canvas_reset_clip(&screen);
    sg_left(SG.page);
    rect_t r = sg_rect();
    draw_window_frame(&screen, r, OS_NAME " Setup", ICON_LOGO, true, WF_NO_MAX | WF_NO_MIN | WF_NOCLOSE, -1, -1, WS_NORMAL);
    rect_t cr = wm_client_rect(&sw);
    canvas_reset_clip(sw.surf);
    gfx_clear(sw.surf, WHITE);
    ui_begin(&sw); sg_page(&sw); ui_end(&sw);
    blit(&screen, cr.x, cr.y, sw.surf, 0, 0, cr.w, cr.h);
    sw.ui.pressed = sw.ui.released = sw.ui.rpressed = sw.ui.dblclick = false; sw.ui.wheel = 0;
    gfx_present(sw.ui.cursor ? sw.ui.cursor : CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
}

/* ---------- actions ---------- */
static void sg_new_partition(void) {
    if (SG.sel < 0 || items[SG.sel].kind != IT_FREE) return;
    int disk = items[SG.sel].disk;
    uint32_t lba = items[SG.sel].lba, secs = items[SG.sel].sectors;
    bool capped = false;
    if (bootinfo.is_efi && secs > 100u * 1024 * 2048) { secs = 100u * 1024 * 2048; capped = true; }
    char sz[32], q[360]; sg_mb(secs / 2048, sz, sizeof sz);
    snprintf(q, sizeof q, "Create a %s partition on Disk %d?%s%s", sz, disk,
             capped ? "\n\nThe FAT32 system partition is limited to 100 GB; the rest of the disk stays unpartitioned." : "",
             gpt_disk[disk] ? "\n\nThis disk uses GPT. Creating the partition writes a new MBR table and erases every existing partition on this disk." : "");
    if (!gui_confirm("Create partition", q, "Create", "Cancel")) return;
    int slot = free_slot(disk);
    if (slot < 0) { gui_setup_message("The partition table on this disk is full. Delete a partition first."); return; }
    ptab[disk][slot] = (part_t){ .type = 0x0C, .lba = lba, .sectors = secs };
    if (!write_ptab(disk)) { gui_setup_message("Setup could not write the partition table to the disk."); return; }
    gpt_disk[disk] = false;
    build_items();
    SG.sel = -1;
    for (int i = 0; i < nitems; i++) if (items[i].kind == IT_PART && items[i].disk == disk && items[i].slot == slot) { SG.sel = i; break; }
    if (SG.sel < 0) sg_select_default();
}
static void sg_delete_partition(void) {
    if (SG.sel < 0 || items[SG.sel].kind != IT_PART) return;
    item_t it = items[SG.sel];
    char sz[32], q[360]; sg_mb(it.sectors / 2048, sz, sizeof sz);
    snprintf(q, sizeof q, "Delete %c: Partition %d on Disk %d (%s)?\n\nEvery file on that partition will be lost.", it.letter, it.slot + 1, it.disk, sz);
    if (!gui_confirm("Delete partition", q, "Delete", "Cancel")) return;
    memset(&ptab[it.disk][it.slot], 0, sizeof ptab[it.disk][it.slot]);
    if (!write_ptab(it.disk)) { gui_setup_message("Setup could not write the partition table to the disk."); return; }
    build_items(); sg_select_default();
}
static void sg_disk_next(void) {
    if (SG.sel < 0 || SG.sel >= nitems || items[SG.sel].kind == IT_DISK) return;
    if (items[SG.sel].kind == IT_FREE) {
        int before = nitems;
        sg_new_partition();
        if (SG.sel < 0 || items[SG.sel].kind != IT_PART) return;
        (void)before;
    }
    item_t it = items[SG.sel];
    if (it.kind != IT_PART) return;
    if (it.sectors < 64 * 2048) { gui_setup_message("This partition is too small for " OS_NAME ". Setup needs at least 64 MB."); return; }
    sg_build_fs(&it);
    SG.confirmed = false;
    SG.page = SG_CONFIRM;
}
static void sg_install(void) {
    if (SG.sel < 0 || items[SG.sel].kind != IT_PART) return;
    item_t it = items[SG.sel];
    int mode = SG_fs[CLAMP(SG.fs, 0, MAX(0, SG_nfs - 1))];
    SG.page = SG_INSTALL;
    gui_setup_progress("Preparing the disk...", 0, NULL);
    bool ok = do_install(it.disk, it.slot, mode);
    SG.page = ok ? SG_DONE : SG_CONFIRM;
}
static void sg_action(void) {
    int a = SG.action; SG.action = SGA_NONE;
    switch (a) {
    case SGA_NEXT:
        if (SG.page == SG_WELCOME) SG.page = SG_LICENSE;
        else if (SG.page == SG_LICENSE) { if (SG.accepted) SG.page = SG_DISK; }
        else if (SG.page == SG_DISK) sg_disk_next();
        else if (SG.page == SG_DONE) system_reboot();
        break;
    case SGA_BACK:
        if (SG.page == SG_LICENSE) SG.page = SG_WELCOME;
        else if (SG.page == SG_DISK) SG.page = SG_LICENSE;
        else if (SG.page == SG_CONFIRM) SG.page = SG_DISK;
        break;
    case SGA_NEW: sg_new_partition(); break;
    case SGA_DELETE: sg_delete_partition(); break;
    case SGA_INSTALL: sg_install(); break;
    case SGA_RESTART: system_reboot(); break;
    case SGA_CANCEL:
        if (SG.page == SG_DONE) { if (SG_from_desk) SG.page = SG_QUIT; else system_reboot(); break; }
        if (gui_confirm("Exit Setup", "Setup will stop. Any partitions you created stay on the disk. Continue?", "Exit Setup", "Go back")) {
            if (SG_from_desk) SG.page = SG_QUIT;
            else system_reboot();
        }
        break;
    }
}
static void sg_key(key_event_t *e) {
    if (SG.page == SG_LICENSE) {
        if (e->key == KEY_PGDN || e->key == KEY_DOWN) { SG.lic_top += e->key == KEY_PGDN ? 200 : 20; return; }
        if (e->key == KEY_PGUP || e->key == KEY_UP) { SG.lic_top = MAX(0, SG.lic_top - (e->key == KEY_PGUP ? 200 : 20)); return; }
        if (e->key == ' ') { SG.accepted = !SG.accepted; return; }
    }
    if (SG.page == SG_DISK) {
        if (e->key == KEY_UP) {
            for (int i = SG.sel - 1; i >= 0; i--) if (items[i].kind != IT_DISK) { SG.sel = i; break; }
            return;
        }
        if (e->key == KEY_DOWN) {
            for (int i = SG.sel + 1; i < nitems; i++) if (items[i].kind != IT_DISK) { SG.sel = i; break; }
            return;
        }
    }
    if (SG.page == SG_CONFIRM) {
        if (e->key == KEY_UP && SG_nfs) { SG.fs = (SG.fs + SG_nfs - 1) % SG_nfs; return; }
        if (e->key == KEY_DOWN && SG_nfs) { SG.fs = (SG.fs + 1) % SG_nfs; return; }
        if (e->key == ' ') { SG.confirmed = !SG.confirmed; return; }
    }
    if (SG.page == SG_LICENSE && e->key == KEY_F8) { SG.accepted = true; SG.action = SGA_NEXT; return; }
    if (e->key == KEY_ENTER) {
        if (SG.page == SG_WELCOME) SG.action = SGA_NEXT;
        else if (SG.page == SG_LICENSE && SG.accepted) SG.action = SGA_NEXT;
        else if (SG.page == SG_DISK) SG.action = SGA_NEXT;
        else if (SG.page == SG_CONFIRM && SG.confirmed) SG.action = SGA_INSTALL;
        else if (SG.page == SG_DONE) SG.action = SGA_RESTART;
    } else if (e->key == KEY_ESC) {
        if (SG.page == SG_WELCOME || SG.page == SG_DONE) SG.action = SGA_CANCEL;
        else if (SG.page > SG_WELCOME) SG.action = SGA_BACK;
    }
}

void setup_gui_mode(bool from_desktop) {
    bool was_dark = dark_mode;
    if (was_dark) { dark_mode = false; theme_compute(theme_id, theme_hue, &T); }
    gui_setup = true; setup_text_active = true; SG_from_desk = from_desktop;
    memset(&SG, 0, sizeof SG); memset(&sw, 0, sizeof sw);
    SG.page = SG_WELCOME;
    build_items(); sg_select_default();
    sg_bg(); sg_sync();
    mouse_state_t ms; key_event_t ke; int prev = 0;
    while (mouse_poll(&ms)) { wm_mouse_x = ms.x; wm_mouse_y = ms.y; prev = ms.buttons; }
    while (keyboard_poll(&ke)) {}
    sg_paint();
    while (SG.page != SG_QUIT) {
        while (keyboard_poll(&ke)) {
            if (!ke.pressed) continue;
            sg_key(&ke);
            sg_paint();
        }
        bool repaint = false;
        while (mouse_poll(&ms)) {
            int oldx = wm_mouse_x, oldy = wm_mouse_y;
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            rect_t cr = wm_client_rect(&sw);
            sw.ui.mx = ms.x - cr.x; sw.ui.my = ms.y - cr.y;
            sw.ui.in_window = pt_in(R(0, 0, cr.w, cr.h), sw.ui.mx, sw.ui.my);
            int changed = ms.buttons ^ prev;
            if ((changed & 1) && (ms.buttons & 1)) { sw.ui.down = true; sw.ui.pressed = true; repaint = true; }
            if ((changed & 1) && !(ms.buttons & 1)) { sw.ui.down = false; sw.ui.released = true; repaint = true; }
            if (ms.wheel) { sw.ui.wheel = ms.wheel; repaint = true; }
            prev = ms.buttons;
            if (repaint) { sg_paint(); repaint = false; }
            else if (oldx != wm_mouse_x || oldy != wm_mouse_y)
                gfx_move_cursor(sw.ui.cursor ? sw.ui.cursor : CURSOR_ARROW, wm_mouse_x, wm_mouse_y);
        }
        if (SG.action) { sg_action(); if (SG.page != SG_QUIT) sg_paint(); }
        task_yield();
    }
    if (sw.surf) { canvas_free(sw.surf); sw.surf = NULL; }
    if (was_dark) { dark_mode = true; theme_compute(theme_id, theme_hue, &T); }
    gui_setup = false; setup_text_active = false;
}

/* desktop entry point (live session) */
static void desk_run(void *arg) {
    if (SCREEN_W < 900 || SCREEN_H < 650) setup_text_mode(true);
    else setup_gui_mode(true);
    wm_redraw_all();
}
static void desk_confirm(void *ctx, int r) { if (r == IDYES) wm_defer(desk_run, NULL); }
void setup_launch_from_desktop(void) {
    if (system_volume) {
        char m[300];
        snprintf(m, sizeof m, OS_NAME " is already installed on this computer and is running from Disk %d, Partition %d.\n\nTo reinstall, start the computer from the " OS_NAME " CD and choose \"Install " OS_NAME "\".", system_disk_index, system_part_index + 1);
        msgbox("Setup", m, MBI_INFO, MBX_OK, NULL, NULL);
        return;
    }
    storage_init();
    if (!blk_count()) {
        msgbox("Setup", "Setup did not find a hard disk in this computer.\n\nIn QEMU, start the CD with a disk attached: make run-install", MBI_WARNING, MBX_OK, NULL, NULL);
        return;
    }
    msgbox("Install " OS_NAME, "Setup will install " OS_NAME " on a hard disk so your files and settings are kept between restarts.\n\n"
           "Setup takes over the screen until it finishes. Unsaved work in open programs will be lost when the computer restarts.\n\nStart Setup now?",
           MBI_QUESTION, MBX_YESNO, desk_confirm, NULL);
}

/* ======================================================================
 * first boot: graphical "finishing setup" wizard
 * ====================================================================== */
bool setup_oobe_pending(void) { const char *s = settings_get("setup"); return s && !strcmp(s, "oobe"); }

static const struct { const char *name; int off; } zones[] = {
    { "(GMT-10:00) Hawaii", -600 }, { "(GMT-08:00) Pacific Time (US & Canada)", -480 }, { "(GMT-07:00) Mountain Time (US & Canada)", -420 },
    { "(GMT-06:00) Central Time (US & Canada)", -360 }, { "(GMT-05:00) Eastern Time (US & Canada)", -300 }, { "(GMT-03:00) Brasilia, Buenos Aires", -180 },
    { "(GMT) Greenwich Mean Time: London, Dublin, Lisbon", 0 }, { "(GMT+01:00) Amsterdam, Berlin, Paris, Rome", 60 },
    { "(GMT+02:00) Athens, Cairo, Helsinki, Istanbul", 120 }, { "(GMT+03:00) Moscow, Riyadh, Nairobi", 180 }, { "(GMT+04:00) Abu Dhabi, Dubai", 240 },
    { "(GMT+05:00) Islamabad, Karachi", 300 }, { "(GMT+05:30) Chennai, Kolkata, Mumbai, New Delhi", 330 }, { "(GMT+07:00) Bangkok, Hanoi, Jakarta", 420 },
    { "(GMT+08:00) Beijing, Hong Kong, Singapore, Perth", 480 }, { "(GMT+09:00) Osaka, Seoul, Tokyo", 540 }, { "(GMT+10:00) Brisbane, Melbourne, Sydney", 600 },
    { "(GMT+12:00) Auckland, Wellington", 720 },
};
typedef struct { int page; char name[32], org[48], comp[32], pass[32], pass2[32]; int tz, tzscroll, pic; char err[96]; bool finish; } oobe_t;
static oobe_t O;
static window_t ow;
static canvas_t *obg;

static void obg_render(void) {
    if (!obg) obg = canvas_new(SCREEN_W, SCREEN_H);
    canvas_t *c = obg;
    gradient_v(c, 0, 0, SCREEN_W, 64, HEX(0x0A2A8C), HEX(0x163EB0));
    hline(c, 0, 63, SCREEN_W, HEX(0x6A8EEA));
    gradient_v(c, 0, 64, SCREEN_W, SCREEN_H - 128, HEX(0x5C82E0), HEX(0x3A5CC4));
    gradient_v(c, 0, SCREEN_H - 64, SCREEN_W, 64, HEX(0x163EB0), HEX(0x0A2A8C));
    gradient_h(c, 0, SCREEN_H - 64, SCREEN_W, 2, HEX(0xE88A30), HEX(0xFFD08A));
    /* left steps column */
    fill_rect(c, 0, 64, 250, SCREEN_H - 128, HEX(0x2E50B8));
    vline(c, 250, 64, SCREEN_H - 128, HEX(0x7A9AF0));
    draw_globe(c, 18, 6, 64, -36);
    draw_wordmark(c, 92, 12, WORDMARK_SMALL, WHITE);
}
static void oobe_left(canvas_t *c, int step, int pct, const char *eta) {
    static const char *steps[] = { "Collecting information", "Dynamic Update", "Preparing installation", ("Installing " OS_NAME), "Finalizing installation" };
    int y = 100;
    for (int i = 0; i < 5; i++) {
        uint32_t col = i == step ? WHITE : i < step ? HEX(0xC8D8FF) : HEX(0x8AA4E8);
        fill_circle(c, 30, y + 7, 5, i <= step ? (i == step ? HEX(0xFFB54A) : HEX(0x7CE07C)) : HEX(0x5C78D0));
        draw_text(c, i == step ? FONT_UI_BOLD : FONT_UI, 44, y, steps[i], col);
        y += 30;
    }
    y += 30;
    draw_text(c, FONT_UI, 22, y, "Setup will complete in", HEX(0xE0E8FF)); y += 16;
    draw_text(c, FONT_UI, 22, y, "approximately:", HEX(0xE0E8FF)); y += 18;
    draw_text(c, FONT_UI_BOLD, 22, y, eta, WHITE); y += 30;
    draw_progress(c, R(22, y, 206, 16), pct);
}
static rect_t wiz_rect(void) { return R(300, 108, 640, 480); }

static void page_header(canvas_t *s, const char *title, const char *sub) {
    fill_rect(s, 0, 0, s->w, 62, WHITE);
    draw_text(s, FONT_UI_BOLD, 20, 14, title, BLACK);
    draw_text(s, FONT_UI, 34, 34, sub, HEX(0x303030));
    draw_globe(s, s->w - 64 - 6, -1, 64, -36);
    hline(s, 0, 62, s->w, HEX(0xA0A0A0)); hline(s, 0, 63, s->w, WHITE);
}
static void oobe_paint(window_t *w) {
    canvas_t *s = w->surf;
    gfx_clear(s, T.face);
    int bx = 20, by = 80, bw = s->w - 40;
    bool next_ok = true;
    switch (O.page) {
    case 0:
        page_header(s, "Welcome to " OS_NAME " Setup", "Setup has copied the system files to your hard disk.");
        by += draw_text_wrapped(s, FONT_UI, bx, by, bw, "Setup will now finish installing " OS_NAME ". Answer a few questions about yourself and your computer, and Setup will do the rest.", BLACK, 4) + 14;
        by += draw_text_wrapped(s, FONT_UI, bx, by, bw, "Your files and settings are stored on the hard disk from now on, so they are kept when you restart or turn off the computer.", BLACK, 4) + 14;
        draw_text_wrapped(s, FONT_UI, bx, by, bw, "To continue, click Next.", BLACK, 4);
        break;
    case 1: {
        page_header(s, "Regional and Language Options", "Select the time zone for your location.");
        draw_text(s, FONT_UI, bx, by, "Time zone:", BLACK);
        const char *names[ARRAY_LEN(zones)]; for (unsigned i = 0; i < ARRAY_LEN(zones); i++) names[i] = zones[i].name;
        ui_list(w, R(bx, by + 20, bw, 250), names, ARRAY_LEN(zones), &O.tz, &O.tzscroll);
        tz_offset_minutes = zones[O.tz].off;
        rtc_time_t t; rtc_now(&t);
        char b[96]; snprintf(b, sizeof b, "Current date and time: %04d-%02d-%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
        draw_text(s, FONT_UI, bx, by + 284, b, HEX(0x303030));
        break;
    }
    case 2: {
        page_header(s, "Personalize Your Software", "Setup uses the information you provide about yourself to personalize " OS_NAME ".");
        draw_text_wrapped(s, FONT_UI, bx, by, bw, "Type your full name and, if you want, the name of your company or organization.", BLACK, 4);
        draw_text(s, FONT_UI, bx + 20, by + 50, "Name:", BLACK);
        if (ui_textbox(w, R(bx + 120, by + 46, 320, 22), O.name, sizeof O.name)) O.finish = true;
        draw_text(s, FONT_UI, bx + 20, by + 86, "Organization:", BLACK);
        if (ui_textbox(w, R(bx + 120, by + 82, 320, 22), O.org, sizeof O.org)) O.finish = true;
        draw_text(s, FONT_UI, bx + 20, by + 122, "Password:", BLACK);
        if (ui_password(w, R(bx + 120, by + 118, 320, 22), O.pass, sizeof O.pass)) O.finish = true;
        draw_text(s, FONT_UI, bx + 20, by + 158, "Confirm:", BLACK);
        if (ui_password(w, R(bx + 120, by + 154, 320, 22), O.pass2, sizeof O.pass2)) O.finish = true;
        draw_text(s, FONT_SMALL, bx + 120, by + 182, "Leave both boxes empty to sign in without a password.", HEX(0x505050));
        if (O.pass[0] && strcmp(O.pass, O.pass2)) draw_text(s, FONT_UI, bx + 120, by + 200, "The passwords do not match.", HEX(0xC02020));
        if (O.pass[0] && strcmp(O.pass, O.pass2)) next_ok = false;
        if (!O.name[0]) next_ok = false;
        break;
    }
    case 3: {
        page_header(s, "Computer Name", "Setup has suggested a name for your computer.");
        draw_text_wrapped(s, FONT_UI, bx, by, bw, "The computer name identifies this computer, for example in the Command Prompt (hostname) and in System Properties. Use up to 15 letters, numbers and hyphens.", BLACK, 4);
        draw_text(s, FONT_UI, bx + 20, by + 66, "Computer name:", BLACK);
        if (ui_textbox(w, R(bx + 140, by + 62, 260, 22), O.comp, 16)) O.finish = true;
        for (char *p = O.comp; *p; p++) { *p = toupper(*p); if (!isalnum(*p) && *p != '-') *p = '-'; }
        if (!O.comp[0]) next_ok = false;
        break;
    }
    case 4: {
        page_header(s, "Choose a Look", "Pick a visual style and an account picture. You can change both later in Control Panel.");
        draw_text(s, FONT_UI_BOLD, bx, by - 6, "Visual style", BLACK);
        for (int i = 0; i < THEME_BUILTIN && i < THEME_COUNT; i++) {
            int x = bx + 4 + (i % 3) * 140, y = by + 10 + (i / 3) * 100;
            cp_theme_thumb(s, x, y, i, theme_id == i ? theme_hue : 0, theme_id == i);
            draw_text_centered(s, FONT_UI, R(x - 4, y + 83, 128, 16), style_name(i), BLACK);
            if (ui_click_area(w, R(x - 4, y - 4, 128, 104))) {
                theme_apply(i, 0);
                const char *wp = style_wallpaper(i);
                for (int j = 0; wp[0] && j < wallpaper_count; j++) if (!strcasecmp(wallpaper_names[j], wp)) { wallpaper_id = j; shell_wallpaper_changed(); }
            }
        }
        draw_text(s, FONT_UI_BOLD, bx + 432, by - 6, "Picture", BLACK);
        for (int i = 0; i < 4; i++) {
            int x = bx + 436 + (i % 2) * 64, y = by + 14 + (i / 2) * 64;
            if (O.pic == i) fill_round_rect(s, x - 4, y - 4, 60, 60, 5, T.select);
            draw_user_picture(s, i, 48, x + 2, y + 2);
            if (ui_click_area(w, R(x - 4, y - 4, 60, 60))) O.pic = i;
        }
        draw_text(s, FONT_UI, bx, by + 236, "Hue:", BLACK);
        int hue = theme_hue;
        if (ui_slider(w, R(bx + 40, by + 230, 300, 24), &hue, 0, 359)) theme_apply(theme_id, hue);
        if (ui_button(w, R(bx + 356, by + 230, 80, 24), "Reset")) theme_apply(theme_id, 0);
        break;
    }
    }
    int fy = s->h - 46;
    hline(s, 0, fy, s->w, HEX(0xA0A0A0)); hline(s, 0, fy + 1, s->w, WHITE);
    if (O.page > 0 && ui_button(w, R(s->w - 190, fy + 12, 80, 24), "< Back")) { O.page--; w->ui.focus = 0; }
    if (ui_button_ex(w, R(s->w - 100, fy + 12, 80, 24), O.page == 4 ? "Finish" : "Next >", next_ok, true) || (O.finish && next_ok)) {
        O.finish = false;
        if (O.page == 2 && !O.comp[0]) {
            int k = 0; for (const char *p = O.name; *p && k < 10; p++) if (isalnum(*p)) O.comp[k++] = toupper(*p);
            O.comp[k] = 0; strlcat(O.comp, "-PC", sizeof O.comp);
        }
        O.page++;
        w->ui.focus = (O.page == 2 || O.page == 3) ? 1 : 0;
    }
    O.finish = false;
}
static void oobe_compose(int step, int pct, const char *eta, bool show_wizard, int frame) {
    memcpy(screen.px, obg->px, SCREEN_W * SCREEN_H * 4);
    canvas_reset_clip(&screen);
    draw_globe(&screen, 18, 6, 64, -36 + frame);
    oobe_left(&screen, step, pct, eta);
    if (show_wizard) {
        rect_t r = wiz_rect();
        draw_window_frame(&screen, r, OS_NAME " Setup", ICON_LOGO, true, WF_NO_MAX | WF_NO_MIN | WF_NOCLOSE, -1, -1, WS_NORMAL);
        rect_t cr = wm_client_rect(&ow);
        blit(&screen, cr.x, cr.y, ow.surf, 0, 0, cr.w, cr.h);
    }
}
static void oobe_sync_window(void) {
    rect_t r = wiz_rect();
    ow.x = r.x; ow.y = r.y; ow.w = r.w; ow.h = r.h;
    ow.flags = WF_NO_MAX | WF_NO_MIN | WF_DIALOG;
    rect_t cr = wm_client_rect(&ow);
    if (!ow.surf || ow.surf->w != cr.w || ow.surf->h != cr.h) { if (ow.surf) canvas_free(ow.surf); ow.surf = canvas_new(cr.w, cr.h); }
}
static void oobe_run_paint(void) {
    oobe_sync_window();
    canvas_reset_clip(ow.surf);
    ui_begin(&ow); oobe_paint(&ow); ui_end(&ow);
    ow.ui.pressed = ow.ui.released = ow.ui.rpressed = ow.ui.dblclick = false; ow.ui.wheel = 0;
}
/* Installing devices: match every device in this computer to its built-in
 * driver, show each one as it is done, and remember the result so that only
 * hardware added later is announced as new. Part of the "Installing" stage,
 * before the pages that ask about the account and the clock. */
static void oobe_install_devices(void) {
    canvas_t *c = &screen;
    devmgr_scan();
    int nd = devmgr_count(), ready = 0, nodrv = 0;
    for (int i = 0; i <= nd; i++) {
        device_t *d = devmgr_get(i < nd ? i : nd - 1);
        if (i < nd) { if (d->status == DS_NO_DRIVER || d->status == DS_FAILED) nodrv++; else ready++; }
        for (int f = 0; f < (i < nd ? 6 : 60); f++) {
            oobe_compose(3, 60 + i * 10 / MAX(1, nd), "3 minutes", false, (i * 6 + f) * 2);
            int x = 300, y = 140;
            draw_text(c, FONT_XL, x, y, "Installing devices", WHITE); y += 50;
            y += draw_text_wrapped(c, FONT_LARGE, x, y, 600, "Setup is detecting the hardware in this computer and matching each device to its driver. "
                                   "Every driver is already part of " OS_NAME ", so nothing needs to be downloaded.", HEX(0xE8F0FF), 6) + 26;
            draw_progress(c, R(x, y, 420, 16), i * 100 / MAX(1, nd)); y += 32;
            if (d) {
                char line[160];
                if (i < nd) {
                    draw_text_ellipsis(c, FONT_LARGE, x, y, 620, d->name, WHITE); y += 26;
                    if (d->drv) snprintf(line, sizeof line, "Driver: %s (%s)", d->drv->name, d->drv->desc);
                    else snprintf(line, sizeof line, "%s", d->status == DS_BUILTIN ? "Built in: no driver needed" : "No driver included for this device");
                    draw_text_ellipsis(c, FONT_UI, x, y, 620, line, d->status == DS_NO_DRIVER || d->status == DS_FAILED ? HEX(0xFFD08A) : HEX(0xC8D8FF));
                } else {
                    snprintf(line, sizeof line, "%d devices are ready to use.", ready);
                    draw_text(c, FONT_LARGE, x, y, line, WHITE); y += 26;
                    if (nodrv) {
                        snprintf(line, sizeof line, "%d %s no driver in %s. Device Manager in Control Panel lists %s.", nodrv, nodrv == 1 ? "device has" : "devices have", OS_NAME, nodrv == 1 ? "it" : "them");
                        draw_text_wrapped(c, FONT_UI, x, y, 620, line, HEX(0xFFD08A), 4);
                    }
                }
            }
            draw_text(c, FONT_UI, 22, SCREEN_H - 120, "Installing devices...", WHITE);
            gfx_present(CURSOR_BUSY, wm_mouse_x, wm_mouse_y);
            task_sleep(25);
        }
    }
    devmgr_save_profile();
    klog("setup: %d devices installed, %d without a driver", ready, nodrv);
}

void setup_oobe(void) {
    memset(&O, 0, sizeof O);
    memset(&ow, 0, sizeof ow);
    O.tz = 6;
    for (unsigned i = 0; i < ARRAY_LEN(zones); i++) if (zones[i].off == tz_offset_minutes) O.tz = i;
    obg_render();
    oobe_install_devices();
    mouse_state_t ms = { 0 }; key_event_t ke;
    while (keyboard_poll(&ke)) {}                     /* nothing typed during that step is meant for the pages */
    while (mouse_poll(&ms)) { wm_mouse_x = ms.x; wm_mouse_y = ms.y; }
    int prev = ms.buttons, frame = 0;
    uint32_t next_frame = 0;
    oobe_run_paint();
    while (O.page < 5) {
        bool repaint = false;
        while (keyboard_poll(&ke)) {
            if (!ke.pressed) continue;
            ow.ui.key = ke; ow.ui.has_key = true;
            oobe_run_paint();
            ow.ui.has_key = false;
        }
        while (mouse_poll(&ms)) {
            wm_mouse_x = ms.x; wm_mouse_y = ms.y;
            rect_t cr = wm_client_rect(&ow);
            ow.ui.mx = ms.x - cr.x; ow.ui.my = ms.y - cr.y;
            ow.ui.in_window = pt_in(R(0, 0, cr.w, cr.h), ow.ui.mx, ow.ui.my);
            int changed = ms.buttons ^ prev;
            if ((changed & 1) && (ms.buttons & 1)) { ow.ui.down = true; ow.ui.pressed = true; repaint = true; }
            if ((changed & 1) && !(ms.buttons & 1)) { ow.ui.down = false; ow.ui.released = true; repaint = true; }
            if (ms.wheel) { ow.ui.wheel = ms.wheel; repaint = true; }
            prev = ms.buttons;
            if (repaint) { oobe_run_paint(); repaint = false; }
        }
        if ((int32_t)(timer_ticks() - next_frame) >= 0) {
            next_frame = timer_ticks() + 50; frame++;
            oobe_run_paint();
            const char *eta = O.page < 3 ? "3 minutes" : "2 minutes";
            oobe_compose(3, 70 + O.page * 4, eta, true, frame);
            int cur = ow.ui.cursor ? ow.ui.cursor : CURSOR_ARROW;
            gfx_present(cur, wm_mouse_x, wm_mouse_y);
        }
        task_yield(); hlt();
    }
    /* finalize */
    strlcpy(user_name, O.name, sizeof user_name);
    strlcpy(computer_name, O.comp, sizeof computer_name);
    password_set(O.pass);
    user_picture = O.pic;
    settings_set("organization", O.org);
    static const char *tasks[] = { "Installing Start menu items...", "Registering components...", "Saving settings...", "Removing any temporary files used..." };
    canvas_t *c = &screen;
    for (int f = 0; f <= 160; f++) {
        int t = MIN(3, f / 40);
        if (f == 120) { settings_set("setup", "done"); settings_save(); persist_flush(); }
        oobe_compose(4, 88 + f * 12 / 160, f < 120 ? "1 minute" : "less than a minute", false, f * 2);
        int x = 300, y = 140;
        draw_text(c, FONT_XL, x, y, "Welcome to the WRLD.", WHITE); y += 50;
        static const char *pitch[] = {
            OS_NAME " brings the look of a classic desktop to a brand new, from-scratch system.",
            "Run " OS_EXE_DESC " Win32 .exe programs, shell scripts and open Mac disk images, side by side.",
            "Make it yours: choose Luna Blue, Olive Green, Silver or Classic, then turn the hue dial to any colour you like.",
            "Everything you change is now saved to your hard disk." };
        for (unsigned i = 0; i < ARRAY_LEN(pitch); i++) y += draw_text_wrapped(c, FONT_LARGE, x, y, 600, pitch[i], HEX(0xE8F0FF), 6) + 18;
        draw_text(c, FONT_UI, 22, SCREEN_H - 120, tasks[t], WHITE);
        gfx_present(CURSOR_BUSY, wm_mouse_x, wm_mouse_y);
        task_sleep(25);
    }
    if (ow.surf) { canvas_free(ow.surf); ow.surf = NULL; }
    if (obg) { canvas_free(obg); obg = NULL; }
    klog("setup: first-boot configuration complete for %s (%s)", user_name, computer_name);
}
