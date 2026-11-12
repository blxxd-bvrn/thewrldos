/* OHCI: the USB 1.1 Open Host Controller, the other companion used on
 * non-Intel chipsets and on many add-in cards. Unlike UHCI it builds the lists
 * itself: endpoint descriptors (EDs) own queues of transfer descriptors (TDs),
 * and the controller walks three lists - control, bulk and periodic.
 *
 * This driver is polled. A transfer puts one ED and its TDs on the right list,
 * enables that list, watches the condition codes until the last TD has been
 * touched, then disables the list again. The periodic list is used the same way
 * for one interrupt poll: its ED is placed in the 32-entry frame table for the
 * duration of the transfer. Full- and low-speed devices are both native here,
 * so a USB 1.1 keyboard and mouse work without a companion controller. */
#ifdef __x86_64__
#include "usb.h"
#include "devmgr.h"

#define HC_REVISION     0x00
#define HC_CONTROL      0x04
#define HC_COMMAND      0x08
#define HC_INTR_STATUS  0x0C
#define HC_INTR_ENABLE  0x10
#define HC_INTR_DISABLE 0x14
#define HC_HCCA         0x18
#define HC_PERIOD_CUR   0x1C
#define HC_CONTROL_HEAD 0x20
#define HC_CONTROL_CUR  0x24
#define HC_BULK_HEAD    0x28
#define HC_BULK_CUR     0x2C
#define HC_DONE_HEAD    0x30
#define HC_FM_INTERVAL  0x34
#define HC_FM_REMAIN    0x38
#define HC_FM_NUMBER    0x3C
#define HC_PERIOD_START 0x40
#define HC_LS_THRESHOLD 0x44
#define HC_RH_DESC_A    0x48
#define HC_RH_DESC_B    0x4C
#define HC_RH_STATUS    0x50
#define HC_RH_PORT(p)   (0x54 + (p) * 4)

#define CTRL_CBSR       (3u << 0)
#define CTRL_PLE        (1u << 2)
#define CTRL_CLE        (1u << 4)
#define CTRL_BLE        (1u << 5)
#define CTRL_HCFS       (3u << 6)
#define CTRL_OPER       (2u << 6)

#define CMD_HCR         (1u << 0)
#define CMD_CLF         (1u << 1)
#define CMD_BLF         (1u << 2)

#define RH_PS_CCS       (1u << 0)
#define RH_PS_PES       (1u << 1)
#define RH_PS_PRS       (1u << 4)
#define RH_PS_PPS       (1u << 8)
#define RH_PS_LSDA      (1u << 9)
#define RH_PS_CSC       (1u << 16)
#define RH_PS_PESC      (1u << 17)
#define RH_PS_PRSC      (1u << 20)
#define RH_CHANGES      (RH_PS_CSC | RH_PS_PESC | (1u << 18) | (1u << 19) | RH_PS_PRSC)
#define RH_HS_LPSC      (1u << 16)

#define ED_LOWSPEED     (1u << 13)
#define ED_OUT          (1u << 11)
#define ED_IN           (2u << 11)
#define ED_SKIP         (1u << 14)

#define TD_CC           0xF0000000u
#define TD_CC_GET(t)    ((t) >> 28)
#define TD_EC           (3u << 26)
#define TD_T_DATA0      (2u << 24)
#define TD_T_DATA1      (3u << 24)
#define TD_DP_SETUP     (0u << 19)
#define TD_DP_OUT       (1u << 19)
#define TD_DP_IN        (2u << 19)
#define TD_R            (1u << 18)

typedef struct { uint32_t control, tail, head, next; } ed_t;
typedef struct { uint32_t info, cbp, next, be; } td_t;

typedef struct {
    uint32_t intr[32];
    uint16_t frame;
    uint16_t pad;
    uint32_t done;
    uint32_t reserved[26];
} hcca_t;

typedef struct {
    usb_hcd_t hcd;
    volatile uint32_t *mmio;
    int ports;
    hcca_t *hcca;
    ed_t *ed_ctl, *ed_bulk, *ed_int;
    td_t *td;                       /* 96 real descriptors and three tails */
    uint8_t *bounce;
    uint32_t buf_start[96];
} ohci_t;

