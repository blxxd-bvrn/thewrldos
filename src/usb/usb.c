/* USB core: root-port enumeration, standard requests, and the class drivers
 * this system needs. HID boot keyboards and mice are translated into the PS/2
 * scancodes and packets the rest of the system already understands; mass
 * storage becomes an ordinary SCSI disk in the block layer. */
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
        if ((d->kind == USB_KIND_KEYBOARD || d->kind == USB_KIND_MOUSE) && d->seen) return 1;
    return 0;
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
/* HID usage IDs to PS/2 set 1 scancodes. 0xFF marks the keys that need an E0
 * prefix; anything else with 0 is a key we do not send. */
static uint8_t hid_scan[0xE8];
static void hid_init_table(void)
{
    static const uint8_t letters[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                         0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    static const uint8_t digits[10] = { 0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B };
    for (int i = 0; i < 26; i++) hid_scan[0x04 + i] = letters[i];
    for (int i = 0; i < 10; i++) hid_scan[0x1E + i] = digits[i];
    hid_scan[0x28] = 0x1C; hid_scan[0x29] = 0x01; hid_scan[0x2A] = 0x0E; hid_scan[0x2B] = 0x0F;
    hid_scan[0x2C] = 0x39; hid_scan[0x2D] = 0x0C; hid_scan[0x2E] = 0x0D; hid_scan[0x2F] = 0x1A;
    hid_scan[0x30] = 0x1B; hid_scan[0x31] = 0x2B; hid_scan[0x33] = 0x27; hid_scan[0x34] = 0x28;
    hid_scan[0x35] = 0x29; hid_scan[0x36] = 0x33; hid_scan[0x37] = 0x34; hid_scan[0x38] = 0x35;
    hid_scan[0x39] = 0x3A;
    hid_scan[0x4F] = 0xFF; hid_scan[0x50] = 0xFF; hid_scan[0x51] = 0xFF; hid_scan[0x52] = 0xFF;
    hid_scan[0xE0] = 0x1D; hid_scan[0xE1] = 0x2A; hid_scan[0xE2] = 0x38; hid_scan[0xE3] = 0xFF;
    hid_scan[0xE4] = 0xFF; hid_scan[0xE5] = 0x36; hid_scan[0xE6] = 0xFF; hid_scan[0xE7] = 0xFF;
}

static void hid_send_usage(uint8_t usage, bool release)
{
    extern void keyboard_scancode(uint8_t code);
    uint8_t up = (uint8_t)(release ? 0x80 : 0);
    if (usage >= 0xE3 && usage <= 0xE7) {
        static const uint8_t e0[5] = { 0x5B, 0x1D, 0x38, 0x5C, 0x38 };   /* LWIN, RCTRL, RALT, RWIN, RALT */
        keyboard_scancode(0xE0);
        keyboard_scancode((uint8_t)(e0[usage - 0xE3] | up));
        return;
    }
    if (usage >= 0x4F && usage <= 0x52) {
        static const uint8_t e0[4] = { 0x4D, 0x4B, 0x50, 0x48 };         /* right, left, down, up */
        keyboard_scancode(0xE0);
        keyboard_scancode((uint8_t)(e0[usage - 0x4F] | up));
        return;
    }
    uint8_t sc = usage < 0xE8 ? hid_scan[usage] : 0;
    if (sc && sc != 0xFF) keyboard_scancode((uint8_t)(sc | up));
}

static void hid_keyboard_report(usb_dev_t *d, const uint8_t *rep, int n)
{
    if (n < 8) return;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        bool was = (d->report[0] & bit) != 0, now = (rep[0] & bit) != 0;
        if (now != was) hid_send_usage((uint8_t)(0xE0 + i), !now);
    }
    for (int i = 2; i < 8; i++) {                       /* newly pressed keys */
        uint8_t u = rep[i];
        if (!u) continue;
        bool seen = false;
        for (int j = 2; j < 8; j++) if (d->report[j] == u) seen = true;
        if (!seen) hid_send_usage(u, false);
    }
    for (int j = 2; j < 8; j++) {                       /* keys that came up */
        uint8_t u = d->report[j];
        if (!u) continue;
        bool still = false;
        for (int i = 2; i < 8; i++) if (rep[i] == u) still = true;
        if (!still) hid_send_usage(u, true);
    }
    memcpy(d->report, rep, 8);
}

