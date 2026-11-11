/* USB core: root-port enumeration, standard requests, and the class drivers
 * this system needs. HID keyboards and mice are translated into the PS/2
 * scancodes and packets the rest of the system already understands (hid.c
 * reads their report formats); mass storage becomes an ordinary SCSI disk in
 * the block layer. */
#ifdef __x86_64__
#include "usb.h"
#include "disk.h"

static usb_dev_t *devices;
static int next_id = 1;
static int next_addr = 2;                       /* USB addresses start at 2 */
int usb_unsupported;

int usb_count(void) { int n = 0; for (usb_dev_t *d = devices; d; d = d->next) n++; return n; }
usb_dev_t *usb_first(void) { return devices; }

static char usb_summary[384];
const char *usb_boot_summary(void) { return usb_summary; }

typedef struct { uint8_t bus, dev, fn; int8_t ok; int16_t ports, conn; } ctrl_stat_t;
static ctrl_stat_t cstats[24];
static int ncstats;
void usb_note_controller(const pci_dev_t *pd, int ok, int ports, int connected) {
    if (ncstats >= (int)ARRAY_LEN(cstats)) return;
    ctrl_stat_t *c = &cstats[ncstats++];
    c->bus = pd->bus; c->dev = pd->dev; c->fn = pd->fn;
    c->ok = (int8_t)ok; c->ports = (int16_t)ports; c->conn = (int16_t)connected;
}
static const ctrl_stat_t *cstat_find(const pci_dev_t *pd) {
    for (int i = 0; i < ncstats; i++)
        if (cstats[i].bus == pd->bus && cstats[i].dev == pd->dev && cstats[i].fn == pd->fn)
            return &cstats[i];
    return NULL;
}

/* A short ring of the last USB failures, for the log-on screen. */
static char diag[4][96];
static int ndiag;
void usb_diag(const char *fmt, ...) {
    char b[96];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (ndiag < 4) { strlcpy(diag[ndiag], b, sizeof diag[ndiag]); ndiag++; }
    else {
        for (int i = 1; i < 4; i++) memcpy(diag[i - 1], diag[i], sizeof diag[i]);
        strlcpy(diag[3], b, sizeof diag[3]);
    }
}
const char *usb_diag_text(void) {
    static char out[384];
    out[0] = 0;
    for (int i = 0; i < ndiag; i++) {
        if (i) strlcat(out, "; ", sizeof out);
        strlcat(out, diag[i], sizeof out);
    }
    return out;
}
int usb_has_input(void) {
    for (usb_dev_t *d = devices; d; d = d->next)
        if (d->kind == USB_KIND_KEYBOARD || d->kind == USB_KIND_MOUSE) return 1;
    return 0;
}
int usb_input_active(void) {
    for (usb_dev_t *d = devices; d; d = d->next)
        if ((d->kind == USB_KIND_KEYBOARD || d->kind == USB_KIND_MOUSE) && d->acted) return 1;
    return 0;
}
const char *usb_device_summary(void) {
    static char out[256];
    int len = 0;
    out[0] = 0;
    for (usb_dev_t *d = devices; d; d = d->next) {
        const char *k = d->kind == USB_KIND_KEYBOARD ? "kbd" : d->kind == USB_KIND_MOUSE ? "mouse"
                      : d->kind == USB_KIND_STORAGE ? "disk" : d->cls == 9 ? "hub" : "dev";
        int n = snprintf(out + len, sizeof out - (size_t)len, "%s%s@%d%s", len ? ", " : "", k,
                         d->addr, (d->kind == USB_KIND_KEYBOARD || d->kind == USB_KIND_MOUSE) && d->acted ? "*" : "");
        if (n > 0 && len + n < (int)sizeof out) len += n;
    }
    return out;
}

/* How far each HID device has got: interrupt polls and the reports they gave,
 * then GET_REPORT answers. A device that never moves past polls has an endpoint
 * the controller cannot reach; one that answers GET_REPORT is still usable. */
const char *usb_live_text(void) {
    static char out[512];
    int len = 0;
    out[0] = 0;
    for (usb_dev_t *d = devices; d; d = d->next) {
        if (d->kind != USB_KIND_KEYBOARD && d->kind != USB_KIND_MOUSE) continue;
        int n = snprintf(out + len, sizeof out - (size_t)len, "%s%s@%d i%u r%u g%u",
                         len ? ", " : "", d->kind == USB_KIND_KEYBOARD ? "kbd" : "mouse", d->addr,
                         (unsigned)d->irq_polls, (unsigned)d->irq_reports, (unsigned)d->ctl_reports);
        if (n > 0 && len + n < (int)sizeof out) len += n;
    }
    return out;
}

