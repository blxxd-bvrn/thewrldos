//! The WRLD Browser's own CSS and text parsing (was `src/apps/html.c`): CSS declarations
//! (colours, gradients, lengths, the box model, positioning, flexbox and grid, `calc()`,
//! `min()`/`max()`/`clamp()`, `var()`), `@media` queries, splitting a style sheet into its
//! rules, custom properties, and the character conversions the tree builder needs.
//! Selectors and HTML tokenising are Lexbor's (`src/lexbor/lexglue.c`); the tree and the
//! cascade around them are in `crate::html`.
//!
//! Strings here are byte slices that stand for C strings: a value ends at its first NUL, and
//! reading past the end reads NUL, as in the C code this replaces, whose results it keeps
//! (the kernel's own `atoi`, `strtoul` and `strtok`, int wrap-around and all).

use alloc::vec::Vec;

// ---------------------------------------------------------------- the style record

/// `css_t` (src/apps/browser.h). `bool` is the kernel's `int`.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Css {
    pub set: u32, pub color: u32, pub bg: u32, pub display: i8, pub align: i8, pub bold: i32, pub italic: i32, pub under: i32, pub size: i8,
    pub radius: i16, pub bg2: u32, pub grad: i8, pub grad_dir: i8, pub border: i16, pub border_color: u32,
    pub m: [i16; 4], pub pad: [i16; 4],
    pub mset: u8, pub pset: u8, pub mauto: u8,
    pub width: i16, pub maxw: i16,
    pub wpct: i32, pub mpct: i32,
    pub fset: u16,
    pub fdir: i8, pub fwrap: i8, pub fjust: i8, pub falign: i8, pub aself: i8, pub grow: i8, pub shrink: i8,
    pub gap: i16, pub rgap: i16, pub basis: i16,
    pub gcols: i16, pub gmin: i16,
    pub bpct: i32,
    pub height: i16, pub minh: i16,
    pub pos: i8, pub flt: i8, pub clr: i8,
    pub oset: u8, pub opct: u8,
    pub off: [i16; 4],
    pub fill: u32, pub fill_cur: i32,
    pub bsz: i8,
}

pub const ST_COLOR: u32 = 0x001;
pub const ST_BG: u32 = 0x002;
pub const ST_DISPLAY: u32 = 0x004;
pub const ST_ALIGN: u32 = 0x008;
pub const ST_BOLD: u32 = 0x010;
pub const ST_ITALIC: u32 = 0x020;
pub const ST_UNDER: u32 = 0x040;
pub const ST_SIZE: u32 = 0x080;
pub const ST_RADIUS: u32 = 0x100;
pub const ST_GRAD: u32 = 0x200;
pub const ST_BORDER: u32 = 0x400;
pub const ST_MARGIN: u32 = 0x800;
pub const ST_PADDING: u32 = 0x1000;
pub const ST_WIDTH: u32 = 0x2000;
pub const ST_MAXW: u32 = 0x4000;
pub const ST_BCOLOR: u32 = 0x8000;
pub const ST_FLEX: u32 = 0x10000;
pub const ST_HEIGHT: u32 = 0x20000;
pub const ST_POS: u32 = 0x40000;
pub const ST_FLOAT: u32 = 0x80000;
pub const ST_CLEAR: u32 = 0x100000;
pub const ST_FILL: u32 = 0x200000;
pub const F_DIR: u16 = 0x01;
pub const F_WRAP: u16 = 0x02;
pub const F_JUST: u16 = 0x04;
pub const F_ALIGN: u16 = 0x08;
pub const F_GAP: u16 = 0x10;
pub const F_GROW: u16 = 0x20;
pub const F_SHRINK: u16 = 0x40;
pub const F_BASIS: u16 = 0x80;
pub const F_GCOLS: u16 = 0x100;
pub const F_RGAP: u16 = 0x200;
pub const F_ASELF: u16 = 0x400;
const DISP_NONE: i8 = 0; const DISP_BLOCK: i8 = 1; const DISP_INLINE: i8 = 2; const DISP_FLEX: i8 = 3; const DISP_GRID: i8 = 4; const DISP_IBLOCK: i8 = 5;
const POS_STATIC: i8 = 0; const POS_RELATIVE: i8 = 1; const POS_ABSOLUTE: i8 = 2; const POS_FIXED: i8 = 3; const POS_STICKY: i8 = 4;
const FJ_START: i8 = 0; const FJ_CENTER: i8 = 1; const FJ_END: i8 = 2; const FJ_BETWEEN: i8 = 3; const FJ_AROUND: i8 = 4; const FJ_EVENLY: i8 = 5;
const FA_STRETCH: i8 = 0; const FA_START: i8 = 1; const FA_CENTER: i8 = 2; const FA_END: i8 = 3;
const BLACK: u32 = 0xFF00_0000;
const fn rgb(r: i32, g: i32, b: i32) -> u32 { 0xFF00_0000 | (r as u32) << 16 | (g as u32) << 8 | b as u32 }

/// Custom properties: the value `var(--name)` reads.
pub trait Vars { fn get(&self, name: &[u8]) -> Option<&[u8]>; }
pub struct NoVars;
impl Vars for NoVars { fn get(&self, _: &[u8]) -> Option<&[u8]> { None } }

// ---------------------------------------------------------------- C string helpers

/// The C string in s: up to its first NUL.
pub fn cstr(s: &[u8]) -> &[u8] { match s.iter().position(|&c| c == 0) { Some(i) => &s[..i], None => s } }
#[inline] fn at(s: &[u8], i: usize) -> u8 { if i < s.len() { s[i] } else { 0 } }
#[inline] pub fn isspace(c: u8) -> bool { matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) }
#[inline] fn isdigit(c: u8) -> bool { c.is_ascii_digit() }
#[inline] fn isalpha(c: u8) -> bool { c.is_ascii_alphabetic() }
#[inline] pub fn isalnum(c: u8) -> bool { c.is_ascii_alphanumeric() }
#[inline] fn isxdigit(c: u8) -> bool { c.is_ascii_hexdigit() }
#[inline] fn lower(c: u8) -> u8 { c.to_ascii_lowercase() }
/// strncmp(s + i, p, strlen(p)) == 0
#[inline] fn sw(s: &[u8], i: usize, p: &[u8]) -> bool { s.len() >= i + p.len() && &s[i..i + p.len()] == p }
/// strncasecmp(s + i, p, strlen(p)) == 0
#[inline] fn swci(s: &[u8], i: usize, p: &[u8]) -> bool { s.len() >= i + p.len() && s[i..i + p.len()].eq_ignore_ascii_case(p) }
/// strstr(s + from, p)
fn find(s: &[u8], from: usize, p: &[u8]) -> Option<usize> {
    if p.is_empty() { return Some(from.min(s.len())); }
    (from..s.len()).find(|&i| sw(s, i, p))
}
fn has(s: &[u8], p: &[u8]) -> bool { find(s, 0, p).is_some() }
/// strchr(s + from, c) for c != 0
fn chr(s: &[u8], from: usize, c: u8) -> Option<usize> { (from..s.len()).find(|&i| s[i] == c) }
/// strpbrk(s + from, set)
fn pbrk(s: &[u8], from: usize, set: &[u8]) -> Option<usize> { (from..s.len()).find(|&i| set.contains(&s[i])) }
/// strcasestr
pub fn has_ci(h: &[u8], n: &[u8]) -> bool { (0..h.len()).any(|i| swci(h, i, n)) }
/// The kernel's strtoul (src/kernel/lib.c): the low 32 bits, and where it stopped.
pub fn strtoul(s: &[u8], mut i: usize, mut base: u32) -> (u32, usize) {
    let mut v: u32 = 0;
    while isspace(at(s, i)) { i += 1; }
    if at(s, i) == b'+' { i += 1; }
    if (base == 0 || base == 16) && at(s, i) == b'0' && (at(s, i + 1) == b'x' || at(s, i + 1) == b'X') { i += 2; base = 16; }
    else if base == 0 && at(s, i) == b'0' && at(s, i + 1) != 0 { base = 8; }
    else if base == 0 { base = 10; }
    loop {
        let c = at(s, i);
        let d = if isdigit(c) { (c - b'0') as u32 } else if c.is_ascii_lowercase() { (c - b'a') as u32 + 10 } else if c.is_ascii_uppercase() { (c - b'A') as u32 + 10 } else { break };
        if d >= base { break; }
        v = v.wrapping_mul(base).wrapping_add(d);
        i += 1;
    }
    (v, i)
}
/// The kernel's atoi: strtol(s, NULL, 10) truncated to int.
pub fn atoi(s: &[u8], mut i: usize) -> i32 {
    while isspace(at(s, i)) { i += 1; }
    let neg = at(s, i) == b'-';
    if neg { i += 1; }
    let v = strtoul(s, i, 10).0 as i32;
    if neg { v.wrapping_neg() } else { v }
}
fn clamp(v: i32, lo: i32, hi: i32) -> i32 { if v < lo { lo } else if v > hi { hi } else { v } }
/// snprintf(buf, 32, "%d<suffix>")
fn fmt_int(v: i32, suffix: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut u = (v as i64).unsigned_abs();
    let mut d = [0u8; 12]; let mut n = 0;
    loop { d[n] = b'0' + (u % 10) as u8; n += 1; u /= 10; if u == 0 { break; } }
    if v < 0 { out.push(b'-'); }
    for k in (0..n).rev() { out.push(d[k]); }
    out.extend_from_slice(suffix);
    out.truncate(31);
    out
}

