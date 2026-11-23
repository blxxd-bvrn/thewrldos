//! WordPad's document readers (were in src/apps/wordpad.c): Rich Text, and the zip container
//! with the XML of Office Open XML (.docx) and OpenDocument Text (.odt). They build a list of
//! paragraphs (UTF-8 text, a character style per byte, the paragraph's alignment, bullet,
//! spacing and indents) that WordPad copies into its own document; the editor, the writers
//! and plain text stay in C.
//!
//! The results are the C readers', with these changes: an "&" that is not a known entity
//! (a bare "&", "&#0;", "&#x;", "&nbsp;") is kept as text, where the C code went round
//! forever; and a zip whose sizes or offsets point outside the file is not read past its end
//! (the C code's 32-bit sums could wrap, a stored member was copied by its uncompressed
//! size, and a member name at the very end was compared beyond the file).

use alloc::vec::Vec;

/// cst_t (src/apps/wordpad.c): a character style. hp is in half-points; script 1 sub, 2 super;
/// bg 0 is none.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Cst { pub fam: u8, pub b: u8, pub i: u8, pub u: u8, pub s: u8, pub script: u8, pub hp: u16, pub fg: u32, pub bg: u32 }

pub const FAM_SANS: u8 = 0;
pub const FAM_SERIF: u8 = 1;
pub const FAM_MONO: u8 = 2;
const DPI: i16 = 96;

/// 11 pt serif, as WordPad
pub fn default_style() -> Cst { Cst { fam: FAM_SERIF, b: 0, i: 0, u: 0, s: 0, script: 0, hp: 22, fg: 0xFF00_0000, bg: 0 } }

/// One paragraph: `st[k]` is the style of `text[k]`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Para { pub text: Vec<u8>, pub st: Vec<Cst>, pub endst: Cst, pub align: u8, pub bullet: u8, pub lsp: u8, pub lind: i16, pub first: i16, pub rind: i16 }
impl Para {
    fn new(endst: Cst) -> Para { Para { text: Vec::new(), st: Vec::new(), endst, align: 0, bullet: 0, lsp: 20, lind: 0, first: 0, rind: 0 } }
    fn ins(&mut self, t: &[u8], c: Cst) { self.text.extend_from_slice(t); self.st.extend(core::iter::repeat(c).take(t.len())); }
    fn ins_cp(&mut self, cp: u32, c: Cst) { let mut b = Vec::new(); put_utf8(&mut b, cp); self.ins(&b, c); }
}
/// A document as the readers leave it: never empty (it starts with one empty paragraph).
pub struct Doc { pub paras: Vec<Para> }
impl Doc {
    fn new() -> Doc { Doc { paras: alloc::vec![Para::new(default_style())] } }
    fn cur(&mut self) -> &mut Para { self.paras.last_mut().unwrap() }
    fn add(&mut self, endst: Cst) { self.paras.push(Para::new(endst)); }
}

/// The C code's put_utf8, for any 32-bit value (larger ones give its 4-byte form, cut to bytes).
fn put_utf8(b: &mut Vec<u8>, c: u32) {
    if c < 0x80 { b.push(c as u8); }
    else if c < 0x800 { b.extend_from_slice(&[(0xC0 | (c >> 6)) as u8, (0x80 | (c & 63)) as u8]); }
    else if c < 0x10000 { b.extend_from_slice(&[(0xE0 | (c >> 12)) as u8, (0x80 | ((c >> 6) & 63)) as u8, (0x80 | (c & 63)) as u8]); }
    else { b.extend_from_slice(&[(0xF0 | (c >> 18)) as u8, (0x80 | ((c >> 12) & 63)) as u8, (0x80 | ((c >> 6) & 63)) as u8, (0x80 | (c & 63)) as u8]); }
}
const CP1252: [u16; 32] = [0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
                           0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178];
fn from1252(c: i32) -> u32 { if (0x80..0xA0).contains(&c) { CP1252[(c - 0x80) as usize] as u32 } else { c as u32 } }

