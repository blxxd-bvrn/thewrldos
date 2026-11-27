//! GLSL to the shared shader IR (src/win32/shader_priv.h) that the Direct3D 10 engine
//! interprets. Every function call is inlined (GLSL has no recursion); a function that
//! returns early runs inside a one-pass loop that `return` breaks out of. Values live in
//! vec4 registers: a scalar in one lane, a vector in the first lanes, a matrix in one
//! register per column, arrays and structures in consecutive registers. Uniforms are rows
//! of constant buffer 0, samplers are texture slots, attributes and varyings are input and
//! output registers named in the signatures.

use super::parse::*;
use crate::dxbc::*;
use alloc::boxed::Box;
use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

#[path = "gen_ex.rs"]
mod ex;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum B { F, I, U, Bo }
#[derive(Clone, PartialEq, Debug)]
pub enum Ty { Void, S(B, u8), M(u8, u8), Smp(u8), St(usize), A(Box<Ty>, u32) }

#[derive(Clone, Debug)]
pub enum Loc {
    R { f: u8, base: i32, rel: Option<(i32, u8)> },
    K(Vec<u32>),
    Smp(i32),
    Depth,
    Null,
}
#[derive(Clone, Debug)]
pub struct Val { pub ty: Ty, pub loc: Loc, pub swz: [u8; 4], pub lv: bool }

#[derive(Clone)]
pub struct StructT { pub name: String, pub fields: Vec<(String, Ty)> }

/// One uniform, attribute or sampler, for the C side (glsl.inc).
pub struct VarInfo { pub name: String, pub gltype: u32, pub count: i32, pub reg: i32, pub stride: i32, pub location: i32 }

type R<T> = Result<T, String>;

pub const FONE: u32 = 0x3F80_0000;
const PI: f32 = 3.141_592_7;

fn vreg(f: u8, base: i32, ty: Ty, lv: bool) -> Val { Val { ty, loc: Loc::R { f, base, rel: None }, swz: [0, 1, 2, 3], lv } }
pub fn kf(v: f32) -> Val { Val { ty: Ty::S(B::F, 1), loc: Loc::K(vec![v.to_bits()]), swz: [0; 4], lv: false } }
pub fn ki(v: i32) -> Val { Val { ty: Ty::S(B::I, 1), loc: Loc::K(vec![v as u32]), swz: [0; 4], lv: false } }
fn kb(v: bool) -> Val { Val { ty: Ty::S(B::Bo, 1), loc: Loc::K(vec![if v { !0 } else { 0 }]), swz: [0; 4], lv: false } }
fn kvec(b: B, c: Vec<u32>) -> Val { let n = c.len() as u8; Val { ty: Ty::S(b, n), loc: Loc::K(c), swz: [0, 1, 2, 3], lv: false } }

impl Ty {
    pub fn n(&self) -> u8 { match self { Ty::S(_, n) => *n, _ => 0 } }
    pub fn b(&self) -> B { match self { Ty::S(b, _) => *b, Ty::M(..) => B::F, _ => B::F } }
}

struct LoopCtx { kind: u8, brk: i32, cont: i32 }   // kind 0 plain, 1 wrapped (continue breaks an inner loop), 2 switch
struct FnCtx { ret: Option<Val>, wrapped: bool, loop_base: usize, retflag: i32, name: String }

pub struct Gen {
    pub stage: u8,                       // 0 fragment, 1 vertex
    #[allow(dead_code)]
    pub version: u32,                    // the #version (all versions compile the same way)
    pub structs: Vec<StructT>,
    funcs: Vec<Func>,
    pub ins: Vec<Ins>,
    pub pool: Vec<Opnd>,
    pub ntemps: i32,
    scopes: Vec<BTreeMap<String, Val>>,
    pub inputs: Vec<DxSig>,
    pub outputs: Vec<DxSig>,
    pub interp: [u8; DX_MAX_REG],
    next_in: i32, next_out: i32,
    pub uniforms: Vec<VarInfo>,
    pub attribs: Vec<VarInfo>,
    pub cb_rows: i32,
    pub nsamplers: i32,
    loops: Vec<LoopCtx>,
    fns: Vec<FnCtx>,
    pub writes_depth: bool,
    line: u32,
    frag_out: i32,
    pub dyn_lv: Vec<(Val, Val)>,     // v[i] targets with a dynamic i (see lvalue)
}

fn gltype(t: &Ty) -> u32 {
    match t {
        Ty::S(B::F, 1) => 0x1406, Ty::S(B::F, n) => 0x8B50 + *n as u32 - 2,
        Ty::S(B::I, 1) => 0x1404, Ty::S(B::I, n) => 0x8B53 + *n as u32 - 2,
        Ty::S(B::U, 1) => 0x1405, Ty::S(B::U, n) => 0x8DC6 + *n as u32 - 2,
        Ty::S(B::Bo, 1) => 0x8B56, Ty::S(B::Bo, n) => 0x8B57 + *n as u32 - 2,
        Ty::M(2, 2) => 0x8B5A, Ty::M(3, 3) => 0x8B5B, Ty::M(4, 4) => 0x8B5C,
        Ty::M(2, 3) => 0x8B65, Ty::M(2, 4) => 0x8B66, Ty::M(3, 2) => 0x8B67, Ty::M(3, 4) => 0x8B68, Ty::M(4, 2) => 0x8B69, Ty::M(4, 3) => 0x8B6A,
        Ty::Smp(1) => 0x8B60, Ty::Smp(2) => 0x8B5F, Ty::Smp(3) => 0x8B62, Ty::Smp(4) => 0x8B5D, Ty::Smp(_) => 0x8B5E,
        _ => 0,
    }
}

impl Gen {
    pub fn new(stage: u8, version: u32) -> Gen {
        let mut g = Gen {
            stage, version, structs: Vec::new(), funcs: Vec::new(), ins: Vec::new(), pool: Vec::new(), ntemps: 0,
            scopes: vec![BTreeMap::new()], inputs: Vec::new(), outputs: Vec::new(), interp: [2; DX_MAX_REG], next_in: 0, next_out: 0,
            uniforms: Vec::new(), attribs: Vec::new(), cb_rows: 0, nsamplers: 0, loops: Vec::new(), fns: Vec::new(), writes_depth: false, line: 0, frag_out: -1, dyn_lv: Vec::new(),
        };
        let v4 = Ty::S(B::F, 4); let v3 = Ty::S(B::F, 3); let f = Ty::S(B::F, 1);
        let s = |n: &str, f: Vec<(&str, Ty)>| StructT { name: n.to_string(), fields: f.into_iter().map(|(a, b)| (a.to_string(), b)).collect() };
        g.structs.push(s("gl_LightSourceParameters", vec![("ambient", v4.clone()), ("diffuse", v4.clone()), ("specular", v4.clone()), ("position", v4.clone()), ("halfVector", v4.clone()),
            ("spotDirection", v3.clone()), ("spotExponent", f.clone()), ("spotCutoff", f.clone()), ("spotCosCutoff", f.clone()), ("constantAttenuation", f.clone()), ("linearAttenuation", f.clone()), ("quadraticAttenuation", f.clone())]));
        g.structs.push(s("gl_MaterialParameters", vec![("emission", v4.clone()), ("ambient", v4.clone()), ("diffuse", v4.clone()), ("specular", v4.clone()), ("shininess", f.clone())]));
        g.structs.push(s("gl_LightModelParameters", vec![("ambient", v4.clone())]));
        g.structs.push(s("gl_LightModelProducts", vec![("sceneColor", v4.clone())]));
        g.structs.push(s("gl_LightProducts", vec![("ambient", v4.clone()), ("diffuse", v4.clone()), ("specular", v4.clone())]));
        g.structs.push(s("gl_FogParameters", vec![("color", v4.clone()), ("density", f.clone()), ("start", f.clone()), ("end", f.clone()), ("scale", f.clone())]));
        g.structs.push(s("gl_PointParameters", vec![("size", f.clone()), ("sizeMin", f.clone()), ("sizeMax", f.clone()), ("fadeThresholdSize", f.clone()), ("distanceConstantAttenuation", f.clone()), ("distanceLinearAttenuation", f.clone()), ("distanceQuadraticAttenuation", f.clone())]));
        g.structs.push(s("gl_DepthRangeParameters", vec![("near", f.clone()), ("far", f.clone()), ("diff", f)]));
        g
    }
    fn err<X>(&self, m: &str) -> R<X> { Err(format!("{}: {}", self.line, m)) }

