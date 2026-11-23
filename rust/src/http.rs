//! The HTTP/1.1 client (was src/net/http.c): URL parsing and resolving, GET/POST over TCP or
//! TLS, redirects, chunked transfer encoding and gzip/deflate content encoding. The C
//! interface (src/include/net.h: url_parse, url_resolve, http_request, http_get, http_free)
//! is at the end, for the kernel and for host test programs (feature `htmldoc`); the
//! connection itself goes through the C functions dns_resolve, tcp_* and tls_*.
//!
//! The parsing keeps the C version's results, fixed-size buffers included (a URL is cut where
//! the C code's buffers cut it). Changes: the request is built without a fixed buffer, so a
//! long method (scripts choose it with fetch) can no longer write past the end of the C
//! code's 3,072-byte stack buffer; and running out of memory for a large response ends the
//! download instead of crashing.

use alloc::vec::Vec;

pub const MAX_BODY: u32 = 24 * 1024 * 1024;
pub const USER_AGENT: &[u8] = b"Mozilla/5.0 (compatible; WRLDBrowser/2.0; THE WRLD OS 1.0)";

// ---------------------------------------------------------------- C string helpers

fn find(h: &[u8], n: &[u8]) -> Option<usize> { h.windows(n.len()).position(|w| w == n) }
/// The C string at the start of `b` (up to its first NUL).
pub fn cstr(b: &[u8]) -> &[u8] { &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())] }
/// snprintf / strlcpy into `cap` bytes: at most cap - 1 of them.
fn cut(mut v: Vec<u8>, cap: usize) -> Vec<u8> { v.truncate(cap.saturating_sub(1)); v }
fn cat(parts: &[&[u8]], cap: usize) -> Vec<u8> { cut(parts.concat(), cap) }
/// strlcat(d, s, cap)
fn strlcat(d: &mut Vec<u8>, s: &[u8], cap: usize) {
    if d.len() >= cap { return; }
    let room = cap - d.len() - 1;
    d.extend_from_slice(&s[..s.len().min(room)]);
}
fn isspace(c: u8) -> bool { matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) }
/// The kernel's strtoul(s, NULL, 10), as a 32-bit value (the C callers keep 32 bits).
fn strtoul10(s: &[u8]) -> u32 {
    let mut i = 0;
    while i < s.len() && isspace(s[i]) { i += 1; }
    if i < s.len() && s[i] == b'+' { i += 1; }
    let mut v: u32 = 0;
    while i < s.len() && s[i].is_ascii_digit() { v = v.wrapping_mul(10).wrapping_add((s[i] - b'0') as u32); i += 1; }
    v
}
/// The kernel's atoi (src/kernel/lib.c).
fn atoi(s: &[u8]) -> i32 {
    let mut i = 0;
    while i < s.len() && isspace(s[i]) { i += 1; }
    let neg = i < s.len() && s[i] == b'-';
    if neg { i += 1; }
    let v = strtoul10(&s[i..]);
    (if neg { v.wrapping_neg() } else { v }) as i32
}

// ---------------------------------------------------------------- URLs

/// What url_parse writes: the scheme (lower case) once "scheme://" is found, then the host
/// (lower case), port and path unless the host is empty or does not fit in `hostcap`.
pub struct ParsedUrl { pub scheme: Option<Vec<u8>>, pub rest: Option<(Vec<u8>, u16, Vec<u8>)> }

pub fn url_parse(url: &[u8], hostcap: usize, pathcap: usize) -> ParsedUrl {
    let none = ParsedUrl { scheme: None, rest: None };
    let Some(p) = find(url, b"://") else { return none };
    if p > 7 { return none; }
    let scheme = url[..p].to_ascii_lowercase();
    let h = p + 3;
    let mut e = h;
    while e < url.len() && !matches!(url[e], b'/' | b':' | b'?' | b'#') { e += 1; }
    if e == h || e - h >= hostcap { return ParsedUrl { scheme: Some(scheme), rest: None }; }
    let host = url[h..e].to_ascii_lowercase();
    let mut port: u16 = if scheme == b"https" { 443 } else { 80 };
    let mut q = e;
    if url.get(q) == Some(&b':') {
        q += 1;
        port = atoi(&url[q..]) as u16;
        while q < url.len() && url[q].is_ascii_digit() { q += 1; }
    }
    let mut path = match url.get(q) {
        Some(b'/') => cut(url[q..].to_vec(), pathcap),
        Some(b'?') => { let mut v = cut(b"/".to_vec(), pathcap); strlcat(&mut v, &url[q..], pathcap); v }
        _ => cut(b"/".to_vec(), pathcap),
    };
    if let Some(f) = path.iter().position(|&c| c == b'#') { path.truncate(f); }
    ParsedUrl { scheme: Some(scheme), rest: Some((host, port, path)) }
}

