/* Block device layer: the registry of disks from every controller driver,
 * 512-byte sector I/O on top of native block sizes, and MBR partition tables. */
#include "kernel.h"
#include "disk.h"

static blkdev_t *disks[BLK_MAX];
static int ndisks;
static bool probed;
char blk_failed[160];

blkdev_t *blk_register(void) {
    if (ndisks >= BLK_MAX) return NULL;
    blkdev_t *d = kzalloc(sizeof *d);
    d->bsize = 512; d->max_blocks = 128; d->lun = 0;
    return d;
}
void blk_commit(blkdev_t *d) {
    if (!d->max_blocks) d->max_blocks = 1;
    if (!d->bsize) d->bsize = 512;
    if (!d->total_sectors) d->total_sectors = d->sectors;
    d->index = ndisks;
    disks[ndisks++] = d;
    char b[128]; blk_describe(d, b, sizeof b);
    klog("disk %d: %s", d->index, b);
}
/* A disk that is gone leaves the list, so it is no longer numbered or opened;
 * its memory stays because a mounted volume (or an open handle) may still hold
 * the pointer, and every read of it now fails instead of touching the device. */
void blk_unregister(blkdev_t *d) {
    if (!d || d->dead) return;
    for (int i = 0; i < ndisks; i++) {
        if (disks[i] != d) continue;
        for (int j = i; j + 1 < ndisks; j++) { disks[j] = disks[j + 1]; disks[j]->index = j; }
        ndisks--;
        disks[ndisks] = NULL;
        d->dead = true;
        d->index = -1;
        klog("disk: \"%s\" removed", d->model);
        return;
    }
}
void storage_init(void) {
    if (probed) return;
    probed = true;
    ata_probe();          /* IDE (PIIX/ICH, and SATA controllers in IDE mode) */
    ahci_probe();         /* SATA AHCI */
    nvme_probe();         /* NVMe */
    mpt_probe();          /* LSI Logic Fusion-MPT: 53C1030 parallel SCSI, SAS1068 */
    pvscsi_probe();       /* VMware Paravirtual SCSI */
    virtio_probe();       /* virtio-blk, virtio-scsi */
    if (!ndisks) klog("storage: no hard disks found");
}
int blk_count(void) { return ndisks; }
blkdev_t *blk_get(int i) { return i >= 0 && i < ndisks ? disks[i] : NULL; }
void blk_describe(blkdev_t *d, char *buf, int cap) {
    char where[24];
    if (!strcmp(d->kind, "IDE")) snprintf(where, sizeof where, "%s %s", d->bus ? "secondary" : "primary", d->slave ? "slave" : "master");
    else if (!strcmp(d->kind, "NVMe")) snprintf(where, sizeof where, "namespace %d", d->lun);
    else if (!strcmp(d->kind, "SATA")) snprintf(where, sizeof where, "port %d", d->id);
    else if (!strcmp(d->kind, "virtio")) snprintf(where, sizeof where, "disk %d", d->busno);
    else snprintf(where, sizeof where, "target %d lun %d", d->id, d->lun);
    uint64_t mb = (d->total_sectors ? d->total_sectors : d->sectors) / 2048;
    snprintf(buf, cap, "%s %s (%s) \"%s\" %llu MB%s%s", d->kind, where, d->driver, d->model, (unsigned long long)mb,
             d->bsize != 512 ? " 4K" : "", d->total_sectors > d->sectors ? " [2 TiB addressable]" : "");
}