/// The C string at the start of `b`.
fn cstr(b: &[u8]) -> &[u8] { &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())] }
fn ci_contains(h: &[u8], n: &[u8]) -> bool { h.len() >= n.len() && h.windows(n.len()).any(|w| w.eq_ignore_ascii_case(n)) }
/// The font family a font name stands for.
pub fn fam_from_name(n: &[u8]) -> u8 {
    let n = cstr(n);
    let has = |s: &[u8]| ci_contains(n, s);
    if has(b"mono") || has(b"courier") || has(b"consol") || has(b"lucida console") { return FAM_MONO; }
    if has(b"serif") && !has(b"sans") { return FAM_SERIF; }
    if has(b"times") || has(b"georgia") || has(b"cambria") || has(b"garamond") || has(b"roman") || has(b"book") { return FAM_SERIF; }
    FAM_SANS
}
fn isspace(c: u8) -> bool { matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) }
/// The kernel's strtoul (src/kernel/lib.c) for base 10 or 16, kept to 32 bits as its callers do.
fn strtoul(s: &[u8], mut base: u32) -> u32 {
    let at = |i: usize| s.get(i).copied().unwrap_or(0);
    let mut i = 0;
    while isspace(at(i)) { i += 1; }
    if at(i) == b'+' { i += 1; }
    if base == 16 && at(i) == b'0' && (at(i + 1) == b'x' || at(i + 1) == b'X') { i += 2; base = 16; }
    let mut v: u32 = 0;
    loop {
        let c = at(i);
        let d = if c.is_ascii_digit() { c - b'0' } else if c.is_ascii_lowercase() { c - b'a' + 10 } else if c.is_ascii_uppercase() { c - b'A' + 10 } else { break } as u32;
        if d >= base { break; }
        v = v.wrapping_mul(base).wrapping_add(d);
        i += 1;
    }
    v
}
/// The kernel's atoi.
fn atoi(s: &[u8]) -> i32 {
    let mut i = 0;
    while i < s.len() && isspace(s[i]) { i += 1; }
    let neg = i < s.len() && s[i] == b'-';
    if neg { i += 1; }
    let v = strtoul(&s[i..], 10);
    (if neg { v.wrapping_neg() } else { v }) as i32
}

// ---------------------------------------------------------------- Rich Text

#[derive(Clone, Copy)]
struct RState { c: Cst, skip: bool, uc: i32 }

