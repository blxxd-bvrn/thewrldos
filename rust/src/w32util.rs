//! The pure parts of the smaller Win32 API files (the functions themselves are in
//! `kernel/w32*.rs`): GUID strings, SAFEARRAY layout, VARIANT conversions, OLE dates and
//! text sniffing, kept here so that `cargo test` checks them on the build machine.

/// A GUID in its in-memory form (Data1, Data2, Data3 little-endian, then Data4).
pub type Guid = [u8; 16];

const CO_E_CLASSSTRING: i32 = 0x8004_01F3u32 as i32;

fn hexv(c: u16) -> Option<u64> {
    match c { 0x30..=0x39 => Some((c - 0x30) as u64), 0x61..=0x66 => Some((c - 0x61 + 10) as u64), 0x41..=0x46 => Some((c - 0x41 + 10) as u64), _ => None }
}

/// CLSIDFromString's parsing: "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (the separators and
/// the closing brace are not checked, as in the C version). `s` stops at its NUL.
pub fn guid_parse(s: &[u16]) -> Result<Guid, i32> {
    if s.first() != Some(&(b'{' as u16)) { return Err(CO_E_CLASSSTRING); }
    const POS: [usize; 11] = [1, 10, 15, 20, 22, 25, 27, 29, 31, 33, 35];
    const LEN: [usize; 11] = [8, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2];
    let mut v = [0u64; 11];
    for k in 0..11 {
        for i in 0..LEN[k] {
            let h = s.get(POS[k] + i).copied().and_then(hexv).ok_or(CO_E_CLASSSTRING)?;
            v[k] = v[k] << 4 | h;
        }
    }
    let mut g = [0u8; 16];
    g[0..4].copy_from_slice(&(v[0] as u32).to_le_bytes());
    g[4..6].copy_from_slice(&(v[1] as u16).to_le_bytes());
    g[6..8].copy_from_slice(&(v[2] as u16).to_le_bytes());
    for i in 0..8 { g[8 + i] = v[3 + i] as u8; }
    Ok(g)
}

/// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" in capitals (38 characters).
pub fn guid_text(g: &Guid) -> [u8; 38] {
    const H: &[u8; 16] = b"0123456789ABCDEF";
    let mut o = [0u8; 38];
    let mut k = 0;
    let mut put = |b: u8, o: &mut [u8; 38]| { o[k] = b; k += 1; };
    put(b'{', &mut o);
    let d1 = u32::from_le_bytes([g[0], g[1], g[2], g[3]]);
    let d2 = u16::from_le_bytes([g[4], g[5]]);
    let d3 = u16::from_le_bytes([g[6], g[7]]);
    for s in (0..8).rev() { put(H[(d1 >> (s * 4) & 15) as usize], &mut o); }
    put(b'-', &mut o);
    for s in (0..4).rev() { put(H[(d2 >> (s * 4) & 15) as usize], &mut o); }
    put(b'-', &mut o);
    for s in (0..4).rev() { put(H[(d3 >> (s * 4) & 15) as usize], &mut o); }
    put(b'-', &mut o);
    for (i, &b) in g[8..].iter().enumerate() {
        if i == 2 { put(b'-', &mut o); }
        put(H[(b >> 4) as usize], &mut o); put(H[(b & 15) as usize], &mut o);
    }
    put(b'}', &mut o);
    o
}

/// The element size of a SAFEARRAY of VARTYPE `vt` (0 for types not supported), for
/// pointers of `ptr` bytes.
pub fn vt_size(vt: u16, ptr: usize) -> usize {
    match vt {
        2 | 11 | 18 => 2,
        3 | 4 | 10 | 19 | 22 | 23 => 4,
        5 | 6 | 7 | 20 | 21 => 8,
        8 | 9 | 13 => ptr,
        12 => if ptr == 8 { 24 } else { 16 },
        16 | 17 => 1,
        _ => 0,
    }
}

/// The element number of `idx` in a SAFEARRAY whose bounds are `b` as stored (the last
/// dimension first): `idx[i]` goes with `b[dims - 1 - i]`. None when out of bounds.
pub fn sa_index(b: &[(u32, i32)], idx: &[i32]) -> Option<usize> {
    let dims = b.len();
    let (mut o, mut mul) = (0usize, 1usize);
    for i in 0..dims {
        let (n, lb) = b[dims - 1 - i];
        let k = idx[i].wrapping_sub(lb);
        if k < 0 || k as u32 >= n { return None; }
        o = o.wrapping_add((k as usize).wrapping_mul(mul));
        mul = mul.wrapping_mul(n as usize);
    }
    Some(o)
}

/// VariantChangeType between the integer and boolean types it supports: `src` is the
/// source VARIANT's first 16 bytes; the result is the new VARIANT's, or DISP_E_TYPEMISMATCH.
pub fn variant_change(src: &[u8; 16], vt: u16) -> Result<[u8; 16], i32> {
    const MISMATCH: i32 = 0x8002_0005u32 as i32;
    let v = &src[8..16];
    let n: i64 = match u16::from_le_bytes([src[0], src[1]]) {
        2 => i16::from_le_bytes([v[0], v[1]]) as i64,
        3 => i32::from_le_bytes([v[0], v[1], v[2], v[3]]) as i64,
        11 => (i16::from_le_bytes([v[0], v[1]]) != 0) as i64,
        17 => v[0] as i64,
        19 => u32::from_le_bytes([v[0], v[1], v[2], v[3]]) as i64,
        _ => return Err(MISMATCH),
    };
    let mut d = [0u8; 16];
    d[0..2].copy_from_slice(&vt.to_le_bytes());
    match vt {
        2 => d[8..10].copy_from_slice(&(n as i16).to_le_bytes()),
        3 | 22 => d[8..12].copy_from_slice(&(n as i32).to_le_bytes()),
        11 => d[8..10].copy_from_slice(&(if n != 0 { -1i16 } else { 0 }).to_le_bytes()),
        19 | 23 => d[8..12].copy_from_slice(&(n as u32).to_le_bytes()),
        5 => d[8..16].copy_from_slice(&(n as f64).to_le_bytes()),
        _ => return Err(MISMATCH),
    }
    Ok(d)
}

