/* Shader Model 4 bytecode for Direct3D 10 (d3d10.c): the DXBC container, its
 * input/output signatures, and an interpreter for vertex and pixel shaders.
 *
 * The program tokens are decoded once into a compact instruction list (operand
 * types, swizzles, masks, modifiers, relative indices, jump targets) and then
 * interpreted for every vertex and every pixel. Supported: the SM4 arithmetic,
 * integer, conversion and comparison instructions, if/else/loop/break/continue,
 * discard, ret, constant buffers (with relative indexing), immediate constant
 * buffers, indexable temporaries, sample/sample_l/sample_b/ld/resinfo. Not
 * supported: geometry shaders, switch, subroutines, comparison samplers and
 * screen-space derivatives (they read as 0). */
#include "dxbc.h"

double floor(double); double ceil(double); double trunc(double); double rint(double);
double sqrt(double); double sin(double); double cos(double); double exp2(double); double log2(double);

enum {
    OP_ADD = 0, OP_AND, OP_BREAK, OP_BREAKC, OP_CALL, OP_CALLC, OP_CASE, OP_CONTINUE, OP_CONTINUEC, OP_CUT,
    OP_DEFAULT, OP_DERIV_RTX, OP_DERIV_RTY, OP_DISCARD, OP_DIV, OP_DP2, OP_DP3, OP_DP4, OP_ELSE, OP_EMIT,
    OP_EMITTHENCUT, OP_ENDIF, OP_ENDLOOP, OP_ENDSWITCH, OP_EQ, OP_EXP, OP_FRC, OP_FTOI, OP_FTOU, OP_GE,
    OP_IADD, OP_IF, OP_IEQ, OP_IGE, OP_ILT, OP_IMAD, OP_IMAX, OP_IMIN, OP_IMUL, OP_INE,
    OP_INEG, OP_ISHL, OP_ISHR, OP_ITOF, OP_LABEL, OP_LD, OP_LD_MS, OP_LOG, OP_LOOP, OP_LT,
    OP_MAD, OP_MIN, OP_MAX, OP_CUSTOMDATA, OP_MOV, OP_MOVC, OP_MUL, OP_NE, OP_NOP, OP_NOT,
    OP_OR, OP_RESINFO, OP_RET, OP_RETC, OP_ROUND_NE, OP_ROUND_NI, OP_ROUND_PI, OP_ROUND_Z, OP_RSQ, OP_SAMPLE,
    OP_SAMPLE_C, OP_SAMPLE_C_LZ, OP_SAMPLE_L, OP_SAMPLE_D, OP_SAMPLE_B, OP_SQRT, OP_SWITCH, OP_SINCOS, OP_UDIV, OP_ULT,
    OP_UGE, OP_UMUL, OP_UMAD, OP_UMAX, OP_UMIN, OP_USHR, OP_UTOF, OP_XOR,
    OP_DCL_FIRST = 88, OP_DCL_TEMPS = 104, OP_DCL_INDEXABLE_TEMP = 105, OP_DCL_LAST = 106,
};
enum { T_TEMP, T_INPUT, T_OUTPUT, T_XTEMP, T_IMM32, T_IMM64, T_SAMPLER, T_RESOURCE, T_CB, T_ICB,
       T_LABEL, T_PRIMID, T_ODEPTH, T_NULL };

typedef struct {
    uint8_t type, mode, mask, mod, dim;
    uint8_t direct;                      /* plain register with constant indices: read through fast_ptr */
    uint8_t swz[4];
    int16_t rel[3];                      /* index of a relative-address operand, or -1 */
    int32_t idx[3];
    uint32_t imm[4];
} opnd_t;
typedef struct {
    uint16_t op;
    uint8_t sat, nonzero, nops, ret_type;
    int16_t o[5];                        /* operands (indices into the pool) */
    int32_t jump;                        /* control flow target */
} ins_t;
#define MAX_XTEMP 4
struct dxbc_shader {
    int type;
    dxsig_t in[DX_MAX_SIG], out[DX_MAX_SIG];
    int nin, nout;
    ins_t *ins; int nins;
    opnd_t *pool; int npool;
    int ntemps;
    struct { int reg, size; } xt[MAX_XTEMP]; int nxt, xt_total;
    vec4_t *icb; int nicb;
};