/// A Rich Text document (also WordPad's undo snapshots and its rich clipboard).
pub fn rtf(s: &[u8]) -> Doc {
    let n = s.len();
    let mut d = Doc::new();
    let mut stack: [RState; 64] = [RState { c: default_style(), skip: false, uc: 1 }; 64];
    let mut sp = 0usize;
    let mut st = RState { c: default_style(), skip: false, uc: 1 };
    let mut fam_of_font = [FAM_SANS; 64];
    let mut cols = [0u32; 64];
    let mut ncol = 0usize;
    let (mut in_colortbl, mut in_fonttbl) = (false, false);
    let mut cur_font: i32 = 0;
    let mut fname: Vec<u8> = Vec::new();
    let (mut cr, mut cg, mut cbl) = (0i32, 0i32, 0i32);
    let (mut palign, mut pbullet, mut plsp) = (0u8, 0u8, 20u8);
    let (mut pl, mut pf, mut pr) = (0i16, 0i16, 0i16);
    let mut skip_chars: i32 = 0;
    /* a bulleted paragraph in RTF: the bullet at li+fi (fi negative), the text at li; here the
       bullet sits at lind and the text a quarter inch after */
    macro_rules! apply_para { () => {{
        let p = d.cur();
        p.align = palign; p.bullet = pbullet; p.lsp = plsp; p.rind = pr;
        if pbullet != 0 { let b0 = if pf < 0 { (pl as i32 + pf as i32) as i16 } else { pl }; p.lind = if b0 < 0 { 0 } else { b0 }; p.first = 0; }
        else { p.lind = pl; p.first = pf; }
    }}; }
    let set_font = |fam: &mut [u8; 64], cur: i32, name: &[u8]| { if (0..64).contains(&cur) { fam[cur as usize] = fam_from_name(name); } };
    let mut i = 0usize;
    while i < n {
        let c = s[i];
        if c == b'{' { if sp < 64 { stack[sp] = st; sp += 1; } i += 1; continue; }
        if c == b'}' {
            if in_fonttbl && !fname.is_empty() { set_font(&mut fam_of_font, cur_font, &fname); fname.clear(); }
            if sp > 0 { let was_tbl = st.skip; sp -= 1; st = stack[sp]; if was_tbl && !st.skip { in_colortbl = false; in_fonttbl = false; } }
            i += 1; continue;
        }
        if c == b'\r' || c == b'\n' { i += 1; continue; }
        if c == b'\\' {
            i += 1;
            if i >= n { break; }
            let e = s[i];
            if e == b'\\' || e == b'{' || e == b'}' {
                if !st.skip { if skip_chars != 0 { skip_chars -= 1; } else { apply_para!(); d.cur().ins(&[e], st.c); } }
                i += 1; continue;
            }
            if e == b'\'' {
                let mut v: i32 = 0;
                for k in 1..=2 {
                    if i + k >= n { break; }
                    let h = s[i + k] as i8 as i32;
                    v = v.wrapping_mul(16).wrapping_add(if h >= b'a' as i32 { h - b'a' as i32 + 10 } else if h >= b'A' as i32 { h - b'A' as i32 + 10 } else { h - b'0' as i32 });
                }
                i += 3;
                if in_fonttbl { continue; }
                if !st.skip { if skip_chars != 0 { skip_chars -= 1; } else { apply_para!(); d.cur().ins_cp(from1252(v), st.c); } }
                continue;
            }
            if e == b'*' { st.skip = true; i += 1; continue; }
            if e == b'~' { if !st.skip { d.cur().ins_cp(0xA0, st.c); } i += 1; continue; }
            if e == b'-' || e == b'_' { i += 1; continue; }
            if e == b'\n' || e == b'\r' { if !st.skip { apply_para!(); d.add(st.c); } i += 1; continue; }
            let w0 = i;
            while i < n && s[i].is_ascii_alphabetic() && i - w0 < 31 { i += 1; }
            let word = &s[w0..i];
            let (mut hasnum, mut num, mut neg) = (false, 0i32, false);
            if i < n && s[i] == b'-' { neg = true; i += 1; }
            while i < n && s[i].is_ascii_digit() { num = num.wrapping_mul(10).wrapping_add((s[i] - b'0') as i32); hasnum = true; i += 1; }
            if neg { num = num.wrapping_neg(); }
            if i < n && s[i] == b' ' { i += 1; }
            let on = (!hasnum || num != 0) as u8;
            let tbl_col = |k: i32| if k > 0 && (k as usize) < ncol { Some(cols[k as usize]) } else { None };
            match word {
                b"fonttbl" => { in_fonttbl = true; st.skip = true; continue; }
                b"colortbl" => { in_colortbl = true; st.skip = true; ncol = 0; cr = 0; cg = 0; cbl = 0; continue; }
                b"stylesheet" | b"info" | b"pict" | b"header" | b"footer" | b"headerl" | b"headerr" | b"footerl" | b"footerr" | b"object" | b"listtable"
                | b"listoverridetable" | b"rsidtbl" | b"generator" | b"themedata" | b"colorschememapping" | b"latentstyles" | b"datastore" | b"xmlnstbl"
                | b"fldinst" | b"pntext" | b"bkmkstart" | b"bkmkend" => { st.skip = true; continue; }
                b"pn" | b"pnlvlblt" => { pbullet = 1; st.skip = true; continue; }
                _ => {}
            }
            if in_fonttbl { if word == b"f" { cur_font = num; fname.clear(); } continue; }
            if in_colortbl { match word { b"red" => cr = num, b"green" => cg = num, b"blue" => cbl = num, _ => {} } continue; }
            match word {
                b"par" | b"sect" | b"page" => { if !st.skip { apply_para!(); d.cur().endst = st.c; d.add(st.c); } }
                b"line" => { if !st.skip { apply_para!(); d.add(st.c); } }
                b"pard" => { palign = 0; pbullet = 0; plsp = 20; pl = 0; pf = 0; pr = 0; }
                b"plain" => { st.c = default_style(); st.c.fam = fam_of_font[(cur_font & 63) as usize]; }
                b"ql" => palign = 0,
                b"qc" => palign = 1,
                b"qr" => palign = 2,
                b"qj" => palign = 3,
                b"li" => pl = (num / 15) as i16,
                b"fi" => pf = (num / 15) as i16,
                b"ri" => pr = (num / 15) as i16,
                b"sl" => { if num > 0 { plsp = (num / 12).clamp(20, 60) as u8; } }
                b"b" => st.c.b = on,
                b"i" => st.c.i = on,
                b"ul" => st.c.u = on,
                b"ulnone" => st.c.u = 0,
                b"strike" => st.c.s = on,
                b"sub" => st.c.script = 1,
                b"super" => st.c.script = 2,
                b"nosupersub" => st.c.script = 0,
                b"fs" => { if num > 0 { st.c.hp = num as u16; } }
                b"f" => { st.c.fam = fam_of_font[(num & 63) as usize]; }
                b"cf" => st.c.fg = tbl_col(num).map_or(0xFF00_0000, |c| c | 0xFF00_0000),
                b"highlight" | b"cb" | b"chcbpat" => st.c.bg = tbl_col(num).map_or(0, |c| c | 0xFF00_0000),
                b"uc" => st.uc = num,
                b"u" => { if !st.skip { apply_para!(); d.cur().ins_cp(if num < 0 { num.wrapping_add(65536) } else { num } as u32, st.c); } skip_chars = st.uc; }
                b"tab" => { if !st.skip { apply_para!(); d.cur().ins(b"\t", st.c); } }
                b"bullet" => { if !st.skip { d.cur().ins_cp(0x2022, st.c); } }
                b"emdash" => { if !st.skip { d.cur().ins_cp(0x2014, st.c); } }
                b"endash" => { if !st.skip { d.cur().ins_cp(0x2013, st.c); } }
                b"lquote" => { if !st.skip { d.cur().ins_cp(0x2018, st.c); } }
                b"rquote" => { if !st.skip { d.cur().ins_cp(0x2019, st.c); } }
                b"ldblquote" => { if !st.skip { d.cur().ins_cp(0x201C, st.c); } }
                b"rdblquote" => { if !st.skip { d.cur().ins_cp(0x201D, st.c); } }
                _ => {}
            }
            continue;
        }
        if in_colortbl {
            if c == b';' {
                if ncol < 64 { cols[ncol] = (cr as u32) << 16 | (cg as u32) << 8 | cbl as u32; ncol += 1; }
                cr = 0; cg = 0; cbl = 0;
            }
            i += 1; continue;
        }
        if in_fonttbl {
            if c == b';' { set_font(&mut fam_of_font, cur_font, &fname); fname.clear(); }
            else if fname.len() < 63 { fname.push(c); }
            i += 1; continue;
        }
        if st.skip { i += 1; continue; }
        if skip_chars != 0 { skip_chars -= 1; i += 1; continue; }
        /* plain text up to the next control character */
        let mut j = i;
        while j < n && !matches!(s[j], b'\\' | b'{' | b'}' | b'\r' | b'\n') { j += 1; }
        apply_para!();
        for &u in &s[i..j] { if u < 0x80 { d.cur().ins(&[u], st.c); } else { d.cur().ins_cp(from1252(u as i32), st.c); } }
        i = j;
    }
    d
}

