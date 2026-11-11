/* EHCI: the USB 2.0 host controller, the companion of a UHCI or OHCI
 * controller on chipsets from about 2002 to 2012. It is the high-speed bus:
 * 480 Mbit/s devices live here, while full- and low-speed ones are left to the
 * companion controller (or, behind a high-speed hub, need split transactions,
 * which this driver does not do).
 *
 * Transfers are queue heads and queue element transfer descriptors in memory.
 * Control and bulk transfers go through the asynchronous schedule; interrupt
 * endpoints are polled by putting their queue head in the periodic frame list
 * for the duration of one poll, because the kernel has no interrupt handler
 * for USB and a poll is synchronous anyway.
 *
 * Layout follows the EHCI 1.0 specification: the capability registers start at
 * the BAR, the operational registers at CAPLENGTH (normally 0x20), and every
 * queue element is 32 bytes with a 32-byte-aligned address. */
#ifdef __x86_64__
#include "usb.h"

/* capability registers (absolute from the BAR) */
#define CAPLENGTH   0x00
#define HCSPARAMS   0x04
#define HCCPARAMS   0x08

/* operational registers (relative to CAPLENGTH) */
#define USBCMD      0x00
#define USBSTS      0x04
#define USBINTR     0x08
#define FRINDEX     0x0C
#define CTRLDSSEG    0x10
#define PERIODICBASE 0x14
#define ASYNCLIST    0x18
#define CONFIGFLAG   0x40
#define PORTSC(p)    (0x44 + (p) * 4)

#define CMD_RUN     (1u << 0)
#define CMD_RESET   (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define CMD_IAAD    (1u << 6)
#define STS_HALT    (1u << 12)
#define STS_PSS     (1u << 14)
#define STS_ASS     (1u << 15)
#define STS_IAA     (1u << 5)

#define PORT_CONNECT (1u << 0)
#define PORT_CSC     (1u << 1)
#define PORT_PE      (1u << 2)
#define PORT_PEC     (1u << 3)
#define PORT_OCC     (1u << 5)
#define PORT_RESET   (1u << 8)
#define PORT_POWER   (1u << 12)
#define PORT_OWNER   (1u << 13)
#define PORT_CHANGE  (PORT_CSC | PORT_PEC | PORT_OCC)

/* queue element transfer descriptor */
#define QTD_ACTIVE  (1u << 7)
#define QTD_HALT    (1u << 6)
#define QTD_DBE     (1u << 5)
#define QTD_BABBLE  (1u << 4)
#define QTD_XACT    (1u << 3)
#define QTD_ERRORS  (QTD_HALT | QTD_DBE | QTD_BABBLE | QTD_XACT)
#define QTD_LEN(t)  (((t) >> 16) & 0x7FFF)
#define QTD_TOGGLE  (1u << 31)
#define QTD_IOC     (1u << 15)
#define QTD_CERR    (3u << 10)

#define PID_OUT   0
#define PID_IN    1
#define PID_SETUP 2

#define QH_HEAD    (1u << 15)   /* this queue head starts the async list */
#define QH_DTC     (1u << 14)   /* data toggle comes from the qTD */
#define QH_HS      2u          /* endpoint speed field, shifted into info1 */
#define QH_FS      0u
#define QH_LS      1u

#define Q_NEXT_QH  2u           /* low bits of a queue head link */

typedef struct {
    uint32_t next, alt, token;
    uint32_t buf[5];
    uint32_t buf_hi[5];        /* 64-bit address high halves; QEMU/real HC read them */
    uint32_t pad[3];           /* keep each descriptor 64 bytes and 32-byte aligned */
} qtd_t;

typedef struct {
    uint32_t next;              /* link to the next queue head */
    uint32_t info1;             /* address, endpoint, speed, max packet */
    uint32_t info2;             /* split masks, hub, multiplier */
    uint32_t current;           /* current qTD (not used by the overlay) */
    uint32_t qtd_next;          /* overlay: next qTD */
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
    uint32_t buf_hi[5];
    uint32_t pad[7];
} qh_t;

typedef struct {
    usb_hcd_t hcd;
    volatile uint32_t *mmio;
    uint16_t op;
    int ports;
    uint32_t *periodic;
    qh_t *async_head;           /* head of the asynchronous reclamation list */
    qh_t *xfer;                 /* the queue head one transfer is built in */
    qh_t *intr;                 /* the periodic queue head */
    qtd_t *qtds;                /* control and bulk */
    qtd_t *iqtd;                /* interrupt polls */
    uint8_t *bounce;
    int nqtd;
    uint16_t qlen[64];          /* bytes asked of each qTD */
    uint16_t iqlen;
} ehci_t;

