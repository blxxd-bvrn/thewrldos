/* xHCI: the USB 3.x host controller interface, and the only USB controller a
 * machine made in the last decade has. Everything moves through rings of TRBs
 * (transfer request blocks) in memory: a command ring for the controller, one
 * transfer ring per endpoint, and an event ring the controller fills in. There
 * are no interrupts here; the rings are polled, like the UHCI driver.
 *
 * Layout follows the xHCI 1.1 specification: registers in BAR0 (MMIO), devices
 * addressed by slot, a device context base address array the controller reads,
 * and contexts that are 32 or 64 bytes depending on HCCPARAMS1.CSZ.
 *
 * Enumeration: reset and enable the port, Enable Slot, Address Device with BSR
 * (which activates endpoint 0 but leaves the address at 0), read the device
 * descriptor, then Address Device again with the address the USB core picked.
 *
 * Interrupt endpoints are asynchronous by nature: an IN TRB stays queued until
 * the device has something to say, so a poll arms a TRB, returns nothing, and
 * collects the completion on a later poll. Bulk and control transfers are
 * awaited directly. */
#ifdef __x86_64__
#include "usb.h"

/* ---------------- register offsets ---------------- */
#define CAPLENGTH   0x00
#define HCSPARAMS1  0x04
#define HCSPARAMS2  0x08
#define HCCPARAMS1  0x10
#define DBOFF       0x14
#define RTSOFF      0x18
/* Operational registers are relative to CAPLENGTH (x->op); the capability
 * registers above are absolute from the BAR. */
#define USBCMD      0x00
#define USBSTS      0x04
#define PAGESIZE    0x08
#define CRCR        0x18
#define DCBAAP      0x30
#define CONFIG      0x38
#define PORTSC(p)   (0x400 + (p) * 0x10)

#define CMD_RUN     (1u << 0)
#define CMD_HCRST   (1u << 1)
#define STS_HCH     (1u << 0)
#define STS_CNR     (1u << 11)

#define PORT_CCS    (1u << 0)
#define PORT_PED    (1u << 1)
#define PORT_PR     (1u << 4)
#define PORT_PP     (1u << 9)
#define PORT_CSC    (1u << 17)
#define PORT_PEC    (1u << 18)
#define PORT_WRC    (1u << 19)
#define PORT_OCC    (1u << 20)
#define PORT_PRC    (1u << 21)
#define PORT_PLC    (1u << 22)
#define PORT_CEC    (1u << 23)
#define PORT_CHANGE (PORT_CSC | PORT_PEC | PORT_WRC | PORT_OCC | PORT_PRC | PORT_PLC | PORT_CEC)

/* TRB types */
#define TRB_NORMAL       1
#define TRB_SETUP        2
#define TRB_DATA         3
#define TRB_STATUS       4
#define TRB_LINK         6
#define TRB_ENABLE_SLOT  9
#define TRB_ADDRESS      11
#define TRB_CONFIGURE    12
#define TRB_CMD_COMPLETE 33
#define TRB_TRANSFER     32

#define TRB_CYCLE  (1u << 0)
#define TRB_TC     (1u << 1)
#define TRB_IOC    (1u << 5)
#define TRB_IDT    (1u << 6)
#define TRB_DIR    (1u << 16)
#define TRB_BSR    (1u << 9)
#define T(t)       ((uint32_t)(t) << 10)

#define TRB_LEN     0x00FFFFFFu
#define TRB_CODE(s) (((s) >> 24) & 0xFF)
#define TRB_EPC(c)  (((c) >> 16) & 0x1F)

#define CC_SUCCESS 1
#define CC_SHORT   13

#define EP_CTL      4
#define EP_BULK_OUT 2
#define EP_BULK_IN  6
#define EP_INT_IN   7

typedef struct { uint64_t param; uint32_t status; uint32_t control; } trb_t;

typedef struct {
    trb_t *trb;
    uint64_t phys;
    int n;                     /* usable entries (the link TRB is one past) */
    int idx, ccs;
} ring_t;

