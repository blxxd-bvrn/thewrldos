//! Direct3D shader bytecode (was `src/win32/dxbc.c`): the DXBC container and its input and
//! output signatures, the Shader Model 4/5 program tokens, the checks every program passes
//! before it runs (`shader_finish`, shared with the DXIL reader in `dxil.rs`), and the
//! interpreter that runs vertex and pixel shaders for Direct3D 10 (src/win32/d3d10.c).
//!
//! The program is decoded once into a compact instruction list (`ins_t`, `opnd_t` in
//! src/win32/shader_priv.h, which the SVGA3D lowering in shader_gpu.c also reads) and then
//! interpreted for every vertex and pixel. The structures stay the C ones, allocated with
//! the C allocator (kmalloc), so C code that builds or reads them is unchanged and
//! `dxbc_free` frees them either way. Every value in the instruction list is checked when
//! the shader is loaded; the interpreter relies on that, as the C one did, and checks
//! dynamic (relative) indices itself. Floating-point results are the same bits as the C
//! code's: the same operations in the same order, and the same C library functions for
//! square roots, exponentials, logarithms, sines and cosines.

#![allow(clippy::missing_safety_doc)]

use crate::CBool;
use core::ffi::{c_char, c_int, c_void};
use core::fmt::{self, Write};
use core::ptr;

pub const OP_ADD: u16 = 0;
pub const OP_AND: u16 = 1;
pub const OP_BREAK: u16 = 2;
pub const OP_BREAKC: u16 = 3;
pub const OP_CALL: u16 = 4;
pub const OP_CALLC: u16 = 5;
pub const OP_CASE: u16 = 6;
pub const OP_CONTINUE: u16 = 7;
pub const OP_CONTINUEC: u16 = 8;
pub const OP_CUT: u16 = 9;
pub const OP_DEFAULT: u16 = 10;
pub const OP_DERIV_RTX: u16 = 11;
pub const OP_DERIV_RTY: u16 = 12;
pub const OP_DISCARD: u16 = 13;
pub const OP_DIV: u16 = 14;
pub const OP_DP2: u16 = 15;
pub const OP_DP3: u16 = 16;
pub const OP_DP4: u16 = 17;
pub const OP_ELSE: u16 = 18;
pub const OP_EMIT: u16 = 19;
pub const OP_EMITTHENCUT: u16 = 20;
pub const OP_ENDIF: u16 = 21;
pub const OP_ENDLOOP: u16 = 22;
pub const OP_ENDSWITCH: u16 = 23;
pub const OP_EQ: u16 = 24;
pub const OP_EXP: u16 = 25;
pub const OP_FRC: u16 = 26;
pub const OP_FTOI: u16 = 27;
pub const OP_FTOU: u16 = 28;
pub const OP_GE: u16 = 29;
pub const OP_IADD: u16 = 30;
pub const OP_IF: u16 = 31;
pub const OP_IEQ: u16 = 32;
pub const OP_IGE: u16 = 33;
pub const OP_ILT: u16 = 34;
pub const OP_IMAD: u16 = 35;
pub const OP_IMAX: u16 = 36;
pub const OP_IMIN: u16 = 37;
pub const OP_IMUL: u16 = 38;
pub const OP_INE: u16 = 39;
pub const OP_INEG: u16 = 40;
pub const OP_ISHL: u16 = 41;
pub const OP_ISHR: u16 = 42;
pub const OP_ITOF: u16 = 43;
pub const OP_LABEL: u16 = 44;
pub const OP_LD: u16 = 45;
pub const OP_LD_MS: u16 = 46;
pub const OP_LOG: u16 = 47;
pub const OP_LOOP: u16 = 48;
pub const OP_LT: u16 = 49;
pub const OP_MAD: u16 = 50;
pub const OP_MIN: u16 = 51;
pub const OP_MAX: u16 = 52;
pub const OP_CUSTOMDATA: u16 = 53;
pub const OP_MOV: u16 = 54;
pub const OP_MOVC: u16 = 55;
pub const OP_MUL: u16 = 56;
pub const OP_NE: u16 = 57;
pub const OP_NOP: u16 = 58;
pub const OP_NOT: u16 = 59;
pub const OP_OR: u16 = 60;
pub const OP_RESINFO: u16 = 61;
pub const OP_RET: u16 = 62;
pub const OP_RETC: u16 = 63;
pub const OP_ROUND_NE: u16 = 64;
pub const OP_ROUND_NI: u16 = 65;
pub const OP_ROUND_PI: u16 = 66;
pub const OP_ROUND_Z: u16 = 67;
pub const OP_RSQ: u16 = 68;
pub const OP_SAMPLE: u16 = 69;
pub const OP_SAMPLE_C: u16 = 70;
pub const OP_SAMPLE_C_LZ: u16 = 71;
pub const OP_SAMPLE_L: u16 = 72;
pub const OP_SAMPLE_D: u16 = 73;
pub const OP_SAMPLE_B: u16 = 74;
pub const OP_SQRT: u16 = 75;
pub const OP_SWITCH: u16 = 76;
pub const OP_SINCOS: u16 = 77;
pub const OP_UDIV: u16 = 78;
pub const OP_ULT: u16 = 79;
pub const OP_UGE: u16 = 80;
pub const OP_UMUL: u16 = 81;
pub const OP_UMAD: u16 = 82;
pub const OP_UMAX: u16 = 83;
pub const OP_UMIN: u16 = 84;
pub const OP_USHR: u16 = 85;
pub const OP_UTOF: u16 = 86;
pub const OP_XOR: u16 = 87;
pub const OP_RCP: u16 = 129;
pub const OP_F32TOF16: u16 = 130;
pub const OP_F16TOF32: u16 = 131;
pub const OP_UADDC: u16 = 132;
pub const OP_USUBB: u16 = 133;
pub const OP_COUNTBITS: u16 = 134;
pub const OP_FIRSTBIT_HI: u16 = 135;
pub const OP_FIRSTBIT_LO: u16 = 136;
pub const OP_FIRSTBIT_SHI: u16 = 137;
pub const OP_UBFE: u16 = 138;
pub const OP_IBFE: u16 = 139;
pub const OP_BFI: u16 = 140;
pub const OP_BFREV: u16 = 141;
pub const OP_SWAPC: u16 = 142;
pub const OP_BLOCK: u16 = 256;
pub const OP_BRANCH: u16 = 257;
pub const OP_PHI: u16 = 258;
pub const OP_DCL_FIRST: u16 = 88; pub const OP_DCL_TEMPS: u16 = 104; pub const OP_DCL_INDEXABLE_TEMP: u16 = 105; pub const OP_DCL_LAST: u16 = 106;
pub const T_TEMP: u8 = 0;
pub const T_INPUT: u8 = 1;
pub const T_OUTPUT: u8 = 2;
pub const T_XTEMP: u8 = 3;
pub const T_IMM32: u8 = 4;
pub const T_IMM64: u8 = 5;
pub const T_SAMPLER: u8 = 6;
pub const T_RESOURCE: u8 = 7;
pub const T_CB: u8 = 8;
pub const T_ICB: u8 = 9;
pub const T_LABEL: u8 = 10;
pub const T_PRIMID: u8 = 11;
pub const T_ODEPTH: u8 = 12;
pub const T_NULL: u8 = 13;

pub const DX_MAX_SIG: usize = 32;
pub const DX_MAX_REG: usize = 32;
pub const MAX_XTEMP: usize = 16;
pub const SHADER_MAX_OPERANDS: i32 = 32767;
pub const SHADER_MAX_INS: i32 = 65536;
pub const SHADER_MAX_TEMPS: i32 = 4096;

extern "C" {
    fn kmalloc(n: usize) -> *mut c_void;
    fn kzalloc(n: usize) -> *mut c_void;
    fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
    /* src/win32/shader_abi.c: no float passes by value between Rust and C (the 64-bit
     * kernel's Rust code is soft-float, C passes floats in SSE registers) */
    fn dxbc_math(func: c_int, x: *const f64, r: *mut f64);
    fn dxbc_sample(env: *const Env, tex: c_int, smp: c_int, uv: *const f32, lod: *const f32, out: *mut f32);
}
/// The C library's sqrt, sin, cos, exp2 and log2 (0..4), as the C interpreter called them.
#[inline(always)]
fn libm(func: c_int, x: f32) -> f32 { let (a, mut r) = (x as f64, 0f64); unsafe { dxbc_math(func, &a, &mut r) } r as f32 }

/// `vec4_t`: four 32-bit lanes, read as floats, unsigned or signed integers.
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Vec4 { pub u: [u32; 4] }
impl Vec4 {
    #[inline(always)] fn f(&self, i: usize) -> f32 { f32::from_bits(self.u[i]) }
    #[inline(always)] fn i(&self, i: usize) -> i32 { self.u[i] as i32 }
    #[inline(always)] fn setf(&mut self, i: usize, v: f32) { self.u[i] = v.to_bits() }
}

/// `dxsig_t`: one element of an input or output signature.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DxSig { pub name: [u8; 32], pub index: i32, pub sysval: i32, pub reg: i32, pub mask: i32, pub component_type: i32 }

/// `opnd_t`
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Opnd {
    pub ty: u8, pub mode: u8, pub mask: u8, pub md: u8, pub dim: u8, pub direct: u8, pub identity: u8,
    pub swz: [u8; 4],
    pub rel: [i16; 3],
    pub idx: [i32; 3],
    pub imm: [u32; 4],
}
/// `ins_t`
#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Ins { pub op: u16, pub sat: u8, pub nonzero: u8, pub nops: u8, pub ret_type: u8, pub mask: u8, pub o: [i16; 5], pub jump: i32, pub alt: i32 }

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Xt { pub reg: i32, pub size: i32 }

/// `struct dxbc_shader` (shader_priv.h).
#[repr(C)]
pub struct Shader {
    pub ty: i32, pub major: i32, pub minor: i32,
    pub dxil: CBool, pub writes_depth: CBool,
    pub inp: [DxSig; DX_MAX_SIG], pub out: [DxSig; DX_MAX_SIG],
    pub nin: i32, pub nout: i32,
    pub ins: *mut Ins, pub nins: i32,
    pub pool: *mut Opnd, pub npool: i32,
    pub ntemps: i32,
    pub xt: [Xt; MAX_XTEMP], pub nxt: i32, pub xt_total: i32,
    pub icb: *mut Vec4, pub nicb: i32,
    pub read_mask: [u8; DX_MAX_REG], pub write_mask: [u8; DX_MAX_REG], pub definite_mask: [u8; DX_MAX_REG],
    pub interpolation: [u8; DX_MAX_REG],
    pub interpolation_declared: u32,
    pub clear_regs: [u8; DX_MAX_REG],
    pub nclear: i32,
}
impl Shader {
    pub fn pool(&self) -> &[Opnd] { if self.pool.is_null() || self.npool <= 0 { &[] } else { unsafe { core::slice::from_raw_parts(self.pool, self.npool as usize) } } }
    pub fn pool_mut(&mut self) -> &mut [Opnd] { if self.pool.is_null() || self.npool <= 0 { &mut [] } else { unsafe { core::slice::from_raw_parts_mut(self.pool, self.npool as usize) } } }
    pub fn ins(&self) -> &[Ins] { if self.ins.is_null() || self.nins <= 0 { &[] } else { unsafe { core::slice::from_raw_parts(self.ins, self.nins as usize) } } }
    pub fn ins_mut(&mut self) -> &mut [Ins] { if self.ins.is_null() || self.nins <= 0 { &mut [] } else { unsafe { core::slice::from_raw_parts_mut(self.ins, self.nins as usize) } } }
}

