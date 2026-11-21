//! Linux software packages, the parts that read them (was `src/apps/pkg.c` and the gzip half
//! of `src/pkg/unpack.c`): ar, tar (ustar, GNU long names, pax) and cpio archives, Debian
//! control files, RPM headers, a whole .deb or .rpm read into a [`Package`], and the
//! pre-install check (the packages it depends on, the libraries and symbol versions its
//! programs load, their loaders and script interpreters), plus the pieces of a package the
//! installer looks at later: `.desktop` entries and `update-alternatives` lines.
//!
//! Everything here reads files from the internet, so every offset is checked against the
//! buffer; where the C code's arithmetic could wrap past a check, it is done without wrap.
//! Results otherwise match the C code's byte for byte (field sizes, truncation, which of
//! two entries wins), so the installer, its records and the package windows are unchanged.
//! Installing, removing, the package database and the menus stay in C (`src/apps/pkg.c`);
//! decompression is `src/pkg/unpack.c` (xz, Zstandard) and [`gunzip`] here.

use alloc::collections::{BTreeMap, BTreeSet};
use alloc::vec::Vec;

pub const PE_FILE: u8 = 0;
pub const PE_DIR: u8 = 1;
pub const PE_SYMLINK: u8 = 2;
pub const PE_HARDLINK: u8 = 3;

// ---------------------------------------------------------------- C string helpers

pub fn cstr(s: &[u8]) -> &[u8] { match s.iter().position(|&c| c == 0) { Some(i) => &s[..i], None => s } }
#[inline] fn at(s: &[u8], i: usize) -> u8 { s.get(i).copied().unwrap_or(0) }
/// strlcpy into a buffer of `cap` bytes: at most cap - 1 of the C string
pub fn lcpy(s: &[u8], cap: usize) -> Vec<u8> { let s = cstr(s); s[..s.len().min(cap.saturating_sub(1))].to_vec() }
/// strlcat
fn lcat(dst: &mut Vec<u8>, s: &[u8], cap: usize) { let room = cap.saturating_sub(1).saturating_sub(dst.len()); let s = cstr(s); dst.extend_from_slice(&s[..s.len().min(room)]); }
fn find(h: &[u8], from: usize, n: &[u8]) -> Option<usize> { if n.is_empty() { return Some(from); } (from..h.len()).find(|&i| h[i..].starts_with(n)) }
fn has(h: &[u8], n: &[u8]) -> bool { find(h, 0, n).is_some() }
fn rfind_byte(s: &[u8], c: u8) -> Option<usize> { s.iter().rposition(|&x| x == c) }
fn isspace(c: u8) -> bool { matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) }
/// The kernel's strtoul in base 10 (skips white space and '+'): the value's low 32 bits and
/// where it stopped.
fn strtoul10(s: &[u8], mut i: usize) -> (u32, usize) {
    while isspace(at(s, i)) { i += 1; }
    if at(s, i) == b'+' { i += 1; }
    let mut v: u32 = 0;
    loop {
        let c = at(s, i);
        let d = if c.is_ascii_digit() { (c - b'0') as u32 } else if c.is_ascii_alphabetic() { (c.to_ascii_lowercase() - b'a') as u32 + 10 } else { break };
        if d >= 10 { break; }
        v = v.wrapping_mul(10).wrapping_add(d);
        i += 1;
    }
    (v, i)
}
fn le16(d: &[u8], o: u64) -> u16 { let o = o as usize; at(d, o) as u16 | (at(d, o + 1) as u16) << 8 }
fn le32(d: &[u8], o: u64) -> u32 { let o = o as usize; (0..4).map(|k| (at(d, o + k) as u32) << (8 * k)).fold(0, |a, b| a | b) }
fn le64(d: &[u8], o: u64) -> u64 { le32(d, o) as u64 | (le32(d, o + 4) as u64) << 32 }
fn be32(d: &[u8], o: usize) -> u32 { (at(d, o) as u32) << 24 | (at(d, o + 1) as u32) << 16 | (at(d, o + 2) as u32) << 8 | at(d, o + 3) as u32 }
fn fmt_u32(v: u32) -> Vec<u8> { let mut s = Vec::new(); let mut v = v; loop { s.push(b'0' + (v % 10) as u8); v /= 10; if v == 0 { break; } } s.reverse(); s }

/// Trim spaces and tabs in front, and white space behind.
pub fn trim(s: &[u8]) -> &[u8] {
    let mut s = s;
    while let [b' ' | b'\t', r @ ..] = s { s = r; }
    while let [r @ .., b' ' | b'\t' | b'\r' | b'\n'] = s { s = r; }
    s
}
/// Append `s` to a ", "-separated list of at most cap - 1 bytes, without repeating an item.
pub fn list_add(list: &mut Vec<u8>, cap: usize, s: &[u8]) {
    let s = cstr(s);
    let l = s.len();
    if l == 0 { return; }                                // C looped for ever on an empty item
    let mut p = 0;
    while let Some(i) = find(list, p, s) {
        if (i == 0 || (i >= 2 && list[i - 2] == b',')) && (at(list, i + l) == b',' || i + l >= list.len()) { return; }
        p = i + l;
    }
    if !list.is_empty() { lcat(list, b", ", cap); }
    lcat(list, s, cap);
}
/// The value of "Field:" in an RFC 822-style block (deb control; .desktop with '='), with
/// continuation lines joined by '\n'.
pub fn field(text: &[u8], name: &[u8], sep: u8) -> Option<Vec<u8>> {
    let text = cstr(text);
    let nl = name.len();
    let mut p = 0;
    while p < text.len() {
        if text.len() >= p + nl && text[p..p + nl].eq_ignore_ascii_case(name) && at(text, p + nl) == sep {
            let v = p + nl + 1;
            let mut e = v;
            loop {
                match text[e.min(text.len())..].iter().position(|&c| c == b'\n') {
                    None => { e = text.len(); break; }
                    Some(k) => {
                        let eol = e + k;
                        if sep == b':' && (at(text, eol + 1) == b' ' || at(text, eol + 1) == b'\t') { e = eol + 1; continue; }
                        e = eol; break;
                    }
                }
            }
            return Some(trim(&text[v.min(e)..e]).to_vec());
        }
        match text[p..].iter().position(|&c| c == b'\n') { Some(k) => p += k + 1, None => break }
    }
    None
}
/// The first line of a field, cut to cap - 1 bytes.
pub fn field_line(text: &[u8], name: &[u8], cap: usize) -> Vec<u8> {
    match field(text, name, b':') {
        Some(v) => { let v = match v.iter().position(|&c| c == b'\n') { Some(i) => &v[..i], None => &v[..] }; lcpy(v, cap) }
        None => Vec::new(),
    }
}

// ---------------------------------------------------------------- paths

