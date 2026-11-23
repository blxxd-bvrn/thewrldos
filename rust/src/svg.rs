//! SVG for The WRLD Browser (was src/apps/svg.c): the drawing elements icons and logos use,
//! rendered through the kernel's path rasteriser (src/gui/raster.c).
//!
//! Supported: <svg> with viewBox and preserveAspectRatio's default (xMidYMid meet), <g>,
//! <path> (every command, elliptical arcs included), <rect> (with rx/ry), <circle>,
//! <ellipse>, <line>, <polyline>, <polygon> and <use>; fill, stroke, stroke-width,
//! fill-rule, opacity, fill-opacity and stroke-opacity from attributes, style="" or
//! inherited from a parent; currentColor; transform (matrix, translate, scale, rotate,
//! skewX, skewY). A gradient fill is drawn in its first stop's colour. Text, filters, masks
//! and clip paths are not drawn.
//!
//! The arithmetic is the C code's, in single precision and in the same order, with its own
//! sine, cosine, arctangent and square root, so the same coordinates reach the rasteriser.
//! Where C converted a float to an integer, x86 gives 0x80000000 for a value out of range
//! (or NaN); `fti` does the same, where Rust's `as` would saturate.

use crate::css::{self, ST_COLOR, ST_DISPLAY, ST_FILL};
use crate::html::Node;
use core::ffi::c_int;
use core::ptr::null_mut;

/// canvas_t (src/include/gfx.h)
#[repr(C)]
pub struct Canvas { px: *mut u32, w: c_int, h: c_int, stride: c_int, clip: Rect }
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Rect { x: c_int, y: c_int, w: c_int, h: c_int }
#[repr(C)]
pub struct RasterPath { _p: [u8; 0] }

#[allow(clashing_extern_declarations)]
extern "C" {
    fn raster_path_new() -> *mut RasterPath;
    fn raster_reset(p: *mut RasterPath);
    fn raster_move_to(p: *mut RasterPath, x: i32, y: i32);
    fn raster_line_to(p: *mut RasterPath, x: i32, y: i32);
    fn raster_quad_to(p: *mut RasterPath, cx: i32, cy: i32, x: i32, y: i32);
    fn raster_cubic_to(p: *mut RasterPath, c1x: i32, c1y: i32, c2x: i32, c2y: i32, x: i32, y: i32);
    fn raster_close(p: *mut RasterPath);
    fn raster_fill(c: *mut Canvas, p: *const RasterPath, color: u32, evenodd: crate::CBool);
    fn raster_stroke(c: *mut Canvas, p: *const RasterPath, width: c_int, color: u32);
    fn canvas_push_clip(c: *mut Canvas, r: Rect) -> Rect;
    fn canvas_new(w: c_int, h: c_int) -> *mut Canvas;
    fn gfx_clear(c: *mut Canvas, col: u32);
}
use crate::html::{html_parse, doc_free};

const BLACK: u32 = 0xFF00_0000;

/* ---------------- C's float to int ---------------- */
/// (int32_t)v as x86 does it: truncation, and 0x80000000 when out of range or NaN.
#[inline] pub fn fti(v: f32) -> i32 { if v.is_nan() || v >= 2147483648.0 || v < -2147483648.0 { i32::MIN } else { v as i32 } }
/// (uint32_t)v: through a 64-bit conversion, as GCC does it on x86.
#[inline] fn ftu(v: f32) -> u32 { if v.is_nan() || v >= 9.223372e18 || v < -9.223372e18 { 0 } else { v as i64 as u32 } }

/* ---------------- small float maths (the kernel has no libm) ---------------- */
const PI_F: f32 = 3.14159265;
fn fabs_(x: f32) -> f32 { if x < 0.0 { -x } else { x } }
fn sqrt_(x: f32) -> f32 {
    if x <= 0.0 { return 0.0; }
    let mut r = if x > 1.0 { x / 2.0 } else { 1.0 };
    for _ in 0..20 { r = 0.5 * (r + x / r); }
    r
}
fn sin_(mut x: f32) -> f32 {
    /* far-off angles are brought near zero first: subtracting 2 pi from 1e10 (or infinity)
       changes nothing, and svg.c's loop below then never ended */
    if !x.is_finite() || x > 1.0e7 || x < -1.0e7 { return 0.0; }
    if x > 1.0e4 || x < -1.0e4 { x -= ((x / (2.0 * PI_F)) as i64) as f32 * (2.0 * PI_F); }
    while x > PI_F { x -= 2.0 * PI_F; }
    while x < -PI_F { x += 2.0 * PI_F; }
    let x2 = x * x;
    x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0)))))
}
fn cos_(x: f32) -> f32 { sin_(x + PI_F / 2.0) }
fn atan2_(y: f32, x: f32) -> f32 {
    if x == 0.0 && y == 0.0 { return 0.0; }
    let (ax, ay) = (fabs_(x), fabs_(y));
    let a = if ax > ay { ay / ax } else { ax / ay };
    let s = a * a;
    let mut r = ((-0.0464964749f32 * s + 0.15931422) * s - 0.327622764) * s * a + a;
    if ay > ax { r = PI_F / 2.0 - r; }
    if x < 0.0 { r = PI_F - r; }
    if y < 0.0 { -r } else { r }
}

