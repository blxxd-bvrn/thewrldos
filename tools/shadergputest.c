/* Host checks for hardware-subset selection and native SM3 serialization. */
#define main shader_core_main
#include "shadertest.c"
#undef main
#include "shader_gpu.h"

static const char *export_directory;
static void export_gpu(const shader_gpu_t *p,const char *name) {
    if(!p||!export_directory)return;
    char path[512];snprintf(path,sizeof path,"%s/%s.sm3",export_directory,name);
    FILE *f=fopen(path,"wb");check("export GPU test shader",f!=NULL);
    if(f){check("write GPU test shader",fwrite(p->words,4,p->nwords,f)==p->nwords);fclose(f);}
}
typedef struct { vec4_t r[32],in[16],out[12],color[1],constant[256]; } sm3_state_t;
static void validate_sm3(const shader_gpu_t *p) {
    for(int pc=1;pc<(int)p->nwords;) {
        uint32_t t=p->words[pc];int op=t&65535,n=(t>>24)&15;
        if(op==65535)break;
        check("SM3 instruction bounds",pc+n<(int)p->nwords);
        if(pc+n>=(int)p->nwords)break;
        if(op!=31&&op!=81&&op!=42&&op!=43) {
            check("SM3 stage opcode",p->vertex||(op!=12&&op!=13));
            int input=-1,constant=-1,first=op==41?0:1;
            for(int i=first;i<n;i++) {
                uint32_t a=p->words[pc+1+i];int type=((a>>28)&7)|((a>>8)&24),r=a&2047;
                int *port=type==1?&input:type==2?&constant:NULL;
                if(port){check("SM3 distinct source read ports",*port<0||*port==r);*port=r;}
            }
        }
        pc+=n+1;
    }
}
static vec4_t sm3_source(sm3_state_t *s,uint32_t t) {
    int type=((t>>28)&7)|((t>>8)&24),r=t&2047;
    vec4_t v={0},raw={0};
    if(type==0&&r<32)raw=s->r[r];else if(type==1&&r<16)raw=s->in[r];else if(type==2&&r<256)raw=s->constant[r];else check("SM3 source register",0);
    for(int i=0;i<4;i++){v.u[i]=raw.u[(t>>(16+2*i))&3];int m=(t>>24)&15;if(m==11||m==12)v.f[i]=fabsf(v.f[i]);if(m==1||m==12)v.f[i]=-v.f[i];}
    return v;
}
static void sm3_store(sm3_state_t *s,uint32_t t,vec4_t v) {
    int type=((t>>28)&7)|((t>>8)&24),r=t&2047;vec4_t *p=NULL;
    if(type==0&&r<32)p=&s->r[r];else if(type==6&&r<12)p=&s->out[r];else if(type==8&&r==0)p=s->color;
    if(!p){check("SM3 destination register",0);return;}
    for(int i=0;i<4;i++)if(t&(1u<<(16+i))){if(t&(1u<<20))v.f[i]=!(v.f[i]>0)?0:v.f[i]>1?1:v.f[i];p->u[i]=v.u[i];}
}
static int sm3_skip(const shader_gpu_t *p,int pc,bool stop_at_else) {
    int depth=0;
    while(pc<(int)p->nwords){uint32_t t=p->words[pc];int op=t&65535,n=(t>>24)&15;if(op==41)depth++;else if(op==43){if(!depth)return pc+1;depth--;}else if(op==42&&!depth&&stop_at_else)return pc+1;pc+=n+1;}
    check("SM3 structured branch",0);return p->nwords;
}
static void compare_gpu(shader_gpu_t *p,dxbc_shader_t *shader,dxbc_env_t *env,vec4_t *input) {
    validate_sm3(p);
    sm3_state_t *s=calloc(1,sizeof *s);vec4_t expected[DX_MAX_REG]={0};
    check("reference native shader",dxbc_run(shader,env,input,expected));
    for(int i=0;i<p->ninputs;i++)s->in[i]=input[p->input_reg[i]];
    shader_gpu_constants(p,env,(float (*)[4])s->constant,480,360);
    for(int pc=1;pc<(int)p->nwords;){
        uint32_t t=p->words[pc];int op=t&65535,n=(t>>24)&15,next=pc+n+1;const uint32_t *o=p->words+pc+1;
        if(op==65535)break;
        if(op==31){pc=next;continue;}
        if(op==81){int r=o[0]&2047;check("SM3 constant index",r<256);if(r<256)memcpy(s->constant[r].u,o+1,16);pc=next;continue;}
        if(op==41){vec4_t a=sm3_source(s,o[0]),b=sm3_source(s,o[1]);int c=(t>>16)&255;bool yes=c==1?a.f[0]>b.f[0]:c==2?a.f[0]==b.f[0]:c==3?a.f[0]>=b.f[0]:c==4?a.f[0]<b.f[0]:c==5?a.f[0]!=b.f[0]:a.f[0]<=b.f[0];pc=yes?next:sm3_skip(p,next,true);continue;}
        if(op==42){pc=sm3_skip(p,next,false);continue;}if(op==43){pc=next;continue;}
        vec4_t a={0},b={0},c={0},v={0};
        if(n>1)a=sm3_source(s,o[1]);
        if(n>2&&op!=66&&op!=95)b=sm3_source(s,o[2]);if(n>3)c=sm3_source(s,o[3]);
        for(int i=0;i<4;i++)switch(op){
        case 1:v.u[i]=a.u[i];break;case 2:v.f[i]=a.f[i]+b.f[i];break;case 3:v.f[i]=a.f[i]-b.f[i];break;
        case 4:v.f[i]=a.f[i]*b.f[i]+c.f[i];break;case 5:v.f[i]=a.f[i]*b.f[i];break;case 6:v.f[i]=1/a.f[0];break;
        case 7:v.f[i]=1/sqrtf(fabsf(a.f[0]));break;case 8:v.f[i]=a.f[0]*b.f[0]+a.f[1]*b.f[1]+a.f[2]*b.f[2];break;
        case 9:v.f[i]=a.f[0]*b.f[0]+a.f[1]*b.f[1]+a.f[2]*b.f[2]+a.f[3]*b.f[3];break;
        case 10:v.f[i]=fminf(a.f[i],b.f[i]);break;case 11:v.f[i]=fmaxf(a.f[i],b.f[i]);break;
        case 12:v.f[i]=a.f[i]<b.f[i];break;case 13:v.f[i]=a.f[i]>=b.f[i];break;
        case 14:v.f[i]=exp2f(a.f[0]);break;case 15:v.f[i]=log2f(fabsf(a.f[0]));break;case 19:v.f[i]=a.f[i]-floorf(a.f[i]);break;
        case 66:case 95:break;default:check("SM3 reference opcode",0);pc=p->nwords;break;
        }
        if(op==66||op==95){int slot=o[2]&2047;check("SM3 combined sampler",slot<p->ntextures);if(slot<p->ntextures)env->sample(env->ctx,p->texture[slot].texture,p->texture[slot].sampler,a.f,op==95?a.f[3]:0,v.f);}
        sm3_store(s,o[0],v);pc=next;
    }
    if(p->vertex){int map[DX_MAX_REG],next=1;for(int i=0;i<DX_MAX_REG;i++)map[i]=-1;
        for(int i=0;i<shader->nout;i++)if(shader->out[i].sysval==1)map[shader->out[i].reg]=0;
        for(int i=0;i<shader->nout;i++)if(map[shader->out[i].reg]<0)map[shader->out[i].reg]=next++;
        s->out[0].f[0]+=s->out[0].f[3]/480;s->out[0].f[1]-=s->out[0].f[3]/360;
        for(int i=0;i<shader->nout;i++){const dxsig_t *v=&shader->out[i];for(int c=0;c<4;c++)if(v->mask&(1u<<c))check("GPU VS lowering vs native IR",fabsf(expected[v->reg].f[c]-s->out[map[v->reg]].f[c])<.0001f);}
    }else{int r=shader->out[0].reg;for(int c=0;c<4;c++)check("GPU PS lowering vs native IR",fabsf(expected[r].f[c]-s->color[0].f[c])<.0001f);}
    free(s);
}
static void alias_tests(void) {
    dxbc_shader_t *vs=fixture("vertex","vs",5);
    for(int test=0;test<2;test++) {
        dxbc_shader_t *s=unit_shader(OP_MOV,1,1,1);s->type=0;s->nin=s->nout=1;s->interpolation[0]=2;
        strcpy(s->in[0].name,"COLOR");s->in[0].mask=15;s->in[0].component_type=3;
        strcpy(s->out[0].name,"SV_Target");s->out[0].mask=15;s->out[0].component_type=3;s->out[0].sysval=64;
        s->pool=krealloc(s->pool,9*sizeof *s->pool);s->npool=test?9:5;
        for(int i=0;i<9;i++)s->pool[i]=reg_operand(T_TEMP,0,15);
        s->pool[1]=reg_operand(T_INPUT,0,15);
        s->ins=krealloc(s->ins,5*sizeof *s->ins);memset(s->ins,0,5*sizeof *s->ins);s->nins=test?5:4;
        s->ins[0]=(ins_t){.op=OP_MOV,.nops=2,.o={0,1}};
        if(!test){for(int i=0;i<4;i++)s->pool[2].swz[i]=3-i;s->pool[3]=reg_operand(T_OUTPUT,0,15);
            s->ins[1]=(ins_t){.op=OP_RCP,.nops=2,.o={0,2}};s->ins[2]=(ins_t){.op=OP_MOV,.nops=2,.o={3,4}};
        }else{
            s->pool[2].mask=1;s->pool[4].type=T_IMM32;s->pool[4].dim=0;for(int i=0;i<4;i++)s->pool[4].imm[i]=0x3ECCCCCD;
            for(int i=0;i<4;i++)s->pool[5].swz[i]=0;
            s->pool[6].type=T_IMM32;s->pool[6].dim=0;const vec4_t yes={.f={0,2,3,4}};memcpy(s->pool[6].imm,yes.u,16);
            s->pool[7]=reg_operand(T_OUTPUT,0,15);
            s->ins[1]=(ins_t){.op=OP_LT,.nops=3,.o={2,3,4}};s->ins[2]=(ins_t){.op=OP_MOVC,.nops=4,.o={0,5,6,1}};s->ins[3]=(ins_t){.op=OP_MOV,.nops=2,.o={7,8}};
        }
        s->ins[s->nins-1].op=OP_RET;finish_unit(s);
        char error[160];shader_gpu_t *p=shader_gpu_compile(s,vs,error,sizeof error);if(!p)printf("alias GPU fallback: %s\n",error);
        check("aliased scalar/conditional GPU shader compiles",p!=NULL);
        vec4_t in[DX_MAX_REG]={0};in[0]=(vec4_t){.f={.3f,.5f,.7f,1}};dxbc_env_t env={0};if(p)compare_gpu(p,s,&env,in);
        char name[32];snprintf(name,sizeof name,"alias-%d",test);export_gpu(p,name);
        shader_gpu_free(p);dxbc_free(s);
    }
    dxbc_free(vs);
}
static void port_tests(void) {
    dxbc_shader_t *link=fixture("vertex","vs",5);
    link->nout=4;memset(link->out,0,sizeof link->out);
    strcpy(link->out[0].name,"SV_Position");link->out[0].sysval=1;link->out[0].mask=15;link->out[0].component_type=3;
    for(int i=1;i<4;i++){strcpy(link->out[i].name,"ATTR");link->out[i].index=i-1;link->out[i].reg=i;link->out[i].mask=15;link->out[i].component_type=3;}
    for(int vertex=0;vertex<2;vertex++)for(int mode=0;mode<4;mode++) {
        dxbc_shader_t *s=unit_shader(OP_MAD,1,3,0);s->type=vertex;
        s->nout=1;strcpy(s->out[0].name,vertex?"SV_Position":"SV_Target");s->out[0].mask=15;s->out[0].component_type=3;s->out[0].sysval=vertex?1:64;
        if(mode==1||mode==2) {
            s->nin=mode==1?3:1;
            for(int i=0;i<s->nin;i++){strcpy(s->in[i].name,"ATTR");s->in[i].index=i;s->in[i].reg=i;s->in[i].mask=15;s->in[i].component_type=3;s->interpolation[i]=2;}
        }
        for(int i=0;i<3;i++) {
            bool input=mode==1||(mode==2&&i==0);
            if(input)s->pool[i+1]=reg_operand(T_INPUT,i,15);
            else {s->pool[i+1]=reg_operand(T_CB,0,15);s->pool[i+1].dim=2;s->pool[i+1].idx[1]=mode==3&&i==2?1:i;}
        }
        for(int c=0;c<4;c++)s->pool[3].swz[c]=3-c;
        s->pool[3].mod=1;
        finish_unit(s);char error[160];shader_gpu_t *p=shader_gpu_compile(s,link,error,sizeof error);
        check("multiple constant/input sources lower to legal SM3 ports",p!=NULL);
        vec4_t input[DX_MAX_REG]={0},cb[3]={0};
        for(int i=0;i<3;i++)for(int c=0;c<4;c++){input[i].f[c]=.1f*(1+i+c);cb[i].f[c]=.2f*(1+i+c);}
        dxbc_env_t env={0};env.cb[0]=cb;env.cb_rows[0]=3;
        if(p)compare_gpu(p,s,&env,input);
        char name[32];snprintf(name,sizeof name,"ports-%s-%d",vertex?"vs":"ps",mode);export_gpu(p,name);
        shader_gpu_free(p);dxbc_free(s);
    }
    dxbc_free(link);
}
static void comparison_tests(void) {
    dxbc_shader_t *link=fixture("vertex","vs",5);
    for(int ge=0;ge<2;ge++)for(int mode=0;mode<3;mode++) {
        dxbc_shader_t *s=unit_shader(OP_MOV,1,1,1);s->type=0;s->nin=s->nout=1;s->interpolation[0]=2;
        strcpy(s->in[0].name,"COLOR");s->in[0].mask=15;s->in[0].component_type=3;
        strcpy(s->out[0].name,"SV_Target");s->out[0].mask=15;s->out[0].component_type=3;s->out[0].sysval=64;
        s->pool=krealloc(s->pool,9*sizeof *s->pool);s->npool=9;
        for(int i=0;i<9;i++)s->pool[i]=reg_operand(T_TEMP,0,15);
        s->pool[1]=reg_operand(T_INPUT,0,15);
        for(int c=0;c<4;c++)s->pool[3].swz[c]=3-c;
        if(mode==2){s->pool[3]=reg_operand(T_CB,0,15);s->pool[4]=reg_operand(T_CB,0,15);s->pool[3].dim=s->pool[4].dim=2;s->pool[4].idx[1]=1;}
        else if(mode==1)s->pool[4]=reg_operand(T_INPUT,0,15);
        s->pool[5]=reg_operand(T_OUTPUT,0,15);
        for(int i=7;i<9;i++){s->pool[i].type=T_IMM32;s->pool[i].dim=0;vec4_t v={.f={.2f,.4f,.6f,1}};if(i==8)for(int c=0;c<3;c++)v.f[c]*=.5f;memcpy(s->pool[i].imm,v.u,16);}
        s->ins=krealloc(s->ins,4*sizeof *s->ins);s->nins=4;
        s->ins[0]=(ins_t){.op=OP_MOV,.nops=2,.o={0,1}};
        s->ins[1]=(ins_t){.op=ge?OP_GE:OP_LT,.nops=3,.o={2,3,4}};
        s->ins[2]=(ins_t){.op=OP_MOVC,.nops=4,.o={5,6,7,8}};
        s->ins[3]=(ins_t){.op=OP_RET};finish_unit(s);
        char error[160];shader_gpu_t *p=shader_gpu_compile(s,link,error,sizeof error);check("pixel ordered comparison shader compiles",p!=NULL);
        vec4_t input[DX_MAX_REG]={0},cb[2];dxbc_env_t env={0};env.cb[0]=cb;env.cb_rows[0]=2;
        const vec4_t cases[]={{{.3f,.5f,.7f,1}},{{0,0,-0.0f,1}}, {.u={0x7FC00000,0x7F800000,0xFF800000,0x3F800000}}};
        for(unsigned i=0;i<ARRAY_LEN(cases);i++){input[0]=cb[0]=cases[i];cb[1]=(vec4_t){.f={.4f,.5f,.7f,1}};if(p)compare_gpu(p,s,&env,input);}
        char name[32];snprintf(name,sizeof name,"comparison-%d-%d",ge,mode);export_gpu(p,name);
        shader_gpu_free(p);
        s->ins[2].sat=1;s->pool[7]=reg_operand(T_TEMP,0,15);
        p=shader_gpu_compile(s,link,error,sizeof error);
        check("saturated raw comparison data uses CPU fallback",p==NULL);shader_gpu_free(p);dxbc_free(s);
    }
    dxbc_free(link);
}
static void limit_tests(void) {
    dxbc_shader_t *s=unit_shader(OP_MAD,1,3,0);s->nout=1;
    strcpy(s->out[0].name,"SV_Position");s->out[0].mask=15;s->out[0].component_type=3;s->out[0].sysval=1;
    for(int i=0;i<3;i++){s->pool[i+1]=reg_operand(T_CB,0,15);s->pool[i+1].dim=2;s->pool[i+1].idx[1]=i;}
    s->ins=krealloc(s->ins,201*sizeof *s->ins);s->nins=201;
    for(int i=1;i<200;i++)s->ins[i]=s->ins[0];s->ins[200]=(ins_t){.op=OP_RET};finish_unit(s);
    char error[160];shader_gpu_t *p=shader_gpu_compile(s,NULL,error,sizeof error);
    check("expanded SM3 slot limit uses CPU fallback",!p&&strstr(error,"instruction slot limit"));shader_gpu_free(p);dxbc_free(s);
}