/// /bin, /sbin, /lib and /lib64 are /usr/bin ... (merged /usr, as the Linux layer has it),
/// unless that would not fit in cap - 1 bytes.
pub fn canon(p: &[u8], cap: usize) -> Vec<u8> {
    for m in [&b"/lib"[..], b"/lib64", b"/bin", b"/sbin"] {
        if p.starts_with(m) && (p.len() == m.len() || p[m.len()] == b'/') {
            if p.len() + 4 >= cap { return p.to_vec(); }
            let mut r = b"/usr".to_vec(); r.extend_from_slice(p); return r;
        }
    }
    p.to_vec()
}
/// A path from an archive as an absolute, normalised Linux path of at most cap - 1 bytes;
/// None for ".." or one too long.
pub fn norm_path(inp: &[u8], cap: usize) -> Option<Vec<u8>> {
    let inp = cstr(inp);
    let mut tmp = alloc::vec![b'/'];
    for part in inp.split(|&c| c == b'/').filter(|p| !p.is_empty()) {
        if part == b"." { continue; }
        if part == b".." { return None; }
        if tmp.len() > 1 { tmp.push(b'/'); }
        if tmp.len() + part.len() >= 511 { return None; }
        tmp.extend_from_slice(part);
    }
    let tmp = canon(&tmp, 512);
    if tmp.len() >= cap { return None; }
    Some(tmp)
}
/// `target` resolved from the folder of `linkpath` ('..' allowed, stops at /), at most cap - 1 bytes.
pub fn join_link(linkpath: &[u8], target: &[u8], cap: usize) -> Vec<u8> {
    let (linkpath, target) = (cstr(linkpath), cstr(target));
    let buf: Vec<u8> = if at(target, 0) == b'/' { lcpy(target, 600) } else {
        let mut dir = lcpy(linkpath, 512);
        if let Some(sl) = rfind_byte(&dir, b'/') { dir.truncate(sl); }
        let mut b = dir; b.push(b'/'); b.extend_from_slice(target); b.truncate(599); b
    };
    let mut parts: Vec<&[u8]> = Vec::new();
    for p in buf.split(|&c| c == b'/').filter(|p| !p.is_empty()) {
        if p == b".." { parts.pop(); }
        else if p != b"." && parts.len() < 64 { parts.push(&p[..p.len().min(95)]); }
    }
    let mut out = Vec::new();
    for p in &parts { lcat(&mut out, b"/", cap); lcat(&mut out, p, cap); }
    if out.is_empty() { out = lcpy(b"/", cap); }
    out
}
/// Linux path -> WRLD path (at most cap - 1 bytes): / is C:\Linux and /bin, /sbin, /lib and
/// /lib64 live under /usr.
pub fn lx_to_wrld(lp: &[u8], cap: usize) -> Vec<u8> {
    let lp = cstr(lp);
    let mut tmp = lcpy(lp, 600);
    for m in [&b"/lib"[..], b"/lib64", b"/bin", b"/sbin"] {
        if lp.starts_with(m) && (lp.len() == m.len() || lp[m.len()] == b'/') { tmp = b"/usr".to_vec(); tmp.extend_from_slice(lp); tmp.truncate(599); break; }
    }
    let mut out = b"C:\\Linux".to_vec();
    if tmp != b"/" { out.extend_from_slice(&tmp); }
    out.truncate(cap.saturating_sub(1));
    for c in out.iter_mut() { if *c == b'/' { *c = b'\\'; } }
    out
}
fn base_of(p: &[u8]) -> &[u8] { match rfind_byte(p, b'/') { Some(i) => &p[i + 1..], None => p } }

// ---------------------------------------------------------------- archives

fn tar_num(f: &[u8]) -> u64 {
    if f[0] & 0x80 != 0 { return f[1..].iter().fold(0u64, |v, &c| v << 8 | c as u64); }   // GNU base-256
    let mut v: u64 = 0;
    for &c in f { if c == 0 { break; } if (b'0'..=b'7').contains(&c) { v = v.wrapping_mul(8).wrapping_add((c - b'0') as u64); } else if c != b' ' { break; } }
    v
}
/// %.Ns: a header string of at most n bytes
fn hstr(h: &[u8], o: usize, n: usize) -> &[u8] { cstr(&h[o..(o + n).min(h.len())]) }
/// Walk a tar archive (ustar, GNU long names, pax), calling f(name, type, data, link) for
/// each member. False when a member runs past the end.
pub fn tar_walk(d: &[u8], f: &mut dyn FnMut(&[u8], u8, &[u8], &[u8])) -> bool {
    let n = d.len() as u64;
    let (mut longname, mut longlink): (Vec<u8>, Vec<u8>) = (Vec::new(), Vec::new());
    let mut off: u64 = 0;
    while off + 512 <= n {
        let h = &d[off as usize..off as usize + 512];
        if h[0] == 0 { break; }
        let size = tar_num(&h[124..136]);
        let ty = h[156];
        if size > n - off - 512 { return false; }
        let data = &d[(off + 512) as usize..(off + 512 + size) as usize];
        if ty == b'L' || ty == b'K' {                    // GNU long name / link for the next member
            let v = cstr(&data[..data.len().min(511)]).to_vec();
            if ty == b'L' { longname = v; } else { longlink = v; }
        } else if ty == b'x' {                           // pax: "LEN key=value\n" records
            let (start, end) = ((off + 512) as usize, (off + 512 + size) as usize);
            let mut p = start;
            while p < end {
                let (len, sp) = strtoul10(d, p);
                let len = len as usize;
                if len == 0 || p + len > end || at(d, sp) != b' ' { break; }
                let kv = sp + 1;
                let rec_end = p + len - 1;
                let val = |k: usize| -> Vec<u8> { let s = kv + k; if rec_end > s { cstr(&d[s..s + (rec_end - s).min(511)]).to_vec() } else { Vec::new() } };
                if d[kv.min(d.len())..].starts_with(b"path=") { longname = val(5); }
                else if d[kv.min(d.len())..].starts_with(b"linkpath=") { longlink = val(9); }
                p += len;
            }
        } else if ty != b'g' {
            let name: Vec<u8> = if !longname.is_empty() { longname.clone() }
                // POSIX ustar has a name prefix at 345; old GNU tar ("ustar  ", what dpkg writes) keeps times there
                else if &h[257..263] == b"ustar\0" && h[345] != 0 { let mut v = hstr(h, 345, 155).to_vec(); v.push(b'/'); v.extend_from_slice(hstr(h, 0, 100)); v }
                else { hstr(h, 0, 100).to_vec() };
            let link: Vec<u8> = if !longlink.is_empty() { longlink.clone() } else { hstr(h, 157, 100).to_vec() };
            f(&name, if ty != 0 { ty } else { b'0' }, data, &link);
            longname.clear(); longlink.clear();
        }
        off += 512 + ((size + 511) & !511);
    }
    true
}

/// One file of a package. `data` is an offset into the package's payload.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry { pub path: Vec<u8>, pub link: Vec<u8>, pub data: usize, pub size: u32, pub ty: u8 }

