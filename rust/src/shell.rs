//! The shell language (`kernel/shk.rs` runs it; was `src/shell/sh.c`): the tokenizer, the
//! recursive-descent parser to a syntax tree, `$((..))` arithmetic, wildcard matching, word
//! expansion ($var, ${..} operators, $(..), `..`, ~, globs, field splitting) through a small
//! environment trait, and the text logic of the built-in commands. Strings are bytes, as in C;
//! numbers are C `long` (`isize`: 32-bit on the 32-bit edition, 64-bit on the 64-bit one).
//!
//! Changes from the C version: `$((..))` no longer skips the character after it; deep nesting
//! (parentheses, blocks, arithmetic) is refused instead of overflowing the stack; wildcard
//! matching does not recurse; `${v#pat}` and `${v%pat}` work on values of any length.

use alloc::rc::Rc;
use alloc::vec::Vec;

pub type Long = isize;

/// Nesting allowed in the parser and in arithmetic before giving up (the C version recursed
/// without limit and could overflow the task's stack).
const MAX_NEST: u32 = 200;

fn is_space(c: u8) -> bool { matches!(c, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c) }
fn at(s: &[u8], i: usize) -> u8 { s.get(i).copied().unwrap_or(0) }
fn lower(c: u8) -> u8 { c.to_ascii_lowercase() }
/// The C string in `s` (up to its first NUL).
pub fn cstr(s: &[u8]) -> &[u8] { &s[..s.iter().position(|&c| c == 0).unwrap_or(s.len())] }

/// The kernel's strtoul: the value and where parsing stopped (after any blanks, '+' and a 0x).
pub fn strtoul_end(s: &[u8], base: u32) -> (usize, usize) {
    let mut i = 0;
    while is_space(at(s, i)) { i += 1; }
    if at(s, i) == b'+' { i += 1; }
    let mut base = base;
    if (base == 0 || base == 16) && at(s, i) == b'0' && matches!(at(s, i + 1), b'x' | b'X') { i += 2; base = 16; }
    else if base == 0 && at(s, i) == b'0' && at(s, i + 1) != 0 { base = 8; }
    else if base == 0 { base = 10; }
    let mut v: usize = 0;
    loop {
        let c = at(s, i);
        let d = match c { b'0'..=b'9' => c - b'0', b'a'..=b'z' => c - b'a' + 10, b'A'..=b'Z' => c - b'A' + 10, _ => break } as u32;
        if d >= base { break; }
        v = v.wrapping_mul(base as usize).wrapping_add(d as usize);
        i += 1;
    }
    (v, i)
}
/// The kernel's strtol (blanks, then '-', then strtoul).
pub fn strtol_end(s: &[u8], base: u32) -> (Long, usize) {
    let mut i = 0;
    while is_space(at(s, i)) { i += 1; }
    let neg = at(s, i) == b'-';
    if neg { i += 1; }
    let (v, n) = strtoul_end(&s[i.min(s.len())..], base);
    let v = v as Long;
    (if neg { v.wrapping_neg() } else { v }, i + n)
}
pub fn strtol(s: &[u8], base: u32) -> Long { strtol_end(s, base).0 }
pub fn atoi(s: &[u8]) -> i32 { strtol(s, 10) as i32 }
/// "%ld"
pub fn ltoa(v: Long) -> Vec<u8> { alloc::format!("{}", v).into_bytes() }

// ---------------------------------------------------------------- tokens

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Tok { Word(Vec<u8>), Semi, Nl, And, Or, Pipe, Amp, Gt, GtGt, Lt, Lp, Rp, DSemi, TwoGt, TwoTo1, Eof }

/// After `p`'s opening character: the offset of the matching close, skipping quotes.
pub fn scan_balanced(p: &[u8], open: u8, close: u8) -> Option<usize> {
    let mut depth = 1;
    let mut i = 0;
    while i < p.len() && p[i] != 0 {
        let c = p[i];
        if c == b'\\' && at(p, i + 1) != 0 { i += 2; continue; }
        if c == b'\'' { i += 1; while at(p, i) != 0 && p[i] != b'\'' { i += 1; } if at(p, i) != 0 { i += 1; } continue; }
        if c == b'"' {
            i += 1;
            while at(p, i) != 0 && p[i] != b'"' { if p[i] == b'\\' && at(p, i + 1) != 0 { i += 1; } i += 1; }
            if at(p, i) != 0 { i += 1; }
            continue;
        }
        if c == open { depth += 1; }
        else if c == close { depth -= 1; if depth == 0 { return Some(i); } }
        i += 1;
    }
    None
}

/// The tokens of `src`, or None when a quote, $(..) or ${..} is not closed (more input needed).
pub fn tokenize(src: &[u8]) -> Option<Vec<Tok>> {
    let src = cstr(src);
    let mut t = Vec::new();
    let mut p = 0;
    while p < src.len() {
        let c = src[p];
        let n = at(src, p + 1);
        match c {
            b' ' | b'\t' | b'\r' => { p += 1; continue; }
            b'\\' if n == b'\n' => { p += 2; continue; }
            b'#' => { while p < src.len() && src[p] != b'\n' { p += 1; } continue; }
            b'\n' => { t.push(Tok::Nl); p += 1; continue; }
            b';' => { if n == b';' { t.push(Tok::DSemi); p += 2; } else { t.push(Tok::Semi); p += 1; } continue; }
            b'&' => { if n == b'&' { t.push(Tok::And); p += 2; } else { t.push(Tok::Amp); p += 1; } continue; }
            b'|' => { if n == b'|' { t.push(Tok::Or); p += 2; } else { t.push(Tok::Pipe); p += 1; } continue; }
            b'>' => { if n == b'>' { t.push(Tok::GtGt); p += 2; } else { t.push(Tok::Gt); p += 1; } continue; }
            b'<' => { t.push(Tok::Lt); p += 1; continue; }
            b'(' => { t.push(Tok::Lp); p += 1; continue; }
            b')' => { t.push(Tok::Rp); p += 1; continue; }
            b'2' if n == b'>' => {
                if at(src, p + 2) == b'&' && at(src, p + 3) == b'1' { t.push(Tok::TwoTo1); p += 4; } else { t.push(Tok::TwoGt); p += 2; }
                continue;
            }
            _ => {}
        }
        let st = p;
        while p < src.len() && !b" \t\r\n;&|<>()".contains(&src[p]) {
            let c = src[p];
            if c == b'\\' && at(src, p + 1) != 0 { p += 2; continue; }
            if c == b'\'' {
                p += 1;
                while p < src.len() && src[p] != b'\'' { p += 1; }
                if p >= src.len() { return None; }
                p += 1; continue;
            }
            if c == b'"' {
                p += 1;
                while p < src.len() && src[p] != b'"' {
                    if src[p] == b'\\' && at(src, p + 1) != 0 { p += 1; }
                    else if src[p] == b'$' && at(src, p + 1) == b'(' { p += scan_balanced(&src[p + 2..], b'(', b')')? + 2; }
                    p += 1;
                }
                if p >= src.len() { return None; }
                p += 1; continue;
            }
            if c == b'`' {
                p += 1;
                while p < src.len() && src[p] != b'`' { p += 1; }
                if p >= src.len() { return None; }
                p += 1; continue;
            }
            if c == b'$' && at(src, p + 1) == b'(' { p += scan_balanced(&src[p + 2..], b'(', b')')? + 3; continue; }
            if c == b'$' && at(src, p + 1) == b'{' { p += scan_balanced(&src[p + 2..], b'{', b'}')? + 3; continue; }
            p += 1;
        }
        t.push(Tok::Word(src[st..p.min(src.len())].to_vec()));
    }
    t.push(Tok::Eof);
    Some(t)
}

// ---------------------------------------------------------------- syntax tree

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RedirKind { Out, Append, In, Err, ErrToOut }
#[derive(Debug, Clone)]
pub struct Redir { pub kind: RedirKind, pub target: Vec<u8> }
pub type Sub = Option<alloc::boxed::Box<Node>>;
#[derive(Debug)]
pub enum Node {
    Simple { words: Vec<Vec<u8>>, redirs: Vec<Redir> },
    Pipe(Vec<Node>),
    And(alloc::boxed::Box<Node>, alloc::boxed::Box<Node>),
    Or(alloc::boxed::Box<Node>, alloc::boxed::Box<Node>),
    /// a non-empty command list (an empty one is None where it may appear)
    List(Vec<Node>),
    If { cond: Sub, then: Sub, els: Sub },
    Loop { until: bool, cond: Sub, body: Sub },
    For { name: Vec<u8>, words: Vec<Vec<u8>>, body: Sub },
    Case { word: Vec<u8>, arms: Vec<(Vec<Vec<u8>>, Sub)> },
    /// shared, so a definition can run again (a loop) without losing its body
    Func { name: Vec<u8>, body: Option<Rc<Node>> },
    Group(Sub),
    Subshell(Sub),
    Not(Sub),
}

