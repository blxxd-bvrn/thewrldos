//! Image decoders for the browser, the wallpapers, the sign-in screen, Image Viewer and Paint
//! (`kernel/imgk.rs` gives them canvases; was `src/apps/imgdec.c` and Paint's BMP reader):
//! PNG (every colour type and bit depth, Adam7 interlacing), GIF (first frame, transparency,
//! interlacing), baseline and progressive JPEG (Huffman, any sampling factors, restart
//! markers) and 24/32-bit BMP. Pixels are 0xAARRGGBB.
//!
//! Valid images decode to exactly what the C decoders produced. Damaged ones can no longer
//! make a decoder read past the file or its tables (chunk lengths, palette and Huffman
//! indexes, sampling factors, coefficient positions and table numbers are checked; a zero
//! sampling factor divided by zero in C), or write through an allocation that failed.

use alloc::vec::Vec;

/// Where a decoder puts its pixels: `begin` gives a w*h buffer, already white, or None when
/// the image is too large.
pub trait Sink { fn begin(&mut self, w: u32, h: u32) -> Option<&mut [u32]>; }
pub type R = Result<(), &'static str>;

fn rgb(r: u32, g: u32, b: u32) -> u32 { 0xFF00_0000 | (r << 16) | (g << 8) | b }
fn be32(d: &[u8], o: usize) -> u32 { u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) }
fn at(d: &[u8], o: usize) -> u8 { d.get(o).copied().unwrap_or(0) }
fn le16(d: &[u8], o: usize) -> u32 { at(d, o) as u32 | (at(d, o + 1) as u32) << 8 }

// ---------------------------------------------------------------- PNG

fn paeth(a: i32, b: i32, c: i32) -> i32 {
    let p = a + b - c;
    let (pa, pb, pc) = ((p - a).abs(), (p - b).abs(), (p - c).abs());
    if pa <= pb && pa <= pc { a } else if pb <= pc { b } else { c }
}
const AX: [u32; 7] = [0, 4, 0, 2, 0, 1, 0];
const AY: [u32; 7] = [0, 0, 4, 0, 2, 0, 1];
const DX: [u32; 7] = [8, 8, 4, 4, 2, 2, 1];
const DY: [u32; 7] = [8, 8, 8, 4, 4, 2, 2];

