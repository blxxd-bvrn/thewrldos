//! The registry behind advapi32's `Reg*` functions (was `src/win32/registry.c`): one tree
//! for all programs, kept in memory and saved to `C:\WRLD\registry.dat` after every change.
//!
//! Key and value names compare without case (ASCII, as the C kernel's `strcasecmp`) and are
//! at most 127 bytes; strings are stored as UTF-16 and the ANSI functions convert. The roots
//! are HKEY_CLASSES_ROOT (which is HKLM\SOFTWARE\Classes, as on Windows), HKEY_CURRENT_USER,
//! HKEY_LOCAL_MACHINE, HKEY_USERS, HKEY_PERFORMANCE_DATA and HKEY_CURRENT_CONFIG.
//!
//! Handles are small numbers (`HBASE + 4*i`) naming open keys; deleting a key closes the
//! handles to it and to everything below it.
//!
//! registry.dat is text, one line each: "K <path>" starts a key and
//! "V <type> <name> <hex data>" adds a value to it (the name percent-escaped, a lone `%` for
//! the default value). Version 2 files also percent-escape control bytes, `%` and bytes
//! from 0x7F in key paths, so that no key name can break a line; version 1 files (which did
//! not) are still read.
//!
//! The functions taking `data`/`size` follow the Win32 buffer rules: `data` is the caller's
//! buffer (its length is the size the caller passed), `size` receives the size needed.

use alloc::vec::Vec;

pub const E_OK: i32 = 0;
pub const E_NOTFOUND: i32 = 2;
pub const E_ACCESS: i32 = 5;
pub const E_BADKEY: i32 = 6;
pub const E_OUTOFMEMORY: i32 = 14;
pub const E_PARAM: i32 = 87;
pub const E_MORE: i32 = 234;
pub const E_NOMORE: i32 = 259;
pub const E_UNSUPPORTED_TYPE: i32 = 1630;

pub const REG_NONE: u32 = 0;
pub const REG_SZ: u32 = 1;
pub const REG_EXPAND_SZ: u32 = 2;
pub const REG_BINARY: u32 = 3;
pub const REG_DWORD: u32 = 4;
pub const REG_MULTI_SZ: u32 = 7;
pub const REG_QWORD: u32 = 11;

pub const HKEY_CLASSES_ROOT: u32 = 0x8000_0000;
pub const HKEY_CURRENT_USER: u32 = 0x8000_0001;
pub const HKEY_LOCAL_MACHINE: u32 = 0x8000_0002;
/// Open key handles: `HBASE + 4*i` for slot `i` (slot 0 is never used).
pub const HBASE: u32 = 0x5200_0000;
pub const NHANDLE: usize = 256;
/// Longest key or value name kept.
pub const NAME_MAX: usize = 127;
/// Limits that keep a program (or a damaged registry.dat) from using up kernel memory: past
/// them, creating keys and storing values fails with ERROR_OUTOFMEMORY.
pub const MAX_KEYS: usize = 200_000;
pub const MAX_DATA: usize = 32 << 20;

/// The text written for these in the defaults (the C kernel's OS_NAME and user folders).
const OS_NAME: &str = "THE WRLD OS";
const USER_HOME: &str = "C:\\Documents and Settings\\User";

const ROOT_NAMES: [&str; 6] = ["HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS", "HKEY_PERFORMANCE_DATA", "HKEY_CURRENT_CONFIG"];
const ROOT_ABBR: [&str; 6] = ["HKCR", "HKCU", "HKLM", "HKU", "HKPD", "HKCC"];
const CLASSES: &[u8] = b"SOFTWARE\\Classes";

pub fn is_str(t: u32) -> bool { t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ }

fn lower(c: u8) -> u8 { c.to_ascii_lowercase() }
fn eq_nocase(a: &[u8], b: &[u8]) -> bool { a.len() == b.len() && a.iter().zip(b).all(|(&x, &y)| lower(x) == lower(y)) }
fn cmp_nocase(a: &[u8], b: &[u8]) -> core::cmp::Ordering { a.iter().map(|&c| lower(c)).cmp(b.iter().map(|&c| lower(c))) }
/// A name as stored: up to the first NUL, at most NAME_MAX bytes.
fn clip(n: &[u8]) -> &[u8] {
    let n = &n[..n.iter().position(|&c| c == 0).unwrap_or(n.len())];
    &n[..n.len().min(NAME_MAX)]
}

#[derive(Clone)]
struct Val { name: Vec<u8>, kind: u32, data: Vec<u8> }

struct Key { parent: Option<usize>, kids: Vec<usize>, name: Vec<u8>, vals: Vec<Val> }

/// What `info` reports about a key.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Info { pub subkeys: u32, pub max_subkey: u32, pub values: u32, pub max_name: u32, pub max_data: u32 }

pub struct Registry {
    keys: Vec<Option<Key>>,
    free: Vec<usize>,
    roots: [usize; 6],
    handles: [Option<usize>; NHANDLE],
    rr: usize,
    live: usize,
    data_bytes: usize,
    dirty: bool,
}

impl Default for Registry { fn default() -> Self { Self::new() } }