#[derive(Debug, PartialEq, Eq)]
pub enum ParseError { Incomplete, Syntax(Vec<u8>) }

struct Parser { t: Vec<Tok>, pos: usize, incomplete: bool, err: Option<Vec<u8>>, depth: u32 }
fn bx(n: Node) -> alloc::boxed::Box<Node> { alloc::boxed::Box::new(n) }
fn sub(n: Option<Node>) -> Sub { n.map(bx) }

impl Parser {
    fn peek(&self) -> &Tok { self.t.get(self.pos).unwrap_or(&Tok::Eof) }
    fn peek_at(&self, k: usize) -> &Tok { self.t.get(self.pos + k).unwrap_or(&Tok::Eof) }
    fn word(&self) -> Option<&[u8]> { if let Tok::Word(w) = self.peek() { Some(w) } else { None } }
    fn is_kw(&self, kw: &[u8]) -> bool { self.word() == Some(kw) }
    fn skip_nl(&mut self) { while matches!(self.peek(), Tok::Nl | Tok::Semi) { self.pos += 1; } }
    fn skip_newlines(&mut self) { while *self.peek() == Tok::Nl { self.pos += 1; } }
    fn set_err(&mut self, m: &[u8]) { if self.err.is_none() { self.err = Some(m.to_vec()); } }
    fn failed(&self) -> bool { self.incomplete || self.err.is_some() }
    fn expect_kw(&mut self, kw: &[u8]) -> bool {
        self.skip_nl();
        if self.is_kw(kw) { self.pos += 1; return true; }
        if *self.peek() == Tok::Eof { self.incomplete = true; }
        else { let mut m = b"syntax error: expected '".to_vec(); m.extend_from_slice(kw); m.push(b'\''); self.set_err(&m); }
        false
    }
    fn is_terminator(&self) -> bool {
        match self.peek() {
            Tok::Eof | Tok::Rp | Tok::DSemi => true,
            Tok::Word(w) => [&b"then"[..], b"elif", b"else", b"fi", b"do", b"done", b"esac", b"}"].contains(&w.as_slice()),
            _ => false,
        }
    }
    fn compound_body(&mut self, endkw: &[u8]) -> Result<Sub, ()> {
        let b = self.list();
        if !self.expect_kw(endkw) { return Err(()); }
        Ok(sub(b))
    }
    fn command(&mut self) -> Option<Node> {
        self.depth += 1;
        if self.depth > MAX_NEST { self.set_err(b"syntax error: nested too deeply"); self.depth -= 1; return None; }
        let r = self.command_inner();
        self.depth -= 1;
        r
    }
    fn command_inner(&mut self) -> Option<Node> {
        if let Some(w) = self.word().map(|w| w.to_vec()) {
            match w.as_slice() {
                b"!" => { self.pos += 1; let a = self.command(); return Some(Node::Not(sub(a))); }
                b"if" => return self.if_rest(),
                b"while" | b"until" => {
                    let until = w == b"until";
                    self.pos += 1;
                    let cond = self.list();
                    if !self.expect_kw(b"do") { return None; }
                    let body = self.compound_body(b"done").ok()?;
                    return Some(Node::Loop { until, cond: sub(cond), body });
                }
                b"for" => {
                    self.pos += 1;
                    let Some(name) = self.word().map(|w| w.to_vec()) else { self.set_err(b"syntax error after 'for'"); return None };
                    self.pos += 1;
                    self.skip_newlines();
                    let mut words = Vec::new();
                    if self.is_kw(b"in") { self.pos += 1; while let Some(w) = self.word() { words.push(w.to_vec()); self.pos += 1; } }
                    else { words.push(b"\"$@\"".to_vec()); }
                    if !self.expect_kw(b"do") { return None; }
                    let body = self.compound_body(b"done").ok()?;
                    return Some(Node::For { name, words, body });
                }
                b"case" => {
                    self.pos += 1;
                    let Some(word) = self.word().map(|w| w.to_vec()) else { self.set_err(b"syntax error after 'case'"); return None };
                    self.pos += 1;
                    if !self.expect_kw(b"in") { return None; }
                    let mut arms = Vec::new();
                    loop {
                        self.skip_nl();
                        if self.is_kw(b"esac") { self.pos += 1; break; }
                        if *self.peek() == Tok::Eof { self.incomplete = true; return None; }
                        if *self.peek() == Tok::Lp { self.pos += 1; }
                        let mut pats = Vec::new();
                        while let Some(w) = self.word() {
                            pats.push(w.to_vec());
                            self.pos += 1;
                            if *self.peek() == Tok::Pipe { self.pos += 1; } else { break; }
                        }
                        if *self.peek() != Tok::Rp { self.set_err(b"syntax error in case pattern"); return None; }
                        self.pos += 1;
                        let body = self.list();
                        if self.failed() { return None; }
                        arms.push((pats, sub(body)));
                        self.skip_newlines();
                        if *self.peek() == Tok::DSemi { self.pos += 1; }
                    }
                    return Some(Node::Case { word, arms });
                }
                b"{" => { self.pos += 1; let a = self.compound_body(b"}").ok()?; return Some(Node::Group(a)); }
                b"function" => {
                    self.pos += 1;
                    let name = self.word().map(|w| w.to_vec()).unwrap_or_else(|| b"f".to_vec());
                    if *self.peek() != Tok::Eof { self.pos += 1; }
                    if *self.peek() == Tok::Lp && *self.peek_at(1) == Tok::Rp { self.pos += 2; }
                    self.skip_newlines();
                    let body = self.command();
                    return Some(Node::Func { name, body: body.map(Rc::new) });
                }
                _ => {}
            }
            if *self.peek_at(1) == Tok::Lp && *self.peek_at(2) == Tok::Rp {
                self.pos += 3;
                self.skip_newlines();
                let body = self.command()?;
                return Some(Node::Func { name: w, body: Some(Rc::new(body)) });
            }
        }
        if *self.peek() == Tok::Lp {
            self.pos += 1;
            let a = self.list();
            if *self.peek() != Tok::Rp { if *self.peek() == Tok::Eof { self.incomplete = true; } return None; }
            self.pos += 1;
            return Some(Node::Subshell(sub(a)));
        }
        let mut words = Vec::new();
        let mut redirs = Vec::new();
        loop {
            let kind = match self.peek() {
                Tok::Word(w) => {
                    if words.is_empty() && self.is_terminator() { break; }
                    words.push(w.clone()); self.pos += 1; continue;
                }
                Tok::Gt => RedirKind::Out, Tok::GtGt => RedirKind::Append, Tok::Lt => RedirKind::In, Tok::TwoGt => RedirKind::Err,
                Tok::TwoTo1 => { self.pos += 1; redirs.push(Redir { kind: RedirKind::ErrToOut, target: Vec::new() }); continue; }
                _ => break,
            };
            self.pos += 1;
            let Some(t) = self.word().map(|w| w.to_vec()) else { self.set_err(b"syntax error: missing redirection target"); return None };
            redirs.push(Redir { kind, target: t });
            self.pos += 1;
        }
        if words.is_empty() && redirs.is_empty() { return None; }
        Some(Node::Simple { words, redirs })
    }
    /// at "if": cond, then, and elif (a nested if) / else
    fn if_rest(&mut self) -> Option<Node> {
        self.pos += 1;
        let cond = self.list();
        if !self.expect_kw(b"then") { return None; }
        let then = self.list();
        self.skip_nl();
        if self.is_kw(b"elif") {
            self.depth += 1;
            if self.depth > MAX_NEST { self.set_err(b"syntax error: nested too deeply"); return None; }
            let e = self.if_rest();
            self.depth -= 1;
            let e = e?;
            return Some(Node::If { cond: sub(cond), then: sub(then), els: Some(bx(e)) });
        }
        let mut els = None;
        if self.is_kw(b"else") { self.pos += 1; els = self.list(); }
        if !self.expect_kw(b"fi") { return None; }
        Some(Node::If { cond: sub(cond), then: sub(then), els: sub(els) })
    }
    fn pipeline(&mut self) -> Option<Node> {
        let first = self.command()?;
        if *self.peek() != Tok::Pipe { return Some(first); }
        let mut v = alloc::vec![first];
        while *self.peek() == Tok::Pipe {
            self.pos += 1;
            self.skip_newlines();
            match self.command() {
                Some(c) => v.push(c),
                None => { if *self.peek() == Tok::Eof { self.incomplete = true; } return None; }
            }
        }
        Some(Node::Pipe(v))
    }
    fn andor(&mut self) -> Option<Node> {
        let mut l = self.pipeline()?;
        while matches!(self.peek(), Tok::And | Tok::Or) {
            let and = *self.peek() == Tok::And;
            self.pos += 1;
            self.skip_newlines();
            let Some(r) = self.pipeline() else { if *self.peek() == Tok::Eof { self.incomplete = true; } return None };
            l = if and { Node::And(bx(l), bx(r)) } else { Node::Or(bx(l), bx(r)) };
        }
        Some(l)
    }
    fn list(&mut self) -> Option<Node> {
        let mut items = Vec::new();
        loop {
            self.skip_nl();
            while *self.peek() == Tok::Amp { self.pos += 1; }
            if self.is_terminator() || self.failed() { break; }
            let before = self.pos;
            match self.andor() {
                Some(n) => items.push(n),
                None => {
                    if self.failed() { break; }
                    if self.pos == before { self.set_err(b"syntax error near unexpected token"); break; }
                }
            }
        }
        if items.is_empty() { None } else { Some(Node::List(items)) }
    }
}

