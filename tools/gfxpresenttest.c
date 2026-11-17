/* Actual gfx.c cursor/presentation integration, with a guarded RAM framebuffer.
 * Link with function sections/GC: font/file/UI paths are not exercised here. */
#define __USE_MINGW_ANSI_STDIO 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#undef offsetof
#undef va_start
#undef va_end
#undef va_arg
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include "../src/gui/gfx.c"
#pragma GCC diagnostic pop

static int checks, fails, fail_alloc, mode_calls;
static void check(const char *s,int yes){checks++;if(!yes){printf("FAIL %s\n",s);fails++;}}
void *kmalloc(size_t n){return fail_alloc?NULL:malloc(n);}
void *kzalloc(size_t n){void *p=kmalloc(n);if(p)memset(p,0,n);return p;}
void kfree(void *p){free(p);}
uint32_t mm_total(void){return 512u<<20;}
uint32_t mm_used(void){return 0;}
void klog(const char *fmt,...){(void)fmt;}
void panic(const char *fmt,...){printf("unexpected display panic: %s\n",fmt);exit(2);}
void *memset32(uint32_t *p,uint32_t v,size_t n){void *r=p;while(n--)*p++=v;return r;}
void video_update(int x,int y,int w,int h){check("clipped hardware update",x>=0&&y>=0&&w>0&&h>0&&x+w<=SCREEN_W&&y+h<=SCREEN_H);}
bool video_mode_ok(int w,int h){return w>0&&h>0&&w<=1024&&h<=768;}
bool video_set_mode(int w,int h){(void)w;(void)h;mode_calls++;return false;}
theme_t T;
uint32_t vfs_generation;
uint32_t timer_ticks(void){return 0;}
vnode_t *vfs_root(int d){(void)d;return NULL;}
vnode_t *vfs_resolve(const char *p,const char *cwd){(void)p;(void)cwd;return NULL;}
uint8_t *vfs_read_file(const char *p,uint32_t *n){(void)p;if(n)*n=0;return NULL;}
void vfs_sort_children(vnode_t *d){(void)d;}
size_t strlcpy(char *d,const char *s,size_t n){size_t len=strlen(s);if(n){size_t k=len<n-1?len:n-1;memcpy(d,s,k);d[k]=0;}return len;}

