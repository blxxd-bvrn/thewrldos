/* GDT, IDT, PIC, exception handling, serial log */
#include "kernel.h"
#include "disk.h"

/* ---------------- GDT / IDT ---------------- */
#ifdef __x86_64__
/* The boot stub's GDT (null, 64-bit code 0x08, data 0x10) stays loaded. The
 * TEB of x64 Win32 programs is reached through GS, whose base is an MSR. */
typedef struct PACKED { uint16_t limit; uint64_t base; } desc_ptr_t;
extern void idt_flush(desc_ptr_t *p);
static uint8_t default_teb[4096] __attribute__((aligned(16)));
static inline void wrmsr(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
/* GDT: kernel code/data, then the user segments in the order SYSRET expects
 * (32-bit code 0x18, data 0x20, 64-bit code 0x28) and the TSS at 0x30, whose
 * RSP0 is the kernel stack used when a Linux program in ring 3 is interrupted. */
typedef struct PACKED { uint32_t r0; uint64_t rsp0, rsp1, rsp2; uint64_t r1; uint64_t ist[7]; uint64_t r2; uint16_t r3, iomap; } tss64_t;
static tss64_t tss;
static uint64_t gdt64[8] __attribute__((aligned(16)));
extern void gdt_load64(desc_ptr_t *p, uint16_t tss_sel);
extern void syscall_entry(void);
uint64_t lx_kstack_top, lx_user_rsp_tmp;
static inline uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((uint64_t)hi << 32) | lo; }
void gdt_init(void) {
    wrmsr(0xC0000101, (uintptr_t)default_teb);
    gdt64[0] = 0;
    gdt64[1] = 0x00AF9A000000FFFFull;      /* 0x08 kernel code */
    gdt64[2] = 0x00CF92000000FFFFull;      /* 0x10 kernel data */
    gdt64[3] = 0x00CFFA000000FFFFull;      /* 0x18 user 32-bit code (unused) */
    gdt64[4] = 0x00CFF2000000FFFFull;      /* 0x20 user data */
    gdt64[5] = 0x00AFFA000000FFFFull;      /* 0x28 user 64-bit code */
    uint64_t b = (uintptr_t)&tss, lim = sizeof tss - 1;
    tss.iomap = sizeof tss;
    gdt64[6] = (lim & 0xFFFF) | ((b & 0xFFFFFF) << 16) | (0x89ull << 40) | (((lim >> 16) & 0xF) << 48) | (((b >> 24) & 0xFF) << 56);
    gdt64[7] = b >> 32;
    static desc_ptr_t gp;
    gp.limit = sizeof gdt64 - 1; gp.base = (uintptr_t)gdt64;
    gdt_load64(&gp, 0x30);
    /* SYSCALL/SYSRET, and no-execute pages */
    uint32_t a, bb, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(bb), "=c"(c), "=d"(d) : "a"(0x80000001));
    uint64_t efer = rdmsr(0xC0000080) | 1;                 /* SCE */
    if (d & (1u << 20)) efer |= 1u << 11;                  /* NXE */
    wrmsr(0xC0000080, efer);
    wrmsr(0xC0000081, (0x18ull << 48) | (0x08ull << 32));  /* STAR */
    wrmsr(0xC0000082, (uintptr_t)syscall_entry);           /* LSTAR */
    wrmsr(0xC0000084, 0x47700);                            /* FMASK: IF, DF, TF, AC, NT, IOPL */
}
void arch_set_kernel_stack(uintptr_t top) { tss.rsp0 = top; lx_kstack_top = top; }
void arch_set_user_fs(uint64_t base) { wrmsr(0xC0000100, base); }
void gdt_set_fs_base(uintptr_t base) { wrmsr(0xC0000101, base ? base : (uintptr_t)default_teb); }
typedef struct PACKED { uint16_t off_lo, sel; uint8_t ist, flags; uint16_t off_mid; uint32_t off_hi, zero; } idt_entry_t;
#else
typedef struct PACKED { uint16_t limit_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } gdt_entry_t;
typedef struct PACKED { uint16_t limit; uint32_t base; } desc_ptr_t;
static gdt_entry_t gdt[8];
static desc_ptr_t gdt_ptr;
extern void gdt_flush(desc_ptr_t *p);
extern void fs_reload(void);
extern void idt_flush(desc_ptr_t *p);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_lo = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_hi = (base >> 24) & 0xFF;
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].gran = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access = access;
}
static uint32_t default_teb[1024] __attribute__((aligned(16)));
void gdt_init(void) {
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* kernel code */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);   /* kernel data */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* user code */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* user data */
    gdt_set(5, (uint32_t)default_teb, 0xFFF, 0x92, 0x40); /* FS: thread environment block */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint32_t)gdt;
    gdt_flush(&gdt_ptr);
}
void gdt_set_fs_base(uintptr_t base) {
    if (!base) base = (uint32_t)default_teb;
    gdt[5].base_lo = base & 0xFFFF;
    gdt[5].base_mid = (base >> 16) & 0xFF;
    gdt[5].base_hi = (base >> 24) & 0xFF;
    gdt[5].access = 0x92;
    fs_reload();
}
typedef struct PACKED { uint16_t off_lo, sel; uint8_t zero, flags; uint16_t off_hi; } idt_entry_t;
#endif