/// The syntax tree of a script (None when it has no commands).
pub fn parse(src: &[u8]) -> Result<Option<Node>, ParseError> {
    let Some(t) = tokenize(src) else { return Err(ParseError::Incomplete) };
    let mut p = Parser { t, pos: 0, incomplete: false, err: None, depth: 0 };
    let n = p.list();
    if p.incomplete { return Err(ParseError::Incomplete); }
    if let Some(e) = p.err { return Err(ParseError::Syntax(e)); }
    if *p.peek() != Tok::Eof {
        let mut m = b"syntax error near unexpected token '".to_vec();
        match p.peek() { Tok::Word(w) => m.extend_from_slice(w), Tok::Rp => m.push(b')'), Tok::DSemi => m.extend_from_slice(b";;"), _ => m.push(b'?') }
        m.push(b'\'');
        return Err(ParseError::Syntax(m));
    }
    Ok(n)
}

// ---------------------------------------------------------------- environment

/// What expansion needs from the running shell.
pub trait Env {
    /// a shell variable
    fn var(&self, name: &[u8]) -> Option<Vec<u8>>;
    fn set_var(&mut self, name: &[u8], val: &[u8]);
    /// $?, $#, $$, $@, $*, $0..$9, RANDOM, SECONDS and PWD (None for any other name)
    fn special(&mut self, name: &[u8]) -> Option<Vec<u8>>;
    /// the output of a command, trailing line breaks removed
    fn cmd_subst(&mut self, cmd: &[u8]) -> Vec<u8>;
    /// the names in a folder ("" is the current one), sorted as Explorer lists them; None if
    /// it is not a folder
    fn dir_names(&mut self, dir: &[u8]) -> Option<Vec<Vec<u8>>>;
    fn home(&self) -> Vec<u8>;
}

// ---------------------------------------------------------------- arithmetic

struct Arith<'a, E: Env + ?Sized> { s: &'a [u8], p: usize, env: &'a mut E, depth: u32 }
impl<E: Env + ?Sized> Arith<'_, E> {
    fn c(&self, k: usize) -> u8 { at(self.s, self.p + k) }
    fn ws(&mut self) { while is_space(self.c(0)) { self.p += 1; } }
    fn primary(&mut self) -> Long {
        self.ws();
        self.depth += 1;
        if self.depth > MAX_NEST { self.p = self.s.len(); self.depth -= 1; return 0; }
        let r = self.primary_inner();
        self.depth -= 1;
        r
    }
    fn primary_inner(&mut self) -> Long {
        match self.c(0) {
            b'(' => { self.p += 1; let v = self.expr(); self.ws(); if self.c(0) == b')' { self.p += 1; } return v; }
            b'-' => { self.p += 1; return self.primary().wrapping_neg(); }
            b'+' => { self.p += 1; return self.primary(); }
            b'!' => { self.p += 1; return (self.primary() == 0) as Long; }
            b'~' => { self.p += 1; return !self.primary(); }
            c if c.is_ascii_digit() => { let (v, n) = strtol_end(&self.s[self.p..], 0); self.p += n; return v; }
            _ => {}
        }
        if self.c(0) == b'$' { self.p += 1; }
        let c = self.c(0);
        if c.is_ascii_alphabetic() || c == b'_' {
            let st = self.p;
            while (self.c(0).is_ascii_alphanumeric() || self.c(0) == b'_') && self.p - st < 63 { self.p += 1; }
            let name = &self.s[st..self.p];
            return self.env.var(name).map(|v| strtol(&v, 0)).unwrap_or(0);
        }
        0
    }
    fn bin(&mut self, prec: u8) -> Long {
        let mut l = self.primary();
        loop {
            self.ws();
            let (a, b) = (self.c(0), self.c(1));
            let (op, pr, len): (u8, u8, usize) = match (a, b) {
                (b'|', b'|') => (b'o', 1, 2), (b'&', b'&') => (b'a', 2, 2), (b'|', _) => (b'|', 3, 1), (b'^', _) => (b'^', 4, 1),
                (b'&', _) => (b'&', 5, 1), (b'=', b'=') => (b'e', 6, 2), (b'!', b'=') => (b'n', 6, 2), (b'<', b'=') => (b'l', 7, 2),
                (b'>', b'=') => (b'g', 7, 2), (b'<', b'<') => (b'L', 8, 2), (b'>', b'>') => (b'R', 8, 2), (b'<', _) => (b'<', 7, 1),
                (b'>', _) => (b'>', 7, 1), (b'+', _) => (b'+', 9, 1), (b'-', _) => (b'-', 9, 1), (b'*', _) => (b'*', 10, 1),
                (b'/', _) => (b'/', 10, 1), (b'%', _) => (b'%', 10, 1), _ => return l,
            };
            if pr < prec { return l; }
            self.p += len;
            let r = self.bin(pr + 1);
            l = match op {
                b'o' => (l != 0 || r != 0) as Long, b'a' => (l != 0 && r != 0) as Long,
                b'|' => l | r, b'^' => l ^ r, b'&' => l & r,
                b'e' => (l == r) as Long, b'n' => (l != r) as Long, b'l' => (l <= r) as Long, b'g' => (l >= r) as Long,
                b'<' => (l < r) as Long, b'>' => (l > r) as Long,
                b'L' => l.wrapping_shl(r as u32), b'R' => l.wrapping_shr(r as u32),
                b'+' => l.wrapping_add(r), b'-' => l.wrapping_sub(r), b'*' => l.wrapping_mul(r),
                b'/' => if r != 0 { l.wrapping_div(r) } else { 0 },
                _ => if r != 0 { l.wrapping_rem(r) } else { 0 },
            };
        }
    }
    fn expr(&mut self) -> Long {
        let v = self.bin(1);
        self.ws();
        if self.c(0) == b'?' {
            self.p += 1;
            let x = self.expr();
            self.ws();
            if self.c(0) == b':' { self.p += 1; }
            let y = self.expr();
            return if v != 0 { x } else { y };
        }
        v
    }
}

/// $((..)), expr and let: an arithmetic expression, with the assignment forms name=expr,
/// name+=expr (-= *= /=), name++ and name--.
pub fn arith<E: Env + ?Sized>(env: &mut E, e: &[u8]) -> Long {
    let e = cstr(e);
    let mut p = 0;
    while is_space(at(e, p)) { p += 1; }
    let mut n = 0;
    while (at(e, p + n).is_ascii_alphanumeric() || at(e, p + n) == b'_') && n < 63 { n += 1; }
    if n > 0 && !e[p].is_ascii_digit() {
        let name = e[p..p + n].to_vec();
        let mut q = p + n;
        while is_space(at(e, q)) { q += 1; }
        let (q0, q1) = (at(e, q), at(e, q + 1));
        let cur = || env_long(env_ref(env), &name);
        if (q0 == b'+' && q1 == b'+') || (q0 == b'-' && q1 == b'-') {
            let c = cur();
            env.set_var(&name, &ltoa(c.wrapping_add(if q0 == b'+' { 1 } else { -1 })));
            return c;
        }
        if (q0 == b'=' && q1 != b'=') || (matches!(q0, b'+' | b'-' | b'*' | b'/') && q1 == b'=') {
            let start = q + if q0 == b'=' { 1 } else { 2 };
            let mut a = Arith { s: e, p: start, env: &mut *env, depth: 0 };
            let mut r = a.expr();
            let c = env_long(env, &name);
            r = match (q0, q1) {
                (b'+', _) => c.wrapping_add(r),
                (b'-', b'=') => c.wrapping_sub(r),
                (b'*', _) => c.wrapping_mul(r),
                (b'/', _) => if r != 0 { c.wrapping_div(r) } else { 0 },
                _ => r,
            };
            env.set_var(&name, &ltoa(r));
            return r;
        }
    }
    let mut a = Arith { s: e, p: 0, env, depth: 0 };
    a.expr()
}
fn env_ref<E: Env + ?Sized>(e: &E) -> &E { e }
fn env_long<E: Env + ?Sized>(e: &E, name: &[u8]) -> Long { e.var(name).map(|v| strtol(&v, 0)).unwrap_or(0) }