static uint32_t encode(uint32_t p){
    return ((((p>>16)&255)*((1u<<fb_info.red_bits)-1)+127)/255<<fb_info.red_pos)|
           ((((p>>8)&255)*((1u<<fb_info.green_bits)-1)+127)/255<<fb_info.green_pos)|
           (((p&255)*((1u<<fb_info.blue_bits)-1)+127)/255<<fb_info.blue_pos);
}
static uint32_t reference_pixel(const uint32_t *background,int kind,int mx,int my,int x,int y){
    uint32_t bg=background[y*SCREEN_W+x];int w,h,hx,hy;cursor_metrics(kind,&w,&h,&hx,&hy);
    int cx=x-mx+hx,cy=y-my+hy;if(cx<0||cy<0||cx>=w||cy>=h)return bg;
    if(ucur[kind].px){uint32_t p=ucur[kind].px[cy*w+cx];int a=p>>24;if(!a)return bg;if(a==255)return p;
        /* Preserve gfx.c's existing unsigned /255 cursor interpolation exactly. */
        uint32_t r=C_R(bg)+(C_R(p)-C_R(bg))*(uint32_t)a/255u,g=C_G(bg)+(C_G(p)-C_G(bg))*(uint32_t)a/255u,b=C_B(bg)+(C_B(p)-C_B(bg))*(uint32_t)a/255u;
        return RGB(r,g,b);
    }
    char ch=cursors[kind].art[cy*w+cx];if(ch=='.')return bg;
    if(kind==CURSOR_IBEAM||kind==CURSOR_CROSS)return bg^0xFFFFFF;
    return ch=='B'?0:0xFFFFFF;
}
static void verify(uint8_t *raw,size_t n,const uint32_t *background,int kind,int x,int y){
    unsigned bytes=(fb_info.bpp+7)/8;uint32_t mask=encode(0xFFFFFF);
    for(size_t i=0;i<n;i++)if(i<13||i>=13+(size_t)fb_info.pitch*fb_info.height||((i-13)%fb_info.pitch)>=fb_info.width*bytes)check("present guards/padding",raw[i]==0xCD);
    for(int j=0;j<SCREEN_H;j++)for(int k=0;k<SCREEN_W;k++){
        uint32_t actual=0;memcpy(&actual,raw+13+(size_t)j*fb_info.pitch+k*bytes,bytes);
        uint32_t want=encode(reference_pixel(background,kind,x,y,k,j));
        if((actual&mask)!=want&&fails<24)printf("cursor detail bpp=%u Rpos=%u kind=%d xy=%d,%d pixel=%d,%d actual=%08x expected=%08x\n",fb_info.bpp,fb_info.red_pos,kind,x,y,k,j,actual&mask,want);
        check("displayed cursor matches reference",(actual&mask)==want);
    }
    check("cursor never modifies canonical desktop",!memcmp(screen.px,background,(size_t)SCREEN_W*SCREEN_H*4));
}
int main(void){
    fb_info=(fb_info_t){.width=96,.height=64,.pitch=384,.bpp=32,.red_pos=16,.red_bits=8,.green_pos=8,.green_bits=8,.blue_pos=0,.blue_bits=8};
    size_t cap=96*4*64+512;uint8_t *raw=malloc(cap);fb_info.addr=(uintptr_t)(raw+13);fb_info.write_combining=true;memset(raw,0xCD,cap);gfx_init();
    uint32_t background[96*64];for(unsigned i=0;i<ARRAY_LEN(background);i++)background[i]=0xFF000000u|((i*1664525u+1234567)&0xFFFFFF);
    memcpy(screen.px,background,sizeof background);
    const int positions[][2]={{10,10},{11,12},{0,0},{95,63},{-10,-10},{110,80}};
    for(int format=0;format<4;format++){
        fb_info.bpp=format==1?24:format==2?16:32;
        fb_info.pitch=96*((fb_info.bpp+7)/8)+7;
        fb_info.red_pos=format==2?11:format==3?0:16;fb_info.red_bits=format==2?5:8;
        fb_info.green_pos=format==2?5:8;fb_info.green_bits=format==2?6:8;
        fb_info.blue_pos=format==3?16:0;fb_info.blue_bits=format==2?5:8;
        memset(raw,0xCD,cap);gfx_refresh_framebuffer();
        for(int kind=0;kind<CURSOR_COUNT;kind++)for(unsigned p=0;p<ARRAY_LEN(positions);p++){
            int x=positions[p][0],y=positions[p][1];gfx_move_cursor(kind,x,y);verify(raw,cap,background,kind,x,y);
            gfx_present(kind,x,y);verify(raw,cap,background,kind,x,y);check("cursor-only history stays coherent",display.bytes_last==0);
        }
        uint32_t custom[]={0,0x80FF0000,0xFF00FF00,0xFF0000FF,0x40000000,0xFFFFFFFF,0,0xC0778899,0};
        ucur[CURSOR_HAND]=(ucur_t){3,3,1,1,custom};gfx_move_cursor(CURSOR_HAND,40,20);verify(raw,cap,background,CURSOR_HAND,40,20);
        gfx_present(CURSOR_HAND,40,20);verify(raw,cap,background,CURSOR_HAND,40,20);
        ucur[CURSOR_HAND].px=NULL; /* same-size old cursor history repaired by full present */
        gfx_present(CURSOR_ARROW,30,30);verify(raw,cap,background,CURSOR_ARROW,30,30);
        uint32_t alert[4]={0xFFFF0000,0xFF00FF00,0xFF0000FF,0xFFFFFFFF};canvas_t c;canvas_init(&c,alert,2,2);gfx_emergency_present(&c,40,10);
        check("emergency display invalidates history",!display.have_frame);gfx_present(CURSOR_ARROW,30,30);verify(raw,cap,background,CURSOR_ARROW,30,30);
    }
    uint32_t *old=screen.px;uintptr_t address=fb_info.addr;int calls=mode_calls;
    fail_alloc=1;check("resize OOM keeps working mode",!gfx_set_resolution(128,96)&&screen.px==old&&fb_info.addr==address&&mode_calls==calls);
    fail_alloc=0;check("driver refusal keeps back buffer",!gfx_set_resolution(128,96)&&screen.px==old&&mode_calls==calls+1);
    kfree(screen.px);kfree(display_shadow);free(raw);
    printf("Cursor/presenter tests: %s (%d checks)\n",fails?"FAILED":"passed",checks);return fails?1:0;
}