pub fn png(d: &[u8], sink: &mut dyn Sink) -> R {
    let n = d.len();
    let (mut w, mut h, mut depth, mut ct, mut inter) = (0u32, 0u32, 0u32, 0u32, 0u32);
    let mut pal = [0xFF00_0000u32; 256];
    let (mut trns_g, mut trns_r, mut trns_gg, mut trns_b) = (-1i32, -1i32, -1i32, -1i32);
    let mut idat: Vec<u8> = Vec::new();
    let mut o = 8usize;
    while o + 12 <= n {
        let len = be32(d, o) as usize;
        let Some(end) = (o + 12).checked_add(len).filter(|&e| e <= n) else { break };
        let ty = &d[o + 4..o + 8];
        let cd = &d[o + 8..end - 4];
        match ty {
            b"IHDR" if len >= 13 => { w = be32(cd, 0); h = be32(cd, 4); depth = cd[8] as u32; ct = cd[9] as u32; inter = cd[12] as u32; }
            b"PLTE" => { for i in 0..(len / 3).min(256) { pal[i] = rgb(cd[i * 3] as u32, cd[i * 3 + 1] as u32, cd[i * 3 + 2] as u32); } }
            b"tRNS" => {
                if ct == 3 { for i in 0..len.min(256) { pal[i] = (pal[i] & 0xFF_FFFF) | (cd[i] as u32) << 24; } }
                else if ct == 0 && len >= 2 { trns_g = (cd[0] as i32) << 8 | cd[1] as i32; }
                else if ct == 2 && len >= 6 { trns_r = (cd[0] as i32) << 8 | cd[1] as i32; trns_gg = (cd[2] as i32) << 8 | cd[3] as i32; trns_b = (cd[4] as i32) << 8 | cd[5] as i32; }
            }
            b"IDAT" => { if idat.try_reserve(len).is_err() { return Err("PNG too large"); } idat.extend_from_slice(cd); }
            b"IEND" => break,
            _ => {}
        }
        o = end;
    }
    let ch: u32 = match ct { 0 => 1, 2 => 3, 3 => 1, 4 => 2, 6 => 4, _ => 0 };
    if w == 0 || ch == 0 || idat.is_empty() || !matches!(depth, 1 | 2 | 4 | 8 | 16) { return Err("unsupported PNG"); }
    let Some(img) = sink.begin(w, h) else { return Err("PNG too large") };
    let bpp = ((ch * depth / 8) as usize).max(1);
    let passes = if inter != 0 { 7 } else { 1 };
    let dims = |p: usize| -> (u32, u32) {
        if inter == 0 { return (w, h); }
        if AX[p] >= w || AY[p] >= h { return (0, 0); }
        ((w - AX[p] + DX[p] - 1) / DX[p], (h - AY[p] + DY[p] - 1) / DY[p])
    };
    let mut total: u64 = 0;
    for p in 0..passes { let (pw, ph) = dims(p); if pw > 0 && ph > 0 { total += ph as u64 * (1 + (pw as u64 * ch as u64 * depth as u64 + 7) / 8); } }
    let mut raw = Vec::new();
    if raw.try_reserve_exact(total as usize + 1).is_err() { return Err("PNG too large"); }
    raw.resize(total as usize, 0);
    let got = match crate::inflate::inflate_zlib(&idat, &mut raw) { Ok(r) => r.written, Err(_) => return Err("corrupt PNG data") };
    drop(idat);
    let mut rp = 0usize;
    for p in 0..passes {
        let (pw, ph) = dims(p);
        if pw == 0 || ph == 0 { continue; }
        let stride = ((pw as u64 * ch as u64 * depth as u64 + 7) / 8) as usize;
        let mut prev = alloc::vec![0u8; stride];
        let mut cur = alloc::vec![0u8; stride];
        for y in 0..ph {
            if rp + 1 + stride > got { break; }
            let f = raw[rp];
            cur.copy_from_slice(&raw[rp + 1..rp + 1 + stride]);
            rp += 1 + stride;
            for i in 0..stride {
                let a = if i >= bpp { cur[i - bpp] as i32 } else { 0 };
                let b = prev[i] as i32;
                let c = if i >= bpp { prev[i - bpp] as i32 } else { 0 };
                let add = match f { 1 => a, 2 => b, 3 => (a + b) / 2, 4 => paeth(a, b, c), _ => 0 };
                cur[i] = cur[i].wrapping_add(add as u8);
            }
            let s16 = depth == 16;
            for x in 0..pw as usize {
                let s = |k: usize| -> u32 { if s16 { cur[(x * ch as usize + k) * 2] as u32 } else { cur[x * ch as usize + k] as u32 } };
                let v = |k: usize| -> i32 { if s16 { (cur[(x * ch as usize + k) * 2] as i32) << 8 | cur[(x * ch as usize + k) * 2 + 1] as i32 } else { cur[x * ch as usize + k] as i32 } };
                let px = if depth < 8 {
                    let bit = x * depth as usize;
                    let val = ((cur[bit / 8] >> (8 - depth as usize - bit % 8)) as u32) & ((1 << depth) - 1);
                    if ct == 3 { pal[val as usize] }
                    else { let g = val * 255 / ((1 << depth) - 1); (if trns_g == val as i32 { 0 } else { 0xFF00_0000 }) | g << 16 | g << 8 | g }
                } else if ct == 3 { pal[cur[x] as usize] }
                else if ct == 0 { let g = s(0); (if trns_g == v(0) { 0 } else { 0xFF00_0000 }) | g << 16 | g << 8 | g }
                else if ct == 4 { let g = s(0); s(1) << 24 | g << 16 | g << 8 | g }
                else if ct == 2 { (if trns_r == v(0) && trns_gg == v(1) && trns_b == v(2) { 0 } else { 0xFF00_0000 }) | s(0) << 16 | s(1) << 8 | s(2) }
                else { s(3) << 24 | s(0) << 16 | s(1) << 8 | s(2) };
                let (ix, iy) = if inter != 0 { (AX[p] + x as u32 * DX[p], AY[p] + y * DY[p]) } else { (x as u32, y) };
                img[(iy * w + ix) as usize] = px;
            }
            core::mem::swap(&mut prev, &mut cur);
        }
    }
    Ok(())
}

// ---------------------------------------------------------------- GIF

