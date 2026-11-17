/* Small native DXIL 1.0 / SM6.0 frontend, not an LLVM runtime.
 *
 * Reads the published LLVM 3.7 bitstream encoding used by DXIL, then lowers a
 * bounded vertex/pixel subset into shader_priv.h's native IR. Supported: scalar
 * f32/i32 arithmetic, comparisons/selects, acyclic branches/two-input phis,
 * input/output signatures, legacy constant buffers and Texture2D sampling.
 * Unsupported LLVM instructions/DXIL intrinsics fail at upload, never execute
 * arbitrary native code. Waves, atomics, UAVs, pointers, helper functions,
 * tessellation/geometry/compute, doubles and newer profiles are not claimed.
 * Format references: Microsoft's DxilContainer.h, LLVMBitCodes.h, BitCodes.h. */
#include "shader_priv.h"

#define BC_TYPES 512
#define BC_VALUES 8192
#define BC_METADATA 4096
#define BC_RECORD 2048
#define BC_ABBREVS 512
#define BC_ABOPS 32
#define BC_BLOCK_ABBREVS 64
#define BC_BLOCKS 64
#define BC_BBS 128

enum { TY_VOID = 2, TY_FLOAT = 3, TY_INT = 7, TY_PTR = 8, TY_ARRAY = 11, TY_VECTOR = 12,
       TY_STRUCT = 20, TY_FUNCTION = 21 };
enum { V_NONE, V_CONSTANT, V_UNDEF, V_FUNCTION, V_OPERAND, V_HANDLE, V_OPAQUE };
typedef struct { uint16_t kind, bits, element, count, result; uint16_t params[16]; } ty_t;
typedef struct {
    uint16_t type, kind, fn_type;
    bool declaration;
    uint64_t integer;
    opnd_t operand;
    int resource_class, binding;
    int block, range_id;
    char name[64];
} value_t;
typedef struct { uint8_t kind; uint16_t n; int value; uint32_t refs[16]; char text[96]; } md_t;
typedef struct { uint64_t value; uint8_t encoding, literal; } abop_t;
typedef struct { int n; abop_t op[BC_ABOPS]; } abbrev_t;
typedef struct { int n; uint16_t id[BC_BLOCK_ABBREVS]; } ablist_t;
typedef struct { bool valid; uint32_t lower, count, rows; } resource_t;
typedef struct {
    const uint8_t *bytes; uint32_t size, bit, limit;
    dxbc_shader_t *s;
    char *err; int errlen; bool bad;
    abbrev_t *ab; int nab;
    ablist_t info[BC_BLOCKS];
    uint64_t rec[BC_RECORD]; int nr;
    ty_t *types; int ntypes, expected_types, constant_type;
    value_t *values; int nvalues, module_values, entry, defined_functions;
    md_t *md; int nmd, entry_metadata, version_metadata, model_metadata;
    resource_t resources[4][16];
    int sig_map[2][DX_MAX_SIG], sig_col[2][DX_MAX_SIG], sig_columns[2][DX_MAX_SIG];
    char metadata_name[96];
    uint32_t body_start, body_end, body_width;
    int pass, nblocks, block, block_pc[BC_BBS], branch_a[BC_BBS], branch_b[BC_BBS];
    int ins_cap, pool_cap;
    int use_block;
    uint32_t dependencies[BC_BBS][BC_BBS / 32];
    bool triple, version, module_seen;
} parser_t;

static void fail(parser_t *p, const char *why) {
    if (!p->bad && p->err && p->errlen > 0) snprintf(p->err, p->errlen, "DXIL: %s", why);
    p->bad = true;
}
static uint32_t u32(const void *data) { uint32_t n; memcpy(&n, data, 4); return n; }
static uint64_t bits(parser_t *p, unsigned n) {
    if (n > 64 || p->bit > p->limit || n > p->limit - p->bit) { fail(p, "truncated bitstream"); return 0; }
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++, p->bit++) v |= (uint64_t)((p->bytes[p->bit >> 3] >> (p->bit & 7)) & 1) << i;
    return v;
}
static uint64_t vbr(parser_t *p, unsigned width) {
    if (width < 2 || width > 32) { fail(p, "invalid VBR width"); return 0; }
    uint64_t value = 0;
    unsigned shift = 0;
    for (int chunks = 0; chunks < 64 && !p->bad; chunks++) {
        uint64_t x = bits(p, width), lo = x & ((1ull << (width - 1)) - 1);
        if (shift >= 64 || (lo && lo > (~0ull >> shift))) { fail(p, "VBR overflow"); return 0; }
        value |= lo << shift;
        if (!(x >> (width - 1))) return value;
        shift += width - 1;
    }
    fail(p, "unterminated VBR"); return 0;
}
static uint64_t signed_value(uint64_t v) { return (v & 1) ? 0ull - (v >> 1) : v >> 1; }
static void align32(parser_t *p) { if (p->bit > p->limit || p->limit - p->bit < ((0u - p->bit) & 31)) fail(p, "bad block alignment"); else p->bit = (p->bit + 31) & ~31u; }
static char char6(unsigned v) { return v < 26 ? 'a' + v : v < 52 ? 'A' + v - 26 : v < 62 ? '0' + v - 52 : v == 62 ? '.' : '_'; }
static void string_record(parser_t *p, int first, char *out, int cap) {
    int n = p->nr - first;
    if (n < 0 || n >= cap) { fail(p, "name is too long"); return; }
    for (int i = 0; i < n; i++) { if (p->rec[first + i] > 255) { fail(p, "invalid string"); return; } out[i] = p->rec[first + i]; }
    out[n] = 0;
}
static int read_abbrev(parser_t *p) {
    if (p->nab == BC_ABBREVS) { fail(p, "too many bitstream abbreviations"); return 0; }
    int id = p->nab++;
    abbrev_t *a = &p->ab[id];
    a->n = (int)vbr(p, 5);
    if (a->n < 1 || a->n > BC_ABOPS) { fail(p, "abbreviation is too large"); return id; }
    for (int i = 0; i < a->n && !p->bad; i++) {
        abop_t *o = &a->op[i]; o->literal = bits(p, 1);
        if (o->literal) o->value = vbr(p, 8);
        else {
            o->encoding = bits(p, 3);
            if (o->encoding == 1 || o->encoding == 2) o->value = vbr(p, 5);
            if (o->encoding < 1 || o->encoding > 5 || o->value > 32) fail(p, "invalid abbreviation encoding");
        }
    }
    return id;
}
static uint64_t abvalue(parser_t *p, const abop_t *o) {
    if (o->literal) return o->value;
    if (o->encoding == 1) return bits(p, (unsigned)o->value);
    if (o->encoding == 2) return vbr(p, (unsigned)o->value);
    if (o->encoding == 4) return (uint8_t)char6((unsigned)bits(p, 6));
    fail(p, "invalid scalar abbreviation"); return 0;
}
static void append(parser_t *p, uint64_t v) { if (p->nr == BC_RECORD) fail(p, "record is too large"); else p->rec[p->nr++] = v; }
static uint32_t record(parser_t *p, const ablist_t *list, unsigned id) {
    p->nr = 0;
    if (id == 3) {
        uint32_t code = (uint32_t)vbr(p, 6), n = (uint32_t)vbr(p, 6);
        if (n > BC_RECORD) { fail(p, "record is too large"); return 0; }
        for (uint32_t i = 0; i < n && !p->bad; i++) append(p, vbr(p, 6));
        return code;
    }
    if (id < 4 || id - 4 >= (unsigned)list->n) { fail(p, "unknown abbreviation"); return 0; }
    const abbrev_t *a = &p->ab[list->id[id - 4]];
    uint64_t code = abvalue(p, &a->op[0]);
    for (int i = 1; i < a->n && !p->bad; i++) {
        const abop_t *o = &a->op[i];
        if (!o->literal && o->encoding == 3) {
            uint64_t n = vbr(p, 6);
            if (++i >= a->n || n > BC_RECORD - p->nr) { fail(p, "invalid abbreviated array"); break; }
            for (uint64_t j = 0; j < n && !p->bad; j++) append(p, abvalue(p, &a->op[i]));
        } else if (!o->literal && o->encoding == 5) {
            uint64_t n = vbr(p, 6); align32(p);
            if (n > BC_RECORD - p->nr || n > (p->limit - p->bit) / 8) { fail(p, "blob is too large"); break; }
            for (uint64_t j = 0; j < n && !p->bad; j++) append(p, bits(p, 8));
            align32(p);
        } else append(p, abvalue(p, o));
    }
    return (uint32_t)code;
}

