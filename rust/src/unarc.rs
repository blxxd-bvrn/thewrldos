//! Archive extraction for Explorer's "Extract All..." and the `unrar`/`unzip` commands.
//!
//! Decompression only, written for THE WRLD OS from the published descriptions of the
//! formats (no code from RAR's own unpacker):
//! - ZIP: stored and deflated members (the deflate decoder is `inflate`).
//! - RAR 2.9 to 4.x ("RAR3"): the LZ coder, PPMd variant H (`ppmd`) and the standard
//!   filters (x86 E8/E8E9, Itanium, delta, RGB, audio).
//! - RAR 5 and 7: the LZ coder with its filters (delta, x86 E8/E8E9, ARM).
//! Solid archives work (members are decoded in order through one history); encrypted and
//! multi-volume archives are reported, not extracted.
//!
//! Every member comes back through a callback with its name made safe for the WRLD file
//! system: '\' separators, no drive letters, no leading '\', and no "." or ".." parts.
use alloc::vec;
use alloc::vec::Vec;

/// Why a member (or the whole archive) could not be extracted.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fail {
    /// Not a ZIP or RAR file
    NotArchive,
    /// Needs a password
    Encrypted,
    /// Part of a multi-volume (split) archive
    Split,
    /// A compression method this decoder does not know
    Method,
    /// Damaged data
    Corrupt,
    /// Not enough memory for the member
    NoMem,
    /// Decoded, but the checksum does not match
    Crc,
}
impl Fail {
    pub fn text(self) -> &'static str {
        match self {
            Fail::NotArchive => "not a ZIP or RAR archive",
            Fail::Encrypted => "protected by a password",
            Fail::Split => "part of a split (multi-volume) archive",
            Fail::Method => "compressed with a method that is not supported",
            Fail::Corrupt => "damaged",
            Fail::NoMem => "too large for the available memory",
            Fail::Crc => "damaged (checksum error)",
        }
    }
}

/// One member, as handed to the callback of `extract`.
pub struct Member<'a> {
    /// Safe relative path, '\' separated (empty members are skipped before the callback)
    pub name: &'a [u8],
    pub dir: bool,
    /// The contents, or why they could not be had (empty when only listing)
    pub data: Result<&'a [u8], Fail>,
    /// Size once extracted
    pub size: u64,
    /// Where the member ends in the archive (for a progress bar)
    pub at: usize,
}

/// Called every 256 KB of output while a member decodes, with the bytes done so far: the
/// kernel lets other tasks run there (its tasks are not preempted) and updates progress.
static mut TICK: Option<fn(u64)> = None;
/// Sets the hook above (once, at boot or before the first extraction).
pub fn set_tick(f: fn(u64)) { unsafe { TICK = Some(f); } }
#[inline] fn tick(done: usize, next: &mut usize) {
    if done >= *next {
        *next = done + (256 << 10);
        if let Some(f) = unsafe { TICK } { f(done as u64); }
    }
}

/// Which format `d` is in, if any (RAR self-extractors are searched for the signature).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind { Zip, Rar3, Rar5 }
pub fn kind(d: &[u8]) -> Option<(Kind, usize)> {
    if d.len() >= 4 && &d[..4] == b"PK\x03\x04" || d.len() >= 22 && &d[..4] == b"PK\x05\x06" { return Some((Kind::Zip, 0)); }
    let lim = d.len().min(1 << 20);
    let mut i = 0;
    while i + 7 <= lim {
        if &d[i..i + 6] == b"Rar!\x1a\x07" {
            if d[i + 6] == 0 { return Some((Kind::Rar3, i + 7)); }
            if d[i + 6] == 1 && i + 8 <= d.len() && d[i + 7] == 0 { return Some((Kind::Rar5, i + 8)); }
        }
        i += 1;
    }
    // a ZIP with something in front (self-extractor): its end record is found from the back
    if zip_dir(d).is_some_and(|e| !e.is_empty()) || d.windows(4).take(1 << 16).any(|w| w == b"PK\x03\x04") { return Some((Kind::Zip, 0)); }
    None
}

