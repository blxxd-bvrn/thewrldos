/* Apple Disk Image (UDIF .dmg) support: koly trailer, XML plist blkx table,
 * mish chunk maps (raw / zero / zlib), MBR/GPT partition maps, then FAT mount. */
#include "kernel.h"
#include "fs.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62; if (c == '/') return 63;
    return -1;
}
static uint8_t *b64decode(const char *s, const char *e, uint32_t *outlen) {
    uint8_t *out = kmalloc((e - s) * 3 / 4 + 4);
    uint32_t n = 0, acc = 0; int bits = 0;
    for (; s < e; s++) {
        int v = b64val(*s);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (acc >> bits) & 0xFF; }
    }
    *outlen = n;
    return out;
}
static const char *memfind(const char *h, const char *end, const char *needle) {
    int l = strlen(needle);
    for (; h + l <= end; h++) if (!memcmp(h, needle, l)) return h;
    return NULL;
}

uint8_t *dmg_extract(const uint8_t *file, uint32_t size, uint32_t *out_size, char *info, int icap) {
    info[0] = 0;
    if (size < 512 || memcmp(file + size - 512, "koly", 4)) {
        /* not UDIF: treat as a raw (read/write, UDRW-less) disk image */
        snprintf(info, icap, "Raw disk image (no UDIF trailer)");
        uint8_t *copy = kmalloc(size); memcpy(copy, file, size); *out_size = size; return copy;
    }
    const uint8_t *k = file + size - 512;
    uint64_t data_off = be64(k + 24);
    uint64_t xml_off = be64(k + 216), xml_len = be64(k + 224);
    uint64_t sectors = be64(k + 492);
    if (!xml_len || xml_off + xml_len > size || sectors == 0 || sectors > 256 * 1024 * 2) {
        snprintf(info, icap, "Unsupported UDIF image (no XML block map, or image larger than 256 MB)"); return NULL;
    }
    uint32_t disk_size = (uint32_t)sectors * 512;
    uint8_t *disk = kzalloc(disk_size);
    if (!disk) { snprintf(info, icap, "Not enough memory"); return NULL; }
    const char *x = (const char *)file + xml_off, *xe = x + xml_len;
    int nblk = 0, nchunks = 0; uint32_t comp = 0;
    const char *p = memfind(x, xe, "<key>blkx</key>");
    if (!p) { kfree(disk); snprintf(info, icap, "UDIF plist has no blkx table"); return NULL; }
    while ((p = memfind(p, xe, "<data>"))) {
        const char *de = memfind(p, xe, "</data>");
        if (!de) break;
        uint32_t mlen; uint8_t *m = b64decode(p + 6, de, &mlen);
        p = de;
        if (mlen < 204 || memcmp(m, "mish", 4)) { kfree(m); continue; }
        nblk++;
        uint64_t base = be64(m + 8);
        uint32_t nch = be32(m + 200);
        for (uint32_t i = 0; i < nch && 204 + (i + 1) * 40 <= mlen; i++) {
            const uint8_t *c = m + 204 + i * 40;
            uint32_t type = be32(c);
            uint64_t sec = base + be64(c + 8), cnt = be64(c + 16), coff = be64(c + 24), clen = be64(c + 32);
            if (type == 0xFFFFFFFF || type == 0x7FFFFFFE) continue;
            if ((sec + cnt) * 512 > disk_size) continue;
            uint8_t *dst = disk + sec * 512;
            uint32_t want = (uint32_t)cnt * 512;
            nchunks++;
            if (type == 0 || type == 2) memset(dst, 0, want);
            else if (type == 1) { if (data_off + coff + clen <= size) memcpy(dst, file + data_off + coff, MIN(clen, want)); }
            else if (type == 0x80000005) {
                uint32_t got;
                if (data_off + coff + clen > size || inflate_zlib(file + data_off + coff, clen, dst, want, &got)) {
                    snprintf(info, icap, "zlib chunk failed to decompress"); kfree(m); kfree(disk); return NULL;
                }
                comp++;
            } else {
                const char *nm = type == 0x80000004 ? "ADC" : type == 0x80000006 ? "bzip2" : type == 0x80000007 ? "LZFSE" : type == 0x80000008 ? "LZMA" : "unknown";
                snprintf(info, icap, "Compression '%s' (0x%08x) is not supported. Convert with: hdiutil convert -format UDZO", nm, type);
                kfree(m); kfree(disk); return NULL;
            }
        }
        kfree(m);
    }
    snprintf(info, icap, "UDIF image: %u sectors, %d partition maps, %d chunks (%u zlib)", (uint32_t)sectors, nblk, nchunks, comp);
    *out_size = disk_size;
    return disk;
}

