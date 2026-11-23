//! The WRLD Browser's ad blocker (was src/apps/adblock.c).
//!  - requests for pictures and scripts on known ad / tracking servers are never made
//!    (built-in list plus C:\WRLD\adblock.txt)
//!  - page elements whose class or id looks like an ad slot are hidden
//!  - sites can be allowed ("@@example.com" in the list file)
//! Top-level navigation (typing an address, following a link) is never blocked.
//!
//! The rules are plain functions here; the C interface (src/apps/browser.h: adblock_*)
//! reads the list file and is built for the kernel and for host test programs.

use alloc::vec::Vec;

/// Matched against the end of the host name: "doubleclick.net" also blocks
/// "ad.doubleclick.net" but not "notdoubleclick.net".
pub const BUILTIN_HOSTS: &[&[u8]] = &[
    b"doubleclick.net", b"googlesyndication.com", b"googleadservices.com", b"google-analytics.com", b"googletagmanager.com",
    b"googletagservices.com", b"adservice.google.com", b"pagead2.googlesyndication.com", b"imasdk.googleapis.com",
    b"amazon-adsystem.com", b"adnxs.com", b"adsrvr.org", b"taboola.com", b"outbrain.com", b"criteo.com", b"criteo.net",
    b"scorecardresearch.com", b"quantserve.com", b"quantcount.com", b"moatads.com", b"pubmatic.com", b"rubiconproject.com",
    b"openx.net", b"casalemedia.com", b"advertising.com", b"bidswitch.net", b"smartadserver.com", b"media.net", b"zedo.com",
    b"popads.net", b"propellerads.com", b"adform.net", b"serving-sys.com", b"hotjar.com", b"mixpanel.com", b"chartbeat.com",
    b"chartbeat.net", b"krxd.net", b"bluekai.com", b"demdex.net", b"everesttech.net", b"mathtag.com", b"adsafeprotected.com",
    b"doubleverify.com", b"teads.tv", b"yieldmo.com", b"sharethrough.com", b"33across.com", b"indexww.com", b"lijit.com",
    b"sovrn.com", b"contextweb.com", b"spotxchange.com", b"springserve.com", b"tremorhub.com", b"yahoo-mbcdn.com",
    b"ads.yahoo.com", b"analytics.yahoo.com", b"adtechus.com", b"atdmt.com", b"adroll.com", b"revcontent.com", b"mgid.com",
    b"zergnet.com", b"exoclick.com", b"juicyads.com", b"trafficjunky.net", b"adcash.com", b"hilltopads.net", b"onclickads.net",
    b"adsterra.com", b"infolinks.com", b"buysellads.com", b"carbonads.com", b"carbonads.net", b"intellitxt.com",
    b"connect.facebook.net", b"pixel.facebook.com", b"ads-twitter.com", b"static.ads-twitter.com", b"analytics.twitter.com",
    b"ads.linkedin.com", b"px.ads.linkedin.com", b"bat.bing.com", b"clarity.ms", b"newrelic.com", b"nr-data.net",
    b"segment.io", b"segment.com", b"optimizely.com", b"crazyegg.com", b"mouseflow.com", b"fullstory.com", b"omtrdc.net",
    b"2o7.net", b"coremetrics.com", b"imrworldwide.com", b"statcounter.com", b"histats.com", b"addthis.com", b"sharethis.com",
    b"adition.com", b"adhigh.net", b"advertserve.com", b"adzerk.net", b"ad.gt", b"adlightning.com", b"admixer.net",
    b"appnexus.com", b"ib-ibi.com", b"rlcdn.com", b"tapad.com", b"agkn.com", b"turn.com", b"w55c.net", b"eyeota.net",
    b"lotame.com", b"crwdcntrl.net", b"id5-sync.com", b"liadm.com", b"permutive.com", b"gumgum.com", b"undertone.com",
];
/// Path fragments that only ad servers use.
pub const BUILTIN_PATHS: &[&[u8]] = &[
    b"/pagead/", b"/adserver", b"/ads.js", b"/adsbygoogle", b"/doubleclick/", b"/ad_banner", b"/adframe", b"/advert.", b"/banner_ad",
    b"/gampad/", b"/prebid", b"/adview", b"/adclick", b"/ads/banner", b"/sponsor-ads",
];
/// Class / id names of ad slots (whole word, case-insensitive).
pub const BUILTIN_HIDE: &[&[u8]] = &[
    b"ad", b"ads", b"adv", b"advert", b"adverts", b"advertisement", b"advertising", b"ad-banner", b"ad-container", b"ad-slot", b"ad-unit",
    b"ad-wrapper", b"ad-box", b"ad_box", b"adbox", b"adbanner", b"adsbygoogle", b"ad-leaderboard", b"leaderboard-ad", b"banner-ad",
    b"banner_ad", b"sidebar-ad", b"top-ad", b"bottom-ad", b"sponsored", b"sponsored-content", b"sponsor", b"dfp-ad", b"gpt-ad",
    b"google-ad", b"google-ads", b"adsense", b"native-ad", b"outbrain", b"taboola", b"OUTBRAIN", b"ad-placeholder", b"adplaceholder",
];
pub const BUILTIN_HIDE_PREFIX: &[&[u8]] = &[b"div-gpt-ad", b"google_ads_", b"ad-slot-", b"adslot", b"taboola-", b"ezoic-ad"];

