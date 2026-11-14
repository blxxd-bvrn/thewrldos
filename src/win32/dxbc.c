/* Shader Model 4/5 bytecode for Direct3D 10 (d3d10.c): the DXBC container, its
 * input/output signatures, and an interpreter for vertex and pixel shaders.
 *
 * The program tokens are decoded once into a compact instruction list (operand
 * types, swizzles, masks, modifiers, relative indices, jump targets) and then
 * interpreted for every vertex and every pixel. Supported: the SM4 arithmetic,
 * integer, conversion and comparison instructions, if/else/loop/break/continue,
 * discard, ret, constant buffers (with relative indexing), immediate constant
 * buffers, indexable temporaries, sample/sample_l/sample_b/ld/resinfo. Not
 * supported: geometry shaders, switch, subroutines, comparison samplers and
 * screen-space derivatives. Unsupported instructions fail at upload. */
#include "shader_priv.h"

double sqrt(double); double sin(double); double cos(double); double exp2(double); double log2(double);

int dxbc_type(const dxbc_shader_t *s) { return s->type; }
int dxbc_inputs(const dxbc_shader_t *s, const dxsig_t **sig) { *sig = s->in; return s->nin; }
int dxbc_outputs(const dxbc_shader_t *s, const dxsig_t **sig) { *sig = s->out; return s->nout; }
int dxbc_model(const dxbc_shader_t *s, int *minor) { if (minor) *minor = s->minor; return s->major; }
bool dxbc_is_dxil(const dxbc_shader_t *s) { return s->dxil; }
int dxbc_read_mask(const dxbc_shader_t *s, int reg) { return reg >= 0 && reg < DX_MAX_REG ? s->read_mask[reg] : 0; }
int dxbc_interpolation(const dxbc_shader_t *s, int reg) { return reg >= 0 && reg < DX_MAX_REG ? s->interpolation[reg] : 2; }
bool dxbc_writes_depth(const dxbc_shader_t *s) { return s->writes_depth; }
void dxbc_free(dxbc_shader_t *s) { if (!s) return; kfree(s->ins); kfree(s->pool); kfree(s->icb); kfree(s); }

static uint32_t word(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int parse_sig(const uint8_t *d, uint32_t size, dxsig_t *sig, int stride) {
    if (size < 8) return -1;
    uint32_t n = word(d), table = word(d + 4);
    if (n > DX_MAX_SIG || table < 8 || table > size || n > (size - table) / stride) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = d + table + i * stride + (stride > 24 ? 4 : 0);
        if (stride > 24 && word(e - 4)) return -1;  /* stream zero only */
        uint32_t no = word(e);
        dxsig_t *s = &sig[i];
        memset(s, 0, sizeof *s);
        if (no >= size) return -1;
        uint32_t k = 0;
        while (k < size - no && d[no + k] && k < sizeof s->name - 1) { s->name[k] = d[no + k]; k++; }
        if (k == size - no || d[no + k]) return -1;
        s->index = word(e + 4); s->sysval = word(e + 8); s->component_type = word(e + 12);
        /* FXC's legacy OSGN encodes render targets with NONE; DXC's OSG1 uses
         * TARGET. Normalize the two container encodings for pipeline linkage. */
        if (!s->sysval && !strcasecmp(s->name, "SV_Target")) s->sysval = 64;
        if (!s->sysval && !strcasecmp(s->name, "SV_Depth")) s->sysval = 65;
        s->reg = word(e + 16); s->mask = word(e + 20) & 0xF;
        if (s->reg < 0 && (s->reg != -1 || s->sysval != 65)) return -1;
        if (s->reg >= DX_MAX_REG - 1 || !s->mask || s->component_type < 1 || s->component_type > 3) return -1;
        for (uint32_t j = 0; j < i; j++)
            if (sig[j].reg == s->reg && (sig[j].mask & s->mask)) return -1;
    }
    return n;
}

/* operand decoding */
typedef struct { const uint32_t *t; uint32_t pos, end; dxbc_shader_t *s; int cap, depth; bool bad; const char *why; } dec_t;
static int new_opnd(dec_t *d) {
    if (d->s->npool >= SHADER_MAX_OPERANDS) { d->bad = true; d->why = "too many operands"; return -1; }
    if (d->s->npool == d->cap) {
        d->cap = MIN(SHADER_MAX_OPERANDS, d->cap ? d->cap * 2 : 256);
        opnd_t *p = krealloc(d->s->pool, sizeof(opnd_t) * d->cap);
        if (!p) { d->bad = true; d->why = "out of memory"; return -1; }
        d->s->pool = p;
    }
    return d->s->npool++;
}
static uint32_t next(dec_t *d) { if (d->pos >= d->end) { d->bad = true; d->why = "truncated instruction"; return 0; } return d->t[d->pos++]; }
static int parse_opnd(dec_t *d) {
    if (d->bad || ++d->depth > 16) { d->bad = true; d->why = "operand nesting too deep"; return -1; }
    uint32_t tok = next(d);
    int k = new_opnd(d);
    if (k < 0) return -1;
    opnd_t o; memset(&o, 0, sizeof o);
    o.rel[0] = o.rel[1] = o.rel[2] = -1;
    int nc = tok & 3;
    if (nc == 3) { d->bad = true; d->why = "N-component operands"; return -1; }
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
        do {
            e = next(d);
            if ((e & 0x3F) == 1) o.mod = (e >> 6) & 0xFF;
            else if ((e & 0x3F) != 0) { d->bad = true; d->why = "extended operand form"; }
        } while ((e & 0x80000000u) && !d->bad);
    }
    if (o.type == T_IMM32) {
        int n = nc == 1 ? 1 : 4;
        for (int i = 0; i < n; i++) o.imm[i] = next(d);
        if (n == 1) o.imm[1] = o.imm[2] = o.imm[3] = o.imm[0];
    } else if (o.type == T_IMM64) { d->bad = true; d->why = "double-precision values"; }
    for (int i = 0; i < o.dim && !d->bad; i++) {
        int rep = (tok >> (22 + 3 * i)) & 7;
        if (rep == 0) o.idx[i] = next(d);
        else if (rep == 1) { o.idx[i] = next(d); if (next(d)) { d->bad = true; d->why = "64-bit register index"; } }
        else if (rep == 2) { int r = parse_opnd(d); o.rel[i] = r; }
        else if (rep == 3) { o.idx[i] = next(d); int r = parse_opnd(d); o.rel[i] = r; }
        else { d->bad = true; d->why = "unknown index form"; }
    }
    d->s->pool[k] = o;
    d->depth--;
    return k;
}