typedef struct xdev {
    int slot, addr, speed, root_port, route;
    bool bsr;                  /* endpoint 0 answers, the address is still 0 */
    int ep0_maxpkt;
    ring_t ep0;
    struct {
        ring_t ring;
        int maxpkt, dci;
        bool configured;
        uint64_t armed;        /* an interrupt IN TRB waiting in the controller */
        int armed_len;
    } ep[16];
    int max_dci;
    struct xdev *next;
} xdev_t;

typedef struct {
    usb_hcd_t hcd;
    volatile uint32_t *mmio;
    uint16_t op;               /* operational register base (CAPLENGTH) */
    int max_slots, max_ports;
    int ctx;                   /* bytes per context */
    uint32_t dboff, rtsoff;
    uint64_t *dcbaa;
    ring_t cmd, evt;
    trb_t *erst;
    uint8_t *buf;              /* one page of bounce buffer */
    xdev_t *devs, *pending;
    uint8_t port_speed[32];
    /* events that were popped while looking for someone else's completion */
    uint64_t pendp[64];
    uint32_t pends[64], pendc[64];
    int npend;
} xhci_t;

static uint32_t r32(xhci_t *x, int off) { return x->mmio[off / 4]; }
static void w32(xhci_t *x, int off, uint32_t v) { x->mmio[off / 4] = v; }
static uint64_t r64(xhci_t *x, int off) { return (uint64_t)r32(x, off) | ((uint64_t)r32(x, off + 4) << 32); }
static void w64(xhci_t *x, int off, uint64_t v) { w32(x, off, (uint32_t)v); w32(x, off + 4, (uint32_t)(v >> 32)); }
static void ring_doorbell(xhci_t *x, int slot, int target) { w32(x, x->dboff + slot * 4, (uint32_t)target); }
static void uwait(uint32_t ms) { uint32_t t0 = timer_ticks(); while ((int32_t)(timer_ticks() - t0) < (int32_t)ms) task_yield(); }

/* ---------------- rings ---------------- */
static void ring_init(ring_t *r, int entries, bool linked)
{
    r->trb = kmalloc_aligned((size_t)entries * sizeof(trb_t), 64);
    r->phys = (uint64_t)(uintptr_t)r->trb;
    if (!r->trb) { r->n = 0; return; }
    memset(r->trb, 0, (size_t)entries * sizeof(trb_t));
    r->n = linked ? entries - 1 : entries;
    r->idx = 0; r->ccs = 1;
    if (linked) {
        trb_t *l = &r->trb[r->n];
        l->param = r->phys;
        l->status = 0;
        l->control = T(TRB_LINK) | TRB_TC | TRB_CYCLE;
    }
}

static uint64_t ring_push(ring_t *r, uint64_t param, uint32_t status, uint32_t type, uint32_t flags)
{
    if (!r->n) return 0;
    if (r->idx >= r->n) {                                     /* follow the link */
        trb_t *l = &r->trb[r->n];
        l->control = (l->control & ~TRB_CYCLE) | (uint32_t)r->ccs;
        r->idx = 0;
        r->ccs ^= 1;
    }
    trb_t *t = &r->trb[r->idx];
    t->param = param;
    t->status = status;
    t->control = T(type) | flags | (uint32_t)r->ccs;
    uint64_t addr = r->phys + (uint64_t)r->idx * sizeof(trb_t);
    r->idx++;
    return addr;
}

/* the dequeue pointer for a context: address plus the current cycle state */
static uint64_t ring_dequeue(ring_t *r) { return (r->phys + (uint64_t)r->idx * sizeof(trb_t)) | (uint64_t)(r->ccs & 1); }

static bool evt_pop(xhci_t *x, uint64_t *param, uint32_t *status, uint32_t *control)
{
    ring_t *r = &x->evt;
    volatile trb_t *v = (volatile trb_t *)&r->trb[r->idx];
    uint32_t c = v->control;
    if ((c & TRB_CYCLE) != (uint32_t)r->ccs) return false;
    *param = v->param;
    *status = v->status;
    *control = c;
    r->idx++;
    if (r->idx >= r->n) { r->idx = 0; r->ccs ^= 1; }
    w64(x, x->rtsoff + 0x38, (r->phys + (uint64_t)r->idx * sizeof(trb_t)) | 8);   /* ERDP + EHB */
    return true;
}