pub type SampleFn = unsafe extern "C" fn(ctx: *mut c_void, tex: c_int, smp: c_int, uv: *const f32, lod: f32, out: *mut f32);
pub type LoadFn = unsafe extern "C" fn(ctx: *mut c_void, tex: c_int, xy: *const i32, out: *mut f32);
pub type SizeFn = unsafe extern "C" fn(ctx: *mut c_void, tex: c_int, out: *mut u32);

/// `dxbc_env_t`: constant buffers and texture access for one draw.
#[repr(C)]
pub struct Env {
    pub cb: [*const Vec4; 14],
    pub cb_rows: [u32; 14],
    pub ctx: *mut c_void,
    pub sample: Option<SampleFn>,
    pub load: Option<LoadFn>,
    pub size: Option<SizeFn>,
    pub primitive_id: u32,
}

/// `dxbc_exec_t`: one prepared run (bind once per draw, run many times).
#[repr(C)]
pub struct Exec {
    pub shader: *const Shader,
    pub env: *const Env,
    pub inp: *const Vec4,
    pub out: *mut Vec4, pub r: *mut Vec4, pub x: *mut Vec4,
    pub bound: *mut *const u32,
    pub storage: *mut c_void,
    pub scratch: Vec4,
    pub block: i32, pub previous_block: i32,
    pub discard: CBool,
}

/// The layout C reports for these structures (`rust_shader_layout`, src/win32/shader_abi.c).
pub fn layout() -> [u32; 24] {
    use core::mem::{offset_of, size_of};
    [size_of::<DxSig>() as u32, size_of::<Opnd>() as u32, offset_of!(Opnd, rel) as u32, offset_of!(Opnd, idx) as u32,
     offset_of!(Opnd, imm) as u32, size_of::<Ins>() as u32, offset_of!(Ins, o) as u32, offset_of!(Ins, jump) as u32,
     size_of::<Shader>() as u32, offset_of!(Shader, inp) as u32, offset_of!(Shader, ins) as u32, offset_of!(Shader, pool) as u32,
     offset_of!(Shader, xt) as u32, offset_of!(Shader, icb) as u32, offset_of!(Shader, read_mask) as u32,
     offset_of!(Shader, interpolation_declared) as u32, offset_of!(Shader, nclear) as u32,
     size_of::<Env>() as u32, offset_of!(Env, ctx) as u32, offset_of!(Env, primitive_id) as u32,
     size_of::<Exec>() as u32, offset_of!(Exec, storage) as u32, offset_of!(Exec, scratch) as u32, offset_of!(Exec, discard) as u32]
}

/* ---------------- error messages: snprintf into the caller's buffer ---------------- */

pub(crate) struct ErrBuf { p: *mut u8, cap: usize, n: usize }
impl ErrBuf {
    pub(crate) fn new(err: *mut c_char, errlen: c_int) -> Option<ErrBuf> {
        if err.is_null() || errlen <= 0 { None } else { Some(ErrBuf { p: err as *mut u8, cap: errlen as usize, n: 0 }) }
    }
    pub(crate) fn bytes(&mut self, b: &[u8]) {
        for &c in b {
            if self.n + 1 < self.cap { unsafe { *self.p.add(self.n) = c } self.n += 1; }
        }
    }
    pub(crate) fn finish(&mut self) { unsafe { *self.p.add(self.n) = 0 } }
}
impl Write for ErrBuf {
    fn write_str(&mut self, s: &str) -> fmt::Result { self.bytes(s.as_bytes()); Ok(()) }
}
pub(crate) fn set_err(err: *mut c_char, errlen: c_int, args: fmt::Arguments) {
    if let Some(mut b) = ErrBuf::new(err, errlen) { let _ = b.write_fmt(args); b.finish(); }
}
/// For messages with a raw byte string in them (`%s` of a name read from the program).
pub(crate) fn set_err_parts(err: *mut c_char, errlen: c_int, parts: &[&[u8]]) {
    if let Some(mut b) = ErrBuf::new(err, errlen) { for p in parts { b.bytes(p) } b.finish(); }
}
macro_rules! seterr { ($e:expr, $l:expr, $($a:tt)*) => { $crate::dxbc::set_err($e, $l, format_args!($($a)*)) } }
pub(crate) use seterr;

pub(crate) fn cstr(b: &[u8]) -> &[u8] { let n = b.iter().position(|&c| c == 0).unwrap_or(b.len()); &b[..n] }
pub(crate) fn ieq(a: &[u8], b: &[u8]) -> bool { cstr(a).eq_ignore_ascii_case(cstr(b)) }

fn word(d: &[u8], at: usize) -> u32 {
    match d.get(at..at + 4) { Some(b) => u32::from_le_bytes([b[0], b[1], b[2], b[3]]), None => 0 }
}

/* ---------------- the C interface: queries ---------------- */

#[no_mangle] pub unsafe extern "C" fn dxbc_type(s: *const Shader) -> c_int { (*s).ty }
#[no_mangle] pub unsafe extern "C" fn dxbc_inputs(s: *const Shader, sig: *mut *const DxSig) -> c_int { *sig = (*s).inp.as_ptr(); (*s).nin }
#[no_mangle] pub unsafe extern "C" fn dxbc_outputs(s: *const Shader, sig: *mut *const DxSig) -> c_int { *sig = (*s).out.as_ptr(); (*s).nout }
#[no_mangle] pub unsafe extern "C" fn dxbc_model(s: *const Shader, minor: *mut c_int) -> c_int { if !minor.is_null() { *minor = (*s).minor } (*s).major }
#[no_mangle] pub unsafe extern "C" fn dxbc_is_dxil(s: *const Shader) -> CBool { (*s).dxil }
#[no_mangle] pub unsafe extern "C" fn dxbc_read_mask(s: *const Shader, reg: c_int) -> c_int { if reg >= 0 && (reg as usize) < DX_MAX_REG { (*s).read_mask[reg as usize] as c_int } else { 0 } }
#[no_mangle] pub unsafe extern "C" fn dxbc_interpolation(s: *const Shader, reg: c_int) -> c_int { if reg >= 0 && (reg as usize) < DX_MAX_REG { (*s).interpolation[reg as usize] as c_int } else { 2 } }
#[no_mangle] pub unsafe extern "C" fn dxbc_writes_depth(s: *const Shader) -> CBool { (*s).writes_depth }
#[no_mangle] pub unsafe extern "C" fn dxbc_free(s: *mut Shader) {
    if s.is_null() { return; }
    crate::jit::forget(s); crate::jit4::forget(s);
    kfree((*s).ins as *mut c_void); kfree((*s).pool as *mut c_void); kfree((*s).icb as *mut c_void); kfree(s as *mut c_void);
}

/* ---------------- signatures ---------------- */

fn parse_sig(d: &[u8], sig: &mut [DxSig; DX_MAX_SIG], stride: u32) -> i32 {
    let size = d.len() as u32;
    if size < 8 { return -1; }
    let (n, table) = (word(d, 0), word(d, 4));
    if n > DX_MAX_SIG as u32 || table < 8 || table > size || n > (size - table) / stride { return -1; }
    for i in 0..n {
        let e = (table + i * stride + if stride > 24 { 4 } else { 0 }) as usize;
        if stride > 24 && word(d, e - 4) != 0 { return -1; }   /* stream zero only */
        let no = word(d, e);
        let mut s = DxSig::default();
        if no >= size { sig[i as usize] = s; return -1; }
        let mut k = 0u32;
        while k < size - no && d[(no + k) as usize] != 0 && k < 31 { s.name[k as usize] = d[(no + k) as usize]; k += 1; }
        if k == size - no || d[(no + k) as usize] != 0 { sig[i as usize] = s; return -1; }
        s.index = word(d, e + 4) as i32; s.sysval = word(d, e + 8) as i32; s.component_type = word(d, e + 12) as i32;
        /* FXC's legacy OSGN encodes render targets with NONE; DXC's OSG1 uses TARGET. */
        if s.sysval == 0 && ieq(&s.name, b"SV_Target") { s.sysval = 64; }
        if s.sysval == 0 && ieq(&s.name, b"SV_Depth") { s.sysval = 65; }
        s.reg = word(d, e + 16) as i32; s.mask = (word(d, e + 20) & 0xF) as i32;
        sig[i as usize] = s;
        if s.reg < 0 && (s.reg != -1 || s.sysval != 65) { return -1; }
        if s.reg >= DX_MAX_REG as i32 - 1 || s.mask == 0 || s.component_type < 1 || s.component_type > 3 { return -1; }
        for j in 0..i as usize {
            if sig[j].reg == s.reg && (sig[j].mask & s.mask) != 0 { return -1; }
        }
    }
    n as i32
}

/* ---------------- growing the C arrays ---------------- */

/// Append to one of the shader's kmalloc'd arrays, doubling `cap` (capped at `max`).
pub(crate) unsafe fn grow<T>(arr: &mut *mut T, n: i32, cap: &mut i32, first: i32, max: i32) -> bool {
    if n < *cap { return true; }
    let c = core::cmp::min(max, if *cap != 0 { *cap * 2 } else { first });
    let p = krealloc(*arr as *mut c_void, core::mem::size_of::<T>() * c as usize) as *mut T;
    if p.is_null() { return false; }
    *arr = p; *cap = c; true
}

/* ---------------- operand decoding ---------------- */

