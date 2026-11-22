/* What the Rust shader engine (rust/src/dxbc.rs, dxil.rs) needs from C.
 *
 * The 64-bit kernel's Rust code is built for a soft-float target, so it cannot pass a
 * float or double by value to C (C passes them in SSE registers). Everything that would
 * goes through these functions, which take pointers: the C library's square root, sine,
 * cosine, exp2 and log2 (the same ones the C interpreter used), and the texture sampling
 * callback, whose LOD is a float. */
#include "shader_priv.h"

double sqrt(double); double sin(double); double cos(double); double exp2(double); double log2(double);

void dxbc_math(int fn, const double *x, double *r) {
    switch (fn) {
    case 0: *r = sqrt(*x); break;
    case 1: *r = sin(*x); break;
    case 2: *r = cos(*x); break;
    case 3: *r = exp2(*x); break;
    default: *r = log2(*x); break;
    }
}
void dxbc_sample(const dxbc_env_t *env, int tex, int smp, const float *uv, const float *lod, float *out) {
    env->sample(env->ctx, tex, smp, uv, *lod, out);
}

/* The shader structures, which the Rust code mirrors (rust/src/dxbc.rs,
 * dxil.rs); their layout, for the check rust_init makes at boot. */
const uint32_t rust_shader_layout[24] = {
    sizeof(dxsig_t), sizeof(opnd_t), offsetof(opnd_t, rel), offsetof(opnd_t, idx), offsetof(opnd_t, imm),
    sizeof(ins_t), offsetof(ins_t, o), offsetof(ins_t, jump),
    sizeof(dxbc_shader_t), offsetof(dxbc_shader_t, in), offsetof(dxbc_shader_t, ins), offsetof(dxbc_shader_t, pool),
    offsetof(dxbc_shader_t, xt), offsetof(dxbc_shader_t, icb), offsetof(dxbc_shader_t, read_mask),
    offsetof(dxbc_shader_t, interpolation_declared), offsetof(dxbc_shader_t, nclear),
    sizeof(dxbc_env_t), offsetof(dxbc_env_t, ctx), offsetof(dxbc_env_t, primitive_id),
    sizeof(dxbc_exec_t), offsetof(dxbc_exec_t, storage), offsetof(dxbc_exec_t, scratch), offsetof(dxbc_exec_t, discard),
};
