/* The other processors as compute workers.
 *
 * THE WRLD OS schedules its tasks on the boot processor only. The application
 * processors (APs) are started here and then do one thing: run jobs that smp_run hands
 * out (pieces of a 3D draw), pure computation on memory the job owns, with no kernel
 * calls, no allocation and no task switching. Interrupts stay off on them except while
 * they sleep in hlt, from which an inter-processor interrupt (vector SMP_WAKE) wakes
 * them; the device interrupts keep going to the boot processor.
 *
 * Start-up: the INIT-SIPI-SIPI sequence to all other processors at once; the
 * trampoline (src/arch/<arch>/ap.asm, copied to TRAMP) brings each into the kernel's
 * mode on the kernel's page tables with its own stack. "nosmp" on the command line
 * keeps them asleep. */
#include "kernel.h"
#include "smp.h"

#define TRAMP 0x8000u
#define STACK 65536u
#define SMP_WAKE 0xF1

extern const uint8_t ap_tramp_start[], ap_tramp_end[], ap_tramp_params[];
typedef struct PACKED { uint32_t cr3, cr4, cr0, efer; uint64_t stacks; uint32_t stacksz, count; uint64_t entry; } tparams_t;
typedef struct PACKED { uint16_t limit; uintptr_t base; } dptr_t;

static volatile uint32_t *lapic;
static uint8_t *stacks;
static dptr_t gdtr, idtr;
static uint64_t pat, xcr0; static bool have_pat, have_xsave;
static volatile int online;                  /* APs that reached their loop */
int smp_workers_n = 1;                       /* processors that take part in smp_run, the boot one included */

/* the current job */
static void (*volatile job_fn)(void *arg, int worker, int nworkers);
static void *volatile job_arg;
static volatile uint32_t job_seq, job_done;
static volatile int job_n;
static volatile int busy;

static uint64_t rdmsr_(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
static void wrmsr_(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static void cpuid_(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0)); }
static void lapic_w(uint32_t off, uint32_t v) { lapic[off / 4] = v; }
static uint32_t lapic_r(uint32_t off) { return lapic[off / 4]; }
static void icr(uint32_t lo, uint32_t dest) {
    while (lapic_r(0x300) & (1u << 12)) __asm__ volatile("pause");   /* delivery pending */
    lapic_w(0x310, dest << 24);
    lapic_w(0x300, lo);
}
static void spin_us(uint32_t us) {
    /* a calibrated delay is not needed this early: the timer ticks every millisecond */
    uint32_t t = timer_ticks(), ms = (us + 999) / 1000;
    while (timer_ticks() - t < ms + 1) __asm__ volatile("pause");
}

/* the wake-up interrupt: acknowledge it and return (to after the hlt) */
#ifdef __x86_64__
__asm__(".globl smp_wake_isr\nsmp_wake_isr:\n push %rax\n mov smp_eoi(%rip), %rax\n movl $0, (%rax)\n pop %rax\n iretq\n");
#else
__asm__(".globl smp_wake_isr\nsmp_wake_isr:\n push %eax\n mov smp_eoi, %eax\n movl $0, (%eax)\n pop %eax\n iret\n");
#endif
extern void smp_wake_isr(void);
volatile uint32_t *smp_eoi;

static void ap_main(int idx) {
    __asm__ volatile("lgdt %0" :: "m"(gdtr));
    __asm__ volatile("lidt %0" :: "m"(idtr));
#ifdef __x86_64__
    __asm__ volatile("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%ss" ::: "ax");
#endif
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80; __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    if (have_xsave) __asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)));
    if (have_pat) wrmsr_(0x277, pat);
    /* the local APIC on, so the wake-up interrupt reaches us */
    wrmsr_(0x1B, rdmsr_(0x1B) | (1u << 11));
    lapic_w(0xF0, 0x1FF);
    lapic_w(0x80, 0);
    int me = idx + 1;
    if (me >= SMP_MAX) { for (;;) __asm__ volatile("cli; hlt"); }
    __sync_fetch_and_add(&online, 1);
    uint32_t seen = 0;
    for (;;) {
        /* sleep until a job with a new number; sti;hlt cannot miss the interrupt */
        __asm__ volatile("cli");
        while (job_seq == seen) { __asm__ volatile("sti; hlt; cli" ::: "memory"); }
        seen = job_seq;
        __sync_synchronize();
        if (me < job_n) job_fn(job_arg, me, job_n);
        __sync_fetch_and_add(&job_done, 1);
    }
}

