/* Pixel code compiled twice (included from glsl.inc in two parts): once as the rest of
 * the kernel (names ending _x87; the 32-bit edition uses the x87 unit there) and once for
 * SSE (_sse), for the four-pixel path, which only runs where SSE2 is because the shader
 * code it calls needs it. In the 64-bit edition both are SSE code. */
/* entry points: the 32-bit kernel keeps its stack 4-byte aligned, SSE code wants 16 */
#ifndef SL_ENTRY
#ifdef __i386__
#define SL_ENTRY __attribute__((force_align_arg_pointer))
#else
#define SL_ENTRY
#endif
#endif
#define PX(n) n##_x87
#define SL_X87
#include "glsl_px2.inc"
#undef SL_X87
#undef PX
#pragma GCC push_options
#ifdef __i386__
#pragma GCC target("sse2,fpmath=sse")
#endif
#include <emmintrin.h>
#define PX(n) n##_sse
#include "glsl_px2.inc"
#undef PX
#pragma GCC pop_options
