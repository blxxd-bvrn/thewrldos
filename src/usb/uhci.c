/* UHCI: Intel's Universal Host Controller Interface, the USB 1.1 controller on
 * PIIX/ICH chipsets and in QEMU. A 1024-entry frame list points at one queue
 * head; every transfer is a chain of transfer descriptors hung off that queue
 * head, and the driver polls them until they finish.
 *
 * The register and descriptor bits follow Intel's UHCI 1.1D document: the TD
 * status dword keeps the actual length in bits 10..0 and the flags from bit 17
 * up, and the token's length field counts one less than the packet (0x7FF for a
 * zero-length packet). */
#ifdef __x86_64__
#include "usb.h"
#include "devmgr.h"

#define UHCI_CMD    0x00
#define UHCI_STS    0x02
#define UHCI_INTR   0x04
#define UHCI_FRNUM  0x06
#define UHCI_FLBASE 0x08
#define UHCI_SOF    0x0C
#define UHCI_PORT   0x10

#define CMD_RS      0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF      0x0040
#define STS_MASK    0x003F
#define STS_HCHALTED 0x0020
#define PORT_CCS    0x0001
#define PORT_CSC    0x0002
#define PORT_PED    0x0004
#define PORT_PEDC   0x0008
#define PORT_LSDA   0x0100
#define PORT_PR     0x0200

#define TD_SPD      0x20000000u
#define TD_ACTIVE   0x00800000u
#define TD_STALLED  0x00400000u
#define TD_DBUFERR  0x00200000u
#define TD_BABBLE   0x00100000u
#define TD_NAK      0x00080000u
#define TD_CRCTO    0x00040000u
#define TD_BITSTUFF 0x00020000u
#define TD_ACTLEN   0x000007FFu
#define TD_ERRORS   (TD_STALLED | TD_DBUFERR | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define TD_MAX 32

typedef struct { uint32_t link, status, token, buffer[5]; } td_t;
typedef struct { uint32_t link, element, reserved[2]; } qh_t;

typedef struct {
    usb_hcd_t hcd;
    uint16_t io;
    uint32_t *frames;
    qh_t *qh;
    td_t *tds;
    uint8_t *bounce;
    int rot;                                   /* where the next interrupt poll starts */
} uhci_t;

#define TD_CTL_FIRST 0                        /* control and bulk transfers */
#define TD_CTL_COUNT 16
#define TD_INT_FIRST 16                       /* interrupt polls, rotated so a NAKed
                                               * descriptor is not reused while the
                                               * controller may still hold it */
#define TD_INT_COUNT 16

static uhci_t *the_uhci;

static uint16_t ureg16(uhci_t *u, int r) { return inw(u->io + r); }
static void uwreg16(uhci_t *u, int r, uint16_t v) { outw(u->io + r, v); }
static void uwreg32(uhci_t *u, int r, uint32_t v) { outw(u->io + r, (uint16_t)v); outw(u->io + r + 2, (uint16_t)(v >> 16)); }

static void td_fill(td_t *td, int pid, int addr, int ep, int toggle, int len, const void *buf)
{
    int mlen = len ? (len - 1) & 0x7FF : 0x7FF;          /* the token counts one less */
    td->link = 1;                                        /* last in the chain */
    td->status = TD_ACTIVE;
    td->token = (uint32_t)pid | ((uint32_t)(addr & 0x7F) << 8) | ((uint32_t)(ep & 0x0F) << 15) |
                ((uint32_t)(toggle & 1) << 19) | ((uint32_t)mlen << 21);
    uintptr_t p = (uintptr_t)buf;
    td->buffer[0] = (uint32_t)p;
    td->buffer[1] = (uint32_t)(p + 4096) & ~0xFFFu;       /* UHCI can span four more pages */
    td->buffer[2] = td->buffer[1] + 4096;
    td->buffer[3] = td->buffer[1] + 8192;
    td->buffer[4] = td->buffer[1] + 12288;
}

/* Point each descriptor at the next one; the last one terminates the chain. */
static void uhci_link(uhci_t *u, int base, int n)
{
    for (int i = 0; i < n; i++)
        u->tds[base + i].link = (i + 1 < n) ? (uint32_t)(uintptr_t)&u->tds[base + i + 1] : 1;
}

/* Wait for the chain. A short packet ends the transfer early (the controller
 * stops at that descriptor), so a descriptor marked SPD that finished while
 * everything before it finished means the transfer is over. Returns the bytes
 * the device moved, -2 if nothing happened in timeout_ms, -1 on an error. */
static bool uhci_chain_done(uhci_t *u, int base, int ntds)
{
    bool active = false;
    for (int i = 0; i < ntds; i++) if (u->tds[base + i].status & TD_ACTIVE) active = true;
    if (!active) return true;
    for (int i = ntds - 1; i >= 0; i--) {                /* a finished SPD descriptor ends it */
        if ((u->tds[base + i].status & (TD_ACTIVE | TD_SPD)) != TD_SPD) continue;
        bool prev = true;
        for (int j = 0; j < i; j++) if (u->tds[base + j].status & TD_ACTIVE) prev = false;
        if (prev) return true;
    }
    return false;
}

