/* NVMe solid-state disks (QEMU, VMware, VirtualBox, Hyper-V gen2, real
 * hardware). One admin queue pair and one I/O queue pair per controller,
 * polled, one command in flight, data through a 64 KiB bounce buffer. */
#include "kernel.h"
#include "disk.h"
#include "devmgr.h"

#define QD 16                              /* entries per queue */

typedef struct {
    volatile uint8_t *regs;
    uint32_t dstrd;                        /* doorbell stride in bytes */
    uint8_t *asq, *acq, *iosq, *iocq;      /* queues (4 KiB each) */
    uint16_t asq_tail, acq_head, iosq_tail, iocq_head;
    uint8_t aphase, iophase;
    uint16_t cid;
    uint8_t *buf;                          /* 64 KiB bounce buffer (page aligned) */
    uint64_t *prp;                         /* PRP list for the bounce buffer */
    uint32_t max_bytes;
    int index;
} nvme_ctrl_t;

typedef struct { nvme_ctrl_t *c; uint32_t nsid; } nvme_ns_t;

static inline uint32_t rd32(nvme_ctrl_t *c, int off) { return *(volatile uint32_t *)(c->regs + off); }
static inline void wr32(nvme_ctrl_t *c, int off, uint32_t v) { *(volatile uint32_t *)(c->regs + off) = v; }
static inline uint64_t rd64(nvme_ctrl_t *c, int off) { return rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32); }
static inline void put64(uint32_t *p, uint64_t v) { p[0] = (uint32_t)v; p[1] = (uint32_t)(v >> 32); }
static inline void wr64(nvme_ctrl_t *c, int off, uint64_t v) { wr32(c, off, (uint32_t)v); wr32(c, off + 4, (uint32_t)(v >> 32)); }

/* submit one command on queue pair qid (0 admin, 1 I/O) and wait for it */
static int submit(nvme_ctrl_t *c, int qid, uint32_t *cmd, uint32_t ms) {
    uint8_t *sq = qid ? c->iosq : c->asq, *cq = qid ? c->iocq : c->acq;
    uint16_t *tail = qid ? &c->iosq_tail : &c->asq_tail, *head = qid ? &c->iocq_head : &c->acq_head;
    uint8_t *phase = qid ? &c->iophase : &c->aphase;
    uint16_t cid = ++c->cid;
    cmd[0] = (cmd[0] & 0xFFFF) | ((uint32_t)cid << 16);
    memcpy(sq + *tail * 64, cmd, 64);
    *tail = (*tail + 1) % QD;
    __asm__ volatile("" ::: "memory");
    wr32(c, 0x1000 + (2 * qid) * c->dstrd, *tail);
    uint32_t t0 = timer_ticks();
    for (;;) {
        volatile uint32_t *e = (volatile uint32_t *)(cq + *head * 16);
        uint32_t dw3 = e[3];
        if (((dw3 >> 16) & 1) == *phase) {
            uint16_t status = (dw3 >> 17) & 0x7FFF;
            *head = (*head + 1) % QD;
            if (!*head) *phase ^= 1;
            wr32(c, 0x1000 + (2 * qid + 1) * c->dstrd, *head);
            if ((dw3 & 0xFFFF) != cid) continue;              /* a stale completion */
            return status;
        }
        if (timer_ticks() - t0 > ms) { klog("nvme%d: command %02x timed out", c->index, cmd[0] & 0xFF); return -1; }
    }
}

static void set_prps(nvme_ctrl_t *c, uint32_t *cmd, uint32_t bytes) {
    uint64_t a = dma_addr(c->buf);
    put64(&cmd[6], a);
    uint32_t pages = (bytes + 4095) / 4096;
    if (pages == 2) put64(&cmd[8], a + 4096);
    else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++) c->prp[i - 1] = a + i * 4096ull;
        put64(&cmd[8], dma_addr(c->prp));
    }
}

static int rw(nvme_ns_t *ns, uint8_t op, uint64_t lba, uint32_t n, uint32_t bytes) {
    uint32_t cmd[16] = { 0 };
    cmd[0] = op; cmd[1] = ns->nsid;
    set_prps(ns->c, cmd, bytes);
    cmd[10] = (uint32_t)lba; cmd[11] = (uint32_t)(lba >> 32);
    cmd[12] = (n - 1) & 0xFFFF;
    return submit(ns->c, 1, cmd, 10000);
}
static bool nvme_read(blkdev_t *d, uint64_t blk, uint32_t n, void *buf) {
    nvme_ns_t *ns = d->priv;
    int st = rw(ns, 0x02, blk, n, n * d->bsize);
    if (st) { klog("nvme: read failed at %u (status %x)", (uint32_t)blk, st); return false; }
    memcpy(buf, ns->c->buf, n * d->bsize);
    return true;
}
static bool nvme_write(blkdev_t *d, uint64_t blk, uint32_t n, const void *buf) {
    nvme_ns_t *ns = d->priv;
    memcpy(ns->c->buf, buf, n * d->bsize);
    int st = rw(ns, 0x01, blk, n, n * d->bsize);
    if (st) klog("nvme: write failed at %u (status %x)", (uint32_t)blk, st);
    return st == 0;
}
static bool nvme_flush(blkdev_t *d) {
    nvme_ns_t *ns = d->priv;
    uint32_t cmd[16] = { 0 };
    cmd[0] = 0x00; cmd[1] = ns->nsid;
    return submit(ns->c, 1, cmd, 10000) == 0;
}
static const blkops_t nvme_ops = { nvme_read, nvme_write, nvme_flush };

