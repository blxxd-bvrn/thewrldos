//! The pure parts of the in-memory file system (`kernel/vfsk.rs`, was `src/kernel/vfs.c`):
//! path strings with the C functions' buffer sizes, the parts a path resolves through, the
//! order Explorer lists a folder in, and the initrd's ustar archive.

use alloc::vec::Vec;

fn is_sep(c: u8) -> bool { c == b'\\' || c == b'/' }

/// strlcpy into a buffer of `cap` bytes: at most `cap - 1` of `s`.
pub fn lcpy(s: &[u8], cap: usize) -> Vec<u8> { s[..s.len().min(cap.saturating_sub(1))].to_vec() }
/// strlcat: appends what fits in `cap` bytes (with the NUL).
pub fn lcat(out: &mut Vec<u8>, s: &[u8], cap: usize) {
    let room = cap.saturating_sub(1).saturating_sub(out.len());
    out.extend_from_slice(&s[..s.len().min(room)]);
}

/// path_normalize: '/' becomes '\'.
pub fn normalize(p: &mut [u8]) { for c in p { if *c == b'/' { *c = b'\\'; } } }
/// path_basename: where the last part starts.
pub fn basename_at(p: &[u8]) -> usize { p.iter().rposition(|&c| is_sep(c)).map_or(0, |i| i + 1) }
/// path_dirname into `cap` bytes: the folder ("C:\" for a file in a root, "" for none).
pub fn dirname(p: &[u8], cap: usize) -> Vec<u8> {
    let mut out = lcpy(p, cap);
    normalize(&mut out);
    match out.iter().rposition(|&c| c == b'\\') {
        None => Vec::new(),
        Some(s) if s == 2 && out[1] == b':' => { out.truncate(3); out }
        Some(s) => { out.truncate(s); out }
    }
}
/// path_join into `cap` bytes.
pub fn join(a: &[u8], b: &[u8], cap: usize) -> Vec<u8> {
    let mut out = lcpy(a, cap);
    if out.last().is_some_and(|&c| !is_sep(c)) { lcat(&mut out, b"\\", cap); }
    lcat(&mut out, b, cap);
    out
}

/// One step of a path as vfs_resolve takes it.
#[derive(Debug, PartialEq, Eq)]
pub enum Part<'a> { Here, Up, Name(&'a [u8]) }
/// The steps of a path after its drive or root: parts of more than 127 bytes are taken 127
/// at a time, as the C version did.
pub fn parts(p: &[u8]) -> Vec<Part<'_>> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < p.len() {
        while i < p.len() && is_sep(p[i]) { i += 1; }
        if i >= p.len() { break; }
        let s = i;
        while i < p.len() && !is_sep(p[i]) && i - s < 127 { i += 1; }
        out.push(match &p[s..i] { b"." => Part::Here, b".." => Part::Up, n => Part::Name(n) });
    }
    out
}

/// The C kernel's strcasecmp order (ASCII case folded).
pub fn casecmp(a: &[u8], b: &[u8]) -> core::cmp::Ordering {
    a.iter().map(|c| c.to_ascii_lowercase()).cmp(b.iter().map(|c| c.to_ascii_lowercase()))
}
/// vfs_sort_children's order: folders first, then by name; equal entries end up in reverse
/// (an insertion sort that inserts before equals), as the C version did. Takes (dir, name).
pub fn sort_order(items: &[(bool, &[u8])]) -> Vec<usize> {
    let mut sorted: Vec<usize> = Vec::with_capacity(items.len());
    for (i, &(dir, name)) in items.iter().enumerate() {
        let pos = sorted.iter().position(|&j| { let (d, n) = items[j]; !(d & !dir || (d == dir && casecmp(n, name) == core::cmp::Ordering::Less)) }).unwrap_or(sorted.len());
        sorted.insert(pos, i);
    }
    sorted
}

