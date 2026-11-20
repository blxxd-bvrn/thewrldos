//! The Windows executable format (PE32 and PE32+): headers and section table, building the
//! image from the file, base relocations, the import table, exports (with forwarders), the
//! resource tree and the TLS directory. Replaces the file-format half of src/win32/pe.c; the
//! process model (address spaces, the module list, DLL search, the built-in APIs) stays in
//! C and calls these through kernel.rs.
//!
//! Programs and DLLs come from anywhere, so nothing in them is trusted: every RVA, size,
//! count and string is checked against the file or the image before use, with arithmetic
//! that cannot overflow. The C version followed the import, export, relocation and resource
//! tables wherever their RVAs pointed and read the section table past the end of the file.

use alloc::string::String;
use alloc::vec::Vec;

fn le16(b: &[u8], o: usize) -> Option<u16> { Some(u16::from_le_bytes(b.get(o..o.checked_add(2)?)?.try_into().ok()?)) }
fn le32(b: &[u8], o: usize) -> Option<u32> { Some(u32::from_le_bytes(b.get(o..o.checked_add(4)?)?.try_into().ok()?)) }
fn le64(b: &[u8], o: usize) -> Option<u64> { Some(u64::from_le_bytes(b.get(o..o.checked_add(8)?)?.try_into().ok()?)) }

/// A NUL-terminated string at `o`, at most `max` bytes (cut there, as the C version's buffers did).
fn cstr(b: &[u8], o: usize, max: usize) -> Option<&[u8]> {
    let s = b.get(o..)?;
    let n = s.iter().take(max).position(|&c| c == 0).unwrap_or(s.len().min(max));
    Some(&s[..n])
}

/// The two kinds of program this OS runs: one per edition.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Arch { X86, X64 }

impl Arch {
    fn machine(self) -> u16 { match self { Arch::X86 => 0x014C, Arch::X64 => 0x8664 } }
    fn magic(self) -> u16 { match self { Arch::X86 => 0x10B, Arch::X64 => 0x20B } }
    fn ptr(self) -> usize { match self { Arch::X86 => 4, Arch::X64 => 8 } }
}

/// A section table entry.
#[derive(Clone, Copy, Debug, Default)]
pub struct Section { pub va: u32, pub vsize: u32, pub raw_size: u32, pub raw_ptr: u32 }

/// What the loader uses from a file's headers.
#[derive(Clone, Debug, Default)]
pub struct Headers {
    pub image_base: u64,
    pub stack_reserve: u64,
    pub entry: u32,
    pub size_image: u32,
    pub size_headers: u32,
    pub subsystem: u16,
    pub characteristics: u16,
    pub machine: u16,
    /// data directories in use: (RVA, size); index 0 exports, 1 imports, 2 resources,
    /// 3 exceptions, 5 relocations, 9 TLS, 14 CLR
    pub dirs: [(u32, u32); 16],
    pub nrva: u32,
    pub sections: Vec<Section>,
}

impl Headers {
    pub fn dir(&self, i: usize) -> (u32, u32) { if (i as u32) < self.nrva { self.dirs[i] } else { (0, 0) } }
    /// IMAGE_FILE_RELOCS_STRIPPED
    pub fn relocs_stripped(&self) -> bool { self.characteristics & 1 != 0 }
    pub fn has_relocs(&self) -> bool { self.dir(5).1 != 0 && !self.relocs_stripped() }
    /// The image's size in memory, rounded up to pages.
    pub fn image_pages(&self) -> u32 { self.size_image.checked_add(0xFFF).map_or(0, |s| s & !0xFFF) }
}