fn collect_tar(ents: &mut Vec<Entry>, nfiles: &mut i32, base: usize, name: &[u8], ty: u8, data: &[u8], link: &[u8]) {
    let Some(path) = norm_path(name, 256) else { return };
    if path == b"/" { return; }
    let mut e = Entry { path, link: Vec::new(), data: 0, size: 0, ty: PE_FILE };
    match ty {
        b'5' => e.ty = PE_DIR,
        b'2' => { e.ty = PE_SYMLINK; e.link = lcpy(link, 256); }
        b'1' => { e.ty = PE_HARDLINK; if let Some(l) = norm_path(link, 256) { e.link = l; } }
        b'0' | b'7' => { e.ty = PE_FILE; e.data = data.as_ptr() as usize - base; e.size = data.len() as u32; *nfiles += 1; }
        _ => return,                                     // devices, fifos
    }
    if ents.try_reserve(1).is_ok() { ents.push(e); }
}
/// One hex digit of a cpio header, as the C code read it (any byte gives some value).
fn hex8(s: &[u8]) -> u32 {
    s[..8].iter().fold(0u32, |v, &c| { let c = c as i32; let x = if c >= b'a' as i32 { c - b'a' as i32 + 10 } else if c >= b'A' as i32 { c - b'A' as i32 + 10 } else { c - b'0' as i32 }; v << 4 | x as u32 })
}
/// cpio "newc" (070701/070702), the payload of an .rpm. False when it is damaged.
fn cpio_walk(d: &[u8], ents: &mut Vec<Entry>, nfiles: &mut i32) -> bool {
    let n = d.len() as u64;
    let mut off: u64 = 0;
    while off + 110 <= n {
        let h = &d[off as usize..];
        if &h[..5] != b"07070" { return false; }
        let (mode, size, namesize, nlink, ino) = (hex8(&h[14..]), hex8(&h[54..]), hex8(&h[94..]), hex8(&h[38..]), hex8(&h[6..]));
        if off + 110 + namesize as u64 > n { return false; }
        let data_off = (off + 110 + namesize as u64 + 3) & !3;
        if data_off + size as u64 > n { return false; }
        let name = cstr(&d[(off + 110) as usize..]);
        if name == b"TRAILER!!!" { break; }
        if let Some(path) = norm_path(name, 256) {
            if path != b"/" {
                let mut e = Entry { path, link: Vec::new(), data: 0, size: 0, ty: PE_FILE };
                let fmt = mode & 0o170000;
                let mut keep = true;
                if fmt == 0o040000 { e.ty = PE_DIR; }
                else if fmt == 0o120000 { e.ty = PE_SYMLINK; e.link = cstr(&d[data_off as usize..data_off as usize + (size as usize).min(255)]).to_vec(); }
                else if fmt == 0o100000 {
                    e.ty = PE_FILE; e.data = data_off as usize; e.size = size; *nfiles += 1;
                    // hard links: the data comes with the last name of the group; earlier ones are empty
                    let mut ln = b"#ino:".to_vec(); ln.extend_from_slice(&fmt_u32(ino));
                    if nlink > 1 && size == 0 { e.ty = PE_HARDLINK; e.link = ln; *nfiles -= 1; }
                    else if nlink > 1 { e.link = ln; }
                } else { keep = false; }
                if keep && ents.try_reserve(1).is_ok() { ents.push(e); }
            }
        }
        off = (data_off + size as u64 + 3) & !3;
    }
    true
}
struct ArMember<'a> { data: &'a [u8], name: Vec<u8> }
fn ar_members(d: &[u8], max: usize) -> Option<Vec<ArMember<'_>>> {
    if d.len() < 8 || &d[..8] != b"!<arch>\n" { return None; }
    let n = d.len() as u64;
    let mut off: u64 = 8;
    let mut m = Vec::new();
    while off + 60 <= n && m.len() < max {
        let h = &d[off as usize..off as usize + 60];
        let size = strtoul10(cstr(&h[48..58]), 0).0 as u64;
        if off + 60 + size > n { return None; }
        // 16 bytes, trailing spaces and slashes cut (stopping at a NUL), as a C string
        let mut raw = h[..16].to_vec();
        let mut i = 16;
        while i > 0 && (raw[i - 1] == b' ' || raw[i - 1] == b'/') { raw[i - 1] = 0; i -= 1; }
        let name = cstr(&raw).to_vec();
        m.push(ArMember { data: &d[(off + 60) as usize..(off + 60 + size) as usize], name });
        off += 60 + size + (size & 1);
    }
    Some(m)
}

// ---------------------------------------------------------------- packages

/// The compression a buffer starts with (`unpack_kind`).
pub fn unpack_kind(s: &[u8]) -> i32 {
    let n = s.len();
    if n >= 3 && s[0] == 0x1F && s[1] == 0x8B && s[2] == 8 { return 1; }
    if n >= 6 && &s[..6] == b"\xFD7zXZ\0" { return 2; }
    if n >= 4 && &s[..4] == b"\x28\xB5\x2F\xFD" { return 3; }
    if n >= 3 && &s[..3] == b"BZh" { return 4; }
    if n >= 13 && s[0] == 0x5D && s[1] == 0 && s[2] == 0 { return 5; }
    0
}
pub fn unpack_kind_name(k: i32) -> &'static [u8] {
    match k { 0 => b"none", 1 => b"gzip", 2 => b"xz", 3 => b"zstd", 4 => b"bzip2", 5 => b"lzma", _ => b"?" }
}
/// An unpacked buffer, wherever it was allocated (the kernel hands C's straight to pkg_t).
pub trait Bytes {
    fn bytes(&self) -> &[u8];
    /// Give up the buffer to C (kfree); None if it cannot be.
    fn leak(self: alloc::boxed::Box<Self>) -> Option<*mut u8> { None }
}
impl Bytes for Vec<u8> { fn bytes(&self) -> &[u8] { self } }
/// Decompression (src/pkg/unpack.c's unpack_all): the whole buffer, at most `max` bytes,
/// or the reason it failed.
pub trait Unpack { fn unpack(&self, d: &[u8], max: u32) -> Result<alloc::boxed::Box<dyn Bytes>, Vec<u8>>; }

/// A package read whole (`pkg_t`). Text fields keep the C field sizes.
#[derive(Default)]
pub struct Package {
    pub fmt: u8,
    pub name: Vec<u8>, pub version: Vec<u8>, pub arch: Vec<u8>,
    pub summary: Vec<u8>, pub maint: Vec<u8>, pub homepage: Vec<u8>, pub section: Vec<u8>, pub license: Vec<u8>,
    pub desc: Option<Vec<u8>>, pub depends: Option<Vec<u8>>, pub provides: Option<Vec<u8>>,
    pub inst_kb: u32,
    pub comp: Vec<u8>,
    pub scripts: Vec<u8>,
    pub postinst: Option<Vec<u8>>,
    pub payload: Option<alloc::boxed::Box<dyn Bytes>>,
    pub ents: Vec<Entry>,
    pub nfiles: i32,
    path_idx: BTreeMap<Vec<u8>, usize>,
    base_idx: BTreeSet<Vec<u8>>,
}
fn err<T>(m: &[u8]) -> Result<T, Vec<u8>> { Err(lcpy(m, 200)) }
fn cat(parts: &[&[u8]]) -> Vec<u8> { let mut v = Vec::new(); for p in parts { v.extend_from_slice(p); } v }