// ---------------------------------------------------------------- zip

fn rd16(z: &[u8], o: u64) -> u64 { z[o as usize] as u64 | (z[o as usize + 1] as u64) << 8 }
fn rd32(z: &[u8], o: u64) -> u64 { rd16(z, o) | rd16(z, o + 2) << 16 }
/// One member of a zip archive, decompressed; None if it is absent or damaged.
pub fn zip_get(z: &[u8], name: &[u8]) -> Option<Vec<u8>> {
    let n = z.len() as u64;
    if n < 22 { return None; }
    let mut e = n - 22;
    while e > 0 && rd32(z, e) != 0x0605_4b50 { e -= 1; }
    if rd32(z, e) != 0x0605_4b50 { return None; }
    let cnt = rd16(z, e + 10);
    let mut cd = rd32(z, e + 16);
    let mut k = 0;
    while k < cnt && cd + 46 <= n {
        if rd32(z, cd) != 0x0201_4b50 { break; }
        let (method, nl, xl, cl) = (rd16(z, cd + 10), rd16(z, cd + 28), rd16(z, cd + 30), rd16(z, cd + 32));
        let (csize, usize_, lho) = (rd32(z, cd + 20), rd32(z, cd + 24), rd32(z, cd + 42));
        if nl == name.len() as u64 && cd + 46 + nl <= n && &z[(cd + 46) as usize..(cd + 46 + nl) as usize] == name && lho + 30 <= n {
            let data = lho + 30 + rd16(z, lho + 26) + rd16(z, lho + 28);
            if data + csize > n || usize_ > 64 * 1024 * 1024 { return None; }
            let src = &z[data as usize..(data + csize) as usize];
            return match method {
                0 => if usize_ <= csize { Some(src[..usize_ as usize].to_vec()) } else { None },
                8 => {
                    let mut out = Vec::new();
                    out.try_reserve_exact(usize_ as usize).ok()?;
                    out.resize(usize_ as usize, 0);
                    let d = crate::inflate::inflate_raw(src, &mut out).ok()?;
                    out.truncate(d.written);
                    Some(out)
                }
                _ => None,
            };
        }
        cd += 46 + nl + xl + cl;
        k += 1;
    }
    None
}

