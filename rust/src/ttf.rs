//! A small TrueType engine (was `src/gui/ttf.c`): the sfnt table directory, Unicode cmaps
//! (formats 4 and 12), horizontal metrics and `kern` pairs, OpenType shaping with the font's
//! GSUB and GPOS tables (ligatures, contextual and chaining substitutions, Arabic joining
//! forms, pair kerning and mark attachment), and `glyf` outlines with composites,
//! rasterised with 8 sub-scanlines per pixel and exact span coverage.
//!
//! Every read goes through `rd16`/`rd32`, which return 0 past the end of the file, and
//! every structure is also checked against the file before it is used, so a damaged font
//! loses a glyph or a substitution, never the kernel. Integer arithmetic wraps where the C
//! code's did (offsets are 32-bit and wrap), so a font gives the same pixels as before.
//!
//! The C interface at the end (`ttf.h`) needs nothing but the caller's allocator: the
//! kernel (src/gui/gfx.c) and host tools link the same code. Coverage bitmaps and shaping
//! results are allocated with that allocator and belong to the caller.

use alloc::vec::Vec;
use core::ffi::{c_int, c_void};

const fn tag(s: &[u8; 4]) -> u32 { (s[0] as u32) << 24 | (s[1] as u32) << 16 | (s[2] as u32) << 8 | s[3] as u32 }
const F2DOT14_ONE: i32 = 16384;
const MAX_POINTS: usize = 1 << 16;
const MAX_EDGES: usize = 20000;
const SUBSAMPLES: i32 = 8;
const MAX_PX: i32 = 512;
const MAX_SHAPE_LOOKUPS: usize = 128;
const LF_IGNORE_BASE: u16 = 0x0002;
const LF_IGNORE_LIG: u16 = 0x0004;
const LF_IGNORE_MARK: u16 = 0x0008;
const LF_MARK_SET: u16 = 0x0010;

/// One glyph after shaping, in font units (`ttf_shaped_t`).
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Shaped { pub gid: i32, pub advance: i32, pub x_offset: i32, pub y_offset: i32, pub cluster: i32 }

/// A rendered glyph: a coverage bitmap (0 blank .. 255 covered) and where it goes.
#[derive(Default, Debug, PartialEq, Eq)]
pub struct Glyph { pub width: i32, pub height: i32, pub xoff: i32, pub yoff: i32, pub advance: i32, pub coverage: Vec<u8> }

#[allow(dead_code)]
pub struct Face<'a> {
    d: &'a [u8],
    size: u32,
    head: u32, head_len: u32,
    maxp: u32, maxp_len: u32,
    hhea: u32, hhea_len: u32,
    hmtx: u32, hmtx_len: u32,
    cmap: u32, cmap_len: u32,
    loca: u32, loca_len: u32,
    glyf: u32, glyf_len: u32,
    kern: u32, kern_len: u32,
    gsub: u32, gsub_len: u32,
    gpos: u32, gpos_len: u32,
    gdef: u32, gdef_len: u32,
    num_glyphs: u32,
    num_hmetrics: u32,
    pub units_per_em: i32,
    pub ascender: i32, pub descender: i32, pub line_gap: i32,
    loca_long: bool,
    cmap_off: u32,
    cmap_format: i32,
    kern_off: u32, kern_pairs: u32,
}

// ---------------------------------------------------------------- reading

fn b(d: &[u8], o: usize) -> u32 { d.get(o).copied().unwrap_or(0) as u32 }
#[inline]
fn rd16_at(d: &[u8], o: u32) -> u16 {
    let o = o as usize;
    match d.get(o..o.wrapping_add(2)) { Some(s) if o < usize::MAX - 2 => u16::from_be_bytes([s[0], s[1]]), _ => (b(d, o) << 8 | b(d, o.wrapping_add(1))) as u16 }
}
#[inline]
fn rd32_at(d: &[u8], o: u32) -> u32 {
    let o = o as usize;
    match d.get(o..o.wrapping_add(4)) { Some(s) if o < usize::MAX - 4 => u32::from_be_bytes([s[0], s[1], s[2], s[3]]), _ => b(d, o) << 24 | b(d, o.wrapping_add(1)) << 16 | b(d, o.wrapping_add(2)) << 8 | b(d, o.wrapping_add(3)) }
}

/// A bounds-checked reader over one glyph.
struct Cur<'a> { g: &'a [u8], p: usize, end: usize, ok: bool }
impl Cur<'_> {
    fn u8(&mut self) -> u8 { if !self.ok || self.end < self.p + 1 { self.ok = false; return 0; } self.p += 1; self.g[self.p - 1] }
    fn u16(&mut self) -> u16 { if !self.ok || self.end < self.p + 2 { self.ok = false; return 0; } self.p += 2; (self.g[self.p - 2] as u16) << 8 | self.g[self.p - 1] as u16 }
    fn s16(&mut self) -> i16 { self.u16() as i16 }
}

/// The shaping buffer: the glyphs and their Arabic forms side by side, with a fixed capacity.
struct GBuf { g: Vec<Shaped>, form: Vec<u8>, cap: usize, forms: bool }
impl GBuf {
    fn n(&self) -> i32 { self.g.len() as i32 }
    fn insert(&mut self, at: usize, count: i32) -> bool {
        if count <= 0 || self.g.len() + count as usize > self.cap { return false; }
        self.g.splice(at..at, core::iter::repeat(Shaped::default()).take(count as usize));
        if self.forms { self.form.splice(at..at, core::iter::repeat(0).take(count as usize)); }
        true
    }
    fn remove(&mut self, at: i32, count: i32) {
        if count <= 0 || at < 0 || at + count > self.n() { return; }
        self.g.drain(at as usize..(at + count) as usize);
        if self.forms { self.form.drain(at as usize..(at + count) as usize); }
    }
}

#[derive(Clone, Copy, Default)]
struct Pt { x: i32, y: i32, on: u8 }
#[derive(Clone, Copy)]
struct Xform { a: i32, b: i32, c: i32, d: i32, e: i32, f: i32 }
struct Outline { pts: Vec<Pt>, ends: Vec<u16> }
#[derive(Clone, Copy)]
struct Edge { x0: i32, y0: i32, x1: i32, y1: i32 }
#[derive(Default)]
struct BBox { minx: i32, miny: i32, maxx: i32, maxy: i32, any: bool }

fn mul14(a: i32, x: i32, c: i32, y: i32) -> i32 { ((a as i64).wrapping_mul(x as i64).wrapping_add((c as i64).wrapping_mul(y as i64)) >> 14) as i32 }
fn xform_mul(p: &Xform, c: &Xform) -> Xform {
    Xform {
        a: mul14(p.a, c.a, p.c, c.b), b: mul14(p.b, c.a, p.d, c.b),
        c: mul14(p.a, c.c, p.c, c.d), d: mul14(p.b, c.c, p.d, c.d),
        e: mul14(p.a, c.e, p.c, c.f).wrapping_add(p.e), f: mul14(p.b, c.e, p.d, c.f).wrapping_add(p.f),
    }
}

impl<'a> Face<'a> {
    fn rd16(&self, o: u32) -> u16 { rd16_at(self.d, o) }
    fn rs16(&self, o: u32) -> i16 { rd16_at(self.d, o) as i16 }
    fn rd32(&self, o: u32) -> u32 { rd32_at(self.d, o) }
    fn in_file(&self, off: u32, n: u32) -> bool { off <= self.size && n <= self.size - off }

    // ------------------------------------------------------------ opening