struct Dec<'a> { t: &'a [u32], pos: u32, end: u32, s: *mut Shader, cap: i32, depth: i32, bad: bool, why: Option<&'static str> }
impl<'a> Dec<'a> {
    fn new_opnd(&mut self) -> i32 {
        let s = unsafe { &mut *self.s };
        if s.npool >= SHADER_MAX_OPERANDS { self.bad = true; self.why = Some("too many operands"); return -1; }
        if s.npool == self.cap {
            let c = core::cmp::min(SHADER_MAX_OPERANDS, if self.cap != 0 { self.cap * 2 } else { 256 });
            let p = unsafe { krealloc(s.pool as *mut c_void, core::mem::size_of::<Opnd>() * c as usize) } as *mut Opnd;
            self.cap = c;
            if p.is_null() { self.bad = true; self.why = Some("out of memory"); return -1; }
            s.pool = p;
        }
        unsafe { s.pool.add(s.npool as usize).write(Opnd::default()) }
        s.npool += 1;
        s.npool - 1
    }
    fn next(&mut self) -> u32 {
        if self.pos >= self.end { self.bad = true; self.why = Some("truncated instruction"); return 0; }
        let v = self.t[self.pos as usize]; self.pos += 1; v
    }
    fn parse_opnd(&mut self) -> i32 {
        if self.bad || { self.depth += 1; self.depth > 16 } { self.bad = true; self.why = Some("operand nesting too deep"); return -1; }
        let tok = self.next();
        let k = self.new_opnd();
        if k < 0 { return -1; }
        let mut o = Opnd { rel: [-1; 3], ..Default::default() };
        let nc = tok & 3;
        if nc == 3 { self.bad = true; self.why = Some("N-component operands"); return -1; }
        o.ty = ((tok >> 12) & 0xFF) as u8;
        o.dim = ((tok >> 20) & 3) as u8;
        o.mask = 0xF; o.mode = 1;
        for i in 0..4 { o.swz[i] = i as u8; }
        if nc == 2 {
            o.mode = ((tok >> 2) & 3) as u8;
            if o.mode == 0 { o.mask = ((tok >> 4) & 0xF) as u8; }
            else if o.mode == 1 { for i in 0..4 { o.swz[i] = ((tok >> (4 + 2 * i)) & 3) as u8; } }
            else { let c = ((tok >> 4) & 3) as u8; o.swz = [c; 4]; o.mask = 1 << c; }
        } else if nc == 1 { o.swz = [0; 4]; o.mask = 1; }
        if tok & 0x8000_0000 != 0 {
            loop {
                let e = self.next();
                if e & 0x3F == 1 { o.md = ((e >> 6) & 0xFF) as u8; }
                else if e & 0x3F != 0 { self.bad = true; self.why = Some("extended operand form"); }
                if !(e & 0x8000_0000 != 0 && !self.bad) { break; }
            }
        }
        if o.ty == T_IMM32 {
            let n = if nc == 1 { 1 } else { 4 };
            for i in 0..n { o.imm[i] = self.next(); }
            if n == 1 { o.imm = [o.imm[0]; 4]; }
        } else if o.ty == T_IMM64 { self.bad = true; self.why = Some("double-precision values"); }
        let mut i = 0;
        while i < o.dim as usize && !self.bad {
            let rep = (tok >> (22 + 3 * i)) & 7;
            if rep == 0 { o.idx[i] = self.next() as i32; }
            else if rep == 1 { o.idx[i] = self.next() as i32; if self.next() != 0 { self.bad = true; self.why = Some("64-bit register index"); } }
            else if rep == 2 { let r = self.parse_opnd(); o.rel[i] = r as i16; }
            else if rep == 3 { o.idx[i] = self.next() as i32; let r = self.parse_opnd(); o.rel[i] = r as i16; }
            else { self.bad = true; self.why = Some("unknown index form"); }
            i += 1;
        }
        unsafe { *(*self.s).pool.add(k as usize) = o; }
        self.depth -= 1;
        k
    }
}

/// Operands an instruction takes (-1: not implemented, -2: 0 or 1), and how many are destinations.
pub(crate) fn operand_count(op: u16, destinations: &mut i32) -> i32 {
    *destinations = 1;
    match op {
        OP_MOV | OP_INEG | OP_NOT | OP_FRC | OP_EXP | OP_LOG | OP_RSQ | OP_SQRT | OP_ROUND_NE | OP_ROUND_NI | OP_ROUND_PI
        | OP_ROUND_Z | OP_ITOF | OP_UTOF | OP_FTOI | OP_FTOU | OP_RCP | OP_F32TOF16 | OP_F16TOF32 | OP_COUNTBITS
        | OP_FIRSTBIT_HI | OP_FIRSTBIT_LO | OP_FIRSTBIT_SHI | OP_BFREV => 2,
        OP_ADD | OP_MUL | OP_DIV | OP_DP2 | OP_DP3 | OP_DP4 | OP_MIN | OP_MAX | OP_EQ | OP_NE | OP_LT | OP_GE | OP_IEQ | OP_INE
        | OP_ILT | OP_IGE | OP_ULT | OP_UGE | OP_AND | OP_OR | OP_XOR | OP_IADD | OP_ISHL | OP_ISHR | OP_USHR | OP_IMIN
        | OP_IMAX | OP_UMIN | OP_UMAX | OP_LD | OP_RESINFO | OP_PHI => 3,
        OP_MOVC | OP_MAD | OP_IMAD | OP_UMAD | OP_UBFE | OP_IBFE | OP_SAMPLE => 4,
        OP_SAMPLE_L | OP_SAMPLE_B | OP_BFI => 5,
        OP_SINCOS => { *destinations = 2; 3 }
        OP_IMUL | OP_UMUL | OP_UDIV | OP_UADDC | OP_USUBB => { *destinations = 2; 4 }
        OP_SWAPC => { *destinations = 2; 5 }
        OP_IF | OP_BREAKC | OP_CONTINUEC | OP_RETC | OP_DISCARD => { *destinations = 0; 1 }
        OP_NOP | OP_RET | OP_BREAK | OP_CONTINUE | OP_LOOP | OP_ENDLOOP | OP_ELSE | OP_ENDIF | OP_BLOCK => { *destinations = 0; 0 }
        OP_BRANCH => { *destinations = 0; -2 }
        _ => -1,
    }
}

/* ---------------- checks shared by both formats ---------------- */

/// Relative-index children are reads too; their pool IDs strictly increase, so this ends.
fn mark_read(s: &mut Shader, k: usize, lanes: u32) {
    let o = s.pool()[k];
    if o.ty == T_INPUT {
        if o.direct == 0 { s.read_mask = [15; DX_MAX_REG]; }
        else { for i in 0..4 { if lanes & (1 << i) != 0 { s.read_mask[o.idx[0] as usize] |= 1 << o.swz[i]; } } }
    }
    for i in 0..o.dim as usize { if o.rel[i] >= 0 { mark_read(s, o.rel[i] as usize, 1); } }
}
fn immediate_zero(o: &Opnd) -> bool { o.ty == T_IMM32 && o.md == 0 && (o.imm[(o.swz[0] & 3) as usize] & 0x7FFF_FFFF) == 0 }
fn supported_semantic(s: &Shader, v: &DxSig, output: bool) -> bool {
    if s.ty == 1 {
        return if output { v.sysval == 0 || (v.sysval == 1 && v.mask == 15 && v.component_type == 3) }
               else { v.sysval == 0 || v.sysval == 6 || v.sysval == 8 };
    }
    if output { (v.sysval == 64 && v.index == 0) || (v.sysval == 65 && v.component_type == 3) }
    else { v.sysval == 0 || v.sysval == 1 || v.sysval == 7 || v.sysval == 9 }
}

/// Frontends validate their own encodings and control flow; this checks the executable
/// instruction list and works out register use for both formats.
#[no_mangle]
pub unsafe extern "C" fn shader_finish(s: *mut Shader, err: *mut c_char, errlen: c_int) -> CBool {
    finish(&mut *s, err, errlen) as CBool
}
pub(crate) fn finish(s: &mut Shader, err: *mut c_char, errlen: c_int) -> bool {
    macro_rules! bad { ($($a:tt)*) => {{ seterr!(err, errlen, $($a)*); return false; }} }
    if s.ntemps < 0 || s.ntemps > SHADER_MAX_TEMPS || s.xt_total < 0 || s.xt_total > SHADER_MAX_TEMPS { bad!("too many temporaries"); }
    if s.nins == 0 || s.nins > SHADER_MAX_INS || s.npool > SHADER_MAX_OPERANDS { bad!("invalid instruction/operand count"); }
    for i in 0..s.nin.clamp(0, DX_MAX_SIG as i32) as usize { if !supported_semantic(s, &s.inp[i], false) { bad!("input system semantic {} is not implemented", s.inp[i].sysval); } }
    for i in 0..s.nout.clamp(0, DX_MAX_SIG as i32) as usize { if !supported_semantic(s, &s.out[i], true) { bad!("output system semantic {} is not implemented", s.out[i].sysval); } }
    let npool = s.npool.max(0) as usize;
    let (ntemps, nicb, ty) = (s.ntemps, s.nicb, s.ty);
    for i in 0..npool {
        let mut o = s.pool()[i];
        if o.md > 3 || o.mode > 2 || o.dim > 2 { bad!("unsupported operand form"); }
        for k in 0..o.dim as usize {
            if o.rel[k] >= 0 && ((o.rel[k] as usize) <= i || o.rel[k] as i32 >= s.npool || s.pool()[o.rel[k] as usize].md != 0) { bad!("invalid relative index"); }
        }
        let relative = o.rel[0] >= 0 || o.rel[1] >= 0;
        match o.ty {
            T_TEMP | T_INPUT | T_OUTPUT | T_ICB => {
                if o.dim != 1 { bad!("invalid register dimensions"); }
                if !relative {
                    let n = if o.ty == T_TEMP { ntemps } else if o.ty == T_ICB { nicb } else { DX_MAX_REG as i32 - 1 };
                    if o.idx[0] < 0 || o.idx[0] >= n { bad!("register {} out of range", o.idx[0]); }
                    o.direct = 1;
                }
            }
            T_IMM32 => { if o.dim != 0 { bad!("indexed immediate"); } o.direct = 1; }
            T_CB => {
                if o.dim != 2 || o.rel[0] >= 0 || o.idx[0] < 0 || o.idx[0] >= 14 { bad!("constant buffer out of range"); }
                if !relative { o.direct = 1; }
            }
            T_XTEMP => {
                if o.dim != 2 || o.rel[0] >= 0 { bad!("indexable temporary form"); }
                let nxt = s.nxt.clamp(0, MAX_XTEMP as i32) as usize;
                if !s.xt[..nxt].iter().any(|x| x.reg == o.idx[0]) { bad!("undeclared indexable temporary"); }
            }
            T_SAMPLER | T_RESOURCE => {
                if o.dim != 1 || relative || o.idx[0] < 0 || o.idx[0] >= 16 { bad!("texture/sampler binding out of range"); }
            }
            T_NULL | T_ODEPTH | T_PRIMID => {
                if o.dim != 0 { bad!("invalid scalar register dimensions"); }
                if o.ty == T_ODEPTH { if ty != 0 || o.mask != 1 { bad!("invalid depth output"); } o.direct = 1; }
            }
            _ => bad!("operand type {} is not supported", o.ty),
        }
        o.identity = (o.swz == [0, 1, 2, 3]) as u8;
        s.pool_mut()[i] = o;
    }
    let mut returned = false;
    let mut control_depth = 0;
    let mut definite = [0u8; DX_MAX_REG];
    let mut read_outputs = 0u32;
    for i in 0..s.nins as usize {
        let mut ins = s.ins()[i];
        let mut nd = 0;
        let n = operand_count(ins.op, &mut nd);
        if n == -1 { bad!("instruction {} is not supported", ins.op); }
        if (n >= 0 && ins.nops as i32 != n) || (n == -2 && ins.nops > 1) { bad!("wrong operand count for instruction {}", ins.op); }
        ins.mask = 0;
        for j in 0..ins.nops as usize {
            if ins.o[j] < 0 || ins.o[j] as i32 >= s.npool { s.ins_mut()[i].mask = ins.mask; bad!("instruction operand out of range"); }
            let o = s.pool()[ins.o[j] as usize];
            if (j as i32) < nd {
                if o.ty != T_NULL && o.ty != T_TEMP && o.ty != T_OUTPUT && o.ty != T_XTEMP && o.ty != T_ODEPTH { s.ins_mut()[i].mask = ins.mask; bad!("invalid destination register"); }
                if o.md != 0 { s.ins_mut()[i].mask = ins.mask; bad!("modified destination register"); }
                if o.ty != T_NULL { ins.mask |= o.mask; }
                if o.ty == T_OUTPUT {
                    if o.direct != 0 { s.write_mask[o.idx[0] as usize] |= o.mask; if !returned && control_depth == 0 { definite[o.idx[0] as usize] |= o.mask; } }
                    else { s.write_mask = [15; DX_MAX_REG]; }
                }
                if o.ty == T_ODEPTH { if s.ty != 0 { s.ins_mut()[i].mask = ins.mask; bad!("depth output outside pixel shader"); } s.writes_depth = 1; s.write_mask[DX_MAX_REG - 1] |= 1; }
            } else {
                let texture = matches!(ins.op, OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B | OP_LD | OP_RESINFO) && j == 2;
                let sampler = matches!(ins.op, OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B) && j == 3;
                if o.ty == T_NULL || o.ty == T_ODEPTH || (o.ty == T_RESOURCE && !texture) || (o.ty == T_SAMPLER && !sampler) { s.ins_mut()[i].mask = ins.mask; bad!("invalid source register"); }
                if o.ty == T_OUTPUT { read_outputs |= if o.direct != 0 { 1u32 << o.idx[0] } else { !0 }; }
            }
        }
        if nd == 0 { ins.mask = 15; }
        s.ins_mut()[i].mask = ins.mask;
        if ins.op == OP_DISCARD && s.ty != 0 { bad!("discard outside pixel shader"); }
        for j in 0..ins.nops as usize {
            let k = ins.o[j] as usize;
            let o = s.pool()[k];
            if (j as i32) < nd { for kk in 0..o.dim as usize { if o.rel[kk] >= 0 { mark_read(s, o.rel[kk] as usize, 1); } } continue; }
            let mut lanes = ins.mask as u32;
            if ins.op == OP_DP2 { lanes = 3; }
            else if ins.op == OP_DP3 { lanes = 7; }
            else if ins.op == OP_DP4 { lanes = 15; }
            else if nd == 0 || (ins.op == OP_SAMPLE_L && j == 4) || (ins.op == OP_SAMPLE_B && j == 4) || ins.op == OP_RESINFO { lanes = 1; }
            else if matches!(ins.op, OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B | OP_LD) { lanes = 3; }
            mark_read(s, k, lanes);
        }
        let pool = s.pool();
        if matches!(ins.op, OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B) {
            if pool[ins.o[2] as usize].ty != T_RESOURCE || pool[ins.o[3] as usize].ty != T_SAMPLER { bad!("invalid sampling operands"); }
            if s.major >= 5 && ins.op != OP_SAMPLE && !immediate_zero(&pool[ins.o[4] as usize]) { bad!("the SM5/6 subset supports only constant LOD/bias zero"); }
        } else if ins.op == OP_LD || ins.op == OP_RESINFO {
            if pool[ins.o[2] as usize].ty != T_RESOURCE { bad!("invalid texture operand"); }
            let level = &pool[ins.o[1] as usize];
            if s.major >= 5 && (ins.op == OP_LD || level.ty != T_IMM32 || level.md != 0 || level.imm[(level.swz[0] & 3) as usize] != 0) { bad!("modern texel loads/nonzero LOD queries are not implemented"); }
        }
        if ins.op == OP_IF || ins.op == OP_LOOP { control_depth += 1; }
        else if ins.op == OP_ENDIF || ins.op == OP_ENDLOOP { control_depth -= 1; }
        if ins.op == OP_RET || ins.op == OP_RETC { returned = true; }
    }
    /* DXIL supplies a CFG intersection over all return blocks. DXBC can still omit clears
     * for unconditional writes after an if/else, unless a return may have bypassed them. */
    if s.dxil == 0 { s.definite_mask = definite; }
    s.nclear = 0;
    for i in 0..DX_MAX_REG {
        if s.write_mask[i] != 0 && (s.definite_mask[i] != 15 || read_outputs & (1u32 << i) != 0) { s.clear_regs[s.nclear as usize] = i as u8; s.nclear += 1; }
    }
    true
}