// ---------------------------------------------------------------- wildcards

/// Shell pattern matching (* ? [a-z] [!..]), case-insensitive as on Windows.
pub fn glob_match(p: &[u8], s: &[u8]) -> bool {
    let (p, s) = (cstr(p), cstr(s));
    let (mut pi, mut si) = (0usize, 0usize);
    let mut star: Option<(usize, usize)> = None;
    loop {
        if pi < p.len() {
            let c = p[pi];
            if c == b'*' { star = Some((pi, si)); pi += 1; continue; }
            if si < s.len() {
                if c == b'[' {
                    if let Some(e) = p[pi + 1..].iter().position(|&x| x == b']').map(|k| pi + 1 + k) {
                        let neg = matches!(at(p, pi + 1), b'!' | b'^');
                        let ch = lower(s[si]);
                        let mut ok = false;
                        let mut q = pi + 1 + neg as usize;
                        while q < e {
                            if at(p, q + 1) == b'-' && q + 2 < e { if ch >= lower(p[q]) && ch <= lower(p[q + 2]) { ok = true; } q += 3; }
                            else { if lower(p[q]) == ch { ok = true; } q += 1; }
                        }
                        if ok != neg { pi = e + 1; si += 1; continue; }
                    } else if lower(c) == lower(s[si]) { pi += 1; si += 1; continue; }
                } else if c == b'?' || lower(c) == lower(s[si]) { pi += 1; si += 1; continue; }
            }
        } else if si == s.len() { return true; }
        // mismatch: let the last * take one more character
        match star {
            Some((sp, ss)) if ss < s.len() => { star = Some((sp, ss + 1)); pi = sp + 1; si = ss + 1; }
            _ => return false,
        }
    }
}

// ---------------------------------------------------------------- expansion

/// ${expr} (or a bare name): the value with its operator applied.
pub fn expand_param<E: Env + ?Sized>(env: &mut E, expr: &[u8], out: &mut Vec<u8>) {
    let expr = cstr(expr);
    let mut i = 0;
    let len = at(expr, 0) == b'#' && at(expr, 1) != 0;
    if len { i = 1; }
    let st = i;
    while i - st < 63 {
        let c = at(expr, i);
        if !(c.is_ascii_alphanumeric() || c == b'_' || (i == st && c != 0 && b"?#$@*!".contains(&c))) { break; }
        i += 1;
        if i - st == 1 && !c.is_ascii_alphabetic() && c != b'_' && b"?#$@*!0123456789".contains(&c) { break; }
    }
    let name = &expr[st..i];
    let rest = &expr[i..];
    let val: Option<Vec<u8>> = match env.special(name) { Some(v) => Some(v), None => env.var(name) };
    if len { out.extend_from_slice(&ltoa(val.as_ref().map_or(0, |v| v.len()) as Long)); return; }
    let r0 = at(rest, 0);
    if r0 == b':' && matches!(at(rest, 1), b'-' | b'=' | b'+') {
        let empty = val.as_ref().map_or(true, |v| v.is_empty());
        let word = &rest[2..];
        match rest[1] {
            b'-' => out.extend_from_slice(if empty { word } else { val.as_deref().unwrap_or(b"") }),
            b'=' => { if empty { env.set_var(name, word); out.extend_from_slice(word); } else { out.extend_from_slice(val.as_deref().unwrap_or(b"")); } }
            _ => if !empty { out.extend_from_slice(word); },
        }
        return;
    }
    if r0 == b':' && at(rest, 1).is_ascii_digit() {
        let (off, e) = strtol_end(&rest[1..], 10);
        let after = &rest[1 + e..];
        let l = if at(after, 0) == b':' { strtol(&after[1..], 10) } else { -1 };
        if let Some(v) = &val {
            let off = (off.max(0) as usize).min(v.len());
            let st = &v[off..];
            let m = if l < 0 { st.len() } else { (l as usize).min(st.len()) };
            out.extend_from_slice(&st[..m]);
        }
        return;
    }
    if (r0 == b'#' || r0 == b'%') && val.is_some() {
        let v = val.unwrap();
        let longest = at(rest, 1) == r0;
        let pat = &rest[if longest { 2 } else { 1 }..];
        if r0 == b'#' {
            let mut best = None;
            for i in 0..=v.len() { if glob_match(pat, &v[..i]) { best = Some(i); if !longest { break; } } }
            out.extend_from_slice(&v[best.unwrap_or(0)..]);
        } else {
            let mut best = None;
            for i in (0..=v.len()).rev() { if glob_match(pat, &v[i..]) { best = Some(i); if !longest { break; } } }
            out.extend_from_slice(&v[..best.unwrap_or(v.len())]);
        }
        return;
    }
    if r0 == b'/' && val.is_some() {
        let v = val.unwrap();
        let mut pat = &rest[1..];
        let all = at(pat, 0) == b'/';
        if all { pat = &pat[1..]; }
        let (p2, rep): (&[u8], &[u8]) = match pat.iter().position(|&c| c == b'/') { Some(k) => (&pat[..k.min(127)], &pat[k + 1..]), None => (&pat[..pat.len().min(127)], b"") };
        let mut k = 0;
        while k < v.len() {
            if !p2.is_empty() && v[k..].starts_with(p2) {
                out.extend_from_slice(rep);
                k += p2.len();
                if !all { out.extend_from_slice(&v[k..]); break; }
            } else { out.push(v[k]); k += 1; }
        }
        return;
    }
    if let Some(v) = val { out.extend_from_slice(&v); }
}

/// The variable name after a '$' at `w[p]` (one special character or digit, or a name of up
/// to 63 characters), and where it ends.
fn dollar_name(w: &[u8], p: usize) -> (Vec<u8>, usize) {
    let c = at(w, p);
    if (c != 0 && b"?#$@*!".contains(&c)) || c.is_ascii_digit() { return (alloc::vec![c], p + 1); }
    let mut e = p;
    while (at(w, e).is_ascii_alphanumeric() || at(w, e) == b'_') && e - p < 63 { e += 1; }
    (w[p..e].to_vec(), e)
}
fn starts_var(c: u8) -> bool { c.is_ascii_alphanumeric() || c == b'_' || (c != 0 && b"?#$@*!".contains(&c)) }

/// One expansion at `w[p]` ('$' or '`'): what it produces and where it ends; None when the
/// '$' starts nothing.
fn expand_dollar<E: Env + ?Sized>(env: &mut E, w: &[u8], p: usize) -> Option<(Vec<u8>, usize)> {
    let mut tmp = Vec::new();
    if w[p] == b'`' {
        let e = w[p + 1..].iter().position(|&c| c == b'`').map_or(w.len(), |k| p + 1 + k);
        tmp = env.cmd_subst(&w[p + 1..e]);
        return Some((tmp, if e < w.len() { e + 1 } else { e }));
    }
    let (n1, n2) = (at(w, p + 1), at(w, p + 2));
    if n1 == b'(' && n2 == b'(' {
        if let Some(l) = scan_balanced(&w[p + 2..], b'(', b')') {
            // "$((" EXPR "))": l is the offset of the outer ')' from the second '('
            let inner_end = (p + 2 + l).saturating_sub(1).max(p + 3);
            let v = arith(env, &w[p + 3..inner_end]);
            return Some((ltoa(v), p + 2 + l + 1));
        }
        return None;
    }
    if n1 == b'(' {
        let l = scan_balanced(&w[p + 2..], b'(', b')')?;
        return Some((env.cmd_subst(&w[p + 2..p + 2 + l]), p + l + 3));
    }
    if n1 == b'{' {
        let l = scan_balanced(&w[p + 2..], b'{', b'}')?;
        expand_param(env, &w[p + 2..p + 2 + l], &mut tmp);
        return Some((tmp, p + l + 3));
    }
    if starts_var(n1) {
        let (name, e) = dollar_name(w, p + 1);
        expand_param(env, &name, &mut tmp);
        return Some((tmp, e));
    }
    None
}