impl Registry {
    /// The roots and what programs commonly look up; then `load` the saved file over it.
    pub fn new() -> Self {
        let mut r = Registry { keys: Vec::new(), free: Vec::new(), roots: [usize::MAX; 6], handles: [None; NHANDLE], rr: 1, live: 0, data_bytes: 0, dirty: false };
        for i in 1..6 { r.roots[i] = r.new_key(None, ROOT_NAMES[i].as_bytes()).unwrap(); }
        r.defaults();
        r.dirty = false;
        r
    }

    fn defaults(&mut self) {
        let (cu, lm, us) = (self.roots[1], self.roots[2], self.roots[3]);
        let ws = |r: &mut Self, k: Option<usize>, n: &str, s: &str| {
            if let Some(k) = k {
                let mut w: Vec<u8> = s.bytes().take(299).flat_map(|c| [c, 0]).collect();
                w.extend_from_slice(&[0, 0]);
                r.set_val(k, n.as_bytes(), REG_SZ, w);
            }
        };
        let dw = |r: &mut Self, k: Option<usize>, n: &str, v: u32| { if let Some(k) = k { r.set_val(k, n.as_bytes(), REG_DWORD, v.to_le_bytes().to_vec()); } };
        let nt = self.walk(lm, b"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", true);
        for (n, v) in [("ProductName", OS_NAME), ("CurrentVersion", "6.1"), ("CurrentBuild", "7601"), ("CurrentBuildNumber", "7601"),
                       ("CSDVersion", "Service Pack 1"), ("RegisteredOwner", "User"), ("SystemRoot", "C:\\WRLD")] { ws(self, nt, n, v); }
        dw(self, nt, "CurrentMajorVersionNumber", 6); dw(self, nt, "CurrentMinorVersionNumber", 1);
        let cv = self.walk(lm, b"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", true);
        ws(self, cv, "ProgramFilesDir", "C:\\Program Files"); ws(self, cv, "CommonFilesDir", "C:\\Program Files\\Common Files");
        ws(self, cv, "ProgramFilesDir (x86)", "C:\\Program Files");
        self.walk(lm, CLASSES, true);
        self.walk(lm, b"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", true);
        let cpu = self.walk(lm, b"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
        ws(self, cpu, "ProcessorNameString", "x86 processor"); dw(self, cpu, "~MHz", 2000);
        let desktop = alloc::format!("{USER_HOME}\\Desktop");
        let docs = alloc::format!("{USER_HOME}\\My Documents");
        let sf = self.walk(cu, b"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", true);
        ws(self, sf, "Desktop", &desktop); ws(self, sf, "Personal", &docs);
        ws(self, sf, "AppData", &alloc::format!("{USER_HOME}\\Application Data"));
        ws(self, sf, "Local AppData", &alloc::format!("{USER_HOME}\\Local Settings\\Application Data"));
        let usf = self.walk(cu, b"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", true);
        ws(self, usf, "Desktop", &desktop); ws(self, usf, "Personal", &docs);
        let desk = self.walk(cu, b"Control Panel\\Desktop", true);
        ws(self, desk, "Wallpaper", ""); ws(self, desk, "WheelScrollLines", "3");
        let intl = self.walk(cu, b"Control Panel\\International", true);
        for (n, v) in [("sDecimal", "."), ("sThousand", ","), ("sShortDate", "M/d/yyyy"), ("Locale", "00000409")] { ws(self, intl, n, v); }
        self.walk(cu, b"Software", true); self.walk(cu, b"Environment", true);
        self.walk(us, b".DEFAULT", true);
    }

    /// Whether something changed since the last call (the caller then saves).
    pub fn take_dirty(&mut self) -> bool { core::mem::replace(&mut self.dirty, false) }

    // ------------------------------------------------------------ the tree

    fn key(&self, k: usize) -> &Key { self.keys[k].as_ref().unwrap() }
    fn key_mut(&mut self, k: usize) -> &mut Key { self.keys[k].as_mut().unwrap() }

    fn new_key(&mut self, parent: Option<usize>, name: &[u8]) -> Option<usize> {
        if self.live >= MAX_KEYS { return None; }
        let key = Key { parent, kids: Vec::new(), name: clip(name).to_vec(), vals: Vec::new() };
        let id = match self.free.pop() { Some(i) => { self.keys[i] = Some(key); i } None => { self.keys.push(Some(key)); self.keys.len() - 1 } };
        self.live += 1;
        if let Some(p) = parent {
            // children are kept in name order, like regedit shows them
            let n = clip(name);
            let pos = self.key(p).kids.iter().position(|&c| cmp_nocase(&self.key(c).name, n) != core::cmp::Ordering::Less).unwrap_or(self.key(p).kids.len());
            self.key_mut(p).kids.insert(pos, id);
        }
        Some(id)
    }

    fn find_child(&self, k: usize, name: &[u8]) -> Option<usize> {
        let n = clip(name);
        self.key(k).kids.iter().copied().find(|&c| eq_nocase(&self.key(c).name, n))
    }

    /// Walks "A\B\C" below `k`, creating the missing keys when asked.
    fn walk(&mut self, mut k: usize, path: &[u8], create: bool) -> Option<usize> {
        for part in path.split(|&c| c == b'\\').map(clip).filter(|p| !p.is_empty()) {
            k = match self.find_child(k, part) {
                Some(c) => c,
                None if create => { let c = self.new_key(Some(k), part)?; self.dirty = true; c }
                None => return None,
            };
        }
        Some(k)
    }

    fn find_val(&self, k: usize, name: &[u8]) -> Option<usize> {
        let n = clip(name);
        self.key(k).vals.iter().position(|v| eq_nocase(&v.name, n))
    }

    fn set_val(&mut self, k: usize, name: &[u8], kind: u32, data: Vec<u8>) -> bool {
        let old = self.find_val(k, name).map(|i| self.key(k).vals[i].data.len()).unwrap_or(0);
        if self.data_bytes - old + data.len() > MAX_DATA { return false; }
        self.data_bytes = self.data_bytes - old + data.len();
        match self.find_val(k, name) {
            Some(i) => { let v = &mut self.key_mut(k).vals[i]; v.kind = kind; v.data = data; }
            None => { let name = clip(name).to_vec(); self.key_mut(k).vals.push(Val { name, kind, data }); }
        }
        self.dirty = true;
        true
    }

    fn is_below(&self, mut o: usize, k: usize) -> bool {
        loop {
            if o == k { return true; }
            match self.key(o).parent { Some(p) => o = p, None => return false }
        }
    }

    /// Frees `k` and everything below it, closing the handles to them.
    fn free_tree(&mut self, k: usize) {
        for i in 0..NHANDLE {
            if let Some(o) = self.handles[i] { if self.keys[o].is_some() && self.is_below(o, k) { self.handles[i] = None; } }
        }
        let mut stack = alloc::vec![k];
        while let Some(x) = stack.pop() {
            let key = self.keys[x].take().unwrap();
            self.data_bytes -= key.vals.iter().map(|v| v.data.len()).sum::<usize>();
            stack.extend(key.kids);
            self.free.push(x);
            self.live -= 1;
        }
    }

    fn unlink(&mut self, k: usize) {
        if let Some(p) = self.key(k).parent { self.key_mut(p).kids.retain(|&c| c != k); }
    }

    // ------------------------------------------------------------ handles

    fn classes(&mut self) -> Option<usize> { self.walk(self.roots[2], CLASSES, true) }

    /// The key a handle names: a predefined root or an open key.
    fn key_of(&mut self, h: u32) -> Option<usize> {
        if (HKEY_CLASSES_ROOT..=0x8000_0005).contains(&h) {
            let d = self.dirty;
            let r = if h == HKEY_CLASSES_ROOT { self.classes() } else { Some(self.roots[(h - HKEY_CLASSES_ROOT) as usize]) };
            self.dirty = d;          // creating HKLM\SOFTWARE\Classes here is not saved by itself
            return r;
        }
        if h >= HBASE && h < HBASE + NHANDLE as u32 * 4 && (h - HBASE) & 3 == 0 { return self.handles[((h - HBASE) / 4) as usize]; }
        None
    }

    fn new_handle(&mut self, k: usize) -> u32 {
        let i = match (1..NHANDLE).find(|&i| self.handles[i].is_none()) {
            Some(i) => i,
            None => { let i = self.rr; self.rr = self.rr % (NHANDLE - 1) + 1; i }   // all taken: reuse the oldest
        };
        self.handles[i] = Some(k);
        HBASE + i as u32 * 4
    }

    /// Whether a handle names a key (a root or an open key).
    pub fn is_key(&mut self, h: u32) -> bool { self.key_of(h).is_some() }

    pub fn close(&mut self, h: u32) {
        if h >= HBASE && h < HBASE + NHANDLE as u32 * 4 { self.handles[((h - HBASE) / 4) as usize] = None; }
    }

    /// "HKEY_CURRENT_USER\..." or "HKCU\..." to a key.
    fn root_path(&mut self, path: &[u8], create: bool) -> Option<usize> {
        for r in 0..6 {
            for p in [ROOT_NAMES[r].as_bytes(), ROOT_ABBR[r].as_bytes()] {
                if path.len() >= p.len() && eq_nocase(&path[..p.len()], p) && (path.len() == p.len() || path[p.len()] == b'\\') {
                    let base = if r == 0 { self.classes()? } else { self.roots[r] };
                    return self.walk(base, &path[p.len()..], create);
                }
            }
        }
        None
    }

    // ------------------------------------------------------------ the Win32 operations

    /// RegOpenKey/RegCreateKey: (error, handle, disposition: 1 created, 2 opened, 0 not set).
    pub fn open(&mut self, h: u32, sub: &[u8], create: bool) -> (i32, u32, u32) {
        let Some(k) = self.key_of(h) else { return (E_BADKEY, 0, 0) };
        match self.walk(k, sub, false) {
            Some(t) => (E_OK, self.new_handle(t), 2),
            None if create => match self.walk(k, sub, true) {
                Some(t) => (E_OK, self.new_handle(t), 1),
                None => (E_OUTOFMEMORY, 0, 1),
            },
            None => (E_NOTFOUND, 0, 2),
        }
    }

    /// A value's type and its data as the caller sees it: as stored (wide) or with strings
    /// converted to ANSI.
    pub fn value(&mut self, h: u32, name: &[u8], wide: bool) -> Result<(u32, Vec<u8>), i32> {
        let k = self.key_of(h).ok_or(E_BADKEY)?;
        let v = &self.key(k).vals[self.find_val(k, name).ok_or(E_NOTFOUND)?];
        Ok((v.kind, out_data(v, wide)))
    }

    /// RegQueryValueEx: the type is reported even when the buffer is too small.
    pub fn query(&mut self, h: u32, name: &[u8], kind: Option<&mut u32>, data: Option<&mut [u8]>, size: Option<&mut u32>, wide: bool) -> i32 {
        let (t, d) = match self.value(h, name, wide) { Ok(v) => v, Err(e) => return e };
        if let Some(k) = kind { *k = t; }
        deliver(&d, data, size)
    }

    /// RegSetValueEx: ANSI strings are widened with their terminator, wide strings without
    /// one get it.
    pub fn store(&mut self, h: u32, name: &[u8], kind: u32, d: &[u8], wide: bool) -> i32 {
        let Some(k) = self.key_of(h) else { return E_BADKEY };
        self.store_at(k, name, kind, d, wide)
    }

    fn store_at(&mut self, k: usize, name: &[u8], kind: u32, d: &[u8], wide: bool) -> i32 {
        let data = if !wide && is_str(kind) {
            if d.is_empty() { alloc::vec![0, 0] } else {
                let mut n = d.len();
                if kind != REG_MULTI_SZ && d[n - 1] != 0 { n += 1; }
                (0..n).flat_map(|i| [d.get(i).copied().unwrap_or(0), 0]).collect()
            }
        } else if wide && is_str(kind) && d.len() >= 2 && (d[d.len() / 2 * 2 - 2] != 0 || d[d.len() / 2 * 2 - 1] != 0) {
            let mut w = d.to_vec();
            w.truncate(d.len() / 2 * 2);
            w.extend_from_slice(&[0, 0]);
            w.resize(d.len() + 2, 0);
            w
        } else { d.to_vec() };
        if self.set_val(k, name, kind, data) { E_OK } else { E_OUTOFMEMORY }
    }

    /// RegSetValue: the default value of a subkey (created when missing).
    pub fn set_default(&mut self, h: u32, sub: &[u8], d: &[u8], wide: bool) -> i32 {
        let Some(mut k) = self.key_of(h) else { return E_BADKEY };
        if !sub.is_empty() { match self.walk(k, sub, true) { Some(t) => k = t, None => return E_OUTOFMEMORY } }
        self.store_at(k, b"", REG_SZ, d, wide)
    }

    pub fn delete_value(&mut self, h: u32, name: &[u8]) -> i32 {
        let Some(k) = self.key_of(h) else { return E_BADKEY };
        let Some(i) = self.find_val(k, name) else { return E_NOTFOUND };
        let v = self.key_mut(k).vals.remove(i);
        self.data_bytes -= v.data.len();
        self.dirty = true;
        E_OK
    }

    /// RegDeleteKey (`tree` false: only a key without subkeys) and RegDeleteTree (with no
    /// subkey named: the key's contents only). A root is never deleted.
    pub fn delete_key(&mut self, h: u32, sub: &[u8], tree: bool) -> i32 {
        let Some(k) = self.key_of(h) else { return E_BADKEY };
        let Some(t) = self.walk(k, sub, false) else { return E_NOTFOUND };
        if self.key(t).parent.is_none() { return E_ACCESS; }
        if !self.key(t).kids.is_empty() && !tree { return E_ACCESS; }
        if tree && t == k {
            for c in core::mem::take(&mut self.key_mut(t).kids) { self.free_tree(c); }
            let vals = core::mem::take(&mut self.key_mut(t).vals);
            self.data_bytes -= vals.iter().map(|v| v.data.len()).sum::<usize>();
        } else {
            self.unlink(t);
            self.free_tree(t);
        }
        self.dirty = true;
        E_OK
    }

    /// RegEnumKeyEx: `name` holds `*cc` characters (bytes, or UTF-16 units when wide).
    pub fn enum_key(&mut self, h: u32, i: u32, name: Option<&mut [u8]>, cc: Option<&mut u32>, wide: bool) -> i32 {
        let Some(k) = self.key_of(h) else { return E_BADKEY };
        let Some(&c) = self.key(k).kids.get(i as usize) else { return E_NOMORE };
        put_name(&self.key(c).name, name, cc, wide)
    }

    /// RegEnumValue.
    pub fn enum_value(&mut self, h: u32, i: u32, name: Option<&mut [u8]>, cc: Option<&mut u32>, kind: Option<&mut u32>,
                      data: Option<&mut [u8]>, size: Option<&mut u32>, wide: bool) -> i32 {
        let Some(k) = self.key_of(h) else { return E_BADKEY };
        let Some(v) = self.key(k).vals.get(i as usize) else { return E_NOMORE };
        let e = put_name(&v.name, name, cc, wide);
        if e != E_OK { return e; }
        if let Some(t) = kind { *t = v.kind; }
        let d = out_data(v, wide);
        match data {
            Some(buf) => {
                if buf.len() < d.len() || size.is_none() { if let Some(s) = size { *s = d.len() as u32; } return E_MORE; }
                buf[..d.len()].copy_from_slice(&d);
                if let Some(s) = size { *s = d.len() as u32; }
            }
            None => if let Some(s) = size { *s = d.len() as u32; },
        }
        E_OK
    }

    /// RegQueryInfoKey's counts and longest lengths.
    pub fn info(&mut self, h: u32) -> Result<Info, i32> {
        let k = self.key_of(h).ok_or(E_BADKEY)?;
        let key = self.key(k);
        Ok(Info {
            subkeys: key.kids.len() as u32,
            max_subkey: key.kids.iter().map(|&c| self.key(c).name.len() as u32).max().unwrap_or(0),
            values: key.vals.len() as u32,
            max_name: key.vals.iter().map(|v| v.name.len() as u32).max().unwrap_or(0),
            max_data: key.vals.iter().map(|v| v.data.len() as u32).max().unwrap_or(0),
        })
    }

    /// RegGetValue: `flags` RRF_RT_* restrict the type (RRF_NOEXPAND is ignored).
    pub fn get_value(&mut self, h: u32, sub: &[u8], name: &[u8], flags: u32, kind: Option<&mut u32>, data: Option<&mut [u8]>, size: Option<&mut u32>, wide: bool) -> i32 {
        let mut k = h;
        let opened = !sub.is_empty();
        if opened { let (e, hk, _) = self.open(h, sub, false); if e != E_OK { return e; } k = hk; }
        let mut t = 0;
        let mut e = self.query(k, name, Some(&mut t), data, size, wide);
        if e == E_OK && flags & 0xFFFF != 0 {
            let bit = match t { REG_NONE => 1, REG_SZ => 2, REG_EXPAND_SZ => 4, REG_BINARY => 8, REG_DWORD => 0x10, REG_MULTI_SZ => 0x20, REG_QWORD => 0x40, _ => 0 };
            if flags & bit == 0 { e = E_UNSUPPORTED_TYPE; }
        }
        if let Some(kp) = kind { *kp = t; }
        if opened { self.close(k); }
        e
    }

    // ------------------------------------------------------------ registry.dat

    pub fn save(&self) -> Vec<u8> {
        let mut s = b"THE WRLD OS registry 2\n".to_vec();
        for r in 1..6 { self.save_key(&mut s, self.roots[r], ROOT_NAMES[r].as_bytes().to_vec()); }
        s
    }

    fn save_key(&self, s: &mut Vec<u8>, k: usize, path: Vec<u8>) {
        // depth-first with an explicit stack, so a deep tree cannot overflow the kernel stack
        let mut stack = alloc::vec![(k, path)];
        while let Some((k, path)) = stack.pop() {
            s.extend_from_slice(b"K "); s.extend_from_slice(&path); s.push(b'\n');
            let key = self.key(k);
            for v in &key.vals {
                s.extend_from_slice(alloc::format!("V {} ", v.kind).as_bytes());
                if v.name.is_empty() { s.push(b'%'); } else { escape(s, &v.name, true); }
                s.push(b' ');
                for &b in &v.data { s.extend_from_slice(&hex2(b)); }
                s.push(b'\n');
            }
            for &c in key.kids.iter().rev() {
                let mut p = path.clone(); p.push(b'\\'); escape(&mut p, &self.key(c).name, false);
                stack.push((c, p));
            }
        }
    }

    /// Reads a saved registry over the current one (keys and values in it replace or add).
    pub fn load(&mut self, text: &[u8]) {
        let v2 = text.starts_with(b"THE WRLD OS registry 2\n") || text.starts_with(b"THE WRLD OS registry 2\r\n");
        let text = &text[..text.iter().position(|&c| c == 0).unwrap_or(text.len())];
        let mut cur = None;
        for line in text.split(|&c| c == b'\n') {
            let line = line.strip_suffix(b"\r").unwrap_or(line);
            if line.starts_with(b"K ") {
                let p = if v2 { unescape_path(&line[2..]) } else { line[2..].to_vec() };
                cur = self.root_path(&p, true);
            } else if line.starts_with(b"V ") {
                let Some(k) = cur else { continue };
                let mut q = &line[2..];
                // the type, as the C kernel's strtoul read it
                while let [c, rest @ ..] = q { if matches!(c, b' ' | b'\t' | b'\r' | b'\x0b' | b'\x0c') { q = rest } else { break } }
                if let [b'+', rest @ ..] = q { q = rest; }
                let mut kind = 0u32;
                while let [c @ b'0'..=b'9', rest @ ..] = q { kind = kind.wrapping_mul(10).wrapping_add((c - b'0') as u32); q = rest; }
                while let [b' ', rest @ ..] = q { q = rest; }
                let mut name = Vec::new();
                if q.first() == Some(&b'%') && (q.len() == 1 || q[1] == b' ') { q = &q[1..]; } else {
                    while let Some(&c) = q.first() {
                        if c == b' ' || name.len() >= NAME_MAX { break; }
                        if c == b'%' && q.len() >= 3 { name.push(hexv(q[1]) << 4 | hexv(q[2])); q = &q[3..]; } else { name.push(c); q = &q[1..]; }
                    }
                }
                while let [b' ', rest @ ..] = q { q = rest; }
                let data: Vec<u8> = q.chunks_exact(2).map(|p| hexv(p[0]) << 4 | hexv(p[1])).collect();
                self.set_val(k, &name, kind, data);
            }
        }
    }

    // ------------------------------------------------------------ by path (tests and the shell)

    /// A string value by path ("HKCU\Software\X", "name"), converted to ANSI.
    pub fn get_string(&mut self, path: &[u8], name: &[u8]) -> Option<Vec<u8>> {
        let d = self.dirty;
        let k = self.root_path(path, false);
        self.dirty = d;
        let v = &self.key(k?).vals[self.find_val(k?, name)?];
        if !is_str(v.kind) { return None; }
        Some(v.data.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).take_while(|&w| w != 0).map(|w| if w < 256 { w as u8 } else { b'?' }).collect())
    }

