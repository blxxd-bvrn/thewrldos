//! Installed-system persistence, the parts that are plain logic (was `src/kernel/persist.c`):
//! the settings store behind `settings_get`/`settings_set`, reading and writing
//! `C:\WRLD\wrld.ini`, the account password digest, the paths the system disk is written
//! with, and the one-off changes for systems installed by older versions. The write-back
//! engine itself is `kernel::persistk`.
//!
//! Strings are byte slices standing for C strings, cut at their first NUL; lengths and
//! truncation follow the C code (keys of at most 23 bytes, values of 63, 64 entries).

use alloc::vec::Vec;

pub const MAX_SETTINGS: usize = 64;
pub const KEY_CAP: usize = 24;
pub const VAL_CAP: usize = 64;
/// All 64 maximum-length key/value pairs fit.
pub const TEXT_CAP: usize = 6144;

fn cstr(s: &[u8]) -> &[u8] { match s.iter().position(|&c| c == 0) { Some(i) => &s[..i], None => s } }
/// strlcpy into a fixed buffer
fn put(dst: &mut [u8], s: &[u8]) { let s = cstr(s); let n = s.len().min(dst.len() - 1); dst[..n].copy_from_slice(&s[..n]); dst[n..].fill(0); }

/// The settings: key=value pairs, keys compared without case. The buffers do not move, so
/// `settings_get` can hand C a pointer into them.
pub struct Settings { pub kv: [([u8; KEY_CAP], [u8; VAL_CAP]); MAX_SETTINGS], pub n: usize }
impl Settings {
    pub const fn new() -> Self { Settings { kv: [([0; KEY_CAP], [0; VAL_CAP]); MAX_SETTINGS], n: 0 } }
    pub fn find(&self, key: &[u8]) -> Option<usize> {
        let key = cstr(key);
        (0..self.n).find(|&i| cstr(&self.kv[i].0).eq_ignore_ascii_case(key))
    }
    pub fn get(&self, key: &[u8]) -> Option<&[u8]> { self.find(key).map(|i| cstr(&self.kv[i].1)) }
    /// Replace a value, or add the pair while there is room (a full store drops new keys).
    pub fn set(&mut self, key: &[u8], val: &[u8]) {
        if let Some(i) = self.find(key) { put(&mut self.kv[i].1, val); return; }
        if self.n < MAX_SETTINGS { let i = self.n; put(&mut self.kv[i].0, key); put(&mut self.kv[i].1, val); self.n += 1; }
    }
    /// The lines of a wrld.ini: `key=value`, with `;` comments and `[section]` lines ignored.
    pub fn load_ini(&mut self, text: &[u8]) {
        for line in cstr(text).split(|&c| c == b'\n') {
            let line = match line.iter().position(|&c| c == b'\r') { Some(i) => &line[..i], None => line };
            let Some(eq) = line.iter().position(|&c| c == b'=') else { continue };
            if line[0] == b';' || line[0] == b'[' { continue; }
            self.set(&line[..eq], &line[eq + 1..]);
        }
    }
    /// The wrld.ini text for the settings.
    pub fn to_ini(&self) -> Vec<u8> {
        let mut t = b"; THE WRLD OS settings\r\n[WRLD]\r\n".to_vec();
        for i in 0..self.n {
            if t.len() >= TEXT_CAP - 100 { break; }
            t.extend_from_slice(cstr(&self.kv[i].0)); t.push(b'=');
            t.extend_from_slice(cstr(&self.kv[i].1)); t.extend_from_slice(b"\r\n");
        }
        t
    }
}

/// The account password as stored: a salted FNV-1a digest in 16 hex digits ("" for none).
/// It is not a real password KDF, but it keeps the plain password out of wrld.ini.
pub fn password_digest(plain: &[u8]) -> [u8; 16] {
    fn fnv1a(s: &[u8], mut h: u64) -> u64 { for &c in cstr(s) { h ^= c as u64; h = h.wrapping_mul(1099511628211); } h }
    let h = fnv1a(plain, fnv1a(b"THE WRLD OS account", 1469598103934665603));
    let mut out = [0u8; 16];
    for i in 0..16 { out[i] = b"0123456789abcdef"[((h >> (60 - 4 * i)) & 15) as usize]; }
    out
}

