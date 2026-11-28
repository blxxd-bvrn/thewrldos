//! Four pixels at a time: a second machine-code compiler for the shader IR (shader_priv.h)
//! that runs one shader for four invocations at once, one in each SSE lane.
//!
//! jit.rs keeps the four components of a register in one SSE register, so most of an
//! instruction's work goes to swizzles and lanes nobody reads (a dot product is a
//! multiply, three shuffles and three scalar adds). Here every register is stored as
//! four component vectors of four pixels (64 bytes: x of pixels 0-3, y of pixels 0-3...),
//! so a swizzle is only a choice of address and every operation works on four pixels.
//!
//! The pixels may disagree at a branch, so code runs under an execution mask: an `if`
//! narrows it, `else` and `endif` restore it, `break`, `continue`, `ret` and `discard`
//! switch lanes off until the end of the loop or the shader. Inside branches and loops a
//! store keeps the old value in the lanes that are off. Texture sampling calls the
//! sampler once for each lane that is on. Instructions not compiled here run in the
//! interpreter, once for each lane that is on (`quad_step`), so every result has the same
//! bits as the interpreter's and jit.rs's.
//!
//! The same code runs in both editions, as in jit.rs, whose assembler it uses.

#![allow(clippy::missing_safety_doc)]

use crate::dxbc::*;
use crate::jit::*;
use alloc::vec::Vec;
use core::ffi::{c_int, c_void};
use core::mem::offset_of;

extern "C" {
    fn kmalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
}

/// One context for four invocations of one shader (`dxbc_quad_t` in C, opaque there).
#[repr(C, align(16))]
pub struct Quad {
    pub m: [u32; 4],                 // the execution mask
    pub z: [u32; 4],                 // lanes off until the loop ends or for good (break, continue, ret, discard)
    pub rr: [u32; 4],                // lanes that returned
    pub d: [u32; 4],                 // lanes that discarded
    pub live: [u32; 4],              // lanes that run at all
    pub tmp: [[u32; 4]; 4],          // results when a destination is also a source
    pub soa: [[u32; 4]; 4],          // a texture coordinate or texel, component vectors
    pub uv: [[u32; 4]; 4],           // the same per lane
    pub tex: [[u32; 4]; 4],
    pub lod: [u32; 4],
    pub stack: [[[u32; 4]; 3]; 64],  // per nesting level: saved mask, condition / saved z, break lanes
    pub r: *mut u8, pub inp: *const u8, pub out: *mut u8,
    pub env: *const Env, pub shader: *const Shader,
    pub cbp: [*const Vec4; 14],      // the constant buffers (every row the code reads is there)
    pub code: usize, pub mem: *mut c_void,
    pub sample4: usize,              // Sample4Fn, or 0: the sampler one lane at a time
    pub budget: i32,
    pub ex: Exec,                    // the interpreter's context for one lane (quad_step)
    pub ain: [Vec4; DX_MAX_REG], pub aout: [Vec4; DX_MAX_REG],
}

macro_rules! q { ($f:ident) => { offset_of!(Quad, $f) as i32 } }

/// Per instruction: its nesting depth, its stack slot and its jump target.
#[derive(Clone, Copy, Default)]
struct Ctl { depth: usize, slot: usize, target: usize }

struct G4<'a> { a: Asm, s: &'a Shader, pool: &'a [Opnd], ctl: Vec<Ctl>, seen_ret: bool }

const ODEPTH_REG: i32 = DX_MAX_REG as i32 - 1;

impl<'a> G4<'a> {
    fn ldq(&mut self, x: u8, off: i32) { self.a.movups_ld(x, M::B(EBX, off)); }
    fn stq(&mut self, off: i32, x: u8) { self.a.movups_st(M::B(EBX, off), x); }
    fn movmsk(&mut self, r: u8, x: u8) { self.a.byte(0x0F); self.a.byte(0x50); self.a.modreg(r, x); }
    fn mov_st(&mut self, m: M, r: u8) { self.a.byte(0x89); self.a.modrm(r, m, 0); }
    fn pand(&mut self, x: u8, y: u8) { self.a.sse_r(0x66, 0xDB, x, y); }
    fn pandn(&mut self, x: u8, y: u8) { self.a.sse_r(0x66, 0xDF, x, y); }   /* x = ~x & y */
    fn por(&mut self, x: u8, y: u8) { self.a.sse_r(0x66, 0xEB, x, y); }

