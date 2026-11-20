#!/usr/bin/env python3
"""Writes the disk-image test vectors for rust/src/fat.rs and rust/src/dmg.rs into rust/testdata/.

FAT volumes are made with mkfs.fat and mtools, so they are laid out the way real tools do it:
  fat12.img.z, fat16.img.z, fat32.img.z    zlib-compressed volume images
  fat12.txt, fat16.txt, fat32.txt          what each holds (see below)
DMG images wrap such volumes in UDIF (the writer in tools/mkdmg.py) with different chunk
kinds and partition maps:
  dmg_*.dmg.z (zlib-compressed), and dmg_expect.txt           "NAME ok MANIFEST" or "NAME err TEXT-IN-MESSAGE"

A manifest's first line is "label LABEL fat BITS"; then one line per entry, in any order:
  D path                                   a directory
  F path size fnv1a64 yyyy-mm-dd hh:mm:ss  a file: size, FNV-1a 64 of its bytes, modified time
Names that are not ASCII appear with '_' for each such character (the reader does that).

    python3 tools/gen_diskimage_testdata.py      (needs mkfs.fat and mtools)
"""
import base64, calendar, os, shutil, struct, subprocess, sys, tempfile, uuid, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkdmg  # noqa: E402

OUT = os.path.normpath(os.path.join(HERE, "..", "rust", "testdata"))
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", TZ="UTC")

def lcg(n, seed):
    out = bytearray(); x = seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)