/* whole native blocks, split into requests the device accepts */
static bool native_io(blkdev_t *d, uint64_t blk, uint32_t n, uint8_t *p, bool wr) {
    while (n) {
        uint32_t k = MIN(n, d->max_blocks);
        if (!(wr ? d->ops->write(d, blk, k, p) : d->ops->read(d, blk, k, p))) return false;
        blk += k; n -= k; p += (size_t)k * d->bsize;
    }
    return true;
}
static uint8_t *scratch;                 /* one native block, for disks with 4 KiB sectors */
static bool io(blkdev_t *d, uint32_t lba, uint32_t count, uint8_t *p, bool wr) {
    if (!d || d->dead || lba + count > d->sectors || lba + count < lba) return false;
    uint32_t spb = d->bsize / 512;
    if (spb <= 1) return native_io(d, lba, count, p, wr);
    if (!scratch) scratch = kmalloc_aligned(4096, 4096);
    while (count) {
        uint64_t blk = lba / spb; uint32_t off = lba % spb;
        if (!off && count >= spb) {
            uint32_t nb = count / spb;
            if (!native_io(d, blk, nb, p, wr)) return false;
            lba += nb * spb; count -= nb * spb; p += (size_t)nb * d->bsize;
            continue;
        }
        uint32_t take = MIN(spb - off, count);        /* part of one block: read-modify-write */
        if (d->bsize > 4096 || !d->ops->read(d, blk, 1, scratch)) return false;
        if (wr) { memcpy(scratch + off * 512, p, take * 512); if (!d->ops->write(d, blk, 1, scratch)) return false; }
        else memcpy(p, scratch + off * 512, take * 512);
        lba += take; count -= take; p += take * 512;
    }
    return true;
}
bool blk_read(blkdev_t *d, uint32_t lba, uint32_t count, void *buf) { return io(d, lba, count, buf, false); }
bool blk_write(blkdev_t *d, uint32_t lba, uint32_t count, const void *buf) {
    if (!io(d, lba, count, (uint8_t *)buf, true)) return false;
    return !d->ops->flush || d->ops->flush(d);
}
bool blk_zero(blkdev_t *d, uint32_t lba, uint32_t count) {
    static uint8_t *z;
    if (!z) z = kzalloc(128 * 512);
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (!blk_write(d, lba, n, z)) return false;
        lba += n; count -= n;
    }
    return true;
}

/* ---------------- MBR ---------------- */
int mbr_read(blkdev_t *d, part_t parts[4]) {
    uint8_t s[512];
    memset(parts, 0, sizeof(part_t) * 4);
    if (!blk_read(d, 0, 1, s)) return 0;
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = s + 446 + i * 16;
        parts[i].boot = e[0]; parts[i].type = e[4];
        memcpy(&parts[i].lba, e + 8, 4); memcpy(&parts[i].sectors, e + 12, 4);
        if (!parts[i].type || !parts[i].sectors) memset(&parts[i], 0, sizeof(part_t));
    }
    return 1;
}
static void lba_to_chs(uint32_t lba, uint8_t *out) {
    uint32_t c = lba / (255 * 63), h = (lba / 63) % 255, s = lba % 63 + 1;
    if (c > 1023) { out[0] = 0xFE; out[1] = 0xFF; out[2] = 0xFF; return; }
    out[0] = h; out[1] = (s & 0x3F) | ((c >> 2) & 0xC0); out[2] = c & 0xFF;
}
bool mbr_write(blkdev_t *d, const part_t parts[4], const uint8_t *bootcode) {
    uint8_t s[512];
    if (!blk_read(d, 0, 1, s) || s[510] != 0x55 || s[511] != 0xAA) {
        memset(s, 0, 512);
        uint32_t sig = rand_next(); memcpy(s + 440, &sig, 4);
    }
    if (bootcode) memcpy(s, bootcode, 440);
    if (!*(uint32_t *)(s + 440)) { uint32_t sig = rand_next() | 1; memcpy(s + 440, &sig, 4); }
    s[444] = s[445] = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *e = s + 446 + i * 16;
        memset(e, 0, 16);
        if (!parts[i].type) continue;
        e[0] = parts[i].boot ? 0x80 : 0; e[4] = parts[i].type;
        lba_to_chs(parts[i].lba, e + 1); lba_to_chs(parts[i].lba + parts[i].sectors - 1, e + 5);
        memcpy(e + 8, &parts[i].lba, 4); memcpy(e + 12, &parts[i].sectors, 4);
    }
    s[510] = 0x55; s[511] = 0xAA;
    return blk_write(d, 0, 1, s);
}
const char *part_type_name(uint8_t t) {
    switch (t) {
    case 0x01: return "FAT12"; case 0x04: case 0x06: case 0x0E: return "FAT16"; case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT"; case 0x05: case 0x0F: return "Extended"; case 0x82: return "Linux swap"; case 0x83: return "Linux";
    case 0xEE: return "GPT protective"; case 0xEF: return "EFI System"; case 0xA5: return "FreeBSD"; case 0xAF: return "HFS+";
    default: return "Unknown";
    }
}

/* for the removable-media layer (rust/src/kernel/mediak.rs) */
uint64_t blk_bytes(blkdev_t *d) { return d && !d->dead ? (uint64_t)d->sectors * 512 : 0; }
bool blk_dead(blkdev_t *d) { return !d || d->dead; }
