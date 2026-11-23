//! The WRLD Browser's document tree as seen from JavaScript (was src/apps/dom.c; the
//! interface is src/include/jsapi.h, dom_*). Nodes live in the page's arena and are never
//! freed before the page, so a node pointer stays valid for the page's lifetime. Strings are
//! UTF-8 everywhere: the tree stores the same bytes the renderer draws. Strings handed to
//! the script engine are kmalloc'd; it frees them with dom_free.
//!
//! The page (page_t) and its form controls (ctrl_t) are C structures from
//! src/apps/browser.h, mirrored here up to the fields this file uses; rust_init checks them.
//! Fetching (dom_fetch) and the wall clock as a double (dom_epoch_ms) stay in
//! src/apps/domnet.c: one talks to the HTTP client, the other returns a float, which the
//! 64-bit kernel's soft-float Rust code cannot hand to C.

#![allow(clippy::missing_safety_doc)]

use crate::html::{self, Arena, Doc, Node};
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use core::ptr::{self, null_mut};

extern "C" {
    fn kmalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn arena_alloc(a: *mut Arena, n: u32) -> *mut c_void;
    fn arena_strdup(a: *mut Arena, s: *const c_char, n: c_int) -> *mut c_char;
    fn url_resolve(base: *const c_char, rel: *const c_char, out: *mut c_char, cap: c_int) -> crate::CBool;
    fn url_parse(url: *const c_char, scheme: *mut c_char, host: *mut c_char, hostcap: c_int, port: *mut u16, path: *mut c_char, pathcap: c_int) -> crate::CBool;
    fn timer_ticks() -> u32;
    static rust_dom_layout: [u32; 12];
}

const C_RADIO: c_int = 5;
const C_SELECT: c_int = 7;

/// ctrl_t
#[repr(C)]
pub struct Ctrl {
    ty: c_int, form: c_int,
    node: *mut Node,
    name: [u8; 64], value: [u8; 512], label: [u8; 64],
    checked: crate::CBool,
    sel: c_int, nopts: c_int,
    opts: *mut *mut u8, optvals: *mut *mut u8,
    r: [c_int; 4],
}
/// page_t, up to scripts_pending.
#[repr(C)]
pub struct Page {
    doc: *mut Doc,
    url: [u8; 1024], title: [u8; 160], referrer: [u8; 1024],
    secure: crate::CBool, tls: [u8; 80],
    source: *mut u8, srclen: u32,
    imgs: *mut c_void, nimgs: c_int,
    ctrls: *mut Ctrl, nctrls: c_int,
    forms: *mut c_void, nforms: c_int,
    items: *mut c_void, nitems: c_int, capitems: c_int,
    links: *mut *mut u8, nlinks: c_int, caplinks: c_int,
    anchors: *mut c_void, nanchors: c_int, capanchors: c_int,
    height: c_int, lay_width: c_int,
    bg: u32, fg: u32, linkc: u32,
    worker_running: crate::CBool, orphan: crate::CBool,
    ads_ok: crate::CBool, blocked: c_int,
    js: *mut c_void,
    browser: *mut c_void,
    tree_dirty: crate::CBool, title_dirty: crate::CBool, scripts_pending: crate::CBool,
}
pub fn layout_ok() -> bool {
    use core::mem::{offset_of, size_of};
    let want = unsafe { rust_dom_layout };
    want == [size_of::<Ctrl>() as u32, offset_of!(Ctrl, node) as u32, offset_of!(Ctrl, checked) as u32, offset_of!(Ctrl, optvals) as u32,
             offset_of!(Page, title) as u32, offset_of!(Page, referrer) as u32, offset_of!(Page, ctrls) as u32, offset_of!(Page, nctrls) as u32,
             offset_of!(Page, blocked) as u32, offset_of!(Page, tree_dirty) as u32, offset_of!(Page, title_dirty) as u32, offset_of!(Page, scripts_pending) as u32]
}

#[inline] unsafe fn pg<'a>(page: *mut c_void) -> &'a mut Page { &mut *(page as *mut Page) }
#[inline] fn nd(n: *mut c_void) -> *mut Node { n as *mut Node }
unsafe fn cs<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
fn cstr(b: &[u8]) -> &[u8] { crate::lxpath::cstr(b) }
unsafe fn tag<'a>(n: *mut Node) -> &'a [u8] { cstr(&(*n).tag) }
fn setstr(dst: &mut [u8], src: &[u8]) {
    let src = cstr(src);
    let n = src.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}
unsafe fn arena(p: &Page) -> *mut Arena { html::doc_arena(p.doc) }
unsafe fn attr_name<'a>(n: *mut Node, i: usize) -> &'a [u8] { cs(*(*n).attr.add(i * 2)) }
unsafe fn node_attr<'a>(n: *mut Node, name: &[u8]) -> Option<&'a [u8]> {
    for i in 0..(*n).nattr.max(0) as usize { if attr_name(n, i) == name { return Some(cs(*(*n).attr.add(i * 2 + 1))); } }
    None
}

