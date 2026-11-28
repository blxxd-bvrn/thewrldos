//! A machine-code compiler for the shader IR (shader_priv.h) that dxbc.rs interprets: the
//! programs of Direct3D 10 and of GLSL (glsl/) become x86 SSE code, compiled once per
//! shader and run for every vertex and pixel instead of the interpreter.
//!
//! Every register stays in memory, where the interpreter keeps it (the temporaries, the
//! inputs, the outputs and the constant buffers of the `Exec` and its `Env`), so each
//! instruction is a few loads, SSE operations and a store. The common arithmetic,
//! comparison, logic, conversion and control-flow instructions are compiled; any other
//! instruction (texture sampling, sine and cosine, exponentials, integer division...)
//! calls the interpreter for just that instruction (`dxbc_exec_step`), so it gives the
//! same bits it always did. Programs with DXIL blocks or indexable temporaries are left
//! to the interpreter.
//!
//! The code uses eight general and eight SSE registers and no REX prefixes except on
//! pointer loads, so the same bytes run in the 32-bit and the 64-bit edition; only the
//! entry, the exit and the calls out differ. Constants sit after the code and are
//! addressed RIP-relative (64-bit) or absolutely (32-bit) with the same ModRM form.

#![allow(clippy::missing_safety_doc)]

use crate::dxbc::*;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};
use core::mem::offset_of;
use core::sync::atomic::{AtomicBool, Ordering};

pub(crate) const X64: bool = cfg!(target_pointer_width = "64");
pub(crate) const PS: i32 = core::mem::size_of::<usize>() as i32;

pub(crate) const EAX: u8 = 0; pub(crate) const ECX: u8 = 1; pub(crate) const EDX: u8 = 2; pub(crate) const EBX: u8 = 3; pub(crate) const ESP: u8 = 4; pub(crate) const EBP: u8 = 5; pub(crate) const ESI: u8 = 6; pub(crate) const EDI: u8 = 7;


/// A memory operand: [base + disp], [base + index + disp], or constant k.
#[derive(Clone, Copy)]
pub(crate) enum M { B(u8, i32), BI(u8, u8, i32), K(usize) }

pub(crate) struct Asm {
    pub(crate) b: Vec<u8>,
    pub(crate) consts: Vec<[u32; 4]>,
    pub(crate) cfix: Vec<(usize, usize, usize)>,     // disp32 position, end of the instruction, constant
    pub(crate) label: Vec<usize>,                     // code offset of each IR instruction
    pub(crate) jfix: Vec<(usize, usize)>,
    /// 256-bit AVX forms (VEX, 8 lanes) of the SSE instructions, for jit4.rs at 8 lanes
    pub(crate) wide: bool,             // rel32 position, IR target (nins: the "ok" exit, nins+1: "fail")
}

impl Asm {
    pub(crate) fn byte(&mut self, x: u8) { self.b.push(x); }
    pub(crate) fn d32(&mut self, x: i32) { self.b.extend_from_slice(&x.to_le_bytes()); }
    pub(crate) fn konst(&mut self, v: [u32; 4]) -> usize {
        if let Some(i) = self.consts.iter().position(|c| *c == v) { return i; }
        self.consts.push(v); self.consts.len() - 1
    }
    pub(crate) fn k4(&mut self, x: u32) -> usize {
        if !self.wide { return self.konst([x; 4]); }
        /* 32 bytes: two entries in a row (looked up as a pair) */
        if let Some(i) = (0..self.consts.len().saturating_sub(1)).find(|&i| self.consts[i] == [x; 4] && self.consts[i + 1] == [x; 4]) { return i; }
        self.consts.push([x; 4]); self.consts.push([x; 4]); self.consts.len() - 2
    }
    /// VEX prefix (three-byte form) for 256-bit operations: legacy prefix pre, opcode map
    /// (1: 0F, 2: 0F38), second source v (0 when there is none)
    pub(crate) fn vex(&mut self, pre: u8, map: u8, v: u8) {
        let pp = match pre { 0x66 => 1, 0xF3 => 2, 0xF2 => 3, _ => 0 };
        self.byte(0xC4); self.byte(0xE0 | map); self.byte(((!v & 15) << 3) | 4 | pp);
    }
    /// The register an SSE instruction also reads, as VEX's second source: none for the
    /// one-operand forms, the shifted register for the shift-by-immediate group
    fn vex_v(op: u8, x: u8, y: u8) -> u8 {
        match op { 0x10 | 0x11 | 0x28 | 0x29 | 0x50 | 0x51 | 0x5B | 0x70 => 0, 0x71 | 0x72 | 0x73 => y, _ => x }
    }
    pub(crate) fn vbroadcastss(&mut self, x: u8, m: M) { self.vex(0x66, 2, 0); self.byte(0x18); self.modrm(x, m, 0); }
    pub(crate) fn vzeroupper(&mut self) { self.byte(0xC5); self.byte(0xF8); self.byte(0x77); }
    /// ModRM (+SIB, disp32) for register field r and operand m; imm: immediate bytes that follow.
    pub(crate) fn modrm(&mut self, r: u8, m: M, imm: usize) {
        match m {
            M::B(base, disp) => {
                self.byte(0x80 | (r << 3) | base);
                if base == ESP { self.byte(0x24); }
                self.d32(disp);
            }
            M::BI(base, idx, disp) => { self.byte(0x84 | (r << 3)); self.byte((idx << 3) | base); self.d32(disp); }
            M::K(k) => { self.byte((r << 3) | 5); let p = self.b.len(); self.d32(0); self.cfix.push((p, p + 4 + imm, k)); }
        }
    }
    pub(crate) fn modreg(&mut self, r: u8, rm: u8) { self.byte(0xC0 | (r << 3) | rm); }
    /* SSE: [prefix] 0F op */
    pub(crate) fn sse_m(&mut self, pre: u8, op: u8, x: u8, m: M) {
        if self.wide { self.vex(pre, 1, Self::vex_v(op, x, 0)); self.byte(op); self.modrm(x, m, 0); return; }
        if pre != 0 { self.byte(pre); } self.byte(0x0F); self.byte(op); self.modrm(x, m, 0);
    }
    pub(crate) fn sse_r(&mut self, pre: u8, op: u8, x: u8, y: u8) {
        if self.wide { self.vex(pre, 1, Self::vex_v(op, x, y)); self.byte(op); self.modreg(x, y); return; }
        if pre != 0 { self.byte(pre); } self.byte(0x0F); self.byte(op); self.modreg(x, y);
    }
    pub(crate) fn sse_ri(&mut self, pre: u8, op: u8, x: u8, y: u8, imm: u8) { self.sse_r(pre, op, x, y); self.byte(imm); }
    pub(crate) fn sse_mi(&mut self, pre: u8, op: u8, x: u8, m: M, imm: u8) {
        if self.wide { self.vex(pre, 1, Self::vex_v(op, x, 0)); self.byte(op); self.modrm(x, m, 1); self.byte(imm); return; }
        if pre != 0 { self.byte(pre); } self.byte(0x0F); self.byte(op); self.modrm(x, m, 1); self.byte(imm);
    }
    pub(crate) fn movups_ld(&mut self, x: u8, m: M) { self.sse_m(0, 0x10, x, m); }
    pub(crate) fn movups_st(&mut self, m: M, x: u8) { self.sse_m(0, 0x11, x, m); }
    pub(crate) fn movaps(&mut self, x: u8, y: u8) { self.sse_r(0, 0x28, x, y); }
    pub(crate) fn zero(&mut self, x: u8) { self.sse_r(0, 0x57, x, x); }
    pub(crate) fn shufps(&mut self, x: u8, y: u8, imm: u8) { self.sse_ri(0, 0xC6, x, y, imm); }
    pub(crate) fn pshufd(&mut self, x: u8, y: u8, imm: u8) { self.sse_ri(0x66, 0x70, x, y, imm); }
    pub(crate) fn movd_to_gp(&mut self, r: u8, x: u8) { self.byte(0x66); self.byte(0x0F); self.byte(0x7E); self.modreg(x, r); }
    /* general purpose */
    pub(crate) fn rexw(&mut self) { if X64 { self.byte(0x48); } }
    pub(crate) fn mov_ld(&mut self, r: u8, m: M) { self.byte(0x8B); self.modrm(r, m, 0); }
    pub(crate) fn mov_ld_ptr(&mut self, r: u8, m: M) { self.rexw(); self.mov_ld(r, m); }
    pub(crate) fn mov_st_imm(&mut self, m: M, v: i32) { self.byte(0xC7); self.modrm(0, m, 4); self.d32(v); }
    pub(crate) fn add_imm(&mut self, r: u8, v: i32) { self.byte(0x81); self.modreg(0, r); self.d32(v); }
    pub(crate) fn cmp_imm(&mut self, r: u8, v: i32) { self.byte(0x81); self.modreg(7, r); self.d32(v); }
    pub(crate) fn cmp_m_imm(&mut self, m: M, v: i32) { self.byte(0x81); self.modrm(7, m, 4); self.d32(v); }
    pub(crate) fn cmp_r_m(&mut self, r: u8, m: M) { self.byte(0x3B); self.modrm(r, m, 0); }
    pub(crate) fn test(&mut self, r: u8) { self.byte(0x85); self.modreg(r, r); }
    pub(crate) fn shl(&mut self, r: u8, n: u8) { self.byte(0xC1); self.modreg(4, r); self.byte(n); }
    /// A forward jump (cc: 0x80+cond, or 0 for jmp) to be patched; returns the patch position.
    pub(crate) fn jfwd(&mut self, cc: u8) -> usize { if cc == 0 { self.byte(0xE9); } else { self.byte(0x0F); self.byte(cc); } let p = self.b.len(); self.d32(0); p }
    pub(crate) fn here(&mut self, p: usize) { let rel = (self.b.len() - (p + 4)) as i32; self.b[p..p + 4].copy_from_slice(&rel.to_le_bytes()); }
    pub(crate) fn jir(&mut self, cc: u8, target: usize) { let p = self.jfwd(cc); self.jfix.push((p, target)); }
}