// ---------------------------------------------------------------- characters

const CP1252: [u16; 32] = [0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178];

fn put_utf8(cp: u32, out: &mut impl FnMut(u8)) {
    if cp < 0x80 { out(cp as u8); }
    else if cp < 0x800 { out((0xC0 | (cp >> 6)) as u8); out((0x80 | (cp & 0x3F)) as u8); }
    else if cp < 0x10000 { out((0xE0 | (cp >> 12)) as u8); out((0x80 | ((cp >> 6) & 0x3F)) as u8); out((0x80 | (cp & 0x3F)) as u8); }
    else { out((0xF0 | (cp >> 18)) as u8); out((0x80 | ((cp >> 12) & 0x3F)) as u8); out((0x80 | ((cp >> 6) & 0x3F)) as u8); out((0x80 | (cp & 0x3F)) as u8); }
}

/// Validates and normalises UTF-8: stray Windows-1252 bytes become the characters they stand
/// for, and NUL, CR and invisible joiners are dropped. At most 3 bytes out per byte in.
pub fn utf8_normalise(s: &[u8], mut out: impl FnMut(u8)) {
    let n = s.len();
    let mut i = 0;
    while i < n {
        let c = s[i];
        let (cp, len): (u32, usize);
        if c < 0x80 { cp = c as u32; len = 1; }
        else if c & 0xE0 == 0xC0 && i + 1 < n && s[i + 1] & 0xC0 == 0x80 { cp = ((c as u32 & 0x1F) << 6) | (s[i + 1] as u32 & 0x3F); len = 2; }
        else if c & 0xF0 == 0xE0 && i + 2 < n && s[i + 1] & 0xC0 == 0x80 && s[i + 2] & 0xC0 == 0x80 {
            cp = ((c as u32 & 0x0F) << 12) | ((s[i + 1] as u32 & 0x3F) << 6) | (s[i + 2] as u32 & 0x3F); len = 3;
        } else if c & 0xF8 == 0xF0 && i + 3 < n {
            cp = ((c as u32 & 0x07) << 18) | ((s[i + 1] as u32 & 0x3F) << 12) | ((s[i + 2] as u32 & 0x3F) << 6) | (s[i + 3] as u32 & 0x3F); len = 4;
        } else {
            cp = if (0x80..0xA0).contains(&c) { let w = CP1252[(c - 0x80) as usize]; if w != 0 { w as u32 } else { b'?' as u32 } } else { c as u32 };
            len = 1;
        }
        i += len;
        if cp == 0 || cp == b'\r' as u32 || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0xFEFF { continue; }
        put_utf8(cp, &mut out);
    }
}
/// Windows-1252 / Latin-1 bytes to UTF-8 (at most 3 bytes per byte).
pub fn utf8_from_1252(s: &[u8], mut out: impl FnMut(u8)) {
    for &b in s {
        let mut cp = b as u32;
        if (0x80..0xA0).contains(&cp) { let w = CP1252[(cp - 0x80) as usize]; cp = if w != 0 { w as u32 } else { b'?' as u32 }; }
        put_utf8(cp, &mut out);
    }
}
/// Is the document UTF-8? The charset from the HTTP header, else a `charset=` in the first
/// 4 KB, else yes.
pub fn is_utf8(src: &[u8], header: Option<&[u8]>) -> bool {
    let mut cs: Vec<u8> = Vec::new();
    if let Some(h) = header { let h = cstr(h); cs.extend_from_slice(&h[..h.len().min(31)]); }
    if cs.is_empty() {
        let lim = src.len().min(4096);
        let mut m = None;
        let mut i = 0;
        while i + 8 < lim { if swci(src, i, b"charset=") { m = Some(i + 8); break; } i += 1; }
        if let Some(mut m) = m {
            if at(src, m) == b'"' || at(src, m) == b'\'' { m += 1; }
            let mut k = 0;
            while k < 31 { let c = at(src, m + k); if !(isalnum(c) || c == b'-' || c == b'_') { break; } cs.push(c); k += 1; }
        }
    }
    !(!cs.is_empty() && (swci(&cs, 0, b"iso-8859") || swci(&cs, 0, b"windows-125") || cs.eq_ignore_ascii_case(b"latin1") || cs.eq_ignore_ascii_case(b"us-ascii")))
}
/// `<meta http-equiv=refresh content="5; url=...">`: the delay and the address (at most 1023 bytes).
pub fn meta_refresh(ct: &[u8]) -> (i32, Option<&[u8]>) {
    let ct = cstr(ct);
    let secs = atoi(ct, 0);
    let Some(u) = (0..ct.len()).find(|&i| swci(ct, i, b"url=")) else { return (secs, None) };
    let mut u = u + 4;
    while matches!(at(ct, u), b' ' | b'\'' | b'"') { u += 1; }
    let mut r = &ct[u..];
    r = &r[..r.len().min(1023)];
    if let Some(q) = pbrk(r, 0, b"'\"") { r = &r[..q]; }
    (secs, Some(r))
}
/// A `<title>`'s text: whitespace characters become spaces, then the ends are trimmed; at
/// most 159 bytes.
pub fn title(text: &[u8]) -> Vec<u8> {
    let mut t: Vec<u8> = cstr(text).iter().map(|&c| if c == b'\n' || c == b'\t' || c == b'\r' { b' ' } else { c }).collect();
    let lead = t.iter().take_while(|&&c| c == b' ').count();
    t.drain(..lead);
    t.truncate(159);
    while t.last() == Some(&b' ') { t.pop(); }
    t
}
pub fn is_void(t: &[u8]) -> bool {
    const V: [&[u8]; 15] = [b"br", b"img", b"hr", b"input", b"meta", b"link", b"area", b"base", b"col", b"embed", b"source", b"wbr", b"param", b"track", b"keygen"];
    V.contains(&t)
}

// ---------------------------------------------------------------- colours

const NAMED: &[(&str, u32)] = &[
    ("black", 0x000000), ("white", 0xFFFFFF), ("red", 0xFF0000), ("green", 0x008000), ("blue", 0x0000FF), ("yellow", 0xFFFF00),
    ("gray", 0x808080), ("grey", 0x808080), ("silver", 0xC0C0C0), ("maroon", 0x800000), ("navy", 0x000080), ("purple", 0x800080),
    ("olive", 0x808000), ("teal", 0x008080), ("lime", 0x00FF00), ("aqua", 0x00FFFF), ("fuchsia", 0xFF00FF), ("orange", 0xFFA500),
    ("darkgray", 0xA9A9A9), ("darkgrey", 0xA9A9A9), ("lightgray", 0xD3D3D3), ("lightgrey", 0xD3D3D3), ("darkblue", 0x00008B), ("darkred", 0x8B0000),
    ("darkgreen", 0x006400), ("brown", 0xA52A2A), ("pink", 0xFFC0CB), ("gold", 0xFFD700), ("beige", 0xF5F5DC), ("ivory", 0xFFFFF0),
    ("whitesmoke", 0xF5F5F5), ("gainsboro", 0xDCDCDC), ("lightblue", 0xADD8E6), ("skyblue", 0x87CEEB), ("steelblue", 0x4682B4),
    ("royalblue", 0x4169E1), ("dimgray", 0x696969), ("dimgrey", 0x696969), ("crimson", 0xDC143C), ("orangered", 0xFF4500),
    ("lightyellow", 0xFFFFE0), ("lightgreen", 0x90EE90), ("indigo", 0x4B0082), ("violet", 0xEE82EE), ("tan", 0xD2B48C), ("khaki", 0xF0E68C),
    ("salmon", 0xFA8072), ("coral", 0xFF7F50), ("tomato", 0xFF6347), ("slategray", 0x708090), ("darkslategray", 0x2F4F4F), ("cornflowerblue", 0x6495ED),
    ("aliceblue", 0xF0F8FF), ("ghostwhite", 0xF8F8FF), ("snow", 0xFFFAFA), ("linen", 0xFAF0E6), ("wheat", 0xF5DEB3), ("midnightblue", 0x191970),
];

/// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()`, a colour name, or a bare
/// `rrggbb` (old `bgcolor=` attributes). Alpha is dropped.
pub fn parse_color(s: &[u8]) -> Option<u32> {
    let s = cstr(s);
    let mut i = 0;
    while at(s, i) == b' ' { i += 1; }
    if at(s, i) == b'#' {
        i += 1;
        let mut n = 0; while isxdigit(at(s, i + n)) { n += 1; }
        let mut v = strtoul(s, i, 16).0;
        if n == 3 || n == 4 { if n == 4 { v >>= 4; } v = ((v >> 8 & 15) * 0x110000) | ((v >> 4 & 15) * 0x1100) | ((v & 15) * 0x11); }
        else if n == 8 { v >>= 8; }
        else if n != 6 { return None; }
        return Some(0xFF00_0000 | v);
    }
    if swci(s, i, b"rgb") {
        let mut p = chr(s, i, b'(')?;
        let mut v = [0i32; 3];
        for k in 0..3 {
            p += 1;
            while at(s, p) == b' ' { p += 1; }
            v[k] = atoi(s, p);
            if let (Some(pc), Some(e)) = (chr(s, p, b'%'), pbrk(s, p, b",)")) { if pc < e { v[k] = v[k].wrapping_mul(255) / 100; } }
            match pbrk(s, p, b",) ") { Some(q) => p = q, None => break }
            while at(s, p) == b' ' { p += 1; }
            if at(s, p) != b',' && k < 2 && at(s, p) == b')' { break; }
        }
        return Some(rgb(clamp(v[0], 0, 255), clamp(v[1], 0, 255), clamp(v[2], 0, 255)));
    }
    let mut w = [0u8; 23]; let mut k = 0;
    while isalpha(at(s, i)) && k < 23 { w[k] = lower(s[i]); k += 1; i += 1; }
    let w = &w[..k];
    for (n, c) in NAMED.iter() { if n.as_bytes() == w { return Some(0xFF00_0000 | c); } }
    if k != 0 && isxdigit(w[0]) && k == 6 { return Some(0xFF00_0000 | strtoul(w, 0, 16).0); }
    None
}

// ---------------------------------------------------------------- values

/// The end of one comma-separated argument in s[p..end] (commas inside parentheses do not count).
fn token_end(s: &[u8], mut p: usize, end: usize) -> usize {
    let mut depth = 0;
    while p < end {
        match s[p] { b'(' => depth += 1, b')' => { if depth != 0 { depth -= 1; } } b',' if depth == 0 => break, _ => {} }
        p += 1;
    }
    p
}

/// `linear-gradient(to right, #123456, #abcdef)` and `radial-gradient(...)`: the two-colour
/// form a page uses for banners and buttons.
fn parse_gradient(s: &[u8], c: &mut Css) -> bool {
    let mut i = 0;
    while at(s, i) == b' ' { i += 1; }
    let kind = if swci(s, i, b"linear-gradient") { 1 } else if swci(s, i, b"radial-gradient") { 2 } else { 0 };
    if kind == 0 { return false; }
    let Some(p0) = chr(s, i, b'(') else { return false };
    let mut p = p0 + 1;
    let end = (p..s.len()).rev().find(|&k| s[k] == b')').unwrap_or(s.len());
    let mut t = token_end(s, p, end);
    let tok = |p: usize, t: usize| -> Vec<u8> { s[p..p + (t - p).min(95)].to_vec() };
    let mut tk: Vec<u8> = tok(p, t).iter().map(|&b| lower(b)).collect();
    while tk.last() == Some(&b' ') { tk.pop(); }
    let mut dir = 0;
    let first = match parse_color(&tk) {
        Some(f) => f,
        None => {
            if has(&tk, b"right") || has(&tk, b"left") || has(&tk, b"90deg") || has(&tk, b"270deg") { dir = 1; }
            else if has(&tk, b"diagonal") || has(&tk, b"45deg") || has(&tk, b"135deg") { dir = 2; }
            p = t;
            if p < end && s[p] == b',' { p += 1; }
            t = token_end(s, p, end);
            match parse_color(&tok(p, t)) { Some(f) => f, None => return false }
        }
    };
    p = t;
    if p < end && s[p] == b',' { p += 1; }
    t = token_end(s, p, end);
    let Some(second) = parse_color(&tok(p, t)) else { return false };
    c.bg = first; c.bg2 = second;
    c.grad = kind; c.grad_dir = dir;
    c.set |= ST_BG | ST_GRAD;
    true
}

/// A CSS length in px: px, em/rem (16 px), pt, or a plain number; (px, percent or vw, auto).
fn css_len(s: &[u8], mut i: usize) -> (i32, bool, bool) {
    while at(s, i) == b' ' { i += 1; }
    let is_auto = sw(s, i, b"auto");
    let mut pct = false;
    let neg = at(s, i) == b'-';
    if neg || at(s, i) == b'+' { i += 1; }
    let (mut whole, mut frac, mut fd) = (0i32, 0i32, 0);
    while isdigit(at(s, i)) { whole = whole.wrapping_mul(10).wrapping_add((s[i] - b'0') as i32); i += 1; }
    if at(s, i) == b'.' {
        i += 1;
        while isdigit(at(s, i)) { if fd < 2 { frac = frac * 10 + (s[i] - b'0') as i32; fd += 1; } i += 1; }
    }
    while fd < 2 { frac *= 10; fd += 1; }
    let x100 = whole.wrapping_mul(100).wrapping_add(frac);
    let mut px = if sw(s, i, b"em") || sw(s, i, b"rem") { x100.wrapping_mul(16) / 100 }
        else if sw(s, i, b"pt") { x100.wrapping_mul(4) / 300 }
        else if at(s, i) == b'%' { pct = true; x100 / 100 }
        else if sw(s, i, b"vw") { pct = true; x100 / 100 }
        else { x100 / 100 };
    px = clamp(px, 0, 8000);
    (if neg { -px } else { px }, pct, is_auto)
}

/// margin / padding with one to four values
fn css_sides(v: &[u8], out: &mut [i16; 4], set: &mut u8, autos: Option<&mut u8>) {
    let (mut vals, mut aut, mut k) = ([0i32; 4], [false; 4], 0);
    let mut p = 0;
    while at(v, p) != 0 && k < 4 {
        while at(v, p) == b' ' || at(v, p) == b'\t' { p += 1; }
        if at(v, p) == 0 || at(v, p) == b'!' { break; }
        let (px, _, a) = css_len(v, p);
        vals[k] = px; aut[k] = a; k += 1;
        while at(v, p) != 0 && at(v, p) != b' ' && at(v, p) != b'\t' { p += 1; }
    }
    if k == 0 { return; }
    let idx = [0, if k > 1 { 1 } else { 0 }, if k > 2 { 2 } else { 0 }, if k > 3 { 3 } else if k > 1 { 1 } else { 0 }];
    let mut autos = autos;
    for i in 0..4 {
        out[i] = vals[idx[i]] as i16;
        if let Some(a) = autos.as_deref_mut() { if aut[idx[i]] { *a |= 1 << i; } else { *a &= !(1u8 << i); } }
    }
    *set = 0xF;
}