int usb_control(usb_dev_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, int len)
{
    return d->hcd->control(d->hcd, d->addr, d->low_speed, type, req, value, index, buf, len);
}
int usb_in(usb_dev_t *d, int ep, uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms)
{
    uint8_t *p = buf;
    int done = 0;
    while (done < len) {
        int r = d->hcd->transfer(d->hcd, d->addr, d->low_speed, ep, true, maxpkt, toggle, p + done, len - done, timeout_ms);
        if (r <= 0) return done ? done : r;
        done += r;
    }
    return done;
}
int usb_out(usb_dev_t *d, int ep, uint16_t maxpkt, uint8_t *toggle, const void *buf, int len, int timeout_ms)
{
    const uint8_t *p = buf;
    int done = 0;
    while (done < len) {
        int r = d->hcd->transfer(d->hcd, d->addr, d->low_speed, ep, false, maxpkt, toggle, (void *)(p + done), len - done, timeout_ms);
        if (r <= 0) return done ? done : r;
        done += r;
    }
    return done;
}

static int get_descriptor(usb_hcd_t *h, int addr, bool low, uint8_t type, uint8_t index, void *buf, int len)
{
    return h->control(h, addr, low, 0x80, 6, (uint16_t)((type << 8) | index), 0, buf, len);
}

static void enumerate_one(usb_hcd_t *h, bool low, const char *where, int root_port, int route,
                          int hub_addr, int hub_port, int speed);
static void hub_attach(usb_dev_t *hub);

/* ---------------- HID ---------------- */
/* HID usage IDs to PS/2 set 1 scancodes; hid_e0 marks the ones that take an E0
 * prefix. A usage with no scancode is a key we do not send. */
static uint8_t hid_scan[0xE8];
static uint8_t hid_e0[0xE8];
static void hid_key(int usage, uint8_t sc, bool e0) { hid_scan[usage] = sc; hid_e0[usage] = (uint8_t)e0; }
static void hid_init_table(void)
{
    static const uint8_t letters[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                         0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    static const uint8_t digits[10] = { 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B };
    static const uint8_t keypad[11] = { 0x4F,0x50,0x51,0x4B,0x4C,0x4D,0x47,0x48,0x49,0x52,0x53 };   /* 1..9, 0, . */
    for (int i = 0; i < 26; i++) hid_key(0x04 + i, letters[i], false);
    for (int i = 0; i < 10; i++) hid_key(0x1E + i, digits[i], false);
    hid_key(0x28, 0x1C, false); hid_key(0x29, 0x01, false); hid_key(0x2A, 0x0E, false); hid_key(0x2B, 0x0F, false);
    hid_key(0x2C, 0x39, false); hid_key(0x2D, 0x0C, false); hid_key(0x2E, 0x0D, false); hid_key(0x2F, 0x1A, false);
    hid_key(0x30, 0x1B, false); hid_key(0x31, 0x2B, false); hid_key(0x32, 0x2B, false); hid_key(0x33, 0x27, false);
    hid_key(0x34, 0x28, false); hid_key(0x35, 0x29, false); hid_key(0x36, 0x33, false); hid_key(0x37, 0x34, false);
    hid_key(0x38, 0x35, false); hid_key(0x39, 0x3A, false);
    for (int i = 0; i < 10; i++) hid_key(0x3A + i, (uint8_t)(0x3B + i), false);          /* F1..F10 */
    hid_key(0x44, 0x57, false); hid_key(0x45, 0x58, false);                              /* F11, F12 */
    hid_key(0x49, 0x52, true); hid_key(0x4A, 0x47, true); hid_key(0x4B, 0x49, true);     /* Insert, Home, Page Up */
    hid_key(0x4C, 0x53, true); hid_key(0x4D, 0x4F, true); hid_key(0x4E, 0x51, true);     /* Delete, End, Page Down */
    hid_key(0x4F, 0x4D, true); hid_key(0x50, 0x4B, true);                                /* right, left */
    hid_key(0x51, 0x50, true); hid_key(0x52, 0x48, true);                                /* down, up */
    hid_key(0x54, 0x35, true); hid_key(0x55, 0x37, false); hid_key(0x56, 0x4A, false);   /* keypad / * - */
    hid_key(0x57, 0x4E, false); hid_key(0x58, 0x1C, true);                               /* keypad + Enter */
    for (int i = 0; i < 11; i++) hid_key(0x59 + i, keypad[i], false);
    hid_key(0x64, 0x56, false); hid_key(0x65, 0x5D, true);                               /* non-US \, Menu */
    hid_key(0xE0, 0x1D, false); hid_key(0xE1, 0x2A, false); hid_key(0xE2, 0x38, false);  /* left Ctrl, Shift, Alt */
    hid_key(0xE3, 0x5B, true);  hid_key(0xE4, 0x1D, true);  hid_key(0xE5, 0x36, false);  /* left Win, right Ctrl, Shift */
    hid_key(0xE6, 0x38, true);  hid_key(0xE7, 0x5C, true);                               /* right Alt, Win */
}

static void hid_send_usage(uint8_t usage, bool release)
{
    extern void keyboard_scancode(uint8_t code);
    uint8_t sc = usage < 0xE8 ? hid_scan[usage] : 0;
    if (!sc) return;
    if (hid_e0[usage]) keyboard_scancode(0xE0);
    keyboard_scancode((uint8_t)(sc | (release ? 0x80 : 0)));
}

/* A PS/2 keyboard repeats a held key by itself; a USB keyboard only says which
 * keys are down, so the repeat is ours. Modifiers and the lock keys do not. */
#define HID_REPEAT_DELAY 500
#define HID_REPEAT_RATE  33
static bool hid_repeats(uint8_t u)
{
    return u >= 0x04 && u < 0xE0 && u != 0x39 && u != 0x47 && u != 0x53 && hid_scan[u];
}

/* `rep` is in the boot keyboard layout: modifier bits, a reserved byte, and up
 * to six usages that are held down. */
static void hid_keyboard_report(usb_dev_t *d, const uint8_t *rep, int n)
{
    uint8_t r[8];
    memset(r, 0, sizeof r);
    memcpy(r, rep, (size_t)MIN(n, 8));
    if (n < 3 || r[2] == 0x01) return;                  /* too many keys at once: nothing changed */
    for (int i = 0; i < 8; i++) if (r[i]) d->acted = true;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        bool was = (d->report[0] & bit) != 0, now = (r[0] & bit) != 0;
        if (now != was) hid_send_usage((uint8_t)(0xE0 + i), !now);
    }
    for (int j = 2; j < 8; j++) {                       /* keys that came up */
        uint8_t u = d->report[j];
        if (!u) continue;
        bool still = false;
        for (int i = 2; i < 8; i++) if (r[i] == u) still = true;
        if (still) continue;
        hid_send_usage(u, true);
        if (u == d->rep_usage) d->rep_usage = 0;
    }
    for (int i = 2; i < 8; i++) {                       /* newly pressed keys */
        uint8_t u = r[i];
        if (!u) continue;
        bool seen = false;
        for (int j = 2; j < 8; j++) if (d->report[j] == u) seen = true;
        if (seen) continue;
        hid_send_usage(u, false);
        if (hid_repeats(u)) { d->rep_usage = u; d->rep_next = timer_ticks() + HID_REPEAT_DELAY; }
    }
    memcpy(d->report, r, 8);
}

