//! Work that is the same for every pixel (or vertex) of a draw, done once per draw.
//!
//! Shaders often compute from uniforms alone: a light direction normalised, material
//! colours added up, a branch on a uniform flag. `dxbc_hoist` finds those instructions
//! and splits the shader in two:
//!
//! * a pre-shader that runs once per draw (`dxbc_hoist_run`) and records, as rows of a
//!   constant buffer, the value each such register has where the rest of the shader reads it;
//! * the main shader, where those instructions are gone and the reads come from the rows.
//!
//! A temporary register counts as uniform when every write to it is an instruction whose
//! sources are uniform (constants, immediates, uniform temporaries) and that runs on a path
//! all invocations share: not inside a branch on a non-uniform condition, nor in a loop that
//! some invocations leave early. A value read inside such a branch is recorded where the
//! branch starts (nothing in it writes the register). A register written inside a loop and
//! read there changes from one iteration to the next, so it is not hoisted. Texture reads,
//! derivatives and anything reading inputs stay in the main shader. The results are the
//! same bits as the original shader's.

#![allow(clippy::missing_safety_doc)]

use crate::dxbc::*;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void};
use crate::CBool;

extern "C" {
    fn kzalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
}

fn pure_op(op: u16) -> bool {
    !matches!(op, OP_SAMPLE | OP_SAMPLE_C | OP_SAMPLE_C_LZ | OP_SAMPLE_L | OP_SAMPLE_D | OP_SAMPLE_B | OP_LD | OP_LD_MS | OP_RESINFO
        | OP_DERIV_RTX | OP_DERIV_RTY | OP_EMIT | OP_EMITTHENCUT | OP_CUT | OP_CUSTOMDATA)
        && op < OP_BLOCK && !(OP_DCL_FIRST..=OP_DCL_LAST).contains(&op)
}
fn control(op: u16) -> bool {
    matches!(op, OP_IF | OP_ELSE | OP_ENDIF | OP_LOOP | OP_ENDLOOP | OP_BREAK | OP_BREAKC | OP_CONTINUE | OP_CONTINUEC | OP_RET | OP_RETC | OP_DISCARD | OP_NOP)
}

struct An<'a> { ins: &'a [Ins], pool: &'a [Opnd], nd: Vec<usize>, end: Vec<usize> }

impl<'a> An<'a> {
    fn uni(&self, k: i16, u: &[bool]) -> bool {
        let o = &self.pool[k as usize];
        match o.ty {
            T_IMM32 => true,
            T_CB => o.rel[0] < 0 && o.rel[1] < 0,
            T_TEMP => o.rel[0] < 0 && o.idx[0] >= 0 && (o.idx[0] as usize) < u.len() && u[o.idx[0] as usize],
            _ => false,
        }
    }
    fn srcs(&self, pc: usize) -> core::ops::Range<usize> { self.nd[pc]..self.ins[pc].nops as usize }
    /// Loops some invocations may leave early: a break or continue of their own that is
    /// conditional on something non-uniform.
    fn loop_divergent(&self, s: usize, u: &[bool]) -> bool {
        let mut nu_ifs: Vec<bool> = Vec::new();
        let mut pc = s + 1;
        while pc < self.end[s] {
            let i = &self.ins[pc];
            match i.op {
                OP_LOOP => { pc = self.end[pc]; continue; }
                OP_IF => nu_ifs.push(!self.uni(i.o[0], u)),
                OP_ENDIF => { nu_ifs.pop(); }
                OP_BREAK | OP_CONTINUE => if nu_ifs.iter().any(|&b| b) { return true; },
                OP_BREAKC | OP_CONTINUEC => if nu_ifs.iter().any(|&b| b) || !self.uni(i.o[0], u) { return true; },
                _ => {}
            }
            pc += 1;
        }
        false
    }
    /// For each instruction: the start of the outermost divergent region around it, and
    /// whether it computes a uniform value.
    fn classify(&self, u: &[bool]) -> (Vec<Option<usize>>, Vec<bool>) {
        let n = self.ins.len();
        let mut region = alloc::vec![None; n];
        let mut uni = alloc::vec![false; n];
        let mut st: Vec<bool> = Vec::new();        // per open IF/LOOP: did it start the divergent region
        let mut cur: Option<usize> = None;
        for pc in 0..n {
            let i = &self.ins[pc];
            match i.op {
                OP_IF | OP_LOOP => {
                    let div = cur.is_none() && if i.op == OP_IF { !self.uni(i.o[0], u) } else { self.loop_divergent(pc, u) };
                    if div { cur = Some(pc); }
                    st.push(div);
                    region[pc] = cur;
                }
                OP_ENDIF | OP_ENDLOOP => { region[pc] = cur; if st.pop() == Some(true) { cur = None; } }
                _ => {
                    region[pc] = cur;
                    if !control(i.op) {
                        uni[pc] = cur.is_none() && pure_op(i.op) && self.srcs(pc).all(|j| self.uni(i.o[j], u))
                            && (0..self.nd[pc]).all(|j| matches!(self.pool[i.o[j] as usize].ty, T_TEMP | T_NULL | T_OUTPUT | T_ODEPTH));
                    }
                }
            }
        }
        (region, uni)
    }
    fn temp_dests(&self, pc: usize) -> impl Iterator<Item = usize> + '_ {
        let i = &self.ins[pc];
        (0..self.nd[pc]).filter_map(move |j| { let o = &self.pool[i.o[j] as usize]; if o.ty == T_TEMP && o.rel[0] < 0 && o.idx[0] >= 0 { Some(o.idx[0] as usize) } else { None } })
    }
    /// An uniform instruction that only writes temporaries: gone from the main shader.
    fn hoisted(&self, pc: usize, uni: &[bool], u: &[bool]) -> bool {
        uni[pc] && (0..self.nd[pc]).all(|j| { let o = &self.pool[self.ins[pc].o[j] as usize]; o.ty == T_NULL || (o.ty == T_TEMP && (o.idx[0] as usize) < u.len() && u[o.idx[0] as usize]) })
    }
}

