/* Bounded native-IR -> SVGA3D legacy shader compiler. Failure is a CPU fallback,
 * not shader-upload failure: the CPU frontend supports a larger instruction set. */
#ifndef SHADER_GPU_H
#define SHADER_GPU_H
#include "dxbc.h"
typedef struct shader_gpu {
    uint32_t *words, nwords;
    uint64_t serial, link_key;
    int ncb;
    struct { int slot, row; } cb[224];
    int ninputs, input_reg[16];
    int ntextures;
    struct { int texture, sampler; } texture[8];
    bool vertex;
} shader_gpu_t;
shader_gpu_t *shader_gpu_compile(const dxbc_shader_t *s, const dxbc_shader_t *vs, char *err, int errlen);
void shader_gpu_free(shader_gpu_t *p);
void shader_gpu_constants(const shader_gpu_t *p, const dxbc_env_t *env, float (*out)[4], int view_w, int view_h);
#endif