/// Reads and checks a file's headers. `dll` says which kind is wanted. The messages are the
/// ones the C loader gave, for the user (one more: a section table past the end of the file).
pub fn parse(d: &[u8], dll: bool, arch: Arch) -> Result<Headers, String> {
    let err = |s: &str| Err(String::from(s));
    if d.len() < 64 || &d[..2] != b"MZ" { return err("Not a valid Win32 application (no MZ header)."); }
    let lf = le32(d, 60).unwrap_or(0) as usize;
    if lf.checked_add(24).is_none_or(|e| e > d.len()) || &d[lf..lf + 4] != b"PE\0\0" {
        return err("This is an MS-DOS program; 16-bit DOS executables are not supported.");
    }
    let machine = le16(d, lf + 4).unwrap_or(0);
    if machine == 0xAA64 { return err("This is an ARM64 program; only x86 and x64 programs are supported."); }
    match arch {
        Arch::X64 if machine == 0x14C => return err("This is a 32-bit (x86) program. This is the 64-bit edition of THE WRLD OS, which runs 64-bit (x64) programs; use the 32-bit edition for this one, or rebuild it with x86_64-w64-mingw32-gcc."),
        Arch::X86 if machine == 0x8664 => return err("This is a 64-bit (x64) program. Run it on the 64-bit edition of THE WRLD OS (wrldos64.iso), or rebuild it for i686 (i686-w64-mingw32-gcc)."),
        _ => {}
    }
    if machine != arch.machine() { return Err(alloc::format!("Unsupported machine type 0x{machine:04x}.")); }
    let nsec = le16(d, lf + 6).unwrap_or(0) as usize;
    let opt_size = le16(d, lf + 20).unwrap_or(0) as usize;
    let characteristics = le16(d, lf + 22).unwrap_or(0);
    let oh = lf + 24;
    let fixed = match arch { Arch::X86 => 96, Arch::X64 => 112 };
    let bad_opt = match arch { Arch::X86 => "Unsupported optional header (expected PE32 (x86)).", Arch::X64 => "Unsupported optional header (expected PE32+ (x64))." };
    if oh + opt_size > d.len() || le16(d, oh) != Some(arch.magic()) || opt_size < fixed { return err(bad_opt); }
    let o = &d[oh..oh + opt_size];
    let mut h = Headers {
        entry: le32(o, 16).unwrap_or(0),
        size_image: le32(o, 56).unwrap_or(0),
        size_headers: le32(o, 60).unwrap_or(0),
        subsystem: le16(o, 68).unwrap_or(0),
        characteristics,
        machine,
        ..Headers::default()
    };
    let (nrva_at, dirs_at) = match arch {
        Arch::X86 => { h.image_base = le32(o, 28).unwrap_or(0) as u64; h.stack_reserve = le32(o, 72).unwrap_or(0) as u64; (92, 96) }
        Arch::X64 => { h.image_base = le64(o, 24).unwrap_or(0); h.stack_reserve = le64(o, 72).unwrap_or(0); (108, 112) }
    };
    // the directories the header says it has, that also fit in it
    h.nrva = le32(o, nrva_at).unwrap_or(0).min(16).min(((opt_size - dirs_at) / 8) as u32);
    for i in 0..h.nrva as usize {
        h.dirs[i] = (le32(o, dirs_at + i * 8).unwrap_or(0), le32(o, dirs_at + i * 8 + 4).unwrap_or(0));
    }
    let st = oh + opt_size;
    if st.checked_add(nsec * 40).is_none_or(|e| e > d.len()) { return err("The program file is damaged (its section table runs past the end of the file)."); }
    for i in 0..nsec {
        let s = &d[st + i * 40..st + i * 40 + 40];
        h.sections.push(Section { vsize: le32(s, 8).unwrap_or(0), va: le32(s, 12).unwrap_or(0), raw_size: le32(s, 16).unwrap_or(0), raw_ptr: le32(s, 20).unwrap_or(0) });
    }
    let is_dll = characteristics & 0x2000 != 0;
    if !dll && is_dll { return err("This file is a DLL, not a program."); }
    if dll && !is_dll { return err("This file is not a DLL."); }
    if h.dir(14).1 != 0 { return err("This is a .NET (CLR) assembly; the .NET runtime is not available."); }
    Ok(h)
}

/// Fills `img` (the image's memory, zeroed, `image_pages()` long) from the file: the headers,
/// then each section's data. Sections that do not fit are left out, as before.
pub fn load_image(d: &[u8], h: &Headers, img: &mut [u8]) {
    let n = (h.size_headers as usize).min(d.len()).min(img.len());
    img[..n].copy_from_slice(&d[..n]);
    for s in &h.sections {
        let span = s.vsize.max(s.raw_size) as u64;
        if s.va as u64 + span > img.len() as u64 { continue; }
        let raw = s.raw_size.min(if s.vsize != 0 { s.vsize } else { s.raw_size }) as usize;
        let (src, dst) = (s.raw_ptr as usize, s.va as usize);
        if let (Some(from), Some(to)) = (d.get(src..src.saturating_add(raw)), img.get_mut(dst..dst + raw)) {
            to.copy_from_slice(from);
        }
    }
}