pub(crate) const JAE: u8 = 0x83; pub(crate) const JE: u8 = 0x84; pub(crate) const JNE: u8 = 0x85; pub(crate) const JBE: u8 = 0x86;

struct Lk { a: Asm }
struct Gen<'a> { a: Asm, s: &'a Shader, pool: &'a [Opnd] }

fn off_exec_r() -> i32 { offset_of!(Exec, r) as i32 }
fn off_exec_inp() -> i32 { offset_of!(Exec, inp) as i32 }
fn off_exec_out() -> i32 { offset_of!(Exec, out) as i32 }
fn off_exec_env() -> i32 { offset_of!(Exec, env) as i32 }
fn off_exec_block() -> i32 { offset_of!(Exec, block) as i32 }
fn off_exec_discard() -> i32 { offset_of!(Exec, discard) as i32 }
pub(crate) fn off_env_cb() -> i32 { offset_of!(Env, cb) as i32 }
pub(crate) fn off_env_rows() -> i32 { offset_of!(Env, cb_rows) as i32 }
pub(crate) fn off_env_ctx() -> i32 { offset_of!(Env, ctx) as i32 }
pub(crate) fn off_env_sample() -> i32 { offset_of!(Env, sample) as i32 }
fn off_exec_scratch() -> i32 { offset_of!(Exec, scratch) as i32 }

pub(crate) const BUDGET: i32 = 1_000_000;    // loop iterations per run, as the interpreter bounds instructions