/* ---------------- strings ---------------- */
/// A kmalloc'd copy for the script engine: the tree already holds UTF-8.
unsafe fn to_utf8(s: &[u8]) -> *mut c_char {
    let o = kmalloc(s.len() + 1) as *mut u8;
    ptr::copy_nonoverlapping(s.as_ptr(), o, s.len());
    *o.add(s.len()) = 0;
    o as *mut c_char
}
/// A copy for the tree, normalised: scripts can hand us a JavaScript string that is already
/// UTF-8, and stray Windows-1252 bytes still become the characters they stand for.
unsafe fn to_text(p: &Page, u: *const c_char) -> *mut u8 {
    let n = cs(u as *const u8).len() as u32;
    let r = arena_alloc(arena(p), n * 3 + 1) as *mut u8;
    html::utf8_normalise(u as *const u8, n, r);
    r
}
unsafe fn adup(p: &Page, s: &[u8]) -> *mut u8 { arena_strdup(arena(p), s.as_ptr() as *const c_char, s.len() as c_int) as *mut u8 }

#[no_mangle] pub unsafe extern "C" fn dom_free(s: *mut c_char) { kfree(s as *mut c_void); }
#[no_mangle] pub unsafe extern "C" fn dom_changed(page: *mut c_void) { if !page.is_null() { pg(page).tree_dirty = 1; } }

/* ---------------- navigation ---------------- */
#[no_mangle] pub unsafe extern "C" fn dom_root(page: *mut c_void) -> *mut c_void { html::doc_root(pg(page).doc) as *mut c_void }
/// The tree's root stands in for <body> (the parser folds html/head/body away).
#[no_mangle] pub unsafe extern "C" fn dom_node_type(n: *mut c_void) -> c_int { if (*nd(n)).ty == 1 { 3 } else { 1 } }
#[no_mangle] pub unsafe extern "C" fn dom_tag(n: *mut c_void) -> *const c_char {
    let x = nd(n);
    if (*x).ty == 1 { c"#text".as_ptr() } else if tag(x) == b"#doc" { c"body".as_ptr() } else { (*x).tag.as_ptr() as *const c_char }
}
#[no_mangle] pub unsafe extern "C" fn dom_parent(n: *mut c_void) -> *mut c_void { (*nd(n)).parent as *mut c_void }
#[no_mangle] pub unsafe extern "C" fn dom_first(n: *mut c_void) -> *mut c_void { (*nd(n)).first as *mut c_void }
#[no_mangle] pub unsafe extern "C" fn dom_last(n: *mut c_void) -> *mut c_void { (*nd(n)).last as *mut c_void }
#[no_mangle] pub unsafe extern "C" fn dom_next(n: *mut c_void) -> *mut c_void { (*nd(n)).next as *mut c_void }
#[no_mangle] pub unsafe extern "C" fn dom_prev(n: *mut c_void) -> *mut c_void {
    let x = nd(n);
    let p = (*x).parent;
    if p.is_null() { return null_mut(); }
    let mut prev: *mut Node = null_mut();
    let mut c = (*p).first;
    while !c.is_null() && c != x { prev = c; c = (*c).next; }
    prev as *mut c_void
}
/// The next node in document order inside root (null at the end).
unsafe fn walk_next(mut n: *mut Node, root: *mut Node) -> *mut Node {
    if !(*n).first.is_null() { return (*n).first; }
    while !n.is_null() && n != root { if !(*n).next.is_null() { return (*n).next; } n = (*n).parent; }
    null_mut()
}
#[no_mangle] pub unsafe extern "C" fn dom_by_id(page: *mut c_void, id: *const c_char) -> *mut c_void {
    let p = pg(page);
    let want = cs(to_text(p, id));
    let root = html::doc_root(p.doc);
    let mut n = (*root).first;
    while !n.is_null() {
        if (*n).ty == 0 { if let Some(v) = node_attr(n, b"id") { if v == want { return n as *mut c_void; } } }
        n = walk_next(n, root);
    }
    null_mut()
}

