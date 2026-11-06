/* GDT, IDT, PIC, exception handling, serial log */
#include "kernel.h"
#include "disk.h"

/* ---------------- GDT ---------------- */
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
void gdt_set_fs_base(uint32_t base) {
    if (!base) base = (uint32_t)default_teb;
    gdt[5].base_lo = base & 0xFFFF;
    gdt[5].base_mid = (base >> 16) & 0xFF;
    gdt[5].base_hi = (base >> 24) & 0xFF;
    gdt[5].access = 0x92;
    fs_reload();
}

/* ---------------- IDT ---------------- */
typedef struct PACKED { uint16_t off_lo, sel; uint8_t zero, flags; uint16_t off_hi; } idt_entry_t;
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

static void idt_set(int n, uint32_t off, uint8_t flags) {
    idt[n].off_lo = off & 0xFFFF; idt[n].off_hi = off >> 16;
    idt[n].sel = 0x08; idt[n].zero = 0; idt[n].flags = flags;
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
    for (int i = 0; i < 48; i++) idt_set(i, (uint32_t)isr_table[i], 0x8E);
    idt_set(128, (uint32_t)isr128, 0xEE);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint32_t)idt;
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
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }
    if (r->int_no == 128) { r->eax = 0; return; }
    uint32_t cr2; __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    task_t *t = task_current();
    char msg[200];
    snprintf(msg, sizeof msg, "%s (0x%02x) at EIP=%08x, error=%x, CR2=%08x",
             r->int_no < 32 ? exc_names[r->int_no] : "UNKNOWN", r->int_no, r->eip, r->err_code, cr2);
    klog("EXCEPTION in task %d (%s): %s", t ? t->pid : -1, t ? t->name : "?", msg);
    if (t && t->pid != 0) {
        /* A process faulted: terminate it, keep the system running. */
        if (fault_hook) fault_hook(t, msg);
        t->exit_code = -1;
        task_exit(0xC0000005);
    }
    panic("%s\nEAX=%08x EBX=%08x ECX=%08x EDX=%08x\nESI=%08x EDI=%08x EBP=%08x", msg,
          r->eax, r->ebx, r->ecx, r->edx, r->esi, r->edi, r->ebp);
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
int pci_scan(pci_dev_t *out, int max) {
    int n = 0;
    for (int bus = 0; bus < 8; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
                uint32_t cl = pci_read(bus, dev, fn, 8);
                if (n < max) {
                    out[n].bus = bus; out[n].dev = dev; out[n].fn = fn;
                    out[n].vendor = id & 0xFFFF; out[n].device = id >> 16;
                    out[n].cls = cl >> 24; out[n].sub = (cl >> 16) & 0xFF; out[n].prog = (cl >> 8) & 0xFF;
                }
                n++;
                if (fn == 0 && !((pci_read(bus, dev, 0, 0x0C) >> 16) & 0x80)) break;
            }
    return n < max ? n : max;
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
    cli();
    outw(0x604, 0x2000);   /* QEMU (newer) */
    outw(0xB004, 0x2000);  /* Bochs / old QEMU */
    outw(0x4004, 0x3400);  /* VirtualBox */
    for (;;) hlt();
}
void system_reboot(void) {
    persist_flush();
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    desc_ptr_t null_idt = { 0, 0 };
    idt_flush(&null_idt);
    __asm__ volatile("int $3");
    for (;;) hlt();
}