/// SystemTimeToVariantTime: an OLE date (days since 30 December 1899) from a SYSTEMTIME
/// (year, month, weekday, day, hour, minute, second, ms).
pub fn ole_date(st: &[u16; 8]) -> f64 {
    let (y, m, d) = (st[0] as i64, st[1] as i64, st[3] as i64);
    let yy = y - (m <= 2) as i64;
    let era = if yy >= 0 { yy } else { yy - 399 } / 400;
    let yoe = yy - era * 400;
    let doy = (153 * (m + if m > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    (days + 25_569) as f64 + (st[4] as i64 * 3600 + st[5] as i64 * 60 + st[6] as i64) as f64 / 86400.0
}

/// IsTextUnicode's guess: (is UTF-16, the IS_TEXT_UNICODE_* result flags).
pub fn text_is_unicode(b: &[u8]) -> (bool, i32) {
    if b.len() >= 2 && b[0] == 0xFF && b[1] == 0xFE { return (true, 8); }
    let n = b.len() as i32;
    let (mut odd, mut even) = (0i32, 0i32);
    for p in b.chunks_exact(2) { if p[1] == 0 { odd += 1; } if p[0] == 0 { even += 1; } }
    let u = n >= 4 && odd > n / 4 && even < n / 16;
    (u, if u { 2 } else { 0 })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn w(s: &str) -> alloc::vec::Vec<u16> { s.encode_utf16().collect() }

    #[test]
    fn guids() {
        let s = "{6B29FC40-CA47-1067-B31D-00DD010662DA}";
        let g = guid_parse(&w(s)).unwrap();
        assert_eq!(&g[..8], &[0x40, 0xFC, 0x29, 0x6B, 0x47, 0xCA, 0x67, 0x10]);
        assert_eq!(&guid_text(&g), s.as_bytes());
        assert_eq!(guid_text(&guid_parse(&w("{6b29fc40-ca47-1067-b31d-00dd010662da}")).unwrap()), *s.as_bytes().first_chunk::<38>().unwrap());
        for bad in ["", "6B29FC40", "{6B29FC40", "{6B29FC40-CA47-1067-B31D-00DD010662DX}"] { assert_eq!(guid_parse(&w(bad)), Err(CO_E_CLASSSTRING)); }
    }

    #[test]
    fn safearrays_and_variants() {
        assert_eq!((vt_size(12, 8), vt_size(12, 4), vt_size(8, 4), vt_size(99, 8)), (24, 16, 4, 0));
        // a 3 x 4 array with lower bounds 1 and 0, stored last dimension first
        let b = [(4u32, 0i32), (3, 1)];
        assert_eq!(sa_index(&b, &[1, 0]), Some(0));
        assert_eq!(sa_index(&b, &[3, 0]), Some(2));
        assert_eq!(sa_index(&b, &[1, 1]), Some(3));
        assert_eq!(sa_index(&b, &[0, 0]), None);
        assert_eq!(sa_index(&b, &[1, 4]), None);
        let mut v = [0u8; 16]; v[0] = 3; v[8..12].copy_from_slice(&(-5i32).to_le_bytes());
        let d = variant_change(&v, 5).unwrap();
        assert_eq!(f64::from_le_bytes(d[8..16].try_into().unwrap()), -5.0);
        assert_eq!(&variant_change(&v, 11).unwrap()[8..10], &[0xFF, 0xFF]);
        v[0] = 8;
        assert!(variant_change(&v, 3).is_err());
    }

    #[test]
    fn dates_and_text() {
        assert_eq!(ole_date(&[2000, 1, 6, 1, 0, 0, 0, 0]), 36526.0);
        assert_eq!(ole_date(&[1899, 12, 6, 30, 12, 0, 0, 0]), 0.5);
        assert_eq!(ole_date(&[1600, 3, 0, 1, 0, 0, 0, 0]), -109511.0);
        assert_eq!(text_is_unicode(&[0xFF, 0xFE, b'a', 0]), (true, 8));
        assert_eq!(text_is_unicode(b"h\0e\0l\0l\0o\0 \0w\0o\0r\0l\0d\0"), (true, 2));
        assert_eq!(text_is_unicode(b"h\0e\0l\0l\0o\0"), (false, 0));       // under 16 bytes never (the C formula)
        assert_eq!(text_is_unicode(b"hello world"), (false, 0));
    }
}

// ================================================================ kernel32 (k32more)

/// The C kernel's strtoul: whitespace, '+', "0x" for base 0/16, a leading 0 for octal with
/// base 0, digits while they are below the base (wrapping).
pub fn kstrtoul(s: &[u8], base: u32) -> u64 {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) { i += 1; }
    if s.get(i) == Some(&b'+') { i += 1; }
    let mut base = base;
    if (base == 0 || base == 16) && s.get(i) == Some(&b'0') && matches!(s.get(i + 1), Some(b'x') | Some(b'X')) { i += 2; base = 16; }
    else if base == 0 && s.get(i) == Some(&b'0') && s.get(i + 1).is_some_and(|&c| c != 0) { base = 8; }
    else if base == 0 { base = 10; }
    let mut v: u64 = 0;
    while let Some(&c) = s.get(i) {
        let d = match c { b'0'..=b'9' => c - b'0', b'a'..=b'z' => c - b'a' + 10, b'A'..=b'Z' => c - b'A' + 10, _ => break } as u32;
        if d >= base { break; }
        v = v.wrapping_mul(base as u64).wrapping_add(d as u64);
        i += 1;
    }
    v
}

/// The C kernel's strtol (whitespace, '-', then strtoul: '+', "0x" for base 0/16, a leading
/// 0 for octal with base 0), wrapping like its unsigned long; callers truncate.
pub fn kstrtol(s: &[u8], base: u32) -> i64 {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) { i += 1; }
    let neg = s.get(i) == Some(&b'-');
    let v = kstrtoul(&s[i + neg as usize..], base) as i64;
    if neg { v.wrapping_neg() } else { v }
}

/// One line of an INI file: the section it is in (its header's line number and name), the
/// key and value when it is "key=value", and the line itself (without its line break).
#[derive(Clone, Copy, Debug)]
pub struct IniLine<'a> { pub sec: Option<(usize, &'a [u8])>, pub key: Option<&'a [u8]>, pub val: &'a [u8], pub line: &'a [u8] }

fn trim_end(mut s: &[u8]) -> &[u8] { while let [r @ .., b' ' | b'\t'] = s { s = r; } s }
fn trim_start(mut s: &[u8]) -> &[u8] { while let [b' ' | b'\t', r @ ..] = s { s = r; } s }
fn name_eq(a: &[u8], b: &[u8]) -> bool { a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.to_ascii_lowercase() == y.to_ascii_lowercase()) }

/// The lines of an INI file, as the C version read them: "[section]" (to its ']'),
/// "key = value" (spaces around both trimmed, one pair of quotes removed), comments
/// (';' or '#') and anything else kept as plain lines.
pub fn ini_lines(d: &[u8]) -> alloc::vec::Vec<IniLine<'_>> {
    let mut out = alloc::vec::Vec::new();
    let mut sec: Option<(usize, &[u8])> = None;
    let mut i = 0;
    while i < d.len() {
        let e = d[i..].iter().position(|&c| c == b'\n').map(|p| i + p).unwrap_or(d.len());
        let mut s = &d[i..e];
        if let [r @ .., b'\r'] = s { s = r; }
        let t = trim_start(s);
        let mut l = IniLine { sec, key: None, val: &[], line: s };
        if t.first() == Some(&b'[') {
            let name = match t.iter().position(|&c| c == b']') { Some(c) => &t[1..c], None => &t[1..] };
            sec = Some((out.len(), name));
            l.sec = sec;
        } else if !t.is_empty() && t[0] != b';' && t[0] != b'#' {
            if let Some(eq) = t.iter().position(|&c| c == b'=') {
                let k = trim_end(&t[..eq]);
                let mut v = trim_end(trim_start(&t[eq + 1..]));
                if v.len() >= 2 && ((v[0] == b'"' && v[v.len() - 1] == b'"') || (v[0] == b'\'' && v[v.len() - 1] == b'\'')) { v = &v[1..v.len() - 1]; }
                l.key = Some(k); l.val = v;
            }
        }
        out.push(l);
        i = e + 1;
    }
    out
}
/// Whether a line is in section `sec` (lines before any header count as the section "").
fn in_sec(l: &IniLine, sec: &[u8]) -> bool { match l.sec { Some((_, n)) => name_eq(n, sec), None => sec.is_empty() } }

/// GetPrivateProfileString on a file's text: with no section, the section names; with no
/// key, the section's keys (each NUL-terminated, then a NUL); else the value or the default
/// (trailing spaces removed). Writes into `buf` (its size is the caller's) and returns the
/// count the C version returned.
pub fn ini_get(text: &[u8], sec: Option<&[u8]>, key: Option<&[u8]>, def: Option<&[u8]>, buf: &mut [u8]) -> usize {
    let size = buf.len();
    if size == 0 { return 0; }
    let lines = ini_lines(text);
    let list = |names: &mut dyn Iterator<Item = &[u8]>, buf: &mut [u8]| {
        let mut o = 0;
        for n in names { if o + n.len() + 2 > size { break; } buf[o..o + n.len()].copy_from_slice(n); o += n.len(); buf[o] = 0; o += 1; }
        buf[if o < size { o } else { size - 1 }] = 0;
        o
    };
    let Some(sec) = sec else {
        let mut it = lines.iter().enumerate().filter(|(i, l)| l.key.is_none() && l.sec.is_some_and(|(h, _)| h == *i)).map(|(_, l)| l.sec.unwrap().1);
        return list(&mut it, buf);
    };
    let Some(key) = key else {
        let mut it = lines.iter().filter(|l| l.key.is_some() && in_sec(l, sec)).map(|l| l.key.unwrap());
        return list(&mut it, buf);
    };
    if let Some(l) = lines.iter().find(|l| l.key.is_some_and(|k| name_eq(k, key)) && in_sec(l, sec)) {
        let n = l.val.len().min(size - 1);
        buf[..n].copy_from_slice(&l.val[..n]); buf[n] = 0;
        return n;
    }
    let d = def.unwrap_or(b"");
    let d = { let mut d = d; while let [r @ .., b' '] = d { d = r; } d };
    let n = d.len().min(size - 1);
    buf[..n].copy_from_slice(&d[..n]); buf[n] = 0;
    n
}