static ty_t *type_at(parser_t *p, uint64_t id) {
    if (id >= (uint64_t)p->ntypes) { fail(p, "type reference out of range"); return &p->types[0]; }
    return &p->types[id];
}
static value_t *new_value(parser_t *p, int type) {
    if (p->nvalues == BC_VALUES) { fail(p, "too many SSA values"); return &p->values[0]; }
    type_at(p, type);
    value_t *v = &p->values[p->nvalues++]; memset(v, 0, sizeof *v); v->type = type;
    v->block = p->pass ? p->block : -1;
    return v;
}
static opnd_t operand(int kind, int index, int component, bool dest) {
    opnd_t o; memset(&o, 0, sizeof o);
    o.type = kind; o.dim = kind == T_IMM32 || kind == T_NULL || kind == T_ODEPTH ? 0 : 1;
    o.idx[0] = index; o.mask = dest ? (1u << component) : 15; o.mode = dest ? 0 : 1;
    o.rel[0] = o.rel[1] = o.rel[2] = -1;
    for (int i = 0; i < 4; i++) o.swz[i] = component;
    return o;
}
static opnd_t immediate(uint32_t bits_) { opnd_t o = operand(T_IMM32, 0, 0, false); for (int i = 0; i < 4; i++) o.imm[i] = bits_; return o; }
static int pool_add(parser_t *p, const opnd_t *o) {
    dxbc_shader_t *s = p->s;
    if (s->npool == SHADER_MAX_OPERANDS) { fail(p, "too many native operands"); return 0; }
    if (s->npool == p->pool_cap) {
        int cap = MIN(SHADER_MAX_OPERANDS, p->pool_cap ? p->pool_cap * 2 : 128);
        opnd_t *a = krealloc(s->pool, cap * sizeof *a);
        if (!a) { fail(p, "out of memory"); return 0; }
        s->pool = a; p->pool_cap = cap;
    }
    s->pool[s->npool] = *o; return s->npool++;
}
static ins_t *emit(parser_t *p, int op, int n, const opnd_t *args) {
    dxbc_shader_t *s = p->s;
    if (s->nins == SHADER_MAX_INS) { fail(p, "too many native instructions"); return &s->ins[0]; }
    if (s->nins == p->ins_cap) {
        int cap = MIN(SHADER_MAX_INS, p->ins_cap ? p->ins_cap * 2 : 128);
        ins_t *a = krealloc(s->ins, cap * sizeof *a);
        if (!a) { fail(p, "out of memory"); return NULL; }
        s->ins = a; p->ins_cap = cap;
    }
    ins_t *I = &s->ins[s->nins++]; memset(I, 0, sizeof *I);
    I->op = op; I->nops = n; I->jump = I->alt = -1;
    for (int i = 0; i < n && !p->bad; i++) I->o[i] = pool_add(p, &args[i]);
    return I;
}
static opnd_t temporary(parser_t *p, bool vector) {
    if (p->s->ntemps == SHADER_MAX_TEMPS) { fail(p, "too many native temporaries"); return immediate(0); }
    opnd_t o = operand(T_TEMP, p->s->ntemps++, 0, true); o.mask = vector ? 15 : 1; return o;
}
static opnd_t source(parser_t *p, const value_t *v) {
    const ty_t *t = type_at(p, v->type);
    if (v->kind == V_OPERAND || v->kind == V_CONSTANT || v->kind == V_UNDEF) {
        if (t->kind != TY_FLOAT && !(t->kind == TY_INT && (t->bits == 32 || t->bits == 1))) fail(p, "only scalar f32/i32/boolean arithmetic is implemented");
        return v->operand;
    }
    fail(p, "unsupported SSA operand"); return immediate(0);
}
static value_t *ref(parser_t *p, uint64_t encoded, bool sign) {
    if (encoded > 0xFFFFFFFFu) { fail(p, "SSA reference overflow"); return &p->values[0]; }
    int64_t distance = sign ? (int64_t)signed_value(encoded) : (int64_t)(uint32_t)encoded;
    int64_t id = p->nvalues - distance;
    if (id < 0 || id >= p->nvalues) { fail(p, "forward/out-of-range SSA reference"); return &p->values[0]; }
    value_t *v = &p->values[id];
    int use = p->use_block >= 0 ? p->use_block : p->block;
    if (v->block >= 0 && p->pass && use >= 0 && use < BC_BBS)
        p->dependencies[use][v->block / 32] |= 1u << (v->block & 31);
    return v;
}
static value_t *pair(parser_t *p, int *slot) {
    if (*slot >= p->nr) { fail(p, "missing value/type pair"); return &p->values[0]; }
    return ref(p, p->rec[(*slot)++], false);  /* forward values deliberately unsupported */
}
static uint32_t constant(parser_t *p, const value_t *v) {
    ty_t *t = type_at(p, v->type);
    if (v->kind != V_CONSTANT || t->kind != TY_INT || t->bits > 32) { fail(p, "dynamic binding/index is not implemented"); return 0; }
    return (uint32_t)v->integer;
}
static value_t *result(parser_t *p, int type, int op, int nsrc, const opnd_t *srcs, bool vector) {
    opnd_t args[5]; args[0] = temporary(p, vector);
    for (int i = 0; i < nsrc; i++) args[i + 1] = srcs[i];
    if (!p->bad) emit(p, op, nsrc + 1, args);
    value_t *v = new_value(p, type); v->kind = V_OPERAND; v->operand = args[0];
    v->operand.mode = 1; v->operand.mask = 15;
    for (int i = 0; i < 4; i++) v->operand.swz[i] = vector ? i : 0;
    return v;
}