impl Package {
    /// Read a .deb or .rpm. `limit` bounds the unpacked files; Err is the message to show.
    pub fn load(d: &[u8], u: &dyn Unpack, limit: u32) -> Result<Package, Vec<u8>> {
        let mut p = Package::default();
        if d.len() >= 8 && &d[..8] == b"!<arch>\n" { p.fmt = b'd'; p.load_deb(d, u, limit)?; }
        else if d.len() >= 4 && be32(d, 0) == 0xEDAB_EEDB { p.fmt = b'r'; p.load_rpm(d, u, limit)?; }
        else { return err(b"not a package"); }
        p.build_index();
        Ok(p)
    }
    fn load_deb(&mut self, d: &[u8], u: &dyn Unpack, limit: u32) -> Result<(), Vec<u8>> {
        let m = ar_members(d, 8).unwrap_or_default();
        if m.len() < 3 || m[0].name != b"debian-binary" { return err(b"This is not a Debian package (the ar archive is not valid)."); }
        if m[0].data.len() < 3 || m[0].data[0] != b'2' {
            let st = m[0].data.as_ptr() as usize - d.as_ptr() as usize;
            let v = cstr(&d[st..(st + 4).min(d.len())]);
            return err(&cat(&[b"This Debian package uses format ", v, b", which is not supported (2.0 is)."]));
        }
        let (mut ctl, mut dat) = (None, None);
        for x in &m[1..] {
            if x.name.starts_with(b"control.tar") { ctl = Some(x.data); }
            else if x.name.starts_with(b"data.tar") { dat = Some(x.data); }
        }
        let (Some(ctl), Some(dat)) = (ctl, dat) else { return err(b"The package has no control or data archive.") };
        let ctb = match u.unpack(ctl, 16 << 20) { Ok(c) => c, Err(e) => return err(&cat(&[b"Could not read the package information: ", &e, b"."])) };
        let ct = ctb.bytes();
        let (mut control, mut postinst, mut scripts): (Option<Vec<u8>>, Option<Vec<u8>>, Vec<u8>) = (None, None, Vec::new());
        tar_walk(&ct, &mut |name, _ty, data, _link| {
            let b = base_of(name);
            if b == b"control" && control.is_none() { control = Some(cstr(data).to_vec()); }
            else if matches!(b, b"preinst" | b"postinst" | b"prerm" | b"postrm") {
                list_add(&mut scripts, 96, b);
                if b == b"postinst" && postinst.is_none() && data.len() < 1 << 20 { postinst = Some(cstr(data).to_vec()); }
            }
        });
        let Some(c) = control else { return err(b"The package has no control file.") };
        self.scripts = scripts;
        self.postinst = postinst;
        self.name = field_line(&c, b"Package", 64);
        self.version = field_line(&c, b"Version", 96);
        self.arch = field_line(&c, b"Architecture", 24);
        self.maint = field_line(&c, b"Maintainer", 128);
        self.homepage = field_line(&c, b"Homepage", 160);
        self.section = field_line(&c, b"Section", 48);
        self.inst_kb = strtoul10(&field_line(&c, b"Installed-Size", 24), 0).0;
        if let Some(desc) = field(&c, b"Description", b':') {
            let (first, body) = match desc.iter().position(|&x| x == b'\n') { Some(i) => (&desc[..i], &desc[i + 1..]), None => (&desc[..], &b""[..]) };
            self.summary = lcpy(first, 160);
            // the long description: one space of indent; " ." is an empty line
            let mut o = Vec::new();
            let mut q = 0;
            while q < body.len() {
                let e = body[q..].iter().position(|&x| x == b'\n');
                let line = &body[q..q + e.unwrap_or(body.len() - q)];
                let mut s = line;
                while let [b' ' | b'\t', r @ ..] = s { s = r; }
                if s == b"." { s = b""; }
                o.extend_from_slice(s); o.push(b'\n');
                q += e.map_or(body.len() - q, |k| k + 1);
            }
            self.desc = Some(o);
        }
        let (dep, pre) = (field(&c, b"Depends", b':'), field(&c, b"Pre-Depends", b':'));
        let mut deps = Vec::new();
        if let Some(p) = &pre { deps.extend_from_slice(p); }
        if let Some(dd) = &dep { if pre.is_some() { deps.extend_from_slice(b", "); } deps.extend_from_slice(dd); }
        for x in deps.iter_mut() { if *x == b'\n' { *x = b' '; } }
        self.depends = Some(deps);
        self.provides = field(&c, b"Provides", b':');
        if self.name.is_empty() { return err(b"The package has no name."); }
        self.comp = lcpy(unpack_kind_name(unpack_kind(dat)), 8);
        let payload = match u.unpack(dat, limit) { Ok(p) => p, Err(e) => return err(&cat(&[b"Could not unpack the files: ", &e, b"."])) };
        let pl = payload.bytes();
        let base = pl.as_ptr() as usize;
        let (mut ents, mut nfiles) = (Vec::new(), 0);
        let ok = tar_walk(pl, &mut |name, ty, data, link| collect_tar(&mut ents, &mut nfiles, base, name, ty, data, link));
        self.payload = Some(payload);
        self.ents = ents; self.nfiles = nfiles;
        if !ok { return err(b"The file archive in the package is damaged."); }
        Ok(())
    }
    fn load_rpm(&mut self, d: &[u8], u: &dyn Unpack, limit: u32) -> Result<(), Vec<u8>> {
        let n = d.len();
        if n < 96 + 16 || be32(d, 0) != 0xEDAB_EEDB { return err(b"This is not an RPM package."); }
        if d[6] == 0 && d[7] == 1 { return err(b"This is a source RPM (.src.rpm): it holds source code to build, not a program to install."); }
        let Some((_sig, sl)) = Rpm::header(&d[96..]) else { return err(b"The RPM signature header is damaged.") };
        let hoff = 96 + ((sl as usize + 7) & !7);
        let Some((h, hl)) = (if hoff < n { Rpm::header(&d[hoff..]) } else { None }) else { return err(b"The RPM header is damaged.") };
        if let Some(s) = h.str(1000, 0) { self.name = lcpy(s, 64); }
        let (ver, rel) = (h.str(1001, 0), h.str(1002, 0));
        let epoch = h.int(1003, 0);
        let mut v = Vec::new();
        if epoch != 0 { v.extend_from_slice(&fmt_u32(epoch as u32)); v.push(b':'); }
        v.extend_from_slice(ver.unwrap_or(b"?")); v.push(b'-'); v.extend_from_slice(rel.unwrap_or(b"?"));
        self.version = lcpy(&v, 96);
        if let Some(s) = h.str(1022, 0) { self.arch = lcpy(s, 24); }
        if let Some(s) = h.str(1004, 0) { self.summary = lcpy(s, 160); }
        if let Some(s) = h.str(1005, 0) { self.desc = Some(s.to_vec()); }
        if let Some(s) = h.str(1015, 0).or_else(|| h.str(1011, 0)) { self.maint = lcpy(s, 128); }
        if let Some(s) = h.str(1020, 0) { self.homepage = lcpy(s, 160); }
        if let Some(s) = h.str(1016, 0) { self.section = lcpy(s, 48); }
        if let Some(s) = h.str(1014, 0) { self.license = lcpy(s, 64); }
        let mut size = h.int(5009, 0);
        if size == 0 { size = h.int(1009, 0); }
        self.inst_kb = (size.wrapping_add(1023) / 1024) as u32;
        for (tag, name) in [(1023u32, &b"%pre"[..]), (1024, b"%post"), (1025, b"%preun"), (1026, b"%postun")] {
            if h.count(tag) != 0 { list_add(&mut self.scripts, 96, name); }
        }
        if let Some(s) = h.str(1024, 0) { self.postinst = Some(s.to_vec()); }
        // requirements, without rpm's own (rpmlib(...)) and those the loader meets (rtld(...));
        // the strings of a tag are consecutive, so the first missing one ends the list
        let reqs: Vec<&[u8]> = (0..h.count(1049)).map_while(|i| h.str(1049, i)).collect();
        let cap = 64 + reqs.iter().map(|r| r.len() + 2).sum::<usize>();
        let mut deps = Vec::new();
        for (i, r) in reqs.iter().enumerate() {
            if r.starts_with(b"rpmlib(") || r.starts_with(b"rtld(") || r.starts_with(b"config(") { continue; }
            if h.int(1048, i as u32) & (1 << 24) != 0 { continue; }   // RPMSENSE_RPMLIB
            list_add(&mut deps, cap, r);
        }
        self.depends = Some(deps);
        let provs: Vec<&[u8]> = (0..h.count(1047)).map_while(|i| h.str(1047, i)).collect();
        let cap = 16 + provs.iter().map(|r| r.len() + 2).sum::<usize>();
        let mut pv = Vec::new();
        for r in &provs { list_add(&mut pv, cap, r); }
        self.provides = Some(pv);
        if self.name.is_empty() { return err(b"The package has no name."); }
        if let Some(pf) = h.str(1124, 0) { if pf != b"cpio" { return err(&cat(&[b"The files are in a ", pf, b" archive, which is not supported (cpio is)."])); } }
        let pay = &d[hoff + hl as usize..];
        self.comp = lcpy(unpack_kind_name(unpack_kind(pay)), 8);
        self.payload = Some(match u.unpack(pay, limit) { Ok(p) => p, Err(e) => return err(&cat(&[b"Could not unpack the files: ", &e, b"."])) });
        let pl = self.payload();
        if pl.len() >= 6 && &pl[..5] == b"07070" {
            let (mut ents, mut nfiles) = (Vec::new(), 0);
            let ok = cpio_walk(pl, &mut ents, &mut nfiles);
            self.ents = ents; self.nfiles = nfiles;
            if !ok { return err(b"The file archive in the package is damaged."); }
        } else { return err(b"The files are not in a cpio archive this installer can read."); }
        // hard links in cpio: point each empty name at the member that carries the data
        let mut by_ino: BTreeMap<Vec<u8>, Vec<u8>> = BTreeMap::new();
        for e in &self.ents { if e.ty == PE_FILE && !e.link.is_empty() { by_ino.entry(e.link.clone()).or_insert_with(|| e.path.clone()); } }
        for e in self.ents.iter_mut() {
            if e.ty == PE_HARDLINK && e.link.starts_with(b"#ino:") { if let Some(t) = by_ino.get(&e.link) { e.link = t.clone(); } }
        }
        for e in self.ents.iter_mut() { if e.ty == PE_FILE { e.link.clear(); } }
        Ok(())
    }
    fn build_index(&mut self) {
        for (i, e) in self.ents.iter().enumerate() {
            self.path_idx.entry(e.path.clone()).or_insert(i);
            if e.ty != PE_DIR { self.base_idx.insert(base_of(&e.path).to_vec()); }
        }
    }
    pub fn payload(&self) -> &[u8] { self.payload.as_deref().map_or(&[], |p| p.bytes()) }
    pub fn data(&self, e: &Entry) -> &[u8] { &self.payload()[e.data..e.data + e.size as usize] }
}
/// An RPM header structure: the index entries and the data store.
pub struct Rpm<'a> { idx: &'a [u8], store: &'a [u8] }
impl<'a> Rpm<'a> {
    fn header(d: &'a [u8]) -> Option<(Rpm<'a>, u32)> {
        if d.len() < 16 || d[0] != 0x8E || d[1] != 0xAD || d[2] != 0xE8 { return None; }
        let (n, sl) = (be32(d, 8) as u64, be32(d, 12) as u64);
        if n > 100_000 || 16 + n * 16 + sl > d.len() as u64 { return None; }
        let (n, sl) = (n as usize, sl as usize);
        Some((Rpm { idx: &d[16..16 + n * 16], store: &d[16 + n * 16..16 + n * 16 + sl] }, (16 + n * 16 + sl) as u32))
    }
    /// (type, offset, count) of the first entry with that tag, if its offset is in the store
    fn find(&self, tag: u32) -> Option<(u32, u32, u32)> {
        let e = self.idx.chunks_exact(16).find(|e| be32(e, 0) == tag)?;
        let (t, o, c) = (be32(e, 4), be32(e, 8), be32(e, 12));
        if (o as usize) < self.store.len() { Some((t, o, c)) } else { None }
    }
    fn count(&self, tag: u32) -> u32 { self.find(tag).map_or(0, |x| x.2) }
    /// The k-th string of a STRING / STRING_ARRAY / I18NSTRING tag.
    pub fn str(&self, tag: u32, k: u32) -> Option<&'a [u8]> {
        let (t, o, c) = self.find(tag)?;
        if !(t == 6 || t == 8 || t == 9) || k >= c { return None; }
        let mut s = &self.store[o as usize..];
        for _ in 0..k { let z = s.iter().position(|&x| x == 0)?; s = &s[z + 1..]; }
        let z = s.iter().position(|&x| x == 0)?;
        Some(&s[..z])
    }
    fn int(&self, tag: u32, k: u32) -> u64 {
        let Some((t, o, c)) = self.find(tag) else { return 0 };
        if k >= c { return 0; }
        let (s, o, k) = (self.store, o as u64, k as u64);
        let rd = |p: u64| be32(s, p as usize);
        if t == 4 && o + (k + 1) * 4 <= s.len() as u64 { return rd(o + k * 4) as u64; }
        if t == 5 && o + (k + 1) * 8 <= s.len() as u64 { return (rd(o + k * 8) as u64) << 32 | rd(o + k * 8 + 4) as u64; }
        if t == 3 && o + (k + 1) * 2 <= s.len() as u64 { return (s[(o + k * 2) as usize] as u64) << 8 | s[(o + k * 2 + 1) as usize] as u64; }
        0
    }
}

