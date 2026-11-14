/* VMware SVGA3D: native, fenced 3D commands and guest-memory DMA. No guest Mesa,
 * LLVM or graphics runtime is used. The host must expose a hardware 3D backend.
 *
 * Wire values/layouts follow VMware's MIT-licensed svga_reg.h/svga3d_cmd.h /
 * svga3d_types.h interfaces (VMware, Inc., 1998-2021). This implementation owns
 * one bounded legacy context and scratch surfaces; CPU buffers are authoritative
 * between draws. Readback is committed only after a completed fence. */
#include "kernel.h"
#include "gpu3d.h"

#define STAGING_BYTES (16u * 1024 * 1024)
#define MAX_WORDS 16384
#define CID 1
#define INVALID_ID 0xFFFFFFFFu
enum { SURFACE_DEFINE = 1040, SURFACE_DESTROY = 1041, SURFACE_DMA = 1044, CONTEXT_DEFINE = 1045,
       SET_ZRANGE = 1048, SET_RENDER_STATE = 1049, SET_RENDER_TARGET = 1050, SET_TEXTURE_STATE = 1051,
       SET_VIEWPORT = 1055, SHADER_DEFINE = 1059, SHADER_DESTROY = 1060, SET_SHADER = 1061,
       SET_CONSTANT = 1062, DRAW_PRIMITIVES = 1063, SET_SCISSOR = 1064 };
enum { S_COLOR = 1, S_DEPTH = 2, S_VERTEX = 3, S_INDEX = 4, S_TEXTURE = 8 };
enum { F_BGRA = 2, F_D24 = 9, F_BUFFER = 37, F_D32F = 76 };
static struct {
    int state;                         /* 0 absent, 1 probing, 2 ready, 3 lost */
    uint8_t *staging;
    uint32_t *descriptors;
    uint32_t packet[MAX_WORDS + 2];
    struct { uint32_t w, h, format; } surface[16];
    uint64_t vs, ps;
    uint32_t fence, completed;
    uint32_t depth_format;
} G;

static uint32_t fbits(float f) { union { float f; uint32_t u; } b = { f }; return b.u; }
static bool packet(uint32_t id, const uint32_t *body, uint32_t n) {
    if (n > MAX_WORDS || G.state == 3) return false;
    G.packet[0] = id; G.packet[1] = n * 4;
    if (n) memcpy(G.packet + 2, body, n * 4);
    return video_svga_submit(G.packet, n + 2);
}
static bool lost(const char *why) {
    if (G.state != 3) klog("gpu: SVGA3D disabled (%s); native CPU fallback retained", why);
    G.state = 3;
    /* Do not free/unregister the GMR after timeout: an outstanding host DMA may
     * still reference it. No later draw reuses the staging memory. */
    video_svga_cpu_fallback();
    return false;
}
bool gpu3d_available(void) { return G.state == 2; }
bool gpu3d_float_depth(void) { return gpu3d_available() && G.depth_format == F_D32F; }
const char *gpu3d_name(void) { return gpu3d_available() ? "VMware SVGA3D GPU" : "native CPU"; }
uint32_t gpu3d_completed_draws(void) { return G.completed; }

