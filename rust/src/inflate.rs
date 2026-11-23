//! DEFLATE (RFC 1951) and zlib (RFC 1950) decompression into a caller's buffer.
//!
//! Replaces src/kernel/inflate.c. It decodes the same streams the same way (canonical
//! Huffman decoding one bit at a time, as in zlib's "puff"), with two differences that only
//! show on bad input or concurrent use: every read and write is bounds-checked, so damaged
//! data is an error instead of a stray memory access, and the Huffman tables live on the
//! caller's stack, so two threads can decompress at once (the C version kept them in
//! static variables). No allocation; about 1.5 KB of stack.

/// Why a stream could not be decoded.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// Not valid DEFLATE (or zlib) data, or it ends early.
    Data,
    /// The output buffer is too small.
    Full,
}

/// A decoded stream: bytes written to the output and bytes of input used (a partly used
/// last byte counts), so that a caller can find what follows the stream (gzip trailers).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Done {
    pub written: usize,
    pub consumed: usize,
}

/// Input read least-significant bit first.
struct Bits<'a> {
    src: &'a [u8],
    pos: usize,
    buf: u32,
    cnt: u32,
}

impl<'a> Bits<'a> {
    fn new(src: &'a [u8]) -> Self { Bits { src, pos: 0, buf: 0, cnt: 0 } }

    fn bit(&mut self) -> Result<u32, Error> {
        if self.cnt == 0 {
            let b = *self.src.get(self.pos).ok_or(Error::Data)?;
            self.pos += 1;
            self.buf = b as u32;
            self.cnt = 8;
        }
        let v = self.buf & 1;
        self.buf >>= 1;
        self.cnt -= 1;
        Ok(v)
    }

    fn bits(&mut self, n: u32) -> Result<u32, Error> {
        let mut v = 0;
        for i in 0..n { v |= self.bit()? << i; }
        Ok(v)
    }

    /// Drops the rest of the current byte (stored blocks start on a byte boundary).
    fn align(&mut self) { self.cnt = 0; }

    /// The next `n` whole bytes.
    fn bytes(&mut self, n: usize) -> Result<&'a [u8], Error> {
        let end = self.pos.checked_add(n).ok_or(Error::Data)?;
        let s = self.src.get(self.pos..end).ok_or(Error::Data)?;
        self.pos = end;
        Ok(s)
    }
}

/// A canonical Huffman code: how many codes of each length, and the symbols in code order.
struct Huff {
    counts: [u16; 16],
    symbols: [u16; 288],
}

impl Huff {
    /// The code for symbols 0..lens.len() with the given code lengths (0 = unused).
    fn new(lens: &[u8]) -> Huff {
        let mut h = Huff { counts: [0; 16], symbols: [0; 288] };
        for &l in lens { h.counts[(l & 15) as usize] += 1; }
        h.counts[0] = 0;
        let mut offs = [0u16; 16];
        for i in 1..15 { offs[i + 1] = offs[i] + h.counts[i]; }
        for (sym, &l) in lens.iter().enumerate() {
            let l = (l & 15) as usize;
            if l == 0 { continue; }
            if let Some(slot) = h.symbols.get_mut(offs[l] as usize) { *slot = sym as u16; }
            offs[l] += 1;
        }
        h
    }

    fn decode(&self, b: &mut Bits) -> Result<u16, Error> {
        let (mut code, mut first, mut idx) = (0i32, 0i32, 0i32);
        for len in 1..16 {
            code |= b.bit()? as i32;
            let cnt = self.counts[len] as i32;
            if code - cnt < first {
                // a malformed code can make this negative: get() then fails
                return self.symbols.get((idx + code - first) as usize).copied().ok_or(Error::Data);
            }
            idx += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        Err(Error::Data)
    }
}

const LBASE: [u16; 29] = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DBASE: [u16; 30] = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DEXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
/// The order code-length code lengths come in (RFC 1951 3.2.7).
const CLORDER: [usize; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

struct Out<'a> {
    dst: &'a mut [u8],
    pos: usize,
}

impl Out<'_> {
    fn push(&mut self, v: u8) -> Result<(), Error> {
        *self.dst.get_mut(self.pos).ok_or(Error::Full)? = v;
        self.pos += 1;
        Ok(())
    }

    /// `len` bytes from `dist` back; the ranges may overlap (a run), so byte by byte.
    fn copy_back(&mut self, dist: usize, len: usize) -> Result<(), Error> {
        if dist == 0 || dist > self.pos { return Err(Error::Data); }
        if len > self.dst.len() - self.pos { return Err(Error::Full); }
        for _ in 0..len {
            self.dst[self.pos] = self.dst[self.pos - dist];
            self.pos += 1;
        }
        Ok(())
    }

    fn extend(&mut self, s: &[u8]) -> Result<(), Error> {
        let d = self.dst.get_mut(self.pos..self.pos + s.len()).ok_or(Error::Full)?;
        d.copy_from_slice(s);
        self.pos += s.len();
        Ok(())
    }
}