// ---------------------------------------------------------------- checking a package

/// A package as the check sees it: the C pkg_t in the kernel, [`Package`] on the host.
pub trait PkgView {
    fn fmt(&self) -> u8;
    fn depends(&self) -> Option<&[u8]>;
    fn len(&self) -> usize;
    fn path(&self, i: usize) -> &[u8];
    fn ty(&self, i: usize) -> u8;
    fn data(&self, i: usize) -> &[u8];
    /// The entry with this (already canonical) path, the first one if there are two.
    fn find(&self, path: &[u8]) -> Option<usize>;
    fn has_basename(&self, base: &[u8]) -> bool;
}
impl PkgView for Package {
    fn fmt(&self) -> u8 { self.fmt }
    fn depends(&self) -> Option<&[u8]> { self.depends.as_deref() }
    fn len(&self) -> usize { self.ents.len() }
    fn path(&self, i: usize) -> &[u8] { &self.ents[i].path }
    fn ty(&self, i: usize) -> u8 { self.ents[i].ty }
    fn data(&self, i: usize) -> &[u8] { Package::data(self, &self.ents[i]) }
    fn find(&self, path: &[u8]) -> Option<usize> { self.path_idx.get(path).copied() }
    fn has_basename(&self, base: &[u8]) -> bool { self.base_idx.contains(base) }
}
/// What is installed: a Linux path is missing (None), a folder (Some(None)) or a file.
pub trait Sys {
    fn lookup(&self, lpath: &[u8]) -> Option<Option<&[u8]>>;
    /// A Debian dependency met by an installed package, something one provides, or the OS image.
    fn dep_met(&self, name: &[u8]) -> bool;
    fn yield_now(&self) {}
}
/// What a package needs that is not here (`pkg_check_t`).
#[derive(Default, Debug, PartialEq, Eq)]
pub struct Check {
    pub missing_pkgs: Vec<u8>, pub missing_libs: Vec<u8>, pub missing_vers: Vec<u8>, pub missing_interp: Vec<u8>, pub programs: Vec<u8>,
    pub foreign_elf: bool, pub ok: bool,
}
fn find_entry(p: &dyn PkgView, path: &[u8]) -> Option<usize> { let c = canon(&lcpy(path, 300), 300); p.find(&c) }
/// A library's data (empty for a link in the package): in the package, or installed.
fn find_lib<'a>(p: &'a dyn PkgView, sys: &'a dyn Sys, name: &[u8], runpath: Option<&[u8]>, origin: &[u8]) -> Option<&'a [u8]> {
    if let Some(rp) = runpath.filter(|r| !r.is_empty()) {
        let rp = lcpy(rp, 512);
        let mut segs: Vec<&[u8]> = rp.split(|&c| c == b':').collect();
        if segs.len() > 1 && segs.last().map_or(false, |s| s.is_empty()) { segs.pop(); }   // "a:" ends after a
        for dir0 in segs {
            let dir: Vec<u8> = if dir0.starts_with(b"$ORIGIN") { lcpy(&cat(&[origin, &dir0[7..]]), 512) }
                else if dir0.starts_with(b"${ORIGIN}") { lcpy(&cat(&[origin, &dir0[9..]]), 512) }
                else { lcpy(dir0, 512) };
            let mut full = cat(&[&dir, b"/", name]); full.truncate(599);
            let lp = join_link(b"/x", &full, 512);
            if let Some(i) = find_entry(p, &lp) { if p.ty(i) == PE_FILE { return Some(p.data(i)); } }
            if let Some(Some(d)) = sys.lookup(&lp) { return Some(d); }
        }
    }
    for dir in [&b"/usr/lib/x86_64-linux-gnu"[..], b"/usr/lib64", b"/usr/lib", b"/usr/local/lib", b"/usr/lib/x86_64-linux-gnu/pulseaudio"] {
        let mut lp = cat(&[dir, b"/", name]); lp.truncate(511);
        let mut e = find_entry(p, &lp);
        if e.is_none() && lp.starts_with(b"/usr/lib") { let mut alt = cat(&[b"/lib", &lp[8..]]); alt.truncate(519); e = find_entry(p, &alt); }
        if let Some(i) = e { return Some(if p.ty(i) == PE_FILE { p.data(i) } else { &[] }); }   // a link in the package: present
        if let Some(Some(d)) = sys.lookup(&lp) { return Some(d); }
    }
    if p.has_basename(name) { return Some(&[]); }
    None
}
fn mem_has(d: &[u8], s: &[u8]) -> bool { let mut k = s.to_vec(); k.push(0); d.windows(k.len()).any(|w| w == &k[..]) }
fn exists(p: &dyn PkgView, sys: &dyn Sys, path: &[u8]) -> bool { find_entry(p, path).is_some() || sys.lookup(path).is_some() }
/// One ELF file: its loader, the libraries it needs and their symbol versions.
fn check_elf(p: &dyn PkgView, sys: &dyn Sys, path: &[u8], d: &[u8], c: &mut Check, seen: &mut BTreeSet<Vec<u8>>) {
    let n = d.len() as u64;
    if n < 64 || &d[..4] != b"\x7FELF" { return; }
    if d[4] != 2 || d[5] != 1 || le16(d, 18) != 62 { c.foreign_elf = true; return; }   // not x86-64
    let phoff = le64(d, 32);
    let (phentsize, phnum) = (le16(d, 54) as u64, le16(d, 56) as u64);
    if phoff.checked_add(phnum * phentsize).map_or(true, |e| e > n) || phentsize < 56 { return; }
    let (mut dyn_off, mut dyn_sz) = (0u64, 0u64);
    let mut loads: Vec<(u64, u64, u64)> = Vec::new();
    for i in 0..phnum {
        let ph = phoff + i * phentsize;
        let ty = le32(d, ph);
        let (off, va, fsz) = (le64(d, ph + 8), le64(d, ph + 16), le64(d, ph + 32));
        if ty == 1 && loads.len() < 16 { loads.push((va, off, fsz)); }
        else if ty == 2 { dyn_off = off; dyn_sz = fsz; }
        else if ty == 3 && off.checked_add(fsz).map_or(false, |e| e <= n) && fsz > 1 && fsz < 256 {   // PT_INTERP
            let ip = cstr(&d[off as usize..(off + fsz - 1) as usize]);
            if !exists(p, sys, ip) { list_add(&mut c.missing_interp, 256, ip); }
        }
    }
    if dyn_off == 0 || dyn_off.checked_add(dyn_sz).map_or(true, |e| e > n) { return; }
    let va2off = |a: u64| -> u64 { loads.iter().find(|l| a >= l.0 && a < l.0.wrapping_add(l.2)).map_or(0, |l| a.wrapping_sub(l.0).wrapping_add(l.1)) };
    let (mut strtab, mut strsz, mut runpath, mut verneed, mut verneednum, mut has_rp) = (0u64, 0u64, 0u64, 0u64, 0u64, false);
    let mut needed: Vec<u64> = Vec::new();
    let mut o = dyn_off;
    while o + 16 <= dyn_off + dyn_sz {
        let (tag, val) = (le64(d, o) as i64, le64(d, o + 8));
        if tag == 0 { break; }
        match tag {
            1 => { if needed.len() < 64 { needed.push(val); } }
            5 => strtab = val,
            10 => strsz = val,
            29 | 15 => { runpath = val; has_rp = true; }
            0x6fff_fffe => verneed = val,
            0x6fff_ffff => verneednum = val,
            _ => {}
        }
        o += 16;
    }
    let so = va2off(strtab);
    if so == 0 || so.checked_add(strsz).map_or(true, |e| e > n) { return; }
    let strs = &d[so as usize..(so + strsz) as usize];
    let st = |i: u64| -> &[u8] { if i < strsz { let s = &strs[i as usize..]; match s.iter().position(|&x| x == 0) { Some(z) => &s[..z], None => b"" } } else { b"" } };
    let mut origin = lcpy(path, 256);
    if let Some(sl) = rfind_byte(&origin, b'/') { origin.truncate(sl); }
    let rp = if has_rp { Some(st(runpath)) } else { None };
    for &nd in &needed {
        let lib = st(nd);
        if !lib.is_empty() && find_lib(p, sys, lib, rp, &origin).is_none() { list_add(&mut c.missing_libs, 512, lib); }
    }
    // symbol versions: each library must define the versions the program asks for
    let mut vo = if verneed != 0 { va2off(verneed) } else { 0 };
    let mut k = 0u64;
    while vo != 0 && k < verneednum && vo + 16 <= n {
        let (cnt, file, aux, next) = (le16(d, vo + 2), le32(d, vo + 4), le32(d, vo + 8), le32(d, vo + 12));
        let lib = st(file as u64);
        let ld = if lib.is_empty() { None } else { find_lib(p, sys, lib, rp, &origin) };
        let mut ao = vo + aux as u64;
        let mut a = 0;
        while a < cnt && ld.map_or(false, |l| !l.is_empty()) && ao + 16 <= n {
            let (name, anext) = (le32(d, ao + 8), le32(d, ao + 12));
            let v = st(name as u64);
            let mut key = cat(&[lib, b"|", v]); key.truncate(159);
            if !v.is_empty() && !seen.contains(&key) {
                seen.insert(key);
                if !mem_has(ld.unwrap_or(&[]), v) { let mut b = cat(&[v, b" (", lib, b")"]); b.truncate(95); list_add(&mut c.missing_vers, 512, &b); }
            }
            if anext == 0 { break; }
            ao += anext as u64;
            a += 1;
        }
        if next == 0 { break; }
        vo += next as u64;
        k += 1;
    }
}
fn is_bin_dir(path: &[u8]) -> bool {
    [&b"/usr/bin/"[..], b"/usr/games/", b"/usr/sbin/", b"/usr/local/bin/", b"/bin/", b"/sbin/"].iter()
        .any(|d| path.starts_with(d) && !path[d.len()..].contains(&b'/'))
}
/// The pre-install check: dependencies, libraries, versions, interpreters, the commands it adds.
pub fn check(p: &dyn PkgView, sys: &dyn Sys) -> Check {
    let mut c = Check::default();
    if let Some(deps) = p.depends().filter(|d| !d.is_empty()) {
        for g in cstr(deps).split(|&x| x == b',') {
            let (mut met, mut first) = (false, Vec::new());
            for a in lcpy(g, 256).split(|&x| x == b'|') {
                let mut nm = trim(&lcpy(a, 96)).to_vec();
                if let Some(i) = nm.iter().position(|&x| x == b'(') { nm.truncate(i); nm = trim(&nm).to_vec(); }
                if let Some(i) = nm.iter().position(|&x| x == b' ') { nm.truncate(i); }
                if first.is_empty() { first = nm.clone(); }
                met = if p.fmt() == b'r' {
                    if at(&nm, 0) == b'/' { exists(p, sys, &nm) }
                    else if has(&nm, b".so") { find_lib(p, sys, &nm, None, b"/").is_some() }
                    else { true }                        // package names differ between distributions
                } else { !nm.is_empty() && sys.dep_met(&nm) };
                if met { break; }
            }
            if !met && !first.is_empty() { list_add(&mut c.missing_pkgs, 512, &first); }
        }
    }
    let mut checked = 0;
    let mut seen = BTreeSet::new();
    for i in 0..p.len() {
        if checked >= 400 { break; }
        let ty = p.ty(i);
        if ty != PE_FILE && ty != PE_SYMLINK { continue; }
        let path = p.path(i);
        let bin = is_bin_dir(path);
        if bin { list_add(&mut c.programs, 512, base_of(path)); }
        let d = p.data(i);
        if ty != PE_FILE || d.len() < 4 { continue; }
        if &d[..4] == b"\x7FELF" {
            check_elf(p, sys, path, d, &mut c, &mut seen);
            checked += 1;
            if checked & 15 == 0 { sys.yield_now(); }
        } else if bin && d[0] == b'#' && d[1] == b'!' {  // a script: is its interpreter here?
            let line: Vec<u8> = d[2..].iter().take_while(|&&x| x != b'\n').take(127).copied().collect();
            let line = trim(cstr(&line)).to_vec();
            let (cmd, arg) = match line.iter().position(|&x| x == b' ') { Some(i) => (&line[..i], Some(&line[i + 1..])), None => (&line[..], None) };
            let interp = match arg {
                Some(arg) if cmd == b"/usr/bin/env" => { let a = match arg.iter().position(|&x| x == b' ') { Some(i) => &arg[..i], None => arg }; let mut v = cat(&[b"/usr/bin/", a]); v.truncate(159); v }
                _ => lcpy(cmd, 160),
            };
            if at(&interp, 0) == b'/' && !exists(p, sys, &interp) { list_add(&mut c.missing_interp, 256, &interp); }
        }
    }
    c.ok = c.missing_pkgs.is_empty() && c.missing_libs.is_empty() && c.missing_vers.is_empty() && c.missing_interp.is_empty() && !c.foreign_elf;
    c
}