/// Extracts every member of `d` in archive order, calling `f` for each; `f` returns false
/// to stop. Returns the number of members handed over, or why the archive itself failed.
pub fn extract(d: &[u8], f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> { run(d, false, f) }
/// Like `extract`, but only names and sizes: nothing is decoded.
pub fn list(d: &[u8], f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> { run(d, true, f) }
fn run(d: &[u8], list: bool, f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> {
    match kind(d) {
        Some((Kind::Zip, _)) => zip(d, list, f),
        Some((Kind::Rar3, at)) => rar3(d, at, list, f),
        Some((Kind::Rar5, at)) => rar5(d, at, list, f),
        None => Err(Fail::NotArchive),
    }
}

// ---------------------------------------------------------------- CRC-32

const fn crc_table() -> [u32; 256] {
    let mut t = [0u32; 256];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 { c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 }; k += 1; }
        t[i] = c;
        i += 1;
    }
    t
}
static CRC: [u32; 256] = crc_table();
pub fn crc32(d: &[u8]) -> u32 {
    let mut c = !0u32;
    for &b in d { c = CRC[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8); }
    !c
}

// ---------------------------------------------------------------- names

/// `raw` (either separator) as a safe relative WRLD path.
fn safe_name(raw: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(raw.len());
    for part in raw.split(|&c| c == b'/' || c == b'\\') {
        let mut p = part;
        if out.is_empty() && p.len() == 2 && p[1] == b':' { continue; }   // "C:"
        while let Some((&c, rest)) = p.split_last() { if c == b' ' || c == b'.' && p != b"." && p != b".." { p = rest; } else { break; } }
        if p.is_empty() || p == b"." || p == b".." { continue; }
        if !out.is_empty() { out.push(b'\\'); }
        for &c in p { out.push(match c { 0..=31 | b':' | b'*' | b'?' | b'"' | b'<' | b'>' | b'|' => b'_', c => c }); }
    }
    out
}

fn put_utf8(o: &mut Vec<u8>, c: u32) {
    match c {
        0..=0x7f => o.push(c as u8),
        0x80..=0x7ff => { o.push(0xc0 | (c >> 6) as u8); o.push(0x80 | (c & 0x3f) as u8); }
        0x800..=0xffff => { o.push(0xe0 | (c >> 12) as u8); o.push(0x80 | ((c >> 6) & 0x3f) as u8); o.push(0x80 | (c & 0x3f) as u8); }
        _ => { o.push(0xf0 | (c >> 18) as u8); o.push(0x80 | ((c >> 12) & 0x3f) as u8); o.push(0x80 | ((c >> 6) & 0x3f) as u8); o.push(0x80 | (c & 0x3f) as u8); }
    }
}

// ---------------------------------------------------------------- ZIP

fn le16(d: &[u8], o: usize) -> u64 { if o + 2 <= d.len() { u16::from_le_bytes([d[o], d[o + 1]]) as u64 } else { 0 } }
fn le32(d: &[u8], o: usize) -> u64 { if o + 4 <= d.len() { u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as u64 } else { 0 } }
fn le64(d: &[u8], o: usize) -> u64 { le32(d, o) | le32(d, o + 4) << 32 }

/// One ZIP member as the directory (or, failing that, its local header) describes it.
struct ZEnt { name: Vec<u8>, flags: u64, method: u64, crc: u32, csize: u64, usize: u64, lho: u64 }

/// The central directory: entries, or None when there is no end record. Offsets are fixed up
/// for data in front of the archive (self-extractors, padding).
fn zip_dir(d: &[u8]) -> Option<Vec<ZEnt>> {
    if d.len() < 22 { return None; }
    let lowest = d.len().saturating_sub(65557 + 1024);
    let eocd = (lowest..=d.len() - 22).rev().find(|&i| le32(d, i) == 0x0605_4b50)?;
    let (mut count, mut cdsize, mut cdoff) = (le16(d, eocd + 10), le32(d, eocd + 12), le32(d, eocd + 16));
    let mut cd_end = eocd as u64;
    if eocd >= 20 && le32(d, eocd - 20) == 0x0706_4b50 {
        let z = le64(d, eocd - 12) as usize;
        let z = if le32(d, z) == 0x0606_4b50 { Some(z) } else { (0..eocd - 20).rev().take(1 << 16).find(|&i| le32(d, i) == 0x0606_4b50) };
        if let Some(z) = z { count = le64(d, z + 32); cdsize = le64(d, z + 40); cdoff = le64(d, z + 48); cd_end = z as u64; }
    }
    // bytes in front of the archive move every offset by the same amount
    let shift = cd_end.saturating_sub(cdsize).saturating_sub(cdoff);
    let mut o = (cdoff + shift) as usize;
    if le32(d, o) != 0x0201_4b50 { o = cdoff as usize; }
    let mut out = Vec::new();
    for _ in 0..count.min(1 << 20) {
        if o + 46 > d.len() || le32(d, o) != 0x0201_4b50 { break; }
        let (nl, xl, cl) = (le16(d, o + 28) as usize, le16(d, o + 30) as usize, le16(d, o + 32) as usize);
        let mut e = ZEnt { name: d[o + 46..(o + 46 + nl).min(d.len())].to_vec(), flags: le16(d, o + 8), method: le16(d, o + 10), crc: le32(d, o + 16) as u32,
                           csize: le32(d, o + 20), usize: le32(d, o + 24), lho: le32(d, o + 42) };
        // ZIP64 sizes and offset
        let (mut x, xe) = (o + 46 + nl, (o + 46 + nl + xl).min(d.len()));
        while x + 4 <= xe {
            let (id, sz) = (le16(d, x), le16(d, x + 2) as usize);
            if id == 1 {
                let mut q = x + 4;
                if e.usize == 0xffff_ffff { e.usize = le64(d, q); q += 8; }
                if e.csize == 0xffff_ffff { e.csize = le64(d, q); q += 8; }
                if e.lho == 0xffff_ffff { e.lho = le64(d, q); }
            }
            x += 4 + sz;
        }
        e.lho += if le32(d, (e.lho + shift) as usize) == 0x0403_4b50 { shift } else { 0 };
        o += 46 + nl + xl + cl;
        out.push(e);
    }
    Some(out)
}

/// No central directory: the members in order from their local headers.
fn zip_scan(d: &[u8]) -> Vec<ZEnt> {
    let mut out = Vec::new();
    let mut o = 0usize;
    while o + 30 <= d.len() && out.len() < (1 << 20) {
        if le32(d, o) != 0x0403_4b50 { o += 1; continue; }
        let nl = le16(d, o + 26) as usize;
        let xl = le16(d, o + 28) as usize;
        let e = ZEnt { name: d[(o + 30).min(d.len())..(o + 30 + nl).min(d.len())].to_vec(), flags: le16(d, o + 6), method: le16(d, o + 8), crc: le32(d, o + 14) as u32,
                       csize: le32(d, o + 18), usize: le32(d, o + 22), lho: o as u64 };
        o += 30 + nl + xl + if e.flags & 8 != 0 { 0 } else { e.csize as usize };
        out.push(e);
    }
    out
}

fn zip(d: &[u8], list: bool, f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> {
    let entries = match zip_dir(d) { Some(e) if !e.is_empty() => e, _ => zip_scan(d) };
    if entries.is_empty() { return Err(Fail::Corrupt); }
    let mut n = 0;
    for e in entries {
        let name = safe_name(&e.name);
        if name.is_empty() { continue; }
        let dir = matches!(e.name.last(), Some(b'/') | Some(b'\\'));
        let mut buf = Vec::new();
        let lho = e.lho as usize;
        let src = (lho + 30 <= d.len() && le32(d, lho) == 0x0403_4b50).then(|| lho + 30 + le16(d, lho + 26) as usize + le16(d, lho + 28) as usize)
            .and_then(|st| usize::try_from(e.csize).ok().and_then(|c| st.checked_add(c)).filter(|&en| en <= d.len()).map(|en| &d[st..en]));
        let at = src.map_or(lho, |s| s.as_ptr() as usize - d.as_ptr() as usize + s.len());
        let data = if dir || list { Ok(&buf[..]) }
            else if e.flags & 1 != 0 { Err(Fail::Encrypted) }
            else {
                match src {
                    None => Err(Fail::Corrupt),
                    Some(src) => {
                        let r = match e.method {
                            0 => if src.len() as u64 == e.usize { Ok(src) } else { Err(Fail::Corrupt) },
                            8 => match usize::try_from(e.usize) {
                                Ok(us) if buf.try_reserve_exact(us).is_ok() => {
                                    buf.resize(us, 0);
                                    match crate::inflate::inflate_raw(src, &mut buf) { Ok(r) if r.written == buf.len() => Ok(&buf[..]), _ => Err(Fail::Corrupt) }
                                }
                                _ => Err(Fail::NoMem),
                            },
                            99 => Err(Fail::Encrypted),
                            _ => Err(Fail::Method),
                        };
                        match r { Ok(v) if crc32(v) != e.crc => Err(Fail::Crc), r => r }
                    }
                }
            };
        n += 1;
        if !f(&Member { name: &name, dir, data, size: e.usize, at }) { break; }
    }
    Ok(n)
}

// ---------------------------------------------------------------- bit input (most significant bit first)

pub struct Bits<'a> { pub d: &'a [u8], pub pos: usize }
impl<'a> Bits<'a> {
    #[inline] fn byte(&self, i: usize) -> u32 { match self.d.get(i) { Some(&b) => b as u32, None => 0 } }
    /// The next 16 bits, not consumed
    #[inline] pub fn get16(&self) -> u32 {
        let b = self.pos >> 3;
        let v = (self.byte(b) << 16) | (self.byte(b + 1) << 8) | self.byte(b + 2);
        (v >> (8 - (self.pos & 7))) & 0xffff
    }
    #[inline] pub fn add(&mut self, n: u32) { self.pos += n as usize; }
    /// `n` bits (up to 64) as a number
    pub fn read(&mut self, mut n: u32) -> u64 {
        let mut v = 0u64;
        while n > 0 { let k = n.min(16); v = (v << k) | (self.get16() >> (16 - k)) as u64; self.add(k); n -= k; }
        v
    }
    pub fn byte_in(&mut self) -> u32 { self.read(8) as u32 }
    fn align(&mut self) { self.pos = (self.pos + 7) & !7; }
    /// Read well past the end: the data is damaged
    fn overrun(&self) -> bool { self.pos > self.d.len() * 8 + 64 }
}

