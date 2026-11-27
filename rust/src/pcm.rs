//! Uncompressed and simply compressed audio files for THE WRLD Media Player (was
//! src/media/pcm.c): WAV (PCM 8/16/24/32-bit, 32/64-bit float, A-law, mu-law, IMA ADPCM,
//! Microsoft ADPCM, and WAVE_FORMAT_EXTENSIBLE around those), AIFF and AIFC (big- and
//! little-endian PCM, float, A-law, mu-law) and Sun/NeXT .au. Output is 16-bit stereo.
//!
//! The C interface (src/include/mediadec.h: pcm_open, pcm_read, pcm_close) is at the end,
//! for the kernel and for host test programs (feature `htmldoc`).
//!
//! Changes from the C version: an AIFF sound chunk whose offset runs past the end of the
//! file (where the C code's 32-bit sums could wrap and read outside the file) has no
//! samples, and an ADPCM block size smaller than its own header is refused as a bad header.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Enc { Int, Float, Ulaw, Alaw, Ima, MsAdpcm }

/// What pcm_open reports about a file.
pub struct Info { pub rate: i32, pub channels: i32, pub ms: u32, pub kbps: i32, pub format: &'static str }

/// An open file: the sample data is `data[start..start + len]` of the caller's buffer.
pub struct Pcm {
    start: usize, len: u32,
    frames: u32, pos: u32,
    ch: i32, bits: i32, enc: Enc, be: bool,
    block: u32, spb: u32,                       /* ADPCM: bytes and frames per block */
    blk: Vec<i16>, blk_n: u32, blk_index: u32,  /* the decoded block */
}

fn le16(d: &[u8], o: usize) -> u32 { d[o] as u32 | (d[o + 1] as u32) << 8 }
fn le32(d: &[u8], o: usize) -> u32 { le16(d, o) | le16(d, o + 2) << 16 }
fn be16(d: &[u8], o: usize) -> u32 { (d[o] as u32) << 8 | d[o + 1] as u32 }
fn be32(d: &[u8], o: usize) -> u32 { be16(d, o) << 16 | be16(d, o + 2) }

fn ulaw(u: u8) -> i16 {
    let u = !u;
    let mut t = (((u & 0x0F) as i32) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    (if u & 0x80 != 0 { 0x84 - t } else { t - 0x84 }) as i16
}
fn alaw(a: u8) -> i16 {
    let a = a ^ 0x55;
    let mut t = ((a & 0x0F) as i32) << 4;
    let seg = ((a & 0x70) >> 4) as i32;
    if seg == 0 { t += 8; } else if seg == 1 { t += 0x108; } else { t += 0x108; t <<= seg - 1; }
    (if a & 0x80 != 0 { t } else { -t }) as i16
}
/// 80-bit IEEE extended (the AIFF sample rate)
fn ext80(d: &[u8], o: usize) -> u32 {
    let e = (((d[o] & 0x7F) as i32) << 8 | d[o + 1] as i32) - 16383 - 31;
    let m = be32(d, o + 2);
    if e > 0 { return 0; }
    if e < -31 { 0 } else { m >> -e }
}

const IMA_STEP: [i32; 89] = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767];
const IMA_INDEX: [i32; 16] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];
const MS_COEF: [[i32; 2]; 7] = [[256, 0], [512, -256], [0, 0], [192, 64], [240, 0], [460, -208], [392, -232]];
const MS_ADAPT: [i32; 16] = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230];
fn clamp16(v: i32) -> i32 { v.clamp(-32768, 32767) }
/// C's (int) of a double on x86: NaN and values out of range give INT_MIN.
fn d2i(v: f64) -> i32 { if v >= -2147483648.0 && v < 2147483648.0 { v as i32 } else { i32::MIN } }

/// The `snprintf(err, ...)` texts of the C version (bytes: an AIFC compression name is
/// copied as it is in the file).
pub type R<T> = Result<T, Vec<u8>>;
fn err<T>(s: String) -> R<T> { Err(s.into_bytes()) }