static bool target(uint32_t type, uint32_t sid) {
    uint32_t a[] = { CID, type, sid, 0, 0 }; return packet(SET_RENDER_TARGET, a, ARRAY_LEN(a));
}
static bool texture_state(uint32_t slot, uint32_t name, uint32_t value) {
    uint32_t a[] = { CID, slot, name, value }; return packet(SET_TEXTURE_STATE, a, ARRAY_LEN(a));
}
static bool surface(uint32_t sid, uint32_t format, uint32_t w, uint32_t h, uint32_t flags) {
    if (G.surface[sid].format == format && G.surface[sid].w == w && G.surface[sid].h == h) return true;
    if (G.surface[sid].format) { if (!packet(SURFACE_DESTROY, &sid, 1)) return false; }
    uint32_t a[] = { sid, flags, format, 1,0,0,0,0,0, w,h,1 };
    if (!packet(SURFACE_DEFINE, a, ARRAY_LEN(a))) return false;
    G.surface[sid].format = format; G.surface[sid].w = w; G.surface[sid].h = h;
    return true;
}
static bool dma(uint32_t sid, uint32_t offset, uint32_t pitch, uint32_t w, uint32_t h, bool readback) {
    uint32_t a[] = {
        1, offset, pitch,               /* GMR 1 and guest image pitch */
        sid, 0, 0, readback ? 2 : 1,
        0,0,0, w,h,1, 0,0,0,          /* one full image/buffer copy box */
        12, STAGING_BYTES - 1, 0        /* bounded DMA suffix */
    };
    return packet(SURFACE_DMA, a, ARRAY_LEN(a));
}
static bool shader(uint32_t type, const gpu3d_stage_t *p, uint64_t *current) {
    if (*current == p->serial) return true;
    uint32_t sid = type, unbind[] = { CID, type, INVALID_ID };
    if (!packet(SET_SHADER, unbind, 3)) return false;
    if (*current) { uint32_t a[] = { CID, sid, type }; if (!packet(SHADER_DESTROY, a, 3)) return false; }
    if (p->nwords > MAX_WORDS - 3) return false;
    G.packet[0] = SHADER_DEFINE; G.packet[1] = (3 + p->nwords) * 4;
    G.packet[2] = CID; G.packet[3] = sid; G.packet[4] = type;
    memcpy(G.packet + 5, p->words, p->nwords * 4);
    if (!video_svga_submit(G.packet, p->nwords + 5)) return false;
    *current = p->serial;
    return true;
}
static bool constants(uint32_t type, const gpu3d_stage_t *p) {
    if (!p->nconstants) return true;
    G.packet[0] = SET_CONSTANT; G.packet[1] = (4 + p->nconstants * 4) * 4;
    G.packet[2] = CID; G.packet[3] = 0; G.packet[4] = type; G.packet[5] = 0;
    memcpy(G.packet + 6, p->constants, p->nconstants * 16);
    return video_svga_submit(G.packet, 6 + p->nconstants * 4);
}
static bool reserve(uint32_t *at, uint32_t bytes, uint32_t *offset) {
    uint32_t start = (*at + 63) & ~63u;
    if (start > STAGING_BYTES || bytes > STAGING_BYTES - start) return false;
    *offset = start; *at = start + bytes; return true;
}
static void color_copy(uint8_t *dst, const uint8_t *src, uint32_t width, bool swap) {
    if (!swap) { memcpy(dst, src, width * 4); return; }
    for (uint32_t x = 0; x < width; x++, dst += 4, src += 4) {
        dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
    }
}