    /// Parses an sfnt (or the first face of a collection); None for a file this engine
    /// cannot read (CFF 'OTTO' outlines are not supported).
    pub fn open(d: &'a [u8]) -> Option<Face<'a>> {
        let size = d.len();
        if size < 12 { return None; }
        let mut base = 0u32;
        let mut version = rd32_at(d, 0);
        if version == tag(b"ttcf") {
            if size < 16 { return None; }
            let count = rd32_at(d, 8);
            base = rd32_at(d, 12);
            if count == 0 || base as usize > size || size - (base as usize) < 12 { return None; }
            version = rd32_at(d, base);
        }
        if version != 0x0001_0000 && version != tag(b"true") && version != tag(b"typ1") { return None; }
        let num_tables = rd16_at(d, base.wrapping_add(4));
        let mut f = Face {
            d, size: size.min(0xFFFF_FFFF) as u32,
            head: 0, head_len: 0, maxp: 0, maxp_len: 0, hhea: 0, hhea_len: 0, hmtx: 0, hmtx_len: 0, cmap: 0, cmap_len: 0,
            loca: 0, loca_len: 0, glyf: 0, glyf_len: 0, kern: 0, kern_len: 0, gsub: 0, gsub_len: 0, gpos: 0, gpos_len: 0, gdef: 0, gdef_len: 0,
            num_glyphs: 0, num_hmetrics: 0, units_per_em: 0, ascender: 0, descender: 0, line_gap: 0, loca_long: false,
            cmap_off: 0, cmap_format: 0, kern_off: 0, kern_pairs: 0,
        };
        for i in 0..num_tables as u32 {
            let rec = base.wrapping_add(12).wrapping_add(i.wrapping_mul(16));
            if !f.in_file(rec, 16) { break; }
            let t = f.rd32(rec);
            let off = f.rd32(rec.wrapping_add(8));
            let len = f.rd32(rec.wrapping_add(12));
            if !f.in_file(off, len) { continue; }
            match t {
                x if x == tag(b"head") => { f.head = off; f.head_len = len; }
                x if x == tag(b"maxp") => { f.maxp = off; f.maxp_len = len; }
                x if x == tag(b"hhea") => { f.hhea = off; f.hhea_len = len; }
                x if x == tag(b"hmtx") => { f.hmtx = off; f.hmtx_len = len; }
                x if x == tag(b"cmap") => { f.cmap = off; f.cmap_len = len; }
                x if x == tag(b"loca") => { f.loca = off; f.loca_len = len; }
                x if x == tag(b"glyf") => { f.glyf = off; f.glyf_len = len; }
                x if x == tag(b"kern") => { f.kern = off; f.kern_len = len; }
                x if x == tag(b"GSUB") => { f.gsub = off; f.gsub_len = len; }
                x if x == tag(b"GPOS") => { f.gpos = off; f.gpos_len = len; }
                x if x == tag(b"GDEF") => { f.gdef = off; f.gdef_len = len; }
                _ => {}
            }
        }
        if f.head == 0 || f.head_len < 54 || f.maxp == 0 || f.maxp_len < 6 || f.hhea == 0 || f.hhea_len < 36 || f.cmap == 0 || f.hmtx == 0
            || f.loca == 0 || f.glyf == 0 || f.loca_len == 0 || f.glyf_len == 0 { return None; }
        f.units_per_em = f.rd16(f.head + 18) as i32;
        f.loca_long = f.rs16(f.head + 50) != 0;
        if f.units_per_em < 16 || f.units_per_em > 16384 { return None; }
        f.num_glyphs = f.rd16(f.maxp + 4) as u32;
        f.num_hmetrics = f.rd16(f.hhea + 34) as u32;
        if f.num_glyphs == 0 || f.num_hmetrics == 0 { return None; }
        f.ascender = f.rs16(f.hhea + 4) as i32;
        f.descender = f.rs16(f.hhea + 6) as i32;
        f.line_gap = f.rs16(f.hhea + 8) as i32;

        // the best Unicode cmap: full repertoire first, then the BMP
        if f.cmap_len >= 4 {
            let n = f.rd16(f.cmap + 2) as u32;
            let mut best = 0;
            for i in 0..n {
                let rec = f.cmap.wrapping_add(4).wrapping_add(i.wrapping_mul(8));
                if !f.in_file(rec, 8) { break; }
                let (plat, enc) = (f.rd16(rec), f.rd16(rec + 2));
                let sub = f.cmap.wrapping_add(f.rd32(rec + 4));
                if !f.in_file(sub, 4) { continue; }
                let fmt = f.rd16(sub);
                let score = if fmt == 12 { if plat == 3 && enc == 10 { 5 } else if plat == 0 { 4 } else { 0 } }
                    else if fmt == 4 { if plat == 3 && enc == 1 { 3 } else if plat == 0 { 2 } else if plat == 3 && enc == 0 { 1 } else { 0 } }
                    else { 0 };
                if score > best { best = score; f.cmap_off = sub; f.cmap_format = fmt as i32; }
            }
        }
        // kerning pairs, format 0, horizontal only
        if f.kern != 0 && f.kern_len >= 4 && f.rd16(f.kern) == 0 {
            let nt = f.rd16(f.kern + 2);
            let mut p = f.kern.wrapping_add(4);
            for _ in 0..nt {
                if !f.in_file(p, 6) { break; }
                let (len, cov) = (f.rd16(p.wrapping_add(2)), f.rd16(p.wrapping_add(4)));
                if len < 14 || !f.in_file(p, len as u32) { break; }
                if cov >> 8 == 0 && cov & 1 != 0 && cov & 0x0E == 0 {
                    let np = f.rd16(p.wrapping_add(6)) as u32;
                    if f.in_file(p.wrapping_add(14), np.wrapping_mul(6)) { f.kern_off = p.wrapping_add(14); f.kern_pairs = np; }
                    break;
                }
                p = p.wrapping_add(len as u32);
            }
        }
        Some(f)
    }
    pub fn num_glyphs(&self) -> i32 { self.num_glyphs as i32 }

    // ------------------------------------------------------------ cmap and metrics

    fn cmap12(&self, c: u32) -> i32 {
        let p = self.cmap_off;
        if !self.in_file(p, 16) { return 0; }
        let n = self.rd32(p + 12);
        let (mut lo, mut hi) = (0u32, n);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let g = p.wrapping_add(16).wrapping_add(mid.wrapping_mul(12));
            if !self.in_file(g, 12) { return 0; }
            let (first, last) = (self.rd32(g), self.rd32(g + 4));
            if c < first { hi = mid; }
            else if c > last { lo = mid + 1; }
            else { let gid = self.rd32(g + 8).wrapping_add(c - first); return if gid < self.num_glyphs { gid as i32 } else { 0 }; }
        }
        0
    }
    fn cmap4(&self, c: u32) -> i32 {
        let p = self.cmap_off;
        if c > 0xFFFF || !self.in_file(p, 14) { return 0; }
        let seg2 = self.rd16(p + 6) as u32;
        let seg = seg2 / 2;
        let endp = p + 14;
        let startp = endp + seg2 + 2;
        let deltap = startp + seg2;
        let rangep = deltap + seg2;
        if seg == 0 || !self.in_file(endp, seg2 + 2 + seg2 * 3) { return 0; }
        let (mut lo, mut hi) = (0u32, seg);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let e = self.rd16(endp + mid * 2) as u32;
            if c <= e { hi = mid; } else { lo = mid + 1; }
        }
        if lo >= seg { return 0; }
        let start = self.rd16(startp + lo * 2) as u32;
        if c < start { return 0; }
        let delta = self.rd16(deltap + lo * 2);
        let ro = self.rd16(rangep + lo * 2) as u32;
        if ro == 0 { let g = (c as u16).wrapping_add(delta) as u32; return if g < self.num_glyphs { g as i32 } else { 0 }; }
        let g = rangep + lo * 2 + ro + (c - start) * 2;
        if !self.in_file(g, 2) { return 0; }
        let mut gid = self.rd16(g);
        if gid == 0 { return 0; }
        gid = gid.wrapping_add(delta);
        if (gid as u32) < self.num_glyphs { gid as i32 } else { 0 }
    }
    /// The glyph for a Unicode code point, 0 (".notdef") when the font has none.
    pub fn glyph_index(&self, c: u32) -> i32 {
        match self.cmap_format { 12 => self.cmap12(c), 4 => self.cmap4(c), _ => 0 }
    }
    /// The advance width in font units.
    pub fn advance(&self, gid: i32) -> i32 {
        if gid < 0 || gid as u32 >= self.num_glyphs { return 0; }
        let idx = if (gid as u32) < self.num_hmetrics { gid as u32 } else { self.num_hmetrics - 1 };
        let off = self.hmtx.wrapping_add(idx.wrapping_mul(4));
        if !self.in_file(off, 2) { return 0; }
        self.rd16(off) as i32
    }
    /// The `kern` table's adjustment for a pair, in font units.
    pub fn kerning(&self, left: i32, right: i32) -> i32 {
        if self.kern_pairs == 0 || left < 0 || right < 0 { return 0; }
        let key = (left as u32) << 16 | right as u32;
        let (mut lo, mut hi) = (0u32, self.kern_pairs);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let p = self.kern_off.wrapping_add(mid.wrapping_mul(6));
            if !self.in_file(p, 6) { return 0; }
            let k = (self.rd16(p) as u32) << 16 | self.rd16(p + 2) as u32;
            if key < k { hi = mid; } else if key > k { lo = mid + 1; } else { return self.rs16(p + 4) as i32; }
        }
        0
    }

    // ------------------------------------------------------------ OpenType layout

    /// The bytes [off, off + n) once in_file has vouched for them.
    fn tbl(&self, off: u32, n: u32) -> &'a [u8] { &self.d[off as usize..(off + n) as usize] }
    fn coverage_index(&self, off: u32, gid: i32) -> i32 {
        if off == 0 || gid < 0 || !self.in_file(off, 4) { return -1; }
        let fmt = self.rd16(off);
        let n = self.rd16(off + 2) as usize;
        let at = |s: &[u8], i: usize| ((s[i] as i32) << 8) | s[i + 1] as i32;
        if fmt == 1 {
            if !self.in_file(off + 4, n as u32 * 2) { return -1; }
            let s = self.tbl(off + 4, n as u32 * 2);
            let (mut lo, mut hi) = (0usize, n);
            while lo < hi {
                let mid = lo + (hi - lo) / 2;
                if at(s, mid * 2) < gid { lo = mid + 1; } else { hi = mid; }
            }
            if lo < n && at(s, lo * 2) == gid { return lo as i32; }
            return -1;
        }
        if fmt == 2 {
            if !self.in_file(off + 4, n as u32 * 6) { return -1; }
            let s = self.tbl(off + 4, n as u32 * 6);
            let (mut lo, mut hi) = (0usize, n);
            while lo < hi {
                let mid = lo + (hi - lo) / 2;
                if at(s, mid * 6 + 2) < gid { lo = mid + 1; } else { hi = mid; }
            }
            if lo < n {
                let (st, e) = (at(s, lo * 6), at(s, lo * 6 + 2));
                if gid >= st && gid <= e { return at(s, lo * 6 + 4) + gid - st; }
            }
        }
        -1
    }
    fn classdef_get(&self, off: u32, gid: i32) -> i32 {
        if off == 0 || gid < 0 || !self.in_file(off, 4) { return 0; }
        let fmt = self.rd16(off);
        if fmt == 1 {
            let (start, n) = (self.rd16(off + 2) as i32, self.rd16(off + 4) as u32);
            if gid < start || (gid - start) as u32 >= n { return 0; }
            let p = off.wrapping_add(6).wrapping_add((gid - start) as u32 * 2);
            return if self.in_file(p, 2) { self.rd16(p) as i32 } else { 0 };
        }
        if fmt == 2 {
            let n = self.rd16(off + 2) as u32;
            if !self.in_file(off + 4, n * 6) { return 0; }
            let at = |s: &[u8], i: usize| ((s[i] as i32) << 8) | s[i + 1] as i32;
            for r in self.tbl(off + 4, n * 6).chunks_exact(6) {
                let (s, e) = (at(r, 0), at(r, 2));
                if gid >= s && gid <= e { return at(r, 4); }
                if gid < s { break; }
            }
        }
        0
    }
    /// GDEF: base 1, ligature 2, mark 3 (0 unknown).
    fn glyph_class(&self, gid: i32) -> i32 {
        if self.gdef == 0 || self.gdef_len < 12 { return 0; }
        self.classdef_get(self.gdef.wrapping_add(self.rd16(self.gdef + 4) as u32), gid)
    }
    fn mark_set_has(&self, set: i32, gid: i32) -> bool {
        if self.gdef == 0 || self.gdef_len < 18 || set < 0 { return true; }
        let ms = self.gdef.wrapping_add(self.rd16(self.gdef + 12) as u32);
        if ms == 0 || !self.in_file(ms, 4) { return true; }
        let n = self.rd16(ms + 2);
        if set as u32 >= n as u32 { return true; }
        let cov = self.rd32(ms.wrapping_add(4).wrapping_add(set as u32 * 4));
        self.coverage_index(ms.wrapping_add(cov), gid) >= 0
    }
    fn ignore_glyph(&self, flag: u16, set: i32, gid: i32) -> bool {
        let cls = self.glyph_class(gid);
        if flag & LF_IGNORE_BASE != 0 && cls == 1 { return true; }
        if flag & LF_IGNORE_LIG != 0 && cls == 2 { return true; }
        if flag & LF_IGNORE_MARK != 0 && cls == 3 { return true; }
        if flag & LF_MARK_SET != 0 && cls == 3 && !self.mark_set_has(set, gid) { return true; }
        if flag & LF_MARK_SET == 0 {
            let attach = ((flag >> 8) & 0xFF) as i32;
            if cls == 3 && attach != 0 {
                let mc = self.gdef.wrapping_add(self.rd16(self.gdef + 10) as u32);
                if self.classdef_get(mc, gid) != attach { return true; }
            }
        }
        false
    }
    fn next_glyph(&self, b: &GBuf, flag: u16, set: i32, from: i32) -> i32 {
        ((from + 1)..b.n()).find(|&i| !self.ignore_glyph(flag, set, b.g[i as usize].gid)).unwrap_or(-1)
    }
    fn prev_glyph(&self, b: &GBuf, flag: u16, set: i32, from: i32) -> i32 {
        (0..from).rev().find(|&i| !self.ignore_glyph(flag, set, b.g[i as usize].gid)).unwrap_or(-1)
    }
    /// The script table for `tag`, or the default one, or 0.
    fn find_script(&self, table: u32, t: u32) -> u32 {
        if table == 0 || !self.in_file(table, 10) { return 0; }
        let sl = table.wrapping_add(self.rd16(table + 4) as u32);
        if !self.in_file(sl, 2) { return 0; }
        let n = self.rd16(sl);
        let mut dflt = 0;
        for i in 0..n as u32 {
            let r = sl.wrapping_add(2).wrapping_add(i * 6);
            if !self.in_file(r, 6) { break; }
            let tt = self.rd32(r);
            let s = sl.wrapping_add(self.rd16(r.wrapping_add(4)) as u32);
            if tt == t { return s; }
            if tt == tag(b"DFLT") { dflt = s; }
        }
        dflt
    }
    /// The lookup indices of `feat` for `script` (at most 128).
    fn feature_lookups(&self, table: u32, script: u32, feat: u32) -> Vec<u16> {
        let mut out = Vec::new();
        let s = self.find_script(table, script);
        if s == 0 || !self.in_file(s, 6) { return out; }
        let ls = self.rd16(s) as u32;
        if ls == 0 { return out; }                      // no default language
        let ls = ls.wrapping_add(s);
        if !self.in_file(ls, 6) { return out; }
        let nf = self.rd16(ls.wrapping_add(4));
        let fl = table.wrapping_add(self.rd16(table + 6) as u32);
        if !self.in_file(fl, 2) { return out; }
        let nfeature = self.rd16(fl);
        for i in 0..nf as u32 {
            let fr = ls.wrapping_add(6).wrapping_add(i * 2);
            if !self.in_file(fr, 2) { break; }
            let fi = self.rd16(fr);
            if fi >= nfeature { continue; }
            let rec = fl.wrapping_add(2).wrapping_add(fi as u32 * 6);
            if !self.in_file(rec, 6) || self.rd32(rec) != feat { continue; }
            let ft = fl.wrapping_add(self.rd16(rec.wrapping_add(4)) as u32);
            if !self.in_file(ft, 4) { continue; }
            let nl = self.rd16(ft.wrapping_add(2));
            for k in 0..nl as u32 {
                if out.len() >= MAX_SHAPE_LOOKUPS { break; }
                let lr = ft.wrapping_add(4).wrapping_add(k * 2);
                if !self.in_file(lr, 2) { break; }
                out.push(self.rd16(lr));
            }
        }
        out
    }
    /// The offset of lookup `i` of a GSUB or GPOS table.
    fn lookup_at(&self, table: u32, i: u16) -> u32 {
        let ll = table.wrapping_add(self.rd16(table.wrapping_add(8)) as u32);
        ll.wrapping_add(self.rd16(ll.wrapping_add(2).wrapping_add(i as u32 * 2)) as u32)
    }

    // ------------------------------------------------------------ GSUB

    fn set_glyph(&self, b: &mut GBuf, pos: usize, gid: i32) { b.g[pos].gid = gid; b.g[pos].advance = self.advance(gid); }
    fn gsub_single(&self, sub: u32, b: &mut GBuf, pos: i32, delta: &mut i32) -> bool {
        if !self.in_file(sub, 6) { return false; }
        let fmt = self.rd16(sub);
        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub + 2) as u32), b.g[pos as usize].gid);
        if idx < 0 { return false; }
        let g = if fmt == 1 { (b.g[pos as usize].gid as u16).wrapping_add(self.rd16(sub + 4)) as i32 }
            else if fmt == 2 {
                let n = self.rd16(sub + 4) as u32;
                if idx as u32 >= n || !self.in_file(sub + 6, n * 2) { return false; }
                self.rd16(sub + 6 + idx as u32 * 2) as i32
            } else { return false };
        self.set_glyph(b, pos as usize, g);
        *delta = 0;
        true
    }
    fn gsub_multiple(&self, sub: u32, b: &mut GBuf, pos: i32, delta: &mut i32) -> bool {
        if !self.in_file(sub, 6) || self.rd16(sub) != 1 { return false; }
        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub + 2) as u32), b.g[pos as usize].gid);
        if idx < 0 { return false; }
        let nseq = self.rd16(sub + 4) as u32;
        if idx as u32 >= nseq || !self.in_file(sub + 6, nseq * 2) { return false; }
        let sp = sub.wrapping_add(self.rd16(sub + 6 + idx as u32 * 2) as u32);
        if !self.in_file(sp, 2) { return false; }
        let count = self.rd16(sp) as i32;
        if count == 0 { b.remove(pos, 1); *delta = -1; return true; }
        if count == 1 { let g = self.rd16(sp.wrapping_add(2)) as i32; self.set_glyph(b, pos as usize, g); *delta = 0; return true; }
        if !b.insert(pos as usize + 1, count - 1) { return false; }
        let first = b.g[pos as usize];
        for i in 0..count {
            let mut g = first;
            g.gid = self.rd16(sp.wrapping_add(2).wrapping_add(i as u32 * 2)) as i32;
            g.advance = self.advance(g.gid);
            g.x_offset = 0; g.y_offset = 0;
            b.g[(pos + i) as usize] = g;
        }
        *delta = count - 1;
        true
    }
    fn gsub_alternate(&self, sub: u32, b: &mut GBuf, pos: i32, delta: &mut i32) -> bool {
        if !self.in_file(sub, 6) || self.rd16(sub) != 1 { return false; }
        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub + 2) as u32), b.g[pos as usize].gid);
        if idx < 0 { return false; }
        let nseq = self.rd16(sub + 4) as u32;
        if idx as u32 >= nseq || !self.in_file(sub + 6, nseq * 2) { return false; }
        let sp = sub.wrapping_add(self.rd16(sub + 6 + idx as u32 * 2) as u32);
        if !self.in_file(sp, 4) || self.rd16(sp) == 0 { return false; }
        let g = self.rd16(sp + 2) as i32;
        self.set_glyph(b, pos as usize, g);
        *delta = 0;
        true
    }
    fn gsub_ligature(&self, sub: u32, b: &mut GBuf, pos: i32, flag: u16, set: i32, delta: &mut i32) -> bool {
        if !self.in_file(sub, 6) || self.rd16(sub) != 1 { return false; }
        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub + 2) as u32), b.g[pos as usize].gid);
        if idx < 0 { return false; }
        let nsets = self.rd16(sub + 4) as u32;
        if idx as u32 >= nsets || !self.in_file(sub + 6, nsets * 2) { return false; }
        let lp = sub.wrapping_add(self.rd16(sub + 6 + idx as u32 * 2) as u32);
        if !self.in_file(lp, 2) { return false; }
        let nlig = self.rd16(lp);
        for i in 0..nlig as u32 {
            let lr = lp.wrapping_add(self.rd16(lp.wrapping_add(2).wrapping_add(i * 2)) as u32);
            if !self.in_file(lr, 4) { break; }
            let lig = self.rd16(lr) as i32;
            let comps = self.rd16(lr + 2) as i32;
            if !(2..=16).contains(&comps) { continue; }
            let mut p = [0i32; 17];
            p[0] = pos;
            let mut ok = true;
            for k in 1..comps as usize {
                if !ok { break; }
                p[k] = self.next_glyph(b, flag, set, p[k - 1]);
                if p[k] < 0 || self.rd16(lr.wrapping_add(4).wrapping_add((k as u32 - 1) * 2)) as i32 != b.g[p[k] as usize].gid { ok = false; }
            }
            if !ok { continue; }
            self.set_glyph(b, pos as usize, lig);
            for k in (1..comps as usize).rev() { b.remove(p[k], 1); }
            *delta = -(comps - 1);
            return true;
        }
        false
    }
    /// Format 3 contextual / chaining: coverage tables for the input, and records that run
    /// other lookups at a chosen position.
    fn gsub_context3(&self, sub: u32, b: &mut GBuf, pos: i32, flag: u16, set: i32, chained: bool, depth: i32, delta: &mut i32) -> bool {
        let mut input = [0i32; 33];
        let ni: i32;
        let mut p: u32;
        let records: u16;
        let cov = |off: u32| sub.wrapping_add(self.rd16(off) as u32);
        if chained {
            p = sub.wrapping_add(2);
            let nb = self.rd16(p) as i32; p = p.wrapping_add(2);
            if nb > 32 || (nb != 0 && !self.in_file(p, nb as u32 * 2)) { return false; }
            let mut at = pos;
            for i in 0..nb as u32 {
                at = self.prev_glyph(b, flag, set, at);
                if at < 0 || self.coverage_index(cov(p.wrapping_add(i * 2)), b.g[at as usize].gid) < 0 { return false; }
            }
            p = p.wrapping_add(nb as u32 * 2);
            ni = self.rd16(p) as i32; p = p.wrapping_add(2);
            if !(1..=32).contains(&ni) || !self.in_file(p, ni as u32 * 2) { return false; }
            input[0] = pos;
            for i in 0..ni as usize {
                if i != 0 { input[i] = self.next_glyph(b, flag, set, input[i - 1]); }
                if input[i] < 0 || self.coverage_index(cov(p.wrapping_add(i as u32 * 2)), b.g[input[i] as usize].gid) < 0 { return false; }
            }
            p = p.wrapping_add(ni as u32 * 2);
            let na = self.rd16(p) as i32; p = p.wrapping_add(2);
            if na > 32 || (na != 0 && !self.in_file(p, na as u32 * 2)) { return false; }
            let mut at2 = input[ni as usize - 1];
            for i in 0..na as u32 {
                at2 = self.next_glyph(b, flag, set, at2);
                if at2 < 0 || self.coverage_index(cov(p.wrapping_add(i * 2)), b.g[at2 as usize].gid) < 0 { return false; }
            }
            p = p.wrapping_add(na as u32 * 2);
            records = self.rd16(p); p = p.wrapping_add(2);
        } else {
            let glyphs = self.rd16(sub.wrapping_add(2)) as i32;
            records = self.rd16(sub.wrapping_add(4));
            if !(1..=32).contains(&glyphs) { return false; }
            p = sub.wrapping_add(6);
            ni = glyphs;
            input[0] = pos;
            for i in 0..ni as usize {
                if i != 0 { input[i] = self.next_glyph(b, flag, set, input[i - 1]); }
                if input[i] < 0 || self.coverage_index(cov(p.wrapping_add(i as u32 * 2)), b.g[input[i] as usize].gid) < 0 { return false; }
            }
            p = p.wrapping_add(ni as u32 * 2);
        }
        if records > 32 { return false; }
        if records > 0 && !self.in_file(p, records as u32 * 4) { return false; }
        let mut changed = 0;
        for r in 0..records as u32 {
            let seq = self.rd16(p.wrapping_add(r * 4));
            let li = self.rd16(p.wrapping_add(r * 4 + 2));
            if seq as i32 >= ni { continue; }
            let at = input[seq as usize];
            if at < 0 || at >= b.n() { continue; }
            let mut d2 = 0;
            if self.gsub_at(self.lookup_at(self.gsub, li), b, at, depth + 1, &mut d2) && d2 != 0 {
                for k in 0..ni as usize { if input[k] > at { input[k] += d2; } }
                changed += d2;
            }
        }
        *delta = changed;
        true
    }
    /// A lookup applied at one position: true when it matched; *delta is how many glyphs
    /// appeared (positive) or disappeared (negative).
    fn gsub_at(&self, lookup: u32, b: &mut GBuf, pos: i32, depth: i32, delta: &mut i32) -> bool {
        if depth > 8 || pos < 0 || pos >= b.n() || self.gsub == 0 || !self.in_file(lookup, 6) { return false; }
        let ty = self.rd16(lookup) as i32;
        let nsub = self.rd16(lookup + 4) as i32;
        let flag = self.rd16(lookup + 2);
        let mut set = 0;
        if flag & LF_MARK_SET != 0 {
            let o = lookup.wrapping_add(6).wrapping_add(nsub as u32 * 2);
            if !self.in_file(o, 2) { return false; }
            set = self.rd16(o) as i32;
        }
        if self.ignore_glyph(flag, set, b.g[pos as usize].gid) { return false; }
        for s in 0..nsub as u32 {
            let mut sub = lookup.wrapping_add(self.rd16(lookup.wrapping_add(6).wrapping_add(s * 2)) as u32);
            let mut t = ty;
            if ty == 7 {                                 // extension
                if !self.in_file(sub, 8) || self.rd16(sub) != 1 { continue; }
                t = self.rd16(sub + 2) as i32;
                sub = sub.wrapping_add(self.rd32(sub + 4));
            }
            let ok = match t {
                1 => self.gsub_single(sub, b, pos, delta),
                2 => self.gsub_multiple(sub, b, pos, delta),
                3 => self.gsub_alternate(sub, b, pos, delta),
                4 => self.gsub_ligature(sub, b, pos, flag, set, delta),
                5 => self.gsub_context3(sub, b, pos, flag, set, false, depth, delta),
                6 => self.gsub_context3(sub, b, pos, flag, set, true, depth, delta),
                _ => false,
            };
            if ok { return true; }
        }
        false
    }
    fn gsub_lookup(&self, lookup: u32, b: &mut GBuf, depth: i32) {
        let mut i = 0;
        while i < b.n() {
            let mut delta = 0;
            if self.gsub_at(lookup, b, i, depth, &mut delta) {
                if delta < 0 && i + delta < 0 { i = 0; } else { i += 1 + delta.max(0); }
            } else { i += 1; }
        }
    }
    fn gsub_feature(&self, script: u32, feat: u32, b: &mut GBuf) {
        for li in self.feature_lookups(self.gsub, script, feat) { self.gsub_lookup(self.lookup_at(self.gsub, li), b, 0); }
    }
    /// The same feature, but only at the glyphs that ask for it (Arabic forms).
    fn gsub_form_feature(&self, script: u32, feat: u32, form: u8, b: &mut GBuf) {
        let lookups = self.feature_lookups(self.gsub, script, feat);
        if lookups.is_empty() || !b.forms { return; }
        for li in lookups {
            let lookup = self.lookup_at(self.gsub, li);
            let mut i = 0;
            while i < b.n() {
                if b.form[i as usize] != form { i += 1; continue; }
                let mut delta = 0;
                if self.gsub_at(lookup, b, i, 0, &mut delta) { i += 1 + delta.max(0); } else { i += 1; }
            }
        }
    }

    // ------------------------------------------------------------ GPOS

    fn value_size(fmt: u16) -> u32 { (0..8).filter(|i| fmt & (1 << i) != 0).count() as u32 * 2 }
    /// (x placement, y placement, x advance)
    fn value_read(&self, mut p: u32, fmt: u16) -> Option<(i32, i32, i32)> {
        if !self.in_file(p, Self::value_size(fmt)) { return None; }
        let (mut xpl, mut ypl, mut xadv) = (0, 0, 0);
        if fmt & 1 != 0 { xpl = self.rs16(p) as i32; p += 2; }
        if fmt & 2 != 0 { ypl = self.rs16(p) as i32; p += 2; }
        if fmt & 4 != 0 { xadv = self.rs16(p) as i32; }
        Some((xpl, ypl, xadv))
    }
    fn value_apply(g: &mut Shaped, v: (i32, i32, i32)) {
        g.x_offset = g.x_offset.wrapping_add(v.0); g.y_offset = g.y_offset.wrapping_add(v.1); g.advance = g.advance.wrapping_add(v.2);
    }
    fn anchor_read(&self, p: u32) -> Option<(i32, i32)> {
        if !self.in_file(p, 6) { return None; }
        Some((self.rs16(p + 2) as i32, self.rs16(p + 4) as i32))
    }
    fn gpos_pair(&self, sub: u32, b: &mut GBuf, pos: i32, flag: u16, set: i32) -> bool {
        if !self.in_file(sub, 10) { return false; }
        let fmt = self.rd16(sub);
        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub + 2) as u32), b.g[pos as usize].gid);
        if idx < 0 { return false; }
        let j = self.next_glyph(b, flag, set, pos);
        if j < 0 { return false; }
        let (vf1, vf2) = (self.rd16(sub + 4), self.rd16(sub + 6));
        let (s1, s2) = (Self::value_size(vf1), Self::value_size(vf2));
        if fmt == 1 {
            let nsets = self.rd16(sub + 8) as u32;
            if idx as u32 >= nsets || !self.in_file(sub + 10, nsets * 2) { return false; }
            let ps = sub.wrapping_add(self.rd16(sub + 10 + idx as u32 * 2) as u32);
            if !self.in_file(ps, 2) { return false; }
            let np = self.rd16(ps);
            // PairValueRecord: secondGlyph, then the two value records (the C code stepped
            // 2 bytes too far per record, so only the first pair of a set ever matched)
            let stride = 2 + s1 + s2;
            for i in 0..np as u32 {
                let r = ps.wrapping_add(2).wrapping_add(i.wrapping_mul(stride));
                if !self.in_file(r, 2) { break; }
                if self.rd16(r) as i32 != b.g[j as usize].gid { continue; }
                let Some(v1) = self.value_read(r.wrapping_add(2), vf1) else { return false };
                let Some(v2) = self.value_read(r.wrapping_add(2 + s1), vf2) else { return false };
                Self::value_apply(&mut b.g[pos as usize], v1);
                Self::value_apply(&mut b.g[j as usize], v2);
                return true;
            }
            return false;
        }
        if fmt == 2 {
            let c1 = self.classdef_get(sub.wrapping_add(self.rd16(sub + 8) as u32), b.g[pos as usize].gid);
            let c2 = self.classdef_get(sub.wrapping_add(self.rd16(sub + 10) as u32), b.g[j as usize].gid);
            let (n1, n2) = (self.rd16(sub + 12) as i32, self.rd16(sub + 14) as i32);
            if c1 >= n1 || c2 >= n2 { return false; }
            let rec = sub.wrapping_add(16).wrapping_add(((c1 as u32).wrapping_mul(n2 as u32).wrapping_add(c2 as u32)).wrapping_mul(s1 + s2));
            let Some(v1) = self.value_read(rec, vf1) else { return false };
            let Some(v2) = self.value_read(rec.wrapping_add(s1), vf2) else { return false };
            Self::value_apply(&mut b.g[pos as usize], v1);
            Self::value_apply(&mut b.g[j as usize], v2);
            return true;
        }
        false
    }
    /// MarkArray / BaseArray / Mark2Array: a count, then class_count anchor offsets per entry.
    fn anchor_pair(&self, array: u32, entry: i32, cls: i32, class_count: i32) -> Option<(i32, i32)> {
        let p = array.wrapping_add(2).wrapping_add((entry as u32).wrapping_mul(class_count as u32).wrapping_mul(2)).wrapping_add(cls as u32 * 2);
        if !self.in_file(p, 2) { return None; }
        let off = self.rd16(p);
        if off == 0 { return None; }
        self.anchor_read(array.wrapping_add(off as u32))
    }
    /// (class, x, y)
    fn mark_anchor(&self, ma: u32, entry: i32) -> Option<(i32, i32, i32)> {
        let p = ma.wrapping_add(2).wrapping_add(entry as u32 * 4);
        if !self.in_file(p, 4) { return None; }
        let cls = self.rd16(p) as i32;
        let off = self.rd16(p + 2);
        if off == 0 { return None; }
        let (x, y) = self.anchor_read(ma.wrapping_add(off as u32))?;
        Some((cls, x, y))
    }
    /// The base (or ligature, or mark) a mark at pos attaches to: back over marks only.
    fn attach_target(&self, cov: u32, b: &GBuf, pos: i32) -> i32 {
        let mut i = pos - 1;
        let mut k = 0;
        while i >= 0 && k < 64 {
            let g = b.g[i as usize].gid;
            if self.coverage_index(cov, g) >= 0 { return i; }
            if self.glyph_class(g) != 3 { break; }
            i -= 1; k += 1;
        }
        -1
    }
    fn gpos_mark(&self, kind: i32, sub: u32, b: &mut GBuf, pos: i32) -> bool {
        if !self.in_file(sub, 12) || self.rd16(sub) != 1 { return false; }
        let mc = sub.wrapping_add(self.rd16(sub + 2) as u32);
        let bc = sub.wrapping_add(self.rd16(sub + 4) as u32);
        let class_count = self.rd16(sub + 6) as i32;
        let ma = sub.wrapping_add(self.rd16(sub + 8) as u32);
        let ba = sub.wrapping_add(self.rd16(sub + 10) as u32);
        let mi = self.coverage_index(mc, b.g[pos as usize].gid);
        if mi < 0 || class_count <= 0 { return false; }
        let base = self.attach_target(bc, b, pos);
        if base < 0 { return false; }
        let bi = self.coverage_index(bc, b.g[base as usize].gid);
        if bi < 0 { return false; }
        let (ax, ay, mx, my);
        if kind == 5 {                                   // mark to ligature: its last component
            if !self.in_file(ba, 2) { return false; }
            let nlig = self.rd16(ba);
            if bi as u32 >= nlig as u32 { return false; }
            let at = ba.wrapping_add(self.rd16(ba.wrapping_add(2).wrapping_add(bi as u32 * 2)) as u32);
            if !self.in_file(at, 2) { return false; }
            let comps = self.rd16(at) as i32;
            if comps <= 0 { return false; }
            let Some((cls, x, y)) = self.mark_anchor(ma, mi) else { return false };
            if cls >= class_count { return false; }
            let comp = comps - 1;
            let p = at.wrapping_add(2).wrapping_add((comp as u32).wrapping_mul(class_count as u32).wrapping_mul(2)).wrapping_add(cls as u32 * 2);
            if !self.in_file(p, 2) || self.rd16(p) == 0 { return false; }
            let Some((x2, y2)) = self.anchor_read(at.wrapping_add(self.rd16(p) as u32)) else { return false };
            (ax, ay, mx, my) = (x2, y2, x, y);
        } else {
            let Some((cls, x, y)) = self.mark_anchor(ma, mi) else { return false };
            if cls >= class_count { return false; }
            let Some((x2, y2)) = self.anchor_pair(ba, bi, cls, class_count) else { return false };
            (ax, ay, mx, my) = (x2, y2, x, y);
        }
        let g = &mut b.g[pos as usize];
        g.x_offset = g.x_offset.wrapping_add(ax - mx);
        g.y_offset = g.y_offset.wrapping_add(ay - my);
        true
    }
    fn gpos_at(&self, lookup: u32, b: &mut GBuf, pos: i32, flag: u16, set: i32) {
        let ty = self.rd16(lookup) as i32;
        let nsub = self.rd16(lookup.wrapping_add(4)) as u32;
        for s in 0..nsub {
            let mut sub = lookup.wrapping_add(self.rd16(lookup.wrapping_add(6).wrapping_add(s * 2)) as u32);
            let mut t = ty;
            if ty == 9 {
                if !self.in_file(sub, 8) || self.rd16(sub) != 1 { continue; }
                t = self.rd16(sub + 2) as i32;
                sub = sub.wrapping_add(self.rd32(sub + 4));
            }
            let ok = match t {
                1 => {                                    // single adjustment
                    if self.rd16(sub) != 1 { false } else {
                        let idx = self.coverage_index(sub.wrapping_add(self.rd16(sub.wrapping_add(2)) as u32), b.g[pos as usize].gid);
                        if idx < 0 { false } else {
                            let vf = self.rd16(sub.wrapping_add(4));
                            match self.value_read(sub.wrapping_add(6).wrapping_add((idx as u32).wrapping_mul(Self::value_size(vf))), vf) {
                                Some(v) => { Self::value_apply(&mut b.g[pos as usize], v); true }
                                None => false,
                            }
                        }
                    }
                }
                2 => self.gpos_pair(sub, b, pos, flag, set),
                4 | 5 | 6 => self.gpos_mark(t, sub, b, pos),
                _ => false,
            };
            if ok { return; }
        }
    }
    fn gpos_feature(&self, script: u32, feat: u32, b: &mut GBuf) {
        for li in self.feature_lookups(self.gpos, script, feat) {
            let lookup = self.lookup_at(self.gpos, li);
            if !self.in_file(lookup, 6) { continue; }
            let flag = self.rd16(lookup + 2);
            let mut set = 0;
            if flag & LF_MARK_SET != 0 {
                let ns = self.rd16(lookup + 4) as u32;
                let o = lookup.wrapping_add(6).wrapping_add(ns * 2);
                if self.in_file(o, 2) { set = self.rd16(o) as i32; }
            }
            for i in 0..b.n() { self.gpos_at(lookup, b, i, flag, set); }
        }
    }

    /// Shapes one run of text (one script, one direction) with the font's GSUB and GPOS.
    /// `script` is an OpenType script tag or 0. None when the text has no glyphs (or the
    /// buffer cannot be had).
    pub fn shape(&self, cps: &[u32], script: u32, _rtl: bool) -> Option<Vec<Shaped>> {
        let n = cps.len();
        if n == 0 || n > 65536 { return None; }
        let cap = n * 3 + 16;
        let (mut g, mut form) = (Vec::new(), Vec::new());
        if g.try_reserve_exact(cap).is_err() || form.try_reserve_exact(cap).is_err() { return None; }
        let orig = arabic_forms(cps)?;
        for (i, &cp) in cps.iter().enumerate() {
            if matches!(cp, 0x200B | 0x200C | 0x200D | 0x2060 | 0x00AD) { continue; }   // invisible format characters
            let gid = self.glyph_index(cp);
            g.push(Shaped { gid, advance: self.advance(gid), x_offset: 0, y_offset: 0, cluster: i as i32 });
            form.push(orig[i]);
        }
        if g.is_empty() { return None; }
        let mut b = GBuf { g, form, cap, forms: true };
        if self.gsub != 0 {
            self.gsub_feature(script, tag(b"ccmp"), &mut b);
            self.gsub_feature(script, tag(b"locl"), &mut b);
            if script == tag(b"arab") {
                self.gsub_form_feature(script, tag(b"isol"), FORM_ISOL, &mut b);
                self.gsub_form_feature(script, tag(b"init"), FORM_INIT, &mut b);
                self.gsub_form_feature(script, tag(b"medi"), FORM_MEDI, &mut b);
                self.gsub_form_feature(script, tag(b"fina"), FORM_FINA, &mut b);
            }
            self.gsub_feature(script, tag(b"rlig"), &mut b);
            self.gsub_feature(script, tag(b"calt"), &mut b);
            self.gsub_feature(script, tag(b"liga"), &mut b);
            self.gsub_feature(script, tag(b"clig"), &mut b);
        }
        if self.gpos != 0 {
            self.gpos_feature(script, tag(b"kern"), &mut b);
            self.gpos_feature(script, tag(b"mark"), &mut b);
            self.gpos_feature(script, tag(b"mkmk"), &mut b);
        }
        Some(b.g)
    }

    // ------------------------------------------------------------ outlines

    fn glyph_slice(&self, gid: i32) -> Result<Option<&'a [u8]>, ()> {
        if gid < 0 || gid as u32 >= self.num_glyphs { return Err(()); }
        let g = gid as u32;
        let (a, b) = if self.loca_long {
            let o = self.loca.wrapping_add(g.wrapping_mul(4));
            if !self.in_file(o, 8) { return Err(()); }
            (self.rd32(o), self.rd32(o + 4))
        } else {
            let o = self.loca.wrapping_add(g.wrapping_mul(2));
            if !self.in_file(o, 4) { return Err(()); }
            (self.rd16(o) as u32 * 2, self.rd16(o + 2) as u32 * 2)
        };
        if b <= a { return Ok(None); }
        if a >= self.glyf_len || b > self.glyf_len { return Err(()); }
        let s = (self.glyf + a) as usize;
        Ok(Some(&self.d[s..s + (b - a) as usize]))
    }
    /// 1 added, 0 nothing, -1 bad glyph
    fn emit_simple(&self, g: &[u8], x: &Xform, o: &mut Outline) -> i32 {
        let glen = g.len();
        let mut c = Cur { g, p: 0, end: glen, ok: true };
        let ncont = c.s16();
        if !c.ok || ncont <= 0 || ncont > 1024 { return if ncont == 0 { 1 } else { -1 }; }
        if c.p + 8 > glen { return -1; }
        let endp_off = c.p + 8;                          // past the bounding box
        let nc = ncont as usize;
        if endp_off + nc * 2 + 2 > glen { return -1; }
        let r16 = |o: usize| (g[o] as u32) << 8 | g[o + 1] as u32;
        let npoints = r16(endp_off + (nc - 1) * 2) as usize + 1;
        let ilen_at = endp_off + nc * 2;
        let flags_at = ilen_at + 2 + r16(ilen_at) as usize;
        if flags_at > glen || npoints > MAX_POINTS { return -1; }
        let base = o.pts.len();
        if base + npoints > MAX_POINTS || o.ends.len() + nc > MAX_POINTS { return -1; }
        if o.pts.try_reserve(npoints).is_err() || o.ends.try_reserve(nc).is_err() { return -1; }
        // the flags, with repeats expanded; they ride in .on until the end
        let mut fc = Cur { g, p: flags_at, end: glen, ok: true };
        let mut pts: Vec<Pt> = Vec::new();
        if pts.try_reserve_exact(npoints).is_err() { return -1; }
        while pts.len() < npoints {
            let fl = fc.u8();
            if !fc.ok { return -1; }
            pts.push(Pt { x: 0, y: 0, on: fl });
            if fl & 0x08 != 0 {
                let mut rep = fc.u8();
                if !fc.ok { return -1; }
                while rep > 0 && pts.len() < npoints { pts.push(Pt { x: 0, y: 0, on: fl }); rep -= 1; }
            }
        }
        let mut v: i32 = 0;
        for p in pts.iter_mut() {
            let fl = p.on;
            if fl & 0x02 != 0 { let dd = fc.u8() as i32; v = v.wrapping_add(if fl & 0x10 != 0 { dd } else { -dd }); }
            else if fl & 0x10 == 0 { v = v.wrapping_add(fc.s16() as i32); }
            if !fc.ok { return -1; }
            p.x = v;
        }
        v = 0;
        for p in pts.iter_mut() {
            let fl = p.on;
            if fl & 0x04 != 0 { let dd = fc.u8() as i32; v = v.wrapping_add(if fl & 0x20 != 0 { dd } else { -dd }); }
            else if fl & 0x20 == 0 { v = v.wrapping_add(fc.s16() as i32); }
            if !fc.ok { return -1; }
            p.y = v;
        }
        for p in pts.iter_mut() {
            let (px, py) = (p.x, p.y);
            p.x = mul14(x.a, px, x.c, py).wrapping_add(x.e);
            p.y = mul14(x.b, px, x.d, py).wrapping_add(x.f);
            p.on &= 1;
        }
        o.pts.extend_from_slice(&pts);
        for k in 0..nc { o.ends.push((base as u32 + r16(endp_off + k * 2) + 1) as u16); }
        1
    }
    fn emit_composite(&self, g: &[u8], x: &Xform, o: &mut Outline, depth: i32) -> i32 {
        if depth >= 8 { return -1; }
        let mut c = Cur { g, p: 10, end: g.len(), ok: true };
        loop {
            let flags = c.u16();
            let comp = c.u16();
            if !c.ok { return -1; }
            let (mut dx, mut dy): (i32, i32);
            if flags & 0x0001 != 0 { dx = c.s16() as i32; dy = c.s16() as i32; }
            else if flags & 0x0002 != 0 { dx = c.u8() as i8 as i32; dy = c.u8() as i8 as i32; }
            else { c.u8(); c.u8(); dx = 0; dy = 0; }          // point matching: uncommon, treated as 0
            let (mut a, mut bb, mut cc, mut dd) = (F2DOT14_ONE, 0, 0, F2DOT14_ONE);
            if flags & 0x0008 != 0 { a = c.s16() as i32; dd = a; }
            else if flags & 0x0040 != 0 { a = c.s16() as i32; dd = c.s16() as i32; }
            else if flags & 0x0080 != 0 { a = c.s16() as i32; bb = c.s16() as i32; cc = c.s16() as i32; dd = c.s16() as i32; }
            if !c.ok { return -1; }
            if flags & 0x0800 != 0 && flags & 0x1000 == 0 {   // scaled component offset
                let sx = mul14(a, dx, cc, dy);
                let sy = mul14(bb, dx, dd, dy);
                dx = sx; dy = sy;
            }
            let full = xform_mul(x, &Xform { a, b: bb, c: cc, d: dd, e: dx, f: dy });
            let r = self.emit_glyph(comp as i32, &full, o, depth + 1);
            if r < 0 { return r; }
            if flags & 0x0020 == 0 { break; }
        }
        1
    }
    fn emit_glyph(&self, gid: i32, x: &Xform, o: &mut Outline, depth: i32) -> i32 {
        let g = match self.glyph_slice(gid) { Err(()) => return -1, Ok(None) => return 0, Ok(Some(g)) => g };
        if g.len() < 10 { return -1; }
        if (rd16_at(g, 0) as i16) >= 0 { self.emit_simple(g, x, o) } else { self.emit_composite(g, x, o, depth) }
    }

    /// Rasterises one glyph at `px` pixels per em (1..512); `slant` shears the outline by
    /// that percentage of the em per em above the baseline. Err keeps the advance.
    pub fn render(&self, gid: i32, px: i32, slant: i32) -> Result<Glyph, i32> {
        let px = px.clamp(1, MAX_PX);
        let adv = self.advance(gid);
        let advance = ((adv as i64 * px as i64 + self.units_per_em as i64 / 2) / self.units_per_em as i64) as i32;
        let mut out = Glyph { advance, ..Default::default() };
        if gid < 0 || gid as u32 >= self.num_glyphs { return Ok(out); }
        let scale_q = (((px as i64) << 16) / self.units_per_em as i64) as i32;
        let mut o = Outline { pts: Vec::new(), ends: Vec::new() };
        let id = Xform { a: F2DOT14_ONE, b: 0, c: 0, d: F2DOT14_ONE, e: 0, f: 0 };
        let r = self.emit_glyph(gid, &id, &mut o, 0);
        if r <= 0 { return if r < 0 { Err(advance) } else { Ok(out) }; }
        let mut e: Vec<Edge> = Vec::new();
        let mut bb = BBox::default();
        let mut first = 0usize;
        for &end in &o.ends {
            let last = end as i64 - 1;
            if last < first as i64 || last >= o.pts.len() as i64 { return Err(advance); }
            if !contour_edges(&o, first, last as usize, scale_q, slant, &mut e, &mut bb) { return Err(advance); }
            first = last as usize + 1;
        }
        drop(o);
        if !bb.any || e.is_empty() { return Ok(out); }
        match rasterise(&e, &bb, &mut out) { 0 => Ok(out), _ => Err(advance) }
    }
}