#define TD_SLOTS 32
#define TD_CTL   0
#define TD_BULK  TD_SLOTS
#define TD_INT   (TD_SLOTS * 2)
#define TD_DUMMY (TD_SLOTS * 3)          /* one past each queue, never executed */

static uint32_t or32(ohci_t *o, int off) { return o->mmio[off / 4]; }
static void ow32(ohci_t *o, int off, uint32_t v) { o->mmio[off / 4] = v; }
static void uwait(uint32_t ms) { uint32_t t0 = timer_ticks(); while ((int32_t)(timer_ticks() - t0) < (int32_t)ms) task_yield(); }
static uint32_t phys(const void *p) { return (uint32_t)(uintptr_t)p; }

static void td_fill(td_t *t, uint32_t dp, uint32_t toggle, const void *buf, int len) {
    memset(t, 0, sizeof *t);
    t->info = TD_CC | TD_EC | toggle | dp | (len ? TD_R : 0);
    t->cbp = buf && len ? (uint32_t)(uintptr_t)buf : 0;
    t->be = buf && len ? (uint32_t)(uintptr_t)buf + (uint32_t)len - 1 : 0;
    t->next = 0;
}

static void ed_fill(ed_t *e, int addr, int ep, int in, bool low, int maxpkt) {
    memset(e, 0, sizeof *e);
    e->control = (uint32_t)(addr & 0x7F) | (uint32_t)(ep & 0x0F) << 7 |
                 (uint32_t)(in ? ED_IN : ED_OUT) | (low ? ED_LOWSPEED : 0) |
                 (uint32_t)(maxpkt & 0x7FF) << 16;
    e->tail = 0;
    e->head = 0;
    e->next = 0;
}

/* Point an ED at the TD chain and put it on the control (0), bulk (1) or
 * periodic (2) list. `last` is the descriptor that must finish. */
static void ohci_link(ohci_t *o, ed_t *ed, int first, int n, int list) {
    int dummy = TD_DUMMY + list;
    memset(&o->td[dummy], 0, sizeof o->td[dummy]);
    o->td[dummy].info = TD_CC;                           /* never reached */
    for (int i = 0; i < n; i++)
        o->td[first + i].next = (i + 1 < n) ? phys(&o->td[first + i + 1]) : phys(&o->td[dummy]);
    ed->head = phys(&o->td[first]);
    ed->tail = phys(&o->td[dummy]);                      /* the controller stops here */
    if (list == 2) {
        for (int i = 0; i < 32; i++) o->hcca->intr[i] = phys(ed);
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) | CTRL_PLE);
    } else if (list == 1) {
        ow32(o, HC_BULK_HEAD, phys(ed));
        ow32(o, HC_COMMAND, CMD_BLF);
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) | CTRL_BLE);
    } else {
        ow32(o, HC_CONTROL_HEAD, phys(ed));
        ow32(o, HC_COMMAND, CMD_CLF);
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) | CTRL_CLE);
    }
}

static void ohci_unlink(ohci_t *o, ed_t *ed, int list) {
    if (list == 2) {
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) & ~CTRL_PLE);
        for (int i = 0; i < 32; i++) o->hcca->intr[i] = 0;
    } else if (list == 1) {
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) & ~CTRL_BLE);
        ow32(o, HC_COMMAND, CMD_BLF);
        ow32(o, HC_BULK_HEAD, 0);
    } else {
        ow32(o, HC_CONTROL, or32(o, HC_CONTROL) & ~CTRL_CLE);
        ow32(o, HC_COMMAND, CMD_CLF);
        ow32(o, HC_CONTROL_HEAD, 0);
    }
    ed->head = ed->tail = 0;
    ed->next = 0;
    uwait(1);                                            /* let the controller let go */
}

static int td_error(uint32_t cc) {
    return cc != 0 && cc != 0x9 && cc != 0xD && cc != 0xF;
}

/* Wait for the last descriptor of a chain. Returns 0, -2 (nothing happened) or
 * -1 (the controller reported an error). */