static int find_partition(const uint8_t *d, uint32_t size, uint32_t *off, uint32_t *len, char *why, int wcap) {
    /* GPT */
    if (size > 1024 && !memcmp(d + 512, "EFI PART", 8)) {
        uint32_t ent = *(const uint32_t *)(d + 512 + 72) * 512, n = *(const uint32_t *)(d + 512 + 80), esz = *(const uint32_t *)(d + 512 + 84);
        for (uint32_t i = 0; i < n && i < 128; i++) {
            const uint8_t *e = d + ent + i * esz;
            if (e + 48 > d + size) break;
            uint64_t first = *(const uint64_t *)(e + 32), last = *(const uint64_t *)(e + 40);
            if (!first) continue;
            const uint8_t *s = d + first * 512;
            if (first * 512 + 1536 > size) continue;
            if (s[510] == 0x55 && s[511] == 0xAA) { *off = first * 512; *len = (last - first + 1) * 512; return 1; }
            if (s[1024] == 'H' && (s[1025] == '+' || s[1025] == 'X')) { snprintf(why, wcap, "HFS+ volume found (GPT partition %u)", i + 1); return -1; }
            if (!memcmp(s + 32, "NXSB", 4)) { snprintf(why, wcap, "APFS container found (GPT partition %u)", i + 1); return -1; }
        }
    }
    /* MBR */
    if (d[510] == 0x55 && d[511] == 0xAA && d[0] != 0xEB && d[0] != 0xE9) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = d + 446 + i * 16;
            uint32_t lba = *(const uint32_t *)(e + 8), cnt = *(const uint32_t *)(e + 12);
            if (e[4] && lba && (uint64_t)(lba + cnt) * 512 <= size) { *off = lba * 512; *len = cnt * 512; return 1; }
        }
    }
    if (size > 1536 && d[1024] == 'H' && (d[1025] == '+' || d[1025] == 'X')) { snprintf(why, wcap, "HFS+ volume found"); return -1; }
    if (size > 64 && !memcmp(d + 32, "NXSB", 4)) { snprintf(why, wcap, "APFS container found"); return -1; }
    *off = 0; *len = size;
    return 1;
}

char dmg_mount(vnode_t *file, char *msg, int mcap) {
    char info[200];
    uint32_t dsz;
    uint8_t *disk = dmg_extract(file->data, file->size, &dsz, info, sizeof info);
    if (!disk) { snprintf(msg, mcap, "%s", info); return 0; }
    uint32_t off, len; char why[120] = "";
    int r = find_partition(disk, dsz, &off, &len, why, sizeof why);
    if (r < 0) {
        snprintf(msg, mcap, "%s. %s - this file system is not supported yet (FAT12/16/32 volumes are).", info, why);
        kfree(disk); return 0;
    }
    char label[32], err[100];
    vnode_t *root = fat_mount_image(disk + off, len, label, sizeof label, err, sizeof err);
    kfree(disk);
    if (!root) { snprintf(msg, mcap, "%s. Could not mount volume: %s", info, err); return 0; }
    char l = vfs_mount(root, label, 2);
    if (!l) { snprintf(msg, mcap, "No free drive letters"); return 0; }
    snprintf(msg, mcap, "Mounted '%s' as %c: (%s)", label, l, info);
    klog("dmg: %s", msg);
    return l;
}