/* ---------------- the container and the SM4/5 program ---------------- */

unsafe fn new_shader() -> *mut Shader {
    let s = kzalloc(core::mem::size_of::<Shader>()) as *mut Shader;
    if !s.is_null() { (*s).interpolation = [2; DX_MAX_REG]; }
    s
}

#[no_mangle]
pub unsafe extern "C" fn dxbc_load(code: *const c_void, len: usize, err: *mut c_char, errlen: c_int) -> *mut Shader {
    if code.is_null() { seterr!(err, errlen, "not DXBC bytecode"); return ptr::null_mut(); }
    load(core::slice::from_raw_parts(code as *const u8, len), err, errlen)
}

pub(crate) unsafe fn load(b: &[u8], err: *mut c_char, errlen: c_int) -> *mut Shader {
    let mut s: *mut Shader = ptr::null_mut();
    let mut owned: *mut u32 = ptr::null_mut();
    macro_rules! fail { ($($a:tt)*) => {{ seterr!(err, errlen, $($a)*); kfree(owned as *mut c_void); dxbc_free(s); return ptr::null_mut(); }} }
    if b.len() < 32 || &b[..4] != b"DXBC" { fail!("not DXBC bytecode"); }
    let (total, nch) = (word(b, 24), word(b, 28));
    if word(b, 20) != 1 || total < 32 || total as usize > b.len() || total > 16 * 1024 * 1024 || nch > (total - 32) / 4 || nch > 256 { fail!("bad container"); }
    s = new_shader();
    if s.is_null() { fail!("out of memory"); }
    let sh = &mut *s;
    let mut program: Option<&[u8]> = None;
    let mut dxil: Option<&[u8]> = None;
    let (mut input_sig, mut output_sig) = (false, false);
    for i in 0..nch {
        let off = word(b, (32 + i * 4) as usize);
        if off & 3 != 0 || off < 32 + nch * 4 || off > total || total - off < 8 { fail!("bad chunk"); }
        let cc = &b[off as usize..off as usize + 4];
        let sz = word(b, off as usize + 4);
        if sz > total - off - 8 { fail!("bad chunk size"); }
        let data = &b[off as usize + 8..(off + 8 + sz) as usize];
        if cc == b"ISGN" || cc == b"ISG1" {
            if input_sig { fail!("duplicate input signature"); } input_sig = true;
            sh.nin = parse_sig(data, &mut sh.inp, if cc[3] == b'1' { 32 } else { 24 });
        } else if cc == b"OSGN" || cc == b"OSG1" || cc == b"OSG5" {
            if output_sig { fail!("duplicate output signature"); } output_sig = true;
            sh.nout = parse_sig(data, &mut sh.out, if cc[3] == b'1' { 32 } else if cc[3] == b'5' { 28 } else { 24 });
        } else if cc == b"SHDR" || cc == b"SHEX" {
            if program.is_some() || sz & 3 != 0 || sz > 4 * 1024 * 1024 { fail!("invalid shader program chunk"); }
            program = Some(data);
        } else if cc == b"DXIL" {
            if dxil.is_some() { fail!("duplicate DXIL program"); }
            dxil = Some(data);
        }
        if sh.nin < 0 || sh.nout < 0 { fail!("invalid or unsupported shader signature"); }
    }
    if let Some(d) = dxil {
        if program.is_some() { fail!("mixed DXBC and DXIL programs"); }
        if !crate::dxil::load(d, sh, err, errlen) || !finish(sh, err, errlen) { dxbc_free(s); return ptr::null_mut(); }
        return s;
    }
    let program = match program { Some(p) if p.len() / 4 >= 2 => p, _ => fail!("no shader program") };
    let plen = (program.len() / 4) as u32;
    /* a copy as words: accepts unaligned caller byte arrays */
    owned = kmalloc(plen as usize * 4) as *mut u32;
    if owned.is_null() { fail!("out of memory"); }
    for (k, c) in program.chunks_exact(4).take(plen as usize).enumerate() { *owned.add(k) = u32::from_le_bytes([c[0], c[1], c[2], c[3]]); }
    let prog = core::slice::from_raw_parts(owned as *const u32, plen as usize);
    sh.ty = ((prog[0] >> 16) & 0xFFFF) as i32;
    sh.major = (prog[0] >> 4 & 15) as i32; sh.minor = (prog[0] & 15) as i32;
    if !((sh.major == 4 && sh.minor <= 1) || (sh.major == 5 && sh.minor == 0)) { fail!("shader model {}.{} is not a supported DXBC profile", sh.major, sh.minor); }
    if sh.ty > 1 { fail!("only vertex and pixel shader stages are implemented"); }
    let end = prog[1];
    if end < 2 || end > plen { fail!("truncated shader program"); }
    let mut d = Dec { t: prog, pos: 2, end, s, cap: 0, depth: 0, bad: false, why: None };
    let mut cap = 0i32;
    while d.pos < end {
        let at = d.pos;
        let tok = prog[at as usize];
        let op = (tok & 0x7FF) as u16;
        let mut ilen = (tok >> 24) & 0x7F;
        if op == OP_CUSTOMDATA {
            ilen = if at + 1 < end { prog[at as usize + 1] } else { 0 };
            if ilen < 2 || ilen > end - at { fail!("bad custom data"); }
            if (tok >> 11) & 0x1FFFFF == 3 {          /* immediate constant buffer */
                let n = ((ilen - 2) / 4) as i32;
                if (ilen - 2) % 4 != 0 || n > 4096 { fail!("bad immediate constant buffer"); }
                kfree(sh.icb as *mut c_void);
                sh.icb = kmalloc(core::mem::size_of::<Vec4>() * n.max(1) as usize) as *mut Vec4; sh.nicb = n;
                if sh.icb.is_null() { fail!("out of memory"); }
                for k in 0..n as usize {
                    let w = &prog[at as usize + 2 + k * 4..at as usize + 6 + k * 4];
                    *sh.icb.add(k) = Vec4 { u: [w[0], w[1], w[2], w[3]] };
                }
            }
            d.pos = at + ilen; continue;
        }
        if ilen == 0 || ilen > end - at { fail!("bad instruction length at {}", at); }
        if (OP_DCL_FIRST..=OP_DCL_LAST).contains(&op) {
            let p = |k: u32| prog[(at + k) as usize];
            if op == OP_DCL_TEMPS {
                if ilen != 2 || p(1) > SHADER_MAX_TEMPS as u32 { fail!("invalid temporary declaration"); }
                sh.ntemps = p(1) as i32;
            } else if op == OP_DCL_INDEXABLE_TEMP {
                if ilen != 4 || sh.nxt == MAX_XTEMP as i32 || p(2) == 0 || p(2) > (SHADER_MAX_TEMPS - sh.xt_total) as u32 || p(3) == 0 || p(3) > 4 { fail!("invalid indexable temporary declaration"); }
                for k in 0..sh.nxt as usize { if sh.xt[k].reg as u32 == p(1) { fail!("duplicate indexable temporary"); } }
                sh.xt[sh.nxt as usize] = Xt { reg: p(1) as i32, size: p(2) as i32 };
                sh.xt_total += p(2) as i32; sh.nxt += 1;
            } else if op == OP_DCL_LAST {
                if ilen != 1 || (tok & 0x00FF_F800 & !(0x800u32 | 0x8000 | 0x10000)) != 0 { fail!("unsupported global shader requirements"); }
            } else {
                if (92..=94).contains(&op) { fail!("geometry shader declaration"); }
                d.pos = at + 1; d.end = at + ilen; d.depth = 0;
                if tok & 0x8000_0000 != 0 { fail!("extended declaration is not supported"); }
                let oi = d.parse_opnd();
                if d.bad || oi < 0 { fail!("bad declaration operand"); }
                let o = *sh.pool.add(oi as usize);
                if op == 88 {
                    if (tok >> 11) & 31 != 3 || o.ty != T_RESOURCE || d.pos + 1 != d.end { fail!("only Texture2D resources are implemented"); }
                    let ret = prog[d.pos as usize];
                    for k in 0..4 {
                        let t = (ret >> (k * 4)) & 15;
                        if t != 1 && t != 2 && t != 5 { fail!("only floating/normalized Texture2D resources are implemented"); }
                    }
                } else if op == 89 {
                    if o.ty != T_CB || o.dim != 2 || o.idx[1] < 0 || o.idx[1] > 4096 || d.pos != d.end { fail!("bad constant buffer declaration"); }
                } else if op == 90 {
                    if o.ty != T_SAMPLER || (tok >> 11) & 15 != 0 || d.pos != d.end { fail!("comparison samplers are not implemented"); }
                } else if op == 91 {
                    if d.pos + 1 != d.end || prog[d.pos as usize] == 0 { fail!("bad index range declaration"); }
                } else {
                    let input = (95..=100).contains(&op);
                    let system = matches!(op, 96 | 97 | 99 | 100 | 102 | 103);
                    if (input && o.ty != T_INPUT && o.ty != T_PRIMID) || (!input && o.ty != T_OUTPUT && o.ty != T_ODEPTH) || d.pos + system as u32 != d.end { fail!("bad input/output declaration"); }
                    if (98..=100).contains(&op) && o.idx[0] >= 0 && o.idx[0] < DX_MAX_REG as i32 {
                        let mode = (tok >> 11) & 15;
                        if !(1..=5).contains(&mode) { fail!("sample-frequency interpolation is not implemented"); }
                        let m = if mode == 1 { 1 } else if mode >= 4 { 4 } else { 2 };
                        let r = o.idx[0] as usize;
                        if sh.interpolation_declared & (1 << r) != 0 && sh.interpolation[r] != m { fail!("mixed interpolation within a register is not implemented"); }
                        sh.interpolation_declared |= 1 << r; sh.interpolation[r] = m;
                    }
                }
            }
            d.end = end;
            d.pos = at + ilen; continue;
        }
        let mut destinations = 0;
        if op >= OP_BLOCK || operand_count(op, &mut destinations) < 0 || (sh.major == 4 && op > OP_XOR) { fail!("instruction {} is not implemented for this profile", op); }
        if sh.nins == SHADER_MAX_INS { fail!("too many instructions"); }
        if !grow(&mut sh.ins, sh.nins, &mut cap, 64, SHADER_MAX_INS) { fail!("out of memory"); }
        let mut ins = Ins { op, sat: ((tok >> 13) & 1) as u8, nonzero: ((tok >> 18) & 1) as u8, ret_type: ((tok >> 11) & 3) as u8, jump: -1, alt: -1, ..Default::default() };
        d.pos = at + 1; d.end = at + ilen;
        if tok & 0x8000_0000 != 0 {
            loop {
                let e = d.next();
                /* resource dimension/type hints are redundant; sample offsets are not */
                if e & 63 == 1 && e & 0x001F_FE00 != 0 { d.bad = true; d.why = Some("sample texel offsets are not implemented"); }
                else if e & 63 > 3 { d.bad = true; d.why = Some("extended opcode form"); }
                if !(e & 0x8000_0000 != 0 && !d.bad) { break; }
            }
        }
        while d.pos < d.end && !d.bad && ins.nops < 5 { ins.o[ins.nops as usize] = d.parse_opnd() as i16; ins.nops += 1; }
        *sh.ins.add(sh.nins as usize) = ins;
        if d.bad || d.pos != d.end { fail!("{}", d.why.unwrap_or("bad operand count")); }
        d.end = end; d.pos = at + ilen;
        sh.nins += 1;
    }
    /* jump targets */
    let mut stack = [0usize; 64];
    let mut sp = 0usize;
    let n = sh.nins as usize;
    {
        let ins = sh.ins_mut();
        for i in 0..n {
            match ins[i].op {
                OP_IF => { if sp == 64 { fail!("nesting too deep"); } stack[sp] = i; sp += 1; }
                OP_ELSE => {
                    if sp == 0 || ins[stack[sp - 1]].op != OP_IF { fail!("else without if"); }
                    ins[stack[sp - 1]].jump = i as i32 + 1; stack[sp - 1] = i;
                }
                OP_ENDIF => {
                    if sp == 0 || (ins[stack[sp - 1]].op != OP_IF && ins[stack[sp - 1]].op != OP_ELSE) { fail!("endif without matching if"); }
                    sp -= 1; ins[stack[sp]].jump = i as i32 + 1;
                }
                OP_LOOP => { if sp == 64 { fail!("nesting too deep"); } stack[sp] = i; sp += 1; }
                OP_ENDLOOP => {
                    if sp == 0 || ins[stack[sp - 1]].op != OP_LOOP { fail!("endloop without matching loop"); }
                    sp -= 1;
                    ins[i].jump = stack[sp] as i32 + 1;
                    for j in ins[i].jump as usize..i {          /* break/continue of this loop */
                        let b = &mut ins[j];
                        if (b.op == OP_BREAK || b.op == OP_BREAKC) && b.jump < 0 { b.jump = i as i32 + 1; }
                        if (b.op == OP_CONTINUE || b.op == OP_CONTINUEC) && b.jump < 0 { b.jump = i as i32; }
                    }
                    ins[stack[sp]].jump = i as i32 + 1;
                }
                _ => {}
            }
        }
    }
    if sp != 0 { fail!("unbalanced control flow"); }
    for i in sh.ins() {
        if matches!(i.op, OP_BREAK | OP_BREAKC | OP_CONTINUE | OP_CONTINUEC) && i.jump < 0 { fail!("break/continue outside loop"); }
    }
    kfree(owned as *mut c_void);
    if !finish(sh, err, errlen) { dxbc_free(s); return ptr::null_mut(); }
    s
}