/* Completions for other endpoints are kept here until their owner asks; the
 * event ring is shared by every device on the controller. */
static void pend_put(xhci_t *x, uint64_t p, uint32_t s, uint32_t c)
{
    if (x->npend >= 64) return;
    x->pendp[x->npend] = p; x->pends[x->npend] = s; x->pendc[x->npend] = c;
    x->npend++;
}
static bool pend_take(xhci_t *x, uint32_t type, uint64_t match, uint32_t *status, uint32_t *control)
{
    for (int i = 0; i < x->npend; i++) {
        if (((x->pendc[i] >> 10) & 0x3F) == type && (match == 0 || x->pendp[i] == match)) {
            *status = x->pends[i];
            *control = x->pendc[i];
            for (int j = i; j + 1 < x->npend; j++) {
                x->pendp[j] = x->pendp[j + 1];
                x->pends[j] = x->pends[j + 1];
                x->pendc[j] = x->pendc[j + 1];
            }
            x->npend--;
            return true;
        }
    }
    return false;
}
/* Non-blocking: look in the stash, then drain what the controller wrote. */
static bool take_event(xhci_t *x, uint32_t type, uint64_t match, uint32_t *status, uint32_t *control)
{
    if (pend_take(x, type, match, status, control)) return true;
    uint64_t param; uint32_t s, c;
    while (evt_pop(x, &param, &s, &c)) {
        if (((c >> 10) & 0x3F) == type && (match == 0 || param == match)) {
            *status = s;
            *control = c;
            return true;
        }
        pend_put(x, param, s, c);
    }
    return false;
}

static bool evt_wait(xhci_t *x, uint32_t type, uint64_t match, int timeout_ms,
                     uint32_t *code, int *slot, uint32_t *left)
{
    uint32_t t0 = timer_ticks();
    for (;;) {
        uint32_t status, control;
        if (take_event(x, type, match, &status, &control)) {
            if (code) *code = TRB_CODE(status);
            if (slot) *slot = (control >> 24) & 0xFF;
            if (left) *left = status & TRB_LEN;
            return true;
        }
        if ((int32_t)(timer_ticks() - t0) >= timeout_ms) return false;
    }
}

/* ---------------- contexts ---------------- */
/* One context is 32 or 64 bytes; a device context holds a slot and 31 endpoint
 * contexts, an input context the same plus the input control context. */
static uint32_t *ctx_alloc(xhci_t *x, int contexts)
{
    uint32_t *p = kmalloc_aligned((size_t)contexts * x->ctx, 64);
    if (p) memset(p, 0, (size_t)contexts * x->ctx);
    return p;
}
static int ctx_dwords(xhci_t *x) { return x->ctx / 4; }
static uint32_t *ictx_slot(xhci_t *x, uint32_t *in) { return in + ctx_dwords(x); }
static uint32_t *ictx_ep(xhci_t *x, uint32_t *in, int dci) { return in + ctx_dwords(x) * (1 + dci); }

static void fill_slot(xhci_t *x, uint32_t *s, xdev_t *d, int entries, int addr)
{
    (void)x;
    s[0] = (uint32_t)(d->route & 0xFFFFF) | ((uint32_t)d->speed << 20) | ((uint32_t)entries << 27);
    s[1] = (uint32_t)d->root_port << 16;
    s[3] = (uint32_t)addr;
}
/* `dequeue` is a ring pointer with the DCS bit in bit 0 */
static void fill_ep(xhci_t *x, uint32_t *e, int type, uint64_t dequeue, int maxpkt, int interval)
{
    (void)x;
    e[0] = (uint32_t)(interval & 0xFF) << 16;
    e[1] = (uint32_t)type << 3 | (3u << 1) | ((uint32_t)maxpkt << 16);
    e[2] = (uint32_t)(dequeue & 0xFFFFFFF0ull) | (uint32_t)(dequeue & 1);
    e[3] = (uint32_t)(dequeue >> 32);
    e[4] = 8;
}

