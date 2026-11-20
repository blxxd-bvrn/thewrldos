//! The read-write FAT32 driver for installed systems (was `src/kernel/fat32.c`): format,
//! mount, long file names, whole-file create and replace, recursive delete, rename and move,
//! and loading a volume's whole tree. The FAT is kept in memory and written back by `sync`.
//!
//! It works over a [`Dev`] (512-byte sectors and the time for new entries), so it runs on
//! the build machine against an in-memory disk. On valid volumes it writes exactly what the C
//! driver wrote (the same clusters, short names and entries), which is how it was tested.
//! A damaged volume cannot make it read or write outside its buffers, loop or recurse
//! without end: boot sectors that do not add up are refused, cluster numbers past the volume
//! end a chain, file sizes are checked against their chains before anything is allocated,
//! and deletions stop at cycles and at 64 levels.
//!
//! Changes from the C driver: ".." is refused as the name of a new file or folder (the C
//! driver created entries named ".." that no listing shows and no deletion frees); a rename
//! whose target path runs through a file fails before anything changes (the C driver had
//! already removed the source, losing it and its clusters); and a volume formatted without a
//! label gets "NO NAME" instead of a blank label entry.

use alloc::vec;
use alloc::vec::Vec;

/// The disk under a volume, in 512-byte sectors.
pub trait Dev {
    fn read(&mut self, lba: u32, count: u32, buf: &mut [u8]) -> bool;
    fn write(&mut self, lba: u32, count: u32, buf: &[u8]) -> bool;
    fn zero(&mut self, lba: u32, count: u32) -> bool;
    /// The current time as a DOS (date, time) pair, for new entries.
    fn now(&self) -> (u16, u16);
}

/// DOS date and time from a calendar time (years before 1980 count as 1980).
pub fn dos_time(year: u32, month: u32, day: u32, hour: u32, minute: u32, second: u32) -> (u16, u16) {
    ((((year.max(1980) - 1980) << 9) | (month << 5) | day) as u16, ((hour << 11) | (minute << 5) | (second / 2)) as u16)
}

const A_RO: u8 = 0x01;
const A_LABEL: u8 = 0x08;
const A_DIR: u8 = 0x10;
const A_ARCH: u8 = 0x20;
const A_LFN: u8 = 0x0F;
const EOC: u32 = 0x0FFF_FFFF;
const LFN_OFFS: [usize; 13] = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
/// Deepest directory a deletion descends into (paths have at most 32 parts).
const MAX_DEPTH: usize = 64;
/// Deepest directory loaded into the file tree.
const LOAD_DEPTH: usize = 24;
/// Largest directory read (in clusters) and grown (in bytes).
const DIR_MAX_CLUS: u32 = 4096;
const DIR_MAX_BYTES: usize = 2 * 1024 * 1024;

fn le16(b: &[u8], o: usize) -> u16 { u16::from_le_bytes([b[o], b[o + 1]]) }
fn le32(b: &[u8], o: usize) -> u32 { u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) }
fn put16(b: &mut [u8], o: usize, v: u16) { b[o..o + 2].copy_from_slice(&v.to_le_bytes()); }
fn put32(b: &mut [u8], o: usize, v: u32) { b[o..o + 4].copy_from_slice(&v.to_le_bytes()); }
fn lower(c: u8) -> u8 { c.to_ascii_lowercase() }
fn eq_nocase(a: &[u8], b: &[u8]) -> bool { a.len() == b.len() && a.iter().zip(b).all(|(&x, &y)| lower(x) == lower(y)) }

/// One directory entry as seen through `entries`: its index, the index of the first entry of
/// its long name, the name and the fields used.
#[derive(Clone, Debug)]
pub struct Entry { pub idx: usize, pub first: usize, pub name: Vec<u8>, pub attr: u8, pub clus: u32, pub size: u32, pub mdate: u16, pub mtime: u16 }
impl Entry {
    pub fn is_dir(&self) -> bool { self.attr & A_DIR != 0 }
    pub fn readonly(&self) -> bool { self.attr & A_RO != 0 }
}

/// What `load_tree` hands its builder.
pub trait Tree {
    type Node: Copy;
    fn dir(&mut self, parent: Self::Node, name: &[u8], mdate: u16, mtime: u16) -> Self::Node;
    /// A file: its contents, or None when they could not be read (it is then empty).
    fn file(&mut self, parent: Self::Node, name: &[u8], mdate: u16, mtime: u16, readonly: bool, data: Option<Vec<u8>>);
    /// Bytes loaded so far (whole clusters) of the total in use.
    fn progress(&mut self, _done: u64, _total: u64) {}
}

struct Dir { buf: Vec<u8>, clus: u32 }

pub struct Vol<D: Dev> {
    pub dev: D,
    lba: u32,
    spc: u32,
    csz: u32,
    reserved: u32,
    nfats: u32,
    fatsz: u32,
    root_clus: u32,
    nclus: u32,
    data_lba: u32,
    fsinfo: u32,
    fat: Vec<u32>,
    dirty: Vec<bool>,
    free_count: u32,
    hint: u32,
    serial: u32,
    /// The volume label, NUL-terminated.
    label: [u8; 12],
    /// Loading progress: bytes done.
    load_done: u64,
}

