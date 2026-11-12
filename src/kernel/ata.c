/* ATA/IDE hard disk driver (bus-master DMA or PIO, polled, LBA28 + LBA48) */
#include "kernel.h"
#include "disk.h"
#include "devmgr.h"

#define ATA_DATA 0
#define ATA_ERR 1
#define ATA_COUNT 2
#define ATA_LBA0 3
#define ATA_LBA1 4
#define ATA_LBA2 5
#define ATA_DRIVE 6
#define ATA_CMD 7
#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_BSY 0x80


static inline void insw(uint16_t port, void *buf, uint32_t n) { __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(n) : "d"(port) : "memory"); }
static inline void outsw(uint16_t port, const void *buf, uint32_t n) { __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(n) : "d"(port)); }

static void delay400(blkdev_t *d) { for (int i = 0; i < 4; i++) inb(d->ctl); }
/* waits are bounded by time, not by loop counts: on a VM every register read
 * is a slow exit to the hypervisor, so a fixed count can take minutes */
static int wait_ready_ms(uint16_t ctl, bool need_drq, uint32_t ms) {
    uint32_t t0 = timer_ticks();
    for (uint32_t i = 0;; i++) {
        uint8_t s = inb(ctl);
        if (s == 0xFF) return -1;
        if (!(s & ST_BSY)) {
            if (s & (ST_ERR | ST_DF)) return -2;
            if (!need_drq || (s & ST_DRQ)) return 0;
        }
        if ((i & 63) == 63 && timer_ticks() - t0 > ms) return -3;
    }
}
static int wait_ready(uint16_t io, uint16_t ctl, bool need_drq) { (void)io; return wait_ready_ms(ctl, need_drq, 5000); }

/* ---- bus-master IDE DMA (PIIX/ICH IDE in QEMU, VirtualBox, VMware) ---- */
static uint16_t bmide;                    /* bus master base (primary; secondary is +8) */
static uint32_t *prdt[2];
static uint8_t *bounce;                   /* 64 KiB, never crosses a 64 KiB boundary */
static bool ide_match(const pci_dev_t *p) { return p->cls == 0x01 && p->sub == 0x01; }
DRIVER_PCI(drv_ide, "atapi", "IDE/ATA controllers (bus-master DMA or PIO)", DC_STORAGE, ide_match);
static void dma_setup(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (!ide_match(&devs[i])) continue;
        uint32_t bar4 = pci_cfg_read(devs[i].bus, devs[i].dev, devs[i].fn, 0x20);
        if (!(bar4 & 1) || !(bar4 & ~3u)) { dev_claim(&devs[i], &drv_ide, "Standard IDE controller (PIO)"); continue; }
        uint32_t cmd = pci_cfg_read(devs[i].bus, devs[i].dev, devs[i].fn, 0x04);
        pci_cfg_write(devs[i].bus, devs[i].dev, devs[i].fn, 0x04, (cmd & 0xFFFF) | 0x05);    /* I/O + bus master */
        bmide = bar4 & ~3u;
        for (int c = 0; c < 2; c++) prdt[c] = kmalloc_aligned(16, 16);
        bounce = kmalloc_aligned(65536, 65536);
        klog("ata: bus-master DMA at %x (%04x:%04x)", bmide, devs[i].vendor, devs[i].device);
        dev_claim(&devs[i], &drv_ide, "Standard dual-channel PCI IDE controller (bus-master DMA)");
        return;
    }
}
static bool dma_xfer(blkdev_t *d, uint32_t lba, uint32_t n, bool write) {
    uint16_t bm = bmide + (d->bus ? 8 : 0), io = d->io;
    uint32_t *prd = prdt[d->bus];
    prd[0] = (uint32_t)(uintptr_t)bounce; prd[1] = 0x80000000u | ((n * 512) & 0xFFFF);   /* 0 means 64 KiB */
    outb(bm, 0);                                       /* stop */
    outb(bm + 2, inb(bm + 2) | 0x06);                  /* clear error + interrupt */
    outl(bm + 4, (uint32_t)(uintptr_t)prd);
    if (wait_ready_ms(d->ctl, false, 2000) < -1) return false;
    bool ext = d->lba48 && (lba + n > 0x0FFFFFFF);
    if (ext) {
        outb(io + ATA_DRIVE, 0x40 | (d->slave << 4)); delay400(d);
        outb(io + ATA_COUNT, 0); outb(io + ATA_LBA0, (lba >> 24) & 0xFF); outb(io + ATA_LBA1, 0); outb(io + ATA_LBA2, 0);
        outb(io + ATA_COUNT, n & 0xFF); outb(io + ATA_LBA0, lba & 0xFF); outb(io + ATA_LBA1, (lba >> 8) & 0xFF); outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
        outb(io + ATA_CMD, write ? 0x35 : 0x25);
    } else {
        outb(io + ATA_DRIVE, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)); delay400(d);
        outb(io + ATA_COUNT, n & 0xFF); outb(io + ATA_LBA0, lba & 0xFF); outb(io + ATA_LBA1, (lba >> 8) & 0xFF); outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
        outb(io + ATA_CMD, write ? 0xCA : 0xC8);
    }
    outb(bm, write ? 0x01 : 0x09);                     /* start; bit 3 = transfer into memory */
    uint32_t t0 = timer_ticks();
    for (;;) {
        uint8_t bs = inb(bm + 2), st = inb(d->ctl);
        if (bs & 0x02) break;                          /* bus master error */
        if (!(bs & 0x01) && !(st & ST_BSY)) break;     /* finished */
        if (timer_ticks() - t0 > 5000) { outb(bm, 0); klog("ata: DMA timeout at lba %u", lba); return false; }
    }
    outb(bm, 0);
    uint8_t bs = inb(bm + 2), st = inb(io + 7);
    outb(bm + 2, bs | 0x06);
    if ((bs & 0x02) || (st & (ST_ERR | ST_DF))) { klog("ata: DMA error at lba %u (bm %02x, status %02x)", lba, bs, st); return false; }
    return true;
}

