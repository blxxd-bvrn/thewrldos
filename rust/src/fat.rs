//! Read-only FAT12/16/32 volumes with VFAT long names, from an image in memory (disk images:
//! DMG, raw .img). Replaces src/kernel/fat.c; the C interface is in kernel.rs.
//!
//! The whole tree is read at once and handed to a [`Builder`] (in the kernel: VFS nodes).
//! Images come from downloads, so nothing in them is trusted: every read is bounds-checked,
//! cluster chains stop at the volume's cluster count (which also ends loops), the total
//! file data copied is limited to the image's size (cross-linked files cannot multiply
//! memory), and the number of entries is limited (directories that point back at their
//! ancestors cannot make the tree explode). The C version could read and write past its
//! buffers on such images.

use alloc::string::String;
use alloc::vec::Vec;

/// A FAT directory entry's date and time, as stored (month 1-12, seconds even).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DosTime {
    pub year: u16,
    pub month: u8,
    pub day: u8,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
}

impl DosTime {
    fn from_fat(date: u16, time: u16) -> Self {
        DosTime {
            year: 1980 + (date >> 9),
            month: ((date >> 5) & 15) as u8,
            day: (date & 31) as u8,
            hour: (time >> 11) as u8,
            minute: ((time >> 5) & 63) as u8,
            second: ((time & 31) * 2) as u8,
        }
    }
}

/// Receives the volume's tree, parents before their children.
pub trait Builder {
    type Node: Copy;
    fn dir(&mut self, parent: Self::Node, name: &str, mtime: DosTime) -> Self::Node;
    fn file(&mut self, parent: Self::Node, name: &str, mtime: DosTime, data: Vec<u8>);
}

/// What was mounted.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Info {
    /// 12, 16 or 32
    pub bits: u8,
    /// from the boot sector; "Untitled" when it has none
    pub label: String,
    pub clusters: u32,
    /// the image was damaged: some entries or data were left out
    pub damaged: bool,
}

/// Entries one volume may have (the largest FAT32 volume an image here can hold has far fewer).
const MAX_ENTRIES: usize = 200_000;
const MAX_DEPTH: u32 = 16;
/// FAT allows 65,536 entries in a directory: 2 MB.
const MAX_DIR_BYTES: usize = 65536 * 32;

fn le16(b: &[u8], o: usize) -> Option<u16> { Some(u16::from_le_bytes([*b.get(o)?, *b.get(o + 1)?])) }
fn le32(b: &[u8], o: usize) -> Option<u32> { Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?)) }

/// A volume's layout, read from its boot sector.
pub struct Volume<'a> {
    img: &'a [u8],
    bits: u8,
    cluster_size: usize,
    fat_off: usize,
    data_off: usize,
    root_off: usize,
    root_entries: usize,
    root_clus: u32,
    nclus: u32,
    label: String,
}