// ---------------------------------------------------------------- XML (enough for .docx and .odt)

fn at(x: &[u8], i: usize) -> u8 { x.get(i).copied().unwrap_or(0) }
/// The value of attribute `name` in the tag starting at x[t] (looked for up to its '>'), cut
/// to cap - 1 bytes; empty when absent. As in the C code, the value itself is read up to its
/// closing quote wherever that is.
fn xattr(x: &[u8], t: usize, name: &[u8], cap: usize) -> Vec<u8> {
    let nl = name.len();
    let mut p = t;
    while at(x, p) != 0 && at(x, p) != b'>' {
        if (p == t || matches!(x[p - 1], b' ' | b'\t' | b'\n')) && x.len() >= p + nl && &x[p..p + nl] == name && at(x, p + nl) == b'='
            && (at(x, p + nl + 1) == b'"' || at(x, p + nl + 1) == b'\'') {
            let q = x[p + nl + 1];
            let mut v = p + nl + 2;
            let mut out = Vec::new();
            while at(x, v) != 0 && at(x, v) != q && out.len() < cap.saturating_sub(1) { out.push(x[v]); v += 1; }
            return out;
        }
        p += 1;
    }
    Vec::new()
}
fn tag_is(x: &[u8], p: usize, name: &[u8]) -> bool {
    x.len() >= p + name.len() && &x[p..p + name.len()] == name && matches!(at(x, p + name.len()), b' ' | b'>' | b'/' | b'\t' | b'\n')
}
/// Text between tags, with its entities.
fn xml_text(p: &mut Para, x: &[u8], from: usize, to: usize, c: Cst) {
    let mut i = from;
    while i < to {
        if x[i] == b'&' {
            if let Some(l) = x[i..to].iter().position(|&b| b == b';') {
                let rest = &x[i..];
                let cp = if rest.starts_with(b"&amp;") { b'&' as u32 } else if rest.starts_with(b"&lt;") { b'<' as u32 } else if rest.starts_with(b"&gt;") { b'>' as u32 }
                         else if rest.starts_with(b"&quot;") { b'"' as u32 } else if rest.starts_with(b"&apos;") { b'\'' as u32 }
                         else if at(x, i + 1) == b'#' { if at(x, i + 2) == b'x' { strtoul(&x[i + 3..], 16) } else { strtoul(&x[i + 2..], 10) } }
                         else { 0 };
                if cp != 0 { p.ins_cp(cp, c); i += l + 1; continue; }
            }
            /* not an entity: the "&" stays as text (the C code stopped here and went round forever) */
            p.ins(b"&", c);
            i += 1;
            continue;
        }
        let mut j = i;
        while j < to && x[j] != b'&' { j += 1; }
        p.ins(&x[i..j], c);
        i = j;
    }
}
fn hexcol(v: &[u8]) -> u32 {
    if !v.is_empty() && !v.eq_ignore_ascii_case(b"auto") { 0xFF00_0000 | strtoul(if v[0] == b'#' { &v[1..] } else { v }, 16) } else { 0xFF00_0000 }
}
fn named_highlight(v: &[u8]) -> u32 {
    const H: [(&[u8], u32); 15] = [(b"yellow", 0xFFFF00), (b"green", 0x00FF00), (b"cyan", 0x00FFFF), (b"magenta", 0xFF00FF), (b"blue", 0x0000FF), (b"red", 0xFF0000),
        (b"darkBlue", 0x000080), (b"darkCyan", 0x008080), (b"darkGreen", 0x008000), (b"darkMagenta", 0x800080), (b"darkRed", 0x800000), (b"darkYellow", 0x808000),
        (b"darkGray", 0x808080), (b"lightGray", 0xC0C0C0), (b"black", 0x000000)];
    H.iter().find(|h| h.0 == v).map_or(0, |h| 0xFF00_0000 | h.1)
}
fn find_from(x: &[u8], from: usize, c: u8) -> Option<usize> { x[from..].iter().position(|&b| b == c).map(|i| from + i) }