impl<'a> Gen<'a> {
    fn base_of(f: u8) -> Option<(u8, i32)> {
        match f { T_TEMP => Some((ESI, 0)), T_INPUT => Some((EBP, 0)), T_OUTPUT => Some((EDI, 0)), T_ODEPTH => Some((EDI, 16 * (DX_MAX_REG as i32 - 1))), _ => None }
    }
    /// The address of a register operand; for a dynamic index, code that jumps to the
    /// returned patch position when the index is out of range. None: not compiled here.
    fn addr(&mut self, o: &Opnd) -> Option<(M, Option<usize>)> {
        match o.ty {
            T_TEMP | T_INPUT | T_OUTPUT => {
                let (b, _) = Self::base_of(o.ty)?;
                if o.rel[0] < 0 { return Some((M::B(b, 16 * o.idx[0]), None)); }
                let r = self.pool[o.rel[0] as usize];
                let (rb, _) = Self::base_of(r.ty)?;
                if r.rel[0] >= 0 || r.md != 0 || !(r.ty == T_TEMP || r.ty == T_INPUT || r.ty == T_OUTPUT) { return None; }
                self.a.mov_ld(EAX, M::B(rb, 16 * r.idx[0] + 4 * (r.swz[0] & 3) as i32));
                if o.idx[0] != 0 { self.a.add_imm(EAX, o.idx[0]); }
                let limit = if o.ty == T_TEMP { self.s.ntemps.max(1) } else { DX_MAX_REG as i32 };
                self.a.cmp_imm(EAX, limit);
                let oob = self.a.jfwd(JAE);
                self.a.shl(EAX, 4);
                Some((M::BI(b, EAX, 0), Some(oob)))
            }
            T_ODEPTH => Some((M::B(EDI, 16 * (DX_MAX_REG as i32 - 1)), None)),
            T_CB => {
                if o.idx[0] < 0 || o.idx[0] >= 14 || o.rel[0] >= 0 { return None; }
                let k = o.idx[0];
                self.a.mov_ld_ptr(ECX, M::B(EBX, off_exec_env()));
                self.a.mov_ld_ptr(EDX, M::B(ECX, off_env_cb() + PS * k));
                if o.rel[1] < 0 {
                    self.a.cmp_m_imm(M::B(ECX, off_env_rows() + 4 * k), o.idx[1]);
                    let oob = self.a.jfwd(JBE);
                    return Some((M::B(EDX, 16 * o.idx[1]), Some(oob)));
                }
                let r = self.pool[o.rel[1] as usize];
                let (rb, _) = Self::base_of(r.ty)?;
                if r.rel[0] >= 0 || r.md != 0 || !(r.ty == T_TEMP || r.ty == T_INPUT || r.ty == T_OUTPUT) { return None; }
                self.a.mov_ld(EAX, M::B(rb, 16 * r.idx[0] + 4 * (r.swz[0] & 3) as i32));
                if o.idx[1] != 0 { self.a.add_imm(EAX, o.idx[1]); }
                self.a.cmp_r_m(EAX, M::B(ECX, off_env_rows() + 4 * k));
                let oob = self.a.jfwd(JAE);
                self.a.shl(EAX, 4);
                Some((M::BI(EDX, EAX, 0), Some(oob)))
            }
            _ => None,
        }
    }
    /// Can this operand be read (or written) by compiled code?
    fn ok_opnd(&self, k: i16, dst: bool) -> bool {
        let o = &self.pool[k as usize];
        /* displacements must fit: indices beyond these go to the interpreter, which checks them */
        let small = |v: i32| (-65536..=65536).contains(&v);
        if !small(o.idx[0]) || !small(o.idx[1]) { return false; }
        let rel_ok = |r: i16| r < 0 || { let x = &self.pool[r as usize]; x.rel[0] < 0 && x.md == 0 && (x.ty == T_TEMP || x.ty == T_INPUT || x.ty == T_OUTPUT) && small(x.idx[0]) };
        match o.ty {
            T_NULL => dst,
            T_IMM32 => !dst,
            T_TEMP | T_OUTPUT => rel_ok(o.rel[0]),
            T_INPUT => !dst && rel_ok(o.rel[0]),
            T_ODEPTH => true,
            T_CB => !dst && o.rel[0] < 0 && o.idx[0] >= 0 && o.idx[0] < 14 && o.idx[1] >= 0 && rel_ok(o.rel[1]),
            _ => false,
        }
    }
    /// xmm x = the source operand, swizzled, with its modifier (integer or float).
    fn load(&mut self, k: i16, x: u8, int: bool) {
        let o = self.pool[k as usize];
        if o.ty == T_IMM32 {
            let mut v = [0u32; 4];
            for i in 0..4 { v[i] = o.imm[(o.swz[i] & 3) as usize]; }
            for e in v.iter_mut() {
                if int { if o.md & 2 != 0 && (*e as i32) < 0 { *e = e.wrapping_neg(); } if o.md & 1 != 0 { *e = e.wrapping_neg(); } }
                else { if o.md & 2 != 0 { *e &= 0x7FFF_FFFF; } if o.md & 1 != 0 { *e ^= 0x8000_0000; } }
            }
            let c = self.a.konst(v);
            self.a.movups_ld(x, M::K(c));
            return;
        }
        let (m, oob) = self.addr(&o).expect("checked operand");
        self.a.movups_ld(x, m);
        if let Some(p) = oob {
            let done = self.a.jfwd(0);
            self.a.here(p);
            self.a.zero(x);
            self.a.here(done);
        }
        if o.identity == 0 || o.ty == T_ODEPTH {
            let s = o.swz;
            self.a.shufps(x, x, s[0] | (s[1] << 2) | (s[2] << 4) | (s[3] << 6));
        }
        if o.md != 0 {
            if int {
                if o.md & 2 != 0 { self.a.movaps(7, x); self.a.sse_ri(0x66, 0x72, 4, 7, 31); /* psrad x7, 31 */ self.a.sse_r(0x66, 0xEF, x, 7); self.a.sse_r(0x66, 0xFA, x, 7); }
                if o.md & 1 != 0 { self.a.zero(7); self.a.sse_r(0x66, 0xFA, 7, x); self.a.movaps(x, 7); }
            } else {
                if o.md & 2 != 0 { let c = self.a.k4(0x7FFF_FFFF); self.a.sse_m(0, 0x54, x, M::K(c)); }
                if o.md & 1 != 0 { let c = self.a.k4(0x8000_0000); self.a.sse_m(0, 0x57, x, M::K(c)); }
            }
        }
    }
    /// Store xmm x to the destination operand, lanes as its mask says, saturated if asked.
    fn store(&mut self, k: i16, x: u8, sat: bool) {
        let o = self.pool[k as usize];
        if o.ty == T_NULL { return; }
        if sat {
            let z = self.a.k4(0); let one = self.a.k4(0x3F80_0000);
            self.a.sse_m(0, 0x5F, x, M::K(z));     /* maxps: NaN becomes 0 */
            self.a.sse_m(0, 0x5D, x, M::K(one));   /* minps */
        }
        let (m, oob) = self.addr(&o).expect("checked operand");
        let mask = o.mask & 15;
        if mask == 15 { self.a.movups_st(m, x); }
        else {
            /* whole-register read, blend and write: a later full read then forwards from the
             * store (lane-sized stores followed by a full load stall the processor) */
            let mut mv = [0u32; 4];
            for l in 0..4 { if mask & (1 << l) != 0 { mv[l] = !0; } }
            let mk = self.a.konst(mv);
            self.a.movups_ld(7, m);
            self.a.movups_ld(6, M::K(mk));
            self.a.sse_r(0, 0x54, x, 6);              /* andps x, mask */
            self.a.sse_r(0, 0x55, 6, 7);              /* andnps x6, old */
            self.a.sse_r(0, 0x56, x, 6);              /* orps */
            self.a.movups_st(m, x);
        }
        if let Some(p) = oob { self.a.here(p); }
    }
    /// eax = lane 0 of the operand (for conditions; a modifier does not change zero/nonzero)
    fn cond(&mut self, k: i16) { self.load(k, 0, true); self.a.movd_to_gp(EAX, 0); self.a.test(EAX); }
    fn helper(&mut self, pc: usize) {
        #[cfg(test)] { let op = self.s.ins()[pc].op; HELPED.with(|h| h.borrow_mut().push(op)); }
        if X64 {
            self.a.byte(0x48); self.a.byte(0x89); self.a.byte(0xDF);              /* mov rdi, rbx */
            self.a.byte(0xBE); self.a.d32(pc as i32);                             /* mov esi, pc */
            self.a.byte(0x48); self.a.byte(0xB8); self.a.b.extend_from_slice(&(dxbc_exec_step as *const () as usize as u64).to_le_bytes());   /* mov rax, imm64 */
            self.a.byte(0xFF); self.a.byte(0xD0);                                 /* call rax */
            self.a.mov_ld_ptr(ESI, M::B(EBX, off_exec_r()));
            self.a.mov_ld_ptr(EDI, M::B(EBX, off_exec_out()));
        } else {
            self.a.byte(0x68); self.a.d32(pc as i32);                             /* push pc */
            self.a.byte(0x53);                                                    /* push ebx */
            self.a.byte(0xB8); self.a.d32(dxbc_exec_step as *const () as usize as i32);        /* mov eax, fn */
            self.a.byte(0xFF); self.a.byte(0xD0);                                 /* call eax */
            self.a.byte(0x83); self.a.byte(0xC4); self.a.byte(8);                 /* add esp, 8 */
        }
        self.a.cmp_imm(EAX, -1);
        let n = self.s.nins as usize;
        self.a.jir(JE, n + 1);
    }
    /// env->sample(ctx, texture, sampler, uv, lod, out) called directly; the texel lands at
    /// [esp] and goes through the texture operand's swizzle, as in the interpreter.
    fn sample(&mut self, i: &Ins) {
        let o = i.o;
        let t = self.pool[o[2] as usize]; let sm = self.pool[o[3] as usize];
        self.load(o[1], 0, false);
        self.a.movups_st(M::B(EBX, off_exec_scratch()), 0);                      /* uv */
        if i.op == OP_SAMPLE_L { self.load(o[4], 1, false); } else { self.a.zero(1); }
        self.a.zero(7); self.a.movups_st(M::B(ESP, 0), 7);                      /* texel = 0 if there is no sampler */
        self.a.mov_ld_ptr(EAX, M::B(EBX, off_exec_env()));
        self.a.byte(0x83); self.a.modrm(7, M::B(EAX, off_env_sample()), 1); self.a.byte(0);   /* cmp dword [eax+sample], 0 */
        let skip = if X64 {
            /* the pointer's high half too */
            let lo0 = self.a.jfwd(JNE);
            self.a.byte(0x83); self.a.modrm(7, M::B(EAX, off_env_sample() + 4), 1); self.a.byte(0);
            let sk = self.a.jfwd(JE);
            self.a.here(lo0);
            sk
        } else { self.a.jfwd(JE) };
        if X64 {
            self.a.mov_ld_ptr(EDI, M::B(EAX, off_env_ctx()));                    /* rdi = ctx */
            self.a.byte(0xBE); self.a.d32(t.idx[0]);                              /* esi = texture */
            self.a.byte(0xBA); self.a.d32(sm.idx[0]);                             /* edx = sampler */
            self.a.byte(0x48); self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, off_exec_scratch()), 0);   /* lea rcx, uv */
            self.a.movaps(0, 1);                                                   /* xmm0 = lod */
            self.a.byte(0x4C); self.a.byte(0x8D); self.a.byte(0x04); self.a.byte(0x24);                 /* lea r8, [rsp] */
            self.a.mov_ld_ptr(EAX, M::B(EAX, off_env_sample()));
            self.a.byte(0xFF); self.a.byte(0xD0);                                 /* call rax */
            self.a.mov_ld_ptr(ESI, M::B(EBX, off_exec_r()));
            self.a.mov_ld_ptr(EDI, M::B(EBX, off_exec_out()));
        } else {
            self.a.byte(0x8D); self.a.modrm(ECX, M::B(ESP, 0), 0); self.a.byte(0x51);       /* lea ecx, [esp]; push ecx (out) */
            self.a.movd_to_gp(EDX, 1); self.a.byte(0x52);                                   /* push lod */
            self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, off_exec_scratch()), 0); self.a.byte(0x51);   /* push uv */
            self.a.byte(0x68); self.a.d32(sm.idx[0]);
            self.a.byte(0x68); self.a.d32(t.idx[0]);
            self.a.byte(0xFF); self.a.modrm(6, M::B(EAX, off_env_ctx()), 0);                /* push ctx */
            self.a.byte(0xFF); self.a.modrm(2, M::B(EAX, off_env_sample()), 0);             /* call [eax+sample] */
            self.a.byte(0x83); self.a.byte(0xC4); self.a.byte(24);
        }
        self.a.here(skip);
        self.a.movups_ld(0, M::B(ESP, 0));
        let sw = t.swz;
        if sw != [0, 1, 2, 3] { self.a.shufps(0, 0, (sw[0] & 3) | ((sw[1] & 3) << 2) | ((sw[2] & 3) << 4) | ((sw[3] & 3) << 6)); }
        self.store(o[0], 0, i.sat != 0);
    }
    fn budget(&mut self) {
        self.a.byte(0xFF); self.a.modrm(1, M::B(EBX, off_exec_block()), 0);      /* dec dword [ebx+block] */
        let n = self.s.nins as usize;
        self.a.jir(JE, n + 1);
    }

    fn ins(&mut self, pc: usize, i: &Ins) {
        let o = i.o;
        let n = i.nops as usize;
        let mut nd = 0;
        let _ = operand_count(i.op, &mut nd);
        let all_ok = (0..n).all(|j| self.ok_opnd(o[j], (j as i32) < nd));
        let sat = i.sat != 0;
        macro_rules! bin { ($pre:expr, $op:expr, $int:expr) => {{ self.load(o[1], 0, $int); self.load(o[2], 1, $int); self.a.sse_r($pre, $op, 0, 1); self.store(o[0], 0, sat && !$int); }} }
        let ones = |g: &mut Gen| g.a.k4(!0);
        match i.op {
            OP_NOP | OP_LOOP | OP_ENDIF => {}
            OP_ELSE | OP_BREAK => { self.a.jir(0, i.jump as usize); }
            OP_ENDLOOP | OP_CONTINUE => { self.budget(); self.a.jir(0, i.jump as usize); }
            OP_RET => { let e = self.s.nins as usize; self.a.jir(0, e); }
            OP_IF | OP_BREAKC | OP_CONTINUEC | OP_RETC | OP_DISCARD if self.ok_opnd(o[0], false) => {
                self.cond(o[0]);
                /* take the branch when (cond != 0) == nonzero, except IF, which skips when it is not */
                let take_if_nonzero = if i.op == OP_IF { i.nonzero == 0 } else { i.nonzero != 0 };
                let cc = if take_if_nonzero { JNE } else { JE };
                match i.op {
                    OP_IF | OP_BREAKC => self.a.jir(cc, i.jump as usize),
                    OP_CONTINUEC => {
                        let skip = self.a.jfwd(if take_if_nonzero { JE } else { JNE });
                        self.budget(); self.a.jir(0, i.jump as usize);
                        self.a.here(skip);
                    }
                    OP_RETC => { let e = self.s.nins as usize; self.a.jir(cc, e); }
                    _ => {
                        let skip = self.a.jfwd(if take_if_nonzero { JE } else { JNE });
                        self.a.mov_st_imm(M::B(EBX, off_exec_discard()), 1);
                        let e = self.s.nins as usize + 1; self.a.jir(0, e);
                        self.a.here(skip);
                    }
                }
            }
            OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B if self.ok_opnd(o[0], true) && self.ok_opnd(o[1], false) && (i.op != OP_SAMPLE_L || self.ok_opnd(o[4], false))
                && self.pool[o[2] as usize].ty == T_RESOURCE && self.pool[o[3] as usize].ty == T_SAMPLER => self.sample(i),
            _ if !all_ok || i.op >= OP_BLOCK => self.helper(pc),
            OP_MOV => { self.load(o[1], 0, false); self.store(o[0], 0, sat); }
            OP_ADD => bin!(0, 0x58, false),
            OP_MUL => bin!(0, 0x59, false),
            OP_DIV => bin!(0, 0x5E, false),
            OP_MIN | OP_MAX => {
                /* the interpreter keeps a when b is NaN; minps/maxps give b then */
                self.load(o[1], 0, false); self.load(o[2], 1, false);
                self.a.movaps(2, 0);
                self.a.sse_r(0, if i.op == OP_MIN { 0x5D } else { 0x5F }, 2, 1);   /* x2 = min/max(a, b) */
                self.a.movaps(3, 1); self.a.sse_ri(0, 0xC2, 3, 1, 3);             /* x3 = b is NaN */
                self.a.sse_r(0, 0x54, 0, 3);                                      /* a & nan */
                self.a.sse_r(0, 0x55, 3, 2);                                      /* ~nan & m */
                self.a.sse_r(0, 0x56, 0, 3);
                self.store(o[0], 0, sat);
            }
            OP_MAD => { self.load(o[1], 0, false); self.load(o[2], 1, false); self.load(o[3], 2, false); self.a.sse_r(0, 0x59, 0, 1); self.a.sse_r(0, 0x58, 0, 2); self.store(o[0], 0, sat); }
            OP_DP2 | OP_DP3 | OP_DP4 => {
                self.load(o[1], 0, false); self.load(o[2], 1, false);
                self.a.sse_r(0, 0x59, 0, 1);                                     /* mulps */
                let k = match i.op { OP_DP2 => 2, OP_DP3 => 3, _ => 4 };
                for l in 1..k { self.a.pshufd(1 + l as u8, 0, l as u8); }       /* x2.. = lanes 1.. */
                for l in 1..k { self.a.sse_r(0xF3, 0x58, 0, 1 + l as u8); }     /* addss, in order */
                self.a.shufps(0, 0, 0);
                self.store(o[0], 0, sat);
            }
            OP_EQ | OP_NE | OP_LT | OP_GE => {
                self.load(o[1], 0, false); self.load(o[2], 1, false);
                match i.op {
                    OP_EQ => self.a.sse_ri(0, 0xC2, 0, 1, 0),
                    OP_NE => self.a.sse_ri(0, 0xC2, 0, 1, 4),
                    OP_LT => self.a.sse_ri(0, 0xC2, 0, 1, 1),
                    _ => { self.a.sse_ri(0, 0xC2, 1, 0, 2); self.a.movaps(0, 1); }   /* b <= a: false for NaN, as a >= b */
                }
                self.store(o[0], 0, false);
            }
            OP_AND => bin!(0x66, 0xDB, true),
            OP_OR => bin!(0x66, 0xEB, true),
            OP_XOR => bin!(0x66, 0xEF, true),
            OP_IADD => bin!(0x66, 0xFE, true),
            OP_IEQ => bin!(0x66, 0x76, true),
            OP_NOT => { self.load(o[1], 0, true); let c = ones(self); self.a.sse_m(0x66, 0xEF, 0, M::K(c)); self.store(o[0], 0, false); }
            OP_INEG => { self.load(o[1], 1, true); self.a.zero(0); self.a.sse_r(0x66, 0xFA, 0, 1); self.store(o[0], 0, false); }
            OP_INE => { self.load(o[1], 0, true); self.load(o[2], 1, true); self.a.sse_r(0x66, 0x76, 0, 1); let c = ones(self); self.a.sse_m(0x66, 0xEF, 0, M::K(c)); self.store(o[0], 0, false); }
            OP_ILT | OP_IGE => {
                self.load(o[1], 0, true); self.load(o[2], 1, true);
                self.a.sse_r(0x66, 0x66, 1, 0);                                   /* pcmpgtd x1, x0: b > a */
                if i.op == OP_IGE { let c = ones(self); self.a.sse_m(0x66, 0xEF, 1, M::K(c)); }
                self.store(o[0], 1, false);
            }
            OP_MOVC => {
                self.load(o[1], 0, true); self.load(o[2], 1, false); self.load(o[3], 2, false);
                self.a.zero(7); self.a.sse_r(0x66, 0x76, 0, 7);                  /* x0 = cond == 0 */
                self.a.sse_r(0x66, 0xDB, 2, 0);                                   /* pand x2, x0 */
                self.a.sse_r(0x66, 0xDF, 0, 1);                                   /* pandn x0, x1 */
                self.a.sse_r(0x66, 0xEB, 0, 2);                                   /* por */
                self.store(o[0], 0, sat);
            }
            OP_ITOF => { self.load(o[1], 0, true); self.a.sse_r(0, 0x5B, 0, 0); self.store(o[0], 0, sat); }
            OP_FTOI => {
                /* truncate; NaN is 0 and the out-of-range ends saturate, as the interpreter does */
                self.load(o[1], 0, false);
                self.a.movaps(1, 0);
                self.a.sse_r(0xF3, 0x5B, 2, 0);                                   /* cvttps2dq x2, x0 */
                let big = self.a.k4(0x4F00_0000);                                  /* 2^31 */
                self.a.sse_mi(0, 0xC2, 1, M::K(big), 5);                         /* x1 = !(x < 2^31): too big or NaN */
                let max = self.a.k4(0x7FFF_FFFF);
                self.a.movups_ld(3, M::K(max));
                self.a.sse_r(0x66, 0xDB, 3, 1);                                   /* x3 = max & big */
                self.a.sse_r(0x66, 0xDF, 1, 2);                                   /* x1 = ~big & x2 */
                self.a.sse_r(0x66, 0xEB, 1, 3);
                self.a.movaps(2, 0); self.a.sse_ri(0, 0xC2, 2, 0, 7);            /* x2 = ordered (not NaN) */
                self.a.sse_r(0x66, 0xDB, 1, 2);
                self.store(o[0], 1, false);
            }
            OP_SQRT => { self.load(o[1], 0, false); self.a.sse_r(0, 0x51, 0, 0); self.store(o[0], 0, sat); }
            OP_RSQ | OP_RCP => {
                self.load(o[1], 1, false);
                if i.op == OP_RSQ { self.a.sse_r(0, 0x51, 1, 1); }
                let one = self.a.k4(0x3F80_0000);
                self.a.movups_ld(0, M::K(one));
                self.a.sse_r(0, 0x5E, 0, 1);
                self.store(o[0], 0, sat);
            }
            OP_ROUND_NI | OP_FRC | OP_ROUND_Z | OP_ROUND_PI => {
                /* floor/truncate through integers; at 2^23 and beyond (and NaN, infinity)
                 * the value is already integral */
                self.load(o[1], 0, false);
                if i.op == OP_ROUND_PI { let c = self.a.k4(0x8000_0000); self.a.sse_m(0, 0x57, 0, M::K(c)); }   /* ceil(x) = -floor(-x) */
                self.a.sse_r(0xF3, 0x5B, 1, 0);                                   /* cvttps2dq */
                self.a.sse_r(0, 0x5B, 1, 1);                                      /* cvtdq2ps: t */
                if i.op != OP_ROUND_Z {
                    self.a.movaps(2, 0); self.a.sse_ri(0, 0xC2, 2, 1, 1);        /* x2 = x < t */
                    let one = self.a.k4(0x3F80_0000);
                    self.a.sse_m(0, 0x54, 2, M::K(one));
                    self.a.sse_r(0, 0x5C, 1, 2);                                  /* t -= 1 where x < t */
                }
                /* keep x where |x| >= 2^23 (or NaN): x3 = mask */
                self.a.movaps(3, 0);
                let abs = self.a.k4(0x7FFF_FFFF); self.a.sse_m(0, 0x54, 3, M::K(abs));
                let big = self.a.k4(0x4B00_0000); self.a.sse_mi(0, 0xC2, 3, M::K(big), 5);   /* !(|x| < 2^23) */
                self.a.movaps(2, 0); self.a.sse_r(0, 0x54, 2, 3);                /* x2 = x & mask */
                self.a.sse_r(0, 0x55, 3, 1);                                      /* x3 = ~mask & t */
                self.a.sse_r(0, 0x56, 3, 2);                                      /* x3 = floor */
                /* a zero result keeps the sign of x (-0.3 truncates to -0), as every other result already does */
                let sg = self.a.k4(0x8000_0000);
                self.a.movaps(2, 0); self.a.sse_m(0, 0x54, 2, M::K(sg)); self.a.sse_r(0, 0x56, 3, 2);
                match i.op {
                    OP_FRC => { self.a.sse_r(0, 0x5C, 0, 3); self.store(o[0], 0, sat); }   /* x - floor(x) */
                    OP_ROUND_PI => { let c = self.a.k4(0x8000_0000); self.a.sse_m(0, 0x57, 3, M::K(c)); self.store(o[0], 3, sat); }
                    _ => self.store(o[0], 3, sat),
                }
            }
            _ => self.helper(pc),
        }
    }
}