// ---------------------------------------------------------------- Arabic joining

const JOIN_U: u8 = 0; const JOIN_R: u8 = 1; const JOIN_D: u8 = 2; const JOIN_L: u8 = 3; const JOIN_C: u8 = 4; const JOIN_T: u8 = 5;
const FORM_NONE: u8 = 0; const FORM_ISOL: u8 = 1; const FORM_INIT: u8 = 2; const FORM_MEDI: u8 = 3; const FORM_FINA: u8 = 4;

fn join_type(c: u32) -> u8 {
    let r = |lo: u32, hi: u32| c >= lo && c <= hi;
    // transparent: combining marks never break a join
    if r(0x0300, 0x036F) || r(0x0483, 0x0489) || r(0x0591, 0x05BD) || c == 0x05BF || r(0x05C1, 0x05C2) || r(0x05C4, 0x05C5) || c == 0x05C7
        || r(0x0610, 0x061A) || r(0x064B, 0x065F) || c == 0x0670 || r(0x06D6, 0x06DC) || r(0x06DF, 0x06E4) || r(0x06E7, 0x06E8) || r(0x06EA, 0x06ED)
        || c == 0x0711 || r(0x0730, 0x074A) || r(0x07A6, 0x07B0) || r(0x07EB, 0x07F3) || r(0x0816, 0x0819) || r(0x081B, 0x0823) || r(0x0825, 0x0827)
        || r(0x0829, 0x082D) || r(0x0859, 0x085B) || r(0x0898, 0x089F) || r(0x08CA, 0x08E1) || r(0x08E3, 0x08FF) || r(0x1AB0, 0x1AFF)
        || r(0x1DC0, 0x1DFF) || r(0x20D0, 0x20FF) || r(0xFE00, 0xFE0F) || r(0xFE20, 0xFE2F) { return JOIN_T; }
    if c == 0x0640 || c == 0x200D { return JOIN_C; }    // tatweel, ZWJ
    if c == 0x200C || c == 0x200B { return JOIN_U; }    // ZWNJ, ZWSP
    if c == 0x0710 || c == 0x07FA { return JOIN_R; }    // Syriac alaph, Mandaic
    if r(0x0620, 0x064A) || r(0x066E, 0x06D3) || c == 0x06D5 || r(0x06EE, 0x06EF) || r(0x06FA, 0x06FC) || c == 0x06FF || r(0x0750, 0x077F) || r(0x08A0, 0x08C8) {
        // right-joining: alef, dal, thal, reh, zain, waw and friends
        if matches!(c, 0x0622 | 0x0623 | 0x0624 | 0x0625 | 0x0627 | 0x0629 | 0x0648 | 0x0671 | 0x0672 | 0x0673 | 0x06C0 | 0x06CD | 0x06CF | 0x06D5
            | 0x06EE | 0x06EF | 0x06FE | 0x08AA | 0x08AE | 0x08B1 | 0x08B2 | 0x08B9)
            || r(0x062F, 0x0632) || r(0x0675, 0x0677) || r(0x0688, 0x0699) || r(0x06C3, 0x06CB) || r(0x06D2, 0x06D3) { return JOIN_R; }
        return JOIN_D;
    }
    if r(0x0700, 0x070D) || r(0x0800, 0x0815) || r(0x0840, 0x0858) || r(0x0860, 0x086A) || r(0x0870, 0x0887) || r(0x0889, 0x088E) { return JOIN_D; }
    if c == 0x1807 || r(0x1820, 0x1878) || r(0xA840, 0xA871) { return JOIN_D; }   // Mongolian, Phags-pa
    JOIN_U
}
fn arabic_forms(cps: &[u32]) -> Option<Vec<u8>> {
    let n = cps.len();
    let mut form = Vec::new();
    if form.try_reserve_exact(n).is_err() { return None; }
    let types: Vec<u8> = cps.iter().map(|&c| join_type(c)).collect();
    for i in 0..n {
        let t = types[i];
        if !matches!(t, JOIN_R | JOIN_D | JOIN_L | JOIN_C) { form.push(FORM_NONE); continue; }
        let mut p = i as isize - 1;
        while p >= 0 && types[p as usize] == JOIN_T { p -= 1; }
        let mut q = i + 1;
        while q < n && types[q] == JOIN_T { q += 1; }
        let pt = if p >= 0 { types[p as usize] } else { JOIN_U };
        let qt = if q < n { types[q] } else { JOIN_U };
        let prev = matches!(pt, JOIN_D | JOIN_L | JOIN_C) && matches!(t, JOIN_D | JOIN_R | JOIN_C);
        let next = matches!(t, JOIN_D | JOIN_L | JOIN_C) && matches!(qt, JOIN_D | JOIN_R | JOIN_C);
        form.push(if prev && next { FORM_MEDI } else if prev { FORM_FINA } else if next { FORM_INIT } else { FORM_ISOL });
    }
    Some(form)
}