pub fn open(d: &[u8]) -> R<(Pcm, Info)> {
    let n = d.len() as u64;
    let mut f = Pcm { start: 0, len: 0, frames: 0, pos: 0, ch: 0, bits: 0, enc: Enc::Int, be: false, block: 0, spb: 0,
                      blk: Vec::new(), blk_n: 0, blk_index: u32::MAX };
    let mut has_data = false;
    let mut small_block = false;
    let mut rate: u32 = 0;
    let what: &'static str;
    if n >= 12 && &d[..4] == b"RIFF" && &d[8..12] == b"WAVE" {
        let (mut fmt, mut ba) = (0u32, 0u32);
        let mut o = 12u64;
        while o + 8 <= n {
            let u = o as usize;
            let cl = le32(d, u + 4);
            if &d[u..u + 4] == b"fmt " && cl >= 16 && o + 8 + 16 <= n {
                fmt = le16(d, u + 8); f.ch = le16(d, u + 10) as i32; rate = le32(d, u + 12); ba = le16(d, u + 20); f.bits = le16(d, u + 22) as i32;
                if fmt == 0xFFFE && cl >= 40 && o + 8 + 26 <= n { fmt = le16(d, u + 32); }
            } else if &d[u..u + 4] == b"data" {
                f.start = u + 8; f.len = (cl as u64).min(n - o - 8) as u32; has_data = true;
            }
            if cl as u64 > n { break; }
            o += 8 + cl as u64 + (cl & 1) as u64;
        }
        match fmt {
            1 => { f.enc = Enc::Int; what = "WAV (PCM)"; }
            3 => { f.enc = Enc::Float; what = "WAV (float)"; }
            6 => { f.enc = Enc::Alaw; f.bits = 8; what = "WAV (A-law)"; }
            7 => { f.enc = Enc::Ulaw; f.bits = 8; what = "WAV (mu-law)"; }
            0x11 => { f.enc = Enc::Ima; what = "WAV (IMA ADPCM)"; }
            2 => { f.enc = Enc::MsAdpcm; what = "WAV (MS ADPCM)"; }
            _ => return err(format!("WAV format 0x{:x} is not supported", fmt)),
        }
        if f.enc == Enc::Ima || f.enc == Enc::MsAdpcm {
            let head = if f.enc == Enc::Ima { 4 } else { 7 } * f.ch.max(0) as u32;
            if ba == 0 || f.ch < 1 || f.ch > 2 { return Err(b"bad ADPCM header".to_vec()); }
            /* a block smaller than its header: the C code's sums wrapped; refused below,
               after the checks the C code made */
            if ba < head { small_block = true; }
            f.block = ba;
            let per = 4 * f.ch as u32;
            f.spb = if f.enc == Enc::Ima { ba.wrapping_sub(head).wrapping_mul(8) / per + 1 } else { ba.wrapping_sub(head).wrapping_mul(8) / per + 2 };
            let blocks = f.len / ba + (f.len % ba != 0) as u32;
            f.frames = blocks.wrapping_mul(f.spb);
            f.bits = 4;
        }
    } else if n >= 12 && &d[..4] == b"FORM" && (&d[8..12] == b"AIFF" || &d[8..12] == b"AIFC") {
        let aifc = &d[8..12] == b"AIFC";
        f.be = true; f.enc = Enc::Int; what = if aifc { "AIFC" } else { "AIFF" };
        let mut o = 12u64;
        while o + 8 <= n {
            let u = o as usize;
            let cl = be32(d, u + 4);
            if &d[u..u + 4] == b"COMM" && cl >= 18 && o + 26 <= n {
                f.ch = be16(d, u + 8) as i32; f.bits = be16(d, u + 14) as i32; rate = ext80(d, u + 16);
                if aifc && cl >= 22 && o + 30 <= n {
                    let c = &d[u + 26..u + 30];
                    if c == b"sowt" { f.be = false; }
                    else if c == b"fl32" || c == b"FL32" { f.enc = Enc::Float; f.bits = 32; }
                    else if c == b"fl64" || c == b"FL64" { f.enc = Enc::Float; f.bits = 64; }
                    else if c == b"ulaw" || c == b"ULAW" { f.enc = Enc::Ulaw; f.bits = 8; }
                    else if c == b"alaw" || c == b"ALAW" { f.enc = Enc::Alaw; f.bits = 8; }
                    else if c != b"NONE" && c != b"twos" {
                        let mut m = b"AIFC compression ".to_vec();
                        m.extend(c.iter().take_while(|&&b| b != 0));
                        m.extend_from_slice(b" is not supported");
                        return Err(m);
                    }
                }
            } else if &d[u..u + 4] == b"SSND" && cl >= 8 && o + 16 <= n {
                let off = be32(d, u + 8);
                let s = o + 16 + off as u64;
                has_data = true;
                /* the C code's 32-bit length (it wraps when off is larger than the chunk),
                   cut to what the file holds */
                let len = cl.wrapping_sub(8).wrapping_sub(off);
                if s > n { f.start = n as usize; f.len = 0; }
                else { f.start = s as usize; f.len = (len as u64).min(n - s) as u32; }
            }
            if cl as u64 > n { break; }
            o += 8 + cl as u64 + (cl & 1) as u64;
        }
    } else if n >= 24 && &d[..4] == b".snd" {
        let mut off = be32(d, 4) as u64;
        let (size, enc) = (be32(d, 8), be32(d, 12));
        rate = be32(d, 16); f.ch = be32(d, 20) as i32; f.be = true;
        if off > n { off = n; }
        f.start = off as usize; has_data = true;
        f.len = if size == u32::MAX || size as u64 > n - off { (n - off) as u32 } else { size };
        what = "Sun audio";
        match enc {
            1 => { f.enc = Enc::Ulaw; f.bits = 8; }
            2 => { f.enc = Enc::Int; f.bits = 8; }
            3 => { f.enc = Enc::Int; f.bits = 16; }
            4 => { f.enc = Enc::Int; f.bits = 24; }
            5 => { f.enc = Enc::Int; f.bits = 32; }
            6 => { f.enc = Enc::Float; f.bits = 32; }
            7 => { f.enc = Enc::Float; f.bits = 64; }
            27 => { f.enc = Enc::Alaw; f.bits = 8; }
            _ => return err(format!(".au encoding {} is not supported", enc)),
        }
    } else { return Err(b"not a WAV, AIFF or .au file".to_vec()); }
    if !has_data || f.ch < 1 || f.ch > 8 || !(1000..=384000).contains(&rate) { return Err(b"the file's audio header is damaged".to_vec()); }
    if f.enc == Enc::Int && ![8, 16, 24, 32].contains(&f.bits) { return err(format!("{}-bit samples are not supported", f.bits)); }
    if f.enc == Enc::Float && f.bits != 32 && f.bits != 64 { return err(format!("{}-bit float is not supported", f.bits)); }
    if small_block { return Err(b"bad ADPCM header".to_vec()); }
    if f.enc != Enc::Ima && f.enc != Enc::MsAdpcm { f.frames = f.len / (f.ch * (f.bits / 8)) as u32; }
    if f.spb != 0 {
        let want = f.spb as usize * f.ch as usize;
        if f.blk.try_reserve_exact(want).is_err() { return Err(b"not enough memory".to_vec()); }
        f.blk.resize(want, 0);
    }
    let ms = (f.frames as u64 * 1000 / rate as u64) as u32;
    let kbps = if f.spb != 0 { (f.len as u64 * 8 / (if ms != 0 { ms } else { 1 }) as u64) as i32 }
               else { (rate as u64 * f.ch as u64 * f.bits as u64 / 1000) as i32 };
    let info = Info { rate: rate as i32, channels: f.ch, ms, kbps, format: what };
    Ok((f, info))
}