static bool execute(const gpu3d_draw_t *d) {
    if ((G.state != 1 && G.state != 2) || !d || !d->color || !d->vertices || !d->indices ||
        !d->width || !d->height || d->width > 2048 || d->height > 2048 ||
        d->color_pitch < d->width * 4 || (d->depth && d->depth_pitch < d->width * 4) ||
        !d->input_slots || d->input_slots > 16 || !d->input_mask || (d->input_mask >> d->input_slots) ||
        !d->nvertices || d->nvertices > 65536 || d->nindices < 3 || d->nindices > 196608 ||
        !d->vs.words || !d->ps.words || !d->vs.serial || !d->ps.serial ||
        !d->vs.nwords || !d->ps.nwords || d->vs.nwords > 8192 || d->ps.nwords > 8192 ||
        d->vs.words[0] != 0xFFFE0300u || d->ps.words[0] != 0xFFFF0300u ||
        d->vs.words[d->vs.nwords - 1] != 0xFFFF || d->ps.words[d->ps.nwords - 1] != 0xFFFF ||
        d->vs.nconstants > 256 || d->ps.nconstants > 224 ||
        (d->vs.nconstants && !d->vs.constants) || (d->ps.nconstants && !d->ps.constants) ||
        d->ntextures > GPU3D_MAX_TEXTURES || !d->vw || !d->vh || d->vx > d->width || d->vy > d->height ||
        d->vw > d->width - d->vx || d->vh > d->height - d->vy ||
        !(d->minz >= 0 && d->maxz <= 1 && d->minz <= d->maxz)) return false;
    for (uint32_t i = 0; i < d->nindices; i++) if (d->indices[i] >= d->nvertices) return false;
    uint32_t used = 0, color_at, depth_at = 0, vertex_at, index_at, texture_at[GPU3D_MAX_TEXTURES];
    uint32_t pixels = d->width * d->height * 4, vb = d->nvertices * d->input_slots * 16, ib = d->nindices * 2;
    if (!reserve(&used, pixels, &color_at) || (d->depth && !reserve(&used, pixels, &depth_at)) ||
        !reserve(&used, vb, &vertex_at) || !reserve(&used, ib, &index_at)) return false;
    for (uint32_t i = 0; i < d->ntextures; i++) {
        const gpu3d_texture_t *t = &d->texture[i];
        if (!t->data || !t->width || !t->height || t->width > 2048 || t->height > 2048 || t->pitch < t->width * 4 ||
            t->address_u < 1 || t->address_u > 4 || t->address_v < 1 || t->address_v > 4 ||
            !reserve(&used, t->width * t->height * 4, &texture_at[i])) return false;
    }
    /* All validation precedes submission. Input/output segments never overlap;
     * every upload remains stable until the final readback fence completes. */
    for (uint32_t y = 0; y < d->height; y++) {
        color_copy(G.staging + color_at + y * d->width * 4, d->color + y * d->color_pitch, d->width, !d->bgra);
        if (d->depth) {
            const float *z=(const float *)((const uint8_t *)d->depth+y*d->depth_pitch);
            if(G.depth_format==F_D32F) memcpy(G.staging+depth_at+y*d->width*4,z,d->width*4);
            else { uint32_t *p=(uint32_t *)(G.staging+depth_at+y*d->width*4);
                for(uint32_t x=0;x<d->width;x++){if(!(z[x]>=0&&z[x]<=1))return false;uint32_t n=z[x]>=1?0xFFFFFFu:(uint32_t)((double)z[x]*16777215.0+.5);p[x]=n<<8;}
            }
        }
    }
    memcpy(G.staging + vertex_at, d->vertices, vb); memcpy(G.staging + index_at, d->indices, ib);
    for (uint32_t i = 0; i < d->ntextures; i++) for (uint32_t y = 0; y < d->texture[i].height; y++) {
        const gpu3d_texture_t *t = &d->texture[i];
        color_copy(G.staging + texture_at[i] + y * t->width * 4, t->data + y * t->pitch, t->width, !t->bgra);
    }
    __sync_synchronize();
    if (!target(2, INVALID_ID) || !target(0, INVALID_ID) || !target(1, INVALID_ID)) return lost("target unbind");
    for (uint32_t i = 0; i < 8; i++) if (!texture_state(i, 1, INVALID_ID)) return lost("texture unbind");
    if (!surface(S_COLOR, F_BGRA, d->width, d->height, 1u << 6) ||
        (d->depth && !surface(S_DEPTH, G.depth_format, d->width, d->height, 1u << 7)) ||
        !surface(S_VERTEX, F_BUFFER, vb, 1, 1u << 4) ||
        !surface(S_INDEX, F_BUFFER, ib, 1, 1u << 3)) return lost("surface definition");
    if (!dma(S_COLOR, color_at, d->width * 4, d->width, d->height, false) ||
        (d->depth && !dma(S_DEPTH, depth_at, d->width * 4, d->width, d->height, false)) ||
        !dma(S_VERTEX, vertex_at, vb, vb, 1, false) || !dma(S_INDEX, index_at, ib, ib, 1, false)) return lost("surface upload");
    if (!shader(1, &d->vs, &G.vs) || !shader(2, &d->ps, &G.ps) || !constants(1, &d->vs) || !constants(2, &d->ps)) return lost("shader upload");
    uint32_t bind_vs[] = { CID, 1, 1 }, bind_ps[] = { CID, 2, 2 };
    if (!packet(SET_SHADER, bind_vs, 3) || !packet(SET_SHADER, bind_ps, 3) || !target(2, S_COLOR) || !target(0, d->depth ? S_DEPTH : INVALID_ID)) return lost("shader/target binding");
    uint32_t vp[] = { CID, d->vx, d->vy, d->vw, d->vh }, zr[] = { CID, fbits(d->minz), fbits(d->maxz) };
    if (!packet(SET_VIEWPORT, vp, 5) || !packet(SET_SCISSOR, vp, 5) || !packet(SET_ZRANGE, zr, 3)) return lost("viewport");
    uint32_t rs[] = { CID,
        1, d->depth && d->depth_test, 2, d->depth_write, 3,0, 4,0, 5,d->blend, 6,0, 7,0, 8,0, 9,0,
        27,0, 29,3, 30,2, 32,d->src_blend, 33,d->dst_blend, 34,d->blend_op,
        35,d->cull, 36,d->depth_func, 43,d->front_ccw ? 2u : 1u, 44,1, 47,d->color_mask, 55,1,
        65,0x3F800000u, 68,1, 85,0, 93,1, 94,d->src_alpha, 95,d->dst_alpha, 96,d->alpha_op
    };
    if (!packet(SET_RENDER_STATE, rs, ARRAY_LEN(rs))) return lost("render state");
    for (uint32_t i = 0; i < d->ntextures; i++) {
        const gpu3d_texture_t *t = &d->texture[i]; uint32_t sid = S_TEXTURE + i;
        if (!surface(sid, F_BGRA, t->width, t->height, 1u << 5) || !dma(sid, texture_at[i], t->width * 4, t->width, t->height, false)) return lost("texture upload");
        uint32_t ts[] = { CID, i,1,sid, i,8,t->address_u, i,9,t->address_v, i,10,0,
            i,11,t->linear ? 2u : 1u, i,12,t->linear ? 2u : 1u, i,13,t->border, i,25,0x3F800000u };
        if (!packet(SET_TEXTURE_STATE, ts, ARRAY_LEN(ts))) return lost("sampler state");
    }
    uint32_t prim[3 + 16 * 9 + 7], n = 3, declarations = 0;
    prim[0] = CID; prim[2] = 1;
    for (uint32_t i = 0; i < d->input_slots; i++) if (d->input_mask & (1u << i)) {
        uint32_t decl[] = { 3,0,5,i, S_VERTEX,i * 16,d->input_slots * 16, 0,d->nvertices - 1 };
        memcpy(prim + n, decl, sizeof decl); n += ARRAY_LEN(decl); declarations++;
    }
    prim[1] = declarations;
    uint32_t range[] = { d->strip ? 5u : 1u, d->strip ? d->nindices - 2 : d->nindices / 3, S_INDEX,0,2,2,0 };
    memcpy(prim + n, range, sizeof range); n += ARRAY_LEN(range);
    if (!packet(DRAW_PRIMITIVES, prim, n) || !dma(S_COLOR, color_at, d->width * 4, d->width, d->height, true) ||
        (d->depth && !dma(S_DEPTH, depth_at, d->width * 4, d->width, d->height, true)) ||
        !video_svga_fence(++G.fence, 1500)) return lost("draw/readback fence");
    for (uint32_t y = 0; y < d->height; y++) {
        color_copy(d->color + y * d->color_pitch, G.staging + color_at + y * d->width * 4, d->width, !d->bgra);
        if (d->depth) {
            float *z=(float *)((uint8_t *)d->depth+y*d->depth_pitch);
            if(G.depth_format==F_D32F)memcpy(z,G.staging+depth_at+y*d->width*4,d->width*4);
            else{const uint32_t *p=(const uint32_t *)(G.staging+depth_at+y*d->width*4);for(uint32_t x=0;x<d->width;x++){uint32_t n=p[x]>>8;z[x]=n==0xFFFFFFu?1:(float)((double)n/16777215.0);}}
        }
    }
    G.completed++;
    return true;
}
bool gpu3d_draw(const gpu3d_draw_t *d) { return gpu3d_available() && execute(d); }