/// RFC 3986 section 5.2.4: "." and ".." segments removed from a path that starts with "/"
/// (empty segments are kept); the result is cut at 2,047 bytes like the C code's buffer.
pub fn remove_dot_segments(mut inp: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    let pop = |out: &mut Vec<u8>| { while out.last().is_some_and(|&c| c != b'/') { out.pop(); } out.pop(); };
    while !inp.is_empty() {
        if inp.starts_with(b"../") { inp = &inp[3..]; }
        else if inp.starts_with(b"./") { inp = &inp[2..]; }
        else if inp.starts_with(b"/./") { inp = &inp[2..]; }
        else if inp == b"/." { inp = b"/"; }
        else if inp.starts_with(b"/../") { inp = &inp[3..]; pop(&mut out); }
        else if inp == b"/.." { inp = b"/"; pop(&mut out); }
        else if inp == b"." || inp == b".." { inp = &[]; }
        else {
            let start = if inp[0] == b'/' { 1 } else { 0 };
            let e = inp[start..].iter().position(|&c| c == b'/').map_or(inp.len(), |i| start + i);
            if out.len() + e < 2048 { out.extend_from_slice(&inp[..e]); }
            inp = &inp[e..];
        }
    }
    out
}
/// "scheme:" at the start (RFC 3986: a letter, then letters, digits, "+", "-" or "."), two
/// characters or more so that a drive letter ("C:/x") is not taken for one.
fn has_scheme(s: &[u8]) -> bool {
    let Some(c) = s.iter().position(|&b| b == b':') else { return false };
    c >= 2 && s[0].is_ascii_alphabetic() && s[1..c].iter().all(|&b| b.is_ascii_alphanumeric() || b == b'+' || b == b'-' || b == b'.')
}

/// Resolves `rel` against `base` (RFC 3986) into at most cap - 1 bytes; false when the base
/// has no scheme (then the result is `rel` as it is).
///
/// Fixed from the C version: "." and ".." in a path that starts with "/" are resolved
/// ("/../g"), a trailing "." no longer leaves "//", "//" inside a relative path is kept, and
/// any "scheme:" reference is absolute (the C code knew only a few, so "tel:123" was taken
/// as a relative path, while anything with "://" in its first ten characters, "./://" too,
/// was taken as absolute), and a "//host/path" reference has its dot segments removed.
pub fn url_resolve(base: &[u8], rel: &[u8], cap: usize) -> (bool, Vec<u8>) {
    let mut r = 0;
    while r < rel.len() && matches!(rel[r], b' ' | b'\t' | b'\n' | b'\r') { r += 1; }
    let rel = &rel[r..];
    if has_scheme(rel) { return (true, cut(rel.to_vec(), cap)); }
    let Some(sch) = find(base, b"://") else { return (false, cut(rel.to_vec(), cap)) };
    if rel.starts_with(b"//") {                          /* another host: its path's dot segments go too */
        let a = rel[2..].iter().position(|&c| matches!(c, b'/' | b'?' | b'#')).map_or(rel.len(), |i| 2 + i);
        let q = rel[a..].iter().position(|&c| c == b'?' || c == b'#').map_or(rel.len(), |i| a + i);
        return (true, cat(&[&base[..sch + 1], &rel[..a], &remove_dot_segments(&rel[a..q]), &rel[q..]], cap));
    }
    let hs = sch + 3;
    let mut pe = hs;
    while pe < base.len() && !matches!(base[pe], b'/' | b'?' | b'#') { pe += 1; }
    let root = cut(base[..pe].to_vec(), 512);
    if rel.is_empty() || rel[0] == b'#' {
        let mut out = cut(base.to_vec(), cap);
        if let Some(f) = out.iter().position(|&c| c == b'#') { out.truncate(f); }
        strlcat(&mut out, rel, cap);
        return (true, out);
    }
    /* directory of the base path */
    let mut path = cut(if base.get(pe) == Some(&b'/') { base[pe..].to_vec() } else { b"/".to_vec() }, 1024);
    if let Some(q) = path.iter().position(|&c| c == b'?' || c == b'#') { path.truncate(q); }
    if rel[0] == b'?' { return (true, cat(&[&root, &path, rel], cap)); }
    if let Some(sl) = path.iter().rposition(|&c| c == b'/') { path.truncate(sl + 1); }
    let mut joined = if rel[0] == b'/' { cut(rel.to_vec(), 2048) } else { cat(&[&path, rel], 2048) };
    let mut tail = Vec::new();
    if let Some(q) = joined.iter().position(|&c| c == b'?' || c == b'#') { tail = cut(joined[q..].to_vec(), 1024); joined.truncate(q); }
    let mut res = remove_dot_segments(&joined);
    if res.is_empty() || res[0] != b'/' { res.insert(0, b'/'); }
    let mut out = cat(&[&root, &res], cap);
    strlcat(&mut out, &tail, cap);
    (true, out)
}