// ---------------------------------------------------------------- Huffman tables (canonical, RAR style)

const QBITS: u32 = 10;
struct Huff { lim: [u32; 16], pos: [u32; 16], num: Vec<u16>, quick: Vec<u32> }
impl Huff {
    fn new(lens: &[u8]) -> Huff {
        let mut count = [0u32; 16];
        for &l in lens { count[(l & 15) as usize] += 1; }
        count[0] = 0;
        let (mut lim, mut pos, mut upper) = ([0u32; 16], [0u32; 16], 0u32);
        for i in 1..16 {
            upper = upper.wrapping_add(count[i]);
            lim[i] = upper.wrapping_shl(16 - i as u32);
            upper = upper.wrapping_mul(2);
            pos[i] = pos[i - 1] + count[i - 1];
        }
        let mut num = vec![0u16; lens.len().max(1)];
        let mut next = pos;
        for (i, &l) in lens.iter().enumerate() {
            let l = (l & 15) as usize;
            if l != 0 { let p = next[l] as usize; if p < num.len() { num[p] = i as u16; } next[l] += 1; }
        }
        let mut h = Huff { lim, pos, num, quick: vec![0u32; 1 << QBITS] };
        for p in 0..(1u32 << QBITS) {
            let (s, n) = h.slow(p << (16 - QBITS));
            if n <= QBITS { h.quick[p as usize] = s | (n << 16); }
        }
        h
    }
    fn slow(&self, bf: u32) -> (u32, u32) {
        let bf = bf & 0xfffe;
        let mut bits = 15usize;
        for i in 1..15 { if bf < self.lim[i] { bits = i; break; } }
        let dist = bf.wrapping_sub(self.lim[bits - 1]) >> (16 - bits);
        let mut p = self.pos[bits].wrapping_add(dist) as usize;
        if p >= self.num.len() { p = 0; }
        (self.num[p] as u32, bits as u32)
    }
    #[inline] fn decode(&self, b: &mut Bits) -> u32 {
        let bf = b.get16();
        let q = self.quick[(bf >> (16 - QBITS)) as usize];
        if q != 0 { b.add(q >> 16); return q & 0xffff; }
        let (s, n) = self.slow(bf);
        b.add(n);
        s
    }
}

/// The bit lengths of a table coded with the 20-symbol "bit length" table, as RAR 3 and 5
/// share it; `old` (RAR 3) is added modulo 16 to the plain lengths.
fn read_lengths(b: &mut Bits, out: &mut [u8], old: Option<&[u8]>) -> Result<(), Fail> {
    let mut bl = [0u8; 20];
    let mut i = 0;
    while i < 20 {
        let l = (b.get16() >> 12) as u8; b.add(4);
        if l == 15 {
            let z = (b.get16() >> 12) as usize; b.add(4);
            if z == 0 { bl[i] = 15; i += 1; } else { let mut z = z + 2; while z > 0 && i < 20 { bl[i] = 0; i += 1; z -= 1; } }
        } else { bl[i] = l; i += 1; }
    }
    let bd = Huff::new(&bl);
    let n = out.len();
    let mut i = 0;
    while i < n {
        if b.overrun() { return Err(Fail::Corrupt); }
        let s = bd.decode(b);
        if s < 16 {
            out[i] = match old { Some(o) => ((s as u8).wrapping_add(o[i])) & 15, None => s as u8 };
            i += 1;
        } else if s < 18 {
            let k = if s == 16 { let v = (b.get16() >> 13) + 3; b.add(3); v } else { let v = (b.get16() >> 9) + 11; b.add(7); v };
            if i == 0 { return Err(Fail::Corrupt); }
            for _ in 0..k { if i < n { out[i] = out[i - 1]; i += 1; } }
        } else {
            let k = if s == 18 { let v = (b.get16() >> 13) + 3; b.add(3); v } else { let v = (b.get16() >> 9) + 11; b.add(7); v };
            for _ in 0..k { if i < n { out[i] = 0; i += 1; } }
        }
    }
    Ok(())
}

/// Copies `len` bytes from `dist` back (the history is the whole output so far; bytes before
/// its start read as zero), stopping at `cap`.
#[inline]
fn copy_match(out: &mut Vec<u8>, len: u32, dist: u64, cap: usize) {
    let n = (len as usize).min(cap.saturating_sub(out.len()));
    if dist == 0 || dist as usize > out.len() { for _ in 0..n { out.push(0); } return; }
    let from = out.len() - dist as usize;
    if dist as usize >= n { out.extend_from_within(from..from + n); }
    else { for k in 0..n { let c = out[from + k]; out.push(c); } }
}

fn reserve(out: &mut Vec<u8>, extra: u64) -> Result<(), Fail> {
    let extra = usize::try_from(extra).map_err(|_| Fail::NoMem)?;
    out.try_reserve(extra).map_err(|_| Fail::NoMem)
}

// ---------------------------------------------------------------- filters (shared by RAR 3 and 5)

#[derive(Clone, Copy, PartialEq, Eq)]
enum FType { E8, E8E9, Itanium, Delta, Rgb, Audio, Arm, Unknown }
#[derive(Clone, Copy)]
struct Filter { start: u64, len: u32, ty: FType, r: [u32; 7], v5: bool }

fn rd4(d: &[u8], i: usize) -> u32 { u32::from_le_bytes([d[i], d[i + 1], d[i + 2], d[i + 3]]) }
fn wr4(d: &mut [u8], i: usize, v: u32) { d[i..i + 4].copy_from_slice(&v.to_le_bytes()); }