    /// The register file and offset of a register operand's component vector c (swizzle
    /// not applied), for direct temporaries, inputs, outputs and the depth output.
    fn reg(&self, o: &Opnd, c: usize) -> Option<M> {
        let at = |b: u8, i: i32| Some(M::B(b, 64 * i + 16 * c as i32));
        match o.ty {
            T_TEMP if o.rel[0] < 0 && o.idx[0] >= 0 && o.idx[0] < self.s.ntemps.max(1) => at(ESI, o.idx[0]),
            T_INPUT if o.rel[0] < 0 && o.idx[0] >= 0 && o.idx[0] < DX_MAX_REG as i32 => at(EBP, o.idx[0]),
            T_OUTPUT if o.rel[0] < 0 && o.idx[0] >= 0 && o.idx[0] < DX_MAX_REG as i32 => at(EDI, o.idx[0]),
            T_ODEPTH => at(EDI, ODEPTH_REG),
            _ => None,
        }
    }
    fn ok_src(&self, k: i16) -> bool {
        let o = &self.pool[k as usize];
        match o.ty {
            T_IMM32 => true,
            T_CB => o.rel[0] < 0 && o.rel[1] < 0 && o.idx[0] >= 0 && o.idx[0] < 14 && (0..=65536).contains(&o.idx[1]),
            _ => self.reg(o, 0).is_some() && o.ty != T_ODEPTH,
        }
    }
    fn ok_dst(&self, k: i16) -> bool {
        let o = &self.pool[k as usize];
        o.ty == T_NULL || (matches!(o.ty, T_TEMP | T_OUTPUT | T_ODEPTH) && self.reg(o, 0).is_some())
    }
    /// xmm x = component c of a source operand (after its swizzle) for the four lanes, with
    /// its modifier.
    fn load(&mut self, k: i16, c: usize, x: u8, int: bool) {
        let o = self.pool[k as usize];
        let sw = (o.swz[c] & 3) as usize;
        match o.ty {
            T_IMM32 => {
                let mut e = o.imm[sw];
                if int { if o.md & 2 != 0 && (e as i32) < 0 { e = e.wrapping_neg(); } if o.md & 1 != 0 { e = e.wrapping_neg(); } }
                else { if o.md & 2 != 0 { e &= 0x7FFF_FFFF; } if o.md & 1 != 0 { e ^= 0x8000_0000; } }
                let kk = self.a.k4(e);
                self.a.movups_ld(x, M::K(kk));
                return;
            }
            T_CB => {
                /* dxbc_quad_new checked that the row exists */
                self.a.mov_ld_ptr(ECX, M::B(EBX, q!(cbp) + PS * o.idx[0]));
                self.a.sse_m(0xF3, 0x10, x, M::B(ECX, 16 * o.idx[1] + 4 * sw as i32));   /* movss */
                self.a.shufps(x, x, 0);
            }
            _ => { let m = self.reg(&o, sw).expect("checked operand"); self.a.movups_ld(x, m); }
        }
        if o.md != 0 {
            if int {
                if o.md & 2 != 0 { self.a.movaps(7, x); self.a.sse_ri(0x66, 0x72, 4, 7, 31); self.a.sse_r(0x66, 0xEF, x, 7); self.a.sse_r(0x66, 0xFA, x, 7); }
                if o.md & 1 != 0 { self.a.zero(7); self.a.sse_r(0x66, 0xFA, 7, x); self.a.movaps(x, 7); }
            } else {
                if o.md & 2 != 0 { let c = self.a.k4(0x7FFF_FFFF); self.a.sse_m(0, 0x54, x, M::K(c)); }
                if o.md & 1 != 0 { let c = self.a.k4(0x8000_0000); self.a.sse_m(0, 0x57, x, M::K(c)); }
            }
        }
    }
    /// Must a store keep the lanes that are off? Not at the top level before any `ret`:
    /// lanes off there are lanes that never ran or discarded, whose values nobody reads.
    fn blend(&self, pc: usize) -> bool { self.ctl[pc].depth > 0 || self.seen_ret }
    /// Store xmm x (clobbered) to component c of the destination.
    fn store(&mut self, k: i16, c: usize, x: u8, sat: bool, blend: bool) {
        let o = self.pool[k as usize];
        if o.ty == T_NULL { return; }
        if sat {
            let z = self.a.k4(0); let one = self.a.k4(0x3F80_0000);
            self.a.sse_m(0, 0x5F, x, M::K(z));
            self.a.sse_m(0, 0x5D, x, M::K(one));
        }
        let m = self.reg(&o, c).expect("checked operand");
        if blend {
            let x6 = if x == 6 { 5 } else { 6 };
            self.a.movups_ld(7, m);
            self.ldq(x6, q!(m));
            self.a.sse_r(0, 0x54, x, x6);
            self.a.sse_r(0, 0x55, x6, 7);
            self.a.sse_r(0, 0x56, x, x6);
        }
        self.a.movups_st(m, x);
    }
    /// Component-wise instruction: f emits component c into an xmm register it returns.
    fn each(&mut self, pc: usize, i: &Ins, sat: bool, f: &mut dyn FnMut(&mut G4<'a>, usize) -> u8) {
        let d = self.pool[i.o[0] as usize];
        if d.ty == T_NULL { return; }
        let mask = d.mask & 15;
        let blend = self.blend(pc);
        /* a destination that is also a source: results go through tmp until all are made */
        let n = i.nops as usize;
        let alias = mask.count_ones() > 1 && (1..n).any(|j| { let s = &self.pool[i.o[j] as usize]; s.ty == d.ty && s.idx[0] == d.idx[0] && matches!(s.ty, T_TEMP | T_OUTPUT) });
        for c in 0..4 {
            if mask & (1 << c) == 0 { continue; }
            let x = f(self, c);
            if alias { self.stq(q!(tmp) + 16 * c as i32, x); } else { self.store(i.o[0], c, x, sat, blend); }
        }
        if alias {
            for c in 0..4 { if mask & (1 << c) != 0 { self.ldq(0, q!(tmp) + 16 * c as i32); self.store(i.o[0], c, 0, sat, blend); } }
        }
    }
    /// The same value (xmm 0) to every component of the destination.
    fn splat(&mut self, pc: usize, i: &Ins, sat: bool) {
        let d = self.pool[i.o[0] as usize];
        if d.ty == T_NULL { return; }
        let blend = self.blend(pc);
        if sat { self.store_sat_prep(); }
        self.stq(q!(tmp), 0);
        for c in 0..4 { if d.mask & (1 << c) != 0 { self.ldq(0, q!(tmp)); self.store(i.o[0], c, 0, false, blend); } }
    }
    fn store_sat_prep(&mut self) {
        let z = self.a.k4(0); let one = self.a.k4(0x3F80_0000);
        self.a.sse_m(0, 0x5F, 0, M::K(z));
        self.a.sse_m(0, 0x5D, 0, M::K(one));
    }
    /// xmm 0 = lanes where (operand.x != 0) == nonzero
    fn cond(&mut self, k: i16, nonzero: u8) {
        self.load(k, 0, 0, true);
        self.a.zero(7);
        self.a.sse_r(0x66, 0x76, 0, 7);           /* == 0 */
        if nonzero != 0 { let c = self.a.k4(!0); self.a.sse_m(0x66, 0xEF, 0, M::K(c)); }
    }
    /// leave when every lane that runs has returned or discarded
    fn exit_if_done(&mut self) {
        self.ldq(0, q!(rr)); self.ldq(1, q!(d)); self.por(0, 1);
        self.ldq(1, q!(live)); self.pandn(0, 1);
        self.movmsk(EAX, 0); self.a.test(EAX);
        let n = self.s.nins as usize; self.a.jir(JE, n);
    }
    /// lanes x (xmm 0) off: into z (and into extra, a field or a stack word), out of m
    fn retire(&mut self, extra: Option<i32>) {
        if let Some(off) = extra { self.ldq(1, off); self.por(1, 0); self.stq(off, 1); }
        self.ldq(1, q!(z)); self.por(1, 0); self.stq(q!(z), 1);
        self.ldq(1, q!(m)); self.pandn(0, 1); self.stq(q!(m), 0);
    }
    fn slot(&self, s: usize, w: i32) -> i32 { q!(stack) + 48 * s as i32 + 16 * w }
    fn reload_files(&mut self) {
        if X64 { self.a.mov_ld_ptr(ESI, M::B(EBX, q!(r))); self.a.mov_ld_ptr(EDI, M::B(EBX, q!(out))); }
    }
    fn helper(&mut self, pc: usize) {
        #[cfg(test)] { let op = self.s.ins()[pc].op; HELPED4.with(|h| h.borrow_mut().push(op)); }
        let f = quad_step as *const () as usize;
        if X64 {
            self.a.byte(0x48); self.a.byte(0x89); self.a.byte(0xDF);                   /* mov rdi, rbx */
            self.a.byte(0xBE); self.a.d32(pc as i32);                                  /* mov esi, pc */
            self.a.byte(0x48); self.a.byte(0xB8); self.a.b.extend_from_slice(&(f as u64).to_le_bytes());
            self.a.byte(0xFF); self.a.byte(0xD0);
            self.reload_files();
        } else {
            self.a.byte(0x68); self.a.d32(pc as i32);
            self.a.byte(0x53);
            self.a.byte(0xB8); self.a.d32(f as i32);
            self.a.byte(0xFF); self.a.byte(0xD0);
            self.a.byte(0x83); self.a.byte(0xC4); self.a.byte(8);
        }
        self.a.cmp_imm(EAX, -1);
        let n = self.s.nins as usize;
        self.a.jir(JE, n + 1);
    }
    /// texture sampling: the coordinates per lane, the sampler called for each lane that is
    /// on, the texels back into component vectors through the texture's swizzle
    fn sample(&mut self, pc: usize, i: &Ins) {
        let o = i.o;
        let t = self.pool[o[2] as usize]; let sm = self.pool[o[3] as usize];
        for c in 0..4 { self.load(o[1], c, 0, false); self.stq(q!(soa) + 16 * c as i32, 0); }
        if i.op == OP_SAMPLE_L { self.load(o[4], 0, 1, false); } else { self.a.zero(1); }
        self.stq(q!(lod), 1);
        /* a sampler for four lanes: one call, coordinates and texels as component vectors
         * (texels into uv, which that path does not use otherwise) */
        if X64 { self.a.byte(0x48); }
        self.a.cmp_m_imm(M::B(EBX, q!(sample4)), 0);
        let one_by_one = self.a.jfwd(JE);
        self.ldq(0, q!(m)); self.movmsk(EAX, 0);
        let ts = (t.idx[0] & 0xFFFF) | (sm.idx[0] << 16);
        if X64 {
            self.a.byte(0x41); self.a.byte(0x89); self.a.byte(0xC0);                                        /* mov r8d, eax: lanes */
            self.a.mov_ld_ptr(EAX, M::B(EBX, q!(env)));
            self.a.mov_ld_ptr(EDI, M::B(EAX, off_env_ctx()));                                                 /* rdi = ctx */
            self.a.byte(0xBE); self.a.d32(ts);                                                                /* esi = texture | sampler << 16 */
            self.a.byte(0x48); self.a.byte(0x8D); self.a.modrm(EDX, M::B(EBX, q!(soa)), 0);                 /* lea rdx, coordinates */
            self.a.byte(0x48); self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(lod)), 0);                 /* lea rcx, lod */
            self.a.byte(0x4C); self.a.byte(0x8D); self.a.modrm(1, M::B(EBX, q!(uv)), 0);                    /* lea r9, texels */
            self.a.byte(0xFF); self.a.modrm(2, M::B(EBX, q!(sample4)), 0);                                    /* call [rbx+sample4] */
        } else {
            self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(uv)), 0); self.a.byte(0x51);                  /* texels */
            self.a.byte(0x50);                                                                                /* lanes */
            self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(lod)), 0); self.a.byte(0x51);
            self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(soa)), 0); self.a.byte(0x51);
            self.a.byte(0x68); self.a.d32(ts);
            self.a.mov_ld_ptr(EAX, M::B(EBX, q!(env)));
            self.a.byte(0xFF); self.a.modrm(6, M::B(EAX, off_env_ctx()), 0);
            self.a.byte(0xFF); self.a.modrm(2, M::B(EBX, q!(sample4)), 0);
            self.a.byte(0x83); self.a.byte(0xC4); self.a.byte(24);
        }
        self.reload_files();
        self.sample_store(pc, i, q!(uv));
        let done = self.a.jfwd(0);
        self.a.here(one_by_one);
        for l in 0..4 { for c in 0..4 {
            self.a.mov_ld(EAX, M::B(EBX, q!(soa) + 16 * c + 4 * l));
            self.mov_st(M::B(EBX, q!(uv) + 16 * l + 4 * c), EAX);
        } }
        self.a.zero(7);
        for l in 0..4 { self.stq(q!(tex) + 16 * l, 7); }
        self.a.mov_ld_ptr(EAX, M::B(EBX, q!(env)));
        self.a.byte(0x83); self.a.modrm(7, M::B(EAX, off_env_sample()), 1); self.a.byte(0);
        let skip = if X64 {
            let lo0 = self.a.jfwd(JNE);
            self.a.byte(0x83); self.a.modrm(7, M::B(EAX, off_env_sample() + 4), 1); self.a.byte(0);
            let sk = self.a.jfwd(JE);
            self.a.here(lo0);
            sk
        } else { self.a.jfwd(JE) };
        for l in 0..4i32 {
            self.a.cmp_m_imm(M::B(EBX, q!(m) + 4 * l), 0);
            let off = self.a.jfwd(JE);
            self.a.mov_ld_ptr(EAX, M::B(EBX, q!(env)));
            if X64 {
                self.a.mov_ld_ptr(EDI, M::B(EAX, off_env_ctx()));
                self.a.byte(0xBE); self.a.d32(t.idx[0]);
                self.a.byte(0xBA); self.a.d32(sm.idx[0]);
                self.a.byte(0x48); self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(uv) + 16 * l), 0);    /* lea rcx */
                self.a.sse_m(0xF3, 0x10, 0, M::B(EBX, q!(lod) + 4 * l));                                 /* movss xmm0 */
                self.a.byte(0x4C); self.a.byte(0x8D); self.a.modrm(0, M::B(EBX, q!(tex) + 16 * l), 0);     /* lea r8 */
                self.a.mov_ld_ptr(EAX, M::B(EAX, off_env_sample()));
                self.a.byte(0xFF); self.a.byte(0xD0);
            } else {
                self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(tex) + 16 * l), 0); self.a.byte(0x51);
                self.a.byte(0xFF); self.a.modrm(6, M::B(EBX, q!(lod) + 4 * l), 0);
                self.a.byte(0x8D); self.a.modrm(ECX, M::B(EBX, q!(uv) + 16 * l), 0); self.a.byte(0x51);
                self.a.byte(0x68); self.a.d32(sm.idx[0]);
                self.a.byte(0x68); self.a.d32(t.idx[0]);
                self.a.byte(0xFF); self.a.modrm(6, M::B(EAX, off_env_ctx()), 0);
                self.a.byte(0xFF); self.a.modrm(2, M::B(EAX, off_env_sample()), 0);
                self.a.byte(0x83); self.a.byte(0xC4); self.a.byte(24);
            }
            self.a.here(off);
        }
        self.reload_files();
        self.a.here(skip);
        for l in 0..4 { for c in 0..4 {
            self.a.mov_ld(EAX, M::B(EBX, q!(tex) + 16 * l + 4 * c));
            self.mov_st(M::B(EBX, q!(soa) + 16 * c + 4 * l), EAX);
        } }
        self.sample_store(pc, i, q!(soa));
        self.a.here(done);
    }
    /// the texels (component vectors at [ebx+at]) through the texture's swizzle to the destination
    fn sample_store(&mut self, pc: usize, i: &Ins, at: i32) {
        let o = i.o;
        let t = self.pool[o[2] as usize];
        let d = self.pool[o[0] as usize];
        if d.ty == T_NULL { return; }
        let blend = self.blend(pc);
        for c in 0..4 {
            if d.mask & (1 << c) == 0 { continue; }
            self.ldq(0, at + 16 * (t.swz[c] & 3) as i32);
            self.store(o[0], c, 0, i.sat != 0, blend);
        }
    }
    fn budget(&mut self) {
        self.a.byte(0xFF); self.a.modrm(1, M::B(EBX, q!(budget)), 0);
        let n = self.s.nins as usize;
        self.a.jir(JE, n + 1);
    }

    fn ins(&mut self, pc: usize, i: &Ins) {
        let o = i.o;
        let n = i.nops as usize;
        let mut nd = 0;
        let _ = operand_count(i.op, &mut nd);
        let all_ok = (0..n).all(|j| if (j as i32) < nd { self.ok_dst(o[j]) } else { self.ok_src(o[j]) });
        let sat = i.sat != 0;
        let ct = self.ctl[pc];
        macro_rules! bin { ($pre:expr, $op:expr, $int:expr) => {
            self.each(pc, i, sat && !$int, &mut |g, c| { g.load(o[1], c, 0, $int); g.load(o[2], c, 1, $int); g.a.sse_r($pre, $op, 0, 1); 0 })
        } }
        match i.op {
            OP_NOP => {}
            OP_IF => {
                self.cond(o[0], i.nonzero);
                self.ldq(1, q!(m));
                let (sm, st) = (self.slot(ct.slot, 0), self.slot(ct.slot, 1));
                self.stq(sm, 1); self.stq(st, 0);
                self.pand(1, 0); self.stq(q!(m), 1);
                self.movmsk(EAX, 1); self.a.test(EAX);
                self.a.jir(JE, ct.target);
            }
            OP_ELSE => {
                let (sm, st) = (self.slot(ct.slot, 0), self.slot(ct.slot, 1));
                self.ldq(0, sm); self.ldq(1, st); self.pandn(1, 0);
                self.ldq(2, q!(z)); self.pandn(2, 1); self.stq(q!(m), 2);
                self.movmsk(EAX, 2); self.a.test(EAX);
                self.a.jir(JE, ct.target);
            }
            OP_ENDIF => {
                let sm = self.slot(ct.slot, 0);
                self.ldq(0, sm); self.ldq(2, q!(z)); self.pandn(2, 0); self.stq(q!(m), 2);
            }
            OP_LOOP => {
                let (sm, sz, sb) = (self.slot(ct.slot, 0), self.slot(ct.slot, 1), self.slot(ct.slot, 2));
                self.ldq(0, q!(m)); self.stq(sm, 0);
                self.ldq(0, q!(z)); self.stq(sz, 0);
                self.a.zero(0); self.stq(sb, 0);
            }
            OP_ENDLOOP => {
                let (sm, sz, sb) = (self.slot(ct.slot, 0), self.slot(ct.slot, 1), self.slot(ct.slot, 2));
                self.ldq(1, q!(rr)); self.ldq(2, q!(d)); self.por(1, 2);         /* x1 = returned | discarded */
                self.ldq(3, sz); self.por(3, 1); self.stq(q!(z), 3);
                self.ldq(2, sb); self.por(2, 1);
                self.ldq(0, sm); self.pandn(2, 0); self.stq(q!(m), 2);           /* m = entry & ~(break | ret | discard) */
                self.movmsk(EAX, 2); self.a.test(EAX);
                let out = self.a.jfwd(JE);
                self.budget();
                self.a.jir(0, ct.target);
                self.a.here(out);
                self.ldq(0, sm); self.pandn(1, 0); self.stq(q!(m), 1);           /* after it: entry & ~(ret | discard) */
            }
            OP_BREAK | OP_CONTINUE => {
                self.ldq(0, q!(m));
                let extra = if i.op == OP_BREAK { Some(self.slot(ct.slot, 2)) } else { None };
                self.retire(extra);
            }
            OP_BREAKC | OP_CONTINUEC | OP_RETC | OP_DISCARD => {
                self.cond(o[0], i.nonzero);
                self.ldq(1, q!(m)); self.pand(0, 1);
                let extra = match i.op { OP_BREAKC => Some(self.slot(ct.slot, 2)), OP_RETC => Some(q!(rr)), OP_DISCARD => Some(q!(d)), _ => None };
                self.retire(extra);
                if matches!(i.op, OP_RETC | OP_DISCARD) { self.exit_if_done(); }
            }
            OP_RET => {
                self.ldq(0, q!(m));
                self.retire(Some(q!(rr)));
                self.exit_if_done();
            }
            OP_SAMPLE | OP_SAMPLE_L | OP_SAMPLE_B if self.ok_dst(o[0]) && self.ok_src(o[1]) && (i.op != OP_SAMPLE_L || self.ok_src(o[4]))
                && self.pool[o[2] as usize].ty == T_RESOURCE && self.pool[o[3] as usize].ty == T_SAMPLER => self.sample(pc, i),
            _ if !all_ok || i.op >= OP_BLOCK => self.helper(pc),
            OP_MOV => self.each(pc, i, sat, &mut |g, c| { g.load(o[1], c, 0, false); 0 }),
            OP_ADD => bin!(0, 0x58, false),
            OP_MUL => bin!(0, 0x59, false),
            OP_DIV => bin!(0, 0x5E, false),
            OP_MIN | OP_MAX => {
                let op = if i.op == OP_MIN { 0x5D } else { 0x5F };
                self.each(pc, i, sat, &mut |g, c| {
                    g.load(o[1], c, 0, false); g.load(o[2], c, 1, false);
                    g.a.movaps(2, 0); g.a.sse_r(0, op, 2, 1);
                    g.a.movaps(3, 1); g.a.sse_ri(0, 0xC2, 3, 1, 3);
                    g.a.sse_r(0, 0x54, 0, 3); g.a.sse_r(0, 0x55, 3, 2); g.a.sse_r(0, 0x56, 0, 3);
                    0
                })
            }
            OP_MAD => self.each(pc, i, sat, &mut |g, c| { g.load(o[1], c, 0, false); g.load(o[2], c, 1, false); g.load(o[3], c, 2, false); g.a.sse_r(0, 0x59, 0, 1); g.a.sse_r(0, 0x58, 0, 2); 0 }),
            OP_DP2 | OP_DP3 | OP_DP4 => {
                let k = match i.op { OP_DP2 => 2, OP_DP3 => 3, _ => 4 };
                self.load(o[1], 0, 0, false); self.load(o[2], 0, 1, false); self.a.sse_r(0, 0x59, 0, 1);
                for c in 1..k { self.load(o[1], c, 1, false); self.load(o[2], c, 2, false); self.a.sse_r(0, 0x59, 1, 2); self.a.sse_r(0, 0x58, 0, 1); }
                self.splat(pc, i, sat);
            }
            OP_EQ | OP_NE | OP_LT | OP_GE => {
                let op = i.op;
                self.each(pc, i, false, &mut |g, c| {
                    g.load(o[1], c, 0, false); g.load(o[2], c, 1, false);
                    match op {
                        OP_EQ => g.a.sse_ri(0, 0xC2, 0, 1, 0),
                        OP_NE => g.a.sse_ri(0, 0xC2, 0, 1, 4),
                        OP_LT => g.a.sse_ri(0, 0xC2, 0, 1, 1),
                        _ => { g.a.sse_ri(0, 0xC2, 1, 0, 2); g.a.movaps(0, 1); }
                    }
                    0
                })
            }
            OP_AND => bin!(0x66, 0xDB, true),
            OP_OR => bin!(0x66, 0xEB, true),
            OP_XOR => bin!(0x66, 0xEF, true),
            OP_IADD => bin!(0x66, 0xFE, true),
            OP_IEQ => bin!(0x66, 0x76, true),
            OP_NOT => self.each(pc, i, false, &mut |g, c| { g.load(o[1], c, 0, true); let k = g.a.k4(!0); g.a.sse_m(0x66, 0xEF, 0, M::K(k)); 0 }),
            OP_INEG => self.each(pc, i, false, &mut |g, c| { g.load(o[1], c, 1, true); g.a.zero(0); g.a.sse_r(0x66, 0xFA, 0, 1); 0 }),
            OP_INE => self.each(pc, i, false, &mut |g, c| { g.load(o[1], c, 0, true); g.load(o[2], c, 1, true); g.a.sse_r(0x66, 0x76, 0, 1); let k = g.a.k4(!0); g.a.sse_m(0x66, 0xEF, 0, M::K(k)); 0 }),
            OP_ILT | OP_IGE => {
                let ge = i.op == OP_IGE;
                self.each(pc, i, false, &mut |g, c| {
                    g.load(o[1], c, 0, true); g.load(o[2], c, 1, true);
                    g.a.sse_r(0x66, 0x66, 1, 0);
                    if ge { let k = g.a.k4(!0); g.a.sse_m(0x66, 0xEF, 1, M::K(k)); }
                    1
                })
            }
            OP_MOVC => self.each(pc, i, sat, &mut |g, c| {
                g.load(o[1], c, 0, true); g.load(o[2], c, 1, false); g.load(o[3], c, 2, false);
                g.a.zero(7); g.a.sse_r(0x66, 0x76, 0, 7);
                g.a.sse_r(0x66, 0xDB, 2, 0); g.a.sse_r(0x66, 0xDF, 0, 1); g.a.sse_r(0x66, 0xEB, 0, 2);
                0
            }),
            OP_ITOF => self.each(pc, i, sat, &mut |g, c| { g.load(o[1], c, 0, true); g.a.sse_r(0, 0x5B, 0, 0); 0 }),
            OP_FTOI => self.each(pc, i, false, &mut |g, c| {
                g.load(o[1], c, 0, false);
                g.a.movaps(1, 0);
                g.a.sse_r(0xF3, 0x5B, 2, 0);
                let big = g.a.k4(0x4F00_0000);
                g.a.sse_mi(0, 0xC2, 1, M::K(big), 5);
                let max = g.a.k4(0x7FFF_FFFF);
                g.a.movups_ld(3, M::K(max));
                g.a.sse_r(0x66, 0xDB, 3, 1); g.a.sse_r(0x66, 0xDF, 1, 2); g.a.sse_r(0x66, 0xEB, 1, 3);
                g.a.movaps(2, 0); g.a.sse_ri(0, 0xC2, 2, 0, 7);
                g.a.sse_r(0x66, 0xDB, 1, 2);
                1
            }),
            OP_SQRT => self.each(pc, i, sat, &mut |g, c| { g.load(o[1], c, 0, false); g.a.sse_r(0, 0x51, 0, 0); 0 }),
            OP_RSQ | OP_RCP => {
                let rsq = i.op == OP_RSQ;
                self.each(pc, i, sat, &mut |g, c| {
                    g.load(o[1], c, 1, false);
                    if rsq { g.a.sse_r(0, 0x51, 1, 1); }
                    let one = g.a.k4(0x3F80_0000);
                    g.a.movups_ld(0, M::K(one));
                    g.a.sse_r(0, 0x5E, 0, 1);
                    0
                })
            }
            OP_ROUND_NI | OP_FRC | OP_ROUND_Z | OP_ROUND_PI => {
                let op = i.op;
                self.each(pc, i, sat, &mut |g, c| {
                    g.load(o[1], c, 0, false);
                    if op == OP_ROUND_PI { let k = g.a.k4(0x8000_0000); g.a.sse_m(0, 0x57, 0, M::K(k)); }
                    g.a.sse_r(0xF3, 0x5B, 1, 0);
                    g.a.sse_r(0, 0x5B, 1, 1);
                    if op != OP_ROUND_Z {
                        g.a.movaps(2, 0); g.a.sse_ri(0, 0xC2, 2, 1, 1);
                        let one = g.a.k4(0x3F80_0000);
                        g.a.sse_m(0, 0x54, 2, M::K(one));
                        g.a.sse_r(0, 0x5C, 1, 2);
                    }
                    g.a.movaps(3, 0);
                    let abs = g.a.k4(0x7FFF_FFFF); g.a.sse_m(0, 0x54, 3, M::K(abs));
                    let big = g.a.k4(0x4B00_0000); g.a.sse_mi(0, 0xC2, 3, M::K(big), 5);
                    g.a.movaps(2, 0); g.a.sse_r(0, 0x54, 2, 3);
                    g.a.sse_r(0, 0x55, 3, 1);
                    g.a.sse_r(0, 0x56, 3, 2);
                    let sg = g.a.k4(0x8000_0000);
                    g.a.movaps(2, 0); g.a.sse_m(0, 0x54, 2, M::K(sg)); g.a.sse_r(0, 0x56, 3, 2);
                    match op {
                        OP_FRC => { g.a.sse_r(0, 0x5C, 0, 3); 0 }
                        OP_ROUND_PI => { let k = g.a.k4(0x8000_0000); g.a.sse_m(0, 0x57, 3, M::K(k)); 3 }
                        _ => 3,
                    }
                })
            }
            _ => self.helper(pc),
        }
        if matches!(i.op, OP_RET | OP_RETC) { self.seen_ret = true; }
    }
}