    /* ---------------- types ---------------- */
    pub fn regs(&self, t: &Ty) -> i32 {
        match t {
            Ty::S(..) | Ty::Smp(_) => 1, Ty::M(c, _) => *c as i32, Ty::Void => 0,
            Ty::A(e, n) => self.regs(e) * *n as i32,
            Ty::St(i) => self.structs[*i].fields.iter().map(|f| self.regs(&f.1)).sum(),
        }
    }
    fn tyname(&self, t: &Ty) -> String {
        match t {
            Ty::Void => "void".into(),
            Ty::S(b, 1) => match b { B::F => "float", B::I => "int", B::U => "uint", B::Bo => "bool" }.into(),
            Ty::S(b, n) => format!("{}vec{}", match b { B::F => "", B::I => "i", B::U => "u", B::Bo => "b" }, n),
            Ty::M(c, r) => if c == r { format!("mat{}", c) } else { format!("mat{}x{}", c, r) },
            Ty::Smp(_) => "sampler".into(),
            Ty::St(i) => self.structs[*i].name.clone(),
            Ty::A(e, n) => format!("{}[{}]", self.tyname(e), n),
        }
    }
    fn base_type(&self, n: &str) -> R<Ty> {
        Ok(match n {
            "void" => Ty::Void, "float" | "double" => Ty::S(B::F, 1), "int" => Ty::S(B::I, 1), "uint" => Ty::S(B::U, 1), "bool" => Ty::S(B::Bo, 1),
            "vec2" | "dvec2" => Ty::S(B::F, 2), "vec3" | "dvec3" => Ty::S(B::F, 3), "vec4" | "dvec4" => Ty::S(B::F, 4),
            "ivec2" => Ty::S(B::I, 2), "ivec3" => Ty::S(B::I, 3), "ivec4" => Ty::S(B::I, 4),
            "uvec2" => Ty::S(B::U, 2), "uvec3" => Ty::S(B::U, 3), "uvec4" => Ty::S(B::U, 4),
            "bvec2" => Ty::S(B::Bo, 2), "bvec3" => Ty::S(B::Bo, 3), "bvec4" => Ty::S(B::Bo, 4),
            "mat2" | "mat2x2" => Ty::M(2, 2), "mat3" | "mat3x3" => Ty::M(3, 3), "mat4" | "mat4x4" => Ty::M(4, 4),
            "mat2x3" => Ty::M(2, 3), "mat2x4" => Ty::M(2, 4), "mat3x2" => Ty::M(3, 2), "mat3x4" => Ty::M(3, 4), "mat4x2" => Ty::M(4, 2), "mat4x3" => Ty::M(4, 3),
            "sampler2D" | "sampler2DRect" | "samplerExternalOES" | "isampler2D" | "usampler2D" | "sampler2DArray" => Ty::Smp(0),
            "samplerCube" | "samplerCubeShadow" => Ty::Smp(1), "sampler3D" => Ty::Smp(2), "sampler2DShadow" | "sampler2DRectShadow" | "sampler1DShadow" => Ty::Smp(3), "sampler1D" => Ty::Smp(4),
            _ => match self.structs.iter().position(|s| s.name == n) { Some(i) => Ty::St(i), None => return self.err(&format!("unknown type {}", n)) },
        })
    }
    fn const_int(&mut self, e: &E) -> R<i32> {
        let v = self.ex(e)?;
        match (&v.ty, &v.loc) {
            (Ty::S(B::I, 1) | Ty::S(B::U, 1), Loc::K(c)) => Ok(c[0] as i32),
            (Ty::S(B::F, 1), Loc::K(c)) => Ok(f32::from_bits(c[0]) as i32),
            _ => self.err("an array size must be a constant integer"),
        }
    }
    fn with_dims(&mut self, mut t: Ty, dims: &[Option<E>], unsized_len: Option<u32>) -> R<Ty> {
        for d in dims.iter().rev() {
            let n = match d { Some(e) => self.const_int(e)?, None => match unsized_len { Some(n) => n as i32, None => return self.err("an array needs a size") } };
            if n <= 0 || n > 4096 { return self.err("bad array size"); }
            t = Ty::A(Box::new(t), n as u32);
        }
        Ok(t)
    }
    fn spec_type(&mut self, s: &TypeSpec) -> R<Ty> { let b = self.base_type(&s.name)?; self.with_dims(b, &s.dims, None) }

