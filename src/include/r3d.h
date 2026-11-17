/* WRLD 3D: the OS's software 3D engine (src/gui/r3d.c).
 *
 * A fixed-function pipeline in the spirit of early OpenGL: 4x4 matrices,
 * per-vertex lighting (one directional light plus ambient), clipping against
 * the near plane, a depth buffer, perspective-correct texture mapping with
 * bilinear filtering, vertex colours and alpha blending. It draws into any
 * canvas, so the desktop (Flip 3D) and applications use the same engine.
 * This fixed-function engine stays on the CPU. D3D10's separate shader pipeline
 * can use the native legacy VMware SVGA3D backend (gpu3d.h), with CPU fallback. */
#ifndef R3D_H
#define R3D_H
#include "gfx.h"

extern bool accel3d;                     /* Control Panel > Visual Effects > 3D acceleration */

typedef struct { float m[16]; } mat4;    /* column-major, as OpenGL */
mat4 m4_identity(void);
mat4 m4_mul(mat4 a, mat4 b);             /* a * b: b is applied first */
mat4 m4_translate(float x, float y, float z);
mat4 m4_scale(float x, float y, float z);
mat4 m4_rotate_x(float rad);
mat4 m4_rotate_y(float rad);
mat4 m4_rotate_z(float rad);
mat4 m4_perspective(float fovy_rad, float aspect, float znear, float zfar);
float r3d_sin(float x);
float r3d_cos(float x);
float r3d_sqrt(float x);

typedef struct {
    float x, y, z;                       /* position */
    float u, v;                          /* texture coordinates, 0..1 */
    float nx, ny, nz;                    /* normal (for lighting) */
    uint32_t color;                      /* 0xAARRGGBB, multiplied with the texture */
    float w;                             /* homogeneous w of the position; 0 means 1 */
} r3d_vertex_t;

typedef struct {
    canvas_t *target;
    rect_t view;                         /* viewport inside the target */
    mat4 proj, view_m, model;
    canvas_t *texture;                   /* NULL: vertex colour only */
    bool bilinear;
    bool lighting;
    float light[3];                      /* direction towards the light, normalised */
    float ambient, diffuse;
    bool cull_back;                      /* skip triangles facing away */
    bool front_cw;                       /* front faces wind clockwise on screen (Direct3D); OpenGL is counter-clockwise */
    float *depth;                        /* this context's depth buffer (view.w * view.h), or NULL for the shared one */
    bool depth_test, depth_write;
    int alpha;                           /* 0..255, multiplied with texture and vertex alpha */
    uint32_t triangles, pixels;          /* statistics for the frame */
} r3d_t;

/* Start drawing into `target` at `view`: sets defaults and clears the depth buffer.
 * Returns false when 3D acceleration is turned off or memory is short. */
bool r3d_begin(r3d_t *r, canvas_t *target, rect_t view);
void r3d_clear_depth(r3d_t *r);
/* like r3d_begin, but with a depth buffer of the caller's (view.w * view.h floats) that is
 * cleared only if `clear`: for APIs whose frames span many draw calls (OpenGL, Direct3D) */
bool r3d_begin_with(r3d_t *r, canvas_t *target, rect_t view, float *depth, bool clear);
void r3d_triangles(r3d_t *r, const r3d_vertex_t *v, int count);   /* count: multiple of 3 */
void r3d_quad(r3d_t *r, const r3d_vertex_t q[4]);                   /* two triangles, q in order */
const char *r3d_renderer(void);                                    /* a description for the UI */
#endif