/// Applies the base relocations for a move by `delta` (image placed at base + delta).
pub fn relocate(img: &mut [u8], h: &Headers, delta: i64) {
    let (rva, size) = h.dir(5);
    if delta == 0 || size == 0 { return; }
    let start = rva as usize;
    let end = (rva as u64 + size as u64).min(img.len() as u64) as usize;
    let mut r = start;
    while r + 8 <= end {
        let page = le32(img, r).unwrap_or(0) as u64;
        let bsz = le32(img, r + 4).unwrap_or(0) as usize;
        if bsz < 8 { break; }
        let block_end = r.saturating_add(bsz).min(end);
        let mut e = r + 8;
        while e + 2 <= block_end {
            let v = le16(img, e).unwrap_or(0);
            e += 2;
            let (kind, off) = (v >> 12, (v & 0xFFF) as u64);
            let at = page + off;
            let width = match kind { 10 => 8, 3 => 4, 1 | 2 => 2, _ => continue };
            if at + width > img.len() as u64 { continue; }
            let at = at as usize;
            match kind {
                10 => { let x = le64(img, at).unwrap_or(0).wrapping_add(delta as u64); img[at..at + 8].copy_from_slice(&x.to_le_bytes()); }
                3 => { let x = le32(img, at).unwrap_or(0).wrapping_add(delta as u32); img[at..at + 4].copy_from_slice(&x.to_le_bytes()); }
                1 => { let x = le16(img, at).unwrap_or(0).wrapping_add((delta >> 16) as u16); img[at..at + 2].copy_from_slice(&x.to_le_bytes()); }
                _ => { let x = le16(img, at).unwrap_or(0).wrapping_add(delta as u16); img[at..at + 2].copy_from_slice(&x.to_le_bytes()); }
            }
        }
        r = r.saturating_add(bsz);
    }
}

/// One imported function: by name, or by ordinal.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Import<'a> { Name(&'a [u8]), Ordinal(u16) }

/// Walks the import table: for each DLL, `dll(name)` gives a handle, then for each of its
/// entries `entry(handle, dll, import, slot)` gives the address to put in the import
/// address table at image offset `slot`, which this writes. Returns how many entries were bound.
pub fn bind_imports<H: Clone>(img: &mut [u8], h: &Headers, arch: Arch,
                             mut dll: impl FnMut(&[u8]) -> H, mut entry: impl FnMut(H, &[u8], Import, usize) -> u64) -> usize {
    let (rva, size) = h.dir(1);
    if size == 0 { return 0; }
    let ptr = arch.ptr();
    let ord_flag = 1u64 << (ptr * 8 - 1);
    let mut bound = 0;
    let mut desc = rva as usize;
    for _ in 0..4096 {
        let (Some(oft), Some(name_rva), Some(ft)) = (le32(img, desc), le32(img, desc + 12), le32(img, desc + 16)) else { break };
        if name_rva == 0 { break; }
        let Some(name) = cstr(img, name_rva as usize, 255).map(|s| s.to_vec()) else { break };
        let hd = dll(&name);
        let ilt = if oft != 0 { oft } else { ft } as usize;
        for k in 0..65536usize {
            let (Some(io), Some(ao)) = (ilt.checked_add(k * ptr), (ft as usize).checked_add(k * ptr)) else { break };
            let v = match arch { Arch::X86 => le32(img, io).map(|x| x as u64), Arch::X64 => le64(img, io) };
            let Some(v) = v.filter(|&v| v != 0) else { break };
            if ao + ptr > img.len() { break; }
            let addr = if v & ord_flag != 0 {
                entry(hd.clone(), &name, Import::Ordinal(v as u16), ao)
            } else {
                let nm = cstr(img, (v as u32 as usize).saturating_add(2), 255).unwrap_or(b"").to_vec();
                entry(hd.clone(), &name, Import::Name(&nm), ao)
            };
            match arch {
                Arch::X86 => img[ao..ao + 4].copy_from_slice(&(addr as u32).to_le_bytes()),
                Arch::X64 => img[ao..ao + 8].copy_from_slice(&addr.to_le_bytes()),
            }
            bound += 1;
        }
        desc += 20;
    }
    bound
}

