/* Host checks against video.c's actual FIFO code. Port I/O, PCI and clock are
 * mocked; no VM, display or privileged instruction is needed. */
#include "kernel.h"
int printf(const char *, ...);
void *malloc(size_t);
void free(void *);
static void fake_outl(uint16_t port,uint32_t value);
static uint32_t fake_inl(uint16_t port);
static void fake_outw(uint16_t port,uint16_t value) { (void)port;(void)value; }
static uint16_t fake_inw(uint16_t port) { (void)port;return 0; }
#define outl fake_outl
#define inl fake_inl
#define outw fake_outw
#define inw fake_inw
#include "../src/kernel/video.c"
#undef outl
#undef inl
#undef outw
#undef inw

static uint32_t memory[4096],registers[96],reads[96],writes[96],selected,now;
static uint32_t seen[4096],nseen,kicks;
static bool consume,ack_fences;
static int checks,failures;
static pci_dev_t test_pci[3];
static uint32_t test_bars[3][6];
static int test_pci_count,config_writes,claims;
static void check(const char *name,bool good) {
    checks++;if(!good){printf("FAIL %s\n",name);failures++;}
}
static void fake_outl(uint16_t port,uint32_t value) {
    if(port==svga_io){selected=value;return;}
    if(port!=svga_io+1||selected>=ARRAY_LEN(registers))return;
    registers[selected]=value;writes[selected]++;
    if(selected==S_CONFIG_DONE&&value)memory[F_CAPS]=0x41;
    if(selected!=S_SYNC)return;
    kicks++;
    if(!consume)return;
    uint32_t first=nseen;
    while(fifo[F_STOP]!=fifo[F_NEXT]&&nseen<ARRAY_LEN(seen)) {
        uint32_t at=fifo[F_STOP];seen[nseen++]=fifo[at/4];
        fifo[F_STOP]=at+4==fifo[F_MAX]?fifo[F_MIN]:at+4;
    }
    if(ack_fences&&nseen-first==2&&seen[first]==30)fifo[F_FENCE]=seen[first+1];
}
static uint32_t fake_inl(uint16_t port) {
    if(port!=svga_io+1||selected>=ARRAY_LEN(registers))return 0;
    reads[selected]++;return registers[selected];
}
uint32_t timer_ticks(void) { return now++; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
void klog(const char *fmt,...) { (void)fmt; }
void paging_set_uncached(uintptr_t p) { (void)p; }
uintptr_t paging_map_framebuffer(uint64_t p,uint32_t n,bool *wc) { (void)n;if(wc)*wc=true;return (uintptr_t)p; }
void gfx_refresh_framebuffer(void) { }
void gpu3d_init(void) { }
int pci_list(pci_dev_t **p) { *p=test_pci;return test_pci_count; }
uint32_t pci_cfg_read(uint8_t b,uint8_t d,uint8_t f,uint8_t o) {
    for(int i=0;i<test_pci_count;i++)if(test_pci[i].bus==b&&test_pci[i].dev==d&&test_pci[i].fn==f&&o>=0x10&&o<0x28)return test_bars[i][(o-0x10)/4];
    return 0;
}
void pci_cfg_write(uint8_t b,uint8_t d,uint8_t f,uint8_t o,uint32_t v) { (void)b;(void)d;(void)f;(void)o;(void)v;config_writes++; }
void dev_claim(const pci_dev_t *p,const driver_t *d,const char *n) { (void)p;(void)d;(void)n;claims++; }
fb_info_t fb_info;
bootinfo_t bootinfo;
bool boot_safe_mode;

static void reset(void) {
    memset(memory,0,sizeof memory);memset(registers,0,sizeof registers);
    memset(reads,0,sizeof reads);memset(writes,0,sizeof writes);
    fifo=memory;fifo_size=sizeof memory;svga_io=0x1000;svga_on=true;svga_caps=0x8000;svga_fifo_caps=0x41;
    memory[F_MIN]=F_NUM_REGS*4;memory[F_MAX]=sizeof memory;
    memory[F_NEXT]=memory[F_STOP]=memory[F_MIN];memory[F_CAPS]=0x41;
    memory[16]=0xDEADBEEF;registers[24]=0x1234; /* obsolete host data/cursor ID, NOT errors */
    now=selected=nseen=kicks=0;consume=ack_fences=true;
    drv=DRV_SVGA;max_w=2560;max_h=1600;vram=16u<<20;
    test_pci_count=config_writes=claims=0;memset(test_bars,0,sizeof test_bars);memset(test_pci,0,sizeof test_pci);memset(&bootinfo,0,sizeof bootinfo);
}
static void display_selection(void) {
    reset();test_pci_count=2;
    test_pci[0]=(pci_dev_t){.bus=0,.dev=1,.cls=3,.vendor=0x8086,.device=0x1234};
    test_pci[1]=(pci_dev_t){.bus=1,.dev=0,.cls=3,.vendor=0x10DE,.device=0x2507};
    fb_info.physical=0xE0000000;test_bars[1][1]=0xE0000008;
    check("boot BAR, not first GPU, owns display",boot_adapter(test_pci,2)==&test_pci[1]);
    check("association never sizes BARs with writes",config_writes==0);
    test_bars[1][1]=0;test_bars[1][2]=0x0020000C;test_bars[1][3]=1;fb_info.physical=0x100200000ull;
    check("64-bit BAR ownership",boot_adapter(test_pci,2)==&test_pci[1]);
    bool io=true;check("64-bit BAR decoder preserves high address",pci_bar64(&test_pci[1],2,&io)==0x100200000ull&&!io);
    check("legacy BAR accessor still refuses high MMIO",pci_bar(&test_pci[1],2,NULL)==0);
    test_bars[0][1]=0xC001;check("I/O BAR decode",pci_bar64(&test_pci[0],1,&io)==0xC000&&io);
    check("invalid BAR index rejected",pci_bar64(&test_pci[0],6,NULL)==0&&pci_bar64(NULL,0,NULL)==0);
    test_bars[0][5]=0xC;check("truncated 64-bit BAR rejected",pci_bar64(&test_pci[0],5,NULL)==0);
    fb_info.physical=0xF0000000;check("ambiguous GPUs are not guessed",boot_adapter(test_pci,2)==NULL);
    test_bars[0][0]=test_bars[1][0]=0xF0000008;check("duplicate BAR match remains ambiguous",boot_adapter(test_pci,2)==NULL);
    check("sole adapter fallback",boot_adapter(test_pci,1)==&test_pci[0]);
    bootinfo.cmdline="verbose video=basic";check("explicit basic recovery recognized",basic_requested());
    bootinfo.cmdline="basicdisplay";check("basic recovery alias",basic_requested());
    bootinfo.cmdline="xvideo=basic video=basic2";check("recovery requires whole token",!basic_requested());
    bootinfo.cmdline="video=basic";drv=DRV_NONE;svga_on=false;bootinfo.is_efi=true;
    video_init();check("forced Basic Display never initializes another GPU",drv==DRV_NONE&&config_writes==0&&!svga_on);
    check("firmware fallback reports driver and fixed mode",strstr(video_driver_name(),"WRLD Basic Display Driver")&&!video_can_set_mode());
}
int main(void) {
    const uint32_t update[]={1,10,20,30,40};
    reset();check("whole packet submits",video_svga_submit(update,ARRAY_LEN(update)));
    check("whole packet published before host kick",kicks==1&&nseen==ARRAY_LEN(update)&&!memcmp(seen,update,sizeof update));
    check("obsolete DEAD slots never read as status",reads[24]==0);
    check("reservation cleared after publish",memory[F_RESERVED]==0);
    reset();memory[F_NEXT]=memory[F_STOP]=memory[F_MAX]-8;
    check("wrapped packet submits",video_svga_submit(update,ARRAY_LEN(update)));
    check("wrapped packet is contiguous to host",nseen==ARRAY_LEN(update)&&!memcmp(seen,update,sizeof update)&&memory[F_NEXT]==memory[F_MIN]+12);
    reset();consume=false;memory[F_STOP]=memory[F_NEXT]+4;uint32_t prior=memory[F_NEXT];memory[prior/4]=0xBADCAFE;
    check("full FIFO times out",!video_svga_submit(update,ARRAY_LEN(update)));
    check("timeout leaves unpublished packet untouched",memory[F_NEXT]==prior&&memory[prior/4]==0xBADCAFE&&now>=250&&now<260);
    reset();memory[F_NEXT]++;
    check("misaligned NEXT rejected",!video_svga_submit(update,ARRAY_LEN(update))&&kicks==0);
    reset();memory[F_STOP]=memory[F_MAX];
    check("out-of-range STOP rejected",!video_svga_submit(update,ARRAY_LEN(update))&&kicks==0);
    reset();memory[F_MIN]=12;
    check("invalid register/data boundary rejected",!video_svga_submit(update,ARRAY_LEN(update)));
    reset();check("oversized packet rejected",!video_svga_submit(update,65537));
    check("packet larger than ring rejected",!video_svga_submit(update,4096));
    check("null/empty packets rejected",!video_svga_submit(NULL,1)&&!video_svga_submit(update,0));
    svga_on=false;check("inactive FIFO rejected",!video_svga_submit(update,ARRAY_LEN(update)));
    reset();check("fenced completion",video_svga_fence(1,25)&&memory[F_FENCE]==1);
    reset();memory[F_FENCE]=0xFFFFFFFEu;
    check("fence counter wrap",video_svga_fence(0xFFFFFFFFu,25)&&video_svga_fence(0,25)&&video_svga_fence(1,25));
    reset();ack_fences=false;
    check("consumed commands without fence do not complete",!video_svga_fence(1,25)&&now>=25&&now<30);
    reset();svga_fifo_caps=0;check("unsupported fences rejected",!video_svga_fence(1,25)&&kicks==0);
    reset();svga_on=false;svga_fifo_caps=memory[F_CAPS]=0;registers[S_BPL]=1280*4;registers[S_FB_START]=0xE0000000u;
    check("ordinary 2D mode switch",video_set_mode(1280,800));
    check("mode switch refreshes FIFO capabilities",svga_fifo_caps==0x41&&fb_info.width==1280&&fb_info.height==800&&fb_info.pitch==5120);
    check("refreshed fence capability works",video_svga_fence(1,25));
    reset();svga_caps=0;video_svga_cpu_fallback();check("legacy devices do not receive TRACES writes",writes[45]==0);
    svga_caps=0x00200000u;video_svga_cpu_fallback();check("advertised trace fallback enabled",writes[45]==1&&registers[45]==1);
    display_selection();
    printf("SVGA transport tests: %s (%d checks)\n",failures?"FAILED":"passed",checks);
    return failures?1:0;
}