/// What a fresh C:\WRLD\adblock.txt holds.
pub const DEFAULT_LIST: &[u8] = b"; The WRLD Browser ad blocker - your own rules, one per line.\r\n\
;   example.com        block pictures and scripts from example.com and its subdomains\r\n\
;   ##.promo-box       hide elements with class=\"promo-box\"\r\n\
;   ###top-banner      hide the element with id=\"top-banner\"\r\n\
;   @@example.org      allow everything on pages from example.org\r\n\
; Lines starting with ; or ! are comments. Changes apply to the next page you open.\r\n\
\r\n";

/// The user's rules, lower case: hosts to block, ".class" / "#id" to hide, sites allowed.
#[derive(Default)]
pub struct Lists { pub hosts: Vec<Vec<u8>>, pub hide: Vec<Vec<u8>>, pub allow: Vec<Vec<u8>> }

/// A rule as the C code kept it: a C string (cut at a NUL), in lower case.
fn rule(s: &[u8]) -> Vec<u8> { s.iter().take_while(|&&c| c != 0).map(|c| c.to_ascii_lowercase()).collect() }

pub fn parse(data: &[u8]) -> Lists {
    let mut l = Lists::default();
    let end = data.len();
    let mut s = 0;
    while s < end {
        let mut e = s;
        while e < end && data[e] != b'\n' && data[e] != b'\r' { e += 1; }
        let (mut a, mut b) = (s, e);
        while a < b && (data[a] == b' ' || data[a] == b'\t') { a += 1; }
        while b > a && (data[b - 1] == b' ' || data[b - 1] == b'\t') { b -= 1; }
        if b > a && data[a] != b';' && data[a] != b'!' && data[a] != b'#' {
            if b - a > 2 && data[a] == b'@' && data[a + 1] == b'@' { l.allow.push(rule(&data[a + 2..b])); }
            else {                                       /* also accept "||example.com^" from adblock lists */
                if b - a > 2 && data[a] == b'|' && data[a + 1] == b'|' { a += 2; }
                if b > a && data[b - 1] == b'^' { b -= 1; }
                if b > a && !data[a..b].contains(&b'/') { l.hosts.push(rule(&data[a..b])); }
            }
        } else if b - a > 3 && data[a] == b'#' && data[a + 1] == b'#' && (data[a + 2] == b'.' || data[a + 2] == b'#') {
            l.hide.push(rule(&data[a + 2..b]));
        }
        s = e;
        while s < end && (data[s] == b'\n' || data[s] == b'\r') { s += 1; }
    }
    l
}