/* ---------------- strings as the C code read them ---------------- */
#[inline] fn at(s: &[u8], i: usize) -> u8 { if i < s.len() { s[i] } else { 0 } }
unsafe fn cs<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
fn ieq(a: &[u8], b: &[u8]) -> bool { a.eq_ignore_ascii_case(b) }
fn iprefix(s: &[u8], p: &[u8]) -> bool { s.len() >= p.len() && s[..p.len()].eq_ignore_ascii_case(p) }
unsafe fn tag<'a>(n: *mut Node) -> &'a [u8] { let t = &(*n).tag; &t[..t.iter().position(|&c| c == 0).unwrap_or(12)] }

/* ---------------- attributes and style ---------------- */
unsafe fn sattr<'a>(n: *mut Node, name: &[u8]) -> Option<&'a [u8]> {
    for i in 0..(*n).nattr.max(0) as usize {
        if ieq(cs(*(*n).attr.add(i * 2)), name) { return Some(cs(*(*n).attr.add(i * 2 + 1))); }
    }
    None
}
/// A property from style="a: b; ..." or the attribute of the same name, as at most cap - 1 bytes.
unsafe fn sprop(n: *mut Node, name: &[u8], cap: usize) -> Option<alloc::vec::Vec<u8>> {
    if let Some(st) = sattr(n, b"style") {
        let nl = name.len();
        let mut p = 0;
        while at(st, p) != 0 {
            while at(st, p) == b' ' || at(st, p) == b';' { p += 1; }
            if iprefix(&st[p.min(st.len())..], name) {
                let mut q = p + nl;
                while at(st, q) == b' ' { q += 1; }
                if at(st, q) == b':' {
                    q += 1;
                    while at(st, q) == b' ' { q += 1; }
                    let mut out = alloc::vec::Vec::new();
                    while at(st, q + out.len()) != 0 && at(st, q + out.len()) != b';' && out.len() < cap - 1 { out.push(at(st, q + out.len())); }
                    while out.last() == Some(&b' ') { out.pop(); }
                    return Some(out);
                }
            }
            while at(st, p) != 0 && at(st, p) != b';' { p += 1; }
            if at(st, p) == 0 { break; }
            p += 1;
        }
    }
    sattr(n, name).map(|a| a[..a.len().min(cap - 1)].to_vec())
}
fn is_sep(c: u8) -> bool { c == b' ' || c == b',' || c == b'\n' || c == b'\t' || c == b'\r' }
fn num(s: &[u8], pp: &mut usize) -> f32 {
    let mut p = *pp;
    while is_sep(at(s, p)) { p += 1; }
    let (mut sign, mut v, mut frac) = (1.0f32, 0.0f32, 0.1f32);
    if at(s, p) == b'-' { sign = -1.0; p += 1; } else if at(s, p) == b'+' { p += 1; }
    while at(s, p).is_ascii_digit() { v = v * 10.0 + (at(s, p) - b'0') as i32 as f32; p += 1; }
    if at(s, p) == b'.' {
        p += 1;
        while at(s, p).is_ascii_digit() { v += (at(s, p) - b'0') as i32 as f32 * frac; frac *= 0.1; p += 1; }
    }
    let c1 = at(s, p + 1);
    if (at(s, p) == b'e' || at(s, p) == b'E') && (c1 == b'-' || c1 == b'+' || c1.is_ascii_digit()) {
        p += 1;
        let mut es = 1;
        let mut e: i32 = 0;
        if at(s, p) == b'-' { es = -1; p += 1; } else if at(s, p) == b'+' { p += 1; }
        while at(s, p).is_ascii_digit() { e = e.wrapping_mul(10).wrapping_add((at(s, p) - b'0') as i32); p += 1; }
        /* stops once v reaches 0 or infinity (where it would stay): an exponent of 2 billion
           digits' worth no longer takes seconds */
        while e > 0 && v != 0.0 && v.is_finite() { e -= 1; v = if es > 0 { v * 10.0 } else { v / 10.0 }; }
    }
    *pp = p;
    sign * v
}
fn has_num(s: &[u8], mut p: usize) -> bool {
    while is_sep(at(s, p)) { p += 1; }
    let c = at(s, p);
    c.is_ascii_digit() || c == b'-' || c == b'+' || c == b'.'
}
unsafe fn len_attr(n: *mut Node, name: &[u8], def: f32) -> f32 {
    match sattr(n, name) { None => def, Some(v) => { let mut p = 0; num(v, &mut p) } }
}