/// One compressed block, up to its end-of-block symbol.
fn block(b: &mut Bits, out: &mut Out, lt: &Huff, dt: &Huff) -> Result<(), Error> {
    loop {
        let sym = lt.decode(b)? as usize;
        if sym < 256 {
            out.push(sym as u8)?;
        } else if sym == 256 {
            return Ok(());
        } else {
            let s = sym - 257;
            let base = *LBASE.get(s).ok_or(Error::Data)? as usize;
            let len = base + b.bits(LEXTRA[s] as u32)? as usize;
            let ds = dt.decode(b)? as usize;
            let dbase = *DBASE.get(ds).ok_or(Error::Data)? as usize;
            let dist = dbase + b.bits(DEXTRA[ds] as u32)? as usize;
            out.copy_back(dist, len)?;
        }
    }
}

/// Decodes a raw DEFLATE stream (all its blocks) into `dst`.
pub fn inflate_raw(src: &[u8], dst: &mut [u8]) -> Result<Done, Error> {
    let mut b = Bits::new(src);
    let mut out = Out { dst, pos: 0 };
    loop {
        let last = b.bit()?;
        match b.bits(2)? {
            0 => {
                // stored: LEN, NLEN (not checked, as before), then LEN bytes
                b.align();
                let h = b.bytes(4)?;
                let len = u16::from_le_bytes([h[0], h[1]]) as usize;
                let data = b.bytes(len)?;
                out.extend(data)?;
            }
            1 => {
                // fixed Huffman codes
                let mut l = [0u8; 288];
                l[..144].fill(8);
                l[144..256].fill(9);
                l[256..280].fill(7);
                l[280..].fill(8);
                let lt = Huff::new(&l);
                let dt = Huff::new(&[5u8; 30]);
                block(&mut b, &mut out, &lt, &dt)?;
            }
            2 => {
                // dynamic Huffman codes
                let hlit = b.bits(5)? as usize + 257;
                let hdist = b.bits(5)? as usize + 1;
                let hclen = b.bits(4)? as usize + 4;
                let mut lens = [0u8; 320];
                for &k in &CLORDER[..hclen] { lens[k] = b.bits(3)? as u8; }
                let ct = Huff::new(&lens[..19]);
                lens = [0; 320];
                let total = hlit + hdist;
                let mut n = 0;
                while n < total {
                    let sym = ct.decode(&mut b)?;
                    if sym < 16 {
                        lens[n] = sym as u8;
                        n += 1;
                        continue;
                    }
                    let (val, rep) = match sym {
                        16 => {
                            if n == 0 { return Err(Error::Data); }
                            (lens[n - 1], 3 + b.bits(2)? as usize)
                        }
                        17 => (0, 3 + b.bits(3)? as usize),
                        _ => (0, 11 + b.bits(7)? as usize),
                    };
                    for _ in 0..rep {
                        if n >= total { break; }
                        lens[n] = val;
                        n += 1;
                    }
                }
                let lt = Huff::new(&lens[..hlit]);
                let dt = Huff::new(&lens[hlit..total]);
                block(&mut b, &mut out, &lt, &dt)?;
            }
            _ => return Err(Error::Data),
        }
        if last == 1 { break; }
    }
    Ok(Done { written: out.pos, consumed: b.pos })
}

