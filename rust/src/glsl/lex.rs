//! The GLSL preprocessor and tokenizer: comments, line continuations, `#version`,
//! `#define` (object-like and function-like), `#undef`, `#if`/`#ifdef`/`#ifndef`/`#elif`/
//! `#else`/`#endif`, `#error`; `#extension`, `#pragma` and `#line` are accepted and ignored.

use alloc::collections::BTreeMap;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use alloc::format;

#[derive(Clone, Debug, PartialEq)]
pub enum T {
    Id(String),
    Int(i64, bool),      // value, unsigned
    Float(f32),
    P(&'static str),     // punctuation / operator
    Eof,
}
#[derive(Clone, Debug)]
pub struct Tok { pub t: T, pub line: u32 }

const PUNCT: [&str; 47] = [
    "<<=", ">>=", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "^^", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "(", ")", "[", "]", "{", "}", ".", ",", ";", ":", "?", "+", "-", "*", "/", "%", "<", ">", "!", "~", "&", "|", "^", "=", "#", "\\",
];

/// Tokens of one line of text (no directives, no comments).
fn tokenize(s: &[u8], line: u32, out: &mut Vec<Tok>) -> Result<(), String> {
    let mut i = 0;
    while i < s.len() {
        let c = s[i];
        if c == b' ' || c == b'\t' || c == b'\r' || c == 0x0b || c == 0x0c { i += 1; continue; }
        if c.is_ascii_alphabetic() || c == b'_' {
            let st = i;
            while i < s.len() && (s[i].is_ascii_alphanumeric() || s[i] == b'_') { i += 1; }
            out.push(Tok { t: T::Id(String::from_utf8_lossy(&s[st..i]).into_owned()), line });
            continue;
        }
        if c.is_ascii_digit() || (c == b'.' && i + 1 < s.len() && s[i + 1].is_ascii_digit()) {
            let st = i;
            let mut float = false;
            if c == b'0' && i + 1 < s.len() && (s[i + 1] == b'x' || s[i + 1] == b'X') {
                i += 2;
                let hs = i;
                while i < s.len() && s[i].is_ascii_hexdigit() { i += 1; }
                let mut v: i64 = 0;
                for &d in &s[hs..i] { v = v.wrapping_mul(16).wrapping_add((d as char).to_digit(16).unwrap_or(0) as i64); }
                let u = i < s.len() && (s[i] == b'u' || s[i] == b'U');
                if u { i += 1; }
                out.push(Tok { t: T::Int(v & 0xFFFF_FFFF, u), line });
                continue;
            }
            while i < s.len() && s[i].is_ascii_digit() { i += 1; }
            if i < s.len() && s[i] == b'.' { float = true; i += 1; while i < s.len() && s[i].is_ascii_digit() { i += 1; } }
            if i < s.len() && (s[i] == b'e' || s[i] == b'E') {
                let mut j = i + 1;
                if j < s.len() && (s[j] == b'+' || s[j] == b'-') { j += 1; }
                if j < s.len() && s[j].is_ascii_digit() { float = true; i = j; while i < s.len() && s[i].is_ascii_digit() { i += 1; } }
            }
            let text = core::str::from_utf8(&s[st..i]).unwrap_or("0");
            if i < s.len() && (s[i] == b'f' || s[i] == b'F') { float = true; i += 1; }
            else if i + 1 < s.len() && (s[i] == b'l' || s[i] == b'L') && (s[i + 1] == b'f' || s[i + 1] == b'F') { float = true; i += 2; }
            if float {
                out.push(Tok { t: T::Float(parse_float(text)), line });
            } else {
                let u = i < s.len() && (s[i] == b'u' || s[i] == b'U');
                if u { i += 1; }
                let v = if text.len() > 1 && text.starts_with('0') {
                    let mut v: i64 = 0; for d in text.bytes() { v = v.wrapping_mul(8).wrapping_add((d - b'0') as i64 & 7); } v
                } else {
                    let mut v: i64 = 0; for d in text.bytes() { v = v.wrapping_mul(10).wrapping_add((d - b'0') as i64); } v
                };
                out.push(Tok { t: T::Int(v & 0xFFFF_FFFF, u), line });
            }
            continue;
        }
        let mut found = false;
        for p in PUNCT.iter() {
            if s[i..].starts_with(p.as_bytes()) { out.push(Tok { t: T::P(p), line }); i += p.len(); found = true; break; }
        }
        if !found {
            if c >= 0x80 { i += 1; continue; }   /* stray bytes of other encodings in a source line: skip */
            return Err(format!("{}: unexpected character '{}'", line, c as char));
        }
    }
    Ok(())
}

/// A decimal float, correctly rounded enough for shader constants.
pub fn parse_float(t: &str) -> f32 {
    let b = t.as_bytes();
    let mut i = 0;
    let mut mant: u64 = 0;
    let mut exp: i32 = 0;
    let mut digits = 0;
    while i < b.len() && b[i].is_ascii_digit() { if digits < 18 { mant = mant * 10 + (b[i] - b'0') as u64; digits += 1; } else { exp += 1; } i += 1; }
    if i < b.len() && b[i] == b'.' {
        i += 1;
        while i < b.len() && b[i].is_ascii_digit() { if digits < 18 { mant = mant * 10 + (b[i] - b'0') as u64; digits += 1; exp -= 1; } i += 1; }
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        let neg = i < b.len() && b[i] == b'-';
        if i < b.len() && (b[i] == b'-' || b[i] == b'+') { i += 1; }
        let mut e = 0i32;
        while i < b.len() && b[i].is_ascii_digit() { e = (e * 10 + (b[i] - b'0') as i32).min(10000); i += 1; }
        exp += if neg { -e } else { e };
    }
    let mut v = mant as f64;
    let mut p = 10f64;
    let mut e = exp.unsigned_abs();
    let mut scale = 1f64;
    while e > 0 { if e & 1 != 0 { scale *= p; } p *= p; e >>= 1; }
    if exp < 0 { v /= scale; } else { v *= scale; }
    v as f32
}

#[derive(Clone)]
struct Macro { params: Option<Vec<String>>, body: Vec<Tok> }

pub struct Pre { pub toks: Vec<Tok>, pub version: u32, #[allow(dead_code)] pub es: bool }

fn strip_comments(src: &[u8]) -> Result<Vec<u8>, String> {
    let mut o = Vec::with_capacity(src.len());
    let mut i = 0;
    let mut line = 1;
    while i < src.len() {
        if src[i] == b'/' && i + 1 < src.len() && src[i + 1] == b'/' {
            while i < src.len() && src[i] != b'\n' {
                /* a backslash-newline continues a // comment too */
                if src[i] == b'\\' && i + 1 < src.len() && src[i + 1] == b'\n' { i += 1; o.push(b'\n'); line += 1; }
                i += 1;
            }
        } else if src[i] == b'/' && i + 1 < src.len() && src[i + 1] == b'*' {
            let st = line;
            i += 2;
            o.push(b' ');
            loop {
                if i + 1 >= src.len() { return Err(format!("{}: unterminated comment", st)); }
                if src[i] == b'*' && src[i + 1] == b'/' { i += 2; break; }
                if src[i] == b'\n' { o.push(b'\n'); line += 1; }
                i += 1;
            }
        } else {
            if src[i] == b'\n' { line += 1; }
            o.push(src[i]);
            i += 1;
        }
    }
    Ok(o)
}

struct Cond { active: bool, taken: bool, parent: bool }

pub fn preprocess(src: &[u8]) -> Result<Pre, String> {
    let text = strip_comments(src)?;
    /* logical lines: join backslash-newline, but keep line numbers */
    let mut lines: Vec<(u32, Vec<u8>)> = Vec::new();
    {
        let mut cur = Vec::new();
        let mut start = 1u32;
        let mut line = 1u32;
        let mut i = 0;
        while i <= text.len() {
            if i == text.len() || text[i] == b'\n' {
                if !cur.is_empty() && *cur.last().unwrap() == b'\\' && i < text.len() { cur.pop(); line += 1; i += 1; continue; }
                lines.push((start, core::mem::take(&mut cur)));
                line += 1; start = line; i += 1; continue;
            }
            cur.push(text[i]); i += 1;
        }
    }
    let mut macros: BTreeMap<String, Macro> = BTreeMap::new();
    let one = |v: i64, line| Macro { params: None, body: alloc::vec![Tok { t: T::Int(v, false), line }] };
    macros.insert("GL_compatibility_profile".into(), one(1, 0));
    macros.insert("GL_ARB_shader_objects".into(), one(1, 0));
    macros.insert("__VERSION__".into(), one(110, 0));
    let mut out = Vec::new();
    let mut conds: Vec<Cond> = Vec::new();
    let mut version = 110u32;
    let mut es = false;
    let mut seen_code = false;
    for (line, l) in lines.iter() {
        let line = *line;
        let trimmed: &[u8] = { let mut s = &l[..]; while let Some((&c, r)) = s.split_first() { if c == b' ' || c == b'\t' || c == b'\r' { s = r; } else { break; } } s };
        let active = conds.last().map(|c| c.active).unwrap_or(true);
        if trimmed.first() == Some(&b'#') {
            let mut toks = Vec::new();
            tokenize(&trimmed[1..], line, &mut toks)?;
            let name = match toks.first() { Some(Tok { t: T::Id(n), .. }) => n.clone(), None => continue, _ => return Err(format!("{}: bad directive", line)) };
            let rest = &toks[1..];
            match name.as_str() {
                "ifdef" | "ifndef" => {
                    let d = match rest.first() { Some(Tok { t: T::Id(n), .. }) => macros.contains_key(n), _ => return Err(format!("{}: #{} needs a name", line, name)) };
                    let v = if name == "ifdef" { d } else { !d };
                    conds.push(Cond { active: active && v, taken: v, parent: active });
                }
                "if" => {
                    let v = if active { eval_if(rest, &macros, line)? != 0 } else { false };
                    conds.push(Cond { active: active && v, taken: v, parent: active });
                }
                "elif" => {
                    let c = conds.last_mut().ok_or_else(|| format!("{}: #elif without #if", line))?;
                    if c.taken || !c.parent { c.active = false; }
                    else { let v = eval_if(rest, &macros, line)? != 0; c.active = v; c.taken = v; }
                }
                "else" => {
                    let c = conds.last_mut().ok_or_else(|| format!("{}: #else without #if", line))?;
                    c.active = c.parent && !c.taken; c.taken = true;
                }
                "endif" => { if conds.pop().is_none() { return Err(format!("{}: #endif without #if", line)); } }
                _ if !active => {}
                "version" => {
                    if seen_code { return Err(format!("{}: #version must come first", line)); }
                    if let Some(Tok { t: T::Int(v, _), .. }) = rest.first() { version = *v as u32; }
                    if let Some(Tok { t: T::Id(p), .. }) = rest.get(1) { es = p == "es"; }
                    macros.insert("__VERSION__".into(), one(version as i64, line));
                    if es { macros.insert("GL_ES".into(), one(1, line)); }
                }
                "define" => {
                    let n = match rest.first() { Some(Tok { t: T::Id(n), .. }) => n.clone(), _ => return Err(format!("{}: #define needs a name", line)) };
                    /* function-like only if '(' follows the name with no space */
                    let after = {
                        let s = trimmed;
                        let p = s.windows(n.len()).position(|w| w == n.as_bytes()).map(|p| p + n.len()).unwrap_or(s.len());
                        s.get(p).copied()
                    };
                    if after == Some(b'(') && matches!(rest.get(1), Some(Tok { t: T::P("("), .. })) {
                        let mut ps = Vec::new();
                        let mut k = 2;
                        loop {
                            match rest.get(k) {
                                Some(Tok { t: T::P(")"), .. }) => { k += 1; break; }
                                Some(Tok { t: T::Id(p), .. }) => { ps.push(p.clone()); k += 1; if let Some(Tok { t: T::P(","), .. }) = rest.get(k) { k += 1; } }
                                _ => return Err(format!("{}: bad macro parameters", line)),
                            }
                        }
                        macros.insert(n, Macro { params: Some(ps), body: rest[k..].to_vec() });
                    } else {
                        macros.insert(n, Macro { params: None, body: rest[1..].to_vec() });
                    }
                }
                "undef" => { if let Some(Tok { t: T::Id(n), .. }) = rest.first() { macros.remove(n); } }
                "error" => return Err(format!("{}: #error", line)),
                "extension" | "pragma" | "line" => {}
                _ => return Err(format!("{}: unknown directive #{}", line, name)),
            }
            continue;
        }
        if !active { continue; }
        let mut toks = Vec::new();
        tokenize(l, line, &mut toks)?;
        if toks.is_empty() { continue; }
        seen_code = true;
        let mut depth = 0;
        expand(&toks, &macros, &mut out, &mut depth)?;
    }
    if !conds.is_empty() { return Err("unterminated #if".to_string()); }
    let last = out.last().map(|t| t.line).unwrap_or(1);
    out.push(Tok { t: T::Eof, line: last });
    Ok(Pre { toks: out, version, es })
}

fn expand(toks: &[Tok], macros: &BTreeMap<String, Macro>, out: &mut Vec<Tok>, depth: &mut u32) -> Result<(), String> {
    *depth += 1;
    if *depth > 64 { return Err("macro expansion too deep".to_string()); }
    let mut i = 0;
    while i < toks.len() {
        if let T::Id(n) = &toks[i].t {
            if let Some(m) = macros.get(n) {
                let line = toks[i].line;
                match &m.params {
                    None => {
                        let body: Vec<Tok> = m.body.iter().map(|t| Tok { t: t.t.clone(), line }).collect();
                        let mut sub = macros.clone(); sub.remove(n);
                        expand(&body, &sub, out, depth)?;
                        i += 1; continue;
                    }
                    Some(ps) => {
                        if !matches!(toks.get(i + 1), Some(Tok { t: T::P("("), .. })) { out.push(toks[i].clone()); i += 1; continue; }
                        let mut args: Vec<Vec<Tok>> = alloc::vec![Vec::new()];
                        let mut lvl = 0; let mut k = i + 2;
                        loop {
                            let t = toks.get(k).ok_or_else(|| format!("{}: unterminated macro call", line))?;
                            match &t.t {
                                T::P("(") => { lvl += 1; args.last_mut().unwrap().push(t.clone()); }
                                T::P(")") => { if lvl == 0 { break; } lvl -= 1; args.last_mut().unwrap().push(t.clone()); }
                                T::P(",") if lvl == 0 => args.push(Vec::new()),
                                _ => args.last_mut().unwrap().push(t.clone()),
                            }
                            k += 1;
                        }
                        if ps.is_empty() && args.len() == 1 && args[0].is_empty() { args.clear(); }
                        if args.len() != ps.len() { return Err(format!("{}: macro {} takes {} arguments", line, n, ps.len())); }
                        let mut body = Vec::new();
                        for t in &m.body {
                            if let T::Id(b) = &t.t { if let Some(p) = ps.iter().position(|p| p == b) { for a in &args[p] { body.push(Tok { t: a.t.clone(), line }); } continue; } }
                            body.push(Tok { t: t.t.clone(), line });
                        }
                        let mut sub = macros.clone(); sub.remove(n);
                        expand(&body, &sub, out, depth)?;
                        i = k + 1; continue;
                    }
                }
            }
        }
        out.push(toks[i].clone());
        i += 1;
    }
    *depth -= 1;
    Ok(())
}

fn eval_if(toks: &[Tok], macros: &BTreeMap<String, Macro>, line: u32) -> Result<i64, String> {
    /* defined X / defined(X) first, then macros, then names left over are 0 */
    let mut pre = Vec::new();
    let mut i = 0;
    while i < toks.len() {
        if let T::Id(n) = &toks[i].t {
            if n == "defined" {
                let (name, skip) = match (toks.get(i + 1), toks.get(i + 2)) {
                    (Some(Tok { t: T::P("("), .. }), Some(Tok { t: T::Id(x), .. })) => (x.clone(), 4),
                    (Some(Tok { t: T::Id(x), .. }), _) => (x.clone(), 2),
                    _ => return Err(format!("{}: bad defined()", line)),
                };
                pre.push(Tok { t: T::Int(macros.contains_key(&name) as i64, false), line });
                i += skip; continue;
            }
        }
        pre.push(toks[i].clone()); i += 1;
    }
    let mut ex = Vec::new();
    let mut d = 0;
    expand(&pre, macros, &mut ex, &mut d)?;
    let mut p = 0;
    let v = pp_expr(&ex, &mut p, 0, line)?;
    Ok(v)
}
fn pp_prec(op: &str) -> Option<u8> {
    Some(match op { "||" => 1, "&&" => 2, "|" => 3, "^" => 4, "&" => 5, "==" | "!=" => 6, "<" | ">" | "<=" | ">=" => 7, "<<" | ">>" => 8, "+" | "-" => 9, "*" | "/" | "%" => 10, _ => return None })
}
fn pp_unary(t: &[Tok], p: &mut usize, line: u32) -> Result<i64, String> {
    let tok = t.get(*p).ok_or_else(|| format!("{}: bad #if expression", line))?;
    *p += 1;
    Ok(match &tok.t {
        T::Int(v, _) => *v,
        T::Float(_) => return Err(format!("{}: float in #if", line)),
        T::Id(_) => 0,
        T::P("(") => { let v = pp_expr(t, p, 0, line)?; if !matches!(t.get(*p), Some(Tok { t: T::P(")"), .. })) { return Err(format!("{}: missing ) in #if", line)); } *p += 1; v }
        T::P("!") => (pp_unary(t, p, line)? == 0) as i64,
        T::P("-") => pp_unary(t, p, line)?.wrapping_neg(),
        T::P("+") => pp_unary(t, p, line)?,
        T::P("~") => !pp_unary(t, p, line)?,
        _ => return Err(format!("{}: bad #if expression", line)),
    })
}
fn pp_expr(t: &[Tok], p: &mut usize, min: u8, line: u32) -> Result<i64, String> {
    let mut a = pp_unary(t, p, line)?;
    loop {
        let op = match t.get(*p) { Some(Tok { t: T::P(o), .. }) => *o, _ => break };
        let pr = match pp_prec(op) { Some(x) if x > min => x, _ => break };
        *p += 1;
        let b = pp_expr(t, p, pr, line)?;
        a = match op {
            "||" => (a != 0 || b != 0) as i64, "&&" => (a != 0 && b != 0) as i64, "|" => a | b, "^" => a ^ b, "&" => a & b,
            "==" => (a == b) as i64, "!=" => (a != b) as i64, "<" => (a < b) as i64, ">" => (a > b) as i64, "<=" => (a <= b) as i64, ">=" => (a >= b) as i64,
            "<<" => a.wrapping_shl(b as u32 & 63), ">>" => a.wrapping_shr(b as u32 & 63), "+" => a.wrapping_add(b), "-" => a.wrapping_sub(b), "*" => a.wrapping_mul(b),
            "/" => if b == 0 { return Err(format!("{}: division by zero in #if", line)) } else { a.wrapping_div(b) },
            "%" => if b == 0 { return Err(format!("{}: division by zero in #if", line)) } else { a.wrapping_rem(b) },
            _ => a,
        };
    }
    Ok(a)
}