// ---------------------------------------------------------------- flattening and rasterising

fn edge_add(e: &mut Vec<Edge>, bb: &mut BBox, x0: i32, y0: i32, x1: i32, y1: i32) -> bool {
    if !bb.any {
        *bb = BBox { minx: x0.min(x1), maxx: x0.max(x1), miny: y0.min(y1), maxy: y0.max(y1), any: true };
    } else {
        bb.minx = bb.minx.min(x0).min(x1); bb.maxx = bb.maxx.max(x0).max(x1);
        bb.miny = bb.miny.min(y0).min(y1); bb.maxy = bb.maxy.max(y0).max(y1);
    }
    if x0 == x1 && y0 == y1 { return true; }
    if e.len() >= MAX_EDGES || e.try_reserve(1).is_err() { return false; }
    e.push(Edge { x0, y0, x1, y1 });
    true
}
fn to_screen(x: i32, y: i32, scale_q: i32, slant: i32) -> (i32, i32) {
    let mut px = (x as i64).wrapping_mul(scale_q as i64) >> 10;
    let py = -((y as i64).wrapping_mul(scale_q as i64) >> 10);
    if slant != 0 { px = px.wrapping_add((y as i64).wrapping_mul(scale_q as i64).wrapping_mul(slant as i64) / (100 * 1024)); }
    (px as i32, py as i32)
}
fn flatten_quad(e: &mut Vec<Edge>, bb: &mut BBox, x0: i32, y0: i32, cx: i32, cy: i32, x1: i32, y1: i32) -> bool {
    let d = cx.wrapping_sub(x0).wrapping_abs().wrapping_add(cy.wrapping_sub(y0).wrapping_abs())
        .wrapping_add(x1.wrapping_sub(cx).wrapping_abs()).wrapping_add(y1.wrapping_sub(cy).wrapping_abs());
    let n = (d >> 5).clamp(1, 64);                       // about half a pixel per piece
    let (mut px, mut py) = (x0, y0);
    for i in 1..=n {
        let t = ((i as i64) << 16) / n as i64;
        let u = 65536 - t;
        let q = |a: i32, c: i32, z: i32| (u.wrapping_mul(u).wrapping_mul(a as i64).wrapping_add((2 * u * t).wrapping_mul(c as i64)).wrapping_add(t.wrapping_mul(t).wrapping_mul(z as i64)) >> 32) as i32;
        let (x, y) = (q(x0, cx, x1), q(y0, cy, y1));
        if !edge_add(e, bb, px, py, x, y) { return false; }
        px = x; py = y;
    }
    true
}
/// One contour as a cyclic sequence that starts on the curve.
fn contour_edges(o: &Outline, first: usize, last: usize, scale_q: i32, slant: i32, e: &mut Vec<Edge>, bb: &mut BBox) -> bool {
    let p = &o.pts;
    let (mode, m, mx, my) = if p[first].on != 0 { (0, last - first + 1, 0, 0) }
        else if p[last].on != 0 { (1, last - first + 1, 0, 0) }
        else { (2, last - first + 2, p[first].x.wrapping_add(p[last].x) / 2, p[first].y.wrapping_add(p[last].y) / 2) };
    let get = |i: usize| -> Pt {
        match mode {
            2 => if i == 0 { Pt { x: mx, y: my, on: 1 } } else { p[first + i - 1] },
            1 => if i == 0 { p[last] } else { p[first + i - 1] },
            _ => p[first + i],
        }
    };
    let p0 = get(0);
    let (sx0, sy0) = to_screen(p0.x, p0.y, scale_q, slant);
    let (mut px, mut py) = (sx0, sy0);
    let mut i = 1;
    while i < m {
        let pt = get(i);
        if pt.on != 0 {
            let (x, y) = to_screen(pt.x, pt.y, scale_q, slant);
            if !edge_add(e, bb, px, py, x, y) { return false; }
            px = x; py = y; i += 1;
        } else {
            let ni = i + 1;
            let (cx, cy) = to_screen(pt.x, pt.y, scale_q, slant);
            if ni < m && get(ni).on == 0 {
                let nn = get(ni);
                let (ex, ey) = to_screen(pt.x.wrapping_add(nn.x) / 2, pt.y.wrapping_add(nn.y) / 2, scale_q, slant);
                if !flatten_quad(e, bb, px, py, cx, cy, ex, ey) { return false; }
                px = ex; py = ey; i += 1;
            } else {
                let (ex, ey);
                if ni < m { let nn = get(ni); (ex, ey) = to_screen(nn.x, nn.y, scale_q, slant); i += 2; }
                else { (ex, ey) = (sx0, sy0); i = m; }
                if !flatten_quad(e, bb, px, py, cx, cy, ex, ey) { return false; }
                px = ex; py = ey;
            }
        }
    }
    if (px != sx0 || py != sy0) && !edge_add(e, bb, px, py, sx0, sy0) { return false; }
    true
}
fn add_span(acc: &mut [i32], ox: i32, xa: i32, xb: i32) {
    if xb <= xa { return; }
    let w = acc.len() as i32;
    let c0 = ((xa >> 6).wrapping_sub(ox)).max(0);
    let c1 = ((xb.wrapping_add(63) >> 6).wrapping_sub(ox)).min(w);
    for c in c0..c1 {
        let px = ox.wrapping_add(c) << 6;
        let lo = xa.max(px);
        let hi = xb.min(px.wrapping_add(64));
        if hi > lo { acc[c as usize] += hi - lo; }
    }
}
/// 8 sub-scanlines per pixel row, spans measured exactly: 512 levels of coverage.
fn rasterise(e: &[Edge], bb: &BBox, out: &mut Glyph) -> i32 {
    let (x0, y0) = (bb.minx >> 6, bb.miny >> 6);
    let (x1, y1) = (bb.maxx.wrapping_add(63) >> 6, bb.maxy.wrapping_add(63) >> 6);
    let (w, h) = (x1.wrapping_sub(x0), y1.wrapping_sub(y0));
    if w <= 0 || h <= 0 { return 0; }
    if w > 4096 || h > 4096 { return -1; }
    let (mut cov, mut acc, mut cr): (Vec<u8>, Vec<i32>, Vec<(i32, i8)>) = (Vec::new(), Vec::new(), Vec::new());
    if cov.try_reserve_exact((w * h) as usize).is_err() || acc.try_reserve_exact(w as usize).is_err() || cr.try_reserve_exact(e.len()).is_err() { return -1; }
    cov.resize((w * h) as usize, 0);
    acc.resize(w as usize, 0);
    for row in 0..h {
        acc.fill(0);
        for s in 0..SUBSAMPLES {
            let y = y0.wrapping_add(row).wrapping_mul(64).wrapping_add((2 * s + 1) * (64 / (2 * SUBSAMPLES)));
            cr.clear();
            for ed in e {
                let (ya, yb) = (ed.y0, ed.y1);
                if ya == yb { continue; }
                let (lo, hi) = if ya < yb { (ya, yb) } else { (yb, ya) };
                if y < lo || y >= hi { continue; }
                let num = (y.wrapping_sub(ya) as i64).wrapping_mul(ed.x1.wrapping_sub(ed.x0) as i64);
                let x = ed.x0.wrapping_add(num.wrapping_div(yb.wrapping_sub(ya) as i64) as i32);
                cr.push((x, if yb > ya { 1 } else { -1 }));
            }
            cr.sort_by_key(|c| c.0);                     // stable, like the insertion sort it replaces
            let (mut wind, mut open, mut span) = (0i32, false, 0i32);
            for &(x, dir) in &cr {
                wind += dir as i32;
                if !open && wind != 0 { span = x; open = true; }
                else if open && wind == 0 { add_span(&mut acc, x0, span, x); open = false; }
            }
        }
        let dst = &mut cov[(row * w) as usize..((row + 1) * w) as usize];
        for (d, &a) in dst.iter_mut().zip(acc.iter()) { *d = ((a.wrapping_mul(255).wrapping_add(256)) / 512).min(255) as u8; }
    }
    out.width = w; out.height = h; out.xoff = x0; out.yoff = y0;
    out.coverage = cov;
    0
}