/* ---------------- transforms ---------------- */
/// x' = a x + c y + e, y' = b x + d y + f
#[derive(Clone, Copy)]
struct Mat { a: f32, b: f32, c: f32, d: f32, e: f32, f: f32 }
const ID: Mat = Mat { a: 1.0, b: 0.0, c: 0.0, d: 1.0, e: 0.0, f: 0.0 };
fn mat(a: f32, b: f32, c: f32, d: f32, e: f32, f: f32) -> Mat { Mat { a, b, c, d, e, f } }
/// m then n applied first: m * n
fn mul(m: Mat, n: Mat) -> Mat {
    Mat { a: m.a * n.a + m.c * n.b, b: m.b * n.a + m.d * n.b, c: m.a * n.c + m.c * n.d, d: m.b * n.c + m.d * n.d,
          e: m.a * n.e + m.c * n.f + m.e, f: m.b * n.e + m.d * n.f + m.f }
}
fn parse_transform(mut m: Mat, t: &[u8]) -> Mat {
    let mut p = 0;
    while at(t, p) != 0 {
        while at(t, p) == b' ' || at(t, p) == b',' { p += 1; }
        let name = p;
        while at(t, p) != 0 && at(t, p) != b'(' { p += 1; }
        if at(t, p) == 0 { break; }
        let mut nl = p - name;
        while nl > 0 && t[name + nl - 1] == b' ' { nl -= 1; }
        let nm = &t[name..name + nl];
        p += 1;
        let mut v = [0f32; 6];
        let mut k = 0;
        while at(t, p) != 0 && at(t, p) != b')' && k < 6 { if !has_num(t, p) { p += 1; continue; } v[k] = num(t, &mut p); k += 1; }
        if at(t, p) == b')' { p += 1; }
        let mut n = ID;
        if nm == b"matrix" && k == 6 { n = mat(v[0], v[1], v[2], v[3], v[4], v[5]); }
        else if nm == b"translate" { n.e = v[0]; n.f = if k > 1 { v[1] } else { 0.0 }; }
        else if nm == b"scale" { n.a = v[0]; n.d = if k > 1 { v[1] } else { v[0] }; }
        else if nm == b"rotate" {
            let r = v[0] * PI_F / 180.0;
            let (cs_, sn) = (cos_(r), sin_(r));
            let rot = mat(cs_, sn, -sn, cs_, 0.0, 0.0);
            if k >= 3 { n = mul(mul(mat(1.0, 0.0, 0.0, 1.0, v[1], v[2]), rot), mat(1.0, 0.0, 0.0, 1.0, -v[1], -v[2])); } else { n = rot; }
        }
        else if nm == b"skewX" { let r = v[0] * PI_F / 180.0; n.c = sin_(r) / cos_(r); }
        else if nm == b"skewY" { let r = v[0] * PI_F / 180.0; n.b = sin_(r) / cos_(r); }
        m = mul(m, n);
    }
    m
}

