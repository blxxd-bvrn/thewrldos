/* Removable media: CD/DVD/Blu-ray discs and USB flash drives become drive letters by themselves,
 * as in Windows. A task looks at every removable drive every second and a half: a disc that went
 * in, or a stick that was plugged in, is mounted (rust/src/kernel/mediak.rs reads its file
 * system: ISO 9660/Joliet, UDF, FAT, exFAT) and AutoPlay asks what to do with it; a disc that came
 * out, or a stick that was pulled, takes its letter with it. Files are read from the medium the
 * first time they are opened. */
#include "kernel.h"
#include "disk.h"
#include "fs.h"
#include "wm.h"
#include "apps.h"
#include "theme.h"
#include "gfx.h"

int media_mount(blkdev_t *d, int kind, char *out, int cap);
int media_unmount(blkdev_t *d, char *letters, int cap);
bool media_mounted(blkdev_t *d);
blkdev_t *media_disk_of(char letter);
int media_drop_dead(char *letters, int cap);

/* what was last seen of each drive: a disc in it, or a failed mount (not retried until it changes) */
static struct { blkdev_t *d; int present; bool failed, ejected; } seen[BLK_MAX];
static int nseen;
static int *slot(blkdev_t *d) {
    for (int i = 0; i < nseen; i++) if (seen[i].d == d) return &seen[i].present;
    if (nseen == BLK_MAX) return NULL;
    seen[nseen].d = d; seen[nseen].present = -1; seen[nseen].failed = seen[nseen].ejected = false;
    return &seen[nseen++].present;
}
static bool *failed_of(blkdev_t *d) { for (int i = 0; i < nseen; i++) if (seen[i].d == d) return &seen[i].failed; return NULL; }
/* ejected: let go of by the user; not mounted again until the medium goes and comes back */
static bool *ejected_of(blkdev_t *d) { for (int i = 0; i < nseen; i++) if (seen[i].d == d) return &seen[i].ejected; return NULL; }

/* ---------------- AutoPlay ---------------- */
typedef struct { char letter; char label[40], fs[8]; int music, video, pics; char media[256], pic[256]; bool optical; } ap_t;
static void ap_paint(window_t *w) {
    ap_t *a = w->data; canvas_t *s = w->surf;
    gfx_clear(s, T.window_bg);
    gradient_v(s, 0, 0, s->w, 64, HEX(0xF4F8FD), HEX(0xDCE7F5));
    draw_icon(s, a->optical ? ICON_DRIVE_CD : ICON_DRIVE_USB, 48, 14, 8);
    char t[96]; snprintf(t, sizeof t, "%s (%c:)", a->label, a->letter);
    draw_text_ellipsis(s, FONT_LARGE, 74, 12, s->w - 84, t, HEX(0x1E395B));
    draw_text(s, FONT_UI, 74, 38, a->optical ? "What do you want " OS_NAME " to do with this disc?" : "What do you want " OS_NAME " to do with this device?", T.text);
    int y = 80;
    struct { bool show; int icon; const char *label, *sub; int act; } opts[] = {
        { a->music + a->video > 0, ICON_MEDIA_PLAYER, a->video ? "Play video" : "Play audio", "using THE WRLD Media Player", 1 },
        { a->pics > 0, ICON_IMAGE_FILE, "View pictures", "using the picture viewer", 2 },
        { true, ICON_EXPLORER, "Open folder to view files", "using File Explorer", 3 },
        { true, ICON_STOP, "Take no action", NULL, 4 },
    };
    for (unsigned i = 0; i < ARRAY_LEN(opts); i++) {
        if (!opts[i].show) continue;
        rect_t r = R(14, y, s->w - 28, 44);
        bool hot = ui_hover(w, r);
        if (hot) { fill_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0xE5F3FF)); draw_round_rect(s, r.x, r.y, r.w, r.h, 3, HEX(0xB8D6FB)); }
        draw_icon(s, opts[i].icon, 32, r.x + 8, r.y + 6);
        draw_text(s, FONT_UI_BOLD, r.x + 50, r.y + (opts[i].sub ? 7 : 14), opts[i].label, HEX(0x1E395B));
        if (opts[i].sub) draw_text(s, FONT_UI, r.x + 50, r.y + 24, opts[i].sub, T.gray_text);
        /* a button too, so Tab and Enter reach it */
        if (ui_click_area(w, r)) {
            char root[4] = { a->letter, ':', '\\', 0 };
            switch (opts[i].act) {
            case 1: mediaplayer_open(a->media); break;
            case 2: imageview_open(a->pic); break;
            case 3: explorer_open(root); break;
            default: break;
            }
            wm_close(w); return;
        }
        y += 50;
    }
}
static void ap_destroy(window_t *w) { kfree(w->data); w->data = NULL; }
static void autoplay(void *arg) {
    ap_t *a = arg;
    if (!vfs_drive_kind(a->letter)) { kfree(a); return; }
    int n = 2 + (a->music + a->video > 0) + (a->pics > 0);
    window_t *w = wm_create("AutoPlay", -1, -1, 400, 86 + n * 50, WF_NO_MAX | WF_NO_MIN);
    w->data = a; w->paint = ap_paint; w->destroy = ap_destroy; w->icon = a->optical ? ICON_DRIVE_CD : ICON_DRIVE_USB;
    wm_center(w);
}
static void balloon(void *arg) { char *m = arg; notify_balloon("Removable media", m); kfree(m); }
static void tell(const char *fmt, ...) {
    char *m = kmalloc(200); if (!m) return;
    va_list ap; va_start(ap, fmt); vsnprintf(m, 200, fmt, ap); va_end(ap);
    wm_defer(balloon, m);
}

