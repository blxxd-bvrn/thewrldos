//! NTFS 3.1 for installed systems (was `src/kernel/ntfs.c`): format, mount, read and write.
//!
//! On disk: 4 KiB clusters, 1 KiB MFT records and 4 KiB index blocks, both with
//! update-sequence fixups. Files keep $STANDARD_INFORMATION, $FILE_NAME (Win32 namespace), a
//! resident $SECURITY_DESCRIPTOR and an unnamed $DATA (resident when small, otherwise a run
//! list). Directories keep a $I30 index of $FILE_NAME keys: $INDEX_ROOT, plus
//! $INDEX_ALLOCATION and $BITMAP when the entries do not fit in the record.
//!
//! In memory the driver keeps the directory tree, the cluster bitmap and the MFT bitmap.
//! Writes go straight to the file's record and clusters; a changed directory's index is
//! rebuilt (as a balanced B-tree) by `sync`. Limits: no compression, encryption, attribute
//! lists or alternate streams, and the MFT does not grow (16384 records on volumes of 512 MB
//! or more).
//!
//! It works over a [`Dev`], so it runs on the build machine against disk images in memory. On
//! valid volumes it writes exactly what the C driver wrote. What it reads from a disk is
//! checked: attribute and name offsets against their record, run lists against their
//! attribute and the volume, bitmap and MFT sizes against the volume, file sizes against
//! their runs; walks over the tree are iterative, so a deep or looping tree cannot overflow
//! the kernel stack.

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

/// The disk under a volume, in 512-byte sectors, and what the driver needs of the system.
pub trait Dev {
    fn read(&mut self, lba: u32, count: u32, buf: &mut [u8]) -> bool;
    fn write(&mut self, lba: u32, count: u32, buf: &[u8]) -> bool;
    /// The current time in NTFS units (100 ns since 1601).
    fn now(&self) -> u64;
    fn rand(&mut self) -> u32;
    fn log(&mut self, _msg: &str) {}
}

/// NTFS time from a calendar time.
pub fn ntfs_time(year: i32, month: i32, day: i32, hour: i32, minute: i32, second: i32) -> u64 {
    let y = year - (month <= 2) as i32;
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = (y - era * 400) as u32;
    let doy = ((153 * (month + if month > 2 { -3 } else { 9 }) + 2) / 5 + day - 1) as u32;
    let days = era as i64 * 146_097 + yoe as i64 * 365 + (yoe / 4) as i64 - (yoe / 100) as i64 + doy as i64 - 719_468;
    let secs = days * 86400 + hour as i64 * 3600 + minute as i64 * 60 + second as i64;
    ((secs + 11_644_473_600) as u64).wrapping_mul(10_000_000)
}

const CLUSTER: u64 = 4096;
const SPC: u64 = CLUSTER / 512;
const RECSZ: usize = 1024;
const IDXSZ: usize = 4096;
const FIRST_USER_REC: u32 = 24;
const AT_SI: u32 = 0x10;
const AT_FN: u32 = 0x30;
const AT_SD: u32 = 0x50;
const AT_VOLNAME: u32 = 0x60;
const AT_VOLINFO: u32 = 0x70;
const AT_DATA: u32 = 0x80;
const AT_IROOT: u32 = 0x90;
const AT_IALLOC: u32 = 0xA0;
const AT_BITMAP: u32 = 0xB0;
const AT_END: u32 = 0xFFFF_FFFF;
const SPARSE: u64 = !0;
/// Most MFT records and bitmap bytes a mounted volume may claim.
const MAX_RECS: u64 = 1 << 22;
const MAX_MFTBM: u64 = 1 << 20;
/// Largest cluster bitmap mounted: 64 MB (a 2 TB volume).
const MAX_BM: u64 = 64 << 20;

fn mref(rec: u32, seq: u16) -> u64 { rec as u64 | (seq as u64) << 48 }
fn g16(b: &[u8], o: usize) -> u16 { b.get(o..o + 2).map_or(0, |s| u16::from_le_bytes([s[0], s[1]])) }
fn g32(b: &[u8], o: usize) -> u32 { b.get(o..o + 4).map_or(0, |s| u32::from_le_bytes(s.try_into().unwrap())) }
fn g64(b: &[u8], o: usize) -> u64 { b.get(o..o + 8).map_or(0, |s| u64::from_le_bytes(s.try_into().unwrap())) }
fn p16(b: &mut [u8], o: usize, v: u16) { b[o..o + 2].copy_from_slice(&v.to_le_bytes()); }
fn p32(b: &mut [u8], o: usize, v: u32) { b[o..o + 4].copy_from_slice(&v.to_le_bytes()); }
fn p64(b: &mut [u8], o: usize, v: u64) { b[o..o + 8].copy_from_slice(&v.to_le_bytes()); }
fn al8(x: usize) -> usize { (x + 7) & !7 }