    /* ---------------- registers and operands ---------------- */
    fn temp(&mut self, t: &Ty) -> R<Val> {
        let n = self.regs(t).max(1);
        if self.ntemps + n > SHADER_MAX_TEMPS { return self.err("the shader needs too many registers"); }
        let v = vreg(T_TEMP, self.ntemps, t.clone(), true);
        self.ntemps += n;
        Ok(v)
    }
    fn tempi(&mut self) -> i32 { let r = self.ntemps; self.ntemps += 1; r }
    fn push_op(&mut self, o: Opnd) -> R<i16> {
        if self.pool.len() >= SHADER_MAX_OPERANDS as usize { return self.err("the shader is too long"); }
        self.pool.push(o);
        Ok((self.pool.len() - 1) as i16)
    }
    fn reg_opnd(&mut self, f: u8, base: i32, rel: Option<(i32, u8)>, dst: bool, mask: u8, swz: [u8; 4], md: u8) -> R<i16> {
        let mut o = Opnd { ty: f, mode: if dst { 0 } else { 1 }, mask: if dst { mask } else { 15 }, md, dim: 1, rel: [-1; 3], swz, ..Default::default() };
        if f == T_CB { o.dim = 2; o.idx[0] = 0; o.idx[1] = base; } else { o.idx[0] = base; }
        let k = self.push_op(o)?;
        if let Some((r, lane)) = rel {
            let ro = Opnd { ty: T_TEMP, mode: 1, mask: 15, dim: 1, idx: [r, 0, 0], rel: [-1; 3], swz: [lane; 4], ..Default::default() };
            let rk = self.push_op(ro)?;
            self.pool[k as usize].rel[if f == T_CB { 1 } else { 0 }] = rk;
        }
        Ok(k)
    }
    /// A source operand whose lane L holds component map[L] of v.
    fn src_map(&mut self, v: &Val, map: [u8; 4], md: u8) -> R<i16> {
        let n = v.ty.n().max(1) as usize;
        let m = map.map(|x| (x as usize).min(n - 1));
        match &v.loc {
            Loc::K(c) => {
                let mut imm = [0u32; 4];
                for l in 0..4 { imm[l] = c[m[l].min(c.len() - 1)]; }
                let o = Opnd { ty: T_IMM32, mode: 1, mask: 15, md, dim: 0, rel: [-1; 3], swz: [0, 1, 2, 3], imm, ..Default::default() };
                self.push_op(o)
            }
            Loc::R { f, base, rel } => { let swz = m.map(|x| v.swz[x]); self.reg_opnd(*f, *base, *rel, false, 15, swz, md) }
            Loc::Depth => self.reg_opnd(T_ODEPTH, 0, None, false, 15, [0; 4], md).map(|k| { self.pool[k as usize].dim = 0; k }),
            _ => self.err("this value cannot be read"),
        }
    }
    fn src(&mut self, v: &Val) -> R<i16> { self.src_map(v, [0, 1, 2, 3], 0) }
    fn src_md(&mut self, v: &Val, md: u8) -> R<i16> { self.src_map(v, [0, 1, 2, 3], md) }
    fn dst(&mut self, v: &Val) -> R<i16> {
        let n = v.ty.n().max(1) as usize;
        let mut mask = 0u8;
        for i in 0..n { mask |= 1 << v.swz[i]; }
        match &v.loc {
            Loc::R { f, base, rel } => self.reg_opnd(*f, *base, *rel, true, mask, [0, 1, 2, 3], 0),
            Loc::Depth => { let o = Opnd { ty: T_ODEPTH, mode: 0, mask: 1, dim: 0, rel: [-1; 3], swz: [0, 1, 2, 3], ..Default::default() }; self.writes_depth = true; self.push_op(o) }
            Loc::Null => { let o = Opnd { ty: T_NULL, mode: 0, mask: 0, dim: 0, rel: [-1; 3], ..Default::default() }; self.push_op(o) }
            _ => self.err("cannot assign to this"),
        }
    }
    fn null(&mut self) -> R<i16> { let o = Opnd { ty: T_NULL, mode: 0, mask: 0, dim: 0, rel: [-1; 3], ..Default::default() }; self.push_op(o) }
    fn emit(&mut self, op: u16, ops: &[i16]) -> R<usize> { self.emit_nz(op, 0, ops) }
    fn emit_nz(&mut self, op: u16, nz: u8, ops: &[i16]) -> R<usize> {
        if self.ins.len() >= SHADER_MAX_INS as usize { return self.err("the shader is too long"); }
        let mut i = Ins { op, nonzero: nz, nops: ops.len() as u8, jump: -1, alt: -1, ..Default::default() };
        for (k, o) in ops.iter().enumerate() { i.o[k] = *o; }
        self.ins.push(i);
        Ok(self.ins.len() - 1)
    }
    /// A new temporary of type t (a scalar or vector) = op(srcs).
    fn alu(&mut self, op: u16, t: Ty, srcs: &[&Val]) -> R<Val> { self.alu_md(op, t, srcs, &[]) }
    fn alu_md(&mut self, op: u16, t: Ty, srcs: &[&Val], mds: &[u8]) -> R<Val> {
        let d = self.temp(&t)?;
        let mut ops = vec![self.dst(&d)?];
        for (k, s) in srcs.iter().enumerate() { let md = mds.get(k).copied().unwrap_or(0); let o = self.src_md(s, md)?; ops.push(o); }
        self.emit(op, &ops)?;
        Ok(Val { lv: false, ..d })
    }
    fn alu_sat(&mut self, op: u16, t: Ty, srcs: &[&Val]) -> R<Val> {
        let v = self.alu(op, t, srcs)?;
        if let Some(i) = self.ins.last_mut() { i.sat = 1; }
        Ok(v)
    }
    fn mov_to(&mut self, d: &Val, s: &Val) -> R<()> {
        let n = d.ty.n().max(1) as usize;
        let mut map = [0u8; 4];
        for i in 0..n { map[d.swz[i] as usize] = i as u8; }
        let dd = self.dst(d)?;
        let ss = self.src_map(s, map, 0)?;
        self.emit(OP_MOV, &[dd, ss])?;
        Ok(())
    }
    fn materialize(&mut self, v: &Val) -> R<Val> {
        if let Loc::K(_) = v.loc { let t = self.temp(&v.ty)?; self.mov_to(&t, v)?; return Ok(Val { lv: false, ..t }); }
        Ok(v.clone())
    }
    /// The scalar component i of a vector value.
    fn comp(&self, v: &Val, i: usize) -> Val {
        let b = v.ty.b();
        match &v.loc {
            Loc::K(c) => Val { ty: Ty::S(b, 1), loc: Loc::K(vec![c[i.min(c.len() - 1)]]), swz: [0; 4], lv: false },
            _ => { let s = v.swz[i.min(3)]; Val { ty: Ty::S(b, 1), loc: v.loc.clone(), swz: [s; 4], lv: v.lv } }
        }
    }
    /// A part of a composite value: `off` registers in, of type t.
    fn part(&mut self, v: &Val, off: i32, t: Ty) -> R<Val> {
        match &v.loc {
            Loc::R { f, base, rel } => Ok(Val { ty: t, loc: Loc::R { f: *f, base: base + off, rel: *rel }, swz: [0, 1, 2, 3], lv: v.lv }),
            Loc::Smp(s) => Ok(Val { ty: t, loc: Loc::Smp(s + off), swz: [0, 1, 2, 3], lv: false }),
            _ => self.err("cannot take a part of this value"),
        }
    }
    fn is_const(v: &Val) -> bool { matches!(v.loc, Loc::K(_)) }
    fn kcomps(v: &Val) -> Vec<u32> { if let Loc::K(c) = &v.loc { (0..v.ty.n().max(1) as usize).map(|i| c[i.min(c.len() - 1)]).collect() } else { Vec::new() } }

    /* ---------------- conversions ---------------- */
    fn conv(&mut self, v: Val, to: B) -> R<Val> {
        let (from, n) = match v.ty { Ty::S(b, n) => (b, n), _ => return Ok(v) };
        if from == to { return Ok(v); }
        if let Loc::K(_) = v.loc {
            let c = Self::kcomps(&v);
            let o: Vec<u32> = c.iter().map(|&x| match (from, to) {
                (B::I, B::F) => (x as i32 as f32).to_bits(), (B::U, B::F) => (x as f32).to_bits(), (B::Bo, B::F) => if x != 0 { FONE } else { 0 },
                (B::F, B::I) => f32::from_bits(x) as i32 as u32, (B::F, B::U) => f32::from_bits(x) as u32,
                (B::Bo, B::I) | (B::Bo, B::U) => (x != 0) as u32,
                (B::F, B::Bo) => if f32::from_bits(x) != 0.0 { !0 } else { 0 },
                (_, B::Bo) => if x != 0 { !0 } else { 0 },
                _ => x,
            }).collect();
            return Ok(kvec(to, o));
        }
        let t = Ty::S(to, n);
        match (from, to) {
            (B::I, B::F) => self.alu(OP_ITOF, t, &[&v]),
            (B::U, B::F) => self.alu(OP_UTOF, t, &[&v]),
            (B::Bo, B::F) => self.alu(OP_AND, t, &[&v, &kvec(B::U, vec![FONE])]),
            (B::F, B::I) => self.alu(OP_FTOI, t, &[&v]),
            (B::F, B::U) => self.alu(OP_FTOU, t, &[&v]),
            (B::Bo, B::I) | (B::Bo, B::U) => self.alu(OP_AND, t, &[&v, &kvec(B::U, vec![1])]),
            (B::F, B::Bo) => self.alu(OP_NE, t, &[&v, &kf(0.0)]),
            (_, B::Bo) => self.alu(OP_INE, t, &[&v, &ki(0)]),
            _ => Ok(Val { ty: t, ..v }),
        }
    }
    /// v as type t, with the implicit conversions GLSL (and lenient drivers) allow.
    fn coerce(&mut self, v: Val, t: &Ty) -> R<Val> {
        if &v.ty == t { return Ok(v); }
        match (&v.ty, t) {
            (Ty::S(_, a), Ty::S(b, c)) if a == c => self.conv(v, *b),
            (Ty::S(_, 1), Ty::S(b, _)) => { let s = self.conv(v, *b)?; self.ctor(t, vec![s]) }
            _ => self.err(&format!("cannot convert {} to {}", self.tyname(&v.ty), self.tyname(t))),
        }
    }
    fn to_bool(&mut self, v: Val) -> R<Val> {
        match v.ty { Ty::S(_, 1) => self.conv(v, B::Bo), _ => self.err("a condition must be a scalar") }
    }