// ---------------------------------------------------------------- what the installer reads

/// A `.desktop` file's [Desktop Entry]: (name, command without field codes, terminal), or
/// None for one that is not a visible application.
pub fn desktop_entry(file: &[u8]) -> Option<(Vec<u8>, Vec<u8>, bool)> {
    let t = cstr(file);
    let start = find(t, 0, b"[Desktop Entry]").unwrap_or(0);
    let mut body = &t[start..];
    if body.len() > 1 { if let Some(k) = find(body, 1, b"\n[") { body = &body[..k + 1]; } }
    let (name, exec) = (field(body, b"Name", b'=')?, field(body, b"Exec", b'=')?);
    let term = field(body, b"Terminal", b'=');
    let nod = field(body, b"NoDisplay", b'=');
    let ty = field(body, b"Type", b'=');
    if nod.as_deref().map_or(false, |v| v.eq_ignore_ascii_case(b"true")) || ty.as_deref().map_or(false, |v| v != b"Application") { return None; }
    // drop the field codes (%f %u %F %U %i %c %k)
    let mut cmd = Vec::new();
    let mut q = 0;
    while q < exec.len() && cmd.len() < 255 {
        if exec[q] == b'%' && q + 1 < exec.len() { q += 1; if exec[q] == b'%' { cmd.push(b'%'); } q += 1; continue; }
        cmd.push(exec[q]); q += 1;
    }
    let cmd = trim(&cmd).to_vec();
    Some((name, cmd, term.as_deref().map_or(false, |v| v.eq_ignore_ascii_case(b"true"))))
}
/// Split a command line into words (double and single quotes, backslashes), at most max - 1.
pub fn split_words(s: &[u8], max: usize) -> Vec<Vec<u8>> {
    let s = cstr(s);
    let mut out = Vec::new();
    let mut i = 0;
    while i < s.len() && out.len() < max - 1 {
        while at(s, i) == b' ' { i += 1; }
        if i >= s.len() { break; }
        let (mut w, mut q) = (Vec::new(), 0u8);
        while i < s.len() && (q != 0 || s[i] != b' ') {
            if q == 0 && (s[i] == b'"' || s[i] == b'\'') { q = s[i]; i += 1; continue; }
            if q != 0 && s[i] == q { q = 0; i += 1; continue; }
            if s[i] == b'\\' && i + 1 < s.len() && q != b'\'' { i += 1; }
            w.push(s[i]); i += 1;
        }
        if i < s.len() { i += 1; }
        out.push(w);
    }
    out
}
/// The (link, target) pairs of a setup script's `update-alternatives --install / --slave`
/// lines, both absolute and canonical (at most 299 bytes).
pub fn alternatives(script: &[u8]) -> Vec<(Vec<u8>, Vec<u8>)> {
    let sc = cstr(script);
    let mut out = Vec::new();
    let mut q = 0;
    while let Some(k) = find(sc, q, b"update-alternatives") {
        // the whole command: continuation lines joined
        let mut cmd = Vec::new();
        let mut r = k + 19;
        while r < sc.len() && cmd.len() < 1023 {
            if sc[r] == b'\\' && at(sc, r + 1) == b'\n' { r += 2; cmd.push(b' '); continue; }
            if matches!(sc[r], b'\n' | b';' | b'|' | b'&') { break; }
            cmd.push(sc[r]); r += 1;
        }
        q = r;
        let w = split_words(&cmd, 48);
        let n = w.len();
        let mut i = 0;
        while i < n {
            let args = if w[i] == b"--install" { 4 } else if w[i] == b"--slave" { 3 } else { 0 };
            if args == 0 || i + 3 >= n { i += 1; continue; }
            let (link, path) = (&w[i + 1], &w[i + 3]);
            i += args + 1;
            if at(link, 0) != b'/' || at(path, 0) != b'/' || link.contains(&b'$') || path.contains(&b'$') { continue; }
            out.push((canon(&lcpy(link, 300), 300), canon(&lcpy(path, 300), 300)));
        }
    }
    out
}