fn fix_jumps(ins: &mut [Ins]) -> bool {
    for i in ins.iter_mut() { i.jump = -1; }
    let mut stack: Vec<usize> = Vec::new();
    for i in 0..ins.len() {
        match ins[i].op {
            OP_IF | OP_LOOP => stack.push(i),
            OP_ELSE => { let Some(&t) = stack.last() else { return false }; ins[t].jump = i as i32 + 1; *stack.last_mut().unwrap() = i; }
            OP_ENDIF => { let Some(t) = stack.pop() else { return false }; ins[t].jump = i as i32 + 1; }
            OP_ENDLOOP => {
                let Some(t) = stack.pop() else { return false };
                ins[i].jump = t as i32 + 1;
                for j in t + 1..i {
                    let b = &mut ins[j];
                    if (b.op == OP_BREAK || b.op == OP_BREAKC) && b.jump < 0 { b.jump = i as i32 + 1; }
                    if (b.op == OP_CONTINUE || b.op == OP_CONTINUEC) && b.jump < 0 { b.jump = i as i32; }
                }
                ins[t].jump = i as i32 + 1;
            }
            _ => {}
        }
    }
    stack.is_empty()
}

/// A new shader like `s` with these instructions, operands and temporaries.
unsafe fn build(s: &Shader, ins: &[Ins], pool: &[Opnd], ntemps: i32) -> *mut Shader {
    let p = kzalloc(core::mem::size_of::<Shader>()) as *mut Shader;
    if p.is_null() { return p; }
    core::ptr::copy_nonoverlapping(s as *const Shader, p, 1);
    let sh = &mut *p;
    sh.ins = core::ptr::null_mut(); sh.pool = core::ptr::null_mut(); sh.icb = core::ptr::null_mut();
    let ib = kzalloc(core::mem::size_of::<Ins>() * ins.len().max(1)) as *mut Ins;
    let pb = kzalloc(core::mem::size_of::<Opnd>() * pool.len().max(1)) as *mut Opnd;
    let cb = if s.nicb > 0 && !s.icb.is_null() { kzalloc(core::mem::size_of::<Vec4>() * s.nicb as usize) as *mut Vec4 } else { core::ptr::null_mut() };
    if ib.is_null() || pb.is_null() || (s.nicb > 0 && !s.icb.is_null() && cb.is_null()) { kfree(ib as *mut c_void); kfree(pb as *mut c_void); kfree(cb as *mut c_void); kfree(p as *mut c_void); return core::ptr::null_mut(); }
    core::ptr::copy_nonoverlapping(ins.as_ptr(), ib, ins.len());
    core::ptr::copy_nonoverlapping(pool.as_ptr(), pb, pool.len());
    if !cb.is_null() { core::ptr::copy_nonoverlapping(s.icb, cb, s.nicb as usize); }
    sh.ins = ib; sh.nins = ins.len() as i32; sh.pool = pb; sh.npool = pool.len() as i32; sh.icb = cb; sh.ntemps = ntemps;
    let mut err = [0u8; 160];
    if !finish(sh, err.as_mut_ptr() as *mut c_char, err.len() as c_int) { dxbc_free(p); return core::ptr::null_mut(); }
    p
}

