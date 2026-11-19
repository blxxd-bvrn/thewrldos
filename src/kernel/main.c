/* THE WRLD OS kernel entry point */
#include "kernel.h"
#include "devmgr.h"
#include "bootinfo.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "fs.h"
#include "apps.h"
#include "disk.h"
#include "net.h"
#include "audio.h"
#include "usb.h"

extern void (*panic_hook)(const char *msg);
extern void (*fault_hook)(task_t *t, const char *msg);
static bool gfx_ready;
static const char *panic_code = "KERNEL_MODE_EXCEPTION_NOT_HANDLED";

static void text_panic(const char *msg) {
    volatile uint16_t *vga = (uint16_t *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F20;
    const char *hdr = "*** STOP: THE WRLD OS kernel panic ***";
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
    draw_text(c, FONT_MONO, 40, y, panic_code, WHITE); y += 36;
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
    snprintf(vbuf, sizeof vbuf, "%s %s\r\nVersion %s (Build %s)\r\nKernel: " OS_ARCH_DESC ", cooperative scheduler\r\n", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD);
    vfs_write_file("C:\\WRLD\\version.txt", vbuf, strlen(vbuf));
    const char *dlls[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "msvcrt.dll", "advapi32.dll", "shell32.dll", "comctl32.dll", "ntdll.dll" };
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) {
        char p[128], body[128];
        snprintf(p, sizeof p, "C:\\WRLD\\System32\\%s", dlls[i]);
        snprintf(body, sizeof body, "Built-in %s: implemented natively by the WRLD Win32 subsystem.\r\n", dlls[i]);
        vnode_t *n = vfs_write_file(p, body, strlen(body)); if (n) n->readonly = true;
    }
}

/* progress while the installed system's files are read from the hard disk */
static void boot_load_progress(uint64_t done, uint64_t total) {
    char msg[96];
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct > 100) pct = 100;
    snprintf(msg, sizeof msg, "Loading files from the hard disk... %d%% (%u of %u MB)", pct,
             (uint32_t)(done >> 20), (uint32_t)((total + (1 << 20) - 1) >> 20));
    boot_screen_status(msg, 35 + pct * 24 / 100);
}