/// word/document.xml of a .docx
pub fn docx(x: &[u8]) -> Doc {
    let x = cstr(x);
    let mut d = Doc::new();
    let (mut run, mut prun) = (default_style(), default_style());
    let (mut in_rpr, mut in_t, mut first_para, mut in_ppr) = (false, false, true, false);
    let mut q = 0usize;
    while q < x.len() {
        if x[q] != b'<' {
            let Some(e) = find_from(x, q, b'<') else { break };
            if in_t { xml_text(d.cur(), x, q, e, run); }
            q = e; continue;
        }
        let t = q + 1;
        let Some(end) = find_from(x, q, b'>') else { break };
        let selfclose = x[end - 1] == b'/';
        let v = |name: &[u8]| xattr(x, t, name, 64);
        if tag_is(x, t, b"w:p") {
            if !first_para { d.add(prun); }
            first_para = false;
            if selfclose { q = end + 1; continue; }
        }
        else if tag_is(x, t, b"w:pPr") { in_ppr = true; }
        else if tag_is(x, t, b"/w:pPr") { in_ppr = false; }
        else if tag_is(x, t, b"w:jc") && in_ppr { let a = v(b"w:val"); d.cur().align = match &a[..] { b"center" => 1, b"right" | b"end" => 2, b"both" => 3, _ => 0 }; }
        else if tag_is(x, t, b"w:numPr") && in_ppr { d.cur().bullet = 1; }
        else if tag_is(x, t, b"w:ind") && in_ppr {
            let mut l = v(b"w:left"); if l.is_empty() { l = v(b"w:start"); }
            let p = d.cur();
            if !l.is_empty() { p.lind = (atoi(&l) / 15) as i16; }
            let f = v(b"w:firstLine"); if !f.is_empty() { p.first = (atoi(&f) / 15) as i16; }
            let h = v(b"w:hanging"); if !h.is_empty() { p.first = (atoi(&h).wrapping_neg() / 15) as i16; }
            let r = v(b"w:right"); if !r.is_empty() { p.rind = (atoi(&r) / 15) as i16; }
        }
        else if tag_is(x, t, b"w:spacing") && in_ppr { let l = v(b"w:line"); if !l.is_empty() { d.cur().lsp = (atoi(&l).wrapping_mul(20) / 240).clamp(20, 60) as u8; } }
        else if tag_is(x, t, b"w:r") { run = default_style(); }
        else if tag_is(x, t, b"w:rPr") { in_rpr = true; }
        else if tag_is(x, t, b"/w:rPr") { in_rpr = false; }
        else if in_rpr && (tag_is(x, t, b"w:b") || tag_is(x, t, b"w:i") || tag_is(x, t, b"w:strike")) {
            let a = v(b"w:val");
            let on = (a.is_empty() || (a != b"0" && a != b"false")) as u8;
            if tag_is(x, t, b"w:b") { run.b = on; } else if tag_is(x, t, b"w:i") { run.i = on; } else { run.s = on; }
        }
        else if in_rpr && tag_is(x, t, b"w:u") { run.u = (v(b"w:val") != b"none") as u8; }
        else if in_rpr && tag_is(x, t, b"w:sz") { let a = atoi(&v(b"w:val")); if a > 0 { run.hp = a as u16; } }
        else if in_rpr && tag_is(x, t, b"w:color") { run.fg = hexcol(&v(b"w:val")); }
        else if in_rpr && tag_is(x, t, b"w:highlight") { run.bg = named_highlight(&v(b"w:val")); }
        else if in_rpr && tag_is(x, t, b"w:shd") { let f = v(b"w:fill"); if !f.is_empty() && !f.eq_ignore_ascii_case(b"auto") { run.bg = hexcol(&f); } }
        else if in_rpr && tag_is(x, t, b"w:vertAlign") { run.script = match &v(b"w:val")[..] { b"subscript" => 1, b"superscript" => 2, _ => 0 }; }
        else if in_rpr && tag_is(x, t, b"w:rFonts") { let mut f = v(b"w:ascii"); if f.is_empty() { f = v(b"w:hAnsi"); } if !f.is_empty() { run.fam = fam_from_name(&f); } }
        else if tag_is(x, t, b"w:t") { in_t = !selfclose; }
        else if tag_is(x, t, b"/w:t") { in_t = false; }
        else if tag_is(x, t, b"w:tab") && !in_ppr { d.cur().ins(b"\t", run); }
        else if tag_is(x, t, b"w:br") || tag_is(x, t, b"w:cr") { d.add(run); }
        if in_rpr { prun = run; }
        q = end + 1;
    }
    /* WordPad writes a bullet as a "• " run (no numbering part): read it back as a bullet */
    for b in d.paras.iter_mut() {
        if b.text.len() >= 4 && b.text.starts_with(b"\xe2\x80\xa2 ") {
            b.text.drain(..4); b.st.drain(..4);
            b.bullet = 1; b.lind = if b.lind > DPI / 4 { b.lind - DPI / 4 } else { 0 }; b.first = 0;
        }
    }
    d
}