static int ohci_wait(ohci_t *o, int first, int n, int timeout_ms) {
    uint32_t t0 = timer_ticks();
    for (;;) {
        for (int i = 0; i < n; i++) {
            uint32_t cc = TD_CC_GET(o->td[first + i].info);
            if (td_error(cc)) return -1;
            if (i == n - 1 && cc != 0xF) return 0;
        }
        if ((int32_t)(timer_ticks() - t0) >= timeout_ms) return -2;
        task_yield();
    }
}

static int td_actual(const td_t *t, uint32_t start, int len) {
    if (!len) return 0;
    uint32_t cbp = t->cbp;
    if (cbp) {
        int got = (int)(cbp - start);
        if (got < 0) got = 0;
        if (got > len) got = len;
        return got;
    }
    return len;
}

static int ohci_control(usb_hcd_t *h, int addr, bool low, uint8_t type, uint8_t req,
                        uint16_t value, uint16_t index, void *buf, int len)
{
    ohci_t *o = h->priv;
    if (len > 512) len = 512;
    uint8_t *data = o->bounce;
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    bool in = (type & 0x80) != 0;
    if (!in && len) memcpy(data, buf, len);

    int maxpkt = low ? 8 : 64;
    ed_fill(o->ed_ctl, addr, 0, false, low, maxpkt);
    o->ed_ctl->control &= ~(3u << 11);                  /* direction comes from each TD */
    int n = 0;
    td_fill(&o->td[TD_CTL + n], TD_DP_SETUP, TD_T_DATA0, setup, 8); n++;
    int first_data = n, ndata = 0, off = 0;
    while (off < len && n < TD_SLOTS - 1) {
        int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
        td_fill(&o->td[TD_CTL + n], in ? TD_DP_IN : TD_DP_OUT,
                ((off / maxpkt) & 1) ? TD_T_DATA0 : TD_T_DATA1, data + off, chunk);
        o->buf_start[TD_CTL + n] = (uint32_t)(uintptr_t)(data + off);
        off += chunk; n++; ndata++;
    }
    len = off;
    td_fill(&o->td[TD_CTL + n], in ? TD_DP_OUT : TD_DP_IN, TD_T_DATA1, NULL, 0); n++;

    ohci_link(o, o->ed_ctl, TD_CTL, n, 0);
    int r = ohci_wait(o, TD_CTL, n, 2000);
    ohci_unlink(o, o->ed_ctl, 0);
    if (r < 0) return -1;

    int got = 0;
    for (int i = 0; i < ndata; i++) {
        td_t *t = &o->td[TD_CTL + first_data + i];
        int requested = (int)(t->be >= o->buf_start[TD_CTL + first_data + i] ?
                              t->be - o->buf_start[TD_CTL + first_data + i] + 1 : 0);
        got += td_actual(t, o->buf_start[TD_CTL + first_data + i], requested);
    }
    if (in && len) {
        if (got > len) got = len;
        memcpy(buf, data, got);
    }
    return in ? got : len;
}