/// Nesting, stack slots and targets for the structured control flow; None if the shader
/// has control flow this compiler does not handle.
fn control(s: &Shader) -> Option<Vec<Ctl>> {
    let ins = s.ins();
    let mut ctl = alloc::vec![Ctl::default(); ins.len()];
    let mut st: Vec<usize> = Vec::new();
    for (pc, i) in ins.iter().enumerate() {
        ctl[pc].depth = st.len();
        match i.op {
            OP_IF | OP_LOOP => { if st.len() >= 64 { return None; } ctl[pc].slot = st.len(); st.push(pc); }
            OP_ELSE => {
                let t = *st.last()?; if ins[t].op != OP_IF { return None; }
                ctl[t].target = pc; ctl[pc].slot = ctl[t].slot; *st.last_mut()? = pc;
                ctl[pc].depth = st.len();
            }
            OP_ENDIF => {
                let t = st.pop()?; if !matches!(ins[t].op, OP_IF | OP_ELSE) { return None; }
                ctl[t].target = pc; ctl[pc].slot = ctl[t].slot;
                ctl[pc].depth = st.len();
            }
            OP_ENDLOOP => {
                let t = st.pop()?; if ins[t].op != OP_LOOP { return None; }
                ctl[pc].slot = ctl[t].slot; ctl[pc].target = t + 1;
                ctl[pc].depth = st.len() + 1;     /* still inside: its stores (none) and checks */
            }
            OP_BREAK | OP_BREAKC | OP_CONTINUE | OP_CONTINUEC => {
                let l = *st.iter().rev().find(|&&p| ins[p].op == OP_LOOP)?;
                ctl[pc].slot = ctl[l].slot;
            }
            OP_SWITCH | OP_CASE | OP_DEFAULT | OP_ENDSWITCH | OP_CALL | OP_CALLC | OP_LABEL => return None,
            _ => {}
        }
    }
    if !st.is_empty() { return None; }
    Some(ctl)
}