/// Machine code for a shader, or None when it stays with the interpreter.
pub fn compile(s: &Shader) -> Option<(*mut u8, usize)> {
    if s.xt_total != 0 || s.nins <= 0 || s.dxil != 0 { return None; }
    let ins = s.ins();
    if ins.iter().any(|i| i.op >= OP_BLOCK) { return None; }
    /* control flow is compiled or not at all */
    {
        let g0 = Gen { a: Asm { b: Vec::new(), consts: Vec::new(), cfix: Vec::new(), label: Vec::new(), jfix: Vec::new(), wide: false }, s, pool: s.pool() };
        if ins.iter().any(|i| matches!(i.op, OP_IF | OP_BREAKC | OP_CONTINUEC | OP_RETC | OP_DISCARD) && !g0.ok_opnd(i.o[0], false)) { return None; }
        if ins.iter().any(|i| matches!(i.op, OP_SWITCH | OP_CASE | OP_DEFAULT | OP_ENDSWITCH | OP_CALL | OP_CALLC | OP_LABEL)) { return None; }
    }
    let mut g = Gen { a: Asm { b: Vec::new(), consts: Vec::new(), cfix: Vec::new(), label: Vec::new(), jfix: Vec::new(), wide: false }, s, pool: s.pool() };
    let n = ins.len();
    /* entry: save registers, the context in ebx, the register files in esi, ebp, edi */
    for r in [EBX, EBP, ESI, EDI] { g.a.byte(0x50 + r); }
    /* 32 bytes of scratch at [esp] (the texel a sample returns), the stack 16-byte aligned for calls */
    if X64 { g.a.byte(0x48); g.a.byte(0x83); g.a.byte(0xEC); g.a.byte(40); g.a.byte(0x48); g.a.byte(0x89); g.a.byte(0xFB); }   /* sub rsp, 40; mov rbx, rdi */
    else { g.a.mov_ld(EBX, M::B(ESP, 20)); g.a.byte(0x83); g.a.byte(0xEC); g.a.byte(32); }       /* sub esp, 32 */
    g.a.mov_ld_ptr(ESI, M::B(EBX, off_exec_r()));
    g.a.mov_ld_ptr(EBP, M::B(EBX, off_exec_inp()));
    g.a.mov_ld_ptr(EDI, M::B(EBX, off_exec_out()));
    g.a.mov_st_imm(M::B(EBX, off_exec_discard()), 0);
    g.a.mov_st_imm(M::B(EBX, off_exec_block()), BUDGET);
    if s.nclear > 0 { g.a.zero(0); for k in 0..s.nclear as usize { g.a.movups_st(M::B(EDI, 16 * s.clear_regs[k] as i32), 0); } }
    for (pc, i) in ins.iter().enumerate() { g.a.label.push(g.a.b.len()); g.ins(pc, i); }
    link(g.a, n)
}