/* One input report, as the device sent it. `stale` marks an answer that may be
 * a repeat of the last one: a mouse's buttons are still right, but its movement
 * has already been counted. */
static void hid_report(usb_dev_t *d, const uint8_t *rep, int n, bool stale)
{
    extern void mouse_inject(int dx, int dy, int buttons, int wheel);
    if (!d->seen) {
        d->seen = true;
        klog("usb: %s at address %d is sending reports", d->kind == USB_KIND_KEYBOARD ? "keyboard" : "mouse", d->addr);
    }
    int keep = MIN(n, (int)sizeof d->last_rep);
    memcpy(d->last_rep, rep, (size_t)keep);
    d->last_len = keep;
    int dx = 0, dy = 0, btn = 0, wheel = 0;
    bool mouse = false;
    if (d->boot) {
        if (d->kind == USB_KIND_KEYBOARD) {
            /* a keyboard that kept its report ID despite the boot protocol */
            if (d->fmt.ids && n == 9) { rep++; n--; }
            hid_keyboard_report(d, rep, n);
            return;
        }
        if (n < 3) return;
        btn = rep[0] & 7; dx = (int8_t)rep[1]; dy = (int8_t)rep[2];
        if (n >= 4) { int8_t w = (int8_t)rep[3]; if (w >= -8 && w <= 8) wheel = w; }
        mouse = true;
    } else {
        int id = 0;
        if (d->fmt.ids) {                                /* the report ID says which report this is */
            if (n < 2) return;
            id = rep[0]; rep++; n--;
        }
        if (d->fmt.mouse.present && id == d->fmt.mouse.id) {
            mouse = hid_decode_mouse(&d->fmt, rep, n, &dx, &dy, &btn, &wheel);
        } else if (d->fmt.keys.present && id == d->fmt.keys.id) {
            uint8_t keys[8];
            if (hid_decode_keys(&d->fmt, rep, n, keys)) hid_keyboard_report(d, keys, 8);
        }
    }
    if (!mouse) return;
    if (stale) dx = dy = wheel = 0;
    if (dx || dy || (btn & 7) || wheel) d->acted = true;
    /* HID counts Y downwards and the wheel away from the user; the mouse code
     * takes PS/2 packets, which count both the other way. */
    mouse_inject(dx, -dy, btn & 7, -CLAMP(wheel, -8, 8));
}

/* ---------------- mass storage: bulk-only transport ---------------- */
typedef struct PACKED {
    uint32_t sig, tag, dlen;
    uint8_t flags, lun, cblen;
    uint8_t cdb[16];
} cbw_t;
typedef struct PACKED { uint32_t sig, tag, residue; uint8_t status; } csw_t;

typedef struct {
    usb_dev_t *dev;
    uint8_t lun;
    uint32_t tag, blocks, bsize;
} stor_t;