void gpu3d_init(void) {
    uint32_t caps, fifo_caps, hw;
    if (G.state || !video_svga_info(&caps, &fifo_caps, &hw)) return;
    if (!(caps & 0x4000) || !(caps & 0x100000) || !video_svga_start()) return;
    video_svga_info(&caps, &fifo_caps, &hw);
    klog("gpu: SVGA3D offered caps=%08x fifo=%08x hw=%08x VS=%u PS=%u D32F=%08x", caps, fifo_caps, hw,
        video_svga_devcap(4), video_svga_devcap(6), video_svga_devcap(175));
    if (!(fifo_caps & 1) || hw < 0x20000) { klog("gpu: legacy host-backed SVGA3D unavailable; CPU fallback (guest-backed backend not implemented)"); return; }
    if (!video_svga_devcap(0) || video_svga_devcap(4) < 7 || video_svga_devcap(6) < 13 ||
        video_svga_devcap(29) < 32 || video_svga_devcap(30) < 32) return;
    G.depth_format=video_svga_devcap(175)?F_D32F:F_D24;
    if(G.depth_format==F_D24&&!video_svga_devcap(44)) return;
    G.staging = kmalloc_aligned(STAGING_BYTES, 4096); G.descriptors = kmalloc_aligned(4096, 4096);
    if (!G.staging || !G.descriptors) { kfree_aligned(G.staging); kfree_aligned(G.descriptors); G.staging = NULL; G.descriptors = NULL; return; }
    if (dma_addr(G.staging) + STAGING_BYTES > 0x100000000ull || dma_addr(G.descriptors) > 0xFFFFF000ull) {
        kfree_aligned(G.staging); kfree_aligned(G.descriptors); G.staging = NULL; G.descriptors = NULL; return;
    }
    memset(G.descriptors, 0, 4096); G.descriptors[0] = (uint32_t)(dma_addr(G.staging) >> 12); G.descriptors[1] = STAGING_BYTES / 4096;
    if (!video_svga_gmr(1, (uint32_t)(dma_addr(G.descriptors) >> 12))) return;
    G.state = 1;
    uint32_t cid = CID;
    if (!packet(CONTEXT_DEFINE, &cid, 1)) { lost("context creation"); return; }
    const uint32_t marker[] = { 0x12345678,0x89ABCDEF,0x3F800000,0xA55A55AA };
    memcpy(G.staging, marker, sizeof marker);
    if (!surface(S_COLOR,F_BGRA,4,1,1u<<6) || !dma(S_COLOR,0,16,4,1,false) || !video_svga_fence(++G.fence,1500)) { lost("DMA upload probe"); return; }
    memset(G.staging,0xCC,sizeof marker);
    if (!dma(S_COLOR,0,16,4,1,true) || !video_svga_fence(++G.fence,1500)) { lost("DMA readback probe"); return; }
    if (memcmp(G.staging,marker,sizeof marker)) { klog("gpu: DMA probe got %08x %08x",((uint32_t *)G.staging)[0],((uint32_t *)G.staging)[1]); lost("DMA roundtrip probe"); return; }
    klog("gpu: SVGA3D GMR upload/readback probe passed");
    /* A GPU-rendered/read-back triangle, not merely a capability bit. */
    static const uint32_t vs[] = { 0xFFFE0300, 0x0200001F,0x80000005,0x900F0000,
        0x0200001F,0x80000000,0xE00F0000, 0x02000001,0xE00F0000,0x90E40000, 0xFFFF };
    static const uint32_t ps[] = { 0xFFFF0300, 0x05000051,0xA00F0000,0x3F666666,0x3E4CCCCD,0x3E99999A,0x3F800000,
        0x02000001,0x800F0800,0xA0E40000, 0xFFFF };
    static const float vertices[][4] = { {-1,-1,.5f,1}, {0,1,.5f,1}, {1,-1,.5f,1} };
    static const uint16_t indices[] = { 0,1,2 };
    uint32_t *color = kmalloc(64 * 64 * 4); float *depth = kmalloc(64 * 64 * 4);
    if (!color || !depth) { kfree(color); kfree(depth); lost("self-test memory"); return; }
    for (int i = 0; i < 64 * 64; i++) { color[i] = 0xFF102030; depth[i] = 1; }
    gpu3d_draw_t d = { .width = 64, .height = 64, .color_pitch = 256, .depth_pitch = 256,
        .color = (uint8_t *)color, .depth = depth, .bgra = true,
        .vertices = vertices[0], .nvertices = 3, .input_slots = 1, .input_mask = 1, .indices = indices, .nindices = 3,
        .vw = 64, .vh = 64, .maxz = 1, .depth_test = true, .depth_write = true, .depth_func = 2, .cull = 1, .color_mask = 15,
        .src_blend = 2, .dst_blend = 1, .blend_op = 1, .src_alpha = 2, .dst_alpha = 1, .alpha_op = 1,
        .vs = { vs, ARRAY_LEN(vs), 1, NULL, 0 }, .ps = { ps, ARRAY_LEN(ps), 2, NULL, 0 } };
    bool ok = execute(&d);
    uint32_t center = color[32 * 64 + 32]; float z = depth[32 * 64 + 32];
    if (ok && ((center >> 16) & 255) >= 220 && ((center >> 8) & 255) >= 45 && ((center >> 8) & 255) <= 60 &&
        (center & 255) >= 70 && (center & 255) <= 85 && z > .49f && z < .51f && color[0] == 0xFF102030) {
        G.state = 2;
        klog("gpu: VMware SVGA3D GPU ready; fenced triangle/readback passed (pixel=%08x depth=%u/1000)", center, (uint32_t)(z * 1000));
    } else {
        klog("gpu: SVGA3D self-test result pixel=%08x depth-bits=%08x", center, fbits(z));
        lost("triangle/readback self-test");
    }
    kfree(color); kfree(depth);
}