/* ---------------- attributes ---------------- */
#[no_mangle] pub unsafe extern "C" fn dom_get_attr(n: *mut c_void, name: *const c_char) -> *mut c_char {
    if (*nd(n)).ty != 0 { return null_mut(); }
    match node_attr(nd(n), cs(name as *const u8)) { Some(v) => to_utf8(v), None => null_mut() }
}
unsafe fn set_attr_text(p: &Page, n: *mut Node, name: &[u8], val: *mut u8) {
    for i in 0..(*n).nattr.max(0) as usize { if attr_name(n, i) == name { *(*n).attr.add(i * 2 + 1) = val; return; } }
    let cnt = (*n).nattr as usize;
    let na = arena_alloc(arena(p), (core::mem::size_of::<*mut u8>() * (cnt * 2 + 4)) as u32) as *mut *mut u8;
    if cnt != 0 { ptr::copy_nonoverlapping((*n).attr, na, cnt * 2); }
    *na.add(cnt * 2) = adup(p, name);
    *na.add(cnt * 2 + 1) = val;
    (*n).attr = na; (*n).nattr += 1;
}
unsafe fn attr_changed(p: &mut Page, n: *mut Node, name: &[u8]) {
    if name == b"class" || name == b"style" || name == b"id" || name == b"hidden" { html::html_restyle(p.doc, n); }
    if (*n).aux > 0 && tag(n) == b"input" && (name == b"value" || name == b"checked") {
        let c = if (*n).aux - 1 < p.nctrls { p.ctrls.add(((*n).aux - 1) as usize) } else { null_mut() };
        if !c.is_null() && (*c).node == n {
            let v = node_attr(n, b"value");
            if name == b"value" { if let Some(v) = v { setstr(&mut (*c).value, v); } }
            if name == b"checked" { (*c).checked = node_attr(n, b"checked").is_some() as crate::CBool; }
        }
    }
    p.tree_dirty = 1;
}
#[no_mangle] pub unsafe extern "C" fn dom_set_attr(page: *mut c_void, n: *mut c_void, name: *const c_char, value: *const c_char) {
    let name = cs(name as *const u8);
    if (*nd(n)).ty != 0 || name.is_empty() { return; }
    let nm: Vec<u8> = name.iter().take(39).map(|c| c.to_ascii_lowercase()).collect();
    let p = pg(page);
    let v = to_text(p, value);
    set_attr_text(p, nd(n), &nm, v);
    attr_changed(p, nd(n), &nm);
}
#[no_mangle] pub unsafe extern "C" fn dom_remove_attr(page: *mut c_void, n: *mut c_void, name: *const c_char) {
    let x = nd(n);
    let name = cs(name as *const u8);
    for i in 0..(*x).nattr.max(0) as usize {
        if attr_name(x, i).eq_ignore_ascii_case(name) {
            ptr::copy((*x).attr.add(i * 2 + 2), (*x).attr.add(i * 2), ((*x).nattr as usize - i - 1) * 2);
            (*x).nattr -= 1;
            attr_changed(pg(page), x, name);
            return;
        }
    }
}
#[no_mangle] pub unsafe extern "C" fn dom_attr_count(n: *mut c_void) -> c_int { if (*nd(n)).ty != 0 { 0 } else { (*nd(n)).nattr } }
#[no_mangle] pub unsafe extern "C" fn dom_attr_name(n: *mut c_void, i: c_int) -> *mut c_char {
    if i >= 0 && i < (*nd(n)).nattr { to_utf8(attr_name(nd(n), i as usize)) } else { null_mut() }
}

/* ---------------- selectors ---------------- */
/// Parsed by Lexbor and matched by rust/src/html.rs (the matcher the style sheets use).
#[no_mangle] pub unsafe extern "C" fn dom_matches(n: *mut c_void, sel: *const c_char) -> c_int {
    if (*nd(n)).ty != 0 { return 0; }
    (html::html_matches(nd(n), sel) > 0) as c_int
}
#[no_mangle] pub unsafe extern "C" fn dom_query(page: *mut c_void, root: *mut c_void, sel: *const c_char, out: *mut *mut c_void, max: c_int, first_only: c_int) -> c_int {
    let r = if !root.is_null() { nd(root) } else { html::doc_root(pg(page).doc) };
    if html::html_selector_valid(sel) == 0 { return 0; }
    let mut cnt = 0;
    let mut n = (*r).first;
    while !n.is_null() && cnt < max {
        if (*n).ty == 0 && html::html_matches(n, sel) > 0 { *out.add(cnt as usize) = n as *mut c_void; cnt += 1; if first_only != 0 { break; } }
        n = walk_next(n, r);
    }
    cnt
}