static uint32_t er32(ehci_t *e, int off) { return e->mmio[off / 4]; }
static void ew32(ehci_t *e, int off, uint32_t v) { e->mmio[off / 4] = v; }
static uint32_t eop(ehci_t *e, int off) { return er32(e, e->op + off); }
static void eopw(ehci_t *e, int off, uint32_t v) { ew32(e, e->op + off, v); }
static void uwait(uint32_t ms) { uint32_t t0 = timer_ticks(); while ((int32_t)(timer_ticks() - t0) < (int32_t)ms) task_yield(); }

static uint64_t phys(const void *p) { return (uint64_t)(uintptr_t)p; }

/* ---------------- queue elements ---------------- */
static void qtd_fill(qtd_t *q, int pid, int toggle, const void *buf, int len) {
    memset(q, 0, sizeof *q);
    q->next = 1;
    q->alt = 1;
    q->token = (uint32_t)toggle << 31 | (uint32_t)(len & 0x7FFF) << 16 |
               QTD_CERR | (uint32_t)pid << 8 | QTD_ACTIVE;
    uintptr_t p = (uintptr_t)buf;
    q->buf[0] = (uint32_t)p;
    q->buf[1] = (uint32_t)(p + 4096) & ~0xFFFu;
    q->buf[2] = (uint32_t)(p + 8192) & ~0xFFFu;
    q->buf[3] = (uint32_t)(p + 12288) & ~0xFFFu;
    q->buf[4] = (uint32_t)(p + 16384) & ~0xFFFu;
}

/* Chain the descriptors in a slot and point the head at them. */
static void qtd_chain(ehci_t *e, int first, int n) {
    for (int i = 0; i < n; i++)
        e->qtds[first + i].next = (i + 1 < n) ? (uint32_t)phys(&e->qtds[first + i + 1]) : 1;
}

static void qh_reset(qh_t *q, int addr, int ep, int speed, int maxpkt, int control) {
    memset(q, 0, sizeof *q);
    q->next = 1;
    q->info1 = (uint32_t)(addr & 0x7F) | (uint32_t)(ep & 0x0F) << 8 |
               (uint32_t)(speed & 3) << 12 | QH_DTC | (uint32_t)(maxpkt & 0x7FF) << 16 |
               (4u << 28);                               /* NAK reload count */
    q->info2 = 1u << 30;                                 /* one transaction per microframe */
    q->qtd_next = 1;
    q->alt_next = 1;
    q->token = QTD_ACTIVE;
}

/* ---------------- asynchronous transfers ---------------- */
static int ehci_wait_async(ehci_t *e, int timeout_ms) {
    uint32_t t0 = timer_ticks();
    for (;;) {
        uint32_t token = e->xfer->token;
        if (!(token & QTD_ACTIVE) || (token & QTD_ERRORS)) return 0;
        if ((int32_t)(timer_ticks() - t0) >= timeout_ms) return -2;
        task_yield();
    }
}

/* Run the qTDs in e->qtds (already chained) through the async schedule.
 * Returns 0 when they completed, -2 on a timeout, -1 on a bus error. */