// ---------------------------------------------------------------- responses

/// The value of header `name` (case-insensitive) in a "\r\n"-separated block, cut to cap - 1.
pub fn hdr_find(hdrs: &[u8], name: &[u8], cap: usize) -> Option<Vec<u8>> {
    let nl = name.len();
    let mut p = 0;
    while p < hdrs.len() {
        let e = find(&hdrs[p..], b"\r\n").map_or(hdrs.len(), |i| p + i);
        if e - p > nl && hdrs[p..p + nl].eq_ignore_ascii_case(name) && hdrs[p + nl] == b':' {
            let mut v = p + nl + 1;
            while v < e && (hdrs[v] == b' ' || hdrs[v] == b'\t') { v += 1; }
            let l = (e - v).min(cap.saturating_sub(1));
            return Some(hdrs[v..v + l].to_vec());
        }
        p = if e < hdrs.len() { e + 2 } else { e };
    }
    None
}
/// A chunked body, joined.
pub fn dechunk(b: &[u8]) -> Vec<u8> {
    let len = b.len();
    let mut o = Vec::with_capacity(len);
    let mut i = 0;
    while i < len {
        let (mut sz, mut any) = (0u32, false);
        while i < len && b[i].is_ascii_hexdigit() {
            let c = b[i].to_ascii_lowercase();
            sz = sz.wrapping_mul(16).wrapping_add(if c <= b'9' { c - b'0' } else { c - b'a' + 10 } as u32);
            i += 1; any = true;
        }
        while i < len && b[i] != b'\n' { i += 1; }
        i += 1;
        if !any || sz == 0 { break; }
        let n = (sz as usize).min(len - i.min(len));
        if n > 0 { o.extend_from_slice(&b[i..i + n]); }
        i += n;
        if i < len && b[i] == b'\r' { i += 1; }
        if i < len && b[i] == b'\n' { i += 1; }
    }
    o
}
/// Inflate into a buffer of `cap` bytes; None on bad data or a full buffer.
fn inflate_into(src: &[u8], cap: u32, zlib: bool) -> Option<Vec<u8>> {
    let mut o = Vec::new();
    o.try_reserve_exact(cap as usize + 1).ok()?;
    o.resize(cap as usize, 0);
    let d = if zlib { crate::inflate::inflate_zlib(src, &mut o) } else { crate::inflate::inflate_raw(src, &mut o) }.ok()?;
    o.truncate(d.written);
    Some(o)
}
/// A gzip member's contents.
pub fn ungzip(b: &[u8]) -> Option<Vec<u8>> {
    let len = b.len();
    if len < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 8 { return None; }
    let fl = b[3];
    let mut i = 10usize;
    if fl & 4 != 0 { if i + 2 > len { return None; } i += 2 + (b[i] as usize | (b[i + 1] as usize) << 8); }
    if fl & 8 != 0 { while i < len && b[i] != 0 { i += 1; } i += 1; }
    if fl & 16 != 0 { while i < len && b[i] != 0 { i += 1; } i += 1; }
    if fl & 2 != 0 { i += 2; }
    if i >= len { return None; }
    let isize = u32::from_le_bytes([b[len - 4], b[len - 3], b[len - 2], b[len - 1]]);
    let cap = isize.wrapping_add(64).max((len as u32).wrapping_mul(4)).min(MAX_BODY);
    inflate_into(&b[i..], cap, false)
}