/// WritePrivateProfileString on a file's text: the new text. No key deletes the section;
/// no value deletes the key; otherwise the key is replaced or added at the end of its
/// section, or the section is added at the end of the file. Lines end with CR LF.
pub fn ini_put(text: &[u8], sec: &[u8], key: Option<&[u8]>, val: Option<&[u8]>) -> alloc::vec::Vec<u8> {
    let lines = ini_lines(text);
    let mut out = alloc::vec::Vec::with_capacity(text.len() + 64);
    let sec_found = lines.iter().any(|l| in_sec(l, sec));
    let last_in_sec = lines.iter().rposition(|l| in_sec(l, sec) && !l.line.is_empty());
    let mut done = false;
    let kv = |out: &mut alloc::vec::Vec<u8>, k: &[u8], v: &[u8]| { out.extend_from_slice(k); out.push(b'='); out.extend_from_slice(v); out.extend_from_slice(b"\r\n"); };
    for (i, l) in lines.iter().enumerate() {
        let here = in_sec(l, sec);
        if here && key.is_none() { continue; }
        if here && l.key.is_some_and(|k| name_eq(k, key.unwrap())) {
            if let Some(v) = val { kv(&mut out, key.unwrap(), v); }
            done = true;
            continue;
        }
        out.extend_from_slice(l.line); out.extend_from_slice(b"\r\n");
        if Some(i) == last_in_sec && !done { if let (Some(k), Some(v)) = (key, val) { kv(&mut out, k, v); done = true; } }
    }
    if !sec_found { if let (Some(k), Some(v)) = (key, val) { out.push(b'['); out.extend_from_slice(sec); out.extend_from_slice(b"]\r\n"); kv(&mut out, k, v); } }
    out
}

/// GetPrivateProfileSection: "key=value\0...\0\0"; returns the count before the last NUL.
pub fn ini_section(text: &[u8], sec: Option<&[u8]>, buf: &mut [u8]) -> usize {
    let size = buf.len();
    if size < 2 { return 0; }
    let mut o = 0;
    if let Some(sec) = sec {
        for l in ini_lines(text).iter().filter(|l| l.key.is_some() && in_sec(l, sec)) {
            let k = l.key.unwrap();
            if o + k.len() + 1 + l.val.len() + 1 + 1 > size { break; }
            buf[o..o + k.len()].copy_from_slice(k); o += k.len(); buf[o] = b'='; o += 1;
            buf[o..o + l.val.len()].copy_from_slice(l.val); o += l.val.len(); buf[o] = 0; o += 1;
        }
    }
    buf[o] = 0;
    o
}

/// ExpandEnvironmentStrings: %NAME% replaced by `get(NAME)` where it is set (names cut at
/// 127 bytes), the result cut at 4095 bytes.
pub fn expand_env(s: &[u8], get: impl Fn(&[u8]) -> Option<alloc::vec::Vec<u8>>) -> alloc::vec::Vec<u8> {
    let mut out = alloc::vec::Vec::new();
    let mut i = 0;
    while i < s.len() && out.len() < 4095 {
        if s[i] == b'%' {
            if let Some(e) = s[i + 1..].iter().position(|&c| c == b'%').map(|p| i + 1 + p) {
                if e > i + 1 {
                    let name = &s[i + 1..e.min(i + 1 + 127)];
                    if let Some(v) = get(name) {
                        for &c in &v { if out.len() >= 4095 { break; } out.push(c); }
                        i = e + 1;
                        continue;
                    }
                }
            }
        }
        out.push(s[i]);
        i += 1;
    }
    out
}

/// VerSetConditionMask.
pub fn ver_cond_mask(mut mask: u64, ty: u32, cond: u8) -> u64 {
    for b in 0..8 { if ty & (1 << b) != 0 { mask |= ((cond & 7) as u64) << (b * 3); } }
    mask
}
fn cmp_cond(have: i64, want: i64, cond: u64) -> bool {
    match cond { 1 => have == want, 2 => have > want, 3 => have >= want, 4 => have < want, 5 => have <= want, _ => true }
}
/// VerifyVersionInfo against Windows 7 SP1 (6.1.7601, service pack 1.0, NT platform).
pub fn verify_version(major: u32, minor: u32, build: u32, platform: u32, spmaj: u16, spmin: u16, ty: u32, mask: u64) -> bool {
    let (mj, mn, bd, sm, sn) = (6i64, 1i64, 7601i64, 1i64, 0i64);
    let mut ok = true;
    if ty & 2 != 0 {
        let c = (mask >> 3) & 7;
        let mut have = (mj << 32) | (mn << 16);
        let mut want = ((major as i64) << 32) | (((if ty & 1 != 0 { minor } else { 0 }) as i64) << 16);
        if ty & 0x20 != 0 { have |= sm << 8; want |= (spmaj as i64) << 8; }
        if ty & 0x10 != 0 { have |= sn; want |= spmin as i64; }
        if ty & 1 == 0 { have &= !(0xFFFFi64 << 16); want &= !(0xFFFFi64 << 16); }
        ok = cmp_cond(have, want, c);
    } else {
        if ty & 1 != 0 { ok = ok && cmp_cond(mn, minor as i64, mask & 7); }
        if ty & 0x20 != 0 { ok = ok && cmp_cond(sm, spmaj as i64, (mask >> 15) & 7); }
        if ty & 0x10 != 0 { ok = ok && cmp_cond(sn, spmin as i64, (mask >> 12) & 7); }
    }
    if ty & 4 != 0 { ok = ok && cmp_cond(bd, build as i64, (mask >> 6) & 7); }
    if ty & 8 != 0 { ok = ok && platform == 2; }
    ok
}

/// GetStringTypeEx's CT_CTYPE1 flags for a byte (code page 1252); other types give 0.
pub fn ctype1(c: u8) -> u16 {
    let mut v = 0;
    if c.is_ascii_uppercase() { v |= 1; }
    if c.is_ascii_lowercase() { v |= 2; }
    if c.is_ascii_digit() { v |= 4; }
    if c == b' ' || (9..=13).contains(&c) { v |= 8; }
    if c > 32 && c < 127 && !c.is_ascii_alphanumeric() { v |= 0x10; }
    if c < 32 || c == 127 { v |= 0x20; }
    if c == b' ' || c == b'\t' { v |= 0x40; }
    if c.is_ascii_hexdigit() { v |= 0x80; }
    if c.is_ascii_alphabetic() || c >= 0xC0 { v |= 0x100; }
    v
}

/// A console key code (a character, or 0x100 | 'A'..'H' for arrows and editing keys) as a
/// virtual-key code and the character it types.
pub fn console_vk(c: i32) -> (u16, u16) {
    if (32..127).contains(&c) {
        let b = c as u8;
        return (if b.is_ascii_alphabetic() { b.to_ascii_uppercase() as u16 } else if b.is_ascii_digit() { b as u16 } else if b == b' ' { 0x20 } else { 0 }, c as u16);
    }
    match c {
        10 | 13 => return (0x0D, 13),
        8 | 127 => return (0x08, 8),
        9 => return (0x09, 9),
        27 => return (0x1B, 27),
        _ => {}
    }
    if c >= 0x100 {
        let vk = match (c & 0xFF) as u8 { b'A' => 0x26, b'B' => 0x28, b'C' => 0x27, b'D' => 0x25, b'H' => 0x24, b'F' => 0x23, _ => 0 };
        return (vk, 0);
    }
    (0, 0)
}

