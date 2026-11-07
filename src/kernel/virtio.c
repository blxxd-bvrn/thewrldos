/* virtio disks: virtio-blk and virtio-scsi (QEMU/KVM, Proxmox, VirtualBox,
 * cloud hypervisors). Both PCI transports: virtio 1.0 "modern" (capabilities,
 * memory BARs) and the legacy I/O-port interface of older/transitional
 * devices. One split virtqueue per device, one request at a time, polled. */
#include "kernel.h"
#include "disk.h"
#include "virtio.h"

/* ---- transport ---- */
static inline uint8_t c8(vdev_t *v, int off) { return *(volatile uint8_t *)(v->common + off); }
static inline uint16_t c16(vdev_t *v, int off) { return *(volatile uint16_t *)(v->common + off); }
static inline uint32_t c32(vdev_t *v, int off) { return *(volatile uint32_t *)(v->common + off); }
static inline void w8(vdev_t *v, int off, uint8_t x) { *(volatile uint8_t *)(v->common + off) = x; }
static inline void w16(vdev_t *v, int off, uint16_t x) { *(volatile uint16_t *)(v->common + off) = x; }
static inline void w32(vdev_t *v, int off, uint32_t x) { *(volatile uint32_t *)(v->common + off) = x; }

uint8_t virtio_get_status(vdev_t *v) { return v->modern ? c8(v, 0x14) : inb(v->io + 0x12); }
void virtio_set_status(vdev_t *v, uint8_t s) { if (v->modern) w8(v, 0x14, s); else outb(v->io + 0x12, s); }
uint32_t virtio_cfg32(vdev_t *v, int off) { return v->modern ? *(volatile uint32_t *)(v->devcfg + off) : inl(v->io + 0x14 + off); }
uint16_t virtio_cfg16(vdev_t *v, int off) { return v->modern ? *(volatile uint16_t *)(v->devcfg + off) : inw(v->io + 0x14 + off); }
uint8_t virtio_cfg8(vdev_t *v, int off) { return v->modern ? *(volatile uint8_t *)(v->devcfg + off) : inb(v->io + 0x14 + off); }
static uint64_t cfg64(vdev_t *v, int off) { return virtio_cfg32(v, off) | ((uint64_t)virtio_cfg32(v, off + 4) << 32); }
#define cfg32 virtio_cfg32
#define cfg16 virtio_cfg16
#define get_status virtio_get_status
#define set_status virtio_set_status

/* reset, acknowledge, negotiate: returns the device feature bits we accepted (low 32) */
bool virtio_negotiate(vdev_t *v, uint32_t want, uint32_t *got) {
    set_status(v, 0);
    uint32_t t0 = timer_ticks();
    while (get_status(v) && timer_ticks() - t0 < 1000) {}
    set_status(v, 1);                                         /* ACKNOWLEDGE */
    set_status(v, 1 | 2);                                     /* DRIVER */
    if (v->modern) {
        w32(v, 0x00, 1);
        if (!(c32(v, 0x04) & 1)) return false;                /* VIRTIO_F_VERSION_1 */
        w32(v, 0x00, 0);
        *got = c32(v, 0x04) & want;
        w32(v, 0x08, 0); w32(v, 0x0C, *got);
        w32(v, 0x08, 1); w32(v, 0x0C, 1);
        set_status(v, 1 | 2 | 8);                             /* FEATURES_OK */
        if (!(get_status(v) & 8)) return false;
    } else {
        *got = inl(v->io + 0x00) & want;
        outl(v->io + 0x04, *got);
    }
    return true;
}

