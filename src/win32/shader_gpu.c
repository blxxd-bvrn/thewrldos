/* Native SVGA3D shader lowering, not an HLSL/LLVM runtime. The initial hardware
 * subset uses the device's SM3 command format, with explicit CPU fallback for
 * integer/bit operations, large temporary banks, unsupported flow or linkage.
 * Float comparison flags are tracked: SM3 float booleans must never escape into
 * DXBC's raw all-ones integer-bit semantics. No shader bytes execute as CPU code. */
#include "shader_priv.h"
#include "shader_gpu.h"

#define WORDS 8192
#define TEMPS 28                       /* r28-r30 scratch, r31 homogeneous position */
enum { SM_MOV=1, SM_ADD=2, SM_SUB=3, SM_MAD=4, SM_MUL=5, SM_RCP=6, SM_RSQ=7, SM_DP3=8, SM_DP4=9,
       SM_MIN=10, SM_MAX=11, SM_SLT=12, SM_SGE=13, SM_EXP=14, SM_LOG=15, SM_FRC=19,
       SM_DCL=31, SM_IFC=41, SM_ELSE=42, SM_ENDIF=43, SM_TEX=66, SM_DEF=81, SM_TEXL=95 };
enum { R_TEMP=0, R_INPUT=1, R_CONST=2, R_OUTPUT=6, R_COLOR=8, R_SAMPLER=10 };
typedef struct { uint8_t may[32], must[32], positive[32]; } types_t;
typedef struct {
    const dxbc_shader_t *s, *vs;
    shader_gpu_t *p;
    uint32_t code[WORDS]; int n;
    vec4_t imm[224]; int nimm, zero;
    int input[DX_MAX_REG], output[DX_MAX_REG], semantic[DX_MAX_REG];
    uint8_t in_mask[DX_MAX_REG], out_mask[DX_MAX_REG];
    int pos;
    types_t types;
    struct { types_t before, yes; bool have_else; } flow[24]; int depth;
    bool bad; char *err; int errlen;
} build_t;
static uint64_t next_serial = 16;