/// v with every var(--name[, fallback]) replaced (at most cap - 1 bytes); false if a
/// variable is unknown and has no fallback.
fn subst_vars(v: &[u8], cap: usize, depth: i32, vars: &dyn Vars) -> (Vec<u8>, bool) {
    let mut out = Vec::new();
    let mut ok = true;
    let mut i = 0;
    while i < v.len() && out.len() < cap - 1 {
        if !sw(v, i, b"var(") { out.push(v[i]); i += 1; continue; }
        let mut p = i + 4;
        while at(v, p) == b' ' { p += 1; }
        let mut ne = p;
        while ne < v.len() && !matches!(v[ne], b',' | b')' | b' ') { ne += 1; }
        let (mut q, mut fb, mut dep) = (ne, None, 1);
        while q < v.len() && dep != 0 {
            match v[q] {
                b'(' => dep += 1,
                b')' => { dep -= 1; if dep == 0 { break; } }
                b',' if dep == 1 && fb.is_none() => fb = Some(q + 1),
                _ => {}
            }
            q += 1;
        }
        let val = vars.get(&v[p..ne]);
        let tmp: Vec<u8> = match (val, fb) {
            (Some(val), _) if depth < 8 => { let (t, o) = subst_vars(val, 512, depth + 1, vars); if !o { ok = false; } t }
            (_, Some(f)) => { let fl = (q - f).min(511); let (t, o) = subst_vars(&v[f..f + fl], 512, depth + 1, vars); if !o { ok = false; } t }
            _ => { ok = false; Vec::new() }
        };
        let lead = tmp.iter().take_while(|&&c| c == b' ').count();
        for &c in &tmp[lead..] { if out.len() >= cap - 1 { break; } out.push(c); }
        i = if q < v.len() { q + 1 } else { q };
    }
    (out, ok)
}

/// calc(): the common forms, a length plus or minus a length, or a length times a number
fn css_calc(v: &[u8], pct_base: i32) -> i32 {
    let Some(p0) = chr(v, 0, b'(') else { return 0 };
    let mut p = p0 + 1;
    let (mut acc, mut sign, mut first) = (0i32, 1i32, true);
    while at(v, p) != 0 && at(v, p) != b')' {
        while at(v, p) == b' ' { p += 1; }
        if at(v, p) == b'+' && at(v, p + 1) == b' ' { sign = 1; p += 1; continue; }
        if at(v, p) == b'-' && at(v, p + 1) == b' ' { sign = -1; p += 1; continue; }
        if at(v, p) == b'*' || at(v, p) == b'/' {
            let op = v[p]; p += 1;
            while at(v, p) == b' ' { p += 1; }
            let whole = atoi(v, p);
            let mut frac = 0;
            if let Some(dot) = chr(v, p, b'.') { if dot < p + 8 && isdigit(at(v, dot + 1)) { frac = (v[dot + 1] - b'0') as i32; } }
            let x10 = whole.wrapping_mul(10).wrapping_add(frac);
            if op == b'*' { acc = acc.wrapping_mul(x10) / 10; } else if x10 != 0 { acc = acc.wrapping_mul(10).wrapping_div(x10); }
            while at(v, p) != 0 && at(v, p) != b' ' && at(v, p) != b')' { p += 1; }
            continue;
        }
        let (mut whole, mut frac, mut fd) = (0i32, 0i32, 0);
        let neg = at(v, p) == b'-';
        if neg { p += 1; }
        while isdigit(at(v, p)) { whole = whole.wrapping_mul(10).wrapping_add((v[p] - b'0') as i32); p += 1; }
        if at(v, p) == b'.' { p += 1; while isdigit(at(v, p)) { if fd < 2 { frac = frac * 10 + (v[p] - b'0') as i32; fd += 1; } p += 1; } }
        while fd < 2 { frac *= 10; fd += 1; }
        let x100 = whole.wrapping_mul(100).wrapping_add(frac);
        let mut px = if sw(v, p, b"rem") || sw(v, p, b"em") { x100.wrapping_mul(16) / 100 }
            else if at(v, p) == b'%' || sw(v, p, b"vw") { pct_base.wrapping_mul(x100) / 10000 }
            else { x100 / 100 };
        if neg { px = px.wrapping_neg(); }
        acc = if first { px } else { acc.wrapping_add(sign.wrapping_mul(px)) };
        first = false; sign = 1;
        while at(v, p) != 0 && at(v, p) != b' ' && at(v, p) != b')' { p += 1; }
    }
    acc
}
fn parse_justify(v: &[u8]) -> i8 {
    if sw(v, 0, b"center") { FJ_CENTER } else if sw(v, 0, b"flex-end") || sw(v, 0, b"end") || sw(v, 0, b"right") { FJ_END }
    else if sw(v, 0, b"space-between") { FJ_BETWEEN } else if sw(v, 0, b"space-around") { FJ_AROUND } else if sw(v, 0, b"space-evenly") { FJ_EVENLY } else { FJ_START }
}
fn parse_align(v: &[u8]) -> i8 {
    if sw(v, 0, b"center") { FA_CENTER } else if sw(v, 0, b"flex-end") || sw(v, 0, b"end") || sw(v, 0, b"self-end") { FA_END }
    else if sw(v, 0, b"flex-start") || sw(v, 0, b"start") || sw(v, 0, b"self-start") || sw(v, 0, b"baseline") { FA_START } else { FA_STRETCH }
}
/// grid-template-columns: a count of tracks, or repeat(auto-fill|auto-fit, minmax(N, 1fr))
fn parse_grid_cols(v: &[u8], c: &mut Css) {
    c.gcols = 0; c.gmin = 0;
    if sw(v, 0, b"repeat(") {
        let mut p = 7;
        while at(v, p) == b' ' { p += 1; }
        if sw(v, p, b"auto-fill") || sw(v, p, b"auto-fit") {
            let len = match find(v, p, b"minmax(") { Some(m) => Some(m + 7), None => chr(v, p, b',').map(|x| x + 1) };
            if let Some(mut l) = len { while at(v, l) == b' ' { l += 1; } c.gmin = clamp(css_len(v, l).0, 20, 2000) as i16; }
            if c.gmin == 0 { c.gmin = 200; }
        } else { c.gcols = clamp(atoi(v, p), 1, 24) as i16; }
    } else {
        let (mut n, mut dep, mut inw) = (0i32, 0i32, false);
        let mut p = 0;
        while at(v, p) != 0 && at(v, p) != b'!' {
            match v[p] {
                b'(' => dep += 1,
                b')' => dep -= 1,
                b'[' => {                                   // [line-names]
                    while at(v, p) != 0 && at(v, p) != b']' { p += 1; }
                    if at(v, p) == 0 { break; }
                    p += 1; continue;
                }
                _ => {}
            }
            let sp = v[p] == b' ' || v[p] == b'\t';
            if !sp && !inw && dep == 0 { n += 1; inw = true; } else if sp && dep == 0 { inw = false; }
            p += 1;
        }
        c.gcols = clamp(n, 1, 24) as i16;
    }
    c.set |= ST_FLEX; c.fset |= F_GCOLS;
}