impl<D: Dev> Vol<D> {
    /// Mounts the FAT32 volume at `lba` (`sectors` long, 0 for what the boot sector says).
    pub fn mount(mut dev: D, lba: u32, sectors: u32) -> Result<Self, &'static str> {
        let mut s = [0u8; 512];
        if !dev.read(lba, 1, &mut s) || s[510] != 0x55 || s[511] != 0xAA { return Err("no boot sector"); }
        let (bps, spc, reserved, nfats) = (le16(&s, 11), s[13] as u32, le16(&s, 14) as u32, s[16] as u32);
        let (fatsz16, fatsz, root_clus) = (le16(&s, 22), le32(&s, 36), le32(&s, 44));
        if bps != 512 || spc == 0 || spc & (spc - 1) != 0 || nfats == 0 || nfats > 2 || fatsz16 != 0 || fatsz == 0 || root_clus < 2 { return Err("not FAT32"); }
        let mut total = if le32(&s, 32) != 0 { le32(&s, 32) } else { le16(&s, 19) as u32 };
        if sectors != 0 && total > sectors { total = sectors; }
        if fatsz > 32768 { return Err("FAT too large to cache"); }
        let meta = reserved as u64 + nfats as u64 * fatsz as u64;
        if reserved == 0 || meta >= total as u64 { return Err("damaged boot sector"); }
        let mut nclus = (total - meta as u32) / spc;
        if nclus + 2 > fatsz * 128 { nclus = fatsz * 128 - 2; }
        if nclus == 0 || root_clus >= nclus + 2 { return Err("damaged boot sector"); }
        let fsinfo = match le16(&s, 48) { 0 => 1, f => f as u32 };
        let mut label = [0u8; 12];
        label[..11].copy_from_slice(&s[71..82]);
        let mut l = 11;                                // trailing spaces, from the last byte (as C did)
        while l > 0 && label[l - 1] == b' ' { l -= 1; label[l] = 0; }
        let mut raw = vec![0u8; fatsz as usize * 512];
        if !dev.read(lba + reserved, fatsz, &mut raw) { return Err("cannot read the FAT"); }
        let fat: Vec<u32> = raw.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        let free_count = (2..nclus + 2).filter(|&c| fat[c as usize] & 0x0FFF_FFFF == 0).count() as u32;
        Ok(Vol { dev, lba, spc, csz: spc * 512, reserved, nfats, fatsz, root_clus, nclus, data_lba: lba + meta as u32, fsinfo,
                 fat, dirty: vec![false; fatsz as usize], free_count, hint: 2, serial: le32(&s, 67), label, load_done: 0 })
    }

    pub fn serial(&self) -> u32 { self.serial }
    /// The label, NUL-terminated.
    pub fn label(&self) -> &[u8; 12] { &self.label }
    pub fn nclus(&self) -> u32 { self.nclus }
    pub fn cluster_size(&self) -> u32 { self.csz }
    pub fn free_bytes(&self) -> u64 { self.free_count as u64 * self.csz as u64 }
    pub fn total_bytes(&self) -> u64 { self.nclus as u64 * self.csz as u64 }

    // ------------------------------------------------------------ the FAT

    fn fget(&self, c: u32) -> u32 { if c < self.nclus + 2 { self.fat[c as usize] & 0x0FFF_FFFF } else { EOC } }
    fn fset(&mut self, c: u32, val: u32) {
        if c >= self.nclus + 2 { return; }
        let e = &mut self.fat[c as usize];
        *e = (*e & 0xF000_0000) | (val & 0x0FFF_FFFF);
        self.dirty[(c / 128) as usize] = true;
    }
    /// The end of a chain: an end mark, a free or reserved number, or one past the volume.
    fn is_eoc(&self, c: u32) -> bool { c >= 0x0FFF_FFF8 || c < 2 || c >= self.nclus + 2 }
    fn alloc_clus(&mut self, prev: u32) -> u32 {
        for k in 0..self.nclus {
            let c = 2 + (self.hint.wrapping_sub(2).wrapping_add(k)) % self.nclus;
            if self.fget(c) == 0 {
                self.fset(c, EOC);
                if prev != 0 { self.fset(prev, c); }
                self.hint = if c + 1 >= self.nclus + 2 { 2 } else { c + 1 };
                if self.free_count != 0 { self.free_count -= 1; }
                return c;
            }
        }
        0
    }
    fn free_chain(&mut self, mut c: u32) {
        let mut n = 0;
        while !self.is_eoc(c) && n < self.nclus {
            let nx = self.fget(c);
            self.fset(c, 0);
            self.free_count = self.free_count.wrapping_add(1);
            if c < self.hint { self.hint = c; }
            c = nx;
            n += 1;
        }
    }
    fn clus_lba(&self, c: u32) -> u32 { self.data_lba + (c - 2) * self.spc }

    /// Writes the changed FAT sectors to every FAT and the free count to FSInfo.
    pub fn sync(&mut self) {
        let mut s = 0usize;
        while s < self.fatsz as usize {
            if !self.dirty[s] { s += 1; continue; }
            let mut e = s;
            while e < self.fatsz as usize && self.dirty[e] && e - s < 128 { e += 1; }
            let bytes: Vec<u8> = self.fat[s * 128..e * 128].iter().flat_map(|v| v.to_le_bytes()).collect();
            for f in 0..self.nfats { self.dev.write(self.lba + self.reserved + f * self.fatsz + s as u32, (e - s) as u32, &bytes); }
            for d in &mut self.dirty[s..e] { *d = false; }
            s = e;
        }
        let mut fs = [0u8; 512];
        if self.dev.read(self.lba + self.fsinfo, 1, &mut fs) && le32(&fs, 0) == 0x4161_5252 {
            put32(&mut fs, 488, self.free_count);
            put32(&mut fs, 492, self.hint);
            self.dev.write(self.lba + self.fsinfo, 1, &fs);
        }
    }

    // ------------------------------------------------------------ cluster chains

    fn chain_len(&self, mut c: u32) -> u32 {
        let mut n = 0;
        while !self.is_eoc(c) && n <= self.nclus { n += 1; c = self.fget(c); }
        n
    }
    /// Reads `buf.len()` bytes from the chain at `c` (runs of adjacent clusters at once).
    fn read_chain(&mut self, mut c: u32, buf: &mut [u8]) -> bool {
        let bytes = buf.len();
        let csz = self.csz as usize;
        let mut off = 0usize;
        while off < bytes && !self.is_eoc(c) {
            let mut run = 1u32;
            while c + run < self.nclus + 2 && self.fget(c + run - 1) == c + run && off + run as usize * csz < bytes && run < 64 { run += 1; }
            let want = (run as usize * csz).min(bytes - off);
            let (full, tail) = (want / 512, want % 512);
            if full != 0 && !self.dev.read(self.clus_lba(c), full as u32, &mut buf[off..off + full * 512]) { return false; }
            if tail != 0 {
                let mut t = [0u8; 512];
                if !self.dev.read(self.clus_lba(c) + full as u32, 1, &mut t) { return false; }
                buf[off + full * 512..off + want].copy_from_slice(&t[..tail]);
            }
            off += want;
            c = self.fget(c + run - 1);
        }
        off >= bytes
    }
    /// Allocates a new chain for `data` and writes it: its first cluster (0 when empty), or
    /// None when the volume is full.
    fn write_new_chain(&mut self, data: &[u8]) -> Option<u32> {
        let bytes = data.len();
        if bytes == 0 { return Some(0); }
        let csz = self.csz as usize;
        let n = bytes.div_ceil(csz);
        if n > self.free_count as usize { return None; }
        let (mut first, mut prev, mut run_start, mut run_len, mut off) = (0u32, 0u32, 0u32, 0u32, 0usize);
        let mut ok = true;
        for _ in 0..n {
            let c = self.alloc_clus(prev);
            if c == 0 { if first != 0 { self.free_chain(first); } return None; }
            if first == 0 { first = c; }
            if run_len != 0 && c == run_start + run_len && run_len < 128 / self.spc { run_len += 1; } else {
                if run_len != 0 { off += self.write_run(run_start, (run_len as usize * csz).min(bytes - off), &data[off..]); }
                run_start = c;
                run_len = 1;
            }
            prev = c;
        }
        let want = bytes - off;
        let (full, tail) = (want / 512, want % 512);
        if full != 0 && !self.dev.write(self.clus_lba(run_start), full as u32, &data[off..off + full * 512]) { ok = false; }
        if tail != 0 {
            let mut t = [0u8; 512];
            t[..tail].copy_from_slice(&data[off + full * 512..off + want]);
            if !self.dev.write(self.clus_lba(run_start) + full as u32, 1, &t) { ok = false; }
        }
        if ok { Some(first) } else { None }
    }
    fn write_run(&mut self, start: u32, want: usize, data: &[u8]) -> usize {
        let (full, tail) = (want / 512, want % 512);
        if full != 0 { self.dev.write(self.clus_lba(start), full as u32, &data[..full * 512]); }
        if tail != 0 {
            let mut t = [0u8; 512];
            t[..tail].copy_from_slice(&data[full * 512..want]);
            self.dev.write(self.clus_lba(start) + full as u32, 1, &t);
        }
        want
    }

    // ------------------------------------------------------------ directories

    fn dir_load(&mut self, clus: u32) -> Option<Dir> {
        let clus = if clus == 0 { self.root_clus } else { clus };
        let n = self.chain_len(clus);
        if n == 0 || n > DIR_MAX_CLUS { return None; }
        let mut buf = vec![0u8; (n * self.csz) as usize];
        if !self.read_chain(clus, &mut buf) { return None; }
        Some(Dir { buf, clus })
    }
    fn dir_store(&mut self, d: &Dir) -> bool {
        let (mut c, mut prev) = (d.clus, 0u32);
        for chunk in d.buf.chunks(self.csz as usize) {
            if self.is_eoc(c) { c = self.alloc_clus(prev); if c == 0 { return false; } }
            if !self.dev.write(self.clus_lba(c), self.spc, chunk) { return false; }
            prev = c;
            c = self.fget(c);
        }
        true
    }

    /// The entries of a directory, as the C driver's dir_iter saw them ("." and ".." left out).
    fn entries(d: &Dir) -> Vec<Entry> {
        let mut out = Vec::new();
        let mut lfn = [0u8; 256];
        let (mut lfn_first, mut lfn_ck, mut lfn_ok) = (0usize, 0u8, false);
        for (i, e) in d.buf.chunks_exact(32).enumerate() {
            if e[0] == 0 { break; }
            if e[0] == 0xE5 { lfn_ok = false; continue; }
            if e[11] == A_LFN {
                let seq = (e[0] & 0x1F) as usize;
                if e[0] & 0x40 != 0 { lfn = [0; 256]; lfn_first = i; lfn_ck = e[13]; lfn_ok = true; }
                if !lfn_ok || e[13] != lfn_ck || !(1..=19).contains(&seq) { lfn_ok = false; continue; }
                for (k, &o) in LFN_OFFS.iter().enumerate() {
                    let ch = le16(e, o);
                    let pos = (seq - 1) * 13 + k;
                    if ch == 0 || ch == 0xFFFF || pos >= 255 { break; }
                    lfn[pos] = if ch < 128 { ch as u8 } else { b'_' };
                }
                continue;
            }
            if e[11] & A_LABEL != 0 { lfn_ok = false; continue; }
            let mut first = i;
            let name = if lfn_ok && lfn_sum(&e[..11]) == lfn_ck && lfn[0] != 0 {
                first = lfn_first;
                lfn[..lfn.iter().position(|&c| c == 0).unwrap_or(255).min(255)].to_vec()
            } else { sfn_to_str(e) };
            lfn_ok = false;
            if name == b"." || name == b".." { continue; }
            out.push(Entry { idx: i, first, name, attr: e[11], clus: (le16(e, 20) as u32) << 16 | le16(e, 26) as u32,
                             size: le32(e, 28), mdate: le16(e, 24), mtime: le16(e, 22) });
        }
        out
    }
    fn dir_find(d: &Dir, name: &[u8]) -> Option<Entry> { Self::entries(d).into_iter().find(|e| eq_nocase(&e.name, name)) }

    /// Adds an entry (with a long name when needed): its index, or None when the name is too
    /// long or the directory is full.
    fn dir_add(&mut self, d: &mut Dir, name: &[u8], attr: u8, clus: u32, size: u32) -> Option<usize> {
        let mut sfn = [b' '; 11];
        let need_lfn = make_sfn(d, name, &mut sfn);
        let len = name.len();
        if len > 255 || name == b".." { return None; }
        let nl = if need_lfn { (len + 12) / 13 } else { 0 };
        let need = nl + 1;
        let start = loop {
            let mut run = 0;
            let mut found = None;
            for (i, e) in d.buf.chunks_exact(32).enumerate() {
                if e[0] == 0 || e[0] == 0xE5 { run += 1; if run == need { found = Some(i + 1 - need); break; } } else { run = 0; }
            }
            if let Some(s) = found { break s; }
            if d.buf.len() >= DIR_MAX_BYTES { return None; }
            let n = d.buf.len() + self.csz as usize;
            d.buf.resize(n, 0);
        };
        let ck = lfn_sum(&sfn);
        for k in 0..nl {
            let r = &mut d.buf[(start + k) * 32..(start + k + 1) * 32];
            let seq = nl - k;
            r.fill(0);
            r[0] = seq as u8 | if k == 0 { 0x40 } else { 0 };
            r[11] = A_LFN;
            r[13] = ck;
            for (j, &o) in LFN_OFFS.iter().enumerate() {
                let pos = (seq - 1) * 13 + j;
                let ch: u16 = if pos < len { name[pos] as u16 } else if pos == len { 0 } else { 0xFFFF };
                put16(r, o, ch);
            }
        }
        let i = start + nl;
        let (date, time) = self.dev.now();
        let e = &mut d.buf[i * 32..(i + 1) * 32];
        e.fill(0);
        e[..11].copy_from_slice(&sfn);
        e[11] = attr;
        put16(e, 20, (clus >> 16) as u16);
        put16(e, 26, clus as u16);
        put32(e, 28, size);
        put16(e, 24, date); put16(e, 22, time);
        put16(e, 16, date); put16(e, 18, date); put16(e, 14, time);
        Some(i)
    }
    fn dir_remove(d: &mut Dir, first: usize, idx: usize) { for i in first..=idx { d.buf[i * 32] = 0xE5; } }

    // ------------------------------------------------------------ paths

    /// Walks `parts`: the cluster of the last directory (the root's for none), creating
    /// missing ones when asked; 0 on failure.
    fn walk_dirs(&mut self, parts: &[Vec<u8>], create: bool) -> u32 {
        let mut clus = self.root_clus;
        for p in parts {
            let Some(mut d) = self.dir_load(clus) else { return 0 };
            if let Some(e) = Self::dir_find(&d, p) {
                if !e.is_dir() { return 0; }
                clus = if e.clus != 0 { e.clus } else { self.root_clus };
                continue;
            }
            if !create || p == b".." { return 0; }      // ".." would be a folder no listing shows
            let nc = self.alloc_clus(0);
            if nc == 0 { return 0; }
            let mut t = vec![0u8; self.csz as usize];
            t[..11].copy_from_slice(b".          ");
            t[11] = A_DIR;
            put16(&mut t, 20, (nc >> 16) as u16); put16(&mut t, 26, nc as u16);
            t[32..43].copy_from_slice(b"..         ");
            t[43] = A_DIR;
            let up = if clus == self.root_clus { 0 } else { clus };
            put16(&mut t, 52, (up >> 16) as u16); put16(&mut t, 58, up as u16);
            let (date, time) = self.dev.now();
            put16(&mut t, 24, date); put16(&mut t, 22, time);
            put16(&mut t, 56, date); put16(&mut t, 54, time);
            let lba = self.clus_lba(nc);
            self.dev.write(lba, self.spc, &t);
            if self.dir_add(&mut d, p, A_DIR, nc, 0).is_none() || !self.dir_store(&d) { return 0; }
            clus = nc;
        }
        clus
    }

    /// Whether `walk_dirs(parts, true)` could get through `parts`: no part names a file
    /// (missing folders would be created).
    fn walkable(&mut self, parts: &[Vec<u8>]) -> bool {
        let mut clus = self.root_clus;
        for p in parts {
            let Some(d) = self.dir_load(clus) else { return false };
            match Self::dir_find(&d, p) {
                Some(e) if e.is_dir() => clus = if e.clus != 0 { e.clus } else { self.root_clus },
                Some(_) => return false,
                None => return true,
            }
        }
        true
    }

    /// mkdir -p.
    pub fn mkdir(&mut self, path: &[u8]) -> bool { let p = split(path); self.walk_dirs(&p, true) != 0 }

    pub fn exists(&mut self, path: &[u8]) -> bool {
        let p = split(path);
        let Some((last, dirs)) = p.split_last() else { return true };
        let pc = self.walk_dirs(dirs, false);
        if pc == 0 { return false; }
        match self.dir_load(pc) { Some(d) => Self::dir_find(&d, last).is_some(), None => false }
    }

    /// Creates or replaces a file (the folders on its path are created).
    pub fn write(&mut self, path: &[u8], data: &[u8], readonly: bool) -> bool {
        let p = split(path);
        let Some((last, dirs)) = p.split_last() else { return false };
        let pc = self.walk_dirs(dirs, true);
        if pc == 0 { return false; }
        let Some(mut d) = self.dir_load(pc) else { return false };
        let found = Self::dir_find(&d, last);
        if let Some(e) = &found {
            if e.is_dir() { return false; }
            self.free_chain(e.clus);
            let r = &mut d.buf[e.idx * 32..e.idx * 32 + 32];
            put16(r, 20, 0); put16(r, 26, 0); put32(r, 28, 0);
        }
        let Some(c) = self.write_new_chain(data) else { return false };
        let idx = match found { Some(e) => e.idx, None => match self.dir_add(&mut d, last, A_ARCH, 0, 0) {
            Some(i) => i,
            None => { self.free_chain(c); return false; }
        } };
        let (date, time) = self.dev.now();
        let r = &mut d.buf[idx * 32..idx * 32 + 32];
        put16(r, 20, (c >> 16) as u16); put16(r, 26, c as u16);
        put32(r, 28, data.len() as u32);
        r[11] = (r[11] & !A_RO) | A_ARCH | if readonly { A_RO } else { 0 };
        put16(r, 24, date); put16(r, 22, time); put16(r, 18, date);
        self.dir_store(&d)
    }

    /// A file's contents, or None.
    pub fn read(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        let p = split(path);
        let (last, dirs) = p.split_last()?;
        let pc = self.walk_dirs(dirs, false);
        if pc == 0 { return None; }
        let d = self.dir_load(pc)?;
        let e = Self::dir_find(&d, last)?;
        if e.is_dir() { return None; }
        self.file_data(e.clus, e.size)
    }

    /// A file's data: `size` bytes from its chain, checked against the chain's length before
    /// anything is allocated.
    fn file_data(&mut self, clus: u32, size: u32) -> Option<Vec<u8>> {
        if size == 0 { return Some(Vec::new()); }
        if size as u64 > self.chain_len(clus) as u64 * self.csz as u64 { return None; }
        let mut v = Vec::new();
        v.try_reserve_exact(size as usize).ok()?;
        v.resize(size as usize, 0);
        if self.read_chain(clus, &mut v) { Some(v) } else { None }
    }

    /// Frees everything below the directory at `clus` (not its own chain), depth first in the
    /// C driver's order; stops at cycles and at MAX_DEPTH levels.
    fn delete_tree(&mut self, clus: u32) {
        if clus == 0 { return; }
        let Some(d) = self.dir_load(clus) else { return };
        // frames: (entries, next index, the directory's cluster)
        let mut stack: Vec<(Vec<Entry>, usize, u32)> = vec![(Self::entries(&d), 0, clus)];
        while let Some(top) = stack.last_mut() {
            if top.1 >= top.0.len() {
                stack.pop();
                if let Some(parent) = stack.last() {
                    let c = parent.0[parent.1 - 1].clus;
                    self.free_chain(c);
                }
                continue;
            }
            let e = top.0[top.1].clone();
            top.1 += 1;
            let descend = e.is_dir() && e.clus != 0 && stack.len() < MAX_DEPTH && !stack.iter().any(|f| f.2 == e.clus);
            if descend {
                if let Some(sub) = self.dir_load(e.clus) { stack.push((Self::entries(&sub), 0, e.clus)); continue; }
            }
            self.free_chain(e.clus);
        }
    }

    /// Deletes a file, or a folder with everything in it.
    pub fn delete(&mut self, path: &[u8]) -> bool {
        let p = split(path);
        let Some((last, dirs)) = p.split_last() else { return false };
        let pc = self.walk_dirs(dirs, false);
        if pc == 0 { return false; }
        let Some(mut d) = self.dir_load(pc) else { return false };
        let Some(e) = Self::dir_find(&d, last) else { return false };
        if e.is_dir() { self.delete_tree(e.clus); }
        self.free_chain(e.clus);
        Self::dir_remove(&mut d, e.first, e.idx);
        self.dir_store(&d)
    }

    /// Renames or moves a file or folder, replacing what is at `to`. A folder cannot be moved
    /// into itself.
    pub fn rename(&mut self, from: &[u8], to: &[u8]) -> bool {
        let (pa, pb) = (split(from), split(to));
        if pa.is_empty() || pb.is_empty() { return false; }
        let ca = self.walk_dirs(&pa[..pa.len() - 1], false);
        if ca == 0 { return false; }
        let Some(mut d) = self.dir_load(ca) else { return false };
        let Some(e) = Self::dir_find(&d, &pa[pa.len() - 1]) else { return false };
        if e.is_dir() && pb.len() > pa.len() && pa.iter().zip(&pb).all(|(a, b)| eq_nocase(a, b)) { return false; }
        if pb.iter().any(|p| p == b"..") || !self.walkable(&pb[..pb.len() - 1]) { return false; }
        let saved: [u8; 32] = d.buf[e.idx * 32..e.idx * 32 + 32].try_into().unwrap();
        Self::dir_remove(&mut d, e.first, e.idx);
        if !self.dir_store(&d) { return false; }
        let cb = self.walk_dirs(&pb[..pb.len() - 1], true);
        if cb == 0 { return false; }
        let Some(mut d) = self.dir_load(cb) else { return false };
        if let Some(old) = Self::dir_find(&d, &pb[pb.len() - 1]) {
            if old.is_dir() { self.delete_tree(old.clus); }
            self.free_chain(old.clus);
            Self::dir_remove(&mut d, old.first, old.idx);
        }
        let sclus = (le16(&saved, 20) as u32) << 16 | le16(&saved, 26) as u32;
        let Some(i) = self.dir_add(&mut d, &pb[pb.len() - 1], saved[11], sclus, le32(&saved, 28)) else { return false };
        for o in [24usize, 22, 16, 14] { d.buf[i * 32 + o..i * 32 + o + 2].copy_from_slice(&saved[o..o + 2]); }
        let r = self.dir_store(&d);
        if r && saved[11] & A_DIR != 0 && ca != cb && sclus != 0 {
            if let Some(mut sub) = self.dir_load(sclus) {
                if sub.buf.len() >= 64 && sub.buf[32] == b'.' && sub.buf[33] == b'.' {
                    let up = if cb == self.root_clus { 0 } else { cb };
                    put16(&mut sub.buf, 52, (up >> 16) as u16); put16(&mut sub.buf, 58, up as u16);
                    self.dir_store(&sub);
                }
            }
        }
        r
    }

    // ------------------------------------------------------------ the whole tree

    /// Hands every folder and file to `t`, depth first in directory order (folders down to
    /// LOAD_DEPTH levels); returns the number of files, or None when the root cannot be read.
    pub fn load_tree<T: Tree>(&mut self, t: &mut T, root: T::Node) -> Option<u32> {
        let d = self.dir_load(self.root_clus)?;
        let total = (self.nclus - self.free_count.min(self.nclus)) as u64 * self.csz as u64;
        self.load_done = 0;
        let mut files = 0u32;
        let mut stack: Vec<(Vec<Entry>, usize, T::Node)> = vec![(Self::entries(&d), 0, root)];
        while let Some(top) = stack.last_mut() {
            if top.1 >= top.0.len() { stack.pop(); continue; }
            let e = top.0[top.1].clone();
            top.1 += 1;
            let parent = top.2;
            let depth = stack.len() - 1;
            if e.is_dir() {
                let n = t.dir(parent, &e.name, e.mdate, e.mtime);
                if depth < LOAD_DEPTH && e.clus != 0 {
                    if let Some(sub) = self.dir_load(e.clus) { stack.push((Self::entries(&sub), 0, n)); }
                }
            } else {
                let data = self.file_data(e.clus, e.size);
                t.file(parent, &e.name, e.mdate, e.mtime, e.readonly(), data);
                files += 1;
                self.load_done += (e.size as u64).div_ceil(self.csz as u64) * self.csz as u64;
                t.progress(self.load_done, total);
            }
        }
        t.progress(total, total);
        Some(files)
    }
}

