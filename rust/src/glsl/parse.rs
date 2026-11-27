//! The GLSL parser: tokens to a syntax tree. It accepts GLSL 1.10 to 4.x and GLSL ES
//! syntax (qualifiers it has no use for, such as precision and layout, are parsed and
//! mostly dropped), and the common driver extensions to the 1.10 grammar.

use super::lex::{Tok, T};
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use alloc::format;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Bin { Add, Sub, Mul, Div, Mod, Shl, Shr, Lt, Gt, Le, Ge, Eq, Ne, BAnd, BOr, BXor, And, Or, Xor }
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Un { Neg, Plus, Not, BNot }

#[derive(Clone, Debug)]
pub enum Ex {
    Int(i64, bool),
    Float(f32),
    Bool(bool),
    Var(String),
    Bin(Bin, Box<E>, Box<E>),
    Un(Un, Box<E>),
    Assign(Option<Bin>, Box<E>, Box<E>),
    IncDec(bool, bool, Box<E>),           // prefix, increment
    Cond(Box<E>, Box<E>, Box<E>),
    Call(String, Option<Box<E>>, Vec<E>), // name (function or type), array size of an array constructor
    Field(Box<E>, String),
    Index(Box<E>, Box<E>),
    Comma(Box<E>, Box<E>),
    Length(Box<E>),
}
#[derive(Clone, Debug)]
pub struct E { pub k: Ex, pub line: u32 }

#[derive(Clone, Debug, PartialEq)]
pub enum Store { None, Const, Uniform, Attribute, Varying, In, Out, InOut, Shared, Buffer }
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Interp { Smooth, Flat, NoPersp }
#[derive(Clone, Debug)]
pub struct Qual { pub store: Store, pub interp: Interp, pub location: i32, pub invariant: bool }
impl Default for Qual { fn default() -> Self { Qual { store: Store::None, interp: Interp::Smooth, location: -1, invariant: false } } }

#[derive(Clone, Debug)]
pub struct TypeSpec { pub name: String, pub dims: Vec<Option<E>> }   // array dimensions on the type
#[derive(Clone, Debug)]
pub struct Decl { pub qual: Qual, pub ty: TypeSpec, pub name: String, pub dims: Vec<Option<E>>, pub init: Option<E>, pub line: u32 }
#[derive(Clone, Debug)]
pub struct Param { pub out: bool, pub inp: bool, pub ty: TypeSpec, pub name: String, pub dims: Vec<Option<E>> }
#[derive(Clone, Debug)]
pub struct Func { pub ret: TypeSpec, pub name: String, pub params: Vec<Param>, pub body: Option<Vec<St>>, pub line: u32 }
#[derive(Clone, Debug)]
pub struct StructDef { pub name: String, pub fields: Vec<(TypeSpec, String, Vec<Option<E>>)>, pub line: u32 }

#[derive(Clone, Debug)]
pub enum St {
    Block(Vec<St>),
    Decl(Vec<Decl>),
    Struct(StructDef),
    Expr(E),
    If(E, Box<St>, Option<Box<St>>),
    For(Option<Box<St>>, Option<E>, Option<E>, Box<St>),
    While(E, Box<St>),
    Do(Box<St>, E),
    Switch(E, Vec<St>),
    Case(Option<E>, u32),
    Break(u32), Continue(u32), Return(Option<E>, u32), Discard(u32),
    Empty,
}
#[derive(Clone, Debug)]
pub enum Global { Decl(Vec<Decl>), Func(Func), Struct(StructDef), StructDecl(StructDef, Vec<Decl>), Block(Qual, String, Vec<Decl>), Empty }

pub struct Parser<'a> { t: &'a [Tok], p: usize, pub structs: Vec<String>, anon: u32 }