int dxbc_type(const dxbc_shader_t *s) { return s->type; }
int dxbc_inputs(const dxbc_shader_t *s, const dxsig_t **sig) { *sig = s->in; return s->nin; }
int dxbc_outputs(const dxbc_shader_t *s, const dxsig_t **sig) { *sig = s->out; return s->nout; }
void dxbc_free(dxbc_shader_t *s) { if (!s) return; kfree(s->ins); kfree(s->pool); kfree(s->icb); kfree(s); }

static int parse_sig(const uint8_t *d, uint32_t size, dxsig_t *sig) {
    if (size < 8) return 0;
    uint32_t n = *(const uint32_t *)d;
    if (n > DX_MAX_SIG || 8 + n * 24 > size) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t *e = (const uint32_t *)(d + 8 + i * 24);
        uint32_t no = e[0];
        dxsig_t *s = &sig[i];
        memset(s, 0, sizeof *s);
        if (no < size) { int k = 0; while (no + k < size && d[no + k] && k < 31) { s->name[k] = d[no + k]; k++; } }
        s->index = e[1]; s->sysval = e[2]; s->reg = e[4]; s->mask = e[5] & 0xF;
    }
    return n;
}

/* operand decoding */
typedef struct { const uint32_t *t; uint32_t pos, end; dxbc_shader_t *s; int cap; bool bad; const char *why; } dec_t;
static int new_opnd(dec_t *d) {
    if (d->s->npool == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 256;
        opnd_t *p = krealloc(d->s->pool, sizeof(opnd_t) * d->cap);
        if (!p) { d->bad = true; d->why = "out of memory"; return -1; }
        d->s->pool = p;
    }
    return d->s->npool++;
}
static uint32_t next(dec_t *d) { if (d->pos >= d->end) { d->bad = true; d->why = "truncated instruction"; return 0; } return d->t[d->pos++]; }
static int parse_opnd(dec_t *d) {
    uint32_t tok = next(d);
    int k = new_opnd(d);
    if (k < 0) return -1;
    opnd_t o; memset(&o, 0, sizeof o);
    o.rel[0] = o.rel[1] = o.rel[2] = -1;
    int nc = tok & 3;
    o.type = (tok >> 12) & 0xFF;
    o.dim = (tok >> 20) & 3;
    o.mask = 0xF; o.mode = 1;
    for (int i = 0; i < 4; i++) o.swz[i] = i;
    if (nc == 2) {
        o.mode = (tok >> 2) & 3;
        if (o.mode == 0) o.mask = (tok >> 4) & 0xF;
        else if (o.mode == 1) for (int i = 0; i < 4; i++) o.swz[i] = (tok >> (4 + 2 * i)) & 3;
        else { int c = (tok >> 4) & 3; for (int i = 0; i < 4; i++) o.swz[i] = c; o.mask = 1u << c; }
    } else if (nc == 1) { for (int i = 0; i < 4; i++) o.swz[i] = 0; o.mask = 1; }
    if (tok & 0x80000000u) {
        uint32_t e;
        do { e = next(d); if ((e & 0x3F) == 1) o.mod = (e >> 6) & 0xFF; } while ((e & 0x80000000u) && !d->bad);
    }
    if (o.type == T_IMM32) {
        int n = nc == 1 ? 1 : 4;
        for (int i = 0; i < n; i++) o.imm[i] = next(d);
        if (n == 1) o.imm[1] = o.imm[2] = o.imm[3] = o.imm[0];
    } else if (o.type == T_IMM64) { d->bad = true; d->why = "double-precision values"; }
    for (int i = 0; i < o.dim && !d->bad; i++) {
        int rep = (tok >> (22 + 3 * i)) & 7;
        if (rep == 0) o.idx[i] = next(d);
        else if (rep == 1) { o.idx[i] = next(d); next(d); }
        else if (rep == 2) { int r = parse_opnd(d); o.rel[i] = r; }
        else if (rep == 3) { o.idx[i] = next(d); int r = parse_opnd(d); o.rel[i] = r; }
        else { d->bad = true; d->why = "unknown index form"; }
    }
    d->s->pool[k] = o;
    return k;
}