/// Common to both compilers: the exits (ok: label n, fail: n + 1), the epilogue for the
/// prologue both use, the jumps patched, and the code with its constants in memory.
pub(crate) fn link(a: Asm, n: usize) -> Option<(*mut u8, usize)> {
    let mut g = Lk { a };
    /* exits: ok (nins), fail (nins + 1) */
    let ok = g.a.b.len();
    g.a.byte(0xB8); g.a.d32(1);
    let tail = g.a.jfwd(0);
    let fail = g.a.b.len();
    g.a.byte(0xB8); g.a.d32(0);
    g.a.here(tail);
    if g.a.wide { g.a.vzeroupper(); }
    if X64 { g.a.byte(0x48); g.a.byte(0x83); g.a.byte(0xC4); g.a.byte(40); } else { g.a.byte(0x83); g.a.byte(0xC4); g.a.byte(32); }
    for r in [EDI, ESI, EBP, EBX] { g.a.byte(0x58 + r); }
    g.a.byte(0xC3);
    g.a.label.push(ok); g.a.label.push(fail);
    for &(p, t) in &g.a.jfix.clone() {
        let target = if t == n { ok } else if t > n { fail } else { g.a.label[t] };
        let rel = target as i64 - (p as i64 + 4);
        g.a.b[p..p + 4].copy_from_slice(&(rel as i32).to_le_bytes());
    }
    /* the constants after the code, 16-byte aligned */
    let code_len = (g.a.b.len() + 15) & !15;
    let total = code_len + 16 * g.a.consts.len();
    let mem = unsafe { jit_alloc(total + 16) };
    if mem.is_null() { return None; }
    let base = ((mem as usize + 15) & !15) as *mut u8;
    unsafe {
        core::ptr::copy_nonoverlapping(g.a.b.as_ptr(), base, g.a.b.len());
        for k in g.a.b.len()..code_len { *base.add(k) = 0xCC; }
        for (k, c) in g.a.consts.iter().enumerate() { core::ptr::copy_nonoverlapping(c.as_ptr() as *const u8, base.add(code_len + 16 * k), 16); }
        for &(p, end, k) in &g.a.cfix {
            let target = base as usize + code_len + 16 * k;
            let disp = if X64 { (target as i64 - (base as usize + end) as i64) as i32 } else { target as i32 };
            core::ptr::copy_nonoverlapping(disp.to_le_bytes().as_ptr(), base.add(p), 4);
        }
    }
    Some((mem, base as usize - mem as usize))
}

