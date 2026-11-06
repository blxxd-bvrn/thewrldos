#!/usr/bin/env python3
"""Build an Apple UDIF (.dmg) disk image containing a FAT volume.

usage: mkdmg.py SOURCE_DIR OUTPUT.dmg "Volume Name"

The output follows the UDIF layout used by hdiutil "UDZO" images: zlib-compressed
data fork, an XML property list holding a base64 'mish' block table (resource-fork/blkx),
and the 512-byte big-endian 'koly' trailer. Requires mkfs.fat and mtools (mcopy).
"""
import base64, os, struct, subprocess, sys, tempfile, uuid, zlib

SECTOR = 512
CHUNK_SECTORS = 2048

def build_fat(src, label, size_kb):
    fd, img = tempfile.mkstemp(suffix=".img"); os.close(fd); os.unlink(img)
    subprocess.check_call(["mkfs.fat", "-C", img, str(size_kb), "-n", label[:11].upper()], stdout=subprocess.DEVNULL)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1", MTOOLS_NO_VFAT="0")
    for entry in sorted(os.listdir(src)):
        subprocess.check_call(["mcopy", "-s", "-i", img, os.path.join(src, entry), "::/"], env=env)
    data = open(img, "rb").read(); os.unlink(img)
    return data

def make_mish(raw, data_out):
    nsect = len(raw) // SECTOR
    chunks = []
    for s in range(0, nsect, CHUNK_SECTORS):
        cnt = min(CHUNK_SECTORS, nsect - s)
        part = raw[s * SECTOR:(s + cnt) * SECTOR]
        if part.count(0) == len(part):
            chunks.append((0x00000002, s, cnt, len(data_out), 0))
            continue
        comp = zlib.compress(part, 9)
        chunks.append((0x80000005, s, cnt, len(data_out), len(comp)))
        data_out += comp
    chunks.append((0xFFFFFFFF, nsect, 0, len(data_out), 0))
    hdr = b"mish" + struct.pack(">IQQQII", 1, 0, nsect, 0, 0x208, 0) + b"\0" * 24
    hdr += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF) + b"\0" * 124
    hdr += struct.pack(">I", len(chunks))
    for t, s, c, off, ln in chunks:
        hdr += struct.pack(">IIQQQQ", t, 0, s, c, off, ln)
    return hdr

def plist(mish_b64, name):
    lines = "\n".join("\t\t\t\t" + mish_b64[i:i + 52] for i in range(0, len(mish_b64), 52))
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
\t<key>resource-fork</key>
\t<dict>
\t\t<key>blkx</key>
\t\t<array>
\t\t\t<dict>
\t\t\t\t<key>Attributes</key>
\t\t\t\t<string>0x0050</string>
\t\t\t\t<key>CFName</key>
\t\t\t\t<string>{name}</string>
\t\t\t\t<key>Data</key>
\t\t\t\t<data>
{lines}
\t\t\t\t</data>
\t\t\t\t<key>ID</key>
\t\t\t\t<string>0</string>
\t\t\t\t<key>Name</key>
\t\t\t\t<string>{name}</string>
\t\t\t</dict>
\t\t</array>
\t</dict>
</dict>
</plist>
""".encode()

def main():
    src, out, vol = sys.argv[1], sys.argv[2], (sys.argv[3] if len(sys.argv) > 3 else "Untitled")
    raw = build_fat(src, vol.replace(" ", "")[:11], 4096)
    data = bytearray()
    mish = make_mish(raw, data)
    xml = plist(base64.b64encode(mish).decode(), "whole disk (DOS_FAT_16 : 0)")
    xml_off = len(data)
    koly = b"koly" + struct.pack(">III", 4, 512, 1)
    koly += struct.pack(">QQQQQ", 0, 0, len(data), 0, 0)
    koly += struct.pack(">II", 1, 1) + uuid.uuid4().bytes
    koly += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(bytes(data)) & 0xFFFFFFFF) + b"\0" * 124
    koly += struct.pack(">QQ", xml_off, len(xml)) + b"\0" * 120
    koly += struct.pack(">II", 2, 32) + struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF) + b"\0" * 124
    koly += struct.pack(">IQ", 1, len(raw) // SECTOR) + b"\0" * 12
    assert len(koly) == 512, len(koly)
    with open(out, "wb") as f:
        f.write(data); f.write(xml); f.write(koly)
    print(f"{out}: {len(raw)//1024} KB volume -> {os.path.getsize(out)//1024} KB UDIF image")

if __name__ == "__main__":
    main()