static int stor_cmd(usb_dev_t *d, uint8_t lun, const uint8_t *cdb, int cdblen,
                    void *buf, uint32_t len, bool in)
{
    if (!d->ep_in || !d->ep_out) return -1;
    d->toggle_in = d->toggle_out = 0;                    /* the BOT resets both at each command */
    cbw_t cbw;
    memset(&cbw, 0, sizeof cbw);
    static uint32_t bot_tag;
    uint32_t tag = ++bot_tag;
    cbw.sig = 0x43425355;
    cbw.tag = tag;
    cbw.dlen = len;
    cbw.flags = in ? 0x80 : 0x00;
    cbw.lun = lun;
    cbw.cblen = (uint8_t)cdblen;
    memcpy(cbw.cdb, cdb, cdblen);
    if (usb_out(d, d->ep_out, d->maxpkt_out, &d->toggle_out, &cbw, sizeof cbw, 2000) < 0) return -1;

    uint8_t *p = buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done; if (chunk > 4096) chunk = 4096;
        int r = in ? usb_in(d, d->ep_in, d->maxpkt_in, &d->toggle_in, p + done, (int)chunk, 5000)
                   : usb_out(d, d->ep_out, d->maxpkt_out, &d->toggle_out, p + done, (int)chunk, 5000);
        if (r < 0) return -1;
        done += chunk;
    }

    csw_t csw;
    int r = usb_in(d, d->ep_in, d->maxpkt_in, &d->toggle_in, &csw, sizeof csw, 5000);
    if (r < (int)sizeof csw) return -1;
    if (csw.sig != 0x53425355 || csw.tag != tag || csw.status) {
        klog("usb-storage: command %02x failed (status %d)", cdb[0], csw.status);
        return -1;
    }
    return 0;
}

static bool stor_read(blkdev_t *b, uint64_t blk, uint32_t n, void *buf)
{
    stor_t *s = b->priv;
    usb_dev_t *d = s->dev;
    uint64_t lba = blk * 512 / s->bsize;
    uint32_t cnt = (uint32_t)((uint64_t)n * 512 / s->bsize);
    if (lba + cnt > s->blocks) return false;
    uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                        0, (uint8_t)(cnt >> 8), (uint8_t)cnt, 0 };
    return stor_cmd(d, s->lun, cdb, 10, buf, (uint32_t)(cnt * s->bsize), true) == 0;
}
static bool stor_write(blkdev_t *b, uint64_t blk, uint32_t n, const void *buf)
{
    stor_t *s = b->priv;
    usb_dev_t *d = s->dev;
    uint64_t lba = blk * 512 / s->bsize;
    uint32_t cnt = (uint32_t)((uint64_t)n * 512 / s->bsize);
    if (lba + cnt > s->blocks) return false;
    uint8_t cdb[10] = { 0x2A, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                        0, (uint8_t)(cnt >> 8), (uint8_t)cnt, 0 };
    return stor_cmd(d, s->lun, cdb, 10, (void *)buf, (uint32_t)(cnt * s->bsize), false) == 0;
}
static bool stor_flush(blkdev_t *b)
{
    stor_t *s = b->priv;
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   /* SYNCHRONIZE CACHE(10) */
    return stor_cmd(s->dev, s->lun, cdb, 10, NULL, 0, false) == 0;
}
static const blkops_t stor_ops = { stor_read, stor_write, stor_flush };

static void stor_attach(usb_dev_t *d, int iface)
{
    uint8_t lun = 0;
    usb_control(d, 0xA1, 0xFE, 0, (uint16_t)iface, &lun, 1);          /* GET MAX LUN */
    uint8_t inquiry[36];
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };                        /* INQUIRY */
    if (stor_cmd(d, 0, cdb, 6, inquiry, 36, true) < 0) { klog("usb-storage: INQUIRY failed"); return; }
    uint8_t cap[8];
    uint8_t rc[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };             /* READ CAPACITY(10) */
    if (stor_cmd(d, 0, rc, 10, cap, 8, true) < 0) { klog("usb-storage: READ CAPACITY failed"); return; }
    uint32_t last = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) | ((uint32_t)cap[2] << 8) | cap[3];
    uint32_t bsize = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) | ((uint32_t)cap[6] << 8) | cap[7];
    if (bsize != 512 && bsize != 1024 && bsize != 2048 && bsize != 4096) { klog("usb-storage: %u-byte blocks unsupported", bsize); return; }

    stor_t *s = kzalloc(sizeof *s);
    blkdev_t *b = blk_register();
    if (!s || !b) { kfree(s); return; }
    s->dev = d; s->lun = lun; s->blocks = last + 1; s->bsize = bsize;
    b->ops = &stor_ops;
    b->priv = s;
    b->bsize = bsize;
    b->max_blocks = 4096 / bsize;
    b->sectors = (uint32_t)MIN((uint64_t)s->blocks * bsize / 512, 0xFFFFFFFFull);
    b->kind = "USB";
    b->driver = "usbstor";
    snprintf(b->model, sizeof b->model, "%.8s%.16s", (const char *)inquiry + 8, (const char *)inquiry + 16);
    d->kind = USB_KIND_STORAGE;
    blk_commit(b);
    klog("usb-storage: %s, %u blocks of %u bytes", b->model, s->blocks, bsize);
}