/* ---------------- the interpreter ---------------- */

static ZERO4: Vec4 = Vec4 { u: [0; 4] };

struct Run<'a> { s: &'a Shader, pool: &'a [Opnd], e: *mut Exec, env: &'a Env }

impl<'a> Run<'a> {
    #[inline(always)] fn op(&self, k: usize) -> &'a Opnd { unsafe { self.pool.get_unchecked(k) } }
    /// The register an operand addresses; a dynamic index out of range reads zero (scratch).
    unsafe fn reg_ptr(&self, o: &Opnd, write: bool) -> *mut Vec4 {
        let e = &mut *self.e;
        let i0 = if o.dim > 0 { self.index_of(o, 0) } else { 0 };
        match o.ty {
            T_TEMP => if i0 >= 0 && i0 < self.s.ntemps.max(1) { return e.r.add(i0 as usize); },
            T_INPUT => if i0 >= 0 && i0 < DX_MAX_REG as i32 { return e.inp.add(i0 as usize) as *mut Vec4; },
            T_OUTPUT => if i0 >= 0 && i0 < DX_MAX_REG as i32 { return e.out.add(i0 as usize); },
            T_ODEPTH => return e.out.add(DX_MAX_REG - 1),
            T_XTEMP => {
                let mut base = 0;
                for k in 0..self.s.nxt as usize {
                    let xt = self.s.xt[k];
                    if xt.reg == i0 {
                        let i1 = self.index_of(o, 1);
                        if i1 >= 0 && i1 < xt.size { return e.x.add((base + i1) as usize); }
                        break;
                    }
                    base += xt.size;
                }
            }
            T_CB => if !write {
                let i1 = self.index_of(o, 1);
                if i0 >= 0 && i0 < 14 && !self.env.cb[i0 as usize].is_null() && i1 >= 0 && (i1 as u32) < self.env.cb_rows[i0 as usize] {
                    return self.env.cb[i0 as usize].add(i1 as usize) as *mut Vec4;
                }
            },
            T_ICB => if !write && i0 >= 0 && i0 < self.s.nicb { return self.s.icb.add(i0 as usize); },
            T_PRIMID => if !write { e.scratch.u[0] = self.env.primitive_id; return &mut e.scratch; },
            _ => {}
        }
        e.scratch = Vec4::default();
        &mut e.scratch
    }
    #[inline(always)]
    unsafe fn fetch_raw(&self, k: usize) -> Vec4 {
        let o = self.op(k);
        let p: *const u32 = if o.direct != 0 { *(*self.e).bound.add(k) } else { self.reg_ptr(o, false) as *const u32 };
        if o.identity != 0 { return Vec4 { u: [*p, *p.add(1), *p.add(2), *p.add(3)] }; }
        Vec4 { u: [*p.add(o.swz[0] as usize), *p.add(o.swz[1] as usize), *p.add(o.swz[2] as usize), *p.add(o.swz[3] as usize)] }
    }
    unsafe fn index_of(&self, o: &Opnd, k: usize) -> i32 {
        let mut v = o.idx[k];
        if o.rel[k] >= 0 { let r = self.fetch_raw(o.rel[k] as usize); v = (v as u32).wrapping_add(r.u[0]) as i32; }
        v
    }
    #[inline(always)]
    unsafe fn src_f(&self, k: i16) -> Vec4 {
        let o = self.op(k as usize);
        let mut v = self.fetch_raw(k as usize);
        if o.md == 0 { return v; }
        if o.md & 2 != 0 { for x in v.u.iter_mut() { *x &= 0x7FFF_FFFF; } }
        if o.md & 1 != 0 { for x in v.u.iter_mut() { *x ^= 0x8000_0000; } }
        v
    }
    #[inline(always)]
    unsafe fn src_i(&self, k: i16) -> Vec4 {
        let o = self.op(k as usize);
        let mut v = self.fetch_raw(k as usize);
        if o.md == 0 { return v; }
        if o.md & 2 != 0 { for x in v.u.iter_mut() { if (*x as i32) < 0 { *x = 0u32.wrapping_sub(*x); } } }
        if o.md & 1 != 0 { for x in v.u.iter_mut() { *x = 0u32.wrapping_sub(*x); } }
        v
    }
    #[inline(always)]
    unsafe fn store(&self, k: i16, mut v: Vec4, sat: bool) {
        let o = self.op(k as usize);
        if o.ty == T_NULL { return; }
        let e = &mut *self.e;
        let d = if o.direct != 0 && o.ty == T_TEMP { e.r.add(o.idx[0] as usize) }
                else if o.direct != 0 && o.ty == T_OUTPUT { e.out.add(o.idx[0] as usize) }
                else { self.reg_ptr(o, true) };
        if o.mask == 15 && !sat { *d = v; return; }
        for i in 0..4 {
            if o.mask & (1 << i) != 0 {
                if sat { let f = v.f(i); v.setf(i, if f > 1.0 { 1.0 } else if f > 0.0 { f } else { 0.0 }); }   /* NaN becomes 0 */
                (*d).u[i] = v.u[i];
            }
        }
    }
}