/// One word expanded into fields: quotes removed, $.. and `..` expanded, and when `split`,
/// unquoted results split at blanks and unquoted wildcards matched against file names.
pub fn expand_word<E: Env + ?Sized>(env: &mut E, w: &[u8], out: &mut Vec<Vec<u8>>, split: bool) {
    let w = cstr(w);
    let mut cur: Vec<u8> = Vec::new();
    let (mut have, mut globbable, mut quoted_any) = (false, false, false);
    let mut p = 0;
    if at(w, 0) == b'~' && matches!(at(w, 1), 0 | b'/' | b'\\') { cur.extend_from_slice(&env.home()); have = true; p = 1; }
    while p < w.len() {
        let c = w[p];
        if c == b'\\' { if p + 1 < w.len() { cur.push(w[p + 1]); p += 2; } else { p += 1; } have = true; continue; }
        if c == b'\'' {
            p += 1;
            while p < w.len() && w[p] != b'\'' { cur.push(w[p]); p += 1; }
            if p < w.len() { p += 1; }
            have = true; quoted_any = true;
            continue;
        }
        if c == b'"' {
            p += 1; have = true; quoted_any = true;
            while p < w.len() && w[p] != b'"' {
                let d = w[p];
                if d == b'\\' && p + 1 < w.len() && b"\"\\$`".contains(&w[p + 1]) { cur.push(w[p + 1]); p += 2; continue; }
                if d == b'$' || d == b'`' {
                    match expand_dollar(env, w, p) {
                        Some((v, e)) => { cur.extend_from_slice(&v); p = e; }
                        None => { cur.push(b'$'); p += 1; }
                    }
                    continue;
                }
                cur.push(d); p += 1;
            }
            if p < w.len() { p += 1; }
            continue;
        }
        if c == b'$' || c == b'`' {
            match expand_dollar(env, w, p) {
                None => { cur.push(b'$'); p += 1; have = true; }
                Some((v, e)) => {
                    p = e;
                    if v.is_empty() { continue; }
                    if !split { cur.extend_from_slice(&v); have = true; continue; }
                    for &q in &v {
                        if q == b' ' || q == b'\t' || q == b'\n' {
                            if have { out.push(core::mem::take(&mut cur)); have = false; }
                        } else { cur.push(q); have = true; }
                    }
                }
            }
            continue;
        }
        if c == b'*' || c == b'?' || c == b'[' { globbable = true; }
        cur.push(c); have = true; p += 1;
    }
    if !have { return; }
    if globbable && split && !quoted_any {
        let sl = cur.iter().rposition(|&c| c == b'/' || c == b'\\');
        let (dir, pat) = match sl { Some(k) => (cur[..k + 1].to_vec(), cur[k + 1..].to_vec()), None => (Vec::new(), cur.clone()) };
        if let Some(names) = env.dir_names(&dir) {
            let before = out.len();
            for n in names {
                if glob_match(&pat, &n) && (at(&n, 0) != b'.' || at(&pat, 0) == b'.') {
                    let mut full = dir.clone();
                    full.extend_from_slice(&n);
                    out.push(full);
                }
            }
            if out.len() > before { return; }
        }
    }
    out.push(cur);
}
/// A word expanded without field splitting or wildcards (its first field, or "").
pub fn expand_single<E: Env + ?Sized>(env: &mut E, w: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    expand_word(env, w, &mut v, false);
    v.into_iter().next().unwrap_or_default()
}

// ---------------------------------------------------------------- built-in text logic

/// The lines of a text (a final line break adds no empty line).
pub fn lines(d: &[u8]) -> impl Iterator<Item = &[u8]> {
    let d = cstr(d);
    let mut p = 0;
    core::iter::from_fn(move || {
        if p >= d.len() { return None; }
        let e = d[p..].iter().position(|&c| c == b'\n');
        let l = e.unwrap_or(d.len() - p);
        let r = &d[p..p + l];
        p += l + e.is_some() as usize;
        Some(r)
    })
}

/// echo's output (-n, -e, -E).
pub fn echo(args: &[Vec<u8>]) -> Vec<u8> {
    let (mut nl, mut esc) = (true, false);
    let mut i = 1;
    while i < args.len() && at(&args[i], 0) == b'-' && args[i].len() > 1 {
        if !args[i][1..].iter().all(|f| b"neE".contains(f)) { break; }
        for &f in &args[i][1..] { match f { b'n' => nl = false, b'e' => esc = true, _ => esc = false } }
        i += 1;
    }
    let mut o = Vec::new();
    'all: for k in i..args.len() {
        if k > i { o.push(b' '); }
        let a = cstr(&args[k]);
        let mut p = 0;
        while p < a.len() {
            if esc && a[p] == b'\\' && p + 1 < a.len() {
                p += 1;
                match a[p] {
                    b'n' => o.push(b'\n'), b't' => o.push(b'\t'), b'e' | b'E' => o.push(27), b'\\' => o.push(b'\\'), b'a' => {}
                    b'c' => { nl = false; break 'all; }
                    b'0' => {
                        let mut v: u32 = 0;
                        let mut d = 0;
                        while d < 3 && matches!(at(a, p + 1), b'0'..=b'7') { p += 1; v = v * 8 + (a[p] - b'0') as u32; d += 1; }
                        o.push(v as u8);
                    }
                    x => { o.push(b'\\'); o.push(x); }
                }
            } else { o.push(a[p]); }
            p += 1;
        }
    }
    if nl { o.push(b'\n'); }
    o
}

/// One printf conversion as the kernel's vsnprintf formats it (src/kernel/lib.c): flags '-'
/// and '0' and a width; a precision only cuts strings; %c takes no width. The result is cut
/// at a NUL and at 255 bytes, as the C version's buffer did.
fn c_format(spec: &[u8], conv: u8, arg: &[u8]) -> Vec<u8> {
    let (mut left, mut zero, mut i) = (false, false, 0);
    while i < spec.len() && (spec[i] == b'-' || spec[i] == b'0') { if spec[i] == b'-' { left = true; } else { zero = true; } i += 1; }
    let mut width = 0usize;
    while i < spec.len() && spec[i].is_ascii_digit() { width = (width * 10 + (spec[i] - b'0') as usize).min(1000); i += 1; }
    let mut prec: Option<usize> = None;
    if i < spec.len() && spec[i] == b'.' {
        i += 1;
        let mut v = 0usize;
        while i < spec.len() && spec[i].is_ascii_digit() { v = (v * 10 + (spec[i] - b'0') as usize).min(1000); i += 1; }
        prec = Some(v);
    }
    let mut o = Vec::new();
    if i < spec.len() {
        // a character the formatter does not take here: it prints '%', it and the rest
        o.push(b'%'); o.extend_from_slice(&spec[i..]); o.push(conv);
    } else {
        match conv {
            b's' => {
                let l = prec.map_or(arg.len(), |p| p.min(arg.len()));
                if !left { o.resize(width.saturating_sub(l), b' '); }
                o.extend_from_slice(&arg[..l]);
                if left { o.resize(o.len() + width.saturating_sub(l), b' '); }
            }
            b'c' => o.push(at(arg, 0)),
            _ => {
                let n = strtol(arg, 0) as i32;
                let (neg, digits) = match conv {
                    b'd' | b'i' => (n < 0, alloc::format!("{}", (n as i64).unsigned_abs())),
                    b'u' => (false, alloc::format!("{}", n as u32)),
                    b'x' => (false, alloc::format!("{:x}", n as u32)),
                    b'X' => (false, alloc::format!("{:X}", n as u32)),
                    _ => (false, alloc::format!("{:o}", n as u32)),
                };
                let pad = width.saturating_sub(digits.len() + neg as usize);
                if !left && !zero { o.resize(pad, b' '); }
                if neg { o.push(b'-'); }
                if !left && zero { o.resize(o.len() + pad, b'0'); }
                o.extend_from_slice(digits.as_bytes());
                if left { o.resize(o.len() + pad, b' '); }
            }
        }
    }
    o.truncate(255);
    let l = cstr(&o).len();
    o.truncate(l);
    o
}
/// printf FORMAT ARGS...: the format is reused while arguments remain.
pub fn printf(args: &[Vec<u8>]) -> Vec<u8> {
    let mut o = Vec::new();
    if args.len() < 2 { return o; }
    let f = cstr(&args[1]);
    let mut ai = 2;
    'pass: loop {
        let mut p = 0;
        while p < f.len() {
            let c = f[p];
            if c == b'\\' && p + 1 < f.len() {
                p += 1;
                o.push(match f[p] { b'n' => b'\n', b't' => b'\t', b'e' => 27, x => x });
                p += 1; continue;
            }
            if c == b'%' && p + 1 < f.len() {
                p += 1;
                let st = p;
                while p < f.len() && (f[p] == b'-' || f[p] == b'0' || f[p].is_ascii_digit() || f[p] == b'.') && p - st < 11 { p += 1; }
                if p >= f.len() { o.push(b'%'); o.extend_from_slice(&f[st..]); break 'pass; }
                let conv = f[p];
                let spec = &f[st..p];
                p += 1;
                if conv == b'%' { o.push(b'%'); continue; }
                let a: &[u8] = if ai < args.len() { ai += 1; cstr(&args[ai - 1]) } else { b"" };
                match conv {
                    b'd' | b'i' | b'x' | b'X' | b'o' | b'u' | b'c' | b's' => o.extend_from_slice(&c_format(spec, conv, a)),
                    _ => {}
                }
                continue;
            }
            o.push(c); p += 1;
        }
        if !(ai < args.len() && f.contains(&b'%')) { break; }
    }
    o
}