/* ---------------- text and markup ---------------- */
unsafe fn text_of(n: *mut Node, buf: &mut Vec<u8>) {
    let mut c = (*n).first;
    while !c.is_null() {
        if (*c).ty == 1 { buf.extend_from_slice(cs((*c).text)); }
        else if tag(c) != b"script" && tag(c) != b"style" { text_of(c, buf); }
        else if (*n).ty == 0 && tag(n) == b"script" { text_of(c, buf); }
        c = (*c).next;
    }
}
#[no_mangle] pub unsafe extern "C" fn dom_text(n: *mut c_void) -> *mut c_char {
    let x = nd(n);
    if (*x).ty == 1 { return to_utf8(cs((*x).text)); }
    let mut b = Vec::new();
    if tag(x) == b"script" || tag(x) == b"style" {
        let mut c = (*x).first;
        while !c.is_null() { if (*c).ty == 1 { b.extend_from_slice(cs((*c).text)); } c = (*c).next; }
    } else { text_of(x, &mut b); }
    to_utf8(&b)
}
unsafe fn new_node(p: &Page, ty: u8, t: Option<&[u8]>) -> *mut Node {
    let n = arena_alloc(arena(p), core::mem::size_of::<Node>() as u32) as *mut Node;
    ptr::write_bytes(n, 0, 1);
    (*n).ty = ty;
    if let Some(t) = t { for (k, c) in t.iter().take(11).enumerate() { (*n).tag[k] = c.to_ascii_lowercase(); } }
    n
}
unsafe fn unlink(c: *mut Node) {
    let p = (*c).parent;
    if p.is_null() { return; }
    let mut prev: *mut Node = null_mut();
    let mut x = (*p).first;
    while !x.is_null() && x != c { prev = x; x = (*x).next; }
    if !prev.is_null() { (*prev).next = (*c).next; } else { (*p).first = (*c).next; }
    if (*p).last == c { (*p).last = prev; }
    (*c).parent = null_mut(); (*c).next = null_mut();
}
unsafe fn link_before(p: *mut Node, c: *mut Node, before: *mut Node) {
    (*c).parent = p;
    if before.is_null() || (*before).parent != p {
        (*c).next = null_mut();
        if !(*p).last.is_null() { (*(*p).last).next = c; } else { (*p).first = c; }
        (*p).last = c;
        return;
    }
    let mut prev: *mut Node = null_mut();
    let mut x = (*p).first;
    while !x.is_null() && x != before { prev = x; x = (*x).next; }
    (*c).next = before;
    if !prev.is_null() { (*prev).next = c; } else { (*p).first = c; }
}
/// Script elements added later still run.
unsafe fn mark_scripts(p: &mut Page, n: *mut Node) {
    if (*n).ty != 0 { return; }
    if tag(n) == b"script" && (*n).aux == 0 { p.scripts_pending = 1; }
    let mut c = (*n).first;
    while !c.is_null() { mark_scripts(p, c); c = (*c).next; }
}
#[no_mangle] pub unsafe extern "C" fn dom_set_text(page: *mut c_void, n: *mut c_void, text: *const c_char) {
    let p = pg(page);
    let x = nd(n);
    if (*x).ty == 1 { (*x).text = to_text(p, text); p.tree_dirty = 1; return; }
    while !(*x).first.is_null() { unlink((*x).first); }
    if *text != 0 { let t = new_node(p, 1, None); (*t).text = to_text(p, text); link_before(x, t, null_mut()); }
    if tag(x) == b"title" {
        let s: &[u8] = if (*x).first.is_null() { b"" } else { cs((*(*x).first).text) };
        setstr(&mut p.title, s);
        p.title_dirty = 1;
    }
    p.tree_dirty = 1;
}
/* serialisation */
fn esc(s: &mut Vec<u8>, t: &[u8], attr: bool) {
    for &c in t {
        match c {
            b'&' => s.extend_from_slice(b"&amp;"),
            b'<' => s.extend_from_slice(b"&lt;"),
            b'>' => s.extend_from_slice(b"&gt;"),
            b'"' if attr => s.extend_from_slice(b"&quot;"),
            _ => s.push(c),
        }
    }
}
unsafe fn serialize(s: &mut Vec<u8>, n: *mut Node, raw: bool) {
    if (*n).ty == 1 { if raw { s.extend_from_slice(cs((*n).text)); } else { esc(s, cs((*n).text), false); } return; }
    s.push(b'<'); s.extend_from_slice(tag(n));
    for i in 0..(*n).nattr.max(0) as usize {
        s.push(b' '); s.extend_from_slice(attr_name(n, i)); s.extend_from_slice(b"=\"");
        esc(s, cs(*(*n).attr.add(i * 2 + 1)), true); s.push(b'"');
    }
    s.push(b'>');
    if html::html_is_void((*n).tag.as_ptr() as *const c_char) != 0 { return; }
    let r = tag(n) == b"script" || tag(n) == b"style";
    let mut c = (*n).first;
    while !c.is_null() { serialize(s, c, r); c = (*c).next; }
    s.extend_from_slice(b"</"); s.extend_from_slice(tag(n)); s.push(b'>');
}
#[no_mangle] pub unsafe extern "C" fn dom_inner_html(n: *mut c_void, outer: c_int) -> *mut c_char {
    let x = nd(n);
    let mut s = Vec::new();
    if outer != 0 || (*x).ty == 1 { serialize(&mut s, x, false); }
    else {
        let r = tag(x) == b"script" || tag(x) == b"style";
        let mut c = (*x).first;
        while !c.is_null() { serialize(&mut s, c, r); c = (*c).next; }
    }
    to_utf8(&s)
}
/// Copy a parsed fragment's nodes into the page's arena.
unsafe fn copy_tree(p: &Page, src: *mut Node) -> *mut Node {
    let n = new_node(p, (*src).ty, None);
    (*n).tag = (*src).tag;
    if (*src).ty == 1 { (*n).text = adup(p, cs((*src).text)); }
    else if (*src).nattr != 0 {
        let cnt = (*src).nattr as usize;
        (*n).attr = arena_alloc(arena(p), (core::mem::size_of::<*mut u8>() * (cnt * 2 + 2)) as u32) as *mut *mut u8;
        (*n).nattr = (*src).nattr;
        for i in 0..cnt * 2 { *(*n).attr.add(i) = adup(p, cs(*(*src).attr.add(i))); }
    }
    (*n).css = (*src).css;
    let mut c = (*src).first;
    while !c.is_null() { let k = copy_tree(p, c); link_before(n, k, null_mut()); c = (*c).next; }
    n
}
unsafe fn insert_fragment(p: &mut Page, parent: *mut Node, before: *mut Node, src: *const c_char, run_scripts: bool) -> c_int {
    /* <style> blocks in the fragment add their rules to the page's document */
    let nrules = html::doc_nrules(p.doc);
    let d = html::html_parse_fragment(p.doc, (*parent).tag.as_ptr() as *const c_char, src, p.url.as_ptr() as *const c_char);
    let mut cnt = 0;
    let mut c = (*html::doc_root(d)).first;
    while !c.is_null() {
        let k = copy_tree(p, c);
        if !run_scripts {                                /* scripts set through innerHTML never run */
            let mut x = k;
            while !x.is_null() { if (*x).ty == 0 && tag(x) == b"script" { (*x).aux = -1; } x = walk_next(x, k); }
        }
        link_before(parent, k, before);
        if (*k).ty == 0 { html::html_restyle(p.doc, k); mark_scripts(p, k); }
        cnt += 1;
        c = (*c).next;
    }
    html::doc_free(d);
    if html::doc_nrules(p.doc) != nrules { html::html_restyle_all(p.doc); }
    p.tree_dirty = 1;
    cnt
}
#[no_mangle] pub unsafe extern "C" fn dom_set_inner_html(page: *mut c_void, n: *mut c_void, h: *const c_char) -> c_int {
    let x = nd(n);
    if (*x).ty == 1 { dom_set_text(page, n, h); return 0; }
    while !(*x).first.is_null() { unlink((*x).first); }
    insert_fragment(pg(page), x, null_mut(), h, false)
}
#[no_mangle] pub unsafe extern "C" fn dom_insert_html(page: *mut c_void, n: *mut c_void, where_: *const c_char, h: *const c_char) -> c_int {
    let x = nd(n);
    let w = cs(where_ as *const u8);
    let from_write = w == b"write";                      /* document.write output runs its scripts */
    let p = pg(page);
    if w == b"beforebegin" && !(*x).parent.is_null() { return insert_fragment(p, (*x).parent, x, h, false); }
    if (w == b"afterend" || from_write) && !(*x).parent.is_null() { return insert_fragment(p, (*x).parent, (*x).next, h, from_write); }
    if w == b"afterbegin" { return insert_fragment(p, x, (*x).first, h, false); }
    if w == b"beforeend" { return insert_fragment(p, x, null_mut(), h, false); }
    0
}
#[no_mangle] pub unsafe extern "C" fn dom_create(page: *mut c_void, t: *const c_char) -> *mut c_void {
    new_node(pg(page), 0, Some(cs(t as *const u8))) as *mut c_void
}
#[no_mangle] pub unsafe extern "C" fn dom_create_text(page: *mut c_void, text: *const c_char) -> *mut c_void {
    let p = pg(page);
    let n = new_node(p, 1, None);
    (*n).text = to_text(p, text);
    n as *mut c_void
}
unsafe fn clone_node(p: &Page, src: *mut Node, deep: bool) -> *mut Node {
    let n = new_node(p, (*src).ty, None);
    (*n).tag = (*src).tag;
    (*n).text = (*src).text;
    if (*src).nattr != 0 {
        let cnt = (*src).nattr as usize;
        (*n).attr = arena_alloc(arena(p), (core::mem::size_of::<*mut u8>() * (cnt * 2 + 2)) as u32) as *mut *mut u8;
        ptr::copy_nonoverlapping((*src).attr, (*n).attr, cnt * 2);
        (*n).nattr = (*src).nattr;
    }
    (*n).css = (*src).css;
    if deep { let mut c = (*src).first; while !c.is_null() { link_before(n, clone_node(p, c, true), null_mut()); c = (*c).next; } }
    n
}
#[no_mangle] pub unsafe extern "C" fn dom_clone(page: *mut c_void, n: *mut c_void, deep: c_int) -> *mut c_void { clone_node(pg(page), nd(n), deep != 0) as *mut c_void }
#[no_mangle] pub unsafe extern "C" fn dom_insert(page: *mut c_void, parent: *mut c_void, child: *mut c_void, before: *mut c_void) -> c_int {
    let (p, c, b) = (nd(parent), nd(child), nd(before));
    let mut a = p;
    while !a.is_null() { if a == c { return -1; } a = (*a).parent; }     /* would make a cycle */
    if (*p).ty == 1 { return -1; }
    if b == c { return 0; }
    unlink(c);
    link_before(p, c, b);
    let pgr = pg(page);
    if (*c).ty == 0 { html::html_restyle(pgr.doc, c); mark_scripts(pgr, c); }
    pgr.tree_dirty = 1;
    0
}
#[no_mangle] pub unsafe extern "C" fn dom_remove(page: *mut c_void, child: *mut c_void) -> c_int { unlink(nd(child)); pg(page).tree_dirty = 1; 0 }

