/* Host-side checks/benchmark for the real native shader loader and interpreter.
 * Shader fixtures are emitted by FXC/D3DCompile and DXC, not a private dialect. */
#define __USE_MINGW_ANSI_STDIO 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#undef offsetof
#undef va_start
#undef va_end
#undef va_arg
#undef NULL
#if defined(_WIN32) && defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
#include "shader_priv.h"
#if defined(_WIN32) && defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "../samples/d3d10cube_shaders.h"
#include "../samples/d3d10cube_modern_shaders.h"

static int failures, checks;
static unsigned allocations;
static int allocation_budget = -1, live_allocations;
typedef struct { size_t size; uint64_t magic; } allocation_t;
void *kmalloc(size_t n) {
    allocations++;
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) allocation_budget--;
    allocation_t *a = malloc(sizeof *a + n + 32);
    if (!a) return NULL;
    a->size = n; a->magic = 0x51ADEEA110CA7Eull;
    memset((char *)(a + 1) + n, 0xA7, 32); live_allocations++;
    return a + 1;
}
void *kzalloc(size_t n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void kfree(void *p) {
    if (!p) return;
    allocation_t *a = (allocation_t *)p - 1;
    if (a->magic != 0x51ADEEA110CA7Eull) { puts("FAIL allocator header corruption"); abort(); }
    for (int i = 0; i < 32; i++) if (((unsigned char *)p)[a->size + i] != 0xA7) { puts("FAIL allocation overrun"); abort(); }
    a->magic = 0; live_allocations--; free(a);
}
void *krealloc(void *p, size_t n) {
    void *q = kmalloc(n);
    if (q && p) { memcpy(q, p, MIN(n, ((allocation_t *)p - 1)->size)); kfree(p); }
    return q;
}

static void check(const char *name, int good) {
    checks++;
    if (!good) { printf("FAIL %s\n", name); failures++; }
}
static int close_float(float a, float b) { return fabsf(a - b) < 0.00002f; }
static int sampled_texture, sampled_sampler;
static void sample(void *ctx, int tex, int sampler, const float *uv, float lod, float *out) {
    (void)ctx; (void)tex; (void)sampler; (void)lod;
    sampled_texture = tex; sampled_sampler = sampler;
    out[0] = uv[0] * 0.2f + 0.3f; out[1] = uv[1] * 0.4f + 0.1f; out[2] = 0.7f; out[3] = 1;
}
static void bench_sample(void *ctx, int tex, int sampler, const float *uv, float lod, float *out) {
    (void)ctx; (void)tex; (void)sampler; (void)lod;
    out[0] = uv[0] * 0.3f + 0.2f; out[1] = uv[1] * 0.4f + 0.1f; out[2] = 0.6f; out[3] = 1;
}
static unsigned char *read_file(const char *name, size_t *size) {
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    if (!b || n < 0 || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *size = n; return b;
}
static dxbc_shader_t *fixture(const char *entry, const char *stage, int model) {
    char path[200], error[200]; size_t size;
    snprintf(path, sizeof path, "tools/shaders/%s-%s_%d_0.%s", entry, stage, model, model == 6 ? "dxil" : "dxbc");
    unsigned char *b = read_file(path, &size);
    if (!b) { printf("FAIL could not read %s\n", path); failures++; return NULL; }
    dxbc_shader_t *s = dxbc_load(b, size, error, sizeof error);
    free(b);
    if (!s) { printf("FAIL %s: %s\n", path, error); failures++; }
    else { check(path, dxbc_model(s, NULL) == model); check("source format", dxbc_is_dxil(s) == (model == 6)); }
    return s;
}
static void compare_runs(dxbc_shader_t *s, dxbc_env_t *env, vec4_t *in, vec4_t *out) {
    vec4_t reference[DX_MAX_REG] = { 0 };
    check("single invocation runs", dxbc_run(s, env, in, reference));
    dxbc_exec_t exec;
    if (!dxbc_exec_init(&exec, s, env, in, out)) { check("prepare context", 0); return; }
    unsigned before = allocations;
    check("prepared invocation runs", dxbc_exec_run(&exec));
    check("no allocation in prepared run", allocations == before);
    const dxsig_t *sig; int n = dxbc_outputs(s, &sig);
    for (int k = 0; k < n; k++) {
        int r = sig[k].sysval == 65 ? DX_MAX_REG - 1 : sig[k].reg;
        if (r >= 0) for (int i = 0; i < 4; i++) if (sig[k].mask & (1u << i))
            check("prepared/convenience output", out[r].u[i] == reference[r].u[i]);
    }
    dxbc_exec_fini(&exec);
}
static vec4_t semantic(dxbc_shader_t *s, vec4_t *regs, const char *name, int index, const vec4_t *set) {
    const dxsig_t *sig;
    int n = set ? dxbc_inputs(s, &sig) : dxbc_outputs(s, &sig);
    vec4_t value = { 0 };
    for (int k = 0; k < n; k++) if (!strcmp(sig[k].name, name) && sig[k].index == index) {
        int at = 0, r = sig[k].sysval == 65 ? DX_MAX_REG - 1 : sig[k].reg;
        if (r < 0) break;
        for (int i = 0; i < 4; i++) if (sig[k].mask & (1u << i)) {
            if (set) regs[r].u[i] = set->u[at];
            value.u[at++] = regs[r].u[i];
        }
        return value;
    }
    check("semantic found", 0); return value;
}
static void extra_graphics(int model) {
    vec4_t in[DX_MAX_REG] = { 0 }, out[DX_MAX_REG] = { 0 }, tint = { .f = { .8f, .5f, .4f, 1 } };
    dxbc_env_t env = { 0 }; env.cb[3] = &tint; env.cb_rows[3] = 1; env.sample = sample;
    const char *entries[] = { "packed_vertex", "packed_pixel", "sparse", "maths", "depth", "flat_color", "linear_color", "sample_implicit", "half_round", "bits" };
    for (int k = 0; k < (int)ARRAY_LEN(entries); k++) {
        if (model == 4 && k >= 8) continue;
        dxbc_shader_t *s = fixture(entries[k], k == 0 ? "vs" : "ps", model);
        if (!s) continue;
        memset(in, 0, sizeof in); memset(out, 0xA5, sizeof out);
        vec4_t color = { .f = { .6f, .8f, .5f, 1 } }, uv = { .f = { .1f, .2f } };
        if (k == 1) {
            semantic(s, in, "TEXCOORD", 0, &uv); semantic(s, in, "TEXCOORD", 1, &color);
            vec4_t scalar = { .f = { .375f } }; semantic(s, in, "TEXCOORD", 2, &scalar);
        } else if (k == 9) {
            color = (vec4_t){ .u = { 0xF0F00001, 1, 0x80000000u, 0x100 } };
            semantic(s, in, "COLOR", 0, &color);
        } else {
            const dxsig_t *sig; int n = dxbc_inputs(s, &sig);
            for (int j = 0; j < n; j++) {
                if (!strcmp(sig[j].name, "COLOR")) semantic(s, in, "COLOR", 0, &color);
                if (!strcmp(sig[j].name, "TEXCOORD")) semantic(s, in, "TEXCOORD", sig[j].index, &uv);
                if (!strcmp(sig[j].name, "POSITION")) { vec4_t p = { .f = { .2f, .3f, .4f, 1 } }; semantic(s, in, "POSITION", 0, &p); }
            }
        }
        compare_runs(s, &env, in, out);
        if (!k) {
            vec4_t low = semantic(s, out, "TEXCOORD", 0, NULL), high = semantic(s, out, "TEXCOORD", 1, NULL), scalar = semantic(s, out, "TEXCOORD", 2, NULL);
            check("packed VS low", close_float(low.f[0], .1f) && close_float(low.f[1], .2f));
            check("packed VS high", close_float(high.f[0], .6f) && close_float(high.f[1], .8f));
            check("packed VS scalar", close_float(scalar.f[0], .5f));
        } else if (k == 1) check("packed PS columns", close_float(out[0].f[0], .7f) && close_float(out[0].f[1], 1) && close_float(out[0].f[2], .375f) && out[0].f[3] == 1);
        else if (k == 2) {
            check("sparse resource bindings", sampled_texture == 7 && sampled_sampler == 5);
            check("sparse constant buffer", close_float(out[0].f[0], .32f * tint.f[0]));
        } else if (k == 3) {
            check("sin", close_float(out[0].f[0], sinf(.6f))); check("cos", close_float(out[0].f[1], cosf(.8f)));
            check("floor", out[0].f[2] == 0); check("round", out[0].f[3] == 1);
        } else if (k == 4) check("shader depth", dxbc_writes_depth(s) && out[DX_MAX_REG - 1].f[0] == .25f);
        else if (k == 5 || k == 6) {
            const dxsig_t *sig; dxbc_inputs(s, &sig);
            check("interpolation mode", dxbc_interpolation(s, sig[0].reg) == (k == 5 ? 1 : 4));
            check("interpolation shader", close_float(out[0].f[1], color.f[1]));
        } else if (k == 7) check("implicit sampling", sampled_texture == 0 && sampled_sampler == 0 && close_float(out[0].f[0], .32f));
        else if (k == 8) check("half conversion", out[0].f[0] == .60009765625f && out[0].f[1] == .7998046875f && out[0].f[2] == .5f && out[0].f[3] == 1);
        else if (k == 9) {
            check("integer bit operations", out[0].u[0] == 9 && out[0].u[1] == 0x80000000u && out[0].u[2] == 31 && out[0].u[3] == 8);
            memset(in, 0, sizeof in); compare_runs(s, &env, in, out);
            check("firstbit zero", out[0].u[2] == ~0u && out[0].u[3] == ~0u);
        }
        dxbc_free(s);
    }
}
static opnd_t reg_operand(int type, int index, int mask) {
    opnd_t o = { .type = type, .mode = 1, .mask = mask, .dim = 1, .rel = { -1, -1, -1 }, .idx = { index } };
    for (int i = 0; i < 4; i++) o.swz[i] = i;
    return o;
}
static dxbc_shader_t *unit_shader(int opcode, int destinations, int sources, int temps) {
    dxbc_shader_t *s = kzalloc(sizeof *s);
    s->type = 1; s->major = 5; s->ntemps = temps;
    s->npool = destinations + sources; s->nins = 2;
    s->ins = kzalloc(2 * sizeof *s->ins); s->pool = kzalloc(s->npool * sizeof *s->pool);
    s->ins[0].op = opcode; s->ins[0].nops = s->npool; s->ins[1].op = OP_RET;
    for (int i = 0; i < s->npool; i++) { s->ins[0].o[i] = i; s->pool[i] = reg_operand(i < destinations ? T_OUTPUT : T_INPUT, i < destinations ? i : i - destinations, 15); }
    return s;
}
static void finish_unit(dxbc_shader_t *s) {
    char error[160]; bool good = shader_finish(s, error, sizeof error);
    if (!good) printf("FAIL native unit IR: %s\n", error);
    check("native unit IR validates", good);
}
static void operation_cases(void) {
    vec4_t in[DX_MAX_REG] = { 0 }, out[DX_MAX_REG] = { 0 };
    dxbc_env_t env = { 0 };
    const int ops[] = { OP_UADDC, OP_USUBB, OP_SWAPC, OP_UBFE, OP_IBFE, OP_BFI };
    for (int k = 0; k < (int)ARRAY_LEN(ops); k++) {
        int nd = k < 3 ? 2 : 1, ns = k < 2 ? 2 : k == 5 ? 4 : 3;
        dxbc_shader_t *s = unit_shader(ops[k], nd, ns, 0);
        if (k < 2) { in[0] = (vec4_t){ .u = { ~0u, 0, 7, 0x80000000u } }; in[1] = (vec4_t){ .u = { 1, 1, 2, 0x80000000u } }; }
        else if (k == 2) { in[0] = (vec4_t){ .u = { 0, 1, 0x80000000u, 0 } }; in[1] = (vec4_t){ .u = { 1, 2, 3, 4 } }; in[2] = (vec4_t){ .u = { 5, 6, 7, 8 } }; }
        else { in[0] = (vec4_t){ .u = { 4, 0, 31, 8 } }; in[1] = (vec4_t){ .u = { 4, 0, 4, 30 } }; in[2] = (vec4_t){ .u = { 0xF0, ~0u, 0x80000000u, ~0u } }; in[3] = (vec4_t){ .u = { 0x12345678, ~0u, 0, 7 } }; }
        finish_unit(s); compare_runs(s, &env, in, out);
        if (k == 0) check("add carry", out[0].u[0] == 0 && out[1].u[0] == 1 && out[0].u[2] == 9 && out[1].u[2] == 0);
        else if (k == 1) check("subtract borrow", out[0].u[1] == ~0u && out[1].u[1] == 1 && out[0].u[2] == 5 && out[1].u[2] == 0);
        else if (k == 2) check("conditional swap", out[0].u[0] == 1 && out[1].u[0] == 5 && out[0].u[1] == 6 && out[1].u[1] == 2);
        else if (k == 3) check("unsigned bitfield", out[0].u[0] == 15 && out[0].u[1] == 0 && out[0].u[2] == 0x8000000 && out[0].u[3] == 3);
        else if (k == 4) check("signed bitfield", out[0].i[0] == -1 && out[0].u[1] == 0 && out[0].u[2] == 0xF8000000u && out[0].i[3] == -1);
        else check("insert bitfield", out[0].u[0] == 0x12345608 && out[0].u[1] == ~0u && out[0].u[3] == 0xC0000007);
        dxbc_free(s);
    }
    dxbc_shader_t *s = unit_shader(OP_F16TOF32, 1, 1, 0); finish_unit(s);
    dxbc_exec_t exec; check("prepare exhaustive half test", dxbc_exec_init(&exec, s, &env, in, out));
    dxbc_shader_t *back = unit_shader(OP_F32TOF16, 1, 1, 0); finish_unit(back);
    vec4_t half_out[DX_MAX_REG] = { 0 }; dxbc_exec_t reverse;
    check("prepare half reverse", dxbc_exec_init(&reverse, back, &env, out, half_out));
    unsigned before = allocations;
    for (unsigned h = 0; h < 65536; h++) {
        in[0].u[0] = h; dxbc_exec_run(&exec); dxbc_exec_run(&reverse);
        unsigned expected = (h & 0x7C00) == 0x7C00 && (h & 1023) ? h | 0x200 : h;
        check("all half encodings round trip", half_out[0].u[0] == expected);
    }
    check("half loop does not allocate", before == allocations);
    dxbc_exec_fini(&exec); dxbc_exec_fini(&reverse); dxbc_free(s); dxbc_free(back);

    const int rounds[] = { OP_ROUND_NE, OP_ROUND_NI, OP_ROUND_PI, OP_ROUND_Z };
    const uint32_t cases[] = { 0, 0x80000000u, 0x3F000000, 0xBF000000, 0x3FC00000, 0xBFC00000, 0x4AFFFFFD, 0xCAFFFFFD, 0x7F800000, 0xFF800000, 0x7FC12345 };
    for (int k = 0; k < 4; k++) {
        s = unit_shader(rounds[k], 1, 1, 0); finish_unit(s);
        for (int j = 0; j < (int)ARRAY_LEN(cases); j++) {
            in[0].u[0] = cases[j]; check("rounding runs", dxbc_run(s, &env, in, out));
            float x = in[0].f[0]; vec4_t expected = { .f = { k == 0 ? nearbyintf(x) : k == 1 ? floorf(x) : k == 2 ? ceilf(x) : truncf(x) } };
            check("rounding incl. signed zero/NaN", out[0].u[0] == expected.u[0]);
        }
        dxbc_free(s);
    }
    s = unit_shader(OP_MOV, 1, 1, 1024); s->pool[0].mask = 2; finish_unit(s);
    in[0] = (vec4_t){ .f = { 2, 3, 4, 5 } }; check("prepare large temporary bank", dxbc_exec_init(&exec, s, &env, in, out));
    before = allocations;
    for (int j = 0; j < 1000; j++) check("large bank invocation", dxbc_exec_run(&exec));
    check("large bank zero hot-path allocations", allocations == before);
    check("destination mask", out[0].u[0] == 0 && out[0].f[1] == 3 && out[0].u[2] == 0 && out[0].u[3] == 0);
    dxbc_exec_fini(&exec); dxbc_free(s);

    s = unit_shader(OP_MOVC, 1, 3, 0); s->ins[0].sat = 1; finish_unit(s);
    in[0] = (vec4_t){ .u = { 1, 0, 1, 0 } }; in[1] = (vec4_t){ .f = { -1, 7, 2, 7 } }; in[2] = (vec4_t){ .f = { 7, .25f, 7, .5f } };
    check("saturating conditional move runs", dxbc_run(s, &env, in, out));
    check("saturating conditional move", out[0].f[0] == 0 && out[0].f[1] == .25f && out[0].f[2] == 1 && out[0].f[3] == .5f);
    dxbc_free(s);
    s = unit_shader(OP_FTOI, 1, 1, 0); finish_unit(s);
    in[0] = (vec4_t){ .f = { 2147483520.0f, -2147483648.0f, 0, 2147483648.0f } }; in[0].u[2] = 0x7FC00000;
    dxbc_run(s, &env, in, out); check("signed conversion boundaries", out[0].u[0] == 0x7FFFFF80u && out[0].u[1] == 0x80000000u && out[0].u[2] == 0 && out[0].u[3] == 0x7FFFFFFFu); dxbc_free(s);
    s = unit_shader(OP_FTOU, 1, 1, 0); finish_unit(s);
    in[0] = (vec4_t){ .f = { 4294967040.0f, -1, 0, 4294967296.0f } }; in[0].u[2] = 0x7FC00000;
    dxbc_run(s, &env, in, out); check("unsigned conversion boundaries", out[0].u[0] == 0xFFFFFF00u && out[0].u[1] == 0 && out[0].u[2] == 0 && out[0].u[3] == ~0u); dxbc_free(s);

    s = unit_shader(OP_MOV, 1, 1, 0);
    s->pool = krealloc(s->pool, 3 * sizeof *s->pool); s->npool = 3;
    s->pool[1] = reg_operand(T_CB, 0, 15); s->pool[1].dim = 2; s->pool[1].rel[1] = 2;
    s->pool[2] = reg_operand(T_INPUT, 7, 15); finish_unit(s);
    vec4_t cb[] = { { .f = { 1, 2, 3, 4 } }, { .f = { 5, 6, 7, 8 } }, { .f = { 0 } } }; env.cb[0] = cb; env.cb_rows[0] = 2;
    check("relative address contributes input usage", dxbc_read_mask(s, 7) == 1);
    in[7].u[0] = 1; check("relative CB invocation", dxbc_run(s, &env, in, out)); check("relative CB value", out[0].f[2] == 7);
    in[7].u[0] = ~0u; check("out-of-range CB invocation", dxbc_run(s, &env, in, out)); check("out-of-range CB is zero", out[0].u[0] == 0);
    dxbc_free(s);
    s = unit_shader(OP_MOV, 1, 1, 0);
    s->pool[1] = reg_operand(T_CB, 0, 15); s->pool[1].dim = 2; s->pool[1].idx[1] = -2; finish_unit(s);
    env.cb[0] = cb + 2; env.cb_rows[0] = ~0u;
    check("negative direct CB index invocation", dxbc_run(s, &env, in, out));
    check("negative CB index stays zero even with large caller row count", out[0].u[0] == 0);
    dxbc_free(s); env.cb[0] = cb; env.cb_rows[0] = 2;

    s = unit_shader(OP_LOOP, 0, 0, 0); s->ins[1].op = OP_ENDLOOP; s->ins[1].jump = 1; finish_unit(s);
    check("infinite loop execution budget", !dxbc_run(s, &env, in, out)); dxbc_free(s);
    for (int k = 0; k < 2; k++) {
        s = unit_shader(k ? OP_MAX : OP_MIN, 1, 2, 0); finish_unit(s);
        in[0].u[0] = 0x7FC12345; in[1].f[0] = 2;
        dxbc_run(s, &env, in, out); check("min/max first NaN", out[0].f[0] == 2);
        in[0].f[0] = 2; in[1].u[0] = 0x7FC12345;
        dxbc_run(s, &env, in, out); check("min/max second NaN", out[0].f[0] == 2); dxbc_free(s);
    }
    s = unit_shader(OP_MOV, 1, 2, 0);
    s->pool = krealloc(s->pool, 4 * sizeof *s->pool); s->npool = 4;
    s->pool[3] = s->pool[0];
    s->ins = krealloc(s->ins, 6 * sizeof *s->ins); memset(s->ins, 0, 6 * sizeof *s->ins); s->nins = 6;
    s->pool[0].mask = 1;
    s->ins[0] = (ins_t){ .op = OP_IF, .nonzero = 1, .nops = 1, .o = { 2 }, .jump = 4 };
    s->ins[1] = (ins_t){ .op = OP_MOV, .nops = 2, .o = { 0, 1 } };
    s->ins[2].op = OP_RET; s->ins[3].op = OP_ENDIF;
    s->ins[4] = (ins_t){ .op = OP_MOV, .nops = 2, .o = { 3, 1 } }; s->ins[5].op = OP_RET;
    finish_unit(s);
    check("conditional early return retains output clear", s->nclear == 1);
    check("prepare conditional output", dxbc_exec_init(&exec, s, &env, in, out));
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 4; i++) out[0].f[i] = 99;
        for (int i = 0; i < 4; i++) in[0].f[i] = j + .5f;
        in[1].u[0] = j & 1; dxbc_exec_run(&exec);
        check("early partial return after fully written invocation", out[0].f[0] == j + .5f && out[0].f[1] == (j & 1 ? 0 : j + .5f) && out[0].f[2] == out[0].f[1] && out[0].f[3] == out[0].f[1]);
    }
    dxbc_exec_fini(&exec); dxbc_free(s);
}
static void cube_equivalence(void) {
    const void *vcode[] = { g_vs, g_vs5, g_vs6 }, *pcode[] = { g_ps, g_ps5, g_ps6 };
    const size_t vsizes[] = { sizeof g_vs, sizeof g_vs5, sizeof g_vs6 }, psizes[] = { sizeof g_ps, sizeof g_ps5, sizeof g_ps6 };
    vec4_t cb[10] = { 0 }, in[DX_MAX_REG] = { 0 }, expected[2][DX_MAX_REG] = { 0 };
    for (int i = 0; i < 4; i++) { cb[i].f[i] = 1; cb[i + 4].f[i] = 1; }
    cb[8] = (vec4_t){ .f = { .4f, .7f, -.6f, 0 } }; cb[9].f[0] = .05f;
    dxbc_env_t env = { 0 }; env.cb[0] = cb; env.cb_rows[0] = 10; env.sample = bench_sample;
    for (int stage = 0; stage < 2; stage++) for (int m = 0; m < 3; m++) {
        char error[160];
        dxbc_shader_t *s = dxbc_load(stage ? pcode[m] : vcode[m], stage ? psizes[m] : vsizes[m], error, sizeof error);
        if (!s) { printf("FAIL cube SM%d %s: %s\n", m + 4, stage ? "PS" : "VS", error); check("cube fixture load", 0); continue; }
        if (stage) check("cube PS needs no per-invocation output clear", s->nclear == 0);
        memset(in, 0, sizeof in);
        if (!stage) {
            vec4_t p = { .f = { .3f, -.2f, .5f, 1 } }, n = { .f = { .25f, .5f, -.75f, 0 } }, uv = { .f = { .25f, .75f } };
            semantic(s, in, "POSITION", 0, &p); semantic(s, in, "NORMAL", 0, &n); semantic(s, in, "TEXCOORD", 0, &uv);
        } else {
            vec4_t uv = { .f = { .25f, .75f } }, c = { .f = { .75f, .5f, .9f, 1 } };
            semantic(s, in, "TEXCOORD", 0, &uv); semantic(s, in, "COLOR", 0, &c);
        }
        vec4_t out[DX_MAX_REG] = { 0 }; compare_runs(s, &env, in, out);
        const char *names[] = { "SV_Position", "TEXCOORD", "COLOR" };
        int n = stage ? 1 : 3;
        for (int k = 0; k < n; k++) {
            vec4_t v = semantic(s, out, stage ? "SV_Target" : names[k], 0, NULL);
            if (!m) expected[stage][k] = v;
            else for (int i = 0; i < 4; i++) check("SM4/5/6 cube equivalent results", close_float(v.f[i], expected[stage][k].f[i]));
        }
        dxbc_free(s);
    }
}
static void reject_fixture(const char *entry, const char *stage, int model) {
    char path[200], error[200] = { 0 }; size_t size = 0;
    snprintf(path, sizeof path, "tools/shaders/%s-%s_%d_0.%s", entry, stage, model, model == 6 ? "dxil" : "dxbc");
    unsigned char *b = read_file(path, &size); check("rejection fixture exists", b != NULL);
    dxbc_shader_t *s = dxbc_load(b, size, error, sizeof error);
    check(path, s == NULL && error[0]); dxbc_free(s); free(b);
}
static void rejection_cases(void) {
    for (int m = 4; m <= 6; m++) { reject_fixture("derivative", "ps", m); reject_fixture("cube_texture", "ps", m); }
    reject_fixture("wave", "ps", 6); reject_fixture("compute", "cs", 6); reject_fixture("dynamic_loop", "ps", 6);
    reject_fixture("resource_space", "ps", 6); reject_fixture("uav", "ps", 6);
    reject_fixture("helper_call", "ps", 6); reject_fixture("double_math", "ps", 6);
    reject_fixture("lod_one", "ps", 5); reject_fixture("lod_one", "ps", 6);
    {
        char error[160]; size_t size;
        unsigned char *b = read_file("tools/shaders/newer_profile-ps_6_1.dxil", &size);
        check("newer profile fixture exists", b != NULL);
        dxbc_shader_t *s = b ? dxbc_load(b, size, error, sizeof error) : NULL;
        check("SM6.1 explicitly rejected", !s && b); dxbc_free(s); free(b);
    }
    for (int m = 4; m <= 6; m++) {
        char path[160], error[160]; size_t size;
        snprintf(path, sizeof path, "tools/shaders/color-ps_%d_0.%s", m, m == 6 ? "dxil" : "dxbc");
        unsigned char *b = read_file(path, &size); if (!b) { check("corruption fixture", 0); continue; }
        for (size_t n = 0; n < size; n++) { dxbc_shader_t *s = dxbc_load(b, n, error, sizeof error); check("all container truncations rejected", s == NULL); dxbc_free(s); }
        unsigned char *unaligned = malloc(size + 1); memcpy(unaligned + 1, b, size);
        dxbc_shader_t *s = dxbc_load(unaligned + 1, size, error, sizeof error); check("unaligned container supported", s != NULL); dxbc_free(s); free(unaligned);
        int baseline = live_allocations;
        for (int n = 0; n < 28; n++) {
            allocation_budget = n; s = dxbc_load(b, size, error, sizeof error); allocation_budget = -1;
            dxbc_free(s); check("allocation-failure cleanup", live_allocations == baseline);
        }
        uint32_t state = 0x574C4400u + m;
        for (int n = 0; n < 1000; n++) {
            unsigned char *copy = malloc(size); memcpy(copy, b, size);
            state = state * 1664525u + 1013904223u; size_t at = state % size;
            state = state * 1664525u + 1013904223u; copy[at] ^= 1u << (state >> 29);
            s = dxbc_load(copy, size, error, sizeof error);
            if (s) { vec4_t in[DX_MAX_REG] = { 0 }, out[DX_MAX_REG] = { 0 }; dxbc_env_t env = { 0 }; dxbc_run(s, &env, in, out); }
            dxbc_free(s); free(copy);
            check("bounded mutation cleanup", live_allocations == baseline);
        }
        free(b);
    }
}
static void graphics_cases(int model) {
    vec4_t cb[5] = { 0 }, in[DX_MAX_REG] = { 0 }, out[DX_MAX_REG] = { 0 };
    const float tint[] = { 0.7f, 0.5f, 0.9f, 0.8f }, pos[] = { 0.2f, 0.3f, 0.4f, 1 }, color[] = { 0.6f, 0.8f, 0.5f, 1 };
    memcpy(cb[0].f, tint, sizeof tint);
    for (int i = 0; i < 4; i++) cb[i + 1].f[i] = 1;
    memcpy(in[0].f, pos, sizeof pos); memcpy(in[1].f, color, sizeof color); in[2].f[0] = 0.1f; in[2].f[1] = 0.2f;
    dxbc_env_t env = { 0 }; env.cb[0] = cb; env.cb_rows[0] = 5; env.sample = sample;
    dxbc_shader_t *s = fixture("vertex", "vs", model);
    if (s) {
        compare_runs(s, &env, in, out);
        for (int i = 0; i < 4; i++) { check("vertex transform", close_float(out[0].f[i], pos[i])); check("vertex color", close_float(out[1].f[i], color[i] * tint[i])); }
        check("vertex UV", close_float(out[2].f[0], 0.1f) && close_float(out[2].f[1], 0.2f));
        dxbc_free(s);
    }
    s = fixture("color", "ps", model);
    if (s) {
        compare_runs(s, &env, in, out);
        for (int i = 0; i < 4; i++) check("pixel branch/phi true", close_float(out[0].f[i], color[i] * tint[i] + 0.1f));
        in[2].f[0] = 0.6f;
        compare_runs(s, &env, in, out);
        for (int i = 0; i < 4; i++) check("pixel branch/phi false", close_float(out[0].f[i], color[i] * tint[i]));
        in[2].f[0] = 0.1f;
        dxbc_free(s);
    }
    s = fixture("texture_level", "ps", model);
    if (s) {
        compare_runs(s, &env, in, out);
        float uv[] = { 0.1f, 0.2f }, expected[4]; sample(NULL, 0, 0, uv, 0, expected);
        for (int i = 0; i < 4; i++) check("Texture2D sample", close_float(out[0].f[i], expected[i] * color[i]));
        dxbc_free(s);
    }
    s = fixture("reciprocal", "ps", model);
    if (s) {
        compare_runs(s, &env, in, out);
        for (int i = 0; i < 4; i++) check("reciprocal", close_float(out[0].f[i], 1.0f / (color[i] + 2)));
        dxbc_free(s);
    }
}
static void benchmark(void) {
    char error[160];
    dxbc_shader_t *s = dxbc_load(g_ps, sizeof g_ps, error, sizeof error);
    if (!s) { puts(error); failures++; return; }
    vec4_t cb[10] = { 0 }, in[DX_MAX_REG] = { 0 }, out[DX_MAX_REG] = { 0 };
    dxbc_env_t env = { 0 }; env.cb[0] = cb; env.cb_rows[0] = 10; env.sample = bench_sample;
    in[1].f[0] = 0.3f; in[1].f[1] = 0.2f;
    in[2].f[0] = 0.75f; in[2].f[1] = 0.5f; in[2].f[2] = 0.9f; in[2].f[3] = 1;
    dxbc_exec_t exec;
    if (!dxbc_exec_init(&exec, s, &env, in, out)) { failures++; dxbc_free(s); return; }
    volatile float checksum = 0; unsigned before = allocations;
    clock_t start = clock(); const int count = 1000000;
    for (int i = 0; i < count; i++) { cb[9].f[0] = (i & 31) * 0.01f; dxbc_exec_run(&exec); checksum += out[0].f[0]; }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("prepared cube pixel shader: %.6f s, %.0f invocations/s, %u hot-path allocations, checksum %.9g\n", seconds, count / seconds, allocations - before, checksum);
    dxbc_exec_fini(&exec); dxbc_free(s);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench-only")) { benchmark(); return failures ? 1 : 0; }
    graphics_cases(4); graphics_cases(5); graphics_cases(6);
    extra_graphics(4); extra_graphics(5); extra_graphics(6);
    operation_cases(); rejection_cases(); cube_equivalence();
    if (argc > 1 && !strcmp(argv[1], "--bench")) benchmark();
    check("all shader allocations released", live_allocations == 0);
    printf("shader tests: %s (%d checks)\n", failures ? "FAILED" : "passed", checks);
    return failures ? 1 : 0;
}