/// Machine code for four invocations of a shader, or None (it then runs one at a time).
pub fn compile(s: &Shader) -> Option<(*mut u8, usize)> {
    if s.xt_total != 0 || s.nins <= 0 || s.dxil != 0 { return None; }
    let ins = s.ins();
    if ins.iter().any(|i| i.op >= OP_BLOCK) { return None; }
    let ctl = control(s)?;
    let pool = s.pool();
    let mut g = G4 { a: Asm { b: Vec::new(), consts: Vec::new(), cfix: Vec::new(), label: Vec::new(), jfix: Vec::new() }, s, pool, ctl, seen_ret: false };
    /* registers must be addressed directly (the helper copies whole registers per lane),
     * and conditions read here */
    for i in ins {
        for j in 0..i.nops as usize {
            let o = &pool[i.o[j] as usize];
            if matches!(o.ty, T_TEMP | T_INPUT | T_OUTPUT) && g.reg(o, 0).is_none() { return None; }
            for r in o.rel { if r >= 0 { let x = &pool[r as usize]; if matches!(x.ty, T_TEMP | T_INPUT | T_OUTPUT) && g.reg(x, 0).is_none() { return None; } } }
        }
        if matches!(i.op, OP_IF | OP_BREAKC | OP_CONTINUEC | OP_RETC | OP_DISCARD) && !g.ok_src(i.o[0]) { return None; }
    }
    let n = ins.len();
    for r in [EBX, EBP, ESI, EDI] { g.a.byte(0x50 + r); }
    if X64 { g.a.byte(0x48); g.a.byte(0x83); g.a.byte(0xEC); g.a.byte(40); g.a.byte(0x48); g.a.byte(0x89); g.a.byte(0xFB); }
    else { g.a.mov_ld(EBX, M::B(ESP, 20)); g.a.byte(0x83); g.a.byte(0xEC); g.a.byte(32); }
    g.a.mov_ld_ptr(ESI, M::B(EBX, q!(r)));
    g.a.mov_ld_ptr(EBP, M::B(EBX, q!(inp)));
    g.a.mov_ld_ptr(EDI, M::B(EBX, q!(out)));
    if s.nclear > 0 { g.a.zero(0); for k in 0..s.nclear as usize { for c in 0..4 { g.a.movups_st(M::B(EDI, 64 * s.clear_regs[k] as i32 + 16 * c), 0); } } }
    for (pc, i) in ins.iter().enumerate() { g.a.label.push(g.a.b.len()); g.ins(pc, i); }
    link(g.a, n)
}