/// Runs one filter over `d` (the block), `off` being the block's offset in its file.
fn run_filter(f: &Filter, d: &mut [u8], off: u64) -> Result<(), Fail> {
    let n = d.len();
    match f.ty {
        FType::E8 | FType::E8E9 => {
            const FS: u32 = 0x100_0000;
            let cmp2 = if f.ty == FType::E8E9 { 0xe9 } else { 0xe8 };
            let mut i = 0usize;
            while i + 5 <= n {
                let c = d[i];
                i += 1;
                if c == 0xe8 || c == cmp2 {
                    let mut o = (i as u64 + off) as u32;
                    if f.v5 { o %= FS; }
                    let a = rd4(d, i);
                    if a & 0x8000_0000 != 0 { if a.wrapping_add(o) & 0x8000_0000 == 0 { wr4(d, i, a.wrapping_add(FS)); } }
                    else if a.wrapping_sub(FS) & 0x8000_0000 != 0 { wr4(d, i, a.wrapping_sub(o)); }
                    i += 4;
                }
            }
        }
        FType::Arm => {
            let mut i = 0usize;
            while i + 4 <= n {
                if d[i + 3] == 0xeb {
                    let mut o = d[i] as u32 | (d[i + 1] as u32) << 8 | (d[i + 2] as u32) << 16;
                    o = o.wrapping_sub(((off + i as u64) / 4) as u32);
                    d[i] = o as u8; d[i + 1] = (o >> 8) as u8; d[i + 2] = (o >> 16) as u8;
                }
                i += 4;
            }
        }
        FType::Itanium => {
            fn getb(d: &[u8], pos: usize, cnt: u32) -> u32 {
                let a = pos / 8;
                let v = rd4(d, a) >> (pos & 7);
                v & (u32::MAX >> (32 - cnt))
            }
            fn setb(d: &mut [u8], v: u32, pos: usize, cnt: u32) {
                let a = pos / 8;
                let sh = (pos & 7) as u32;
                let mut and = !((u32::MAX >> (32 - cnt)) << sh);
                let mut v = v << sh;
                for k in 0..4 { d[a + k] &= and as u8; d[a + k] |= v as u8; and = (and >> 8) | 0xff00_0000; v >>= 8; }
            }
            if n < 21 { return Ok(()); }
            const MASKS: [u8; 16] = [4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0];
            let mut fo = (off >> 4) as u32;
            let mut i = 0usize;
            while i + 21 <= n {
                let b = (d[i] & 0x1f) as i32 - 0x10;
                if b >= 0 {
                    let m = MASKS[b as usize];
                    for k in 0..3 {
                        if m & (1 << k) != 0 {
                            let sp = k * 41 + 5;
                            if getb(&d[i..], sp + 37, 4) == 5 {
                                let o = getb(&d[i..], sp + 13, 20);
                                setb(&mut d[i..], o.wrapping_sub(fo) & 0xfffff, sp + 13, 20);
                            }
                        }
                    }
                }
                i += 16;
                fo = fo.wrapping_add(1);
            }
        }
        FType::Delta => {
            let ch = (f.r[0] as usize).max(1);
            let src = d.to_vec();
            let mut s = 0;
            for c in 0..ch {
                let mut prev = 0u8;
                let mut p = c;
                while p < n { if s < n { prev = prev.wrapping_sub(src[s]); s += 1; } d[p] = prev; p += ch; }
            }
        }
        FType::Rgb => {
            let width = f.r[0].wrapping_sub(3) as usize;
            let pos_r = f.r[1] as usize;
            if n < 3 || width > n || pos_r > 2 { return Ok(()); }
            let src = d.to_vec();
            let mut s = 0;
            for c in 0..3 {
                let mut prev = 0u32;
                let mut i = c;
                while i < n {
                    let mut pred = prev;
                    if i >= width + 3 {
                        let up = d[i - width] as u32;
                        let ul = d[i - width - 3] as u32;
                        let p = prev.wrapping_add(up).wrapping_sub(ul);
                        let pa = (p.wrapping_sub(prev) as i32).unsigned_abs();
                        let pb = (p.wrapping_sub(up) as i32).unsigned_abs();
                        let pc = (p.wrapping_sub(ul) as i32).unsigned_abs();
                        pred = if pa <= pb && pa <= pc { prev } else if pb <= pc { up } else { ul };
                    }
                    let v = (pred.wrapping_sub(src[s] as u32)) as u8;
                    s += 1;
                    d[i] = v;
                    prev = v as u32;
                    i += 3;
                }
            }
            let mut i = pos_r;
            while i + 2 < n { let g = d[i + 1]; d[i] = d[i].wrapping_add(g); d[i + 2] = d[i + 2].wrapping_add(g); i += 3; }
        }
        FType::Audio => {
            let ch = (f.r[0] as usize).max(1);
            let src = d.to_vec();
            let mut s = 0;
            for c in 0..ch {
                let (mut prev, mut pdelta) = (0u32, 0i32);
                let mut dif = [0u32; 7];
                let (mut d1, mut d2) = (0i32, 0i32);
                let mut d3: i32;
                let (mut k1, mut k2, mut k3) = (0i32, 0i32, 0i32);
                let mut i = c;
                let mut count = 0u32;
                while i < n {
                    d3 = d2;
                    d2 = pdelta.wrapping_sub(d1);
                    d1 = pdelta;
                    let mut pred = (8u32.wrapping_mul(prev) as i32).wrapping_add(k1.wrapping_mul(d1)).wrapping_add(k2.wrapping_mul(d2)).wrapping_add(k3.wrapping_mul(d3)) as u32;
                    pred = (pred >> 3) & 0xff;
                    let cur = src[s] as u32;
                    s += 1;
                    pred = pred.wrapping_sub(cur);
                    d[i] = pred as u8;
                    pdelta = (pred.wrapping_sub(prev)) as u8 as i8 as i32;
                    prev = pred & 0xff;
                    let dd = ((cur as u8 as i8) as i32) << 3;
                    dif[0] += dd.unsigned_abs();
                    dif[1] += dd.wrapping_sub(d1).unsigned_abs();
                    dif[2] += dd.wrapping_add(d1).unsigned_abs();
                    dif[3] += dd.wrapping_sub(d2).unsigned_abs();
                    dif[4] += dd.wrapping_add(d2).unsigned_abs();
                    dif[5] += dd.wrapping_sub(d3).unsigned_abs();
                    dif[6] += dd.wrapping_add(d3).unsigned_abs();
                    if count & 0x1f == 0 {
                        let (mut min, mut nmin) = (dif[0], 0);
                        dif[0] = 0;
                        for j in 1..7 { if dif[j] < min { min = dif[j]; nmin = j; } dif[j] = 0; }
                        match nmin {
                            1 => if k1 >= -16 { k1 -= 1 },
                            2 => if k1 < 16 { k1 += 1 },
                            3 => if k2 >= -16 { k2 -= 1 },
                            4 => if k2 < 16 { k2 += 1 },
                            5 => if k3 >= -16 { k3 -= 1 },
                            6 => if k3 < 16 { k3 += 1 },
                            _ => {}
                        }
                    }
                    i += ch;
                    count += 1;
                }
            }
        }
        FType::Unknown => return Err(Fail::Method),
    }
    Ok(())
}

/// The finished member `out[base..]` with the filters that fall in it applied, in order.
fn filtered<'o>(out: &'o [u8], base: usize, filters: &[Filter], tmp: &'o mut Vec<u8>) -> Result<&'o [u8], Fail> {
    let file = &out[base..];
    let mine = filters.iter().filter(|f| f.start >= base as u64 && f.start < out.len() as u64 && f.len > 0);
    if mine.clone().next().is_none() { return Ok(file); }
    tmp.clear();
    tmp.try_reserve_exact(file.len()).map_err(|_| Fail::NoMem)?;
    tmp.extend_from_slice(file);
    for f in mine {
        let s = f.start as usize - base;
        let e = (s + f.len as usize).min(tmp.len());
        run_filter(f, &mut tmp[s..e], s as u64)?;
    }
    Ok(&tmp[..])
}

// ---------------------------------------------------------------- RAR 5

fn vint(d: &[u8], p: &mut usize) -> Option<u64> {
    let mut v = 0u64;
    for k in 0..10 {
        let c = *d.get(*p)?;
        *p += 1;
        v |= ((c & 0x7f) as u64) << (7 * k);
        if c & 0x80 == 0 { return Some(v); }
    }
    None
}