/// lstrcmpW / lstrcmpiW (case folded for ASCII only): -1, 0 or 1.
pub fn wcmp(a: &[u16], b: &[u16], ci: bool) -> i32 {
    let f = |c: u16| if ci && c < 128 { (c as u8).to_ascii_lowercase() as u16 } else { c };
    let mut i = 0;
    loop {
        let (x, y) = (f(a.get(i).copied().unwrap_or(0)), f(b.get(i).copied().unwrap_or(0)));
        if x != y || x == 0 { return if x < y { -1 } else if x > y { 1 } else { 0 }; }
        i += 1;
    }
}

/// The global atom table: strings with reference counts, numbered from 0xC000; "#123" and
/// numbers below 0x10000 are integer atoms.
pub struct Atoms { names: alloc::vec::Vec<Option<(alloc::vec::Vec<u8>, i32)>> }
impl Default for Atoms { fn default() -> Self { Self::new() } }
impl Atoms {
    pub const MAX: usize = 512;
    pub const fn new() -> Self { Atoms { names: alloc::vec::Vec::new() } }
    pub fn find(&self, n: &[u8]) -> u16 {
        if n.first() == Some(&b'#') { return kstrtol(&n[1..], 10) as u16; }
        for (i, a) in self.names.iter().enumerate() { if let Some((s, _)) = a { if name_eq(s, n) { return 0xC000 + i as u16; } } }
        0
    }
    pub fn add(&mut self, n: &[u8]) -> u16 {
        let a = self.find(n);
        if a >= 0xC000 { if let Some((_, r)) = &mut self.names[(a - 0xC000) as usize] { *r += 1; } return a; }
        if a != 0 { return a; }
        let slot = match self.names.iter().position(|a| a.is_none()) {
            Some(i) => i,
            None if self.names.len() < Self::MAX => { self.names.push(None); self.names.len() - 1 }
            None => return 0,
        };
        self.names[slot] = Some((n.to_vec(), 1));
        0xC000 + slot as u16
    }
    pub fn delete(&mut self, a: u16) {
        if a < 0xC000 { return; }
        let i = (a - 0xC000) as usize;
        if let Some(Some((_, r))) = self.names.get_mut(i) { *r -= 1; if *r <= 0 { self.names[i] = None; } }
    }
    /// The name of an atom: "#123" for integer atoms, None for unknown ones.
    pub fn name(&self, a: u16) -> Option<alloc::vec::Vec<u8>> {
        if a < 0xC000 { return Some(alloc::format!("#{a}").into_bytes()); }
        self.names.get((a - 0xC000) as usize)?.as_ref().map(|(s, _)| s.clone())
    }
}

#[cfg(test)]
mod k32_tests {
    use super::*;
    const INI: &[u8] = b"; comment\r\n[Settings]\r\nName = \"Quoted value\"  \r\nCount=42\r\n\r\n[Other]\nkey='x'\n[settings]\nlate=1\n";

    #[test]
    fn ini_reading() {
        let mut b = [0xAAu8; 64];
        assert_eq!(ini_get(INI, Some(b"SETTINGS"), Some(b"name"), None, &mut b), 12);
        assert_eq!(&b[..13], b"Quoted value\0");
        assert_eq!(ini_get(INI, Some(b"settings"), Some(b"late"), None, &mut b), 1);
        assert_eq!(ini_get(INI, Some(b"Other"), Some(b"nope"), Some(b"def  "), &mut b), 3);
        assert_eq!(&b[..4], b"def\0");
        let n = ini_get(INI, None, None, None, &mut b);
        assert_eq!(&b[..n + 1], b"Settings\0Other\0settings\0\0");
        let n = ini_get(INI, Some(b"Settings"), None, None, &mut b);
        assert_eq!(&b[..n + 1], b"Name\0Count\0late\0\0");
        let mut small = [0u8; 4];
        assert_eq!(ini_get(INI, Some(b"Settings"), Some(b"Name"), None, &mut small), 3);
        assert_eq!(&small, b"Quo\0");
        let n = ini_section(INI, Some(b"other"), &mut b);
        assert_eq!(&b[..n + 1], b"key=x\0\0");
        assert_eq!(kstrtol(b" 0x1F", 0), 31);
        assert_eq!(kstrtol(b"-010", 0), -8);
        assert_eq!(kstrtol(b"42abc", 0), 42);
    }

    #[test]
    fn ini_writing() {
        let t = ini_put(INI, b"Other", Some(b"key"), Some(b"new"));
        assert!(t.windows(9).any(|w| w == b"key=new\r\n"));
        let t = ini_put(&t, b"Other", Some(b"added"), Some(b"1"));
        let mut b = [0u8; 32];
        assert_eq!(ini_get(&t, Some(b"other"), Some(b"added"), None, &mut b), 1);
        let t = ini_put(&t, b"Settings", None, None);                        // delete both [Settings] and [settings]
        assert_eq!(ini_get(&t, Some(b"Settings"), Some(b"Count"), Some(b"gone"), &mut b), 4);
        let t = ini_put(&t, b"New", Some(b"a"), Some(b"b"));
        assert!(t.ends_with(b"[New]\r\na=b\r\n"));
        let t = ini_put(&t, b"New", Some(b"a"), None);
        assert_eq!(ini_get(&t, Some(b"New"), Some(b"a"), None, &mut b), 0);
        assert_eq!(ini_put(b"", b"S", Some(b"k"), Some(b"v")), b"[S]\r\nk=v\r\n");
    }

    #[test]
    fn env_version_atoms_and_text() {
        let get = |n: &[u8]| if name_eq(n, b"windir") { Some(b"C:\\WRLD".to_vec()) } else { None };
        assert_eq!(expand_env(b"%WINDIR%\\System32 %NOPE% 100%", get), b"C:\\WRLD\\System32 %NOPE% 100%");
        let m = ver_cond_mask(ver_cond_mask(0, 2, 3), 1, 3);
        assert!(verify_version(6, 1, 0, 0, 0, 0, 3, m));
        assert!(verify_version(5, 1, 0, 0, 0, 0, 3, m));
        assert!(!verify_version(6, 2, 0, 0, 0, 0, 3, m));
        assert!(!verify_version(10, 0, 0, 0, 0, 0, 3, m));
        let mut a = Atoms::new();
        let x = a.add(b"MyClass");
        assert_eq!(x, 0xC000);
        assert_eq!(a.add(b"myclass"), x);
        assert_eq!(a.find(b"#123"), 123);
        a.delete(x);
        assert_eq!(a.find(b"MYCLASS"), x);
        a.delete(x);
        assert_eq!(a.find(b"MYCLASS"), 0);
        assert_eq!(a.name(5).unwrap(), b"#5");
        assert_eq!(ctype1(b'a'), 2 | 0x80 | 0x100);
        assert_eq!(console_vk(0x141), (0x26, 0));
        assert_eq!(console_vk(b'q' as i32), (b'Q' as u16, b'q' as u16));
        assert_eq!(wcmp(&[b'A' as u16], &[b'a' as u16], true), 0);
        assert_eq!(wcmp(&[b'A' as u16], &[b'a' as u16], false), -1);
    }
}

// ================================================================ shlwapi and shell32 (shell.c)

pub const MAXP: usize = 260;
pub fn is_sep(c: u16) -> bool { c == b'\\' as u16 || c == b'/' as u16 }
/// shlwapi's case folding: ASCII only.
pub fn wlow(c: u16) -> u16 { if c < 128 { (c as u8).to_ascii_lowercase() as u16 } else { c } }
fn at(s: &[u16], i: usize) -> u16 { s.get(i).copied().unwrap_or(0) }