/* ---------------- enumeration ---------------- */
/* The class, and the bulk and interrupt endpoints, of the interface a storage
 * or hub driver will use: the first one of the device's class. */
static void parse_config(usb_dev_t *d, const uint8_t *cfg, int len, int *iface_out)
{
    int i = 0;
    bool mine = false, have = false;
    while (i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len) {
        const uint8_t *p = cfg + i;
        if (p[1] == 4 && p[0] >= 9) {
            if (!d->cls) { d->cls = p[5]; d->subcls = p[6]; d->proto = p[7]; }
            mine = !have && p[5] == d->cls && p[3] == 0;
            if (mine) { *iface_out = p[2]; have = true; }
        } else if (p[1] == 5 && p[0] >= 7 && mine) {
            int ep = p[2] & 0x0F, attr = p[3] & 3, maxpkt = (p[4] | (p[5] << 8)) & 0x7FF;
            if (attr == 3 && (p[2] & 0x80) && !d->ep_int) { d->ep_int = ep; d->maxpkt_int = (uint16_t)maxpkt; }
            else if (attr == 2) {
                if (p[2] & 0x80) { d->ep_in = ep; d->maxpkt_in = (uint16_t)maxpkt; }
                else { d->ep_out = ep; d->maxpkt_out = (uint16_t)maxpkt; }
            }
        }
        i += p[0];
    }
}

/* One HID interface becomes one keyboard or mouse. A device can have several
 * (a keyboard with a media-key interface, a wireless receiver carrying both a
 * keyboard and a mouse), each with its own endpoint and report format. */
static int hid_attach(const usb_dev_t *tmpl, const hid_iface_t *hi, const char *where)
{
    usb_dev_t *d = kzalloc(sizeof *d);
    if (!d) return 0;
    *d = *tmpl;
    d->id = next_id++;
    d->cls = 3; d->subcls = hi->subcls; d->proto = hi->proto;
    d->iface = hi->num; d->ep_int = hi->ep; d->maxpkt_int = hi->maxpkt ? hi->maxpkt : 8;

    int rlen = hi->rlen ? hi->rlen : 256;
    if (rlen > 1024) rlen = 1024;
    uint8_t *rd = kzalloc((size_t)rlen);
    int n = rd ? usb_control(d, 0x81, 6, 0x2200, hi->num, rd, rlen) : 0;
    if (n > 0) hid_parse_format(&d->fmt, rd, n);
    kfree(rd);

    bool boot_kbd = hi->subcls == 1 && hi->proto == 1, boot_mouse = hi->subcls == 1 && hi->proto == 2;
    const char *what;
    if (boot_kbd || (!boot_mouse && d->fmt.keys.present)) {
        d->kind = USB_KIND_KEYBOARD; what = "keyboard";
        /* The boot protocol fixes the report at eight bytes with no report ID. */
        if (boot_kbd) d->boot = usb_control(d, 0x21, 0x0B, 0, hi->num, NULL, 0) >= 0 || !d->fmt.keys.present;
    } else if (boot_mouse || d->fmt.mouse.present) {
        d->kind = USB_KIND_MOUSE; what = "mouse";
        /* A mouse is read the way its own descriptor lays the report out, which
         * is the format every system uses with it; the boot protocol is only for
         * a mouse whose descriptor could not be read. */
        d->boot = !d->fmt.mouse.present;
        if (boot_mouse) usb_control(d, 0x21, 0x0B, d->boot ? 0 : 1, hi->num, NULL, 0);
    } else {
        kfree(d);
        return 0;
    }
    usb_control(d, 0x21, 0x0A, 0, hi->num, NULL, 0);                 /* SET_IDLE(0): report on change only */

    d->next = devices; devices = d;
    klog("usb: %s %04x:%04x at address %d (%s%s, interface %d, %s protocol, %d-byte packets)",
         what, d->vendor, d->product, d->addr, where, d->low_speed ? ", low speed" : "",
         hi->num, d->boot ? "boot" : "report", d->maxpkt_int);
    return 1;
}