/* ---------------- inline style ---------------- */
fn trim_decl(s: &[u8]) -> Option<(usize, usize, usize)> {
    /* the property's start and end (spaces trimmed) and the colon */
    let colon = s.iter().position(|&c| c == b':')?;
    let mut a = 0;
    while a < colon && s[a] == b' ' { a += 1; }
    let mut b = colon;
    while b > a && s[b - 1] == b' ' { b -= 1; }
    Some((a, b, colon))
}
unsafe fn style_rewrite(p: &mut Page, n: *mut Node, prop: &[u8], value: &[u8]) {
    let old = node_attr(n, b"style").unwrap_or(b"");
    let mut out: Vec<u8> = Vec::new();
    let mut s = 0;
    while s < old.len() {
        let e = old[s..].iter().position(|&c| c == b';').map_or(old.len(), |i| s + i);
        if let Some((a, b, _)) = trim_decl(&old[s..e]) {
            let same = prop.len() == b - a && prop.eq_ignore_ascii_case(&old[s + a..s + b]);
            if !same && out.len() + (e - s) + 2 < 1024 { out.extend_from_slice(&old[s + a..e]); out.push(b';'); }
        }
        s = if e < old.len() { e + 1 } else { e };
    }
    if !value.is_empty() {
        let mut add = prop.to_vec(); add.push(b':'); add.extend_from_slice(value); add.push(b';');
        let room = 1023usize.saturating_sub(out.len());                   /* snprintf into what is left of 1024 bytes */
        out.extend_from_slice(&add[..add.len().min(room)]);
    }
    set_attr_text(p, n, b"style", adup(p, &out));
    html::html_restyle(p.doc, n);
    p.tree_dirty = 1;
}
#[no_mangle] pub unsafe extern "C" fn dom_set_style(page: *mut c_void, n: *mut c_void, prop: *const c_char, value: *const c_char) {
    if (*nd(n)).ty != 0 { return; }
    let p = pg(page);
    let v = cs(to_text(p, value));
    let pr = cs(prop as *const u8);
    let pr = &pr[..pr.len().min(47)];
    style_rewrite(p, nd(n), pr, v);
}
#[no_mangle] pub unsafe extern "C" fn dom_get_style(n: *mut c_void, prop: *const c_char) -> *mut c_char {
    if (*nd(n)).ty != 0 { return null_mut(); }
    let Some(s) = node_attr(nd(n), b"style") else { return null_mut() };
    let prop = cs(prop as *const u8);
    let mut i = 0;
    while i < s.len() {
        let e = s[i..].iter().position(|&c| c == b';').map_or(s.len(), |k| i + k);
        if let Some((a, b, colon)) = trim_decl(&s[i..e]) {
            if prop.len() == b - a && prop.eq_ignore_ascii_case(&s[i + a..i + b]) {
                let mut v = i + colon + 1;
                while v < e && s[v] == b' ' { v += 1; }
                let mut ve = e;
                while ve > v && s[ve - 1] == b' ' { ve -= 1; }
                return to_utf8(&s[v..ve]);
            }
        }
        i = if e < s.len() { e + 1 } else { e };
    }
    null_mut()
}