impl Pcm {
    /// Decode ADPCM block `b` into self.blk (interleaved, self.ch channels).
    fn adpcm_block(&mut self, d: &[u8], b: u32) {
        let base = self.start + b as usize * self.block as usize;
        let avail = (self.len as u64 - b as u64 * self.block as u64).min(self.block as u64) as usize;
        let p = &d[base..base + avail];
        let ch = self.ch as usize;
        for x in self.blk.iter_mut() { *x = 0; }
        self.blk_n = 0; self.blk_index = b;
        if self.enc == Enc::Ima {
            if avail < 4 * ch { return; }
            let (mut pred, mut idx) = ([0i32; 2], [0i32; 2]);
            for c in 0..ch {
                pred[c] = le16(p, c * 4) as i16 as i32; idx[c] = (p[c * 4 + 2] as i32).min(88);
                self.blk[c] = pred[c] as i16;
            }
            let (mut n, mut o) = (1u32, 4 * ch);
            /* then groups of 4 bytes (8 samples) per channel, in turn */
            while o + 4 * ch <= avail && n + 8 <= self.spb {
                for c in 0..ch {
                    for k in 0..8 {
                        let nib = ((p[o + c * 4 + k / 2] >> ((k & 1) * 4)) & 15) as i32;
                        let step = IMA_STEP[idx[c] as usize];
                        let mut diff = step >> 3;
                        if nib & 1 != 0 { diff += step >> 2; }
                        if nib & 2 != 0 { diff += step >> 1; }
                        if nib & 4 != 0 { diff += step; }
                        pred[c] = clamp16(if nib & 8 != 0 { pred[c] - diff } else { pred[c] + diff });
                        idx[c] = (idx[c] + IMA_INDEX[nib as usize]).clamp(0, 88);
                        self.blk[(n as usize + k) * ch + c] = pred[c] as i16;
                    }
                }
                o += 4 * ch; n += 8;
            }
            self.blk_n = n;
        } else {
            if avail < 7 * ch { return; }
            let (mut pi, mut delta, mut s1, mut s2) = ([0usize; 2], [0i32; 2], [0i32; 2], [0i32; 2]);
            for c in 0..ch {
                pi[c] = if p[c] < 7 { p[c] as usize } else { 0 };
                delta[c] = le16(p, ch + c * 2) as i16 as i32;
                s1[c] = le16(p, 3 * ch + c * 2) as i16 as i32;
                s2[c] = le16(p, 5 * ch + c * 2) as i16 as i32;
                self.blk[c] = s2[c] as i16; self.blk[ch + c] = s1[c] as i16;
            }
            let (mut n, mut o, mut c, mut half) = (2u32, 7 * ch, 0usize, false);
            while o < avail && n < self.spb {
                let nib = if half { p[o] & 15 } else { p[o] >> 4 } as i32;
                let sn = if nib & 8 != 0 { nib - 16 } else { nib };
                /* the C code's int arithmetic, which wraps on hostile input */
                let lin = (s1[c] * MS_COEF[pi[c]][0] + s2[c] * MS_COEF[pi[c]][1]) / 256;
                let pred = clamp16(lin.wrapping_add(sn.wrapping_mul(delta[c])));
                s2[c] = s1[c]; s1[c] = pred;
                delta[c] = delta[c].wrapping_mul(MS_ADAPT[nib as usize]) / 256;
                if delta[c] < 16 { delta[c] = 16; }
                self.blk[n as usize * ch + c] = pred as i16;
                c += 1;
                if c == ch { c = 0; n += 1; }
                if half { o += 1; }
                half = !half;
            }
            self.blk_n = n;
        }
    }
    fn sample(&self, d: &[u8], o: usize) -> i32 {
        let b = self.bits / 8;
        match self.enc {
            Enc::Ulaw => return ulaw(d[o]) as i32,
            Enc::Alaw => return alaw(d[o]) as i32,
            Enc::Float => {
                let mut v = if b == 4 {
                    f32::from_bits(if self.be { be32(d, o) } else { le32(d, o) }) as f64
                } else {
                    f64::from_bits(if self.be { (be32(d, o) as u64) << 32 | be32(d, o + 4) as u64 } else { (le32(d, o + 4) as u64) << 32 | le32(d, o) as u64 })
                };
                if v.is_nan() { v = 0.0; }
                return clamp16(d2i(v * 32767.0));
            }
            _ => {}
        }
        match b {
            1 => if self.be { (d[o] as i8 as i32) << 8 } else { (d[o] as i32 - 128) << 8 },   /* WAV 8-bit is unsigned, AIFF signed */
            2 => (if self.be { be16(d, o) } else { le16(d, o) }) as i16 as i32,
            3 => (if self.be { be16(d, o) } else { le16(d, o + 1) }) as i16 as i32,
            _ => (if self.be { be16(d, o) } else { le16(d, o + 2) }) as i16 as i32,
        }
    }
    /// Up to out.len() / 2 stereo frames; the number written.
    pub fn read(&mut self, d: &[u8], out: &mut [i16]) -> usize {
        let max = out.len() / 2;
        let mut n = 0;
        while n < max && self.pos < self.frames {
            let (l, r);
            if self.spb != 0 {
                let (b, k) = (self.pos / self.spb, self.pos % self.spb);
                if b != self.blk_index { self.adpcm_block(d, b); }
                if k >= self.blk_n { self.pos = (b + 1).wrapping_mul(self.spb); continue; }
                let ch = self.ch as usize;
                l = self.blk[k as usize * ch] as i32;
                r = if ch > 1 { self.blk[k as usize * ch + 1] as i32 } else { l };
            } else {
                let bps = (self.bits / 8) as usize;
                let p = self.start + self.pos as usize * self.ch as usize * bps;
                let mut a = self.sample(d, p);
                let mut z = if self.ch > 1 { self.sample(d, p + bps) } else { a };
                if self.ch > 2 {                                    /* fold the centre and surrounds in */
                    let c = self.sample(d, p + 2 * bps);
                    a = clamp16((a * 3 + c * 2) / 4); z = clamp16((z * 3 + c * 2) / 4);
                }
                l = a; r = z;
            }
            out[n * 2] = l as i16; out[n * 2 + 1] = r as i16;
            n += 1; self.pos += 1;
        }
        n
    }
}