/// Float32 rounding: 0 nearest even, 1 down, 2 up, 3 toward zero.
fn round32(x: f32, mode: i32) -> f32 {
    let mut u = x.to_bits();
    let magnitude = u & 0x7FFF_FFFF;
    let sign = u >> 31;
    if magnitude >= 0x4B00_0000 || magnitude == 0 {          /* integral, infinity, NaN or signed zero */
        /* the C function returned x in an x87 register on 32-bit, which quiets a NaN */
        if cfg!(target_arch = "x86") && magnitude > 0x7F80_0000 { return f32::from_bits(u | 0x40_0000); }
        return x;
    }
    if magnitude < 0x3F80_0000 {
        u &= 0x8000_0000;
        if (mode == 1 && sign != 0) || (mode == 2 && sign == 0) || (mode == 0 && magnitude > 0x3F00_0000) { return if sign != 0 { -1.0 } else { 1.0 }; }
        return f32::from_bits(u);
    }
    let shift = 150 - ((magnitude >> 23) & 255);
    let mask = (1u32 << shift) - 1;
    let fraction = u & mask;
    let half = 1u32 << (shift - 1);
    let increment = fraction != 0 && ((mode == 1 && sign != 0) || (mode == 2 && sign == 0) ||
                    (mode == 0 && (fraction > half || (fraction == half && u & (1 << shift) != 0))));
    u &= !mask;
    if increment { f32::from_bits(u) + if sign != 0 { -1.0 } else { 1.0 } } else { f32::from_bits(u) }
}
fn fr(x: f32) -> f32 { fsub(x, round32(x, 1)) }
fn half_bits(v: u32) -> u32 {
    let sign = (v >> 16) & 0x8000;
    let mut m = v & 0x7F_FFFF;
    let e = ((v >> 23) & 255) as i32 - 127 + 15;
    if (v >> 23) & 255 == 255 { return sign | 0x7C00 | if m != 0 { (m >> 13) | 0x200 } else { 0 }; }
    if e >= 31 { return sign | 0x7C00; }
    if e <= 0 {
        if e < -10 { return sign; }
        m |= 0x80_0000;
        let shift = (14 - e) as u32;
        let (q, rest, mid) = (m >> shift, m & ((1u32 << shift) - 1), 1u32 << (shift - 1));
        return sign | (q + (rest > mid || (rest == mid && q & 1 != 0)) as u32);
    }
    sign | (((e as u32) << 10) + ((m + 0xFFF + ((m >> 13) & 1)) >> 13))
}
fn float_bits(h: u32) -> u32 {
    let sign = (h & 0x8000) << 16;
    let mut m = h & 1023;
    let e = (h >> 10) & 31;
    if e == 0 {
        if m == 0 { return sign; }
        let mut power = -14i32;
        while m & 1024 == 0 { m <<= 1; power -= 1; }
        return sign | (((power + 127) as u32) << 23) | ((m & 1023) << 13);
    }
    if e == 31 { return sign | 0x7F80_0000 | (m << 13) | if m != 0 { 0x40_0000 } else { 0 }; }
    sign | ((e + 112) << 23) | (m << 13)
}

/* NaN bits, as the C interpreter produced them. SSE (the C code on 64-bit) returns the
 * first operand's NaN, quieted, then the second's, and 0xFFC00000 for an invalid operation
 * such as 0 * inf; the C compiler kept the operands in source order. A shader can read
 * those bits back as integers, so the Rust code gives the same ones:
 * - with SSE (host builds) the instruction itself, in asm, because LLVM may swap the
 *   operands of a commutative operation;
 * - in the 64-bit kernel, whose Rust code is soft-float, the soft-float result with the
 *   SSE rule applied to a NaN;
 * - on x87 (the 32-bit edition) the hardware rule, which is what C got there too. */
#[cfg(all(target_arch = "x86_64", not(target_os = "none")))]
mod fp {
    #[inline(always)] pub fn add(a: f32, b: f32) -> f32 { let mut r = a; unsafe { core::arch::asm!("addss {r}, {b}", r = inout(xmm_reg) r, b = in(xmm_reg) b, options(pure, nomem, nostack)); } r }
    #[inline(always)] pub fn sub(a: f32, b: f32) -> f32 { let mut r = a; unsafe { core::arch::asm!("subss {r}, {b}", r = inout(xmm_reg) r, b = in(xmm_reg) b, options(pure, nomem, nostack)); } r }
    #[inline(always)] pub fn mul(a: f32, b: f32) -> f32 { let mut r = a; unsafe { core::arch::asm!("mulss {r}, {b}", r = inout(xmm_reg) r, b = in(xmm_reg) b, options(pure, nomem, nostack)); } r }
    #[inline(always)] pub fn div(a: f32, b: f32) -> f32 { let mut r = a; unsafe { core::arch::asm!("divss {r}, {b}", r = inout(xmm_reg) r, b = in(xmm_reg) b, options(pure, nomem, nostack)); } r }
}
#[cfg(all(target_arch = "x86_64", target_os = "none"))]
mod fp {
    #[inline(always)] fn nan(x: f32) -> bool { x.to_bits() & 0x7FFF_FFFF > 0x7F80_0000 }
    #[inline(always)] fn sse(a: f32, b: f32, r: f32) -> f32 {
        if !nan(r) { r }
        else if nan(a) { f32::from_bits(a.to_bits() | 0x40_0000) }
        else if nan(b) { f32::from_bits(b.to_bits() | 0x40_0000) }
        else { f32::from_bits(0xFFC0_0000) }
    }
    #[inline(always)] pub fn add(a: f32, b: f32) -> f32 { sse(a, b, a + b) }
    #[inline(always)] pub fn sub(a: f32, b: f32) -> f32 { sse(a, b, a - b) }
    #[inline(always)] pub fn mul(a: f32, b: f32) -> f32 { sse(a, b, a * b) }
    #[inline(always)] pub fn div(a: f32, b: f32) -> f32 { sse(a, b, a / b) }
}
#[cfg(not(target_arch = "x86_64"))]
mod fp {
    #[inline(always)] pub fn add(a: f32, b: f32) -> f32 { a + b }
    #[inline(always)] pub fn sub(a: f32, b: f32) -> f32 { a - b }
    #[inline(always)] pub fn mul(a: f32, b: f32) -> f32 { a * b }
    #[inline(always)] pub fn div(a: f32, b: f32) -> f32 { a / b }
}
use fp::{add as fadd, div as fdiv, mul as fmul, sub as fsub};

#[no_mangle]
pub unsafe extern "C" fn dxbc_exec_init(r: *mut Exec, s: *const Shader, env: *const Env, inp: *const Vec4, out: *mut Vec4) -> CBool {
    if r.is_null() || s.is_null() || env.is_null() || inp.is_null() || out.is_null() { return 0; }
    ptr::write_bytes(r, 0, 1);
    let sh = &*s;
    let need = sh.ntemps.max(1) as usize + sh.xt_total.max(0) as usize;
    let rb = core::mem::size_of::<Vec4>() * need;
    let bindings = core::mem::size_of::<*const u32>() * sh.npool.max(0) as usize;
    let storage = kmalloc(rb + bindings + 16);
    if storage.is_null() { return 0; }
    let e = &mut *r;
    e.storage = storage; e.shader = s; e.env = env; e.inp = inp; e.out = out;
    e.r = storage as *mut Vec4; e.x = e.r.add(sh.ntemps.max(1) as usize);
    e.bound = (storage as *mut u8).add(rb) as *mut *const u32;
    ptr::write_bytes(e.r as *mut u8, 0, rb);
    /* machine code for this shader, if it compiles (jit.rs): kept after the bindings */
    *((storage as *mut u8).add(rb + bindings) as *mut usize) = match crate::jit::get(s) { Some(f) => f as usize, None => 0 };
    let envr = &*env;
    for (i, o) in sh.pool().iter().enumerate() {
        *e.bound.add(i) = if o.direct != 0 {
            match o.ty {
                T_TEMP => (*e.r.add(o.idx[0] as usize)).u.as_ptr(),
                T_INPUT => (*inp.add(o.idx[0] as usize)).u.as_ptr(),
                T_OUTPUT => (*out.add(o.idx[0] as usize)).u.as_ptr(),
                T_IMM32 => o.imm.as_ptr(),
                T_CB => {
                    let c = envr.cb[o.idx[0] as usize];
                    if !c.is_null() && o.idx[1] >= 0 && (o.idx[1] as u32) < envr.cb_rows[o.idx[0] as usize] { (*c.add(o.idx[1] as usize)).u.as_ptr() } else { ZERO4.u.as_ptr() }
                }
                T_ICB => (*sh.icb.add(o.idx[0] as usize)).u.as_ptr(),
                T_ODEPTH => (*out.add(DX_MAX_REG - 1)).u.as_ptr(),
                _ => ZERO4.u.as_ptr(),
            }
        } else { ptr::null() };
    }
    1
}

#[no_mangle]
pub unsafe extern "C" fn dxbc_exec_fini(r: *mut Exec) {
    if !r.is_null() { kfree((*r).storage); ptr::write_bytes(r, 0, 1); }
}