/* ---------------- form controls ---------------- */
unsafe fn ctrl_of(p: &Page, n: *mut Node) -> *mut Ctrl {
    if (*n).ty != 0 || (*n).aux <= 0 || (*n).aux > p.nctrls { return null_mut(); }
    let c = p.ctrls.add(((*n).aux - 1) as usize);
    if (*c).node == n { c } else { null_mut() }
}
#[no_mangle] pub unsafe extern "C" fn dom_get_value(page: *mut c_void, n: *mut c_void) -> *mut c_char {
    let x = nd(n);
    let c = ctrl_of(pg(page), x);
    if !c.is_null() {
        if (*c).ty == C_SELECT { return to_utf8(if (*c).nopts != 0 { cs(*(*c).optvals.add((*c).sel as usize)) } else { b"" }); }
        return to_utf8(cstr(&(*c).value));
    }
    if tag(x) == b"textarea" { return dom_text(n); }
    if tag(x) == b"option" && node_attr(x, b"value").is_none() { return dom_text(n); }
    if tag(x) == b"select" {                             /* not collected yet: first selected option */
        let mut first: *mut Node = null_mut();
        let mut o = (*x).first;
        while !o.is_null() {
            if (*o).ty == 0 && tag(o) == b"option" { if first.is_null() { first = o; } if node_attr(o, b"selected").is_some() { first = o; break; } }
            o = walk_next(o, x);
        }
        return if first.is_null() { to_utf8(b"") } else { dom_get_value(page, first as *mut c_void) };
    }
    to_utf8(node_attr(x, b"value").unwrap_or(b""))
}
#[no_mangle] pub unsafe extern "C" fn dom_set_value(page: *mut c_void, n: *mut c_void, value: *const c_char) {
    let p = pg(page);
    let x = nd(n);
    let c = ctrl_of(p, x);
    let val = to_text(p, value);
    if !c.is_null() {
        if (*c).ty == C_SELECT { for i in 0..(*c).nopts { if cs(*(*c).optvals.add(i as usize)) == cs(val) { (*c).sel = i; } } }
        else { setstr(&mut (*c).value, cs(val)); }
    } else if tag(x) == b"textarea" { dom_set_text(page, n, value); }
    else { set_attr_text(p, x, b"value", val); }
    pg(page).tree_dirty = 1;
}
#[no_mangle] pub unsafe extern "C" fn dom_get_checked(page: *mut c_void, n: *mut c_void) -> c_int {
    let c = ctrl_of(pg(page), nd(n));
    if !c.is_null() { (*c).checked } else { node_attr(nd(n), b"checked").is_some() as c_int }
}
#[no_mangle] pub unsafe extern "C" fn dom_set_checked(page: *mut c_void, n: *mut c_void, on: c_int) {
    let p = pg(page);
    let c = ctrl_of(p, nd(n));
    if !c.is_null() {
        if on != 0 && (*c).ty == C_RADIO {
            for i in 0..p.nctrls as usize {
                let o = p.ctrls.add(i);
                if (*o).ty == C_RADIO && (*o).form == (*c).form && cstr(&(*o).name) == cstr(&(*c).name) { (*o).checked = 0; }
            }
        }
        (*c).checked = on;
    } else if on != 0 { set_attr_text(p, nd(n), b"checked", adup(p, b"")); }
    else { dom_remove_attr(page, n, c"checked".as_ptr()); }
    pg(page).tree_dirty = 1;
}
#[no_mangle] pub unsafe extern "C" fn dom_get_selected_index(page: *mut c_void, n: *mut c_void) -> c_int {
    let c = ctrl_of(pg(page), nd(n));
    if !c.is_null() && (*c).ty == C_SELECT { (*c).sel } else { 0 }
}
#[no_mangle] pub unsafe extern "C" fn dom_set_selected_index(page: *mut c_void, n: *mut c_void, i: c_int) {
    let c = ctrl_of(pg(page), nd(n));
    if !c.is_null() && (*c).ty == C_SELECT && i >= 0 && i < (*c).nopts { (*c).sel = i; pg(page).tree_dirty = 1; }
}