/// PathFindFileName: where the last component starts.
pub fn find_file_name(p: &[u16]) -> usize {
    let mut r = 0;
    for i in 0..p.len() { if (is_sep(p[i]) || p[i] == b':' as u16) && i + 1 < p.len() { r = i + 1; } }
    r
}
/// PathFindExtension: where the extension's dot is, or the end.
pub fn find_extension(p: &[u16]) -> usize {
    let mut d = None;
    for (i, &c) in p.iter().enumerate() { if is_sep(c) || c == b' ' as u16 { d = None; } else if c == b'.' as u16 { d = Some(i); } }
    d.unwrap_or(p.len())
}
pub fn is_relative(p: &[u16]) -> bool { !(is_sep(at(p, 0)) || (at(p, 0) != 0 && at(p, 1) == b':' as u16)) }
pub fn is_root(p: &[u16]) -> bool {
    let c = |i| at(p, i);
    if is_sep(c(0)) && c(1) == 0 { return true; }
    if c(0) != 0 && c(1) == b':' as u16 && is_sep(c(2)) && c(3) == 0 { return true; }
    if is_sep(c(0)) && is_sep(c(1)) { return p[2..].iter().filter(|&&c| is_sep(c)).count() <= 1; }
    false
}

/// PathCanonicalize: "." and ".." resolved.
pub fn canonicalize(inp: &[u16]) -> alloc::vec::Vec<u16> {
    if inp.is_empty() { return alloc::vec![b'\\' as u16]; }
    let bs = b'\\' as u16;
    let mut t: alloc::vec::Vec<u16> = alloc::vec::Vec::new();
    let mut root = 0;
    if at(inp, 0) != 0 && at(inp, 1) == b':' as u16 {
        t.push(inp[0]); t.push(b':' as u16); root = 2;
        if is_sep(at(inp, 2)) { t.push(bs); root = 3; }
    } else if is_sep(inp[0]) {
        t.push(bs); root = 1;
        if is_sep(at(inp, 1)) { t.push(bs); root = 2; }
    }
    let mut s = root.min(inp.len());
    while s < inp.len() {
        let mut e = s; while e < inp.len() && !is_sep(inp[e]) { e += 1; }
        let comp = &inp[s..e];
        if comp == [b'.' as u16] {
        } else if comp == [b'.' as u16, b'.' as u16] {
            if t.len() > root {
                t.pop();
                while t.len() > root && t[t.len() - 1] != bs { t.pop(); }
                if t.len() > root { t.pop(); }
            }
        } else {
            if t.len() > root && t[t.len() - 1] != bs { t.push(bs); }
            for &c in comp { if t.len() >= MAXP - 2 { break; } t.push(c); }
        }
        s = if e < inp.len() { e + 1 } else { e };
    }
    if t.len() > root && t.len() < MAXP - 1 && inp[inp.len() - 1] == bs && t[t.len() - 1] != bs { t.push(bs); }
    if t.is_empty() { t.push(bs); }
    t
}
/// PathCombine: None when the result would not fit in MAX_PATH.
pub fn combine(dir: Option<&[u16]>, file: Option<&[u16]>) -> Option<alloc::vec::Vec<u16>> {
    if dir.is_none() && file.is_none() { return None; }
    let mut t: alloc::vec::Vec<u16> = alloc::vec::Vec::new();
    let cap = MAXP * 2;
    match (dir, file) {
        (d, Some(f)) if !is_relative(f) || d.is_none_or(|d| d.is_empty()) => { for &c in f { if t.len() >= cap - 1 { break; } t.push(c); } }
        (d, f) => {
            for &c in d.unwrap_or(&[]) { if t.len() >= cap - 2 { break; } t.push(c); }
            if let Some(f) = f.filter(|f| !f.is_empty()) {
                if !t.is_empty() && !is_sep(t[t.len() - 1]) { t.push(b'\\' as u16); }
                for &c in f { if t.len() >= cap - 1 { break; } t.push(c); }
            }
        }
    }
    if t.len() >= MAXP { return None; }
    Some(canonicalize(&t))
}
/// PathCommonPrefix: the length of the common leading path.
pub fn common_prefix(a: &[u16], b: &[u16]) -> usize {
    let (mut last, mut i) = (0, 0);
    while at(a, i) != 0 && at(b, i) != 0 && wlow(a[i]) == wlow(b[i]) { if is_sep(a[i]) { last = i; } i += 1; }
    if at(a, i) == 0 && (at(b, i) == 0 || is_sep(at(b, i))) { last = i; } else if at(b, i) == 0 && is_sep(at(a, i)) { last = i; }
    if last == 2 && at(a, 1) == b':' as u16 { last = 3; }
    last
}
/// PathRemoveFileSpec on a copy: the new path and whether it changed.
pub fn remove_file_spec(p: &[u16]) -> (alloc::vec::Vec<u16>, bool) {
    let mut v = p.to_vec();
    let f = find_file_name(p);
    if f == 0 {
        if at(p, 0) != 0 && is_sep(p[0]) && at(p, 1) != 0 { v.truncate(1); return (v, true); }
        if !is_sep(at(p, 0)) && at(p, 0) != 0 { v.clear(); return (v, true); }
        return (v, false);
    }
    let mut e = f;
    if e > 0 && is_sep(p[e - 1]) { e -= 1; }
    if e == 0 || (e == 2 && at(p, 1) == b':' as u16) { e += 1; }
    let ch = e < p.len();
    v.truncate(e);
    (v, ch)
}
/// PathRelativePathTo; `from_dir` when `from` is a folder.
pub fn relative_path(from: &[u16], from_dir: bool, to: &[u16]) -> Option<alloc::vec::Vec<u16>> {
    let mut f = from[..from.len().min(MAXP - 1)].to_vec();
    if !from_dir { f = remove_file_spec(&f).0; }
    let c = common_prefix(&f, to);
    if c == 0 { return None; }
    let mut out = alloc::vec::Vec::new();
    let mut r = c; if r < f.len() && is_sep(f[r]) { r += 1; }
    if r < f.len() {
        out.extend_from_slice(&[b'.' as u16, b'.' as u16]);
        for &ch in &f[r..] { if is_sep(ch) && out.len() + 3 < MAXP { out.extend_from_slice(&[b'\\' as u16, b'.' as u16, b'.' as u16]); } }
    } else { out.push(b'.' as u16); }
    let mut t = c; if t < to.len() && is_sep(to[t]) { t += 1; }
    if t < to.len() { out.push(b'\\' as u16); for &ch in &to[t..] { if out.len() >= MAXP - 1 { break; } out.push(ch); } }
    Some(out)
}
/// A wildcard match ('*', '?'), case folded for ASCII.
pub fn wild(pat: &[u16], s: &[u16]) -> bool {
    let (mut p, mut i) = (0, 0);
    while p < pat.len() {
        if pat[p] == b'*' as u16 {
            while p + 1 < pat.len() && pat[p + 1] == b'*' as u16 { p += 1; }
            if p + 1 >= pat.len() { return true; }
            for k in i..=s.len() { if wild(&pat[p + 1..], &s[k..]) { return true; } }
            return false;
        }
        if i >= s.len() || (pat[p] != b'?' as u16 && wlow(pat[p]) != wlow(s[i])) { return false; }
        p += 1; i += 1;
    }
    i >= s.len()
}
/// PathMatchSpec: `spec` is patterns separated by ';'.
pub fn match_spec(file: &[u16], spec: &[u16]) -> bool {
    let name = &file[find_file_name(file)..];
    let mut i = 0;
    while i < spec.len() {
        while i < spec.len() && (spec[i] == b' ' as u16 || spec[i] == b';' as u16) { i += 1; }
        let st = i;
        while i < spec.len() && spec[i] != b';' as u16 && i - st < MAXP - 1 { i += 1; }
        let mut one = &spec[st..i];
        while let [r @ .., c] = one { if *c == b' ' as u16 { one = r; } else { break; } }
        if !one.is_empty() && (wild(one, name) || one == [b'*' as u16, b'.' as u16, b'*' as u16]) { return true; }
    }
    false
}
/// PathCompactPathEx into `n` characters (with the NUL): the text and the result.
pub fn compact_path(p: &[u16], n: usize) -> (alloc::vec::Vec<u16>, bool) {
    if p.len() < n { return (p.to_vec(), true); }
    if n < 5 { return (alloc::vec::Vec::new(), false); }
    let name = &p[find_file_name(p)..];
    let dot = b'.' as u16;
    if name.len() + 4 >= n { let mut v = name[..n - 4].to_vec(); v.extend_from_slice(&[dot, dot, dot]); return (v, true); }
    let keep = n - 1 - name.len() - 4;
    let mut v = p[..keep].to_vec();
    v.extend_from_slice(&[dot, dot, dot, b'\\' as u16]);
    v.extend_from_slice(name);
    (v, true)
}
/// StrCmpLogicalW: digit runs compare as numbers.
pub fn cmp_logical(a: &[u16], b: &[u16]) -> i32 {
    let digit = |c: u16| (b'0' as u16..=b'9' as u16).contains(&c);
    let (mut i, mut j) = (0, 0);
    while i < a.len() && j < b.len() {
        if digit(a[i]) && digit(b[j]) {
            let (mut x, mut y) = (0u64, 0u64);
            while i < a.len() && digit(a[i]) { x = x.wrapping_mul(10).wrapping_add((a[i] - b'0' as u16) as u64); i += 1; }
            while j < b.len() && digit(b[j]) { y = y.wrapping_mul(10).wrapping_add((b[j] - b'0' as u16) as u64); j += 1; }
            if x != y { return if x < y { -1 } else { 1 }; }
            continue;
        }
        let (x, y) = (wlow(a[i]), wlow(b[j]));
        if x != y { return if x < y { -1 } else { 1 }; }
        i += 1; j += 1;
    }
    if i < a.len() { 1 } else if j < b.len() { -1 } else { 0 }
}
/// shlwapi's compare of at most `n` characters (all for None), -1/0/1.
pub fn wcmpn(a: &[u16], b: &[u16], n: Option<usize>, ci: bool) -> i32 {
    let mut i = 0;
    while n.is_none_or(|n| i < n) {
        let (x, y) = (at(a, i), at(b, i));
        let (x, y) = if ci { (wlow(x), wlow(y)) } else { (x, y) };
        if x != y { return if x < y { -1 } else { 1 }; }
        if x == 0 { return 0; }
        i += 1;
    }
    0
}
/// StrToIntEx: the value and whether there were digits (`hex`: STIF_SUPPORT_HEX).
pub fn str_to_int_ex(s: &[u16], hex: bool) -> (i32, bool) {
    let mut i = 0;
    while at(s, i) == b' ' as u16 || at(s, i) == b'\t' as u16 { i += 1; }
    let mut sg = 1i32;
    if at(s, i) == b'-' as u16 { sg = -1; i += 1; } else if at(s, i) == b'+' as u16 { i += 1; }
    let (mut v, mut any) = (0i32, false);
    if hex && at(s, i) == b'0' as u16 && (at(s, i + 1) == b'x' as u16 || at(s, i + 1) == b'X' as u16) {
        i += 2;
        while at(s, i) < 128 && (at(s, i) as u8).is_ascii_hexdigit() {
            let c = at(s, i) as u8;
            v = v.wrapping_mul(16).wrapping_add(if c.is_ascii_digit() { c - b'0' } else { c.to_ascii_lowercase() - b'a' + 10 } as i32);
            any = true; i += 1;
        }
    } else {
        while (b'0' as u16..=b'9' as u16).contains(&at(s, i)) { v = v.wrapping_mul(10).wrapping_add((at(s, i) - b'0' as u16) as i32); any = true; i += 1; }
    }
    (v.wrapping_mul(sg), any)
}
/// StrFormatByteSize's text.
pub fn fmt_bytes(n: i64) -> alloc::string::String {
    if n < 1024 { return alloc::format!("{} bytes", n as i32); }
    const U: [&str; 4] = ["KB", "MB", "GB", "TB"];
    let mut v = n as f64 / 1024.0;
    let mut i = 0;
    while v >= 1024.0 && i < 3 { v /= 1024.0; i += 1; }
    let w = v as i32;
    let f = ((v - w as f64) * 100.0) as i32;
    if v < 10.0 { alloc::format!("{}.{:02} {}", w, f, U[i]) } else if v < 100.0 { alloc::format!("{}.{} {}", w, f / 10, U[i]) } else { alloc::format!("{} {}", w, U[i]) }
}
/// UrlEscape: (the escaped text, cut at 2040 characters as in C).
pub fn url_escape(s: &[u16], segment_only: bool) -> alloc::vec::Vec<u16> {
    const KEEP: &[u8] = b"-_.~!*'();:@&=+$,/?#[]";
    const H: &[u8; 16] = b"0123456789ABCDEF";
    let mut t = alloc::vec::Vec::new();
    for &c in s {
        if t.len() >= 2040 { break; }
        let mut keep = c < 128 && ((c as u8).is_ascii_alphanumeric() || KEEP.contains(&(c as u8)));
        if segment_only { keep = keep && c != b'#' as u16 && c != b'?' as u16; }
        if keep { t.push(c); } else { t.extend_from_slice(&[b'%' as u16, H[((c >> 4) & 15) as usize] as u16, H[(c & 15) as usize] as u16]); }
    }
    t
}
fn hexw(c: u16) -> Option<u16> { match c { 0x30..=0x39 => Some(c - 0x30), 0x61..=0x66 => Some(c - 0x61 + 10), 0x41..=0x46 => Some(c - 0x41 + 10), _ => None } }
/// UrlUnescape (at most 2047 characters).
pub fn url_unescape(s: &[u16]) -> alloc::vec::Vec<u16> {
    let mut t = alloc::vec::Vec::new();
    let mut i = 0;
    while i < s.len() && t.len() < 2047 {
        if s[i] == b'%' as u16 { if let (Some(a), Some(b)) = (hexw(at(s, i + 1)), hexw(at(s, i + 2))) { t.push(a << 4 | b); i += 3; continue; } }
        t.push(s[i]); i += 1;
    }
    t
}
/// ColorHLSToRGB (hue, lightness, saturation 0..240) as a COLORREF.
pub fn hls_to_rgb(h: u16, l: u16, s: u16) -> u32 {
    let (h, l, s) = (h as i32, l as i32, s as i32);
    if s == 0 { let v = (l * 255 / 240) as u32; return v | v << 8 | v << 16; }
    let m2 = if l <= 120 { (l * (240 + s) + 120) / 240 } else { l + s - (l * s + 120) / 240 };
    let m1 = 2 * l - m2;
    let mut c = [0u32; 3];
    for (i, hh) in [h + 80, h, h - 80].into_iter().enumerate() {
        let mut x = hh; if x < 0 { x += 240; } if x > 240 { x -= 240; }
        let v = if x < 40 { m1 + ((m2 - m1) * x + 20) / 40 } else if x < 120 { m2 } else if x < 160 { m1 + ((m2 - m1) * (160 - x) + 20) / 40 } else { m1 };
        c[i] = ((v * 255 + 120) / 240) as u32;
    }
    c[0] | c[1] << 8 | c[2] << 16
}
/// ColorRGBToHLS.
pub fn rgb_to_hls(rgb: u32) -> (u16, u16, u16) {
    let (r, g, b) = ((rgb & 255) as i32, ((rgb >> 8) & 255) as i32, ((rgb >> 16) & 255) as i32);
    let (mx, mn) = (r.max(g).max(b), r.min(g).min(b));
    let l = ((mx + mn) * 240 + 255) / 510;
    if mx == mn { return (160, l as u16, 0); }
    let d = mx - mn;
    let s = if l <= 120 { (d * 240 + (mx + mn) / 2) / (mx + mn) } else { (d * 240 + (510 - mx - mn) / 2) / (510 - mx - mn) };
    let mut h = if r == mx { (40 * (g - b)) / d } else if g == mx { 80 + (40 * (b - r)) / d } else { 160 + (40 * (r - g)) / d };
    if h < 0 { h += 240; }
    (h as u16, l as u16, s as u16)
}
/// CommandLineToArgvW's split: the program name (quotes only delimit), then the arguments
/// with the backslash and quote rules. An empty line gives one argument, `exe`.
pub fn cmdline_to_argv(cmd: &[u16], exe: &[u8]) -> alloc::vec::Vec<alloc::vec::Vec<u16>> {
    let (sp, tab, q, bs) = (b' ' as u16, b'\t' as u16, b'"' as u16, b'\\' as u16);
    if cmd.is_empty() { return alloc::vec![exe.iter().map(|&c| c as u16).collect()]; }
    let mut out = alloc::vec::Vec::new();
    let mut i = 0;
    let mut first = true;
    while i < cmd.len() {
        while i < cmd.len() && (cmd[i] == sp || cmd[i] == tab) { i += 1; }
        if i >= cmd.len() { break; }
        let mut a = alloc::vec::Vec::new();
        let mut inq = false;
        if first {
            while i < cmd.len() && (inq || (cmd[i] != sp && cmd[i] != tab)) { if cmd[i] == q { inq = !inq; } else { a.push(cmd[i]); } i += 1; }
        } else {
            while i < cmd.len() && (inq || (cmd[i] != sp && cmd[i] != tab)) {
                if cmd[i] == bs {
                    let mut n = 0; while i < cmd.len() && cmd[i] == bs { n += 1; i += 1; }
                    if at(cmd, i) == q { for _ in 0..n / 2 { a.push(bs); } if n & 1 == 1 { a.push(q); i += 1; } } else { for _ in 0..n { a.push(bs); } }
                    continue;
                }
                if cmd[i] == q { if inq && at(cmd, i + 1) == q { a.push(q); i += 2; continue; } inq = !inq; i += 1; continue; }
                a.push(cmd[i]); i += 1;
            }
        }
        out.push(a);
        first = false;
    }
    out
}