/// $UpCase for the characters this driver folds.
pub fn upcase16(c: u16) -> u16 {
    match c {
        0x61..=0x7A => c - 32,
        0xE0..=0xFE if c != 0xF7 => c - 32,
        0xFF => 0x178,
        0x100..=0x17F => {
            if (0x139..=0x148).contains(&c) || (0x179..=0x17E).contains(&c) { if c & 1 != 0 { c } else { c - 1 } }
            else if c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F { if c & 1 != 0 { c - 1 } else { c } }
            else { c }
        }
        0x3B1..=0x3C9 if c != 0x3C2 => c - 32,
        0x3C2 => 0x3A3,
        0x430..=0x44F => c - 32,
        0x450..=0x45F => c - 80,
        0xFF41..=0xFF5A => c - 32,
        _ => c,
    }
}
/// COLLATION_FILENAME.
fn name_cmp(a: &[u16], b: &[u16]) -> core::cmp::Ordering {
    use core::cmp::Ordering::*;
    for (x, y) in a.iter().zip(b) { let (x, y) = (upcase16(*x), upcase16(*y)); if x != y { return x.cmp(&y); } }
    if a.len() != b.len() { return a.len().cmp(&b.len()); }
    for (x, y) in a.iter().zip(b) { if x != y { return x.cmp(y); } }
    Equal
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Run { lcn: u64, len: u64 }

#[derive(Clone, Default)]
struct Node {
    rec: u32, seq: u16,
    dir: bool, sys: bool, dirty: bool, resident: bool,
    fattr: u32,
    ctime: u64, mtime: u64,
    size: u64, alloc: u64,
    runs: Vec<Run>,
    /// The name as stored: Latin-1 bytes ('?' above 255), at most 255.
    name: Vec<u8>,
    n16: Vec<u16>,
    parent: Option<usize>,
    /// Children, most recently linked first (the C driver's list).
    kids: Vec<usize>,
}

const DEFAULT_SD: [u8; 80] = [
    0x01, 0x00, 0x04, 0x80, 48, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0,
    // DACL: Everyone, full control, inherited by files and folders
    0x02, 0x00, 28, 0, 1, 0, 0, 0, 0x00, 0x03, 20, 0, 0xFF, 0x01, 0x1F, 0x00, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    // owner and group: BUILTIN\Administrators
    1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 0x02, 0, 0,
    1, 2, 0, 0, 0, 0, 0, 5, 0x20, 0, 0, 0, 0x20, 0x02, 0, 0];
const I30: [u16; 4] = [b'$' as u16, b'I' as u16, b'3' as u16, b'0' as u16];

/// What `load_tree` hands its builder.
pub trait Tree {
    type Node: Copy;
    fn dir(&mut self, parent: Self::Node, name: &[u8]) -> Self::Node;
    /// A file: its contents, or None when they could not be read (it is then empty).
    fn file(&mut self, parent: Self::Node, name: &[u8], readonly: bool, data: Option<Vec<u8>>);
    fn progress(&mut self, _done: u64, _total: u64) {}
}

pub struct Vol<D: Dev> {
    pub dev: D,
    lba: u32,
    total_sectors: u64,
    nclus: u64,
    mft_runs: Vec<Run>,
    nrec: u32,
    mftbm_runs: Vec<Run>,
    mftbm: Vec<u8>,
    mftbm_bytes: u32,
    mftbm_dirty: bool,
    bm_runs: Vec<Run>,
    bm: Vec<u8>,
    bm_bytes: u32,
    bm_dirty: bool,
    free_clusters: u64,
    hint: u64,
    serial64: u64,
    /// The label, NUL-terminated.
    label: [u8; 33],
    nodes: Vec<Option<Node>>,
    root: usize,
    usn: u16,
}

/// A record or index block being built.
fn rec_init(b: &mut [u8], n: u32, seq: u16, flags: u16) {
    b[..RECSZ].fill(0);
    b[..4].copy_from_slice(b"FILE");
    p16(b, 0x04, 0x30); p16(b, 0x06, 3);
    p16(b, 0x10, if seq != 0 { seq } else { 1 });
    p16(b, 0x12, 1);
    p16(b, 0x14, 0x38);
    p16(b, 0x16, flags);
    p32(b, 0x1C, RECSZ as u32);
    p32(b, 0x2C, n);
    p32(b, 0x38, AT_END);
    p32(b, 0x18, 0x38 + 8);
}
/// Appends an attribute of `len` bytes before the end marker: its offset, or None.
fn rec_add(b: &mut [u8], ty: u32, len: usize) -> Option<usize> {
    let at = (g32(b, 0x18) as usize).checked_sub(8)?;
    let len = al8(len);
    if at + len + 8 > RECSZ { return None; }
    b[at..at + len].fill(0);
    p32(b, at, ty); p32(b, at + 4, len as u32);
    let inst = g16(b, 0x28); p16(b, 0x28, inst.wrapping_add(1)); p16(b, at + 0x0E, inst);
    p32(b, at + len, AT_END);
    p32(b, 0x18, (at + len + 8) as u32);
    Some(at)
}
fn add_resident(b: &mut [u8], ty: u32, name: &[u16], val: &[u8], rflags: u8) -> bool {
    let hdr = 0x18 + al8(name.len() * 2);
    let Some(at) = rec_add(b, ty, hdr + val.len()) else { return false };
    b[at + 9] = name.len() as u8; p16(b, at + 10, 0x18);
    for (i, &c) in name.iter().enumerate() { p16(b, at + 0x18 + 2 * i, c); }
    p32(b, at + 0x10, val.len() as u32); p16(b, at + 0x14, hdr as u16); b[at + 0x16] = rflags;
    b[at + hdr..at + hdr + val.len()].copy_from_slice(val);
    true
}
fn encode_runs(r: &[Run], cap: usize) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let mut prev: i64 = 0;
    for run in r {
        let mut lb = 0u8;
        while lb < 8 && run.len >> (8 * lb as u32) != 0 { lb += 1; }
        if lb == 0 { lb = 1; }
        let sparse = run.lcn == SPARSE;
        let d = if sparse { 0 } else { (run.lcn as i64).wrapping_sub(prev) };
        let mut ob = 0u8;
        if !sparse { ob = 1; while ob < 8 && !(d >= -(1i64 << (8 * ob - 1)) && d < (1i64 << (8 * ob - 1))) { ob += 1; } }
        if out.len() + 1 + lb as usize + ob as usize + 1 > cap { return None; }
        out.push(lb | ob << 4);
        for k in 0..lb { out.push((run.len >> (8 * k)) as u8); }
        for k in 0..ob { out.push(((d as u64) >> (8 * k)) as u8); }
        if !sparse { prev = run.lcn as i64; }
    }
    out.push(0);
    Some(out)
}
fn add_nonresident(b: &mut [u8], ty: u32, name: &[u16], r: &[Run], size: u64) -> bool {
    let Some(map) = encode_runs(r, 512) else { return false };
    let clus: u64 = r.iter().fold(0u64, |a, x| a.wrapping_add(x.len));
    let nameoff = 0x40;
    let mapoff = al8(nameoff + name.len() * 2);
    let Some(at) = rec_add(b, ty, mapoff + map.len()) else { return false };
    b[at + 8] = 1; b[at + 9] = name.len() as u8; p16(b, at + 10, nameoff as u16);
    for (i, &c) in name.iter().enumerate() { p16(b, at + nameoff + 2 * i, c); }
    p64(b, at + 0x10, 0);
    p64(b, at + 0x18, clus.saturating_sub(1));
    p16(b, at + 0x20, mapoff as u16);
    p64(b, at + 0x28, clus.wrapping_mul(CLUSTER));
    p64(b, at + 0x30, size);
    p64(b, at + 0x38, size);
    b[at + mapoff..at + mapoff + map.len()].copy_from_slice(&map);
    true
}
/// An attribute by type and name (None: unnamed): its offset in the record.
fn find_attr(b: &[u8], ty: u32, aname: Option<&[u8]>) -> Option<usize> {
    let mut off = g16(b, 0x14) as usize;
    while off + 8 <= RECSZ {
        let (t, len) = (g32(b, off), g32(b, off + 4) as usize);
        if t == AT_END || len < 0x18 || off + len > RECSZ { break; }
        if t == ty {
            let nl = b[off + 9] as usize;
            let no = g16(b, off + 10) as usize;
            let matched = match aname {
                None => nl == 0,
                Some(a) => nl == a.len() && off + no + 2 * nl <= RECSZ && a.iter().enumerate().all(|(i, &c)| g16(b, off + no + 2 * i) == c as u16),
            };
            if matched { return Some(off); }
        }
        off += len;
    }
    None
}
/// The run list of a non-resident attribute (at most `max` runs), read within the attribute.
fn decode_runs(b: &[u8], a: usize, max: usize) -> Vec<Run> {
    let mut out = Vec::new();
    let mut p = a + g16(b, a + 0x20) as usize;
    let end = (a + g32(b, a + 4) as usize).min(b.len());
    let mut lcn: i64 = 0;
    while p < end && b[p] != 0 && out.len() < max {
        let (lb, ob) = ((b[p] & 15) as usize, (b[p] >> 4) as usize);
        p += 1;
        if p + lb + ob > end { break; }
        let mut len = 0u64;
        for k in 0..lb.min(8) { len |= (b[p + k] as u64) << (8 * k); }
        p += lb;
        if ob != 0 {
            let mut d: i64 = 0;
            for k in 0..ob.min(8) { d |= (b[p + k] as i64) << (8 * k); }
            if b[p + ob - 1] & 0x80 != 0 && ob < 8 { d |= (!0u64 << (8 * ob)) as i64; }
            lcn = lcn.wrapping_add(d);
            out.push(Run { lcn: lcn as u64, len });
        } else { out.push(Run { lcn: SPARSE, len }); }
        p += ob;
    }
    out
}
/// A resident attribute's value, if it lies within the record.
fn resident_value(b: &[u8], a: usize) -> Option<&[u8]> {
    let (len, off) = (g32(b, a + 0x10) as usize, a + g16(b, a + 0x14) as usize);
    b.get(off..off.checked_add(len)?)
}
/// The bytes a record has left after its header, SI, FN and SD (and the end marker).
fn rec_room(nlen: usize, sd: bool) -> i32 { RECSZ as i32 - 0x38 - 0x48 - al8(0x18 + 0x42 + nlen * 2) as i32 - if sd { 0x68 } else { 0 } - 8 }

struct Ient { key: Vec<u8>, refn: u64, child: i64 }
fn ient_size(e: &Ient) -> usize { al8(0x10 + e.key.len()) + if e.child >= 0 { 8 } else { 0 } }
fn put_entry(o: &mut [u8], e: Option<&Ient>, last_child: i64) -> usize {
    match e {
        None => {
            let len = 0x10 + if last_child >= 0 { 8 } else { 0 };
            o[..len].fill(0);
            p16(o, 8, len as u16); p16(o, 12, 2 | (last_child >= 0) as u16);
            if last_child >= 0 { p64(o, len - 8, last_child as u64); }
            len
        }
        Some(e) => {
            let len = ient_size(e);
            o[..len].fill(0);
            p64(o, 0, e.refn); p16(o, 8, len as u16); p16(o, 10, e.key.len() as u16);
            p16(o, 12, (e.child >= 0) as u16);
            o[0x10..0x10 + e.key.len()].copy_from_slice(&e.key);
            if e.child >= 0 { p64(o, len - 8, e.child as u64); }
            len
        }
    }
}
fn key_name(k: &[u8]) -> Vec<u16> { (0..k[0x40] as usize).map(|i| g16(k, 0x42 + 2 * i)).collect() }
const BLK_ENTRIES: usize = IDXSZ - 0x40 - 0x18;
const ROOT_RESERVE: i32 = 0x20 + 0x20 + 0x60 + 0x30;

fn fixup_undo(b: &mut [u8], size: usize) -> bool {
    let (ofs, cnt) = (g16(b, 4) as usize, g16(b, 6) as usize);
    if ofs + cnt * 2 > size { return false; }
    let usn = g16(b, ofs);
    let mut i = 1;
    while i < cnt && i * 512 <= size {
        let end = i * 512 - 2;
        if g16(b, end) != usn { return false; }
        let v = g16(b, ofs + 2 * i); p16(b, end, v);
        i += 1;
    }
    true
}

impl<D: Dev> Vol<D> {
    fn node(&self, i: usize) -> &Node { self.nodes[i].as_ref().unwrap() }
    fn nm(&mut self, i: usize) -> &mut Node { self.nodes[i].as_mut().unwrap() }
    fn add_node(&mut self, n: Node) -> usize { self.nodes.push(Some(n)); self.nodes.len() - 1 }

    pub fn serial(&self) -> u32 { self.serial64 as u32 }
    pub fn label(&self) -> &[u8; 33] { &self.label }
    pub fn nrec(&self) -> u32 { self.nrec }
    pub fn free_bytes(&self) -> u64 { self.free_clusters.wrapping_mul(CLUSTER) }
    pub fn total_bytes(&self) -> u64 { self.nclus.wrapping_mul(CLUSTER) }

    // ------------------------------------------------------------ sectors and runs