/// The declarations of one rule body or style="" attribute (at most 8191 bytes).
pub fn decls(s: &[u8], c: &mut Css, vars: &dyn Vars) {
    let s = cstr(&s[..s.len().min(8191)]);
    let mut pos = 0;
    while pos < s.len() {
        // strtok(buf, ";")
        while pos < s.len() && s[pos] == b';' { pos += 1; }
        if pos >= s.len() { break; }
        let st = pos;
        while pos < s.len() && s[pos] != b';' { pos += 1; }
        let d = &s[st..pos];
        if pos < s.len() { pos += 1; }
        let Some(colon) = chr(d, 0, b':') else { continue };
        let mut k = &d[..colon];
        let mut v = &d[colon + 1..];
        while let [b' ' | b'\n' | b'\t' | b'\r', rest @ ..] = k { k = rest; }
        while let [rest @ .., b' ' | b'\n' | b'\t'] = k { k = rest; }
        while let [b' ' | b'\t', rest @ ..] = v { v = rest; }
        let kl: Vec<u8> = k.iter().map(|&b| lower(b)).collect();
        let k = &kl[..];
        if at(k, 0) == b'-' && at(k, 1) == b'-' { continue; }
        let vb;
        if has(v, b"var(") { let (o, ok) = subst_vars(v, 1024, 0, vars); if !ok { continue; } vb = o; v = &vb; }
        let cb;
        if sw(v, 0, b"calc(") || sw(v, 0, b"min(") || sw(v, 0, b"max(") || sw(v, 0, b"clamp(") {
            let mut b = if v[0] == b'c' && v[1] == b'a' { fmt_int(css_calc(v, 800), b"px") }
                else if v[1] == b'l' { fmt_int(match chr(v, 0, b',') { Some(a) => css_len(v, a + 1).0, None => 0 }, b"px") }
                else { fmt_int(css_len(v, chr(v, 0, b'(').unwrap_or(0) + 1).0, b"px") };
            if chr(v, 0, b'%').is_some() && !has(v, b"px") && !has(v, b"em") && v[0] != b'c' { b = fmt_int(atoi(v, chr(v, 0, b'(').unwrap_or(0) + 1), b"%"); }
            cb = b; v = &cb;
        }
        let len = |v: &[u8]| css_len(v, 0);
        match k {
            b"color" => { if let Some(x) = parse_color(v) { c.color = x; c.set |= ST_COLOR; } }
            b"background" | b"background-image" => {
                if !parse_gradient(v, c) && k == b"background" { if let Some(x) = parse_color(v) { c.bg = x; c.set |= ST_BG; } }
            }
            b"background-color" => { if let Some(x) = parse_color(v) { c.bg = x; c.set |= ST_BG; } }
            b"border-radius" => { c.set |= ST_RADIUS; c.radius = clamp(atoi(v, 0), 0, 512) as i16; }
            b"border" => {
                c.set |= ST_BORDER;
                c.border = if sw(v, 0, b"none") || (at(v, 0) == b'0' && (at(v, 1) == b' ' || at(v, 1) == 0)) { 0 } else { clamp(len(v).0, 0, 32) as i16 };
                if c.border == 0 && !sw(v, 0, b"none") && !sw(v, 0, b"0") && (has(v, b"solid") || has(v, b"dashed") || has(v, b"dotted")) { c.border = 3; }
                c.border_color = BLACK;
                let mut w = 0;
                while at(v, w) != 0 {
                    while at(v, w) == b' ' { w += 1; }
                    let mut e = w; let mut depth = 0i32;
                    while at(v, e) != 0 && (depth != 0 || v[e] != b' ') { if v[e] == b'(' { depth += 1; } else if v[e] == b')' { depth -= 1; } e += 1; }
                    let word = &v[w..w + (e - w).min(47)];
                    const STYLES: [&[u8]; 10] = [b"solid", b"dashed", b"dotted", b"double", b"groove", b"ridge", b"inset", b"outset", b"none", b"hidden"];
                    let kw = isdigit(at(word, 0)) || at(word, 0) == b'.' || STYLES.iter().any(|s| word.eq_ignore_ascii_case(s));
                    if !word.is_empty() && !kw { if let Some(bc) = parse_color(word) { c.border_color = bc; break; } }
                    w = e;
                }
            }
            b"margin" => { css_sides(v, &mut c.m, &mut c.mset, Some(&mut c.mauto)); c.set |= ST_MARGIN; }
            b"padding" => { css_sides(v, &mut c.pad, &mut c.pset, None); c.set |= ST_PADDING; }
            _ if sw(k, 0, b"margin-") || sw(k, 0, b"padding-") => {
                let marg = k[0] == b'm';
                let side = &k[if marg { 7 } else { 8 }..];
                let i = match side { b"top" => 0, b"right" => 1, b"bottom" => 2, b"left" => 3, _ => 4 };
                if i < 4 {
                    let (px, _, a) = len(v);
                    if marg { c.m[i] = px as i16; c.mset |= 1 << i; if a { c.mauto |= 1 << i; } else { c.mauto &= !(1u8 << i); } c.set |= ST_MARGIN; }
                    else { c.pad[i] = px.max(0) as i16; c.pset |= 1 << i; c.set |= ST_PADDING; }
                }
            }
            b"width" => { let (px, pc, a) = len(v); if !a && px > 0 { c.width = px as i16; c.wpct = pc as i32; c.set |= ST_WIDTH; } }
            b"height" => { let (px, pc, a) = len(v); if !a && !pc && px > 0 { c.height = px as i16; c.set |= ST_HEIGHT; } }
            b"box-sizing" => c.bsz = if sw(v, 0, b"border") { 2 } else { 1 },
            b"min-height" => { let (px, pc, a) = len(v); if !a && !pc && px > 0 { c.minh = px as i16; c.set |= ST_HEIGHT; } }
            b"max-width" => { let (px, pc, a) = len(v); if !a && px > 0 { c.maxw = px as i16; c.mpct = pc as i32; c.set |= ST_MAXW; } }
            b"border-width" => { c.set |= ST_BORDER; c.border = clamp(len(v).0, 0, 32) as i16; if c.border_color == 0 { c.border_color = BLACK; } }
            b"border-color" => { if let Some(bc) = parse_color(v) { c.border_color = bc; c.set |= ST_BCOLOR; } }
            b"display" => {
                c.set |= ST_DISPLAY;
                c.display = if sw(v, 0, b"none") { DISP_NONE } else if has(v, b"flex") { DISP_FLEX } else if has(v, b"grid") { DISP_GRID }
                    else if sw(v, 0, b"inline-block") || sw(v, 0, b"inline-table") { DISP_IBLOCK }
                    else if sw(v, 0, b"inline") || sw(v, 0, b"contents") { DISP_INLINE } else { DISP_BLOCK };
            }
            b"position" => {
                c.set |= ST_POS; c.oset |= 0x10;
                c.pos = if sw(v, 0, b"relative") { POS_RELATIVE } else if sw(v, 0, b"absolute") { POS_ABSOLUTE } else if sw(v, 0, b"fixed") { POS_FIXED }
                    else if sw(v, 0, b"sticky") || sw(v, 0, b"-webkit-sticky") { POS_STICKY } else { POS_STATIC };
            }
            b"top" | b"right" | b"bottom" | b"left" | b"inset" => {
                let i0 = match k[0] { b't' => 0, b'r' => 1, b'b' => 2, b'l' => 3, _ => 4 };
                for i in 0..4 {
                    if i0 < 4 && i != i0 { continue; }
                    let (px, pc, a) = len(v);
                    if a { c.oset &= !(1u8 << i); continue; }
                    c.off[i] = clamp(px, -10000, 10000) as i16; c.oset |= 1 << i;
                    if pc { c.opct |= 1 << i; } else { c.opct &= !(1u8 << i); }
                }
                c.set |= ST_POS;
            }
            b"fill" => {
                c.set |= ST_FILL; c.fill_cur = swci(v, 0, b"currentcolor") as i32;
                if c.fill_cur == 0 { match parse_color(v) { Some(x) => c.fill = x, None => c.fill = 0 } }
            }
            b"float" => { c.set |= ST_FLOAT; c.flt = if sw(v, 0, b"left") || sw(v, 0, b"inline-start") { 1 } else if sw(v, 0, b"right") || sw(v, 0, b"inline-end") { 2 } else { 0 }; }
            b"clear" => { c.set |= ST_CLEAR; c.clr = if sw(v, 0, b"left") { 1 } else if sw(v, 0, b"right") { 2 } else if sw(v, 0, b"both") { 3 } else { 0 }; }
            b"flex-direction" => { c.set |= ST_FLEX; c.fset |= F_DIR; c.fdir = (if has(v, b"column") { 1 } else { 0 }) | (if has(v, b"reverse") { 2 } else { 0 }); }
            b"flex-wrap" => { c.set |= ST_FLEX; c.fset |= F_WRAP; c.fwrap = sw(v, 0, b"wrap") as i8; }
            b"flex-flow" => {
                c.set |= ST_FLEX; c.fset |= F_DIR | F_WRAP;
                c.fdir = (if has(v, b"column") { 1 } else { 0 }) | (if has(v, b"reverse") && !has(v, b"wrap-reverse") { 2 } else { 0 });
                c.fwrap = (has(v, b"wrap") && !has(v, b"nowrap")) as i8;
            }
            b"justify-content" => { c.set |= ST_FLEX; c.fset |= F_JUST; c.fjust = parse_justify(v); }
            b"align-items" | b"place-items" => { c.set |= ST_FLEX; c.fset |= F_ALIGN; c.falign = parse_align(v); }
            b"align-self" => { c.set |= ST_FLEX; c.fset |= F_ASELF; c.aself = if sw(v, 0, b"auto") { -1 } else { parse_align(v) }; }
            b"gap" | b"grid-gap" => {
                c.set |= ST_FLEX; c.fset |= F_GAP | F_RGAP;
                c.rgap = clamp(len(v).0, 0, 400) as i16;
                c.gap = match chr(v, 0, b' ') { Some(sp) if at(v, sp + 1) != 0 && at(v, sp + 1) != b'!' => clamp(css_len(v, sp + 1).0, 0, 400) as i16, _ => c.rgap };
            }
            b"column-gap" | b"grid-column-gap" => { c.set |= ST_FLEX; c.fset |= F_GAP; c.gap = clamp(len(v).0, 0, 400) as i16; }
            b"row-gap" | b"grid-row-gap" => { c.set |= ST_FLEX; c.fset |= F_RGAP; c.rgap = clamp(len(v).0, 0, 400) as i16; }
            b"flex-grow" => { c.set |= ST_FLEX; c.fset |= F_GROW; c.grow = clamp(atoi(v, 0), 0, 100) as i8; }
            b"flex-shrink" => { c.set |= ST_FLEX; c.fset |= F_SHRINK; c.shrink = clamp(atoi(v, 0), 0, 100) as i8; }
            b"flex-basis" => {
                c.set |= ST_FLEX; c.fset |= F_BASIS;
                // C left the percent flag unset (stack garbage) for "content"; it is false here
                let (b, pc) = if sw(v, 0, b"auto") || sw(v, 0, b"content") { (-1, false) } else { let (px, pc, _) = len(v); (px as i16, pc) };
                c.basis = b; c.bpct = (if sw(v, 0, b"auto") { false } else { pc }) as i32;
            }
            b"flex" => {
                c.set |= ST_FLEX; c.fset |= F_GROW | F_SHRINK | F_BASIS; c.bpct = 0;
                if sw(v, 0, b"none") { c.grow = 0; c.shrink = 0; c.basis = -1; }
                else if sw(v, 0, b"auto") { c.grow = 1; c.shrink = 1; c.basis = -1; }
                else {
                    let mut p = 0; let mut nums = 0; c.grow = 0; c.shrink = 1; c.basis = 0;
                    while at(v, p) != 0 && at(v, p) != b'!' {
                        while at(v, p) == b' ' { p += 1; }
                        if at(v, p) == 0 || at(v, p) == b'!' { break; }
                        let mut e = p; while at(v, e) != 0 && v[e] != b' ' { e += 1; }
                        let unit = v[p..e].iter().any(|&q| isalpha(q) || q == b'%');
                        if !unit && nums == 0 { c.grow = clamp(atoi(v, p), 0, 100) as i8; nums += 1; }
                        else if !unit && nums == 1 { c.shrink = clamp(atoi(v, p), 0, 100) as i8; nums += 1; }
                        else if sw(v, p, b"auto") { c.basis = -1; c.bpct = 0; }
                        else { let (px, pc, _) = css_len(v, p); c.basis = px as i16; c.bpct = pc as i32; }
                        p = e;
                    }
                }
            }
            b"grid-template-columns" => parse_grid_cols(v, c),
            b"visibility" => { if sw(v, 0, b"hidden") { c.set |= ST_DISPLAY; c.display = 0; } }
            b"text-align" => { c.set |= ST_ALIGN; c.align = if sw(v, 0, b"center") { 1 } else if sw(v, 0, b"right") { 2 } else { 0 }; }
            b"font-weight" => { c.set |= ST_BOLD; c.bold = (sw(v, 0, b"bold") || atoi(v, 0) >= 600) as i32; }
            b"font-style" => { c.set |= ST_ITALIC; c.italic = (sw(v, 0, b"italic") || sw(v, 0, b"oblique")) as i32; }
            b"text-decoration" | b"text-decoration-line" => { c.set |= ST_UNDER; c.under = has(v, b"underline") as i32; }
            b"font-size" => {
                let mut px = atoi(v, 0);
                if has(v, b"em") || has(v, b"rem") {
                    let whole = atoi(v, 0);
                    let frac = match chr(v, 0, b'.') { Some(d) => at(v, d + 1) as i8 as i32 - b'0' as i32, None => 0 };   // C's char is signed
                    px = whole.wrapping_mul(10).wrapping_add(frac).wrapping_mul(16) / 10;
                }
                else if chr(v, 0, b'%').is_some() { px = px.wrapping_mul(16) / 100; }
                else if has(v, b"pt") { px = px.wrapping_mul(4) / 3; }
                else if has(v, b"small") { px = 12; }
                else if has(v, b"large") { px = 20; }
                if px != 0 { c.set |= ST_SIZE; c.size = if px <= 13 { -1 } else if px <= 18 { 0 } else if px <= 21 { 3 } else if px <= 26 { 2 } else { 1 }; }
            }
            _ => {}
        }
    }
}