dxbc_shader_t *dxbc_load(const void *code, size_t len, char *err, int errlen) {
    #define FAIL(...) do { if (err) snprintf(err, errlen, __VA_ARGS__); dxbc_free(s); return NULL; } while (0)
    const uint8_t *b = code;
    dxbc_shader_t *s = NULL;
    if (!code || len < 32 || memcmp(b, "DXBC", 4)) FAIL("not DXBC bytecode");
    s = kzalloc(sizeof *s);
    if (!s) FAIL("out of memory");
    uint32_t nch = *(const uint32_t *)(b + 28);
    const uint32_t *prog = NULL; uint32_t plen = 0;
    if (32 + nch * 4 > len) FAIL("bad container");
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t off = *(const uint32_t *)(b + 32 + i * 4);
        if (off + 8 > len) FAIL("bad chunk");
        uint32_t cc = *(const uint32_t *)(b + off), sz = *(const uint32_t *)(b + off + 4);
        if (off + 8 + sz > len) FAIL("bad chunk size");
        const uint8_t *data = b + off + 8;
        if (!memcmp(&cc, "ISGN", 4)) s->nin = parse_sig(data, sz, s->in);
        else if (!memcmp(&cc, "OSGN", 4)) s->nout = parse_sig(data, sz, s->out);
        else if (!memcmp(&cc, "SHDR", 4) || !memcmp(&cc, "SHEX", 4)) { prog = (const uint32_t *)data; plen = sz / 4; }
    }
    if (!prog || plen < 2) FAIL("no shader program");
    s->type = (prog[0] >> 16) & 0xFFFF;
    if ((prog[0] >> 4 & 0xF) > 4) FAIL("shader model %u (only 4.x is supported)", prog[0] >> 4 & 0xF);
    if (s->type > 1) FAIL("geometry shaders are not supported");
    uint32_t end = MIN(prog[1], plen);
    dec_t d = { prog, 2, end, s, 0, false, NULL };
    int cap = 0;
    while (d.pos < end) {
        uint32_t at = d.pos, tok = prog[at];
        int op = tok & 0x7FF;
        uint32_t ilen = (tok >> 24) & 0x7F;
        if (op == OP_CUSTOMDATA) {
            ilen = at + 1 < end ? prog[at + 1] : 0;
            if (ilen < 2 || at + ilen > end) FAIL("bad custom data");
            if (((tok >> 11) & 0x1FFFFF) == 3) {          /* immediate constant buffer */
                int n = (ilen - 2) / 4;
                kfree(s->icb); s->icb = kmalloc(sizeof(vec4_t) * MAX(n, 1)); s->nicb = n;
                if (s->icb) memcpy(s->icb, prog + at + 2, sizeof(vec4_t) * n);
            }
            d.pos = at + ilen; continue;
        }
        if (!ilen || at + ilen > end) FAIL("bad instruction length at %u", at);
        if (op >= OP_DCL_FIRST && op <= OP_DCL_LAST) {
            if (op == OP_DCL_TEMPS) s->ntemps = prog[at + 1];
            else if (op == OP_DCL_INDEXABLE_TEMP && s->nxt < MAX_XTEMP) {
                s->xt[s->nxt].reg = prog[at + 1]; s->xt[s->nxt].size = prog[at + 2];
                s->xt_total += prog[at + 2]; s->nxt++;
            }
            d.pos = at + ilen; continue;
        }
        if (op > OP_XOR) FAIL("instruction %d is not Shader Model 4", op);
        if (op == OP_CALL || op == OP_CALLC || op == OP_LABEL || op == OP_SWITCH || op == OP_CASE || op == OP_DEFAULT ||
            op == OP_ENDSWITCH || op == OP_SAMPLE_C || op == OP_SAMPLE_C_LZ || op == OP_SAMPLE_D || op == OP_LD_MS ||
            op == OP_EMIT || op == OP_EMITTHENCUT || op == OP_CUT)
            FAIL("instruction %d is not supported", op);
        if (s->nins == cap) { cap = cap ? cap * 2 : 64; ins_t *n = krealloc(s->ins, sizeof(ins_t) * cap); if (!n) FAIL("out of memory"); s->ins = n; }
        ins_t *in = &s->ins[s->nins];
        memset(in, 0, sizeof *in);
        in->op = op; in->sat = (tok >> 13) & 1; in->nonzero = (tok >> 18) & 1; in->ret_type = (tok >> 11) & 3;
        in->jump = -1;
        d.pos = at + 1; d.end = at + ilen;
        if (tok & 0x80000000u) { uint32_t e; do e = next(&d); while ((e & 0x80000000u) && !d.bad); }
        while (d.pos < d.end && !d.bad && in->nops < 5) in->o[in->nops++] = parse_opnd(&d);
        if (d.bad) FAIL("%s", d.why ? d.why : "bad operand");
        d.end = end; d.pos = at + ilen;
        s->nins++;
    }
    /* jump targets */
    int stack[64], sp = 0, loops[64], lp = 0;
    for (int i = 0; i < s->nins; i++) {
        ins_t *in = &s->ins[i];
        switch (in->op) {
        case OP_IF: if (sp == 64) FAIL("nesting too deep"); stack[sp++] = i; break;
        case OP_ELSE: if (!sp) FAIL("else without if"); s->ins[stack[sp - 1]].jump = i + 1; stack[sp - 1] = i; break;
        case OP_ENDIF: if (!sp) FAIL("endif without if"); s->ins[stack[--sp]].jump = i + 1; break;
        case OP_LOOP: if (lp == 64) FAIL("nesting too deep"); loops[lp++] = i; break;
        case OP_ENDLOOP:
            if (!lp) FAIL("endloop without loop");
            in->jump = loops[--lp] + 1;
            for (int j = in->jump; j < i; j++) {         /* break/continue of this loop */
                ins_t *b = &s->ins[j];
                if ((b->op == OP_BREAK || b->op == OP_BREAKC) && b->jump < 0) b->jump = i + 1;
                if ((b->op == OP_CONTINUE || b->op == OP_CONTINUEC) && b->jump < 0) b->jump = i;
            }
            s->ins[loops[lp]].jump = i + 1;
            break;
        }
    }
    if (sp || lp) FAIL("unbalanced control flow");
    if (s->ntemps > 4096) FAIL("too many temporaries");
    for (int i = 0; i < s->npool; i++) {
        opnd_t *o = &s->pool[i];
        bool rel = o->rel[0] >= 0 || o->rel[1] >= 0 || o->rel[2] >= 0;
        if (rel) continue;
        if (o->type == T_IMM32) o->direct = 1;
        else if (o->type == T_TEMP) { if (o->idx[0] < 0 || o->idx[0] >= MAX(s->ntemps, 1)) FAIL("r%d is not declared", o->idx[0]); o->direct = 1; }
        else if ((o->type == T_INPUT || o->type == T_OUTPUT) && o->dim == 1) { if (o->idx[0] < 0 || o->idx[0] >= DX_MAX_REG) FAIL("register out of range"); o->direct = 1; }
        else if (o->type == T_CB && o->dim == 2) { if (o->idx[0] < 0 || o->idx[0] >= 14) FAIL("constant buffer out of range"); o->direct = 1; }
    }
    return s;
    #undef FAIL
}