/// A file's path on the system volume: its VFS path (at most 511 bytes) without the drive.
pub fn rel_path(full: &[u8]) -> &[u8] {
    let full = &full[..full.len().min(511)];
    if full.len() >= 2 && full[1] == b':' { &full[2..] } else { full }
}
/// Was `old` (a VFS path before a rename) on drive C:?
pub fn on_c(old: &[u8]) -> bool { old.len() >= 2 && old[0].eq_ignore_ascii_case(&b'c') && old[1] == b':' }

/// "Windows Classic.wrldstyle" from before the system folder was renamed, with its Name=
/// line changed to the new name.
pub fn renamed_style(st: &[u8]) -> Vec<u8> {
    let mut nb = Vec::with_capacity(st.len() + 16);
    let mut k = 0;
    while k < st.len() {
        if k + 20 <= st.len() && &st[k..k + 20] == b"Name=Windows Classic" { nb.extend_from_slice(b"Name=WRLD Classic"); k += 20; }
        else { nb.push(st[k]); k += 1; }
    }
    nb
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn store() {
        let mut s = Settings::new();
        s.set(b"Theme", b"Luna");
        s.set(b"THEME", b"Royale");
        assert_eq!(s.get(b"theme"), Some(&b"Royale"[..]));
        assert_eq!(s.n, 1);
        let long = [b'k'; 30];
        s.set(&long, &[b'v'; 100]);
        assert_eq!(s.get(&long[..23]).map(|v| v.len()), Some(63));
        s.set(&long, b"x");                         // the stored key is cut to 23 bytes: no match, a new pair
        assert_eq!(s.n, 3);
        for i in 0..100 { s.set(alloc::format!("k{}", i).as_bytes(), b"1"); }
        assert_eq!(s.n, MAX_SETTINGS);
    }
    #[test]
    fn ini() {
        let mut s = Settings::new();
        s.load_ini(b"; c\r\n[WRLD]\r\nuser=Ann\r\n=empty\nbad\nvol=5\r junk\na=b=c\n;x=y\x00tail=1");
        assert_eq!(s.get(b"user"), Some(&b"Ann"[..]));
        assert_eq!(s.get(b""), Some(&b"empty"[..]));
        assert_eq!(s.get(b"vol"), Some(&b"5"[..]));
        assert_eq!(s.get(b"a"), Some(&b"b=c"[..]));
        assert_eq!(s.get(b"tail"), None);
        let t = s.to_ini();
        assert_eq!(t, b"; THE WRLD OS settings\r\n[WRLD]\r\nuser=Ann\r\n=empty\r\nvol=5\r\na=b=c\r\n");
        let mut s2 = Settings::new(); s2.load_ini(&t);
        assert_eq!(s2.to_ini(), t);
        let mut big = Settings::new();
        for i in 0..64 { big.set(alloc::format!("{:023}", i).as_bytes(), &[b'v'; 63]); }
        assert!(big.to_ini().len() < TEXT_CAP);
    }
    #[test]
    fn other() {
        // the C code's digest for these (fnv1a, salted)
        assert_eq!(&password_digest(b"hunter2"), b"d258a6cfaae45f00");
        assert_eq!(&password_digest(b""), b"6f7e63aa16ee8784");
        assert_eq!(&password_digest("p\u{e4}ss w0rd!".as_bytes()), b"b7ce4e11d3caaad9");
        assert_ne!(password_digest(b""), password_digest(b"a"));
        assert_eq!(rel_path(b"C:\\WRLD\\wrld.ini"), b"\\WRLD\\wrld.ini");
        assert_eq!(rel_path(b"x"), b"x");
        assert!(on_c(b"c:\\a") && !on_c(b"D:\\a") && !on_c(b"C"));
        assert_eq!(renamed_style(b"[x]\nName=Windows Classic\nName=Windows"), b"[x]\nName=WRLD Classic\nName=Windows");
    }
}