static void device_attach(usb_hcd_t *h, int addr, bool low, const uint8_t *dev, const char *where,
                          int root_port, int route)
{
    usb_dev_t *d = kzalloc(sizeof *d);
    if (!d) return;
    d->hcd = h; d->addr = addr; d->low_speed = low;
    d->root_port = root_port; d->route = route;
    d->vendor = (uint16_t)(dev[8] | (dev[9] << 8));
    d->product = (uint16_t)(dev[10] | (dev[11] << 8));
    d->bcdusb = (uint16_t)(dev[2] | (dev[3] << 8));
    d->cls = dev[4]; d->subcls = dev[5]; d->proto = dev[6];
    d->id = next_id++;

    uint8_t cfg9[9];
    if (get_descriptor(h, addr, low, 2, 0, cfg9, 9) < 9) { klog("usb: address %d: no configuration", addr); kfree(d); return; }
    int total = cfg9[2] | (cfg9[3] << 8);
    if (total < 9) total = 9;
    if (total > 1024) total = 1024;
    uint8_t *cfg = kzalloc((size_t)total);
    if (!cfg) { kfree(d); return; }
    if (get_descriptor(h, addr, low, 2, 0, cfg, total) < 9) { klog("usb: address %d: configuration read failed", addr); kfree(cfg); kfree(d); return; }
    int iface = 0;
    parse_config(d, cfg, total, &iface);
    d->iface = iface;
    hid_iface_t hi[4];
    int nhid = (d->cls == 8 || d->cls == 9) ? 0 : hid_find_interfaces(cfg, total, hi, (int)ARRAY_LEN(hi));
    kfree(cfg);

    if (h->control(h, addr, low, 0x00, 9, 1, 0, NULL, 0) < 0) {      /* SET_CONFIGURATION(1) */
        klog("usb: address %d: SET_CONFIGURATION failed", addr);
        kfree(d);
        return;
    }

    if (nhid) {                                                      /* HID: one device per interface */
        int made = 0;
        for (int k = 0; k < nhid; k++) made += hid_attach(d, &hi[k], where);
        if (!made)
            klog("usb: %04x:%04x at address %d (%s): HID, but not a keyboard or mouse", d->vendor, d->product, addr, where);
        kfree(d);
    } else if (d->cls == 8) {                                        /* mass storage */
        snprintf(d->name, sizeof d->name, "usb-storage");
        d->next = devices; devices = d;                              /* visible to the storage driver */
        stor_attach(d, iface);
        klog("usb: mass storage %04x:%04x at address %d (%s)", d->vendor, d->product, addr, where);
    } else if (d->cls == 9) {                                        /* hub: look behind it */
        d->next = devices; devices = d;
        klog("usb: hub %04x:%04x at address %d (%s)", d->vendor, d->product, addr, where);
        hub_attach(d);
    } else {
        klog("usb: %04x:%04x class %d at address %d (%s): no driver", d->vendor, d->product, d->cls, addr, where);
        kfree(d);
    }
}

/* A hub is not a device we use directly: power every port, reset the ones with
 * something connected, and enumerate whatever appears there. */
static void hub_attach(usb_dev_t *hub)
{
    uint8_t hd[16];
    int ss = hub->bcdusb >= 0x0300;
    int dtype = ss ? 0x2B : 0x29;                          /* SuperSpeed or USB 2 hub descriptor */
    int hlen = ss ? 12 : 8;
    int hn = 0;
    for (int try = 0; try < 3 && hn < 3; try++) {
        hn = usb_control(hub, 0xA0, 6, (uint16_t)(dtype << 8), 0, hd, hlen);
        if (hn >= 3) break;
        uint32_t t0 = timer_ticks();
        while ((int32_t)(timer_ticks() - t0) < 20) task_yield();
    }
    if (hn < 3) {
        klog("usb: hub at address %d: no hub descriptor", hub->addr);
        usb_diag("hub %d: no descriptor", hub->addr);
        return;
    }
    int ports = hd[2];
    /* xHCI has to know this is a hub, and how many ports it has, before it can
     * route a transfer to anything behind it. */
    if (hub->hcd->hub_info)
        hub->hcd->hub_info(hub->hcd, hub->addr, ports, ss ? 0 : (hd[3] >> 5) & 3);
    if (ports > 8) ports = 8;
    for (int p = 1; p <= ports; p++) {
        usb_control(hub, 0x23, 3, 8, (uint16_t)p, NULL, 0);           /* SET_FEATURE(PORT_POWER) */
        uint32_t t0 = timer_ticks();
        while ((int32_t)(timer_ticks() - t0) < 50) task_yield();
        uint8_t st[4];
        if (usb_control(hub, 0xA3, 0, 0, (uint16_t)p, st, 4) < 4) continue;
        if (!(st[0] & 1)) continue;                                   /* nothing plugged in */
        usb_control(hub, 0x23, 3, 4, (uint16_t)p, NULL, 0);            /* SET_FEATURE(PORT_RESET) */
        t0 = timer_ticks();
        while ((int32_t)(timer_ticks() - t0) < 40) task_yield();
        usb_control(hub, 0x23, 1, 20, (uint16_t)p, NULL, 0);           /* CLEAR_FEATURE(C_PORT_RESET) */
        t0 = timer_ticks();
        while ((int32_t)(timer_ticks() - t0) < 20) task_yield();
        if (usb_control(hub, 0xA3, 0, 0, (uint16_t)p, st, 4) < 4) continue;
        if (!(st[0] & 1)) continue;
        bool low = (st[1] & 0x02) != 0;                               /* port status bit 9 */
        int speed = ss ? 4 : (st[1] & 0x04) ? 3 : low ? 2 : 1;        /* SuperSpeed, high, low, full */
        char where[24];
        snprintf(where, sizeof where, "hub %d port %d", hub->addr, p);
        enumerate_one(hub->hcd, low, where, hub->root_port, (hub->route << 4) | p,
                      hub->addr, p, speed);
    }
}