/// The first frame. A few failures give no message (""), as in C.
pub fn gif(d: &[u8], sink: &mut dyn Sink) -> R {
    let n = d.len();
    if n < 13 { return Err(""); }
    let (mut sw, mut sh, flags) = (le16(d, 6), le16(d, 8), d[10]);
    let mut gpal = [0u32; 256];
    let mut o = 13usize;
    if flags & 0x80 != 0 {
        let gn = 2usize << (flags & 7);
        let mut i = 0;
        while i < gn && o + 3 <= n { gpal[i] = rgb(d[o] as u32, d[o + 1] as u32, d[o + 2] as u32); i += 1; o += 3; }
    }
    let mut transp: i32 = -1;
    while o < n {
        let b = d[o];
        o += 1;
        if b == 0x21 {
            let label = at(d, o);
            o += 1;
            if label == 0xF9 && o + 5 < n && d[o + 1] & 1 != 0 { transp = d[o + 4] as i32; }
            while o < n && d[o] != 0 { o += d[o] as usize + 1; }
            o += 1;
        } else if b == 0x2C {
            if o + 9 > n { break; }
            let (ix, iy, iw, ih, lf) = (le16(d, o) as i64, le16(d, o + 2) as i64, le16(d, o + 4) as i64, le16(d, o + 6) as i64, d[o + 8]);
            o += 9;
            let mut pal = gpal;
            if lf & 0x80 != 0 {
                let ln = 2usize << (lf & 7);
                let mut i = 0;
                while i < ln && o + 3 <= n { pal[i] = rgb(d[o] as u32, d[o + 1] as u32, d[o + 2] as u32); i += 1; o += 3; }
            }
            if transp >= 0 { pal[transp as usize] = 0; }
            if sw == 0 || sh == 0 { sw = iw as u32; sh = ih as u32; }
            let Some(img) = sink.begin(sw, sh) else { return Err("GIF too large") };
            img.fill(0);
            let minc = at(d, o) as u32;
            o += 1;
            if !(2..=8).contains(&minc) { return Err(""); }
            let mut lzw = Vec::new();
            while o < n && d[o] != 0 {
                let bl = d[o] as usize;
                if o + 1 + bl > n { break; }
                lzw.extend_from_slice(&d[o + 1..o + 1 + bl]);
                o += bl + 1;
            }
            let mut prefix = [0u16; 4096];
            let mut suffix = [0u8; 4096];
            let mut stack = [0u8; 4097];
            let clear = 1i32 << minc;
            let eoi = clear + 1;
            let (mut avail, mut csize) = (clear + 2, minc + 1);
            let mut cmask = (1i32 << csize) - 1;
            let (mut old, mut first) = (-1i32, 0u8);
            for i in 0..clear as usize { prefix[i] = 0xFFFF; suffix[i] = i as u8; }
            let (mut bits, mut nbits) = (0u32, 0u32);
            let (mut pix, total) = (0i64, iw * ih);
            let (mut pass, mut row, mut col) = (0usize, 0i64, 0i64);
            const ISTART: [i64; 4] = [0, 4, 2, 1];
            const ISTEP: [i64; 4] = [8, 8, 4, 2];
            let mut i = 0usize;
            while i < lzw.len() && pix < total {
                while nbits < csize && i < lzw.len() { bits |= (lzw[i] as u32) << nbits; i += 1; nbits += 8; }
                if nbits < csize { break; }
                let code = (bits & cmask as u32) as i32;
                bits >>= csize; nbits -= csize;
                if code == clear { csize = minc + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
                if code == eoi { break; }
                let mut sp = 0usize;
                let mut c = code as usize;
                if old == -1 { stack[sp] = suffix[code as usize]; sp += 1; first = code as u8; old = code; }
                else {
                    if code >= avail { stack[sp] = first; sp += 1; c = old as usize; }
                    while c >= clear as usize && c < 4096 && sp < 4096 { stack[sp] = suffix[c]; sp += 1; c = prefix[c] as usize; }
                    first = suffix.get(c).copied().unwrap_or(0);
                    stack[sp] = first; sp += 1;
                    if avail < 4096 {
                        prefix[avail as usize] = old as u16; suffix[avail as usize] = first; avail += 1;
                        if avail & cmask == 0 && avail < 4096 { csize += 1; cmask = (1 << csize) - 1; }
                    }
                    old = code;
                }
                while sp > 0 && pix < total {
                    sp -= 1;
                    let v = stack[sp] as usize;
                    let px = ix + col;
                    let py = iy + if lf & 0x40 != 0 { row } else { pix / iw };
                    if px < sw as i64 && py < sh as i64 && pal[v] != 0 { img[(py * sw as i64 + px) as usize] = pal[v]; }
                    pix += 1;
                    col += 1;
                    if col >= iw {
                        col = 0;
                        if lf & 0x40 != 0 { row += ISTEP[pass]; while row >= ih && pass < 3 { pass += 1; row = ISTART[pass]; } }
                    }
                }
            }
            return Ok(());
        } else { break; }
    }
    Err("no image in GIF")
}

// ---------------------------------------------------------------- JPEG

const ZZ: [usize; 64] = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63];