struct R5 {
    ld: Huff, dd: Huff, ldd: Huff, rd: Huff,
    have_tables: bool,
    old: [u64; 4],
    last_len: u32,
    filters: Vec<Filter>,
}
impl R5 {
    fn new() -> R5 { R5 { ld: Huff::new(&[]), dd: Huff::new(&[]), ldd: Huff::new(&[]), rd: Huff::new(&[]), have_tables: false, old: [0; 4], last_len: 0, filters: Vec::new() } }
    fn reset(&mut self) { self.have_tables = false; self.old = [0; 4]; self.last_len = 0; self.filters.clear(); }
    fn tables(&mut self, b: &mut Bits, dc: usize) -> Result<(), Fail> {
        const NC: usize = 306; const LDC: usize = 16; const RC: usize = 44;
        let mut t = vec![0u8; NC + dc + LDC + RC];
        read_lengths(b, &mut t, None)?;
        self.ld = Huff::new(&t[..NC]);
        self.dd = Huff::new(&t[NC..NC + dc]);
        self.ldd = Huff::new(&t[NC + dc..NC + dc + LDC]);
        self.rd = Huff::new(&t[NC + dc + LDC..]);
        self.have_tables = true;
        Ok(())
    }
}
struct Blk { end: usize, last: bool, table: bool }
fn r5_block(b: &mut Bits) -> Result<Blk, Fail> {
    b.align();
    let flags = b.byte_in();
    let cnt = ((flags >> 3) & 3) + 1;
    if cnt == 4 { return Err(Fail::Corrupt); }
    let sum = b.byte_in();
    let mut size = 0u32;
    for k in 0..cnt { size |= b.byte_in() << (8 * k); }
    if (0x5a ^ flags ^ size ^ (size >> 8) ^ (size >> 16)) & 0xff != sum { return Err(Fail::Corrupt); }
    let start = b.pos / 8;
    let end = ((start + size as usize) * 8).saturating_sub(8) + ((flags & 7) + 1) as usize;
    Ok(Blk { end, last: flags & 0x40 != 0, table: flags & 0x80 != 0 })
}
fn r5_len(b: &mut Bits, slot: u32) -> u32 {
    if slot < 8 { return 2 + slot; }
    let lb = slot / 4 - 1;
    2 + ((4 | (slot & 3)) << lb) + b.read(lb) as u32
}
fn r5_unpack(st: &mut R5, src: &[u8], out: &mut Vec<u8>, cap: usize, dc: usize) -> Result<(), Fail> {
    let mut b = Bits { d: src, pos: 0 };
    let mut blk = r5_block(&mut b)?;
    if blk.table { st.tables(&mut b, dc)?; }
    if !st.have_tables { return Err(Fail::Corrupt); }
    let base = out.len();
    let mut next = base;
    loop {
        if out.len() >= cap { break; }
        tick(out.len() - base, &mut next);
        while b.pos >= blk.end {
            if blk.last { return Ok(()); }
            blk = r5_block(&mut b)?;
            if blk.table { st.tables(&mut b, dc)?; }
        }
        if b.overrun() { return Err(Fail::Corrupt); }
        let slot = st.ld.decode(&mut b);
        if slot < 256 { out.push(slot as u8); continue; }
        if slot >= 262 {
            let mut len = r5_len(&mut b, slot - 262);
            let ds = st.dd.decode(&mut b) as u64;
            let mut dist = 1u64;
            if ds < 4 { dist += ds; } else {
                let db = (ds / 2 - 1) as u32;
                dist += (2 | (ds & 1)) << db;
                if db >= 4 {
                    if db > 4 { dist += b.read(db - 4) << 4; }
                    dist += st.ldd.decode(&mut b) as u64;
                } else { dist += b.read(db); }
            }
            if dist > 0x100 { len += 1; if dist > 0x2000 { len += 1; if dist > 0x40000 { len += 1; } } }
            st.old = [dist, st.old[0], st.old[1], st.old[2]];
            st.last_len = len;
            copy_match(out, len, dist, cap);
            continue;
        }
        if slot == 256 {
            let fdata = |b: &mut Bits| -> u32 { let n = (b.get16() >> 14) + 1; b.add(2); let mut v = 0; for k in 0..n { v |= b.byte_in() << (8 * k); } v };
            let start = fdata(&mut b) as u64;
            let mut len = fdata(&mut b);
            if len > 0x40_0000 { len = 0; }
            let ty = b.read(3) as u32;
            let mut r = [0u32; 7];
            if ty == 0 { r[0] = b.read(5) as u32 + 1; }
            let ty = match ty { 0 => FType::Delta, 1 => FType::E8, 2 => FType::E8E9, 3 => FType::Arm, _ => FType::Unknown };
            if st.filters.len() >= 8192 { return Err(Fail::Corrupt); }
            st.filters.push(Filter { start: out.len() as u64 + start, len, ty, r, v5: true });
            continue;
        }
        if slot == 257 { if st.last_len != 0 { copy_match(out, st.last_len, st.old[0], cap); } continue; }
        let k = (slot - 258) as usize;
        let dist = st.old[k];
        for i in (1..=k).rev() { st.old[i] = st.old[i - 1]; }
        st.old[0] = dist;
        let ls = st.rd.decode(&mut b);
        let len = r5_len(&mut b, ls);
        st.last_len = len;
        copy_match(out, len, dist, cap);
    }
    Ok(())
}