/// What a module exports under a name or ordinal.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Export { Rva(u32), Forward(Vec<u8>) }

/// Looks up an export by `name` ("#12" is ordinal 12) or, without a name, by ordinal.
pub fn find_export(img: &[u8], exp: (u32, u32), name: Option<&[u8]>, mut ord: u32) -> Option<Export> {
    let (erva, esize) = exp;
    let ed = erva as usize;
    if erva == 0 || ed.checked_add(40)? > img.len() { return None; }
    let (obase, nfunc, nname) = (le32(img, ed + 16)?, le32(img, ed + 20)?, le32(img, ed + 24)?);
    let (funcs, names, ords) = (le32(img, ed + 28)? as usize, le32(img, ed + 32)? as usize, le32(img, ed + 36)? as usize);
    let mut name = name;
    if let Some(n) = name.filter(|n| n.first() == Some(&b'#')) {
        ord = core::str::from_utf8(&n[1..]).ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0);
        name = None;
    }
    let name_at = |i: u32| -> Option<&[u8]> { cstr(img, le32(img, names.checked_add(i as usize * 4)?)? as usize, 4096) };
    let ord_at = |i: u32| -> Option<u32> { le16(img, ords.checked_add(i as usize * 2)?).map(|x| x as u32) };
    let idx = match name {
        Some(want) => {
            // names are sorted: binary search, then a linear pass for files that are not
            let (mut lo, mut hi) = (0i64, nname as i64 - 1);
            let mut found = None;
            while lo <= hi {
                let mid = ((lo + hi) / 2) as u32;
                let Some(n) = name_at(mid) else { break };
                match n.cmp(want) {
                    core::cmp::Ordering::Equal => { found = ord_at(mid); break; }
                    core::cmp::Ordering::Less => lo = mid as i64 + 1,
                    core::cmp::Ordering::Greater => hi = mid as i64 - 1,
                }
            }
            found.or_else(|| (0..nname.min(1 << 20)).find(|&i| name_at(i) == Some(want)).and_then(ord_at))?
        }
        None => ord.checked_sub(obase)?,
    };
    if idx >= nfunc { return None; }
    let rva = le32(img, funcs.checked_add(idx as usize * 4)?)?;
    if rva == 0 { return None; }
    if rva >= erva && (rva as u64) < erva as u64 + esize as u64 {
        // a forwarder: "OTHER.Name" or "OTHER.#12"
        let s = cstr(img, rva as usize, 127)?;
        return Some(Export::Forward(s.to_vec()));
    }
    Some(Export::Rva(rva))
}

/// A resource type, name or language to look for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ResId { Any, Num(u32), Name(Vec<u16>) }

fn res_name_eq(root: &[u8], off: u32, want: &[u16]) -> bool {
    let at = (off & 0x7FFF_FFFF) as usize;
    let Some(len) = le16(root, at) else { return false };
    if len as usize != want.len() { return false; }
    let up = |c: u16| if c < 128 { (c as u8).to_ascii_uppercase() as u16 } else { 0 };
    (0..len as usize).all(|i| le16(root, at + 2 + i * 2).is_some_and(|c| up(c) == up(want[i]) || c == want[i]))
}

/// The entry of the directory at `dir` (relative to the resource root) matching `id`.
fn res_level(root: &[u8], dir: u32, id: &ResId) -> Option<(u32, u32)> {
    let d = dir as usize;
    let n = le16(root, d.checked_add(12)?)? as usize + le16(root, d + 14)? as usize;
    for i in 0..n.min(65536) {
        let e = d + 16 + i * 8;
        let (name, off) = (le32(root, e)?, le32(root, e + 4)?);
        let hit = match id {
            ResId::Any => true,
            ResId::Num(v) => name & 0x8000_0000 == 0 && name == *v,
            ResId::Name(s) => name & 0x8000_0000 != 0 && res_name_eq(root, name, s),
        };
        if hit { return Some((name, off)); }
    }
    None
}