    fn sec_read(&mut self, sector: u64, count: u32, buf: &mut [u8]) -> bool {
        if sector.saturating_add(count as u64) > self.total_sectors + 1 || buf.len() < count as usize * 512 { return false; }
        self.dev.read(self.lba.wrapping_add(sector as u32), count, buf)
    }
    fn sec_write(&mut self, sector: u64, count: u32, buf: &[u8]) -> bool {
        if sector.saturating_add(count as u64) > self.total_sectors + 1 || buf.len() < count as usize * 512 { return false; }
        self.dev.write(self.lba.wrapping_add(sector as u32), count, buf)
    }
    /// A byte offset inside a run list as a volume sector.
    fn run_sector(r: &[Run], off: u64) -> Option<u64> {
        let mut vcn = off / CLUSTER;
        for run in r {
            if vcn < run.len {
                if run.lcn == SPARSE { return None; }
                return Some(run.lcn.wrapping_add(vcn).wrapping_mul(SPC).wrapping_add((off % CLUSTER) / 512));
            }
            vcn -= run.len;
        }
        None
    }
    fn runs_read(&mut self, r: &[Run], mut off: u64, buf: &mut [u8]) -> bool {
        let mut done = 0;
        while done < buf.len() {
            let Some(s) = Self::run_sector(r, off) else { return false };
            let chunk = (buf.len() - done).min((CLUSTER - off % CLUSTER) as usize);
            let secs = chunk.div_ceil(512);
            if chunk % 512 == 0 { if !self.sec_read(s, secs as u32, &mut buf[done..done + chunk]) { return false; } }
            else { let mut t = vec![0u8; secs * 512]; if !self.sec_read(s, secs as u32, &mut t) { return false; } buf[done..done + chunk].copy_from_slice(&t[..chunk]); }
            done += chunk; off += chunk as u64;
        }
        true
    }
    fn runs_write(&mut self, r: &[Run], mut off: u64, buf: &[u8]) -> bool {
        let mut done = 0;
        while done < buf.len() {
            let Some(s) = Self::run_sector(r, off) else { return false };
            let chunk = (buf.len() - done).min((CLUSTER - off % CLUSTER) as usize);
            let secs = chunk.div_ceil(512);
            let ok = if chunk % 512 == 0 { self.sec_write(s, secs as u32, &buf[done..done + chunk]) }
                     else { let mut t = vec![0u8; secs * 512]; t[..chunk].copy_from_slice(&buf[done..done + chunk]); self.sec_write(s, secs as u32, &t) };
            if !ok { return false; }
            done += chunk; off += chunk as u64;
        }
        true
    }
    /// Update sequence numbers before writing a record or index block.
    fn fixup_apply(&mut self, b: &mut [u8], size: usize) {
        let (ofs, cnt) = (g16(b, 4) as usize, g16(b, 6) as usize);
        self.usn = self.usn.wrapping_add(1);
        if self.usn == 0 || self.usn == 0xFFFF { self.usn = 1; }
        p16(b, ofs, self.usn);
        let mut i = 1;
        while i < cnt && i * 512 <= size {
            let end = i * 512 - 2;
            let v = g16(b, end); p16(b, ofs + 2 * i, v); p16(b, end, self.usn);
            i += 1;
        }
    }
    fn read_rec(&mut self, n: u32, b: &mut [u8]) -> bool {
        if n >= self.nrec { return false; }
        let r = self.mft_runs.clone();
        if !self.runs_read(&r, n as u64 * RECSZ as u64, &mut b[..RECSZ]) { return false; }
        &b[..4] == b"FILE" && fixup_undo(b, RECSZ)
    }
    fn write_rec(&mut self, n: u32, b: &[u8]) -> bool {
        let mut t = b[..RECSZ].to_vec();
        self.fixup_apply(&mut t, RECSZ);
        let r = self.mft_runs.clone();
        self.runs_write(&r, n as u64 * RECSZ as u64, &t)
    }

    // ------------------------------------------------------------ clusters

    fn bm_get(&self, c: u64) -> bool { self.bm.get((c >> 3) as usize).is_some_and(|&b| b & (1 << (c & 7)) != 0) }
    fn bm_set(&mut self, c: u64, on: bool) {
        if on == self.bm_get(c) { return; }
        let Some(b) = self.bm.get_mut((c >> 3) as usize) else { return };
        if on { *b |= 1 << (c & 7); self.free_clusters = self.free_clusters.wrapping_sub(1); } else { *b &= !(1 << (c & 7)); self.free_clusters = self.free_clusters.wrapping_add(1); }
        self.bm_dirty = true;
    }
    fn free_runs(&mut self, r: &[Run]) {
        for run in r {
            if run.lcn == SPARSE || run.lcn >= self.nclus { continue; }
            let end = run.lcn.saturating_add(run.len).min(self.nclus);
            for c in run.lcn..end { self.bm_set(c, false); }
        }
    }
    /// Allocates n clusters: one contiguous run if possible, else up to `max` runs.
    fn alloc_runs(&mut self, n: u64, max: usize) -> Option<Vec<Run>> {
        if n == 0 { return Some(Vec::new()); }
        if n > self.free_clusters { return None; }
        for pass in 0..2 {
            let start = if pass == 1 { 0 } else { self.hint };
            let mut run = 0u64;
            for c in start..self.nclus {
                if self.bm_get(c) { run = 0; continue; }
                run += 1;
                if run == n {
                    let s = c + 1 - n;
                    for k in 0..n { self.bm_set(s + k, true); }
                    self.hint = c + 1;
                    return Some(vec![Run { lcn: s, len: n }]);
                }
            }
        }
        let mut out: Vec<Run> = Vec::new();
        let mut need = n;
        let mut c = 0;
        while c < self.nclus && need > 0 {
            if !self.bm_get(c) {
                match out.last_mut() {
                    Some(l) if l.lcn + l.len == c => l.len += 1,
                    _ => { if out.len() == max { let o = out.clone(); self.free_runs(&o); return None; } out.push(Run { lcn: c, len: 1 }); }
                }
                self.bm_set(c, true);
                need -= 1;
            }
            c += 1;
        }
        if need > 0 { let o = out.clone(); self.free_runs(&o); return None; }
        Some(out)
    }

    // ------------------------------------------------------------ the tree

    fn set_name(n: &mut Node, name: &[u8]) {
        let name = &name[..name.iter().position(|&c| c == 0).unwrap_or(name.len())];
        n.name = name[..name.len().min(255)].to_vec();
        n.n16 = name.iter().take(255).map(|&c| c as u16).collect();
    }
    fn child_named(&self, dir: usize, name: &[u8]) -> Option<usize> {
        self.node(dir).kids.iter().copied().find(|&c| {
            let cn = &self.node(c).name;
            cn.len() == name.len() && cn.iter().zip(name).all(|(&a, &b)| upcase16(a as u16) == upcase16(b as u16))
        })
    }
    /// A path's node, or with `parent_only` its folder and where the last part starts.
    fn lookup(&self, path: &[u8], parent_only: bool) -> Option<(usize, usize)> {
        let sep = |c: u8| c == b'\\' || c == b'/';
        let mut cur = self.root;
        let mut s = 0;
        while s < path.len() && sep(path[s]) { s += 1; }
        loop {
            let mut e = s; while e < path.len() && !sep(path[e]) { e += 1; }
            let mut nx = e; while nx < path.len() && sep(path[nx]) { nx += 1; }
            if parent_only && nx >= path.len() { return Some((cur, s)); }
            if e == s { return Some((cur, s)); }
            let c = self.child_named(cur, &path[s..e])?;
            if self.node(c).sys { return None; }
            cur = c; s = nx;
            if s >= path.len() { return Some((cur, s)); }
            if !self.node(cur).dir { return None; }
        }
    }
    fn link_child(&mut self, dir: usize, c: usize) { self.nm(c).parent = Some(dir); self.nm(dir).kids.insert(0, c); }
    fn unlink_child(&mut self, c: usize) {
        let Some(p) = self.node(c).parent else { return };
        self.nm(p).kids.retain(|&k| k != c);
        let n = self.nm(c); n.parent = None;
    }
    fn alloc_rec(&mut self) -> Option<u32> {
        for i in FIRST_USER_REC..self.nrec {
            let (byte, bit) = ((i >> 3) as usize, 1u8 << (i & 7));
            if self.mftbm.get(byte).is_some_and(|&b| b & bit == 0) { self.mftbm[byte] |= bit; self.mftbm_dirty = true; return Some(i); }
        }
        None
    }
    fn next_seq(&mut self, n: u32) -> u16 {
        let mut b = vec![0u8; RECSZ];
        let r = self.mft_runs.clone();
        if self.runs_read(&r, n as u64 * RECSZ as u64, &mut b) && &b[..4] == b"FILE" { let s = g16(&b, 0x10).wrapping_add(1); return if s != 0 { s } else { 1 }; }
        1
    }
    /// The $FILE_NAME value a node has in its parent.
    fn make_fn(&self, i: usize) -> Vec<u8> {
        let n = self.node(i);
        let p = self.node(n.parent.unwrap_or(i));
        let mut out = vec![0u8; 0x42 + n.n16.len() * 2];
        p64(&mut out, 0, mref(p.rec, p.seq));
        p64(&mut out, 0x08, n.ctime); p64(&mut out, 0x10, n.mtime); p64(&mut out, 0x18, n.mtime); p64(&mut out, 0x20, n.mtime);
        if !n.dir { p64(&mut out, 0x28, n.alloc); p64(&mut out, 0x30, n.size); }
        p32(&mut out, 0x38, n.fattr | if n.dir { 0x1000_0000 } else { 0 });
        out[0x40] = n.n16.len() as u8; out[0x41] = if n.sys { 3 } else { 1 };
        for (k, &c) in n.n16.iter().enumerate() { p16(&mut out, 0x42 + 2 * k, c); }
        out
    }
    /// $STANDARD_INFORMATION, $FILE_NAME and $SECURITY_DESCRIPTOR at the start of a record.
    fn rec_common_sd(&self, b: &mut [u8], i: usize, sd: bool) -> bool {
        let n = self.node(i);
        let mut si = [0u8; 0x30];
        p64(&mut si, 0, n.ctime); p64(&mut si, 8, n.mtime); p64(&mut si, 16, n.mtime); p64(&mut si, 24, n.mtime);
        p32(&mut si, 32, n.fattr);
        let f = self.make_fn(i);
        add_resident(b, AT_SI, &[], &si, 0) && add_resident(b, AT_FN, &[], &f, 1) && (!sd || add_resident(b, AT_SD, &[], &DEFAULT_SD, 0))
    }

    // ------------------------------------------------------------ directory indexes