bool virtio_queue_setup(vdev_t *v, virtq_t *q, uint16_t index, uint16_t max) {
    uint16_t size;
    if (v->modern) {
        w16(v, 0x16, index);
        size = c16(v, 0x18);
        if (!size) return false;
        if (size > max) size = max;
        w16(v, 0x18, size);
    } else {
        outw(v->io + 0x0E, index);
        size = inw(v->io + 0x0C);                             /* fixed by the device */
        if (!size) return false;
    }
    uint32_t avail_off = 16u * size, used_off = (avail_off + 6 + 2u * size + 4095) & ~4095u;
    uint32_t total = used_off + 6 + 8u * size;
    uint8_t *mem = kmalloc_aligned((total + 4095) & ~4095u, 4096);
    memset(mem, 0, total);
    q->index = index; q->size = size; q->last_used = 0;
    q->desc = (vq_desc_t *)mem;
    q->avail = (volatile uint16_t *)(mem + avail_off);
    q->used = mem + used_off;
    q->avail[0] = 1;                                          /* VRING_AVAIL_F_NO_INTERRUPT: we poll */
    if (v->modern) {
        uint64_t d = dma_addr(mem), a = d + avail_off, u = d + used_off;
        w32(v, 0x20, (uint32_t)d); w32(v, 0x24, (uint32_t)(d >> 32));
        w32(v, 0x28, (uint32_t)a); w32(v, 0x2C, (uint32_t)(a >> 32));
        w32(v, 0x30, (uint32_t)u); w32(v, 0x34, (uint32_t)(u >> 32));
        q->notify_off = c16(v, 0x1E);
        w16(v, 0x1C, 1);                                      /* queue_enable */
    } else {
        outl(v->io + 0x08, (uint32_t)(dma_addr(mem) >> 12));
    }
    return true;
}

void virtio_notify(vdev_t *v, virtq_t *q) {
    if (v->modern) *(volatile uint16_t *)(v->notify + q->notify_off * v->notify_mult) = q->index;
    else outw(v->io + 0x10, q->index);
}
void virtio_driver_ok(vdev_t *v) { set_status(v, v->modern ? (1 | 2 | 8 | 4) : (1 | 2 | 4)); }

/* run one descriptor chain and wait for the device to return it */
static bool vq_run(vdev_t *v, int n, void *const bufs[], const uint32_t lens[], const bool dev_writes[]) {
    virtq_t *q = &v->q;
    for (int i = 0; i < n; i++) {
        q->desc[i].addr = dma_addr(bufs[i]); q->desc[i].len = lens[i];
        q->desc[i].flags = (i + 1 < n ? VQ_NEXT : 0) | (dev_writes[i] ? VQ_WRITE : 0);
        q->desc[i].next = i + 1;
    }
    uint16_t idx = q->avail[1];
    q->avail[2 + idx % q->size] = 0;
    __asm__ volatile("mfence" ::: "memory");
    q->avail[1] = idx + 1;
    __asm__ volatile("mfence" ::: "memory");
    virtio_notify(v, q);
    uint32_t t0 = timer_ticks();
    while (*(volatile uint16_t *)(q->used + 2) == q->last_used)
        if (timer_ticks() - t0 > 10000) { klog("virtio: request timed out"); return false; }
    q->last_used++;
    return true;
}

/* find the transport: modern capabilities if reachable, else legacy BAR0 */
bool virtio_transport(pci_dev_t *pd, vdev_t *v) {
    uint32_t status = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x04) >> 16;
    if (status & 0x10) {
        volatile uint8_t *common = NULL, *notify = NULL, *devcfg = NULL;
        uint32_t mult = 0;
        uint8_t ptr = pci_cfg_read(pd->bus, pd->dev, pd->fn, 0x34) & 0xFC;
        for (int guard = 0; ptr && guard < 48; guard++) {
            uint32_t h = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr);
            if ((h & 0xFF) == 0x09) {
                uint8_t type = (h >> 24) & 0xFF;
                uint8_t bar = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 4) & 0xFF;
                uint32_t off = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 8);
                bool isio = false;
                uintptr_t base = bar < 6 ? pci_bar(pd, bar, &isio) : 0;
                volatile uint8_t *p = base && !isio ? (volatile uint8_t *)(base + off) : NULL;
                if (type == 1 && !common) common = p;
                else if (type == 2 && !notify) { notify = p; mult = pci_cfg_read(pd->bus, pd->dev, pd->fn, ptr + 16); }
                else if (type == 4 && !devcfg) devcfg = p;
            }
            ptr = (h >> 8) & 0xFC;
        }
        if (common && notify && devcfg) {
            v->modern = true; v->common = common; v->notify = notify; v->devcfg = devcfg; v->notify_mult = mult;
            pci_enable(pd, false, true);
            return true;
        }
    }
    bool isio;
    uintptr_t b0 = pci_bar(pd, 0, &isio);
    if (b0 && isio) { v->io = b0; pci_enable(pd, true, false); return true; }
    return false;
}