static int xhci_cmd(xhci_t *x, int slot, uint64_t param, uint32_t status, uint32_t type,
                    uint32_t flags, uint32_t *code_out, int *slot_out)
{
    uint64_t addr = ring_push(&x->cmd, param, status, type, flags | ((uint32_t)slot << 24));
    if (!addr) return -1;
    ring_doorbell(x, 0, 0);
    uint32_t code = 0; int got_slot = 0;
    if (!evt_wait(x, TRB_CMD_COMPLETE, addr, 1000, &code, &got_slot, NULL)) {
        klog("xhci: command %u timed out", type);
        usb_diag("xHCI cmd %u timeout", type);
        return -1;
    }
    if (code_out) *code_out = code;
    if (slot_out) *slot_out = got_slot;
    return code == CC_SUCCESS ? 0 : -1;
}

static int xhci_address(xhci_t *x, xdev_t *d, int addr, bool bsr, int maxpkt)
{
    uint32_t *in = ctx_alloc(x, 32);
    if (!in) return -1;
    in[1] = 0x3;                                             /* add slot and EP0 */
    fill_slot(x, ictx_slot(x, in), d, 1, addr);
    fill_ep(x, ictx_ep(x, in, 1), EP_CTL, ring_dequeue(&d->ep0), maxpkt, 0);
    uint32_t code = 0;
    int r = xhci_cmd(x, d->slot, (uint64_t)(uintptr_t)in, 0, TRB_ADDRESS, bsr ? TRB_BSR : 0, &code, NULL);
    kfree_aligned(in);
    if (r < 0) { klog("xhci: address device failed (code %u, slot %d)", code, d->slot); return -1; }
    return 0;
}

static int xhci_configure(xhci_t *x, xdev_t *d, int epnum, bool in, int maxpkt, int eptype)
{
    int dci = epnum * 2 + (in ? 1 : 0);
    ring_init(&d->ep[epnum].ring, 32, true);
    if (!d->ep[epnum].ring.n) return -1;
    uint32_t *inctx = ctx_alloc(x, 32);
    if (!inctx) return -1;
    inctx[1] = 0x1 | (1u << dci);
    int entries = d->max_dci > dci ? d->max_dci : dci;
    fill_slot(x, ictx_slot(x, inctx), d, entries, d->addr);
    fill_ep(x, ictx_ep(x, inctx, dci), eptype, ring_dequeue(&d->ep[epnum].ring), maxpkt, 6);
    uint32_t code = 0;
    int r = xhci_cmd(x, d->slot, (uint64_t)(uintptr_t)inctx, 0, TRB_CONFIGURE, 0, &code, NULL);
    kfree_aligned(inctx);
    if (r < 0) { klog("xhci: configure endpoint %d failed (code %u)", epnum, code); return -1; }
    d->max_dci = entries;
    d->ep[epnum].configured = true;
    d->ep[epnum].maxpkt = maxpkt;
    d->ep[epnum].dci = dci;
    return 0;
}

/* ---------------- transfers ---------------- */
static xdev_t *dev_by_addr(xhci_t *x, int addr)
{
    if (addr == 0) return x->pending;
    for (xdev_t *d = x->devs; d; d = d->next) if (!d->bsr && d->addr == addr) return d;
    return NULL;
}

static int xhci_run_trb(xhci_t *x, xdev_t *d, int epnum, int dci, uint64_t param, uint32_t status,
                        bool in, int timeout_ms, uint32_t *left)
{
    uint64_t addr = ring_push(&d->ep[epnum].ring, param, status, TRB_NORMAL, TRB_IOC | (in ? TRB_DIR : 0));
    if (!addr) return -1;
    ring_doorbell(x, d->slot, dci);
    uint32_t code = 0;
    if (!evt_wait(x, TRB_TRANSFER, addr, timeout_ms, &code, NULL, left)) return -2;
    if (code != CC_SUCCESS && code != CC_SHORT) {
        klog("xhci: transfer on slot %d dci %d failed (code %u)", d->slot, dci, code);
        return -1;
    }
    return 0;
}

