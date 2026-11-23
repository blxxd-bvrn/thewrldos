//! The WRLD Browser's documents (was `src/apps/html.c`): Lexbor's HTML5 parser replays
//! the page as start/text/end events (`src/lexbor/lexglue.c`) and this builds the tree from
//! them, with the page's title, base address, refresh, body colours, linked style sheets and
//! `<style>` sheets; then the cascade gives every element its style, with selectors parsed
//! and matched by Lexbor on this tree. The text parsing (CSS, media queries, charsets) is
//! `crate::css`.
//!
//! The tree's nodes stay C `node_t`s in the page's arena (browser.c, dom.rs and svg.rs walk
//! them directly) and `doc_t` keeps its public fields; its style sheets, rules and custom
//! properties are Rust's, behind `doc_t.css`. Built into the kernel, and with the `htmldoc`
//! feature into a host test program that links Lexbor and the arena from C.

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr::{null, null_mut};
use crate::css::{self, Css};
use crate::CBool;

// ---------------------------------------------------------------- the C structures

/// node_t (src/apps/browser.h)
#[repr(C)]
pub struct Node {
    pub ty: u8,
    pub tag: [u8; 12],
    pub text: *mut u8,
    pub attr: *mut *mut u8,
    pub nattr: c_int,
    pub css: Css,
    pub aux: c_int,
    pub parent: *mut Node,
    pub first: *mut Node,
    pub last: *mut Node,
    pub next: *mut Node,
}
#[repr(C)]
pub struct Arena { head: *mut c_void }
#[repr(C)]
pub struct Link { href: *mut u8, sheet: c_int }
/// doc_t (src/apps/browser.h)
#[repr(C)]
pub struct Doc {
    arena: Arena,
    root: *mut Node,
    title: [u8; 160],
    base: [u8; 1024],
    refresh: [u8; 1024],
    refresh_s: c_int,
    bgcolor: u32, text: u32, link: u32, vlink: u32,
    rtl: CBool,
    nsheets: c_int,
    links: [Link; MAX_LINKED_SHEETS],
    nlinks: c_int,
    css: *mut Sheets,
    nrules: c_int,
}
const MAX_LINKED_SHEETS: usize = 12;

extern "C" {
    static rust_html_layout: [u32; 12];
}
/// rust_init checks the structures above against the C ones (src/apps/html.c).
pub fn layout_ok() -> bool {
    use core::mem::{offset_of, size_of};
    let want = unsafe { rust_html_layout };
    want == [size_of::<Css>() as u32, offset_of!(Css, bsz) as u32, offset_of!(Css, fill_cur) as u32, size_of::<Node>() as u32,
             offset_of!(Node, css) as u32, offset_of!(Node, next) as u32, size_of::<Doc>() as u32, offset_of!(Doc, refresh_s) as u32,
             offset_of!(Doc, links) as u32, offset_of!(Doc, nlinks) as u32, offset_of!(Doc, css) as u32, offset_of!(Doc, nrules) as u32]
}

#[repr(C)]
struct LxgAttr { name: *const u8, name_len: c_uint, value: *const u8, value_len: c_uint }
#[repr(C)]
struct LxgSink {
    ctx: *mut c_void,
    start: unsafe extern "C" fn(*mut c_void, *const u8, c_uint, *const LxgAttr, c_int),
    end: unsafe extern "C" fn(*mut c_void),
    text: unsafe extern "C" fn(*mut c_void, *const u8, c_uint),
}
#[repr(C)]
struct LxgTree {
    parent: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    prev: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    next: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    tag: unsafe extern "C" fn(*mut c_void) -> *const c_char,
    attr: unsafe extern "C" fn(*mut c_void, *const c_char) -> *const c_char,
    is_empty: unsafe extern "C" fn(*mut c_void) -> c_int,
    is_root: unsafe extern "C" fn(*mut c_void) -> c_int,
}
type EachF = unsafe extern "C" fn(*mut c_void, *mut c_void, c_uint, c_int, *const u8, c_uint);
const LXG_KEY_ANY: c_int = 0;
const LXG_KEY_TAG: c_int = 1;
const LXG_KEY_CLASS: c_int = 2;
const LXG_KEY_ID: c_int = 3;

extern "C" {
    fn lxg_parse_html(utf8: *const u8, len: c_uint, sink: *const LxgSink) -> c_int;
    fn lxg_parse_fragment(context: *const u8, utf8: *const u8, len: c_uint, sink: *const LxgSink) -> c_int;
    fn lxg_pool_new() -> *mut c_void;
    fn lxg_pool_free(pool: *mut c_void);
    fn lxg_selectors(pool: *mut c_void, sel: *const u8, len: c_uint, each: EachF, ctx: *mut c_void) -> c_int;
    fn lxg_match(sel: *mut c_void, node: *mut c_void, tree: *const LxgTree) -> c_int;
    fn lxg_version() -> *const c_char;
    fn arena_alloc(a: *mut Arena, n: u32) -> *mut c_void;
    fn arena_free(a: *mut Arena);
    fn kzalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    fn url_resolve(base: *const c_char, rel: *const c_char, out: *mut c_char, cap: c_int) -> CBool;
    fn klog(fmt: *const c_char, ...);
}