/// A response as the client keeps it.
#[derive(Debug, PartialEq, Eq)]
pub struct Response { pub status: i32, pub content_type: Option<Vec<u8>>, pub charset: Option<Vec<u8>>, pub location: Option<Vec<u8>>, pub body: Vec<u8> }

/// Where the headers end ("\r\n\r\n", in the text before any NUL) and the Content-Length they
/// give, as the download loop sees them.
pub fn header_end(buf: &[u8]) -> Option<(usize, u32)> {
    let text = cstr(buf);
    let e = find(text, b"\r\n\r\n")?;
    let total = hdr_find(&text[..e], b"Content-Length", 32).map_or(0, |v| strtoul10(&v));
    Some((e + 4, total))
}
/// Everything the server sent (the connection closed, or Content-Length bytes arrived),
/// read into a response; Err is the message for the user.
pub fn parse_response(buf: &[u8], url: &[u8]) -> Result<Response, &'static str> {
    let (hdr_end, total) = match header_end(buf) {
        Some(h) => h,
        None => match find(cstr(buf), b"\n\n") { Some(e) => (e + 2, 0), None => return Err("The server sent an invalid response.") },
    };
    let hdrs = cstr(&buf[..hdr_end - 2]);
    if !hdrs.starts_with(b"HTTP/") { return Err("The server did not answer with HTTP."); }
    let status = atoi(hdrs.iter().position(|&c| c == b' ').map_or(&b"0"[..], |s| &hdrs[s + 1..]));
    let (mut content_type, mut charset, mut location) = (None, None, None);
    if let Some(v) = hdr_find(hdrs, b"Content-Type", 256) {
        let mut ct = cut(v.clone(), 96);
        if let Some(semi) = ct.iter().position(|&c| c == b';') { ct.truncate(semi); }
        content_type = Some(ct);
        if let Some(cs) = find(&v, b"charset=") { charset = Some(cut(v[cs + 8..].to_vec(), 24)); }
    }
    if let Some(v) = hdr_find(hdrs, b"Location", 256) { location = Some(url_resolve(url, &v, 1024).1); }
    let mut body: &[u8] = &buf[hdr_end..];
    if total != 0 && body.len() > total as usize { body = &body[..total as usize]; }
    let dec;
    if hdr_find(hdrs, b"Transfer-Encoding", 256).is_some_and(|v| find(&v, b"chunked").is_some()) { dec = dechunk(body); body = &dec; }
    let unz;
    if let Some(v) = hdr_find(hdrs, b"Content-Encoding", 256) {
        let u = if find(&v, b"gzip").is_some() { ungzip(body) }
                else if find(&v, b"deflate").is_some() {
                    let cap = (body.len() as u32).wrapping_mul(8).wrapping_add(1024).min(MAX_BODY);
                    inflate_into(body, cap, true).or_else(|| inflate_into(body, cap, false))
                } else { None };
        if let Some(u) = u { unz = u; body = &unz; }
    }
    Ok(Response { status, content_type, charset, location, body: body.to_vec() })
}

/// The request line and headers.
pub fn request(method: &[u8], path: &[u8], hosthdr: &[u8], body: Option<(&[u8], u32)>) -> Vec<u8> {
    let mut r = Vec::new();
    for p in [method, b" ", path, b" HTTP/1.1\r\nHost: ", hosthdr, b"\r\nUser-Agent: ", USER_AGENT, b"\r\n\
Accept: text/html,application/xhtml+xml,text/plain;q=0.9,image/*;q=0.8,*/*;q=0.5\r\nAccept-Language: en\r\n\
Accept-Encoding: gzip, deflate\r\nConnection: close\r\n"] { r.extend_from_slice(p); }
    if let Some((ctype, blen)) = body {
        r.extend_from_slice(b"Content-Type: "); r.extend_from_slice(ctype);
        r.extend_from_slice(b"\r\nContent-Length: "); r.extend_from_slice(alloc::format!("{}", blen).as_bytes()); r.extend_from_slice(b"\r\n");
    }
    r.extend_from_slice(b"\r\n");
    r
}

// ---------------------------------------------------------------- the C interface