/* ---------------- building paths ---------------- */
struct Pb { p: *mut RasterPath, m: Mat }
fn fx(v: f32) -> i32 { fti(v * 64.0 + if v < 0.0 { -0.5 } else { 0.5 }) }
impl Pb {
    fn pt(&self, x: f32, y: f32) -> (i32, i32) { (fx(self.m.a * x + self.m.c * y + self.m.e), fx(self.m.b * x + self.m.d * y + self.m.f)) }
    unsafe fn mv(&mut self, x: f32, y: f32) { let (a, b) = self.pt(x, y); raster_move_to(self.p, a, b); }
    unsafe fn line(&mut self, x: f32, y: f32) { let (a, b) = self.pt(x, y); raster_line_to(self.p, a, b); }
    unsafe fn cubic(&mut self, x1: f32, y1: f32, x2: f32, y2: f32, x: f32, y: f32) {
        let (a, b) = self.pt(x1, y1); let (c, d) = self.pt(x2, y2); let (e, f) = self.pt(x, y);
        raster_cubic_to(self.p, a, b, c, d, e, f);
    }
    unsafe fn quad(&mut self, x1: f32, y1: f32, x: f32, y: f32) {
        let (a, b) = self.pt(x1, y1); let (e, f) = self.pt(x, y);
        raster_quad_to(self.p, a, b, e, f);
    }
    /// An elliptical arc from (x0,y0), as cubic pieces of at most 90 degrees (SVG implementation notes F.6).
    unsafe fn arc(&mut self, x0: f32, y0: f32, mut rx: f32, mut ry: f32, rot: f32, large: bool, sweep: bool, x: f32, y: f32) {
        if rx == 0.0 || ry == 0.0 { self.line(x, y); return; }
        rx = fabs_(rx); ry = fabs_(ry);
        let phi = rot * PI_F / 180.0;
        let (cp, sp) = (cos_(phi), sin_(phi));
        let (dx, dy) = ((x0 - x) / 2.0, (y0 - y) / 2.0);
        let x1 = cp * dx + sp * dy;
        let y1 = -sp * dx + cp * dy;
        let lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if lam > 1.0 { let s = sqrt_(lam); rx *= s; ry *= s; }
        let num_ = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        let den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        let mut co = if den > 0.0 && num_ > 0.0 { sqrt_(num_ / den) } else { 0.0 };
        if large == sweep { co = -co; }
        let cxp = co * rx * y1 / ry;
        let cyp = -co * ry * x1 / rx;
        let cx = cp * cxp - sp * cyp + (x0 + x) / 2.0;
        let cy = sp * cxp + cp * cyp + (y0 + y) / 2.0;
        let t1 = atan2_((y1 - cyp) / ry, (x1 - cxp) / rx);
        let t2 = atan2_((-y1 - cyp) / ry, (-x1 - cxp) / rx);
        let mut dt = t2 - t1;
        if sweep && dt < 0.0 { dt += 2.0 * PI_F; } else if !sweep && dt > 0.0 { dt -= 2.0 * PI_F; }
        let mut segs = fti(fabs_(dt) / (PI_F / 2.0) + 0.999);
        if segs < 1 { segs = 1; }
        let step = dt / segs as f32;
        let k = 4.0f32 / 3.0 * sin_(step / 4.0) / cos_(step / 4.0);
        let ex = |ux: f32, uy: f32| cx + rx * ux * cp - ry * uy * sp;
        let ey = |ux: f32, uy: f32| cy + rx * ux * sp + ry * uy * cp;
        for i in 0..segs {
            let a1 = t1 + i as f32 * step;
            let a2 = a1 + step;
            let (c1, s1, c2, s2) = (cos_(a1), sin_(a1), cos_(a2), sin_(a2));
            let (e1x, e1y, e2x, e2y) = (c1 - k * s1, s1 + k * c1, c2 + k * s2, s2 - k * c2);
            self.cubic(ex(e1x, e1y), ey(e1x, e1y), ex(e2x, e2y), ey(e2x, e2y), ex(c2, s2), ey(c2, s2));
        }
    }
    unsafe fn path_d(&mut self, d: &[u8]) {
        let (mut cx, mut cy, mut sx, mut sy, mut lcx, mut lcy) = (0f32, 0f32, 0f32, 0f32, 0f32, 0f32);
        let mut last = 0u8;
        let mut cmd = 0u8;
        let mut p = 0usize;
        while at(d, p) != 0 {
            while is_sep(at(d, p)) { p += 1; }
            if at(d, p) == 0 { break; }
            let letter = at(d, p).is_ascii_alphabetic();
            if letter { cmd = at(d, p); p += 1; }
            else if cmd == 0 { break; }
            else if !has_num(d, p) { p += 1; continue; }
            let rel = cmd >= b'a';
            let (ox, oy) = if rel { (cx, cy) } else { (0.0, 0.0) };
            macro_rules! n { () => { num(d, &mut p) } }
            match cmd | 0x20 {
                b'm' => { let x = n!() + ox; let y = n!() + oy; self.mv(x, y); cx = x; sx = x; cy = y; sy = y; cmd = if rel { b'l' } else { b'L' }; }
                b'l' => { let x = n!() + ox; let y = n!() + oy; self.line(x, y); cx = x; cy = y; }
                b'h' => { let x = n!() + ox; self.line(x, cy); cx = x; }
                b'v' => { let y = n!() + oy; self.line(cx, y); cy = y; }
                b'c' => {
                    let x1 = n!() + ox; let y1 = n!() + oy; let x2 = n!() + ox; let y2 = n!() + oy; let x = n!() + ox; let y = n!() + oy;
                    self.cubic(x1, y1, x2, y2, x, y); lcx = x2; lcy = y2; cx = x; cy = y;
                }
                b's' => {
                    let sm = last == b'c' || last == b's';
                    let x1 = if sm { 2.0 * cx - lcx } else { cx }; let y1 = if sm { 2.0 * cy - lcy } else { cy };
                    let x2 = n!() + ox; let y2 = n!() + oy; let x = n!() + ox; let y = n!() + oy;
                    self.cubic(x1, y1, x2, y2, x, y); lcx = x2; lcy = y2; cx = x; cy = y;
                }
                b'q' => {
                    let x1 = n!() + ox; let y1 = n!() + oy; let x = n!() + ox; let y = n!() + oy;
                    self.quad(x1, y1, x, y); lcx = x1; lcy = y1; cx = x; cy = y;
                }
                b't' => {
                    let sm = last == b'q' || last == b't';
                    let x1 = if sm { 2.0 * cx - lcx } else { cx }; let y1 = if sm { 2.0 * cy - lcy } else { cy };
                    let x = n!() + ox; let y = n!() + oy;
                    self.quad(x1, y1, x, y); lcx = x1; lcy = y1; cx = x; cy = y;
                }
                b'a' => {
                    let rx = n!(); let ry = n!(); let rot = n!();
                    /* the two flags may be written without separators ("a1 1 0 011 1") */
                    while at(d, p) == b' ' || at(d, p) == b',' { p += 1; }
                    let large = at(d, p) == b'1'; if at(d, p) != 0 { p += 1; }
                    while at(d, p) == b' ' || at(d, p) == b',' { p += 1; }
                    let sweep = at(d, p) == b'1'; if at(d, p) != 0 { p += 1; }
                    let x = num(d, &mut p) + ox; let y = num(d, &mut p) + oy;
                    self.arc(cx, cy, rx, ry, rot, large, sweep, x, y); cx = x; cy = y;
                }
                /* numbers after Z are an error: the path ends there (svg.c went round forever) */
                b'z' if !letter => break,
                b'z' => { raster_close(self.p); cx = sx; cy = sy; }
                _ => { p += 1; }
            }
            last = cmd | 0x20;
        }
    }
}