/* Read one device's descriptors and give it an address. `where` names the port
 * or hub port in the log. */
static void enumerate_one(usb_hcd_t *h, bool low, const char *where, int root_port, int route,
                          int hub_addr, int hub_port, int speed)
{
    uint32_t t0 = timer_ticks();
    while ((int32_t)(timer_ticks() - t0) < 30) task_yield();
    /* xHCI needs a slot (and endpoint 0) before it can move anything */
    if (h->new_device && !h->new_device(h, root_port, route, low, hub_addr, hub_port, speed)) {
        klog("usb: %s: could not prepare the device", where);
        return;
    }
    uint8_t d8[8];
    if (get_descriptor(h, 0, low, 1, 0, d8, 8) < 8) { klog("usb: %s: no device descriptor", where); usb_diag("%s: no descriptor", where); return; }
    int addr = next_addr;
    if (addr < 2 || addr > 127) { klog("usb: out of device addresses"); return; }
    if (h->control(h, 0, low, 0x00, 5, (uint16_t)addr, 0, NULL, 0) < 0) { klog("usb: %s: SET_ADDRESS failed", where); usb_diag("%s: SET_ADDRESS failed", where); return; }
    next_addr++;
    t0 = timer_ticks();
    while ((int32_t)(timer_ticks() - t0) < 10) task_yield();
    uint8_t dev[18];
    if (get_descriptor(h, addr, low, 1, 0, dev, 18) < 18) { klog("usb: address %d: descriptor read failed", addr); usb_diag("%s: descriptor failed", where); return; }
    device_attach(h, addr, low, dev, where, root_port, route);
}

void usb_attach(usb_hcd_t *h)
{
    for (int port = 0; port < h->ports; port++) {
        bool low = false;
        if (!h->port_reset || !h->port_reset(h, port, &low)) continue;
        char where[24];
        snprintf(where, sizeof where, "%s port %d", h->name, port + 1);
        enumerate_one(h, low, where, port + 1, 0, 0, 0, 0);
    }
}

/* ---------------- polling and start-up ---------------- */
/* A HID device answers its interrupt endpoint only when something changed, so
 * silence is normal and proves nothing. GET_REPORT is the stand-in for a device
 * whose interrupt endpoint never delivers anything: after a second of silence
 * the current report is asked for directly, at about the rate a mouse reports.
 * A mouse answers with its last report until it has a new one, so a report that
 * has not changed is one movement already counted, not another. Once the
 * interrupt endpoint has delivered a report of its own, it is trusted alone. */
#define HID_SILENT_MS     1000
#define HID_KBD_CTL_MS      32
#define HID_MOUSE_CTL_MS    16
#define HID_CTL_RETRY_MS   500                  /* after a failure */
#define HID_CTL_GIVE_UP      8

/* how many bytes the device's input report is: asking for more than it has is
 * enough to make some devices refuse the request */
static int hid_report_len(const usb_dev_t *d)
{
    int n;
    if (d->boot) n = d->kind == USB_KIND_KEYBOARD ? 8 : MIN((int)d->maxpkt_int, 8);
    else {
        n = d->kind == USB_KIND_MOUSE ? d->fmt.mouse.len : d->fmt.keys.len;
        if (d->fmt.ids) n++;
        if (n <= 1) n = MIN((int)d->maxpkt_int, (int)sizeof d->last_rep);
    }
    if (n < 1) n = 8;
    if (n > (int)sizeof d->last_rep) n = (int)sizeof d->last_rep;
    return n;
}

static void hid_control_poll(usb_dev_t *d)
{
    uint8_t rep[64];
    memset(rep, 0, sizeof rep);
    int id = d->boot ? 0 : d->kind == USB_KIND_MOUSE ? d->fmt.mouse.id : d->fmt.keys.id;
    int want = hid_report_len(d);
    d->ctl_polls++;
    int c = usb_control(d, 0xA1, 1, (uint16_t)(0x0100 | id), (uint16_t)d->iface, rep, want);
    if (c <= 0) {
        if (++d->ctl_fail <= 2)
            usb_diag("%s@%d GET_REPORT failed (%d)", d->kind == USB_KIND_KEYBOARD ? "kbd" : "mouse", d->addr, c);
        d->ctl_next = timer_ticks() + HID_CTL_RETRY_MS;
        if (d->ctl_fail >= HID_CTL_GIVE_UP) d->use_control = -1;
        return;
    }
    d->ctl_fail = 0;
    d->use_control = 1;
    d->ctl_reports++;
    /* A mouse keeps answering with the same report until it moves, and an
     * interrupt report may already have delivered that one. */
    bool stale = d->last_len && c == d->last_len && !memcmp(rep, d->last_rep, (size_t)c);
    hid_report(d, rep, c, stale);
}