// ---------------------------------------------------------------- helpers

/// The C string at p (empty for NULL).
unsafe fn cs<'a>(p: *const u8) -> &'a [u8] {
    if p.is_null() { return &[]; }
    let mut n = 0;
    while *p.add(n) != 0 { n += 1; }
    core::slice::from_raw_parts(p, n)
}
unsafe fn bytes<'a>(p: *const u8, n: c_uint) -> &'a [u8] { if p.is_null() || n == 0 { &[] } else { core::slice::from_raw_parts(p, n as usize) } }
/// strlcpy into a fixed C buffer
fn put(dst: &mut [u8], s: &[u8]) { let n = s.len().min(dst.len() - 1); dst[..n].copy_from_slice(&s[..n]); dst[n] = 0; }
fn cbuf(b: &[u8]) -> &[u8] { css::cstr(b) }

/// A normalised copy of n bytes of text in the arena (3 bytes out per stray byte in).
unsafe fn textdup(a: *mut Arena, u: &[u8]) -> *mut u8 {
    let t = arena_alloc(a, (u.len() as u32).wrapping_mul(4).wrapping_add(1)) as *mut u8;
    let mut o = 0;
    css::utf8_normalise(u, |b| { *t.add(o) = b; o += 1; });
    *t.add(o) = 0;
    t
}
unsafe fn arena_copy(a: *mut Arena, s: &[u8]) -> *mut u8 {
    let t = arena_alloc(a, s.len() as u32 + 1) as *mut u8;
    core::ptr::copy_nonoverlapping(s.as_ptr(), t, s.len());
    *t.add(s.len()) = 0;
    t
}
unsafe fn attr<'a>(n: *const Node, name: &[u8]) -> Option<&'a [u8]> {
    for i in 0..(*n).nattr.max(0) as usize {
        if cs(*(*n).attr.add(i * 2)) == name { return Some(cs(*(*n).attr.add(i * 2 + 1))); }
    }
    None
}
fn lower(c: u8) -> u8 { c.to_ascii_lowercase() }

// ---------------------------------------------------------------- style sheets, rules, variables

struct Rule { sel: *mut c_void, spec: u32, order: u32, css: Css }
struct Bucket { hash: u32, kind: c_int, key: Vec<u8>, idx: Vec<u32> }
const CSS_BUCKETS: usize = 256;
/// Every complex selector of every rule, bucketed by the key of its right-most compound
/// selector so an element only tries the rules that could match it.
struct RuleSet { rules: Vec<Rule>, buckets: Vec<Vec<Bucket>>, selpool: *mut c_void }
struct Var { name: Vec<u8>, val: Vec<u8>, prio: u8 }
/// CSS custom properties (--name: value), one set per document.
struct VarSet(Vec<Var>);
impl css::Vars for VarSet { fn get(&self, n: &[u8]) -> Option<&[u8]> { self.0.iter().rev().find(|v| v.name == n).map(|v| &v.val[..]) } }
struct Sheet { text: Vec<u8>, sheet: c_int }
/// A document's style: every sheet's text (so the rules can be parsed again when a linked
/// sheet arrives), the rules, and the variables.
pub struct Sheets { set: RuleSet, sheets: Vec<Sheet>, vars: VarSet, var_filter: bool }

fn khash(kind: c_int, k: &[u8]) -> u32 {
    let mut h = 2166136261u32 ^ kind as u32;
    for &b in k { h = (h ^ lower(b) as u32).wrapping_mul(16777619); }
    h
}
impl RuleSet {
    fn bucket(&self, kind: c_int, key: &[u8]) -> Option<&Bucket> {
        let h = khash(kind, key);
        self.buckets.get((h as usize) % CSS_BUCKETS)?.iter().find(|b| b.hash == h && b.kind == kind && b.key.eq_ignore_ascii_case(key))
    }
    fn clear(&mut self) {
        self.rules.clear();
        for b in self.buckets.iter_mut() { b.clear(); }
        if !self.selpool.is_null() { unsafe { lxg_pool_free(self.selpool) }; self.selpool = null_mut(); }
    }
}
struct AddCtx<'a> { set: &'a mut RuleSet, css: Css, order: u32 }
unsafe extern "C" fn add_rule(ctx: *mut c_void, sel: *mut c_void, spec: c_uint, kind: c_int, key: *const u8, kl: c_uint) {
    let a = &mut *(ctx as *mut AddCtx);
    let set = &mut *a.set;
    if set.rules.try_reserve(1).is_err() { return; }
    if set.buckets.is_empty() {
        if set.buckets.try_reserve_exact(CSS_BUCKETS).is_err() { return; }
        for _ in 0..CSS_BUCKETS { set.buckets.push(Vec::new()); }
    }
    let key: &[u8] = if kind == LXG_KEY_ANY { b"*" } else { bytes(key, kl) };
    let h = khash(kind, key);
    let list = &mut set.buckets[(h as usize) % CSS_BUCKETS];
    let bi = match list.iter().position(|b| b.hash == h && b.kind == kind && b.key.eq_ignore_ascii_case(key)) {
        Some(i) => i,
        None => {
            let mut k = Vec::new();
            if list.try_reserve(1).is_err() || k.try_reserve_exact(key.len()).is_err() { return; }
            k.extend_from_slice(key);
            list.push(Bucket { hash: h, kind, key: k, idx: Vec::new() });
            list.len() - 1
        }
    };
    if list[bi].idx.try_reserve(1).is_err() { return; }
    list[bi].idx.push(set.rules.len() as u32);
    set.rules.push(Rule { sel, spec, order: a.order, css: a.css });
}