    /* ---------------- storing ---------------- */
    fn store(&mut self, d: &Val, s: Val) -> R<()> {
        if !d.lv { return self.err("cannot assign to this (it is read-only)"); }
        let s = self.coerce(s, &d.ty)?;
        self.copy(d, &s)
    }
    fn copy(&mut self, d: &Val, s: &Val) -> R<()> {
        match &d.ty {
            Ty::S(..) => self.mov_to(d, s),
            Ty::M(c, r) => { for k in 0..*c as i32 { let dp = self.part(d, k, Ty::S(B::F, *r))?; let sp = self.part(s, k, Ty::S(B::F, *r))?; self.mov_to(&dp, &sp)?; } Ok(()) }
            Ty::A(e, n) => { let er = self.regs(e); for k in 0..*n as i32 { let dp = self.part(d, k * er, (**e).clone())?; let sp = self.part(s, k * er, (**e).clone())?; self.copy(&dp, &sp)?; } Ok(()) }
            Ty::St(i) => {
                let f = self.structs[*i].fields.clone();
                let mut off = 0;
                for (_, ft) in f { let dp = self.part(d, off, ft.clone())?; let sp = self.part(s, off, ft.clone())?; self.copy(&dp, &sp)?; off += self.regs(&ft); }
                Ok(())
            }
            _ => self.err("cannot assign this type"),
        }
    }

    /* ---------------- signatures, uniforms ---------------- */
    fn sig(&mut self, out: bool, name: &str, index: i32, reg: i32, t: &Ty, sysval: i32, interp: Interp) {
        let mut s = DxSig::default();
        let nb = name.as_bytes();
        let l = nb.len().min(31);
        s.name[..l].copy_from_slice(&nb[..l]);
        s.index = index; s.sysval = sysval; s.reg = reg;
        s.mask = match t { Ty::S(_, n) => (1 << *n) - 1, _ => 15 };
        s.component_type = match t.b() { B::F => 3, B::I => 2, _ => 1 };
        if !out && self.stage == 0 && (reg as usize) < DX_MAX_REG {
            self.interp[reg as usize] = if t.b() != B::F || interp == Interp::Flat { 1 } else if interp == Interp::NoPersp { 4 } else { 2 };
        }
        if out { if self.outputs.len() < DX_MAX_SIG { self.outputs.push(s); } } else if self.inputs.len() < DX_MAX_SIG { self.inputs.push(s); }
    }
    /// Input (or output) registers for a varying or attribute of type t: one signature
    /// entry per register, all with the same name and consecutive indices.
    fn io(&mut self, out: bool, name: &str, t: &Ty, sysval: i32, interp: Interp) -> R<i32> {
        let n = self.regs(t);
        let base = if out { self.next_out } else { self.next_in };
        if base + n > DX_MAX_REG as i32 - 1 { return self.err("too many attributes or varyings"); }
        let (elem, cnt) = match t { Ty::A(e, c) => ((**e).clone(), *c as i32), _ => (t.clone(), 1) };
        let er = self.regs(&elem);
        let col = match &elem { Ty::M(_, r) => Ty::S(B::F, *r), x => x.clone() };
        for k in 0..cnt { for j in 0..er { self.sig(out, name, k * er + j, base + k * er + j, &col, sysval, interp); } }
        if out { self.next_out += n; } else { self.next_in += n; }
        Ok(base)
    }
    fn uniform_info(&mut self, name: String, t: &Ty, reg: i32) {
        match t {
            Ty::St(i) => {
                let f = self.structs[*i].fields.clone();
                let mut off = 0;
                for (fname, ft) in f { self.uniform_info(format!("{}.{}", name, fname), &ft, reg + off); off += self.regs(&ft); }
            }
            Ty::A(e, n) if matches!(**e, Ty::St(_) | Ty::A(..)) => {
                let er = self.regs(e);
                for k in 0..*n as i32 { self.uniform_info(format!("{}[{}]", name, k), e, reg + k * er); }
            }
            Ty::A(e, n) => { let s = self.regs(e); self.uniforms.push(VarInfo { name, gltype: gltype(e), count: *n as i32, reg, stride: s, location: -1 }); }
            _ => { let s = self.regs(t); self.uniforms.push(VarInfo { name, gltype: gltype(t), count: 1, reg, stride: s, location: -1 }); }
        }
    }
    fn uniform(&mut self, name: &str, t: &Ty) -> R<Val> {
        let smp = matches!(t, Ty::Smp(_)) || matches!(t, Ty::A(e, _) if matches!(**e, Ty::Smp(_)));
        if smp {
            let n = match t { Ty::A(_, n) => *n as i32, _ => 1 };
            if self.nsamplers + n > 16 { return self.err("too many samplers (16 is the limit)"); }
            let s = self.nsamplers; self.nsamplers += n;
            let e = match t { Ty::A(e, _) => (**e).clone(), x => x.clone() };
            self.uniforms.push(VarInfo { name: name.to_string(), gltype: gltype(&e), count: n, reg: s, stride: 1, location: -1 });
            return Ok(Val { ty: t.clone(), loc: Loc::Smp(s), swz: [0, 1, 2, 3], lv: false });
        }
        let n = self.regs(t);
        if self.cb_rows + n > 4096 { return self.err("too many uniforms"); }
        let r = self.cb_rows; self.cb_rows += n;
        self.uniform_info(name.to_string(), t, r);
        Ok(vreg(T_CB, r, t.clone(), false))
    }