/// The cascade: src's properties over dst's.
pub fn merge(dst: &mut Css, src: &Css) {
    if src.set & ST_COLOR != 0 { dst.color = src.color; }
    if src.set & ST_BG != 0 { dst.bg = src.bg; }
    if src.set & ST_DISPLAY != 0 { dst.display = src.display; }
    if src.set & ST_ALIGN != 0 { dst.align = src.align; }
    if src.set & ST_BOLD != 0 { dst.bold = src.bold; }
    if src.set & ST_ITALIC != 0 { dst.italic = src.italic; }
    if src.set & ST_UNDER != 0 { dst.under = src.under; }
    if src.set & ST_SIZE != 0 { dst.size = src.size; }
    if src.set & ST_RADIUS != 0 { dst.radius = src.radius; }
    if src.set & ST_GRAD != 0 { dst.bg2 = src.bg2; dst.grad = src.grad; dst.grad_dir = src.grad_dir; }
    if src.set & ST_BORDER != 0 { dst.border = src.border; dst.border_color = src.border_color; }
    if src.set & ST_BCOLOR != 0 { dst.border_color = src.border_color; }
    for i in 0..4 {
        if src.mset & (1 << i) != 0 { dst.m[i] = src.m[i]; dst.mset |= 1 << i; dst.mauto = (dst.mauto & !(1u8 << i)) | (src.mauto & (1 << i)); }
        if src.pset & (1 << i) != 0 { dst.pad[i] = src.pad[i]; dst.pset |= 1 << i; }
    }
    if src.set & ST_WIDTH != 0 { dst.width = src.width; dst.wpct = src.wpct; }
    if src.set & ST_MAXW != 0 { dst.maxw = src.maxw; dst.mpct = src.mpct; }
    if src.set & ST_POS != 0 {
        if src.oset & 0x10 != 0 { dst.pos = src.pos; dst.oset |= 0x10; }
        for i in 0..4 { if src.oset & (1 << i) != 0 { dst.off[i] = src.off[i]; dst.oset |= 1 << i; dst.opct = (dst.opct & !(1u8 << i)) | (src.opct & (1 << i)); } }
    }
    if src.set & ST_FLOAT != 0 { dst.flt = src.flt; }
    if src.set & ST_FILL != 0 { dst.fill = src.fill; dst.fill_cur = src.fill_cur; }
    if src.set & ST_CLEAR != 0 { dst.clr = src.clr; }
    if src.bsz != 0 { dst.bsz = src.bsz; }
    if src.set & ST_HEIGHT != 0 { if src.height != 0 { dst.height = src.height; } if src.minh != 0 { dst.minh = src.minh; } }
    if src.set & ST_FLEX != 0 {
        let f = src.fset;
        if f & F_DIR != 0 { dst.fdir = src.fdir; }
        if f & F_WRAP != 0 { dst.fwrap = src.fwrap; }
        if f & F_JUST != 0 { dst.fjust = src.fjust; }
        if f & F_ALIGN != 0 { dst.falign = src.falign; }
        if f & F_ASELF != 0 { dst.aself = src.aself; }
        if f & F_GAP != 0 { dst.gap = src.gap; }
        if f & F_RGAP != 0 { dst.rgap = src.rgap; }
        if f & F_GROW != 0 { dst.grow = src.grow; }
        if f & F_SHRINK != 0 { dst.shrink = src.shrink; }
        if f & F_BASIS != 0 { dst.basis = src.basis; dst.bpct = src.bpct; }
        if f & F_GCOLS != 0 { dst.gcols = src.gcols; dst.gmin = src.gmin; }
        dst.fset |= f;
    }
    dst.set |= src.set;
}