static void hid_report(usb_dev_t *d, const uint8_t *rep, int n)
{
    if (!d->seen) {
        d->seen = true;
        klog("usb: %s at address %d is sending reports", d->kind == USB_KIND_KEYBOARD ? "keyboard" : "mouse", d->addr);
    }
    if (d->kind == USB_KIND_KEYBOARD) { hid_keyboard_report(d, rep, n); return; }
    if (d->kind == USB_KIND_MOUSE) {
        extern void mouse_inject(int dx, int dy, int buttons, int wheel);
        if (n < 3) return;
        int wheel = 0;
        if (n >= 4) { int8_t w = (int8_t)rep[3]; if (w >= -8 && w <= 8) wheel = -w; }
        mouse_inject((int8_t)rep[1], (int8_t)rep[2], rep[0] & 7, wheel);
    }
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
static void parse_config(usb_dev_t *d, const uint8_t *cfg, int len, int *iface_out)
{
    int i = 0, iface = -1;
    while (i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len) {
        const uint8_t *p = cfg + i;
        if (p[1] == 4 && p[0] >= 9) {
            iface = p[2];
            if (!d->cls) { d->cls = p[5]; d->subcls = p[6]; d->proto = p[7]; }
            *iface_out = iface;
        } else if (p[1] == 5 && p[0] >= 7) {
            int ep = p[2] & 0x0F, attr = p[3] & 3, maxpkt = p[4] | (p[5] << 8);
            if (attr == 3 && (p[2] & 0x80) && !d->ep_int) { d->ep_int = ep; d->maxpkt_int = maxpkt; }
            else if (attr == 2) {
                if (p[2] & 0x80) { d->ep_in = ep; d->maxpkt_in = maxpkt; }
                else { d->ep_out = ep; d->maxpkt_out = maxpkt; }
            }
        }
        i += p[0];
    }
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
    d->cls = dev[4]; d->subcls = dev[5]; d->proto = dev[6];
    d->id = next_id++;

    uint8_t cfg9[9];
    if (get_descriptor(h, addr, low, 2, 0, cfg9, 9) < 9) { klog("usb: address %d: no configuration", addr); kfree(d); return; }
    int total = cfg9[2] | (cfg9[3] << 8);
    if (total < 9) total = 9;
    if (total > 256) total = 256;
    uint8_t cfg[256];
    if (get_descriptor(h, addr, low, 2, 0, cfg, total) < 9) { klog("usb: address %d: configuration read failed", addr); kfree(d); return; }
    int iface = 0;
    parse_config(d, cfg, total, &iface);

    if (h->control(h, addr, low, 0x00, 9, 1, 0, NULL, 0) < 0) {      /* SET_CONFIGURATION(1) */
        klog("usb: address %d: SET_CONFIGURATION failed", addr);
        kfree(d);
        return;
    }

    const char *what = "device";
    if (d->cls == 3) {                                               /* HID */
        usb_control(d, 0x21, 0x0B, 0, (uint16_t)iface, NULL, 0);     /* SET_PROTOCOL(boot) */
        usb_control(d, 0x21, 0x0A, 0, (uint16_t)iface, NULL, 0);     /* SET_IDLE(0) */
        if (d->proto == 2) { d->kind = USB_KIND_MOUSE; what = "mouse"; }
        else { d->kind = USB_KIND_KEYBOARD; what = "keyboard"; }
        if (!d->ep_int) { klog("usb: %s at address %d has no interrupt endpoint", what, addr); kfree(d); return; }
    } else if (d->cls == 8) {                                        /* mass storage */
        what = "mass storage";
        snprintf(d->name, sizeof d->name, "usb-storage");
        d->next = devices; devices = d;                              /* visible to the storage driver */
        stor_attach(d, iface);
        klog("usb: %s %04x:%04x at address %d (%s)", what, d->vendor, d->product, addr, where);
        return;
    } else if (d->cls == 9) {                                        /* hub: look behind it */
        d->next = devices; devices = d;
        klog("usb: hub %04x:%04x at address %d (%s)", d->vendor, d->product, addr, where);
        hub_attach(d);
        return;
    } else {
        klog("usb: %04x:%04x class %d at address %d (%s): no driver", d->vendor, d->product, d->cls, addr, where);
        kfree(d);
        return;
    }
    d->next = devices; devices = d;
    klog("usb: %s %04x:%04x at address %d (%s%s)", what, d->vendor, d->product, addr, where,
         low ? ", low speed" : "");
}

/* A hub is not a device we use directly: power every port, reset the ones with
 * something connected, and enumerate whatever appears there. */
static void hub_attach(usb_dev_t *hub)
{
    uint8_t hd[8];
    if (usb_control(hub, 0xA0, 6, 0x2900, 0, hd, sizeof hd) < 3) {
        klog("usb: hub at address %d: no hub descriptor", hub->addr);
        return;
    }
    int ports = hd[2];
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
        int speed = (st[1] & 0x04) ? 3 : low ? 2 : 1;                 /* high, low, full */
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
void usb_poll(void)
{
    for (usb_dev_t *d = devices; d; d = d->next) {
        if (d->kind != USB_KIND_KEYBOARD && d->kind != USB_KIND_MOUSE) continue;
        uint8_t rep[8];
        memset(rep, 0, sizeof rep);
        int n = usb_in(d, d->ep_int, d->maxpkt_int, &d->toggle_int, rep, d->kind == USB_KIND_KEYBOARD ? 8 : 4, 3);
        if (n > 0) hid_report(d, rep, n);
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
void usb_note_controller(const pci_dev_t *pd, int ok, int ports, int connected) { (void)pd; (void)ok; (void)ports; (void)connected; }
void usb_diag(const char *fmt, ...) { (void)fmt; }
const char *usb_diag_text(void) { return ""; }
int usb_unsupported;
#endif