    /* ---------------- built-in variables ---------------- */
    fn builtin_var(&mut self, n: &str) -> R<Option<Val>> {
        let v4 = Ty::S(B::F, 4);
        let st = |g: &Gen, name: &str| Ty::St(g.structs.iter().position(|s| s.name == name).unwrap_or(0));
        let konst = |v: i32| Some(ki(v));
        let v = match n {
            "gl_MaxLights" | "gl_MaxClipPlanes" | "gl_MaxTextureCoords" | "gl_MaxTextureUnits" | "gl_MaxDrawBuffers" | "gl_MaxClipDistances" => konst(8),
            "gl_MaxVertexAttribs" | "gl_MaxTextureImageUnits" | "gl_MaxCombinedTextureImageUnits" | "gl_MaxVertexTextureImageUnits" | "gl_MaxVaryingVectors" => konst(16),
            "gl_MaxVertexUniformComponents" | "gl_MaxFragmentUniformComponents" => konst(1024),
            "gl_MaxVaryingFloats" | "gl_MaxVaryingComponents" => konst(64),
            "gl_MaxVertexUniformVectors" | "gl_MaxFragmentUniformVectors" => konst(256),
            "gl_ModelViewMatrix" | "gl_ProjectionMatrix" | "gl_ModelViewProjectionMatrix" | "gl_ModelViewMatrixInverse" | "gl_ProjectionMatrixInverse"
            | "gl_ModelViewProjectionMatrixInverse" | "gl_ModelViewMatrixTranspose" | "gl_ProjectionMatrixTranspose" | "gl_ModelViewProjectionMatrixTranspose"
            | "gl_ModelViewMatrixInverseTranspose" | "gl_ProjectionMatrixInverseTranspose" | "gl_ModelViewProjectionMatrixInverseTranspose" => Some(self.uniform(n, &Ty::M(4, 4))?),
            "gl_TextureMatrix" | "gl_TextureMatrixInverse" | "gl_TextureMatrixTranspose" | "gl_TextureMatrixInverseTranspose" => Some(self.uniform(n, &Ty::A(Box::new(Ty::M(4, 4)), 8))?),
            "gl_NormalMatrix" => Some(self.uniform(n, &Ty::M(3, 3))?),
            "gl_NormalScale" => Some(self.uniform(n, &Ty::S(B::F, 1))?),
            "gl_LightSource" => { let t = st(self, "gl_LightSourceParameters"); Some(self.uniform(n, &Ty::A(Box::new(t), 8))?) }
            "gl_FrontMaterial" | "gl_BackMaterial" => { let t = st(self, "gl_MaterialParameters"); Some(self.uniform(n, &t)?) }
            "gl_LightModel" => { let t = st(self, "gl_LightModelParameters"); Some(self.uniform(n, &t)?) }
            "gl_FrontLightModelProduct" | "gl_BackLightModelProduct" => { let t = st(self, "gl_LightModelProducts"); Some(self.uniform(n, &t)?) }
            "gl_FrontLightProduct" | "gl_BackLightProduct" => { let t = st(self, "gl_LightProducts"); Some(self.uniform(n, &Ty::A(Box::new(t), 8))?) }
            "gl_Fog" => { let t = st(self, "gl_FogParameters"); Some(self.uniform(n, &t)?) }
            "gl_Point" => { let t = st(self, "gl_PointParameters"); Some(self.uniform(n, &t)?) }
            "gl_DepthRange" => { let t = st(self, "gl_DepthRangeParameters"); Some(self.uniform(n, &t)?) }
            "gl_TextureEnvColor" | "gl_ClipPlane" | "gl_EyePlaneS" | "gl_EyePlaneT" | "gl_EyePlaneR" | "gl_EyePlaneQ"
            | "gl_ObjectPlaneS" | "gl_ObjectPlaneT" | "gl_ObjectPlaneR" | "gl_ObjectPlaneQ" => Some(self.uniform(n, &Ty::A(Box::new(v4.clone()), 8))?),
            _ => None,
        };
        if v.is_some() { return Ok(v); }
        let flt = |k: u8| Ty::S(B::F, k);
        let v = if self.stage == 1 {
            match n {
                "gl_Vertex" | "gl_Color" | "gl_SecondaryColor" | "gl_MultiTexCoord0" | "gl_MultiTexCoord1" | "gl_MultiTexCoord2" | "gl_MultiTexCoord3"
                | "gl_MultiTexCoord4" | "gl_MultiTexCoord5" | "gl_MultiTexCoord6" | "gl_MultiTexCoord7" | "gl_Normal" | "gl_FogCoord" => {
                    let t = if n == "gl_Normal" { flt(3) } else if n == "gl_FogCoord" { flt(1) } else { v4.clone() };
                    let r = self.io(false, n, &t, 0, Interp::Smooth)?;
                    self.attribs.push(VarInfo { name: n.to_string(), gltype: gltype(&t), count: 1, reg: r, stride: 1, location: -1 });
                    Some(vreg(T_INPUT, r, t, false))
                }
                "gl_VertexID" | "gl_InstanceID" | "gl_VertexIndex" | "gl_InstanceIndex" => {
                    let sv = if n.starts_with("gl_Vertex") { 6 } else { 8 };
                    let r = self.io(false, n, &Ty::S(B::I, 1), sv, Interp::Flat)?;
                    Some(vreg(T_INPUT, r, Ty::S(B::I, 1), false))
                }
                "gl_FrontColor" | "gl_BackColor" | "gl_FrontSecondaryColor" | "gl_BackSecondaryColor" => { let r = self.io(true, n, &v4, 0, Interp::Smooth)?; Some(vreg(T_OUTPUT, r, v4.clone(), true)) }
                "gl_TexCoord" => { let t = Ty::A(Box::new(v4.clone()), 8); let r = self.io(true, n, &t, 0, Interp::Smooth)?; Some(vreg(T_OUTPUT, r, t, true)) }
                "gl_FogFragCoord" => { let r = self.io(true, n, &flt(1), 0, Interp::Smooth)?; Some(vreg(T_OUTPUT, r, flt(1), true)) }
                "gl_PointSize" => Some(self.temp(&flt(1))?),
                "gl_ClipVertex" => Some(self.temp(&v4)?),
                "gl_ClipDistance" => Some(self.temp(&Ty::A(Box::new(flt(1)), 8))?),
                _ => None,
            }
        } else {
            match n {
                "gl_Color" | "gl_SecondaryColor" => {
                    let link = if n == "gl_Color" { "gl_FrontColor" } else { "gl_FrontSecondaryColor" };
                    let r = self.io(false, link, &v4, 0, Interp::Smooth)?; Some(vreg(T_INPUT, r, v4.clone(), false))
                }
                "gl_TexCoord" => { let t = Ty::A(Box::new(v4.clone()), 8); let r = self.io(false, n, &t, 0, Interp::Smooth)?; Some(vreg(T_INPUT, r, t, false)) }
                "gl_FogFragCoord" => { let r = self.io(false, n, &flt(1), 0, Interp::Smooth)?; Some(vreg(T_INPUT, r, flt(1), false)) }
                "gl_FragCoord" => { let r = self.io(false, n, &v4, 1, Interp::NoPersp)?; Some(vreg(T_INPUT, r, v4.clone(), false)) }
                "gl_FrontFacing" => { let r = self.io(false, n, &Ty::S(B::U, 1), 9, Interp::Flat)?; Some(Val { ty: Ty::S(B::Bo, 1), ..vreg(T_INPUT, r, Ty::S(B::Bo, 1), false) }) }
                "gl_PointCoord" => Some(kvec(B::F, vec![0.5f32.to_bits(), 0.5f32.to_bits()])),
                "gl_FragColor" => { let r = self.frag_target()?; Some(vreg(T_OUTPUT, r, v4.clone(), true)) }
                "gl_FragData" => { let r = self.frag_target()?; let t = Ty::A(Box::new(v4.clone()), 1); Some(vreg(T_OUTPUT, r, t, true)) }
                "gl_FragDepth" => Some(Val { ty: flt(1), loc: Loc::Depth, swz: [0; 4], lv: true }),
                _ => None,
            }
        };
        Ok(v)
    }
    fn frag_target(&mut self) -> R<i32> {
        if self.frag_out < 0 {
            let r = self.next_out; self.next_out += 1;
            self.sig(true, "SV_Target", 0, r, &Ty::S(B::F, 4), 64, Interp::Smooth);
            self.frag_out = r;
        }
        Ok(self.frag_out)
    }
    fn lookup(&mut self, n: &str) -> R<Val> {
        for s in self.scopes.iter().rev() { if let Some(v) = s.get(n) { return Ok(v.clone()); } }
        if n.starts_with("gl_") {
            if let Some(v) = self.builtin_var(n)? { self.scopes[0].insert(n.to_string(), v.clone()); return Ok(v); }
        }
        self.err(&format!("'{}' is not declared", n))
    }
    fn bind(&mut self, n: &str, v: Val) { self.scopes.last_mut().unwrap().insert(n.to_string(), v); }