/// One member of a ustar archive.
#[derive(Debug, PartialEq, Eq)]
pub struct TarEntry { pub name: Vec<u8>, pub dir: bool, pub data: core::ops::Range<usize> }
fn octal(s: &[u8]) -> u64 { s.iter().take_while(|c| (b'0'..=b'7').contains(c)).fold(0u64, |v, &c| v.wrapping_mul(8).wrapping_add((c - b'0') as u64)) }
fn cstr_n(s: &[u8], n: usize) -> &[u8] { let s = &s[..n.min(s.len())]; &s[..s.iter().position(|&c| c == 0).unwrap_or(s.len())] }
/// The folders and files of a ustar archive (other kinds are skipped), names without a
/// leading "./" or trailing "/", and where each file's data is in the archive. A member
/// whose data runs past the archive ends it.
pub fn tar_entries(data: &[u8]) -> Vec<TarEntry> {
    let mut out = Vec::new();
    let mut off = 0usize;
    while off + 512 <= data.len() {
        let h = &data[off..off + 512];
        if h[0] == 0 { break; }
        let mut name = if &h[257..262] == b"ustar" && h[345] != 0 {
            let mut n = cstr_n(&h[345..], 155).to_vec(); n.push(b'/'); n.extend_from_slice(cstr_n(h, 100)); n
        } else { cstr_n(h, 100).to_vec() };
        name.truncate(255);
        let fsize = octal(&h[124..136]);
        let ty = h[156];
        let mut nm: &[u8] = &name;
        if nm.starts_with(b"./") { nm = &nm[2..]; }
        if nm.last() == Some(&b'/') { nm = &nm[..nm.len() - 1]; }
        let start = off + 512;
        let Some(end) = (start as u64).checked_add(fsize).filter(|&e| e <= data.len() as u64) else { break };
        if !nm.is_empty() {
            if ty == b'5' { out.push(TarEntry { name: nm.to_vec(), dir: true, data: start..start }); }
            else if ty == b'0' || ty == 0 { out.push(TarEntry { name: nm.to_vec(), dir: false, data: start..end as usize }); }
        }
        let Some(next) = (start as u64).checked_add((fsize + 511) & !511) else { break };
        off = next as usize;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths() {
        assert_eq!(dirname(b"C:/a/b.txt", 512), b"C:\\a");
        assert_eq!(dirname(b"C:\\b.txt", 512), b"C:\\");
        assert_eq!(dirname(b"b.txt", 512), b"");
        assert_eq!(join(b"C:\\a", b"b", 512), b"C:\\a\\b");
        assert_eq!(join(b"C:\\a\\", b"b", 512), b"C:\\a\\b");
        assert_eq!(join(b"", b"b", 512), b"b");
        assert_eq!(join(b"C:\\abc", b"def", 6), b"C:\\ab");
        assert_eq!(basename_at(b"C:\\a/b"), 5);
        let long = [b'x'; 300];
        let mut p = b"\\a\\.\\..\\".to_vec(); p.extend_from_slice(&long);
        let v = parts(&p);
        assert_eq!(v[..3], [Part::Name(b"a"), Part::Here, Part::Up]);
        assert_eq!(v.len(), 6);                                    // 300 bytes: 127 + 127 + 46
    }

    #[test]
    fn sorting() {
        let items: [(bool, &[u8]); 5] = [(false, b"b.txt"), (true, b"Zed"), (false, b"A.txt"), (true, b"alpha"), (false, b"B.TXT")];
        assert_eq!(sort_order(&items), [3, 1, 2, 4, 0]);
    }

    #[test]
    fn tar() {
        let mut a = Vec::new();
        let mut hdr = |name: &str, ty: u8, data: &[u8], a: &mut Vec<u8>| {
            let mut h = [0u8; 512];
            h[..name.len()].copy_from_slice(name.as_bytes());
            let sz = alloc::format!("{:011o}", data.len());
            h[124..135].copy_from_slice(sz.as_bytes());
            h[156] = ty; h[257..262].copy_from_slice(b"ustar");
            a.extend_from_slice(&h); a.extend_from_slice(data);
            while a.len() % 512 != 0 { a.push(0); }
        };
        hdr("./WRLD/", b'5', b"", &mut a);
        hdr("./WRLD/x.txt", b'0', b"hello", &mut a);
        hdr("link", b'2', b"", &mut a);
        let e = tar_entries(&a);
        assert_eq!(e.len(), 2);
        assert_eq!((e[0].name.as_slice(), e[0].dir), (&b"WRLD"[..], true));
        assert_eq!((e[1].name.as_slice(), &a[e[1].data.clone()]), (&b"WRLD/x.txt"[..], &b"hello"[..]));
        // a size beyond the archive ends it instead of reading past it
        let mut bad = a.clone(); bad[512 + 124..512 + 135].copy_from_slice(b"77777777777");
        assert_eq!(tar_entries(&bad).len(), 1);
        for n in 0..a.len() { let _ = tar_entries(&a[..n]); }
    }
}