/// What test needs to know about a path.
pub struct FileInfo { pub dir: bool, pub size: u32, pub readonly: bool }
/// test / [ / [[ (the arguments without the brackets).
pub fn test_eval(args: &[&[u8]], stat: &mut dyn FnMut(&[u8]) -> Option<FileInfo>) -> bool {
    test_rec(args, stat, 0)
}
fn test_rec(args: &[&[u8]], stat: &mut dyn FnMut(&[u8]) -> Option<FileInfo>, depth: u32) -> bool {
    if args.is_empty() || depth > 1000 { return false; }
    if args[0] == b"!" { return !test_rec(&args[1..], stat, depth + 1); }
    for i in 1..args.len().saturating_sub(1) {
        if args[i] == b"-a" { return test_rec(&args[..i], stat, depth + 1) && test_rec(&args[i + 1..], stat, depth + 1); }
        if args[i] == b"-o" { return test_rec(&args[..i], stat, depth + 1) || test_rec(&args[i + 1..], stat, depth + 1); }
    }
    match args.len() {
        1 => !args[0].is_empty(),
        2 => {
            let (op, a) = (args[0], args[1]);
            match op {
                b"-z" => a.is_empty(),
                b"-n" => !a.is_empty(),
                b"-e" | b"-r" | b"-x" => stat(a).is_some(),
                b"-f" => stat(a).is_some_and(|f| !f.dir),
                b"-d" => stat(a).is_some_and(|f| f.dir),
                b"-s" => stat(a).is_some_and(|f| f.size > 0),
                b"-w" => stat(a).is_some_and(|f| !f.readonly),
                _ => false,
            }
        }
        3 => {
            let (a, op, b) = (args[0], args[1], args[2]);
            let (x, y) = (strtol(a, 10), strtol(b, 10));
            match op {
                b"=" | b"==" => a == b, b"!=" => a != b,
                b"-eq" => x == y, b"-ne" => x != y, b"-lt" => x < y, b"-le" => x <= y, b"-gt" => x > y, b"-ge" => x >= y,
                b"<" => a < b, b">" => a > b,
                _ => false,
            }
        }
        _ => false,
    }
}

/// head / tail: the lines to print (n may be negative, as with atoi).
pub fn head_tail(d: &[u8], n: i32, tail: bool) -> Vec<u8> {
    let d = cstr(d);
    let mut nl = d.iter().filter(|&&c| c == b'\n').count() as i64;
    let unterminated = !d.is_empty() && d[d.len() - 1] != b'\n';
    if unterminated { nl += 1; }
    let n = n as i64;
    let (start, end) = if tail { ((nl - n).max(0), nl) } else { (0, n.min(nl)) };
    let mut o = Vec::new();
    let (mut ln, mut p) = (0i64, 0usize);
    while p < d.len() && ln < end {
        let l = d[p..].iter().position(|&c| c == b'\n').map_or(d.len() - p, |k| k + 1);
        if ln >= start { o.extend_from_slice(&d[p..p + l]); }
        ln += 1; p += l;
    }
    if unterminated && end == nl && end > start { o.push(b'\n'); }
    o
}
/// wc: lines, words, bytes.
pub fn wc(d: &[u8]) -> (usize, usize, usize) {
    let d = cstr(d);
    let (mut l, mut w, mut inw) = (0, 0, false);
    for &c in d { if c == b'\n' { l += 1; } if is_space(c) { inw = false; } else if !inw { inw = true; w += 1; } }
    (l, w, d.len())
}
fn contains(h: &[u8], n: &[u8], ci: bool) -> bool {
    if n.is_empty() { return true; }
    h.windows(n.len()).any(|w| if ci { w.eq_ignore_ascii_case(n) } else { w == n })
}
pub struct GrepOpts { pub ci: bool, pub num: bool, pub inv: bool, pub count: bool, pub quiet: bool, pub after: i32 }
/// grep: the output and the number of matching lines.
pub fn grep(d: &[u8], pat: &[u8], o: &GrepOpts) -> (Vec<u8>, usize) {
    let mut out = Vec::new();
    let (mut matches, mut after, mut ln) = (0usize, 0i32, 0usize);
    let show = !o.count && !o.quiet;
    for line in lines(d) {
        ln += 1;
        if contains(line, pat, o.ci) != o.inv {
            matches += 1; after = o.after;
            if show { if o.num { out.extend_from_slice(alloc::format!("\x1b[92m{}:\x1b[0m", ln).as_bytes()); } out.extend_from_slice(line); out.push(b'\n'); }
        } else if after > 0 {
            after -= 1;
            if show { if o.num { out.extend_from_slice(alloc::format!("{}-", ln).as_bytes()); } out.extend_from_slice(line); out.push(b'\n'); }
        }
    }
    if o.count { out.extend_from_slice(alloc::format!("{}\n", matches).as_bytes()); }
    (out, matches)
}
/// sort (-r -n -u): a stable insertion sort, as the C version did.
pub fn sort(d: &[u8], rev: bool, num: bool, uniq: bool) -> Vec<u8> {
    let mut v: Vec<&[u8]> = lines(d).collect();
    let cmp = |a: &[u8], b: &[u8]| -> core::cmp::Ordering {
        let c = if num { atoi(a).cmp(&atoi(b)) } else { a.cmp(b) };
        if rev { c.reverse() } else { c }
    };
    v.sort_by(|a, b| cmp(a, b));
    let mut o = Vec::new();
    for k in 0..v.len() {
        if uniq && k > 0 && v[k] == v[k - 1] { continue; }
        o.extend_from_slice(v[k]); o.push(b'\n');
    }
    o
}
pub fn uniq(d: &[u8]) -> Vec<u8> {
    let mut o = Vec::new();
    let mut prev: Option<&[u8]> = None;
    for l in lines(d) { if prev != Some(l) { o.extend_from_slice(l); o.push(b'\n'); } prev = Some(l); }
    o
}
pub fn rev(d: &[u8]) -> Vec<u8> {
    let mut o = Vec::new();
    for l in lines(d) { o.extend(l.iter().rev()); o.push(b'\n'); }
    o
}
fn tr_set(s: &[u8]) -> Vec<u8> {
    match s {
        b"[:lower:]" | b"a-z" => (b'a'..=b'z').collect(),
        b"[:upper:]" | b"A-Z" => (b'A'..=b'Z').collect(),
        _ => s[..s.len().min(255)].to_vec(),
    }
}
/// tr SET1 SET2 (a set shorter than SET1 repeats its last character), or tr -d SET.
pub fn tr(d: &[u8], set1: &[u8], set2: Option<&[u8]>) -> Vec<u8> {
    let d = cstr(d);
    match set2 {
        None => d.iter().copied().filter(|c| !set1.contains(c)).collect(),
        Some(s2) => {
            let (a, b) = (tr_set(set1), tr_set(s2));
            d.iter().map(|&c| match a.iter().position(|&x| x == c) { Some(i) if !b.is_empty() => b[i.min(b.len() - 1)], _ => c }).collect()
        }
    }
}
/// seq's numbers (the 32-bit edition's arithmetic is 32-bit, as in C); stops at an overflow.
pub fn seq_next(i: Long, step: Long, end: Long) -> Option<Long> {
    let n = i.checked_add(step)?;
    if (step > 0 && n <= end) || (step < 0 && n >= end) { Some(n) } else { None }
}
pub const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
pub const MONS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
pub struct Date { pub year: i32, pub month: i32, pub day: i32, pub hour: i32, pub minute: i32, pub second: i32, pub weekday: i32 }
fn day_name(d: &Date) -> &'static str { DAYS[(d.weekday.rem_euclid(7)) as usize] }
fn mon_name(d: &Date) -> &'static str { MONS[((d.month + 11).rem_euclid(12)) as usize] }
/// date +FORMAT (%Y %m %d %H %M %S %a %b)
pub fn date_format(fmt: &[u8], t: &Date) -> Vec<u8> {
    let mut o = Vec::new();
    let mut p = 0;
    while p < fmt.len() {
        if fmt[p] != b'%' || p + 1 >= fmt.len() { o.push(fmt[p]); p += 1; continue; }
        p += 1;
        let s = match fmt[p] {
            b'Y' => alloc::format!("{:04}", t.year), b'm' => alloc::format!("{:02}", t.month), b'd' => alloc::format!("{:02}", t.day),
            b'H' => alloc::format!("{:02}", t.hour), b'M' => alloc::format!("{:02}", t.minute), b'S' => alloc::format!("{:02}", t.second),
            b'a' => day_name(t).into(), b'b' => mon_name(t).into(),
            c => alloc::format!("%{}", c as char),
        };
        o.extend_from_slice(s.as_bytes());
        p += 1;
    }
    o
}
pub fn date_default(t: &Date) -> Vec<u8> {
    alloc::format!("{} {} {:2} {:02}:{:02}:{:02} {:04}\n", day_name(t), mon_name(t), t.day, t.hour, t.minute, t.second, t.year).into_bytes()
}
/// hexdump: the first 512 bytes, 16 to a line.
pub fn hexdump(d: &[u8]) -> Vec<u8> {
    let lim = d.len().min(512);
    let mut o = Vec::new();
    for off in (0..lim).step_by(16) {
        o.extend_from_slice(alloc::format!("{:08x}  ", off).as_bytes());
        for i in 0..16 { if off + i < lim { o.extend_from_slice(alloc::format!("{:02x} ", d[off + i]).as_bytes()); } else { o.extend_from_slice(b"   "); } }
        o.extend_from_slice(b" |");
        for i in 0..16 { if off + i < lim { let c = d[off + i]; o.push(if (32..127).contains(&c) { c } else { b'.' }); } }
        o.extend_from_slice(b"|\n");
    }
    if d.len() > lim { o.extend_from_slice(alloc::format!("... ({} more bytes)\n", d.len() - lim).as_bytes()); }
    o
}