/// The rules of one sheet into the rule set, with the variables known so far.
fn parse_rules(set: &mut RuleSet, vars: &VarSet, text: &[u8], sheet: c_int) {
    let mut rule_no: u32 = 0;
    css::rules(text, &mut |sel, body| {
        let mut c = Css::default();
        css::decls(body, &mut c, vars);
        if c.set == 0 { return; }
        let order = (sheet as u32) << 20 | rule_no.min(0xFFFFF);
        rule_no = rule_no.wrapping_add(1);
        unsafe {
            if set.selpool.is_null() { set.selpool = lxg_pool_new(); }
            let pool = set.selpool;
            let mut a = AddCtx { set: &mut *set, css: c, order };
            lxg_selectors(pool, sel.as_ptr(), sel.len() as c_uint, add_rule, &mut a as *mut AddCtx as *mut c_void);
        }
    });
}

/// Selectors parsed only to ask whether anything matches (custom properties).
struct VarSel { v: [*mut c_void; 16], n: usize }
unsafe extern "C" fn varsel_add(ctx: *mut c_void, sel: *mut c_void, _: c_uint, _: c_int, _: *const u8, _: c_uint) {
    let l = &mut *(ctx as *mut VarSel);
    if l.n < 16 { l.v[l.n] = sel; l.n += 1; }
}
/// Does any element of the finished tree match one of a rule's selectors? Variables live per
/// element in CSS; this engine keeps one set per page, so a rule that matches nothing (a
/// dark-mode or state variant such as .bar.dark) must not set them.
unsafe fn rule_matches_any(root: *mut Node, sel: &[u8], pool: &mut *mut c_void) -> bool {
    if pool.is_null() { *pool = lxg_pool_new(); }
    let mut l = VarSel { v: [null_mut(); 16], n: 0 };
    if lxg_selectors(*pool, sel.as_ptr(), sel.len() as c_uint, varsel_add, &mut l as *mut VarSel as *mut c_void) < 0 || l.n == 0 { return false; }
    let mut n = root;
    while !n.is_null() {
        if (*n).ty == 0 { for i in 0..l.n { if lxg_match(l.v[i], n as *mut c_void, &TREE_OPS) != 0 { return true; } } }
        if (*n).ty == 0 && !(*n).first.is_null() { n = (*n).first; continue; }
        while !n.is_null() && (*n).next.is_null() { n = (*n).parent; }
        if !n.is_null() { n = (*n).next; }
    }
    false
}
fn var_set(vars: &mut VarSet, name: &[u8], val: &[u8], prio: u8) {
    let val = css::var_value(val);
    if let Some(v) = vars.0.iter_mut().find(|v| v.name == name) {
        if prio < v.prio { return; }                  // a component's value does not replace the page's
        let mut nv = Vec::new();
        if nv.try_reserve_exact(val.len()).is_err() { return; }
        nv.extend_from_slice(val);
        v.val = nv; v.prio = prio;
        return;
    }
    let name = css::cstr(name);
    let (mut n, mut v) = (Vec::new(), Vec::new());
    if vars.0.try_reserve(1).is_err() || n.try_reserve_exact(name.len()).is_err() || v.try_reserve_exact(val.len()).is_err() { return; }
    n.extend_from_slice(name); v.extend_from_slice(val);
    vars.0.push(Var { name: n, val: v, prio });
}
/// The custom properties in a sheet's rules; `:root`, html and body set the page's values.
unsafe fn collect_vars(vars: &mut VarSet, filter: bool, root: *mut Node, text: &[u8], pool: &mut *mut c_void) {
    css::rules(text, &mut |sel, body| {
        let prio = css::var_prio(sel);
        if !css::has_vars(body) { return; }        // skip the matching for rules without variables
        if prio == 1 && filter && !rule_matches_any(root, sel, pool) { return; }
        css::var_decls(body, &mut |n, v| var_set(vars, n, v, prio));
    });
}

unsafe fn sheets<'a>(d: *mut Doc) -> Option<&'a mut Sheets> {
    if (*d).css.is_null() {
        (*d).css = Box::into_raw(Box::new(Sheets { set: RuleSet { rules: Vec::new(), buckets: Vec::new(), selpool: null_mut() }, sheets: Vec::new(), vars: VarSet(Vec::new()), var_filter: false }));
    }
    Some(&mut *(*d).css)
}
unsafe fn sync(d: *mut Doc) { if let Some(s) = (*d).css.as_ref() { (*d).nrules = s.set.rules.len() as c_int; } }
fn free_pool(p: &mut *mut c_void) { if !p.is_null() { unsafe { lxg_pool_free(*p) }; *p = null_mut(); } }