int main(int argc,char **argv) {
    if(argc>1)export_directory=argv[1];
    static const struct { const char *name,*stage; int expected; } cases[]={
        {"cube-vertex","vs",1},{"cube-pixel","ps",1},{"vertex","vs",1},{"color","ps",1},
        {"texture_level","ps",1},{"reciprocal","ps",1},{"sparse","ps",1},{"smooth_color","ps",1},
        {"bits","ps",0},{"half_round","ps",0},{"depth","ps",0},{"flat_color","ps",0},
        {"linear_color","ps",0},{"clip_color","ps",0},{"uint_vertex","vs",0}
    };
    for(int model=4;model<=6;model++) {
        dxbc_shader_t *vs=fixture("vertex","vs",model),*cube=fixture("cube-vertex","vs",model==4?5:model);
        for(unsigned i=0;i<ARRAY_LEN(cases);i++) {
            if(model==4&&(!strcmp(cases[i].name,"bits")||!strcmp(cases[i].name,"half_round"))) continue;
            if(model==4&&i<2) continue; /* embedded assembly tested below */
            dxbc_shader_t *s=fixture(cases[i].name,cases[i].stage,model);
            if(!s) continue;
            char error[160];
            shader_gpu_t *p=shader_gpu_compile(s,!strncmp(cases[i].name,"cube-",5)?cube:vs,error,sizeof error);
            int expected=cases[i].expected;
            if(model==6&&(!strcmp(cases[i].name,"cube-vertex")||!strcmp(cases[i].name,"cube-pixel")||!strcmp(cases[i].name,"color"))) expected=0;
            if((p!=NULL)!=expected) printf("GPU subset %s/%d: %s\n",cases[i].name,model,p?"unexpected hardware selection":error);
            check("GPU subset/fallback selection",(p!=NULL)==expected);
            if(p) {
                check("legacy shader version",p->words[0]==(p->vertex?0xFFFE0300u:0xFFFF0300u));
                check("bounded terminated shader",p->nwords<=8192&&p->words[p->nwords-1]==0xFFFF);
                if(!strcmp(cases[i].name,"sparse")) check("combined sparse texture/sampler",p->ntextures==1&&p->texture[0].texture==7&&p->texture[0].sampler==5);
                float constants[256][4]; vec4_t cb[5]={0}; dxbc_env_t env={0}; env.cb[0]=cb;env.cb_rows[0]=5;
                shader_gpu_constants(p,&env,constants,480,360);
                if(p->vertex) check("half-pixel position correction",constants[255][0]==-1.0f/480&&constants[255][1]==1.0f/360);
                vec4_t input[DX_MAX_REG]={0},scene[10]={0};env.cb[0]=scene;env.cb_rows[0]=10;env.cb[3]=scene;env.cb_rows[3]=10;env.sample=sample;
                bool is_cube=!strncmp(cases[i].name,"cube-",5);
                for(int c=0;c<4;c++){scene[0].f[c]=1;if(is_cube){scene[c].f[c]=1;scene[c+4].f[c]=1;}else scene[c+1].f[c]=1;}
                scene[8]=(vec4_t){.f={.4f,.7f,-.6f,0}};
                const dxsig_t *sig;int ni=dxbc_inputs(s,&sig);
                for(int k=0;k<ni;k++)if(!sig[k].sysval){vec4_t v={.f={.3f,.5f,.7f,1}};if(!strcmp(sig[k].name,"TEXCOORD"))v=(vec4_t){.f={.1f,.2f,.3f,.4f}};semantic(s,input,sig[k].name,sig[k].index,&v);}
                compare_gpu(p,s,&env,input);
                if(!p->vertex){scene[0].f[0]=.6f;scene[9].f[0]=.8f;compare_gpu(p,s,&env,input);}
                char name[64];snprintf(name,sizeof name,"%s-%d",cases[i].name,model);export_gpu(p,name);
            } else check("fallback reason",error[0]!=0);
            shader_gpu_free(p);dxbc_free(s);
        }
        dxbc_free(vs);dxbc_free(cube);
    }
    char error[160]; const void *code[]={g_vs,g_ps};const size_t size[]={sizeof g_vs,sizeof g_ps};
    dxbc_shader_t *vs=dxbc_load(g_vs,sizeof g_vs,error,sizeof error);
    for(int i=0;i<2;i++) {
        dxbc_shader_t *s=dxbc_load(code[i],size[i],error,sizeof error);
        shader_gpu_t *p=shader_gpu_compile(s,vs,error,sizeof error);
        if(!p) printf("embedded cube GPU fallback: %s\n",error);
        check("original assembled SM4 cube accelerates",p!=NULL);
        if(p)validate_sm3(p);
        export_gpu(p,i?"embedded-ps":"embedded-vs");
        shader_gpu_free(p);dxbc_free(s);
    }
    dxbc_free(vs);
    alias_tests();
    port_tests();
    comparison_tests();
    limit_tests();
    check("GPU compiler allocation cleanup",live_allocations==0);
    printf("GPU shader compiler tests: %s (%d checks)\n",failures?"FAILED":"passed",checks);
    return failures?1:0;
}