// ---------------------------------------------------------------- unzip

/// One member of a ZIP archive's central directory.
pub struct ZipEntry<'a> { pub name: Vec<u8>, pub method: u16, pub usize: u32, pub data: Option<&'a [u8]> }
fn r16(d: &[u8], o: usize) -> u16 { if o + 2 <= d.len() { u16::from_le_bytes([d[o], d[o + 1]]) } else { 0 } }
fn r32(d: &[u8], o: usize) -> u32 { if o + 4 <= d.len() { u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) } else { 0 } }
/// The members of a ZIP archive (names over 255 bytes cut, any with ".." left out, '/' as
/// '\'), each with its compressed data when that lies inside the file; None when there is no
/// end-of-central-directory record. Every offset is checked against the file.
pub fn zip_entries(d: &[u8]) -> Option<Vec<ZipEntry<'_>>> {
    if d.len() < 22 { return None; }
    let lowest = d.len().saturating_sub(65557);
    let eocd = (lowest..=d.len() - 22).rev().find(|&i| r32(d, i) == 0x0605_4b50)?;
    let count = r16(d, eocd + 10) as usize;
    let mut o = r32(d, eocd + 16) as usize;
    let mut out = Vec::new();
    for _ in 0..count {
        if o + 46 > d.len() || r32(d, o) != 0x0201_4b50 { break; }
        let method = r16(d, o + 10);
        let (nl, xl, cl) = (r16(d, o + 28) as usize, r16(d, o + 30) as usize, r16(d, o + 32) as usize);
        let (csize, usize_, lho) = (r32(d, o + 20) as usize, r32(d, o + 24), r32(d, o + 42) as usize);
        let nend = (o + 46 + nl.min(255)).min(d.len());
        let mut name = d[o + 46..nend].to_vec();
        if let Some(z) = name.iter().position(|&c| c == 0) { name.truncate(z); }
        o += 46 + nl + xl + cl;
        if name.windows(2).any(|w| w == b"..") { continue; }
        for c in name.iter_mut() { if *c == b'/' { *c = b'\\'; } }
        let data = (lho + 30 <= d.len()).then(|| lho + 30 + r16(d, lho + 26) as usize + r16(d, lho + 28) as usize)
            .and_then(|st| st.checked_add(csize).filter(|&e| e <= d.len()).map(|e| &d[st..e]));
        out.push(ZipEntry { name, method, usize: usize_, data });
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::collections::BTreeMap;
    use alloc::string::String;

    struct T { vars: BTreeMap<Vec<u8>, Vec<u8>>, args: Vec<Vec<u8>>, files: Vec<Vec<u8>> }
    impl T { fn new() -> T { T { vars: BTreeMap::new(), args: alloc::vec![b"sh".to_vec(), b"one".to_vec(), b"two".to_vec()], files: alloc::vec![b"a.txt".to_vec(), b"b.txt".to_vec(), b"c.doc".to_vec(), b".hidden".to_vec()] } } }
    impl Env for T {
        fn var(&self, n: &[u8]) -> Option<Vec<u8>> { self.vars.get(n).cloned() }
        fn set_var(&mut self, n: &[u8], v: &[u8]) { self.vars.insert(n.to_vec(), v.to_vec()); }
        fn special(&mut self, n: &[u8]) -> Option<Vec<u8>> {
            match n {
                b"#" => Some(ltoa(self.args.len() as Long - 1)),
                b"@" | b"*" => Some(self.args[1..].join(&b' ')),
                _ if !n.is_empty() && n[0].is_ascii_digit() => Some(self.args.get(atoi(n) as usize).cloned().unwrap_or_default()),
                _ => None,
            }
        }
        fn cmd_subst(&mut self, c: &[u8]) -> Vec<u8> { let mut v = b"<".to_vec(); v.extend_from_slice(c); v.push(b'>'); v }
        fn dir_names(&mut self, d: &[u8]) -> Option<Vec<Vec<u8>>> { if d.is_empty() { Some(self.files.clone()) } else { None } }
        fn home(&self) -> Vec<u8> { b"C:\\Home".to_vec() }
    }
    fn ex(t: &mut T, w: &str) -> Vec<String> {
        let mut v = Vec::new(); expand_word(t, w.as_bytes(), &mut v, true);
        v.into_iter().map(|x| String::from_utf8(x).unwrap()).collect()
    }

    #[test]
    fn tokens_and_parse() {
        let t = tokenize(b"echo a|b && c 2>&1 >> f; x").unwrap();
        assert_eq!(t[2], Tok::Pipe);
        assert!(t.contains(&Tok::TwoTo1) && t.contains(&Tok::GtGt) && t.contains(&Tok::And));
        assert_eq!(tokenize(b"echo 'abc"), None);
        assert_eq!(tokenize(b"echo $(ls"), None);
        assert!(matches!(parse(b"if true; then echo y; fi"), Ok(Some(Node::List(_)))));
        assert_eq!(parse(b"if true; then").err(), Some(ParseError::Incomplete));
        assert_eq!(parse(b"for x in a b; do echo $x").err(), Some(ParseError::Incomplete));
        assert_eq!(parse(b"fi").err(), Some(ParseError::Syntax(b"syntax error near unexpected token 'fi'".to_vec())));
        assert_eq!(parse(b"case x in a b) ;; esac").err(), Some(ParseError::Syntax(b"syntax error in case pattern".to_vec())));
        assert!(matches!(parse(b"f() { echo hi; }\nf"), Ok(Some(_))));
        assert!(parse(b"while x; do y; done; until a; do b; done; case $v in a|b) c;; *) d;; esac").is_ok());
        assert_eq!(parse(b"echo >").err(), Some(ParseError::Syntax(b"syntax error: missing redirection target".to_vec())));
        // nesting no longer overflows the stack
        let deep: Vec<u8> = core::iter::repeat(b'(').take(5000).chain(core::iter::repeat(b')').take(5000)).collect();
        assert!(parse(&deep).is_err());
        assert!(matches!(parse(b""), Ok(None)));
    }

    #[test]
    fn arithmetic() {
        let mut t = T::new();
        assert_eq!(arith(&mut t, b"1+2*3"), 7);
        assert_eq!(arith(&mut t, b"(1+2)*3"), 9);
        assert_eq!(arith(&mut t, b"0x10 + 010"), 24);
        assert_eq!(arith(&mut t, b"7/0"), 0);
        assert_eq!(arith(&mut t, b"1 ? 5 : 6"), 5);
        assert_eq!(arith(&mut t, b"x=4"), 4);
        assert_eq!(arith(&mut t, b"x+=3"), 7);
        assert_eq!(arith(&mut t, b"x++"), 7);
        assert_eq!(arith(&mut t, b"$x * 2"), 16);
        assert_eq!(arith(&mut t, b"!0 + ~0 + (3 < 4) + (2 == 2 && 1 || 0)"), 2);
        let deep: Vec<u8> = core::iter::repeat(b'(').take(5000).collect();
        assert_eq!(arith(&mut t, &deep), 0);
    }

    #[test]
    fn globs() {
        assert!(glob_match(b"*.txt", b"A.TXT"));
        assert!(glob_match(b"a?c", b"abc") && !glob_match(b"a?c", b"ac"));
        assert!(glob_match(b"[a-c]x", b"Bx") && !glob_match(b"[!a-c]x", b"bx"));
        assert!(glob_match(b"*a*b*c", b"xxaxxbxxc") && !glob_match(b"*a*b*c", b"xxaxxbxx"));
        assert!(glob_match(b"[x", b"[x"));
        assert!(glob_match(b"*", b"") && !glob_match(b"?", b""));
        let long = alloc::vec![b'a'; 200000];
        assert!(glob_match(b"a*a", &long));
    }

    #[test]
    fn expansion() {
        let mut t = T::new();
        t.set_var(b"v", b"hello world");
        t.set_var(b"f", b"dir/name.tar.gz");
        assert_eq!(ex(&mut t, "$v"), ["hello", "world"]);
        assert_eq!(ex(&mut t, "\"$v\""), ["hello world"]);
        assert_eq!(ex(&mut t, "'$v'"), ["$v"]);
        assert_eq!(ex(&mut t, "${#v}"), ["11"]);
        assert_eq!(ex(&mut t, "${u:-def}"), ["def"]);
        assert_eq!(ex(&mut t, "${f%.*}"), ["dir/name.tar"]);
        assert_eq!(ex(&mut t, "${f%%.*}"), ["dir/name"]);
        assert_eq!(ex(&mut t, "${f#*/}"), ["name.tar.gz"]);
        assert_eq!(ex(&mut t, "${f##*.}"), ["gz"]);
        assert_eq!(ex(&mut t, "${v/o/0}"), ["hell0", "world"]);
        assert_eq!(ex(&mut t, "${v//o/0}"), ["hell0", "w0rld"]);
        assert_eq!(ex(&mut t, "${v:6:3}"), ["wor"]);
        assert_eq!(ex(&mut t, "$((1+2))x"), ["3x"]);              // C dropped the "x"
        assert_eq!(ex(&mut t, "\"$((2*3))\"y"), ["6y"]);
        assert_eq!(ex(&mut t, "a$(cmd)b"), ["a<cmd>b"]);
        assert_eq!(ex(&mut t, "`x y`"), ["<x", "y>"]);
        assert_eq!(ex(&mut t, "$1-$2:$#"), ["one-two:2"]);
        assert_eq!(ex(&mut t, "~/x"), ["C:\\Home/x"]);
        assert_eq!(ex(&mut t, "*.txt"), ["a.txt", "b.txt"]);
        assert_eq!(ex(&mut t, "*.none"), ["*.none"]);
        assert_eq!(ex(&mut t, "\"*.txt\""), ["*.txt"]);
        assert_eq!(ex(&mut t, "$nothing").len(), 0);
        assert_eq!(ex(&mut t, "\"$nothing\""), [""]);
        assert_eq!(ex(&mut t, "a\\ b"), ["a b"]);
        assert_eq!(ex(&mut t, "$"), ["$"]);
        let mut big = b"${v#".to_vec(); big.extend(core::iter::repeat(b'*').take(10)); big.push(b'}');
        t.set_var(b"v", &alloc::vec![b'x'; 5000]);
        assert_eq!(ex(&mut t, core::str::from_utf8(&big).unwrap())[0].len(), 5000);
    }

    #[test]
    fn text() {
        let a = |v: &[&str]| -> Vec<Vec<u8>> { v.iter().map(|s| s.as_bytes().to_vec()).collect() };
        assert_eq!(echo(&a(&["echo", "-n", "a", "b"])), b"a b");
        assert_eq!(echo(&a(&["echo", "-e", "a\\tb\\c", "z"])), b"a\tb");
        assert_eq!(echo(&a(&["echo", "-x"])), b"-x\n");
        assert_eq!(printf(&a(&["printf", "%5d|%-4s|%03x|%c\\n", "42", "ab", "255", "Zed"])), b"   42|ab  |0ff|Z\n");
        assert_eq!(printf(&a(&["printf", "%5c|%05d|%-3d|", "Q", "-42", "7"])), b"Q|-0042|7  |");
        assert_eq!(printf(&a(&["printf", "%5-d.", "1", "2"])), b"%-d.%-d.");
        assert_eq!(printf(&a(&["printf", "x%5", "1"])), b"x%5");
        assert_eq!(printf(&a(&["printf", "%s,", "a", "b", "c"])), b"a,b,c,");
        assert_eq!(printf(&a(&["printf", "%s%%"])), b"%");                // the C version looped forever
        assert_eq!(printf(&a(&["printf", "%.2s %.3d %u", "abc", "7", "-1"])), b"ab 7 4294967295");
        let mut st = |p: &[u8]| -> Option<FileInfo> { (p == b"f").then_some(FileInfo { dir: false, size: 3, readonly: false }) };
        assert!(test_eval(&[b"-f", b"f"], &mut st) && !test_eval(&[b"-d", b"f"], &mut st));
        assert!(test_eval(&[b"3", b"-lt", b"10"], &mut st) && test_eval(&[b"a", b"=", b"a", b"-a", b"!", b"-z", b"x"], &mut st));
        assert_eq!(head_tail(b"1\n2\n3\n4", 2, false), b"1\n2\n");
        assert_eq!(head_tail(b"1\n2\n3\n4", 2, true), b"3\n4\n");
        assert_eq!(wc(b"a b\nc\n"), (2, 3, 6));
        let o = GrepOpts { ci: true, num: false, inv: false, count: false, quiet: false, after: 0 };
        assert_eq!(grep(b"Apple\nbanana\napricot\n", b"AP", &o), (b"Apple\napricot\n".to_vec(), 2));
        assert_eq!(sort(b"b\na\nc\na\n", false, false, true), b"a\nb\nc\n");
        assert_eq!(sort(b"10\n9\n100\n", true, true, false), b"100\n10\n9\n");
        assert_eq!(uniq(b"a\na\nb\na\n"), b"a\nb\na\n");
        assert_eq!(tr(b"hello", b"a-z", Some(b"A-Z")), b"HELLO");
        assert_eq!(tr(b"hello", b"lo", None), b"he");
        let t = Date { year: 2026, month: 11, day: 20, hour: 7, minute: 5, second: 9, weekday: 5 };
        assert_eq!(date_format(b"%Y-%m-%d %H:%M:%S %a %b %q", &t), b"2026-11-20 07:05:09 Fri Nov %q");
        assert!(hexdump(b"AB\x00").starts_with(b"00000000  41 42 00"));
    }

    #[test]
    fn zip() {
        // one stored member "x/a.txt" = "hi"
        let mut z = Vec::new();
        let name = b"x/a.txt";
        z.extend_from_slice(&0x0403_4b50u32.to_le_bytes()); z.extend_from_slice(&[0; 22]); z.extend_from_slice(&(name.len() as u16).to_le_bytes()); z.extend_from_slice(&[0, 0]); z.extend_from_slice(name); z.extend_from_slice(b"hi");
        let cd = z.len();
        z.extend_from_slice(&0x0201_4b50u32.to_le_bytes()); z.extend_from_slice(&[0; 6]); z.extend_from_slice(&0u16.to_le_bytes()); z.extend_from_slice(&[0; 8]);
        z.extend_from_slice(&2u32.to_le_bytes()); z.extend_from_slice(&2u32.to_le_bytes()); z.extend_from_slice(&(name.len() as u16).to_le_bytes()); z.extend_from_slice(&[0; 12]); z.extend_from_slice(&0u32.to_le_bytes()); z.extend_from_slice(name);
        let cdl = z.len() - cd;
        z.extend_from_slice(&0x0605_4b50u32.to_le_bytes()); z.extend_from_slice(&[0; 6]); z.extend_from_slice(&1u16.to_le_bytes()); z.extend_from_slice(&(cdl as u32).to_le_bytes()); z.extend_from_slice(&(cd as u32).to_le_bytes()); z.extend_from_slice(&[0, 0]);
        let e = zip_entries(&z).unwrap();
        assert_eq!(e.len(), 1);
        assert_eq!((e[0].name.as_slice(), e[0].data), (&b"x\\a.txt"[..], Some(&b"hi"[..])));
        for n in 0..z.len() { let _ = zip_entries(&z[..n]); }
        let mut bad = z.clone(); let p = cd + 42; bad[p..p + 4].copy_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        assert!(zip_entries(&bad).unwrap()[0].data.is_none());
    }
}
