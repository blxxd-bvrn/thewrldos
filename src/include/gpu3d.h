/* Native paravirtual 3D offload. This is not a physical NVIDIA/AMD/Intel driver.
 * A successful draw has completed GPU execution/readback; false leaves caller
 * buffers untouched so the native CPU renderer can safely retry the draw. */
#ifndef GPU3D_H
#define GPU3D_H
#include "types.h"

#define GPU3D_MAX_TEXTURES 8
typedef struct {
    const uint32_t *words;
    uint32_t nwords;
    uint64_t serial;
    const float (*constants)[4];
    uint32_t nconstants;
} gpu3d_stage_t;
typedef struct {
    const uint8_t *data;
    uint32_t width, height, pitch;
    bool bgra, linear;
    uint32_t address_u, address_v, border;
} gpu3d_texture_t;
typedef struct {
    uint32_t width, height, color_pitch, depth_pitch;
    uint8_t *color;
    float *depth;
    bool bgra;
    const float *vertices;                 /* vec4 input registers, tightly packed per vertex */
    uint32_t nvertices, input_slots, input_mask;
    const uint16_t *indices;
    uint32_t nindices;
    bool strip;
    uint32_t vx, vy, vw, vh;
    float minz, maxz;
    bool depth_test, depth_write, front_ccw;
    uint32_t depth_func, cull, color_mask;
    bool blend;
    uint32_t src_blend, dst_blend, blend_op, src_alpha, dst_alpha, alpha_op;
    gpu3d_stage_t vs, ps;
    uint32_t ntextures;
    gpu3d_texture_t texture[GPU3D_MAX_TEXTURES];
} gpu3d_draw_t;

void gpu3d_init(void);
bool gpu3d_available(void);
bool gpu3d_float_depth(void);
const char *gpu3d_name(void);
uint32_t gpu3d_completed_draws(void);
bool gpu3d_draw(const gpu3d_draw_t *draw);

/* SVGA transport owned by video.c. All packets publish atomically, including
 * wraparound; waits are bounded. The staging GMR is retained after device loss. */
bool video_svga_info(uint32_t *caps, uint32_t *fifo_caps, uint32_t *hw_version);
uint32_t video_svga_devcap(uint32_t cap);
bool video_svga_start(void);
bool video_svga_submit(const uint32_t *words, uint32_t nwords);
bool video_svga_fence(uint32_t fence, uint32_t timeout_ms);
bool video_svga_gmr(uint32_t id, uint32_t descriptor_ppn);
void video_svga_cpu_fallback(void);
#endif