    fn rebuild_dir(&mut self, d: usize) -> bool {
        let kids = self.node(d).kids.clone();
        let mut lv: Vec<Ient> = kids.iter().map(|&c| Ient { key: self.make_fn(c), refn: mref(self.node(c).rec, self.node(c).seq), child: -1 }).collect();
        if d == self.root { lv.push(Ient { key: self.make_fn(d), refn: mref(5, self.node(d).seq), child: -1 }); }
        let n = lv.len();
        lv.sort_by(|x, y| name_cmp(&key_name(&x.key), &key_name(&y.key)));
        let nlen = self.node(d).n16.len();
        let sd = rec_room(nlen, true) - ROOT_RESERVE >= 0x40;     // very long folder names: no room for a descriptor
        let room = (rec_room(nlen, sd) - ROOT_RESERVE).max(0x18) as usize;
        // the B-tree, bottom up; blocks receives the index blocks in VCN order
        let mut blocks: Vec<u8> = Vec::new();
        let mut nblocks = 0usize;
        let mut level_end: i64 = -1;
        loop {
            let total = 0x10 + if level_end >= 0 { 8 } else { 0 } + lv.iter().map(ient_size).sum::<usize>();
            if total <= room { break; }
            let mut up: Vec<Ient> = Vec::new();
            let mut i = 0;
            let cnt = lv.len();
            loop {
                blocks.resize((nblocks + 1) * IDXSZ, 0);
                let mut used = 0; let first = i;
                while i < cnt && used + ient_size(&lv[i]) <= BLK_ENTRIES - 8 { used += ient_size(&lv[i]); i += 1; }
                if i == first && i < cnt { i += 1; }              // always progress
                let fin = i >= cnt;
                let endchild = if fin { level_end } else { lv[i].child };
                let blk = &mut blocks[nblocks * IDXSZ..(nblocks + 1) * IDXSZ];
                let mut o = 0x40; let mut internal = endchild >= 0;
                for e in &lv[first..if fin { cnt } else { i }] { o += put_entry(&mut blk[o..], Some(e), 0); if e.child >= 0 { internal = true; } }
                o += put_entry(&mut blk[o..], None, endchild);
                blk[..4].copy_from_slice(b"INDX");
                p16(blk, 4, 0x28); p16(blk, 6, (1 + IDXSZ / 512) as u16);
                p64(blk, 0x10, nblocks as u64);
                p32(blk, 0x18, 0x28); p32(blk, 0x1C, (o - 0x18) as u32);
                p32(blk, 0x20, (IDXSZ - 0x18) as u32); blk[0x24] = internal as u8;
                let vcn = nblocks as i64; nblocks += 1;
                if fin { level_end = vcn; break; }
                let sep = &lv[i];
                up.push(Ient { key: sep.key.clone(), refn: sep.refn, child: vcn });
                i += 1;
            }
            lv = up;
        }
        // index blocks on disk: free the old ones, allocate new
        let old = core::mem::take(&mut self.nm(d).runs);
        self.free_runs(&old);
        let mut ok = true;
        if nblocks > 0 {
            match self.alloc_runs(nblocks as u64, 16) {
                None => ok = false,
                Some(r) => {
                    self.nm(d).runs = r.clone();
                    for b in 0..nblocks {
                        if !ok { break; }
                        let mut blk = blocks[b * IDXSZ..(b + 1) * IDXSZ].to_vec();
                        self.fixup_apply(&mut blk, IDXSZ);
                        ok = self.runs_write(&r, (b * IDXSZ) as u64, &blk);
                    }
                }
            }
        }
        // the directory record
        let mut b = vec![0u8; RECSZ];
        let (rec, seq) = (self.node(d).rec, self.node(d).seq);
        rec_init(&mut b, rec, seq, 3);
        ok = ok && self.rec_common_sd(&mut b, d, sd);
        let mut root = vec![0u8; RECSZ];
        p32(&mut root, 0, AT_FN); p32(&mut root, 4, 1);
        p32(&mut root, 8, IDXSZ as u32); root[12] = (IDXSZ as u64 / CLUSTER) as u8;
        let mut o = 0x20;
        for e in &lv { o += put_entry(&mut root[o..], Some(e), 0); }
        o += put_entry(&mut root[o..], None, level_end);
        let ilen = (o - 0x10) as u32;
        p32(&mut root, 0x10, 0x10); p32(&mut root, 0x14, ilen); p32(&mut root, 0x18, ilen);
        root[0x1C] = (level_end >= 0) as u8;
        ok = ok && add_resident(&mut b, AT_IROOT, &I30, &root[..0x10 + ilen as usize], 0);
        if ok && nblocks > 0 {
            let mut bmp = [0u8; 64];
            let bl = nblocks.div_ceil(64) * 8;
            if bl > bmp.len() { ok = false; }
            for i in 0..nblocks { if !ok { break; } bmp[i >> 3] |= 1 << (i & 7); }
            let runs = self.node(d).runs.clone();
            ok = ok && add_nonresident(&mut b, AT_IALLOC, &I30, &runs, (nblocks * IDXSZ) as u64) && add_resident(&mut b, AT_BITMAP, &I30, &bmp[..bl], 0);
        }
        if ok { ok = self.write_rec(rec, &b); }
        if !ok { let m = alloc::format!("ntfs: could not rebuild the index of {} ({} entries)", String::from_utf8_lossy(&self.node(d).name), n); self.dev.log(&m); }
        self.nm(d).dirty = false;
        ok
    }

    // ------------------------------------------------------------ file records

    fn write_file_rec(&mut self, f: usize, data: &[u8]) -> bool {
        let mut b = vec![0u8; RECSZ];
        let (rec, seq) = (self.node(f).rec, self.node(f).seq);
        rec_init(&mut b, rec, seq, 1);
        if !self.rec_common_sd(&mut b, f, true) { return false; }
        let ok = if self.node(f).resident { add_resident(&mut b, AT_DATA, &[], data, 0) }
                 else { let (r, s) = (self.node(f).runs.clone(), self.node(f).size); add_nonresident(&mut b, AT_DATA, &[], &r, s) };
        ok && self.write_rec(rec, &b)
    }
    /// Rename or move: rewrites the $FILE_NAME, keeping $DATA (or the index) as it is.
    fn rewrite_names(&mut self, n: usize) -> bool {
        if self.node(n).dir { self.nm(n).dirty = true; return true; }
        let mut old = vec![0u8; RECSZ];
        let (rec, seq) = (self.node(n).rec, self.node(n).seq);
        if !self.read_rec(rec, &mut old) { return false; }
        let Some(da) = find_attr(&old, AT_DATA, None) else { return false };
        let dlen = g32(&old, da + 4) as usize;
        let mut b = vec![0u8; RECSZ];
        rec_init(&mut b, rec, seq, 1);
        if !self.rec_common_sd(&mut b, n, true) { return false; }
        let Some(at) = rec_add(&mut b, AT_DATA, dlen) else { return false };
        let inst = g16(&b, at + 0x0E);
        b[at..at + dlen].copy_from_slice(&old[da..da + dlen]);
        p16(&mut b, at + 0x0E, inst);
        self.write_rec(rec, &b)
    }
    fn new_node(&mut self, parent: usize, name: &[u8], dir: bool) -> Option<usize> {
        let Some(r) = self.alloc_rec() else { self.dev.log("ntfs: the MFT is full"); return None };
        let seq = self.next_seq(r);
        let now = self.dev.now();
        let mut n = Node { rec: r, seq, dir, ctime: now, mtime: now, fattr: if dir { 0 } else { 0x20 }, ..Default::default() };
        Self::set_name(&mut n, name);
        let i = self.add_node(n);
        self.link_child(parent, i);
        self.nm(parent).dirty = true;
        if dir { self.nm(i).dirty = true; }
        Some(i)
    }

    // ------------------------------------------------------------ public

    pub fn exists(&self, path: &[u8]) -> bool { self.lookup(path, false).is_some() }

    /// mkdir -p.
    pub fn mkdir(&mut self, path: &[u8]) -> bool {
        let sep = |c: u8| c == b'\\' || c == b'/';
        let mut cur = self.root;
        let mut s = 0;
        while s < path.len() {
            while s < path.len() && sep(path[s]) { s += 1; }
            if s >= path.len() { break; }
            let mut e = s; while e < path.len() && !sep(path[e]) { e += 1; }
            let name = &path[s..s + (e - s).min(255)];
            let c = match self.child_named(cur, &path[s..e]) {
                Some(c) if !self.node(c).dir => return false,
                Some(c) => c,
                None => match self.new_node(cur, name, true) { Some(c) => c, None => return false },
            };
            cur = c; s = e;
        }
        true
    }

    fn del_rec(&mut self, n: usize) {
        let mut b = vec![0u8; RECSZ];
        let rec = self.node(n).rec;
        if self.read_rec(rec, &mut b) {
            let f = g16(&b, 0x16) & !3; p16(&mut b, 0x16, f);
            let s = g16(&b, 0x10).wrapping_add(1); p16(&mut b, 0x10, if s != 0 { s } else { 1 });
            self.write_rec(rec, &b);
        }
        if let Some(byte) = self.mftbm.get_mut((rec >> 3) as usize) { *byte &= !(1 << (rec & 7)); }
        self.mftbm_dirty = true;
        let r = self.node(n).runs.clone();
        self.free_runs(&r);
    }
    /// The nodes below `n` and `n` itself, children before their parents, in list order.
    fn post_order(&self, n: usize) -> Vec<usize> {
        let mut out = Vec::new();
        let mut seen = vec![false; self.nodes.len()];
        seen[n] = true;
        let mut stack = vec![(n, 0usize)];
        while let Some((x, i)) = stack.pop() {
            let kids = &self.node(x).kids;
            if i < kids.len() {
                stack.push((x, i + 1));
                let c = kids[i];
                if !seen[c] { seen[c] = true; stack.push((c, 0)); }
            } else { out.push(x); }
        }
        out
    }
    pub fn delete(&mut self, path: &[u8]) -> bool {
        let Some((n, _)) = self.lookup(path, false) else { return false };
        if n == self.root || self.node(n).sys { return false; }
        let order = self.post_order(n);
        for &x in &order { self.del_rec(x); }
        let p = self.node(n).parent;
        self.unlink_child(n);
        if let Some(p) = p { self.nm(p).dirty = true; }
        for &x in &order { self.nodes[x] = None; }
        true
    }