#[derive(Clone)]
struct Huff { bits: [u8; 17], vals: [u8; 256], maxcode: [i32; 18], valptr: [i32; 17], mincode: [i32; 17], look: [[u8; 2]; 256] }
impl Huff {
    fn new() -> Huff { Huff { bits: [0; 17], vals: [0; 256], maxcode: [0; 18], valptr: [0; 17], mincode: [0; 17], look: [[0; 2]; 256] } }
    fn build(&mut self) {
        let (mut code, mut k) = (0i32, 0usize);
        self.look = [[0; 2]; 256];
        for l in 1..=16usize {
            self.valptr[l] = k as i32;
            self.mincode[l] = code;
            for _ in 0..self.bits[l] {
                if l <= 8 {
                    for f in 0..(1i32 << (8 - l)) {
                        let idx = (code << (8 - l)) | f;
                        if (0..256).contains(&idx) { self.look[idx as usize] = [l as u8, self.vals.get(k).copied().unwrap_or(0)]; }
                    }
                }
                code += 1; k += 1;
            }
            self.maxcode[l] = if self.bits[l] != 0 { code - 1 } else { -1 };
            code <<= 1;
        }
        self.maxcode[17] = 0x7FFF_FFFF;
    }
}
#[derive(Default, Clone)]
struct Comp { id: i32, h: usize, v: usize, tq: usize, td: usize, ta: usize, bw: usize, bh: usize, bwx: usize, bhy: usize, iw: usize, ih: usize, pred: i32, data: Vec<u8>, coef: Vec<i16> }
struct Jpeg<'a> {
    d: &'a [u8], pos: usize, bitbuf: u32, bitcnt: i32, marker: bool,
    q: [[u16; 64]; 4], hd: [Huff; 4], ha: [Huff; 4],
    w: usize, h: usize, ncomp: usize, hmax: usize, vmax: usize, mcux: usize, mcuy: usize, restart: usize,
    c: [Comp; 4], progressive: bool, sc: [usize; 4], nsc: usize, ss: usize, se: usize, ah: u32, al: u32, eobrun: i32,
    cos: [[i32; 8]; 8],
}
fn idct_table() -> [[i32; 8]; 8] {
    const C: [i32; 8] = [4096, 4017, 3784, 3406, 2896, 2276, 1567, 799];
    let mut t = [[0; 8]; 8];
    for x in 0..8 { for u in 0..8 {
        let mut k = ((2 * x + 1) * u) % 32;
        let mut s = 1;
        if k > 16 { k = 32 - k; }
        if k > 8 { k = 16 - k; s = -1; }
        let mut v = s * C[k];
        if u == 0 { v = 2896; }
        t[x][u] = v / 2;
    } }
    t
}
fn extend(v: i32, n: u32) -> i32 { if n != 0 && v < (1 << (n - 1)) { v - (1 << n) + 1 } else { v } }