    /* ---------------- declarations ---------------- */
    pub fn global(&mut self, g: &Global) -> R<()> {
        match g {
            Global::Empty => Ok(()),
            Global::Struct(s) => self.struct_def(s),
            Global::StructDecl(s, d) => { self.struct_def(s)?; for x in d { self.global_decl(x)?; } Ok(()) }
            Global::Func(f) => { self.line = f.line; self.funcs.push(f.clone()); Ok(()) }
            Global::Decl(d) => { for x in d { self.global_decl(x)?; } Ok(()) }
            Global::Block(q, inst, members) => {
                /* uniform blocks: members become uniforms (with the instance name as a prefix) */
                for m in members {
                    let mut d = m.clone(); d.qual.store = q.store.clone();
                    let name = if inst.is_empty() { d.name.clone() } else { format!("{}.{}", inst, d.name) };
                    let t0 = self.spec_type(&d.ty)?;
                    let t = self.with_dims(t0, &d.dims, None)?;
                    let v = if q.store == Store::Uniform { self.uniform(&name, &t)? } else { self.temp(&t)? };
                    if inst.is_empty() { self.bind(&d.name, v); } else { self.bind(&name, v); }
                }
                if !inst.is_empty() {
                    /* "inst.member" is looked up as one name: see the Field case */
                    let prefix = format!("\u{2}{}", inst);
                    self.bind(&prefix, kb(true));
                }
                Ok(())
            }
        }
    }
    fn struct_def(&mut self, s: &StructDef) -> R<()> {
        self.line = s.line;
        let mut f = Vec::new();
        for (ts, n, d) in &s.fields { let t0 = self.spec_type(ts)?; let t = self.with_dims(t0, d, None)?; f.push((n.clone(), t)); }
        self.structs.push(StructT { name: s.name.clone(), fields: f });
        Ok(())
    }
    fn decl_type(&mut self, d: &Decl) -> R<Ty> {
        let t0 = self.spec_type(&d.ty)?;
        let unsized_len = match &d.init { Some(E { k: Ex::Call(_, _, a), .. }) => Some(a.len() as u32), _ => None };
        self.with_dims(t0, &d.dims, unsized_len)
    }
    fn global_decl(&mut self, d: &Decl) -> R<()> {
        self.line = d.line;
        let t = self.decl_type(d)?;
        let st = &d.qual.store;
        let v = match (st, self.stage) {
            (Store::Uniform, _) => self.uniform(&d.name, &t)?,
            (Store::Attribute, 1) | (Store::In, 1) => {
                let r = self.io(false, &d.name, &t, 0, Interp::Smooth)?;
                let (et, n) = match &t { Ty::A(e, n) => ((**e).clone(), *n as i32), x => (x.clone(), 1) };
                let s = self.regs(&et);
                self.attribs.push(VarInfo { name: d.name.clone(), gltype: gltype(&et), count: n, reg: r, stride: s, location: d.qual.location });
                vreg(T_INPUT, r, t, false)
            }
            (Store::Varying, 1) | (Store::Out, 1) => { let r = self.io(true, &d.name, &t, 0, d.qual.interp)?; vreg(T_OUTPUT, r, t, true) }
            (Store::Varying, 0) | (Store::In, 0) => { let r = self.io(false, &d.name, &t, 0, d.qual.interp)?; vreg(T_INPUT, r, t, false) }
            (Store::Out, 0) => {
                /* fragment outputs: location 0 (or the first) is the colour; the others are not drawn */
                let r = if d.qual.location <= 0 && self.frag_out < 0 { self.frag_target()? } else { let r = self.next_out; self.next_out += self.regs(&t); r };
                vreg(T_OUTPUT, r, t, true)
            }
            (Store::Attribute, _) => return self.err("attributes are for vertex shaders"),
            _ => return self.local_decl(d, t),
        };
        self.bind(&d.name, v);
        Ok(())
    }
    fn local_decl(&mut self, d: &Decl, t: Ty) -> R<()> {
        self.line = d.line;
        if let Some(init) = &d.init {
            let v = self.init_value(init, &t)?;
            if d.qual.store == Store::Const && Self::is_const(&v) && matches!(t, Ty::S(..)) {
                let v = self.coerce(v, &t)?;
                self.bind(&d.name, v);
                return Ok(());
            }
            let tv = self.temp(&t)?;
            self.store(&tv, v)?;
            let ro = d.qual.store == Store::Const;
            self.bind(&d.name, Val { lv: !ro, ..tv });
        } else {
            let tv = self.temp(&t)?;
            self.bind(&d.name, tv);
        }
        Ok(())
    }
    fn init_value(&mut self, e: &E, t: &Ty) -> R<Val> {
        if let Ex::Call(n, _, a) = &e.k {
            if n == "\u{1}list" {
                let mut args = Vec::new();
                for x in a { args.push(self.ex(x)?); }
                return self.ctor(t, args);
            }
        }
        self.ex(e)
    }

