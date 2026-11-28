/* Native vertex/pixel shader engine: SM4/5 DXBC and an SM6.0 DXIL subset.
 * Higher models are extensions of this CPU engine, not D3D11/12 feature levels.
 * Written in Rust (rust/src/dxbc.rs, dxil.rs); this is its C interface. */
#ifndef DXBC_H
#define DXBC_H
#include "kernel.h"

typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; } vec4_t;

typedef struct {
    char name[32];
    int index, sysval, reg, mask, component_type;
} dxsig_t;

#define DX_MAX_SIG 32
#define DX_MAX_REG 32

typedef struct dxbc_shader dxbc_shader_t;
typedef struct {
    const vec4_t *cb[14];                /* constant buffers, as vec4 rows */
    uint32_t cb_rows[14];
    void *ctx;
    /* texture access: sample with sampler `smp` at uv (and level `lod` for sample_l),
     * fetch a texel, and the size of a texture */
    void (*sample)(void *ctx, int tex, int smp, const float *uv, float lod, float *out);
    void (*load)(void *ctx, int tex, const int32_t *xy, float *out);
    void (*size)(void *ctx, int tex, uint32_t *out);
    uint32_t primitive_id;
} dxbc_env_t;

/* NULL with a reason in err (if given) when the bytecode is not usable */
dxbc_shader_t *dxbc_load(const void *code, size_t len, char *err, int errlen);
void dxbc_free(dxbc_shader_t *s);
int dxbc_type(const dxbc_shader_t *s);              /* 0 pixel, 1 vertex, 2 geometry */
int dxbc_inputs(const dxbc_shader_t *s, const dxsig_t **sig);
int dxbc_outputs(const dxbc_shader_t *s, const dxsig_t **sig);
int dxbc_model(const dxbc_shader_t *s, int *minor);
bool dxbc_is_dxil(const dxbc_shader_t *s);
int dxbc_read_mask(const dxbc_shader_t *s, int reg);
int dxbc_interpolation(const dxbc_shader_t *s, int reg);
bool dxbc_writes_depth(const dxbc_shader_t *s);

/* Bind once per draw, run many times. No allocations occur in exec_run, even
 * for shaders with more than 32 temporaries. A context is caller-owned and may
 * not be shared by concurrently executing draws. Input/output arrays stay put
 * until fini; constant-buffer bindings/lengths also stay fixed. Register and
 * constant-buffer contents may change between invocations. */
typedef struct {
    const dxbc_shader_t *shader;
    const dxbc_env_t *env;
    const vec4_t *in;
    vec4_t *out, *r, *x;
    const uint32_t **bound;
    void *storage;
    vec4_t scratch;
    int block, previous_block;
    bool discard;
} dxbc_exec_t;
bool dxbc_exec_init(dxbc_exec_t *exec, const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out);
void dxbc_exec_fini(dxbc_exec_t *exec);
bool dxbc_exec_run(dxbc_exec_t *exec);
/* Four invocations at once (rust/src/jit4.rs), where the shader compiles that way: in
 * and out hold DX_MAX_REG registers as component vectors (register r, component c, lane l
 * at float 16 * r + 4 * c + l). run takes the lanes to run as bits and returns those that
 * did not discard. NULL from new: use dxbc_exec_t one invocation at a time. */
typedef struct dxbc_quad dxbc_quad_t;
dxbc_quad_t *dxbc_quad_new(const dxbc_shader_t *s, const dxbc_env_t *env, const float *in, float *out);
void dxbc_quad_free(dxbc_quad_t *q);
uint32_t dxbc_quad_run(dxbc_quad_t *q, uint32_t live);
/* Allocating convenience wrapper. `in`/`out` hold DX_MAX_REG registers (the
 * last output slot holds depth). False on discard, allocation failure or the
 * instruction budget. Draw loops should use exec_init/run/fini instead. */
bool dxbc_run(const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out);
#endif