/// Finds a resource (type, name, then the language or else the first one): the image
/// offset of its data entry, as FindResource's handle points at.
pub fn find_resource(img: &[u8], res_rva: u32, kind: &ResId, name: &ResId, lang: u32) -> Option<u32> {
    if res_rva == 0 { return None; }
    let root = img.get(res_rva as usize..)?;
    let t = res_level(root, 0, kind)?;
    if t.1 & 0x8000_0000 == 0 { return None; }
    let n = res_level(root, t.1 & 0x7FFF_FFFF, name)?;
    if n.1 & 0x8000_0000 == 0 { return None; }
    let sub = n.1 & 0x7FFF_FFFF;
    let l = (if lang != 0 { res_level(root, sub, &ResId::Num(lang)) } else { None }).or_else(|| res_level(root, sub, &ResId::Any))?;
    if l.1 & 0x8000_0000 != 0 { return None; }
    let entry = res_rva.checked_add(l.1)?;
    (entry as u64 + 16 <= img.len() as u64).then_some(entry)
}

/// A resource's data from its data entry: (RVA, size), if it lies inside the image.
pub fn resource_data(img: &[u8], entry: u32) -> Option<(u32, u32)> {
    let (rva, size) = (le32(img, entry as usize)?, le32(img, entry as usize + 4)?);
    (rva as u64 + size as u64 <= img.len() as u64).then_some((rva, size))
}

/// The TLS directory, with its addresses checked against the image (placed at `base`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Tls {
    /// the template data's offset in the image and length (0: none usable)
    pub data_off: u32,
    pub data_len: u32,
    pub zero_fill: u32,
    /// where the TLS index variable is, if inside the image
    pub index_off: Option<u32>,
    /// the address of the callback array (0: none)
    pub callbacks: u64,
}