static md_t *md_at(parser_t *p, uint32_t ref_) {
    if (!ref_ || ref_ - 1 >= (uint32_t)p->nmd) { fail(p, "metadata reference out of range"); return &p->md[0]; }
    return &p->md[ref_ - 1];
}
static uint32_t md_number(parser_t *p, uint32_t ref_) {
    md_t *m = md_at(p, ref_);
    if (m->kind != 2 || m->value < 0 || m->value >= p->module_values) { fail(p, "invalid typed metadata value"); return 0; }
    return constant(p, &p->values[m->value]);
}
static md_t *node(parser_t *p, uint32_t ref_, int min) {
    md_t *m = md_at(p, ref_);
    if (m->kind != 3 || m->n < min) fail(p, "invalid metadata node");
    return m;
}
static bool scalar_type(parser_t *p, int id, bool floating, int width) {
    ty_t *t = type_at(p, id);
    return floating ? t->kind == TY_FLOAT : t->kind == TY_INT && t->bits == width;
}
static bool four_components(parser_t *p, int id, bool floating, bool status) {
    ty_t *t = type_at(p, id);
    if (t->kind != TY_STRUCT || t->count != (status ? 5 : 4)) return false;
    for (int i = 0; i < 4; i++) if (!scalar_type(p, t->params[i], floating, 32)) return false;
    return !status || scalar_type(p, t->params[4], false, 32);
}
static void resource_metadata(parser_t *p, uint32_t root) {
    if (!root) return;
    md_t *all = node(p, root, 4);
    if (p->bad) return;
    for (int cls = 0; cls < 4 && !p->bad; cls++) {
        if (!all->refs[cls]) continue;
        md_t *list = node(p, all->refs[cls], 0);
        if (cls == 1 && list->n) { fail(p, "UAV resources are not implemented"); return; }
        for (int i = 0; i < list->n && !p->bad; i++) {
            md_t *r = node(p, list->refs[i], cls == 0 ? 9 : 8);
            if (p->bad) return;
            uint32_t id = md_number(p, r->refs[0]), space = md_number(p, r->refs[3]);
            uint32_t lower = md_number(p, r->refs[4]), count = md_number(p, r->refs[5]);
            if (space || id >= 16 || !count || lower >= (cls == 2 ? 14u : 16u) || count > (cls == 2 ? 14u : 16u) - lower || p->resources[cls][id].valid) {
                fail(p, "resource spaces/unbounded or out-of-range bindings are not implemented"); return;
            }
            uint32_t rows = 0;
            if (cls == 0) {
                if (md_number(p, r->refs[6]) != 2 || md_number(p, r->refs[7])) { fail(p, "only non-MSAA Texture2D resources are implemented"); return; }
                md_t *props = node(p, r->refs[8], 2);
                if (props->n != 2 || md_number(p, props->refs[0]) || md_number(p, props->refs[1]) != 9) { fail(p, "only float Texture2D resources are implemented"); return; }
            } else if (cls == 2) {
                uint32_t bytes_ = md_number(p, r->refs[6]);
                if (!bytes_ || bytes_ > 65536 || r->refs[7]) { fail(p, "constant-buffer metadata is not implemented"); return; }
                rows = (bytes_ + 15) / 16;
            } else if (cls == 3 && (md_number(p, r->refs[6]) || r->refs[7])) { fail(p, "comparison/extended samplers are not implemented"); return; }
            for (int k = 0; k < 16; k++) {
                resource_t *q = &p->resources[cls][k];
                if (q->valid && lower < q->lower + q->count && q->lower < lower + count) { fail(p, "overlapping resource bindings"); return; }
            }
            p->resources[cls][id] = (resource_t){ true, lower, count, rows };
        }
    }
}
static void signature_metadata(parser_t *p) {
    if (p->entry_metadata < 0 || p->entry_metadata >= p->nmd) { fail(p, "entry-point metadata is missing"); return; }
    md_t *entry = &p->md[p->entry_metadata];
    if (entry->kind != 3 || entry->n != 5) { fail(p, "invalid entry-point metadata"); return; }
    md_t *fn = md_at(p, entry->refs[0]);
    if (fn->kind != 2 || fn->value != p->entry) { fail(p, "entry-point function mismatch"); return; }
    resource_metadata(p, entry->refs[3]);
    if (entry->refs[4]) {
        md_t *props = node(p, entry->refs[4], 0);
        /* Shader flags beyond ordinary f32/int arithmetic need pipeline support. */
        if (props->n) { fail(p, "entry-point flags/properties are not implemented"); return; }
    }
    if (!entry->refs[2]) { if (p->s->nin || p->s->nout) fail(p, "signature metadata is missing"); return; }
    md_t *signatures = md_at(p, entry->refs[2]);
    if (signatures->kind != 3 || signatures->n < 2) { fail(p, "invalid signature metadata"); return; }
    for (int direction = 0; direction < 2 && !p->bad; direction++) {
        if (!signatures->refs[direction]) continue;
        md_t *list = md_at(p, signatures->refs[direction]);
        int count = direction ? p->s->nout : p->s->nin;
        if (list->kind != 3 || list->n != count) { fail(p, "signature list/container mismatch"); return; }
        for (int i = 0; i < list->n && !p->bad; i++) {
            md_t *element = md_at(p, list->refs[i]);
            if (element->kind != 3 || element->n < 10) { fail(p, "invalid signature element"); return; }
            uint32_t id = md_number(p, element->refs[0]), mode = md_number(p, element->refs[5]);
            uint32_t rows = md_number(p, element->refs[6]), columns = md_number(p, element->refs[7]);
            uint32_t reg = md_number(p, element->refs[8]);
            uint32_t col = md_number(p, element->refs[9]), component = md_number(p, element->refs[2]);
            if (reg == ~0u && col == 255) col = 0;  /* unpacked scalar SV_Depth */
            md_t *name = md_at(p, element->refs[1]), *indices = node(p, element->refs[4], 1);
            if (id >= DX_MAX_SIG || p->sig_map[direction][id] >= 0 || rows != 1 || !columns || columns > 4 || col > 4 - columns || name->kind != 1 || indices->n != 1) { fail(p, "signature arrays/packing are not implemented"); return; }
            uint32_t semantic_index = md_number(p, indices->refs[0]);
            dxsig_t *sigs = direction ? p->s->out : p->s->in;
            int k; for (k = 0; k < count; k++) if (!strcasecmp(sigs[k].name, name->text) && sigs[k].index == (int)semantic_index) break;
            if (k == count) { fail(p, "signature semantic mismatch"); return; }
            dxsig_t *sig = &sigs[k];
            int ctype = component == 9 ? 3 : component == 4 ? 2 : component == 5 ? 1 : 0;
            if (reg != (uint32_t)sig->reg || sig->mask != (((1u << columns) - 1) << col) || ctype != sig->component_type) { fail(p, "signature register/component mismatch"); return; }
            p->sig_map[direction][id] = k; p->sig_col[direction][id] = col; p->sig_columns[direction][id] = columns;
            if (!direction && sig->reg >= 0) {
                if (mode > 5) { fail(p, "sample-frequency interpolation is not implemented"); return; }
                int m = mode == 1 ? 1 : mode >= 4 ? 4 : 2;
                if ((p->s->interpolation_declared & (1u << sig->reg)) && p->s->interpolation[sig->reg] != m) { fail(p, "mixed interpolation within a register is not implemented"); return; }
                p->s->interpolation_declared |= 1u << sig->reg; p->s->interpolation[sig->reg] = m;
            }
        }
    }
}