static bool ata_read(blkdev_t *d, uint64_t blk, uint32_t count, void *buf);
static bool ata_write(blkdev_t *d, uint64_t blk, uint32_t count, const void *buf);
static const blkops_t ata_ops = { ata_read, ata_write, NULL };

static void identify(int bus, int slave) {
    uint16_t io = bus ? 0x170 : 0x1F0, ctl = bus ? 0x376 : 0x3F6;
    if (inb(io + 7) == 0xFF) return;                  /* floating bus */
    outb(ctl, 0x02);                                   /* nIEN: we poll */
    outb(io + ATA_DRIVE, 0xA0 | (slave << 4));
    for (int i = 0; i < 15; i++) inb(ctl);
    outb(io + ATA_COUNT, 0); outb(io + ATA_LBA0, 0); outb(io + ATA_LBA1, 0); outb(io + ATA_LBA2, 0);
    outb(io + ATA_CMD, 0xEC);
    uint8_t st = inb(io + 7);
    if (!st) return;
    { uint32_t t0 = timer_ticks(); while ((inb(io + 7) & ST_BSY) && timer_ticks() - t0 < 1000) {} }
    if (inb(io + ATA_LBA1) || inb(io + ATA_LBA2)) return;   /* ATAPI / SATA signature: not an ATA disk */
    { uint32_t t0 = timer_ticks(); do { st = inb(io + 7); if (st & (ST_DRQ | ST_ERR)) break; } while (timer_ticks() - t0 < 1000); }
    if (st & ST_ERR || !(st & ST_DRQ)) return;
    uint16_t id[256];
    insw(io, id, 256);
    blkdev_t *d = blk_register();
    if (!d) return;
    d->bus = bus; d->slave = slave; d->io = io; d->ctl = ctl;
    d->lba48 = (id[83] & (1 << 10)) != 0;
    d->dma = (id[49] & (1 << 8)) != 0;
    uint32_t s28 = id[60] | ((uint32_t)id[61] << 16);
    uint64_t s48 = id[100] | ((uint32_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    d->total_sectors = (d->lba48 && s48) ? s48 : s28;
    d->sectors = d->total_sectors > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d->total_sectors;
    for (int i = 0; i < 20; i++) { d->model[i * 2] = id[27 + i] >> 8; d->model[i * 2 + 1] = id[27 + i] & 0xFF; }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
    if (!d->sectors) { kfree(d); return; }
    d->ops = &ata_ops; d->kind = "IDE"; d->driver = "atapi";
    d->busno = bus; d->id = slave; d->max_blocks = 128;
    blk_commit(d);
}
void ata_probe(void) {
    if (!bmide) dma_setup();
    for (int b = 0; b < 2; b++) for (int s = 0; s < 2; s++) identify(b, s);
}

static bool setup_cmd(blkdev_t *d, uint32_t lba, uint32_t n, bool write) {
    uint16_t io = d->io;
    if (wait_ready(io, d->ctl, false) < -1) { /* clear a stuck error by reselecting */ }
    if (d->lba48 && (lba + n > 0x0FFFFFFF || n > 256)) {
        outb(io + ATA_DRIVE, 0x40 | (d->slave << 4));
        delay400(d);
        outb(io + ATA_COUNT, (n >> 8) & 0xFF);
        outb(io + ATA_LBA0, (lba >> 24) & 0xFF); outb(io + ATA_LBA1, 0); outb(io + ATA_LBA2, 0);
        outb(io + ATA_COUNT, n & 0xFF);
        outb(io + ATA_LBA0, lba & 0xFF); outb(io + ATA_LBA1, (lba >> 8) & 0xFF); outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
        outb(io + ATA_CMD, write ? 0x34 : 0x24);
    } else {
        outb(io + ATA_DRIVE, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
        delay400(d);
        outb(io + ATA_COUNT, n & 0xFF);
        outb(io + ATA_LBA0, lba & 0xFF); outb(io + ATA_LBA1, (lba >> 8) & 0xFF); outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
        outb(io + ATA_CMD, write ? 0x30 : 0x20);
    }
    return true;
}
static bool ata_read(blkdev_t *d, uint64_t blk, uint32_t count, void *buf) {
    uint32_t lba = (uint32_t)blk;
    uint8_t *p = buf;
    if (bmide && d->dma) {
        while (count) {
            uint32_t n = MIN(count, 128u);
            if (!dma_xfer(d, lba, n, false)) { d->dma = false; klog("ata: disk %d: falling back to PIO", d->index); break; }
            memcpy(p, bounce, n * 512);
            p += n * 512; lba += n; count -= n;
        }
        if (!count) return true;
    }
    while (count) {
        uint32_t n = MIN(count, 128u);
        setup_cmd(d, lba, n, false);
        for (uint32_t i = 0; i < n; i++) {
            delay400(d);
            if (wait_ready(d->io, d->ctl, true)) { klog("ata: read error at lba %u (status %02x err %02x)", lba + i, inb(d->io + 7), inb(d->io + ATA_ERR)); return false; }
            insw(d->io, p, 256); p += 512;
        }
        lba += n; count -= n;
    }
    return true;
}
static bool ata_write(blkdev_t *d, uint64_t blk, uint32_t count, const void *buf) {
    uint32_t lba = (uint32_t)blk;
    const uint8_t *p = buf;
    if (bmide && d->dma) {
        while (count) {
            uint32_t n = MIN(count, 128u);
            memcpy(bounce, p, n * 512);
            if (!dma_xfer(d, lba, n, true)) { d->dma = false; klog("ata: disk %d: falling back to PIO", d->index); break; }
            p += n * 512; lba += n; count -= n;
        }
        if (!count) {
            outb(d->io + ATA_CMD, d->lba48 ? 0xEA : 0xE7);     /* flush the write cache */
            wait_ready_ms(d->ctl, false, 5000);
            return true;
        }
    }
    while (count) {
        uint32_t n = MIN(count, 128u);
        bool ext = d->lba48 && (lba + n > 0x0FFFFFFF);
        setup_cmd(d, lba, n, true);
        for (uint32_t i = 0; i < n; i++) {
            delay400(d);
            if (wait_ready(d->io, d->ctl, true)) { klog("ata: write error at lba %u (status %02x)", lba + i, inb(d->io + 7)); return false; }
            outsw(d->io, p, 256); p += 512;
        }
        outb(d->io + ATA_CMD, ext ? 0xEA : 0xE7);     /* cache flush */
        delay400(d);
        if (wait_ready(d->io, d->ctl, false) < -1) { klog("ata: flush failed"); return false; }
        lba += n; count -= n;
    }
    return true;
}