fn lfn_sum(n: &[u8]) -> u8 { n[..11].iter().fold(0u8, |s, &c| ((s & 1) << 7).wrapping_add(s >> 1).wrapping_add(c)) }

fn sfn_to_str(e: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(12);
    for i in 0..8 {
        if e[i] == b' ' { break; }
        let c = if i == 0 && e[0] == 0x05 { 0xE5 } else { e[i] };
        out.push(if e[12] & 0x08 != 0 { lower(c) } else { c });
    }
    if e[8] != b' ' {
        out.push(b'.');
        for i in 8..11 { if e[i] == b' ' { break; } out.push(if e[12] & 0x10 != 0 { lower(e[i]) } else { e[i] }); }
    }
    if let Some(z) = out.iter().position(|&c| c == 0) { out.truncate(z); }
    out
}

fn sfn_taken(d: &Dir, s: &[u8; 11]) -> bool {
    for e in d.buf.chunks_exact(32) {
        if e[0] == 0 { break; }
        if e[0] == 0xE5 || e[11] == A_LFN { continue; }
        if &e[..11] == s { return true; }
    }
    false
}

/// Builds the 8.3 alias for `name`; true when a long-name entry is needed.
fn make_sfn(d: &Dir, name: &[u8], sfn: &mut [u8; 11]) -> bool {
    const BAD: &[u8] = b"\"*+,/:;<=>?[\\]|";
    *sfn = [b' '; 11];
    let dot = name.iter().rposition(|&c| c == b'.').filter(|&p| p != 0);
    let (mut lossy, mut low) = (false, false);
    let (mut nb, mut ne) = (0usize, 0usize);
    let base = &name[..dot.unwrap_or(name.len())];
    let mut part = |src: &[u8], at: usize, max: usize, n: &mut usize, sfn: &mut [u8; 11]| {
        for &c0 in src {
            if c0 == b' ' || c0 == b'.' { lossy = true; continue; }
            let mut c = c0;
            if c >= 128 || BAD.contains(&c) { c = b'_'; lossy = true; }
            if c.is_ascii_lowercase() { low = true; }
            if *n < max { sfn[at + *n] = c.to_ascii_uppercase(); *n += 1; } else { lossy = true; }
        }
    };
    part(base, 0, 8, &mut nb, sfn);
    if let Some(p) = dot { part(&name[p + 1..], 8, 3, &mut ne, sfn); }
    if nb == 0 { sfn[0] = b'_'; nb = 1; lossy = true; }
    if !lossy && !sfn_taken(d, sfn) { return low; }
    for n in 1..1_000_000u32 {
        let tail = alloc::format!("~{n}");
        let tl = tail.len();
        let keep = nb.min(8 - tl);
        let mut t = *sfn;
        t[keep..keep + tl].copy_from_slice(tail.as_bytes());
        for c in &mut t[keep + tl..8] { *c = b' '; }
        if !sfn_taken(d, &t) { *sfn = t; return true; }
    }
    true
}