pub fn host_matches(host: &[u8], rule: &[u8]) -> bool {
    let (hl, rl) = (host.len(), rule.len());
    if rl > hl { return false; }
    if !host[hl - rl..].eq_ignore_ascii_case(rule) { return false; }
    hl == rl || host[hl - rl - 1] == b'.'
}
/// Is `host` one of the built-in or the user's ad servers?
pub fn host_blocked(host: &[u8], l: &Lists) -> bool {
    BUILTIN_HOSTS.iter().any(|r| host_matches(host, r)) || l.hosts.iter().any(|r| host_matches(host, r))
}
/// Does the URL's path (after "scheme://host") look like an ad server's?
pub fn path_blocked(url: &[u8]) -> bool {
    let Some(sep) = url.windows(3).position(|w| w == b"://") else { return false };
    let Some(slash) = url[sep + 3..].iter().position(|&c| c == b'/') else { return false };
    let low: Vec<u8> = url[sep + 3 + slash..].iter().take(511).map(|c| c.to_ascii_lowercase()).collect();
    BUILTIN_PATHS.iter().any(|p| low.windows(p.len()).any(|w| w == *p))
}
/// Does a class/id word name an ad slot?
pub fn word_is_ad(w: &[u8], is_id: bool, l: &Lists) -> bool {
    if w.is_empty() || w.len() >= 64 { return false; }
    let low: Vec<u8> = w.iter().map(|c| c.to_ascii_lowercase()).collect();
    if BUILTIN_HIDE.iter().any(|h| low.eq_ignore_ascii_case(h)) { return true; }
    if BUILTIN_HIDE_PREFIX.iter().any(|p| low.len() >= p.len() && low[..p.len()].eq_ignore_ascii_case(p)) { return true; }
    let mark = if is_id { b'#' } else { b'.' };
    l.hide.iter().any(|h| h.first() == Some(&mark) && h[1..] == low[..])
}
/// Should an element with this class attribute and id be hidden?
pub fn hides(cls: &[u8], id: Option<&[u8]>, l: &Lists) -> bool {
    if let Some(id) = id { if word_is_ad(id, true, l) { return true; } }
    cls.split(|&c| c == b' ' || c == b'\t' || c == b'\n').any(|w| !w.is_empty() && word_is_ad(w, false, l))
}
/// All the rules (the Security Center shows the count; prefixes are not counted).
pub fn rule_count(l: &Lists) -> usize { BUILTIN_HOSTS.len() + BUILTIN_PATHS.len() + BUILTIN_HIDE.len() + l.hosts.len() + l.hide.len() }

// ---------------------------------------------------------------- the C interface