#[cfg(any(feature = "kernel", feature = "htmldoc"))]
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void, CStr};
    use core::ptr::null_mut;

    /// http_response_t (src/include/net.h)
    #[repr(C)]
    pub struct HttpResponse {
        status: c_int,
        url: [u8; 1024], content_type: [u8; 96], charset: [u8; 24], location: [u8; 1024],
        body: *mut u8, body_len: u32,
        error: [u8; 200],
        secure: crate::CBool,
        tls_info: [u8; 80],
    }
    pub fn layout() -> [u32; 6] {
        use core::mem::{offset_of, size_of};
        [size_of::<HttpResponse>() as u32, offset_of!(HttpResponse, location) as u32, offset_of!(HttpResponse, body) as u32,
         offset_of!(HttpResponse, error) as u32, offset_of!(HttpResponse, secure) as u32, offset_of!(HttpResponse, tls_info) as u32]
    }
    type Progress = Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, u32)>;

    #[allow(clashing_extern_declarations)]
    extern "C" {
        fn kmalloc(n: usize) -> *mut c_void;
        fn kfree(p: *mut c_void);
        fn dns_resolve(name: *const c_char, ip: *mut u32, err: *mut c_char, errcap: c_int) -> crate::CBool;
        fn tcp_connect(ip: u32, port: u16, timeout: u32, err: *mut c_char, errcap: c_int) -> *mut c_void;
        fn tcp_send(t: *mut c_void, data: *const u8, len: u32) -> c_int;
        fn tcp_recv(t: *mut c_void, buf: *mut u8, max: u32, timeout: u32) -> c_int;
        fn tcp_close(t: *mut c_void);
        fn tls_open(t: *mut c_void, host: *const c_char, ip: u32, port: u16, err: *mut c_char, errcap: c_int) -> *mut c_void;
        fn tls_write(c: *mut c_void, data: *const u8, len: u32) -> c_int;
        fn tls_read(c: *mut c_void, buf: *mut u8, max: u32) -> c_int;
        fn tls_close(c: *mut c_void);
        fn tls_error_text(c: *mut c_void) -> *const c_char;
        fn tls_cipher_name(c: *mut c_void) -> *const c_char;
    }

    unsafe fn cs<'a>(p: *const c_char) -> &'a [u8] { if p.is_null() { &[] } else { CStr::from_ptr(p).to_bytes() } }
    unsafe fn put(buf: *mut u8, cap: usize, s: &[u8]) {
        if buf.is_null() || cap == 0 { return; }
        let n = s.len().min(cap - 1);
        core::ptr::copy_nonoverlapping(s.as_ptr(), buf, n);
        *buf.add(n) = 0;
    }
    fn z(s: &[u8]) -> Vec<u8> { let mut v = s.to_vec(); v.push(0); v }
    unsafe fn say(cb: Progress, ctx: *mut c_void, s: &[u8], got: u32, total: u32) {
        if let Some(f) = cb { let m = z(&s[..s.len().min(299)]); f(ctx, m.as_ptr() as *const c_char, got, total); }
    }

    #[no_mangle]
    pub unsafe extern "C" fn url_parse(url: *const c_char, scheme: *mut c_char, host: *mut c_char, hostcap: c_int, port: *mut u16, path: *mut c_char, pathcap: c_int) -> crate::CBool {
        let r = super::url_parse(cs(url), hostcap.max(0) as usize, pathcap.max(0) as usize);
        let Some(sch) = r.scheme else { return 0 };
        put(scheme as *mut u8, 8, &sch);
        let Some((h, p, pa)) = r.rest else { return 0 };
        put(host as *mut u8, hostcap as usize, &h);
        *port = p;
        put(path as *mut u8, pathcap.max(0) as usize, &pa);
        1
    }
    #[no_mangle]
    pub unsafe extern "C" fn url_resolve(base: *const c_char, rel: *const c_char, out: *mut c_char, cap: c_int) -> crate::CBool {
        let (ok, v) = super::url_resolve(cs(base), cs(rel), cap.max(0) as usize);
        put(out as *mut u8, cap.max(0) as usize, &v);
        ok as crate::CBool
    }

    struct Conn { t: *mut c_void, tls: *mut c_void }
    impl Conn {
        unsafe fn write(&self, d: &[u8]) -> c_int { if !self.tls.is_null() { tls_write(self.tls, d.as_ptr(), d.len() as u32) } else { tcp_send(self.t, d.as_ptr(), d.len() as u32) } }
        unsafe fn read(&self, b: *mut u8, n: u32) -> c_int {
            if !self.tls.is_null() { return tls_read(self.tls, b, n); }
            let r = tcp_recv(self.t, b, n, 30000);
            if r == -2 { -1 } else { r }
        }
        unsafe fn close(&self) { if !self.tls.is_null() { tls_close(self.tls) } else if !self.t.is_null() { tcp_close(self.t) } }
    }

    unsafe fn once(method: &[u8], url: &[u8], ctype: *const c_char, body: *const u8, blen: u32, r: &mut HttpResponse, cb: Progress, ctx: *mut c_void) -> bool {
        let err = r.error.as_mut_ptr() as *mut c_char;
        let pu = super::url_parse(url, 256, 2048);
        let (Some(scheme), Some((host, port, path))) = (pu.scheme, pu.rest) else { put(r.error.as_mut_ptr(), 200, b"The address is not valid."); return false };
        let https = scheme == b"https";
        if !https && scheme != b"http" { put(r.error.as_mut_ptr(), 200, &[&b"The '"[..], &scheme, b"' protocol is not supported."].concat()); return false; }
        say(cb, ctx, &[&b"Finding site: "[..], &host].concat(), 0, 0);
        let zhost = z(&host);
        let mut ip = 0u32;
        if dns_resolve(zhost.as_ptr() as *const c_char, &mut ip, err, 200) == 0 { return false; }
        say(cb, ctx, &[&b"Connecting to "[..], &host, b"..."].concat(), 0, 0);
        let mut c = Conn { t: tcp_connect(ip, port, 15000, err, 200), tls: null_mut() };
        if c.t.is_null() { return false; }
        if https {
            say(cb, ctx, b"Establishing a secure connection...", 0, 0);
            c.tls = tls_open(c.t, zhost.as_ptr() as *const c_char, ip, port, err, 200);
            if c.tls.is_null() { return false; }                   /* tls_open closed the TCP connection */
            r.secure = 1;
            put(r.tls_info.as_mut_ptr(), 80, cs(tls_cipher_name(c.tls)));
        }
        let defport = (https && port == 443) || (!https && port == 80);
        let hosthdr = if defport { host.clone() } else { cut([&host[..], alloc::format!(":{}", port).as_bytes()].concat(), 300) };
        let body_hdr = (!body.is_null()).then(|| (if ctype.is_null() { &b"application/x-www-form-urlencoded"[..] } else { cs(ctype) }, blen));
        let req = request(method, &path, &hosthdr, body_hdr);
        if c.write(&req) < 0 || (!body.is_null() && blen != 0 && c.write(core::slice::from_raw_parts(body, blen as usize)) < 0) {
            let m = if !c.tls.is_null() { cs(tls_error_text(c.tls)).to_vec() } else { b"The connection was reset while sending the request.".to_vec() };
            put(r.error.as_mut_ptr(), 200, &m);
            c.close(); return false;
        }
        say(cb, ctx, &[&b"Waiting for "[..], &host, b"..."].concat(), 0, 0);
        /* read everything (Connection: close) */
        let mut cap: u32 = 64 * 1024;
        let mut buf: Vec<u8> = Vec::new();
        if buf.try_reserve_exact(cap as usize).is_err() { put(r.error.as_mut_ptr(), 200, b"Not enough memory."); c.close(); return false; }
        buf.resize(cap as usize, 0);
        let (mut len, mut hdr_end, mut total) = (0u32, 0usize, 0u32);
        loop {
            if len + 16384 > cap {
                if cap >= MAX_BODY { break; }
                if buf.try_reserve_exact(cap as usize).is_err() { break; }   /* out of memory: keep what arrived */
                cap *= 2; buf.resize(cap as usize, 0);
            }
            let k = c.read(buf.as_mut_ptr().add(len as usize), cap - len - 1);
            if k < 0 {
                if len == 0 {
                    let t = if c.tls.is_null() { &[][..] } else { cs(tls_error_text(c.tls)) };
                    put(r.error.as_mut_ptr(), 200, if !t.is_empty() { t } else { b"The server closed the connection without answering." });
                    c.close(); return false;
                }
                break;
            }
            if k == 0 { break; }
            len += k as u32;
            if hdr_end == 0 { if let Some((e, t)) = header_end(&buf[..len as usize]) { hdr_end = e; total = t; } }
            if hdr_end != 0 && total != 0 && len as usize - hdr_end >= total as usize { break; }   /* complete even if the server keeps the socket open */
            say(cb, ctx, b"Downloading...", if hdr_end != 0 { len - hdr_end as u32 } else { 0 }, total);
        }
        c.close();
        match parse_response(&buf[..len as usize], url) {
            Err(m) => { put(r.error.as_mut_ptr(), 200, m.as_bytes()); false }
            Ok(p) => {
                r.status = p.status;
                if let Some(ct) = p.content_type { put(r.content_type.as_mut_ptr(), 96, &ct); }
                if let Some(cs_) = p.charset { put(r.charset.as_mut_ptr(), 24, &cs_); }
                if let Some(l) = p.location { put(r.location.as_mut_ptr(), 1024, &l); }
                let b = kmalloc(p.body.len() + 1) as *mut u8;
                if b.is_null() { put(r.error.as_mut_ptr(), 200, b"Not enough memory."); return false; }
                core::ptr::copy_nonoverlapping(p.body.as_ptr(), b, p.body.len());
                *b.add(p.body.len()) = 0;
                r.body = b; r.body_len = p.body.len() as u32;
                true
            }
        }
    }

    #[no_mangle]
    pub unsafe extern "C" fn http_request(method: *const c_char, url: *const c_char, ctype: *const c_char, body: *const c_void, blen: u32,
                                          r: *mut HttpResponse, cb: Progress, ctx: *mut c_void) -> crate::CBool {
        core::ptr::write_bytes(r, 0, 1);
        let r = &mut *r;
        let mut cur = cut(cs(url).to_vec(), 1024);
        let mut method = cs(method).to_vec();
        let (mut body, mut blen) = (body as *const u8, blen);
        for _ in 0..8 {
            http_free(r);
            core::ptr::write_bytes(r as *mut HttpResponse, 0, 1);
            put(r.url.as_mut_ptr(), 1024, &cur);
            if !once(&method, &cur, ctype, body, blen, r, cb, ctx) { return 0; }
            if (300..400).contains(&r.status) && r.location[0] != 0 && r.status != 304 {
                cur = cut(cstr(&r.location).to_vec(), 1024);
                if r.status == 303 || ((r.status == 301 || r.status == 302) && method != b"GET") { method = b"GET".to_vec(); body = core::ptr::null(); blen = 0; }
                continue;
            }
            return 1;
        }
        put(r.error.as_mut_ptr(), 200, b"The page redirected too many times.");
        0
    }
    #[no_mangle]
    pub unsafe extern "C" fn http_get(url: *const c_char, r: *mut HttpResponse, cb: Progress, ctx: *mut c_void) -> crate::CBool {
        http_request(c"GET".as_ptr(), url, core::ptr::null(), core::ptr::null(), 0, r, cb, ctx)
    }
    #[no_mangle]
    pub unsafe extern "C" fn http_free(r: *mut HttpResponse) {
        let r = &mut *r;
        kfree(r.body as *mut c_void); r.body = null_mut(); r.body_len = 0;
    }
}
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub use ffi::layout;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn urls() {
        let p = url_parse(b"HTTPS://Example.COM:8443/a/b?x=1#frag", 256, 2048);
        assert_eq!(p.scheme.as_deref(), Some(&b"https"[..]));
        assert_eq!(p.rest, Some((b"example.com".to_vec(), 8443, b"/a/b?x=1".to_vec())));
        assert_eq!(url_parse(b"http://h?q", 256, 2048).rest.unwrap().2, b"/?q");
        assert!(url_parse(b"http:///x", 256, 2048).rest.is_none());
        let b = b"http://h.org/a/b/c.html?q#f";
        for (rel, want) in [(&b"d.png"[..], &b"http://h.org/a/b/d.png"[..]), (b"../x", b"http://h.org/a/x"), (b"/r", b"http://h.org/r"),
                            (b"//cdn.net/s.js", b"http://cdn.net/s.js"), (b"?z", b"http://h.org/a/b/c.html?z"), (b"#top", b"http://h.org/a/b/c.html?q#top"),
                            (b"mailto:x@y", b"mailto:x@y"), (b"../../../../up", b"http://h.org/up")] {
            assert_eq!(url_resolve(b, rel, 1024), (true, want.to_vec()), "{}", core::str::from_utf8(rel).unwrap());
        }
        assert_eq!(url_resolve(b"nobase", b"x", 1024), (false, b"x".to_vec()));
        /* RFC 3986 section 5.4 (its one-letter scheme "g:h" is a drive letter here) */
        let b = b"http://a/b/c/d;p?q";
        for (rel, want) in [(&b"gh:i"[..], &b"gh:i"[..]), (b"g", b"http://a/b/c/g"), (b"./g", b"http://a/b/c/g"), (b"g/", b"http://a/b/c/g/"), (b"/g", b"http://a/g"),
                            (b"//g", b"http://g"), (b"?y", b"http://a/b/c/d;p?y"), (b"g?y", b"http://a/b/c/g?y"), (b"#s", b"http://a/b/c/d;p?q#s"), (b"g#s", b"http://a/b/c/g#s"),
                            (b";x", b"http://a/b/c/;x"), (b"", b"http://a/b/c/d;p?q"), (b".", b"http://a/b/c/"), (b"./", b"http://a/b/c/"), (b"..", b"http://a/b/"),
                            (b"../g", b"http://a/b/g"), (b"../..", b"http://a/"), (b"../../g", b"http://a/g"), (b"../../../g", b"http://a/g"), (b"/./g", b"http://a/g"),
                            (b"/../g", b"http://a/g"), (b"g.", b"http://a/b/c/g."), (b".g", b"http://a/b/c/.g"), (b"g..", b"http://a/b/c/g.."), (b"./../g", b"http://a/b/g"),
                            (b"./g/.", b"http://a/b/c/g/"), (b"g/./h", b"http://a/b/c/g/h"), (b"g/../h", b"http://a/b/c/h"), (b"g;x=1/./y", b"http://a/b/c/g;x=1/y"),
                            (b"g;x=1/../y", b"http://a/b/c/y"), (b"g?y/./x", b"http://a/b/c/g?y/./x"), (b"g#s/../x", b"http://a/b/c/g#s/../x"), (b"g//h", b"http://a/b/c/g//h"),
                            (b"tel:123", b"tel:123"), (b"C:/x", b"http://a/b/c/C:/x"), (b"./://", b"http://a/b/c/://"),
                            (b"//g/x/../y?q", b"http://g/y?q")] {
            assert_eq!(url_resolve(b, rel, 1024).1, want.to_vec(), "{}", core::str::from_utf8(rel).unwrap());
        }
    }
    #[test]
    fn responses() {
        let r = parse_response(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", b"http://h/").unwrap();
        assert_eq!((r.status, r.content_type.unwrap(), r.charset.unwrap(), r.body), (200, b"text/html".to_vec(), b"utf-8".to_vec(), b"hello world".to_vec()));
        let r = parse_response(b"HTTP/1.0 302 Found\r\nLocation: /next\r\nContent-Length: 3\r\n\r\nabcdef", b"http://h/a/b").unwrap();
        assert_eq!((r.status, r.location.unwrap(), r.body), (302, b"http://h/next".to_vec(), b"abc".to_vec()));
        assert_eq!(parse_response(b"garbage", b"http://h/"), Err("The server sent an invalid response."));
        let gz: &[u8] = &[31, 139, 8, 0, 0, 0, 0, 0, 2, 255, 203, 72, 205, 201, 201, 87, 72, 175, 202, 44, 80, 40, 207, 47, 202, 73, 1, 0, 107, 125, 232, 183, 16, 0, 0, 0];
        let mut resp = b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n".to_vec(); resp.extend_from_slice(gz);
        assert_eq!(parse_response(&resp, b"http://h/").unwrap().body, b"hello gzip world");
        let zl: &[u8] = &[120, 156, 75, 73, 77, 203, 73, 44, 73, 77, 81, 72, 202, 79, 169, 4, 0, 35, 81, 5, 8];
        let mut resp = b"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n\r\n".to_vec(); resp.extend_from_slice(zl);
        assert_eq!(parse_response(&resp, b"http://h/").unwrap().body, b"deflated body");
        assert_eq!(parse_response(b"SMTP 220\n\nx", b"http://h/"), Err("The server did not answer with HTTP."));
    }
}
