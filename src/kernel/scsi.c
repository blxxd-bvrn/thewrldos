/* SCSI disks behind any host adapter driver (LSI Logic Fusion-MPT, VMware
 * PVSCSI, virtio-scsi): discovery (INQUIRY, REPORT LUNS), capacity, and
 * READ/WRITE(10/16) + SYNCHRONIZE CACHE. */
#include "kernel.h"
#include "disk.h"

typedef struct { scsi_host_t *h; int target, lun; bool rw16; } scsi_disk_t;

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* a command, retried while the device reports UNIT ATTENTION / BECOMING READY */
static int cmd(scsi_host_t *h, int t, int l, const uint8_t *cdb, int cl, void *buf, uint32_t len, int dir) {
    int st = -1;
    for (int tries = 0; tries < 4; tries++) {
        st = h->exec(h, t, l, cdb, cl, buf, len, dir);
        if (st != 2 && st != 8) return st;                /* not CHECK CONDITION / BUSY */
    }
    return st;
}

static bool sd_rw(blkdev_t *d, uint64_t blk, uint32_t n, void *buf, bool write) {
    scsi_disk_t *s = d->priv;
    uint8_t cdb[16] = { 0 };
    int cl;
    if (s->rw16 || blk + n > 0xFFFFFFFFull) {
        cdb[0] = write ? 0x8A : 0x88;
        put_be32(cdb + 2, (uint32_t)(blk >> 32)); put_be32(cdb + 6, (uint32_t)blk);
        put_be32(cdb + 10, n);
        cl = 16;
    } else {
        cdb[0] = write ? 0x2A : 0x28;
        put_be32(cdb + 2, (uint32_t)blk);
        cdb[7] = n >> 8; cdb[8] = n;
        cl = 10;
    }
    int st = cmd(s->h, s->target, s->lun, cdb, cl, buf, n * d->bsize, write ? 2 : 1);
    if (st) klog("scsi: %s target %d: %s failed at %u (status %d)", s->h->driver, s->target, write ? "write" : "read", (uint32_t)blk, st);
    return st == 0;
}
static bool sd_read(blkdev_t *d, uint64_t blk, uint32_t n, void *buf) { return sd_rw(d, blk, n, buf, false); }
static bool sd_write(blkdev_t *d, uint64_t blk, uint32_t n, const void *buf) { return sd_rw(d, blk, n, (void *)buf, true); }
static bool sd_flush(blkdev_t *d) {
    scsi_disk_t *s = d->priv;
    uint8_t cdb[10] = { 0x35 };
    int st = cmd(s->h, s->target, s->lun, cdb, 10, NULL, 0, 0);
    return st == 0 || st == 2;                         /* some devices refuse SYNCHRONIZE CACHE: fine */
}
static const blkops_t sd_ops = { sd_read, sd_write, sd_flush };

static void probe_lun(scsi_host_t *h, int t, int l, uint8_t *buf) {
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    memset(buf, 0, 36);
    if (cmd(h, t, l, inq, 6, buf, 36, 1)) return;
    if ((buf[0] >> 5) != 0) return;                     /* nothing connected at this LUN */
    uint8_t type = buf[0] & 0x1F;
    if (type != 0x00 && type != 0x0E) return;           /* only disks (CD-ROMs etc. are skipped) */
    char model[41]; int k = 0;
    for (int i = 8; i < 16; i++) model[k++] = buf[i] >= 32 && buf[i] < 127 ? buf[i] : ' ';
    while (k && model[k - 1] == ' ') k--;
    model[k++] = ' ';
    for (int i = 16; i < 32; i++) model[k++] = buf[i] >= 32 && buf[i] < 127 ? buf[i] : ' ';
    while (k && model[k - 1] == ' ') k--;
    model[k] = 0;
    uint8_t tur[6] = { 0 };
    for (int i = 0; i < 6 && cmd(h, t, l, tur, 6, NULL, 0, 0); i++) {}
    uint8_t rc10[10] = { 0x25 };
    memset(buf, 0, 32);
    if (cmd(h, t, l, rc10, 10, buf, 8, 1)) { klog("scsi: %s target %d lun %d: READ CAPACITY failed", h->driver, t, l); return; }
    uint64_t last = be32(buf); uint32_t bsz = be32(buf + 4);
    bool rw16 = false;
    if (last == 0xFFFFFFFF) {
        uint8_t rc16[16] = { 0x9E, 0x10 };
        rc16[13] = 32;
        memset(buf, 0, 32);
        if (cmd(h, t, l, rc16, 16, buf, 32, 1)) return;
        last = (uint64_t)be32(buf) << 32 | be32(buf + 4); bsz = be32(buf + 8);
        rw16 = true;
    }
    if (bsz != 512 && bsz != 1024 && bsz != 2048 && bsz != 4096) { klog("scsi: target %d: %u-byte blocks unsupported", t, bsz); return; }
    blkdev_t *d = blk_register();
    if (!d) return;
    scsi_disk_t *s = kzalloc(sizeof *s); s->h = h; s->target = t; s->lun = l; s->rw16 = rw16;
    uint64_t secs = (last + 1) * (bsz / 512);
    d->sectors = secs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)secs;
    d->bsize = bsz;
    d->max_blocks = h->max_xfer / bsz;
    strncpy(d->model, model, sizeof d->model - 1);
    d->ops = &sd_ops; d->priv = s; d->kind = "SCSI"; d->driver = h->driver;
    d->busno = h->busno; d->id = t; d->lun = l;
    blk_commit(d);
}

void scsi_scan(scsi_host_t *h) {
    uint8_t *buf = kmalloc(512);
    for (int t = 0; t <= h->max_target && blk_count() < BLK_MAX; t++) {
        /* does the target answer at all? (an empty LUN 0 still answers INQUIRY) */
        uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
        memset(buf, 0, 36);
        if (h->exec(h, t, 0, inq, 6, buf, 36, 1) < 0) continue;
        int luns[32], nl = 0;
        if (h->max_lun > 0) {                           /* REPORT LUNS */
            uint8_t rl[12] = { 0xA0 };
            put_be32(rl + 6, 512);
            memset(buf, 0, 512);
            if (cmd(h, t, 0, rl, 12, buf, 512, 1) == 0) {
                uint32_t n = be32(buf) / 8;
                for (uint32_t i = 0; i < n && i < 63 && nl < 32; i++) {
                    const uint8_t *e = buf + 8 + i * 8;
                    int lun = ((e[0] & 0x3F) << 8) | e[1];
                    if (lun <= h->max_lun) luns[nl++] = lun;
                }
            }
        }
        for (int i = 0; i < nl; i++) probe_lun(h, t, luns[i], buf);
        bool any = nl > 0;
        if (!any) probe_lun(h, t, 0, buf);
    }
    kfree(buf);
}