/* ---------------- title, URL ---------------- */
#[no_mangle] pub unsafe extern "C" fn dom_title(page: *mut c_void) -> *mut c_char { to_utf8(cstr(&pg(page).title)) }
#[no_mangle] pub unsafe extern "C" fn dom_set_title(page: *mut c_void, t: *const c_char) {
    let n = cs(t as *const u8).len();
    let u = kmalloc(n * 3 + 1) as *mut u8;
    html::utf8_normalise(t as *const u8, n as u32, u);
    setstr(&mut pg(page).title, cs(u));
    kfree(u as *mut c_void);
    pg(page).title_dirty = 1;
}
#[no_mangle] pub unsafe extern "C" fn dom_url(page: *mut c_void) -> *mut c_char { to_utf8(cstr(&pg(page).url)) }
#[no_mangle] pub unsafe extern "C" fn dom_referrer(page: *mut c_void) -> *mut c_char { to_utf8(cstr(&pg(page).referrer)) }
#[no_mangle] pub unsafe extern "C" fn dom_resolve(page: *mut c_void, base: *const c_char, url: *const c_char) -> *mut c_char {
    let p = pg(page);
    let mut out = [0u8; 2048];
    let dbase = html::doc_base(p.doc);
    let b: *const c_char = if !base.is_null() && *base != 0 { base } else if *dbase != 0 { dbase } else { p.url.as_ptr() as *const c_char };
    if url_resolve(b, url, out.as_mut_ptr() as *mut c_char, 2048) == 0 { setstr(&mut out, cs(url as *const u8)); }
    to_utf8(cstr(&out))
}