/* ---------------- interpreter ---------------- */
typedef struct {
    const dxbc_shader_t *s; const dxbc_env_t *env;
    vec4_t *r, *x; const vec4_t *in; vec4_t *out;
    bool discard;
} run_t;
static const vec4_t zero4;

static int32_t index_of(run_t *R, const opnd_t *o, int k);
static vec4_t *reg_ptr(run_t *R, const opnd_t *o, bool write) {
    static vec4_t scratch;
    int i0 = o->dim > 0 ? index_of(R, o, 0) : 0;
    switch (o->type) {
    case T_TEMP: if (i0 >= 0 && i0 < MAX(R->s->ntemps, 1)) return &R->r[i0]; break;
    case T_INPUT: if (i0 >= 0 && i0 < DX_MAX_REG) return (vec4_t *)&R->in[i0]; break;
    case T_OUTPUT: if (i0 >= 0 && i0 < DX_MAX_REG) return &R->out[i0]; break;
    case T_ODEPTH: return &R->out[DX_MAX_REG - 1];
    case T_XTEMP: {
        int base = 0;
        for (int k = 0; k < R->s->nxt; k++) {
            if (R->s->xt[k].reg == i0) { int i1 = index_of(R, o, 1); if (i1 >= 0 && i1 < R->s->xt[k].size) return &R->x[base + i1]; break; }
            base += R->s->xt[k].size;
        }
        break;
    }
    case T_CB: if (!write) {
        int i1 = index_of(R, o, 1);
        if (i0 >= 0 && i0 < 14 && R->env->cb[i0] && i1 >= 0 && (uint32_t)i1 < R->env->cb_rows[i0]) return (vec4_t *)&R->env->cb[i0][i1];
    }
        break;
    case T_ICB: if (!write && i0 >= 0 && i0 < R->s->nicb) return &R->s->icb[i0]; break;
    }
    memset(&scratch, 0, sizeof scratch);
    return &scratch;
}
/* registers addressed by constants were range-checked when the shader was loaded */
static inline const uint32_t *fast_ptr(run_t *R, const opnd_t *o) {
    switch (o->type) {
    case T_TEMP: return R->r[o->idx[0]].u;
    case T_INPUT: return R->in[o->idx[0]].u;
    case T_OUTPUT: return R->out[o->idx[0]].u;
    case T_IMM32: return o->imm;
    case T_CB: return (uint32_t)o->idx[1] < R->env->cb_rows[o->idx[0]] ? R->env->cb[o->idx[0]][o->idx[1]].u : zero4.u;
    }
    return zero4.u;
}
static inline vec4_t fetch_raw(run_t *R, const opnd_t *o) {
    const uint32_t *p;
    if (o->direct) p = fast_ptr(R, o);
    else if (o->type == T_PRIMID) p = zero4.u;
    else p = reg_ptr(R, o, false)->u;
    vec4_t w;
    w.u[0] = p[o->swz[0]]; w.u[1] = p[o->swz[1]]; w.u[2] = p[o->swz[2]]; w.u[3] = p[o->swz[3]];
    return w;
}
static int32_t index_of(run_t *R, const opnd_t *o, int k) {
    int32_t v = o->idx[k];
    if (o->rel[k] >= 0) { vec4_t r = fetch_raw(R, &R->s->pool[o->rel[k]]); v += r.i[0]; }
    return v;
}
static inline vec4_t src_f(run_t *R, int k) {
    const opnd_t *o = &R->s->pool[k];
    vec4_t v = fetch_raw(R, o);
    if (!o->mod) return v;
    if (o->mod & 2) for (int i = 0; i < 4; i++) v.f[i] = v.f[i] < 0 ? -v.f[i] : v.f[i];
    if (o->mod & 1) for (int i = 0; i < 4; i++) v.f[i] = -v.f[i];
    return v;
}
static inline vec4_t src_i(run_t *R, int k) {
    const opnd_t *o = &R->s->pool[k];
    vec4_t v = fetch_raw(R, o);
    if (!o->mod) return v;
    if (o->mod & 2) for (int i = 0; i < 4; i++) v.i[i] = v.i[i] < 0 ? -v.i[i] : v.i[i];
    if (o->mod & 1) for (int i = 0; i < 4; i++) v.i[i] = -v.i[i];
    return v;
}
static inline void store(run_t *R, int k, vec4_t v, bool sat) {
    const opnd_t *o = &R->s->pool[k];
    if (o->type == T_NULL) return;
    vec4_t *d = o->direct && o->type == T_TEMP ? &R->r[o->idx[0]] : o->direct && o->type == T_OUTPUT ? &R->out[o->idx[0]] : reg_ptr(R, o, true);
    for (int i = 0; i < 4; i++) if (o->mask & (1u << i)) {
        if (sat) { float f = v.f[i]; v.f[i] = f > 1 ? 1 : f > 0 ? f : 0; }   /* NaN becomes 0 */
        d->u[i] = v.u[i];
    }
}
static float fr(float x) { return x - (float)floor(x); }