    /* ---------------- statements ---------------- */
    pub fn main(&mut self) -> R<()> {
        let idx = self.funcs.iter().position(|f| f.name == "main" && f.body.is_some()).ok_or_else(|| "0: no main() function".to_string())?;
        let f = self.funcs[idx].clone();
        self.line = f.line;
        if self.stage == 1 {
            /* gl_Position is always an output, so the pipeline has a position */
            if self.next_out == 0 || !self.outputs.iter().any(|s| s.sysval == 1) {
                let r = self.next_out; self.next_out += 1;
                self.sig(true, "gl_Position", 0, r, &Ty::S(B::F, 4), 1, Interp::Smooth);
                self.scopes[0].insert("gl_Position".into(), vreg(T_OUTPUT, r, Ty::S(B::F, 4), true));
            }
        } else {
            self.frag_target()?;
            let r = self.frag_out;
            self.scopes[0].entry("gl_FragColor".into()).or_insert(vreg(T_OUTPUT, r, Ty::S(B::F, 4), true));
        }
        self.scopes.push(BTreeMap::new());
        for s in f.body.as_ref().unwrap() { self.stmt(s)?; }
        self.scopes.pop();
        self.emit(OP_RET, &[])?;
        Ok(())
    }
    fn block(&mut self, v: &[St]) -> R<()> {
        self.scopes.push(BTreeMap::new());
        for s in v { self.stmt(s)?; }
        self.scopes.pop();
        Ok(())
    }
    fn cond_op(&mut self, c: &E) -> R<Option<i16>> {
        let v = self.ex(c)?;
        let v = self.to_bool(v)?;
        if Self::is_const(&v) { return Ok(None); }
        Ok(Some(self.src(&v)?))
    }
    fn after_loop(&mut self) -> R<()> {
        if let Some(f) = self.fns.last() {
            if f.retflag >= 0 && self.loops.len() >= f.loop_base {
                let r = f.retflag;
                let o = self.reg_opnd(T_TEMP, r, None, false, 15, [0; 4], 0)?;
                self.emit_nz(OP_BREAKC, 1, &[o])?;
            }
        }
        Ok(())
    }
    fn set_flag(&mut self, r: i32) -> R<()> {
        let d = self.reg_opnd(T_TEMP, r, None, true, 1, [0, 1, 2, 3], 0)?;
        let s = self.src(&kvec(B::U, vec![!0]))?;
        self.emit(OP_MOV, &[d, s])?;
        Ok(())
    }
    fn clear_flag(&mut self, r: i32) -> R<()> {
        let d = self.reg_opnd(T_TEMP, r, None, true, 1, [0, 1, 2, 3], 0)?;
        let s = self.src(&kvec(B::U, vec![0]))?;
        self.emit(OP_MOV, &[d, s])?;
        Ok(())
    }
    fn has_continue(s: &St) -> bool {
        match s {
            St::Continue(_) => true,
            St::Block(v) => v.iter().any(Self::has_continue),
            St::If(_, a, b) => Self::has_continue(a) || b.as_ref().map(|b| Self::has_continue(b)).unwrap_or(false),
            St::Switch(_, v) => v.iter().any(Self::has_continue),
            _ => false,   // loops own their continues
        }
    }
    /// A loop body with `continue` runs inside an inner one-pass loop, so continue can
    /// skip to the step part; `break` then sets a flag and the outer loop checks it.
    fn loop_body(&mut self, body: &St) -> R<()> {
        if Self::has_continue(body) {
            let brk = self.tempi();
            self.clear_flag(brk)?;
            self.emit(OP_LOOP, &[])?;
            self.loops.push(LoopCtx { kind: 1, brk, cont: -1 });
            self.scopes.push(BTreeMap::new());
            self.stmt(body)?;
            self.scopes.pop();
            self.loops.pop();
            self.emit(OP_BREAK, &[])?;
            self.emit(OP_ENDLOOP, &[])?;
            self.after_loop()?;
            let o = self.reg_opnd(T_TEMP, brk, None, false, 15, [0; 4], 0)?;
            self.emit_nz(OP_BREAKC, 1, &[o])?;
        } else {
            self.loops.push(LoopCtx { kind: 0, brk: -1, cont: -1 });
            self.scopes.push(BTreeMap::new());
            self.stmt(body)?;
            self.scopes.pop();
            self.loops.pop();
        }
        Ok(())
    }
    fn stmt(&mut self, s: &St) -> R<()> {
        match s {
            St::Empty => Ok(()),
            St::Block(v) => self.block(v),
            St::Struct(sd) => self.struct_def(sd),
            St::Decl(ds) => { for d in ds { self.line = d.line; let t = self.decl_type(d)?; self.local_decl(d, t)?; } Ok(()) }
            St::Expr(e) => { self.ex(e)?; Ok(()) }
            St::If(c, a, b) => {
                self.line = c.line;
                let v = self.ex(c)?;
                let v = self.to_bool(v)?;
                if let Loc::K(k) = &v.loc {
                    if k[0] != 0 { self.scoped(a)?; } else if let Some(b) = b { self.scoped(b)?; }
                    return Ok(());
                }
                let o = self.src(&v)?;
                self.emit_nz(OP_IF, 1, &[o])?;
                self.scoped(a)?;
                if let Some(b) = b { self.emit(OP_ELSE, &[])?; self.scoped(b)?; }
                self.emit(OP_ENDIF, &[])?;
                Ok(())
            }
            St::For(init, cond, step, body) => {
                self.scopes.push(BTreeMap::new());
                if let Some(i) = init { self.stmt(i)?; }
                self.emit(OP_LOOP, &[])?;
                if let Some(c) = cond {
                    self.line = c.line;
                    match self.cond_op(c)? {
                        Some(o) => { self.emit_nz(OP_BREAKC, 0, &[o])?; }
                        None => { let v = self.ex(c)?; let v = self.to_bool(v)?; if Self::kcomps(&v)[0] == 0 { self.emit(OP_BREAK, &[])?; } }
                    }
                }
                self.loop_body(body)?;
                if let Some(st) = step { self.ex(st)?; }
                self.emit(OP_ENDLOOP, &[])?;
                self.after_loop()?;
                self.scopes.pop();
                Ok(())
            }
            St::While(c, body) => {
                self.emit(OP_LOOP, &[])?;
                self.line = c.line;
                match self.cond_op(c)? {
                    Some(o) => { self.emit_nz(OP_BREAKC, 0, &[o])?; }
                    None => { let v = self.ex(c)?; let v = self.to_bool(v)?; if Self::kcomps(&v)[0] == 0 { self.emit(OP_BREAK, &[])?; } }
                }
                self.loop_body(body)?;
                self.emit(OP_ENDLOOP, &[])?;
                self.after_loop()
            }
            St::Do(body, c) => {
                self.emit(OP_LOOP, &[])?;
                self.loop_body(body)?;
                self.line = c.line;
                match self.cond_op(c)? {
                    Some(o) => { self.emit_nz(OP_BREAKC, 0, &[o])?; }
                    None => { let v = self.ex(c)?; let v = self.to_bool(v)?; if Self::kcomps(&v)[0] == 0 { self.emit(OP_BREAK, &[])?; } }
                }
                self.emit(OP_ENDLOOP, &[])?;
                self.after_loop()
            }
            St::Switch(sel, body) => self.switch(sel, body),
            St::Case(_, l) => { self.line = *l; self.err("case outside switch") }
            St::Break(l) => {
                self.line = *l;
                match self.loops.last() {
                    None => self.err("break outside a loop"),
                    Some(LoopCtx { kind: 1, brk, .. }) => { let b = *brk; self.set_flag(b)?; self.emit(OP_BREAK, &[])?; Ok(()) }
                    Some(_) => { self.emit(OP_BREAK, &[])?; Ok(()) }
                }
            }
            St::Continue(l) => {
                self.line = *l;
                match self.loops.last() {
                    None => self.err("continue outside a loop"),
                    Some(LoopCtx { kind: 0, .. }) => { self.emit(OP_CONTINUE, &[])?; Ok(()) }
                    Some(LoopCtx { kind: 1, .. }) => { self.emit(OP_BREAK, &[])?; Ok(()) }
                    Some(LoopCtx { cont, .. }) => { let c = *cont; if c < 0 { return self.err("continue inside a switch is not supported here"); } self.set_flag(c)?; self.emit(OP_BREAK, &[])?; Ok(()) }
                }
            }
            St::Discard(l) => {
                self.line = *l;
                if self.stage != 0 { return self.err("discard is for fragment shaders"); }
                let o = self.src(&kvec(B::U, vec![!0]))?;
                self.emit_nz(OP_DISCARD, 1, &[o])?;
                Ok(())
            }
            St::Return(e, l) => {
                self.line = *l;
                if self.fns.is_empty() {
                    if e.is_some() { return self.err("main() returns nothing"); }
                    self.emit(OP_RET, &[])?;
                    return Ok(());
                }
                if let Some(e) = e {
                    let v = self.ex(e)?;
                    let r = self.fns.last().unwrap().ret.clone();
                    match r { Some(r) => self.store(&r, v)?, None => return self.err("this function returns nothing") }
                } else if self.fns.last().unwrap().ret.is_some() { return self.err("return needs a value"); }
                let (wrapped, base, flag) = { let f = self.fns.last().unwrap(); (f.wrapped, f.loop_base, f.retflag) };
                if !wrapped { return Ok(()); }
                if self.loops.len() > base {
                    if flag < 0 { return self.err("internal: return inside a loop"); }
                    self.set_flag(flag)?;
                }
                self.emit(OP_BREAK, &[])?;
                Ok(())
            }
        }
    }
    fn scoped(&mut self, s: &St) -> R<()> { self.scopes.push(BTreeMap::new()); let r = self.stmt(s); self.scopes.pop(); r }
    fn switch(&mut self, sel: &E, body: &[St]) -> R<()> {
        self.line = sel.line;
        let v = self.ex(sel)?;
        let v = self.coerce(v, &Ty::S(B::I, 1))?;
        let sv = self.materialize(&v)?;
        /* is there any case that matches? (for default) */
        let any = self.temp(&Ty::S(B::Bo, 1))?;
        self.store(&any, kb(false))?;
        for s in body {
            if let St::Case(Some(e), l) = s {
                self.line = *l;
                let c = self.ex(e)?; let c = self.coerce(c, &Ty::S(B::I, 1))?;
                let eq = self.alu(OP_IEQ, Ty::S(B::Bo, 1), &[&sv, &c])?;
                let o = self.alu(OP_OR, Ty::S(B::Bo, 1), &[&any, &eq])?;
                self.copy(&any, &o)?;
            }
        }
        let matched = self.temp(&Ty::S(B::Bo, 1))?;
        self.store(&matched, kb(false))?;
        let cont = match self.loops.last() { Some(LoopCtx { kind: 0, .. }) | Some(LoopCtx { kind: 1, .. }) => { let c = self.tempi(); self.clear_flag(c)?; c } _ => -1 };
        self.emit(OP_LOOP, &[])?;
        self.loops.push(LoopCtx { kind: 2, brk: -1, cont });
        self.scopes.push(BTreeMap::new());
        let mut open = false;
        for s in body {
            match s {
                St::Case(e, l) => {
                    self.line = *l;
                    if open { self.emit(OP_ENDIF, &[])?; open = false; }
                    let hit = match e {
                        Some(e) => { let c = self.ex(e)?; let c = self.coerce(c, &Ty::S(B::I, 1))?; self.alu(OP_IEQ, Ty::S(B::Bo, 1), &[&sv, &c])? }
                        None => self.alu(OP_NOT, Ty::S(B::Bo, 1), &[&any])?,
                    };
                    let o = self.alu(OP_OR, Ty::S(B::Bo, 1), &[&matched, &hit])?;
                    self.copy(&matched, &o)?;
                }
                _ => {
                    if !open { let o = self.src(&matched)?; self.emit_nz(OP_IF, 1, &[o])?; open = true; }
                    self.stmt(s)?;
                }
            }
        }
        if open { self.emit(OP_ENDIF, &[])?; }
        self.scopes.pop();
        self.loops.pop();
        self.emit(OP_BREAK, &[])?;
        self.emit(OP_ENDLOOP, &[])?;
        self.after_loop()?;
        if cont >= 0 {
            let o = self.reg_opnd(T_TEMP, cont, None, false, 15, [0; 4], 0)?;
            match self.loops.last() { Some(LoopCtx { kind: 0, .. }) => { self.emit_nz(OP_CONTINUEC, 1, &[o])?; } _ => { self.emit_nz(OP_BREAKC, 1, &[o])?; } }
        }
        Ok(())
    }