    pub fn write(&mut self, path: &[u8], data: &[u8], readonly: bool) -> bool {
        let path = &path[..path.iter().position(|&c| c == 0).unwrap_or(path.len())];
        let mut found = self.lookup(path, true);
        if found.is_none() {                                // create missing folders
            let dirpath = &path[..path.len().min(511)];
            if let Some(slash) = dirpath.iter().rposition(|&c| c == b'\\' || c == b'/') {
                if !self.mkdir(&dirpath[..slash]) { return false; }
            }
            found = self.lookup(path, true);
        }
        let Some((dir, leaf_at)) = found else { return false };
        let leaf = &path[leaf_at..];
        if !self.node(dir).dir || leaf.is_empty() { return false; }
        let f = match self.child_named(dir, leaf) {
            Some(f) if self.node(f).dir || self.node(f).sys => return false,
            Some(f) => f,
            None => match self.new_node(dir, leaf, false) { Some(f) => f, None => return false },
        };
        let old = core::mem::take(&mut self.nm(f).runs);
        self.free_runs(&old);
        let len = data.len();
        let now = self.dev.now();
        let nlen = self.node(f).n16.len();
        {
            let n = self.nm(f);
            n.mtime = now; n.size = len as u64;
            n.fattr = 0x20 | readonly as u32;
            n.resident = (len as i64 + 0x18) <= rec_room(nlen, true) as i64 && len <= 700;
        }
        let mut ok = true;
        if self.node(f).resident { self.nm(f).alloc = al8(len) as u64; }
        else {
            let nc = (len as u64).div_ceil(CLUSTER);
            let Some(r) = self.alloc_runs(nc, 48) else { let m = alloc::format!("ntfs: disk full writing {}", String::from_utf8_lossy(path)); self.dev.log(&m); return false };
            self.nm(f).runs = r.clone();
            self.nm(f).alloc = nc * CLUSTER;
            // whole clusters; the tail cluster padded with zeros
            let full = (len as u64 / CLUSTER * CLUSTER) as usize;
            let mut off = 0u64;
            for run in &r {
                if !ok { break; }
                let bytes = (run.len * CLUSTER).min(full as u64 - off.min(full as u64)) as usize;
                if bytes > 0 { ok = self.sec_write(run.lcn * SPC, (bytes / 512) as u32, &data[off as usize..off as usize + bytes]); }
                off += run.len * CLUSTER;
            }
            if ok && full < len {
                let mut t = vec![0u8; CLUSTER as usize];
                t[..len - full].copy_from_slice(&data[full..]);
                ok = match Self::run_sector(&r, full as u64) { Some(s) => self.sec_write(s, SPC as u32, &t), None => false };
            }
        }
        ok = ok && self.write_file_rec(f, data);
        self.nm(dir).dirty = true;                          // sizes in the index changed
        if !ok { let m = alloc::format!("ntfs: could not write {}", String::from_utf8_lossy(path)); self.dev.log(&m); }
        ok
    }

    pub fn rename(&mut self, from: &[u8], to: &[u8]) -> bool {
        let Some((n, _)) = self.lookup(from, false) else { return false };
        if n == self.root || self.node(n).sys || self.lookup(to, false).is_some() { return false; }
        let Some((nd, leaf_at)) = self.lookup(to, true) else { return false };
        let leaf = to[leaf_at..].to_vec();
        if !self.node(nd).dir || leaf.is_empty() { return false; }
        let mut a = Some(nd);
        while let Some(x) = a { if x == n { return false; } a = self.node(x).parent; }
        let op = self.node(n).parent.unwrap_or(self.root);
        self.unlink_child(n); self.nm(op).dirty = true;
        Self::set_name(self.nm(n), &leaf);
        self.link_child(nd, n); self.nm(nd).dirty = true;
        self.rewrite_names(n)
    }

    /// A file's contents (sizes checked against what holds them; gaps read as zeros).
    fn read_node_data(&mut self, n: usize) -> Option<Vec<u8>> {
        if self.node(n).dir { return None; }
        let sz = self.node(n).size;
        if sz > u32::MAX as u64 - 1 { return None; }
        let sz = sz as usize;
        if self.node(n).resident {
            let mut b = vec![0u8; RECSZ];
            if !self.read_rec(self.node(n).rec, &mut b) { return None; }
            let a = find_attr(&b, AT_DATA, None)?;
            let v = resident_value(&b, a)?;
            let mut buf = vec![0u8; sz.min(RECSZ)];
            let k = v.len().min(buf.len());
            buf[..k].copy_from_slice(&v[..k]);
            buf.resize(sz, 0);
            return Some(buf);
        }
        let runs = self.node(n).runs.clone();
        let covered: u64 = runs.iter().fold(0u64, |a, r| a.saturating_add(r.len.saturating_mul(CLUSTER)));
        if (sz as u64) > covered { return None; }
        let mut buf = Vec::new();
        buf.try_reserve_exact(sz).ok()?;
        buf.resize(sz, 0);
        let mut off = 0usize;
        for run in &runs {
            if off >= sz { break; }
            let bytes = (run.len.saturating_mul(CLUSTER)).min((sz - off) as u64) as usize;
            if run.lcn != SPARSE {
                let secs = bytes.div_ceil(512) as u32;
                let s = run.lcn.checked_mul(SPC)?;
                if bytes % 512 != 0 {
                    let mut t = vec![0u8; secs as usize * 512];
                    if !self.sec_read(s, secs, &mut t) { return None; }
                    buf[off..off + bytes].copy_from_slice(&t[..bytes]);
                } else if !self.sec_read(s, secs, &mut buf[off..off + bytes]) { return None; }
            }
            off += bytes;
        }
        Some(buf)
    }
    pub fn read(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        let (n, _) = self.lookup(path, false)?;
        self.read_node_data(n)
    }

    /// Hands every folder and file (not the system files) to `t`, in the C driver's order;
    /// returns the number of files.
    pub fn load_tree<T: Tree>(&mut self, t: &mut T, root: T::Node) -> u32 {
        let total = self.nclus.wrapping_sub(self.free_clusters).wrapping_mul(CLUSTER);
        let mut done = 0u64;
        let mut files = 0u32;
        let mut stack: Vec<(usize, usize, T::Node)> = vec![(self.root, 0, root)];
        let mut seen = vec![false; self.nodes.len()];
        while let Some(top) = stack.last_mut() {
            let (d, i, vn) = *top;
            let kids = &self.node(d).kids;
            if i >= kids.len() { stack.pop(); continue; }
            top.1 += 1;
            let c = kids[i];
            if self.node(c).sys || seen[c] { continue; }
            seen[c] = true;
            let name = self.node(c).name.clone();
            if self.node(c).dir {
                let n = t.dir(vn, &name);
                stack.push((c, 0, n));
                continue;
            }
            let data = self.read_node_data(c);
            if data.is_none() { let m = alloc::format!("ntfs: read failed for {}", String::from_utf8_lossy(&name)); self.dev.log(&m); }
            t.file(vn, &name, self.node(c).fattr & 1 != 0, data);
            files += 1;
            done = done.wrapping_add(self.node(c).alloc);
            t.progress(done, total);
        }
        t.progress(total, total);
        files
    }

    /// Rebuilds changed folder indexes (deepest first) and writes both bitmaps.
    pub fn sync(&mut self) {
        for d in self.post_order(self.root) {
            if self.node(d).dir && self.node(d).dirty { self.rebuild_dir(d); }
        }
        if self.mftbm_dirty {
            let n = (self.mftbm_bytes as usize + 511) & !511;
            let (r, data) = (self.mftbm_runs.clone(), self.mftbm[..n.min(self.mftbm.len())].to_vec());
            self.runs_write(&r, 0, &data);
            self.mftbm_dirty = false;
        }
        if self.bm_dirty {
            let n = (self.bm_bytes as usize + 511) & !511;
            let (r, data) = (self.bm_runs.clone(), self.bm[..n.min(self.bm.len())].to_vec());
            self.runs_write(&r, 0, &data);
            self.bm_dirty = false;
        }
    }

    // ------------------------------------------------------------ mount