static int uhci_run(uhci_t *u, int base, int ntds, int timeout_ms)
{
    u->qh->element = (uint32_t)(uintptr_t)&u->tds[base];
    uint32_t t0 = timer_ticks();
    bool done = false;
    for (;;) {
        if (uhci_chain_done(u, base, ntds)) { done = true; break; }
        if ((int32_t)(timer_ticks() - t0) >= timeout_ms) break;
        task_yield();                    /* the frame list does the work, not us */
    }
    u->qh->element = 1;
    if (!done) {
        /* The controller may have been in the middle of the transfer when it
         * was taken away: the device was answered, so the answer must not be
         * thrown away. Give that frame time to finish and look once more. */
        t0 = timer_ticks();
        while ((int32_t)(timer_ticks() - t0) < 2) task_yield();
        done = uhci_chain_done(u, base, ntds);
    }
    uwreg16(u, UHCI_STS, STS_MASK);
    if (!done) return -2;

    int len = 0;
    for (int i = 0; i < ntds; i++) {
        td_t *td = &u->tds[base + i];
        if (td->status & TD_ACTIVE) continue;            /* never reached */
        if (td->status & TD_ERRORS) {
            /* A device that has just been unplugged fails every poll until the
             * rescan notices; say so once in a while, not on every frame. */
            static uint32_t last;
            uint32_t now = timer_ticks();
            if (!last || (int32_t)(now - last) >= 2000) {
                klog("uhci: TD %d failed (status %08x, token %08x)", base + i, td->status, td->token);
                last = now;
            }
            return -1;
        }
        len += (td->status & TD_ACTLEN) + 1;
    }
    return len;
}