// ---------------------------------------------------------------- @media

/// The width (and height, 600) @media queries are answered for.
pub const VIEW_W: i32 = 880;
fn media_length(v: &[u8]) -> i32 {
    let whole = atoi(v, 0);
    let mut tenth = 0;
    if let Some(dot) = chr(v, 0, b'.') { if isdigit(at(v, dot + 1)) { tenth = (v[dot + 1] - b'0') as i32; } }
    let mut u = 0;
    while at(v, u) != 0 && (isdigit(v[u]) || v[u] == b'.' || v[u] == b'-' || v[u] == b' ') { u += 1; }
    if sw(v, u, b"em") || sw(v, u, b"rem") { return whole.wrapping_mul(10).wrapping_add(tenth).wrapping_mul(16) / 10; }
    whole
}
fn media_feature(f: &[u8]) -> bool {
    let buf: Vec<u8> = cstr(&f[..f.len().min(95)]).iter().map(|&b| lower(b)).collect();
    let mut name = 0;
    while at(&buf, name) == b' ' { name += 1; }
    let Some(colon) = chr(&buf, 0, b':') else {
        if let Some(op) = pbrk(&buf, 0, b"<>=") {
            if sw(&buf, name, b"width") {
                let (o0, o1) = (buf[op], at(&buf, op + 1));
                let (ge, eqq) = (o0 == b'>', o1 == b'=' || o0 == b'=');
                let v = media_length(&buf[(op + if o1 == b'=' { 2 } else { 1 }).min(buf.len())..]);
                return if o0 == b'=' { VIEW_W == v } else if ge { if eqq { VIEW_W >= v } else { VIEW_W > v } } else if eqq { VIEW_W <= v } else { VIEW_W < v };
            }
        }
        return sw(&buf, name, b"color") || sw(&buf, name, b"hover") || sw(&buf, name, b"pointer") || sw(&buf, name, b"grid");
    };
    let mut ne = colon;
    while ne > 0 && buf[ne - 1] == b' ' { ne -= 1; }
    let nm = if name <= ne { &buf[name..ne] } else { &buf[ne..ne] };
    let mut v = colon + 1;
    while at(&buf, v) == b' ' { v += 1; }
    let v = &buf[v.min(buf.len())..];
    match nm {
        b"min-width" => VIEW_W >= media_length(v),
        b"max-width" => VIEW_W <= media_length(v),
        b"min-height" => 600 >= media_length(v),
        b"max-height" => 600 <= media_length(v),
        b"orientation" => sw(v, 0, b"landscape"),
        b"prefers-color-scheme" => sw(v, 0, b"light"),
        b"prefers-reduced-motion" => sw(v, 0, b"reduce"),
        b"hover" | b"any-hover" => sw(v, 0, b"hover"),
        b"pointer" | b"any-pointer" => sw(v, 0, b"fine"),
        _ => false,
    }
}
/// A media query list (`media=""` or after `@media`): does any query match a screen VIEW_W wide?
pub fn media_ok(q: &[u8]) -> bool {
    let n = q.len();
    if n == 0 { return true; }
    let mut start = 0;
    for i in 0..=n {
        if i < n && q[i] != b',' { continue; }
        let (mut s, e) = (start, i);
        start = i + 1;
        while s < e && isspace(q[s]) { s += 1; }
        let (mut neg, mut ok) = (false, true);
        if e - s >= 4 && q[s..s + 4].eq_ignore_ascii_case(b"not ") { neg = true; s += 4; }
        else if e - s >= 5 && q[s..s + 5].eq_ignore_ascii_case(b"only ") { s += 5; }
        while s < e {
            while s < e && isspace(q[s]) { s += 1; }
            if s >= e { break; }
            if q[s] == b'(' {
                let mut c = s + 1; let mut depth = 1;
                while c < e && depth != 0 { if q[c] == b'(' { depth += 1; } else if q[c] == b')' { depth -= 1; } if depth != 0 { c += 1; } }
                if !media_feature(&q[s + 1..c]) { ok = false; }
                s = if c < e { c + 1 } else { e };
            } else {
                let w = s;
                while s < e && !isspace(q[s]) && q[s] != b'(' { s += 1; }
                let word = &q[w..s];
                if word.eq_ignore_ascii_case(b"and") || word.eq_ignore_ascii_case(b"screen") || word.eq_ignore_ascii_case(b"all") { continue; }
                ok = false;
            }
        }
        if ok != neg { return true; }
    }
    false
}

// ---------------------------------------------------------------- style sheets

/// Skip the comment or string at s[i]; the index after it.
fn css_skip(s: &[u8], mut i: usize) -> usize {
    let n = s.len();
    if s[i] == b'/' && i + 1 < n && s[i + 1] == b'*' { i += 2; while i + 1 < n && !(s[i] == b'*' && s[i + 1] == b'/') { i += 1; } return (i + 2).min(n); }
    if s[i] == b'"' || s[i] == b'\'' { let q = s[i]; i += 1; while i < n && s[i] != q { if s[i] == b'\\' { i += 1; } i += 1; } return (i + 1).min(n); }
    i + 1
}
/// The index of the '}' closing the block opened just before i, or s.len().
fn block_end(s: &[u8], mut i: usize) -> usize {
    let mut depth = 1;
    while i < s.len() {
        let c = s[i];
        if c == b'{' { depth += 1; } else if c == b'}' { depth -= 1; if depth == 0 { return i; } }
        i = if c == b'/' || c == b'"' || c == b'\'' { css_skip(s, i) } else { i + 1 };
    }
    s.len()
}
/// Every style rule of a sheet, in order, as (selector list, body): `@media` blocks that
/// match, `@supports`, `@layer` and `@document` are entered (8 levels deep at most); other
/// at-rules are skipped.
pub fn rules(s: &[u8], f: &mut dyn FnMut(&[u8], &[u8])) { rules_at(s, 0, f) }
fn rules_at(s: &[u8], level: i32, f: &mut dyn FnMut(&[u8], &[u8])) {
    let n = s.len();
    let mut i = 0;
    while i < n {
        while i < n && (isspace(s[i]) || s[i] == b';') { i += 1; }
        if i >= n { break; }
        if s[i] == b'/' && i + 1 < n && s[i + 1] == b'*' { i = css_skip(s, i); continue; }
        if s[i] == b'}' { i += 1; continue; }
        let pre = i;
        while i < n && s[i] != b'{' && !(s[pre] == b'@' && s[i] == b';') { i = if s[i] == b'/' || s[i] == b'"' || s[i] == b'\'' { css_skip(s, i) } else { i + 1 }; }
        if i >= n { break; }
        if s[i] == b';' { i += 1; continue; }
        let pre_end = i;
        i += 1;
        let body = i;
        let body_end = block_end(s, i);
        i = body_end + 1;
        if s[pre] == b'@' {
            let kw = &s[pre + 1..pre_end];
            let kl = kw.iter().take_while(|&&c| isalnum(c) || c == b'-').count();
            let kw0 = &kw[..kl];
            let nest = if kw0.eq_ignore_ascii_case(b"media") { media_ok(&kw[5..]) }
                else { kw0.eq_ignore_ascii_case(b"supports") || kw0.eq_ignore_ascii_case(b"layer") || kw0.eq_ignore_ascii_case(b"document") };
            if nest && level < 8 { rules_at(&s[body..body_end], level + 1, f); }
            continue;
        }
        let mut pl = pre_end - pre;
        while pl > 0 && isspace(s[pre + pl - 1]) { pl -= 1; }
        f(&s[pre..pre + pl], &s[body..body_end]);
    }
}

