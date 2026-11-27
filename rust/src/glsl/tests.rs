//! Host tests: compile shaders and run them in the interpreter.
use super::*;
use crate::dxbc::{Env, Vec4};

fn run(sh: *mut Shader, cb: &[[f32; 4]], inp: &[[f32; 4]]) -> Option<[Vec4; 32]> {
    let mut env: Env = unsafe { core::mem::zeroed() };
    let rows: Vec<Vec4> = cb.iter().map(|r| Vec4 { u: r.map(|f| f.to_bits()) }).collect();
    env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
    let mut i = [Vec4::default(); 32];
    for (k, r) in inp.iter().enumerate() { i[k] = Vec4 { u: r.map(|f| f.to_bits()) }; }
    let mut o = [Vec4::default(); 32];
    let ok = unsafe { crate::dxbc::dxbc_run(sh, &env, i.as_ptr(), o.as_mut_ptr()) };
    if ok != 0 { Some(o) } else { None }
}
fn f(v: &Vec4, i: usize) -> f32 { f32::from_bits(v.u[i]) }
fn near(a: f32, b: f32) -> bool { (a - b).abs() < 1e-3 }
fn ok(stage: u8, src: &str) -> Compiled {
    match compile(stage, src.as_bytes()) { Ok(c) => c, Err(e) => panic!("compile failed: {}\n{}", e, src) }
}

#[test]
fn vertex_transform_and_varyings() {
    let c = ok(1, "uniform mat4 m; attribute vec4 pos; varying vec2 uv;\nvoid main() { gl_Position = m * pos; uv = pos.xy * 0.5 + 0.5; }");
    let u = c.uniforms.iter().find(|u| u.name == "m").unwrap();
    assert_eq!(u.gltype, 0x8B5C);
    let mut cb = vec![[0f32; 4]; c.cb_rows as usize];
    /* column-major: scale x by 2, translate y by 3 */
    cb[u.reg as usize] = [2., 0., 0., 0.]; cb[u.reg as usize + 1] = [0., 1., 0., 0.]; cb[u.reg as usize + 2] = [0., 0., 1., 0.]; cb[u.reg as usize + 3] = [0., 3., 0., 1.];
    let a = c.attribs.iter().find(|a| a.name == "pos").unwrap();
    let mut inp = vec![[0f32; 4]; 8];
    inp[a.reg as usize] = [1., 2., 3., 1.];
    let o = run(c.shader, &cb, &inp).unwrap();
    let outs = unsafe { &(*c.shader).out };
    let pos = outs.iter().find(|s| s.sysval == 1).unwrap().reg as usize;
    assert!(near(f(&o[pos], 0), 2.) && near(f(&o[pos], 1), 5.) && near(f(&o[pos], 2), 3.));
    let uv = outs.iter().find(|s| s.name.starts_with(b"uv")).unwrap().reg as usize;
    assert!(near(f(&o[uv], 0), 1.0) && near(f(&o[uv], 1), 1.5));
}

#[test]
fn functions_loops_and_early_returns() {
    let src = "
        int pick(float w, float h) { if (w != 2.0) { if (w == 0.0 && h < 1.0) return 1; if (w == 1.0) return 2; } return 0; }
        float sum(int n) { float s = 0.0; for (int i = 0; i < 10; i++) { if (i == n) return s; s += float(i); } return -1.0; }
        void main() {
            float a = float(pick(0.0, 0.5)) + 10.0 * float(pick(1.0, 5.0)) + 100.0 * float(pick(2.0, 0.0));
            float b = sum(4);
            float c = 0.0; int k = 0;
            while (true) { k++; if (k > 5) break; if (k == 2) continue; c += 1.0; }
            gl_FragColor = vec4(a, b, c, sum(20));
        }";
    let c = ok(0, src);
    let o = run(c.shader, &[], &[]).unwrap();
    assert!(near(f(&o[0], 0), 21.0), "{}", f(&o[0], 0));
    assert!(near(f(&o[0], 1), 6.0), "{}", f(&o[0], 1));
    assert!(near(f(&o[0], 2), 4.0), "{}", f(&o[0], 2));
    assert!(near(f(&o[0], 3), -1.0), "{}", f(&o[0], 3));
}

