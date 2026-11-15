/* Actual Basic Display converter/damage code, plus the real boot-tag parser.
 * Device memory is guarded ordinary RAM; no firmware or privileged I/O runs. */
#define __USE_MINGW_ANSI_STDIO 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#undef NULL
#undef offsetof
#undef va_start
#undef va_end
#undef va_arg
#include "basicfb.h"
#include "fbmap.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include "../src/kernel/bootinfo.c"
#pragma GCC diagnostic pop

static int checks, fails, damages;
static void check(const char *name, int good) { checks++; if (!good) { printf("FAIL %s\n", name); fails++; } }
static void damage(void *ctx, int x, int y, int w, int h) {
    basicfb_mode_t *m = ctx; damages++;
    check("bounded damage notification", x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= (int)m->width && y + h <= (int)m->height);
}
static uint32_t random_value = 1234567;
static uint32_t next(void) { random_value = random_value * 1664525u + 1013904223u; return random_value; }
static uint32_t packed(uint32_t p, basicfb_mode_t *m) {
    uint32_t r = (((p >> 16) & 255) * ((1u << m->red_bits) - 1) + 127) / 255;
    uint32_t g = (((p >> 8) & 255) * ((1u << m->green_bits) - 1) + 127) / 255;
    uint32_t b = ((p & 255) * ((1u << m->blue_bits) - 1) + 127) / 255;
    return (r << m->red_pos) | (g << m->green_pos) | (b << m->blue_pos);
}
static uint32_t rgb_mask(basicfb_mode_t *m) { return packed(0xFFFFFF, m); }
static void interrupt_present(void *ctx,int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h;basicfb_invalidate(ctx); }
static void verify(uint8_t *raw, size_t size, int offset, basicfb_mode_t *m, const uint32_t *src, uint32_t stride) {
    unsigned bytes = (m->bpp + 7) / 8;
    for (size_t i = 0; i < size; i++) {
        int64_t at = (int64_t)i - offset;
        if (at < 0 || at >= (int64_t)m->pitch * m->height || (uint32_t)at % m->pitch >= m->width * bytes)
            check("framebuffer padding/guards untouched", raw[i] == 0xCD);
    }
    for (unsigned y = 0; y < m->height; y++) for (unsigned x = 0; x < m->width; x++) {
        uint32_t p = 0; memcpy(&p, raw + offset + (size_t)y * m->pitch + x * bytes, bytes);
        check("firmware pixel format conversion", (p & rgb_mask(m)) == packed(src[(size_t)y * stride + x], m));
    }
}
static void formats(void) {
    basicfb_mode_t modes[] = {
        {0,0,0,32,16,8,8,8,0,8}, {0,0,0,32,0,8,8,8,16,8},
        {0,0,0,24,16,8,8,8,0,8}, {0,0,0,24,0,8,8,8,16,8},
        {0,0,0,16,11,5,5,6,0,5}, {0,0,0,16,0,5,5,6,11,5},
        {0,0,0,15,10,5,5,5,0,5}, {0,0,0,32,20,10,10,10,0,10}
    };
    const unsigned widths[] = {1,3,4,31,32,33,67,129};
    for (unsigned k = 0; k < ARRAY_LEN(modes); k++) for (unsigned wi = 0; wi < ARRAY_LEN(widths); wi++) for (int off = 1; off <= 64; off += 21) {
        basicfb_mode_t m = modes[k]; m.width = widths[wi]; m.height = 35; m.pitch = m.width * ((m.bpp + 7) / 8) + 13;
        size_t size = (size_t)m.pitch * m.height + 128;
        uint8_t *raw = malloc(size); uint32_t stride = m.width + 7;
        uint32_t *src = malloc((size_t)stride * m.height * 4), *history = malloc((size_t)m.width * m.height * 4);
        memset(raw, 0xCD, size); for (unsigned i = 0; i < stride * m.height; i++) src[i] = next();
        basicfb_t f; damages = 0;
        check("valid direct RGB mode", basicfb_bind(&f, raw + off, &m, history, (size_t)m.width * m.height * 4, true, damage, &m));
        basicfb_present(&f, src, stride); verify(raw, size, off, &m, src, stride);
        check("first frame fully uploaded", f.bytes_last == (uint64_t)m.width * m.height * ((m.bpp + 7) / 8) && damages == 1);
        basicfb_present(&f, src, stride); check("unchanged frame has zero writes/notifications", f.bytes_last == 0 && damages == 1);
        src[(m.height - 1) * stride + m.width - 1] ^= 0xFF336699u;
        basicfb_present(&f, src, stride); verify(raw, size, off, &m, src, stride);
        check("single changed edge tile bounded", f.tiles_last == 1 && f.regions_last == 1);
        basicfb_invalidate(&f); basicfb_present(&f, src, stride);
        check("explicit invalidation repairs external writes", f.bytes_last == (uint64_t)m.width * m.height * ((m.bpp + 7) / 8));
        free(raw); free(src); free(history);
    }
}
static void regions(void) {
    basicfb_mode_t m = { 96,48,384,32,16,8,8,8,0,8 };
    uint32_t dst[96*48], history[96*48], src[96*48];
    for (unsigned i = 0; i < ARRAY_LEN(src); i++) src[i] = next();
    basicfb_t f; check("bind damage test", basicfb_bind(&f,dst,&m,history,sizeof history,false,damage,&m));
    basicfb_present(&f,src,96); src[17*96+1]++; src[17*96+33]++;
    basicfb_present(&f,src,96); check("adjacent tiles merge into one band",f.tiles_last==2&&f.regions_last==1&&f.bytes_last==64*16*4);
    uint32_t patch[8*8]; for (unsigned i=0;i<ARRAY_LEN(patch);i++)patch[i]=0xFF123456;
    basicfb_write(&f,-3,-2,8,8,patch,8);
    for(int y=0;y<6;y++)for(int x=0;x<5;x++)src[y*96+x]=patch[(y+2)*8+x+3];
    check("negative rectangle clipped with source offset",!memcmp(dst,src,sizeof dst));
    basicfb_present(&f,src,96);check("partial upload keeps history coherent",f.bytes_last==0);
    uint64_t before=f.bytes_written;
    basicfb_write(&f,0,0,0,8,patch,8);basicfb_write(&f,0,0,8,8,patch,7);
    basicfb_write(&f,-2147483647-1,0,8,8,patch,8);basicfb_write(&f,0,2147483647,8,8,patch,8);
    check("empty/invalid/extreme rectangles write nothing",f.bytes_written==before);
    check("no-history bind",basicfb_bind(&f,dst,&m,NULL,0,false,NULL,NULL));
    basicfb_present(&f,src,96);basicfb_present(&f,src,96);check("low-memory fallback stays functional",f.bytes_last==sizeof dst&&!memcmp(dst,src,sizeof dst));
    basicfb_bind(&f,dst,&m,history,sizeof history,false,interrupt_present,&f);basicfb_present(&f,src,96);
    check("invalidation during present cannot be overwritten",!f.have_frame&&f.invalidations==1);
}
static void invalid_modes(void) {
    basicfb_mode_t m={10,10,40,32,16,8,8,8,0,8};const char *why=NULL;
    check("null mode rejected",!basicfb_mode_valid(NULL,&why)&&why);
    basicfb_mode_t b=m;b.width=0;check("zero width rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.height=8193;check("huge height rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.pitch=39;check("short pitch rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.pitch=0xFFFFFFFF;check("overflowing framebuffer rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.bpp=8;check("indexed mode rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.red_bits=0;check("missing masks rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.red_pos=30;check("out-of-pixel mask rejected",!basicfb_mode_valid(&b,NULL));
    b=m;b.blue_pos=8;check("overlapping masks rejected",!basicfb_mode_valid(&b,NULL));
    check("WC uses PA1, not PA4",(fbmap_cache_bits(0x200083,true)&FBMAP_PAT_MASK)==8);
    check("no PAT falls back to UC",(fbmap_cache_bits(0x20109B,false)&FBMAP_PAT_MASK)==0x18);
    check("cache update preserves physical address/permissions",(fbmap_cache_bits(0x123400083,true)&~FBMAP_PAT_MASK)==(0x123400083ull&~FBMAP_PAT_MASK));
    mmio_plan_t plan;
    check("high MMIO bounded plan",mmio_plan(0x3800000000ull,0x100000,0x1000000,40,&plan)&&plan.base==0x3800000000ull&&plan.offset==0&&plan.pages==1);
    check("MMIO page boundary",mmio_plan(0x1FFFFF,2,0x1000000,40,&plan)&&plan.pages==2&&plan.offset==0x1FFFFF);
    check("MMIO offset preserved",mmio_plan(0x100200123ull,4096,0x1000000,40,&plan)&&plan.base==0x100200000ull&&plan.offset==0x123);
    check("MMIO physical width enforced",!mmio_plan(0x100000000ull,4096,0x1000000,32,&plan));
    check("MMIO overflow rejected",!mmio_plan(~0ull-100,200,0x1000000,52,&plan));
    check("MMIO request budget enforced",!mmio_plan(0x100000000ull,0x1000001,0x1000000,40,&plan));
    check("MMIO invalid CPU width rejected",!mmio_plan(0x100000000ull,4096,0x1000000,64,&plan));
}
static void boot_tags(void) {
    uint64_t data[32];uint8_t *p=(uint8_t *)data;
    memset(data,0,sizeof data);uint32_t total=64;memcpy(p,&total,4);
    mb2_fb_t *fb=(mb2_fb_t *)(p+8);fb->type=8;fb->size=38;fb->addr=0x123400000ull;fb->pitch=4096;fb->width=1024;fb->height=768;fb->bpp=32;fb->fb_type=1;
    const uint8_t rgb[]={0,8,8,8,16,8};memcpy(p+8+32,rgb,6);
    mb2_tag_t *efi=(mb2_tag_t *)(p+48);efi->type=12;efi->size=16;uint64_t table=0x123456789ull;memcpy(p+56,&table,8);
    memset(&bootinfo,0,sizeof bootinfo);parse_mb2((uintptr_t)p);
    check("MB2 high framebuffer address preserved",bootinfo.have_fb&&bootinfo.fb_addr==fb->addr);
    check("MB2 RGB masks follow reserved bytes",bootinfo.fb_rgb_valid&&bootinfo.fb_red_pos==0&&bootinfo.fb_blue_pos==16);
    check("EFI64 system table decoded",bootinfo.is_efi&&bootinfo.efi_system_table==table);
    fb->size=32;memset(&bootinfo,0,sizeof bootinfo);parse_mb2((uintptr_t)p);
    check("truncated RGB tag cannot invent a pixel format",!bootinfo.fb_rgb_valid);
    memset(data,0,sizeof data);memcpy(p,&total,4);efi=(mb2_tag_t *)(p+8);efi->type=11;efi->size=12;uint32_t t32=0x12345678;memcpy(p+16,&t32,4);
    memset(&bootinfo,0,sizeof bootinfo);parse_mb2((uintptr_t)p);check("EFI32 system table decoded",bootinfo.is_efi&&bootinfo.efi_system_table==t32);
    uint8_t legacy[sizeof(mb1_info_t)+6];memset(legacy,0,sizeof legacy);mb1_info_t *mb=(mb1_info_t *)legacy;
    mb->flags=1u<<12;mb->fb_type=1;mb->fb_addr=0xE0000000;mb->fb_bpp=16;memcpy(legacy+sizeof *mb,(uint8_t[]){11,5,5,6,0,5},6);
    memset(&bootinfo,0,sizeof bootinfo);parse_mb1((uintptr_t)legacy);
    check("MB1 RGB565 masks decoded",bootinfo.fb_rgb_valid&&bootinfo.fb_red_pos==11&&bootinfo.fb_green_bits==6);
}
static void benchmark(void) {
    basicfb_mode_t m={1920,1080,1920*4,32,16,8,8,8,0,8};size_t bytes=(size_t)m.pitch*m.height;
    uint32_t *src=malloc(bytes),*dst=malloc(bytes),*history=malloc(bytes);
    memset(src,0x44,bytes);basicfb_t f;basicfb_bind(&f,dst,&m,history,bytes,true,NULL,NULL);basicfb_present(&f,src,m.width);
    clock_t start=clock();uint64_t transferred=0;
    for(int i=0;i<240;i++){src[(i%60)*m.width+i%30]^=0x00123456;basicfb_present(&f,src,m.width);transferred+=f.bytes_last;}
    double seconds=(double)(clock()-start)/CLOCKS_PER_SEC;
    printf("Basic Display RAM-backed microbenchmark: 240 sparse 1080p presents %.4f s, %llu/%llu framebuffer bytes (not OS FPS or PCIe/WC bandwidth)\n",seconds,(unsigned long long)transferred,(unsigned long long)bytes*240);
    free(src);free(dst);free(history);
}
int main(int argc,char **argv) {
    formats();regions();invalid_modes();boot_tags();
    if(argc>1&&!strcmp(argv[1],"--bench"))benchmark();
    printf("Basic Display tests: %s (%d checks)\n",fails?"FAILED":"passed",checks);return fails?1:0;
}