static void type_record(parser_t *p, uint32_t code) {
    if (code == 1) {
        if (p->nr != 1 || p->rec[0] > BC_TYPES) fail(p, "type table is too large");
        else p->expected_types = (int)p->rec[0];
        return;
    }
    if (code == 19) return;             /* a name for the following named struct */
    if (p->ntypes >= BC_TYPES || p->ntypes >= p->expected_types) { fail(p, "too many types"); return; }
    ty_t *t = &p->types[p->ntypes++]; t->kind = code;
    if (code == TY_INT) {
        if (p->nr != 1 || !p->rec[0] || p->rec[0] > 64) fail(p, "integer type width");
        else t->bits = (uint16_t)p->rec[0];
    } else if (code == TY_FLOAT) t->bits = 32;
    else if (code == TY_PTR) {
        if (p->nr < 1 || p->rec[0] >= (uint64_t)p->expected_types) fail(p, "pointer type");
        else t->element = p->rec[0];
    } else if (code == TY_VECTOR || code == TY_ARRAY) {
        if (p->nr != 2 || p->rec[0] > 65535 || p->rec[1] >= (uint64_t)p->expected_types) fail(p, "array/vector type");
        else { t->count = p->rec[0]; t->element = p->rec[1]; }
    } else if (code == TY_FUNCTION || code == 18 || code == 20) {
        int first = code == TY_FUNCTION ? 2 : 1;
        if (p->nr < first || p->nr - first > 16 || (code == TY_FUNCTION && p->rec[0])) { fail(p, "complex/variadic type"); return; }
        t->kind = code == TY_FUNCTION ? TY_FUNCTION : TY_STRUCT;
        t->count = p->nr - first;
        if (code == TY_FUNCTION) { if (p->rec[1] >= (uint64_t)p->expected_types) fail(p, "invalid function result type"); else t->result = p->rec[1]; }
        for (int i = 0; i < t->count; i++) { if (p->rec[first + i] >= (uint64_t)p->expected_types) fail(p, "invalid member type"); else t->params[i] = p->rec[first + i]; }
    } else if (code != TY_VOID && code != 5 && code != 6 && code != 16 && code != 4 && code != 10) fail(p, "unsupported LLVM type");
}
static void constant_record(parser_t *p, uint32_t code) {
    if (code == 1) { if (p->nr != 1) fail(p, "constant type record"); else { p->constant_type = p->rec[0]; type_at(p, p->constant_type); } return; }
    ty_t *t = type_at(p, p->constant_type);
    value_t *v = new_value(p, p->constant_type); v->kind = V_CONSTANT; v->operand = immediate(0);
    v->block = -1;
    if (code == 2 || code == 3) { v->kind = code == 3 ? V_UNDEF : V_CONSTANT; return; }
    if (code == 4 && p->nr == 1 && t->kind == TY_INT) {
        v->integer = signed_value(p->rec[0]);
        if (t->bits < 64) v->integer &= (1ull << t->bits) - 1;
        v->operand = immediate(t->bits == 1 && v->integer ? ~0u : (uint32_t)v->integer);
    } else if (code == 6 && p->nr == 1 && t->kind == TY_FLOAT) v->operand = immediate((uint32_t)p->rec[0]);
    else v->kind = V_OPAQUE;            /* metadata arrays/strings, not executable values */
}
static void metadata_record(parser_t *p, uint32_t code) {
    if (code == 4) { string_record(p, 0, p->metadata_name, sizeof p->metadata_name); return; }
    if (code == 10) {
        if (!strcmp(p->metadata_name, "dx.entryPoints")) {
            if (p->nr != 1 || p->rec[0] >= BC_METADATA) fail(p, "multiple/invalid entry points");
            else p->entry_metadata = p->rec[0];
        } else if (!strcmp(p->metadata_name, "dx.version") || !strcmp(p->metadata_name, "dx.shaderModel")) {
            if (p->nr != 1 || p->rec[0] >= BC_METADATA) fail(p, "invalid shader version metadata");
            else if (!strcmp(p->metadata_name, "dx.version")) p->version_metadata = p->rec[0];
            else p->model_metadata = p->rec[0];
        }
        return;
    }
    if (code == 6) return;              /* metadata-kind names do not occupy value IDs */
    if (p->nmd == BC_METADATA) { fail(p, "metadata table is too large"); return; }
    md_t *m = &p->md[p->nmd++]; m->kind = code;
    if (code == 1) string_record(p, 0, m->text, sizeof m->text);
    else if (code == 2) {
        if (p->nr != 2 || p->rec[1] >= (uint64_t)p->nvalues) fail(p, "metadata value");
        else m->value = p->rec[1];
    } else if (code == 3 || code == 5) {
        m->kind = 3;
        if (p->nr > 16) { fail(p, "metadata node is too large"); return; }
        m->n = p->nr;
        for (int i = 0; i < p->nr; i++) { if (p->rec[i] > BC_METADATA) fail(p, "metadata reference"); else m->refs[i] = p->rec[i]; }
    } else fail(p, "debug/extended metadata is not implemented");
}