static int operand_count(int op, int *destinations) {
    *destinations = 1;
    switch (op) {
    case OP_MOV: case OP_INEG: case OP_NOT: case OP_FRC: case OP_EXP: case OP_LOG: case OP_RSQ: case OP_SQRT:
    case OP_ROUND_NE: case OP_ROUND_NI: case OP_ROUND_PI: case OP_ROUND_Z: case OP_ITOF: case OP_UTOF:
    case OP_FTOI: case OP_FTOU: case OP_RCP: case OP_F32TOF16: case OP_F16TOF32: case OP_COUNTBITS:
    case OP_FIRSTBIT_HI: case OP_FIRSTBIT_LO: case OP_FIRSTBIT_SHI: case OP_BFREV: return 2;
    case OP_ADD: case OP_MUL: case OP_DIV: case OP_DP2: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX:
    case OP_EQ: case OP_NE: case OP_LT: case OP_GE: case OP_IEQ: case OP_INE: case OP_ILT: case OP_IGE:
    case OP_ULT: case OP_UGE: case OP_AND: case OP_OR: case OP_XOR: case OP_IADD: case OP_ISHL: case OP_ISHR:
    case OP_USHR: case OP_IMIN: case OP_IMAX: case OP_UMIN: case OP_UMAX: case OP_LD: case OP_RESINFO:
    case OP_PHI: return 3;
    case OP_MOVC: case OP_MAD: case OP_IMAD: case OP_UMAD: case OP_UBFE: case OP_IBFE: case OP_SAMPLE: return 4;
    case OP_SAMPLE_L: case OP_SAMPLE_B: case OP_BFI: return 5;
    case OP_SINCOS: *destinations = 2; return 3;
    case OP_IMUL: case OP_UMUL: case OP_UDIV: case OP_UADDC: case OP_USUBB: *destinations = 2; return 4;
    case OP_SWAPC: *destinations = 2; return 5;
    case OP_IF: case OP_BREAKC: case OP_CONTINUEC: case OP_RETC: case OP_DISCARD: *destinations = 0; return 1;
    case OP_NOP: case OP_RET: case OP_BREAK: case OP_CONTINUE: case OP_LOOP: case OP_ENDLOOP:
    case OP_ELSE: case OP_ENDIF: case OP_BLOCK: *destinations = 0; return 0;
    case OP_BRANCH: *destinations = 0; return -2;       /* 0 or 1 */
    default: return -1;
    }
}

/* Relative-index children are reads too (including vN.x used only to address
 * a constant buffer). Their pool IDs strictly increase, so recursion is bounded. */
static void mark_read(dxbc_shader_t *s, int k, unsigned lanes) {
    const opnd_t *o = &s->pool[k];
    if (o->type == T_INPUT) {
        if (!o->direct) memset(s->read_mask, 15, sizeof s->read_mask);
        else for (int i = 0; i < 4; i++) if (lanes & (1u << i)) s->read_mask[o->idx[0]] |= 1u << o->swz[i];
    }
    for (int i = 0; i < o->dim; i++) if (o->rel[i] >= 0) mark_read(s, o->rel[i], 1);
}
static bool immediate_zero(const opnd_t *o) { return o->type == T_IMM32 && !o->mod && !(o->imm[o->swz[0]] & 0x7FFFFFFFu); }
static bool supported_semantic(const dxbc_shader_t *s, const dxsig_t *v, bool output) {
    if (s->type == 1) return output ? v->sysval == 0 || (v->sysval == 1 && v->mask == 15 && v->component_type == 3) :
                                         v->sysval == 0 || v->sysval == 6 || v->sysval == 8;
    return output ? (v->sysval == 64 && v->index == 0) || (v->sysval == 65 && v->component_type == 3) :
                    v->sysval == 0 || v->sysval == 1 || v->sysval == 7 || v->sysval == 9;
}