fn rar5(d: &[u8], mut p: usize, list: bool, f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> {
    let mut st = R5::new();
    let mut out: Vec<u8> = Vec::new();
    let mut tmp = Vec::new();
    let mut n = 0;
    let mut solid_ok = true;   // the history is good for the next solid member
    while p + 7 <= d.len() {
        let crc = rd4(d, p);
        let mut q = p + 4;
        let size = vint(d, &mut q).ok_or(Fail::Corrupt)? as usize;
        let hstart = p + 4;
        let hend = q.checked_add(size).filter(|&e| e <= d.len() && size > 0).ok_or(Fail::Corrupt)?;
        if crc32(&d[hstart..hend]) != crc { return Err(Fail::Corrupt); }
        let ty = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let hflags = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let extra = if hflags & 1 != 0 { vint(d, &mut q).ok_or(Fail::Corrupt)? as usize } else { 0 };
        let dsize = if hflags & 2 != 0 { vint(d, &mut q).ok_or(Fail::Corrupt)? } else { 0 };
        let dstart = hend;
        let dend = usize::try_from(dsize).ok().and_then(|s| dstart.checked_add(s)).ok_or(Fail::Corrupt)?;
        p = dend;
        match ty {
            4 => return Err(Fail::Encrypted),
            5 => break,
            2 => {}
            _ => continue,
        }
        // file header
        let ff = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let usize_ = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let _attr = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        if ff & 2 != 0 { q += 4; }
        let fcrc = if ff & 4 != 0 { if q + 4 > hend { return Err(Fail::Corrupt); } let c = rd4(d, q); q += 4; Some(c) } else { None };
        let ci = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let _os = vint(d, &mut q).ok_or(Fail::Corrupt)?;
        let nl = vint(d, &mut q).ok_or(Fail::Corrupt)? as usize;
        let raw = d.get(q..q.saturating_add(nl)).ok_or(Fail::Corrupt)?;
        let mut encrypted = false;
        let mut link = false;
        if extra > 0 && extra <= hend - hstart {
            let mut e = hend - extra;
            while e < hend {
                let Some(rs) = vint(d, &mut e) else { break };
                let next = e + rs as usize;
                let Some(rt) = vint(d, &mut e) else { break };
                if rt == 1 { encrypted = true; }
                if rt == 5 { link = true; }
                e = next;
            }
        }
        let name = safe_name(raw);
        let dir = ff & 1 != 0;
        let solid = ci & 0x40 != 0;
        let method = (ci >> 7) & 7;
        let ver = ci & 0x3f;
        let dc = if ver == 1 { 80 } else { 64 };
        let data: Result<&[u8], Fail>;
        let src = d.get(dstart..dend).ok_or(Fail::Corrupt);
        if dir || link || list { data = Ok(&[]); }
        else if encrypted { data = Err(Fail::Encrypted); solid_ok = false; }
        else if hflags & 0x18 != 0 { data = Err(Fail::Split); solid_ok = false; }
        else if method == 0 {
            data = src.and_then(|s| if fcrc.is_some_and(|c| crc32(s) != c) { Err(Fail::Crc) } else { Ok(s) });
        } else if ver > 1 { data = Err(Fail::Method); solid_ok = false; }
        else if solid && !solid_ok { data = Err(Fail::Corrupt); }
        else {
            if !solid { st.reset(); out.clear(); }
            else { st.filters.clear(); }
            let base = out.len();
            let known = ff & 8 == 0;
            let r = src.and_then(|s| {
                reserve(&mut out, if known { usize_ } else { (s.len() as u64) * 4 })?;
                let cap = if known { base.saturating_add(usize_ as usize) } else { usize::MAX >> 1 };
                r5_unpack(&mut st, s, &mut out, cap, dc)
            });
            data = match r {
                Err(e) => { solid_ok = false; Err(e) }
                Ok(()) if known && out.len() - base != usize_ as usize => { solid_ok = false; Err(Fail::Corrupt) }
                Ok(()) => match filtered(&out, base, &st.filters, &mut tmp) {
                    Ok(v) if fcrc.is_some_and(|c| crc32(v) != c) => Err(Fail::Crc),
                    v => v,
                },
            };
        }
        if name.is_empty() { continue; }
        n += 1;
        if !f(&Member { name: &name, dir, data, size: usize_, at: dend }) { break; }
    }
    Ok(n)
}

// ---------------------------------------------------------------- RAR 2.9 - 4.x

const NC3: usize = 299; const DC3: usize = 60; const LDC3: usize = 17; const RC3: usize = 28;
const T3: usize = NC3 + DC3 + LDC3 + RC3;
const LDECODE: [u32; 28] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224];
const LBITS: [u32; 28] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5];
const SDDECODE: [u32; 8] = [0, 4, 8, 16, 32, 64, 128, 192];
const SDBITS: [u32; 8] = [2, 2, 3, 4, 5, 6, 6, 6];
fn ddecode() -> ([u32; DC3], [u32; DC3]) {
    const COUNTS: [u32; 19] = [4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 14, 0, 12];
    let (mut dd, mut db) = ([0u32; DC3], [0u32; DC3]);
    let (mut slot, mut dist) = (0usize, 0u32);
    for (bl, &c) in COUNTS.iter().enumerate() {
        for _ in 0..c { if slot < DC3 { dd[slot] = dist; db[slot] = bl as u32; } slot += 1; dist += 1 << bl; }
    }
    (dd, db)
}

struct R3 {
    ld: Huff, dd: Huff, ldd: Huff, rd: Huff,
    old_table: [u8; T3],
    tables_read: bool,
    ppm_mode: bool,
    ppm: Option<alloc::boxed::Box<crate::ppmd::Model>>,
    esc: u32,
    old: [u32; 4],
    last_len: u32,
    prev_low: u32,
    low_rep: u32,
    progs: Vec<FType>,
    old_lens: Vec<u32>,
    last_filter: usize,
    stack: Vec<Filter>,
    dtab: ([u32; DC3], [u32; DC3]),
}
impl R3 {
    fn new() -> R3 {
        R3 { ld: Huff::new(&[]), dd: Huff::new(&[]), ldd: Huff::new(&[]), rd: Huff::new(&[]), old_table: [0; T3], tables_read: false, ppm_mode: false, ppm: None,
             esc: 2, old: [0; 4], last_len: 0, prev_low: 0, low_rep: 0, progs: Vec::new(), old_lens: Vec::new(), last_filter: 0, stack: Vec::new(), dtab: ddecode() }
    }
    fn reset(&mut self) {
        self.tables_read = false; self.ppm_mode = false; self.esc = 2; self.old = [0; 4]; self.last_len = 0;
        self.old_table = [0; T3]; self.prev_low = 0; self.low_rep = 0;
        self.progs.clear(); self.old_lens.clear(); self.last_filter = 0; self.stack.clear();
    }
    fn tables(&mut self, b: &mut Bits) -> Result<(), Fail> {
        b.align();
        let bf = b.get16();
        if bf & 0x8000 != 0 {
            self.ppm_mode = true;
            let m = self.ppm.get_or_insert_with(|| alloc::boxed::Box::new(crate::ppmd::Model::new()));
            match m.init(b, &mut self.esc) { Ok(()) => { self.tables_read = true; return Ok(()); } Err(()) => return Err(Fail::Corrupt) }
        }
        self.ppm_mode = false;
        self.prev_low = 0; self.low_rep = 0;
        if bf & 0x4000 == 0 { self.old_table = [0; T3]; }
        b.add(2);
        let mut t = [0u8; T3];
        let old = self.old_table;
        read_lengths(b, &mut t, Some(&old))?;
        self.ld = Huff::new(&t[..NC3]);
        self.dd = Huff::new(&t[NC3..NC3 + DC3]);
        self.ldd = Huff::new(&t[NC3 + DC3..NC3 + DC3 + LDC3]);
        self.rd = Huff::new(&t[NC3 + DC3 + LDC3..]);
        self.old_table = t;
        self.tables_read = true;
        Ok(())
    }
    /// A filter definition (`code`, its first byte `first`), at output position `pos`.
    fn add_vm(&mut self, first: u32, code: &[u8], pos: u64) -> Result<(), Fail> {
        let mut b = Bits { d: code, pos: 0 };
        fn data(b: &mut Bits) -> u32 {
            let v = b.get16();
            match v & 0xc000 {
                0 => { b.add(6); (v >> 10) & 0xf }
                0x4000 => if v & 0x3c00 == 0 { b.add(14); 0xffff_ff00 | ((v >> 2) & 0xff) } else { b.add(10); (v >> 6) & 0xff },
                0x8000 => { b.add(2); let v = b.get16(); b.add(16); v }
                _ => { b.add(2); let hi = b.get16(); b.add(16); let lo = b.get16(); b.add(16); (hi << 16) | lo }
            }
        }
        let fpos = if first & 0x80 != 0 {
            let v = data(&mut b);
            if v == 0 { self.progs.clear(); self.old_lens.clear(); 0 } else { (v - 1) as usize }
        } else { self.last_filter };
        if fpos > self.progs.len() || fpos > self.old_lens.len() || fpos > 1024 { return Err(Fail::Corrupt); }
        self.last_filter = fpos;
        let new = fpos == self.progs.len();
        if new { self.progs.push(FType::Unknown); self.old_lens.push(0); }
        let mut start = data(&mut b);
        if first & 0x40 != 0 { start = start.wrapping_add(258); }
        let len = if first & 0x20 != 0 { let l = data(&mut b); self.old_lens[fpos] = l; l } else { self.old_lens.get(fpos).copied().unwrap_or(0) };
        let mut r = [0u32; 7];
        r[4] = len;
        if first & 0x10 != 0 {
            let mask = b.get16() >> 9; b.add(7);
            for k in 0..7 { if mask & (1 << k) != 0 { r[k] = data(&mut b); } }
        }
        if new {
            let size = data(&mut b) as usize;
            if size == 0 || size >= 0x10000 || b.pos / 8 + size > code.len() { return Err(Fail::Corrupt); }
            let mut prog = vec![0u8; size];
            for c in prog.iter_mut() { *c = b.byte_in() as u8; }
            let xor = prog[1..].iter().fold(0u8, |a, &c| a ^ c);
            let ty = if xor != prog[0] { FType::Unknown } else {
                match (size, crc32(&prog)) {
                    (53, 0xad57_6887) => FType::E8,
                    (57, 0x3cd7_e57e) => FType::E8E9,
                    (120, 0x3769_893f) => FType::Itanium,
                    (29, 0x0e06_077d) => FType::Delta,
                    (149, 0x1c2c_5dc8) => FType::Rgb,
                    (216, 0xbc85_e701) => FType::Audio,
                    _ => FType::Unknown,
                }
            };
            self.progs[fpos] = ty;
        }
        if self.stack.len() >= 8192 { return Err(Fail::Corrupt); }
        self.stack.push(Filter { start: pos + start as u64, len, ty: self.progs[fpos], r, v5: false });
        Ok(())
    }
}

