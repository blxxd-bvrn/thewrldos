/* SATA hard disks on AHCI controllers (Intel ICH/PCH, QEMU q35, VirtualBox and
 * VMware "SATA", AMD, ...). Polled, one command at a time per port, through a
 * 64 KiB bounce buffer. */
#include "kernel.h"
#include "disk.h"

#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define GHC_AE 0x80000000u
#define GHC_IE 0x02u

#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0C
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38
#define CMD_ST 0x0001u
#define CMD_SUD 0x0002u
#define CMD_POD 0x0004u
#define CMD_FRE 0x0010u
#define CMD_FR 0x4000u
#define CMD_CR 0x8000u
#define IS_TFES (1u << 30)

typedef struct {
    volatile uint8_t *port;           /* this port's registers */
    uint32_t *clist;                  /* 32 command headers (we use slot 0) */
    uint8_t *fis;                     /* received FIS area */
    uint8_t *table;                   /* command table: CFIS + one PRD */
    uint8_t *buf;                     /* 64 KiB bounce buffer */
    bool lba48;
} ahci_port_t;

static inline uint32_t rd(volatile uint8_t *b, int off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, int off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

static bool wait_clear(volatile uint8_t *b, int off, uint32_t bits, uint32_t ms) {
    uint32_t t0 = timer_ticks();
    while (rd(b, off) & bits) if (timer_ticks() - t0 > ms) return false;
    return true;
}

static void port_stop(volatile uint8_t *p) {
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR, 500);
    wr(p, PX_CMD, rd(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR, 500);
}

/* one ATA command through slot 0; data goes through the bounce buffer */
static bool port_cmd(ahci_port_t *ap, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, bool write) {
    volatile uint8_t *p = ap->port;
    if (!wait_clear(p, PX_TFD, 0x88, 2000)) {                /* BSY | DRQ stuck: restart the port */
        port_stop(p);
        wr(p, PX_SERR, 0xFFFFFFFF); wr(p, PX_IS, 0xFFFFFFFF);
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE); wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
        if (!wait_clear(p, PX_TFD, 0x88, 2000)) return false;
    }
    uint8_t *f = ap->table;
    memset(f, 0, 0x80 + 16);
    f[0] = 0x27; f[1] = 0x80; f[2] = cmd;                    /* register H2D, command */
    f[4] = lba; f[5] = lba >> 8; f[6] = lba >> 16; f[7] = 0x40;
    f[8] = lba >> 24; f[9] = lba >> 32; f[10] = lba >> 40;
    f[12] = count; f[13] = count >> 8;
    uint32_t *prd = (uint32_t *)(f + 0x80);
    if (bytes) {
        prd[0] = (uint32_t)dma_addr(ap->buf); prd[1] = (uint32_t)(dma_addr(ap->buf) >> 32);
        prd[3] = (bytes - 1) & 0x3FFFFF;
    }
    uint32_t *h = ap->clist;
    h[0] = 5 | (write ? 1u << 6 : 0) | ((bytes ? 1u : 0u) << 16);   /* CFL 5 dwords, W, PRDTL */
    h[1] = 0;
    h[2] = (uint32_t)dma_addr(f); h[3] = (uint32_t)(dma_addr(f) >> 32);
    wr(p, PX_IS, 0xFFFFFFFF);
    __asm__ volatile("" ::: "memory");
    wr(p, PX_CI, 1);
    uint32_t t0 = timer_ticks();
    for (;;) {
        if (!(rd(p, PX_CI) & 1)) break;
        if (rd(p, PX_IS) & IS_TFES) break;
        if (timer_ticks() - t0 > 10000) { klog("ahci: command %02x timed out", cmd); port_stop(p); return false; }
    }
    if ((rd(p, PX_IS) & IS_TFES) || (rd(p, PX_TFD) & 0x01)) {
        klog("ahci: command %02x failed at lba %u (tfd %x)", cmd, (uint32_t)lba, rd(p, PX_TFD));
        /* clear the error so the next command can run */
        port_stop(p);
        wr(p, PX_SERR, 0xFFFFFFFF); wr(p, PX_IS, 0xFFFFFFFF);
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_FRE); wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
        return false;
    }
    return true;
}

static bool ahci_read(blkdev_t *d, uint64_t blk, uint32_t n, void *buf) {
    ahci_port_t *ap = d->priv;
    if (!port_cmd(ap, ap->lba48 ? 0x25 : 0xC8, blk, n, n * 512, false)) return false;
    memcpy(buf, ap->buf, n * 512);
    return true;
}
static bool ahci_write(blkdev_t *d, uint64_t blk, uint32_t n, const void *buf) {
    ahci_port_t *ap = d->priv;
    memcpy(ap->buf, buf, n * 512);
    return port_cmd(ap, ap->lba48 ? 0x35 : 0xCA, blk, n, n * 512, true);
}
static bool ahci_flush(blkdev_t *d) {
    ahci_port_t *ap = d->priv;
    return port_cmd(ap, ap->lba48 ? 0xEA : 0xE7, 0, 0, 0, false);
}
static const blkops_t ahci_ops = { ahci_read, ahci_write, ahci_flush };