#[test]
fn builtins_and_discard() {
    let src = "#version 120
        struct S { float a; vec2 b; };
        const float K = 0.5f;
        void main() {
            S s = S(1.0, vec2(2.0, 3.0));
            vec4 v = vec4(s.b, s.a, 0);
            mat2 r = mat2(0.0, 1.0, -1.0, 0.0);
            vec2 q = r * vec2(1.0, 0.0);
            ivec3 p = ivec3(vec3(7.9, -2.2, 3.0));
            float m = mod(7.5, 2.0) + clamp(5.0, 0.0, 1.0) + abs(-2.0) * K;
            if (p.x * p.z == 0) discard;
            gl_FragColor = vec4(q.y, float(p.x + p.y), m, length(vec2(3.0, 4.0)));
        }";
    let c = ok(0, src);
    let o = run(c.shader, &[], &[]).unwrap();
    assert!(near(f(&o[0], 0), 1.0) && near(f(&o[0], 1), 5.0) && near(f(&o[0], 2), 3.5) && near(f(&o[0], 3), 5.0), "{:?}", (0..4).map(|i| f(&o[0], i)).collect::<Vec<_>>());
    let c = ok(0, "uniform float x; void main() { if (x > 0.5) discard; gl_FragColor = vec4(1.0); }");
    assert!(run(c.shader, &[[1.0, 0., 0., 0.]], &[]).is_none());
    assert!(run(c.shader, &[[0.0, 0., 0., 0.]], &[]).is_some());
}

#[test]
fn compat_builtins_and_preprocessor() {
    let src = "
        #define SCALE(x) ((x) * 2.0)
        #ifdef GL_ES
        #error no
        #endif
        #if __VERSION__ >= 110 && defined(SCALE)
        varying vec4 normal;
        #endif
        void main() {
            normal.xyz = normalize(gl_NormalMatrix * gl_Normal);
            normal.w = SCALE(gl_Vertex.y);
            gl_Position = ftransform();
            gl_TexCoord[0] = gl_MultiTexCoord0;
            gl_FrontColor = gl_Color * gl_FrontLightProduct[0].diffuse;
        }";
    let c = ok(1, src);
    let names: Vec<&str> = c.uniforms.iter().map(|u| u.name.as_str()).collect();
    assert!(names.contains(&"gl_NormalMatrix") && names.contains(&"gl_ModelViewProjectionMatrix") && names.contains(&"gl_FrontLightProduct[0].diffuse"), "{:?}", names);
    let fs = "varying vec4 normal; uniform sampler2D t;
        vec4 light() { vec3 n = normalize(normal.xyz); return gl_FrontLightModelProduct.sceneColor + gl_FrontLightProduct[0].diffuse * max(0.0, dot(normalize(gl_LightSource[0].position.xyz), n)); }
        void main() { gl_FragColor = texture2D(t, gl_TexCoord[0].st) * light() * gl_Color; }";
    let c = ok(0, fs);
    assert_eq!(c.nsamplers, 1);
}

#[test]
fn errors_are_reported() {
    for (src, want) in [("void main() { undefinedThing = 1.0; }", "not declared"), ("void main() { float x = vec3(1.0); }", "convert"), ("void f() { f(); } void main() { f(); }", "recursion")] {
        match compile(0, src.as_bytes()) { Ok(_) => panic!("{} compiled", src), Err(e) => assert!(e.contains(want), "{}: {}", src, e) }
    }
}

#[test]
fn local_shader_corpus() {
    /* GLSL files a developer drops in rust/tests/glsl (not part of the repository) */
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let stage = match p.extension().and_then(|x| x.to_str()) { Some("vert") => 1, Some("frag") => 0, _ => continue };
        let src = std::fs::read(&p).unwrap();
        if let Err(err) = compile(stage, &src) { panic!("{}: {}", p.display(), err); }
    }
}

/// How long the interpreter takes per pixel for the corpus' fragment shaders (cargo test -- --ignored --nocapture)
#[test]
#[ignore]
fn bench_corpus_fragment() {
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/glsl");
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.extension().and_then(|x| x.to_str()) != Some("frag") { continue; }
        let c = compile(0, &std::fs::read(&p).unwrap()).unwrap();
        let cb = vec![[0.5f32; 4]; c.cb_rows.max(1) as usize];
        let rows: Vec<Vec4> = cb.iter().map(|r| Vec4 { u: r.map(|f| f.to_bits()) }).collect();
        let mut env: Env = unsafe { core::mem::zeroed() };
        env.cb[0] = rows.as_ptr(); env.cb_rows[0] = rows.len() as u32;
        let inp = [Vec4 { u: [0.3f32.to_bits(); 4] }; 32];
        let mut out = [Vec4::default(); 32];
        let mut ex: crate::dxbc::Exec = unsafe { core::mem::zeroed() };
        unsafe { crate::dxbc::dxbc_exec_init(&mut ex, c.shader, &env, inp.as_ptr(), out.as_mut_ptr()) };
        let n = 200_000;
        let t = std::time::Instant::now();
        for _ in 0..n { unsafe { crate::dxbc::dxbc_exec_run(&mut ex); } }
        let ns = t.elapsed().as_nanos() as f64 / n as f64;
        println!("{}: {} instructions, {:.0} ns per pixel", p.file_name().unwrap().to_string_lossy(), c.ninstr, ns);
        unsafe { crate::dxbc::dxbc_exec_fini(&mut ex) };
    }
}