static int ehci_run_async(ehci_t *e, int first, int n, int timeout_ms) {
    if (!n) return 0;
    e->async_head->next = (uint32_t)phys(e->xfer) | Q_NEXT_QH;
    e->xfer->next = (uint32_t)phys(e->async_head) | Q_NEXT_QH;
    e->xfer->qtd_next = n ? (uint32_t)phys(&e->qtds[first]) : 1;
    e->xfer->alt_next = 1;
    e->xfer->token = QTD_ACTIVE;
    memset(e->xfer->buf, 0, sizeof e->xfer->buf);

    uint32_t cmd = eop(e, USBCMD);
    eopw(e, USBCMD, cmd | CMD_ASE | CMD_RUN);
    uint32_t t0 = timer_ticks();
    while (!(eop(e, USBSTS) & STS_ASS) && (int32_t)(timer_ticks() - t0) < 20) task_yield();
    int r = ehci_wait_async(e, timeout_ms);
    if (r < 0)
        klog("ehci: async timeout cmd %08x sts %08x qh %08x tok %08x qtd %08x tok %08x",
             eop(e, USBCMD), eop(e, USBSTS), e->xfer->info1, e->xfer->token,
             e->qtds[first].token, e->qtds[first + n - 1].token);
    /* Take the queue head out of the list and ring the doorbell while the
     * schedule is still running: that is the only time the controller answers
     * it, and the answer means it has let go of the queue head, so the next
     * transfer can reuse it. Rung with the schedule off, the doorbell stays
     * pending and goes off in the middle of the next transfer instead. */
    e->async_head->next = (uint32_t)phys(e->async_head) | Q_NEXT_QH;
    eopw(e, USBCMD, eop(e, USBCMD) | CMD_IAAD);
    t0 = timer_ticks();
    while (!(eop(e, USBSTS) & STS_IAA) && (int32_t)(timer_ticks() - t0) < 20) task_yield();
    eopw(e, USBSTS, STS_IAA);
    eopw(e, USBCMD, eop(e, USBCMD) & ~(CMD_ASE | CMD_IAAD));
    t0 = timer_ticks();
    while ((eop(e, USBSTS) & STS_ASS) && (int32_t)(timer_ticks() - t0) < 20) task_yield();
    e->xfer->next = 1;
    e->async_head->next = (uint32_t)phys(e->xfer) | Q_NEXT_QH;
    if (r) return r;
    uint32_t tok = e->xfer->token;
    if (tok & QTD_ERRORS) {
        klog("ehci: transfer halted (token %08x)", tok);
        return -1;
    }
    return 0;
}

/* Bytes the descriptor actually moved: the token's remaining length is the
 * part that did not fit. The last descriptor may still be in the overlay. */
static int qtd_actual(const qtd_t *q, int requested, uint32_t overlay) {
    uint32_t tok = q->token & QTD_ACTIVE ? overlay : q->token;
    int left = (int)QTD_LEN(tok);
    int got = requested - left;
    return got < 0 ? 0 : got;
}

static int ehci_control(usb_hcd_t *h, int addr, bool low, uint8_t type, uint8_t req,
                        uint16_t value, uint16_t index, void *buf, int len)
{
    ehci_t *e = h->priv;
    if (low) return -1;                                   /* split transactions are not implemented */
    if (len > 2048) len = 2048;
    uint8_t *data = e->bounce;
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    bool in = (type & 0x80) != 0;
    if (!in && len) memcpy(data, buf, len);
    qh_reset(e->xfer, addr, 0, QH_HS, 64, 1);

    int n = 0;
    qtd_fill(&e->qtds[n], PID_SETUP, 0, setup, 8); n++;
    int first_data = n, ndata = 0;
    int off = 0;
    while (off < len && n < e->nqtd - 1) {
        int chunk = len - off; if (chunk > 64) chunk = 64;
        qtd_fill(&e->qtds[n], in ? PID_IN : PID_OUT, 1 ^ ((off / 64) & 1), data + off, chunk);
        e->qlen[n] = (uint16_t)chunk;
        off += chunk; n++; ndata++;
    }
    len = off;
    qtd_fill(&e->qtds[n], in ? PID_OUT : PID_IN, 1, NULL, 0); n++;
    qtd_chain(e, 0, n);

    int r = ehci_run_async(e, 0, n, 2000);
    if (r < 0) return -1;
    int got = 0;
    for (int i = first_data; i < first_data + ndata; i++)
        got += qtd_actual(&e->qtds[i], e->qlen[i], e->xfer->token);
    if (in && len) {
        if (got > len) got = len;
        memcpy(buf, data, got);
    }
    return in ? got : len;
}