/// Keep a sheet's text and collect its variables first, so the rules can use them.
unsafe fn store_sheet(d: *mut Doc, s: &[u8], sheet: c_int) -> bool {
    let Some(sh) = sheets(d) else { return false };
    let mut t = Vec::new();
    if sh.sheets.try_reserve(1).is_err() || t.try_reserve_exact(s.len()).is_err() { return false; }
    t.extend_from_slice(s);
    sh.sheets.push(Sheet { text: t, sheet });
    let mut pool = null_mut();
    let (filter, root) = (sh.var_filter, (*d).root);
    collect_vars(&mut sh.vars, filter, root, s, &mut pool);
    free_pool(&mut pool);
    true
}
unsafe fn parse_stylesheet(d: *mut Doc, s: &[u8], sheet: c_int) {
    if !store_sheet(d, s, sheet) { return; }
    let sh = &mut *(*d).css;
    parse_rules(&mut sh.set, &sh.vars, s, sheet);
    sync(d);
}
/// Parse every stored sheet's rules again, in sheet order.
unsafe fn reparse_sheets(d: *mut Doc) {
    let Some(sh) = sheets(d) else { return };
    sh.set.clear();
    sh.sheets.sort_by_key(|s| s.sheet);             // stable, like the insertion sort it replaces
    for s in sh.sheets.iter() { parse_rules(&mut sh.set, &sh.vars, &s.text, s.sheet); }
    sync(d);
}
/// The tree is complete: collect the variables again from the rules that match something,
/// and parse the rules with them.
unsafe fn refilter_vars(d: *mut Doc) {
    let Some(sh) = sheets(d) else { return };
    sh.var_filter = true;
    if sh.vars.0.is_empty() { return; }
    sh.vars.0.clear();
    let mut pool = null_mut();
    let root = (*d).root;
    for s in sh.sheets.iter() { collect_vars(&mut sh.vars, true, root, &s.text, &mut pool); }
    reparse_sheets(d);
    free_pool(&mut pool);
}

// ---------------------------------------------------------------- the matcher's view of the tree

unsafe extern "C" fn tr_parent(n: *mut c_void) -> *mut c_void { (*(n as *mut Node)).parent as *mut c_void }
unsafe extern "C" fn tr_prev(n: *mut c_void) -> *mut c_void {
    let x = n as *mut Node;
    let p = (*x).parent;
    if p.is_null() { return null_mut(); }
    let mut last = null_mut();
    let mut c = (*p).first;
    while !c.is_null() && c != x { if (*c).ty == 0 { last = c; } c = (*c).next; }
    last as *mut c_void
}
unsafe extern "C" fn tr_next(n: *mut c_void) -> *mut c_void {
    let mut c = (*(n as *mut Node)).next;
    while !c.is_null() { if (*c).ty == 0 { return c as *mut c_void; } c = (*c).next; }
    null_mut()
}
/// The root stands in for <body>.
unsafe extern "C" fn tr_tag(n: *mut c_void) -> *const c_char {
    let x = n as *mut Node;
    if (*x).parent.is_null() { c"body".as_ptr() } else { (*x).tag.as_ptr() as *const c_char }
}
unsafe extern "C" fn tr_attr(n: *mut c_void, name: *const c_char) -> *const c_char { node_attr(n as *mut Node, name) }
unsafe extern "C" fn tr_empty(n: *mut c_void) -> c_int {
    let mut c = (*(n as *mut Node)).first;
    while !c.is_null() { if (*c).ty == 0 || (!(*c).text.is_null() && *(*c).text != 0) { return 0; } c = (*c).next; }
    1
}
unsafe extern "C" fn tr_isroot(n: *mut c_void) -> c_int { (*(n as *mut Node)).parent.is_null() as c_int }
static TREE_OPS: LxgTree = LxgTree { parent: tr_parent, prev: tr_prev, next: tr_next, tag: tr_tag, attr: tr_attr, is_empty: tr_empty, is_root: tr_isroot };

// ---------------------------------------------------------------- the cascade

