/* Shared native shader IR. DXBC tokens and DXIL bitcode are frontend formats;
 * neither is executed as host machine code or requires a compiler at run time. */
#ifndef SHADER_PRIV_H
#define SHADER_PRIV_H
#include "dxbc.h"

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
    OP_RCP = 129, OP_F32TOF16, OP_F16TOF32, OP_UADDC, OP_USUBB, OP_COUNTBITS,
    OP_FIRSTBIT_HI, OP_FIRSTBIT_LO, OP_FIRSTBIT_SHI, OP_UBFE, OP_IBFE, OP_BFI, OP_BFREV, OP_SWAPC,
    /* Internal IR only: acyclic DXIL control flow with two-way phi nodes. */
    OP_BLOCK = 256, OP_BRANCH, OP_PHI,
};
enum { T_TEMP, T_INPUT, T_OUTPUT, T_XTEMP, T_IMM32, T_IMM64, T_SAMPLER, T_RESOURCE, T_CB, T_ICB,
       T_LABEL, T_PRIMID, T_ODEPTH, T_NULL };

typedef struct {
    uint8_t type, mode, mask, mod, dim, direct, identity;
    uint8_t swz[4];
    int16_t rel[3];
    int32_t idx[3];
    uint32_t imm[4];
} opnd_t;
typedef struct {
    uint16_t op;
    uint8_t sat, nonzero, nops, ret_type, mask;
    int16_t o[5];
    int32_t jump, alt;
} ins_t;
#define MAX_XTEMP 16
#define SHADER_MAX_OPERANDS 32767
#define SHADER_MAX_INS 65536
#define SHADER_MAX_TEMPS 4096

struct dxbc_shader {
    int type, major, minor;
    bool dxil, writes_depth;
    dxsig_t in[DX_MAX_SIG], out[DX_MAX_SIG];
    int nin, nout;
    ins_t *ins; int nins;
    opnd_t *pool; int npool;
    int ntemps;
    struct { int reg, size; } xt[MAX_XTEMP]; int nxt, xt_total;
    vec4_t *icb; int nicb;
    uint8_t read_mask[DX_MAX_REG], write_mask[DX_MAX_REG], definite_mask[DX_MAX_REG];
    uint8_t interpolation[DX_MAX_REG];
    uint32_t interpolation_declared;
    uint8_t clear_regs[DX_MAX_REG];
    int nclear;
};

/* Frontends must validate their own encodings/control flow; this checks the
 * executable IR and computes register usage/bindings for both formats. */
bool shader_finish(dxbc_shader_t *s, char *err, int errlen);
dxbc_shader_t *dxil_load(const uint8_t *program, uint32_t size, dxbc_shader_t *s, char *err, int errlen);
#endif