static int uhci_control(usb_hcd_t *h, int addr, bool low, uint8_t type, uint8_t req,
                        uint16_t value, uint16_t index, void *buf, int len)
{
    uhci_t *u = h->priv;
    uint8_t setup[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8),
                         (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    bool in = (type & 0x80) != 0;
    int maxpkt = low ? 8 : 64;
    uint8_t *data = u->bounce + 8;                       /* leave room for the setup packet */
    memcpy(u->bounce, setup, 8);
    if (!in && len) memcpy(data, buf, len);

    int n = 0, first = 0, ndata = 0;
    const int base = TD_CTL_FIRST;
    if (len > 512) len = 512;                            /* descriptors fit in the fixed slots */
    td_fill(&u->tds[base + n++], PID_SETUP, addr, 0, 0, 8, u->bounce);
    if (len) {
        first = n;
        int off = 0;
        while (off < len && n < TD_CTL_COUNT - 1) {
            int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
            td_fill(&u->tds[base + n], in ? PID_IN : PID_OUT, addr, 0, 1 ^ ((off / maxpkt) & 1), chunk, data + off);
            u->tds[base + n].status |= (off + chunk >= len) ? TD_SPD : 0;   /* the last one may come up short */
            off += chunk; n++; ndata++;
        }
        len = off;
    }
    td_fill(&u->tds[base + n++], in ? PID_OUT : PID_IN, addr, 0, 1, 0, u->bounce);
    uhci_link(u, base, n);

    int r = uhci_run(u, base, n, 2000);
    if (r < 0) return -1;
    int got = 0;
    for (int i = first; i < first + ndata; i++)
        if (!(u->tds[base + i].status & TD_ACTIVE)) got += (u->tds[base + i].status & TD_ACTLEN) + 1;
    if (in && len) memcpy(buf, data, got < len ? got : len);
    return in ? got : len;
}

static int uhci_transfer(usb_hcd_t *h, int addr, bool low, int ep, bool in,
                         uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms)
{
    uhci_t *u = h->priv;
    if (!len) return 0;
    if (maxpkt > 2047) maxpkt = 2047;
    bool poll = timeout_ms <= 100;                       /* an interrupt endpoint: short poll */
    int base = TD_CTL_FIRST, cap = TD_CTL_COUNT;
    if (poll) {                                          /* rotate, so a NAKed descriptor is never reused */
        if (u->rot < 0 || u->rot > TD_INT_COUNT - 2) u->rot = 0;
        base = TD_INT_FIRST + u->rot;
        cap = TD_INT_COUNT - u->rot;
    }
    int max = (int)maxpkt * (cap - 1);
    if (len > max) len = max;
    if (len > 4096) len = 4096;
    if (!in) memcpy(u->bounce, buf, len);
    int n = 0, off = 0, pkts = 0;
    while (off < len && n < cap) {
        int chunk = len - off; if (chunk > maxpkt) chunk = maxpkt;
        td_fill(&u->tds[base + n], in ? PID_IN : PID_OUT, addr, ep, *toggle ^ (pkts & 1), chunk, u->bounce + off);
        u->tds[base + n].status |= (off + chunk >= len) ? TD_SPD : 0;
        off += chunk; n++; pkts++;
    }
    uhci_link(u, base, n);
    int r = uhci_run(u, base, n, timeout_ms);
    if (poll) u->rot = (u->rot + n) % TD_INT_COUNT;
    if (r == -2) return -2;                              /* nothing this time (NAK) */
    if (r < 0) return -1;
    if (in && r > 0) memcpy(buf, u->bounce, r < len ? r : len);
    *toggle ^= (pkts & 1);
    return r;
}

static void uwait(uint32_t ms)
{
    uint32_t t0 = timer_ticks();
    while ((int32_t)(timer_ticks() - t0) < (int32_t)ms) task_yield();
}

/* ---------------- root ports ---------------- */
/* A port reset resets the device; the port itself is enabled by writing PED,
 * which is what the controller checks before it will move any transfer. */
static bool uhci_port_reset(usb_hcd_t *h, int port, bool *low_speed)
{
    uhci_t *u = h->priv;
    uint16_t reg = UHCI_PORT + port * 2;
    uint16_t v = ureg16(u, reg);
    if (!(v & PORT_CCS)) return false;
    uwreg16(u, reg, (uint16_t)(v | PORT_PR));
    uwait(20);
    uwreg16(u, reg, (uint16_t)((v & ~(PORT_PR | PORT_CSC | PORT_PEDC)) | PORT_PED));
    uwait(10);
    v = ureg16(u, reg);
    if (!(v & PORT_PED)) return false;
    *low_speed = (v & PORT_LSDA) != 0;
    return true;
}

/* Is something still on this root port? The connection bit is read only. */
static bool uhci_port_up(usb_hcd_t *h, int port)
{
    uhci_t *u = h->priv;
    if (port < 0 || port >= 2) return false;
    return (ureg16(u, UHCI_PORT + port * 2) & PORT_CCS) != 0;
}

static int uhci_init(uhci_t *u, const pci_dev_t *pd)
{
    bool is_io = false;
    uintptr_t bar = pci_bar(pd, 4, &is_io);
    if (!bar || !is_io) return -1;
    u->io = (uint16_t)bar;
    pci_enable(pd, true, false);

    uwreg16(u, UHCI_CMD, CMD_HCRESET);                   /* self-clearing */
    uwait(10);
    uwreg16(u, UHCI_CMD, 0);
    uwreg16(u, UHCI_STS, STS_MASK);
    uwreg16(u, UHCI_INTR, 0);                            /* polled: no interrupts */

    u->frames = kmalloc_aligned(4096, 4096);
    u->qh = kmalloc_aligned(4096, 16);
    u->tds = kmalloc_aligned(4096, 16);
    u->bounce = kmalloc_aligned(4096, 4096);
    if (!u->frames || !u->qh || !u->tds || !u->bounce) { klog("usb: no memory for the UHCI queues"); return -1; }
    memset(u->frames, 0, 4096);
    memset(u->tds, 0, 4096);
    memset(u->bounce, 0, 4096);
    memset(u->qh, 0, sizeof *u->qh);
    u->qh->link = 1;
    u->qh->element = 1;
    for (int i = 0; i < 1024; i++) u->frames[i] = (uint32_t)(uintptr_t)u->qh | 2;
    uwreg32(u, UHCI_FLBASE, (uint32_t)(uintptr_t)u->frames);
    uwreg16(u, UHCI_FRNUM, 0);
    uwreg16(u, UHCI_SOF, 64);
    uwreg16(u, UHCI_CMD, CMD_CF | CMD_RS);

    u->hcd.name = "UHCI";
    u->hcd.control = uhci_control;
    u->hcd.transfer = uhci_transfer;
    u->hcd.port_reset = uhci_port_reset;
    u->hcd.port_up = uhci_port_up;
    u->hcd.ports = 2;
    u->hcd.priv = u;
    return 0;
}

static bool uhci_match(const pci_dev_t *p) { return (p->vendor == 0x8086 && p->device == 0x7020) || (p->cls == 0x0C && p->sub == 0x03 && p->prog == 0x00); }
DRIVER_PCI(drv_uhci, "usbuhci", "USB 1.1 UHCI host controllers", DC_USBCTL, uhci_match);

void uhci_probe(void)
{
    pci_dev_t *devs = NULL;
    int n = pci_list(&devs), found = 0;
    for (int i = 0; i < n; i++) {
        pci_dev_t *pd = &devs[i];
        bool uhci = (pd->vendor == 0x8086 && pd->device == 0x7020) ||
                    (pd->cls == 0x0C && pd->sub == 0x03 && pd->prog == 0x00);
        if (!uhci) continue;
        uhci_t *u = kzalloc(sizeof *u);
        if (!u) return;
        if (uhci_init(u, pd) < 0) { kfree(u); klog("usb: %04x:%04x UHCI controller did not start", pd->vendor, pd->device); continue; }
        if (!the_uhci) the_uhci = u;                    /* keep the first one for the log */
        found++;
        klog("usb: UHCI at I/O %04x", u->io);
        { char nm[64]; snprintf(nm, sizeof nm, "USB 1.1 UHCI host controller (%d ports)", (int)u->hcd.ports); dev_claim(pd, &drv_uhci, nm); }
        usb_attach(&u->hcd);
    }
    if (!found) klog("usb: no UHCI controller found");
}
#endif
