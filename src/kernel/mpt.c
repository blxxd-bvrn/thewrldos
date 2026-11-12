/* LSI Logic Fusion-MPT SCSI host adapters: 53C1030 ("LSI Logic Parallel" in
 * VMware and VirtualBox) and SAS1064/1068 ("LSI Logic SAS" in VMware and
 * VirtualBox, mptsas1068 in QEMU). Message-passing interface 1.x: setup goes
 * through the doorbell handshake, I/O through the request and reply FIFOs. */
#include "kernel.h"
#include "disk.h"
#include "devmgr.h"

#define R_DOORBELL 0x00
#define R_DIAG 0x08
#define R_HIS 0x30
#define R_HIM 0x34
#define R_REQ_FIFO 0x40
#define R_REPLY_FIFO 0x44
#define HIS_DOORBELL 0x00000001u
#define HIS_IOP_DOORBELL 0x80000000u
#define ST_MASK 0xF0000000u
#define ST_RESET 0x00000000u
#define ST_READY 0x10000000u
#define ST_OPER 0x20000000u
#define ST_FAULT 0x40000000u
#define NREPLY 8
#define REPLY_SIZE 128

typedef struct {
    uint16_t io;                    /* I/O BAR, or 0 */
    volatile uint8_t *mem;          /* memory BAR when there is no I/O BAR */
    uint8_t *req;                   /* request frame (+ SGE) */
    uint8_t *replies;               /* NREPLY reply frames */
    uint8_t *sense;
    uint8_t *buf;                   /* 64 KiB bounce buffer */
    uint32_t ctx;
    int max_devices;
    scsi_host_t host;
} mpt_t;

static uint32_t rd(mpt_t *m, int off) { return m->io ? inl(m->io + off) : *(volatile uint32_t *)(m->mem + off); }
static void wr(mpt_t *m, int off, uint32_t v) { if (m->io) outl(m->io + off, v); else *(volatile uint32_t *)(m->mem + off) = v; }

static bool wait_his(mpt_t *m, uint32_t bit, bool set, uint32_t ms) {
    uint32_t t0 = timer_ticks();
    while (((rd(m, R_HIS) & bit) != 0) != set) if (timer_ticks() - t0 > ms) return false;
    return true;
}

/* doorbell handshake: send a request of ndw dwords, read the reply into rep (max repcap bytes) */
static bool handshake(mpt_t *m, const uint32_t *msg, int ndw, uint16_t *rep, int repcap) {
    if (rd(m, R_DOORBELL) & 0x08000000u) return false;       /* doorbell in use */
    wr(m, R_HIS, 0);
    wr(m, R_DOORBELL, (0x42u << 24) | ((uint32_t)ndw << 16));
    if (!wait_his(m, HIS_DOORBELL, true, 1000)) return false;
    wr(m, R_HIS, 0);
    if (!wait_his(m, HIS_IOP_DOORBELL, false, 1000)) return false;
    for (int i = 0; i < ndw; i++) {
        wr(m, R_DOORBELL, msg[i]);
        if (!wait_his(m, HIS_IOP_DOORBELL, false, 1000)) return false;
    }
    int words = 4;                                            /* until the header says how long the reply is */
    memset(rep, 0, repcap);
    for (int i = 0; i < words; i++) {
        if (!wait_his(m, HIS_DOORBELL, true, 2000)) return false;
        uint16_t w = rd(m, R_DOORBELL) & 0xFFFF;
        if (i < repcap / 2) rep[i] = w;
        wr(m, R_HIS, 0);
        if (i == 1) words = (w & 0xFF) * 2;                   /* MsgLength in dwords, byte 2 */
    }
    if (wait_his(m, HIS_DOORBELL, true, 100)) wr(m, R_HIS, 0);
    return true;
}

static bool wait_state(mpt_t *m, uint32_t st, uint32_t ms) {
    uint32_t t0 = timer_ticks();
    while ((rd(m, R_DOORBELL) & ST_MASK) != st) if (timer_ticks() - t0 > ms) return false;
    return true;
}