fn r3_unpack(st: &mut R3, src: &[u8], out: &mut Vec<u8>, cap: usize) -> Result<(), Fail> {
    let mut b = Bits { d: src, pos: 0 };
    if !st.tables_read { st.tables(&mut b)?; }
    let base = out.len();
    let mut next = base;
    loop {
        if out.len() >= cap { return Ok(()); }
        tick(out.len() - base, &mut next);
        if b.overrun() { return Err(Fail::Corrupt); }
        if st.ppm_mode {
            let m = st.ppm.as_mut().ok_or(Fail::Corrupt)?;
            let ch = m.decode(&mut b).ok_or(Fail::Corrupt)?;
            if ch != st.esc { out.push(ch as u8); continue; }
            let next = m.decode(&mut b).ok_or(Fail::Corrupt)?;
            match next {
                0 => { st.tables(&mut b)?; }
                2 => { st.tables_read = false; return Ok(()); }
                3 => {
                    let first = m.decode(&mut b).ok_or(Fail::Corrupt)?;
                    let mut len = (first & 7) + 1;
                    if len == 7 { len = m.decode(&mut b).ok_or(Fail::Corrupt)? + 7; }
                    else if len == 8 { let hi = m.decode(&mut b).ok_or(Fail::Corrupt)?; let lo = m.decode(&mut b).ok_or(Fail::Corrupt)?; len = hi << 8 | lo; }
                    let mut code = Vec::with_capacity(len as usize);
                    for _ in 0..len { code.push(m.decode(&mut b).ok_or(Fail::Corrupt)? as u8); }
                    let pos = out.len() as u64;
                    st.add_vm(first, &code, pos)?;
                }
                4 => {
                    let mut dist = 0u32;
                    for _ in 0..3 { dist = (dist << 8) | m.decode(&mut b).ok_or(Fail::Corrupt)?; }
                    let len = m.decode(&mut b).ok_or(Fail::Corrupt)?;
                    copy_match(out, len + 32, dist as u64 + 2, cap);
                }
                5 => { let len = m.decode(&mut b).ok_or(Fail::Corrupt)?; copy_match(out, len + 4, 1, cap); }
                _ => out.push(ch as u8),
            }
            continue;
        }
        let num = st.ld.decode(&mut b);
        if num < 256 { out.push(num as u8); continue; }
        if num >= 271 {
            let k = (num - 271) as usize;
            if k >= 28 { return Err(Fail::Corrupt); }
            let mut len = LDECODE[k] + 3 + b.read(LBITS[k]) as u32;
            let dn = st.dd.decode(&mut b) as usize;
            if dn >= DC3 { return Err(Fail::Corrupt); }
            let mut dist = st.dtab.0[dn] + 1;
            let bits = st.dtab.1[dn];
            if bits > 0 {
                if dn > 9 {
                    if bits > 4 { dist = dist.wrapping_add((b.read(bits - 4) as u32) << 4); }
                    if st.low_rep > 0 { st.low_rep -= 1; dist = dist.wrapping_add(st.prev_low); }
                    else {
                        let low = st.ldd.decode(&mut b);
                        if low == 16 { st.low_rep = 15; dist = dist.wrapping_add(st.prev_low); }
                        else { dist = dist.wrapping_add(low); st.prev_low = low; }
                    }
                } else { dist = dist.wrapping_add(b.read(bits) as u32); }
            }
            if dist >= 0x2000 { len += 1; if dist >= 0x40000 { len += 1; } }
            st.old = [dist, st.old[0], st.old[1], st.old[2]];
            st.last_len = len;
                copy_match(out, len, dist as u64, cap);
            continue;
        }
        if num == 256 {
            let bf = b.get16();
            let (new_table, new_file) = if bf & 0x8000 != 0 { b.add(1); (true, false) } else { b.add(2); (bf & 0x4000 != 0, true) };
            st.tables_read = !new_table;
            if new_file { return Ok(()); }
            if new_table { st.tables(&mut b)?; }
            continue;
        }
        if num == 257 {
            let first = b.byte_in();
            let mut len = (first & 7) + 1;
            if len == 7 { len = b.byte_in() + 7; } else if len == 8 { len = b.read(16) as u32; }
            let mut code = Vec::with_capacity(len as usize);
            for _ in 0..len { code.push(b.byte_in() as u8); }
            let pos = out.len() as u64;
            st.add_vm(first, &code, pos)?;
            continue;
        }
        if num == 258 { if st.last_len != 0 { copy_match(out, st.last_len, st.old[0] as u64, cap); } continue; }
        if num < 263 {
            let k = (num - 259) as usize;
            let dist = st.old[k];
            for i in (1..=k).rev() { st.old[i] = st.old[i - 1]; }
            st.old[0] = dist;
            let ln = st.rd.decode(&mut b) as usize;
            if ln >= 28 { return Err(Fail::Corrupt); }
            let len = LDECODE[ln] + 2 + b.read(LBITS[ln]) as u32;
            st.last_len = len;
            copy_match(out, len, dist as u64, cap);
            continue;
        }
        let k = (num - 263) as usize;
        let dist = SDDECODE[k] + 1 + b.read(SDBITS[k]) as u32;
        st.old = [dist, st.old[0], st.old[1], st.old[2]];
        st.last_len = 2;
        copy_match(out, 2, dist as u64, cap);
    }
}

fn rd2(d: &[u8], i: usize) -> u32 { u16::from_le_bytes([d[i], d[i + 1]]) as u32 }