pub type JitFn = unsafe extern "C" fn(*mut Exec) -> c_int;

/* ---------------- the cache: shader -> code ---------------- */
struct Entry { shader: usize, mem: usize, entry: usize }
static LOCK: AtomicBool = AtomicBool::new(false);
static mut CACHE: Vec<Entry> = Vec::new();
fn lock() { while LOCK.compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() { core::hint::spin_loop(); } }
fn unlock() { LOCK.store(false, Ordering::Release); }

/// 1: compile shaders to machine code (THE WRLD OS turns it off where SSE2 is missing).
#[no_mangle]
pub static mut dxbc_jit_enabled: c_int = 1;

pub(crate) fn cpu_ok() -> bool {
    if X64 { return true; }
    #[cfg(target_arch = "x86")]
    #[allow(unused_unsafe)]
    unsafe { let r = core::arch::x86::__cpuid(1); return r.edx & (1 << 26) != 0 && r.edx & (1 << 24) != 0; }
    #[allow(unreachable_code)]
    false
}

/// The code for a shader (compiled on first use), or None.
pub unsafe fn get(s: *const Shader) -> Option<JitFn> {
    if core::ptr::read_volatile(core::ptr::addr_of!(dxbc_jit_enabled)) == 0 || s.is_null() || !cpu_ok() { return None; }
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    if let Some(e) = cache.iter().find(|e| e.shader == s as usize) { let f = e.entry; unlock(); return if f == 0 { None } else { Some(core::mem::transmute::<usize, JitFn>(f)) }; }
    unlock();
    let r = compile(&*s);
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    let (mem, entry) = match r { Some((m, off)) => (m as usize, m as usize + off), None => (0, 0) };
    cache.push(Entry { shader: s as usize, mem, entry });
    unlock();
    if entry == 0 { None } else { Some(core::mem::transmute::<usize, JitFn>(entry)) }
}
/// The shader is being freed: drop its code.
pub unsafe fn forget(s: *const Shader) {
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    if let Some(i) = cache.iter().position(|e| e.shader == s as usize) { let e = cache.swap_remove(i); if e.mem != 0 { jit_free(e.mem as *mut u8); } }
    unlock();
}