/* ---------------- cookies and storage (kept per site for this session) ---------------- */
struct Kv { site: Vec<u8>, k: Vec<u8>, v: Vec<u8> }
static mut COOKIES: Vec<Kv> = Vec::new();
static mut STORAGE: Vec<Kv> = Vec::new();
unsafe fn site_of(p: &Page) -> Vec<u8> {
    let (mut sch, mut host, mut path) = ([0u8; 8], [0u8; 128], [0u8; 8]);
    let mut port = 0u16;
    if url_parse(p.url.as_ptr() as *const c_char, sch.as_mut_ptr() as *mut c_char, host.as_mut_ptr() as *mut c_char, 128, &mut port,
                 path.as_mut_ptr() as *mut c_char, 8) == 0 { return b"local".to_vec(); }
    cstr(&host).to_vec()
}
/// The list in the C code's order: a new entry goes in front.
fn kv_set(l: &mut Vec<Kv>, site: &[u8], k: &[u8], v: Option<&[u8]>) {
    let at = l.iter().position(|e| e.site == site && e.k == k);
    match (v, at) {
        (None, Some(i)) => { l.remove(i); }
        (None, None) => {}
        (Some(v), Some(i)) => l[i].v = v.to_vec(),
        (Some(v), None) => l.insert(0, Kv { site: site[..site.len().min(127)].to_vec(), k: k.to_vec(), v: v.to_vec() }),
    }
}
fn join(l: &[Kv], site: &[u8], f: impl Fn(&Kv, &mut Vec<u8>), sep: &[u8]) -> Vec<u8> {
    let mut s = Vec::new();
    for e in l.iter().filter(|e| e.site == site) { if !s.is_empty() { s.extend_from_slice(sep); } f(e, &mut s); }
    s
}
#[no_mangle] pub unsafe extern "C" fn dom_cookie(page: *mut c_void) -> *mut c_char {
    let site = site_of(pg(page));
    to_utf8(&join(&*ptr::addr_of!(COOKIES), &site, |e, s| { s.extend_from_slice(&e.k); s.push(b'='); s.extend_from_slice(&e.v); }, b"; "))
}
#[no_mangle] pub unsafe extern "C" fn dom_set_cookie(page: *mut c_void, c: *const c_char) {
    let site = site_of(pg(page));
    let c = cs(c as *const u8);
    let Some(eq) = c.iter().position(|&x| x == b'=') else { return };
    let semi = c.iter().position(|&x| x == b';');
    if semi.is_some_and(|s| s < eq) { return; }
    let k = &c[..eq.min(127)];
    let ve = semi.unwrap_or(c.len());
    let v = &c[eq + 1..(eq + 1 + (ve - eq - 1).min(1023))];
    let has = |pat: &[u8]| semi.is_some_and(|s| c[s..].windows(pat.len()).any(|w| w == pat));
    let expire = has(b"expires=Thu, 01 Jan 1970") || has(b"max-age=0") || has(b"Max-Age=0");
    kv_set(&mut *ptr::addr_of_mut!(COOKIES), &site, cstr(k), if expire { None } else { Some(cstr(v)) });
}
#[no_mangle] pub unsafe extern "C" fn dom_storage_get(page: *mut c_void, key: *const c_char) -> *mut c_char {
    let site = site_of(pg(page));
    let k = cs(key as *const u8);
    match (*ptr::addr_of!(STORAGE)).iter().find(|e| e.site == site && e.k == k) { Some(e) => to_utf8(&e.v), None => null_mut() }
}
#[no_mangle] pub unsafe extern "C" fn dom_storage_set(page: *mut c_void, key: *const c_char, value: *const c_char) {
    let site = site_of(pg(page));
    kv_set(&mut *ptr::addr_of_mut!(STORAGE), &site, cs(key as *const u8), if value.is_null() { None } else { Some(cs(value as *const u8)) });
}
#[no_mangle] pub unsafe extern "C" fn dom_storage_clear(page: *mut c_void) {
    let site = site_of(pg(page));
    (*ptr::addr_of_mut!(STORAGE)).retain(|e| e.site != site);
}
#[no_mangle] pub unsafe extern "C" fn dom_storage_keys(page: *mut c_void) -> *mut c_char {
    let site = site_of(pg(page));
    to_utf8(&join(&*ptr::addr_of!(STORAGE), &site, |e, s| s.extend_from_slice(&e.k), b"\n"))
}
#[no_mangle] pub unsafe extern "C" fn dom_now_ms() -> u32 { timer_ticks() }