bool shader_finish(dxbc_shader_t *s, char *err, int errlen) {
    #define BAD(...) do { if (err && errlen > 0) snprintf(err, errlen, __VA_ARGS__); return false; } while (0)
    if (s->ntemps < 0 || s->ntemps > SHADER_MAX_TEMPS || s->xt_total < 0 || s->xt_total > SHADER_MAX_TEMPS) BAD("too many temporaries");
    if (!s->nins || s->nins > SHADER_MAX_INS || s->npool > SHADER_MAX_OPERANDS) BAD("invalid instruction/operand count");
    for (int i = 0; i < s->nin; i++) if (!supported_semantic(s, &s->in[i], false)) BAD("input system semantic %d is not implemented", s->in[i].sysval);
    for (int i = 0; i < s->nout; i++) if (!supported_semantic(s, &s->out[i], true)) BAD("output system semantic %d is not implemented", s->out[i].sysval);
    for (int i = 0; i < s->npool; i++) {
        opnd_t *o = &s->pool[i];
        if (o->mod > 3 || o->mode > 2 || o->dim > 2) BAD("unsupported operand form");
        for (int k = 0; k < o->dim; k++) if (o->rel[k] >= 0) {
            if (o->rel[k] <= i || o->rel[k] >= s->npool || s->pool[o->rel[k]].mod) BAD("invalid relative index");
        }
        bool relative = o->rel[0] >= 0 || o->rel[1] >= 0;
        switch (o->type) {
        case T_TEMP: case T_INPUT: case T_OUTPUT: case T_ICB:
            if (o->dim != 1) BAD("invalid register dimensions");
            if (!relative) {
                int n = o->type == T_TEMP ? s->ntemps : o->type == T_ICB ? s->nicb : DX_MAX_REG - 1;
                if (o->idx[0] < 0 || o->idx[0] >= n) BAD("register %d out of range", o->idx[0]);
                o->direct = 1;
            }
            break;
        case T_IMM32: if (o->dim) BAD("indexed immediate"); o->direct = 1; break;
        case T_CB:
            if (o->dim != 2 || o->rel[0] >= 0 || o->idx[0] < 0 || o->idx[0] >= 14) BAD("constant buffer out of range");
            if (!relative) o->direct = 1;
            break;
        case T_XTEMP: {
            if (o->dim != 2 || o->rel[0] >= 0) BAD("indexable temporary form");
            int k; for (k = 0; k < s->nxt && s->xt[k].reg != o->idx[0]; k++) {}
            if (k == s->nxt) BAD("undeclared indexable temporary");
            break;
        }
        case T_SAMPLER: case T_RESOURCE:
            if (o->dim != 1 || relative || o->idx[0] < 0 || o->idx[0] >= 16) BAD("texture/sampler binding out of range");
            break;
        case T_NULL: case T_ODEPTH: case T_PRIMID:
            if (o->dim) BAD("invalid scalar register dimensions");
            if (o->type == T_ODEPTH) { if (s->type != 0 || o->mask != 1) BAD("invalid depth output"); o->direct = 1; }
            break;
        default: BAD("operand type %u is not supported", o->type);
        }
        o->identity = o->swz[0] == 0 && o->swz[1] == 1 && o->swz[2] == 2 && o->swz[3] == 3;
    }
    bool returned = false;
    int control_depth = 0;
    uint8_t definite[DX_MAX_REG] = { 0 };
    uint32_t read_outputs = 0;
    for (int i = 0; i < s->nins; i++) {
        ins_t *I = &s->ins[i];
        int nd, n = operand_count(I->op, &nd);
        if (n == -1) BAD("instruction %u is not supported", I->op);
        if ((n >= 0 && I->nops != n) || (n == -2 && I->nops > 1)) BAD("wrong operand count for instruction %u", I->op);
        I->mask = 0;
        for (int j = 0; j < I->nops; j++) {
            if (I->o[j] < 0 || I->o[j] >= s->npool) BAD("instruction operand out of range");
            const opnd_t *o = &s->pool[I->o[j]];
            if (j < nd) {
                if (o->type != T_NULL && o->type != T_TEMP && o->type != T_OUTPUT && o->type != T_XTEMP && o->type != T_ODEPTH) BAD("invalid destination register");
                if (o->mod) BAD("modified destination register");
                if (o->type != T_NULL) I->mask |= o->mask;
                if (o->type == T_OUTPUT) {
                    if (o->direct) { s->write_mask[o->idx[0]] |= o->mask; if (!returned && !control_depth) definite[o->idx[0]] |= o->mask; }
                    else memset(s->write_mask, 15, sizeof s->write_mask);
                }
                if (o->type == T_ODEPTH) { if (s->type != 0) BAD("depth output outside pixel shader"); s->writes_depth = true; s->write_mask[DX_MAX_REG - 1] |= 1; }
            } else {
                bool texture = (I->op == OP_SAMPLE || I->op == OP_SAMPLE_L || I->op == OP_SAMPLE_B || I->op == OP_LD || I->op == OP_RESINFO) && j == 2;
                bool sampler = (I->op == OP_SAMPLE || I->op == OP_SAMPLE_L || I->op == OP_SAMPLE_B) && j == 3;
                if (o->type == T_NULL || o->type == T_ODEPTH || (o->type == T_RESOURCE && !texture) || (o->type == T_SAMPLER && !sampler)) BAD("invalid source register");
                if (o->type == T_OUTPUT) read_outputs |= o->direct ? 1u << o->idx[0] : ~0u;
            }
        }
        if (!nd) I->mask = 15;
        if (I->op == OP_DISCARD && s->type != 0) BAD("discard outside pixel shader");
        for (int j = 0; j < I->nops; j++) {
            const opnd_t *o = &s->pool[I->o[j]];
            if (j < nd) { for (int k = 0; k < o->dim; k++) if (o->rel[k] >= 0) mark_read(s, o->rel[k], 1); continue; }
            unsigned lanes = I->mask;
            if (I->op == OP_DP2) lanes = 3;
            else if (I->op == OP_DP3) lanes = 7;
            else if (I->op == OP_DP4) lanes = 15;
            else if (!nd || (I->op == OP_SAMPLE_L && j == 4) || (I->op == OP_SAMPLE_B && j == 4) || I->op == OP_RESINFO) lanes = 1;
            else if (I->op == OP_SAMPLE || I->op == OP_SAMPLE_L || I->op == OP_SAMPLE_B || I->op == OP_LD) lanes = 3;
            mark_read(s, I->o[j], lanes);
        }
        if (I->op == OP_SAMPLE || I->op == OP_SAMPLE_L || I->op == OP_SAMPLE_B) {
            if (s->pool[I->o[2]].type != T_RESOURCE || s->pool[I->o[3]].type != T_SAMPLER) BAD("invalid sampling operands");
            if (s->major >= 5 && I->op != OP_SAMPLE && !immediate_zero(&s->pool[I->o[4]])) BAD("the SM5/6 subset supports only constant LOD/bias zero");
        } else if (I->op == OP_LD || I->op == OP_RESINFO) {
            if (s->pool[I->o[2]].type != T_RESOURCE) BAD("invalid texture operand");
            const opnd_t *level = &s->pool[I->o[1]];
            if (s->major >= 5 && (I->op == OP_LD || level->type != T_IMM32 || level->mod || level->imm[level->swz[0]])) BAD("modern texel loads/nonzero LOD queries are not implemented");
        }
        if (I->op == OP_IF || I->op == OP_LOOP) control_depth++;
        else if (I->op == OP_ENDIF || I->op == OP_ENDLOOP) control_depth--;
        if (I->op == OP_RET || I->op == OP_RETC) returned = true;
    }
    /* DXIL supplies a CFG intersection over all return blocks. DXBC can still
     * omit clears for unconditional writes after an if/else, unless a return
     * may have bypassed them. Partial outputs retain deterministic zero lanes. */
    if (!s->dxil) memcpy(s->definite_mask, definite, sizeof definite);
    s->nclear = 0;
    for (int i = 0; i < DX_MAX_REG; i++) if (s->write_mask[i] && (s->definite_mask[i] != 15 || (read_outputs & (1u << i)))) s->clear_regs[s->nclear++] = i;
    return true;
    #undef BAD
}