/// One instruction of the interpreter; the next pc. (The JIT calls it, through
/// `dxbc_exec_step`, for the instructions it does not compile itself.)
#[inline(always)]
unsafe fn exec_ins(run: &Run, exec: *mut Exec, ins: &Ins, pc: i32, nins: i32, env: &Env) -> i32 {
    let mut v = Vec4::default();
    let mut sat = ins.sat != 0;
    let mut next_pc = pc + 1;
    let mask = ins.mask as u32;
    let o = &ins.o;
    macro_rules! each { (|$i:ident| $body:expr) => {
        if mask == 1 { let $i = 0usize; $body; }
        else if mask == 15 { for $i in 0..4usize { $body; } }
        else { for $i in 0..4usize { if mask & (1 << $i) != 0 { $body; } } }
    } }
    macro_rules! out { () => { run.store(o[0], v, sat) } }
    macro_rules! cond { ($k:expr) => { (run.src_i(o[$k]).u[0] != 0) as u8 } }
    macro_rules! fbin { (|$a:ident, $b:ident| $e:expr) => {{ let $a = run.src_f(o[1]); let $b = run.src_f(o[2]); each!(|i| v.setf(i, { let ($a, $b) = ($a.f(i), $b.f(i)); $e })); out!(); }} }
    macro_rules! fun { (|$a:ident| $e:expr) => {{ let $a = run.src_f(o[1]); each!(|i| v.setf(i, { let $a = $a.f(i); $e })); out!(); }} }
    macro_rules! fcmp { (|$a:ident, $b:ident| $e:expr) => {{ let $a = run.src_f(o[1]); let $b = run.src_f(o[2]); each!(|i| v.u[i] = if { let ($a, $b) = ($a.f(i), $b.f(i)); $e } { !0 } else { 0 }); sat = false; out!(); }} }
    macro_rules! ibin { (|$a:ident, $b:ident| $e:expr) => {{ let $a = run.src_i(o[1]); let $b = run.src_i(o[2]); each!(|i| v.u[i] = { let ($a, $b) = ($a.u[i], $b.u[i]); $e }); out!(); }} }
    macro_rules! iun { (|$a:ident| $e:expr) => {{ let $a = run.src_i(o[1]); each!(|i| v.u[i] = { let $a = $a.u[i]; $e }); out!(); }} }
    match ins.op {
        OP_MOV => { v = run.src_f(o[1]); out!(); }
        OP_MOVC => { let c = run.src_i(o[1]); let _a = run.src_f(o[1]); let b = run.src_f(o[2]); let cc = run.src_f(o[3]); each!(|i| v.u[i] = if c.u[i] != 0 { b.u[i] } else { cc.u[i] }); out!(); }
        OP_ADD => fbin!(|a, b| fadd(a, b)),
        OP_MUL => fbin!(|a, b| fmul(a, b)),
        OP_DIV => fbin!(|a, b| fdiv(a, b)),
        OP_MAD => { let a = run.src_f(o[1]); let b = run.src_f(o[2]); let c = run.src_f(o[3]); each!(|i| v.setf(i, fadd(fmul(a.f(i), b.f(i)), c.f(i)))); out!(); }
        OP_MIN => fbin!(|a, b| if b != b || a < b { a } else { b }),
        OP_MAX => fbin!(|a, b| if b != b || a > b { a } else { b }),
        OP_DP2 => { let a = run.src_f(o[1]); let b = run.src_f(o[2]); let d = fadd(fmul(a.f(0), b.f(0)), fmul(a.f(1), b.f(1))); each!(|i| v.setf(i, d)); out!(); }
        OP_DP3 => { let a = run.src_f(o[1]); let b = run.src_f(o[2]); let d = fadd(fadd(fmul(a.f(0), b.f(0)), fmul(a.f(1), b.f(1))), fmul(a.f(2), b.f(2))); each!(|i| v.setf(i, d)); out!(); }
        OP_DP4 => { let a = run.src_f(o[1]); let b = run.src_f(o[2]); let d = fadd(fadd(fadd(fmul(a.f(0), b.f(0)), fmul(a.f(1), b.f(1))), fmul(a.f(2), b.f(2))), fmul(a.f(3), b.f(3))); each!(|i| v.setf(i, d)); out!(); }
        OP_RSQ => fun!(|a| fdiv(1.0, libm(0, a))),
        OP_SQRT => fun!(|a| libm(0, a)),
        OP_EXP => fun!(|a| libm(3, a)),
        OP_LOG => fun!(|a| libm(4, a)),
        OP_FRC => fun!(|a| fr(a)),
        OP_ROUND_NE => fun!(|a| round32(a, 0)),
        OP_ROUND_NI => fun!(|a| round32(a, 1)),
        OP_ROUND_PI => fun!(|a| round32(a, 2)),
        OP_ROUND_Z => fun!(|a| round32(a, 3)),
        OP_SINCOS => {
            let a = run.src_f(o[2]);
            let d0 = run.op(o[0] as usize);
            if d0.ty != T_NULL {
                for i in 0..4 { if d0.mask & (1 << i) != 0 { v.setf(i, libm(1, a.f(i))); } }
                run.store(o[0], v, sat);
            }
            let d1 = run.op(o[1] as usize);
            if d1.ty != T_NULL {
                for i in 0..4 { if d1.mask & (1 << i) != 0 { v.setf(i, libm(2, a.f(i))); } }
                run.store(o[1], v, sat);
            }
        }
        OP_DERIV_RTX | OP_DERIV_RTY => { out!(); }
        OP_EQ => fcmp!(|a, b| a == b),
        OP_NE => fcmp!(|a, b| a != b),
        OP_LT => fcmp!(|a, b| a < b),
        OP_GE => fcmp!(|a, b| a >= b),
        OP_IEQ => ibin!(|a, b| if a as i32 == b as i32 { !0 } else { 0 }),
        OP_INE => ibin!(|a, b| if a as i32 != b as i32 { !0 } else { 0 }),
        OP_ILT => ibin!(|a, b| if (a as i32) < b as i32 { !0 } else { 0 }),
        OP_IGE => ibin!(|a, b| if a as i32 >= b as i32 { !0 } else { 0 }),
        OP_ULT => ibin!(|a, b| if a < b { !0 } else { 0 }),
        OP_UGE => ibin!(|a, b| if a >= b { !0 } else { 0 }),
        OP_AND => ibin!(|a, b| a & b),
        OP_OR => ibin!(|a, b| a | b),
        OP_XOR => ibin!(|a, b| a ^ b),
        OP_NOT => iun!(|a| !a),
        OP_IADD => ibin!(|a, b| a.wrapping_add(b)),
        OP_INEG => iun!(|a| 0u32.wrapping_sub(a)),
        OP_IMAD | OP_UMAD => { let a = run.src_i(o[1]); let b = run.src_i(o[2]); let c = run.src_i(o[3]); each!(|i| v.u[i] = a.u[i].wrapping_mul(b.u[i]).wrapping_add(c.u[i])); out!(); }
        OP_IMAX => ibin!(|a, b| if a as i32 > b as i32 { a } else { b }),
        OP_IMIN => ibin!(|a, b| if (a as i32) < b as i32 { a } else { b }),
        OP_UMAX => ibin!(|a, b| if a > b { a } else { b }),
        OP_UMIN => ibin!(|a, b| if a < b { a } else { b }),
        OP_ISHL => ibin!(|a, b| a << (b & 31)),
        OP_ISHR => ibin!(|a, b| ((a as i32) >> (b & 31)) as u32),
        OP_USHR => ibin!(|a, b| a >> (b & 31)),
        OP_IMUL | OP_UMUL => {
            let a = run.src_i(o[2]); let b = run.src_i(o[3]);
            let (mut hi, mut lo) = (Vec4::default(), Vec4::default());
            each!(|i| if ins.op == OP_IMUL { let p = a.i(i) as i64 * b.i(i) as i64; hi.u[i] = ((p as u64) >> 32) as u32; lo.u[i] = p as u32; }
                      else { let p = a.u[i] as u64 * b.u[i] as u64; hi.u[i] = (p >> 32) as u32; lo.u[i] = p as u32; });
            run.store(o[0], hi, false); run.store(o[1], lo, false);
        }
        OP_UDIV => {
            let a = run.src_i(o[2]); let b = run.src_i(o[3]);
            let (mut q, mut m) = (Vec4::default(), Vec4::default());
            each!(|i| { q.u[i] = if b.u[i] != 0 { a.u[i] / b.u[i] } else { !0 }; m.u[i] = if b.u[i] != 0 { a.u[i] % b.u[i] } else { !0 }; });
            run.store(o[0], q, false); run.store(o[1], m, false);
        }
        OP_ITOF => { let a = run.src_i(o[1]); each!(|i| v.setf(i, a.i(i) as f32)); out!(); }
        OP_UTOF => { let a = run.src_i(o[1]); each!(|i| v.setf(i, a.u[i] as f32)); out!(); }
        OP_FTOI => { let a = run.src_f(o[1]); each!(|i| { let f = a.f(i); v.u[i] = (if f != f { 0 } else if f >= 2147483648.0 { 0x7FFF_FFFF } else if f <= -2147483648.0 { i32::MIN } else { f as i32 }) as u32; }); out!(); }
        OP_FTOU => { let a = run.src_f(o[1]); each!(|i| { let f = a.f(i); v.u[i] = if !(f > 0.0) { 0 } else if f >= 4294967296.0 { 0xFFFF_FFFF } else { f as u32 }; }); out!(); }
        OP_RCP => fun!(|a| fdiv(1.0, a)),
        OP_F32TOF16 => { let a = run.src_f(o[1]); each!(|i| v.u[i] = half_bits(a.u[i])); sat = false; out!(); }
        OP_F16TOF32 => iun!(|a| float_bits(a)),
        OP_COUNTBITS => iun!(|a| a.count_ones()),
        OP_BFREV => iun!(|a| a.reverse_bits()),
        OP_FIRSTBIT_HI => iun!(|a| if a != 0 { a.leading_zeros() } else { !0 }),
        OP_FIRSTBIT_LO => iun!(|a| if a != 0 { a.trailing_zeros() } else { !0 }),
        OP_FIRSTBIT_SHI => iun!(|a| { let x = if (a as i32) < 0 { !a } else { a }; if x != 0 { x.leading_zeros() } else { !0 } }),
        OP_UADDC | OP_USUBB => {
            let a = run.src_i(o[2]); let b = run.src_i(o[3]); let mut flags = Vec4::default();
            each!(|i| if ins.op == OP_UADDC { v.u[i] = a.u[i].wrapping_add(b.u[i]); flags.u[i] = (v.u[i] < a.u[i]) as u32; }
                      else { v.u[i] = a.u[i].wrapping_sub(b.u[i]); flags.u[i] = (a.u[i] < b.u[i]) as u32; });
            run.store(o[0], v, false); run.store(o[1], flags, false);
        }
        OP_UBFE | OP_IBFE => {
            let a = run.src_i(o[1]); let b = run.src_i(o[2]); let c = run.src_i(o[3]);
            each!(|i| { let offset = b.u[i] & 31; let width = core::cmp::min(a.u[i] & 31, 32 - offset);
                let mut bits = if width != 0 { (c.u[i] >> offset) & ((1u32 << width).wrapping_sub(1)) } else { 0 };
                if ins.op == OP_IBFE && width != 0 { let sign = 1u32 << (width - 1); bits = (bits ^ sign).wrapping_sub(sign); }
                v.u[i] = bits; });
            out!();
        }
        OP_BFI => {
            let a = run.src_i(o[1]); let b = run.src_i(o[2]); let c = run.src_i(o[3]); let d = run.src_i(o[4]);
            each!(|i| { let offset = b.u[i] & 31; let width = core::cmp::min(a.u[i] & 31, 32 - offset);
                let m = if width != 0 { ((1u32 << width).wrapping_sub(1)) << offset } else { 0 };
                v.u[i] = ((c.u[i] << offset) & m) | (d.u[i] & !m); });
            out!();
        }
        OP_SWAPC => {
            let a = run.src_i(o[2]); let b = run.src_i(o[3]); let c = run.src_i(o[4]); let mut other = Vec4::default();
            each!(|i| { v.u[i] = if a.u[i] != 0 { c.u[i] } else { b.u[i] }; other.u[i] = if a.u[i] != 0 { b.u[i] } else { c.u[i] }; });
            run.store(o[0], v, false); run.store(o[1], other, false);
        }
        OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B => {
            let a = run.src_f(o[1]);
            let (t, sm) = (run.op(o[2] as usize), run.op(o[3] as usize));
            let lod = if ins.op == OP_SAMPLE_L { run.src_f(o[4]).f(0) } else { 0.0 };
            let mut px = [0f32; 4];
            let uv = [a.f(0), a.f(1), a.f(2), a.f(3)];
            if env.sample.is_some() { dxbc_sample(env, t.idx[0], sm.idx[0], uv.as_ptr(), &lod, px.as_mut_ptr()); }
            each!(|i| v.setf(i, px[t.swz[i] as usize & 3]));
            out!();
        }
        OP_LD => {
            let a = run.src_i(o[1]);
            let t = run.op(o[2] as usize);
            let mut px = [0f32; 4];
            let xy = [a.i(0), a.i(1), a.i(2), a.i(3)];
            if let Some(f) = env.load { f(env.ctx, t.idx[0], xy.as_ptr(), px.as_mut_ptr()); }
            each!(|i| v.setf(i, px[t.swz[i] as usize & 3]));
            out!();
        }
        OP_RESINFO => {
            let t = run.op(o[2] as usize);
            let mut dim = [1u32; 4];
            if let Some(f) = env.size { f(env.ctx, t.idx[0], dim.as_mut_ptr()); }
            let mut raw = Vec4::default();
            for i in 0..4 {
                if ins.ret_type == 2 { raw.u[i] = dim[i]; }
                else if ins.ret_type == 1 { raw.setf(i, if dim[i] != 0 { 1.0f32 / dim[i] as f32 } else { 0.0 }); }
                else { raw.setf(i, dim[i] as f32); }
            }
            each!(|i| v.u[i] = raw.u[t.swz[i] as usize & 3]);
            out!();
        }
        OP_IF => { if cond!(0) != ins.nonzero { next_pc = ins.jump; } }
        OP_ELSE => { next_pc = ins.jump; }            /* reached from the "then" part: skip to endif */
        OP_ENDIF | OP_NOP | OP_LOOP => {}
        OP_ENDLOOP | OP_BREAK | OP_CONTINUE => { next_pc = ins.jump; }
        OP_BREAKC | OP_CONTINUEC => { if cond!(0) == ins.nonzero { next_pc = ins.jump; } }
        OP_DISCARD => { if cond!(0) == ins.nonzero { (*exec).discard = 1; next_pc = nins; } }
        OP_RET => { next_pc = nins; }
        OP_RETC => { if cond!(0) == ins.nonzero { next_pc = nins; } }
        OP_BLOCK => { (*exec).block = ins.jump; }
        OP_BRANCH => { (*exec).previous_block = (*exec).block; next_pc = if ins.nops == 0 || cond!(0) != 0 { ins.jump } else { ins.alt }; }
        OP_PHI => {
            let pb = (*exec).previous_block;
            v = if pb == ins.jump { run.src_i(o[1]) } else if pb == ins.alt { run.src_i(o[2]) } else { ZERO4 };
            out!();
        }
        _ => {}
    }
    next_pc
}
/// Run the instruction at pc on an initialised context; the next pc, or -1 after a discard.
#[no_mangle]
pub unsafe extern "C" fn dxbc_exec_step(exec: *mut Exec, pc: c_int) -> c_int {
    let s = &*(*exec).shader; let env = &*(*exec).env;
    let run = Run { s, pool: s.pool(), e: exec, env };
    if pc < 0 || pc >= s.nins { return -1; }
    let n = exec_ins(&run, exec, s.ins().get_unchecked(pc as usize), pc, s.nins, env);
    if (*exec).discard != 0 { -1 } else { n }
}