static int xhci_control(usb_hcd_t *h, int addr, bool low, uint8_t type, uint8_t req,
                        uint16_t value, uint16_t index, void *buf, int len)
{
    (void)low;
    xhci_t *x = h->priv;
    xdev_t *d = dev_by_addr(x, addr);
    if (!d) return -1;

    if (type == 0x00 && req == 5) {                          /* SET_ADDRESS: the slot does it */
        if (xhci_address(x, d, (int)value, false, d->ep0_maxpkt ? d->ep0_maxpkt : 8) < 0) return -1;
        d->addr = (int)value;
        d->bsr = false;
        if (x->pending == d) x->pending = NULL;
        d->next = x->devs; x->devs = d;
        return 0;
    }

    if (len > 4096) return -1;
    uint8_t *setup = x->buf;
    setup[0] = type; setup[1] = req;
    setup[2] = (uint8_t)value; setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index; setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)len;   setup[7] = (uint8_t)(len >> 8);
    bool in = (type & 0x80) != 0;
    uint8_t *data = x->buf + 64;
    if (!in && len) memcpy(data, buf, len);

    uint64_t setup_val; memcpy(&setup_val, setup, 8);
    uint64_t first = ring_push(&d->ep0, setup_val, 8, TRB_SETUP, TRB_IDT);
    if (!first) return -1;
    if (len && !ring_push(&d->ep0, (uint64_t)(uintptr_t)data, (uint32_t)len, TRB_DATA, in ? TRB_DIR : 0)) return -1;
    /* the status stage runs the other way from the data stage, and interrupts */
    uint64_t last = ring_push(&d->ep0, 0, 0, TRB_STATUS, TRB_IOC | (in ? 0 : TRB_DIR));
    if (!last) return -1;
    ring_doorbell(x, d->slot, 1);

    uint32_t code = 0, left = 0;
    if (!evt_wait(x, TRB_TRANSFER, last, 2000, &code, NULL, &left)) {
        klog("xhci: control %02x/%02x on slot %d timed out", type, req, d->slot);
        usb_diag("xHCI ctl %02x/%02x timeout", type, req);
        return -1;
    }
    if (code != CC_SUCCESS && code != CC_SHORT) {
        klog("xhci: control %02x/%02x on slot %d failed (code %u)", type, req, d->slot, code);
        usb_diag("xHCI ctl %02x/%02x code %u", type, req, code);
        return -1;
    }
    int got = len - (int)left;
    if (got < 0) got = 0;
    if (in && len) memcpy(buf, data, got);
    if (in && req == 6 && value == 0x0100 && got >= 8) {
        uint8_t m = ((const uint8_t *)buf)[7];
        /* SuperSpeed descriptors give an exponent (9 = 512); USB 2 gives bytes */
        d->ep0_maxpkt = d->speed >= 4 ? (m >= 1 && m <= 12 ? 1 << m : 512) : m;
    }
    return in ? got : len;
}

/* An interrupt IN stays queued in the controller until the device answers, so
 * arm it once and collect the completion on a later poll. */
static int xhci_poll_in(xhci_t *x, xdev_t *d, int epnum, void *buf, int len)
{
    int dci = epnum * 2 + 1;
    uint32_t status, control;
    if (d->ep[epnum].armed && take_event(x, TRB_TRANSFER, d->ep[epnum].armed, &status, &control)) {
        if (TRB_EPC(control) == dci) {
            uint32_t code = TRB_CODE(status);
            int got = d->ep[epnum].armed_len - (int)(status & TRB_LEN);
            d->ep[epnum].armed = 0;
            if (code != CC_SUCCESS && code != CC_SHORT) return -1;
            if (got < 0) got = 0;
            if (got > len) got = len;
            if (got) memcpy(buf, x->buf, got);
            return got;
        }
    }
    if (!d->ep[epnum].armed) {
        uint64_t trb = ring_push(&d->ep[epnum].ring, (uint64_t)(uintptr_t)x->buf, (uint32_t)len,
                                 TRB_NORMAL, TRB_IOC | TRB_DIR);
        if (!trb) return -1;
        ring_doorbell(x, d->slot, dci);
        d->ep[epnum].armed = trb;
        d->ep[epnum].armed_len = len;
    }
    return -2;
}