static int ohci_transfer(usb_hcd_t *h, int addr, bool low, int ep, bool in,
                         uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms)
{
    ohci_t *o = h->priv;
    if (ep < 1 || ep > 15 || !len) return len ? -1 : 0;
    if (maxpkt > 1023) maxpkt = 1023;
    bool poll = timeout_ms <= 100;
    if (poll) {
        if (len > maxpkt) len = maxpkt;
        ed_fill(o->ed_int, addr, ep, in, low, maxpkt);
        td_fill(&o->td[TD_INT], in ? TD_DP_IN : TD_DP_OUT,
                *toggle ? TD_T_DATA1 : TD_T_DATA0, buf, len);
        ohci_link(o, o->ed_int, TD_INT, 1, 2);
        int r = ohci_wait(o, TD_INT, 1, timeout_ms);
        ohci_unlink(o, o->ed_int, 2);
        /* a report that arrived while the list was being taken down was still
         * answered, so it has to be kept */
        if (r == -2) r = ohci_wait(o, TD_INT, 1, 0);
        if (r == -1) return -1;
        if (r == -2) return -2;
        int got = td_actual(&o->td[TD_INT], (uint32_t)(uintptr_t)buf, len);
        *toggle ^= 1;
        return got;
    }

    int slots = TD_SLOTS;
    if (len > maxpkt * (slots - 1)) len = maxpkt * (slots - 1);
    if (len > 16384) len = 16384;
    ed_fill(o->ed_bulk, addr, ep, in, low, maxpkt);
    int n = 0, off = 0, pkts = 0;
    while (off < len && n < slots) {
        int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
        td_fill(&o->td[TD_BULK + n], in ? TD_DP_IN : TD_DP_OUT,
                (*toggle ^ (pkts & 1)) ? TD_T_DATA1 : TD_T_DATA0, (uint8_t *)buf + off, chunk);
        o->buf_start[TD_BULK + n] = (uint32_t)(uintptr_t)((uint8_t *)buf + off);
        off += chunk; n++; pkts++;
    }
    ohci_link(o, o->ed_bulk, TD_BULK, n, 1);
    int r = ohci_wait(o, TD_BULK, n, timeout_ms);
    ohci_unlink(o, o->ed_bulk, 1);
    if (r < 0) return r;
    int got = 0;
    for (int i = 0; i < n; i++) {
        int requested = (int)(o->td[TD_BULK + i].be >= o->buf_start[TD_BULK + i] ?
                              o->td[TD_BULK + i].be - o->buf_start[TD_BULK + i] + 1 : 0);
        got += td_actual(&o->td[TD_BULK + i], o->buf_start[TD_BULK + i], requested);
    }
    *toggle ^= (pkts & 1);
    if (in && got > len) got = len;
    return in ? got : len;
}

/* ---------------- root hub ---------------- */
static bool ohci_port_reset(usb_hcd_t *h, int port, bool *low_speed)
{
    ohci_t *o = h->priv;
    if (port < 0 || port >= o->ports) return false;
    uint32_t p = HC_RH_PORT(port);
    uint32_t v = or32(o, p);
    if (!(v & RH_PS_CCS)) return false;
    if (!(v & RH_PS_PPS)) {
        ow32(o, p, RH_PS_PPS);
        uwait(20);
        v = or32(o, p);
    }
    if (!(v & RH_PS_PRS)) {
        ow32(o, p, RH_PS_PRS);
        uint32_t t0 = timer_ticks();
        while ((or32(o, p) & RH_PS_PRS) && (int32_t)(timer_ticks() - t0) < 200) task_yield();
    }
    v = or32(o, p);
    ow32(o, p, RH_CHANGES);                              /* write one to clear */
    v = or32(o, p);
    if (!(v & RH_PS_PES)) { klog("ohci: port %d did not enable", port + 1); return false; }
    *low_speed = (v & RH_PS_LSDA) != 0;
    return true;
}

/* Is something still on this root port? Reading the status has no side
 * effects; the change bits are cleared by a write in port_reset alone. */
static bool ohci_port_up(usb_hcd_t *h, int port)
{
    ohci_t *o = h->priv;
    if (port < 0 || port >= o->ports) return false;
    return (or32(o, HC_RH_PORT(port)) & RH_PS_CCS) != 0;
}