unsafe fn try_bucket(set: &RuleSet, b: Option<&Bucket>, n: *mut Node, hits: &mut Vec<(u32, u32, u32)>) {
    let Some(b) = b else { return };
    for &i in &b.idx {
        let r = &set.rules[i as usize];
        if lxg_match(r.sel, n as *mut c_void, &TREE_OPS) == 0 { continue; }
        if hits.try_reserve(1).is_err() { return; }
        hits.push((r.spec, r.order, i));
    }
}
unsafe fn sheet_style(d: *mut Doc, n: *mut Node) -> Css {
    let mut c = Css::default();
    let Some(sh) = (*d).css.as_ref() else { return c };
    let set = &sh.set;
    if set.rules.is_empty() { return c; }
    let mut hits = Vec::new();
    if let Some(id) = attr(n, b"id") { if !id.is_empty() { try_bucket(set, set.bucket(LXG_KEY_ID, id), n, &mut hits); } }
    if let Some(cl) = attr(n, b"class") {
        let mut i = 0;
        while i < cl.len() {
            while i < cl.len() && css::isspace(cl[i]) { i += 1; }
            let s = i;
            while i < cl.len() && !css::isspace(cl[i]) { i += 1; }
            if i > s {
                let w = &cl[s..i];
                // class="a a": try a bucket only once
                let dup = (0..s).any(|p| cl[p..].starts_with(w) && (p == 0 || css::isspace(cl[p - 1])) && p + w.len() < cl.len() && css::isspace(cl[p + w.len()]));
                if !dup { try_bucket(set, set.bucket(LXG_KEY_CLASS, w), n, &mut hits); }
            }
        }
    }
    let tag = cs(tr_tag(n as *mut c_void) as *const u8);
    try_bucket(set, set.bucket(LXG_KEY_TAG, tag), n, &mut hits);
    try_bucket(set, set.bucket(LXG_KEY_ANY, b"*"), n, &mut hits);
    hits.sort_by_key(|h| (h.0, h.1));                // lower specificity first, then source order
    for h in &hits { css::merge(&mut c, &set.rules[h.2 as usize].css); }
    c
}
unsafe fn inline_style(d: *mut Doc, n: *mut Node, c: &mut Css) {
    if let Some(st) = attr(n, b"style") {
        let mut s = Css::default();
        match (*d).css.as_ref() { Some(sh) => css::decls(st, &mut s, &sh.vars), None => css::decls(st, &mut s, &css::NoVars) }
        css::merge(c, &s);
    }
}
unsafe fn style_node(d: *mut Doc, n: *mut Node) {
    let mut c = sheet_style(d, n);
    inline_style(d, n, &mut c);
    if attr(n, b"hidden").is_some() { c.set |= css::ST_DISPLAY; c.display = 0; }
    (*n).css = c;
}
/// An iterative pre-order walk over n and its following siblings' subtrees.
unsafe fn apply_css(d: *mut Doc, mut n: *mut Node) {
    let stop = if n.is_null() { null_mut() } else { (*n).parent };
    while !n.is_null() {
        if (*n).ty == 0 { style_node(d, n); }
        if (*n).ty == 0 && !(*n).first.is_null() { n = (*n).first; continue; }
        while !n.is_null() && (*n).next.is_null() { n = (*n).parent; if n == stop { return; } }
        if !n.is_null() { n = (*n).next; }
    }
}
unsafe fn style_root(d: *mut Doc) {
    let r = (*d).root;
    let mut c = sheet_style(d, r);
    inline_style(d, r, &mut c);
    if c.set & css::ST_BG != 0 { (*d).bgcolor = c.bg; }
    if c.set & css::ST_COLOR != 0 { (*d).text = c.color; }
    (*r).css = c;                                    // body's box (margins, max-width) for page_layout
}

// ---------------------------------------------------------------- building the tree