impl Jpeg<'_> {
    fn fill(&mut self) {
        while self.bitcnt <= 24 {
            let mut b: u32 = 0;
            if !self.marker && self.pos < self.d.len() {
                b = self.d[self.pos] as u32;
                self.pos += 1;
                if b == 0xFF {
                    let nx = at(self.d, self.pos);
                    if nx == 0 { self.pos += 1; } else { self.marker = true; self.pos -= 1; b = 0; }
                }
            }
            self.bitbuf |= b << (24 - self.bitcnt);
            self.bitcnt += 8;
        }
    }
    fn getbits(&mut self, n: u32) -> i32 {
        if n == 0 { return 0; }
        self.fill();
        let v = (self.bitbuf >> (32 - n)) as i32;
        self.bitbuf = if n >= 32 { 0 } else { self.bitbuf << n };
        self.bitcnt -= n as i32;
        v
    }
    fn decode_huff(&mut self, ac: bool, t: usize) -> i32 {
        self.fill();
        let idx = (self.bitbuf >> 24) as usize;
        let tab = if ac { &self.ha[t] } else { &self.hd[t] };
        if tab.look[idx][0] != 0 {
            let (l, v) = (tab.look[idx][0], tab.look[idx][1]);
            self.bitbuf <<= l; self.bitcnt -= l as i32;
            return v as i32;
        }
        let mut code = 0i32;
        for l in 1..=16usize {
            code = (code << 1) | (self.bitbuf >> 31) as i32;
            self.bitbuf <<= 1; self.bitcnt -= 1;
            let tab = if ac { &self.ha[t] } else { &self.hd[t] };
            if tab.maxcode[l] >= 0 && code <= tab.maxcode[l] && code >= tab.mincode[l] {
                let i = tab.valptr[l] + code - tab.mincode[l];
                return if (0..256).contains(&i) { tab.vals[i as usize] as i32 } else { 0 };
            }
            if self.bitcnt < 8 { self.fill(); }
        }
        0
    }
    fn idct(&self, inp: &[i32; 64], out: &mut [u8], off: usize, stride: usize) {
        let mut tmp = [0i32; 64];
        for y in 0..8 { for x in 0..8 {
            let mut s: i64 = 0;
            for u in 0..8 { s += self.cos[x][u] as i64 * inp[y * 8 + u] as i64; }
            tmp[y * 8 + x] = (s >> 8) as i32;
        } }
        for x in 0..8 { for y in 0..8 {
            let mut s: i64 = 0;
            for v in 0..8 { s += self.cos[y][v] as i64 * tmp[v * 8 + x] as i64; }
            let val = (s >> 16) as i32 + 128;
            if let Some(p) = out.get_mut(off + y * stride + x) { *p = val.clamp(0, 255) as u8; }
        } }
    }
    fn decode_block(&mut self, ci: usize, blk: &mut [i32; 64]) {
        *blk = [0; 64];
        let (td, ta, tq) = (self.c[ci].td, self.c[ci].ta, self.c[ci].tq);
        let t = self.decode_huff(false, td) as u32;
        let diff = if t != 0 { let b = self.getbits(t); extend(b, t) } else { 0 };
        self.c[ci].pred = self.c[ci].pred.wrapping_add(diff);
        blk[0] = self.c[ci].pred.wrapping_mul(self.q[tq][0] as i32);
        let mut k = 1usize;
        while k < 64 {
            let rs = self.decode_huff(true, ta);
            let (r, s) = ((rs >> 4) as usize, (rs & 15) as u32);
            if s == 0 { if r != 15 { break; } k += 16; continue; }
            k += r;
            if k > 63 { return; }
            let b = self.getbits(s);
            blk[ZZ[k]] = extend(b, s).wrapping_mul(self.q[tq][k] as i32);
            k += 1;
        }
    }
    /// One progressive block of the scan being read (`b` indexes the component's coefficients).
    fn prog_block(&mut self, ci: usize, b: usize) {
        let (td, ta) = (self.c[ci].td, self.c[ci].ta);
        let al = self.al;
        let (ss, se) = (self.ss, self.se);
        if ss == 0 {
            if self.ah == 0 {
                let t = self.decode_huff(false, td) as u32;
                let diff = if t != 0 { let v = self.getbits(t); extend(v, t) } else { 0 };
                self.c[ci].pred = self.c[ci].pred.wrapping_add(diff);
                self.c[ci].coef[b] = self.c[ci].pred.wrapping_shl(al) as i16;
            } else if self.getbits(1) != 0 { self.c[ci].coef[b] |= (1i32 << al) as i16; }
            return;
        }
        let bit = (1i32 << al) as i16;
        if self.ah == 0 {
            if self.eobrun > 0 { self.eobrun -= 1; return; }
            let mut k = ss;
            while k <= se {
                let rs = self.decode_huff(true, ta);
                let (r, s) = ((rs >> 4) as usize, (rs & 15) as u32);
                if s == 0 {
                    if r < 15 { self.eobrun = (1 << r) - 1; if r != 0 { self.eobrun += self.getbits(r as u32); } break; }
                    k += 16; continue;
                }
                k += r;
                if k > se { break; }
                let v = self.getbits(s);
                self.c[ci].coef[b + ZZ[k]] = extend(v, s).wrapping_shl(al) as i16;
                k += 1;
            }
            return;
        }
        let mut k = ss;
        if self.eobrun == 0 {
            while k <= se {
                let rs = self.decode_huff(true, ta);
                let (mut r, s0) = ((rs >> 4) as i32, (rs & 15) as u32);
                let mut s: i16 = 0;
                if s0 != 0 { s = if self.getbits(1) != 0 { bit } else { bit.wrapping_neg() }; }
                else if r != 15 { self.eobrun = 1 << r; if r != 0 { self.eobrun += self.getbits(r as u32); } break; }
                while k <= se {
                    let z = b + ZZ[k];
                    if self.c[ci].coef[z] != 0 {
                        if self.getbits(1) != 0 && self.c[ci].coef[z] & bit == 0 {
                            let c = self.c[ci].coef[z];
                            self.c[ci].coef[z] = c.wrapping_add(if c > 0 { bit } else { bit.wrapping_neg() });
                        }
                    } else { r -= 1; if r < 0 { break; } }
                    k += 1;
                }
                if s != 0 && k <= se { self.c[ci].coef[b + ZZ[k]] = s; }
                k += 1;
            }
        }
        if self.eobrun > 0 {
            while k <= se {
                let z = b + ZZ[k];
                if self.c[ci].coef[z] != 0 && self.getbits(1) != 0 && self.c[ci].coef[z] & bit == 0 {
                    let c = self.c[ci].coef[z];
                    self.c[ci].coef[z] = c.wrapping_add(if c > 0 { bit } else { bit.wrapping_neg() });
                }
                k += 1;
            }
            self.eobrun -= 1;
        }
    }
    fn restart_sync(&mut self) {
        self.bitcnt = 0; self.bitbuf = 0; self.marker = false;
        while self.pos + 1 < self.d.len() && !(self.d[self.pos] == 0xFF && (0xD0..=0xD7).contains(&self.d[self.pos + 1])) { self.pos += 1; }
        self.pos += 2;
    }
    fn prog_scan(&mut self) {
        self.bitcnt = 0; self.bitbuf = 0; self.marker = false;
        self.eobrun = 0;
        for i in 0..self.nsc { let k = self.sc[i]; self.c[k].pred = 0; }
        let mut mcus = 0usize;
        if self.nsc == 1 {
            let ci = self.sc[0];
            for by in 0..self.c[ci].ih { for bx in 0..self.c[ci].iw {
                if self.restart != 0 && mcus != 0 && mcus % self.restart == 0 {
                    self.restart_sync();
                    self.eobrun = 0;
                    for i in 0..self.nsc { let k = self.sc[i]; self.c[k].pred = 0; }
                }
                let b = (by * self.c[ci].bwx + bx) * 64;
                if b + 64 <= self.c[ci].coef.len() { self.prog_block(ci, b); }
                mcus += 1;
            } }
            return;
        }
        for my in 0..self.mcuy { for mx in 0..self.mcux {
            if self.restart != 0 && mcus != 0 && mcus % self.restart == 0 {
                self.restart_sync();
                self.eobrun = 0;
                for i in 0..self.nsc { let k = self.sc[i]; self.c[k].pred = 0; }
            }
            for i in 0..self.nsc {
                let ci = self.sc[i];
                for by in 0..self.c[ci].v { for bx in 0..self.c[ci].h {
                    let b = ((my * self.c[ci].v + by) * self.c[ci].bwx + mx * self.c[ci].h + bx) * 64;
                    if b + 64 <= self.c[ci].coef.len() { self.prog_block(ci, b); }
                } }
            }
            mcus += 1;
        } }
    }
    fn output(&mut self, sink: &mut dyn Sink) -> bool {
        if self.progressive {
            for k in 0..self.ncomp {
                if self.c[k].coef.is_empty() { continue; }
                let mut blk = [0i32; 64];
                let (bw, tq) = (self.c[k].bw, self.c[k].tq);
                for by in 0..self.c[k].ih { for bx in 0..self.c[k].iw {
                    let b = (by * self.c[k].bwx + bx) * 64;
                    for z in 0..64 { blk[ZZ[z]] = self.c[k].coef[b + ZZ[z]] as i32 * self.q[tq][z] as i32; }
                    let mut data = core::mem::take(&mut self.c[k].data);
                    self.idct(&blk, &mut data, by * 8 * bw + bx * 8, bw);
                    self.c[k].data = data;
                } }
            }
        }
        let Some(img) = sink.begin(self.w as u32, self.h as u32) else { return false };
        let px = |c: &Comp, x: usize, y: usize, hmax: usize, vmax: usize| -> i32 { at(&c.data, (y * c.v / vmax) * c.bw + x * c.h / hmax) as i32 };
        for y in 0..self.h { for x in 0..self.w {
            let yy = px(&self.c[0], x, y, self.hmax, self.vmax);
            img[y * self.w + x] = if self.ncomp == 1 { rgb(yy as u32, yy as u32, yy as u32) } else {
                let cb = px(&self.c[1], x, y, self.hmax, self.vmax) - 128;
                let cr = px(&self.c[2], x, y, self.hmax, self.vmax) - 128;
                let r = yy + ((91881 * cr) >> 16);
                let g = yy - ((22554 * cb + 46802 * cr) >> 16);
                let b = yy + ((116130 * cb) >> 16);
                rgb(r.clamp(0, 255) as u32, g.clamp(0, 255) as u32, b.clamp(0, 255) as u32)
            };
        } }
        true
    }
}