/// A RAR 3 Unicode file name: the plain name `a`, then a 0 and the coded name
fn rar3_name(raw: &[u8]) -> Vec<u8> {
    let Some(z) = raw.iter().position(|&c| c == 0) else { return raw.to_vec() };
    let (a, enc) = (&raw[..z], &raw[z + 1..]);
    let mut w: Vec<u32> = Vec::new();
    let mut e = 0usize;
    let get = |e: &mut usize| -> Option<u32> { let c = *enc.get(*e)?; *e += 1; Some(c as u32) };
    let Some(high) = get(&mut e) else { return a.to_vec() };
    let (mut flags, mut fbits) = (0u32, 0u32);
    while e < enc.len() && w.len() < 1024 {
        if fbits == 0 { let Some(c) = get(&mut e) else { break }; flags = c; fbits = 8; }
        match flags >> 6 {
            0 => { let Some(c) = get(&mut e) else { break }; w.push(c); }
            1 => { let Some(c) = get(&mut e) else { break }; w.push(c + (high << 8)); }
            2 => { let (Some(lo), Some(hi)) = (get(&mut e), get(&mut e)) else { break }; w.push(lo | hi << 8); }
            _ => {
                let Some(l) = get(&mut e) else { break };
                if l & 0x80 != 0 {
                    let Some(corr) = get(&mut e) else { break };
                    for _ in 0..(l & 0x7f) + 2 { let p = w.len(); w.push(((*a.get(p).unwrap_or(&0) as u32 + corr) & 0xff) + (high << 8)); }
                } else { for _ in 0..l + 2 { let p = w.len(); w.push(*a.get(p).unwrap_or(&0) as u32); } }
            }
        }
        flags = (flags << 2) & 0xff;
        fbits -= 2;
    }
    let mut o = Vec::new();
    for c in w { if c == 0 { break; } put_utf8(&mut o, c); }
    o
}

fn rar3(d: &[u8], start: usize, list: bool, f: &mut dyn FnMut(&Member) -> bool) -> Result<u32, Fail> {
    let mut st = R3::new();
    let mut out: Vec<u8> = Vec::new();
    let mut tmp = Vec::new();
    let mut n = 0;
    let mut solid_ok = true;
    let mut p = start;
    while p + 7 <= d.len() {
        let ty = d[p + 2];
        let fl = rd2(d, p + 3);
        let hs = rd2(d, p + 5) as usize;
        if hs < 7 || p + hs > d.len() { return Err(Fail::Corrupt); }
        let add = if fl & 0x8000 != 0 && p + 11 <= d.len() { rd4(d, p + 7) as u64 } else { 0 };
        let mut add = add;
        if (ty == 0x74 || ty == 0x7a) && fl & 0x100 != 0 && p + 36 <= d.len() { add |= (rd4(d, p + 32) as u64) << 32; }
        let hp = p;
        let dstart = p + hs;
        let dend = usize::try_from(add).ok().and_then(|a| dstart.checked_add(a)).ok_or(Fail::Corrupt)?;
        p = dend;
        match ty {
            0x73 => { if fl & 0x80 != 0 { return Err(Fail::Encrypted); } continue; }
            0x7b => break,
            0x74 => {}
            _ => continue,
        }
        if hs < 32 { return Err(Fail::Corrupt); }
        let usize_ = rd4(d, hp + 11) as u64 | if fl & 0x100 != 0 && hs >= 40 { (rd4(d, hp + 36) as u64) << 32 } else { 0 };
        let fcrc = rd4(d, hp + 16);
        let ver = d[hp + 24];
        let method = d[hp + 25];
        let nl = rd2(d, hp + 26) as usize;
        let noff = hp + 32 + if fl & 0x100 != 0 { 8 } else { 0 };
        let raw = d.get(noff..noff + nl).filter(|_| noff + nl <= hp + hs).ok_or(Fail::Corrupt)?;
        let name = safe_name(&if fl & 0x200 != 0 { rar3_name(raw) } else { raw.to_vec() });
        let dir = fl & 0xe0 == 0xe0;
        let solid = fl & 0x10 != 0;
        let src = d.get(dstart..dend).ok_or(Fail::Corrupt);
        let data: Result<&[u8], Fail>;
        if dir || list { data = Ok(&[]); }
        else if fl & 0x04 != 0 { data = Err(Fail::Encrypted); solid_ok = false; }
        else if fl & 0x03 != 0 { data = Err(Fail::Split); solid_ok = false; }
        else if method == 0x30 {
            data = src.and_then(|s| if crc32(s) != fcrc { Err(Fail::Crc) } else { Ok(s) });
        } else if !(29..=36).contains(&ver) { data = Err(Fail::Method); solid_ok = false; }
        else if solid && !solid_ok { data = Err(Fail::Corrupt); }
        else {
            if !solid { st.reset(); out.clear(); }
            let base = out.len();
            let r = src.and_then(|s| {
                reserve(&mut out, usize_)?;
                r3_unpack(&mut st, s, &mut out, base.saturating_add(usize_ as usize))
            });
            data = match r {
                Err(e) => { solid_ok = false; Err(e) }
                Ok(()) if out.len() - base != usize_ as usize => { solid_ok = false; Err(Fail::Corrupt) }
                Ok(()) => {
                    let v = filtered(&out, base, &st.stack, &mut tmp);
                    st.stack.retain(|fl| fl.start >= out.len() as u64);
                    match v { Ok(v) if crc32(v) != fcrc => Err(Fail::Crc), v => v }
                }
            };
        }
        if name.is_empty() { continue; }
        n += 1;
        if !f(&Member { name: &name, dir, data, size: usize_, at: dend }) { break; }
    }
    Ok(n)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;

    #[test]
    fn names_are_made_safe() {
        assert_eq!(safe_name(b"a/b\\c.txt"), b"a\\b\\c.txt");
        assert_eq!(safe_name(b"../../etc/passwd"), b"etc\\passwd");
        assert_eq!(safe_name(b"C:\\WRLD\\x"), b"WRLD\\x");
        assert_eq!(safe_name(b"/abs/./x. "), b"abs\\x");
        assert_eq!(safe_name(b"a:b"), b"a_b");
    }
    #[test]
    fn crc() { assert_eq!(crc32(b"123456789"), 0xcbf4_3926); }

    /// The RAR test archives from libarchive's test suite (BSD licence), when downloaded
    /// to $WRLD_RAR_TESTS: every member must decode with a matching checksum.
    #[test]
    fn rar_test_archives() {
        let Ok(dir) = std::env::var("WRLD_RAR_TESTS") else { return };
        let mut names: Vec<_> = std::fs::read_dir(&dir).unwrap().filter_map(|e| e.ok()).map(|e| e.path()).filter(|p| p.extension().is_some_and(|x| x == "rar" || x == "zip" || x == "exe")).collect();
        names.sort();
        let mut bad = 0;
        for p in names {
            let d = std::fs::read(&p).unwrap();
            let mut report = String::new();
            let r = extract(&d, &mut |m| {
                let s = match &m.data { Ok(v) => std::format!("ok {}", v.len()), Err(e) => { std::format!("{:?}", e) } };
                if let Ok(dump) = std::env::var("WRLD_RAR_DUMP") { if let Ok(v) = &m.data { let _ = std::fs::write(std::format!("{}/{}", dump, String::from_utf8_lossy(m.name).replace('\\', "_")), v); } }
                report += &std::format!("  {} {}{}\n", String::from_utf8_lossy(m.name), if m.dir { "(dir) " } else { "" }, s);
                true
            });
            std::println!("{}: {:?}\n{}", p.display(), r, report);
            if r.is_err() || report.contains("Crc") || report.contains("Corrupt") || report.contains("Method") { bad += 1; }
        }
        std::println!("archives with problems: {}", bad);
    }
}