static void bad(build_t *b, const char *why) { if (!b->bad && b->err && b->errlen > 0) snprintf(b->err,b->errlen,"GPU fallback: %s",why); b->bad=true; }
static uint32_t reg(int type, int index) { return 0x80000000u | ((type & 7u)<<28) | ((type & 24u)<<8) | (uint32_t)index; }
static uint32_t dst(int type, int index, int mask, bool sat) { return reg(type,index) | ((uint32_t)mask<<16) | (sat ? 1u<<20 : 0); }
static uint32_t src(int type, int index) { return reg(type,index) | 0x00E40000u; }
static uint32_t lane(uint32_t token, int c) { unsigned swz=(token>>(16+2*c))&3; return (token&~0x00FF0000u) | ((swz*0x55u)<<16); }
static void word(build_t *b,uint32_t w) { if (b->n==WORDS) bad(b,"shader token limit"); else b->code[b->n++]=w; }
static void ins(build_t *b,int op,int n,const uint32_t *args) { word(b,op|((uint32_t)n<<24)); for(int i=0;i<n;i++) word(b,args[i]); }
static void unary(build_t *b,int op,uint32_t d,uint32_t a) { uint32_t x[]={d,a}; ins(b,op,2,x); }
static void binary(build_t *b,int op,uint32_t d,uint32_t a,uint32_t c) { uint32_t x[]={d,a,c}; ins(b,op,3,x); }
static void read_ports(build_t *b,uint32_t *a,int n) {
    /* Both SM3 stages have just one distinct c# and one distinct v# read port
     * per instruction. Stage extra original sources before special lowering
     * uses r28-r30; preserve the original swizzles/modifiers on each use. */
    int first[2]={-1,-1},saved_type[2],saved_reg[2],saved=0;
    for(int i=0;i<n;i++) {
        int type=((a[i]>>28)&7)|((a[i]>>8)&24),r=a[i]&2047;
        int bank=type==R_CONST?0:type==R_INPUT?1:-1;
        if(bank<0)continue;
        if(first[bank]<0){first[bank]=r;continue;}
        if(first[bank]==r)continue;
        int j;for(j=0;j<saved;j++)if(saved_type[j]==type&&saved_reg[j]==r)break;
        if(j==saved) {
            if(saved==2){bad(b,"source read port scratch limit");return;}
            saved_type[saved]=type;saved_reg[saved++]=r;
            unary(b,SM_MOV,dst(R_TEMP,30-j,15,false),src(type,r));
        }
        a[i]=reg(R_TEMP,30-j)|(a[i]&0x0FFF0000u);
    }
}
static int immediate(build_t *b,const uint32_t *v) {
    for(int i=0;i<b->nimm;i++) if(!memcmp(b->imm[i].u,v,16)) return i;
    if(b->nimm==224) { bad(b,"constant limit"); return 0; }
    memcpy(b->imm[b->nimm].u,v,16); return b->nimm++;
}
static int cb_find(build_t *b,int slot,int row) {
    for(int i=0;i<b->p->ncb;i++) if(b->p->cb[i].slot==slot&&b->p->cb[i].row==row) return i;
    if(b->p->ncb==224) { bad(b,"constant buffer register limit"); return 0; }
    int n=b->p->ncb++; b->p->cb[n].slot=slot; b->p->cb[n].row=row; return n;
}
static uint64_t signature_key(const dxbc_shader_t *s) {
    uint64_t h=1469598103934665603ull;
    for(int i=0;i<s->nout;i++) {
        const dxsig_t *v=&s->out[i];
        for(const char *p=v->name;*p;p++) { unsigned c=(uint8_t)*p; if(c>='A'&&c<='Z') c+=32; h=(h^c)*1099511628211ull; }
        const uint32_t a[]={v->index,v->sysval,v->reg,v->mask,v->component_type};
        for(unsigned j=0;j<ARRAY_LEN(a);j++) h=(h^a[j])*1099511628211ull;
    }
    return h;
}
static bool vertex_outputs(build_t *b,const dxbc_shader_t *s,int *map) {
    int position=-1, tex=1;
    for(int i=0;i<DX_MAX_REG;i++) map[i]=-1;
    for(int i=0;i<s->nout;i++) {
        const dxsig_t *v=&s->out[i];
        if(v->reg<0||v->reg>=DX_MAX_REG||v->component_type!=3|| (v->sysval!=0&&v->sysval!=1)) { bad(b,"vertex output type/semantic"); return false; }
        if(v->sysval==1) { if(position>=0&&position!=v->reg) { bad(b,"multiple positions"); return false; } position=v->reg; map[v->reg]=0; }
    }
    if(position<0) { bad(b,"missing vertex position"); return false; }
    for(int i=0;i<s->nout;i++) {
        int r=s->out[i].reg;
        if(map[r]<0) { if(tex>10) { bad(b,"varying register limit"); return false; } map[r]=tex++; }
    }
    return true;
}
static bool link(build_t *b) {
    const dxbc_shader_t *s=b->s;
    for(int i=0;i<DX_MAX_REG;i++) b->input[i]=b->output[i]=b->semantic[i]=-1;
    if(s->type==1) {
        if(!vertex_outputs(b,s,b->output)) return false;
        for(int i=0;i<s->nin;i++) if(s->in[i].sysval||s->in[i].component_type!=3) { bad(b,"vertex system/integer inputs"); return false; }
        for(int r=0;r<DX_MAX_REG;r++) if(s->read_mask[r]) {
            int i=b->p->ninputs;
            if(i==16) { bad(b,"vertex input register limit"); return false; }
            b->p->input_reg[i]=r; b->p->ninputs++; b->input[r]=i; b->in_mask[r]=s->read_mask[r];
        }
        for(int i=0;i<s->nout;i++) { const dxsig_t *v=&s->out[i]; b->out_mask[v->reg]|=v->mask; if(v->sysval==1) b->pos=v->reg; }
        b->p->link_key=signature_key(s);
    } else {
        int output[DX_MAX_REG];
        if(!b->vs||!vertex_outputs(b,b->vs,output)||s->writes_depth) { bad(b,"pixel depth/linkage"); return false; }
        for(int i=0;i<s->nin;i++) {
            const dxsig_t *v=&s->in[i];
            int mask=s->read_mask[v->reg]&v->mask;
            if(!mask) continue;
            if(v->sysval||v->component_type!=3||s->interpolation[v->reg]!=2) { bad(b,"pixel system/integer/non-perspective inputs"); return false; }
            int k; for(k=0;k<b->vs->nout;k++) if(!strcasecmp(v->name,b->vs->out[k].name)&&v->index==b->vs->out[k].index) break;
            if(k==b->vs->nout||v->mask!=b->vs->out[k].mask) { bad(b,"packed-component remapping"); return false; }
            int semantic=output[b->vs->out[k].reg]-1;
            if(semantic<0) { bad(b,"position used as varying"); return false; }
            if(b->input[v->reg]<0) {
                if(b->p->ninputs==10) { bad(b,"pixel input register limit"); return false; }
                b->p->input_reg[b->p->ninputs]=v->reg; b->input[v->reg]=b->p->ninputs++; b->semantic[v->reg]=semantic;
            } else if(b->semantic[v->reg]!=semantic) { bad(b,"split packed varying"); return false; }
            b->in_mask[v->reg]|=mask;
        }
        bool found=false;
        for(int i=0;i<s->nout;i++) {
            const dxsig_t *v=&s->out[i];
            if(v->sysval!=64||v->index||v->component_type!=3||found) { bad(b,"pixel output type/semantic"); return false; }
            b->output[v->reg]=0; b->out_mask[v->reg]=v->mask; found=true;
        }
        if(!found) { bad(b,"no pixel target"); return false; }
        b->p->link_key=signature_key(b->vs);
    }
    return true;
}
static uint32_t source(build_t *b,const opnd_t *o) {
    int type=0,index=0;
    if(o->type==T_TEMP&&o->mod) for(int i=0;i<4;i++) if(b->types.may[o->idx[0]]&(1u<<o->swz[i])) bad(b,"modified raw boolean");
    switch(o->type) {
    case T_TEMP: type=R_TEMP; index=o->idx[0]; break;
    case T_INPUT: type=R_INPUT; index=b->input[o->idx[0]]; if(index<0) bad(b,"undeclared input"); break;
    case T_CB: type=R_CONST; index=cb_find(b,o->idx[0],o->idx[1]); break;
    case T_IMM32: type=R_CONST; index=b->p->ncb+immediate(b,o->imm); break;
    default: bad(b,"operand addressing/type");
    }
    uint32_t v=reg(type,MAX(index,0));
    for(int i=0;i<4;i++) v|=(uint32_t)o->swz[i]<<(16+2*i);
    if(o->mod) v|=(uint32_t)(o->mod==1?1:o->mod==2?11:12)<<24;
    return v;
}
static uint32_t destination(build_t *b,const opnd_t *o,int mask,bool sat) {
    if(o->type==T_TEMP) return dst(R_TEMP,o->idx[0],mask,sat);
    if(o->type==T_OUTPUT&&b->output[o->idx[0]]>=0) {
        if(b->s->type==1) return o->idx[0]==b->pos ? dst(R_TEMP,31,mask,sat) : dst(R_OUTPUT,b->output[o->idx[0]],mask,sat);
        return dst(R_COLOR,0,mask,sat);
    }
    bad(b,"destination addressing/type"); return 0;
}
static bool flag(build_t *b,const opnd_t *o,int lane_index,int kind) {
    int c=o->swz[lane_index];
    if(o->type==T_TEMP) {
        uint8_t f=kind==0?b->types.may[o->idx[0]]:kind==1?b->types.must[o->idx[0]]:b->types.positive[o->idx[0]];
        if(kind<2&&o->mod) return false;
        if(kind==2&&(o->mod&2)&&!(o->mod&1)) return true;
        if(kind==2&&(o->mod&1)) return false;
        return (f&(1u<<c))!=0;
    }
    if(kind==2) {
        if(o->mod&2) return !(o->mod&1);
        if(o->type==T_IMM32) return !((o->imm[c]>>31)^(o->mod&1));
    }
    return false;
}
static bool numeric(build_t *b,const opnd_t *o,int mask) {
    if(o->type==T_TEMP) for(int i=0;i<4;i++) if((mask&(1u<<i)) && (b->types.may[o->idx[0]]&(1u<<o->swz[i]))) { bad(b,"integer comparison flags used as float data"); return false; }
    return true;
}
static void result_types(build_t *b,const opnd_t *d,int may,int must,int positive) {
    if(d->type==T_TEMP) {
        int r=d->idx[0],mask=d->mask;
        b->types.may[r]=(b->types.may[r]&~mask)|(may&mask);
        b->types.must[r]=(b->types.must[r]&~mask)|(must&mask);
        b->types.positive[r]=(b->types.positive[r]&~mask)|(positive&mask);
    } else if(may) bad(b,"raw boolean output");
}
static bool same(const opnd_t *a,const opnd_t *c) {
    return a->type==c->type&&a->mod==c->mod&&!memcmp(a->idx,c->idx,sizeof a->idx)&&!memcmp(a->swz,c->swz,4)&&
        (a->type!=T_IMM32||!memcmp(a->imm,c->imm,16));
}
static void comparison_if(build_t *b,uint32_t a,uint32_t c,int comparison) {
    word(b,SM_IFC|(2u<<24)|((uint32_t)comparison<<16)); word(b,a); word(b,c);
}
static int sample_slot(build_t *b,const opnd_t *t,const opnd_t *s) {
    for(int i=0;i<b->p->ntextures;i++) if(b->p->texture[i].texture==t->idx[0]&&b->p->texture[i].sampler==s->idx[0]) return i;
    int i=b->p->ntextures;
    if(i==8) { bad(b,"texture/sampler pair limit"); return 0; }
    b->p->texture[i].texture=t->idx[0]; b->p->texture[i].sampler=s->idx[0]; b->p->ntextures++; return i;
}
static void instruction(build_t *b,const ins_t *I,int pc) {
    const dxbc_shader_t *s=b->s;
    const opnd_t *o[5]={0}; for(int i=0;i<I->nops;i++) o[i]=&s->pool[I->o[i]];
    uint32_t zero=src(R_CONST,b->p->ncb+b->zero);
    if(I->op==OP_BLOCK||I->op==OP_NOP) return;
    if(I->op==OP_RET) { if(pc!=s->nins-1) bad(b,"early returns"); return; }
    if(I->op==OP_IF) {
        if(b->depth==24||!flag(b,o[0],0,1)) { bad(b,"non-boolean/deep conditional"); return; }
        b->flow[b->depth].before=b->types; b->flow[b->depth].have_else=false; b->depth++;
        comparison_if(b,lane(source(b,o[0]),0),lane(zero,0),I->nonzero?5:2); return;
    }
    if(I->op==OP_ELSE) {
        if(!b->depth||b->flow[b->depth-1].have_else) { bad(b,"conditional structure"); return; }
        b->flow[b->depth-1].yes=b->types; b->types=b->flow[b->depth-1].before; b->flow[b->depth-1].have_else=true;
        ins(b,SM_ELSE,0,NULL); return;
    }
    if(I->op==OP_ENDIF) {
        if(!b->depth) { bad(b,"conditional structure"); return; }
        int n=--b->depth;
        types_t *other=b->flow[n].have_else?&b->flow[n].yes:&b->flow[n].before;
        for(int i=0;i<32;i++) { b->types.may[i]|=other->may[i]; b->types.must[i]&=other->must[i]; b->types.positive[i]&=other->positive[i]; }
        ins(b,SM_ENDIF,0,NULL); return;
    }
    int op=-1, arity=0, source_mask=I->mask;
    switch(I->op) {
    case OP_MOV: op=SM_MOV; arity=1; break;
    case OP_ADD: op=SM_ADD; arity=2; break; case OP_MUL: op=SM_MUL; arity=2; break;
    case OP_MAD: op=SM_MAD; arity=3; break; case OP_MIN: op=SM_MIN; arity=2; break; case OP_MAX: op=SM_MAX; arity=2; break;
    case OP_DP2: op=SM_DP3; arity=2; source_mask=3; break;
    case OP_DP3: op=SM_DP3; arity=2; source_mask=7; break; case OP_DP4: op=SM_DP4; arity=2; source_mask=15; break;
    case OP_FRC: op=SM_FRC; arity=1; break; case OP_RCP: op=SM_RCP; arity=1; break;
    case OP_RSQ: op=SM_RSQ; arity=1; break; case OP_EXP: op=SM_EXP; arity=1; break; case OP_LOG: op=SM_LOG; arity=1; break;
    case OP_DIV: op=SM_MUL; arity=2; break; case OP_SQRT: op=SM_RSQ; arity=1; break;
    case OP_ROUND_NI: op=SM_FRC; arity=1; break;
    case OP_LT: op=SM_SLT; arity=2; break; case OP_GE: op=SM_SGE; arity=2; break;
    case OP_MOVC: op=SM_MOV; arity=3; break;
    case OP_SAMPLE: case OP_SAMPLE_L: op=SM_TEX; arity=1; source_mask=3; break;
    default: bad(b,"instruction not in hardware subset"); return;
    }
    if(!I->mask) return;
    uint32_t d=destination(b,o[0],o[0]->mask,I->sat), a[3]={0};
    for(int i=0;i<arity;i++) { a[i]=source(b,o[i+1]); if(I->op!=OP_MOV&&I->op!=OP_MOVC&&!numeric(b,o[i+1],source_mask)) return; }
    if(I->sat&&(I->op==OP_MOV||I->op==OP_MOVC))
        for(int i=I->op==OP_MOV?1:2;i<=arity;i++)if(!numeric(b,o[i],I->mask))return;
    read_ports(b,a,arity);
    int may=0,must=0,positive=0;
    for(int i=0;i<4;i++) if(I->mask&(1u<<i)) {
        bool p=flag(b,o[1],i,2);
        if(I->op==OP_MOV) { if(flag(b,o[1],i,0)) may|=1u<<i; if(flag(b,o[1],i,1)) must|=1u<<i; }
        if(I->op==OP_LT||I->op==OP_GE) { may|=1u<<i; must|=1u<<i; p=true; }
        else if(I->op==OP_ADD||I->op==OP_MUL||I->op==OP_MIN||I->op==OP_MAX||I->op==OP_DIV) p=p&&flag(b,o[2],i,2);
        else if(I->op==OP_MAD) p=p&&flag(b,o[2],i,2)&&flag(b,o[3],i,2);
        else if(I->op==OP_FRC||I->op==OP_EXP) p=true;
        else if(I->op==OP_DP2||I->op==OP_DP3||I->op==OP_DP4) p=same(o[1],o[2]);
        if(I->sat||p) positive|=1u<<i;
    }
    if((I->op==OP_LT||I->op==OP_GE)&&!b->p->vertex) {
        /* SLT/SGE are vertex-only opcodes in SM3. Ordered scalar IFCs produce
         * the tracked float booleans without a subtract/CMP NaN ambiguity. */
        const uint32_t ones[4]={0x3F800000,0x3F800000,0x3F800000,0x3F800000};
        uint32_t one=src(R_CONST,b->p->ncb+immediate(b,ones));
        if(o[0]->type==T_TEMP)for(int k=0;k<2;k++)if(o[k+1]->type==T_TEMP&&o[k+1]->idx[0]==o[0]->idx[0]) {
            unary(b,SM_MOV,dst(R_TEMP,29+k,15,false),a[k]);a[k]=src(R_TEMP,29+k);
        }
        for(int i=0;i<4;i++)if(I->mask&(1u<<i)) {
            comparison_if(b,lane(a[0],i),lane(a[1],i),I->op==OP_LT?4:3);
            unary(b,SM_MOV,destination(b,o[0],1u<<i,false),one);ins(b,SM_ELSE,0,NULL);
            unary(b,SM_MOV,destination(b,o[0],1u<<i,false),zero);ins(b,SM_ENDIF,0,NULL);
        }
    } else if(I->op==OP_MOVC) {
        may=must=positive=0;
        if(o[0]->type==T_TEMP) for(int k=0;k<3;k++) if(o[k+1]->type==T_TEMP&&o[k+1]->idx[0]==o[0]->idx[0]) {
            unary(b,SM_MOV,dst(R_TEMP,28+k,15,false),a[k]); a[k]=src(R_TEMP,28+k);
        }
        for(int i=0;i<4;i++) if(I->mask&(1u<<i)) {
            if(!flag(b,o[1],i,1)) { bad(b,"raw/non-boolean conditional move"); return; }
            comparison_if(b,lane(a[0],i),lane(zero,0),5);
            unary(b,SM_MOV,destination(b,o[0],1u<<i,I->sat),a[1]); ins(b,SM_ELSE,0,NULL);
            unary(b,SM_MOV,destination(b,o[0],1u<<i,I->sat),a[2]); ins(b,SM_ENDIF,0,NULL);
            if(flag(b,o[2],i,0)||flag(b,o[3],i,0)) may|=1u<<i;
            if(flag(b,o[2],i,1)&&flag(b,o[3],i,1)) must|=1u<<i;
            if(I->sat||(flag(b,o[2],i,2)&&flag(b,o[3],i,2))) positive|=1u<<i;
        }
    } else if(I->op==OP_SAMPLE||I->op==OP_SAMPLE_L) {
        if(s->type!=0) { bad(b,"vertex texture sampling"); return; }
        if(I->op==OP_SAMPLE&&b->depth) { bad(b,"implicit texture gradients in dynamic flow"); return; }
        int slot=sample_slot(b,o[2],o[3]);
        uint32_t coord=a[0];
        if(I->op==OP_SAMPLE_L) { unary(b,SM_MOV,dst(R_TEMP,28,3,false),coord); unary(b,SM_MOV,dst(R_TEMP,28,8,false),zero); coord=src(R_TEMP,28); }
        uint32_t x[]={dst(R_TEMP,29,15,false),coord,src(R_SAMPLER,slot)};
        ins(b,I->op==OP_SAMPLE_L?SM_TEXL:SM_TEX,3,x);
        uint32_t result=reg(R_TEMP,29); for(int i=0;i<4;i++) result|=(uint32_t)o[2]->swz[i]<<(16+2*i);
        unary(b,SM_MOV,d,result); positive=I->mask;
    } else if(I->op==OP_RCP||I->op==OP_RSQ||I->op==OP_SQRT||I->op==OP_EXP||I->op==OP_LOG||I->op==OP_DIV) {
        if(o[0]->type==T_TEMP) for(int k=0;k<arity;k++) if(o[k+1]->type==T_TEMP&&o[k+1]->idx[0]==o[0]->idx[0]) {
            unary(b,SM_MOV,dst(R_TEMP,29+k,15,false),a[k]); a[k]=src(R_TEMP,29+k);
        }
        for(int i=0;i<4;i++) if(I->mask&(1u<<i)) {
            if((I->op==OP_RSQ||I->op==OP_SQRT||I->op==OP_LOG)&&!flag(b,o[1],i,2)) { bad(b,"signed rsq/sqrt/log cannot be represented by legacy hardware"); return; }
            uint32_t dd=destination(b,o[0],1u<<i,I->sat);
            if(I->op==OP_DIV) { unary(b,SM_RCP,dst(R_TEMP,28,1,false),lane(a[1],i)); binary(b,SM_MUL,dd,a[0],lane(src(R_TEMP,28),0)); }
            else if(I->op==OP_SQRT) { unary(b,SM_RSQ,dst(R_TEMP,28,1,false),lane(a[0],i)); unary(b,SM_RCP,dd,lane(src(R_TEMP,28),0)); }
            else unary(b,op,dd,lane(a[0],i));
        }
    } else if(I->op==OP_DP2) {
        binary(b,SM_MUL,dst(R_TEMP,28,3,false),a[0],a[1]); binary(b,SM_ADD,d,lane(src(R_TEMP,28),0),lane(src(R_TEMP,28),1));
    } else if(I->op==OP_ROUND_NI) {
        unary(b,SM_FRC,dst(R_TEMP,28,I->mask,false),a[0]); binary(b,SM_SUB,d,a[0],src(R_TEMP,28));
    } else {
        uint32_t args[]={d,a[0],a[1],a[2]}; ins(b,op,arity+1,args);
    }
    result_types(b,o[0],may,must,positive);
}