/* one mount report line: D|label|fs|music|video|pictures|first media|first picture */
static void report(const char *line, bool optical) {
    ap_t *a = kzalloc(sizeof *a); if (!a) return;
    char f[8][256]; int k = 0, o = 0;
    memset(f, 0, sizeof f);
    for (const char *p = line; *p && *p != '\n' && k < 8; p++) {
        if (*p == '|') { f[k][o] = 0; k++; o = 0; continue; }
        if (o < 255) f[k][o++] = *p;
    }
    a->letter = f[0][0]; strlcpy(a->label, f[1], sizeof a->label); strlcpy(a->fs, f[2], sizeof a->fs);
    a->music = atoi(f[3]); a->video = atoi(f[4]); a->pics = atoi(f[5]);
    strlcpy(a->media, f[6], sizeof a->media); strlcpy(a->pic, f[7], sizeof a->pic);
    a->optical = optical;
    wm_defer(autoplay, a);
}

static bool boot_done;                                  /* the first look happens quietly (no AutoPlay for the boot CD) */
static void check(void) {
    char gone[16];
    if (media_drop_dead(gone, sizeof gone) > 0) tell("The device for %s: was removed.", gone);
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (!d || !d->removable) continue;
        if (system_volume && d->index == system_disk_index) continue;   /* the installed system's own disk */
        int *pr = slot(d); if (!pr) continue;
        int present = d->optical ? (d->media ? d->media(d) : 0) : 1;
        bool mounted = media_mounted(d);
        if (present != *pr) {
            bool *fl = failed_of(d); if (fl) *fl = false;
            if (*pr >= 0 && d->optical) klog("media: %s: %s", d->model, present ? "a disc was inserted" : "the disc was removed");
        }
        if (!present && *ejected_of(d)) *ejected_of(d) = false;
        if (present && !mounted && !*failed_of(d) && !*ejected_of(d)) {
            char out[1024];
            int n = media_mount(d, d->optical ? 3 : 4, out, sizeof out);
            if (n <= 0) {
                *failed_of(d) = true;
                if (boot_done) tell(d->optical ? "The disc in the %s drive cannot be read (its file system is not supported)." : "The %s device has no file system " OS_NAME " can read.", d->model);
            } else if (boot_done || !d->optical) {
                for (char *line = out; *line; ) { char *nl = strchr(line, '\n'); report(line, d->optical); if (!nl) break; line = nl + 1; }
            }
        } else if (!present && mounted) {
            char l[16]; media_unmount(d, l, sizeof l);
            tell("The disc in %s: was removed.", l);
        }
        *pr = present;
    }
    boot_done = true;
}
static void media_task(void *arg) {
    (void)arg;
    for (;;) { check(); task_sleep(1500); }
}
void media_init(void) { task_create("mediad", media_task, NULL, 64 * 1024); }

/* Eject (Explorer's drive menu): a disc comes out; a stick is let go of first */
bool media_eject(char letter) {
    blkdev_t *d = media_disk_of(letter);
    if (!d) return false;
    char l[16]; media_unmount(d, l, sizeof l);
    int *pr = slot(d); if (pr) *pr = 1;
    bool *ej = ejected_of(d); if (ej) *ej = true;
    if (d->optical && d->eject) d->eject(d);
    else if (d->ops && d->ops->flush) d->ops->flush(d);
    return true;
}