/* ---------------- painting ---------------- */
#[derive(Clone, Copy)]
struct Paint { fill: u32, stroke: u32, current: u32, fill_none: bool, stroke_none: bool, evenodd: bool, stroke_w: f32, opacity: f32, fill_op: f32, stroke_op: f32 }

unsafe fn find_id(n: *mut Node, id: &[u8]) -> *mut Node {
    if (*n).ty == 0 { if let Some(v) = sattr(n, b"id") { if v == id { return n; } } }
    let mut c = (*n).first;
    while !c.is_null() { let r = find_id(c, id); if !r.is_null() { return r; } c = (*c).next; }
    null_mut()
}
unsafe fn svg_root_of(n: *mut Node) -> *mut Node {
    let mut r = n;
    while !(*r).parent.is_null() && !ieq(tag(r), b"svg") { r = (*r).parent; }
    r
}
fn color(s: &[u8]) -> Option<u32> { css::parse_color(s) }
unsafe fn paint_value(n: *mut Node, v: &[u8], current: u32, out: &mut u32, none: &mut bool) -> bool {
    let mut i = 0;
    while at(v, i) == b' ' { i += 1; }
    let v = &v[i.min(v.len())..];
    if iprefix(v, b"none") || iprefix(v, b"transparent") { *none = true; return true; }
    if iprefix(v, b"currentcolor") { *out = current; *none = false; return true; }
    if v.starts_with(b"url(") {                                    /* gradient: its first stop */
        let mut id: alloc::vec::Vec<u8> = alloc::vec::Vec::new();
        if let Some(h) = v.iter().position(|&c| c == b'#') {
            let mut l = h + 1;
            while at(v, l) != 0 && at(v, l) != b')' && at(v, l) != b'"' && at(v, l) != b'\'' && id.len() < 63 { id.push(v[l]); l += 1; }
        }
        let mut g = if !id.is_empty() { find_id(svg_root_of(n), &id) } else { null_mut() };
        let mut hop = 0;
        while !g.is_null() && hop < 4 {
            let mut c = (*g).first;
            while !c.is_null() {
                if (*c).ty == 0 && ieq(tag(c), b"stop") {
                    if let Some(sc) = sprop(c, b"stop-color", 48) { if let Some(col) = color(&sc) { *out = col; *none = false; return true; } }
                }
                c = (*c).next;
            }
            let href = sattr(g, b"href").or_else(|| sattr(g, b"xlink:href"));
            g = match href { Some(h) if h.first() == Some(&b'#') => find_id(svg_root_of(n), &h[1..]), _ => null_mut() };
            hop += 1;
        }
        if let Some(fb) = v.iter().position(|&c| c == b')') {                /* url(#x) fallback-colour */
            if let Some(col) = color(&v[fb + 1..]) { *out = col; *none = false; return true; }
        }
        *out = 0xFF80_8080; *none = false; return true;
    }
    if let Some(col) = color(v) { *out = col; *none = false; return true; }
    false
}
unsafe fn inherit(n: *mut Node, pa: &mut Paint) {
    let css_ = &(*n).css;
    if css_.set & ST_COLOR != 0 { pa.current = css_.color; }
    if let Some(v) = sprop(n, b"color", 64) { if let Some(c) = color(&v) { pa.current = c; } }
    if css_.set & ST_FILL != 0 {
        if css_.fill_cur != 0 { pa.fill = pa.current; pa.fill_none = false; } else { pa.fill = css_.fill; pa.fill_none = css_.fill >> 24 == 0; }
    }
    if let Some(v) = sprop(n, b"fill", 64) { let (mut f, mut nn) = (pa.fill, pa.fill_none); paint_value(n, &v, pa.current, &mut f, &mut nn); pa.fill = f; pa.fill_none = nn; }
    if let Some(v) = sprop(n, b"stroke", 64) { let (mut f, mut nn) = (pa.stroke, pa.stroke_none); paint_value(n, &v, pa.current, &mut f, &mut nn); pa.stroke = f; pa.stroke_none = nn; }
    if let Some(v) = sprop(n, b"stroke-width", 64) { let mut p = 0; pa.stroke_w = num(&v, &mut p); }
    if let Some(v) = sprop(n, b"fill-rule", 64) { pa.evenodd = iprefix(&v, b"evenodd"); }
    if let Some(v) = sprop(n, b"opacity", 64) { let mut p = 0; pa.opacity *= num(&v, &mut p); }
    if let Some(v) = sprop(n, b"fill-opacity", 64) { let mut p = 0; pa.fill_op = num(&v, &mut p); }
    if let Some(v) = sprop(n, b"stroke-opacity", 64) { let mut p = 0; pa.stroke_op = num(&v, &mut p); }
}
fn with_alpha(c: u32, mut a: f32) -> u32 {
    if a >= 0.999 { return c; }
    if a < 0.0 { a = 0.0; }
    let al = ftu((c >> 24) as f32 * a);
    (al << 24) | (c & 0xFF_FFFF)
}
unsafe fn draw_shape(cv: *mut Canvas, n: *mut Node, m: Mat, pa: &Paint, path: *mut RasterPath) {
    let mut b = Pb { p: path, m };
    raster_reset(path);
    let t = tag(n);
    if ieq(t, b"path") { let Some(d) = sattr(n, b"d") else { return }; b.path_d(d); }
    else if ieq(t, b"rect") {
        let (x, y, w, h) = (len_attr(n, b"x", 0.0), len_attr(n, b"y", 0.0), len_attr(n, b"width", 0.0), len_attr(n, b"height", 0.0));
        let (mut rx, mut ry) = (len_attr(n, b"rx", -1.0), len_attr(n, b"ry", -1.0));
        if rx < 0.0 { rx = ry; }
        if ry < 0.0 { ry = rx; }
        if rx < 0.0 { rx = 0.0; ry = 0.0; }
        rx = if rx > w / 2.0 { w / 2.0 } else { rx };
        ry = if ry > h / 2.0 { h / 2.0 } else { ry };
        if w <= 0.0 || h <= 0.0 { return; }
        if rx > 0.0 && ry > 0.0 {
            let k = 0.5523f32;
            b.mv(x + rx, y); b.line(x + w - rx, y);
            b.cubic(x + w - rx + rx * k, y, x + w, y + ry - ry * k, x + w, y + ry); b.line(x + w, y + h - ry);
            b.cubic(x + w, y + h - ry + ry * k, x + w - rx + rx * k, y + h, x + w - rx, y + h); b.line(x + rx, y + h);
            b.cubic(x + rx - rx * k, y + h, x, y + h - ry + ry * k, x, y + h - ry); b.line(x, y + ry);
            b.cubic(x, y + ry - ry * k, x + rx - rx * k, y, x + rx, y);
        } else { b.mv(x, y); b.line(x + w, y); b.line(x + w, y + h); b.line(x, y + h); }
        raster_close(path);
    } else if ieq(t, b"circle") || ieq(t, b"ellipse") {
        let (cx, cy) = (len_attr(n, b"cx", 0.0), len_attr(n, b"cy", 0.0));
        let (rx, ry) = if t[0] == b'c' || t[0] == b'C' { let r = len_attr(n, b"r", 0.0); (r, r) } else { (len_attr(n, b"rx", 0.0), len_attr(n, b"ry", 0.0)) };
        if rx <= 0.0 || ry <= 0.0 { return; }
        let k = 0.5523f32;
        b.mv(cx + rx, cy);
        b.cubic(cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
        b.cubic(cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
        b.cubic(cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
        b.cubic(cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
        raster_close(path);
    } else if ieq(t, b"line") {
        let (x1, y1) = (len_attr(n, b"x1", 0.0), len_attr(n, b"y1", 0.0));
        b.mv(x1, y1);
        let (x2, y2) = (len_attr(n, b"x2", 0.0), len_attr(n, b"y2", 0.0));
        b.line(x2, y2);
    } else if ieq(t, b"polyline") || ieq(t, b"polygon") {
        let Some(pts) = sattr(n, b"points") else { return };
        let mut p = 0;
        let mut first = true;
        while has_num(pts, p) {
            let x = num(pts, &mut p);
            if !has_num(pts, p) { break; }
            let y = num(pts, &mut p);
            if first { b.mv(x, y); } else { b.line(x, y); }
            first = false;
        }
        if at(t, 4) == b'g' || at(t, 4) == b'G' { raster_close(path); }
    } else { return; }
    let is_line = ieq(t, b"line") || ieq(t, b"polyline");
    if !pa.fill_none && !is_line { raster_fill(cv, path, with_alpha(pa.fill, pa.opacity * pa.fill_op), pa.evenodd as crate::CBool); }
    if !pa.stroke_none && pa.stroke_w > 0.0 {
        let sc = sqrt_(fabs_(m.a * m.d - m.b * m.c));
        let mut w = fti(pa.stroke_w * sc + 0.5);
        if w < 1 { w = 1; }
        raster_stroke(cv, path, w, with_alpha(pa.stroke, pa.opacity * pa.stroke_op));
    }
}
const SKIP: [&[u8]; 16] = [b"defs", b"lineargradient", b"radialgradient", b"clippath", b"mask", b"pattern", b"symbol", b"marker",
                           b"filter", b"title", b"desc", b"metadata", b"style", b"script", b"text", b"foreignobject"];
unsafe fn draw_node(cv: *mut Canvas, n: *mut Node, mut m: Mat, mut pa: Paint, path: *mut RasterPath, depth: i32) {
    if (*n).ty != 0 || depth > 24 { return; }
    let t = tag(n);
    if SKIP.iter().any(|s| ieq(t, s)) { return; }
    if (*n).css.set & ST_DISPLAY != 0 && (*n).css.display == 0 { return; }
    if let Some(disp) = sattr(n, b"display") { if ieq(disp, b"none") { return; } }
    inherit(n, &mut pa);
    if let Some(tr) = sattr(n, b"transform") { m = parse_transform(m, tr); }
    if ieq(t, b"g") || ieq(t, b"a") || (ieq(t, b"svg") && depth > 0) {
        if ieq(t, b"svg") { let (x, y) = (len_attr(n, b"x", 0.0), len_attr(n, b"y", 0.0)); m = mul(m, mat(1.0, 0.0, 0.0, 1.0, x, y)); }
        let mut c = (*n).first;
        while !c.is_null() { draw_node(cv, c, m, pa, path, depth + 1); c = (*c).next; }
        return;
    }
    if ieq(t, b"use") {
        let href = sattr(n, b"href").or_else(|| sattr(n, b"xlink:href"));
        let rf = match href { Some(h) if h.first() == Some(&b'#') => find_id(svg_root_of(n), &h[1..]), _ => null_mut() };
        if rf.is_null() || rf == n { return; }
        let (ux, uy) = (len_attr(n, b"x", 0.0), len_attr(n, b"y", 0.0));
        m = mul(m, mat(1.0, 0.0, 0.0, 1.0, ux, uy));
        if ieq(tag(rf), b"symbol") {                     /* a symbol: its children, scaled from its viewBox */
            let vb = sattr(rf, b"viewBox");
            let (w, h) = (len_attr(n, b"width", 0.0), len_attr(n, b"height", 0.0));
            if let Some(vb) = vb {
                if w > 0.0 && h > 0.0 {
                    let mut p = 0;
                    let (vx, vy, vw, vh) = (num(vb, &mut p), num(vb, &mut p), num(vb, &mut p), num(vb, &mut p));
                    if vw > 0.0 && vh > 0.0 { let s = if w / vw < h / vh { w / vw } else { h / vh }; m = mul(m, mat(s, 0.0, 0.0, s, -vx * s, -vy * s)); }
                }
            }
            inherit(rf, &mut pa);
            let mut c = (*rf).first;
            while !c.is_null() { draw_node(cv, c, m, pa, path, depth + 1); c = (*c).next; }
        } else { draw_node(cv, rf, m, pa, path, depth + 1); }
        return;
    }
    draw_shape(cv, n, m, &pa, path);
}

/* ---------------- the C interface (src/apps/svg.h) ---------------- */
/// Width and height, from the attributes or the viewBox.
#[no_mangle]
pub unsafe extern "C" fn svg_intrinsic(svg: *mut Node, w: *mut c_int, h: *mut c_int) {
    let mut aw = len_attr(svg, b"width", -1.0);
    let mut ah = len_attr(svg, b"height", -1.0);
    if let Some(wa) = sattr(svg, b"width") { if wa.contains(&b'%') { aw = -1.0; } }
    if let Some(ha) = sattr(svg, b"height") { if ha.contains(&b'%') { ah = -1.0; } }
    let (mut vw, mut vh) = (0f32, 0f32);
    if let Some(vb) = sattr(svg, b"viewBox") { let mut p = 0; num(vb, &mut p); num(vb, &mut p); vw = num(vb, &mut p); vh = num(vb, &mut p); }
    if aw <= 0.0 && ah <= 0.0 { aw = if vw > 0.0 { vw } else { 300.0 }; ah = if vh > 0.0 { vh } else { 150.0 }; }
    else if aw <= 0.0 { aw = if vw > 0.0 && vh > 0.0 { ah * vw / vh } else { ah }; }
    else if ah <= 0.0 { ah = if vw > 0.0 && vh > 0.0 { aw * vh / vw } else { aw }; }
    *w = fti(aw + 0.5); *h = fti(ah + 0.5);
    if *w < 1 { *w = 1; }
    if *h < 1 { *h = 1; }
}
static mut PATH: *mut RasterPath = null_mut();
#[no_mangle]
pub unsafe extern "C" fn svg_draw(cv: *mut Canvas, svg: *mut Node, x: c_int, y: c_int, w: c_int, h: c_int, current: u32) {
    if PATH.is_null() { PATH = raster_path_new(); }
    let path = PATH;
    if path.is_null() || w <= 0 || h <= 0 { return; }
    let mut m = mat(1.0, 0.0, 0.0, 1.0, x as f32, y as f32);
    if let Some(vb) = sattr(svg, b"viewBox") {
        let mut p = 0;
        let (vx, vy, vw, vh) = (num(vb, &mut p), num(vb, &mut p), num(vb, &mut p), num(vb, &mut p));
        if vw > 0.0 && vh > 0.0 {
            let par = sattr(svg, b"preserveAspectRatio");
            let (sx, sy) = (w as f32 / vw, h as f32 / vh);
            if par.is_some_and(|p| p.starts_with(b"none")) { m = mul(m, mat(sx, 0.0, 0.0, sy, -vx * sx, -vy * sy)); }
            else {
                let s = if sx < sy { sx } else { sy };
                m = mul(m, mat(s, 0.0, 0.0, s, -vx * s + (w as f32 - vw * s) / 2.0, -vy * s + (h as f32 - vh * s) / 2.0));
            }
        }
    }
    let mut pa = Paint { fill: BLACK, stroke: BLACK, current, fill_none: false, stroke_none: true, evenodd: false, stroke_w: 1.0, opacity: 1.0, fill_op: 1.0, stroke_op: 1.0 };
    let old = canvas_push_clip(cv, Rect { x, y, w, h });
    inherit(svg, &mut pa);
    let mut c = (*svg).first;
    while !c.is_null() { draw_node(cv, c, m, pa, path, 1); c = (*c).next; }
    (*cv).clip = old;
}
/// An .svg file as a picture: parsed as HTML (the parser builds SVG elements) and drawn once.
#[no_mangle]
pub unsafe extern "C" fn svg_decode(data: *const u8, len: u32) -> *mut Canvas {
    let d = html_parse(data, len, c"utf-8".as_ptr(), c"".as_ptr());
    if d.is_null() { return null_mut(); }
    let mut n = crate::html::doc_root(d);
    let mut svg: *mut Node = null_mut();
    while !n.is_null() && svg.is_null() {                /* the first <svg> in document order */
        if (*n).ty == 0 && ieq(tag(n), b"svg") { svg = n; break; }
        if !(*n).first.is_null() { n = (*n).first; continue; }
        while !n.is_null() && (*n).next.is_null() { n = (*n).parent; }
        if !n.is_null() { n = (*n).next; }
    }
    let mut c: *mut Canvas = null_mut();
    if !svg.is_null() {
        let (mut w, mut h) = (0, 0);
        svg_intrinsic(svg, &mut w, &mut h);
        if w > 2048 { h = h * 2048 / w; w = 2048; }
        if h > 2048 { w = w * 2048 / h; h = 2048; }
        c = canvas_new(w.max(1), h.max(1));
        if !c.is_null() { gfx_clear(c, 0); svg_draw(c, svg, 0, 0, w, h, BLACK); }
    }
    doc_free(d);
    c
}
#[no_mangle]
pub unsafe extern "C" fn svg_sniff(d: *const u8, n: u32) -> crate::CBool {
    let lim = n.min(1024);
    let mut i = 0u32;
    while i + 4 <= lim {
        let p = d.add(i as usize);
        if *p == b'<' && (*p.add(1) | 0x20) == b's' && (*p.add(2) | 0x20) == b'v' && (*p.add(3) | 0x20) == b'g' { return 1; }
        i += 1;
    }
    0
}