static int mpt_exec(scsi_host_t *h, int target, int lun, const uint8_t *cdb, int cdblen, void *buf, uint32_t len, int dir) {
    mpt_t *m = h->priv;
    if (len > 65536 || cdblen > 16) return -1;
    uint8_t *r = m->req;
    memset(r, 0, 64);
    r[0] = target; r[1] = 0; r[3] = 0x00;                    /* SCSI IO request */
    r[4] = cdblen; r[5] = 32;                                 /* sense buffer length */
    uint32_t ctx = (++m->ctx) & 0x7FFFFFFF;
    memcpy(r + 8, &ctx, 4);
    r[0x0D] = lun;                                            /* LUN[1] */
    uint32_t control = dir == 1 ? 0x02000000u : dir == 2 ? 0x01000000u : 0;
    memcpy(r + 0x14, &control, 4);
    memcpy(r + 0x18, cdb, cdblen);
    uint32_t dl = dir ? len : 0;
    memcpy(r + 0x28, &dl, 4);
    uint32_t sa = (uint32_t)dma_addr(m->sense);
    memcpy(r + 0x2C, &sa, 4);
    /* one simple 32-bit SGE: last element, end of buffer, end of list */
    uint32_t fl = 0x10000000u | 0x80000000u | 0x40000000u | 0x01000000u | (dir == 2 ? 0x04000000u : 0) | (dl & 0xFFFFFF);
    uint32_t ba = (uint32_t)dma_addr(m->buf);
    memcpy(r + 0x30, &fl, 4); memcpy(r + 0x34, &ba, 4);
    if (dir == 2) memcpy(m->buf, buf, len);
    __asm__ volatile("" ::: "memory");
    wr(m, R_REQ_FIFO, (uint32_t)dma_addr(r));
    uint32_t t0 = timer_ticks();
    for (;;) {
        uint32_t v = rd(m, R_REPLY_FIFO);
        if (v == 0xFFFFFFFF) {
            if (timer_ticks() - t0 > 10000) { klog("mpt: command %02x to target %d timed out", cdb[0], target); return -1; }
            if ((rd(m, R_DOORBELL) & ST_MASK) == ST_FAULT) { klog("mpt: adapter fault %x", rd(m, R_DOORBELL)); return -1; }
            continue;
        }
        if (!(v & 0x80000000u)) {                             /* context reply: success */
            if (v != ctx) continue;
            if (dir == 1) memcpy(buf, m->buf, len);
            return 0;
        }
        uint8_t *rep = (uint8_t *)(uintptr_t)(v << 1);        /* address reply: details in a reply frame */
        uint32_t rctx; memcpy(&rctx, rep + 8, 4);
        uint16_t ioc; memcpy(&ioc, rep + 0x0E, 2);
        uint8_t sstat = rep[0x0C];
        uint32_t xfer; memcpy(&xfer, rep + 0x14, 4);
        wr(m, R_REPLY_FIFO, (uint32_t)(uintptr_t)rep);        /* give the frame back */
        if (rctx != ctx) continue;
        ioc &= 0x7FFF;
        if (ioc == 0x0041 || ioc == 0x0042 || ioc == 0x0043) return -1;   /* bad bus / bad target / no device */
        if (ioc && ioc != 0x0040 && ioc != 0x0044 && ioc != 0x0045 && ioc != 0x0049 && !sstat) return -1;
        if (dir == 1 && !sstat) memcpy(buf, m->buf, MIN(len, xfer ? xfer : len));
        return sstat;
    }
}