    pub fn mount(dev: D, lba: u32, sectors: u32) -> Result<Self, D> {
        let mut v = Vol { dev, lba, total_sectors: 0, nclus: 0, mft_runs: Vec::new(), nrec: 16, mftbm_runs: Vec::new(), mftbm: Vec::new(), mftbm_bytes: 0,
                          mftbm_dirty: false, bm_runs: Vec::new(), bm: Vec::new(), bm_bytes: 0, bm_dirty: false, free_clusters: 0, hint: 0, serial64: 0,
                          label: [0; 33], nodes: Vec::new(), root: 0, usn: 0 };
        let mut bs = [0u8; 512];
        if !v.dev.read(lba, 1, &mut bs) || &bs[3..11] != b"NTFS    " || bs[510] != 0x55 || bs[511] != 0xAA { return Err(v.dev); }
        if g16(&bs, 0x0B) != 512 || bs[0x0D] as u64 != SPC || bs[0x40] as i8 != -10 || bs[0x44] != 1 {
            let m = alloc::format!("ntfs: unsupported geometry ({} bytes/sector, {} sectors/cluster)", g16(&bs, 0x0B), bs[0x0D]);
            v.dev.log(&m);
            return Err(v.dev);
        }
        match v.mount_rest(&bs, sectors) {
            Some(()) => Ok(v),
            None => { let m = alloc::format!("ntfs: the volume at sector {lba} could not be mounted"); v.dev.log(&m); Err(v.dev) }
        }
    }
    fn mount_rest(&mut self, bs: &[u8], sectors: u32) -> Option<()> {
        self.total_sectors = g64(bs, 0x28);
        if sectors != 0 && self.total_sectors > sectors as u64 { self.total_sectors = sectors as u64; }
        self.nclus = self.total_sectors / SPC;
        self.serial64 = g64(bs, 0x48);
        // bootstrap the MFT run list from record 0
        self.mft_runs = vec![Run { lcn: g64(bs, 0x30), len: 4 }];
        self.nrec = 16;
        let mut b = vec![0u8; RECSZ];
        if !self.read_rec(0, &mut b) { return None; }
        let a = find_attr(&b, AT_DATA, None)?;
        if b[a + 8] == 0 { return None; }
        self.mft_runs = decode_runs(&b, a, 16);
        let mft_clus: u64 = self.mft_runs.iter().fold(0u64, |s, r| s.saturating_add(r.len));
        let nrec = (g64(&b, a + 0x30) / RECSZ as u64).min(mft_clus.saturating_mul(CLUSTER / RECSZ as u64)).min(MAX_RECS);
        let ab = find_attr(&b, AT_BITMAP, None)?;
        if b[ab + 8] == 0 { return None; }
        self.mftbm_runs = decode_runs(&b, ab, 4);
        let mbytes = g64(&b, ab + 0x30);
        if mbytes > MAX_MFTBM { return None; }
        self.mftbm_bytes = mbytes as u32;
        self.nrec = nrec.min(mbytes * 8) as u32;
        self.mftbm = vec![0u8; (self.mftbm_bytes as usize + 1023) & !511];
        let (r, n) = (self.mftbm_runs.clone(), (self.mftbm_bytes as usize + 511) & !511);
        let mut t = vec![0u8; n];
        if !self.runs_read(&r, 0, &mut t) { return None; }
        self.mftbm[..n].copy_from_slice(&t);
        // the cluster bitmap
        if !self.read_rec(6, &mut b) { return None; }
        let a = find_attr(&b, AT_DATA, None)?;
        if b[a + 8] == 0 { return None; }
        self.bm_runs = decode_runs(&b, a, 8);
        let bbytes = g64(&b, a + 0x30);
        if bbytes > MAX_BM || bbytes * 8 < self.nclus { return None; }      // the bitmap covers the volume, and fits in memory
        self.bm_bytes = bbytes as u32;
        let need = (self.nclus as usize).div_ceil(8);
        self.bm = vec![0u8; ((self.bm_bytes as usize + 1023) & !511).max(need)];
        let (r, n) = (self.bm_runs.clone(), (self.bm_bytes as usize + 511) & !511);
        let mut t = vec![0u8; n];
        if !self.runs_read(&r, 0, &mut t) { return None; }
        self.bm[..n].copy_from_slice(&t);
        let whole = (self.nclus / 8) as usize;
        self.free_clusters = self.bm[..whole].iter().map(|b| b.count_zeros() as u64).sum::<u64>()
            + (whole as u64 * 8..self.nclus).filter(|&c| !self.bm_get(c)).count() as u64;
        // the volume label
        if self.read_rec(3, &mut b) {
            if let Some(a) = find_attr(&b, AT_VOLNAME, None) {
                if let Some(nm) = resident_value(&b, a) {
                    for (k, c) in nm.chunks_exact(2).take(32).enumerate() { let w = u16::from_le_bytes([c[0], c[1]]); self.label[k] = if w < 256 { w as u8 } else { b'?' }; }
                }
            }
        }
        // every record in use: a node
        let mut by: Vec<Option<usize>> = vec![None; self.nrec as usize];
        let mut parent = vec![0u64; self.nrec as usize];
        for r in 0..self.nrec {
            if self.mftbm[(r >> 3) as usize] & (1 << (r & 7)) == 0 { continue; }
            if !self.read_rec(r, &mut b) { continue; }
            let fl = g16(&b, 0x16);
            if fl & 1 == 0 || g64(&b, 0x20) != 0 { continue; }        // free, or an extension record
            let mut fnv: Option<Vec<u8>> = None;
            let mut off = g16(&b, 0x14) as usize;
            while off + 0x18 <= RECSZ {                               // the best $FILE_NAME: not DOS-only
                let (t, len) = (g32(&b, off), g32(&b, off + 4) as usize);
                if t == AT_END || len < 0x18 { break; }
                if t == AT_FN {
                    if let Some(val) = resident_value(&b, off) {
                        if val.len() >= 0x42 && val[0x41] != 2 && (fnv.is_none() || val[0x41] == 1) { fnv = Some(val.to_vec()); }
                    }
                }
                off += len;
            }
            if fnv.is_none() && r != 5 { continue; }
            let mut n = Node { rec: r, seq: g16(&b, 0x10), dir: fl & 2 != 0, sys: r < 16, ..Default::default() };
            if let Some(f) = &fnv {
                parent[r as usize] = g64(f, 0) & 0xFFFF_FFFF_FFFF;
                let nl = (f[0x40] as usize).min((f.len() - 0x42) / 2);
                n.n16 = (0..nl).map(|i| g16(f, 0x42 + 2 * i)).collect();
                n.name = n.n16.iter().map(|&c| if c < 256 { c as u8 } else { b'?' }).collect();
                if let Some(z) = n.name.iter().position(|&c| c == 0) { n.name.truncate(z); }
            }
            if let Some(a) = find_attr(&b, AT_SI, None) {
                if let Some(si) = resident_value(&b, a) { if si.len() >= 36 { n.ctime = g64(si, 0); n.mtime = g64(si, 8); n.fattr = g32(si, 32); } }
            }
            if !n.dir {
                if let Some(a) = find_attr(&b, AT_DATA, None) {
                    if b[a + 8] != 0 { n.runs = decode_runs(&b, a, 64); n.size = g64(&b, a + 0x30); n.alloc = g64(&b, a + 0x28); }
                    else { let vl = g32(&b, a + 0x10) as u64; n.size = vl; n.alloc = (vl + 7) & !7; n.resident = true; }
                }
            }
            if n.dir {
                if let Some(a) = find_attr(&b, AT_IALLOC, Some(b"$I30")) { if b[a + 8] != 0 { n.runs = decode_runs(&b, a, 16); } }
            }
            by[r as usize] = Some(self.add_node(n));
        }
        self.root = by.get(5).copied().flatten()?;
        Self::set_name(self.nm(self.root), b".");
        for r in 0..self.nrec as usize {
            let Some(n) = by[r] else { continue };
            if r == 5 { continue; }
            let p = parent[r];
            match by.get(p as usize).copied().flatten() {
                Some(pn) if p < self.nrec as u64 && self.node(pn).dir => {
                    self.link_child(pn, n);
                    if self.node(pn).sys && p != 5 { self.nm(n).sys = true; }
                }
                _ => { let m = alloc::format!("ntfs: record {r} has no parent directory"); self.dev.log(&m); self.nodes[n] = None; }
            }
        }
        let l = self.label.iter().position(|&c| c == 0).unwrap_or(32);
        let m = alloc::format!("ntfs: mounted \"{}\" serial {:08x}, {} records, {} MB free of {} MB", String::from_utf8_lossy(&self.label[..l]),
                               self.serial64 as u32, self.nrec, (self.free_clusters.wrapping_mul(CLUSTER) >> 20) as u32, (self.nclus.wrapping_mul(CLUSTER) >> 20) as u32);
        self.dev.log(&m);
        Some(())
    }
}

// ---------------------------------------------------------------- format

const ATTRDEFS: [(&str, u32, u32, u32, u32, u64, u64); 15] = [
    ("$STANDARD_INFORMATION", 0x10, 0, 0, 0x40, 0x30, 0x48), ("$ATTRIBUTE_LIST", 0x20, 0, 0, 0x80, 0, !0),
    ("$FILE_NAME", 0x30, 0, 0, 0x42, 0x44, 0x242), ("$OBJECT_ID", 0x40, 0, 0, 0x40, 0, 0x100),
    ("$SECURITY_DESCRIPTOR", 0x50, 0, 0, 0x80, 0, !0), ("$VOLUME_NAME", 0x60, 0, 0, 0x40, 2, 0x100),
    ("$VOLUME_INFORMATION", 0x70, 0, 0, 0x40, 0xC, 0xC), ("$DATA", 0x80, 0, 0, 0, 0, !0),
    ("$INDEX_ROOT", 0x90, 0, 0, 0x40, 0, !0), ("$INDEX_ALLOCATION", 0xA0, 0, 0, 0x80, 0, !0),
    ("$BITMAP", 0xB0, 0, 0, 0x80, 0, !0), ("$REPARSE_POINT", 0xC0, 0, 0, 0x80, 0, 0x4000),
    ("$EA_INFORMATION", 0xD0, 0, 0, 0x40, 8, 8), ("$EA", 0xE0, 0, 0, 0, 0, 0x10000),
    ("$LOGGED_UTILITY_STREAM", 0x100, 0, 0, 0x80, 0, 0x10000),
];