/// One instruction the compiler left to the interpreter, for each lane that is on: the
/// lane's registers it names go to the one-lane context, the instruction runs there, and
/// what it wrote comes back. 0, or -1 when the interpreter failed.
unsafe extern "C" fn quad_step(q: *mut Quad, pc: c_int) -> c_int {
    let q = &mut *q;
    let s = &*q.shader;
    let ins = s.ins()[pc as usize];
    let pool = s.pool();
    let mut regs: [(u8, i32); 16] = [(0, 0); 16];
    let mut nr = 0;
    let mut add = |o: &Opnd| {
        let (ty, idx) = match o.ty { T_TEMP | T_INPUT | T_OUTPUT => (o.ty, o.idx[0]), T_ODEPTH => (T_OUTPUT, ODEPTH_REG), _ => return };
        if ty == T_TEMP && (idx < 0 || idx >= s.ntemps.max(1)) { return; }
        if !(0..DX_MAX_REG as i32).contains(&idx) && ty != T_TEMP { return; }
        if nr < 16 && !regs[..nr].contains(&(ty, idx)) { regs[nr] = (ty, idx); nr += 1; }
    };
    for j in 0..ins.nops as usize {
        let o = &pool[ins.o[j] as usize];
        add(o);
        for r in o.rel { if r >= 0 { add(&pool[r as usize]); } }
    }
    for l in 0..4 {
        if q.m[l] == 0 { continue; }
        for &(ty, idx) in &regs[..nr] {
            let (soa, aos): (*const u32, *mut Vec4) = match ty {
                T_TEMP => (q.r.add(64 * idx as usize) as *const u32, q.ex.r.add(idx as usize)),
                T_INPUT => (q.inp.add(64 * idx as usize) as *const u32, &mut q.ain[idx as usize]),
                _ => (q.out.add(64 * idx as usize) as *const u32, &mut q.aout[idx as usize]),
            };
            for c in 0..4 { (*aos).u[c] = *soa.add(4 * c + l); }
        }
        if dxbc_exec_step(&mut q.ex, pc) < 0 { return -1; }
        for &(ty, idx) in &regs[..nr] {
            let (soa, aos): (*mut u32, *const Vec4) = match ty {
                T_TEMP => (q.r.add(64 * idx as usize) as *mut u32, q.ex.r.add(idx as usize)),
                T_INPUT => continue,
                _ => (q.out.add(64 * idx as usize) as *mut u32, &q.aout[idx as usize]),
            };
            for c in 0..4 { *soa.add(4 * c + l) = (*aos).u[c]; }
        }
    }
    0
}