/// The split of a shader: (main, pre, rows), or None when nothing is worth moving.
pub unsafe fn hoist(s: &Shader, slot: i32) -> Option<(*mut Shader, *mut Shader, i32)> {
    if s.dxil != 0 || s.xt_total != 0 || s.nins <= 0 || !(0..14).contains(&slot) { return None; }
    let ins = s.ins(); let pool = s.pool();
    if ins.iter().any(|i| !control(i.op) && !pure_op(i.op) && i.op >= OP_BLOCK) { return None; }
    if ins.iter().any(|i| matches!(i.op, OP_SWITCH | OP_CASE | OP_DEFAULT | OP_ENDSWITCH | OP_CALL | OP_CALLC | OP_LABEL)) { return None; }
    /* the slot must be free */
    if pool.iter().any(|o| o.ty == T_CB && o.idx[0] == slot) { return None; }
    let n = ins.len();
    let mut nd = alloc::vec![0usize; n];
    for (pc, i) in ins.iter().enumerate() { let mut d = 0; let _ = operand_count(i.op, &mut d); nd[pc] = (d.max(0) as usize).min(i.nops as usize); }
    let mut end = alloc::vec![0usize; n];
    { let mut st = Vec::new(); for (pc, i) in ins.iter().enumerate() { match i.op { OP_IF | OP_LOOP => st.push(pc), OP_ENDIF | OP_ENDLOOP => { let t = st.pop()?; end[t] = pc; } _ => {} } } if !st.is_empty() { return None; } }
    let a = An { ins, pool, nd, end };
    let nt = s.ntemps.max(1) as usize;
    let mut u = alloc::vec![true; nt];
    /* registers used as indices stay as they are */
    for o in pool { for r in o.rel { if r >= 0 { let x = &pool[r as usize]; if x.ty == T_TEMP && x.idx[0] >= 0 && (x.idx[0] as usize) < nt { u[x.idx[0] as usize] = false; } } } }
    let loops: Vec<(usize, usize)> = (0..n).filter(|&p| ins[p].op == OP_LOOP).map(|p| (p, a.end[p])).collect();
    let (region, uni) = loop {
        let (region, uni) = a.classify(&u);
        let mut nu = u.clone();
        for pc in 0..n { if !control(ins[pc].op) && !uni[pc] { for t in a.temp_dests(pc) { if t < nt { nu[t] = false; } } } }
        /* read inside a loop that writes it: its value moves with the iterations */
        for pc in 0..n {
            if a.hoisted(pc, &uni, &nu) { continue; }
            for j in a.srcs(pc) {
                let o = &pool[ins[pc].o[j] as usize];
                if o.ty != T_TEMP || o.rel[0] >= 0 || !(0..nt as i32).contains(&o.idx[0]) || !nu[o.idx[0] as usize] { continue; }
                let t = o.idx[0] as usize;
                if loops.iter().any(|&(s0, e0)| s0 < pc && pc < e0 && (s0..e0).any(|q| a.temp_dests(q).any(|d| d == t))) { nu[t] = false; }
            }
        }
        if nu == u { break (region, uni); }
        u = nu;
    };
    let gone = (0..n).filter(|&pc| a.hoisted(pc, &uni, &u)).count();
    if gone == 0 { return None; }
    /* the rows: (where it is recorded, register) */
    let mut rows: Vec<(usize, usize)> = Vec::new();
    let mut main_ins: Vec<Ins> = ins.to_vec();
    let mut main_pool: Vec<Opnd> = pool.to_vec();
    for pc in 0..n {
        if a.hoisted(pc, &uni, &u) { main_ins[pc] = Ins { op: OP_NOP, jump: -1, alt: -1, o: [-1; 5], ..Default::default() }; continue; }
        for j in a.srcs(pc) {
            let o = pool[ins[pc].o[j] as usize];
            if o.ty != T_TEMP || o.rel[0] >= 0 || !(0..nt as i32).contains(&o.idx[0]) || !u[o.idx[0] as usize] { continue; }
            let at = region[pc].unwrap_or(pc);
            let key = (at, o.idx[0] as usize);
            let row = match rows.iter().position(|&r| r == key) { Some(r) => r, None => { rows.push(key); rows.len() - 1 } };
            let mut c = o;
            c.ty = T_CB; c.dim = 2; c.idx = [slot, row as i32, 0]; c.rel = [-1; 3]; c.direct = 0;
            if main_pool.len() >= SHADER_MAX_OPERANDS as usize { return None; }
            main_pool.push(c);
            main_ins[pc].o[j] = (main_pool.len() - 1) as i16;
        }
    }
    /* the pre-shader: the shared path, the uniform instructions, and a copy of each
     * recorded register into a temporary of its own */
    let base = nt;
    let mut pre_ins: Vec<Ins> = Vec::new();
    let mut pre_pool: Vec<Opnd> = pool.to_vec();
    for pc in 0..n {
        for (r, &(at, t)) in rows.iter().enumerate() {
            if at != pc { continue; }
            /* as the GLSL compiler writes them: destinations in mask mode, sources swizzled */
            let d = Opnd { ty: T_TEMP, mode: 0, dim: 1, mask: 15, swz: [0, 1, 2, 3], rel: [-1; 3], idx: [(base + r) as i32, 0, 0], ..Default::default() };
            let sv = Opnd { ty: T_TEMP, mode: 1, dim: 1, mask: 15, swz: [0, 1, 2, 3], rel: [-1; 3], idx: [t as i32, 0, 0], ..Default::default() };
            pre_pool.push(d); pre_pool.push(sv);
            let k = pre_pool.len() as i16;
            pre_ins.push(Ins { op: OP_MOV, nops: 2, mask: 15, o: [k - 2, k - 1, -1, -1, -1], jump: -1, alt: -1, ..Default::default() });
        }
        let i = ins[pc];
        let keep = if region[pc].is_some() { false }
            else if control(i.op) { match i.op { OP_DISCARD | OP_NOP => false, OP_RETC => a.uni(i.o[0], &u), _ => true } }
            else { a.hoisted(pc, &uni, &u) };
        if keep { pre_ins.push(i); }
    }
    if pre_pool.len() > SHADER_MAX_OPERANDS as usize || main_pool.len() > SHADER_MAX_OPERANDS as usize || base + rows.len() > SHADER_MAX_TEMPS as usize { return None; }
    if pre_ins.is_empty() || !fix_jumps(&mut pre_ins) { return None; }
    let pre = build(s, &pre_ins, &pre_pool, (base + rows.len()) as i32);
    if pre.is_null() { return None; }
    let main = build(s, &main_ins, &main_pool, s.ntemps);
    if main.is_null() { dxbc_free(pre); return None; }
    Some((main, pre, rows.len() as i32))
}