/* ---------------- IDT ---------------- */
static idt_entry_t idt[256];
static desc_ptr_t idt_ptr;
static irq_handler_t irq_handlers[16];

#define X(n) extern void isr##n(void);
X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(128)
#undef X
static void (*const isr_table[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39, isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47 };

static void idt_set(int n, uintptr_t off, uint8_t flags) {
    memset(&idt[n], 0, sizeof idt[n]);
    idt[n].off_lo = off & 0xFFFF;
#ifdef __x86_64__
    idt[n].off_mid = (off >> 16) & 0xFFFF; idt[n].off_hi = (uint32_t)(off >> 32);
#else
    idt[n].off_hi = off >> 16;
#endif
    idt[n].sel = 0x08; idt[n].flags = flags;
}

static void pic_remap(void) {
    uint8_t a1 = inb(0x21), a2 = inb(0xA1);
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    (void)a1; (void)a2;
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
}
static void pic_unmask(int irq) {
    uint16_t port = irq < 8 ? 0x21 : 0xA1;
    if (irq >= 8) { irq -= 8; outb(0x21, inb(0x21) & ~(1 << 2)); }
    outb(port, inb(port) & ~(1 << irq));
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) idt_set(i, (uintptr_t)isr_table[i], 0x8E);
    idt_set(128, (uintptr_t)isr128, 0xEE);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uintptr_t)idt;
    pic_remap();
    idt_flush(&idt_ptr);
}
void irq_install(int irq, irq_handler_t h) { irq_handlers[irq] = h; pic_unmask(irq); }

static const char *exc_names[32] = {
    "DIVIDE_BY_ZERO", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND_RANGE", "INVALID_OPCODE",
    "DEVICE_NOT_AVAILABLE", "DOUBLE_FAULT", "COPROCESSOR_SEGMENT", "INVALID_TSS", "SEGMENT_NOT_PRESENT",
    "STACK_FAULT", "GENERAL_PROTECTION_FAULT", "PAGE_FAULT", "RESERVED", "X87_FLOATING_POINT",
    "ALIGNMENT_CHECK", "MACHINE_CHECK", "SIMD_FLOATING_POINT", "VIRTUALIZATION", "CONTROL_PROTECTION",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "SECURITY", "RESERVED" };

void (*fault_hook)(task_t *t, const char *msg) = 0;

void isr_handler(regs_t *r) {
    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = r->int_no - 32;
        if (irq == 7 || irq == 15) { /* spurious? */
            if (irq == 15) outb(0x20, 0x20);
            if (!(irq == 7 ? (outb(0x20, 0x0B), inb(0x20) & 0x80) : (outb(0xA0, 0x0B), inb(0xA0) & 0x80))) return;
        }
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (irq == 0) {
            timer_eoi();
            if (timer_using_lapic()) return;     /* APIC interrupt: no 8259 EOI, which could
                                                  * clear another device's in-service bit */
        }
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }
    if (r->int_no == 128) { r->eax = 0; return; }
    uintptr_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
#ifdef __x86_64__
    { extern bool lx_exception(regs_t *r, uintptr_t cr2); if (lx_exception(r, cr2)) return; }