pub type Jit4Fn = unsafe extern "C" fn(*mut Quad) -> c_int;

/* ---------------- the cache: shader -> code ---------------- */
struct Entry { shader: usize, mem: usize, entry: usize }
static mut CACHE: Vec<Entry> = Vec::new();
static LOCK: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);
fn lock() { while LOCK.compare_exchange(false, true, core::sync::atomic::Ordering::Acquire, core::sync::atomic::Ordering::Relaxed).is_err() { core::hint::spin_loop(); } }
fn unlock() { LOCK.store(false, core::sync::atomic::Ordering::Release); }

unsafe fn get(s: *const Shader) -> usize {
    if core::ptr::read_volatile(core::ptr::addr_of!(dxbc_jit_enabled)) == 0 || s.is_null() || !cpu_ok() { return 0; }
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    if let Some(e) = cache.iter().find(|e| e.shader == s as usize) { let f = e.entry; unlock(); return f; }
    unlock();
    let r = compile(&*s);
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    let (mem, entry) = match r { Some((m, off)) => (m as usize, m as usize + off), None => (0, 0) };
    cache.push(Entry { shader: s as usize, mem, entry });
    unlock();
    entry
}
pub unsafe fn forget(s: *const Shader) {
    lock();
    let cache = &mut *core::ptr::addr_of_mut!(CACHE);
    if let Some(i) = cache.iter().position(|e| e.shader == s as usize) { let e = cache.swap_remove(i); if e.mem != 0 { jit_free(e.mem as *mut u8); } }
    unlock();
}

/* ---------------- the C interface ---------------- */