static int nhost;
static const uint16_t mpt_ids[] = { 0x0030, 0x0032, 0x0040, 0x0041, 0x0050, 0x0054, 0x0056, 0x0058, 0x005A, 0x0062 };
static bool mpt_match(const pci_dev_t *p) {
    if (p->vendor != 0x1000) return false;
    for (unsigned k = 0; k < ARRAY_LEN(mpt_ids); k++) if (p->device == mpt_ids[k]) return true;
    return false;
}
DRIVER_PCI(drv_mpt, "mpt", "LSI Logic Fusion-MPT SCSI and SAS adapters", DC_STORAGE, mpt_match);
static void mpt_init(pci_dev_t *pd) {
    mpt_t *m = kzalloc(sizeof *m);
    bool isio;
    uintptr_t b0 = pci_bar(pd, 0, &isio);
    if (b0 && isio) m->io = b0;
    else {
        uintptr_t b1 = pci_bar(pd, 1, &isio);
        if (!b1 || isio) { klog("mpt: no usable BAR"); return; }
        m->mem = (volatile uint8_t *)b1;
    }
    pci_enable(pd, m->io != 0, m->mem != NULL);
    wr(m, R_HIM, 0x09);                                       /* mask doorbell + reply interrupts: we poll */
    uint32_t st = rd(m, R_DOORBELL) & ST_MASK;
    if (st != ST_READY) {                                     /* the BIOS left it running: message unit reset */
        wr(m, R_DOORBELL, 0x40u << 24);
        if (!wait_state(m, ST_READY, 5000)) { klog("mpt: adapter did not become ready (%x)", rd(m, R_DOORBELL)); return; }
    }
    uint16_t rep[64];
    uint32_t facts[3] = { 0x03u << 24, 0, 0x1234 };           /* IOC facts */
    if (!handshake(m, facts, 3, rep, sizeof rep)) { klog("mpt: IOC facts failed"); return; }
    uint8_t *fr = (uint8_t *)rep;
    uint16_t msgver = rep[0], hdrver = rep[2];
    m->max_devices = fr[0x2E] ? fr[0x2E] : 16;
    uint8_t max_buses = fr[0x2F];
    uint32_t init[11] = { 0 };                                /* IOC init */
    uint8_t *ip = (uint8_t *)init;
    ip[0] = 0x04; ip[3] = 0x02;                               /* host driver, IOC init */
    ip[5] = m->max_devices; ip[6] = max_buses;
    init[2] = 0x2345;
    ip[0x0C] = REPLY_SIZE;                                    /* reply frame size */
    memcpy(ip + 0x28, &msgver, 2); memcpy(ip + 0x2A, &hdrver, 2);
    if (!handshake(m, init, 11, rep, sizeof rep) || !wait_state(m, ST_OPER, 5000)) {
        klog("mpt: IOC init failed (%x)", rd(m, R_DOORBELL)); return;
    }
    m->req = kmalloc_aligned(128, 64);
    m->sense = kmalloc_aligned(64, 64);
    m->replies = kmalloc_aligned(NREPLY * REPLY_SIZE, 64);
    m->buf = kmalloc_aligned(65536, 4096);
    for (int i = 0; i < NREPLY; i++) wr(m, R_REPLY_FIFO, (uint32_t)dma_addr(m->replies + i * REPLY_SIZE));
    uint32_t pe[3] = { 0x06u << 24, 0, 0x3456 };              /* port enable, port 0 */
    if (!handshake(m, pe, 3, rep, sizeof rep)) klog("mpt: port enable failed (continuing)");
    m->host.exec = mpt_exec; m->host.priv = m;
    m->host.driver = (pd->device == 0x0030 || pd->device == 0x0032 || pd->device == 0x0040) ? "symmpi" : "lsi_sas";
    m->host.busno = nhost++;
    m->host.max_target = MIN(m->max_devices - 1, 31);
    m->host.max_lun = 7;
    m->host.max_xfer = 65536;
    klog("mpt: LSI %04x \"%s\", %d targets, %s", pd->device, m->host.driver, m->max_devices, m->io ? "I/O ports" : "MMIO");
    { char nm[72]; snprintf(nm, sizeof nm, "LSI Logic Fusion-MPT adapter (%s)", m->host.driver); dev_claim(pd, &drv_mpt, nm); }
    scsi_scan(&m->host);
}

void mpt_probe(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (mpt_match(&devs[i])) mpt_init(&devs[i]);
    }
}