static int bool_type(parser_t *p) { for (int i = 0; i < p->ntypes; i++) if (p->types[i].kind == TY_INT && p->types[i].bits == 1) return i; fail(p, "missing boolean type"); return 0; }
static void binary_record(parser_t *p) {
    int at = 0; value_t *a = pair(p, &at);
    if (at + 2 > p->nr) { fail(p, "binary instruction"); return; }
    value_t *b = ref(p, p->rec[at++], false);
    int op = p->rec[at++], type = a->type;
    ty_t *t = type_at(p, type);
    opnd_t args[3] = { source(p, a), source(p, b) };
    if (type != b->type || (t->kind != TY_FLOAT && (t->kind != TY_INT || (t->bits != 32 && !(t->bits == 1 && op >= 10 && op <= 12))))) { fail(p, "binary operand types"); return; }
    int native = -1;
    if (t->kind == TY_FLOAT) {
        if (op == 0) native = OP_ADD;
        else if (op == 1) { native = OP_ADD; args[1].mod ^= 1; }
        else if (op == 2) native = OP_MUL;
        else if (op == 4) native = OP_DIV;
    } else {
        const int map[] = { OP_IADD, OP_IADD, OP_IMAD, -1, -1, -1, -1, OP_ISHL, OP_USHR, OP_ISHR, OP_AND, OP_OR, OP_XOR };
        if (op >= 0 && op < (int)ARRAY_LEN(map)) native = map[op];
        if (op == 1) args[1].mod ^= 1;
        if (op == 2) args[2] = immediate(0);
    }
    if (native < 0) { fail(p, "binary operation is not implemented"); return; }
    result(p, type, native, op == 2 && t->kind == TY_INT ? 3 : 2, args, false);
}
static void compare_record(parser_t *p) {
    int at = 0; value_t *a = pair(p, &at);
    if (at + 2 > p->nr) { fail(p, "comparison instruction"); return; }
    value_t *b = ref(p, p->rec[at++], false);
    int pred = p->rec[at];
    ty_t *t = type_at(p, a->type);
    opnd_t args[2] = { source(p, a), source(p, b) }; int op = -1; bool invert = false;
    if (a->type != b->type) { fail(p, "comparison types"); return; }
    if (t->kind == TY_FLOAT) {
        switch (pred) {
        case 1: op = OP_EQ; break; case 2: op = OP_LT; invert = true; break; case 3: op = OP_GE; break;
        case 4: op = OP_LT; break; case 5: op = OP_GE; invert = true; break; case 14: op = OP_NE; break;
        }
    } else if (t->kind == TY_INT && t->bits <= 32) {
        switch (pred) {
        case 32: op = OP_IEQ; break; case 33: op = OP_INE; break; case 34: op = OP_ULT; invert = true; break;
        case 35: op = OP_UGE; break; case 36: op = OP_ULT; break; case 37: op = OP_UGE; invert = true; break;
        case 38: op = OP_ILT; invert = true; break; case 39: op = OP_IGE; break;
        case 40: op = OP_ILT; break; case 41: op = OP_IGE; invert = true; break;
        }
    }
    if (op < 0) { fail(p, "comparison predicate is not implemented"); return; }
    if (invert) { opnd_t tmp = args[0]; args[0] = args[1]; args[1] = tmp; }
    result(p, bool_type(p), op, 2, args, false);
}
static void cast_record(parser_t *p) {
    int at = 0; value_t *v = pair(p, &at);
    if (at + 2 != p->nr) { fail(p, "cast instruction"); return; }
    int type = p->rec[at++], cast = p->rec[at];
    ty_t *from = type_at(p, v->type), *to = type_at(p, type);
    opnd_t args[2] = { source(p, v), immediate(1) }; int op = -1;
    if (cast == 3 && from->kind == TY_FLOAT && to->kind == TY_INT && to->bits == 32) op = OP_FTOU;
    else if (cast == 4 && from->kind == TY_FLOAT && to->kind == TY_INT && to->bits == 32) op = OP_FTOI;
    else if (cast == 5 && from->kind == TY_INT && from->bits == 32 && to->kind == TY_FLOAT) op = OP_UTOF;
    else if (cast == 6 && from->kind == TY_INT && from->bits == 32 && to->kind == TY_FLOAT) op = OP_ITOF;
    else if (cast == 11 && from->bits == 32 && to->bits == 32) op = OP_MOV;
    else if ((cast == 0 || cast == 1 || cast == 2) && from->kind == TY_INT && to->kind == TY_INT && to->bits <= 32 && from->bits <= 32) {
        if (cast == 0 || from->bits == 1) {
            unsigned width = cast == 0 ? to->bits : from->bits;
            args[1] = immediate(width == 32 ? ~0u : (1u << width) - 1);
            op = OP_AND;
        } else if (cast == 1) op = OP_MOV;
        else { fail(p, "sign extension width is not implemented"); return; }
    }
    if (op < 0) { fail(p, "cast is not implemented"); return; }
    value_t *out = result(p, type, op, op == OP_AND ? 2 : 1, args, false);
    if ((cast == 2 && from->bits == 1) || (cast == 0 && to->bits == 1)) {
        opnd_t negate[2] = { out->operand, out->operand }; negate[0].mode = 0; negate[0].mask = 1;
        emit(p, OP_INEG, 2, negate);
    }
}
static void call_record(parser_t *p) {
    if (p->nr < 3) { fail(p, "call record"); return; }
    int at = 2, fn_type = -1;
    uint32_t cc = p->rec[1];
    if (cc & (1u << 15)) { if (at >= p->nr) { fail(p, "call type"); return; } fn_type = p->rec[at++]; }
    value_t *fn = pair(p, &at);
    if (fn->kind != V_FUNCTION || !fn->declaration || strncmp(fn->name, "dx.op.", 6)) { fail(p, "helper/external functions are not implemented"); return; }
    if (fn_type < 0) fn_type = fn->fn_type;
    ty_t *ft = type_at(p, fn_type);
    if (ft->kind != TY_FUNCTION || ft->count > 16 || at + ft->count != p->nr) { fail(p, "call signature"); return; }
    value_t *arg[16];
    for (int i = 0; i < ft->count; i++) {
        arg[i] = ref(p, p->rec[at++], false);
        if (arg[i]->type != ft->params[i]) { fail(p, "call argument type"); return; }
    }
    if (p->bad) return;
    if (!ft->count) { fail(p, "call opcode argument is missing"); return; }
    int opcode = constant(p, arg[0]);
    const char *name = fn->name + 6;
    int rt = ft->result;
    if (opcode == 57 && !strcmp(name, "createHandle") && ft->count == 5) {
        unsigned cls = constant(p, arg[1]), index = constant(p, arg[3]);
        unsigned id = constant(p, arg[2]), nonuniform = constant(p, arg[4]);
        if ((cls != 0 && cls != 2 && cls != 3) || id >= 16 || nonuniform) { fail(p, "resource class/binding is not implemented"); return; }
        resource_t *r = &p->resources[cls][id];
        if (!r->valid || index < r->lower || index - r->lower >= r->count) { fail(p, "handle does not match its resource binding range"); return; }
        value_t *v = new_value(p, rt); v->kind = V_HANDLE; v->resource_class = cls; v->binding = index; v->range_id = id; return;
    }
    if (opcode == 4 && (!strcmp(name, "loadInput.f32") || !strcmp(name, "loadInput.i32")) && ft->count == 5) {
        unsigned id = constant(p, arg[1]), row = constant(p, arg[2]), col = constant(p, arg[3]);
        if (id >= DX_MAX_SIG || p->sig_map[0][id] < 0 || row || col >= (unsigned)p->sig_columns[0][id] || arg[4]->kind != V_UNDEF) { fail(p, "input signature indexing is not implemented"); return; }
        dxsig_t *sig = &p->s->in[p->sig_map[0][id]];
        if (sig->reg < 0 || !scalar_type(p, rt, sig->component_type == 3, 32)) { fail(p, "input signature type mismatch"); return; }
        value_t *v = new_value(p, rt); v->kind = V_OPERAND; v->operand = operand(T_INPUT, sig->reg, col + p->sig_col[0][id], false); return;
    }
    if (opcode == 5 && (!strcmp(name, "storeOutput.f32") || !strcmp(name, "storeOutput.i32")) && ft->count == 5) {
        unsigned id = constant(p, arg[1]), row = constant(p, arg[2]), col = constant(p, arg[3]);
        if (id >= DX_MAX_SIG || p->sig_map[1][id] < 0 || row || col >= (unsigned)p->sig_columns[1][id]) { fail(p, "output signature indexing is not implemented"); return; }
        dxsig_t *sig = &p->s->out[p->sig_map[1][id]];
        if (type_at(p, rt)->kind != TY_VOID || !scalar_type(p, arg[4]->type, sig->component_type == 3, 32)) { fail(p, "output signature type mismatch"); return; }
        if (sig->sysval == 67 || sig->sysval == 68 || (sig->reg < 0 && sig->sysval != 65)) { fail(p, "output semantic is not implemented"); return; }
        opnd_t args[2] = { operand(sig->sysval == 65 ? T_ODEPTH : T_OUTPUT, sig->reg, col + p->sig_col[1][id], true), source(p, arg[4]) };
        emit(p, OP_MOV, 2, args); return;
    }
    if (opcode == 59 && (!strcmp(name, "cbufferLoadLegacy.f32") || !strcmp(name, "cbufferLoadLegacy.i32")) && ft->count == 3) {
        value_t *h = arg[1];
        if (h->kind != V_HANDLE || h->resource_class != 2 ||
            !(four_components(p, rt, true, false) || four_components(p, rt, false, false))) { fail(p, "constant-buffer load type"); return; }
        unsigned row = constant(p, arg[2]);
        if (row >= p->resources[2][h->range_id].rows) { fail(p, "constant-buffer row out of range"); return; }
        value_t *v = new_value(p, rt); v->kind = V_OPERAND; v->operand = operand(T_CB, h->binding, 0, false);
        v->operand.dim = 2; v->operand.idx[1] = row;
        for (int i = 0; i < 4; i++) v->operand.swz[i] = i;
        return;
    }
    if ((opcode == 60 && !strcmp(name, "sample.f32")) || (opcode == 62 && !strcmp(name, "sampleLevel.f32"))) {
        if (ft->count != 11 || !four_components(p, rt, true, true) || arg[1]->kind != V_HANDLE || arg[1]->resource_class != 0 || arg[2]->kind != V_HANDLE || arg[2]->resource_class != 3) { fail(p, "Texture2D sampling signature"); return; }
        for (int i = 5; i <= 6; i++) if (arg[i]->kind != V_UNDEF) { fail(p, "only two-dimensional texture sampling is implemented"); return; }
        if (constant(p, arg[7]) || constant(p, arg[8]) || arg[9]->kind != V_UNDEF) { fail(p, "texture offsets are not implemented"); return; }
        opnd_t uv = temporary(p, true), move[2] = { uv, source(p, arg[3]) }; move[0].mask = 1; emit(p, OP_MOV, 2, move);
        move[0].mask = 2; move[1] = source(p, arg[4]); emit(p, OP_MOV, 2, move);
        uv.mode = 1; for (int i = 0; i < 4; i++) uv.swz[i] = i;
        opnd_t args[4] = { uv, operand(T_RESOURCE, arg[1]->binding, 0, false), operand(T_SAMPLER, arg[2]->binding, 0, false), source(p, arg[10]) };
        for (int i = 0; i < 4; i++) args[1].swz[i] = i;
        if (opcode == 60 && arg[10]->kind != V_UNDEF) { fail(p, "sample clamp is not implemented"); return; }
        result(p, rt, opcode == 62 ? OP_SAMPLE_L : OP_SAMPLE, opcode == 62 ? 4 : 3, args, true); return;
    }
    if (ft->count == 2 && ((opcode == 130 && !strcmp(name, "legacyF32ToF16")) || (opcode == 131 && !strcmp(name, "legacyF16ToF32")))) {
        if (!scalar_type(p, rt, opcode == 131, 32) || !scalar_type(p, arg[1]->type, opcode == 130, 32)) { fail(p, "legacy half-conversion type"); return; }
        opnd_t src = source(p, arg[1]); result(p, rt, opcode == 130 ? OP_F32TOF16 : OP_F16TOF32, 1, &src, false); return;
    }
    if ((opcode == 54 && !strcmp(name, "dot2.f32")) || (opcode == 55 && !strcmp(name, "dot3.f32")) || (opcode == 56 && !strcmp(name, "dot4.f32"))) {
        int count = opcode - 52;
        if (ft->count != 1 + 2 * count || !scalar_type(p, rt, true, 32)) { fail(p, "dot-product signature"); return; }
        opnd_t args[2];
        for (int v = 0; v < 2; v++) {
            args[v] = temporary(p, true);
            for (int c = 0; c < count; c++) {
                value_t *a = arg[1 + v * count + c];
                if (a->type != rt) { fail(p, "dot-product operand type"); return; }
                opnd_t move[2] = { args[v], source(p, a) }; move[0].mask = 1u << c; emit(p, OP_MOV, 2, move);
            }
            args[v].mode = 1; for (int c = 0; c < 4; c++) args[v].swz[c] = c;
        }
        result(p, rt, count == 2 ? OP_DP2 : count == 3 ? OP_DP3 : OP_DP4, 2, args, false); return;
    }
    if (opcode == 82 && !strcmp(name, "discard") && ft->count == 2 && type_at(p, rt)->kind == TY_VOID && scalar_type(p, arg[1]->type, false, 1)) {
        opnd_t condition = source(p, arg[1]); ins_t *I = emit(p, OP_DISCARD, 1, &condition); if (I) I->nonzero = 1; return;
    }
    if ((!strcmp(name, "unary.f32") || !strcmp(name, "unary.i32") || !strcmp(name, "unaryBits.i32")) && ft->count == 2) {
        int op = -1; bool sat = false;
        switch (opcode) {
        case 6: op = OP_MOV; break; case 7: op = OP_MOV; sat = true; break;
        case 12: op = OP_SINCOS; break; case 13: op = OP_SINCOS; break;
        case 21: op = OP_EXP; break; case 22: op = OP_FRC; break; case 23: op = OP_LOG; break;
        case 24: op = OP_SQRT; break; case 25: op = OP_RSQ; break;
        case 26: op = OP_ROUND_NE; break; case 27: op = OP_ROUND_NI; break; case 28: op = OP_ROUND_PI; break; case 29: op = OP_ROUND_Z; break;
        case 30: op = OP_BFREV; break; case 31: op = OP_COUNTBITS; break; case 32: op = OP_FIRSTBIT_LO; break;
        case 33: op = OP_FIRSTBIT_HI; break; case 34: op = OP_FIRSTBIT_SHI; break;
        }
        if (op < 0 || !scalar_type(p, rt, opcode < 30, 32) || arg[1]->type != rt) { fail(p, "unary intrinsic/type is not implemented"); return; }
        opnd_t src = source(p, arg[1]);
        if (opcode == 6) src.mod |= 2;
        if (op == OP_SINCOS) {
            opnd_t dest = temporary(p, false), args[3] = { operand(T_NULL, 0, 0, true), operand(T_NULL, 0, 0, true), src };
            args[opcode == 13 ? 0 : 1] = dest; emit(p, OP_SINCOS, 3, args);
            value_t *v = new_value(p, rt); v->kind = V_OPERAND; v->operand = dest; v->operand.mode = 1; return;
        }
        value_t *v = result(p, rt, op, 1, &src, false); (void)v;
        if (sat && p->s->nins) p->s->ins[p->s->nins - 1].sat = true;
        return;
    }
    if ((!strcmp(name, "binary.f32") || !strcmp(name, "binary.i32")) && ft->count == 3) {
        int op = opcode == 35 || opcode == 37 || opcode == 39 ? OP_MAX : opcode == 36 || opcode == 38 || opcode == 40 ? OP_MIN : -1;
        if (opcode == 37) op = OP_IMAX; if (opcode == 38) op = OP_IMIN;
        if (opcode == 39) op = OP_UMAX; if (opcode == 40) op = OP_UMIN;
        if (op < 0 || !scalar_type(p, rt, opcode <= 36, 32) || arg[1]->type != rt || arg[2]->type != rt) { fail(p, "binary intrinsic/type is not implemented"); return; }
        opnd_t args[2] = { source(p, arg[1]), source(p, arg[2]) }; result(p, rt, op, 2, args, false); return;
    }
    if ((!strcmp(name, "tertiary.f32") || !strcmp(name, "tertiary.i32")) && ft->count == 4 && (opcode == 46 || opcode == 48 || opcode == 49)) {
        if (!scalar_type(p, rt, opcode == 46, 32) || arg[1]->type != rt || arg[2]->type != rt || arg[3]->type != rt) { fail(p, "tertiary intrinsic type is not implemented"); return; }
        opnd_t args[3] = { source(p, arg[1]), source(p, arg[2]), source(p, arg[3]) };
        result(p, rt, opcode == 46 ? OP_MAD : opcode == 48 ? OP_IMAD : OP_UMAD, 3, args, false); return;
    }
    if (p->err && p->errlen > 0) snprintf(p->err, p->errlen, "DXIL: intrinsic %s (opcode %d) is not implemented", name, opcode);
    p->bad = true;
}