static int ehci_transfer(usb_hcd_t *h, int addr, bool low, int ep, bool in,
                         uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms)
{
    ehci_t *e = h->priv;
    if (low || ep < 1 || ep > 15) return -1;
    if (!len) return 0;
    if (maxpkt > 1024) maxpkt = 1024;
    bool poll = timeout_ms <= 100;
    if (poll) {
        qh_reset(e->intr, addr, ep, QH_HS, maxpkt, 0);
        e->intr->info2 = (1u << 30) | (1u << (ep & 7));
        e->iqlen = (uint16_t)(len > maxpkt ? maxpkt : len);
        qtd_fill(&e->iqtd[0], in ? PID_IN : PID_OUT, *toggle, buf, e->iqlen);
        e->iqtd[0].next = 1;
        e->intr->qtd_next = (uint32_t)phys(&e->iqtd[0]);
        e->intr->alt_next = 1;
        e->intr->token = QTD_ACTIVE;
        memset(e->intr->buf, 0, sizeof e->intr->buf);
        for (int i = 0; i < 1024; i++) e->periodic[i] = (uint32_t)phys(e->intr) | Q_NEXT_QH;
        uint32_t cmd = eop(e, USBCMD);
        eopw(e, USBCMD, cmd | CMD_PSE | CMD_RUN);
        uint32_t t0 = timer_ticks();
        while (!(eop(e, USBSTS) & STS_PSS) && (int32_t)(timer_ticks() - t0) < 20) task_yield();
        t0 = timer_ticks();
        int done = 0;
        while ((int32_t)(timer_ticks() - t0) < timeout_ms) {
            uint32_t tok = e->intr->token;
            if (!(tok & QTD_ACTIVE)) { done = 1; if (!(tok & QTD_ERRORS)) break; }
            if (tok & QTD_ERRORS) { done = -1; break; }
            task_yield();
        }
        eopw(e, USBCMD, eop(e, USBCMD) & ~CMD_PSE);
        t0 = timer_ticks();
        while ((eop(e, USBSTS) & STS_PSS) && (int32_t)(timer_ticks() - t0) < 20) task_yield();
        for (int i = 0; i < 1024; i++) e->periodic[i] = 1;
        if (!done) {
            /* a report that arrived while the schedule was stopping was still
             * answered, so it has to be kept */
            uint32_t tok = e->intr->token;
            if (!(tok & QTD_ACTIVE)) done = (tok & QTD_ERRORS) ? -1 : 1;
        }
        if (done <= 0) return done < 0 ? -1 : -2;
        int got = qtd_actual(&e->iqtd[0], e->iqlen, e->intr->token);
        if (got > len) got = len;
        *toggle ^= 1;
        return got;
    }

    int cap = e->nqtd;
    if (len > maxpkt * (cap - 1)) len = maxpkt * (cap - 1);
    if (len > 16384) len = 16384;
    qh_reset(e->xfer, addr, ep, QH_HS, maxpkt, 0);
    int n = 0, off = 0, pkts = 0;
    while (off < len && n < cap) {
        int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
        qtd_fill(&e->qtds[n], in ? PID_IN : PID_OUT, *toggle ^ (pkts & 1), (uint8_t *)buf + off, chunk);
        e->qlen[n] = (uint16_t)chunk;
        off += chunk; n++; pkts++;
    }
    qtd_chain(e, 0, n);
    int r = ehci_run_async(e, 0, n, timeout_ms);
    if (r) return r;
    int got = 0;
    for (int i = 0; i < n; i++) got += qtd_actual(&e->qtds[i], e->qlen[i], e->xfer->token);
    if (in && got > len) got = len;
    *toggle ^= (pkts & 1);
    return in ? got : len;
}

/* ---------------- root ports ---------------- */
static bool ehci_port_reset(usb_hcd_t *h, int port, bool *low_speed)
{
    ehci_t *e = h->priv;
    if (port < 0 || port >= e->ports) return false;
    uint32_t v = eop(e, PORTSC(port));
    if (!(v & PORT_CONNECT)) return false;
    if (!(v & PORT_POWER)) {
        eopw(e, PORTSC(port), v | PORT_POWER);
        uwait(20);
        v = eop(e, PORTSC(port));
    }
    eopw(e, PORTSC(port), (v & ~PORT_CHANGE) | PORT_RESET);
    uwait(50);                                           /* the reset bit is cleared by software */
    v = eop(e, PORTSC(port));
    eopw(e, PORTSC(port), (v & ~PORT_RESET) | PORT_CHANGE);
    uwait(10);
    v = eop(e, PORTSC(port));
    if (!(v & PORT_PE)) {
        /* a full- or low-speed device: hand the port to the companion */
        eopw(e, PORTSC(port), v | PORT_OWNER | PORT_CHANGE);
        return false;
    }
    eopw(e, PORTSC(port), v | PORT_CHANGE);
    *low_speed = false;                                  /* enabled means high speed */
    return true;
}