/// The main shader (NULL: nothing to move), with the pre-shader in *pre and the rows it
/// records in *nrows; the main shader reads them from constant buffer `slot`.
#[no_mangle]
pub unsafe extern "C" fn dxbc_hoist(s: *const Shader, slot: c_int, pre: *mut *mut Shader, nrows: *mut c_int) -> *mut Shader {
    if s.is_null() || pre.is_null() || nrows.is_null() { return core::ptr::null_mut(); }
    match hoist(&*s, slot) {
        Some((m, p, r)) => { *pre = p; *nrows = r; m }
        None => core::ptr::null_mut(),
    }
}
/// Run the pre-shader for a draw (its constant buffers in env) and store the rows.
#[no_mangle]
pub unsafe extern "C" fn dxbc_hoist_run(pre: *const Shader, env: *const Env, rows: *mut Vec4, nrows: c_int) -> CBool {
    if pre.is_null() || env.is_null() || rows.is_null() || nrows <= 0 { return 0; }
    let inp = [Vec4::default(); DX_MAX_REG];
    let mut out = [Vec4::default(); DX_MAX_REG];
    let mut e: Exec = core::mem::zeroed();
    if dxbc_exec_init(&mut e, pre, env, inp.as_ptr(), out.as_mut_ptr()) == 0 { return 0; }
    dxbc_exec_run(&mut e);
    let base = (*pre).ntemps as usize - nrows as usize;
    for k in 0..nrows as usize { *rows.add(k) = *e.r.add(base + k); }
    dxbc_exec_fini(&mut e);
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The split shader against the original, in the interpreter, over many uniform sets
    /// and inputs: same outputs, same discards. Returns (instructions, moved).
    fn same(stage: u8, src: &[u8], name: &str) -> (usize, usize) {
        let c = crate::glsl::compile(stage, src).unwrap_or_else(|e| panic!("{}: {}", name, e));
        let sh = unsafe { &*c.shader };
        let Some((main, pre, nrows)) = (unsafe { hoist(sh, 1) }) else { return (sh.nins as usize, 0) };
        let moved = unsafe { (*main).ins().iter().zip(sh.ins()).filter(|(a, b)| a.op == OP_NOP && b.op != OP_NOP).count() };
        let mut seed = 99u32;
        let mut rnd = || { seed = seed.wrapping_mul(1103515245).wrapping_add(12345); ((seed >> 8) as f32 / 16777216.0) * 4.0 - 2.0 };
        unsafe extern "C" fn smp(_c: *mut c_void, t: c_int, _s: c_int, uv: *const f32, l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2) * 0.5 + t as f32 * 0.1 + i as f32 * 0.01 + l; } }
        for set in 0..40 {
            let mut urows: Vec<Vec4> = (0..c.cb_rows.max(1)).map(|_| Vec4 { u: [rnd().to_bits(), rnd().to_bits(), rnd().to_bits(), rnd().to_bits()] }).collect();
            /* flags that are exactly 0, 1 or 2 now and then, as shaders compare with them */
            if set % 3 == 0 { for r in urows.iter_mut() { for l in 0..4 { if (r.u[l] >> 3) & 1 == 0 { r.u[l] = ((set / 3 % 3) as f32).to_bits(); } } } }
            let mut env: Env = unsafe { core::mem::zeroed() };
            env.cb[0] = urows.as_ptr(); env.cb_rows[0] = urows.len() as u32; env.sample = Some(smp);
            let mut hrows = alloc::vec![Vec4::default(); nrows as usize];
            assert!(unsafe { dxbc_hoist_run(pre, &env, hrows.as_mut_ptr(), nrows) } != 0);
            let mut env2: Env = unsafe { core::mem::zeroed() };
            env2.cb[0] = urows.as_ptr(); env2.cb_rows[0] = urows.len() as u32; env2.sample = Some(smp);
            env2.cb[1] = hrows.as_ptr(); env2.cb_rows[1] = nrows as u32;
            for _ in 0..20 {
                let mut inp = [Vec4::default(); 32];
                for r in inp.iter_mut() { for l in 0..4 { r.u[l] = rnd().to_bits(); } }
                let mut o1 = [Vec4::default(); 32]; let mut o2 = [Vec4::default(); 32];
                let mut e1: Exec = unsafe { core::mem::zeroed() }; let mut e2: Exec = unsafe { core::mem::zeroed() };
                unsafe {
                    dxbc_exec_init(&mut e1, c.shader, &env, inp.as_ptr(), o1.as_mut_ptr());
                    dxbc_exec_init(&mut e2, main, &env2, inp.as_ptr(), o2.as_mut_ptr());
                    let r1 = crate::dxbc::interp_run(&mut e1); let r2 = crate::dxbc::interp_run(&mut e2);
                    assert_eq!(r1 != 0, r2 != 0, "{}: discard differs", name);
                    if r1 != 0 { for k in 0..32 { for l in 0..4 {
                        let (x, y) = (o1[k].u[l], o2[k].u[l]);
                        let nan = |v: u32| v & 0x7FFF_FFFF > 0x7F80_0000;
                        assert!(x == y || (nan(x) && nan(y)), "{}: out[{}].{} original {:08x} split {:08x} (set {})", name, k, l, x, y, set);
                    } } }
                    dxbc_exec_fini(&mut e1); dxbc_exec_fini(&mut e2);
                }
            }
        }
        unsafe { dxbc_free(main); dxbc_free(pre); }
        (sh.nins as usize, moved)
    }

    #[test]
    fn split_matches_the_original() {
        let srcs: [(&str, u8, &str); 5] = [
            ("light", 0, "uniform vec4 lp, amb, dif; uniform float g; varying vec3 n;
                void main() { vec3 l = normalize(lp.xyz); vec4 base = amb + dif * 0.5; float d = max(0.0, dot(normalize(n), l));
                gl_FragColor = (base + dif * d) * g; gl_FragColor.a = amb.a; }"),
            ("branches", 0, "uniform float mode, k; uniform vec4 c; varying vec4 v;
                void main() { vec4 r; if (mode == 1.0) { r = c * k; r.x += sqrt(abs(k)); } else r = c.wzyx;
                if (v.x > 0.0) r += vec4(k * 2.0); else { r -= c * c; if (v.y < -1.0) discard; }
                for (int i = 0; i < 3; i++) r.y += k * float(i);
                float s = 0.0; for (int i = 0; i < 4; i++) { if (v.z > float(i) * 0.5) break; s += k; }
                gl_FragColor = r + vec4(s); }"),
            ("loopvar", 0, "uniform vec4 w[4]; varying vec4 v;
                void main() { vec4 a = vec4(0.0); for (int i = 0; i < 4; i++) a += w[i] * v; vec4 b = w[0] + w[1]; gl_FragColor = a + b; }"),
            ("returns", 0, "uniform float f; uniform vec4 c; varying vec4 v;
                vec4 pick() { if (v.x > 0.5) return c * 2.0; if (f > 0.0) return c + vec4(f); return c; }
                void main() { vec4 m = c * f; if (v.w < 0.0) { gl_FragColor = m; return; } gl_FragColor = pick() + m; }"),
            ("vertex", 1, "uniform mat4 mvp; uniform vec3 lp; attribute vec4 p; attribute vec3 n; varying float d;
                void main() { gl_Position = mvp * p; vec3 l = normalize(lp); d = dot(n, l); }"),
        ];
        for (name, st, src) in srcs { let (n, m) = same(st, src.as_bytes(), name); assert!(m > 0, "{}: nothing moved", name); println!("{}: {} of {} instructions once per draw", name, m, n); }
    }

    #[test]
    fn corpus_split() {
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            let st = match p.extension().and_then(|x| x.to_str()) { Some("vert") => 1, Some("frag") => 0, _ => continue };
            let (n, m) = same(st, &std::fs::read(&p).unwrap(), &p.display().to_string());
            println!("{}: {} of {} instructions once per draw", p.file_name().unwrap().to_string_lossy(), m, n);
        }
    }

    /// cargo test --release --lib hoist::tests::bench_split -- --ignored --nocapture
    #[test]
    #[ignore]
    fn bench_split() {
        use crate::jit4::*;
        let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) != Some("frag") { continue; }
            let c = crate::glsl::compile(0, &std::fs::read(&p).unwrap()).unwrap();
            let Some((main, pre, nr)) = (unsafe { hoist(&*c.shader, 1) }) else { continue };
            let rows = alloc::vec![Vec4 { u: [0.5f32.to_bits(); 4] }; c.cb_rows.max(1) as usize];
            unsafe extern "C" fn smp(_c: *mut c_void, _t: c_int, _s: c_int, uv: *const f32, _l: f32, o: *mut f32) { for i in 0..4 { *o.add(i) = *uv.add(i % 2); } }
            let mut env: Env = unsafe { core::mem::zeroed() };
            env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32; env.sample = Some(smp);
            let mut hr = alloc::vec![Vec4::default(); nr as usize];
            unsafe { dxbc_hoist_run(pre, &env, hr.as_mut_ptr(), nr) };
            env.cb[1] = hr.as_ptr(); env.cb_rows[1] = nr as u32;
            let inp4 = alloc::vec![0.3f32; DX_MAX_REG * 16];
            let mut out4 = alloc::vec![0f32; DX_MAX_REG * 16];
            let n = 400_000;
            let mut t = [0f64; 2];
            for (k, sh) in [c.shader, main].into_iter().enumerate() {
                let q = unsafe { dxbc_quad_new(sh, &env, inp4.as_ptr(), out4.as_mut_ptr()) };
                let t0 = std::time::Instant::now(); for _ in 0..n / 4 { unsafe { dxbc_quad_run(q, 15); } } t[k] = t0.elapsed().as_nanos() as f64 / n as f64;
                unsafe { dxbc_quad_free(q) };
            }
            println!("{}: {:.1} ns a pixel, {:.1} with the uniform work split off ({:.2}x)", p.file_name().unwrap().to_string_lossy(), t[0], t[1], t[0] / t[1]);
            unsafe { dxbc_free(main); dxbc_free(pre); }
        }
    }
}