impl<'a> Volume<'a> {
    /// Checks the boot sector. The errors are the messages the C version gave (and one more).
    pub fn open(img: &'a [u8]) -> Result<Self, &'static str> {
        if img.len() < 512 { return Err("Image too small"); }
        let bps = le16(img, 11).unwrap_or(0) as u64;
        let spc = img[13] as u64;
        let nfats = img[16] as u64;
        if img[510] != 0x55 || img[511] != 0xAA || bps == 0 || spc == 0 || nfats == 0 {
            return Err("No FAT boot sector found");
        }
        let reserved = le16(img, 14).unwrap_or(0) as u64;
        let root_entries = le16(img, 17).unwrap_or(0) as u64;
        let fatsz = match le16(img, 22).unwrap_or(0) { 0 => le32(img, 36).unwrap_or(0) as u64, n => n as u64 };
        let total = match le16(img, 19).unwrap_or(0) { 0 => le32(img, 32).unwrap_or(0) as u64, n => n as u64 };
        let root_secs = (root_entries * 32).div_ceil(bps);
        let data_sec = reserved + nfats * fatsz + root_secs;
        if total < data_sec { return Err("Damaged FAT boot sector (no room for data)"); }
        let nclus = ((total - data_sec) / spc).min(0x0FFF_FFF0);
        let bits = if nclus < 4085 { 12 } else if nclus < 65525 { 16 } else { 32 };
        let off = |sectors: u64| (sectors * bps).min(usize::MAX as u64) as usize;
        let lab_at = if bits == 32 { 71 } else { 43 };
        let mut label: String = img[lab_at..lab_at + 11].iter().map(|&c| if (0x20..0x7F).contains(&c) { c as char } else { '_' }).collect();
        while label.ends_with(' ') { label.pop(); }
        if label.is_empty() || label.starts_with("NO NAME") { label = String::from("Untitled"); }
        Ok(Volume {
            img,
            bits,
            cluster_size: (spc * bps) as usize,
            fat_off: off(reserved),
            data_off: off(data_sec),
            root_off: off(reserved + nfats * fatsz),
            root_entries: root_entries as usize,
            root_clus: le32(img, 44).unwrap_or(0),
            nclus: nclus as u32,
            label,
        })
    }

    pub fn bits(&self) -> u8 { self.bits }
    pub fn label(&self) -> &str { &self.label }

    /// The cluster after `c`, or None at the end of the chain or on anything invalid.
    fn next(&self, c: u32) -> Option<u32> {
        let f = self.fat_off;
        let c = c as usize;
        let v = match self.bits {
            12 => {
                let w = le16(self.img, f.checked_add(c + c / 2)?)? as u32;
                let v = if c & 1 == 1 { w >> 4 } else { w & 0xFFF };
                if v >= 0xFF8 { return None; }
                v
            }
            16 => {
                let v = le16(self.img, f.checked_add(c * 2)?)? as u32;
                if v >= 0xFFF8 { return None; }
                v
            }
            _ => {
                let v = le32(self.img, f.checked_add(c * 4)?)? & 0x0FFF_FFFF;
                if v >= 0x0FFF_FFF8 { return None; }
                v
            }
        };
        self.valid(v).then_some(v)
    }

    fn valid(&self, c: u32) -> bool { c >= 2 && c - 2 < self.nclus }

    /// The bytes of cluster `c` that are inside the image (a damaged image may end early).
    fn cluster(&self, c: u32) -> &'a [u8] {
        let off = (c as u64 - 2) * self.cluster_size as u64 + self.data_off as u64;
        if off >= self.img.len() as u64 { return &[]; }
        let off = off as usize;
        &self.img[off..(off + self.cluster_size).min(self.img.len())]
    }

    /// Up to `limit` bytes of the chain starting at `first`.
    fn read_chain(&self, first: u32, limit: usize) -> Vec<u8> {
        let mut out = Vec::new();
        if !self.valid(first) { return out; }
        let mut c = Some(first);
        let mut steps = 0u32;
        while let Some(k) = c {
            if out.len() >= limit || steps > self.nclus { break; }   // a longer chain has a loop
            let part = self.cluster(k);
            if part.is_empty() { break; }
            let n = part.len().min(limit - out.len());
            out.extend_from_slice(&part[..n]);
            c = self.next(k);
            steps += 1;
        }
        out
    }

    /// Reads the whole tree into `b`, under `root`.
    pub fn load<B: Builder>(&self, root: B::Node, b: &mut B) -> Info {
        let mut w = Walk { vol: self, b, entries: 0, data_left: self.img.len(), damaged: false };
        if self.bits == 32 {
            let d = self.read_chain(self.root_clus, MAX_DIR_BYTES);
            w.dir(&d, root, 0);
        } else {
            let start = self.root_off.min(self.img.len());
            let end = start.saturating_add(self.root_entries * 32).min(self.img.len());
            w.damaged |= end - start < self.root_entries * 32;
            w.dir(&self.img[start..end], root, 0);
        }
        Info { bits: self.bits, label: self.label.clone(), clusters: self.nclus, damaged: w.damaged }
    }
}

struct Walk<'v, 'a, B: Builder> {
    vol: &'v Volume<'a>,
    b: &'v mut B,
    entries: usize,
    data_left: usize,
    damaged: bool,
}

/// Where the 13 UTF-16 characters of a long-name entry are.
const LFN_OFFS: [usize; 13] = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];

/// A character as the VFS gets it: ASCII, with path separators, control and non-ASCII
/// characters as '_'.
fn safe_char(c: u32) -> char {
    match c {
        0x20..=0x7E if c != '/' as u32 && c != '\\' as u32 => c as u8 as char,
        _ => '_',
    }
}