static void function_record(parser_t *p, uint32_t code) {
    if (code == 1) {
        if (p->nr != 1 || !p->rec[0] || p->rec[0] > BC_BBS || p->nblocks) { fail(p, "basic block declaration"); return; }
        p->nblocks = p->rec[0]; p->block = 0; p->block_pc[0] = p->s->nins;
        ins_t *I = emit(p, OP_BLOCK, 0, NULL); if (I) I->jump = 0;
        return;
    }
    if (!p->nblocks || p->block >= p->nblocks) { fail(p, "instruction outside a basic block"); return; }
    if (code == 2) binary_record(p);
    else if (code == 3) cast_record(p);
    else if (code == 9 || code == 28) compare_record(p);
    else if (code == 34) call_record(p);
    else if (code == 26) {
        int at = 0; value_t *agg = pair(p, &at); ty_t *t = type_at(p, agg->type);
        if (at + 1 != p->nr || t->kind != TY_STRUCT || p->rec[at] >= t->count || p->rec[at] >= 4 || agg->kind != V_OPERAND) { fail(p, "aggregate extraction is not implemented"); return; }
        int component = p->rec[at];
        value_t *v = new_value(p, t->params[component]); v->kind = V_OPERAND; v->operand = agg->operand;
        for (int i = 0; i < 4; i++) v->operand.swz[i] = component;
    } else if (code == 5 || code == 29) {
        int at = 0; value_t *yes = pair(p, &at);
        if (at + 2 != p->nr) { fail(p, "select instruction"); return; }
        value_t *no = ref(p, p->rec[at++], false), *condition = ref(p, p->rec[at], false);
        if (yes->type != no->type || type_at(p, condition->type)->bits != 1) { fail(p, "select operand types"); return; }
        opnd_t args[3] = { source(p, condition), source(p, yes), source(p, no) }; result(p, yes->type, OP_MOVC, 3, args, false);
    } else if (code == 16) {
        if (p->nr != 5) { fail(p, "only two-input phi nodes are implemented"); return; }
        int type = p->rec[0], block_a = p->rec[2], block_b = p->rec[4];
        if (block_a < 0 || block_a >= p->block || block_b < 0 || block_b >= p->block || block_a == block_b) { fail(p, "cyclic/invalid phi node"); return; }
        p->use_block = block_a; value_t *a = ref(p, p->rec[1], true);
        p->use_block = block_b; value_t *b = ref(p, p->rec[3], true); p->use_block = -1;
        if (a->type != type || b->type != type) { fail(p, "phi operand types"); return; }
        opnd_t args[2] = { source(p, a), source(p, b) }; result(p, type, OP_PHI, 2, args, false);
        if (!p->bad) { p->s->ins[p->s->nins - 1].jump = block_a; p->s->ins[p->s->nins - 1].alt = block_b; }
    } else if (code == 10 || code == 11) {
        if (code == 10) { if (p->nr) { fail(p, "entry-point return type"); return; } emit(p, OP_RET, 0, NULL); }
        else {
            if (p->nr != 1 && p->nr != 3) { fail(p, "branch record"); return; }
            int a = p->rec[0], b = p->nr == 3 ? (int)p->rec[1] : a;
            if (a <= p->block || b <= p->block || a >= p->nblocks || b >= p->nblocks) { fail(p, "backward/cyclic branches are not implemented"); return; }
            value_t *cond = p->nr == 3 ? ref(p, p->rec[2], false) : NULL;
            if (cond && !scalar_type(p, cond->type, false, 1)) { fail(p, "branch condition type"); return; }
            opnd_t condition = cond ? source(p, cond) : immediate(0);
            ins_t *I = emit(p, OP_BRANCH, p->nr == 3 ? 1 : 0, &condition);
            if (I) { I->jump = a; I->alt = b; }
            p->branch_a[p->block] = a; p->branch_b[p->block] = b;
        }
        if (++p->block < p->nblocks) { p->block_pc[p->block] = p->s->nins; ins_t *I = emit(p, OP_BLOCK, 0, NULL); if (I) I->jump = p->block; }
    } else {
        if (p->err && p->errlen > 0) snprintf(p->err, p->errlen, "DXIL: LLVM instruction record %u is not implemented", code);
        p->bad = true;
    }
}