void shader_gpu_free(shader_gpu_t *p) { if(p) { kfree(p->words); kfree(p); } }
static void limits(build_t *b) {
    int slots=0,depth=0;
    for(int pc=1;pc<b->n;) {
        uint32_t t=b->code[pc];int op=t&65535,n=(t>>24)&15;
        if(op==65535)break;
        if(op==SM_IFC){slots+=3;if(++depth>24)bad(b,"hardware branch nesting limit");}
        else {if(op==SM_ENDIF)depth--;if(op!=SM_DCL&&op!=SM_DEF)slots+=op==SM_TEXL?2:1;}
        pc+=n+1;
    }
    if(slots>512)bad(b,"hardware instruction slot limit");
}
shader_gpu_t *shader_gpu_compile(const dxbc_shader_t *s,const dxbc_shader_t *vs,char *err,int errlen) {
    if(err&&errlen>0) err[0]=0;
    if(!s) return NULL;
    build_t *b=kzalloc(sizeof *b); shader_gpu_t *p=kzalloc(sizeof *p);
    if(!b||!p) { kfree(b); kfree(p); if(err&&errlen>0) snprintf(err,errlen,"GPU fallback: memory"); return NULL; }
    b->s=s; b->vs=vs; b->p=p; b->err=err; b->errlen=errlen; b->pos=-1; p->vertex=s->type==1;
    if(s->ntemps>TEMPS||s->nxt||s->nins>512) bad(b,"temporary/instruction bank exceeds legacy GPU subset");
    if(!b->bad) link(b);
    for(int i=0;i<s->npool&&!b->bad;i++) {
        const opnd_t *o=&s->pool[i];
        if(o->rel[0]>=0||o->rel[1]>=0||o->rel[2]>=0) { bad(b,"relative addressing"); break; }
        if(o->type==T_CB) cb_find(b,o->idx[0],o->idx[1]);
        else if(o->type==T_IMM32) {
            for(int j=0;j<4;j++) if((o->imm[j]&0x7F800000u)==0x7F800000u) bad(b,"nonfinite immediate");
            immediate(b,o->imm);
        }
    }
    const uint32_t zeros[4]={0}; b->zero=immediate(b,zeros);
    /* Compile body first so sampler pairs and local DEF constants are known. */
    for(int i=0;i<s->nins&&!b->bad;i++) instruction(b,&s->ins[i],i);
    if(b->depth) bad(b,"unterminated conditional");
    if(p->ncb+b->nimm>(p->vertex?255:224)) bad(b,"hardware constant bank");
    uint32_t *body=NULL; int body_words=b->n;
    if(!b->bad) { body=kmalloc(MAX(1,body_words)*4); if(!body) bad(b,"memory"); else memcpy(body,b->code,body_words*4); }
    if(!b->bad) {
        b->n=0; word(b,p->vertex?0xFFFE0300u:0xFFFF0300u);
        for(int r=0;r<DX_MAX_REG;r++) if(b->input[r]>=0) {
            uint32_t a[]={0x80000005u|((uint32_t)(p->vertex?b->input[r]:b->semantic[r])<<16),dst(R_INPUT,b->input[r],b->in_mask[r],false)};
            ins(b,SM_DCL,2,a);
        }
        if(p->vertex) {
            for(int r=0;r<DX_MAX_REG;r++) if(b->output[r]>=0) {
                int out=b->output[r]; uint32_t a[]={out?0x80000005u|((uint32_t)(out-1)<<16):0x80000000u,dst(R_OUTPUT,out,b->out_mask[r],false)}; ins(b,SM_DCL,2,a);
            }
        } else for(int i=0;i<p->ntextures;i++) { uint32_t a[]={0x90000000u,dst(R_SAMPLER,i,15,false)}; ins(b,SM_DCL,2,a); }
        for(int i=0;i<b->nimm;i++) {
            uint32_t a[]={dst(R_CONST,p->ncb+i,15,false),b->imm[i].u[0],b->imm[i].u[1],b->imm[i].u[2],b->imm[i].u[3]}; ins(b,SM_DEF,5,a);
        }
        uint32_t zero=src(R_CONST,p->ncb+b->zero);
        if(p->vertex) {
            unary(b,SM_MOV,dst(R_TEMP,31,15,false),zero);
            for(int r=0;r<DX_MAX_REG;r++) if(b->output[r]>0) unary(b,SM_MOV,dst(R_OUTPUT,b->output[r],b->out_mask[r],false),zero);
        } else unary(b,SM_MOV,dst(R_COLOR,0,15,false),zero);
        for(int i=0;i<body_words;i++) word(b,body[i]);
        if(p->vertex) {
            /* D3D9/SVGA3D integer pixel centers -> D3D10 half-integer centers. */
            uint32_t a[]={dst(R_OUTPUT,0,3,false),lane(src(R_TEMP,31),3),src(R_CONST,255),src(R_TEMP,31)};
            ins(b,SM_MAD,4,a); unary(b,SM_MOV,dst(R_OUTPUT,0,12,false),src(R_TEMP,31));
        }
        word(b,0xFFFF);
        if(!b->bad)limits(b);
    }
    kfree(body);
    if(!b->bad) { p->words=kmalloc(b->n*4); if(!p->words) bad(b,"memory"); else { memcpy(p->words,b->code,b->n*4); p->nwords=b->n; p->serial=next_serial++; } }
    bool ok=!b->bad; kfree(b);
    if(!ok) { shader_gpu_free(p); return NULL; }
    return p;
}
void shader_gpu_constants(const shader_gpu_t *p,const dxbc_env_t *env,float (*out)[4],int view_w,int view_h) {
    memset(out,0,(p->vertex?256:224)*16);
    for(int i=0;i<p->ncb;i++) {
        int cb=p->cb[i].slot,row=p->cb[i].row;
        if(cb>=0&&cb<14&&row>=0&&env->cb[cb]&&(uint32_t)row<env->cb_rows[cb]) memcpy(out[i],env->cb[cb][row].f,16);
    }
    if(p->vertex&&view_w>0&&view_h>0) { out[255][0]=-1.0f/view_w; out[255][1]=1.0f/view_h; }
}
