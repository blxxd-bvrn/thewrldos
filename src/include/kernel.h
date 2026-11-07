#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#include "lib.h"

#define OS_NAME     "THE WRLD OS"
#ifdef __x86_64__
#define OS_EDITION  "Home Edition x64"
#define OS_ARCH_DESC "64-bit operating system (x86-64 long mode, 4-level paging)"
#define OS_KERNEL_DESC "64-bit x86-64 kernel: long mode, IDT, PIC, PIT, 4-level paging, heap, scheduler"
#define OS_EXE_DESC "64-bit (x64)"
#define OS_MACHINE "x86_64"
#else
#define OS_EDITION  "Home Edition"
#define OS_ARCH_DESC "32-bit operating system (i686 protected mode, 4 MiB paging)"
#define OS_KERNEL_DESC "32-bit x86 kernel: GDT/IDT, PIC, PIT, paging, heap, scheduler"
#define OS_EXE_DESC "32-bit (x86)"
#define OS_MACHINE "i686"
#endif
#define OS_VERSION  "1.0"
#define OS_BUILD    "2600"
extern char computer_name[32];

/* ---- port I/O ---- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline uintptr_t irq_save(void) { uintptr_t f; __asm__ volatile("pushf; pop %0; cli":"=r"(f)::"memory"); return f; }
static inline void irq_restore(uintptr_t f) { __asm__ volatile("push %0; popf"::"r"(f):"memory","cc"); }

/* ---- interrupts ---- */
#ifdef __x86_64__
#define ARCH_NAME "x64"
#define ARCH_BITS 64
/* pushed by src/arch/x86_64/isr.asm; the e* names hold the full 64-bit registers */
typedef struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t edi, esi, ebp, ebx, edx, ecx, eax;
    uint64_t int_no, err_code;
    uint64_t eip, cs, eflags, esp, ss;
} regs_t;
#else
#define ARCH_NAME "x86"
#define ARCH_BITS 32
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;
#endif
typedef void (*irq_handler_t)(regs_t *r);
void gdt_init(void);
void gdt_set_fs_base(uintptr_t base);    /* x86: FS segment base; x64: GS base (the TEB) */
void idt_init(void);
void irq_install(int irq, irq_handler_t h);

/* ---- logging / serial ---- */
void serial_init(void);
void serial_write(const char *s);
void klog(const char *fmt, ...);
const char *klog_buffer(void);
void panic(const char *fmt, ...);
extern bool boot_verbose;

/* ---- timer / rtc ---- */
#define TIMER_HZ 1000
extern void (*timer_tick_hook)(uint32_t ticks);
struct regs;
extern void (*watchdog_hook)(struct regs *r, uint32_t ticks);   /* timer interrupt, every 64 ms */
void timer_init(void);
uint32_t timer_ticks(void);           /* milliseconds since boot */
extern volatile uint32_t idle_ticks;
typedef struct { int year, month, day, hour, minute, second, weekday; } rtc_time_t;
void rtc_read(rtc_time_t *t);
void rtc_now(rtc_time_t *t);          /* rtc + timezone offset applied */
extern int tz_offset_minutes;
void sleep_ms(uint32_t ms);
void speaker_beep(uint32_t freq, uint32_t ms);

/* ---- input ---- */
enum {
    KEY_ESC = 0x100, KEY_BACKSPACE, KEY_TAB, KEY_ENTER, KEY_LCTRL, KEY_LSHIFT, KEY_RSHIFT,
    KEY_LALT, KEY_CAPS, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8,
    KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT, KEY_DELETE, KEY_WIN, KEY_MENU
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
typedef struct { int key; char ch; bool pressed; int mods; } key_event_t;
void keyboard_init(void);
bool keyboard_poll(key_event_t *ev);
void mouse_init(int w, int h);
void mouse_set_bounds(int w, int h);
typedef struct { int x, y; int buttons; int wheel; } mouse_state_t;
bool mouse_poll(mouse_state_t *st);   /* returns true if changed */
extern int mouse_speed;               /* 1..10 */
extern bool mouse_swap_buttons;

/* ---- memory ---- */
void mm_init(uint32_t mb_info);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
void *kmalloc_aligned(size_t n, size_t align);
size_t kmalloc_usable(void *p);
void kfree_aligned(void *p);
uint32_t mm_total(void);
uint32_t mm_used(void);
uint32_t mm_phys_total(void);
uintptr_t paging_kernel_root(void);                   /* CR3 of the kernel address space */
#ifdef __x86_64__
uintptr_t vm_space_new(void);                          /* a private address space sharing the kernel's 0..4 GiB */
bool vm_map_2m(uintptr_t space, uint64_t va, uintptr_t pa);   /* map one 2 MiB page */
void vm_space_free(uintptr_t space);
#else
uint32_t *paging_kernel_dir(void);
uint32_t *paging_new_dir(void);
void paging_free_dir(uint32_t *dir);
#endif
void paging_set_uncached(uintptr_t addr);

/* ---- tasks (cooperative kernel scheduler) ---- */
enum { TASK_READY, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD };
typedef struct task {
    int pid;
    char name[48];
    int state;
    uintptr_t esp;
    uint8_t *stack;
    uint32_t stack_size;
    uintptr_t cr3;
    uint32_t wake_at;
    uint32_t cpu_ms;
    uint32_t started;
    uint32_t mem_bytes;
    void (*entry)(void *);
    void *arg;
    void *proc;                 /* PE process data if any */
    void *console;              /* attached console */
    uintptr_t teb;              /* FS (x86) / GS (x64) base */
    int exit_code;
    int kind;                   /* 0 kernel, 1 shell, 2 win32, 3 app */
    struct task *next;
} task_t;
void task_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t stack_size);
void task_yield(void);
extern volatile uint32_t gui_heartbeat;
void task_sleep(uint32_t ms);
void task_exit(int code);
bool task_kill(int pid);
int task_exit_code(int pid);
task_t *task_current(void);
task_t *task_find(int pid);
task_t *task_list(void);
int task_count(void);
void task_set_cr3(task_t *t, uintptr_t cr3);
extern void (*task_kill_hook)(task_t *t);

/* ---- misc hardware ---- */
void cpu_get_info(char *vendor, char *brand);
typedef struct { uint8_t bus, dev, fn; uint16_t vendor, device; uint8_t cls, sub, prog; } pci_dev_t;
int pci_scan(pci_dev_t *out, int max);
uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
bool cpu_has_rdrand(void);
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
const char *pci_class_name(uint8_t cls, uint8_t sub);
const char *pci_vendor_name(uint16_t v);
void system_poweroff(void);   /* returns if power-off is not supported */
void acpi_init(void);
bool acpi_poweroff(void);
void acpi_reset(void);
const char *acpi_describe(void);
void system_reboot(void);

/* ---- boot info ---- */
typedef struct { uint32_t addr; uint32_t pitch, width, height, bpp; } fb_info_t;
extern fb_info_t fb_info;
/* display drivers (video.c) */
void video_init(void);
const char *video_driver_name(void);
bool video_can_set_mode(void);
bool video_mode_ok(int w, int h);
bool video_set_mode(int w, int h);
void video_update(int x, int y, int w, int h);
extern uint8_t *initrd_data;
extern uint32_t initrd_size;
extern bool boot_safe_mode;
#endif