/// A path's parts: separated by \ or /, at most 32 of at most 127 bytes, "." left out.
fn split(path: &[u8]) -> Vec<Vec<u8>> {
    let path = &path[..path.iter().position(|&c| c == 0).unwrap_or(path.len())];
    let mut out = Vec::new();
    for p in path.split(|&c| c == b'\\' || c == b'/') {
        if out.len() >= 32 { break; }
        if p.is_empty() { continue; }
        let p = &p[..p.len().min(127)];
        if p != b"." { out.push(p.to_vec()); }
    }
    out
}

/// Formats a FAT32 volume of `total` sectors at `lba`; `full` also zeroes the data area.
/// `cb(percent, what)` reports progress. Returns the number of clusters and their size.
pub fn format<D: Dev>(dev: &mut D, lba: u32, total: u32, label: &[u8], full: bool, serial: u32, mut cb: impl FnMut(i32, &str)) -> Result<(u32, u32), alloc::string::String> {
    use alloc::format;
    if total < 65536 { return Err("The partition is too small for FAT32 (minimum 32 MB).".into()); }
    let spc: u32 = if total < 532_480 { 1 } else if total < 16_777_216 { 8 } else if total < 33_554_432 { 16 } else if total < 67_108_864 { 32 } else { 64 };
    let (reserved, nfats) = (32u32, 2u32);
    let t2 = (256 * spc + nfats) / 2;
    let fatsz = (total - reserved).div_ceil(t2);
    let nclus = (total - reserved - nfats * fatsz) / spc;
    if fatsz > 32768 { return Err("The partition is too large for this version of Setup (maximum about 128 GB).".into()); }
    cb(0, "Writing boot sector");
    if !dev.zero(lba, reserved) { return Err(format!("Setup could not write to the disk (sector {lba}).")); }
    let mut s = [0u8; 512];
    s[..3].copy_from_slice(&[0xEB, 0x58, 0x90]);
    s[3..11].copy_from_slice(b"WRLDOS  ");
    put16(&mut s, 11, 512); s[13] = spc as u8; put16(&mut s, 14, reserved as u16); s[16] = nfats as u8; s[21] = 0xF8;
    put16(&mut s, 24, 63); put16(&mut s, 26, 255); put32(&mut s, 28, lba); put32(&mut s, 32, total); put32(&mut s, 36, fatsz);
    put32(&mut s, 44, 2); put16(&mut s, 48, 1); put16(&mut s, 50, 6); s[64] = 0x80; s[66] = 0x29; put32(&mut s, 67, serial);
    let mut lab = [b' '; 11];
    for (i, &c) in label.iter().take_while(|&&c| c != 0).take(11).enumerate() { lab[i] = c.to_ascii_uppercase(); }
    let named = lab != [b' '; 11];
    if !named { lab = *b"NO NAME    "; }                // and no label entry, which may not be blank
    s[71..82].copy_from_slice(&lab);
    s[82..90].copy_from_slice(b"FAT32   ");
    s[0x5A..0x5E].copy_from_slice(&[0xFA, 0xF4, 0xEB, 0xFD]);          // cli; hlt; jmp $-1: boot through the MBR
    s[510] = 0x55; s[511] = 0xAA;
    dev.write(lba, 1, &s); dev.write(lba + 6, 1, &s);
    let mut fs = [0u8; 512];
    put32(&mut fs, 0, 0x4161_5252); put32(&mut fs, 484, 0x6141_7272);
    put32(&mut fs, 488, nclus - 1); put32(&mut fs, 492, 3); put32(&mut fs, 508, 0xAA55_0000);
    dev.write(lba + 1, 1, &fs); dev.write(lba + 7, 1, &fs);
    let fat_total = nfats * fatsz;
    let mut done = 0;
    while done < fat_total {
        let n = (fat_total - done).min(2048);
        if !dev.zero(lba + reserved + done, n) { return Err("Setup could not write the file allocation table.".into()); }
        done += n;
        cb((5 + (if full { 10 } else { 85 }) * done as u64 / fat_total as u64) as i32, "Creating file allocation tables");
    }
    let mut f0 = [0u8; 512];
    put32(&mut f0, 0, 0x0FFF_FFF8); put32(&mut f0, 4, 0x0FFF_FFFF); put32(&mut f0, 8, 0x0FFF_FFFF);
    for f in 0..nfats { dev.write(lba + reserved + f * fatsz, 1, &f0); }
    let data = lba + reserved + nfats * fatsz;
    let mut root = vec![0u8; spc as usize * 512];
    if named {
        root[..11].copy_from_slice(&lab);
        root[11] = A_LABEL;
        let (date, time) = dev.now();
        put16(&mut root, 24, date); put16(&mut root, 22, time);
    }
    dev.write(data, spc, &root);
    if full {
        let (len, step) = (nclus * spc, 4096u32);
        let mut o = spc;
        while o < len {
            if !dev.zero(data + o, step.min(len - o)) { return Err(format!("Setup found a bad sector at {} and cannot format this partition.", data + o)); }
            cb(15 + (o as u64 * 85 / len as u64) as i32, "Formatting");
            o += step;
        }
    }
    cb(100, "Format complete");
    Ok((nclus, spc * 512))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A disk in memory, with a fixed clock.
    pub struct Mem(pub Vec<u8>);
    impl Dev for Mem {
        fn read(&mut self, lba: u32, n: u32, buf: &mut [u8]) -> bool {
            let (a, b) = (lba as usize * 512, (lba + n) as usize * 512);
            if b > self.0.len() || buf.len() < b - a { return false; }
            buf[..b - a].copy_from_slice(&self.0[a..b]); true
        }
        fn write(&mut self, lba: u32, n: u32, buf: &[u8]) -> bool {
            let (a, b) = (lba as usize * 512, (lba + n) as usize * 512);
            if b > self.0.len() { return false; }
            let k = (b - a).min(buf.len());
            self.0[a..a + k].copy_from_slice(&buf[..k]); true
        }
        fn zero(&mut self, lba: u32, n: u32) -> bool {
            let (a, b) = (lba as usize * 512, (lba + n) as usize * 512);
            if b > self.0.len() { return false; }
            self.0[a..b].fill(0); true
        }
        fn now(&self) -> (u16, u16) { dos_time(2026, 11, 20, 12, 34, 56) }
    }

    fn fresh(mb: usize) -> Vol<Mem> {
        let mut m = Mem(vec![0; mb << 20]);
        format(&mut m, 0, (mb << 11) as u32, b"Test", false, 0x1234_5678, |_, _| {}).unwrap();
        Vol::mount(m, 0, 0).unwrap()
    }

    struct Names(Vec<(usize, Vec<u8>, Option<usize>)>);
    impl Tree for Names {
        type Node = usize;
        fn dir(&mut self, p: usize, name: &[u8], _: u16, _: u16) -> usize { self.0.push((p, name.to_vec(), None)); self.0.len() }
        fn file(&mut self, p: usize, name: &[u8], _: u16, _: u16, _: bool, d: Option<Vec<u8>>) { self.0.push((p, name.to_vec(), Some(d.map(|d| d.len()).unwrap_or(usize::MAX)))); }
    }

    #[test]
    fn format_mount_and_files() {
        let mut v = fresh(40);
        assert_eq!(v.label()[..5], *b"TEST\0");
        assert_eq!(v.serial(), 0x1234_5678);
        let free0 = v.free_bytes();
        assert!(v.write(b"WRLD\\Hello world.txt", b"hello", false));
        assert!(v.write(b"WRLD/short.TXT", &[7u8; 10000], true));
        assert_eq!(v.read(b"wrld\\HELLO WORLD.TXT").unwrap(), b"hello");
        assert_eq!(v.read(b"WRLD\\SHORT.txt").unwrap(), vec![7u8; 10000]);
        assert!(v.exists(b"WRLD") && v.exists(b"") && !v.exists(b"nope\\x"));
        assert!(v.free_bytes() < free0);
        assert!(v.write(b"WRLD\\short.TXT", b"x", false));                     // replace
        assert_eq!(v.read(b"WRLD\\short.TXT").unwrap(), b"x");
        assert!(v.rename(b"WRLD\\short.TXT", b"Other\\Moved file.txt"));
        assert!(v.read(b"WRLD\\short.TXT").is_none());
        assert_eq!(v.read(b"other\\moved file.txt").unwrap(), b"x");
        assert!(!v.rename(b"Other", b"Other\\Inside"));                          // not into itself
        assert!(v.delete(b"WRLD"));
        assert!(!v.exists(b"WRLD\\Hello world.txt"));
        v.sync();
        let free = v.free_bytes();
        let m = v.dev;
        let mut v2 = Vol::mount(m, 0, 0).unwrap();
        assert_eq!(v2.free_bytes(), free);
        let mut t = Names(Vec::new());
        assert_eq!(v2.load_tree(&mut t, 0), Some(1));
        assert_eq!(t.0, vec![(0, b"Other".to_vec(), None), (1, b"Moved file.txt".to_vec(), Some(1))]);
    }

    #[test]
    fn short_names_and_many_entries() {
        let mut v = fresh(40);
        for i in 0..300 { assert!(v.write(alloc::format!("dir\\Long file name number {i}.txt").as_bytes(), &[i as u8], false)); }
        assert!(v.write(b"dir\\UPPER.TXT", b"u", false));
        let d = { let c = v.walk_dirs(&[b"dir".to_vec()], false); v.dir_load(c).unwrap() };
        let es = Vol::<Mem>::entries(&d);
        assert_eq!(es.len(), 301);
        let up = es.iter().find(|e| e.name == b"UPPER.TXT").unwrap();
        assert_eq!(up.first, up.idx);                                               // no long name needed
        assert_eq!(&d.buf[es[0].idx * 32..es[0].idx * 32 + 11], b"LONGFI~1TXT");
        assert_eq!(&d.buf[es[10].idx * 32..es[10].idx * 32 + 11], b"LONGF~11TXT");
        for i in [0, 150, 299] { assert_eq!(v.read(alloc::format!("DIR\\long file name number {i}.TXT").as_bytes()).unwrap(), [i as u8]); }
    }

    #[test]
    fn damaged_volumes() {
        let mut v = fresh(40);
        v.write(b"a\\b\\file", &[1; 5000], false);
        v.sync();
        let good = v.dev.0.clone();
        // a directory that contains itself: deletion must stop
        let mut v = Vol::mount(Mem(good.clone()), 0, 0).unwrap();
        let c = v.walk_dirs(&[b"a".to_vec()], false);
        let mut d = v.dir_load(c).unwrap();
        v.dir_add(&mut d, b"loop", A_DIR, c, 0).unwrap();
        v.dir_store(&d);
        assert!(v.delete(b"a"));
        // a file whose size is far beyond its chain
        let mut v = Vol::mount(Mem(good.clone()), 0, 0).unwrap();
        let c = v.walk_dirs(&[b"a".to_vec(), b"b".to_vec()], false);
        let mut d = v.dir_load(c).unwrap();
        let e = Vol::<Mem>::dir_find(&d, b"file").unwrap();
        put32(&mut d.buf, e.idx * 32 + 28, 0xFFFF_FFF0);
        v.dir_store(&d);
        assert!(v.read(b"a\\b\\file").is_none());
        // boot sectors that do not add up
        for (o, val) in [(14usize, 0u32), (36, 0x8001), (44, 0x0FFF_FFF0), (32, 100)] {
            let mut img = good.clone();
            if o == 14 { put16(&mut img, o, val as u16); } else { put32(&mut img, o, val); }
            assert!(Vol::mount(Mem(img), 0, 0).is_err(), "offset {o}");
        }
        // random damage: never panics
        let mut seed = 99u32;
        let mut rnd = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for _ in 0..120 {
            let mut img = good.clone();
            for _ in 0..(1 + rnd() % 40) {
                let o = match rnd() % 3 { 0 => rnd() as usize % 512, 1 => 0x4000 + rnd() as usize % 0x8000, _ => rnd() as usize % img.len() };
                img[o] = rnd() as u8;
            }
            if let Ok(mut v) = Vol::mount(Mem(img), 0, 0) {
                let mut t = Names(Vec::new());
                v.load_tree(&mut t, 0);
                v.read(b"a\\b\\file");
                v.write(b"a\\new", b"data", false);
                v.rename(b"a\\b", b"c\\d");
                v.delete(b"c");
                v.delete(b"a");
                v.sync();
            }
        }
    }
}