#[derive(Clone)]
struct OStyle { name: Vec<u8>, c: Cst, align: u8, text: bool }
/// content.xml of an .odt, with its automatic styles (bold, italic, underline, size, colour,
/// alignment)
pub fn odt(x: &[u8]) -> Doc {
    let x = cstr(x);
    let mut d = Doc::new();
    let mut os: Vec<OStyle> = Vec::new();
    let mut stack = [default_style(); 16];
    let mut sp = 0usize;
    let mut cur = default_style();
    let mut first = true;
    let mut defining: Option<usize> = None;
    let mut q = 0usize;
    while q < x.len() {
        if x[q] != b'<' {
            let Some(e) = find_from(x, q, b'<') else { break };
            if defining.is_none() && sp > 0 { xml_text(d.cur(), x, q, e, cur); }
            q = e; continue;
        }
        let t = q + 1;
        let Some(end) = find_from(x, q, b'>') else { break };
        let selfclose = x[end - 1] == b'/';
        let v = |name: &[u8]| xattr(x, t, name, 64);
        if tag_is(x, t, b"style:style") && os.len() < 128 {
            os.push(OStyle { name: xattr(x, t, b"style:name", 40), c: default_style(), align: 0, text: false });
            defining = if selfclose { None } else { Some(os.len() - 1) };
        }
        else if tag_is(x, t, b"/style:style") { defining = None; }
        else if defining.is_some() && tag_is(x, t, b"style:text-properties") {
            let s = &mut os[defining.unwrap()];
            if v(b"fo:font-weight") == b"bold" { s.c.b = 1; }
            if v(b"fo:font-style") == b"italic" { s.c.i = 1; }
            let u = v(b"style:text-underline-style"); if !u.is_empty() && u != b"none" { s.c.u = 1; }
            let l = v(b"style:text-line-through-style"); if !l.is_empty() && l != b"none" { s.c.s = 1; }
            let sz = atoi(&v(b"fo:font-size")); if sz > 0 { s.c.hp = sz.wrapping_mul(2) as u16; }
            let col = v(b"fo:color"); if !col.is_empty() { s.c.fg = hexcol(&col); }
            let bg = v(b"fo:background-color"); if bg.first() == Some(&b'#') { s.c.bg = hexcol(&bg); }
            let f = v(b"style:font-name"); if !f.is_empty() { s.c.fam = fam_from_name(&f); }
            let pos = v(b"style:text-position"); if pos.starts_with(b"super") { s.c.script = 2; } else if pos.starts_with(b"sub") { s.c.script = 1; }
            s.text = true;
        }
        else if defining.is_some() && tag_is(x, t, b"style:paragraph-properties") {
            os[defining.unwrap()].align = match &v(b"fo:text-align")[..] { b"center" => 1, b"end" | b"right" => 2, b"justify" => 3, _ => 0 };
        }
        else if tag_is(x, t, b"text:p") || tag_is(x, t, b"text:h") {
            if !first { d.add(cur); }
            first = false;
            let name = v(b"text:style-name");
            let mut c = default_style();
            if tag_is(x, t, b"text:h") { c.b = 1; c.hp = 28; }
            for s in &os { if s.name == name { if s.text { c = s.c; } d.cur().align = s.align; } }
            if sp < 16 { stack[sp] = cur; sp += 1; }
            cur = c;
            if selfclose { sp -= 1; cur = stack[sp]; }
        }
        else if tag_is(x, t, b"/text:p") || tag_is(x, t, b"/text:h") { if sp > 0 { sp -= 1; cur = stack[sp]; } }
        else if tag_is(x, t, b"text:span") {
            let name = v(b"text:style-name");
            if sp < 16 { stack[sp] = cur; sp += 1; }
            for s in &os { if s.name == name && s.text { cur = s.c; } }
            if selfclose { sp -= 1; cur = stack[sp]; }
        }
        else if tag_is(x, t, b"/text:span") { if sp > 0 { sp -= 1; cur = stack[sp]; } }
        else if tag_is(x, t, b"text:s") && sp > 0 { let a = v(b"text:c"); let k = if a.is_empty() { 1 } else { atoi(&a) }; for _ in 0..k.clamp(0, 200) { d.cur().ins(b" ", cur); } }
        else if tag_is(x, t, b"text:tab") && sp > 0 { d.cur().ins(b"\t", cur); }
        else if tag_is(x, t, b"text:line-break") && sp > 0 { d.add(cur); }
        q = end + 1;
    }
    d
}