/// Formats an NTFS volume of `sectors` at `lba`; `full` also zeroes every cluster.
/// `cb(percent, what)` reports progress. Returns the number of clusters and MFT records.
pub fn format<D: Dev>(dev: D, lba: u32, sectors: u32, label: &[u8], full: bool, serial: u32, mut cb: impl FnMut(i32, &str)) -> (D, Result<(u64, u32), String>) {
    if sectors < 64 * 2048 { return (dev, Err("The partition is too small for NTFS (needs 64 MB).".into())); }
    let mut v = Vol { dev, lba, total_sectors: sectors as u64 - 1, nclus: 0, mft_runs: Vec::new(), nrec: 0, mftbm_runs: Vec::new(), mftbm: Vec::new(),
                      mftbm_bytes: 0, mftbm_dirty: false, bm_runs: Vec::new(), bm: Vec::new(), bm_bytes: 0, bm_dirty: false, free_clusters: 0, hint: 0,
                      serial64: 0, label: [0; 33], nodes: Vec::new(), root: 0, usn: 0 };
    let r = format_into(&mut v, sectors, label, full, serial, &mut cb);
    (v.dev, r)
}
fn format_into<D: Dev>(v: &mut Vol<D>, sectors: u32, label: &[u8], full: bool, serial: u32, cb: &mut impl FnMut(i32, &str)) -> Result<(u64, u32), String> {
    use alloc::format;
    v.nclus = v.total_sectors / SPC;
    v.serial64 = (v.dev.rand() as u64) << 32 | serial as u64;
    v.bm_bytes = (v.nclus.div_ceil(64) * 8) as u32;
    v.bm = vec![0u8; (v.bm_bytes as usize + 1023) & !511];
    v.free_clusters = v.nclus;
    for c in v.nclus..v.bm_bytes as u64 * 8 { v.bm[(c >> 3) as usize] |= 1 << (c & 7); }   // past the end: in use
    cb(2, "Creating the file system");
    let zero_clusters = |v: &mut Vol<D>, lcn: u64, n: u64, fill: u8| -> bool {
        let z = vec![fill; 64 * 1024];
        let mut c = 0;
        while c < n { if !v.sec_write((lcn + c) * SPC, (16.min(n - c) * SPC) as u32, &z) { return false; } c += 16; }
        true
    };
    let write_clusters = |v: &mut Vol<D>, lcn: u64, data: &[u8]| -> bool {
        let secs = data.len().div_ceil(512);
        let mut t = vec![0u8; data.len().div_ceil(CLUSTER as usize) * CLUSTER as usize];
        t[..data.len()].copy_from_slice(data);
        v.sec_write(lcn * SPC, secs as u32, &t)
    };
    if full {
        let mut c = 0;
        while c < v.nclus {
            if !zero_clusters(v, c, 256.min(v.nclus - c), 0) { return Err(format!("Setup could not write to the disk (cluster {}).", c as u32)); }
            if c % 4096 == 0 { cb(2 + (c * 80 / v.nclus) as i32, "Formatting"); }
            c += 256;
        }
    }
    // layout
    v.nrec = if v.nclus >= 131_072 { 16384 } else { 4096 };
    let mft_clus = v.nrec as u64 * RECSZ as u64 / CLUSTER;
    let mft_lcn = 16u64;
    let mftbm_bytes = (v.nrec / 8 + 7) & !7;
    let mftbm_lcn = mft_lcn + mft_clus;
    let log_clus = if v.nclus >= 262_144 { 4096 } else { 1024 };
    let log_lcn = mftbm_lcn + 1;
    let attrdef_lcn = log_lcn + log_clus;
    let bm_clus = (v.bm_bytes as u64).div_ceil(CLUSTER);
    let bm_lcn = attrdef_lcn + 1;
    let upcase_lcn = bm_lcn + bm_clus;
    let mirr_lcn = v.nclus / 2;
    if upcase_lcn + 32 >= mirr_lcn { return Err("The partition is too small for NTFS.".into()); }
    for (l, n) in [(0, 2), (mft_lcn, mft_clus), (mftbm_lcn, 1), (log_lcn, log_clus), (attrdef_lcn, 1), (bm_lcn, bm_clus), (upcase_lcn, 32), (mirr_lcn, 1)] {
        for c in 0..n { v.bm_set(l + c, true); }
    }
    v.hint = upcase_lcn + 32;
    v.mft_runs = vec![Run { lcn: mft_lcn, len: mft_clus }];
    v.mftbm_runs = vec![Run { lcn: mftbm_lcn, len: 1 }];
    v.bm_runs = vec![Run { lcn: bm_lcn, len: bm_clus }];
    v.mftbm_bytes = mftbm_bytes;
    v.mftbm = vec![0u8; CLUSTER as usize];
    for r in 0..16 { v.mftbm[r >> 3] |= 1 << (r & 7); }
    // empty records for the whole MFT
    cb(84, "Creating the master file table");
    let mut chunk = vec![0u8; 64 * 1024];
    let mut r = 0;
    while r < v.nrec {
        for k in 0..64 {
            let b = &mut chunk[k * RECSZ..(k + 1) * RECSZ];
            rec_init(b, r + k as u32, 1, 0); p16(b, 0x10, 0);
            let mut t = b.to_vec(); v.fixup_apply(&mut t, RECSZ); b.copy_from_slice(&t);
        }
        let runs = v.mft_runs.clone();
        if !v.runs_write(&runs, r as u64 * RECSZ as u64, &chunk) { return Err("Setup could not write the master file table.".into()); }
        r += 64;
    }
    // system file contents
    if !zero_clusters(v, log_lcn, log_clus, 0xFF) { return Err("Setup could not write $LogFile.".into()); }
    let mut ad = vec![0u8; CLUSTER as usize];
    for (i, d) in ATTRDEFS.iter().enumerate() {
        let e = i * 160;
        for (k, c) in d.0.bytes().enumerate() { p16(&mut ad, e + 2 * k, c as u16); }
        p32(&mut ad, e + 0x80, d.1); p32(&mut ad, e + 0x84, d.2);
        p32(&mut ad, e + 0x88, if d.1 == AT_FN { 1 } else { d.3 });
        p32(&mut ad, e + 0x8C, d.4); p64(&mut ad, e + 0x90, d.5); p64(&mut ad, e + 0x98, d.6);
    }
    let ad_len = (ATTRDEFS.len() * 160 + 160) as u64;
    write_clusters(v, attrdef_lcn, &ad);
    let up: Vec<u8> = (0..65536u32).flat_map(|c| upcase16(c as u16).to_le_bytes()).collect();
    write_clusters(v, upcase_lcn, &up);
    // system file records
    const NAMES: [&str; 12] = ["$MFT", "$MFTMirr", "$LogFile", "$Volume", "$AttrDef", ".", "$Bitmap", "$Boot", "$BadClus", "$Secure", "$UpCase", "$Extend"];
    let now = v.dev.now();
    let mut root = Node { rec: 5, seq: 5, dir: true, sys: true, fattr: 0x06, ctime: now, mtime: now, ..Default::default() };
    Vol::<D>::set_name(&mut root, b".");
    let root = v.add_node(root);
    v.root = root;
    let mut sys = [0usize; 12];
    for r in 0..12 {
        if r == 5 { sys[r] = root; continue; }
        let mut n = Node { rec: r as u32, seq: if r != 0 { r as u16 } else { 1 }, sys: true, fattr: 0x06, ctime: now, mtime: now, dir: r == 11, ..Default::default() };
        Vol::<D>::set_name(&mut n, NAMES[r].as_bytes());
        let i = v.add_node(n);
        v.link_child(root, i);
        sys[r] = i;
    }
    let nr: [(usize, Run, u64); 7] = [
        (0, Run { lcn: mft_lcn, len: mft_clus }, mft_clus * CLUSTER), (1, Run { lcn: mirr_lcn, len: 1 }, 4 * RECSZ as u64),
        (2, Run { lcn: log_lcn, len: log_clus }, log_clus * CLUSTER), (4, Run { lcn: attrdef_lcn, len: 1 }, ad_len),
        (6, Run { lcn: bm_lcn, len: bm_clus }, v.bm_bytes as u64), (7, Run { lcn: 0, len: 2 }, 8192), (10, Run { lcn: upcase_lcn, len: 32 }, 131_072)];
    for (r, run, size) in nr { let n = v.nm(sys[r]); n.size = size; n.alloc = run.len * CLUSTER; }
    let mut ok = true;
    for r in 0..12 {
        if !ok { break; }
        if r == 5 || r == 11 { continue; }                       // directories: written by rebuild_dir
        let n = sys[r];
        let mut b = vec![0u8; RECSZ];
        rec_init(&mut b, r as u32, v.node(n).seq, 1);
        ok = v.rec_common_sd(&mut b, n, true);
        for (rr, run, size) in nr {
            if !ok { break; }
            if rr != r { continue; }
            ok = add_nonresident(&mut b, AT_DATA, &[], &[run], size);
            if r == 0 { ok = ok && add_nonresident(&mut b, AT_BITMAP, &[], &[Run { lcn: mftbm_lcn, len: 1 }], mftbm_bytes as u64); }
        }
        if r == 3 {                                              // $Volume
            let ln: Vec<u8> = label.iter().take_while(|&&c| c != 0).take(32).flat_map(|&c| (c as u16).to_le_bytes()).collect();
            let mut vi = [0u8; 12]; vi[8] = 3; vi[9] = 1;           // NTFS 3.1
            ok = ok && add_resident(&mut b, AT_VOLNAME, &[], &ln, 0) && add_resident(&mut b, AT_VOLINFO, &[], &vi, 0) && add_resident(&mut b, AT_DATA, &[], &[], 0);
        }
        if r == 8 {                                              // $BadClus: empty $DATA and a sparse $Bad
            let bad = [b'$' as u16, b'B' as u16, b'a' as u16, b'd' as u16];
            ok = ok && add_resident(&mut b, AT_DATA, &[], &[], 0) && add_nonresident(&mut b, AT_DATA, &bad, &[Run { lcn: SPARSE, len: v.nclus }], v.nclus * CLUSTER);
            if ok { if let Some(a) = find_attr(&b, AT_DATA, Some(b"$Bad")) { p64(&mut b, a + 0x38, 0); } }
        }
        if r == 9 {                                              // $Secure: empty $SDS, $SDH and $SII indexes
            let n4 = |s: &[u8; 4]| [s[0] as u16, s[1] as u16, s[2] as u16, s[3] as u16];
            let mut ir = [0u8; 0x30];
            p32(&mut ir, 8, IDXSZ as u32); ir[12] = 1;
            p32(&mut ir, 0x10, 0x10); p32(&mut ir, 0x14, 0x20); p32(&mut ir, 0x18, 0x20);
            p16(&mut ir, 0x20 + 8, 0x10); p16(&mut ir, 0x20 + 12, 2);
            ok = ok && add_resident(&mut b, AT_DATA, &n4(b"$SDS"), &[], 0);
            p32(&mut ir, 4, 0x12); ok = ok && add_resident(&mut b, AT_IROOT, &n4(b"$SDH"), &ir, 0);
            p32(&mut ir, 4, 0x10); ok = ok && add_resident(&mut b, AT_IROOT, &n4(b"$SII"), &ir, 0);
            p16(&mut b, 0x16, 1 | 4);
        }
        ok = ok && v.write_rec(r as u32, &b);
    }
    for r in 12..16u32 { if !ok { break; } let mut b = vec![0u8; RECSZ]; rec_init(&mut b, r, r as u16, 0); ok = v.write_rec(r, &b); }
    v.nm(sys[11]).dirty = true; v.nm(root).dirty = true;
    if ok { ok = v.rebuild_dir(sys[11]) && v.rebuild_dir(root); }
    if !ok { return Err("Setup could not create the NTFS system files.".into()); }
    // $MFTMirr: the first four records
    let mut mirr = vec![0u8; 4 * RECSZ];
    let runs = v.mft_runs.clone();
    ok = v.runs_read(&runs, 0, &mut mirr) && write_clusters(v, mirr_lcn, &mirr);
    // the boot sector (and its backup in the partition's last sector)
    let mut bs = [0u8; 512];
    bs[0] = 0xEB; bs[1] = 0x52; bs[2] = 0x90; bs[3..11].copy_from_slice(b"NTFS    ");
    p16(&mut bs, 0x0B, 512); bs[0x0D] = SPC as u8; bs[0x15] = 0xF8;
    p16(&mut bs, 0x18, 63); p16(&mut bs, 0x1A, 255); p32(&mut bs, 0x1C, v.lba);
    bs[0x24] = 0x80; bs[0x26] = 0x80;
    p64(&mut bs, 0x28, v.total_sectors); p64(&mut bs, 0x30, mft_lcn); p64(&mut bs, 0x38, mirr_lcn);
    bs[0x40] = -10i8 as u8; bs[0x44] = 1; p64(&mut bs, 0x48, v.serial64);
    let msg = b"This is an NTFS volume of THE WRLD OS; it is started by GRUB.\0";
    bs[0x54..0x54 + msg.len()].copy_from_slice(msg);
    bs[510] = 0x55; bs[511] = 0xAA;
    let mut boot = vec![0u8; 8192]; boot[..512].copy_from_slice(&bs);
    ok = ok && write_clusters(v, 0, &boot) && { let l = v.lba + sectors - 1; v.dev.write(l, 1, &bs) };
    v.mftbm_dirty = true; v.bm_dirty = true;
    v.sync();
    if !ok { return Err("Setup could not write the boot sector.".into()); }
    cb(100, "Done");
    Ok((v.nclus, v.nrec))
}