#endif
    task_t *t = task_current();
    char msg[200];
    snprintf(msg, sizeof msg, "%s (0x%02x) at %s=%0*llx, error=%x, CR2=%0*llx",
             r->int_no < 32 ? exc_names[r->int_no] : "UNKNOWN", (int)r->int_no, ARCH_BITS == 64 ? "RIP" : "EIP", ARCH_BITS / 4, (unsigned long long)r->eip,
             (unsigned)r->err_code, ARCH_BITS / 4, (unsigned long long)cr2);
    klog("EXCEPTION in task %d (%s): %s", t ? t->pid : -1, t ? t->name : "?", msg);
    if (t && t->pid != 0) {
        /* for debugging programs: the registers, and the return addresses on the stack */
#ifdef __x86_64__
        uintptr_t *sp = (uintptr_t *)r->esp;
#else
        uintptr_t *sp = (uintptr_t *)((uint8_t *)r + sizeof *r);   /* same privilege level: no ESP/SS pushed */
#endif
        klog("  AX=%llx BX=%llx CX=%llx DX=%llx SI=%llx DI=%llx BP=%llx SP=%llx", (unsigned long long)r->eax, (unsigned long long)r->ebx,
             (unsigned long long)r->ecx, (unsigned long long)r->edx, (unsigned long long)r->esi, (unsigned long long)r->edi,
             (unsigned long long)r->ebp, (unsigned long long)(uintptr_t)sp);
        extern char kernel_end[];
        char line[200] = ""; int k = 0, shown = 0;
        for (int i = 0; i < 96 && shown < 12; i++) {
            if (((uintptr_t)(sp + i) & 0xFFF) == 0 && i) break;              /* do not walk off the stack page by page into unmapped memory */
            uintptr_t v = sp[i];
            if ((v >= 0x100000 && v < (uintptr_t)kernel_end) || (v >= 0x400000 && v < 0x4000000)
#ifdef __x86_64__
                || (v >= 0x140000000ull && v < 0x200000000ull)
#endif
               ) {
                k += snprintf(line + k, sizeof line - k, " %llx", (unsigned long long)v); shown++;
            }
        }
        klog("  stack:%s", line);
        (void)k;
    }
    if (t && t->pid != 0) {
        /* A process faulted: terminate it, keep the system running. */
        if (fault_hook) fault_hook(t, msg);
        t->exit_code = -1;
        task_exit(0xC0000005);
    }
    panic("%s\nAX=%llx BX=%llx CX=%llx DX=%llx\nSI=%llx DI=%llx BP=%llx", msg,
          (unsigned long long)r->eax, (unsigned long long)r->ebx, (unsigned long long)r->ecx, (unsigned long long)r->edx,
          (unsigned long long)r->esi, (unsigned long long)r->edi, (unsigned long long)r->ebp);
}

/* ---------------- serial + log ---------------- */
#define COM1 0x3F8
static bool serial_ok;
void serial_init(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x01); outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);
    serial_ok = true;
}
static void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 10000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, c);
}
void serial_write(const char *s) { while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); } }

#define KLOG_SIZE 16384
static char klog_buf[KLOG_SIZE];
static uint32_t klog_len;
void (*klog_hook)(const char *line) = 0;
void klog(const char *fmt, ...) {
    char line[256];
    va_list ap; va_start(ap, fmt);
    uint32_t t = timer_ticks();
    int n = snprintf(line, sizeof line, "[%5u.%03u] ", t / 1000, t % 1000);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    strlcat(line, "\n", sizeof line);
    serial_write(line);
    uint32_t l = strlen(line);
    if (klog_len + l >= KLOG_SIZE) { memmove(klog_buf, klog_buf + KLOG_SIZE / 2, KLOG_SIZE / 2); klog_len -= KLOG_SIZE / 2; }
    memcpy(klog_buf + klog_len, line, l + 1);
    klog_len += l;
    if (klog_hook) klog_hook(line);
}
const char *klog_buffer(void) { return klog_buf; }

void (*panic_hook)(const char *msg) = 0;
void panic(const char *fmt, ...) {
    cli();
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    serial_write("\n*** KERNEL PANIC ***\n"); serial_write(msg); serial_write("\n");
    if (panic_hook) panic_hook(msg);
    for (;;) hlt();
}

/* ---------------- CPU / PCI / power ---------------- */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
void cpu_get_info(char *vendor, char *brand) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t *p = (uint32_t *)brand;
        for (uint32_t l = 0x80000002; l <= 0x80000004; l++) { cpuid(l, &a, &b, &c, &d); *p++ = a; *p++ = b; *p++ = c; *p++ = d; }
        brand[48] = 0;
        char *s = brand; while (*s == ' ') s++;
        memmove(brand, s, strlen(s) + 1);
    } else strcpy(brand, "Generic x86 Processor");
}

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) { return pci_read(bus, dev, fn, off); }
void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}
/* AVX for programs (64-bit edition): a processor that has it reports it, and libraries use it
 * without always checking that the system turned it on, which faults. So turn it on: CR4.OSXSAVE
 * and x87, SSE and AVX state in XCR0 (not AVX-512, whose state would not fit the save area), and
 * have interrupts and system calls save it with XSAVE (src/arch/x86_64/isr.asm, lx.asm). */
