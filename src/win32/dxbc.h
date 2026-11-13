/* Shader Model 4 bytecode (DXBC containers, as written by the HLSL compiler for
 * Direct3D 10): loading and an interpreter (dxbc.c), used by d3d10.c. */
#ifndef DXBC_H
#define DXBC_H
#include "kernel.h"

typedef union { float f[4]; uint32_t u[4]; int32_t i[4]; } vec4_t;

typedef struct {
    char name[32];
    int index, sysval, reg, mask;
} dxsig_t;

#define DX_MAX_SIG 16
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
} dxbc_env_t;

/* NULL with a reason in err (if given) when the bytecode is not usable */
dxbc_shader_t *dxbc_load(const void *code, size_t len, char *err, int errlen);
void dxbc_free(dxbc_shader_t *s);
int dxbc_type(const dxbc_shader_t *s);              /* 0 pixel, 1 vertex, 2 geometry */
int dxbc_inputs(const dxbc_shader_t *s, const dxsig_t **sig);
int dxbc_outputs(const dxbc_shader_t *s, const dxsig_t **sig);
/* run once: `in` and `out` hold DX_MAX_REG registers; false if the pixel was discarded */
bool dxbc_run(const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out);
#endif