static void on_record(parser_t *p, unsigned block, uint32_t code) {
    if (block == 17 && !p->pass) type_record(p, code);
    else if (block == 11) constant_record(p, code);
    else if (block == 15 && !p->pass) metadata_record(p, code);
    else if (block == 14 && !p->pass && code == 1) {
        if (!p->nr || p->rec[0] >= (uint64_t)p->nvalues || p->values[p->rec[0]].kind != V_FUNCTION) { fail(p, "value symbol table"); return; }
        string_record(p, 1, p->values[p->rec[0]].name, sizeof p->values[0].name);
    } else if (block == 12 && p->pass) function_record(p, code);
    else if (block == 8 && !p->pass) {
        if (code == 1) { if (p->nr != 1 || p->rec[0] != 1) fail(p, "only LLVM bitcode version 1 is implemented"); else p->version = true; }
        else if (code == 2) { char triple[64] = { 0 }; string_record(p, 0, triple, sizeof triple); if (strcmp(triple, "dxil-ms-dx")) fail(p, "not a DXIL target triple"); else p->triple = true; }
        else if (code == 4 || code == 7 || code == 9) fail(p, "assembly/global memory is not implemented");
        else if (code == 8) {
            if (p->nr < 8) { fail(p, "function declaration"); return; }
            int type = p->rec[0]; ty_t *t = type_at(p, type);
            if (t->kind == TY_PTR) { type = t->element; t = type_at(p, type); }
            if (t->kind != TY_FUNCTION) { fail(p, "function type"); return; }
            value_t *v = new_value(p, p->rec[0]); v->kind = V_FUNCTION; v->fn_type = type; v->declaration = p->rec[2] != 0;
            if (!v->declaration) {
                p->entry = p->nvalues - 1;
                if (++p->defined_functions != 1 || t->count || type_at(p, t->result)->kind != TY_VOID) fail(p, "only one void entry-point function is implemented");
            }
        }
    }
}
static bool wanted_block(parser_t *p, unsigned id) {
    if (!p->pass) return id == 0 || id == 8 || id == 11 || id == 14 || id == 15 || id == 17;
    return id == 11 || id == 12;
}
static void block(parser_t *p, unsigned id, unsigned width, uint32_t end, int depth) {
    if (p->bad) return;
    if (depth > 16 || id >= BC_BLOCKS || width < 2 || width > 16 || end > p->limit) { fail(p, "invalid block header"); return; }
    uint32_t parent_limit = p->limit; p->limit = end;
    ablist_t list = p->info[id]; int info_target = -1;
    while (p->bit < end && !p->bad) {
        unsigned entry = (unsigned)bits(p, width);
        if (!entry) { align32(p); if (p->bit != end) fail(p, "block length mismatch"); break; }
        if (entry == 1) {
            unsigned child = (unsigned)vbr(p, 8), cw = (unsigned)vbr(p, 4); align32(p);
            uint64_t words = bits(p, 32), stop = (uint64_t)p->bit + words * 32;
            if (child >= BC_BLOCKS || cw < 2 || cw > 16 || stop > end) { fail(p, "sub-block is out of bounds"); break; }
            if (child == 12 && !p->pass) {
                if (p->body_start) { fail(p, "multiple function bodies are not implemented"); break; }
                p->body_start = p->bit; p->body_end = (uint32_t)stop; p->body_width = cw;
            } else if (wanted_block(p, child)) {
                if (child == 8) { if (p->module_seen) fail(p, "duplicate module"); p->module_seen = true; }
                block(p, child, cw, (uint32_t)stop, depth + 1);
            }
            p->bit = (uint32_t)stop;
        } else if (entry == 2) {
            int ab = read_abbrev(p);
            ablist_t *target = id == 0 && info_target >= 0 ? &p->info[info_target] : &list;
            if (target->n == BC_BLOCK_ABBREVS) { fail(p, "too many block abbreviations"); break; }
            target->id[target->n++] = ab;
        } else {
            uint32_t code = record(p, &list, entry);
            if (p->bad) break;
            if (id == 0) { if (code == 1) { if (p->nr != 1 || p->rec[0] >= BC_BLOCKS) fail(p, "invalid block-info target"); else info_target = p->rec[0]; } }
            else on_record(p, id, code);
        }
    }
    if (p->bit > end) fail(p, "block overrun");
    p->limit = parent_limit;
}