uint8_t fpu_xsave;
void cpu_enable_avx(void) {
#ifdef __x86_64__
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!((c >> 26) & 1) || !((c >> 28) & 1)) return;             /* XSAVE and AVX */
    uint64_t cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 | (1ull << 18)));
    __asm__ volatile("xsetbv" :: "c"(0), "a"(7), "d"(0));
    cpuid(0xD, &a, &b, &c, &d);                                    /* the save area for that state */
    if (b > 1024) { __asm__ volatile("xsetbv" :: "c"(0), "a"(3), "d"(0)); __asm__ volatile("mov %0, %%cr4" :: "r"(cr4)); return; }
    fpu_xsave = 1;
#endif
}
bool cpu_has_rdrand(void) { uint32_t a, b, c, d; cpuid(1, &a, &b, &c, &d); return (c >> 30) & 1; }
/* every bus is scanned: VMware puts NVMe, SAS and network adapters behind
 * PCIe root ports on buses 2..0x40 */
static pci_dev_t pci_cache[256];
static int pci_ncache = -1;
int pci_list(pci_dev_t **out) {
    if (pci_ncache < 0) {
        int n = 0;
        for (int bus = 0; bus < 256; bus++)
            for (int dev = 0; dev < 32; dev++)
                for (int fn = 0; fn < 8; fn++) {
                    uint32_t id = pci_read(bus, dev, fn, 0);
                    if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                    uint32_t cl = pci_read(bus, dev, fn, 8);
                    if (n < (int)ARRAY_LEN(pci_cache)) {
                        pci_dev_t *o = &pci_cache[n++];
                        o->bus = bus; o->dev = dev; o->fn = fn;
                        o->vendor = id & 0xFFFF; o->device = id >> 16;
                        o->cls = cl >> 24; o->sub = (cl >> 16) & 0xFF; o->prog = (cl >> 8) & 0xFF;
                    }
                    if (fn == 0 && !((pci_read(bus, dev, 0, 0x0C) >> 16) & 0x80)) break;
                }
        pci_ncache = n;
    }
    *out = pci_cache;
    return pci_ncache;
}
int pci_scan(pci_dev_t *out, int max) {
    pci_dev_t *all; int n = pci_list(&all);
    if (n > max) n = max;
    memcpy(out, all, n * sizeof *out);
    return n;
}
void pci_rescan(void) { pci_ncache = -1; }
void pci_enable(const pci_dev_t *p, bool io, bool mem) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04) & 0xFFFF;
    cmd |= 0x04 | (io ? 1 : 0) | (mem ? 2 : 0);          /* bus master */
    cmd |= 0x400;                                        /* legacy INTx off: drivers poll */
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, cmd);
}
const char *pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01: return s == 0x01 ? "IDE Controller" : s == 0x06 ? "SATA Controller" : s == 0x08 ? "NVMe Controller" : "Storage Controller";
    case 0x02: return "Network Adapter";
    case 0x03: return "Display Adapter";
    case 0x04: return s == 0x03 ? "High Definition Audio" : "Multimedia Device";
    case 0x05: return "Memory Controller";
    case 0x06: return s == 0x00 ? "Host Bridge" : s == 0x01 ? "ISA Bridge" : s == 0x04 ? "PCI-to-PCI Bridge" : s == 0x80 ? "System Bridge" : "Bridge Device";
    case 0x07: return "Communication Controller";
    case 0x08: return "System Peripheral";
    case 0x09: return "Input Device";
    case 0x0C: return s == 0x03 ? "USB Controller" : s == 0x05 ? "SMBus Controller" : "Serial Bus Controller";
    default: return "Unknown Device";
    }
}
const char *pci_vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel Corporation";
    case 0x1234: return "Bochs/QEMU";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "Realtek";
    case 0x1AF4: return "Red Hat VirtIO";
    case 0x15AD: return "VMware";
    case 0x80EE: return "VirtualBox";
    case 0x1B36: return "Red Hat QEMU";
    case 0x1013: return "Cirrus Logic";
    default: return "Unknown Vendor";
    }
}
void system_poweroff(void) {
    persist_flush();
    acpi_poweroff();    /* returns only if the machine could not be switched off */
}
void system_reboot(void) {
    persist_flush();
    cli();
    acpi_reset();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    desc_ptr_t null_idt = { 0, 0 };
    idt_flush(&null_idt);
    __asm__ volatile("int $3");
    for (;;) hlt();
}