/* ---- virtio-blk ---- */
typedef struct { vdev_t v; bool can_flush; uint8_t *hdr, *buf; } vblk_t;

static bool vblk_req(blkdev_t *d, uint32_t type, uint64_t blk, uint32_t n, bool dev_writes) {
    vblk_t *b = d->priv;
    uint64_t sector = blk * (d->bsize / 512);
    memset(b->hdr, 0, 32);
    memcpy(b->hdr, &type, 4); memcpy(b->hdr + 8, &sector, 8);
    uint8_t *status = b->hdr + 16;
    *status = 0xFF;
    uint32_t bytes = n * d->bsize;
    void *bufs[3] = { b->hdr, b->buf, status };
    uint32_t lens[3] = { 16, bytes, 1 };
    bool wr_[3] = { false, dev_writes, true };
    bool ok = bytes ? vq_run(&b->v, 3, bufs, lens, wr_)
                    : vq_run(&b->v, 2, (void *const[]){ b->hdr, status }, (const uint32_t[]){ 16, 1 }, (const bool[]){ false, true });
    return ok && *status == 0;
}
static bool vblk_read(blkdev_t *d, uint64_t blk, uint32_t n, void *buf) {
    vblk_t *b = d->priv;
    if (!vblk_req(d, 0, blk, n, true)) { klog("virtio-blk: read failed at %u", (uint32_t)blk); return false; }
    memcpy(buf, b->buf, n * d->bsize);
    return true;
}
static bool vblk_write(blkdev_t *d, uint64_t blk, uint32_t n, const void *buf) {
    vblk_t *b = d->priv;
    memcpy(b->buf, buf, n * d->bsize);
    if (!vblk_req(d, 1, blk, n, false)) { klog("virtio-blk: write failed at %u", (uint32_t)blk); return false; }
    return true;
}
static bool vblk_flush(blkdev_t *d) {
    vblk_t *b = d->priv;
    return !b->can_flush || vblk_req(d, 4, 0, 0, false);
}
static const blkops_t vblk_ops = { vblk_read, vblk_write, vblk_flush };

static int nvblk;
static void vblk_init(pci_dev_t *pd) {
    vblk_t *b = kzalloc(sizeof *b);
    uint32_t got;
    if (!virtio_transport(pd, &b->v) || !virtio_negotiate(&b->v, (1u << 6) | (1u << 9), &got) || !virtio_queue_setup(&b->v, &b->v.q, 0, 64)) {
        klog("virtio-blk: %02x:%02x.%d: setup failed", pd->bus, pd->dev, pd->fn); return;
    }
    set_status(&b->v, b->v.modern ? (1 | 2 | 8 | 4) : (1 | 2 | 4));   /* DRIVER_OK */
    b->can_flush = got & (1u << 9);
    b->hdr = kmalloc_aligned(64, 64);
    b->buf = kmalloc_aligned(65536, 4096);
    blkdev_t *d = blk_register();
    if (!d) return;
    uint64_t cap = cfg64(&b->v, 0);
    uint32_t bs = (got & (1u << 6)) ? cfg32(&b->v, 20) : 512;
    if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096) bs = 512;
    d->bsize = bs;
    d->sectors = cap > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)cap;
    d->max_blocks = 65536 / bs;
    strcpy(d->model, "VirtIO Block Device");
    d->ops = &vblk_ops; d->priv = b; d->kind = "virtio"; d->driver = "viostor";
    d->busno = nvblk++; d->id = 0;
    blk_commit(d);
}