static int xhci_transfer(usb_hcd_t *h, int addr, bool low, int ep, bool in,
                         uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms)
{
    (void)low; (void)toggle;
    xhci_t *x = h->priv;
    xdev_t *d = dev_by_addr(x, addr);
    if (!d || !ep || ep > 15) return -1;
    if (len > 4096) len = 4096;
    bool poll = timeout_ms <= 100;
    if (!d->ep[ep].configured) {
        int eptype = poll ? EP_INT_IN : (in ? EP_BULK_IN : EP_BULK_OUT);
        if (xhci_configure(x, d, ep, in, maxpkt, eptype) < 0) return -1;
    }
    if (poll && in) return xhci_poll_in(x, d, ep, buf, len);
    if (!in) memcpy(x->buf, buf, len);
    uint32_t left = 0;
    int r = xhci_run_trb(x, d, ep, ep * 2 + (in ? 1 : 0), (uint64_t)(uintptr_t)x->buf,
                         (uint32_t)len, in, timeout_ms, &left);
    if (r == -2) return -2;
    if (r < 0) return -1;
    int got = len - (int)left;
    if (got < 0) got = 0;
    if (in && got) memcpy(buf, x->buf, got);
    return got;
}

/* ---------------- ports ---------------- */
static bool xhci_port_reset(usb_hcd_t *h, int port, bool *low_speed)
{
    xhci_t *x = h->priv;
    if (port < 0 || port >= x->max_ports) return false;
    uint32_t v = r32(x, x->op + PORTSC(port));
    if (!(v & PORT_CCS)) return false;
    if (!(v & PORT_PP)) {
        /* PED is write-one-to-clear: never write a set PED back */
        w32(x, x->op + PORTSC(port), (v & ~(PORT_PED | PORT_CHANGE)) | PORT_PP);
        uwait(100);
        v = r32(x, x->op + PORTSC(port));
    }
    w32(x, x->op + PORTSC(port), (v & ~(PORT_PED | PORT_CHANGE)) | PORT_PR);
    uint32_t t0 = timer_ticks();
    while (!(r32(x, x->op + PORTSC(port)) & PORT_PRC) && (int32_t)(timer_ticks() - t0) < 200) task_yield();
    v = r32(x, x->op + PORTSC(port));
    if (!(v & PORT_PRC)) { klog("xhci: port %d reset did not finish", port + 1); usb_diag("xHCI p%d no PRC", port + 1); return false; }
    w32(x, x->op + PORTSC(port), v & ~(PORT_PED | PORT_PR)); /* clear change bits, PED and reset */
    v = r32(x, x->op + PORTSC(port));
    if (!(v & PORT_CCS)) return false;
    t0 = timer_ticks();
    while (!(v & PORT_PED) && (int32_t)(timer_ticks() - t0) < 100) { task_yield(); v = r32(x, x->op + PORTSC(port)); }
    if (!(v & PORT_PED)) { klog("xhci: port %d did not enable", port + 1); usb_diag("xHCI p%d no PED", port + 1); return false; }
    x->port_speed[port] = (uint8_t)((v >> 10) & 0xF);
    w32(x, x->op + PORTSC(port), v & ~(PORT_PED | PORT_PR)); /* clear CSC/PEC/PRC */
    *low_speed = x->port_speed[port] == 2;
    return true;
}