pub fn tls(img: &[u8], h: &Headers, base: u64, arch: Arch) -> Option<Tls> {
    let (rva, size) = h.dir(9);
    if size == 0 { return None; }
    let d = rva as usize;
    let (start, end, idx, cbs, zf) = match arch {
        Arch::X86 => (le32(img, d)? as u64, le32(img, d + 4)? as u64, le32(img, d + 8)? as u64, le32(img, d + 12)? as u64, le32(img, d + 16)?),
        Arch::X64 => (le64(img, d)?, le64(img, d + 8)?, le64(img, d + 16)?, le64(img, d + 24)?, le32(img, d + 32)?),
    };
    let len = if end > start && end - start < 0x100_0000 { end - start } else { 0 };
    let inside = |a: u64, n: u64| a >= base && a.checked_add(n).is_some_and(|e| e <= base + img.len() as u64);
    let mut t = Tls { zero_fill: zf.min(0x100_0000), callbacks: cbs, ..Tls::default() };
    if len > 0 && inside(start, len) { t.data_off = (start - base) as u32; t.data_len = len as u32; }
    if inside(idx, 4) { t.index_off = Some((idx - base) as u32); }
    Some(t)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeSet;
    use std::format;

    struct Vectors { dll: &'static [u8], exe: &'static [u8], exe_b: &'static [u8], dll_exp: &'static str, exe_exp: &'static str, arch: Arch }
    fn vectors() -> [Vectors; 2] {
        [
            Vectors { dll: include_bytes!("../testdata/pe/testdll_i686.dll"), exe: include_bytes!("../testdata/pe/testexe_i686.exe"),
                      exe_b: include_bytes!("../testdata/pe/testexe_i686_b.exe"), dll_exp: include_str!("../testdata/pe/testdll_i686.expect"),
                      exe_exp: include_str!("../testdata/pe/testexe_i686.expect"), arch: Arch::X86 },
            Vectors { dll: include_bytes!("../testdata/pe/testdll_x86_64.dll"), exe: include_bytes!("../testdata/pe/testexe_x86_64.exe"),
                      exe_b: include_bytes!("../testdata/pe/testexe_x86_64_b.exe"), dll_exp: include_str!("../testdata/pe/testdll_x86_64.expect"),
                      exe_exp: include_str!("../testdata/pe/testexe_x86_64.expect"), arch: Arch::X64 },
        ]
    }
    fn expect<'a>(text: &'a str, key: &str) -> Vec<Vec<&'a str>> {
        text.lines().filter(|l| l.split(' ').next() == Some(key)).map(|l| l.split(' ').skip(1).collect()).collect()
    }
    fn hex(s: &str) -> u64 { u64::from_str_radix(s, 16).unwrap() }
    fn image(file: &[u8], h: &Headers) -> Vec<u8> { let mut img = vec![0u8; h.image_pages() as usize]; load_image(file, h, &mut img); img }

    #[test]
    fn headers_match_objdump() {
        for v in vectors() {
            for (file, exp, dll) in [(v.exe, v.exe_exp, false), (v.dll, v.dll_exp, true)] {
                let h = parse(file, dll, v.arch).unwrap();
                assert_eq!(h.image_base, hex(expect(exp, "base")[0][0]));
                assert_eq!(h.entry as u64, hex(expect(exp, "entry")[0][0]));
                assert_eq!(h.size_image as u64, hex(expect(exp, "size")[0][0]));
                assert_eq!(h.subsystem as u64, expect(exp, "subsystem")[0][0].parse::<u64>().unwrap());
                assert!(!h.sections.is_empty() && h.has_relocs());
            }
            // the wrong kind, the wrong edition
            assert_eq!(parse(v.dll, false, v.arch).unwrap_err(), "This file is a DLL, not a program.");
            assert_eq!(parse(v.exe, true, v.arch).unwrap_err(), "This file is not a DLL.");
            let other = if v.arch == Arch::X86 { Arch::X64 } else { Arch::X86 };
            assert!(parse(v.exe, false, other).unwrap_err().contains("edition"));
        }
        assert_eq!(parse(b"not a program at all, not even close to 64 bytes long......................", false, Arch::X86).unwrap_err(),
                   "Not a valid Win32 application (no MZ header).");
    }

    #[test]
    fn imports_match_objdump() {
        for v in vectors() {
            let h = parse(v.exe, false, v.arch).unwrap();
            let mut img = image(v.exe, &h);
            let mut got = BTreeSet::new();
            let mut slot = 0u64;
            let n = bind_imports(&mut img, &h, v.arch, |d| String::from_utf8_lossy(d).to_lowercase(), |dll, _, imp, _| {
                got.insert(match imp { Import::Name(n) => format!("{dll} {}", String::from_utf8_lossy(n)), Import::Ordinal(o) => format!("{dll} #{o}") });
                slot += 1; 0x5000_0000 + slot
            });
            let want: BTreeSet<String> = expect(v.exe_exp, "import").into_iter().map(|p| p.join(" ")).collect();
            assert_eq!(got, want);
            assert_eq!(n, want.len());
            // the import address table now holds what the callback gave
            let iat = h.dir(12);
            if iat.1 != 0 {
                let first = match v.arch { Arch::X86 => le32(&img, iat.0 as usize).unwrap() as u64, Arch::X64 => le64(&img, iat.0 as usize).unwrap() };
                assert!(first > 0x5000_0000 && first <= 0x5000_0000 + slot, "{first:x}");
            }
        }
    }

    #[test]
    fn exports_match_objdump() {
        for v in vectors() {
            let h = parse(v.dll, true, v.arch).unwrap();
            let img = image(v.dll, &h);
            for e in expect(v.dll_exp, "export") {
                let want = Some(Export::Rva(hex(e[1]) as u32));
                assert_eq!(find_export(&img, h.dir(0), Some(e[0].as_bytes()), 0), want, "{}", e[0]);
                if let Some(o) = e[0].strip_prefix('#') { assert_eq!(find_export(&img, h.dir(0), None, o.parse().unwrap()), want); }
            }
            for f in expect(v.dll_exp, "forward") {
                assert_eq!(find_export(&img, h.dir(0), Some(f[0].as_bytes()), 0), Some(Export::Forward(f[1].as_bytes().to_vec())));
            }
            assert_eq!(find_export(&img, h.dir(0), Some(b"no_such_function"), 0), None);
            assert_eq!(find_export(&img, h.dir(0), None, 99), None);
        }
    }

    #[test]
    fn relocation_matches_relinking() {
        // the same program linked at 0x10000000 and 0x20000000: moving the first by the
        // difference must give the second, section for section
        for v in vectors() {
            let (ha, hb) = (parse(v.exe, false, v.arch).unwrap(), parse(v.exe_b, false, v.arch).unwrap());
            let (mut a, b) = (image(v.exe, &ha), image(v.exe_b, &hb));
            relocate(&mut a, &ha, (hb.image_base - ha.image_base) as i64);
            assert_ne!(ha.image_base, hb.image_base);
            let mut compared = 0;
            for s in &ha.sections {
                let r = s.va as usize..(s.va + s.raw_size.min(s.vsize)) as usize;
                if (s.va, s.vsize) == (ha.dir(5).0, ha.dir(5).1) { continue; }
                assert_eq!(&a[r.clone()], &b[r.clone()], "{:?} section at {:x}", v.arch, s.va);
                compared += r.len();
            }
            assert!(compared > 10_000);
        }
    }

    fn wide(s: &str) -> ResId { ResId::Name(s.encode_utf16().collect()) }

    #[test]
    fn resources() {
        for v in vectors() {
            let h = parse(v.dll, true, v.arch).unwrap();
            let img = image(v.dll, &h);
            let res = h.dir(2).0;
            let data = |e: u32| { let (r, n) = resource_data(&img, e).unwrap(); &img[r as usize..(r + n) as usize] };
            // RCDATA (type 10) 7 in two languages; without one (or with one it lacks) the first,
            // which is German: languages are sorted by number (0x407 before 0x409)
            let en = find_resource(&img, res, &ResId::Num(10), &ResId::Num(7), 0x409).unwrap();
            assert!(data(en).starts_with(b"english seven"));
            let de = find_resource(&img, res, &ResId::Num(10), &ResId::Num(7), 0x407).unwrap();
            assert!(data(de).starts_with(b"deutsch sieben"));
            assert_eq!(find_resource(&img, res, &ResId::Num(10), &ResId::Num(7), 0), Some(de));
            assert_eq!(find_resource(&img, res, &ResId::Num(10), &ResId::Num(7), 0x40C), Some(de));   // no French
            // a named type and name, matched without regard to case
            let w = find_resource(&img, res, &wide("wave"), &wide("Chime"), 0).unwrap();
            assert!(data(w).starts_with(b"RIFF-not-really-a-wave"));
            assert!(find_resource(&img, res, &ResId::Num(10), &ResId::Num(123), 0).is_some());
            // strings 1..15 are in block 1, 17 in block 2 (type 6)
            assert!(find_resource(&img, res, &ResId::Num(6), &ResId::Num(1), 0).is_some());
            assert!(find_resource(&img, res, &ResId::Num(6), &ResId::Num(2), 0).is_some());
            assert_eq!(find_resource(&img, res, &ResId::Num(6), &ResId::Num(9), 0), None);
            assert_eq!(find_resource(&img, res, &wide("WAVE"), &wide("CHIMES"), 0), None);
        }
    }

    #[test]
    fn tls_directory() {
        for v in vectors() {
            let h = parse(v.exe, false, v.arch).unwrap();
            let img = image(v.exe, &h);
            let t = tls(&img, &h, h.image_base, v.arch).expect("the program has TLS");
            assert!(t.callbacks > h.image_base && t.callbacks < h.image_base + img.len() as u64);
            assert!(t.index_off.is_some());
            assert!(t.data_len >= 4);                              // per_thread
        }
    }

    /// Damaged files: errors or partial results, never a panic or an out-of-range access.
    #[test]
    fn fuzz_no_panic() {
        let mut seed = 0x9E37u32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for round in 0..3000 {
            let v = &vectors()[round % 2];
            let (file, dll) = if round % 4 < 2 { (v.exe, false) } else { (v.dll, true) };
            let mut f = file.to_vec();
            for _ in 0..1 + rnd() % 12 {
                // the headers and the tables are where the structure is
                let k = if rnd() % 2 == 0 { rnd() as usize % 1024 } else { rnd() as usize % f.len() };
                f[k] = if rnd() % 2 == 0 { rnd() as u8 } else { f[k] ^ (1 << (rnd() % 8)) };
            }
            if round % 10 == 0 { let n = rnd() as usize % f.len(); f.truncate(n); }
            let Ok(h) = parse(&f, dll, v.arch) else { continue };
            let pages = h.image_pages() as usize;
            if pages == 0 || pages > 64 << 20 { continue; }
            let mut img = image(&f, &h);
            relocate(&mut img, &h, 0x1234_5000);
            bind_imports(&mut img, &h, v.arch, |_| (), |_, _, _, _| 1);
            let _ = find_export(&img, h.dir(0), Some(b"add"), 0);
            let _ = find_export(&img, h.dir(0), None, 2);
            let _ = find_resource(&img, h.dir(2).0, &ResId::Num(10), &ResId::Num(7), 0x407);
            let _ = find_resource(&img, h.dir(2).0, &wide("WAVE"), &ResId::Any, 0);
            let _ = tls(&img, &h, h.image_base, v.arch);
        }
    }
}