bool dxbc_run(const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out) {
    vec4_t rbuf[32], *heap = NULL;
    vec4_t *r = rbuf;
    int need = MAX(s->ntemps, 1) + s->xt_total;
    if (need > 32) { heap = kmalloc(sizeof(vec4_t) * need); if (!heap) return true; r = heap; }
    if (s->xt_total) memset(r + MAX(s->ntemps, 1), 0, sizeof(vec4_t) * s->xt_total);
    run_t R = { s, env, r, r + MAX(s->ntemps, 1), in, out, false };
    long budget = 1000000;
    for (int pc = 0; pc < s->nins && budget-- > 0;) {
        const ins_t *I = &s->ins[pc];
        vec4_t a, b, c, v;
        bool sat = I->sat;
        int next_pc = pc + 1;
        #define F1 a = src_f(&R, I->o[1])
        #define F2 F1; b = src_f(&R, I->o[2])
        #define F3 F2; c = src_f(&R, I->o[3])
        #define I1 a = src_i(&R, I->o[1])
        #define I2 I1; b = src_i(&R, I->o[2])
        #define I3 I2; c = src_i(&R, I->o[3])
        #define EACH(expr) for (int i = 0; i < 4; i++) { expr; }
        #define OUT store(&R, I->o[0], v, sat)
        #define COND(k) (src_i(&R, I->o[k]).u[0] != 0)
        switch (I->op) {
        case OP_MOV: v = src_f(&R, I->o[1]); OUT; break;
        case OP_MOVC: { vec4_t cnd = src_i(&R, I->o[1]); F3; (void)a; EACH(v.u[i] = cnd.u[i] ? b.u[i] : c.u[i]); sat = false; OUT; break; }
        case OP_ADD: F2; EACH(v.f[i] = a.f[i] + b.f[i]); OUT; break;
        case OP_MUL: F2; EACH(v.f[i] = a.f[i] * b.f[i]); OUT; break;
        case OP_DIV: F2; EACH(v.f[i] = a.f[i] / b.f[i]); OUT; break;
        case OP_MAD: F3; EACH(v.f[i] = a.f[i] * b.f[i] + c.f[i]); OUT; break;
        case OP_MIN: F2; EACH(v.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i]); OUT; break;
        case OP_MAX: F2; EACH(v.f[i] = a.f[i] > b.f[i] ? a.f[i] : b.f[i]); OUT; break;
        case OP_DP2: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1]; EACH(v.f[i] = d); } OUT; break;
        case OP_DP3: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2]; EACH(v.f[i] = d); } OUT; break;
        case OP_DP4: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2] + a.f[3] * b.f[3]; EACH(v.f[i] = d); } OUT; break;
        case OP_RSQ: F1; EACH(v.f[i] = 1.0f / (float)sqrt(a.f[i])); OUT; break;
        case OP_SQRT: F1; EACH(v.f[i] = (float)sqrt(a.f[i])); OUT; break;
        case OP_EXP: F1; EACH(v.f[i] = (float)exp2(a.f[i])); OUT; break;
        case OP_LOG: F1; EACH(v.f[i] = (float)log2(a.f[i])); OUT; break;
        case OP_FRC: F1; EACH(v.f[i] = fr(a.f[i])); OUT; break;
        case OP_ROUND_NE: F1; EACH(v.f[i] = (float)rint(a.f[i])); OUT; break;
        case OP_ROUND_NI: F1; EACH(v.f[i] = (float)floor(a.f[i])); OUT; break;
        case OP_ROUND_PI: F1; EACH(v.f[i] = (float)ceil(a.f[i])); OUT; break;
        case OP_ROUND_Z: F1; EACH(v.f[i] = (float)trunc(a.f[i])); OUT; break;
        case OP_SINCOS:
            a = src_f(&R, I->o[2]);
            EACH(v.f[i] = (float)sin(a.f[i])); store(&R, I->o[0], v, sat);
            EACH(v.f[i] = (float)cos(a.f[i])); store(&R, I->o[1], v, sat);
            break;
        case OP_DERIV_RTX: case OP_DERIV_RTY: v = zero4; OUT; break;
        case OP_EQ: F2; EACH(v.u[i] = a.f[i] == b.f[i] ? ~0u : 0); sat = false; OUT; break;
        case OP_NE: F2; EACH(v.u[i] = a.f[i] != b.f[i] ? ~0u : 0); sat = false; OUT; break;
        case OP_LT: F2; EACH(v.u[i] = a.f[i] < b.f[i] ? ~0u : 0); sat = false; OUT; break;
        case OP_GE: F2; EACH(v.u[i] = a.f[i] >= b.f[i] ? ~0u : 0); sat = false; OUT; break;
        case OP_IEQ: I2; EACH(v.u[i] = a.i[i] == b.i[i] ? ~0u : 0); OUT; break;
        case OP_INE: I2; EACH(v.u[i] = a.i[i] != b.i[i] ? ~0u : 0); OUT; break;
        case OP_ILT: I2; EACH(v.u[i] = a.i[i] < b.i[i] ? ~0u : 0); OUT; break;
        case OP_IGE: I2; EACH(v.u[i] = a.i[i] >= b.i[i] ? ~0u : 0); OUT; break;
        case OP_ULT: I2; EACH(v.u[i] = a.u[i] < b.u[i] ? ~0u : 0); OUT; break;
        case OP_UGE: I2; EACH(v.u[i] = a.u[i] >= b.u[i] ? ~0u : 0); OUT; break;
        case OP_AND: I2; EACH(v.u[i] = a.u[i] & b.u[i]); OUT; break;
        case OP_OR: I2; EACH(v.u[i] = a.u[i] | b.u[i]); OUT; break;
        case OP_XOR: I2; EACH(v.u[i] = a.u[i] ^ b.u[i]); OUT; break;
        case OP_NOT: I1; EACH(v.u[i] = ~a.u[i]); OUT; break;
        case OP_IADD: I2; EACH(v.u[i] = a.u[i] + b.u[i]); OUT; break;
        case OP_INEG: I1; EACH(v.u[i] = 0u - a.u[i]); OUT; break;
        case OP_IMAD: I3; EACH(v.i[i] = a.i[i] * b.i[i] + c.i[i]); OUT; break;
        case OP_UMAD: I3; EACH(v.u[i] = a.u[i] * b.u[i] + c.u[i]); OUT; break;
        case OP_IMAX: I2; EACH(v.i[i] = a.i[i] > b.i[i] ? a.i[i] : b.i[i]); OUT; break;
        case OP_IMIN: I2; EACH(v.i[i] = a.i[i] < b.i[i] ? a.i[i] : b.i[i]); OUT; break;
        case OP_UMAX: I2; EACH(v.u[i] = a.u[i] > b.u[i] ? a.u[i] : b.u[i]); OUT; break;
        case OP_UMIN: I2; EACH(v.u[i] = a.u[i] < b.u[i] ? a.u[i] : b.u[i]); OUT; break;
        case OP_ISHL: I2; EACH(v.u[i] = a.u[i] << (b.u[i] & 31)); OUT; break;
        case OP_ISHR: I2; EACH(v.i[i] = a.i[i] >> (b.u[i] & 31)); OUT; break;
        case OP_USHR: I2; EACH(v.u[i] = a.u[i] >> (b.u[i] & 31)); OUT; break;
        case OP_IMUL: case OP_UMUL: {
            a = src_i(&R, I->o[2]); b = src_i(&R, I->o[3]);
            vec4_t hi, lo;
            EACH(if (I->op == OP_IMUL) { int64_t p = (int64_t)a.i[i] * b.i[i]; hi.u[i] = (uint32_t)((uint64_t)p >> 32); lo.u[i] = (uint32_t)p; }
                 else { uint64_t p = (uint64_t)a.u[i] * b.u[i]; hi.u[i] = (uint32_t)(p >> 32); lo.u[i] = (uint32_t)p; });
            store(&R, I->o[0], hi, false); store(&R, I->o[1], lo, false);
            break;
        }
        case OP_UDIV: {
            a = src_i(&R, I->o[2]); b = src_i(&R, I->o[3]);
            vec4_t q, m;
            EACH(q.u[i] = b.u[i] ? a.u[i] / b.u[i] : ~0u; m.u[i] = b.u[i] ? a.u[i] % b.u[i] : ~0u);
            store(&R, I->o[0], q, false); store(&R, I->o[1], m, false);
            break;
        }
        case OP_ITOF: I1; EACH(v.f[i] = (float)a.i[i]); OUT; break;
        case OP_UTOF: I1; EACH(v.f[i] = (float)a.u[i]); OUT; break;
        case OP_FTOI: F1; EACH(v.i[i] = a.f[i] != a.f[i] ? 0 : a.f[i] >= 2147483520.0f ? 0x7FFFFFFF : a.f[i] <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)a.f[i]); OUT; break;
        case OP_FTOU: F1; EACH(v.u[i] = !(a.f[i] > 0) ? 0 : a.f[i] >= 4294967040.0f ? 0xFFFFFFFFu : (uint32_t)a.f[i]); OUT; break;
        case OP_SAMPLE: case OP_SAMPLE_L: case OP_SAMPLE_B: {
            F1;
            const opnd_t *t = &s->pool[I->o[2]], *sm = &s->pool[I->o[3]];
            float lod = I->op == OP_SAMPLE_L ? src_f(&R, I->o[4]).f[0] : 0, px[4] = { 0, 0, 0, 0 };
            if (env->sample) env->sample(env->ctx, t->idx[0], sm->idx[0], a.f, lod, px);
            EACH(v.f[i] = px[t->swz[i]]);
            OUT; break;
        }
        case OP_LD: {
            I1;
            const opnd_t *t = &s->pool[I->o[2]];
            float px[4] = { 0, 0, 0, 0 };
            if (env->load) env->load(env->ctx, t->idx[0], a.i, px);
            EACH(v.f[i] = px[t->swz[i]]);
            OUT; break;
        }
        case OP_RESINFO: {
            const opnd_t *t = &s->pool[I->o[2]];
            uint32_t dim[4] = { 1, 1, 1, 1 };
            if (env->size) env->size(env->ctx, t->idx[0], dim);
            vec4_t raw;
            EACH(if (I->ret_type == 2) raw.u[i] = dim[i]; else if (I->ret_type == 1) raw.f[i] = dim[i] ? 1.0f / dim[i] : 0; else raw.f[i] = (float)dim[i]);
            EACH(v.u[i] = raw.u[t->swz[i]]);
            OUT; break;
        }
        case OP_IF: if (COND(0) != I->nonzero) next_pc = I->jump; break;
        case OP_ELSE: next_pc = I->jump; break;            /* reached from the "then" part: skip to endif */
        case OP_ENDIF: case OP_NOP: case OP_LOOP: break;
        case OP_ENDLOOP: next_pc = I->jump; break;
        case OP_BREAK: case OP_CONTINUE: next_pc = I->jump; break;
        case OP_BREAKC: case OP_CONTINUEC: if (COND(0) == I->nonzero) next_pc = I->jump; break;
        case OP_DISCARD: if (COND(0) == I->nonzero) { R.discard = true; next_pc = s->nins; } break;
        case OP_RET: next_pc = s->nins; break;
        case OP_RETC: if (COND(0) == I->nonzero) next_pc = s->nins; break;
        default: break;
        }
        pc = next_pc;
    }
    kfree(heap);
    return !R.discard;
}