void usb_poll(void)
{
    for (usb_dev_t *d = devices; d; d = d->next) {
        if (d->kind != USB_KIND_KEYBOARD && d->kind != USB_KIND_MOUSE) continue;
        if (d->rep_usage && (int32_t)(timer_ticks() - d->rep_next) >= 0) {
            hid_send_usage(d->rep_usage, false);
            d->rep_next = timer_ticks() + HID_REPEAT_RATE;
        }
        /* One transfer per poll, one packet long: an interrupt IN delivers a
         * whole report, and a short one must be taken as it is. */
        uint8_t rep[64];
        memset(rep, 0, sizeof rep);
        int len = MIN((int)d->maxpkt_int, (int)sizeof rep);
        d->irq_polls++;
        int n = d->hcd->transfer(d->hcd, d->addr, d->low_speed, d->ep_int, true,
                                 d->maxpkt_int, &d->toggle_int, rep, len, 3);
        if (n > 0) {
            d->irq_reports++;
            d->irq_seen = true;
            hid_report(d, rep, n, false);
            continue;
        }
        if (d->irq_seen || d->use_control < 0) continue;
        uint32_t now = timer_ticks();
        if (!d->ctl_next) d->ctl_next = now + HID_SILENT_MS;
        if ((int32_t)(now - d->ctl_next) < 0) continue;
        d->ctl_next = now + (d->kind == USB_KIND_MOUSE ? HID_MOUSE_CTL_MS : HID_KBD_CTL_MS);
        hid_control_poll(d);
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) { usb_poll(); task_sleep(8); }
}

void usb_init(void)
{
    hid_init_table();
    {   /* say what the machine actually has, so a missing driver is visible */
        pci_dev_t *devs = NULL;
        int n = pci_list(&devs);
        for (int i = 0; i < n; i++) {
            pci_dev_t *pd = &devs[i];
            if (pd->cls != 0x0C || pd->sub != 0x03) continue;
            const char *kind = pd->prog == 0x00 ? "UHCI" : pd->prog == 0x10 ? "OHCI"
                             : pd->prog == 0x20 ? "EHCI" : pd->prog == 0x30 ? "xHCI" : "USB";
            bool ours = pd->prog == 0x00 || pd->prog == 0x10 ||
                        pd->prog == 0x20 || pd->prog == 0x30;
            if (!ours) usb_unsupported++;
            klog("usb: %02x:%02x.%x %04x:%04x %s%s", pd->bus, pd->dev, pd->fn, pd->vendor, pd->device,
                 kind, ours ? "" : " (no driver for this controller yet)");
        }
    }
    /* EHCI first: it owns the ports and hands full/low-speed ones to the
     * companion, whose probe comes next and enumerates them. */
    ehci_probe();
    ohci_probe();
    uhci_probe();
    xhci_probe();
    {   /* build the one-line summary after the probes know the result */
        pci_dev_t *devs = NULL;
        int n = pci_list(&devs);
        int len = 0;
        usb_summary[0] = 0;
        for (int i = 0; i < n; i++) {
            pci_dev_t *pd = &devs[i];
            if (pd->cls != 0x0C || pd->sub != 0x03) continue;
            const char *kind = pd->prog == 0x00 ? "UHCI" : pd->prog == 0x10 ? "OHCI"
                             : pd->prog == 0x20 ? "EHCI" : pd->prog == 0x30 ? "xHCI" : "USB";
            const ctrl_stat_t *st = cstat_find(pd);
            int k;
            if (st)
                k = snprintf(usb_summary + len, sizeof usb_summary - (size_t)len,
                             "%s%s %04x:%04x %s %dp/%dc", len ? ", " : "", kind, pd->vendor, pd->device,
                             st->ok ? "ok" : "FAILED", st->ports, st->conn);
            else
                k = snprintf(usb_summary + len, sizeof usb_summary - (size_t)len,
                             "%s%s %04x:%04x", len ? ", " : "", kind, pd->vendor, pd->device);
            if (k > 0 && len + k < (int)sizeof usb_summary) len += k;
        }
        if (!len) snprintf(usb_summary, sizeof usb_summary, "no USB controller found");
        len = (int)strlen(usb_summary);
        snprintf(usb_summary + len, sizeof usb_summary - (size_t)len, "; %d device(s)", usb_count());
    }
    if (devices) {
        task_create("usbd", usb_task, NULL, 8192);
        klog("usb: %d device(s)", usb_count());
    }
}
#else
#include "usb.h"
void usb_init(void) {}                          /* USB needs the 64-bit edition */
void usb_poll(void) {}
int usb_count(void) { return 0; }
const char *usb_boot_summary(void) { return "USB is only in the 64-bit edition"; }
int usb_has_input(void) { return 0; }
int usb_input_active(void) { return 0; }
const char *usb_device_summary(void) { return ""; }
const char *usb_live_text(void) { return ""; }
void usb_note_controller(const pci_dev_t *pd, int ok, int ports, int connected) { (void)pd; (void)ok; (void)ports; (void)connected; }
void usb_diag(const char *fmt, ...) { (void)fmt; }
const char *usb_diag_text(void) { return ""; }
int usb_unsupported;
#endif