pub fn jpeg(d: &[u8], sink: &mut dyn Sink) -> R {
    let mut j = alloc::boxed::Box::new(Jpeg {
        d, pos: 2, bitbuf: 0, bitcnt: 0, marker: false, q: [[0; 64]; 4],
        hd: [Huff::new(), Huff::new(), Huff::new(), Huff::new()], ha: [Huff::new(), Huff::new(), Huff::new(), Huff::new()],
        w: 0, h: 0, ncomp: 0, hmax: 0, vmax: 0, mcux: 0, mcuy: 0, restart: 0,
        c: Default::default(), progressive: false, sc: [0; 4], nsc: 0, ss: 0, se: 0, ah: 0, al: 0, eobrun: 0,
        cos: idct_table(),
    });
    let n = d.len();
    let (mut frame, mut coefs, mut done) = (false, false, false);
    while j.pos + 2 <= n {
        if d[j.pos] != 0xFF { j.pos += 1; continue; }
        let m = d[j.pos + 1];
        if m == 0xFF { j.pos += 1; continue; }
        j.pos += 2;
        if m == 0xD8 || (0xD0..=0xD7).contains(&m) { continue; }
        if m == 0xD9 { if j.progressive && coefs { done = j.output(sink); } break; }
        if j.pos + 2 > n { break; }
        let len = (d[j.pos] as usize) << 8 | d[j.pos + 1] as usize;
        if j.pos + len > n || len < 2 { break; }
        let s = &d[j.pos + 2..j.pos + len];
        match m {
            0xDB => {
                let mut o = 0;
                while o + 1 < s.len() {
                    let (pq, tq) = (s[o] >> 4, (s[o] & 3) as usize);
                    o += 1;
                    for k in 0..64 { j.q[tq][k] = if pq != 0 { (at(s, o) as u16) << 8 | at(s, o + 1) as u16 } else { at(s, o) as u16 }; o += if pq != 0 { 2 } else { 1 }; }
                }
            }
            0xC4 => {
                let mut o = 0;
                while o < s.len() {
                    let (tc, th) = (s[o] >> 4, (s[o] & 3) as usize);
                    o += 1;
                    let t = if tc != 0 { &mut j.ha[th] } else { &mut j.hd[th] };
                    let mut total = 0usize;
                    t.bits[0] = 0;
                    for k in 1..=16 { t.bits[k] = at(s, o); o += 1; total += t.bits[k] as usize; }
                    for k in 0..total.min(256) { t.vals[k] = at(s, o); o += 1; }
                    t.build();
                }
            }
            0xC0 | 0xC1 | 0xC2 => {
                j.progressive = m == 0xC2;
                j.h = (at(s, 1) as usize) << 8 | at(s, 2) as usize;
                j.w = (at(s, 3) as usize) << 8 | at(s, 4) as usize;
                j.ncomp = at(s, 5) as usize;
                if j.ncomp != 1 && j.ncomp != 3 { break; }
                j.hmax = 0; j.vmax = 0;
                for i in 0..j.ncomp {
                    let c = &mut j.c[i];
                    c.id = at(s, 6 + i * 3) as i32;
                    c.h = (at(s, 7 + i * 3) >> 4) as usize;
                    c.v = (at(s, 7 + i * 3) & 15) as usize;
                    c.tq = (at(s, 8 + i * 3) & 3) as usize;
                    j.hmax = j.hmax.max(c.h); j.vmax = j.vmax.max(c.v);
                }
                // a zero sampling factor divided by zero in C
                if (0..j.ncomp).any(|i| j.c[i].h == 0 || j.c[i].v == 0) { break; }
                frame = true;
                j.mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax);
                j.mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);
                let mut ok = true;
                for k in 0..j.ncomp {
                    let (mcux, mcuy, hmax, vmax, w, h, prog) = (j.mcux, j.mcuy, j.hmax, j.vmax, j.w, j.h, j.progressive);
                    let c = &mut j.c[k];
                    c.bw = mcux * c.h * 8; c.bh = mcuy * c.v * 8;
                    c.bwx = mcux * c.h; c.bhy = mcuy * c.v;
                    c.iw = (w * c.h + 8 * hmax - 1) / (8 * hmax);
                    c.ih = (h * c.v + 8 * vmax - 1) / (8 * vmax);
                    c.data = Vec::new();
                    if c.data.try_reserve_exact(c.bw * c.bh).is_err() { ok = false; break; }
                    c.data.resize(c.bw * c.bh, 0);
                    c.coef = Vec::new();
                    if prog {
                        if c.coef.try_reserve_exact(c.bwx * c.bhy * 64).is_err() { ok = false; break; }
                        c.coef.resize(c.bwx * c.bhy * 64, 0);
                        coefs = true;
                    }
                }
                if !ok { break; }
            }
            0xDD => j.restart = (at(s, 0) as usize) << 8 | at(s, 1) as usize,
            0xDA => {
                if !frame { break; }
                j.nsc = at(s, 0) as usize;
                if j.nsc < 1 || j.nsc > j.ncomp { break; }
                for i in 0..j.nsc {
                    j.sc[i] = 0;
                    for k in 0..j.ncomp {
                        if j.c[k].id == at(s, 1 + i * 2) as i32 {
                            j.sc[i] = k;
                            j.c[k].td = (at(s, 2 + i * 2) >> 4) as usize & 3;
                            j.c[k].ta = (at(s, 2 + i * 2) & 15) as usize & 3;
                        }
                    }
                }
                j.ss = at(s, 1 + j.nsc * 2) as usize;
                j.se = (at(s, 2 + j.nsc * 2) as usize).min(63);
                j.ah = (at(s, 3 + j.nsc * 2) >> 4) as u32;
                j.al = (at(s, 3 + j.nsc * 2) & 15) as u32;
                j.pos += len;
                if j.progressive {
                    if !coefs { break; }
                    j.prog_scan();
                    while j.pos + 1 < n {
                        if d[j.pos] != 0xFF { j.pos += 1; continue; }
                        let nx = d[j.pos + 1];
                        if nx == 0xFF { j.pos += 1; continue; }
                        if nx == 0 || (0xD0..=0xD7).contains(&nx) { j.pos += 2; continue; }
                        break;
                    }
                    continue;
                }
                let mut blk = [0i32; 64];
                let mut mcus = 0usize;
                for my in 0..j.mcuy { for mx in 0..j.mcux {
                    if j.restart != 0 && mcus != 0 && mcus % j.restart == 0 {
                        j.restart_sync();
                        for k in 0..j.ncomp { j.c[k].pred = 0; }
                    }
                    for k in 0..j.ncomp {
                        let (cv, chh, bw) = (j.c[k].v, j.c[k].h, j.c[k].bw);
                        for by in 0..cv { for bx in 0..chh {
                            j.decode_block(k, &mut blk);
                            let mut data = core::mem::take(&mut j.c[k].data);
                            j.idct(&blk, &mut data, ((my * cv + by) * 8) * bw + (mx * chh + bx) * 8, bw);
                            j.c[k].data = data;
                        } }
                    }
                    mcus += 1;
                } }
                done = j.output(sink);
                break;
            }
            _ => {}
        }
        j.pos += len;
    }
    if done { Ok(()) } else if j.progressive { Err("incomplete progressive JPEG") } else { Err("unsupported JPEG") }
}