impl<B: Builder> Walk<'_, '_, B> {
    fn dir(&mut self, ents: &[u8], parent: B::Node, depth: u32) {
        if depth > MAX_DEPTH { self.damaged = true; return; }
        let mut lfn = [0u16; 260];
        let mut lfn_ok = false;
        for e in ents.chunks_exact(32) {
            if e[0] == 0 { break; }
            if e[0] == 0xE5 { lfn_ok = false; continue; }
            let attr = e[11];
            if attr == 0x0F {
                // a long-name piece: sequence number, 13 characters
                let seq = (e[0] & 0x1F) as usize;
                if e[0] & 0x40 != 0 { lfn = [0; 260]; lfn_ok = true; }
                for (k, &o) in LFN_OFFS.iter().enumerate() {
                    let ch = u16::from_le_bytes([e[o], e[o + 1]]);
                    let pos = (seq.wrapping_sub(1)).wrapping_mul(13).wrapping_add(k);
                    if pos < 259 && ch != 0 && ch != 0xFFFF { lfn[pos] = ch; }
                }
                continue;
            }
            if attr & 0x08 != 0 { lfn_ok = false; continue; }      // the volume label
            let mut name = String::new();
            if lfn_ok && lfn[0] != 0 {
                for &ch in lfn.iter().take_while(|&&c| c != 0).take(95) { name.push(safe_char(ch as u32)); }
            } else {
                // 8.3, lower-cased as the NT flags say
                let (lower_base, lower_ext) = (e[12] & 8 != 0, e[12] & 16 != 0);
                for &c in e[..8].iter().take_while(|&&c| c != b' ') {
                    name.push(safe_char(if lower_base { c.to_ascii_lowercase() } else { c } as u32));
                }
                if e[8] != b' ' {
                    name.push('.');
                    for &c in e[8..11].iter().take_while(|&&c| c != b' ') {
                        name.push(safe_char(if lower_ext { c.to_ascii_lowercase() } else { c } as u32));
                    }
                }
            }
            lfn_ok = false;
            if name.is_empty() || name == "." || name == ".." { continue; }
            if self.entries >= MAX_ENTRIES { self.damaged = true; return; }
            self.entries += 1;
            let clus = ((u16::from_le_bytes([e[20], e[21]]) as u32) << 16) | u16::from_le_bytes([e[26], e[27]]) as u32;
            let mtime = DosTime::from_fat(u16::from_le_bytes([e[24], e[25]]), u16::from_le_bytes([e[22], e[23]]));
            if attr & 0x10 != 0 {
                let node = self.b.dir(parent, &name, mtime);
                let d = self.vol.read_chain(clus, MAX_DIR_BYTES);
                self.dir(&d, node, depth + 1);
            } else {
                let size = u32::from_le_bytes([e[28], e[29], e[30], e[31]]) as usize;
                let want = size.min(self.data_left);
                if want < size { self.damaged = true; }
                let data = if want > 0 && clus >= 2 { self.vol.read_chain(clus, want) } else { Vec::new() };
                self.data_left -= data.len();
                self.b.file(parent, &name, mtime, data);
            }
        }
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::inflate::inflate_zlib;
    use std::collections::BTreeSet;
    use std::format;

    /// Records the tree as manifest lines (see tools/gen_diskimage_testdata.py).
    #[derive(Default)]
    pub struct Lister { pub paths: Vec<String>, pub lines: BTreeSet<String> }

    pub fn fnv(b: &[u8]) -> u64 { b.iter().fold(0xcbf29ce484222325u64, |h, &c| (h ^ c as u64).wrapping_mul(0x100000001b3)) }

    impl Builder for Lister {
        type Node = usize;
        fn dir(&mut self, parent: usize, name: &str, _t: DosTime) -> usize {
            let p = if parent == usize::MAX { String::from(name) } else { format!("{}/{}", self.paths[parent], name) };
            self.lines.insert(format!("D {p}"));
            self.paths.push(p);
            self.paths.len() - 1
        }
        fn file(&mut self, parent: usize, name: &str, t: DosTime, data: Vec<u8>) {
            let p = if parent == usize::MAX { String::from(name) } else { format!("{}/{}", self.paths[parent], name) };
            self.lines.insert(format!("F {} {} {:016x} {:04}-{:02}-{:02} {:02}:{:02}:{:02}", p, data.len(), fnv(&data),
                                      t.year, t.month, t.day, t.hour, t.minute, t.second));
        }
    }

    pub fn unz(z: &[u8]) -> Vec<u8> {
        let mut out = vec![0u8; 64 << 20];
        let d = inflate_zlib(z, &mut out).expect("test vector decompresses");
        out.truncate(d.written);
        out
    }

    /// The manifest file: (label line, entry lines).
    pub fn manifest(text: &str) -> (String, BTreeSet<String>) {
        let mut l = text.lines();
        let head = String::from(l.next().unwrap());
        (head, l.map(String::from).collect())
    }

    fn check(img: &[u8], man: &str) {
        let (head, want) = manifest(man);
        let v = Volume::open(img).expect("mounts");
        let mut b = Lister::default();
        let info = v.load(usize::MAX, &mut b);
        assert_eq!(format!("label {} fat {}", info.label, info.bits), head);
        assert!(!info.damaged);
        let missing: Vec<_> = want.difference(&b.lines).collect();
        let extra: Vec<_> = b.lines.difference(&want).collect();
        assert!(missing.is_empty() && extra.is_empty(), "missing {missing:?}\nextra {extra:?}");
    }

    #[test]
    fn fat12() { check(&unz(include_bytes!("../testdata/fat12.img.z")), include_str!("../testdata/fat12.txt")); }
    #[test]
    fn fat16() { check(&unz(include_bytes!("../testdata/fat16.img.z")), include_str!("../testdata/fat16.txt")); }
    #[test]
    fn fat32() { check(&unz(include_bytes!("../testdata/fat32.img.z")), include_str!("../testdata/fat32.txt")); }

    #[test]
    fn rejects() {
        assert_eq!(Volume::open(&[0u8; 100]).err(), Some("Image too small"));
        assert_eq!(Volume::open(&[0u8; 512]).err(), Some("No FAT boot sector found"));
        let mut img = unz(include_bytes!("../testdata/fat12.img.z"));
        img[19] = 10; img[20] = 0;                               // 10 sectors in all: less than the FAT and root
        assert!(Volume::open(&img).is_err());
    }

    /// Damaged images: errors or partial trees, never a panic, a loop or a blow-up.
    #[test]
    fn fuzz_no_panic() {
        let base = unz(include_bytes!("../testdata/fat12.img.z"));
        let mut seed = 0xC0FFEEu32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for round in 0..3000 {
            let mut img = base.clone();
            // aim at the interesting parts: boot sector, FATs, root directory, the first data
            for _ in 0..1 + rnd() % 24 {
                let region = [0usize, 512, 0x2600, 0x4200][rnd() as usize % 4];
                let k = (region + rnd() as usize % 4096).min(img.len() - 1);
                img[k] = if rnd() % 2 == 0 { rnd() as u8 } else { img[k] ^ (1 << (rnd() % 8)) };
            }
            if round % 7 == 0 { let n = rnd() as usize % img.len(); img.truncate(n.max(1)); }
            if let Ok(v) = Volume::open(&img) {
                let mut b = Lister::default();
                v.load(usize::MAX, &mut b);
                assert!(b.lines.len() <= MAX_ENTRIES);
            }
        }
    }

    /// A directory whose subdirectory is itself, over and over: limited, not exponential.
    #[test]
    fn self_referencing_directories() {
        let mut img = unz(include_bytes!("../testdata/fat12.img.z"));
        // FAT12 floppy: root at sector 19; make 16 root entries all directories pointing at cluster 2,
        // and make cluster 2 (sector 33) hold the same 16 entries
        let mut ents = Vec::new();
        for i in 0..16u8 {
            let mut e = [b' '; 32];
            e[0] = b'A' + i; e[11] = 0x10; e[12..32].fill(0); e[26] = 2;
            ents.extend_from_slice(&e);
        }
        img[19 * 512..19 * 512 + ents.len()].copy_from_slice(&ents);
        img[33 * 512..33 * 512 + ents.len()].copy_from_slice(&ents);
        let v = Volume::open(&img).unwrap();
        let mut b = Lister::default();
        let info = v.load(usize::MAX, &mut b);
        assert!(info.damaged);
        assert!(b.lines.len() <= MAX_ENTRIES);
    }
}