/// The CSIDL folders (with the user's folders under `home`).
pub fn csidl_path(c: i32, home: &str) -> Option<alloc::string::String> {
    use alloc::format;
    let docs = format!("{home}\\My Documents");
    Some(match c & 0xFF {
        0x00 | 0x10 | 0x19 => format!("{home}\\Desktop"),
        0x05 | 0x2E => docs,
        0x0D => format!("{docs}\\My Music"),
        0x27 => format!("{docs}\\My Pictures"),
        0x0E => format!("{docs}\\My Videos"),
        0x1A => format!("{home}\\Application Data"),
        0x1C => format!("{home}\\Local Settings\\Application Data"),
        0x23 => "C:\\Documents and Settings\\All Users\\Application Data".into(),
        0x28 => home.into(),
        0x24 => "C:\\WRLD".into(),
        0x25 | 0x29 => "C:\\WRLD\\System32".into(),
        0x26 | 0x2A => "C:\\Program Files".into(),
        0x2B | 0x2C => "C:\\Program Files\\Common Files".into(),
        0x14 => "C:\\WRLD\\Fonts".into(),
        0x06 => format!("{home}\\Favorites"),
        0x07 | 0x18 => format!("{home}\\Start Menu\\Programs\\Startup"),
        0x08 => format!("{home}\\Recent"),
        0x09 => format!("{home}\\SendTo"),
        0x0B | 0x16 => format!("{home}\\Start Menu"),
        0x02 | 0x17 => format!("{home}\\Start Menu\\Programs"),
        0x15 => format!("{home}\\Templates"),
        0x20 => format!("{home}\\Local Settings\\Temporary Internet Files"),
        0x21 => format!("{home}\\Cookies"),
        0x22 => format!("{home}\\Local Settings\\History"),
        0x2F => "C:\\Documents and Settings\\All Users\\Start Menu\\Programs\\Administrative Tools".into(),
        _ => return None,
    })
}
/// The CSIDL of a known folder, by the first 32 bits of its GUID.
pub fn known_folder_csidl(d1: u32) -> Option<i32> {
    Some(match d1 {
        0x3EB685DB => 0x1A, 0xF1B32785 | 0xA520A1A4 => 0x1C, 0xFDD39AD0 => 0x05, 0xB4BFCC3A => 0x10,
        0x905E63B6 | 0x7C5A40EF | 0x6D809377 => 0x26, 0x5E6C858F => 0x28, 0xF38BF404 => 0x24, 0x1AC14E77 | 0xD65231B0 => 0x25,
        0x62AB5D82 => 0x23, 0xFD228CB7 => 0x14, 0x4BD8D571 => 0x0D, 0x33E28130 => 0x27, 0x18989B1D => 0x0E,
        0x1777F761 => 0x06, 0xAE50C081 => 0x08, 0x625B53C3 => 0x0B, 0xA77F5D77 => 0x02, 0xB97D20BB => 0x07,
        0xA63293E8 => 0x15, 0x374DE290 => 0x05,
        _ => return None,
    })
}
/// SHGetFileInfo's type name.
pub fn file_type_name(path: &[u8], dir: bool) -> alloc::vec::Vec<u8> {
    let base = &path[path.iter().rposition(|&c| c == b'\\' || c == b'/').map_or(0, |p| p + 1)..];
    let ext = base.iter().rposition(|&c| c == b'.').map(|p| &base[p..]);
    if dir { return b"File Folder".to_vec(); }
    match ext {
        Some(e) if e.eq_ignore_ascii_case(b".exe") => b"Application".to_vec(),
        Some(e) if e.len() > 1 => { let mut t: alloc::vec::Vec<u8> = e[1..].iter().take(15).map(|c| c.to_ascii_uppercase()).collect(); t.extend_from_slice(b" File"); t }
        _ => b"File".to_vec(),
    }
}

