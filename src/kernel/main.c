/* Lumina OS kernel entry point */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "fs.h"
#include "apps.h"

typedef struct PACKED {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr, drives_length, drives_addr, config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info; uint16_t vbe_mode, vbe_if_seg, vbe_if_off, vbe_if_len;
    uint64_t fb_addr; uint32_t fb_pitch, fb_width, fb_height; uint8_t fb_bpp, fb_type;
} mb_info_full_t;

extern void (*panic_hook)(const char *msg);
extern void (*fault_hook)(task_t *t, const char *msg);
static bool gfx_ready;

static void text_panic(const char *msg) {
    volatile uint16_t *vga = (uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F20;
    const char *hdr = "*** STOP: Lumina OS kernel panic ***";
    for (int i = 0; hdr[i]; i++) vga[i] = 0x1F00 | hdr[i];
    int row = 2, col = 0;
    for (; *msg && row < 25; msg++) { if (*msg == '\n') { row++; col = 0; continue; } vga[row * 80 + col++] = 0x1F00 | *msg; if (col >= 80) { row++; col = 0; } }
}
static void gfx_panic(const char *msg) {
    if (!gfx_ready) { text_panic(msg); return; }
    canvas_t *c = &screen;
    canvas_reset_clip(c);
    gfx_clear(c, HEX(0x0000AA));
    int y = 40;
    draw_text(c, FONT_MONO, 40, y, "A problem has been detected and " OS_NAME " has been halted to prevent", WHITE); y += 18;
    draw_text(c, FONT_MONO, 40, y, "damage to your computer.", WHITE); y += 36;
    draw_text(c, FONT_MONO, 40, y, "KERNEL_MODE_EXCEPTION_NOT_HANDLED", WHITE); y += 36;
    const char *p = msg;
    while (*p) { const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p); draw_text_n(c, FONT_MONO, 40, y, p, e - p, WHITE); y += 18; p = *e ? e + 1 : e; }
    y += 18;
    draw_text(c, FONT_MONO, 40, y, "Technical information has been written to the serial port (COM1).", WHITE); y += 18;
    draw_text(c, FONT_MONO, 40, y, "Restart your computer. If this screen appears again, check the kernel log.", WHITE);
    gfx_present(CURSOR_ARROW, -100, -100);
}

typedef struct { char name[48]; char msg[200]; } fault_info_t;
static void fault_dialog(void *arg) {
    fault_info_t *f = arg;
    char text[400];
    snprintf(text, sizeof text, "%s has encountered a problem and needs to close. We are sorry for the inconvenience.\n\n%s", f->name, f->msg);
    msgbox(f->name, text, MBI_ERROR, MBX_OK, NULL, NULL);
    kfree(f);
}
static void on_fault(task_t *t, const char *msg) {
    fault_info_t *f = kmalloc(sizeof *f);
    strlcpy(f->name, t->name, sizeof f->name); strlcpy(f->msg, msg, sizeof f->msg);
    wm_defer(fault_dialog, f);
}
static void on_task_end(task_t *t) {
    wm_destroy_for_pid(t->pid);
    extern void pe_task_cleanup(task_t *t);
    pe_task_cleanup(t);
}

static void write_system_files(void) {
    rtc_time_t t; rtc_now(&t);
    char vbuf[512];
    snprintf(vbuf, sizeof vbuf, "%s %s\r\nVersion %s (Build %s)\r\nKernel: 32-bit protected mode, 4 MiB paging, cooperative scheduler\r\n", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD);
    vfs_write_file("C:\\Windows\\version.txt", vbuf, strlen(vbuf));
    const char *dlls[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "msvcrt.dll", "advapi32.dll", "shell32.dll", "comctl32.dll", "ntdll.dll" };
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) {
        char p[128], body[128];
        snprintf(p, sizeof p, "C:\\Windows\\System32\\%s", dlls[i]);
        snprintf(body, sizeof body, "Built-in %s: implemented natively by the Lumina Win32 subsystem.\r\n", dlls[i]);
        vnode_t *n = vfs_write_file(p, body, strlen(body)); if (n) n->readonly = true;
    }
}

void kmain(uint32_t magic, uint32_t mbaddr) {
    serial_init();
    gdt_init();
    idt_init();
    panic_hook = text_panic;
    if (magic != 0x2BADB002) panic("Not booted by a Multiboot loader (magic %08x)", magic);
    mb_info_full_t *mb = (mb_info_full_t *)mbaddr;
    mm_init(mbaddr);
    task_init();
    timer_init();
    sti();
    klog("%s %s %s (build %s) starting", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD);
    if (!(mb->flags & (1 << 12)) || mb->fb_bpp != 32)
        panic("No 32-bit linear framebuffer from the bootloader (flags=%x bpp=%d).\nBoot with GRUB using the supplied grub.cfg.", mb->flags, mb->fb_bpp);
    fb_info.addr = (uint32_t)mb->fb_addr; fb_info.pitch = mb->fb_pitch;
    fb_info.width = mb->fb_width; fb_info.height = mb->fb_height; fb_info.bpp = mb->fb_bpp;
    klog("framebuffer %ux%ux%u at %08x pitch %u", fb_info.width, fb_info.height, fb_info.bpp, fb_info.addr, fb_info.pitch);
    klog("memory: %u MB detected, heap %u MB", mm_phys_total() / (1024 * 1024), mm_total() / (1024 * 1024));
    gfx_init();
    gfx_ready = true;
    panic_hook = gfx_panic;
    fault_hook = on_fault;
    task_kill_hook = on_task_end;
    rtc_time_t now; rtc_read(&now);
    rand_seed(now.second * 1000 + now.minute * 60000 + timer_ticks());

    boot_screen_begin();
    boot_screen_progress("Initializing devices...", 10);
    keyboard_init();
    mouse_init(SCREEN_W, SCREEN_H);
    char vendor[16], brand[52]; cpu_get_info(vendor, brand);
    klog("cpu: %s (%s)", brand, vendor);
    pci_dev_t devs[32]; int nd = pci_scan(devs, 32);
    for (int i = 0; i < nd; i++) klog("pci %02x:%02x.%x %04x:%04x %s", devs[i].bus, devs[i].dev, devs[i].fn, devs[i].vendor, devs[i].device, pci_class_name(devs[i].cls, devs[i].sub));
    boot_screen_progress("Mounting file systems...", 35);
    vfs_init();
    if (initrd_data) tar_extract(initrd_data, initrd_size, vfs_root('C'));
    else klog("initrd: none supplied");
    write_system_files();
    boot_screen_progress("Starting Win32 subsystem...", 60);
    w32_init();
    boot_screen_progress("Loading personal settings...", 85);
    wm_init();
    shell_init();
    boot_screen_progress("Starting shell...", 100);
    boot_screen_end();
    welcome_screen();
    wm_redraw_all();
    notify_balloon("Welcome to " OS_NAME, "Try the samples in C:\\Samples: a Win32 .exe, a shell script and a .dmg disk image.");
    wm_run();
}