#[no_mangle]
pub unsafe extern "C" fn dxbc_exec_run(exec: *mut Exec) -> CBool {
    let s = &*(*exec).shader;
    {
        let rb = core::mem::size_of::<Vec4>() * (s.ntemps.max(1) as usize + s.xt_total.max(0) as usize);
        let bindings = core::mem::size_of::<*const u32>() * s.npool.max(0) as usize;
        let f = *(((*exec).storage as *const u8).add(rb + bindings) as *const usize);
        if f != 0 { return (core::mem::transmute::<usize, crate::jit::JitFn>(f))(exec) as CBool; }
    }
    interp_run(exec)
}
/// The interpreter itself (what runs when there is no machine code).
pub(crate) unsafe fn interp_run(exec: *mut Exec) -> CBool {
    let s = &*(*exec).shader;
    let env = &*(*exec).env;
    let run = Run { s, pool: s.pool(), e: exec, env };
    let code = s.ins();
    let nins = s.nins;
    {
        let e = &mut *exec;
        e.discard = 0; e.block = -1; e.previous_block = -1;
        if s.xt_total != 0 { ptr::write_bytes(e.x, 0, s.xt_total as usize); }
        for i in 0..s.nclear as usize { *e.out.add(s.clear_regs[i] as usize) = Vec4::default(); }
    }
    let mut budget: i64 = 1_000_000;
    let mut pc: i32 = 0;
    loop {
        if pc >= nins || pc < 0 { break; }
        let b = budget; budget -= 1;
        if b <= 0 { break; }
        let ins = code.get_unchecked(pc as usize);
        pc = exec_ins(&run, exec, ins, pc, nins, env);
    }
    ((*exec).discard == 0 && budget >= 0) as CBool
}

#[no_mangle]
pub unsafe extern "C" fn dxbc_run(s: *const Shader, env: *const Env, inp: *const Vec4, out: *mut Vec4) -> CBool {
    let mut exec = core::mem::MaybeUninit::<Exec>::zeroed();
    if dxbc_exec_init(exec.as_mut_ptr(), s, env, inp, out) == 0 { return 0; }
    let r = dxbc_exec_run(exec.as_mut_ptr());
    dxbc_exec_fini(exec.as_mut_ptr());
    r
}

#[cfg(test)]
mod tests {
    use super::*;
    extern "C" { fn malloc(n: usize) -> *mut c_void; fn calloc(n: usize, m: usize) -> *mut c_void; fn realloc(p: *mut c_void, n: usize) -> *mut c_void; fn free(p: *mut c_void); }
    /* the C allocator the kernel provides, for the test program */
    #[no_mangle] extern "C" fn kmalloc(n: usize) -> *mut c_void { unsafe { malloc(n.max(1)) } }
    #[no_mangle] extern "C" fn kzalloc(n: usize) -> *mut c_void { unsafe { calloc(1, n.max(1)) } }
    #[no_mangle] extern "C" fn krealloc(p: *mut c_void, n: usize) -> *mut c_void { unsafe { realloc(p, n.max(1)) } }
    #[no_mangle] extern "C" fn kfree(p: *mut c_void) { unsafe { free(p) } }
    /* src/win32/shader_abi.c */
    #[no_mangle] unsafe extern "C" fn dxbc_math(f: c_int, x: *const f64, r: *mut f64) { let x = *x; *r = match f { 0 => x.sqrt(), 1 => x.sin(), 2 => x.cos(), 3 => x.exp2(), _ => x.log2() }; }
    #[no_mangle] unsafe extern "C" fn dxbc_sample(env: *const Env, tex: c_int, smp: c_int, uv: *const f32, lod: *const f32, out: *mut f32) { ((*env).sample.unwrap())((*env).ctx, tex, smp, uv, *lod, out) }

    fn fixtures() -> std::vec::Vec<(std::string::String, std::vec::Vec<u8>)> {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/shaders");
        let mut v: std::vec::Vec<_> = std::fs::read_dir(dir).unwrap().filter_map(|e| {
            let p = e.ok()?.path();
            let ext = p.extension()?.to_str()?;
            if ext != "dxbc" && ext != "dxil" { return None; }
            Some((p.file_name()?.to_str()?.into(), std::fs::read(&p).ok()?))
        }).collect();
        v.sort();
        v
    }
    fn load_bytes(b: &[u8]) -> Result<*mut Shader, std::string::String> {
        let mut err = [0 as c_char; 160];
        let s = unsafe { dxbc_load(b.as_ptr() as *const c_void, b.len(), err.as_mut_ptr(), 160) };
        if s.is_null() { Err(unsafe { core::ffi::CStr::from_ptr(err.as_ptr()) }.to_string_lossy().into()) } else { Ok(s) }
    }

    #[test]
    fn fixtures_load_and_run() {
        let all = fixtures();
        assert!(all.len() > 50);
        let mut loaded = 0;
        for (name, b) in &all {
            match load_bytes(b) {
                Ok(s) => {
                    loaded += 1;
                    let rows = [Vec4 { u: [0x3F80_0000; 4] }; 16];
                    let mut env: Env = unsafe { core::mem::zeroed() };
                    for k in 0..14 { env.cb[k] = rows.as_ptr(); env.cb_rows[k] = 16; }
                    let inp = [Vec4 { u: [0x3F00_0000, 0x3E80_0000, 0x3F40_0000, 0x3F80_0000] }; DX_MAX_REG];
                    let mut out = [Vec4::default(); DX_MAX_REG];
                    unsafe { dxbc_run(s, &env, inp.as_ptr(), out.as_mut_ptr()); dxbc_free(s); }
                }
                Err(e) => assert!(!e.is_empty(), "{name}: rejected without a reason"),
            }
        }
        assert!(loaded > 40, "only {loaded} fixtures loaded");
        /* refused for what they use, with the reason */
        let wave = all.iter().find(|(n, _)| n.starts_with("wave")).unwrap();
        assert!(load_bytes(&wave.1).is_err());
    }

    #[test]
    fn damaged_shaders_do_not_panic() {
        let all = fixtures();
        let mut x = 0x2545_F491_4F6C_DD1Du64;
        let mut rnd = || { x ^= x << 13; x ^= x >> 7; x ^= x << 17; x };
        for _ in 0..20000 {
            let k = (rnd() % all.len() as u64) as usize;
            let mut b = all[k].1.clone();
            for _ in 0..1 + rnd() % 4 {
                let at = (rnd() % b.len() as u64) as usize;
                match rnd() % 3 { 0 => b[at] ^= 1 << (rnd() % 8), 1 => b[at] = rnd() as u8, _ => b.truncate(at.max(1)) }
            }
            if let Ok(s) = load_bytes(&b) {
                let env: Env = unsafe { core::mem::zeroed() };
                let inp = [Vec4 { u: [0x7FC0_0000, 1, 0xFFFF_FFFF, 0x3F80_0000] }; DX_MAX_REG];
                let mut out = [Vec4::default(); DX_MAX_REG];
                unsafe { dxbc_run(s, &env, inp.as_ptr(), out.as_mut_ptr()); dxbc_free(s); }
            }
        }
    }
}