/// The priority a rule's custom properties get: 2 for `:root`, `html` and `body` (the
/// page's own values), 1 for others.
pub fn var_prio(sel: &[u8]) -> u8 {
    let sl = sel.len();
    for i in 0..sl {
        if sw(sel, i, b":root") { return 2; }
        if (i == 0 || sel[i - 1] == b',' || sel[i - 1] == b' ') && (sw(sel, i, b"html") || sw(sel, i, b"body")) && (i + 4 >= sl || !isalnum(sel[i + 4])) { return 2; }
    }
    1
}
/// Does a rule body declare any custom property (any `--`)?
pub fn has_vars(body: &[u8]) -> bool { body.windows(2).any(|w| w == b"--") }
/// The custom properties declared in a rule body, as (name, value) in order.
pub fn var_decls(b: &[u8], f: &mut dyn FnMut(&[u8], &[u8])) {
    let bl = b.len();
    let mut i = 0;
    while i < bl {
        while i < bl && (isspace(b[i]) || b[i] == b';') { i += 1; }
        let ks = i;
        while i < bl && b[i] != b':' && b[i] != b';' { i += 1; }
        if i >= bl || b[i] != b':' { while i < bl && b[i] != b';' { i += 1; } continue; }
        let mut ke = i;
        while ke > ks && isspace(b[ke - 1]) { ke -= 1; }
        i += 1;
        let vs = i;
        let mut dep = 0;
        while i < bl && (dep != 0 || b[i] != b';') {
            match b[i] { b'(' | b'{' | b'[' => dep += 1, b')' | b'}' | b']' if dep != 0 => dep -= 1, _ => {} }
            i += 1;
        }
        if ke - ks > 2 && b[ks] == b'-' && b[ks + 1] == b'-' { f(&b[ks..ke], &b[vs..i]); }
    }
}
/// A custom property's value as stored: trimmed, without `!important`.
pub fn var_value(v: &[u8]) -> &[u8] {
    let mut v = v;
    while let [rest @ .., c] = v { if isspace(*c) { v = rest; } else { break; } }
    while let [c, rest @ ..] = v { if isspace(*c) { v = rest; } else { break; } }
    if v.len() > 10 && v[v.len() - 10..].eq_ignore_ascii_case(b"!important") {
        v = &v[..v.len() - 10];
        while let [rest @ .., c] = v { if isspace(*c) { v = rest; } else { break; } }
    }
    cstr(v)
}

#[cfg(test)]
mod tests {
    use super::*;
    struct V(Vec<(Vec<u8>, Vec<u8>)>);
    impl Vars for V { fn get(&self, n: &[u8]) -> Option<&[u8]> { self.0.iter().rev().find(|x| x.0 == n).map(|x| &x.1[..]) } }
    fn d(s: &str) -> Css { let mut c = Css::default(); decls(s.as_bytes(), &mut c, &NoVars); c }

    #[test]
    fn colours() {
        assert_eq!(parse_color(b"#fff"), Some(0xFFFFFFFF));
        assert_eq!(parse_color(b" #12345678"), Some(0xFF123456));
        assert_eq!(parse_color(b"#12"), None);
        assert_eq!(parse_color(b"rgb(255, 0, 50%)"), Some(0xFFFF007F));
        assert_eq!(parse_color(b"RGBA(1,2,3,0.5)"), Some(0xFF010203));
        assert_eq!(parse_color(b"CornflowerBlue"), Some(0xFF6495ED));
        assert_eq!(parse_color(b"abcdef"), Some(0xFFABCDEF));
        assert_eq!(parse_color(b"bogus"), None);
    }
    #[test]
    fn declarations() {
        let c = d("color: red; margin: 1px 2em; WIDTH: 50%; display:flex; flex: 2 1 30px; border: 2px solid blue");
        assert_eq!(c.color, 0xFFFF0000);
        assert_eq!((c.m, c.mset), ([1, 32, 1, 32], 0xF));
        assert_eq!((c.width, c.wpct), (50, 1));
        assert_eq!(c.display, DISP_FLEX);
        assert_eq!((c.grow, c.shrink, c.basis), (2, 1, 30));
        assert_eq!((c.border, c.border_color), (2, 0xFF0000FF));
        let c = d("width: calc(100% - 20px); height: min(40px, 50%); margin-left: auto");
        assert_eq!(c.width, 780);
        assert_eq!(c.height, 40);
        assert_eq!(c.mauto, 8);
        let c = d("background: linear-gradient(to right, #000, white)");
        assert_eq!((c.bg, c.bg2, c.grad, c.grad_dir), (0xFF000000, 0xFFFFFFFF, 1, 1));
        let c = d("grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); font-size: 1.5em");
        assert_eq!((c.gmin, c.size), (150, 2));
        assert_eq!(d("grid-template-columns: [a] 1fr minmax(0, 2fr) 3fr").gcols, 3);
    }
    #[test]
    fn variables() {
        let v = V(alloc::vec![(b"--a".to_vec(), b"#00f".to_vec()), (b"--b".to_vec(), b"var(--a)".to_vec())]);
        let mut c = Css::default();
        decls(b"color: var(--b); background-color: var(--x, var(--a)); border-color: var(--nope)", &mut c, &v);
        assert_eq!((c.color, c.bg, c.set & ST_BCOLOR), (0xFF0000FF, 0xFF0000FF, 0));
        let mut got = Vec::new();
        var_decls(b" --a : 1px ; color: red; --b: f(a;b) ;", &mut |n, v| got.push((n.to_vec(), var_value(v).to_vec())));
        assert_eq!(got, alloc::vec![(b"--a".to_vec(), b"1px".to_vec()), (b"--b".to_vec(), b"f(a;b)".to_vec())]);
        assert_eq!(var_prio(b"html.dark"), 2);
        assert_eq!(var_prio(b".htmlx, a"), 1);
    }
    #[test]
    fn media_and_rules() {
        assert!(media_ok(b"screen and (min-width: 600px)"));
        assert!(!media_ok(b"print"));
        assert!(media_ok(b"print, (max-width: 60em)"));
        assert!(!media_ok(b"not all and (width >= 100px)"));
        let mut got = Vec::new();
        rules(b"a{x:1} /* c } */ @media print { b{} } @media (min-width:1px) { i , p {y:2} } @import 'u'; s{z:\"}\"}",
              &mut |s, b| got.push((s.to_vec(), b.to_vec())));
        let want: Vec<(Vec<u8>, Vec<u8>)> = [("a", "x:1"), ("i , p", "y:2"), ("s", "z:\"}\"")].iter().map(|(a, b)| (a.as_bytes().to_vec(), b.as_bytes().to_vec())).collect();
        assert_eq!(got, want);
    }
    #[test]
    fn text() {
        let mut o = Vec::new();
        utf8_normalise(b"a\x93b\xe2\x80\x8bc\r\xc3\xa9\xff", |b| o.push(b));
        assert_eq!(o, "a\u{201C}bc\u{e9}\u{ff}".as_bytes());
        assert!(!is_utf8(b"<meta charset=\"ISO-8859-1\">", None));
        assert!(is_utf8(b"", Some(b"utf-8")));
        assert_eq!(meta_refresh(b"5; URL='/x'y"), (5, Some(&b"/x"[..])));
        assert_eq!(title(b"\n  Hello\tworld  "), b"Hello world");
    }
    #[test]
    fn junk_does_not_panic() {
        let mut seed = 0x1234_5678u32;
        let alphabet = b"#();:,%-.0123456789 abcdefghijklmnopqrstuvwxyz!{}/*\"'@[]+\\\x00\xff";
        for _ in 0..20000 {
            let mut s = Vec::new();
            for _ in 0..(seed % 80) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; s.push(alphabet[(seed % alphabet.len() as u32) as usize]); }
            let mut c = Css::default();
            for p in [&b"width:"[..], b"margin:", b"background:", b"flex:", b"border:", b"grid-template-columns:", b"font-size:", b"color:var(--a,", b"height:calc("] {
                let mut x = p.to_vec(); x.extend_from_slice(&s); decls(&x, &mut c, &NoVars);
            }
            media_ok(&s);
            rules(&s, &mut |_, b| { var_decls(b, &mut |_, _| {}); });
            let _ = (parse_color(&s), is_utf8(&s, None), meta_refresh(&s), title(&s));
        }
    }
}