// ---------------------------------------------------------------- the C interface

#[cfg(any(feature = "kernel", feature = "htmldoc"))]
mod ffi {
    use super::*;
    use alloc::boxed::Box;
    use core::ffi::{c_char, c_int};

    /// mdec_info_t (src/include/mediadec.h), up to the format name.
    #[repr(C)]
    pub struct MdecInfo { rate: c_int, channels: c_int, ms: u32, kbps: c_int, video: c_int, width: c_int, height: c_int, fps: u32, format: [u8; 40] }
    pub fn layout() -> [u32; 4] {
        use core::mem::offset_of;
        [offset_of!(MdecInfo, ms) as u32, offset_of!(MdecInfo, kbps) as u32, offset_of!(MdecInfo, format) as u32, 40]
    }
    /// The open file and the caller's buffer, which stays valid until pcm_close.
    pub struct Handle { pcm: Pcm, data: *const u8, len: usize }
    unsafe fn put(buf: *mut c_char, cap: c_int, s: &[u8]) {
        if buf.is_null() || cap <= 0 { return; }
        let n = s.len().min(cap as usize - 1);
        core::ptr::copy_nonoverlapping(s.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }

    #[no_mangle]
    pub unsafe extern "C" fn pcm_open(d: *const u8, n: u32, info: *mut MdecInfo, err: *mut c_char, errcap: c_int) -> *mut Handle {
        let data = if d.is_null() || n == 0 { &[][..] } else { core::slice::from_raw_parts(d, n as usize) };
        match open(data) {
            Err(e) => { put(err, errcap, &e); core::ptr::null_mut() }
            Ok((pcm, i)) => {
                let inf = &mut *info;
                inf.rate = i.rate; inf.channels = i.channels; inf.ms = i.ms; inf.kbps = i.kbps;
                put(inf.format.as_mut_ptr() as *mut c_char, 40, i.format.as_bytes());
                Box::into_raw(Box::new(Handle { pcm, data: d, len: data.len() }))
            }
        }
    }
    #[no_mangle]
    pub unsafe extern "C" fn pcm_read(h: *mut Handle, out: *mut i16, max: c_int) -> c_int {
        let h = &mut *h;
        if max <= 0 { return 0; }
        let data = if h.len == 0 { &[][..] } else { core::slice::from_raw_parts(h.data, h.len) };
        h.pcm.read(data, core::slice::from_raw_parts_mut(out, max as usize * 2)) as c_int
    }
    #[no_mangle]
    pub unsafe extern "C" fn pcm_close(h: *mut Handle) { if !h.is_null() { drop(Box::from_raw(h)); } }
}
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub use ffi::layout;

#[cfg(test)]
mod tests {
    use super::*;
    fn wav(fmt: u16, ch: u16, rate: u32, ba: u16, bits: u16, data: &[u8]) -> Vec<u8> {
        let mut v = b"RIFF\0\0\0\0WAVEfmt \x10\0\0\0".to_vec();
        for x in [fmt, ch] { v.extend_from_slice(&x.to_le_bytes()); }
        v.extend_from_slice(&rate.to_le_bytes()); v.extend_from_slice(&0u32.to_le_bytes());
        for x in [ba, bits] { v.extend_from_slice(&x.to_le_bytes()); }
        v.extend_from_slice(b"data"); v.extend_from_slice(&(data.len() as u32).to_le_bytes()); v.extend_from_slice(data);
        v
    }
    #[test]
    fn pcm16_stereo() {
        let d = wav(1, 2, 44100, 4, 16, &[1, 0, 0xff, 0xff, 0, 0x80, 0xff, 0x7f]);
        let (mut p, i) = open(&d).ok().unwrap();
        assert_eq!((i.rate, i.channels, i.format), (44100, 2, "WAV (PCM)"));
        let mut out = [0i16; 8];
        assert_eq!(p.read(&d, &mut out), 2);
        assert_eq!(&out[..4], &[1, -1, -32768, 32767]);
    }
    #[test]
    fn refused() {
        assert_eq!(open(b"nothing").err().unwrap(), b"not a WAV, AIFF or .au file");
        assert_eq!(open(&wav(0x55, 1, 44100, 1, 16, &[])).err().unwrap(), b"WAV format 0x55 is not supported");
        assert_eq!(open(&wav(0x11, 2, 44100, 4, 4, &[0; 8])).err().unwrap(), b"bad ADPCM header");   /* block smaller than its header */
        assert_eq!(open(&wav(1, 1, 999, 2, 16, &[0; 8])).err().unwrap(), b"the file's audio header is damaged");
    }
    #[test]
    fn aiff_offset_past_end_has_no_samples() {
        let mut v = b"FORM\0\0\0\0AIFFCOMM\0\0\0\x12\0\x01\0\0\0\x10\0\x10\x40\x0e\xac\x44\0\0\0\0\0\0SSND\0\0\0\x08\xff\xff\xff\xf0\0\0\0\0".to_vec();
        v.extend_from_slice(&[0x12; 64]);
        let (mut p, _) = open(&v).ok().unwrap();
        let mut out = [0i16; 64];
        assert_eq!(p.read(&v, &mut out), 0);
    }
    #[test]
    fn hostile_adpcm_does_not_panic() {
        for seed in 0..300u32 {
            let mut x = seed.wrapping_mul(2654435761);
            let data: Vec<u8> = (0..700).map(|_| { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x as u8 }).collect();
            for (fmt, ba) in [(0x11u16, 36u16), (2, 14), (2, 7), (0x11, 9)] {
                let d = wav(fmt, 1 + (seed % 2) as u16, 8000, ba, 4, &data);
                if let Ok((mut p, _)) = open(&d) { let mut out = [0i16; 4096]; while p.read(&d, &mut out) > 0 {} }
            }
        }
    }
}
