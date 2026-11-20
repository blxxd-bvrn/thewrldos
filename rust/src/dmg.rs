//! Apple disk images (UDIF .dmg): the koly trailer, the XML property list's blkx table of
//! base64 "mish" chunk maps (raw, zero-fill and zlib chunks), then the MBR/GPT partition map
//! that says where the FAT volume is. Replaces src/kernel/dmg.c; the C interface is in kernel.rs.
//!
//! Every offset and size in an image is checked with overflow-checked arithmetic before
//! use; the C version could overflow those sums, start reading the GPT entries anywhere,
//! and hand the FAT reader a partition that ran past the decoded disk.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use crate::inflate::inflate_zlib;

fn be32(b: &[u8], o: usize) -> u32 { u32::from_be_bytes(b[o..o + 4].try_into().unwrap_or([0; 4])) }
fn be64(b: &[u8], o: usize) -> u64 { u64::from_be_bytes(b[o..o + 8].try_into().unwrap_or([0; 8])) }
fn le32(b: &[u8], o: usize) -> Option<u32> { Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?)) }
fn le64(b: &[u8], o: usize) -> Option<u64> { Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?)) }

/// The largest decoded disk accepted (memory: the whole disk is decoded at once).
pub const MAX_DISK_BYTES: u64 = 256 << 20;

/// A decoded disk: its bytes and a one-line description.
pub struct Disk {
    pub bytes: Vec<u8>,
    pub info: String,
}

fn find(h: &[u8], from: usize, needle: &[u8]) -> Option<usize> {
    h.get(from..)?.windows(needle.len()).position(|w| w == needle).map(|i| from + i)
}

fn b64decode(s: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4 + 4);
    let (mut acc, mut bits) = (0u32, 0u32);
    for &c in s {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => continue,                                       // whitespace, '=' padding
        } as u32;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    out
}

/// A zero-filled buffer, or None when there is not that much memory.
fn zeroed(n: usize) -> Option<Vec<u8>> {
    let mut v = Vec::new();
    v.try_reserve_exact(n).ok()?;
    v.resize(n, 0);
    Some(v)
}

/// Decodes a disk image: a UDIF image's chunks into the disk they describe, or any other
/// file as a raw disk. The error is a message for the user (as the C version gave).
pub fn extract(file: &[u8]) -> Result<Disk, String> {
    let size = file.len();
    if size < 512 || &file[size - 512..size - 508] != b"koly" {
        let mut bytes = Vec::new();
        bytes.try_reserve_exact(size).map_err(|_| String::from("Not enough memory"))?;
        bytes.extend_from_slice(file);
        return Ok(Disk { bytes, info: String::from("Raw disk image (no UDIF trailer)") });
    }
    let k = &file[size - 512..];
    let data_off = be64(k, 24);
    let (xml_off, xml_len) = (be64(k, 216), be64(k, 224));
    let sectors = be64(k, 492);
    let xml_end = xml_off.checked_add(xml_len);
    if xml_len == 0 || xml_end.map_or(true, |e| e > size as u64) || sectors == 0 || sectors > MAX_DISK_BYTES / 512 {
        return Err(String::from("Unsupported UDIF image (no XML block map, or image larger than 256 MB)"));
    }
    let disk_size = (sectors * 512) as usize;
    let mut disk = zeroed(disk_size).ok_or_else(|| String::from("Not enough memory"))?;
    let xml = &file[xml_off as usize..xml_end.unwrap_or(0) as usize];
    let (mut nblk, mut nchunks, mut comp) = (0u32, 0u32, 0u32);
    let mut p = find(xml, 0, b"<key>blkx</key>").ok_or_else(|| String::from("UDIF plist has no blkx table"))?;
    while let Some(ds) = find(xml, p, b"<data>") {
        let Some(de) = find(xml, ds, b"</data>") else { break };
        let m = b64decode(&xml[ds + 6..de]);
        p = de;
        if m.len() < 204 || &m[..4] != b"mish" { continue; }
        nblk += 1;
        let base = be64(&m, 8);
        let nch = be32(&m, 200) as usize;
        for c in m[204..].chunks_exact(40).take(nch) {
            let kind = be32(c, 0);
            if kind == 0xFFFF_FFFF || kind == 0x7FFF_FFFE { continue; }    // end marker, comment
            let (cnt, coff, clen) = (be64(c, 16), be64(c, 24), be64(c, 32));
            // the chunk's sectors, inside the disk (skipped otherwise, as before)
            let Some(range) = base.checked_add(be64(c, 8))
                .and_then(|sec| Some((sec.checked_mul(512)?, sec.checked_add(cnt)?.checked_mul(512)?)))
                .filter(|&(_, end)| end <= disk_size as u64) else { continue };
            let dst = &mut disk[range.0 as usize..range.1 as usize];
            nchunks += 1;
            // the chunk's data in the file, if it is inside it
            let src = data_off.checked_add(coff)
                .and_then(|s| Some((s, s.checked_add(clen)?)))
                .filter(|&(_, e)| e <= size as u64)
                .map(|(s, e)| &file[s as usize..e as usize]);
            match kind {
                0 | 2 => dst.fill(0),
                1 => if let Some(s) = src { let n = s.len().min(dst.len()); dst[..n].copy_from_slice(&s[..n]); },
                0x8000_0005 => {
                    if src.map_or(true, |s| inflate_zlib(s, dst).is_err()) {
                        return Err(String::from("zlib chunk failed to decompress"));
                    }
                    comp += 1;
                }
                _ => {
                    let name = match kind {
                        0x8000_0004 => "ADC", 0x8000_0006 => "bzip2", 0x8000_0007 => "LZFSE", 0x8000_0008 => "LZMA", _ => "unknown",
                    };
                    return Err(format!("Compression '{name}' (0x{kind:08x}) is not supported. Convert with: hdiutil convert -format UDZO"));
                }
            }
        }
    }
    Ok(Disk {
        bytes: disk,
        info: format!("UDIF image: {sectors} sectors, {nblk} partition maps, {nchunks} chunks ({comp} zlib)"),
    })
}