/// A context for four invocations of `s` reading `inp` and writing `out` (DX_MAX_REG
/// registers each, as component vectors: register r, component c, lane l at float
/// 16 * r + 4 * c + l); NULL when the shader does not compile this way.
#[no_mangle]
pub unsafe extern "C" fn dxbc_quad_new(s: *const Shader, env: *const Env, inp: *const f32, out: *mut f32) -> *mut Quad {
    if s.is_null() || env.is_null() || inp.is_null() || out.is_null() { return core::ptr::null_mut(); }
    let code = get(s);
    if code == 0 { return core::ptr::null_mut(); }
    /* the code reads constants without checking: every row it names must exist */
    let e = &*env;
    for o in (*s).pool() {
        if o.ty == T_CB && o.rel[0] < 0 && o.rel[1] < 0 {
            let b = o.idx[0];
            if !(0..14).contains(&b) || e.cb[b as usize].is_null() || o.idx[1] < 0 || o.idx[1] as u32 >= e.cb_rows[b as usize] { return core::ptr::null_mut(); }
        }
    }
    let nt = (*s).ntemps.max(1) as usize;
    let size = core::mem::size_of::<Quad>() + 64 * nt + 32;
    let mem = kmalloc(size);
    if mem.is_null() { return core::ptr::null_mut(); }
    core::ptr::write_bytes(mem as *mut u8, 0, size);
    let q = ((mem as usize + 15) & !15) as *mut Quad;
    let qq = &mut *q;
    qq.cbp = e.cb;
    qq.mem = mem; qq.code = code; qq.shader = s; qq.env = env;
    qq.inp = inp as *const u8; qq.out = out as *mut u8;
    qq.r = (q as *mut u8).add(core::mem::size_of::<Quad>());
    if dxbc_exec_init(&mut qq.ex, s, env, qq.ain.as_ptr(), qq.aout.as_mut_ptr()) == 0 { kfree(mem); return core::ptr::null_mut(); }
    q
}
/// `sample4(ctx, texture | sampler << 16, uv, lod, lanes, out)`: uv and out are four
/// component vectors of four lanes (16 floats), lod four floats, lanes a bit per lane that
/// needs its texel (the others may be left as they are).
pub type Sample4Fn = unsafe extern "C" fn(*mut c_void, c_int, *const f32, *const f32, u32, *mut f32);
/// Give the context a sampler for four lanes at once (it must give each lane the texel the
/// one-lane sampler of the environment would).
#[no_mangle]
pub unsafe extern "C" fn dxbc_quad_sample4(q: *mut Quad, f: Option<Sample4Fn>) {
    if !q.is_null() { (*q).sample4 = f.map_or(0, |f| f as usize); }
}
#[no_mangle]
pub unsafe extern "C" fn dxbc_quad_free(q: *mut Quad) {
    if q.is_null() { return; }
    dxbc_exec_fini(&mut (*q).ex);
    kfree((*q).mem);
}
/// Run the lanes whose bits are set in `live`; the bits of those that did not discard (0
/// for all if the run failed).
#[no_mangle]
pub unsafe extern "C" fn dxbc_quad_run(q: *mut Quad, live: u32) -> u32 {
    let qq = &mut *q;
    for l in 0..4 { let v = if live & (1 << l) != 0 { !0 } else { 0 }; qq.live[l] = v; qq.m[l] = v; qq.z[l] = 0; qq.rr[l] = 0; qq.d[l] = 0; }
    qq.budget = BUDGET;
    qq.ex.discard = 0; qq.ex.block = -1; qq.ex.previous_block = -1;
    let f: Jit4Fn = core::mem::transmute::<usize, Jit4Fn>(qq.code);
    if f(q) == 0 { return 0; }
    let mut bits = 0;
    for l in 0..4 { if qq.live[l] != 0 && qq.d[l] == 0 { bits |= 1 << l; } }
    bits
}

#[cfg(test)]
std::thread_local! { static HELPED4: core::cell::RefCell<Vec<u16>> = core::cell::RefCell::new(Vec::new()); }
#[cfg(test)]
mod tests {
    use super::*;