def pat(n, seed):
    """Long data that compresses well: a 509-byte block repeated. 509 is prime, so every
    512-byte cluster differs and misordered clusters show."""
    b = lcg(509, seed)
    return (b * (n // 509 + 1))[:n]

def fnv(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def ascii_name(s):
    return "".join(c if ord(c) < 128 else "_" for c in s)

class Volume:
    """A FAT volume being filled with mtools; remembers what it should hold."""
    def __init__(self, path, size_kb, bits, label, extra=()):
        self.path, self.bits, self.label, self.entries = path, bits, label, {}
        if os.path.exists(path): os.unlink(path)
        subprocess.check_call(["mkfs.fat", "-C", path, str(size_kb), "-F", str(bits), "-n", label, *extra],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.tmp = tempfile.mkdtemp()
        self.n = 0

    def mkdir(self, p):
        subprocess.check_call(["mmd", "-i", self.path, "::/" + p], env=ENV)
        self.entries[p] = ("D",)

    def put(self, p, data, when=(2021, 6, 15, 13, 45, 30)):
        self.n += 1
        src = os.path.join(self.tmp, "f%d" % self.n)
        with open(src, "wb") as f: f.write(data)
        t = calendar.timegm(when + (0, 0, 0))
        os.utime(src, (t, t))
        subprocess.check_call(["mcopy", "-m", "-o", "-i", self.path, src, "::/" + p], env=ENV)
        self.entries[p] = ("F", len(data), fnv(data), "%04d-%02d-%02d %02d:%02d:%02d" % when)

    def delete(self, p):
        subprocess.check_call(["mdel", "-i", self.path, "::/" + p], env=ENV)
        del self.entries[p]

    def manifest(self):
        lines = ["label %s fat %d" % (self.label, self.bits)]
        for p, e in sorted(self.entries.items()):
            q = ascii_name(p)
            lines.append("D %s" % q if e[0] == "D" else "F %s %d %016x %s" % (q, e[1], e[2], e[3]))
        return "\n".join(lines) + "\n"

    def finish(self):
        shutil.rmtree(self.tmp)
        return open(self.path, "rb").read()

def fat12(path):
    v = Volume(path, 1440, 12, "FLOPPY")
    v.put("README.TXT", b"An 8.3 name in capitals.\r\n")
    v.put("A long file name.txt", b"long names come from VFAT entries\n" * 20)
    v.put("lower.txt", b"lowercase\n", (1999, 12, 31, 23, 59, 58))
    v.put("Café crème.txt", b"not ASCII\n")
    v.mkdir("Sub Folder")
    v.mkdir("Sub Folder/deeper")
    v.put("Sub Folder/deeper/inner.bin", lcg(10000, 1), (1980, 1, 1, 0, 0, 0))
    v.put("Sub Folder/notes.txt", b"nested\n")
    # a fragmented file: fill, delete the middle one (a deleted entry stays), then write across the hole
    v.put("a.bin", lcg(3000, 2)); v.put("b.bin", lcg(3000, 3)); v.put("c.bin", lcg(3000, 4))
    v.delete("b.bin")
    v.put("big.bin", lcg(20000, 5), (2107, 12, 31, 23, 59, 58))
    v.put("empty.txt", b"")
    return v

def fat16(path):
    v = Volume(path, 8192, 16, "MEDIUM", ("-s", "1"))
    for i in range(40):
        v.put("root%02d.txt" % i, (b"file %d\n" % i) * (i + 1))
    v.mkdir("many")
    for i in range(200):                                    # a directory over many clusters
        v.put("many/entry number %03d.dat" % i, pat(100 + i, i))
    v.put("one megabyte.bin", pat(1 << 20, 9))
    return v

def fat32(path):
    v = Volume(path, 40000, 32, "BIG VOL", ("-s", "1"))
    v.mkdir("Program Files")
    v.mkdir("Program Files/App")
    v.put("Program Files/App/app.exe", pat(300000, 11))
    v.put("Program Files/App/readme.txt", b"FAT32: the root directory is a cluster chain\n")
    v.put("hello.txt", b"hello from FAT32\n")
    return v

def small(path):
    """The volume inside the DMG vectors: small, with long names, a subfolder and a fragmented file."""
    v = Volume(path, 360, 12, "SAMPLE")
    v.put("Read Me First.txt", b"inside a disk image\n" * 10)
    v.mkdir("Folder")
    v.put("Folder/data.bin", pat(40000, 21), (2010, 5, 4, 3, 2, 0))
    v.put("x.bin", pat(1500, 22)); v.put("y.bin", pat(1500, 23)); v.delete("x.bin")
    v.put("frag.bin", pat(9000, 24))
    return v

def udif(raw, chunk_kind="zlib"):
    """A UDIF image of a raw disk. chunk_kind: zlib (as hdiutil UDZO), raw, or bzip2 (unsupported)."""
    data = bytearray()
    if chunk_kind == "zlib":
        mish = mkdmg.make_mish(raw, data)
    else:
        nsect = len(raw) // 512
        chunks = []
        for s in range(0, nsect, 2048):
            cnt = min(2048, nsect - s); part = raw[s * 512:(s + cnt) * 512]
            if part.count(0) == len(part):
                chunks.append((0x00000000, s, cnt, len(data), 0))      # zero fill, kind 0
                continue
            body = part if chunk_kind == "raw" else zlib.compress(part)
            chunks.append((1 if chunk_kind == "raw" else 0x80000006, s, cnt, len(data), len(body)))
            data += body
        chunks.append((0xFFFFFFFF, nsect, 0, len(data), 0))
        mish = b"mish" + struct.pack(">IQQQII", 1, 0, nsect, 0, 0x208, 0) + b"\0" * 24
        mish += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(raw)) + b"\0" * 124
        mish += struct.pack(">I", len(chunks))
        for t, s, c, off, ln in chunks:
            mish += struct.pack(">IIQQQQ", t, 0, s, c, off, ln)
    xml = mkdmg.plist(base64.b64encode(mish).decode(), "whole disk")
    xml_off = len(data)
    koly = b"koly" + struct.pack(">III", 4, 512, 1) + struct.pack(">QQQQQ", 0, 0, len(data), 0, 0)
    koly += struct.pack(">II", 1, 1) + uuid.UUID(int=7).bytes
    koly += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(bytes(data))) + b"\0" * 124
    koly += struct.pack(">QQ", xml_off, len(xml)) + b"\0" * 120
    koly += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(raw)) + b"\0" * 124
    koly += struct.pack(">IQ", 1, len(raw) // 512) + b"\0" * 12
    assert len(koly) == 512
    return bytes(data) + xml + koly

def with_mbr(vol, start=63):
    disk = bytearray(start * 512) + vol
    e = struct.pack("<BBBBBBBBII", 0x80, 0, 0, 0, 0x06, 0, 0, 0, start, len(vol) // 512)
    disk[446:462] = e
    disk[510:512] = b"\x55\xAA"
    return bytes(disk)

def with_gpt(vol, start=2048):
    disk = bytearray(start * 512) + vol + bytearray(34 * 512)
    disk[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 0, 0, 0xEE, 0, 0, 0, 1, len(disk) // 512 - 1)
    disk[510:512] = b"\x55\xAA"
    hdr = b"EFI PART" + struct.pack("<IIIIQQQQ", 0x10000, 92, 0, 0, 1, len(disk) // 512 - 1, 34, len(disk) // 512 - 34)
    hdr += uuid.UUID(int=1).bytes + struct.pack("<QIII", 2, 128, 128, 0)
    disk[512:512 + len(hdr)] = hdr
    ent = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le + uuid.UUID(int=2).bytes
    ent += struct.pack("<QQQ", start, start + len(vol) // 512 - 1, 0)
    disk[1024:1024 + len(ent)] = ent
    return bytes(disk)

def main():
    os.makedirs(OUT, exist_ok=True)
    tmp = tempfile.mkdtemp()
    vols = {}
    for name, fn in (("fat12", fat12), ("fat16", fat16), ("fat32", fat32)):
        v = fn(os.path.join(tmp, name + ".img"))
        man = v.manifest(); img = v.finish()
        vols[name] = (img, man)
        open(os.path.join(OUT, name + ".img.z"), "wb").write(zlib.compress(img, 9))
        open(os.path.join(OUT, name + ".txt"), "w").write(man)
    # a small FAT16 volume for the DMGs (as tools/mkdmg.py makes for Sample.dmg)
    small = globals()["small"](os.path.join(tmp, "small.img")); small_man = small.manifest(); small_img = small.finish()
    open(os.path.join(OUT, "small.txt"), "w").write(small_man)
    hfs = bytearray(64 * 1024); hfs[1024:1026] = b"H+"
    dmgs = [
        ("dmg_udzo.dmg", udif(small_img), "ok small"),
        ("dmg_raw.dmg", udif(small_img, "raw"), "ok small"),
        ("dmg_mbr.dmg", udif(with_mbr(small_img)), "ok small"),
        ("dmg_gpt.dmg", udif(with_gpt(small_img)), "ok small"),
        ("dmg_hfs.dmg", udif(bytes(hfs)), "err HFS+ volume found"),
        ("dmg_bzip2.dmg", udif(small_img, "bzip2"), "err Compression 'bzip2'"),
        ("dmg_plain.dmg", small_img, "ok small"),
    ]
    with open(os.path.join(OUT, "dmg_expect.txt"), "w") as f:
        for name, data, exp in dmgs:
            open(os.path.join(OUT, name + ".z"), "wb").write(zlib.compress(data, 9))   # stored compressed
            f.write("%s %s\n" % (name, exp))
    shutil.rmtree(tmp)
    total = sum(os.path.getsize(os.path.join(OUT, n)) for n in os.listdir(OUT) if not n.startswith("inflate"))
    print("disk-image vectors: %d bytes in %s" % (total, OUT))

if __name__ == "__main__":
    main()