static int admin(nvme_ctrl_t *c, uint8_t op, uint32_t nsid, uint32_t dw10, uint32_t dw11, void *prp1) {
    uint32_t cmd[16] = { 0 };
    cmd[0] = op; cmd[1] = nsid;
    if (prp1) put64(&cmd[6], dma_addr(prp1));
    cmd[10] = dw10; cmd[11] = dw11;
    return submit(c, 0, cmd, 5000);
}

static int nctrl;
static bool nvme_match(const pci_dev_t *p) { return p->cls == 0x01 && p->sub == 0x08 && p->prog == 0x02; }
DRIVER_PCI(drv_nvme, "stornvme", "NVM Express (NVMe) controllers", DC_STORAGE, nvme_match);
static void ctrl_init(pci_dev_t *pd) {
    uintptr_t base = pci_bar(pd, 0, NULL);
    if (!base) { klog("nvme: %02x:%02x.%d: BAR0 unusable", pd->bus, pd->dev, pd->fn); return; }
    pci_enable(pd, false, true);
    nvme_ctrl_t *c = kzalloc(sizeof *c);
    c->regs = (volatile uint8_t *)base; c->index = nctrl++;
    uint64_t cap = rd64(c, 0x00);
    c->dstrd = 4u << ((cap >> 32) & 0xF);
    uint32_t to = ((cap >> 24) & 0xFF) * 500 + 1000;
    if (((cap >> 48) & 0xF) > 0) { klog("nvme%d: 4 KiB pages unsupported", c->index); return; }
    /* disable, set up the admin queues, enable */
    if (rd32(c, 0x14) & 1) {
        wr32(c, 0x14, rd32(c, 0x14) & ~1u);
        uint32_t t0 = timer_ticks();
        while ((rd32(c, 0x1C) & 1) && timer_ticks() - t0 < to) {}
    }
    c->asq = kmalloc_aligned(4096, 4096); c->acq = kmalloc_aligned(4096, 4096);
    c->iosq = kmalloc_aligned(4096, 4096); c->iocq = kmalloc_aligned(4096, 4096);
    memset(c->asq, 0, 4096); memset(c->acq, 0, 4096); memset(c->iosq, 0, 4096); memset(c->iocq, 0, 4096);
    c->buf = kmalloc_aligned(65536, 4096);
    c->prp = kmalloc_aligned(4096, 4096);
    c->aphase = c->iophase = 1;
    wr32(c, 0x0C, 0xFFFFFFFF);                                /* mask interrupts: we poll */
    wr32(c, 0x24, (QD - 1) | ((QD - 1) << 16));
    wr64(c, 0x28, dma_addr(c->asq));
    wr64(c, 0x30, dma_addr(c->acq));
    wr32(c, 0x14, 1 | (6 << 16) | (4 << 20));                 /* EN, 64-byte SQEs, 16-byte CQEs, 4 KiB pages */
    uint32_t t0 = timer_ticks();
    while (!(rd32(c, 0x1C) & 1)) {
        if ((rd32(c, 0x1C) & 2) || timer_ticks() - t0 > to) { klog("nvme%d: controller did not become ready", c->index); return; }
    }
    uint8_t *id = c->buf;
    memset(id, 0, 4096);
    if (admin(c, 0x06, 0, 1, 0, id)) { klog("nvme%d: IDENTIFY failed", c->index); return; }
    char model[41]; memcpy(model, id + 24, 40); model[40] = 0;
    for (int k = 39; k >= 0 && (model[k] == ' ' || !model[k]); k--) model[k] = 0;
    uint32_t nn = *(uint32_t *)(id + 516);
    uint8_t mdts = id[77];
    c->max_bytes = 65536;
    if (mdts && (4096u << mdts) < c->max_bytes) c->max_bytes = 4096u << mdts;
    /* I/O queue pair 1 */
    if (admin(c, 0x05, 0, 1 | ((QD - 1) << 16), 1, c->iocq) ||
        admin(c, 0x01, 0, 1 | ((QD - 1) << 16), 1 | (1 << 16), c->iosq)) { klog("nvme%d: creating I/O queues failed", c->index); return; }
    klog("nvme%d: \"%s\" %04x:%04x, %u namespaces, max %u KiB per command", c->index, model, pd->vendor, pd->device, nn, c->max_bytes / 1024);
    { char nm[72]; snprintf(nm, sizeof nm, "NVMe controller (%s)", model); dev_claim(pd, &drv_nvme, nm); }
    if (nn > 64) nn = 64;
    for (uint32_t nsid = 1; nsid <= nn; nsid++) {
        memset(id, 0, 4096);
        if (admin(c, 0x06, nsid, 0, 0, id)) continue;
        uint64_t nsze = *(uint64_t *)id;
        if (!nsze) continue;
        uint32_t lbads = (*(uint32_t *)(id + 128 + (id[26] & 0xF) * 4) >> 16) & 0xFF;
        if (lbads < 9 || lbads > 12) { klog("nvme%d: namespace %u: %u-byte blocks unsupported", c->index, nsid, 1u << lbads); continue; }
        blkdev_t *d = blk_register();
        if (!d) return;
        nvme_ns_t *ns = kzalloc(sizeof *ns); ns->c = c; ns->nsid = nsid;
        d->bsize = 1u << lbads;
        uint64_t secs = nsze << (lbads - 9);
        d->total_sectors = secs;
        d->sectors = secs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)secs;
        d->max_blocks = c->max_bytes / d->bsize;
        memcpy(d->model, model, sizeof d->model);
        d->ops = &nvme_ops; d->priv = ns; d->kind = "NVMe"; d->driver = "stornvme";
        d->busno = c->index; d->id = 0; d->lun = nsid;
        blk_commit(d);
    }
}

void nvme_probe(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++)
        if (nvme_match(&devs[i])) ctrl_init(&devs[i]);
}