static void port_init(volatile uint8_t *abar, int i) {
    volatile uint8_t *p = abar + 0x100 + i * 0x80;
    uint32_t ssts = rd(p, PX_SSTS);
    if ((ssts & 0xF) != 3 && !(rd(p, PX_CMD) & CMD_SUD)) {     /* not spun up yet: do it and wait for the link */
        wr(p, PX_CMD, rd(p, PX_CMD) | CMD_SUD | CMD_POD);
        uint32_t t0 = timer_ticks();
        while (((ssts = rd(p, PX_SSTS)) & 0xF) != 3 && timer_ticks() - t0 < 20) {}
    }
    if ((ssts & 0xF) != 3) return;                            /* no device / no link */
    uint32_t sig = rd(p, PX_SIG);
    if (sig == 0xEB140101 || sig == 0xC33C0101 || sig == 0x96690101) return;   /* ATAPI, enclosure, port multiplier */
    port_stop(p);
    ahci_port_t *ap = kzalloc(sizeof *ap);
    ap->port = p;
    ap->clist = kmalloc_aligned(1024, 1024); memset(ap->clist, 0, 1024);
    ap->fis = kmalloc_aligned(256, 256); memset(ap->fis, 0, 256);
    ap->table = kmalloc_aligned(256, 128);
    ap->buf = kmalloc_aligned(65536, 4096);
    wr(p, PX_CLB, (uint32_t)dma_addr(ap->clist)); wr(p, PX_CLBU, 0);
    wr(p, PX_FB, (uint32_t)dma_addr(ap->fis)); wr(p, PX_FBU, 0);
    wr(p, PX_IE, 0);
    wr(p, PX_SERR, 0xFFFFFFFF); wr(p, PX_IS, 0xFFFFFFFF);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_SUD | CMD_POD | CMD_FRE);
    wait_clear(p, PX_TFD, 0x88, 3000);
    wr(p, PX_CMD, rd(p, PX_CMD) | CMD_ST);
    if (!port_cmd(ap, 0xEC, 0, 0, 512, false)) { klog("ahci: port %d: IDENTIFY failed", i); port_stop(p); return; }
    uint16_t *id = (uint16_t *)ap->buf;
    blkdev_t *d = blk_register();
    if (!d) return;
    ap->lba48 = (id[83] & (1 << 10)) != 0;
    uint64_t s28 = id[60] | ((uint32_t)id[61] << 16);
    uint64_t s48 = id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    uint64_t secs = (ap->lba48 && s48) ? s48 : s28;
    d->sectors = secs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)secs;
    for (int k = 0; k < 20; k++) { d->model[k * 2] = id[27 + k] >> 8; d->model[k * 2 + 1] = id[27 + k] & 0xFF; }
    d->model[40] = 0;
    for (int k = 39; k >= 0 && d->model[k] == ' '; k--) d->model[k] = 0;
    if (!d->sectors) { kfree(d); return; }
    d->ops = &ahci_ops; d->priv = ap; d->kind = "SATA"; d->driver = "storahci";
    d->busno = 0; d->id = i; d->bsize = 512; d->max_blocks = 128;
    blk_commit(d);
}

static void hba_init(pci_dev_t *pd) {
    uintptr_t base = pci_bar(pd, 5, NULL);
    if (!base) { klog("ahci: %02x:%02x.%d has no usable ABAR", pd->bus, pd->dev, pd->fn); return; }
    pci_enable(pd, false, true);
    volatile uint8_t *abar = (volatile uint8_t *)base;
    if (rd(abar, HBA_CAP2) & 1) {                             /* take the controller from the BIOS */
        wr(abar, HBA_BOHC, rd(abar, HBA_BOHC) | 2);
        uint32_t t0 = timer_ticks();
        while ((rd(abar, HBA_BOHC) & 1) && timer_ticks() - t0 < 100) {}
    }
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);
    wr(abar, HBA_GHC, rd(abar, HBA_GHC) & ~GHC_IE);           /* we poll */
    uint32_t pi = rd(abar, HBA_PI), cap = rd(abar, HBA_CAP);
    klog("ahci: controller %04x:%04x at %x, %d ports (implemented %x)", pd->vendor, pd->device, (uint32_t)base, (cap & 0x1F) + 1, pi);
    for (int i = 0; i < 32; i++) if (pi & (1u << i)) port_init(abar, i);
}

void ahci_probe(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++)
        if (devs[i].cls == 0x01 && devs[i].sub == 0x06 && devs[i].prog == 0x01) hba_init(&devs[i]);
}