    /// Four lanes at a time against the interpreter one at a time, lane masks and inputs at
    /// random: every output of every surviving lane, and which lanes discard, must agree.
    fn same4(stage: u8, src: &[u8], name: &str) -> (f64, Vec<u16>) {
        let c = crate::glsl::compile(stage, src).unwrap_or_else(|e| panic!("{}: {}", name, e));
        let sh = c.shader;
        let mut seed = 777u32;
        let mut rnd = || { seed = seed.wrapping_mul(1103515245).wrapping_add(12345); ((seed >> 8) as f32 / 16777216.0) * 4.0 - 2.0 };
        let rows: Vec<Vec4> = (0..c.cb_rows.max(1)).map(|_| Vec4 { u: [rnd().to_bits(), rnd().to_bits(), rnd().to_bits(), rnd().to_bits()] }).collect();
        let mut env: Env = unsafe { core::mem::zeroed() };
        env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
        unsafe extern "C" fn smp(_c: *mut c_void, t: c_int, _s: c_int, uv: *const f32, l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2) * 0.5 + t as f32 * 0.1 + i as f32 * 0.01 + l; } }
        env.sample = Some(smp);
        let mut inp4 = alloc::vec![0f32; DX_MAX_REG * 16];
        let mut out4 = alloc::vec![0f32; DX_MAX_REG * 16];
        let q = unsafe { dxbc_quad_new(sh, &env, inp4.as_ptr(), out4.as_mut_ptr()) };
        assert!(!q.is_null(), "{} does not compile for four lanes", name);
        /* the four-lane sampler on odd rounds: the same texels, from one call */
        unsafe extern "C" fn smp4(_c: *mut c_void, ts: c_int, uv: *const f32, lod: *const f32, lanes: u32, o: *mut f32) {
            for l in 0..4 { if lanes & (1 << l) != 0 {
                let u = [*uv.add(l), *uv.add(4 + l), *uv.add(8 + l), *uv.add(12 + l)];
                let mut t = [0f32; 4];
                smp(core::ptr::null_mut(), ts & 0xFFFF, ts >> 16, u.as_ptr(), *lod.add(l), t.as_mut_ptr());
                for c in 0..4 { *o.add(4 * c + l) = t[c]; }
            } }
        }
        let (mut t1, mut t4) = (0u128, 0u128);
        for round in 0..400 {
            let live: u32 = if round % 5 == 0 { 15 } else { (rnd().to_bits() >> 7) & 15 };
            let mut inp = [[Vec4::default(); 32]; 4];
            for l in 0..4 { for r in 0..32 { for cc in 0..4 {
                let v = if round % 3 == 0 && l > 0 { inp[0][r].u[cc] } else { rnd().to_bits() };   /* lanes that agree, too */
                inp[l][r].u[cc] = v; inp4[16 * r + 4 * cc + l] = f32::from_bits(v);
            } } }
            let mut outs = [[Vec4::default(); 32]; 4];
            let mut alive = [false; 4];
            let t = std::time::Instant::now();
            for l in 0..4 {
                if live & (1 << l) == 0 { continue; }
                let mut e: Exec = unsafe { core::mem::zeroed() };
                unsafe {
                    dxbc_exec_init(&mut e, sh, &env, inp[l].as_ptr(), outs[l].as_mut_ptr());
                    alive[l] = crate::dxbc::interp_run(&mut e) != 0;
                    dxbc_exec_fini(&mut e);
                }
            }
            t1 += t.elapsed().as_nanos();
            unsafe { dxbc_quad_sample4(q, if round & 1 != 0 { Some(smp4) } else { None }) };
            let t = std::time::Instant::now();
            let bits = unsafe { dxbc_quad_run(q, live) };
            t4 += t.elapsed().as_nanos();
            for l in 0..4 {
                if live & (1 << l) == 0 { continue; }
                assert_eq!(alive[l], bits & (1 << l) != 0, "{}: lane {} discard differs (round {})", name, l, round);
                if !alive[l] { continue; }
                for k in 0..32 { for cc in 0..4 {
                    let (a, b) = (outs[l][k].u[cc], out4[16 * k + 4 * cc + l].to_bits());
                    let nan = |x: u32| x & 0x7FFF_FFFF > 0x7F80_0000;
                    assert!(a == b || (nan(a) && nan(b)), "{}: lane {} out[{}].{} interp {:08x} ({}) quad {:08x} ({}) round {}", name, l, k, cc, a, f32::from_bits(a), b, f32::from_bits(b), round);
                } }
            }
        }
        unsafe { dxbc_quad_free(q) };
        (t1 as f64 / t4.max(1) as f64, HELPED4.with(|h| core::mem::take(&mut *h.borrow_mut())))
    }

    #[test]
    fn four_lanes_match_the_interpreter() {
        let srcs: [(&str, u8, &str); 6] = [
            ("arith", 0, "uniform vec4 a, b; uniform mat3 m; varying vec4 v;
                void main() { vec4 x = a * v + b; x.xyz = m * x.xyz; float d = dot(x.xyz, v.xyz) + length(v);
                gl_FragColor = vec4(clamp(x.x, 0.0, 1.0), floor(x.y * 3.7), fract(x.z), min(d, max(a.w, b.w))) + inversesqrt(abs(v) + 0.1); }"),
            ("swizzle", 0, "varying vec4 v; uniform vec4 a;
                void main() { vec4 t = v; t.xy = t.yx * 2.0; t.zw += t.xy; t = t.wzyx - a.xxyy; gl_FragColor = t + vec4(ceil(v.x), trunc(v.y), v.z / (v.w + 3.0), pow(abs(v.x), 2.5)); }"),
            ("control", 0, "uniform vec4 a; varying vec4 v;
                int pick(float w) { if (w > 0.5) return 1; if (w < -0.5) return 2; return 0; }
                void main() { vec4 s = vec4(0.0); for (int i = 0; i < 6; i++) { if (float(i) > v.x * 4.0 + 2.0) break; s += a * float(i); if (i == 2) continue; s.y -= 0.5; }
                if (v.w < -1.5) discard; gl_FragColor = s + float(pick(v.z)) + mod(v, vec4(0.7)) + vec4(ivec4(v * 3.0)); }"),
            ("nested", 0, "varying vec4 v; uniform vec4 a;
                void main() { vec4 c = vec4(0.0);
                if (v.x > 0.0) { if (v.y > 0.0) c.x = 1.0; else { c.y = 2.0; if (v.z < 0.0) discard; } } else c.z = v.w;
                float k = 0.0; for (int i = 0; i < 8; i++) { k += v.y; if (k > 1.0) { c.w = float(i); break; } }
                int j = 0; while (j < 5) { j++; if (v.z > float(j) * 0.3) continue; c.x += 0.25; }
                if (v.w > 1.5) { gl_FragColor = vec4(9.0); return; }
                gl_FragColor = c + a; }"),
            ("logic", 0, "uniform vec4 a; varying vec4 v; uniform float k[4];
                void main() { bvec4 c = lessThan(v, a); vec4 r = mix(v, a, vec4(c)); int i = int(abs(v.x) * 2.0); r.x += k[i];
                gl_FragColor = (v.x > a.x && v.y <= a.y) || v.z == a.z ? r : -r + vec4(sin(v.x), cos(v.y), exp2(v.z), sqrt(abs(v.w))); }"),
            ("texture", 0, "uniform sampler2D t0, t1; varying vec4 v;
                void main() { vec4 c = texture2D(t0, v.xy); if (c.x > 0.6) c = texture2D(t1, v.zw).bgra; else c.xy = texture2D(t0, v.yx).wz;
                if (c.y < -0.4) discard; gl_FragColor = c; }"),
        ];
        for (name, st, src) in srcs { same4(st, src.as_bytes(), name); }
    }

    #[test]
    fn corpus_four_lanes() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            let st = match p.extension().and_then(|x| x.to_str()) { Some("vert") => 1, Some("frag") => 0, _ => continue };
            let (x, h) = same4(st, &std::fs::read(&p).unwrap(), &p.display().to_string());
            println!("{}: four lanes {:.1}x the interpreter one at a time; per-lane ops {:?}", p.file_name().unwrap().to_string_lossy(), x, h);
        }
    }

    /// cargo test --release --lib jit4::tests::bench4 -- --ignored --nocapture
    #[test]
    #[ignore]
    fn bench4() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) != Some("frag") { continue; }
            let c = crate::glsl::compile(0, &std::fs::read(&p).unwrap()).unwrap();
            let rows = alloc::vec![Vec4 { u: [0.5f32.to_bits(); 4] }; c.cb_rows.max(1) as usize];
            let mut env: Env = unsafe { core::mem::zeroed() };
            env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
            unsafe extern "C" fn smp(_c: *mut c_void, _t: c_int, _s: c_int, uv: *const f32, _l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2); } }
            env.sample = Some(smp);
            let inp = [Vec4 { u: [0.3f32.to_bits(); 4] }; 32];
            let mut out = [Vec4::default(); 32];
            let mut ex: Exec = unsafe { core::mem::zeroed() };
            unsafe { dxbc_exec_init(&mut ex, c.shader, &env, inp.as_ptr(), out.as_mut_ptr()) };
            let inp4 = alloc::vec![0.3f32; DX_MAX_REG * 16];
            let mut out4 = alloc::vec![0f32; DX_MAX_REG * 16];
            let q = unsafe { dxbc_quad_new(c.shader, &env, inp4.as_ptr(), out4.as_mut_ptr()) };
            let n = 200_000;
            let t = std::time::Instant::now(); for _ in 0..n { unsafe { dxbc_exec_run(&mut ex); } } let a = t.elapsed().as_nanos() as f64 / n as f64;
            let t = std::time::Instant::now(); for _ in 0..n / 4 { unsafe { dxbc_quad_run(q, 15); } } let b = t.elapsed().as_nanos() as f64 / n as f64;
            println!("{}: one at a time {:.0} ns a pixel, four at a time {:.0} ns a pixel ({:.1}x)", p.file_name().unwrap().to_string_lossy(), a, b, a / b);
            unsafe { dxbc_quad_free(q); dxbc_exec_fini(&mut ex) };
        }
    }
}