/// A .docx or .odt; with the format WordPad records for it (1 docx, 3 odt), or None when the
/// zip holds neither document.
pub fn read_zip(z: &[u8]) -> Option<(Doc, i32)> {
    if let Some(x) = zip_get(z, b"word/document.xml") { return Some((docx(&x), 1)); }
    zip_get(z, b"content.xml").map(|x| (odt(&x), 3))
}

// ---------------------------------------------------------------- the C interface

#[cfg(any(feature = "kernel", feature = "htmldoc"))]
mod ffi {
    use super::*;
    use alloc::boxed::Box;
    use core::ffi::c_int;

    /// wpr_para_t (src/apps/wordpad.c)
    #[repr(C)]
    pub struct WprPara { text: *const u8, st: *const Cst, n: c_int, endst: Cst, align: u8, bullet: u8, lsp: u8, lind: i16, first: i16, rind: i16 }
    pub fn layout() -> [u32; 6] {
        use core::mem::{offset_of, size_of};
        [size_of::<Cst>() as u32, offset_of!(Cst, hp) as u32, offset_of!(Cst, bg) as u32, size_of::<WprPara>() as u32, offset_of!(WprPara, endst) as u32, offset_of!(WprPara, lind) as u32]
    }
    /// kind 0: Rich Text; 1: a zip (.docx or .odt), with the format in *fmt. NULL: nothing read.
    #[no_mangle]
    pub unsafe extern "C" fn wpr_read(kind: c_int, data: *const u8, n: u32, fmt: *mut c_int) -> *mut Doc {
        let s = if data.is_null() || n == 0 { &[][..] } else { core::slice::from_raw_parts(data, n as usize) };
        let d = if kind == 0 { super::rtf(s) } else {
            match read_zip(s) { Some((d, f)) => { if !fmt.is_null() { *fmt = f; } d } None => return core::ptr::null_mut() }
        };
        Box::into_raw(Box::new(d))
    }
    #[no_mangle]
    pub unsafe extern "C" fn wpr_count(d: *mut Doc) -> c_int { let d = &*d; d.paras.len() as c_int }
    #[no_mangle]
    pub unsafe extern "C" fn wpr_para(d: *mut Doc, i: c_int, out: *mut WprPara) {
        let d = &*d;
        let p = &d.paras[i as usize];
        *out = WprPara { text: p.text.as_ptr(), st: p.st.as_ptr(), n: p.text.len() as c_int, endst: p.endst, align: p.align, bullet: p.bullet, lsp: p.lsp,
                         lind: p.lind, first: p.first, rind: p.rind };
    }
    #[no_mangle]
    pub unsafe extern "C" fn wpr_free(d: *mut Doc) { if !d.is_null() { drop(Box::from_raw(d)); } }
}
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub use ffi::layout;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rich_text() {
        let d = rtf(b"{\\rtf1{\\fonttbl{\\f0 Courier New;}}{\\colortbl;\\red255\\green0\\blue0;}\\qc\\b Bold\\b0  \\cf1 red\\'93q\\'94\\u8364?\\par Next\\par}");
        assert_eq!(d.paras.len(), 3);
        assert_eq!(d.paras[0].align, 1);
        assert_eq!(core::str::from_utf8(&d.paras[0].text).unwrap(), "Bold red\u{201c}q\u{201d}\u{20ac}");
        assert_eq!(d.paras[0].st[0].b, 1);
        assert_eq!(d.paras[0].st[5].fg, 0xFFFF0000);
        assert_eq!(d.paras[1].text, b"Next");
    }
    #[test]
    fn docx_and_odt() {
        let d = docx(b"<w:document><w:body><w:p><w:pPr><w:jc w:val=\"right\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>A &amp; B &#x41; & &nbsp;</w:t></w:r></w:p><w:p/></w:body></w:document>");
        assert_eq!(d.paras.len(), 2);
        assert_eq!((d.paras[0].align, &d.paras[0].text[..]), (2, &b"A & B A & &nbsp;"[..]));
        assert_eq!(d.paras[0].st[0].b, 1);
        let o = odt(b"<office:text><style:style style:name=\"P1\"><style:text-properties fo:font-weight=\"bold\"/></style:style><text:p text:style-name=\"P1\">Hi<text:s text:c=\"2\"/>x</text:p></office:text>");
        assert_eq!(o.paras[0].text, b"Hi  x");
        assert_eq!(o.paras[0].st[0].b, 1);
    }
}