void kmain(uint32_t magic, uint32_t mbaddr) {
    serial_init();
#ifdef __x86_64__
    { extern void cpu_enable_avx(void); extern uint8_t fpu_xsave;
      cpu_enable_avx();                                /* before any interrupt saves FPU state */
      klog("cpu: %s", fpu_xsave ? "AVX on for programs (XSAVE)" : "no AVX: SSE state saved with FXSAVE"); }
#else
    { extern void cpu_enable_sse(void); extern uint8_t fpu_fxsr;
      cpu_enable_sse();                                /* before any interrupt saves FPU state */
      klog("cpu: %s", fpu_fxsr ? "SSE on for programs (FXSAVE)" : "no SSE: x87 state saved with FNSAVE"); }
#endif
    gdt_init();
    idt_init();
    panic_hook = text_panic;
    bootinfo_init(magic, mbaddr);
    if (!bootinfo.valid) panic("Not booted by a Multiboot 1 or 2 loader (magic %08x)", magic);
    mm_init();
    task_init();
    timer_init();
    sti();
    timer_check();
    klog("%s %s %s (build %s) starting", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD);
    klog("boot: %s, %s", bootinfo.from_mb2 ? "Multiboot 2" : "Multiboot 1",
         bootinfo.is_efi ? "EFI firmware" : "BIOS/legacy firmware");
    if (!video_boot_framebuffer())
        panic("No usable direct-RGB firmware framebuffer (flags=%d bpp=%d type=%d addr=%llx).\n"
              "Boot with GRUB using the supplied grub.cfg.", bootinfo.have_fb, bootinfo.fb_bpp,
              bootinfo.fb_type, (unsigned long long)bootinfo.fb_addr);
    klog("framebuffer %ux%ux%u at %llx pitch %u", fb_info.width, fb_info.height, fb_info.bpp, (unsigned long long)fb_info.physical, fb_info.pitch);
    klog("memory: %llu MB detected, heap %u MB", (unsigned long long)(mm_phys_total() / (1024 * 1024)), mm_total() / (1024 * 1024));
    gfx_init();
    gfx_ready = true;
    panic_hook = gfx_panic;
    fault_hook = on_fault;
    task_kill_hook = on_task_end;
    rtc_time_t now; rtc_read(&now);
    rand_seed(now.second * 1000 + now.minute * 60000 + timer_ticks());

    boot_screen_begin();
    uint64_t ram_mb = mm_phys_total() / (1024 * 1024);
    if (ram_mb < 64) {
        char w[96]; snprintf(w, sizeof w, "Low memory: %llu MB. Give the virtual machine 128 MB or more.", (unsigned long long)ram_mb);
        for (int i = 0; i < 6; i++) boot_screen_progress(w, 5);
    }
    boot_screen_progress("Initializing devices...", 10);
    keyboard_init();
    mouse_init(SCREEN_W, SCREEN_H);
    usb_init();
    char vendor[16], brand[52]; cpu_get_info(vendor, brand);
    klog("cpu: %s (%s)", brand, vendor);
    acpi_init();
    video_init();
    pci_dev_t devs[32]; int nd = pci_scan(devs, 32);
    for (int i = 0; i < nd; i++) klog("pci %02x:%02x.%x %04x:%04x %s", devs[i].bus, devs[i].dev, devs[i].fn, devs[i].vendor, devs[i].device, pci_class_name(devs[i].cls, devs[i].sub));
    boot_screen_progress("Starting network...", 20);
    net_init();
    boot_screen_progress("Starting sound...", 22);
    audio_init();
    boot_screen_progress("Detecting hard disks...", 25);
    storage_init();
    boot_screen_progress("Mounting file systems...", 35);
    vfs_init();
    vol_load_progress = boot_load_progress;
    if (boot_root_serial) {
        char err[256];
        if (!persist_boot_from_disk(boot_root_serial, err, sizeof err)) { panic_code = "INACCESSIBLE_BOOT_DEVICE"; panic("%s", err); }
    } else if (initrd_data) tar_extract(initrd_data, initrd_size, vfs_root('C'));
    else klog("initrd: none supplied");
    { extern void mm_release_initrd(void); mm_release_initrd(); }
    vol_load_progress = NULL;
    write_system_files();
    if (!system_volume) {   /* live session: offer installation */
        const char *lnk = "setup\r\n";
        vfs_write_file(USER_DESKTOP "\\Install " OS_NAME ".lnk", lnk, strlen(lnk));
    }
    boot_screen_progress("Starting Win32 subsystem...", 60);
    w32_init();
    boot_screen_progress("Loading personal settings...", 85);
    wm_init();
    shell_init();
    settings_load();
    persist_start();
    boot_screen_progress("Starting shell...", 100);
    boot_screen_end();
    if (boot_setup_mode && !system_volume) {
        if (bootinfo.cmdline && strstr(bootinfo.cmdline, "setup=text")) setup_text_mode(false);
        else if (SCREEN_W < 900 || SCREEN_H < 650) setup_text_mode(false);
        else setup_gui_mode(false);
    }
#ifdef WRLD_OOBE_TEST
    setup_oobe();                                    /* make EXTRA_CFLAGS=-DWRLD_OOBE_TEST: the first-run pages on every boot */
#else
    if (setup_oobe_pending()) setup_oobe();
#endif
    welcome_screen();
    wm_redraw_all();
    if (system_sounds) { char err[64]; audio_play_file(SOUND_STARTUP, err, sizeof err); }
    if (usb_unsupported && !usb_count())
        notify_balloon("USB", "This computer's USB controller has no driver yet. A PS/2 keyboard and mouse still work.");
    else if (system_volume) notify_balloon("Welcome to " OS_NAME, "Your files and settings are saved to the hard disk. Try the samples in C:\\Samples.");
    else notify_balloon("Welcome to " OS_NAME, "This is the live session (changes are lost on restart). Double-click \"Install " OS_NAME "\" on the desktop to install it to a hard disk.");
    devmgr_boot_check();                             /* "Found New Hardware" replaces the welcome balloon when there is some */
    shellmanager_start();                            /* shellmanager.exe: desktop, taskbar, sidebar and windows */
    for (;;) task_sleep(10000);                      /* System task stays alive; the shell task runs the GUI */
}