#[cfg(feature = "kernel")]
pub(crate) unsafe fn jit_alloc(n: usize) -> *mut u8 { extern "C" { fn kmalloc(n: usize) -> *mut c_void; } kmalloc(n) as *mut u8 }
#[cfg(feature = "kernel")]
pub(crate) unsafe fn jit_free(p: *mut u8) { extern "C" { fn kfree(p: *mut c_void); } kfree(p as *mut c_void) }
#[cfg(not(feature = "kernel"))]
pub(crate) unsafe fn jit_alloc(n: usize) -> *mut u8 {
    extern "C" { fn mmap(a: *mut c_void, l: usize, p: c_int, f: c_int, fd: c_int, o: i64) -> *mut c_void; }
    let p = mmap(core::ptr::null_mut(), n + 16, 7, 0x22, -1, 0);   /* RWX, private anonymous */
    if p as isize == -1 { return core::ptr::null_mut(); }
    *(p as *mut usize) = n + 16;
    (p as *mut u8).add(16)
}
#[cfg(not(feature = "kernel"))]
pub(crate) unsafe fn jit_free(p: *mut u8) {
    extern "C" { fn munmap(a: *mut c_void, l: usize) -> c_int; }
    let b = p.sub(16); munmap(b as *mut c_void, *(b as *const usize));
}

#[cfg(test)]
std::thread_local! { static HELPED: core::cell::RefCell<Vec<u16>> = core::cell::RefCell::new(Vec::new()); }
#[cfg(test)]
mod tests {
    use super::*;
    use crate::dxbc::{Env, Vec4};