// ---------------------------------------------------------------- gzip

/// gzip: one or more members (RFC 1952), each a raw deflate stream, at most max_out bytes.
pub fn gunzip(s: &[u8], max_out: u32) -> Result<Vec<u8>, &'static [u8]> {
    let n = s.len() as u32;
    // the size of the last member is in its trailer; with several members, grow
    let want = if n >= 4 { u32::from_le_bytes([s[n as usize - 4], s[n as usize - 3], s[n as usize - 2], s[n as usize - 1]]) } else { 0 };
    let mut cap = if want > n { want } else { n.wrapping_mul(4) };
    for _ in 0..6 {
        if cap > max_out { cap = max_out; }
        let mut out = Vec::new();
        if out.try_reserve_exact(cap as usize).is_err() { return Err(b"not enough memory to unpack"); }
        out.resize(cap as usize, 0);
        let (mut at_, mut pos) = (0usize, 0usize);
        let mut bad = 0;
        while pos + 18 <= s.len() && s[pos] == 0x1F && s[pos + 1] == 0x8B {
            let flg = s[pos + 3];
            let mut p = pos + 10;
            if flg & 4 != 0 { if p + 2 > s.len() { bad = 1; break; } p += 2 + (s[p] as usize | (s[p + 1] as usize) << 8); }   // FEXTRA
            if flg & 8 != 0 { while p < s.len() && s[p] != 0 { p += 1; } p += 1; }        // FNAME
            if flg & 16 != 0 { while p < s.len() && s[p] != 0 { p += 1; } p += 1; }       // FCOMMENT
            if flg & 2 != 0 { p += 2; }                                                  // FHCRC
            if p >= s.len() { bad = 1; break; }
            match crate::inflate::inflate_raw(&s[p..], &mut out[at_..]) {
                Ok(dn) => { at_ += dn.written; pos = p + dn.consumed + 8; }               // past the member's CRC and size
                Err(_) => { bad = 2; break; }
            }
        }
        if bad == 0 { out.truncate(at_); return Ok(out); }
        if bad == 1 || cap >= max_out { break; }
        cap = cap.wrapping_mul(2);
    }
    Err(b"the gzip data is damaged or too large")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn helpers() {
        let mut l = Vec::new();
        for s in [&b"libc6"[..], b"zlib1g", b"libc6", b"", b"libc"] { list_add(&mut l, 512, s); }
        assert_eq!(l, b"libc6, zlib1g, libc");
        assert_eq!(norm_path(b"./usr/./bin//hello", 256).unwrap(), b"/usr/bin/hello");
        assert_eq!(norm_path(b"bin/sh", 256).unwrap(), b"/usr/bin/sh");
        assert!(norm_path(b"a/../b", 256).is_none());
        assert_eq!(join_link(b"/usr/bin/x", b"../lib/y", 512), b"/usr/lib/y");
        assert_eq!(join_link(b"/a", b"../../..", 512), b"/");
        assert_eq!(lx_to_wrld(b"/lib/x.so", 600), b"C:\\Linux\\usr\\lib\\x.so");
        assert_eq!(lx_to_wrld(b"/", 600), b"C:\\Linux");
        let c = b"Package: hello\nVersion: 2.10\nDescription: says hello\n Long text\n .\n more\nDepends: libc6 (>= 2.34),\n zlib1g\n";
        assert_eq!(field_line(c, b"package", 64), b"hello");
        assert_eq!(field(c, b"Depends", b':').unwrap(), b"libc6 (>= 2.34),\n zlib1g");
        assert_eq!(split_words(br#"a "b c" 'd\e' f\ g"#, 48), [b"a".to_vec(), b"b c".to_vec(), b"d\\e".to_vec(), b"f g".to_vec()]);
        let alts = alternatives(b"update-alternatives --install /usr/bin/figlet figlet /usr/bin/figlet-figlet 50 \\\n --slave /bin/x x /usr/bin/y\n");
        assert_eq!(alts, [(b"/usr/bin/figlet".to_vec(), b"/usr/bin/figlet-figlet".to_vec()), (b"/usr/bin/x".to_vec(), b"/usr/bin/y".to_vec())]);
        let de = desktop_entry(b"[Desktop Entry]\nName=Foo\nExec=foo %U --x %%\nTerminal=true\n[Action]\nName=Bar\n").unwrap();
        assert_eq!(de, (b"Foo".to_vec(), b"foo  --x %".to_vec(), true));
    }
    struct NoUnpack;
    impl Unpack for NoUnpack { fn unpack(&self, d: &[u8], _: u32) -> Result<alloc::boxed::Box<dyn Bytes>, Vec<u8>> { Ok(alloc::boxed::Box::new(d.to_vec())) } }
    fn tar_member(name: &[u8], ty: u8, data: &[u8]) -> Vec<u8> {
        let mut h = alloc::vec![0u8; 512];
        h[..name.len()].copy_from_slice(name);
        let sz = alloc::format!("{:011o}", data.len());
        h[124..135].copy_from_slice(sz.as_bytes());
        h[156] = ty;
        let mut v = h; v.extend_from_slice(data); while v.len() % 512 != 0 { v.push(0); } v
    }
    #[test]
    fn deb() {
        let ctl = [tar_member(b"./control", b'0', b"Package: t\nVersion: 1\nArchitecture: all\nDescription: x\n"), alloc::vec![0; 1024]].concat();
        let data = [tar_member(b"./usr/bin/t", b'0', b"#!/bin/sh\necho\n"), tar_member(b"./bin", b'5', b""), alloc::vec![0; 1024]].concat();
        let mut ar = b"!<arch>\n".to_vec();
        for (n, d) in [(&b"debian-binary"[..], &b"2.0\n"[..]), (b"control.tar", &ctl[..]), (b"data.tar", &data[..])] {
            let mut h = alloc::format!("{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n", core::str::from_utf8(n).unwrap(), 0, 0, 0, 100644, d.len()).into_bytes();
            h.extend_from_slice(d); if d.len() % 2 == 1 { h.push(b'\n'); } ar.extend_from_slice(&h);
        }
        let p = Package::load(&ar, &NoUnpack, 1 << 20).unwrap();
        assert_eq!((p.name.as_slice(), p.ents.len(), p.nfiles), (&b"t"[..], 2, 1));
        assert_eq!(p.ents[1].path, b"/usr/bin");
    }
    #[test]
    fn junk_does_not_panic() {
        let mut seed = 0xDEAD_BEEFu32;
        let mut next = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for _ in 0..4000 {
            let n = (next() % 2000) as usize;
            let mut d: Vec<u8> = (0..n).map(|_| next() as u8).collect();
            match next() % 3 { 0 if n > 8 => d[..8].copy_from_slice(b"!<arch>\n"), 1 if n > 4 => d[..4].copy_from_slice(&[0xED, 0xAB, 0xEE, 0xDB]), _ => {} }
            let _ = Package::load(&d, &NoUnpack, 1 << 20);
            tar_walk(&d, &mut |_, _, _, _| {});
            let _ = (alternatives(&d), desktop_entry(&d), gunzip(&d, 1 << 16), split_words(&d, 48));
        }
    }
}