dxbc_shader_t *dxil_load(const uint8_t *program, uint32_t size, dxbc_shader_t *s, char *err, int errlen) {
    #define REJECT(msg) do { if (err && errlen > 0) snprintf(err, errlen, "DXIL: %s", msg); return NULL; } while (0)
    if (size < 24 || (size & 3) || u32(program + 8) != 0x4C495844u) REJECT("bad program header");
    uint32_t version = u32(program), words = u32(program + 4), offset = u32(program + 16), length = u32(program + 20);
    if (words < 6 || words > size / 4 || offset < 16 || offset > words * 4 - 8 || length > words * 4 - 8 - offset || length < 8 || length > 1024 * 1024) REJECT("bitcode is out of bounds");
    s->type = version >> 16; s->major = version >> 4 & 15; s->minor = version & 15; s->dxil = true;
    if (s->type > 1) REJECT("only vertex/pixel stages are implemented; compute/geometry/tessellation are not");
    if (s->major != 6 || s->minor != 0 || u32(program + 12) != 0x100) REJECT("only the SM6.0 / DXIL 1.0 subset is implemented");
    const uint8_t *bytes_ = program + 8 + offset;
    if (u32(bytes_) != 0xDEC04342u) REJECT("not LLVM bitcode");
    parser_t *p = kzalloc(sizeof *p);
    if (!p) REJECT("out of memory");
    p->bytes = bytes_; p->size = length; p->bit = 32; p->limit = length * 8;
    p->s = s; p->err = err; p->errlen = errlen; p->entry = p->entry_metadata = p->version_metadata = p->model_metadata = p->use_block = -1;
    memset(p->sig_map, 0xFF, sizeof p->sig_map);
    p->ab = kzalloc(sizeof(abbrev_t) * BC_ABBREVS);
    p->types = kzalloc(sizeof(ty_t) * BC_TYPES);
    p->values = kzalloc(sizeof(value_t) * BC_VALUES);
    p->md = kzalloc(sizeof(md_t) * BC_METADATA);
    if (!p->ab || !p->types || !p->values || !p->md) fail(p, "out of memory");
    if (!p->bad) block(p, 7, 2, length * 8, 0);  /* root is not a module block */
    if (!p->bad && (!p->version || !p->triple || p->defined_functions != 1 || !p->body_start || p->ntypes != p->expected_types)) fail(p, "incomplete module");
    p->module_values = p->nvalues;
    if (!p->bad) {
        md_t *version_md = node(p, p->version_metadata + 1, 2), *model_md = node(p, p->model_metadata + 1, 3);
        if (!p->bad) {
            md_t *stage = md_at(p, model_md->refs[0]);
            if (version_md->n != 2 || md_number(p, version_md->refs[0]) != 1 || md_number(p, version_md->refs[1]) ||
                model_md->n != 3 || stage->kind != 1 || strcmp(stage->text, s->type ? "vs" : "ps") ||
                md_number(p, model_md->refs[1]) != 6 || md_number(p, model_md->refs[2])) fail(p, "shader model/version metadata mismatch");
        }
    }
    if (!p->bad) signature_metadata(p);
    if (!p->bad) {
        for (int i = 0; i < BC_BBS; i++) p->branch_a[i] = p->branch_b[i] = p->block_pc[i] = -1;
        p->pass = 1; p->bit = p->body_start; p->limit = length * 8;
        block(p, 12, p->body_width, p->body_end, 0);
        if (p->block != p->nblocks) fail(p, "unterminated function block");
    }
    if (!p->bad) for (int i = 0; i < s->nins; i++) {
        ins_t *I = &s->ins[i];
        if (I->op == OP_BRANCH) { I->jump = p->block_pc[I->jump]; I->alt = p->block_pc[I->alt]; }
        else if (I->op == OP_PHI) {
            int owner = -1;
            for (int b = 0; b < p->nblocks; b++) if (p->block_pc[b] <= i) owner = b;
            if (owner < 0 || (p->branch_a[I->jump] != owner && p->branch_b[I->jump] != owner) ||
                (p->branch_a[I->alt] != owner && p->branch_b[I->alt] != owner)) fail(p, "phi predecessor mismatch");
            for (int b = 0; b < owner; b++) if ((p->branch_a[b] == owner || p->branch_b[b] == owner) && b != I->jump && b != I->alt) fail(p, "phi is missing a predecessor");
        }
    }
    if (!p->bad) {
        uint32_t dom[BC_BBS][BC_BBS / 32] = { 0 };
        for (int b = 0; b < p->nblocks && !p->bad; b++) {
            bool found = b == 0;
            for (int a = 0; a < b; a++) if (p->branch_a[a] == b || p->branch_b[a] == b) {
                if (!found) memcpy(dom[b], dom[a], sizeof dom[b]);
                else for (int k = 0; k < BC_BBS / 32; k++) dom[b][k] &= dom[a][k];
                found = true;
            }
            if (!found) { fail(p, "unreachable basic blocks are not implemented"); break; }
            dom[b][b / 32] |= 1u << (b & 31);
            for (int k = 0; k < BC_BBS / 32; k++) if (p->dependencies[b][k] & ~dom[b][k]) fail(p, "SSA definition does not dominate its use");
        }
    }
    if (!p->bad) {
        uint8_t written[BC_BBS][DX_MAX_REG] = { 0 }, returns[DX_MAX_REG] = { 0 };
        bool have_return = false;
        for (int b = 0; b < p->nblocks; b++) {
            bool first = true;
            for (int a = 0; a < b; a++) if (p->branch_a[a] == b || p->branch_b[a] == b) {
                if (first) memcpy(written[b], written[a], DX_MAX_REG);
                else for (int k = 0; k < DX_MAX_REG; k++) written[b][k] &= written[a][k];
                first = false;
            }
            int end = b + 1 < p->nblocks ? p->block_pc[b + 1] : s->nins;
            for (int i = p->block_pc[b]; i < end; i++) {
                ins_t *I = &s->ins[i];
                if (I->op == OP_MOV) {
                    opnd_t *o = &s->pool[I->o[0]];
                    if (o->type == T_OUTPUT) written[b][o->idx[0]] |= o->mask;
                    else if (o->type == T_ODEPTH) written[b][DX_MAX_REG - 1] |= 1;
                } else if (I->op == OP_RET) {
                    if (!have_return) memcpy(returns, written[b], DX_MAX_REG);
                    else for (int k = 0; k < DX_MAX_REG; k++) returns[k] &= written[b][k];
                    have_return = true;
                }
            }
        }
        memcpy(s->definite_mask, returns, DX_MAX_REG);
    }
    bool ok = !p->bad;
    kfree(p->ab); kfree(p->types); kfree(p->values); kfree(p->md); kfree(p);
    return ok ? s : NULL;
    #undef REJECT
}