void smp_init(void) {
    extern bool boot_nosmp;
    if (boot_nosmp) { klog("smp: other processors left asleep (nosmp)"); return; }
    uint32_t a, b, c, d;
    cpuid_(1, &a, &b, &c, &d);
    if (!(d & (1u << 9))) return;                                  /* no local APIC */
    uint32_t logical = (d & (1u << 28)) ? (b >> 16) & 255 : 1;
    uint64_t base = rdmsr_(0x1B) & 0xFFFFF000ull;
    if (!base || base > 0xFFFFF000ull) base = 0xFEE00000;
    wrmsr_(0x1B, rdmsr_(0x1B) | (1u << 11));
    paging_set_uncached((uintptr_t)base);
    lapic = (volatile uint32_t *)(uintptr_t)base;
    smp_eoi = lapic + 0xB0 / 4;
    if (!(lapic_r(0xF0) & 0x100)) lapic_w(0xF0, (lapic_r(0xF0) & ~0xFFu) | 0x1FF);   /* software enable (LINT0 as the firmware set it) */
    /* what each AP copies */
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    have_pat = (d & (1u << 16)) != 0; if (have_pat) pat = rdmsr_(0x277);
    uintptr_t cr0, cr3 = 0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr0 & (1u << 31)) cr3 = paging_kernel_root();
    have_xsave = (cr4 & (1u << 18)) != 0;
    if (have_xsave) { uint32_t lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); xcr0 = (uint64_t)hi << 32 | lo; }
    idt_set_gate(SMP_WAKE, (uintptr_t)smp_wake_isr);
    stacks = kmalloc(STACK * SMP_MAX);
    if (!stacks) return;
    /* the trampoline in low memory; what was there comes back afterwards */
    size_t n = ap_tramp_end - ap_tramp_start;
    uint8_t save[512];
    if (n > sizeof save) return;
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMP;
    memcpy(save, t, n);
    memcpy(t, ap_tramp_start, n);
    tparams_t *p = (tparams_t *)(t + (ap_tramp_params - ap_tramp_start));
    p->cr3 = (uint32_t)cr3; p->cr4 = (uint32_t)cr4; p->cr0 = (uint32_t)cr0;
#ifdef __x86_64__
    p->efer = (uint32_t)(rdmsr_(0xC0000080) & ((1u << 8) | (1u << 11)));   /* LME, NXE as here */
#endif
    p->stacks = (uintptr_t)stacks; p->stacksz = STACK; p->count = 0; p->entry = (uintptr_t)ap_main;
    __sync_synchronize();
    icr(0x000C4500, 0);                                            /* INIT to all others */
    spin_us(10000);
    icr(0x000C4600 | (TRAMP >> 12), 0);                            /* startup, twice */
    spin_us(200);
    icr(0x000C4600 | (TRAMP >> 12), 0);
    /* the processors check in within microseconds; give them 100 ms, then until 300 ms
     * for the ones that started to reach their loop */
    uint32_t t0 = timer_ticks();
    while (timer_ticks() - t0 < 100) __asm__ volatile("pause");
    int started = (int)p->count;
    while (timer_ticks() - t0 < 300 && online < MIN(started, SMP_MAX - 1)) __asm__ volatile("pause");
    memcpy(t, save, n);
    smp_workers_n = 1 + online;
    klog("smp: %d other processor%s started (%d running jobs; CPUID says %u logical)", started, started == 1 ? "" : "s", online, logical);
}

/* fn(arg, worker, nworkers) on every worker, the boot processor being worker 0; returns
 * when all are done. A nested or concurrent call runs fn on this processor alone. */
void smp_run(void (*fn)(void *, int, int), void *arg) {
    if (smp_workers_n <= 1 || __sync_lock_test_and_set(&busy, 1)) { fn(arg, 0, 1); return; }
    int n = smp_workers_n;
    job_fn = fn; job_arg = arg; job_n = n; job_done = 0;
    __sync_synchronize();
    job_seq++;
    __sync_synchronize();
    icr(0x000C4000 | SMP_WAKE, 0);                                 /* wake all others */
    fn(arg, 0, n);
    while (job_done < (uint32_t)online) __asm__ volatile("pause");
    __sync_synchronize();
    __sync_lock_release(&busy);
}