/// Decodes a zlib stream: checks the two-byte header (DEFLATE, valid check bits), then the
/// DEFLATE data after it. The Adler-32 trailer is not checked. `consumed` counts from the
/// start of the DEFLATE data, as the C version did.
pub fn inflate_zlib(src: &[u8], dst: &mut [u8]) -> Result<Done, Error> {
    if src.len() < 2 || src[0] & 0x0F != 8 || (((src[0] as u32) << 8) | src[1] as u32) % 31 != 0 {
        return Err(Error::Data);
    }
    inflate_raw(&src[2..], dst)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Test vectors made by Python's zlib (tools/gen_inflate_testdata.py): records of
    /// u32 plain length, the plain bytes, u32 zlib length, the zlib stream (little endian).
    const DATA: &[u8] = include_bytes!("../testdata/inflate.bin");

    fn vectors() -> Vec<(&'static [u8], &'static [u8])> {
        let mut v = Vec::new();
        let mut p = 0;
        let rd = |p: &mut usize| { let n = u32::from_le_bytes(DATA[*p..*p + 4].try_into().unwrap()) as usize; *p += 4; n };
        while p < DATA.len() {
            let n = rd(&mut p); let plain = &DATA[p..p + n]; p += n;
            let m = rd(&mut p); let z = &DATA[p..p + m]; p += m;
            v.push((plain, z));
        }
        v
    }

    #[test]
    fn decodes_all_vectors() {
        let v = vectors();
        assert!(v.len() >= 20, "{} vectors", v.len());
        for (i, (plain, z)) in v.iter().enumerate() {
            let mut out = vec![0u8; plain.len() + 16];
            let d = inflate_zlib(z, &mut out).unwrap_or_else(|e| panic!("vector {i}: {e:?}"));
            assert_eq!(d.written, plain.len(), "vector {i}");
            assert_eq!(&out[..d.written], *plain, "vector {i}");
            // the stream ends 4 bytes (the Adler-32) before the end of the zlib data
            assert_eq!(d.consumed + 2 + 4, z.len(), "vector {i}: consumed");
        }
    }

    #[test]
    fn small_buffer_is_full_not_overrun() {
        for (plain, z) in vectors() {
            if plain.is_empty() { continue; }
            let mut out = vec![0u8; plain.len() - 1];
            assert_eq!(inflate_zlib(z, &mut out), Err(Error::Full));
        }
    }

    #[test]
    fn bad_headers() {
        let mut out = [0u8; 16];
        assert_eq!(inflate_zlib(&[], &mut out), Err(Error::Data));
        assert_eq!(inflate_zlib(&[0x78], &mut out), Err(Error::Data));
        assert_eq!(inflate_zlib(&[0x79, 0x9c, 3, 0], &mut out), Err(Error::Data));   // not DEFLATE
        assert_eq!(inflate_zlib(&[0x78, 0x9d, 3, 0], &mut out), Err(Error::Data));   // check bits
        assert_eq!(inflate_raw(&[0x07], &mut out), Err(Error::Data));                // block type 3
        assert_eq!(inflate_raw(&[0x03, 0x00], &mut out), Ok(Done { written: 0, consumed: 2 }));   // an empty fixed block
    }

    /// Damaged and random input must give an error or some output, never a panic (which in
    /// the kernel would be a kernel panic) or a write past the buffer.
    #[test]
    fn fuzz_no_panic() {
        let mut seed = 0x1234_5678u32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        let v = vectors();
        let mut out = vec![0u8; 1 << 18];
        for round in 0..20000 {
            let (plain, z) = v[round % v.len()];
            let mut s = z[2..].to_vec();
            match rnd() % 4 {
                0 => { let k = rnd() as usize % s.len().max(1); s.truncate(k); }
                1 => for _ in 0..1 + rnd() % 8 { if s.is_empty() { break; } let k = rnd() as usize % s.len(); s[k] ^= 1 << (rnd() % 8); },
                2 => for b in s.iter_mut() { *b = rnd() as u8; },
                _ => { let k = rnd() as usize % s.len().max(1); s.insert(k, rnd() as u8); }
            }
            let cap = if rnd() % 3 == 0 { plain.len() / 2 } else { out.len() };
            if let Ok(d) = inflate_raw(&s, &mut out[..cap]) {
                assert!(d.written <= cap && d.consumed <= s.len());
            }
        }
    }
}

// ---------------------------------------------------------------- the C interface (src/include/fs.h)

#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub mod ffi {
    use core::ffi::c_int;
    use core::sync::atomic::{AtomicU32, Ordering};
    use crate::inflate;

    /// Input bytes the last inflate_raw() consumed: src/pkg/unpack.c steps over gzip members
    /// with it. Same layout as the C `uint32_t inflate_used`.
    #[no_mangle]
    pub static inflate_used: AtomicU32 = AtomicU32::new(0);

    /// Slices for a C buffer; a null pointer is an empty buffer.
    unsafe fn in_slice<'a>(p: *const u8, n: u32) -> &'a [u8] {
        if p.is_null() || n == 0 { &[] } else { core::slice::from_raw_parts(p, n as usize) }
    }
    unsafe fn out_slice<'a>(p: *mut u8, n: u32) -> &'a mut [u8] {
        if p.is_null() || n == 0 { &mut [] } else { core::slice::from_raw_parts_mut(p, n as usize) }
    }

    unsafe fn finish(r: Result<inflate::Done, inflate::Error>, outlen: *mut u32) -> c_int {
        match r {
            Ok(d) => {
                if !outlen.is_null() { *outlen = d.written as u32; }
                inflate_used.store(d.consumed as u32, Ordering::Relaxed);
                0
            }
            Err(_) => -1,
        }
    }

    /// int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t cap, uint32_t *outlen):
    /// a raw DEFLATE stream into dst; 0, or -1 on bad data or a full buffer.
    #[no_mangle]
    pub unsafe extern "C" fn inflate_raw(src: *const u8, srclen: u32, dst: *mut u8, cap: u32, outlen: *mut u32) -> c_int {
        finish(inflate::inflate_raw(in_slice(src, srclen), out_slice(dst, cap)), outlen)
    }

    /// int inflate_zlib(...): the same for a zlib stream (RFC 1950 header, no checksum check).
    #[no_mangle]
    pub unsafe extern "C" fn inflate_zlib(src: *const u8, srclen: u32, dst: *mut u8, cap: u32, outlen: *mut u32) -> c_int {
        finish(inflate::inflate_zlib(in_slice(src, srclen), out_slice(dst, cap)), outlen)
    }
}