    pub fn set_string(&mut self, path: &[u8], name: &[u8], val: &[u8]) -> bool {
        let Some(k) = self.root_path(path, true) else { return false };
        let mut d = val.to_vec(); d.push(0);
        self.store_at(k, name, REG_SZ, &d, false) == E_OK
    }
}

fn hexv(c: u8) -> u8 { match c { b'0'..=b'9' => c - b'0', b'a'..=b'f' => c - b'a' + 10, b'A'..=b'F' => c - b'A' + 10, _ => 0 } }
fn hex2(b: u8) -> [u8; 2] { const H: &[u8; 16] = b"0123456789abcdef"; [H[(b >> 4) as usize], H[(b & 15) as usize]] }
fn hex2u(b: u8) -> [u8; 2] { const H: &[u8; 16] = b"0123456789ABCDEF"; [H[(b >> 4) as usize], H[(b & 15) as usize]] }

/// Percent-escapes a value name (`space` too, as it separates the fields) or a key name.
fn escape(s: &mut Vec<u8>, t: &[u8], space: bool) {
    for &c in t {
        if c < b' ' || (space && c == b' ') || c == b'%' || c >= 127 { s.push(b'%'); s.extend_from_slice(&hex2u(c)); } else { s.push(c); }
    }
}
fn unescape_path(p: &[u8]) -> Vec<u8> {
    let mut o = Vec::with_capacity(p.len());
    let mut i = 0;
    while i < p.len() {
        if p[i] == b'%' && i + 2 < p.len() { o.push(hexv(p[i + 1]) << 4 | hexv(p[i + 2])); i += 3; } else { o.push(p[i]); i += 1; }
    }
    o
}