const MAX_DEPTH: usize = 512;
const FR_NODE: u8 = 0; const FR_PASS: u8 = 1; const FR_TITLE: u8 = 2; const FR_STYLE: u8 = 3; const FR_SKIP: u8 = 4;
struct Builder {
    d: *mut Doc,
    cssdoc: *mut Doc,                                // where <style> rules go (the page, for fragments)
    url: *const c_char,
    st: Vec<(*mut Node, u8)>,
    overflow: u32,
    buf: Vec<u8>,                                    // text of the <title> / <style> being collected
    in_body: bool,
}
impl Builder {
    fn top(&self) -> *mut Node { self.st.last().map(|f| f.0).unwrap_or(null_mut()) }
    /// deeper levels are flattened into the last one
    fn push(&mut self, n: *mut Node, kind: u8) { if self.st.len() >= MAX_DEPTH { self.overflow += 1; return; } self.st.push((n, kind)); }
}
unsafe fn append(p: *mut Node, n: *mut Node) {
    (*n).parent = p;
    if !(*p).last.is_null() { (*(*p).last).next = n; } else { (*p).first = n; }
    (*p).last = n;
}
unsafe fn copy_attrs(d: *mut Doc, a: &[LxgAttr]) -> *mut *mut u8 {
    let ar = &mut (*d).arena as *mut Arena;
    let v = arena_alloc(ar, (core::mem::size_of::<*mut u8>() * (a.len() * 2 + 2)) as u32) as *mut *mut u8;
    for (i, at) in a.iter().enumerate() {
        let nm = arena_alloc(ar, at.name_len + 1) as *mut u8;
        for k in 0..at.name_len as usize { *nm.add(k) = lower(*at.name.add(k)); }
        *v.add(i * 2) = nm;
        *v.add(i * 2 + 1) = textdup(ar, bytes(at.value, at.value_len));
    }
    v
}
unsafe extern "C" fn sink_start(ctx: *mut c_void, name: *const u8, nl: c_uint, a: *const LxgAttr, na: c_int) {
    let b = &mut *(ctx as *mut Builder);
    let d = b.d;
    let attrs: &[LxgAttr] = if a.is_null() || na <= 0 { &[] } else { core::slice::from_raw_parts(a, na as usize) };
    let mut tag = [0u8; 12];
    let nm = bytes(name, nl);
    for k in 0..nm.len().min(11) { tag[k] = lower(nm[k]); }
    let t = cbuf(&tag);
    let kind = b.st.last().map(|f| f.1).unwrap_or(FR_NODE);
    if kind == FR_SKIP || kind == FR_TITLE || kind == FR_STYLE { b.push(null_mut(), FR_SKIP); return; }
    if t == b"html" || t == b"head" {
        if t == b"html" {                            // <html dir="rtl">
            for at in attrs { if bytes(at.name, at.name_len) == b"dir" { (*d).rtl = (bytes(at.value, at.value_len).eq_ignore_ascii_case(b"rtl")) as CBool; } }
        }
        let top = b.top();
        b.push(top, FR_PASS);
        return;
    }
    if t == b"body" {
        b.in_body = true;
        let r = (*d).root;                           // the root stands in for <body>: keep its attributes for selectors
        (*r).attr = copy_attrs(d, attrs); (*r).nattr = attrs.len() as c_int;
        for i in 0..attrs.len() {
            let (nm, v) = (cs(*(*r).attr.add(i * 2)), cs(*(*r).attr.add(i * 2 + 1)));
            match nm {
                b"bgcolor" => { if let Some(c) = css::parse_color(v) { (*d).bgcolor = c; } }
                b"text" => { if let Some(c) = css::parse_color(v) { (*d).text = c; } }
                b"link" => { if let Some(c) = css::parse_color(v) { (*d).link = c; } }
                b"vlink" => { if let Some(c) = css::parse_color(v) { (*d).vlink = c; } }
                b"dir" => (*d).rtl = v.eq_ignore_ascii_case(b"rtl") as CBool,
                _ => {}
            }
        }
        b.push(r, FR_PASS);
        return;
    }
    if t == b"title" && !b.in_body && (*d).title[0] == 0 { b.buf.clear(); b.push(null_mut(), FR_TITLE); return; }
    if t == b"style" {
        for at in attrs { if bytes(at.name, at.name_len) == b"media" && !css::media_ok(bytes(at.value, at.value_len)) { b.push(null_mut(), FR_SKIP); return; } }
        b.buf.clear(); b.push(null_mut(), FR_STYLE); return;
    }
    let ar = &mut (*d).arena as *mut Arena;
    if t == b"base" {
        for at in attrs {
            if bytes(at.name, at.name_len) == b"href" {
                let h = textdup(ar, bytes(at.value, at.value_len));
                url_resolve(b.url, h as *const c_char, (*d).base.as_mut_ptr() as *mut c_char, (*d).base.len() as c_int);
            }
        }
        b.push(null_mut(), FR_SKIP); return;
    }
    let n = arena_alloc(ar, core::mem::size_of::<Node>() as u32) as *mut Node;
    (&mut (*n).tag)[..t.len()].copy_from_slice(t);         // strcpy: up to a NUL in the name
    (*n).attr = copy_attrs(d, attrs); (*n).nattr = attrs.len() as c_int;
    if t == b"meta" {
        if let (Some(he), Some(ct)) = (attr(n, b"http-equiv"), attr(n, b"content")) {
            if he.eq_ignore_ascii_case(b"refresh") {
                let (s, u) = css::meta_refresh(ct);
                (*d).refresh_s = s;
                if let Some(u) = u { put(&mut (*d).refresh, u); }
            }
        }
        b.push(null_mut(), FR_SKIP); return;
    }
    if t == b"link" {                                // style sheets are fetched by the browser later; their order counts
        let (rel, href, media) = (attr(n, b"rel"), attr(n, b"href"), attr(n, b"media"));
        if let (Some(rel), Some(href)) = (rel, href) {
            if !href.is_empty() && css::has_ci(rel, b"stylesheet") && !css::has_ci(rel, b"alternate") && media.map_or(true, css::media_ok)
                && ((*d).nlinks as usize) < MAX_LINKED_SHEETS {
                let i = (*d).nlinks as usize;
                (*d).links[i].href = arena_copy(ar, href);
                (*d).nsheets += 1;
                (*d).links[i].sheet = (*d).nsheets;
                (*d).nlinks += 1;
            }
        }
        b.push(null_mut(), FR_SKIP); return;
    }
    append(b.top(), n);
    b.push(n, FR_NODE);
}
unsafe extern "C" fn sink_end(ctx: *mut c_void) {
    let b = &mut *(ctx as *mut Builder);
    if b.overflow != 0 { b.overflow -= 1; return; }
    if b.st.len() <= 1 { return; }
    let (_, kind) = b.st.pop().unwrap_or((null_mut(), FR_NODE));
    if kind == FR_TITLE {
        let mut norm = Vec::new();
        if norm.try_reserve(b.buf.len() * 3).is_err() { return; }
        css::utf8_normalise(&b.buf, |c| norm.push(c));
        put(&mut (*b.d).title, &css::title(&norm));
    } else if kind == FR_STYLE && !b.buf.is_empty() {
        let cd = b.cssdoc;
        (*cd).nsheets += 1;
        let sheet = (*cd).nsheets;
        let text = core::mem::take(&mut b.buf);
        parse_stylesheet(cd, &text, sheet);
        b.buf = text;
    }
}
unsafe extern "C" fn sink_text(ctx: *mut c_void, s: *const u8, n: c_uint) {
    let b = &mut *(ctx as *mut Builder);
    let kind = b.st.last().map(|f| f.1).unwrap_or(FR_NODE);
    let text = bytes(s, n);
    if kind == FR_TITLE || kind == FR_STYLE {
        if b.buf.try_reserve(text.len() + 1).is_ok() { b.buf.extend_from_slice(text); }
        return;
    }
    if kind == FR_SKIP { return; }
    let p = b.top();
    let ar = &mut (*b.d).arena as *mut Arena;
    let t = arena_alloc(ar, core::mem::size_of::<Node>() as u32) as *mut Node;
    (*t).ty = 1;
    (*t).text = textdup(ar, text);
    append(p, t);
}
unsafe fn new_doc(url: *const c_char) -> *mut Doc {
    let d = kzalloc(core::mem::size_of::<Doc>()) as *mut Doc;
    if d.is_null() { return d; }
    put(&mut (*d).base, cs(url as *const u8));
    (*d).root = arena_alloc(&mut (*d).arena, core::mem::size_of::<Node>() as u32) as *mut Node;
    (&mut (*(*d).root).tag)[..4].copy_from_slice(b"#doc");
    d
}
unsafe fn builder(d: *mut Doc, cssdoc: *mut Doc, url: *const c_char, in_body: bool) -> Option<Box<Builder>> {
    let mut st = Vec::new();
    if st.try_reserve_exact(MAX_DEPTH).is_err() { return None; }
    st.push(((*d).root, FR_NODE));
    Some(Box::new(Builder { d, cssdoc, url: if url.is_null() { c"".as_ptr() } else { url }, st, overflow: 0, buf: Vec::new(), in_body }))
}