// ---------------------------------------------------------------- BMP

/// 24- and 32-bit uncompressed (or bit-field) BMP up to 4096x4096; Err with no message
/// otherwise (callers word it).
pub fn bmp(d: &[u8], sink: &mut dyn Sink) -> R {
    let n = d.len();
    if n < 54 || d[0] != b'B' || d[1] != b'M' { return Err(""); }
    let off = u32::from_le_bytes([d[10], d[11], d[12], d[13]]) as u64;
    let w = i32::from_le_bytes([d[18], d[19], d[20], d[21]]);
    let mut h = i32::from_le_bytes([d[22], d[23], d[24], d[25]]);
    let bpp = u16::from_le_bytes([d[28], d[29]]) as u32;
    let comp = u32::from_le_bytes([d[30], d[31], d[32], d[33]]);
    if (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || w > 4096 || h == 0 || h.unsigned_abs() > 4096 { return Err(""); }
    let top = h < 0;
    if top { h = -h; }
    let (w, h) = (w as u64, h as u64);
    let stride = ((w * bpp as u64 / 8) + 3) & !3;
    if off + stride * h > n as u64 { return Err(""); }
    let Some(img) = sink.begin(w as u32, h as u32) else { return Err("") };
    let bp = (bpp / 8) as usize;
    for y in 0..h {
        let row = (off + stride * if top { y } else { h - 1 - y }) as usize;
        for x in 0..w as usize {
            let p = row + x * bp;
            img[y as usize * w as usize + x] = rgb(d[p + 2] as u32, d[p + 1] as u32, d[p] as u32);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    pub struct VecSink { pub w: u32, pub h: u32, pub px: Vec<u32> }
    impl Sink for VecSink {
        fn begin(&mut self, w: u32, h: u32) -> Option<&mut [u32]> {
            if w == 0 || h == 0 || w > 16384 || h > 16384 || w as u64 * h as u64 > 16 << 20 { return None; }
            self.w = w; self.h = h; self.px = alloc::vec![0xFFFF_FFFF; (w * h) as usize];
            Some(&mut self.px)
        }
    }
    fn sink() -> VecSink { VecSink { w: 0, h: 0, px: Vec::new() } }

    #[test]
    fn bmp_and_damage() {
        // 2x2 24-bit bottom-up: rows are padded to 8 bytes
        let mut b = alloc::vec![0u8; 54];
        b[0] = b'B'; b[1] = b'M'; b[10] = 54; b[18] = 2; b[22] = 2; b[28] = 24;
        b.extend_from_slice(&[0, 0, 255, 0, 255, 0, 0, 0]);    // bottom row: red, green
        b.extend_from_slice(&[255, 0, 0, 255, 255, 255, 0, 0]); // top row: blue, white
        let mut s = sink();
        bmp(&b, &mut s).unwrap();
        assert_eq!(s.px, [0xFF0000FF, 0xFFFFFFFF, 0xFFFF0000, 0xFF00FF00]);
        let mut bad = b.clone(); bad[10..14].copy_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        assert!(bmp(&bad, &mut sink()).is_err());
        // no decoder may panic on any truncation or corruption of small files
        for data in [&b[..]] {
            for n in 0..data.len() { let _ = bmp(&data[..n], &mut sink()); let _ = png(&data[..n], &mut sink()); let _ = gif(&data[..n], &mut sink()); let _ = jpeg(&data[..n], &mut sink()); }
        }
    }

    #[test]
    fn junk_does_not_panic() {
        let mut x: u32 = 1;
        for len in [13usize, 64, 300, 2000] {
            for _ in 0..200 {
                let mut v: Vec<u8> = (0..len).map(|_| { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x as u8 }).collect();
                v[..8].copy_from_slice(b"\x89PNG\r\n\x1a\n"); let _ = png(&v, &mut sink());
                v[..6].copy_from_slice(b"GIF89a"); let _ = gif(&v, &mut sink());
                v[..3].copy_from_slice(&[0xFF, 0xD8, 0xFF]); let _ = jpeg(&v, &mut sink());
                v[..2].copy_from_slice(b"BM"); let _ = bmp(&v, &mut sink());
            }
        }
    }
}