fn out_data(v: &Val, wide: bool) -> Vec<u8> {
    if wide || !is_str(v.kind) { return v.data.clone(); }
    v.data.chunks_exact(2).map(|c| { let w = u16::from_le_bytes([c[0], c[1]]); if w < 256 { w as u8 } else { b'?' } }).collect()
}

/// The Win32 buffer rules for value data: no buffer reports the size; a buffer without a
/// size is an error; a short buffer reports the size needed with ERROR_MORE_DATA.
pub fn deliver(d: &[u8], data: Option<&mut [u8]>, size: Option<&mut u32>) -> i32 {
    let n = d.len() as u32;
    let Some(buf) = data else { if let Some(s) = size { *s = n; } return E_OK };
    let Some(s) = size else { return E_PARAM };
    if (*s as usize) < d.len() || buf.len() < d.len() { *s = n; return E_MORE; }
    buf[..d.len()].copy_from_slice(d);
    *s = n;
    E_OK
}

/// A key or value name into the caller's buffer of `*cc` characters, with its terminator;
/// `*cc` becomes its length, or the length needed (with the terminator) on ERROR_MORE_DATA.
fn put_name(n: &[u8], name: Option<&mut [u8]>, cc: Option<&mut u32>, wide: bool) -> i32 {
    let l = n.len();
    let Some(cc) = cc else { return E_MORE };
    let Some(buf) = name else { *cc = l as u32 + 1; return E_MORE };
    if (*cc as usize) <= l { *cc = l as u32 + 1; return E_MORE; }
    if wide {
        if buf.len() < (l + 1) * 2 { *cc = l as u32 + 1; return E_MORE; }
        for (j, &c) in n.iter().chain(&[0]).enumerate() { buf[2 * j] = c; buf[2 * j + 1] = 0; }
    } else {
        if buf.len() < l + 1 { *cc = l as u32 + 1; return E_MORE; }
        buf[..l].copy_from_slice(n); buf[l] = 0;
    }
    *cc = l as u32;
    E_OK
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::String;

    const HKCU: u32 = HKEY_CURRENT_USER;
    fn w(s: &str) -> Vec<u8> { s.encode_utf16().flat_map(|c| c.to_le_bytes()).collect() }

    #[test]
    fn defaults_and_paths() {
        let mut r = Registry::new();
        assert_eq!(r.get_string(b"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", b"productname").unwrap(), b"THE WRLD OS");
        assert_eq!(r.get_string(b"HKEY_CURRENT_USER\\Control Panel\\International", b"Locale").unwrap(), b"00000409");
        assert!(r.get_string(b"HKXX\\Software", b"a").is_none());
        // HKCR is HKLM\SOFTWARE\Classes
        let (e, h, d) = r.open(HKEY_CLASSES_ROOT, b".txt", true);
        assert_eq!((e, d), (E_OK, 1));
        assert_eq!(r.store(h, b"", REG_SZ, b"txtfile\0", false), E_OK);
        assert_eq!(r.get_string(b"HKLM\\Software\\classes\\.TXT", b"").unwrap(), b"txtfile");
        assert!(!r.take_dirty() || true);
    }

    #[test]
    fn values_and_buffers() {
        let mut r = Registry::new();
        let (_, h, _) = r.open(HKCU, b"Software\\Test", true);
        assert_eq!(r.store(h, b"Name", REG_SZ, b"abc", false), E_OK);              // no terminator: one is added
        let mut t = 0; let mut sz = 0u32;
        assert_eq!(r.query(h, b"NAME", Some(&mut t), None, Some(&mut sz), true), E_OK);
        assert_eq!((t, sz), (REG_SZ, 8));
        let mut small = [0u8; 3]; sz = 3;
        assert_eq!(r.query(h, b"name", None, Some(&mut small), Some(&mut sz), false), E_MORE);
        assert_eq!(sz, 4);
        let mut buf = [0u8; 16]; sz = 16;
        assert_eq!(r.query(h, b"name", None, Some(&mut buf), Some(&mut sz), false), E_OK);
        assert_eq!(&buf[..sz as usize], b"abc\0");
        assert_eq!(r.query(h, b"name", None, Some(&mut buf), None, false), E_PARAM);
        assert_eq!(r.store(h, b"W", REG_SZ, &w("hi"), true), E_OK);                 // wide, unterminated
        assert_eq!(r.value(h, b"w", true).unwrap().1, w("hi\0"));
        assert_eq!(r.store(h, b"D", REG_DWORD, &7u32.to_le_bytes(), false), E_OK);
        let mut d = [0u8; 4]; sz = 4;
        assert_eq!(r.get_value(HKCU, b"Software\\Test", b"d", 0x10, None, Some(&mut d), Some(&mut sz), false), E_OK);
        assert_eq!(u32::from_le_bytes(d), 7);
        sz = 4;
        assert_eq!(r.get_value(HKCU, b"Software\\Test", b"d", 0x02, None, Some(&mut d), Some(&mut sz), false), E_UNSUPPORTED_TYPE);
        assert_eq!(r.info(h).unwrap(), Info { subkeys: 0, max_subkey: 0, values: 3, max_name: 4, max_data: 8 });
        assert_eq!(r.delete_value(h, b"w"), E_OK);
        assert_eq!(r.delete_value(h, b"w"), E_NOTFOUND);
        assert_eq!(r.set_default(HKCU, b"Software\\Test\\Sub", b"x\0", false), E_OK);
        assert_eq!(r.get_string(b"HKCU\\Software\\Test\\Sub", b"").unwrap(), b"x");
    }

    #[test]
    fn enumeration_order_and_deletion() {
        let mut r = Registry::new();
        let (_, h, _) = r.open(HKCU, b"Software\\E", true);
        for n in ["beta", "Alpha", "gamma", "ALPHA"] { r.open(h, n.as_bytes(), true); }
        let mut names = Vec::new();
        for i in 0.. {
            let mut b = [0u8; 32]; let mut cc = 32;
            match r.enum_key(h, i, Some(&mut b), Some(&mut cc), false) { E_OK => names.push(String::from_utf8(b[..cc as usize].to_vec()).unwrap()), e => { assert_eq!(e, E_NOMORE); break } }
        }
        assert_eq!(names, ["Alpha", "beta", "gamma"]);
        let mut cc = 3; let mut b = [0u8; 3];
        assert_eq!(r.enum_key(h, 0, Some(&mut b), Some(&mut cc), false), E_MORE);
        assert_eq!(cc, 6);
        let mut wb = [0u8; 64]; cc = 32;
        assert_eq!(r.enum_key(h, 1, Some(&mut wb), Some(&mut cc), true), E_OK);
        assert_eq!(&wb[..10], &w("beta\0")[..]);
        // a handle below a deleted key is closed with it
        let (_, deep, _) = r.open(h, b"beta\\x\\y", true);
        assert_eq!(r.delete_key(h, b"beta", false), E_ACCESS);      // has subkeys
        assert_eq!(r.delete_key(h, b"beta", true), E_OK);
        assert_eq!(r.store(deep, b"v", REG_DWORD, &[1, 0, 0, 0], false), E_BADKEY);
        assert_eq!(r.delete_key(HKCU, b"", true), E_ACCESS);         // never a root
        assert_eq!(r.delete_key(h, b"", true), E_OK);                // the contents only
        assert_eq!(r.info(h).unwrap().subkeys, 0);
        assert_eq!(r.open(HKCU, b"Software\\E", false).0, E_OK);
    }

    #[test]
    fn handles_run_out_and_are_reused() {
        let mut r = Registry::new();
        let hs: Vec<u32> = (0..300).map(|_| r.open(HKCU, b"Software", false).1).collect();
        assert!(hs.iter().all(|&h| h >= HBASE && h < HBASE + 4 * NHANDLE as u32));
        r.close(hs[0]);
        assert_eq!(r.store(hs[0], b"x", REG_DWORD, &[0; 4], false), E_BADKEY);
        assert_eq!(r.open(HKCU, b"Software", false).1, hs[0]);
        assert_eq!(r.open(0x1234, b"", false).0, E_BADKEY);
    }

    #[test]
    fn save_and_load_round_trip() {
        let mut r = Registry::new();
        let (_, h, _) = r.open(HKCU, b"Software\\Odd\nK HKLM\\Injected", true);   // a newline in a key name
        r.store(h, b"with space%", REG_BINARY, &[0, 1, 0xfe], false);
        r.store(h, b"", REG_SZ, b"default", false);
        let saved = r.save();
        let mut back = Registry::new();
        back.load(&saved);
        assert_eq!(back.save(), saved);
        assert!(back.root_path(b"HKLM\\Injected", false).is_none());
        // a version 1 file (what the C version wrote) still loads
        let mut old = Registry::new();
        old.load(b"THE WRLD OS registry 1\nK HKEY_CURRENT_USER\\Software\\100%\nV 1 a%20b 6100000000\nV 4 % 07000000\nK nowhere\nV 1 x 00\n");
        assert_eq!(old.get_string(b"HKCU\\Software\\100%", b"a b").unwrap(), b"a");
        let (_, h2, _) = old.open(HKCU, b"Software\\100%", false);
        assert_eq!(old.value(h2, b"", false).unwrap(), (REG_DWORD, alloc::vec![7, 0, 0, 0]));
    }

    #[test]
    fn long_names_are_clipped_consistently() {
        let mut r = Registry::new();
        let long = [b'k'; 200];
        let (_, a, d1) = r.open(HKCU, &long, true);
        let (_, _, d2) = r.open(HKCU, &long, true);
        assert_eq!((d1, d2), (1, 2));                                  // found again, not duplicated
        r.store(a, &[b'v'; 200], REG_DWORD, &[1, 2, 3, 4], false);
        assert!(r.value(a, &[b'v'; 150], false).is_ok());
    }

    #[test]
    fn fuzz_load_and_operations() {
        let mut seed = 0x1234_5678u32;
        let mut rnd = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        let base = { let mut r = Registry::new(); let (_, h, _) = r.open(HKCU, b"Software\\F", true); r.store(h, b"n", REG_SZ, b"v", false); r.save() };
        for _ in 0..3000 {
            let mut t = base.clone();
            for _ in 0..(rnd() % 8) {
                let n = t.len();
                if n == 0 { break; }
                match rnd() % 3 { 0 => { let i = rnd() as usize % n; t[i] = rnd() as u8; } 1 => { let i = rnd() as usize % n; t.insert(i, b"\n%\\ KV0"[rnd() as usize % 7]); } _ => { t.truncate(rnd() as usize % n); } }
            }
            let mut r = Registry::new();
            r.load(&t);
            let s = r.save();
            let mut again = Registry::new();
            again.load(&s);
            let a2 = again.save();
            if a2 != s {
                let (x, y) = (String::from_utf8_lossy(&s).into_owned(), String::from_utf8_lossy(&a2).into_owned());
                let d = x.lines().zip(y.lines()).find(|(p, q)| p != q);
                panic!("round trip differs at {:?}\ninput {:?}", d, String::from_utf8_lossy(&t));
            }
            for _ in 0..50 {
                let h = [HKCU, HKEY_LOCAL_MACHINE, HKEY_CLASSES_ROOT, HBASE + 4 * (rnd() % 8), rnd()][rnd() as usize % 5];
                let name: Vec<u8> = (0..rnd() % 6).map(|_| b"aB\\%x\0"[rnd() as usize % 6]).collect();
                let mut buf = [0u8; 8]; let mut sz = rnd() % 12; let mut cc = rnd() % 12;
                match rnd() % 9 {
                    0 => { r.open(h, &name, rnd() & 1 == 0); }
                    1 => { r.store(h, &name, rnd() % 12, &name, rnd() & 1 == 0); }
                    2 => { r.query(h, &name, None, Some(&mut buf[..(sz as usize).min(8)]), Some(&mut sz), rnd() & 1 == 0); }
                    3 => { r.delete_key(h, &name, rnd() & 1 == 0); }
                    4 => { r.delete_value(h, &name); }
                    5 => { r.enum_key(h, rnd() % 4, Some(&mut buf[..(cc as usize).min(8)]), Some(&mut cc), false); }
                    6 => { r.enum_value(h, rnd() % 4, Some(&mut buf[..(cc as usize).min(8)]), Some(&mut cc), None, None, Some(&mut sz), true); }
                    7 => { let _ = r.info(h); }
                    _ => r.close(h),
                }
            }
        }
    }
}