dxbc_shader_t *dxbc_load(const void *code, size_t len, char *err, int errlen) {
    const uint8_t *b = code;
    dxbc_shader_t *s = NULL;
    uint32_t *owned = NULL;
    #define FAIL(...) do { if (err && errlen > 0) snprintf(err, errlen, __VA_ARGS__); kfree(owned); dxbc_free(s); return NULL; } while (0)
    if (!code || len < 32 || memcmp(b, "DXBC", 4)) FAIL("not DXBC bytecode");
    uint32_t total = word(b + 24), nch = word(b + 28);
    if (word(b + 20) != 1 || total < 32 || total > len || total > 16u * 1024 * 1024 || nch > (total - 32) / 4 || nch > 256) FAIL("bad container");
    s = kzalloc(sizeof *s);
    if (!s) FAIL("out of memory");
    memset(s->interpolation, 2, sizeof s->interpolation);
    const uint8_t *program = NULL, *dxil = NULL; uint32_t plen = 0, dxil_size = 0;
    bool input_sig = false, output_sig = false;
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t off = word(b + 32 + i * 4);
        if ((off & 3) || off < 32 + nch * 4 || off > total || total - off < 8) FAIL("bad chunk");
        const uint8_t *cc = b + off;
        uint32_t sz = word(b + off + 4);
        if (sz > total - off - 8) FAIL("bad chunk size");
        const uint8_t *data = b + off + 8;
        if (!memcmp(cc, "ISGN", 4) || !memcmp(cc, "ISG1", 4)) {
            if (input_sig) FAIL("duplicate input signature"); input_sig = true;
            s->nin = parse_sig(data, sz, s->in, cc[3] == '1' ? 32 : 24);
        } else if (!memcmp(cc, "OSGN", 4) || !memcmp(cc, "OSG1", 4) || !memcmp(cc, "OSG5", 4)) {
            if (output_sig) FAIL("duplicate output signature"); output_sig = true;
            s->nout = parse_sig(data, sz, s->out, cc[3] == '1' ? 32 : cc[3] == '5' ? 28 : 24);
        } else if (!memcmp(cc, "SHDR", 4) || !memcmp(cc, "SHEX", 4)) {
            if (program || (sz & 3) || sz > 4u * 1024 * 1024) FAIL("invalid shader program chunk");
            program = data; plen = sz / 4;
        } else if (!memcmp(cc, "DXIL", 4)) {
            if (dxil) FAIL("duplicate DXIL program");
            dxil = data; dxil_size = sz;
        }
        if (s->nin < 0 || s->nout < 0) FAIL("invalid or unsupported shader signature");
    }
    if (dxil) {
        if (program) FAIL("mixed DXBC and DXIL programs");
        if (!dxil_load(dxil, dxil_size, s, err, errlen) || !shader_finish(s, err, errlen)) { dxbc_free(s); return NULL; }
        return s;
    }
    if (!program || plen < 2) FAIL("no shader program");
    owned = kmalloc(plen * 4);
    if (!owned) FAIL("out of memory");
    memcpy(owned, program, plen * 4);       /* accepts unaligned caller byte arrays */
    const uint32_t *prog = owned;
    s->type = (prog[0] >> 16) & 0xFFFF;
    s->major = prog[0] >> 4 & 15; s->minor = prog[0] & 15;
    if (!((s->major == 4 && s->minor <= 1) || (s->major == 5 && s->minor == 0))) FAIL("shader model %d.%d is not a supported DXBC profile", s->major, s->minor);
    if (s->type > 1) FAIL("only vertex and pixel shader stages are implemented");
    uint32_t end = prog[1];
    if (end < 2 || end > plen) FAIL("truncated shader program");
    dec_t d = { .t = prog, .pos = 2, .end = end, .s = s };
    int cap = 0;
    while (d.pos < end) {
        uint32_t at = d.pos, tok = prog[at];
        int op = tok & 0x7FF;
        uint32_t ilen = (tok >> 24) & 0x7F;
        if (op == OP_CUSTOMDATA) {
            ilen = at + 1 < end ? prog[at + 1] : 0;
            if (ilen < 2 || ilen > end - at) FAIL("bad custom data");
            if (((tok >> 11) & 0x1FFFFF) == 3) {          /* immediate constant buffer */
                int n = (ilen - 2) / 4;
                if ((ilen - 2) % 4 || n > 4096) FAIL("bad immediate constant buffer");
                kfree(s->icb); s->icb = kmalloc(sizeof(vec4_t) * MAX(n, 1)); s->nicb = n;
                if (!s->icb) FAIL("out of memory");
                memcpy(s->icb, prog + at + 2, sizeof(vec4_t) * n);
            }
            d.pos = at + ilen; continue;
        }
        if (!ilen || ilen > end - at) FAIL("bad instruction length at %u", at);
        if (op >= OP_DCL_FIRST && op <= OP_DCL_LAST) {
            if (op == OP_DCL_TEMPS) {
                if (ilen != 2 || prog[at + 1] > SHADER_MAX_TEMPS) FAIL("invalid temporary declaration");
                s->ntemps = prog[at + 1];
            } else if (op == OP_DCL_INDEXABLE_TEMP) {
                if (ilen != 4 || s->nxt == MAX_XTEMP || !prog[at + 2] || prog[at + 2] > SHADER_MAX_TEMPS - s->xt_total ||
                    !prog[at + 3] || prog[at + 3] > 4) FAIL("invalid indexable temporary declaration");
                for (int k = 0; k < s->nxt; k++) if ((uint32_t)s->xt[k].reg == prog[at + 1]) FAIL("duplicate indexable temporary");
                s->xt[s->nxt].reg = prog[at + 1]; s->xt[s->nxt].size = prog[at + 2];
                s->xt_total += prog[at + 2]; s->nxt++;
            } else if (op == OP_DCL_LAST) {
                if (ilen != 1 || (tok & 0x00FFF800u & ~(0x800u | 0x8000u | 0x10000u))) FAIL("unsupported global shader requirements");
            } else {
                if (op >= 92 && op <= 94) FAIL("geometry shader declaration");
                d.pos = at + 1; d.end = at + ilen; d.depth = 0;
                if (tok & 0x80000000u) FAIL("extended declaration is not supported");
                int oi = parse_opnd(&d);
                if (d.bad || oi < 0) FAIL("bad declaration operand");
                const opnd_t *o = &s->pool[oi];
                if (op == 88) {
                    if (((tok >> 11) & 31) != 3 || o->type != T_RESOURCE || d.pos + 1 != d.end) FAIL("only Texture2D resources are implemented");
                    uint32_t ret = prog[d.pos];
                    for (int k = 0; k < 4; k++) {
                        int t = (ret >> (k * 4)) & 15;
                        if (t != 1 && t != 2 && t != 5) FAIL("only floating/normalized Texture2D resources are implemented");
                    }
                } else if (op == 89) {
                    if (o->type != T_CB || o->dim != 2 || o->idx[1] < 0 || o->idx[1] > 4096 || d.pos != d.end) FAIL("bad constant buffer declaration");
                } else if (op == 90) {
                    if (o->type != T_SAMPLER || ((tok >> 11) & 15) != 0 || d.pos != d.end) FAIL("comparison samplers are not implemented");
                } else if (op == 91) {
                    if (d.pos + 1 != d.end || !prog[d.pos]) FAIL("bad index range declaration");
                } else {
                    bool input = op >= 95 && op <= 100;
                    bool system = op == 96 || op == 97 || op == 99 || op == 100 || op == 102 || op == 103;
                    if ((input && o->type != T_INPUT && o->type != T_PRIMID) ||
                        (!input && o->type != T_OUTPUT && o->type != T_ODEPTH) || d.pos + (system ? 1 : 0) != d.end) FAIL("bad input/output declaration");
                    if (op >= 98 && op <= 100 && o->idx[0] >= 0 && o->idx[0] < DX_MAX_REG) {
                        int mode = (tok >> 11) & 15;
                        if (mode < 1 || mode > 5) FAIL("sample-frequency interpolation is not implemented");
                        int m = mode == 1 ? 1 : mode >= 4 ? 4 : 2;
                        if ((s->interpolation_declared & (1u << o->idx[0])) && s->interpolation[o->idx[0]] != m) FAIL("mixed interpolation within a register is not implemented");
                        s->interpolation_declared |= 1u << o->idx[0]; s->interpolation[o->idx[0]] = m;
                    }
                }
            }
            d.end = end;
            d.pos = at + ilen; continue;
        }
        int destinations;
        if (op >= OP_BLOCK || operand_count(op, &destinations) < 0 || (s->major == 4 && op > OP_XOR)) FAIL("instruction %d is not implemented for this profile", op);
        if (s->nins == SHADER_MAX_INS) FAIL("too many instructions");
        if (s->nins == cap) { cap = MIN(SHADER_MAX_INS, cap ? cap * 2 : 64); ins_t *n = krealloc(s->ins, sizeof(ins_t) * cap); if (!n) FAIL("out of memory"); s->ins = n; }
        ins_t *in = &s->ins[s->nins];
        memset(in, 0, sizeof *in);
        in->op = op; in->sat = (tok >> 13) & 1; in->nonzero = (tok >> 18) & 1; in->ret_type = (tok >> 11) & 3;
        in->jump = in->alt = -1;
        d.pos = at + 1; d.end = at + ilen;
        if (tok & 0x80000000u) {
            uint32_t e;
            do {
                e = next(&d);
                /* Resource dimension/type hints are redundant; sample offsets
                 * are not, so do not silently ignore a nonzero texel offset. */
                if ((e & 63) == 1 && (e & 0x001FFE00u)) { d.bad = true; d.why = "sample texel offsets are not implemented"; }
                else if ((e & 63) > 3) { d.bad = true; d.why = "extended opcode form"; }
            } while ((e & 0x80000000u) && !d.bad);
        }
        while (d.pos < d.end && !d.bad && in->nops < 5) in->o[in->nops++] = parse_opnd(&d);
        if (d.bad || d.pos != d.end) FAIL("%s", d.why ? d.why : "bad operand count");
        d.end = end; d.pos = at + ilen;
        s->nins++;
    }
    /* jump targets */
    int stack[64], sp = 0;
    for (int i = 0; i < s->nins; i++) {
        ins_t *in = &s->ins[i];
        switch (in->op) {
        case OP_IF: if (sp == 64) FAIL("nesting too deep"); stack[sp++] = i; break;
        case OP_ELSE: if (!sp || s->ins[stack[sp - 1]].op != OP_IF) FAIL("else without if"); s->ins[stack[sp - 1]].jump = i + 1; stack[sp - 1] = i; break;
        case OP_ENDIF:
            if (!sp || (s->ins[stack[sp - 1]].op != OP_IF && s->ins[stack[sp - 1]].op != OP_ELSE)) FAIL("endif without matching if");
            s->ins[stack[--sp]].jump = i + 1; break;
        case OP_LOOP: if (sp == 64) FAIL("nesting too deep"); stack[sp++] = i; break;
        case OP_ENDLOOP:
            if (!sp || s->ins[stack[sp - 1]].op != OP_LOOP) FAIL("endloop without matching loop");
            in->jump = stack[--sp] + 1;
            for (int j = in->jump; j < i; j++) {         /* break/continue of this loop */
                ins_t *b = &s->ins[j];
                if ((b->op == OP_BREAK || b->op == OP_BREAKC) && b->jump < 0) b->jump = i + 1;
                if ((b->op == OP_CONTINUE || b->op == OP_CONTINUEC) && b->jump < 0) b->jump = i;
            }
            s->ins[stack[sp]].jump = i + 1;
            break;
        }
    }
    if (sp) FAIL("unbalanced control flow");
    for (int i = 0; i < s->nins; i++) {
        int op = s->ins[i].op;
        if ((op == OP_BREAK || op == OP_BREAKC || op == OP_CONTINUE || op == OP_CONTINUEC) && s->ins[i].jump < 0) FAIL("break/continue outside loop");
    }
    kfree(owned); owned = NULL;
    if (!shader_finish(s, err, errlen)) { dxbc_free(s); return NULL; }
    return s;
    #undef FAIL
}