static int ehci_init(ehci_t *e, const pci_dev_t *pd)
{
    bool is_io = false;
    uintptr_t bar = pci_bar(pd, 0, &is_io);
    if (!bar || is_io) return -1;
    if (bar >= 0x100000000ull) { klog("ehci: MMIO above the identity map"); return -1; }
    pci_enable(pd, false, true);
    paging_set_uncached(bar);
    e->mmio = (volatile uint32_t *)bar;
    e->op = (uint16_t)(*(volatile uint8_t *)bar);
    uint32_t hcs = er32(e, HCSPARAMS);
    e->ports = (int)(hcs & 0xF);
    if (e->ports > 15) e->ports = 15;
    if (!e->ports) return -1;

    eopw(e, USBCMD, CMD_RESET | CMD_RUN);
    uint32_t t0 = timer_ticks();
    while ((eop(e, USBCMD) & CMD_RESET) && (int32_t)(timer_ticks() - t0) < 500) task_yield();
    t0 = timer_ticks();
    while ((eop(e, USBSTS) & STS_HALT) && (int32_t)(timer_ticks() - t0) < 500) task_yield();

    e->periodic = kmalloc_aligned(4096, 4096);
    e->async_head = kmalloc_aligned(sizeof *e->async_head, 64);
    e->xfer = kmalloc_aligned(sizeof *e->xfer, 64);
    e->intr = kmalloc_aligned(sizeof *e->intr, 64);
    e->nqtd = 64;
    e->qtds = kmalloc_aligned((size_t)e->nqtd * sizeof *e->qtds, 64);
    e->iqtd = kmalloc_aligned(4 * sizeof *e->iqtd, 64);
    e->bounce = kmalloc_aligned(4096, 4096);
    if (!e->periodic || !e->async_head || !e->xfer || !e->intr || !e->qtds || !e->iqtd || !e->bounce)
        return -1;
    memset(e->periodic, 0, 4096);
    for (int i = 0; i < 1024; i++) e->periodic[i] = 1;
    memset(e->qtds, 0, (size_t)e->nqtd * sizeof *e->qtds);
    memset(e->iqtd, 0, 4 * sizeof *e->iqtd);
    memset(e->bounce, 0, 4096);

    memset(e->async_head, 0, sizeof *e->async_head);
    e->async_head->info1 = QH_HEAD;
    e->async_head->next = (uint32_t)phys(e->xfer) | Q_NEXT_QH;
    e->async_head->current = 1;
    e->async_head->qtd_next = 1;                          /* the head owns no qTDs */
    e->async_head->alt_next = 1;
    e->async_head->token = 0;
    qh_reset(e->xfer, 0, 0, QH_HS, 64, 1);
    e->xfer->next = (uint32_t)phys(e->async_head) | Q_NEXT_QH;

    eopw(e, CTRLDSSEG, 0);
    eopw(e, PERIODICBASE, (uint32_t)phys(e->periodic));
    eopw(e, ASYNCLIST, (uint32_t)phys(e->async_head));
    eopw(e, CONFIGFLAG, 1);                                /* ports belong to this controller */
    eopw(e, USBCMD, CMD_RUN);
    t0 = timer_ticks();
    while ((eop(e, USBSTS) & STS_HALT) && (int32_t)(timer_ticks() - t0) < 500) task_yield();
    eopw(e, USBINTR, 0);

    if (hcs & (1u << 4)) {                                 /* per-port power switching */
        for (int p = 0; p < e->ports; p++) {
            uint32_t v = eop(e, PORTSC(p));
            if (!(v & PORT_POWER)) eopw(e, PORTSC(p), v | PORT_POWER);
        }
        uwait(50);
    }

    e->hcd.name = "EHCI";
    e->hcd.control = ehci_control;
    e->hcd.transfer = ehci_transfer;
    e->hcd.port_reset = ehci_port_reset;
    e->hcd.ports = e->ports;
    e->hcd.priv = e;
    return 0;
}

void ehci_probe(void)
{
    pci_dev_t *devs = NULL;
    int n = pci_list(&devs), found = 0;
    for (int i = 0; i < n; i++) {
        pci_dev_t *pd = &devs[i];
        if (!(pd->cls == 0x0C && pd->sub == 0x03 && pd->prog == 0x20)) continue;
        ehci_t *e = kzalloc(sizeof *e);
        if (!e) return;
        if (ehci_init(e, pd) < 0) {
            kfree(e);
            klog("usb: %04x:%04x EHCI controller did not start", pd->vendor, pd->device);
            continue;
        }
        found++;
        klog("usb: EHCI %04x:%04x at %08lx, %d ports", pd->vendor, pd->device,
             (unsigned long)(uintptr_t)e->mmio, e->ports);
        usb_attach(&e->hcd);
    }
    if (!found) klog("usb: no EHCI controller found");
}
#endif