/* The core found a device: give it a slot, and let endpoint 0 answer at address 0. */
static bool xhci_new_device(usb_hcd_t *h, int root_port, int route, bool low)
{
    xhci_t *x = h->priv;
    if (root_port < 1 || root_port > x->max_ports) return false;
    uint32_t code = 0; int slot = 0;
    if (xhci_cmd(x, 0, 0, 0, TRB_ENABLE_SLOT, 0, &code, &slot) < 0 || !slot) {
        klog("xhci: no free slot (code %u)", code);
        return false;
    }
    uint32_t *devctx = ctx_alloc(x, 32);
    if (!devctx) return false;
    x->dcbaa[slot] = (uint64_t)(uintptr_t)devctx;
    xdev_t *d = kzalloc(sizeof *d);
    if (!d) return false;
    d->slot = slot;
    d->root_port = root_port;
    d->route = route;
    d->speed = route ? (low ? 2 : 1) : (x->port_speed[root_port - 1] ? x->port_speed[root_port - 1] : 1);
    ring_init(&d->ep0, 32, true);
    if (!d->ep0.n) { kfree(d); return false; }
    x->pending = d;
    int ep0 = d->speed >= 4 ? 512 : d->speed == 3 ? 64 : 8;
    if (xhci_address(x, d, 0, true, ep0) < 0) {
        klog("xhci: slot %d could not be addressed", slot);
        usb_diag("xHCI slot %d address failed", slot);
        x->pending = NULL;
        return false;
    }
    d->bsr = true;
    return true;
}

/* ---------------- controller ---------------- */
/* The firmware may still own the USB legacy support capability (and its SMI
 * handlers). Take it over before resetting the controller. */
static void xhci_handoff(xhci_t *x, uint32_t hcc1)
{
    uint32_t off = (hcc1 >> 16) & 0xFFFF;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t cap = r32(x, off);
        if ((cap & 0xFF) == 1) {                          /* USB Legacy Support */
            if (cap & (1u << 16)) {
                w32(x, off, (cap & ~0xE0u) | (1u << 17));
                uint32_t t0 = timer_ticks();
                while ((r32(x, off) & (1u << 16)) && (int32_t)(timer_ticks() - t0) < 100)
                    task_yield();
            }
            w32(x, off + 4, 0);                           /* USBLEGCTLSTS: no SMI events */
            break;
        }
        off = (cap >> 8) & 0xFF;
    }
}