static int ohci_init(ohci_t *o, const pci_dev_t *pd)
{
    bool is_io = false;
    uintptr_t bar = pci_bar(pd, 0, &is_io);
    if (!bar || is_io) return -1;
    if (bar >= 0x100000000ull) { klog("ohci: MMIO above the identity map"); return -1; }
    pci_enable(pd, false, true);
    paging_set_uncached(bar);
    o->mmio = (volatile uint32_t *)bar;

    ow32(o, HC_INTR_DISABLE, 0xFFFFFFFFu);
    ow32(o, HC_COMMAND, CMD_HCR);
    uint32_t t0 = timer_ticks();
    while ((or32(o, HC_COMMAND) & CMD_HCR) && (int32_t)(timer_ticks() - t0) < 500) task_yield();
    uwait(10);
    ow32(o, HC_CONTROL, CTRL_CBSR);                      /* reset state */
    t0 = timer_ticks();
    while (((or32(o, HC_CONTROL) & CTRL_HCFS) != 0) &&
           (int32_t)(timer_ticks() - t0) < 100) task_yield();
    ow32(o, HC_CONTROL, CTRL_OPER | CTRL_CBSR);
    t0 = timer_ticks();
    while (((or32(o, HC_CONTROL) & CTRL_HCFS) != CTRL_OPER) &&
           (int32_t)(timer_ticks() - t0) < 100) task_yield();

    uint32_t desc_a = or32(o, HC_RH_DESC_A);
    o->ports = (int)(desc_a & 0xFF);
    if (o->ports > 15) o->ports = 15;
    if (!o->ports) return -1;

    o->hcca = kmalloc_aligned(sizeof *o->hcca, 256);
    o->ed_ctl = kmalloc_aligned(sizeof *o->ed_ctl, 16);
    o->ed_bulk = kmalloc_aligned(sizeof *o->ed_bulk, 16);
    o->ed_int = kmalloc_aligned(sizeof *o->ed_int, 16);
    o->td = kmalloc_aligned((size_t)(TD_SLOTS * 3 + 3) * sizeof *o->td, 16);
    o->bounce = kmalloc_aligned(4096, 4096);
    if (!o->hcca || !o->ed_ctl || !o->ed_bulk || !o->ed_int || !o->td || !o->bounce) return -1;
    memset(o->hcca, 0, sizeof *o->hcca);
    memset(o->td, 0, (size_t)(TD_SLOTS * 3 + 3) * sizeof *o->td);
    memset(o->bounce, 0, 4096);
    memset(o->ed_ctl, 0, sizeof *o->ed_ctl);
    memset(o->ed_bulk, 0, sizeof *o->ed_bulk);
    memset(o->ed_int, 0, sizeof *o->ed_int);
    memset(o->buf_start, 0, sizeof o->buf_start);

    ow32(o, HC_HCCA, phys(o->hcca));
    ow32(o, HC_FM_INTERVAL, 0x27782EDF);
    ow32(o, HC_PERIOD_START, 0x2A2F);
    ow32(o, HC_LS_THRESHOLD, 0x628);
    ow32(o, HC_INTR_STATUS, 0xFFFFFFFFu);
    ow32(o, HC_INTR_DISABLE, 0xFFFFFFFFu);

    if (!(desc_a & (1u << 9)))                           /* power switching present */
        ow32(o, HC_RH_STATUS, RH_HS_LPSC);
    for (int p = 0; p < o->ports; p++) {
        uint32_t v = or32(o, HC_RH_PORT(p));
        if (!(v & RH_PS_PPS)) ow32(o, HC_RH_PORT(p), RH_PS_PPS);
    }
    uwait(100);

    o->hcd.name = "OHCI";
    o->hcd.control = ohci_control;
    o->hcd.transfer = ohci_transfer;
    o->hcd.port_reset = ohci_port_reset;
    o->hcd.port_up = ohci_port_up;
    o->hcd.ports = o->ports;
    o->hcd.priv = o;
    return 0;
}

static bool ohci_match(const pci_dev_t *p) { return (p->cls == 0x0C && p->sub == 0x03 && p->prog == 0x10); }
DRIVER_PCI(drv_ohci, "usbohci", "USB 1.1 OHCI host controllers", DC_USBCTL, ohci_match);

void ohci_probe(void)
{
    pci_dev_t *devs = NULL;
    int n = pci_list(&devs), found = 0;
    for (int i = 0; i < n; i++) {
        pci_dev_t *pd = &devs[i];
        if (!(pd->cls == 0x0C && pd->sub == 0x03 && pd->prog == 0x10)) continue;
        ohci_t *o = kzalloc(sizeof *o);
        if (!o) return;
        if (ohci_init(o, pd) < 0) {
            kfree(o);
            klog("usb: %04x:%04x OHCI controller did not start", pd->vendor, pd->device);
            continue;
        }
        found++;
        klog("usb: OHCI %04x:%04x at %08lx, %d ports", pd->vendor, pd->device,
             (unsigned long)(uintptr_t)o->mmio, o->ports);
        { char nm[64]; snprintf(nm, sizeof nm, "USB 1.1 OHCI host controller (%d ports)", (int)o->ports); dev_claim(pd, &drv_ohci, nm); }
        usb_attach(&o->hcd);
    }
    if (!found) klog("usb: no OHCI controller found");
}
#endif
