#!/usr/bin/env python3
"""Writes rust/testdata/inflate.bin: zlib test vectors for rust/src/inflate.rs.

Each record is: u32 plain length, the plain bytes, u32 compressed length, the zlib stream
(little endian). The inputs are deterministic; they are compressed at several levels and
strategies so that stored, fixed-Huffman and dynamic-Huffman blocks, long matches, runs and
multi-block streams all appear.

    python3 tools/gen_inflate_testdata.py
"""
import os, struct, zlib

def lcg(n, seed):
    out = bytearray()
    x = seed
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)

text = (b"THE WRLD OS decompresses PNG images, ZIP archives and gzip packages with this code. " * 40)
words = b" ".join(b"word%d" % (i * i % 997) for i in range(1500))
inputs = [
    b"",
    b"a",
    b"hello, world\n",
    b"\x00" * 1000,
    b"ab" * 5000,                        # runs with distance 2
    text,
    words,                               # many distinct matches: dynamic codes
    lcg(3000, 1),                        # incompressible: stored blocks at level 0, literals otherwise
    bytes(range(256)) * 40 + text[:2000],   # distances up to 10 KB
    text[:300] + lcg(500, 3) + text[:300],
]
cases = []
for data in inputs:
    for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY), (1, zlib.Z_DEFAULT_STRATEGY), (6, zlib.Z_FIXED),
                            (9, zlib.Z_DEFAULT_STRATEGY), (9, zlib.Z_HUFFMAN_ONLY), (6, zlib.Z_RLE)):
        c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
        z = c.compress(data) + c.flush()
        cases.append((data, z))
# more than one stored block (64 KB each), and the longest distances, once each
big = lcg(70000, 7)
cases.append((big, zlib.compress(big, 0)))
far = text[:600] + lcg(31000, 9) + text[:600]
cases.append((far, zlib.compress(far, 9)))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rust", "testdata", "inflate.bin")
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "wb") as f:
    for data, z in cases:
        f.write(struct.pack("<I", len(data)) + data + struct.pack("<I", len(z)) + z)
print("%d vectors, %d bytes -> %s" % (len(cases), os.path.getsize(out), os.path.normpath(out)))