#[cfg(test)]
mod tests {
    use super::*;

    pub struct Mem { pub d: Vec<u8>, pub seed: u32 }
    impl Dev for Mem {
        fn read(&mut self, lba: u32, n: u32, buf: &mut [u8]) -> bool {
            let (a, b) = (lba as usize * 512, (lba as usize + n as usize) * 512);
            if b > self.d.len() { return false; } buf[..b - a].copy_from_slice(&self.d[a..b]); true
        }
        fn write(&mut self, lba: u32, n: u32, buf: &[u8]) -> bool {
            let (a, b) = (lba as usize * 512, (lba as usize + n as usize) * 512);
            if b > self.d.len() { return false; } self.d[a..b].copy_from_slice(&buf[..b - a]); true
        }
        fn now(&self) -> u64 { ntfs_time(2026, 11, 20, 12, 34, 56) }
        fn rand(&mut self) -> u32 { self.seed = self.seed.wrapping_mul(1_103_515_245).wrapping_add(12345); self.seed }
    }
    fn fresh(mb: usize) -> Vol<Mem> {
        let (m, r) = format(Mem { d: vec![0; mb << 20], seed: 1 }, 0, (mb << 11) as u32, b"Test", false, 0x1234_5678, |_, _| {});
        r.unwrap();
        Vol::mount(m, 0, 0).ok().unwrap()
    }
    struct Names(Vec<(usize, Vec<u8>, Option<usize>)>);
    impl Tree for Names {
        type Node = usize;
        fn dir(&mut self, p: usize, name: &[u8]) -> usize { self.0.push((p, name.to_vec(), None)); self.0.len() }
        fn file(&mut self, p: usize, name: &[u8], _: bool, d: Option<Vec<u8>>) { self.0.push((p, name.to_vec(), Some(d.map(|d| d.len()).unwrap_or(usize::MAX)))); }
    }

    #[test]
    fn time() { assert_eq!(ntfs_time(1601, 1, 1, 0, 0, 0), 0); assert_eq!(ntfs_time(1970, 1, 1, 0, 0, 1), 116_444_736_010_000_000); }

    #[test]
    fn format_write_read_reload() {
        let mut v = fresh(80);
        assert_eq!(&v.label()[..5], b"Test\0");
        let big: Vec<u8> = (0..100_000u32).map(|i| i as u8).collect();
        assert!(v.write(b"WRLD\\small.txt", b"hello", false));
        assert!(v.write(b"WRLD\\System32\\big.bin", &big, true));
        for i in 0..400 { assert!(v.write(alloc::format!("many\\file number {i}.txt").as_bytes(), &[i as u8; 3], false)); }
        assert_eq!(v.read(b"wrld\\SMALL.TXT").unwrap(), b"hello");
        assert_eq!(v.read(b"WRLD\\system32\\big.bin").unwrap(), big);
        assert!(v.rename(b"WRLD\\small.txt", b"WRLD\\System32\\moved.txt"));
        assert!(!v.rename(b"WRLD", b"WRLD\\System32\\inside"));
        assert!(v.delete(b"many\\file number 7.txt"));
        assert!(!v.delete(b"$MFT"));
        v.sync();
        let free = v.free_bytes();
        let mut v2 = Vol::mount(v.dev, 0, 0).ok().unwrap();
        assert_eq!(v2.free_bytes(), free);
        assert_eq!(v2.read(b"WRLD\\System32\\moved.txt").unwrap(), b"hello");
        assert_eq!(v2.read(b"many\\FILE NUMBER 399.TXT").unwrap(), [399u32 as u8; 3]);
        assert!(v2.read(b"many\\file number 7.txt").is_none());
        let mut t = Names(Vec::new());
        assert_eq!(v2.load_tree(&mut t, 0), 401);
        assert!(v2.delete(b"many"));
        v2.sync();
        let mut v3 = Vol::mount(v2.dev, 0, 0).ok().unwrap();
        assert!(!v3.exists(b"many"));
        assert!(v3.exists(b"WRLD\\System32\\big.bin"));
        let mut t = Names(Vec::new());
        assert_eq!(v3.load_tree(&mut t, 0), 2);
    }

    #[test]
    fn damaged_volumes() {
        let mut v = fresh(80);
        for i in 0..40 { v.write(alloc::format!("d{}\\f{i}", i % 4).as_bytes(), &[1; 5000], false); }
        v.sync();
        let good = v.dev.d.clone();
        let mut seed = 7u32;
        let mut rnd = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for _ in 0..150 {
            let mut img = good.clone();
            for _ in 0..(1 + rnd() % 30) {
                let o = match rnd() % 3 { 0 => rnd() as usize % 512, 1 => 16 * 4096 + rnd() as usize % (200 * 1024), _ => rnd() as usize % img.len() };
                img[o] = rnd() as u8;
            }
            if let Ok(mut v) = Vol::mount(Mem { d: img, seed: 1 }, 0, 0) {
                let mut t = Names(Vec::new());
                v.load_tree(&mut t, 0);
                v.read(b"d1\\f5");
                v.write(b"d2\\new", b"data", false);
                v.rename(b"d1", b"d9\\x");
                v.delete(b"d3");
                v.sync();
            }
        }
    }
}