/* ---- virtio-scsi ---- */
typedef struct { vdev_t v; uint32_t cdb_size, sense_size; uint8_t *req, *resp, *buf; scsi_host_t host; } vscsi_t;

static int vscsi_exec(scsi_host_t *h, int target, int lun, const uint8_t *cdb, int cdblen, void *buf, uint32_t len, int dir) {
    vscsi_t *s = h->priv;
    if (len > 65536 || (uint32_t)cdblen > s->cdb_size) return -1;
    uint32_t reqlen = 19 + s->cdb_size, resplen = 12 + s->sense_size;
    memset(s->req, 0, reqlen); memset(s->resp, 0, resplen);
    s->req[0] = 1; s->req[1] = target; s->req[2] = 0x40 | ((lun >> 8) & 0x3F); s->req[3] = lun & 0xFF;
    memcpy(s->req + 19, cdb, cdblen);
    s->resp[11] = 0xFF;
    if (dir == 2) memcpy(s->buf, buf, len);
    void *bufs[3]; uint32_t lens[3]; bool w[3]; int n = 0;
    bufs[n] = s->req; lens[n] = reqlen; w[n++] = false;
    if (dir == 2 && len) { bufs[n] = s->buf; lens[n] = len; w[n++] = false; }
    bufs[n] = s->resp; lens[n] = resplen; w[n++] = true;
    if (dir == 1 && len) { bufs[n] = s->buf; lens[n] = len; w[n++] = true; }
    if (!vq_run(&s->v, n, bufs, lens, w)) return -1;
    uint8_t response = s->resp[11], status = s->resp[10];
    if (response != 0) return -1;                             /* bad target, reset, failure... */
    if (dir == 1 && !status) memcpy(buf, s->buf, len);
    return status;
}

static int nvscsi;
static void vscsi_init(pci_dev_t *pd) {
    vscsi_t *s = kzalloc(sizeof *s);
    uint32_t got;
    if (!virtio_transport(pd, &s->v) || !virtio_negotiate(&s->v, 0, &got) || !virtio_queue_setup(&s->v, &s->v.q, 2, 64)) {
        klog("virtio-scsi: %02x:%02x.%d: setup failed", pd->bus, pd->dev, pd->fn); return;
    }
    set_status(&s->v, s->v.modern ? (1 | 2 | 8 | 4) : (1 | 2 | 4));
    s->sense_size = cfg32(&s->v, 20); s->cdb_size = cfg32(&s->v, 24);
    if (s->sense_size > 252 || !s->sense_size) s->sense_size = 96;
    if (s->cdb_size > 64 || s->cdb_size < 16) s->cdb_size = 32;
    uint32_t max_sectors = cfg32(&s->v, 8);
    uint16_t max_target = cfg16(&s->v, 30);
    uint32_t max_lun = cfg32(&s->v, 32);
    s->req = kmalloc_aligned(128, 64); s->resp = kmalloc_aligned(512, 64);
    s->buf = kmalloc_aligned(65536, 4096);
    s->host.exec = vscsi_exec; s->host.priv = s; s->host.driver = "vioscsi";
    s->host.busno = nvscsi++;
    s->host.max_target = MIN(max_target, 63);
    s->host.max_lun = MIN(max_lun, 255u);
    s->host.max_xfer = (max_sectors && max_sectors < 128) ? max_sectors * 512 : 65536;
    klog("virtio-scsi: %s transport, targets 0-%d", s->v.modern ? "modern" : "legacy", s->host.max_target);
    scsi_scan(&s->host);
}

void virtio_probe(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n; i++) {
        if (devs[i].vendor != 0x1AF4) continue;
        uint16_t id = devs[i].device;
        if (id == 0x1001 || id == 0x1042) vblk_init(&devs[i]);
        else if (id == 0x1004 || id == 0x1048) vscsi_init(&devs[i]);
    }
}
