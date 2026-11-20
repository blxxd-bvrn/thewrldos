#!/usr/bin/env python3
"""Writes the live CD's copy of the kernel as a parts file (boot/wrld.kpt) instead of the whole
ELF: the kernel's read-only segments (code and constant data, most of the file) are left out,
because Setup takes them from the running kernel's memory, where GRUB loaded them unchanged
(src/apps/setup.c, kernel_image). That keeps ~7 MB less in RAM on the live CD.

Format, little-endian:
    "WRLDKPT1"  u32 file size  u32 CRC-32 of the file  u32 range count
    per range:  u32 file offset  u32 length  u64 load address
    the file's other bytes, in order

Usage: kparts.py wrld.elf wrld.kpt
"""
import struct, sys, zlib

PT_LOAD, PF_W = 1, 2


def ranges(elf):
    if elf[:4] != b"\x7fELF":
        sys.exit("kparts: not an ELF file")
    is64 = elf[4] == 2
    if is64:
        phoff, = struct.unpack_from("<Q", elf, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", elf, 0x36)
    else:
        phoff, = struct.unpack_from("<I", elf, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    out = []
    for i in range(phnum):
        o = phoff + i * phentsize
        if is64:
            ptype, flags, off, vaddr, paddr, filesz = struct.unpack_from("<IIQQQQ", elf, o)
        else:
            ptype, off, vaddr, paddr, filesz, _memsz, flags = struct.unpack_from("<IIIIIII", elf, o)
        # read-only, loaded where it runs (identity mapped), and not overlapping the headers
        if ptype == PT_LOAD and not flags & PF_W and filesz and vaddr == paddr and off >= 4096:
            out.append((off, filesz, vaddr))
    out.sort()
    for a, b in zip(out, out[1:]):
        if a[0] + a[1] > b[0]:
            sys.exit("kparts: overlapping segments")
    return out


def build(elf, rs):
    hdr = b"WRLDKPT1" + struct.pack("<III", len(elf), zlib.crc32(elf), len(rs))
    hdr += b"".join(struct.pack("<IIQ", *r) for r in rs)
    rest, pos = bytearray(), 0
    for off, ln, _ in rs:
        rest += elf[pos:off]
        pos = off + ln
    rest += elf[pos:]
    return hdr + bytes(rest)


def rebuild(kpt, mem):
    """What Setup does, with `mem(vaddr, n)` standing in for the kernel's memory."""
    total, crc, n = struct.unpack_from("<III", kpt, 8)
    rs = [struct.unpack_from("<IIQ", kpt, 20 + 16 * i) for i in range(n)]
    rest, out, rp = kpt[20 + 16 * n:], bytearray(), 0
    for off, ln, va in rs:
        k = off - len(out)
        out += rest[rp:rp + k]; rp += k
        out += mem(va, ln)
    out += rest[rp:]
    return bytes(out) if len(out) == total and zlib.crc32(out) == crc else None


def main():
    elf = open(sys.argv[1], "rb").read()
    rs = ranges(elf)
    if not rs:
        sys.exit("kparts: no read-only segments")
    kpt = build(elf, rs)
    seg = {va: elf[off:off + ln] for off, ln, va in rs}
    if rebuild(kpt, lambda va, n: seg[va][:n]) != elf:
        sys.exit("kparts: the parts file does not rebuild the kernel")
    open(sys.argv[2], "wb").write(kpt)
    print("kparts: %s: %d KB of %d KB kept (%d segments from memory)" % (sys.argv[2], len(kpt) // 1024, len(elf) // 1024, len(rs)))


if __name__ == "__main__":
    main()