// ---------------------------------------------------------------- the C interface

/// doc_t *html_parse(const char *src, uint32_t len, const char *charset, const char *url)
#[no_mangle]
pub unsafe extern "C" fn html_parse(src: *const u8, len: u32, charset: *const c_char, url: *const c_char) -> *mut Doc {
    let d = new_doc(url);
    if d.is_null() { return d; }
    let mut text = bytes(src, len);
    // character set: header, then <meta>, default UTF-8
    let header = if charset.is_null() { None } else { Some(cs(charset as *const u8)) };
    let mut conv = Vec::new();
    if !css::is_utf8(text, header) && conv.try_reserve_exact(text.len() * 3 + 1).is_ok() {
        css::utf8_from_1252(text, |b| conv.push(b));
        text = &conv;
    }
    if let Some(mut b) = builder(d, d, url, false) {
        let sink = LxgSink { ctx: &mut *b as *mut Builder as *mut c_void, start: sink_start, end: sink_end, text: sink_text };
        if lxg_parse_html(text.as_ptr(), text.len() as c_uint, &sink) != 0 {
            klog(c"xplorerlite: the HTML parser failed on %s".as_ptr(), if url.is_null() { c"a page".as_ptr() } else { url });
        }
    }
    drop(conv);
    refilter_vars(d);
    style_root(d);
    apply_css(d, (*(*d).root).first);
    d
}
/// innerHTML: nodes go into a temporary document (copied by the caller), while <style>
/// rules are added to the page's own document.
#[no_mangle]
pub unsafe extern "C" fn html_parse_fragment(page: *mut Doc, context_tag: *const c_char, utf8: *const c_char, url: *const c_char) -> *mut Doc {
    let d = new_doc(url);
    if d.is_null() { return d; }
    if let Some(mut b) = builder(d, if page.is_null() { d } else { page }, url, true) {
        let sink = LxgSink { ctx: &mut *b as *mut Builder as *mut c_void, start: sink_start, end: sink_end, text: sink_text };
        let ctx = cs(context_tag as *const u8);
        let ctx_name: *const u8 = if ctx == b"#doc" { b"body\0".as_ptr() } else { context_tag as *const u8 };
        let s = cs(utf8 as *const u8);
        lxg_parse_fragment(ctx_name, s.as_ptr(), s.len() as c_uint, &sink);
    }
    d
}
#[no_mangle]
pub unsafe extern "C" fn doc_free(d: *mut Doc) {
    if d.is_null() { return; }
    if !(*d).css.is_null() {
        let mut sh = Box::from_raw((*d).css);
        sh.set.clear();
        (*d).css = null_mut();
    }
    arena_free(&mut (*d).arena);
    kfree(d as *mut c_void);
}
/// const char *node_attr(node_t *n, const char *name)
#[no_mangle]
pub unsafe extern "C" fn node_attr(n: *mut Node, name: *const c_char) -> *const c_char {
    let want = cs(name as *const u8);
    for i in 0..(*n).nattr.max(0) as usize {
        if cs(*(*n).attr.add(i * 2)) == want { return *(*n).attr.add(i * 2 + 1) as *const c_char; }
    }
    null()
}
/// The document's root node (doc_t.root), for svg.rs and dom.rs.
pub unsafe fn doc_root(d: *mut Doc) -> *mut Node { (*d).root }
/// The document's arena, base URL (doc_t.base) and style rule count, for dom.rs.
pub unsafe fn doc_arena(d: *mut Doc) -> *mut Arena { &mut (*d).arena }
pub unsafe fn doc_base(d: *mut Doc) -> *const c_char { (*d).base.as_ptr() as *const c_char }
pub unsafe fn doc_nrules(d: *mut Doc) -> c_int { (*d).nrules }
/// bool parse_color(const char *s, uint32_t *out)
#[no_mangle]
pub unsafe extern "C" fn parse_color(s: *const c_char, out: *mut u32) -> CBool {
    match css::parse_color(cs(s as *const u8)) { Some(c) => { *out = c; 1 } None => 0 }
}
/// int utf8_normalise(const uint8_t *s, uint32_t n, char *out): out holds n * 3 + 1 bytes.
#[no_mangle]
pub unsafe extern "C" fn utf8_normalise(s: *const u8, n: u32, out: *mut u8) -> c_int {
    let mut o = 0usize;
    css::utf8_normalise(bytes(s, n), |b| { *out.add(o) = b; o += 1; });
    *out.add(o) = 0;
    o as c_int
}
/// Re-apply CSS to n and its subtree.
#[no_mangle]
pub unsafe extern "C" fn html_restyle(d: *mut Doc, n: *mut Node) {
    if n.is_null() || (*n).ty != 0 { return; }
    if (*n).parent.is_null() {
        style_root(d);
        let f = (*n).first;
        if f.is_null() { return; }
        apply_css(d, f);
        return;
    }
    let nx = (*n).next;
    (*n).next = null_mut();
    apply_css(d, n);
    (*n).next = nx;
}
#[no_mangle]
pub unsafe extern "C" fn html_restyle_all(d: *mut Doc) { style_root(d); apply_css(d, (*(*d).root).first); }
/// A downloaded <link> sheet: its variables may be used by sheets parsed before it, so every
/// sheet's rules are parsed again, in document order.
#[no_mangle]
pub unsafe extern "C" fn html_add_stylesheet(d: *mut Doc, text: *const u8, len: u32, sheet: c_int) {
    store_sheet(d, bytes(text, len), sheet);
    reparse_sheets(d);
}