#[cfg(any(feature = "kernel", feature = "htmldoc"))]
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int, c_void, CStr};
    use core::ptr::addr_of_mut;

    /// The start of vnode_t (src/include/fs.h); rust_init checks it against the kernel's copy.
    #[repr(C)]
    pub struct NodeHead { pub name: [c_char; 96], pub dir: crate::CBool, pub readonly: crate::CBool, pub data: *mut u8, pub size: u32 }

    #[allow(clashing_extern_declarations)]
    extern "C" {
        fn vfs_resolve(path: *const c_char, cwd: *const c_char) -> *mut NodeHead;
        fn vfs_write_file(path: *const c_char, data: *const c_void, len: u32) -> *mut NodeHead;
        static vfs_generation: u32;
        fn url_parse(url: *const c_char, scheme: *mut c_char, host: *mut c_char, hostcap: c_int, port: *mut u16, path: *mut c_char, pathcap: c_int) -> crate::CBool;
    }

    #[no_mangle]
    pub static mut adblock_enabled: crate::CBool = 1;
    /// Requests refused since start-up.
    #[no_mangle]
    pub static mut adblock_total: u32 = 0;

    static mut LISTS: Option<Lists> = None;
    static mut LIST_GEN: u32 = 0xFFFF_FFFF;
    static mut LIST_SIZE: u32 = 0;
    const USER_LIST: &CStr = c"C:\\WRLD\\adblock.txt";

    unsafe fn lists() -> &'static Lists { (*addr_of_mut!(LISTS)).get_or_insert_with(Lists::default) }
    /// Reads C:\WRLD\adblock.txt again when it changed (writing the default list first).
    unsafe fn load_user_list() {
        let mut n = vfs_resolve(USER_LIST.as_ptr(), core::ptr::null());
        if n.is_null() {
            vfs_write_file(USER_LIST.as_ptr(), DEFAULT_LIST.as_ptr() as *const c_void, DEFAULT_LIST.len() as u32);
            n = vfs_resolve(USER_LIST.as_ptr(), core::ptr::null());
        }
        if !n.is_null() && LIST_GEN == vfs_generation && LIST_SIZE == (*n).size { return; }
        LIST_GEN = vfs_generation;
        LIST_SIZE = if n.is_null() { 0 } else { (*n).size };
        let l = &mut *addr_of_mut!(LISTS);
        *l = Some(Lists::default());
        if n.is_null() || (*n).dir != 0 { return; }
        let data = if (*n).data.is_null() || (*n).size == 0 { &[][..] } else { core::slice::from_raw_parts((*n).data, (*n).size as usize) };
        *l = Some(parse(data));
    }
    unsafe fn cs<'a>(p: *const c_char) -> &'a [u8] { if p.is_null() { &[] } else { CStr::from_ptr(p).to_bytes() } }
    unsafe fn host_of(url: *const c_char, out: &mut [u8; 256]) -> usize {
        let (mut sch, mut path, mut port) = ([0 as c_char; 8], [0 as c_char; 8], 0u16);
        out[0] = 0;
        url_parse(url, sch.as_mut_ptr(), out.as_mut_ptr() as *mut c_char, 256, &mut port, path.as_mut_ptr(), 8);
        out.iter().position(|&c| c == 0).unwrap_or(255)
    }
    fn starts_ci(s: &[u8], p: &[u8]) -> bool { s.len() >= p.len() && s[..p.len()].eq_ignore_ascii_case(p) }

    /// Is the page itself on the allow list?
    #[no_mangle]
    pub unsafe extern "C" fn adblock_page_allowed(page_url: *const c_char) -> crate::CBool {
        if adblock_enabled == 0 { return 1; }
        load_user_list();
        let mut h = [0u8; 256];
        let hl = host_of(page_url, &mut h);
        lists().allow.iter().any(|r| host_matches(&h[..hl], r)) as crate::CBool
    }
    /// Should a sub-resource (picture, script, frame) be refused?
    #[no_mangle]
    pub unsafe extern "C" fn adblock_blocks(url: *const c_char, page_url: *const c_char) -> crate::CBool {
        if adblock_enabled == 0 { return 0; }
        let u = cs(url);
        if !starts_ci(u, b"http://") && !starts_ci(u, b"https://") { return 0; }
        if !page_url.is_null() && adblock_page_allowed(page_url) != 0 { return 0; }
        load_user_list();
        let mut h = [0u8; 256];
        let hl = host_of(url, &mut h);
        if hl == 0 { return 0; }
        if host_blocked(&h[..hl], lists()) || path_blocked(u) { adblock_total = adblock_total.wrapping_add(1); return 1; }
        0
    }
    /// Should an element with this class and id be hidden?
    #[no_mangle]
    pub unsafe extern "C" fn adblock_hides(cls: *const c_char, id: *const c_char) -> crate::CBool {
        if adblock_enabled == 0 { return 0; }
        hides(cs(cls), if id.is_null() { None } else { Some(cs(id)) }, lists()) as crate::CBool
    }
    #[no_mangle]
    pub unsafe extern "C" fn adblock_rule_count() -> c_int { load_user_list(); rule_count(lists()) as c_int }
    #[no_mangle]
    pub unsafe extern "C" fn adblock_reload() { LIST_GEN = 0xFFFF_FFFF; load_user_list(); }

    pub fn node_head_offsets() -> [usize; 4] {
        use core::mem::offset_of;
        [offset_of!(NodeHead, dir), offset_of!(NodeHead, readonly), offset_of!(NodeHead, data), offset_of!(NodeHead, size)]
    }
}
#[cfg(any(feature = "kernel", feature = "htmldoc"))]
pub use ffi::node_head_offsets;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rules() {
        let l = parse(b"; comment\r\n||Evil.test^\n@@news.site\n##.Promo-Box\n###top\na/b.com\n  spaced.test \n");
        assert_eq!(l.hosts, [b"evil.test".to_vec(), b"spaced.test".to_vec()]);
        assert_eq!(l.allow, [b"news.site".to_vec()]);
        assert_eq!(l.hide, [b".promo-box".to_vec(), b"#top".to_vec()]);
        assert!(host_blocked(b"ad.doubleclick.net", &l) && !host_blocked(b"notdoubleclick.net", &l) && host_blocked(b"x.EVIL.test", &l));
        assert!(path_blocked(b"https://x.org/js/ADS.JS") && !path_blocked(b"https://x.org/ads"));
        assert!(hides(b"nav promo-box", None, &l) && hides(b"", Some(b"div-gpt-ad-1"), &l) && hides(b"x", Some(b"TOP"), &l));
        assert!(!hides(b"top", None, &l) && !hides(b"header", Some(b"main"), &l));
        assert_eq!(rule_count(&l), BUILTIN_HOSTS.len() + BUILTIN_PATHS.len() + BUILTIN_HIDE.len() + 4);
    }
}