fn is_hfs(d: &[u8], at: usize) -> bool { d.get(at) == Some(&b'H') && matches!(d.get(at + 1), Some(b'+') | Some(b'X')) }
fn is_apfs(d: &[u8], at: usize) -> bool { d.get(at + 32..at + 36) == Some(b"NXSB") }
fn has_boot_sig(d: &[u8], at: usize) -> bool { d.get(at + 510..at + 512) == Some(&[0x55, 0xAA]) }

/// Where in a decoded disk the volume to mount is: (offset, length), always inside `d`. The
/// error says which unsupported file system was found.
pub fn find_partition(d: &[u8]) -> Result<(usize, usize), String> {
    let size = d.len() as u64;
    // GPT: the first entry whose partition holds a FAT boot sector (or names what it holds)
    if size > 1024 && &d[512..520] == b"EFI PART" {
        let ent = le32(d, 512 + 72).unwrap_or(0) as u64 * 512;
        let n = le32(d, 512 + 80).unwrap_or(0).min(128) as u64;
        let esz = le32(d, 512 + 84).unwrap_or(0) as u64;
        for i in 0..n {
            let Some(e) = ent.checked_add(i * esz).filter(|e| e + 48 <= size) else { break };
            let e = e as usize;
            let (first, last) = (le64(d, e + 32).unwrap_or(0), le64(d, e + 40).unwrap_or(0));
            if first == 0 || last < first { continue; }
            let Some(s) = first.checked_mul(512).filter(|s| s.checked_add(1536).is_some_and(|x| x <= size)) else { continue };
            let s = s as usize;
            if has_boot_sig(d, s) {
                let len = (last - first + 1).saturating_mul(512).min(size - s as u64);
                return Ok((s, len as usize));
            }
            if is_hfs(d, s + 1024) { return Err(format!("HFS+ volume found (GPT partition {})", i + 1)); }
            if is_apfs(d, s) { return Err(format!("APFS container found (GPT partition {})", i + 1)); }
        }
    }
    // MBR (a FAT boot sector starts with a jump instead)
    if has_boot_sig(d, 0) && d[0] != 0xEB && d[0] != 0xE9 {
        for i in 0..4 {
            let e = 446 + i * 16;
            let (lba, cnt) = (le32(d, e + 8).unwrap_or(0) as u64, le32(d, e + 12).unwrap_or(0) as u64);
            if d[e + 4] != 0 && lba != 0 && (lba + cnt) * 512 <= size {
                return Ok(((lba * 512) as usize, (cnt * 512) as usize));
            }
        }
    }
    if size > 1536 && is_hfs(d, 1024) { return Err(String::from("HFS+ volume found")); }
    if size > 64 && is_apfs(d, 0) { return Err(String::from("APFS container found")); }
    Ok((0, d.len()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fat::tests::{manifest, unz, Lister};
    use crate::fat::Volume;
    use std::format;

    fn vector(name: &str) -> Vec<u8> {
        let z = match name {
            "dmg_udzo.dmg" => &include_bytes!("../testdata/dmg_udzo.dmg.z")[..],
            "dmg_raw.dmg" => &include_bytes!("../testdata/dmg_raw.dmg.z")[..],
            "dmg_mbr.dmg" => &include_bytes!("../testdata/dmg_mbr.dmg.z")[..],
            "dmg_gpt.dmg" => &include_bytes!("../testdata/dmg_gpt.dmg.z")[..],
            "dmg_hfs.dmg" => &include_bytes!("../testdata/dmg_hfs.dmg.z")[..],
            "dmg_bzip2.dmg" => &include_bytes!("../testdata/dmg_bzip2.dmg.z")[..],
            "dmg_plain.dmg" => &include_bytes!("../testdata/dmg_plain.dmg.z")[..],
            _ => panic!("no vector {name}"),
        };
        unz(z)
    }

    /// Decode, find the partition, mount: the error message, or the volume's manifest lines.
    fn open(file: &[u8]) -> Result<(String, std::collections::BTreeSet<String>), String> {
        let disk = extract(file)?;
        let (off, len) = find_partition(&disk.bytes)?;
        let v = Volume::open(&disk.bytes[off..off + len]).map_err(String::from)?;
        let mut b = Lister::default();
        let info = v.load(usize::MAX, &mut b);
        Ok((format!("label {} fat {}", info.label, info.bits), b.lines))
    }

    #[test]
    fn all_vectors() {
        let small = manifest(include_str!("../testdata/small.txt"));
        for line in include_str!("../testdata/dmg_expect.txt").lines() {
            let mut f = line.splitn(3, ' ');
            let (name, kind, rest) = (f.next().unwrap(), f.next().unwrap(), f.next().unwrap());
            let r = open(&vector(name));
            match kind {
                "ok" => { assert_eq!(rest, "small"); assert_eq!(r.as_ref().ok(), Some(&small), "{name}: {:?}", r.as_ref().err()); }
                _ => { let e = r.err().unwrap_or_default(); assert!(e.contains(rest), "{name}: {e:?}"); }
            }
        }
    }

    #[test]
    fn sample_dmg() {
        // the image in C:\Samples (tools/mkdmg.py)
        let (head, lines) = open(include_bytes!("../../rootfs/Samples/Sample.dmg")).expect("Sample.dmg mounts");
        assert!(head.starts_with("label "), "{head}");
        assert!(lines.iter().any(|l| l.contains("README.txt")), "{lines:?}");
    }

    #[test]
    fn fuzz_no_panic() {
        let names = ["dmg_udzo.dmg", "dmg_raw.dmg", "dmg_mbr.dmg", "dmg_gpt.dmg"];
        let bases: Vec<Vec<u8>> = names.iter().map(|n| vector(n)).collect();
        let mut seed = 0xD1A6u32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for round in 0..1500 {
            let mut f = bases[round % bases.len()].clone();
            let len = f.len();
            for _ in 0..1 + rnd() % 12 {
                // the trailer, the plist and the start of the data are where the structure is
                let k = match rnd() % 3 { 0 => len - 1 - rnd() as usize % 512, 1 => len - 512 - 1 - rnd() as usize % 2000.min(len - 513), _ => rnd() as usize % len };
                f[k] = if rnd() % 2 == 0 { rnd() as u8 } else { f[k] ^ (1 << (rnd() % 8)) };
            }
            if round % 9 == 0 { let n = rnd() as usize % len; f.truncate(n); }
            let _ = open(&f);
        }
        // partition maps with absurd entries
        let mut d = vec![0u8; 8192];
        d[512..520].copy_from_slice(b"EFI PART");
        for (o, v) in [(72usize, 0xFFFF_FFF0u32), (80, 200), (84, 0xFFFF_FFFF)] { d[512 + o..516 + o].copy_from_slice(&v.to_le_bytes()); }
        assert!(find_partition(&d).is_ok());
        let mut m = vec![0u8; 4096];
        m[510] = 0x55; m[511] = 0xAA; m[446 + 4] = 6;
        m[446 + 8..446 + 12].copy_from_slice(&0xFFFF_FFFFu32.to_le_bytes());
        m[446 + 12..446 + 16].copy_from_slice(&0xFFFF_FFFFu32.to_le_bytes());
        let (o, l) = find_partition(&m).unwrap();
        assert!(o + l <= m.len());
    }
}
