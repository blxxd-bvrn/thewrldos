#ifndef KERNEL_H
#define KERNEL_H
#include "types.h"
#include "lib.h"

#define OS_NAME     "Lumina OS"
#define OS_EDITION  "Home Edition"
#define OS_VERSION  "1.0"
#define OS_BUILD    "2600"

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
static inline uint32_t irq_save(void) { uint32_t f; __asm__ volatile("pushf; pop %0; cli":"=r"(f)::"memory"); return f; }
static inline void irq_restore(uint32_t f) { __asm__ volatile("push %0; popf"::"r"(f):"memory","cc"); }

/* ---- interrupts ---- */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;
typedef void (*irq_handler_t)(regs_t *r);
void gdt_init(void);
void gdt_set_fs_base(uint32_t base);
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
void kfree_aligned(void *p);
uint32_t mm_total(void);
uint32_t mm_used(void);
uint32_t mm_phys_total(void);
uint32_t *paging_kernel_dir(void);
uint32_t *paging_new_dir(void);
void paging_free_dir(uint32_t *dir);

/* ---- tasks (cooperative kernel scheduler) ---- */
enum { TASK_READY, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD };
typedef struct task {
    int pid;
    char name[48];
    int state;
    uint32_t esp;
    uint8_t *stack;
    uint32_t stack_size;
    uint32_t cr3;
    uint32_t wake_at;
    uint32_t cpu_ms;
    uint32_t started;
    uint32_t mem_bytes;
    void (*entry)(void *);
    void *arg;
    void *proc;                 /* PE process data if any */
    void *console;              /* attached console */
    uint32_t teb;               /* FS base */
    int exit_code;
    int kind;                   /* 0 kernel, 1 shell, 2 win32, 3 app */
    struct task *next;
} task_t;
void task_init(void);
task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t stack_size);
void task_yield(void);
void task_sleep(uint32_t ms);
void task_exit(int code);
bool task_kill(int pid);
task_t *task_current(void);
task_t *task_find(int pid);
task_t *task_list(void);
int task_count(void);
void task_set_cr3(task_t *t, uint32_t cr3);
extern void (*task_kill_hook)(task_t *t);

/* ---- misc hardware ---- */
void cpu_get_info(char *vendor, char *brand);
typedef struct { uint8_t bus, dev, fn; uint16_t vendor, device; uint8_t cls, sub, prog; } pci_dev_t;
int pci_scan(pci_dev_t *out, int max);
const char *pci_class_name(uint8_t cls, uint8_t sub);
const char *pci_vendor_name(uint16_t v);
void system_poweroff(void);
void system_reboot(void);

/* ---- boot info ---- */
typedef struct { uint32_t addr; uint32_t pitch, width, height, bpp; } fb_info_t;
extern fb_info_t fb_info;
extern uint8_t *initrd_data;
extern uint32_t initrd_size;
extern bool boot_safe_mode;
#endif