static int xhci_init(xhci_t *x, const pci_dev_t *pd)
{
    bool is_io = false;
    uintptr_t bar = pci_bar(pd, 0, &is_io);
    if (!bar || is_io) return -1;
    if (bar >= 0x100000000ull) { klog("xhci: MMIO at %llx is above the identity map", (unsigned long long)bar); return -1; }
    pci_enable(pd, false, true);
    paging_set_uncached(bar);
    x->mmio = (volatile uint32_t *)bar;
    x->op = (uint16_t)(*(volatile uint8_t *)bar);             /* CAPLENGTH */
    uint32_t hcs1 = r32(x, HCSPARAMS1);
    uint32_t hcc1 = r32(x, HCCPARAMS1);
    x->max_slots = (int)(hcs1 & 0xFF);
    x->max_ports = (int)((hcs1 >> 24) & 0xFF);
    if (x->max_ports > 32) x->max_ports = 32;
    x->ctx = (hcc1 & (1u << 2)) ? 64 : 32;
    x->dboff = r32(x, DBOFF) & ~3u;
    x->rtsoff = r32(x, RTSOFF) & ~31u;
    if (!x->max_slots || !x->max_ports) return -1;

    xhci_handoff(x, hcc1);
    w32(x, x->op + USBCMD, CMD_HCRST);
    uint32_t t0 = timer_ticks();
    while ((r32(x, x->op + USBCMD) & CMD_HCRST) && (int32_t)(timer_ticks() - t0) < 500) task_yield();
    t0 = timer_ticks();
    while ((r32(x, x->op + USBSTS) & STS_CNR) && (int32_t)(timer_ticks() - t0) < 500) task_yield();
    w32(x, x->op + PAGESIZE, 1);                              /* 4 KiB pages */

    ring_init(&x->cmd, 64, true);
    ring_init(&x->evt, 64, false);
    x->erst = kmalloc_aligned(64, 64);
    x->dcbaa = kmalloc_aligned((size_t)(x->max_slots + 1) * 8, 64);
    x->buf = kmalloc_aligned(4096, 4096);
    if (!x->cmd.trb || !x->evt.trb || !x->erst || !x->dcbaa || !x->buf) return -1;
    memset(x->erst, 0, 64);
    x->erst[0].param = x->evt.phys;
    x->erst[0].status = 64;                                   /* one segment of 64 events */
    memset(x->dcbaa, 0, (size_t)(x->max_slots + 1) * 8);

    /* Some controllers require scratchpad buffers before they will run.
     * DCBAA[0] points at the array of page pointers. */
    uint32_t hcs2 = r32(x, HCSPARAMS2);
    int nsp = (int)((((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F));
    if (nsp > 0) {
        uint64_t *sp = kmalloc_aligned((size_t)nsp * 8, 64);
        if (!sp) { klog("usb: xHCI needs %d scratchpad buffers and there is no memory", nsp); return -1; }
        for (int i = 0; i < nsp; i++) {
            void *p = kmalloc_aligned(4096, 4096);
            if (!p) { klog("usb: xHCI scratchpad allocation failed"); return -1; }
            memset(p, 0, 4096);
            sp[i] = (uint64_t)(uintptr_t)p;
        }
        x->dcbaa[0] = (uint64_t)(uintptr_t)sp;
        klog("usb: xHCI %d scratchpad buffers", nsp);
    }

    w64(x, x->op + CRCR, x->cmd.phys | 1);                    /* ring cycle state */
    w64(x, x->op + DCBAAP, (uint64_t)(uintptr_t)x->dcbaa);
    w32(x, x->op + CONFIG, (uint32_t)x->max_slots);
    w32(x, x->rtsoff + 0x28, 1);                              /* ERSTSZ */
    w64(x, x->rtsoff + 0x30, (uint64_t)(uintptr_t)x->erst);   /* ERSTBA */
    w64(x, x->rtsoff + 0x38, x->evt.phys | 8);                /* ERDP */
    w32(x, x->op + USBCMD, CMD_RUN);
    t0 = timer_ticks();
    while ((r32(x, x->op + USBSTS) & STS_HCH) && (int32_t)(timer_ticks() - t0) < 500) task_yield();

    if (hcc1 & (1u << 3)) {                                   /* ports have power control */
        for (int p = 0; p < x->max_ports; p++) {
            uint32_t v = r32(x, x->op + PORTSC(p));
            if (!(v & PORT_PP))
                w32(x, x->op + PORTSC(p), (v & ~(PORT_PED | PORT_CHANGE)) | PORT_PP);
        }
        uwait(100);
    }

    x->hcd.name = "xHCI";
    x->hcd.control = xhci_control;
    x->hcd.transfer = xhci_transfer;
    x->hcd.port_reset = xhci_port_reset;
    x->hcd.new_device = xhci_new_device;
    x->hcd.ports = x->max_ports;
    x->hcd.priv = x;
    return 0;
}

void xhci_probe(void)
{
    pci_dev_t *devs = NULL;
    int n = pci_list(&devs), found = 0;
    for (int i = 0; i < n; i++) {
        pci_dev_t *pd = &devs[i];
        if (!(pd->cls == 0x0C && pd->sub == 0x03 && pd->prog == 0x30)) continue;
        xhci_t *x = kzalloc(sizeof *x);
        if (!x) return;
        if (xhci_init(x, pd) < 0) {
            kfree(x);
            usb_note_controller(pd, 0, 0, 0);
            klog("usb: %04x:%04x xHCI controller did not start", pd->vendor, pd->device);
            continue;
        }
        found++;
        int conn = 0;
        for (int p = 0; p < x->max_ports; p++)
            if (r32(x, x->op + PORTSC(p)) & PORT_CCS) conn++;
        usb_note_controller(pd, 1, x->max_ports, conn);
        klog("usb: xHCI %04x:%04x at %08lx, %d ports (%d connected), %d slots, %d-byte contexts",
             pd->vendor, pd->device, (unsigned long)(uintptr_t)x->mmio, x->max_ports, conn,
             x->max_slots, x->ctx);
        usb_attach(&x->hcd);
    }
    if (!found) klog("usb: no xHCI controller found");
}
#endif