// querySelector() and matches(): selectors parsed once and kept
const SEL_CACHE: usize = 64;
struct SelEntry { text: Vec<u8>, sels: Vec<*mut c_void>, n: c_int }
static mut SELCACHE: Vec<SelEntry> = Vec::new();
static mut SELPOOL: *mut c_void = null_mut();
unsafe extern "C" fn collect_sel(ctx: *mut c_void, sel: *mut c_void, _: c_uint, _: c_int, _: *const u8, _: c_uint) {
    let l = &mut *(ctx as *mut Vec<*mut c_void>);
    if l.try_reserve(1).is_ok() { l.push(sel); }
}
/// The selectors of a list (cached), or -1 if it is invalid.
unsafe fn selcache_get(sel: &[u8]) -> (c_int, *const *mut c_void) {
    let cache = &mut *core::ptr::addr_of_mut!(SELCACHE);
    if let Some(e) = cache.iter().find(|e| e.text == sel) { return (e.n, e.sels.as_ptr()); }
    if SELPOOL.is_null() || cache.len() >= SEL_CACHE {     // full: start again with a fresh pool
        cache.clear();
        if !SELPOOL.is_null() { lxg_pool_free(SELPOOL); }
        SELPOOL = lxg_pool_new();
    }
    let mut l: Vec<*mut c_void> = Vec::new();
    let r = lxg_selectors(SELPOOL, sel.as_ptr(), sel.len() as c_uint, collect_sel, &mut l as *mut Vec<*mut c_void> as *mut c_void);
    let n = if r < 0 { -1 } else { l.len() as c_int };
    let mut text = Vec::new();
    if cache.try_reserve(1).is_err() || text.try_reserve_exact(sel.len()).is_err() { return (-1, null()); }
    text.extend_from_slice(sel);
    cache.push(SelEntry { text, sels: l, n });
    let e = &cache[cache.len() - 1];
    (e.n, e.sels.as_ptr())
}
/// int html_matches(node_t *n, const char *sel): 1 match, 0 no, -1 invalid selector
#[no_mangle]
pub unsafe extern "C" fn html_matches(n: *mut Node, sel: *const c_char) -> c_int {
    if (*n).ty != 0 { return 0; }
    let (k, v) = selcache_get(cs(sel as *const u8));
    if k < 0 { return -1; }
    for i in 0..k as usize { if lxg_match(*v.add(i), n as *mut c_void, &TREE_OPS) != 0 { return 1; } }
    0
}
#[no_mangle]
pub unsafe extern "C" fn html_selector_valid(sel: *const c_char) -> c_int { (selcache_get(cs(sel as *const u8)).0 >= 0) as c_int }
#[no_mangle]
pub unsafe extern "C" fn html_parser_version() -> *const c_char { lxg_version() }
/// void css_parse_decls(const char *s, int n, css_t *out)
#[no_mangle]
pub unsafe extern "C" fn css_parse_decls(s: *const u8, n: c_int, out: *mut Css) {
    css::decls(bytes(s, n.max(0) as c_uint), &mut *out, &css::NoVars);
}
#[no_mangle]
pub unsafe extern "C" fn html_is_void(t: *const c_char) -> CBool { css::is_void(cs(t as *const u8)) as CBool }