fn r16(d: &[u8], o: usize) -> Option<u32> { d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as u32) }
fn r32(d: &[u8], o: usize) -> Option<u32> { d.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])) }
/// Where a PE file (of either architecture) keeps its first RT_VERSION resource: (file
/// offset, length). Every read is checked against the file.
pub fn version_resource(d: &[u8]) -> Option<(usize, usize)> {
    if d.len() < 0x100 || &d[..2] != b"MZ" { return None; }
    let pe = r32(d, 0x3C)? as usize;
    if d.get(pe..pe + 4)? != b"PE\0\0" { return None; }
    let nsec = r16(d, pe + 6)? as usize;
    let osz = r16(d, pe + 20)? as usize;
    let opt = pe + 24;
    let is64 = r16(d, opt)? == 0x20B;
    let dd = opt + if is64 { 112 } else { 96 };
    let rva = r32(d, dd + 16)?;
    if rva == 0 { return None; }
    let sec = opt + osz;
    let off = |r: u32| -> Option<usize> {
        for i in 0..nsec {
            let s = sec + i * 40;
            let (va, vs, raw) = (r32(d, s + 12)?, r32(d, s + 8)?.max(r32(d, s + 16)?), r32(d, s + 20)?);
            if r >= va && (r as u64) < va as u64 + vs as u64 { let o = (r - va) as u64 + raw as u64; return if o == 0 { None } else { Some(o as usize) }; }
        }
        None
    };
    let root = off(rva)?;
    let mut dir = root;
    for lvl in 0..3 {
        let cnt = (r16(d, dir + 12)? + r16(d, dir + 14)?) as usize;
        let mut hit = None;
        for i in 0..cnt {
            let e = dir + 16 + 8 * i;
            if lvl > 0 || r32(d, e)? == 16 { hit = Some((r32(d, e)?, r32(d, e + 4)?)); break; }
        }
        let (_, to) = hit?;
        if lvl < 2 {
            if to & 0x8000_0000 == 0 { return None; }
            dir = root.checked_add((to & 0x7FFF_FFFF) as usize)?;
        } else {
            if to & 0x8000_0000 != 0 { return None; }
            let de = root.checked_add(to as usize)?;
            let (drva, dlen) = (r32(d, de)?, r32(d, de + 4)? as usize);
            let o = off(drva)?;
            if o.checked_add(dlen)? > d.len() { return None; }
            return Some((o, dlen));
        }
    }
    None
}
/// VerQueryValue's walk of a VS_VERSIONINFO block (`blk`, its length taken from its first
/// WORD) along `path` ("\\StringFileInfo\\040904b0\\FileVersion"): (value offset, value
/// length in the block's units: characters for text), whether it is text.
pub fn ver_find(blk: &[u8], path: &[u16]) -> Option<(usize, usize, bool)> {
    let align = |x: usize| (x + 3) & !3;
    let wl = |o: usize| -> usize { let mut n = 0; while r16(blk, o + 2 * n).is_some_and(|c| c != 0) { n += 1; } n };
    let mut node = 0usize;
    let mut avail = r16(blk, 0)? as usize;
    let mut p = 0;
    loop {
        let (len, vl, ty) = (r16(blk, node)? as usize, r16(blk, node + 2)? as usize, r16(blk, node + 4)?);
        if len < 6 || len > avail { return None; }
        let kl = wl(node + 6);
        let v = align(node + 6 + 2 * (kl + 1));
        let vbytes = if ty == 1 { vl * 2 } else { vl };
        while at(path, p) == b'\\' as u16 { p += 1; }
        if p >= path.len() { return Some((v, vl, ty == 1)); }
        let mut e = p; while e < path.len() && path[e] != b'\\' as u16 { e += 1; }
        let comp = &path[p..e];
        let end = node + len;
        let mut child = align(v + vbytes);
        let mut found = false;
        while child + 6 <= end {
            let cl = r16(blk, child)? as usize;
            if cl < 6 { break; }
            let ck = child + 6;
            let kl = wl(ck);
            let key: alloc::vec::Vec<u16> = (0..kl).map(|i| r16(blk, ck + 2 * i).unwrap_or(0) as u16).collect();
            if kl == comp.len() && wcmpn(&key, comp, Some(comp.len()), true) == 0 { node = child; avail = cl; p = e; found = true; break; }
            child = align(child + cl);
        }
        if !found { return None; }
    }
}

