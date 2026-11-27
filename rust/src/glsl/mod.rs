//! A GLSL compiler for the OpenGL layer (src/win32/glsl.inc): OpenGL Shading Language
//! 1.10 to 4.x and ES source, with the fixed-function built-ins of the compatibility
//! profile (gl_Vertex, gl_ModelViewProjectionMatrix, gl_LightSource, gl_FrontLightProduct,
//! gl_TexCoord, gl_FragColor and the rest), compiled to the shader IR that the Direct3D 10
//! engine already runs (rust/src/dxbc.rs). The C side links a vertex and a fragment
//! shader by the names in their signatures and feeds uniforms in as constant buffer 0.
//!
//! Not implemented: geometry and tessellation shaders, uniform buffers in memory (block
//! members are plain uniforms), dynamic indexing of sampler arrays, shadow comparisons
//! (shadow2D returns 1), derivatives (dFdx and dFdy are 0), cube maps (sampled as 2D).

mod lex;
mod parse;
mod gen;

use crate::dxbc::*;
use crate::CBool;
use alloc::string::String;
use alloc::vec::Vec;
use alloc::format;
use core::ffi::{c_char, c_int, c_void};

extern "C" {
    fn kzalloc(n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
}

/// `glsl_var_t` (src/win32/glsl.inc)
#[repr(C)]
pub struct GlslVar { pub name: [u8; 96], pub gltype: u32, pub count: i32, pub reg: i32, pub stride: i32, pub location: i32 }
/// `glsl_unit_t`: one compiled shader and what the API needs to know about it.
#[repr(C)]
pub struct GlslUnit {
    pub shader: *mut Shader,
    pub uniforms: *mut GlslVar, pub nuniforms: i32,
    pub attribs: *mut GlslVar, pub nattribs: i32,
    pub cb_rows: i32, pub nsamplers: i32, pub stage: i32, pub ninstr: i32,
}

/// IF/ELSE/ENDIF and LOOP/ENDLOOP targets, and break/continue to their loops (as the
/// DXBC loader works them out).
fn fix_jumps(ins: &mut [Ins]) -> Result<(), String> {
    let mut stack: Vec<usize> = Vec::new();
    for i in 0..ins.len() {
        match ins[i].op {
            OP_IF | OP_LOOP => stack.push(i),
            OP_ELSE => { let t = *stack.last().ok_or("else without if")?; ins[t].jump = i as i32 + 1; *stack.last_mut().unwrap() = i; }
            OP_ENDIF => { let t = stack.pop().ok_or("endif without if")?; ins[t].jump = i as i32 + 1; }
            OP_ENDLOOP => {
                let t = stack.pop().ok_or("endloop without loop")?;
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
    if !stack.is_empty() { return Err("unbalanced control flow".into()); }
    Ok(())
}

pub struct Compiled { pub shader: *mut Shader, pub uniforms: Vec<gen::VarInfo>, pub attribs: Vec<gen::VarInfo>, pub cb_rows: i32, pub nsamplers: i32, pub ninstr: i32 }

/// Compile one shader: stage 0 fragment, 1 vertex.
pub fn compile(stage: u8, src: &[u8]) -> Result<Compiled, String> {
    let pre = lex::preprocess(src)?;
    let mut p = parse::Parser::new(&pre.toks);
    let unit = p.unit()?;
    let mut g = gen::Gen::new(stage, pre.version);
    for gl in &unit { g.global(gl)?; }
    g.main()?;
    let mut ins = core::mem::take(&mut g.ins);
    fix_jumps(&mut ins)?;
    unsafe {
        let s = kzalloc(core::mem::size_of::<Shader>()) as *mut Shader;
        if s.is_null() { return Err("out of memory".into()); }
        let sh = &mut *s;
        sh.ty = if stage == 1 { 1 } else { 0 };
        sh.major = 4; sh.minor = 0;
        sh.interpolation = g.interp;
        for (k, v) in g.inputs.iter().enumerate() { sh.inp[k] = *v; }
        for (k, v) in g.outputs.iter().enumerate() { sh.out[k] = *v; }
        sh.nin = g.inputs.len() as i32; sh.nout = g.outputs.len() as i32;
        sh.ntemps = g.ntemps.max(1);
        let ib = kzalloc(core::mem::size_of::<Ins>() * ins.len().max(1)) as *mut Ins;
        let pb = kzalloc(core::mem::size_of::<Opnd>() * g.pool.len().max(1)) as *mut Opnd;
        if ib.is_null() || pb.is_null() { kfree(ib as *mut c_void); kfree(pb as *mut c_void); kfree(s as *mut c_void); return Err("out of memory".into()); }
        core::ptr::copy_nonoverlapping(ins.as_ptr(), ib, ins.len());
        core::ptr::copy_nonoverlapping(g.pool.as_ptr(), pb, g.pool.len());
        sh.ins = ib; sh.nins = ins.len() as i32;
        sh.pool = pb; sh.npool = g.pool.len() as i32;
        let mut err = [0u8; 160];
        if !finish(sh, err.as_mut_ptr() as *mut c_char, err.len() as c_int) {
            let n = err.iter().position(|&c| c == 0).unwrap_or(0);
            dxbc_free(s);
            return Err(format!("0: internal error: {}", String::from_utf8_lossy(&err[..n])));
        }
        Ok(Compiled { shader: s, uniforms: core::mem::take(&mut g.uniforms), attribs: core::mem::take(&mut g.attribs), cb_rows: g.cb_rows, nsamplers: g.nsamplers, ninstr: ins.len() as i32 })
    }
}

fn vars(v: &[gen::VarInfo]) -> *mut GlslVar {
    unsafe {
        let p = kzalloc(core::mem::size_of::<GlslVar>() * v.len().max(1)) as *mut GlslVar;
        if p.is_null() { return p; }
        for (k, x) in v.iter().enumerate() {
            let d = &mut *p.add(k);
            let b = x.name.as_bytes(); let n = b.len().min(95);
            d.name[..n].copy_from_slice(&b[..n]);
            d.gltype = x.gltype; d.count = x.count; d.reg = x.reg; d.stride = x.stride; d.location = x.location;
        }
        p
    }
}

/// Compile `len` bytes of GLSL. NULL on failure, with the reason ("line: message") in log.
#[no_mangle]
pub unsafe extern "C" fn glsl_compile(stage: c_int, src: *const u8, len: usize, log: *mut c_char, loglen: c_int) -> *mut GlslUnit {
    if !log.is_null() && loglen > 0 { *log = 0; }
    if src.is_null() { return core::ptr::null_mut(); }
    let s = core::slice::from_raw_parts(src, len);
    match compile(stage as u8, s) {
        Err(e) => { set_err_parts(log, loglen, &[e.as_bytes()]); core::ptr::null_mut() }
        Ok(c) => {
            let u = kzalloc(core::mem::size_of::<GlslUnit>()) as *mut GlslUnit;
            if u.is_null() { dxbc_free(c.shader); return u; }
            let un = &mut *u;
            un.shader = c.shader; un.cb_rows = c.cb_rows; un.nsamplers = c.nsamplers; un.stage = stage; un.ninstr = c.ninstr;
            un.uniforms = vars(&c.uniforms); un.nuniforms = c.uniforms.len() as i32;
            un.attribs = vars(&c.attribs); un.nattribs = c.attribs.len() as i32;
            u
        }
    }
}
#[no_mangle]
pub unsafe extern "C" fn glsl_free(u: *mut GlslUnit) {
    if u.is_null() { return; }
    dxbc_free((*u).shader);
    kfree((*u).uniforms as *mut c_void);
    kfree((*u).attribs as *mut c_void);
    kfree(u as *mut c_void);
}
#[allow(dead_code)]
fn _cbool(_: CBool) {}

#[cfg(test)]
mod tests;