    /* ---------------- function calls ---------------- */
    fn count_returns(v: &[St]) -> (u32, bool) {
        fn walk(s: &St, n: &mut u32, in_loop: bool, lp: &mut bool) {
            match s {
                St::Return(..) => { *n += 1; if in_loop { *lp = true; } }
                St::Block(v) => for x in v { walk(x, n, in_loop, lp) },
                St::If(_, a, b) => { walk(a, n, in_loop, lp); if let Some(b) = b { walk(b, n, in_loop, lp); } }
                St::For(_, _, _, b) | St::While(_, b) | St::Do(b, _) => walk(b, n, true, lp),
                St::Switch(_, v) => for x in v { walk(x, n, true, lp) },
                _ => {}
            }
        }
        let mut n = 0; let mut lp = false;
        for s in v { walk(s, &mut n, false, &mut lp); }
        (n, lp)
    }
    fn call_user(&mut self, name: &str, args: &[E]) -> R<Option<Val>> {
        let cands: Vec<usize> = (0..self.funcs.len()).filter(|&i| self.funcs[i].name == name && self.funcs[i].params.len() == args.len()).collect();
        if cands.is_empty() { return Ok(None); }
        let mut vals = Vec::new();
        for a in args { vals.push(self.ex(a)?); }
        /* the overload: exact types first, then with conversions */
        let mut pick = None;
        for pass in 0..2 {
            for &i in &cands {
                let f = self.funcs[i].clone();
                if f.body.is_none() { continue; }
                let mut ok = true;
                for (k, p) in f.params.iter().enumerate() {
                    let t0 = self.spec_type(&p.ty)?; let pt = self.with_dims(t0, &p.dims, None)?;
                    let at = &vals[k].ty;
                    let fits = if pass == 0 { *at == pt } else { match (at, &pt) { (Ty::S(_, a), Ty::S(B::F, b)) => a == b, (Ty::S(_, a), Ty::S(_, b)) => a == b, (a, b) => a == b } };
                    if !fits { ok = false; break; }
                }
                if ok { pick = Some(i); break; }
            }
            if pick.is_some() { break; }
        }
        let i = match pick { Some(i) => i, None => {
            if cands.iter().all(|&i| self.funcs[i].body.is_none()) { return self.err(&format!("function {} is declared but not defined", name)); }
            return self.err(&format!("no version of {} takes these arguments", name));
        } };
        let f = self.funcs[i].clone();
        if self.fns.iter().any(|c| c.name == f.name) { return self.err("recursion is not allowed in GLSL"); }
        if self.fns.len() > 32 { return self.err("functions nest too deeply"); }
        let line = self.line;
        self.scopes.push(BTreeMap::new());
        let mut outs = Vec::new();
        for (k, p) in f.params.iter().enumerate() {
            let t0 = self.spec_type(&p.ty)?; let pt = self.with_dims(t0, &p.dims, None)?;
            let tv = self.temp(&pt)?;
            if p.inp { let a = vals[k].clone(); self.store(&tv, a)?; }
            if p.out { if !vals[k].lv { return self.err(&format!("argument {} of {} must be assignable", k + 1, name)); } outs.push((tv.clone(), vals[k].clone())); }
            if !p.name.is_empty() { self.bind(&p.name, tv); }
        }
        let rt = self.spec_type(&f.ret)?;
        let ret = if rt == Ty::Void { None } else { Some(self.temp(&rt)?) };
        let body = f.body.clone().unwrap();
        let (nret, in_loop) = Self::count_returns(&body);
        let last_is_ret = matches!(body.last(), Some(St::Return(..)));
        let wrapped = nret > if last_is_ret { 1 } else { 0 };
        let retflag = if wrapped && in_loop { let r = self.tempi(); self.clear_flag(r)?; r } else { -1 };
        if wrapped { self.emit(OP_LOOP, &[])?; }
        self.fns.push(FnCtx { ret: ret.clone(), wrapped, loop_base: self.loops.len(), retflag, name: f.name.clone() });
        let saved_loops = core::mem::take(&mut self.loops);
        self.fns.last_mut().unwrap().loop_base = 0;
        let r = (|| { for s in &body { self.stmt(s)?; } Ok::<(), String>(()) })();
        self.loops = saved_loops;
        self.fns.pop();
        r?;
        if wrapped { self.emit(OP_BREAK, &[])?; self.emit(OP_ENDLOOP, &[])?; }
        self.scopes.pop();
        self.line = line;
        for (p, a) in outs { let v = Val { lv: false, ..p }; self.store(&a, v)?; }
        Ok(Some(match ret { Some(r) => Val { lv: false, ..r }, None => Val { ty: Ty::Void, loc: Loc::Null, swz: [0; 4], lv: false } }))
    }
}