    /// Run a compiled GLSL shader both ways on many inputs; the outputs must match bit for bit
    /// (NaNs may differ in their payload).
    fn same(stage: u8, src: &[u8], name: &str) -> f64 {
        let c = crate::glsl::compile(stage, src).unwrap_or_else(|e| panic!("{}: {}", name, e));
        let sh = unsafe { &*c.shader };
        let code = compile(sh).unwrap_or_else(|| panic!("{} does not compile to machine code", name));
        let f: JitFn = unsafe { core::mem::transmute(code.0 as usize + code.1) };
        let mut seed = 12345u32;
        let mut rnd = || { seed = seed.wrapping_mul(1103515245).wrapping_add(12345); ((seed >> 8) as f32 / 16777216.0) * 4.0 - 2.0 };
        let rows: Vec<Vec4> = (0..c.cb_rows.max(1)).map(|_| Vec4 { u: [rnd().to_bits(), rnd().to_bits(), rnd().to_bits(), rnd().to_bits()] }).collect();
        let mut env: Env = unsafe { core::mem::zeroed() };
        env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
        unsafe extern "C" fn smp(_c: *mut c_void, t: c_int, _s: c_int, uv: *const f32, _l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2) * 0.5 + t as f32 * 0.1 + i as f32 * 0.01; } }
        env.sample = Some(smp);
        let mut t_int = 0u128; let mut t_jit = 0u128;
        for _ in 0..300 {
            let mut inp = [Vec4::default(); 32];
            for r in inp.iter_mut() { for l in 0..4 { r.u[l] = rnd().to_bits(); } }
            let mut o1 = [Vec4::default(); 32]; let mut o2 = [Vec4::default(); 32];
            let mut e1: Exec = unsafe { core::mem::zeroed() }; let mut e2: Exec = unsafe { core::mem::zeroed() };
            unsafe {
                dxbc_exec_init(&mut e1, c.shader, &env, inp.as_ptr(), o1.as_mut_ptr());
                dxbc_exec_init(&mut e2, c.shader, &env, inp.as_ptr(), o2.as_mut_ptr());
                let t = std::time::Instant::now(); let r1 = crate::dxbc::interp_run(&mut e1); t_int += t.elapsed().as_nanos();
                let t = std::time::Instant::now(); let r2 = f(&mut e2); t_jit += t.elapsed().as_nanos();
                assert_eq!(r1 != 0, r2 != 0, "{}: discard differs", name);
                if r1 != 0 {
                    for k in 0..32 { for l in 0..4 {
                        let (a, b) = (o1[k].u[l], o2[k].u[l]);
                        let nan = |x: u32| x & 0x7FFF_FFFF > 0x7F80_0000;
                        assert!(a == b || (nan(a) && nan(b)), "{}: out[{}].{} interp {:08x} ({}) jit {:08x} ({})", name, k, l, a, f32::from_bits(a), b, f32::from_bits(b));
                    } }
                }
                dxbc_exec_fini(&mut e1); dxbc_exec_fini(&mut e2);
            }
        }
        t_int as f64 / t_jit.max(1) as f64
    }

    #[test]
    fn matches_the_interpreter() {
        let srcs: [(&str, u8, &str); 4] = [
            ("arith", 0, "uniform vec4 a, b; uniform mat3 m; varying vec4 v;
                void main() { vec4 x = a * v + b; x.xyz = m * x.xyz; float d = dot(x.xyz, v.xyz) + length(v);
                gl_FragColor = vec4(clamp(x.x, 0.0, 1.0), floor(x.y * 3.7), fract(x.z), min(d, max(a.w, b.w))) + inversesqrt(abs(v) + 0.1); }"),
            ("control", 0, "uniform vec4 a; varying vec4 v;
                int pick(float w) { if (w > 0.5) return 1; if (w < -0.5) return 2; return 0; }
                void main() { vec4 s = vec4(0.0); for (int i = 0; i < 6; i++) { if (float(i) > v.x * 4.0 + 2.0) break; s += a * float(i); if (i == 2) continue; s.y -= 0.5; }
                if (v.w < -1.5) discard; gl_FragColor = s + float(pick(v.z)) + mod(v, vec4(0.7)) + vec4(ivec4(v * 3.0)); }"),
            ("logic", 0, "uniform vec4 a; varying vec4 v; uniform float k[4];
                void main() { bvec4 c = lessThan(v, a); vec4 r = mix(v, a, vec4(c)); int i = int(abs(v.x) * 2.0); r.x += k[i];
                gl_FragColor = (v.x > a.x && v.y <= a.y) || v.z == a.z ? r : -r + vec4(sin(v.x), cos(v.y), exp2(v.z), sqrt(abs(v.w))); }"),
            ("vertex", 1, "uniform mat4 mvp; attribute vec4 p; attribute vec3 n; varying vec3 vn; varying vec2 uv;
                void main() { gl_Position = mvp * p; vn = normalize(gl_NormalMatrix * n); uv = p.xy * 0.5 + 0.5; gl_TexCoord[0] = gl_MultiTexCoord0 * 2.0; gl_FrontColor = gl_Color; }"),
        ];
        for (name, st, src) in srcs { same(st, src.as_bytes(), name); }
    }

    #[test]
    fn corpus_matches_and_speed() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            let st = match p.extension().and_then(|x| x.to_str()) { Some("vert") => 1, Some("frag") => 0, _ => continue };
            let x = same(st, &std::fs::read(&p).unwrap(), &p.display().to_string());
            let h = HELPED.with(|h| core::mem::take(&mut *h.borrow_mut()));
            println!("{}: machine code {:.1}x the interpreter; interpreted ops {:?}", p.file_name().unwrap().to_string_lossy(), x, h);
        }
    }
}
#[cfg(test)]
mod bench {
    use super::*;
    use crate::dxbc::{Env, Vec4};
    #[test]
    #[ignore]
    fn bench_loop() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            let st = match p.extension().and_then(|x| x.to_str()) { Some("vert") => 1, Some("frag") => 0, _ => continue };
            let c = crate::glsl::compile(st, &std::fs::read(&p).unwrap()).unwrap();
            let code = compile(unsafe { &*c.shader }).unwrap();
            let f: JitFn = unsafe { core::mem::transmute(code.0 as usize + code.1) };
            let rows = vec![Vec4 { u: [0.5f32.to_bits(); 4] }; c.cb_rows.max(1) as usize];
            let mut env: Env = unsafe { core::mem::zeroed() };
            env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
            unsafe extern "C" fn smp(_c: *mut c_void, _t: c_int, _s: c_int, uv: *const f32, _l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2); } }
            env.sample = Some(smp);
            let inp = [Vec4 { u: [0.3f32.to_bits(); 4] }; 32];
            let mut out = [Vec4::default(); 32];
            let mut ex: Exec = unsafe { core::mem::zeroed() };
            unsafe { dxbc_exec_init(&mut ex, c.shader, &env, inp.as_ptr(), out.as_mut_ptr()) };
            let n = 200_000;
            let t = std::time::Instant::now(); for _ in 0..n { unsafe { crate::dxbc::interp_run(&mut ex); } } let a = t.elapsed().as_nanos() as f64 / n as f64;
            let t = std::time::Instant::now(); for _ in 0..n { unsafe { f(&mut ex); } } let b = t.elapsed().as_nanos() as f64 / n as f64;
            println!("{}: {} instructions, interpreter {:.0} ns, machine code {:.0} ns ({:.1}x)", p.file_name().unwrap().to_string_lossy(), c.ninstr, a, b, a / b);
            unsafe { dxbc_exec_fini(&mut ex) };
        }
    }
}