const BUILTIN_TYPES: [&str; 46] = [
    "void", "bool", "int", "uint", "float", "double",
    "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "dvec2", "dvec3", "dvec4",
    "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4",
    "sampler1D", "sampler2D", "sampler3D", "samplerCube", "sampler2DShadow", "sampler1DShadow", "sampler2DRect", "samplerExternalOES",
    "sampler2DArray", "isampler2D", "usampler2D", "samplerCubeShadow", "sampler2DRectShadow",
];

type R<T> = Result<T, String>;

impl<'a> Parser<'a> {
    pub fn new(t: &'a [Tok]) -> Self { Parser { t, p: 0, structs: Vec::new(), anon: 0 } }
    fn peek(&self) -> &T { &self.t[self.p.min(self.t.len() - 1)].t }
    fn peek2(&self) -> &T { &self.t[(self.p + 1).min(self.t.len() - 1)].t }
    fn line(&self) -> u32 { self.t[self.p.min(self.t.len() - 1)].line }
    fn next(&mut self) -> T { let t = self.peek().clone(); if self.p < self.t.len() - 1 { self.p += 1; } t }
    fn is(&self, s: &str) -> bool { matches!(self.peek(), T::P(p) if *p == s) }
    fn is_id(&self, s: &str) -> bool { matches!(self.peek(), T::Id(p) if p == s) }
    fn eat(&mut self, s: &str) -> bool { if self.is(s) || self.is_id(s) { self.next(); true } else { false } }
    fn err<X>(&self, m: &str) -> R<X> {
        let near = match self.peek() { T::Id(s) => format!("'{}'", s), T::P(p) => format!("'{}'", p), T::Int(v, _) => format!("{}", v), T::Float(f) => format!("{}", f), T::Eof => "the end".into() };
        Err(format!("{}: {} (near {})", self.line(), m, near))
    }
    fn expect(&mut self, s: &str) -> R<()> { if self.eat(s) { Ok(()) } else { self.err(&format!("expected '{}'", s)) } }
    fn ident(&mut self) -> R<String> { match self.next() { T::Id(s) => Ok(s), _ => { self.p -= 1; self.err("expected a name") } } }
    pub fn is_type(&self, n: &str) -> bool { BUILTIN_TYPES.contains(&n) || self.structs.iter().any(|s| s == n) }

    pub fn unit(&mut self) -> R<Vec<Global>> {
        let mut g = Vec::new();
        while *self.peek() != T::Eof { g.push(self.global()?); }
        Ok(g)
    }

    fn qualifiers(&mut self) -> R<Qual> {
        let mut q = Qual::default();
        loop {
            let n = match self.peek() { T::Id(n) => n.clone(), _ => break };
            match n.as_str() {
                "const" => q.store = Store::Const,
                "uniform" => q.store = Store::Uniform,
                "attribute" => q.store = Store::Attribute,
                "varying" => q.store = if q.store == Store::In { Store::In } else { Store::Varying },
                "in" => q.store = if q.store == Store::Out { Store::InOut } else { Store::In },
                "out" => q.store = if q.store == Store::In { Store::InOut } else { Store::Out },
                "inout" => q.store = Store::InOut,
                "buffer" => q.store = Store::Buffer,
                "shared" => q.store = Store::Shared,
                "flat" => q.interp = Interp::Flat,
                "noperspective" => q.interp = Interp::NoPersp,
                "smooth" => q.interp = Interp::Smooth,
                "invariant" => q.invariant = true,
                "centroid" | "sample" | "patch" | "highp" | "mediump" | "lowp" | "precise" | "coherent" | "volatile" | "restrict" | "readonly" | "writeonly" => {}
                "layout" => {
                    self.next(); self.expect("(")?;
                    while !self.is(")") {
                        let k = self.ident()?;
                        if self.eat("=") {
                            let neg = self.eat("-");
                            let v = match self.next() { T::Int(v, _) => v as i32, T::Id(_) => 0, _ => return self.err("bad layout value") };
                            if k == "location" { q.location = if neg { -v } else { v }; }
                        }
                        if !self.eat(",") { break; }
                    }
                    self.expect(")")?;
                    continue;
                }
                _ => break,
            }
            self.next();
        }
        Ok(q)
    }
    fn dims(&mut self) -> R<Vec<Option<E>>> {
        let mut d = Vec::new();
        while self.eat("[") {
            if self.eat("]") { d.push(None); continue; }
            let e = self.expr(1)?;
            self.expect("]")?;
            d.push(Some(e));
        }
        Ok(d)
    }
    fn type_spec(&mut self) -> R<(TypeSpec, Option<StructDef>)> {
        if self.is_id("struct") {
            let s = self.struct_def()?;
            let dims = self.dims()?;
            return Ok((TypeSpec { name: s.name.clone(), dims }, Some(s)));
        }
        let n = self.ident()?;
        if !self.is_type(&n) { self.p -= 1; return self.err("unknown type"); }
        let dims = self.dims()?;
        Ok((TypeSpec { name: n, dims }, None))
    }
    fn struct_def(&mut self) -> R<StructDef> {
        let line = self.line();
        self.expect("struct")?;
        let name = if let T::Id(n) = self.peek().clone() { self.next(); n } else { self.anon += 1; format!("__anon{}", self.anon) };
        self.expect("{")?;
        let mut fields = Vec::new();
        while !self.eat("}") {
            let _q = self.qualifiers()?;
            let (ty, sd) = self.type_spec()?;
            if sd.is_some() { return self.err("nested struct definitions are not supported"); }
            loop {
                let f = self.ident()?;
                let d = self.dims()?;
                fields.push((ty.clone(), f, d));
                if !self.eat(",") { break; }
            }
            self.expect(";")?;
        }
        self.structs.push(name.clone());
        Ok(StructDef { name, fields, line })
    }
    fn global(&mut self) -> R<Global> {
        if self.eat(";") { return Ok(Global::Empty); }
        if self.is_id("precision") {
            while !self.eat(";") { if *self.peek() == T::Eof { return self.err("expected ';'"); } self.next(); }
            return Ok(Global::Empty);
        }
        let line = self.line();
        let q = self.qualifiers()?;
        /* "invariant gl_Position;" and similar redeclarations */
        if let T::Id(n) = self.peek().clone() {
            if !self.is_type(&n) && n != "struct" && matches!(self.peek2(), T::P(";") | T::P(",")) { while !self.eat(";") { self.next(); } return Ok(Global::Empty); }
            /* interface blocks: uniform Name { ... } inst; */
            if !self.is_type(&n) && n != "struct" && matches!(self.peek2(), T::P("{")) && q.store != Store::None {
                self.next(); self.next();
                let mut members = Vec::new();
                while !self.eat("}") {
                    let mq = self.qualifiers()?;
                    let (ty, _) = self.type_spec()?;
                    loop {
                        let f = self.ident()?; let d = self.dims()?;
                        members.push(Decl { qual: Qual { store: q.store.clone(), ..mq.clone() }, ty: ty.clone(), name: f, dims: d, init: None, line });
                        if !self.eat(",") { break; }
                    }
                    self.expect(";")?;
                }
                let inst = if let T::Id(i) = self.peek().clone() { self.next(); self.dims()?; i } else { String::new() };
                self.expect(";")?;
                return Ok(Global::Block(q, inst, members));
            }
        }
        let (ty, sd) = self.type_spec()?;
        if let Some(s) = &sd { if self.eat(";") { return Ok(Global::Struct(s.clone())); } }
        let name = self.ident()?;
        if self.is("(") {
            /* a function */
            self.next();
            let mut params = Vec::new();
            if self.is_id("void") && matches!(self.peek2(), T::P(")")) { self.next(); }
            while !self.is(")") {
                let pq = self.qualifiers()?;
                let (pty, _) = self.type_spec()?;
                let (pn, pd) = if let T::Id(n) = self.peek().clone() { self.next(); (n, self.dims()?) } else { (String::new(), Vec::new()) };
                let (inp, out) = match pq.store { Store::Out => (false, true), Store::InOut => (true, true), _ => (true, false) };
                params.push(Param { out, inp, ty: pty, name: pn, dims: pd });
                if !self.eat(",") { break; }
            }
            self.expect(")")?;
            let body = if self.eat(";") { None } else { Some(self.block_body()?) };
            return Ok(Global::Func(Func { ret: ty, name, params, body, line }));
        }
        let decls = self.decl_rest(q, ty, name, line)?;
        if let Some(s) = sd { return Ok(Global::StructDecl(s, decls)); }
        Ok(Global::Decl(decls))
    }
    fn decl_rest(&mut self, q: Qual, ty: TypeSpec, first: String, line: u32) -> R<Vec<Decl>> {
        let mut v = Vec::new();
        let mut name = first;
        loop {
            let dims = self.dims()?;
            let init = if self.eat("=") { Some(self.initializer()?) } else { None };
            v.push(Decl { qual: q.clone(), ty: ty.clone(), name, dims, init, line });
            if !self.eat(",") { break; }
            name = self.ident()?;
        }
        self.expect(";")?;
        Ok(v)
    }
    fn initializer(&mut self) -> R<E> {
        if self.is("{") {
            /* { a, b, ... }: an initializer list, as a constructor of the declared type */
            let line = self.line();
            self.next();
            let mut a = Vec::new();
            while !self.eat("}") { a.push(self.initializer()?); if !self.eat(",") { self.expect("}")?; break; } }
            return Ok(E { k: Ex::Call(String::from("\u{1}list"), None, a), line });
        }
        self.expr(1)
    }
    fn block_body(&mut self) -> R<Vec<St>> {
        self.expect("{")?;
        let mut v = Vec::new();
        while !self.eat("}") { if *self.peek() == T::Eof { return self.err("expected '}'"); } v.push(self.stmt()?); }
        Ok(v)
    }
    fn starts_decl(&self) -> bool {
        match self.peek() {
            T::Id(n) => {
                if matches!(n.as_str(), "const" | "highp" | "mediump" | "lowp" | "struct" | "precise" | "invariant") { return true; }
                if self.is_type(n) {
                    /* "vec3 a" or "vec3[2] a" is a declaration; "vec3(" is a constructor */
                    return matches!(self.peek2(), T::Id(_) | T::P("["));
                }
                false
            }
            _ => false,
        }
    }
    fn stmt(&mut self) -> R<St> {
        let line = self.line();
        if self.is("{") { return Ok(St::Block(self.block_body()?)); }
        if self.eat(";") { return Ok(St::Empty); }
        if let T::Id(k) = self.peek().clone() {
            match k.as_str() {
                "if" => {
                    self.next(); self.expect("(")?; let c = self.expr(0)?; self.expect(")")?;
                    let t = self.stmt()?;
                    let e = if self.eat("else") { Some(Box::new(self.stmt()?)) } else { None };
                    return Ok(St::If(c, Box::new(t), e));
                }
                "for" => {
                    self.next(); self.expect("(")?;
                    let init = if self.eat(";") { None } else { Some(Box::new(self.simple_stmt()?)) };
                    let cond = if self.is(";") { None } else { Some(self.cond_or_decl()?) };
                    self.expect(";")?;
                    let step = if self.is(")") { None } else { Some(self.expr(0)?) };
                    self.expect(")")?;
                    let body = self.stmt()?;
                    return Ok(St::For(init, cond, step, Box::new(body)));
                }
                "while" => { self.next(); self.expect("(")?; let c = self.cond_or_decl()?; self.expect(")")?; let b = self.stmt()?; return Ok(St::While(c, Box::new(b))); }
                "do" => { self.next(); let b = self.stmt()?; self.expect("while")?; self.expect("(")?; let c = self.expr(0)?; self.expect(")")?; self.expect(";")?; return Ok(St::Do(Box::new(b), c)); }
                "switch" => {
                    self.next(); self.expect("(")?; let c = self.expr(0)?; self.expect(")")?;
                    let b = self.block_body()?;
                    return Ok(St::Switch(c, b));
                }
                "case" => { self.next(); let e = self.expr(1)?; self.expect(":")?; return Ok(St::Case(Some(e), line)); }
                "default" => { self.next(); self.expect(":")?; return Ok(St::Case(None, line)); }
                "break" => { self.next(); self.expect(";")?; return Ok(St::Break(line)); }
                "continue" => { self.next(); self.expect(";")?; return Ok(St::Continue(line)); }
                "discard" => { self.next(); self.expect(";")?; return Ok(St::Discard(line)); }
                "return" => { self.next(); let e = if self.is(";") { None } else { Some(self.expr(0)?) }; self.expect(";")?; return Ok(St::Return(e, line)); }
                _ => {}
            }
        }
        self.simple_stmt()
    }
    /* a declaration or an expression statement, ending with ';' */
    fn simple_stmt(&mut self) -> R<St> {
        let line = self.line();
        if self.is_id("precision") { while !self.eat(";") { self.next(); } return Ok(St::Empty); }
        if self.starts_decl() {
            let q = self.qualifiers()?;
            let (ty, sd) = self.type_spec()?;
            if let Some(s) = sd {
                if self.eat(";") { return Ok(St::Struct(s)); }
                let n = self.ident()?;
                let d = self.decl_rest(q, ty, n, line)?;
                return Ok(St::Block(alloc::vec![St::Struct(s), St::Decl(d)]));
            }
            let n = self.ident()?;
            return Ok(St::Decl(self.decl_rest(q, ty, n, line)?));
        }
        let e = self.expr(0)?;
        self.expect(";")?;
        Ok(St::Expr(e))
    }
    fn cond_or_decl(&mut self) -> R<E> {
        if self.starts_decl() { return self.err("declarations in conditions are not supported"); }
        self.expr(0)
    }

    /* ---------------- expressions ---------------- */
    pub fn expr(&mut self, min: u8) -> R<E> {
        /* min 0: comma allowed; 1: assignment level */
        let mut e = self.assign()?;
        if min == 0 {
            while self.is(",") {
                let line = self.line(); self.next();
                let r = self.assign()?;
                e = E { k: Ex::Comma(Box::new(e), Box::new(r)), line };
            }
        }
        Ok(e)
    }
    fn assign(&mut self) -> R<E> {
        let lhs = self.ternary()?;
        let line = self.line();
        let op = match self.peek() {
            T::P("=") => None, T::P("+=") => Some(Bin::Add), T::P("-=") => Some(Bin::Sub), T::P("*=") => Some(Bin::Mul), T::P("/=") => Some(Bin::Div),
            T::P("%=") => Some(Bin::Mod), T::P("<<=") => Some(Bin::Shl), T::P(">>=") => Some(Bin::Shr), T::P("&=") => Some(Bin::BAnd), T::P("|=") => Some(Bin::BOr), T::P("^=") => Some(Bin::BXor),
            _ => return Ok(lhs),
        };
        self.next();
        let rhs = self.assign()?;
        Ok(E { k: Ex::Assign(op, Box::new(lhs), Box::new(rhs)), line })
    }
    fn ternary(&mut self) -> R<E> {
        let c = self.binary(0)?;
        if self.is("?") {
            let line = self.line(); self.next();
            let a = self.expr(0)?;
            self.expect(":")?;
            let b = self.assign()?;
            return Ok(E { k: Ex::Cond(Box::new(c), Box::new(a), Box::new(b)), line });
        }
        Ok(c)
    }
    fn binop(&self) -> Option<(Bin, u8)> {
        Some(match self.peek() {
            T::P("||") => (Bin::Or, 1), T::P("^^") => (Bin::Xor, 2), T::P("&&") => (Bin::And, 3), T::P("|") => (Bin::BOr, 4), T::P("^") => (Bin::BXor, 5), T::P("&") => (Bin::BAnd, 6),
            T::P("==") => (Bin::Eq, 7), T::P("!=") => (Bin::Ne, 7), T::P("<") => (Bin::Lt, 8), T::P(">") => (Bin::Gt, 8), T::P("<=") => (Bin::Le, 8), T::P(">=") => (Bin::Ge, 8),
            T::P("<<") => (Bin::Shl, 9), T::P(">>") => (Bin::Shr, 9), T::P("+") => (Bin::Add, 10), T::P("-") => (Bin::Sub, 10),
            T::P("*") => (Bin::Mul, 11), T::P("/") => (Bin::Div, 11), T::P("%") => (Bin::Mod, 11),
            _ => return None,
        })
    }
    fn binary(&mut self, min: u8) -> R<E> {
        let mut a = self.unary()?;
        while let Some((op, pr)) = self.binop() {
            if pr <= min { break; }
            let line = self.line(); self.next();
            let b = self.binary(pr)?;
            a = E { k: Ex::Bin(op, Box::new(a), Box::new(b)), line };
        }
        Ok(a)
    }
    fn unary(&mut self) -> R<E> {
        let line = self.line();
        let op = match self.peek() {
            T::P("-") => Some(Un::Neg), T::P("+") => Some(Un::Plus), T::P("!") => Some(Un::Not), T::P("~") => Some(Un::BNot),
            T::P("++") => { self.next(); let e = self.unary()?; return Ok(E { k: Ex::IncDec(true, true, Box::new(e)), line }); }
            T::P("--") => { self.next(); let e = self.unary()?; return Ok(E { k: Ex::IncDec(true, false, Box::new(e)), line }); }
            _ => None,
        };
        if let Some(op) = op { self.next(); let e = self.unary()?; return Ok(E { k: Ex::Un(op, Box::new(e)), line }); }
        self.postfix()
    }
    fn postfix(&mut self) -> R<E> {
        let mut e = self.primary()?;
        loop {
            let line = self.line();
            if self.eat("[") { let i = self.expr(0)?; self.expect("]")?; e = E { k: Ex::Index(Box::new(e), Box::new(i)), line }; continue; }
            if self.eat(".") {
                let f = self.ident()?;
                if f == "length" && self.is("(") { self.next(); self.expect(")")?; e = E { k: Ex::Length(Box::new(e)), line }; continue; }
                e = E { k: Ex::Field(Box::new(e), f), line }; continue;
            }
            if self.eat("++") { e = E { k: Ex::IncDec(false, true, Box::new(e)), line }; continue; }
            if self.eat("--") { e = E { k: Ex::IncDec(false, false, Box::new(e)), line }; continue; }
            break;
        }
        Ok(e)
    }
    fn primary(&mut self) -> R<E> {
        let line = self.line();
        match self.next() {
            T::Int(v, u) => Ok(E { k: Ex::Int(v, u), line }),
            T::Float(f) => Ok(E { k: Ex::Float(f), line }),
            T::P("(") => { let e = self.expr(0)?; self.expect(")")?; Ok(e) }
            T::Id(n) => {
                if n == "true" || n == "false" { return Ok(E { k: Ex::Bool(n == "true"), line }); }
                /* T[n](...): an array constructor */
                let mut arr = None;
                if self.is_type(&n) && self.is("[") {
                    self.next();
                    arr = Some(Box::new(if self.is("]") { E { k: Ex::Int(-1, false), line } } else { self.expr(1)? }));
                    self.expect("]")?;
                }
                if self.eat("(") {
                    let mut a = Vec::new();
                    if self.is_id("void") && matches!(self.peek2(), T::P(")")) { self.next(); }
                    while !self.is(")") { a.push(self.assign()?); if !self.eat(",") { break; } }
                    self.expect(")")?;
                    return Ok(E { k: Ex::Call(n, arr, a), line });
                }
                if arr.is_some() { return self.err("expected '(' after an array type"); }
                Ok(E { k: Ex::Var(n), line })
            }
            _ => { self.p -= 1; self.err("expected an expression") }
        }
    }
}