#[cfg(test)]
mod shell_tests {
    use super::*;
    fn w(s: &str) -> alloc::vec::Vec<u16> { s.encode_utf16().collect() }
    fn s(v: &[u16]) -> alloc::string::String { alloc::string::String::from_utf16(v).unwrap() }

    #[test]
    fn paths() {
        assert_eq!(s(&canonicalize(&w("C:\\a\\.\\b\\..\\c\\"))), "C:\\a\\c\\");
        assert_eq!(s(&canonicalize(&w("\\\\server\\x\\..\\y"))), "\\\\server\\y");
        assert_eq!(s(&canonicalize(&w("a\\..\\..\\b"))), "b");
        assert_eq!(s(&combine(Some(&w("C:\\dir")), Some(&w("..\\f.txt"))).unwrap()), "C:\\f.txt");
        assert_eq!(s(&combine(Some(&w("C:\\dir")), Some(&w("D:\\abs"))).unwrap()), "D:\\abs");
        assert_eq!(common_prefix(&w("C:\\a\\b"), &w("c:\\A\\c")), 4);
        assert_eq!(common_prefix(&w("C:\\x"), &w("C:\\y")), 3);
        assert_eq!(s(&relative_path(&w("C:\\a\\b\\f.txt"), false, &w("C:\\a\\c\\g.txt")).unwrap()), "..\\c\\g.txt");
        assert!(match_spec(&w("C:\\dir\\Photo.JPG"), &w("*.txt; *.jpg")));
        assert!(!match_spec(&w("C:\\dir\\Photo.png"), &w("*.txt;*.jpg")));
        assert!(wild(&w("a?c*"), &w("ABCdef")));
        assert_eq!(s(&compact_path(&w("C:\\Program Files\\Some Folder\\file.txt"), 20).0), "C:\\Prog...\\file.txt");
        assert_eq!(s(&remove_file_spec(&w("C:\\dir\\f.txt")).0), "C:\\dir");
        assert_eq!(s(&remove_file_spec(&w("C:\\f.txt")).0), "C:\\");
        assert_eq!(find_extension(&w("a.b\\c d.txt")), 7);
        assert!(is_root(&w("C:\\")) && is_root(&w("\\\\srv\\share")) && !is_root(&w("C:\\x")));
    }

    #[test]
    fn strings_and_more() {
        assert_eq!(cmp_logical(&w("file9"), &w("file10")), -1);
        assert_eq!(str_to_int_ex(&w("  -0x1f"), true), (-31, true));
        assert_eq!(str_to_int_ex(&w("12ab"), false), (12, true));
        assert_eq!(fmt_bytes(532), "532 bytes");
        assert_eq!(fmt_bytes(1536), "1.50 KB");
        assert_eq!(fmt_bytes(52_428_800), "50.0 MB");
        assert_eq!(s(&url_escape(&w("a b/c?d"), false)), "a%20b/c?d");
        assert_eq!(s(&url_unescape(&w("a%20b%zz"))), "a b%zz");
        assert_eq!(hls_to_rgb(0, 120, 240), 0x0000FF);
        assert_eq!(rgb_to_hls(0x0000FF), (0, 120, 240));
        let a = cmdline_to_argv(&w("\"C:\\My App\\x.exe\" a\\\\\"b c\" \"d\"\"e\" f\\g"), b"");
        assert_eq!(a.iter().map(|v| s(v)).collect::<alloc::vec::Vec<_>>(), ["C:\\My App\\x.exe", "a\\b c", "d\"e", "f\\g"]);
        assert_eq!(cmdline_to_argv(&[], b"C:\\x.exe").len(), 1);
        assert_eq!(csidl_path(0x8005, "H").unwrap(), "H\\My Documents");
        assert_eq!(file_type_name(b"C:\\a\\b.txt", false), b"TXT File");
    }

    #[test]
    fn version_blocks() {
        // VS_VERSIONINFO with a StringFileInfo\040904b0\FileVersion = "1.2"
        fn node(key: &str, val: &[u8], text: bool, kids: &[alloc::vec::Vec<u8>]) -> alloc::vec::Vec<u8> {
            let mut n = alloc::vec![0u8; 6];
            for c in key.encode_utf16().chain([0]) { n.extend_from_slice(&c.to_le_bytes()); }
            while n.len() % 4 != 0 { n.push(0); }
            n.extend_from_slice(val);
            while n.len() % 4 != 0 { n.push(0); }
            for k in kids { n.extend_from_slice(k); while n.len() % 4 != 0 { n.push(0); } }
            let len = n.len() as u16;
            n[0..2].copy_from_slice(&len.to_le_bytes());
            n[2..4].copy_from_slice(&(if text { val.len() / 2 } else { val.len() } as u16).to_le_bytes());
            n[4..6].copy_from_slice(&(text as u16).to_le_bytes());
            n
        }
        let fv: alloc::vec::Vec<u8> = "1.2\0".encode_utf16().flat_map(|c| c.to_le_bytes()).collect();
        let blk = node("VS_VERSION_INFO", &[0xBD, 0x04, 0xEF, 0xFE], false, &[node("StringFileInfo", &[], true, &[node("040904b0", &[], true, &[node("FileVersion", &fv, true, &[])])])]);
        let (o, l, t) = ver_find(&blk, &w("\\StringFileInfo\\040904B0\\fileversion")).unwrap();
        assert!(t && l == 4 && &blk[o..o + 8] == &fv[..]);
        let (o, l, t) = ver_find(&blk, &w("\\")).unwrap();
        assert!(!t && l == 4 && blk[o] == 0xBD);
        assert!(ver_find(&blk, &w("\\StringFileInfo\\nope")).is_none());
        for n in 0..blk.len() { let _ = ver_find(&blk[..n], &w("\\StringFileInfo\\040904b0\\FileVersion")); }
    }
}