/* ---------------- interpreter ---------------- */
typedef dxbc_exec_t run_t;
static const vec4_t zero4;

static int32_t index_of(run_t *R, const opnd_t *o, int k);
static vec4_t *reg_ptr(run_t *R, const opnd_t *o, bool write) {
    int i0 = o->dim > 0 ? index_of(R, o, 0) : 0;
    switch (o->type) {
    case T_TEMP: if (i0 >= 0 && i0 < MAX(R->shader->ntemps, 1)) return &R->r[i0]; break;
    case T_INPUT: if (i0 >= 0 && i0 < DX_MAX_REG) return (vec4_t *)&R->in[i0]; break;
    case T_OUTPUT: if (i0 >= 0 && i0 < DX_MAX_REG) return &R->out[i0]; break;
    case T_ODEPTH: return &R->out[DX_MAX_REG - 1];
    case T_XTEMP: {
        int base = 0;
        for (int k = 0; k < R->shader->nxt; k++) {
            if (R->shader->xt[k].reg == i0) { int i1 = index_of(R, o, 1); if (i1 >= 0 && i1 < R->shader->xt[k].size) return &R->x[base + i1]; break; }
            base += R->shader->xt[k].size;
        }
        break;
    }
    case T_CB: if (!write) {
        int i1 = index_of(R, o, 1);
        if (i0 >= 0 && i0 < 14 && R->env->cb[i0] && i1 >= 0 && (uint32_t)i1 < R->env->cb_rows[i0]) return (vec4_t *)&R->env->cb[i0][i1];
    }
        break;
    case T_ICB: if (!write && i0 >= 0 && i0 < R->shader->nicb) return &R->shader->icb[i0]; break;
    case T_PRIMID: if (!write) { R->scratch.u[0] = R->env->primitive_id; return &R->scratch; } break;
    }
    memset(&R->scratch, 0, sizeof R->scratch);
    return &R->scratch;
}
/* registers addressed by constants were range-checked when the shader was loaded */
static inline const uint32_t *fast_ptr(run_t *R, const opnd_t *o) {
    switch (o->type) {
    case T_TEMP: return R->r[o->idx[0]].u;
    case T_INPUT: return R->in[o->idx[0]].u;
    case T_OUTPUT: return R->out[o->idx[0]].u;
    case T_IMM32: return o->imm;
    case T_CB: return R->env->cb[o->idx[0]] && o->idx[1] >= 0 && (uint32_t)o->idx[1] < R->env->cb_rows[o->idx[0]] ? R->env->cb[o->idx[0]][o->idx[1]].u : zero4.u;
    case T_ICB: return R->shader->icb[o->idx[0]].u;
    case T_ODEPTH: return R->out[DX_MAX_REG - 1].u;
    }
    return zero4.u;
}
static inline vec4_t fetch_raw(run_t *R, const opnd_t *o) {
    const uint32_t *p;
    if (o->direct) p = R->bound[o - R->shader->pool];
    else p = reg_ptr(R, o, false)->u;
    vec4_t w;
    if (o->identity) { w.u[0] = p[0]; w.u[1] = p[1]; w.u[2] = p[2]; w.u[3] = p[3]; return w; }
    w.u[0] = p[o->swz[0]]; w.u[1] = p[o->swz[1]]; w.u[2] = p[o->swz[2]]; w.u[3] = p[o->swz[3]];
    return w;
}
static int32_t index_of(run_t *R, const opnd_t *o, int k) {
    int32_t v = o->idx[k];
    if (o->rel[k] >= 0) { vec4_t r = fetch_raw(R, &R->shader->pool[o->rel[k]]); v = (int32_t)((uint32_t)v + r.u[0]); }
    return v;
}
static inline vec4_t src_f(run_t *R, int k) {
    const opnd_t *o = &R->shader->pool[k];
    vec4_t v = fetch_raw(R, o);
    if (!o->mod) return v;
    if (o->mod & 2) for (int i = 0; i < 4; i++) v.u[i] &= 0x7FFFFFFFu;
    if (o->mod & 1) for (int i = 0; i < 4; i++) v.u[i] ^= 0x80000000u;
    return v;
}
static inline vec4_t src_i(run_t *R, int k) {
    const opnd_t *o = &R->shader->pool[k];
    vec4_t v = fetch_raw(R, o);
    if (!o->mod) return v;
    if (o->mod & 2) for (int i = 0; i < 4; i++) if (v.i[i] < 0) v.u[i] = 0u - v.u[i];
    if (o->mod & 1) for (int i = 0; i < 4; i++) v.u[i] = 0u - v.u[i];
    return v;
}
static inline void store(run_t *R, int k, vec4_t v, bool sat) {
    const opnd_t *o = &R->shader->pool[k];
    if (o->type == T_NULL) return;
    vec4_t *d = o->direct && o->type == T_TEMP ? &R->r[o->idx[0]] : o->direct && o->type == T_OUTPUT ? &R->out[o->idx[0]] : reg_ptr(R, o, true);
    if (o->mask == 15 && !sat) { *d = v; return; }
    for (int i = 0; i < 4; i++) if (o->mask & (1u << i)) {
        if (sat) { float f = v.f[i]; v.f[i] = f > 1 ? 1 : f > 0 ? f : 0; }   /* NaN becomes 0 */
        d->u[i] = v.u[i];
    }
}
/* Float32 rounding without changing x87 control words per component/pixel. */
static float round32(float x, int mode) {
    union { float f; uint32_t u; } v = { x };
    uint32_t magnitude = v.u & 0x7FFFFFFFu, sign = v.u >> 31;
    if (magnitude >= 0x4B000000u || !magnitude) return x; /* integral, infinity, NaN or signed zero */
    if (magnitude < 0x3F800000u) {
        v.u &= 0x80000000u;
        if ((mode == 1 && sign) || (mode == 2 && !sign) || (mode == 0 && magnitude > 0x3F000000u)) return sign ? -1 : 1;
        return v.f;
    }
    unsigned shift = 150 - ((magnitude >> 23) & 255);
    uint32_t mask = (1u << shift) - 1, fraction = v.u & mask, half = 1u << (shift - 1);
    bool increment = fraction && ((mode == 1 && sign) || (mode == 2 && !sign) ||
                     (mode == 0 && (fraction > half || (fraction == half && (v.u & (1u << shift))))));
    v.u &= ~mask;
    return increment ? v.f + (sign ? -1.0f : 1.0f) : v.f;
}
static float fr(float x) { return x - round32(x, 1); }
static uint32_t reverse_bits(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
static uint32_t count_bits(uint32_t v) {
    v -= (v >> 1) & 0x55555555u;
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}
static uint32_t half_bits(uint32_t v) {
    uint32_t sign = (v >> 16) & 0x8000, m = v & 0x7FFFFF;
    int e = (int)((v >> 23) & 255) - 127 + 15;
    if (((v >> 23) & 255) == 255) return sign | 0x7C00 | (m ? (m >> 13) | 0x200 : 0);
    if (e >= 31) return sign | 0x7C00;
    if (e <= 0) {
        if (e < -10) return sign;
        m |= 0x800000;
        int shift = 14 - e;
        uint32_t q = m >> shift, rest = m & ((1u << shift) - 1), mid = 1u << (shift - 1);
        return sign | (q + (rest > mid || (rest == mid && (q & 1))));
    }
    return sign | (((uint32_t)e << 10) + ((m + 0xFFF + ((m >> 13) & 1)) >> 13));
}
static uint32_t float_bits(uint32_t h) {
    uint32_t sign = (h & 0x8000) << 16, m = h & 1023, e = (h >> 10) & 31;
    if (!e) {
        if (!m) return sign;
        int power = -14;
        while (!(m & 1024)) { m <<= 1; power--; }
        return sign | ((uint32_t)(power + 127) << 23) | ((m & 1023) << 13);
    }
    if (e == 31) return sign | 0x7F800000u | (m << 13) | (m ? 0x400000 : 0);
    return sign | ((e + 112) << 23) | (m << 13);
}

bool dxbc_exec_init(dxbc_exec_t *R, const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out) {
    if (!R || !s || !env || !in || !out) return false;
    memset(R, 0, sizeof *R);
    int need = MAX(s->ntemps, 1) + s->xt_total;
    size_t rb = sizeof(vec4_t) * need, bindings = sizeof(uint32_t *) * s->npool;
    R->storage = kmalloc(rb + bindings);
    if (!R->storage) return false;
    R->shader = s; R->env = env; R->in = in; R->out = out;
    R->r = R->storage; R->x = R->r + MAX(s->ntemps, 1);
    R->bound = (const uint32_t **)((uint8_t *)R->storage + rb);
    memset(R->r, 0, rb);
    for (int i = 0; i < s->npool; i++) R->bound[i] = s->pool[i].direct ? fast_ptr(R, &s->pool[i]) : NULL;
    return true;
}
void dxbc_exec_fini(dxbc_exec_t *R) { if (R) { kfree(R->storage); memset(R, 0, sizeof *R); } }

bool dxbc_exec_run(dxbc_exec_t *exec) {
    const dxbc_shader_t *s = exec->shader;
    const dxbc_env_t *env = exec->env;
    #define R (*exec)
    R.discard = false; R.block = R.previous_block = -1;
    if (s->xt_total) memset(R.x, 0, sizeof(vec4_t) * s->xt_total);
    for (int i = 0; i < s->nclear; i++) memset(&R.out[s->clear_regs[i]], 0, sizeof(vec4_t));
    long budget = 1000000;
    for (int pc = 0; pc < s->nins && budget-- > 0;) {
        const ins_t *I = &s->ins[pc];
        vec4_t a, b, c, v = { 0 };
        bool sat = I->sat;
        int next_pc = pc + 1;
        #define F1 a = src_f(&R, I->o[1])
        #define F2 F1; b = src_f(&R, I->o[2])
        #define F3 F2; c = src_f(&R, I->o[3])
        #define I1 a = src_i(&R, I->o[1])
        #define I2 I1; b = src_i(&R, I->o[2])
        #define I3 I2; c = src_i(&R, I->o[3])
        #define EACH(...) do { if (I->mask == 1) { int i = 0; __VA_ARGS__; } \
            else if (I->mask == 15) { for (int i = 0; i < 4; i++) { __VA_ARGS__; } } \
            else { for (int i = 0; i < 4; i++) if (I->mask & (1u << i)) { __VA_ARGS__; } } } while (0)
        #define OUT store(&R, I->o[0], v, sat)
        #define COND(k) (src_i(&R, I->o[k]).u[0] != 0)
        switch (I->op) {
        case OP_MOV: v = src_f(&R, I->o[1]); OUT; break;
        case OP_MOVC: { vec4_t cnd = src_i(&R, I->o[1]); F3; (void)a; EACH(v.u[i] = cnd.u[i] ? b.u[i] : c.u[i]); OUT; break; }
        case OP_ADD: F2; EACH(v.f[i] = a.f[i] + b.f[i]); OUT; break;
        case OP_MUL: F2; EACH(v.f[i] = a.f[i] * b.f[i]); OUT; break;
        case OP_DIV: F2; EACH(v.f[i] = a.f[i] / b.f[i]); OUT; break;
        case OP_MAD: F3; EACH(v.f[i] = a.f[i] * b.f[i] + c.f[i]); OUT; break;
        case OP_MIN: F2; EACH(v.f[i] = b.f[i] != b.f[i] || a.f[i] < b.f[i] ? a.f[i] : b.f[i]); OUT; break;
        case OP_MAX: F2; EACH(v.f[i] = b.f[i] != b.f[i] || a.f[i] > b.f[i] ? a.f[i] : b.f[i]); OUT; break;
        case OP_DP2: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1]; EACH(v.f[i] = d); } OUT; break;
        case OP_DP3: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2]; EACH(v.f[i] = d); } OUT; break;
        case OP_DP4: F2; { float d = a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2] + a.f[3] * b.f[3]; EACH(v.f[i] = d); } OUT; break;
        case OP_RSQ: F1; EACH(v.f[i] = 1.0f / (float)sqrt(a.f[i])); OUT; break;
        case OP_SQRT: F1; EACH(v.f[i] = (float)sqrt(a.f[i])); OUT; break;
        case OP_EXP: F1; EACH(v.f[i] = (float)exp2(a.f[i])); OUT; break;
        case OP_LOG: F1; EACH(v.f[i] = (float)log2(a.f[i])); OUT; break;
        case OP_FRC: F1; EACH(v.f[i] = fr(a.f[i])); OUT; break;
        case OP_ROUND_NE: F1; EACH(v.f[i] = round32(a.f[i], 0)); OUT; break;
        case OP_ROUND_NI: F1; EACH(v.f[i] = round32(a.f[i], 1)); OUT; break;
        case OP_ROUND_PI: F1; EACH(v.f[i] = round32(a.f[i], 2)); OUT; break;
        case OP_ROUND_Z: F1; EACH(v.f[i] = round32(a.f[i], 3)); OUT; break;
        case OP_SINCOS:
            a = src_f(&R, I->o[2]);
            if (s->pool[I->o[0]].type != T_NULL) {
                for (int i = 0; i < 4; i++) if (s->pool[I->o[0]].mask & (1u << i)) v.f[i] = (float)sin(a.f[i]);
                store(&R, I->o[0], v, sat);
            }
            if (s->pool[I->o[1]].type != T_NULL) {
                for (int i = 0; i < 4; i++) if (s->pool[I->o[1]].mask & (1u << i)) v.f[i] = (float)cos(a.f[i]);
                store(&R, I->o[1], v, sat);
            }
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
        case OP_IMAD: I3; EACH(v.u[i] = a.u[i] * b.u[i] + c.u[i]); OUT; break;
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
        case OP_FTOI: F1; EACH(v.i[i] = a.f[i] != a.f[i] ? 0 : a.f[i] >= 2147483648.0f ? 0x7FFFFFFF : a.f[i] <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)a.f[i]); OUT; break;
        case OP_FTOU: F1; EACH(v.u[i] = !(a.f[i] > 0) ? 0 : a.f[i] >= 4294967296.0f ? 0xFFFFFFFFu : (uint32_t)a.f[i]); OUT; break;
        case OP_RCP: F1; EACH(v.f[i] = 1.0f / a.f[i]); OUT; break;
        case OP_F32TOF16: F1; EACH(v.u[i] = half_bits(a.u[i])); sat = false; OUT; break;
        case OP_F16TOF32: I1; EACH(v.u[i] = float_bits(a.u[i])); OUT; break;
        case OP_COUNTBITS: I1; EACH(v.u[i] = count_bits(a.u[i])); OUT; break;
        case OP_BFREV: I1; EACH(v.u[i] = reverse_bits(a.u[i])); OUT; break;
        case OP_FIRSTBIT_HI: I1; EACH(v.u[i] = a.u[i] ? (uint32_t)__builtin_clz(a.u[i]) : ~0u); OUT; break;
        case OP_FIRSTBIT_LO: I1; EACH(v.u[i] = a.u[i] ? (uint32_t)__builtin_ctz(a.u[i]) : ~0u); OUT; break;
        case OP_FIRSTBIT_SHI: I1; EACH(uint32_t x = a.i[i] < 0 ? ~a.u[i] : a.u[i]; v.u[i] = x ? (uint32_t)__builtin_clz(x) : ~0u); OUT; break;
        case OP_UADDC: case OP_USUBB: {
            a = src_i(&R, I->o[2]); b = src_i(&R, I->o[3]); vec4_t flags;
            EACH(if (I->op == OP_UADDC) { v.u[i] = a.u[i] + b.u[i]; flags.u[i] = v.u[i] < a.u[i]; }
                 else { v.u[i] = a.u[i] - b.u[i]; flags.u[i] = a.u[i] < b.u[i]; });
            store(&R, I->o[0], v, false); store(&R, I->o[1], flags, false); break;
        }
        case OP_UBFE: case OP_IBFE: {
            I3;
            EACH(unsigned width = a.u[i] & 31, offset = b.u[i] & 31; width = MIN(width, 32 - offset);
                 uint32_t bits = width ? (c.u[i] >> offset) & ((1u << width) - 1) : 0;
                 if (I->op == OP_IBFE && width) { uint32_t sign = 1u << (width - 1); bits = (bits ^ sign) - sign; } v.u[i] = bits;);
            OUT; break;
        }
        case OP_BFI: {
            a = src_i(&R, I->o[1]); b = src_i(&R, I->o[2]); c = src_i(&R, I->o[3]); vec4_t d = src_i(&R, I->o[4]);
            EACH(unsigned width = a.u[i] & 31, offset = b.u[i] & 31; width = MIN(width, 32 - offset);
                 uint32_t mask = width ? ((1u << width) - 1) << offset : 0; v.u[i] = ((c.u[i] << offset) & mask) | (d.u[i] & ~mask););
            OUT; break;
        }
        case OP_SWAPC: {
            a = src_i(&R, I->o[2]); b = src_i(&R, I->o[3]); c = src_i(&R, I->o[4]); vec4_t other;
            EACH(v.u[i] = a.u[i] ? c.u[i] : b.u[i]; other.u[i] = a.u[i] ? b.u[i] : c.u[i]);
            store(&R, I->o[0], v, false); store(&R, I->o[1], other, false); break;
        }
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
            for (int i = 0; i < 4; i++) { if (I->ret_type == 2) raw.u[i] = dim[i]; else if (I->ret_type == 1) raw.f[i] = dim[i] ? 1.0f / dim[i] : 0; else raw.f[i] = (float)dim[i]; }
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
        case OP_BLOCK: R.block = I->jump; break;
        case OP_BRANCH: R.previous_block = R.block; next_pc = !I->nops || COND(0) ? I->jump : I->alt; break;
        case OP_PHI: v = R.previous_block == I->jump ? src_i(&R, I->o[1]) : R.previous_block == I->alt ? src_i(&R, I->o[2]) : zero4; OUT; break;
        default: break;
        }
        pc = next_pc;
    }
    bool keep = !R.discard && budget >= 0;
    #undef R
    return keep;
}
bool dxbc_run(const dxbc_shader_t *s, const dxbc_env_t *env, const vec4_t *in, vec4_t *out) {
    dxbc_exec_t exec;
    if (!dxbc_exec_init(&exec, s, env, in, out)) return false;
    bool result = dxbc_exec_run(&exec);
    dxbc_exec_fini(&exec);
    return result;
}