// ---------------------------------------------------------------- the C interface (ttf.h)

pub type AllocFn = unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void;
pub type FreeFn = unsafe extern "C" fn(*mut c_void, *mut c_void);
/// ttf_glyph_t
#[repr(C)]
pub struct CGlyph { pub width: c_int, pub height: c_int, pub xoff: c_int, pub yoff: c_int, pub advance: c_int, pub coverage: *mut u8 }
/// ttf_face_t: the face, and the caller's allocator for what it hands back.
pub struct CFace { face: Face<'static>, alloc: AllocFn, free: FreeFn, user: *mut c_void }

#[no_mangle]
pub unsafe extern "C" fn ttf_open(data: *const c_void, size: usize, alloc: Option<AllocFn>, free: Option<FreeFn>, user: *mut c_void) -> *mut CFace {
    let (Some(alloc), Some(free)) = (alloc, free) else { return core::ptr::null_mut() };
    if data.is_null() || size < 12 { return core::ptr::null_mut(); }
    let d: &'static [u8] = core::slice::from_raw_parts(data as *const u8, size);
    let Some(face) = Face::open(d) else { return core::ptr::null_mut() };
    let p = alloc(user, core::mem::size_of::<CFace>()) as *mut CFace;
    if p.is_null() { return p; }
    p.write(CFace { face, alloc, free, user });
    p
}
#[no_mangle]
pub unsafe extern "C" fn ttf_close(f: *mut CFace) {
    if f.is_null() { return; }
    let (free, user) = ((*f).free, (*f).user);
    core::ptr::drop_in_place(f);
    free(user, f as *mut c_void);
}
unsafe fn face<'a>(f: *const CFace) -> Option<&'a Face<'static>> { f.as_ref().map(|c| &c.face) }
#[no_mangle]
pub unsafe extern "C" fn ttf_units_per_em(f: *const CFace) -> c_int { face(f).map_or(0, |f| f.units_per_em) }
#[no_mangle]
pub unsafe extern "C" fn ttf_ascender(f: *const CFace) -> c_int { face(f).map_or(0, |f| f.ascender) }
#[no_mangle]
pub unsafe extern "C" fn ttf_descender(f: *const CFace) -> c_int { face(f).map_or(0, |f| f.descender) }
#[no_mangle]
pub unsafe extern "C" fn ttf_line_gap(f: *const CFace) -> c_int { face(f).map_or(0, |f| f.line_gap) }
#[no_mangle]
pub unsafe extern "C" fn ttf_num_glyphs(f: *const CFace) -> c_int { face(f).map_or(0, |f| f.num_glyphs()) }
#[no_mangle]
pub unsafe extern "C" fn ttf_glyph_index(f: *const CFace, c: u32) -> c_int { face(f).map_or(0, |f| f.glyph_index(c)) }
#[no_mangle]
pub unsafe extern "C" fn ttf_advance(f: *const CFace, gid: c_int) -> c_int { face(f).map_or(0, |f| f.advance(gid)) }
#[no_mangle]
pub unsafe extern "C" fn ttf_kerning(f: *const CFace, l: c_int, r: c_int) -> c_int { face(f).map_or(0, |f| f.kerning(l, r)) }
/// int ttf_render(...): 0, or -1; the coverage comes from the face's allocator.
#[no_mangle]
pub unsafe extern "C" fn ttf_render(f: *const CFace, gid: c_int, px: c_int, slant: c_int, out: *mut CGlyph) -> c_int {
    let (Some(cf), false) = (f.as_ref(), out.is_null()) else { return -1 };
    out.write(CGlyph { width: 0, height: 0, xoff: 0, yoff: 0, advance: 0, coverage: core::ptr::null_mut() });
    match cf.face.render(gid, px, slant) {
        Err(adv) => { (*out).advance = adv; -1 }
        Ok(g) => {
            (*out).advance = g.advance;
            if g.coverage.is_empty() { return 0; }
            let p = (cf.alloc)(cf.user, g.coverage.len()) as *mut u8;
            if p.is_null() { return -1; }
            core::ptr::copy_nonoverlapping(g.coverage.as_ptr(), p, g.coverage.len());
            *out = CGlyph { width: g.width, height: g.height, xoff: g.xoff, yoff: g.yoff, advance: g.advance, coverage: p };
            0
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn ttf_glyph_release(f: *const CFace, g: *mut CGlyph) {
    let (Some(cf), Some(g)) = (f.as_ref(), g.as_mut()) else { return };
    if !g.coverage.is_null() { (cf.free)(cf.user, g.coverage as *mut c_void); }
    g.coverage = core::ptr::null_mut();
    g.width = 0; g.height = 0;
}
/// ttf_shaped_t *ttf_shape(face, cps, n, script, rtl, &out_n): from the face's allocator.
#[no_mangle]
pub unsafe extern "C" fn ttf_shape(f: *const CFace, cps: *const u32, n: c_int, script: u32, rtl: c_int, out_n: *mut c_int) -> *mut Shaped {
    if !out_n.is_null() { *out_n = 0; }
    let Some(cf) = f.as_ref() else { return core::ptr::null_mut() };
    if cps.is_null() || n <= 0 || n > 65536 { return core::ptr::null_mut(); }
    let Some(g) = cf.face.shape(core::slice::from_raw_parts(cps, n as usize), script, rtl != 0) else { return core::ptr::null_mut() };
    let p = (cf.alloc)(cf.user, g.len() * core::mem::size_of::<Shaped>()) as *mut Shaped;
    if p.is_null() { return p; }
    core::ptr::copy_nonoverlapping(g.as_ptr(), p, g.len());
    if !out_n.is_null() { *out_n = g.len() as c_int; }
    p
}
#[no_mangle]
pub unsafe extern "C" fn ttf_shape_free(f: *const CFace, s: *mut Shaped) {
    if let (Some(cf), false) = (f.as_ref(), s.is_null()) { (cf.free)(cf.user, s as *mut c_void); }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn junk_does_not_panic() {
        let mut seed = 0x9E37_79B9u32;
        let mut next = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for _ in 0..3000 {
            let n = (next() % 600) as usize + 12;
            let mut d: Vec<u8> = (0..n).map(|_| next() as u8).collect();
            d[..4].copy_from_slice(&[0, 1, 0, 0]);
            if let Some(f) = Face::open(&d) {
                for g in 0..8 { let _ = f.render(g, 16, 0); }
                let _ = f.shape(&[0x41, 0x627, 0x644, 0x301], tag(b"arab"), true);
            }
        }
    }
    #[test]
    fn joining() {
        // lam alef: lam joins forward, alef only back
        assert_eq!(arabic_forms(&[0x0644, 0x0627]).unwrap(), [FORM_INIT, FORM_FINA]);
        assert_eq!(arabic_forms(&[0x0628, 0x0628, 0x0628]).unwrap(), [FORM_INIT, FORM_MEDI, FORM_FINA]);
        assert_eq!(arabic_forms(&[0x41, 0x0627]).unwrap(), [FORM_NONE, FORM_ISOL]);
    }
}
