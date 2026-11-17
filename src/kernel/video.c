/* Display drivers. GRUB starts us in a VESA linear framebuffer; these drivers
 * take over the adapter so the resolution can be changed at runtime:
 *  - Bochs/BGA "DISPI" interface: QEMU standard VGA (1234:1111), Bochs, and
 *    VirtualBox's VBoxVGA (80ee:beef)
 *  - VMware SVGA II (15ad:0405): VMware, and VirtualBox's VMSVGA. Screen
 *    updates are reported through the command FIFO.
 * Without a supported adapter the boot framebuffer is used as-is. */
#include "kernel.h"
#include "devmgr.h"
#include "gfx.h"
#include "gpu3d.h"
#include "bootinfo.h"
#include "basicfb.h"

bool video_boot_framebuffer(void) {
    basicfb_mode_t m = { bootinfo.fb_width, bootinfo.fb_height, bootinfo.fb_pitch, bootinfo.fb_bpp,
        bootinfo.fb_red_pos, bootinfo.fb_red_bits, bootinfo.fb_green_pos, bootinfo.fb_green_bits, bootinfo.fb_blue_pos, bootinfo.fb_blue_bits };
    const char *why = NULL;
    if (!bootinfo.have_fb || bootinfo.fb_type != 1 || !bootinfo.fb_rgb_valid || !basicfb_mode_valid(&m, &why)) {
        klog("basicdisplay: %s", why ? why : "firmware did not supply a direct RGB framebuffer"); return false;
    }
    bool wc = false;
    uintptr_t mapped = paging_map_framebuffer(bootinfo.fb_addr, m.pitch * m.height, &wc);
    if (!mapped) { klog("basicdisplay: framebuffer address cannot be mapped (x86 requires an aperture below 4 GiB)"); return false; }
    fb_info = (fb_info_t){ .addr = mapped, .pitch = m.pitch, .width = m.width, .height = m.height, .bpp = m.bpp,
        .physical = bootinfo.fb_addr, .red_pos = m.red_pos, .red_bits = m.red_bits, .green_pos = m.green_pos,
        .green_bits = m.green_bits, .blue_pos = m.blue_pos, .blue_bits = m.blue_bits, .write_combining = wc };
    klog("basicdisplay: %s %ux%ux%u pitch=%u physical=%llx cache=%s", bootinfo.is_efi ? "UEFI GOP" : "VESA",
        m.width, m.height, m.bpp, m.pitch, (unsigned long long)bootinfo.fb_addr, wc ? "PAT WC (PA1)" : "uncached");
    klog("basicdisplay: RGB masks %u:%u %u:%u %u:%u; native CPU rendering", m.red_pos, m.red_bits, m.green_pos, m.green_bits, m.blue_pos, m.blue_bits);
    return true;
}

/* ---- Bochs / BGA ---- */
#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
enum { D_ID, D_XRES, D_YRES, D_BPP, D_ENABLE, D_BANK, D_VIRT_W, D_VIRT_H, D_XOFF, D_YOFF, D_VRAM64K };
#define D_ENABLED 0x01
#define D_GETCAPS 0x02
#define D_LFB 0x40
static void dw(int i, uint16_t v) { outw(DISPI_INDEX, i); outw(DISPI_DATA, v); }
static uint16_t dr(int i) { outw(DISPI_INDEX, i); return inw(DISPI_DATA); }

/* ---- VMware SVGA II ---- */
enum { S_ID = 0, S_ENABLE = 1, S_WIDTH = 2, S_HEIGHT = 3, S_MAX_W = 4, S_MAX_H = 5, S_BPP = 7, S_BPL = 12, S_FB_START = 13, S_FB_OFFSET = 14,
       S_VRAM = 15, S_FB_SIZE = 16, S_CAPS = 17, S_MEM_START = 18, S_MEM_SIZE = 19, S_CONFIG_DONE = 20, S_SYNC = 21, S_BUSY = 22 };
enum { F_MIN, F_MAX, F_NEXT, F_STOP, F_BUSY = 290, F_NUM_REGS = 291 };
#define F_CAPS 4
#define F_FENCE 6
#define F_HWVERSION 7
#define F_RESERVED 14
/* "DEAD" means obsolete, not device-error status: FIFO slot 16 is arbitrary
 * host data and register 24 was the legacy cursor ID. Never use either to
 * reject commands. FIFO bounds and completed fences detect transport loss. */
#define F_HWVERSION_REVISED 17
static uint16_t svga_io;
static volatile uint32_t *fifo;
static uint32_t fifo_size;
static bool svga_on;
static uint32_t svga_caps, svga_fifo_caps, svga_hwversion;
static void sw(int r, uint32_t v) { outl(svga_io, r); outl(svga_io + 1, v); }
static uint32_t sr(int r) { outl(svga_io, r); return inl(svga_io + 1); }
bool video_svga_submit(const uint32_t *words, uint32_t nwords) {
    if (!svga_on || !words || !nwords || nwords > 65536) return false;
    uint32_t min = fifo[F_MIN], max = fifo[F_MAX], bytes = nwords * 4;
    if (min < F_NUM_REGS * 4 || (min & 3) || (max & 3) || max > fifo_size || max <= min + 4 || bytes > max - min - 4) return false;
    uint32_t start = timer_ticks();
    for (;;) {
        uint32_t next = fifo[F_NEXT], stop = fifo[F_STOP];
        if ((next & 3) || (stop & 3) || next < min || next >= max || stop < min || stop >= max) return false;
        uint32_t free = stop <= next ? max - next + stop - min : stop - next;
        if (bytes + 4 <= free) {
            if (svga_fifo_caps & (1u << 6)) fifo[F_RESERVED] = bytes;
            for (uint32_t i = 0; i < nwords; i++) { fifo[next / 4] = words[i]; next += 4; if (next == max) next = min; }
            __sync_synchronize();
            fifo[F_NEXT] = next;
            if (svga_fifo_caps & (1u << 6)) fifo[F_RESERVED] = 0;
            __sync_synchronize();
            sw(S_SYNC, 1);
            return true;
        }
        sw(S_SYNC, 1);
        if (timer_ticks() - start >= 250) return false;
        __asm__ volatile("pause");
    }
}

bool video_svga_info(uint32_t *caps, uint32_t *fifo_caps, uint32_t *hw_version) {
    if (!svga_io || !fifo) return false;
    svga_fifo_caps = fifo[F_CAPS];
    if (!svga_fifo_caps && svga_on && (svga_caps & 0x08000000u)) svga_fifo_caps = sr(68);
    svga_hwversion = fifo[(svga_fifo_caps & (1u << 8)) ? F_HWVERSION_REVISED : F_HWVERSION];
    if (!svga_hwversion) svga_hwversion = fifo[F_HWVERSION];
    if (caps) *caps = svga_caps;
    if (fifo_caps) *fifo_caps = svga_fifo_caps;
    if (hw_version) *hw_version = svga_hwversion;
    return true;
}
uint32_t video_svga_devcap(uint32_t cap) {
    if (!svga_io || !fifo) return 0;
    if (svga_caps & 0x08000000u) { sw(52, cap); return sr(52); }
    for (uint32_t at = 32; at < 288;) {
        uint32_t n = fifo[at];
        if (n < 2 || n > 288 - at) break;
        if (fifo[at + 1] == 0x100) for (uint32_t i = at + 2; i + 1 < at + n; i += 2) if (fifo[i] == cap) return fifo[i + 1];
        at += n;
    }
    return 0;
}
bool video_svga_fence(uint32_t value, uint32_t timeout_ms) {
    if (!(svga_fifo_caps & 1)) return false;
    uint32_t command[] = { 30, value };
    if (!video_svga_submit(command, 2)) return false;
    uint32_t start = timer_ticks();
    while ((int32_t)(fifo[F_FENCE] - value) < 0) {
        if (timer_ticks() - start >= timeout_ms) return false;
        __asm__ volatile("pause");
    }
    __sync_synchronize();
    return true;
}
bool video_svga_gmr(uint32_t id, uint32_t descriptor_ppn) {
    if (!(svga_caps & 0x00100000u) || id >= sr(43) || sr(44) < 2) return false;
    if (svga_caps & 0x00400000u) {
        const uint32_t *desc = (const uint32_t *)(uintptr_t)((uint64_t)descriptor_ppn << 12);
        uint32_t pages = desc[1];
        if (!pages || pages > 65530) return false;
        uint32_t define[] = { 41, id, pages };
        uint32_t *remap = kmalloc((5 + pages) * 4);
        if (!remap) return false;
        remap[0]=42; remap[1]=id; remap[2]=0; remap[3]=0; remap[4]=pages;
        for (uint32_t i=0;i<pages;i++) remap[5+i]=desc[0]+i;
        __sync_synchronize();
        bool ok=video_svga_submit(define, ARRAY_LEN(define)) && video_svga_submit(remap, 5+pages);
        kfree(remap); return ok;
    }
    __sync_synchronize(); sw(41, id); sw(42, descriptor_ppn); return true;
}
void video_svga_cpu_fallback(void) {
    if (svga_on && (svga_caps & 0x00200000u)) sw(45, 1); /* optional framebuffer dirty tracing */
}

enum { DRV_NONE, DRV_BGA, DRV_SVGA };
static int drv;
static char name[64] = "WRLD Basic Display Driver (firmware framebuffer)";
static uint32_t lfb, vram = 0, max_w = 0, max_h = 0;

static bool bga_match(const pci_dev_t *p) { return (p->vendor == 0x1234 && p->device == 0x1111) || (p->vendor == 0x80EE && p->device == 0xBEEF); }
static bool svga_match(const pci_dev_t *p) { return p->vendor == 0x15AD && p->device == 0x0405; }
DRIVER_PCI(drv_bga, "bga", "Bochs/QEMU and VirtualBox graphics (resolution changes)", DC_DISPLAY, bga_match);
DRIVER_PCI(drv_vmsvga, "vmsvga", "VMware SVGA II (resolution changes)", DC_DISPLAY, svga_match);
/* any other display adapter keeps the picture the firmware set up */
const driver_t drv_basic_display = { "wrldbasic", "WRLD Basic Display Driver (VESA / UEFI GOP, CPU rendering)", DC_DISPLAY, "firmware", NULL };
DRIVER_REGISTER(drv_basic_display);

static bool basic_requested(void) {
    const char *p = bootinfo.cmdline;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *end = p; while (*end && *end != ' ' && *end != '\t') end++;
        size_t n = end - p;
        if ((n == 11 && !memcmp(p, "video=basic", n)) || (n == 12 && !memcmp(p, "basicdisplay", n))) return true;
        p = end;
    }
    return false;
}
/* Firmware gives no PCI BDF. Match exact memory BAR bases without ever writing
 * BARs to size them; if ambiguous, keep a logical basic display rather than
 * claiming an arbitrary GPU or switching a different adapter's output. */
static const pci_dev_t *boot_adapter(pci_dev_t *devs, int n) {
    const pci_dev_t *sole = NULL, *match = NULL; int displays = 0, matches = 0;
    for (int i = 0; i < n; i++) if (devs[i].cls == 3) {
        const pci_dev_t *p = &devs[i]; sole = p; displays++;
        bool found = false;
        for (int bar = 0; bar < 6; bar++) {
            uint32_t lo = pci_cfg_read(p->bus, p->dev, p->fn, 0x10 + bar * 4);
            if (!lo || lo == ~0u || (lo & 1)) continue;
            uint64_t base = lo & ~0xFull;
            if ((lo & 6) == 4 && bar < 5) { base |= (uint64_t)pci_cfg_read(p->bus, p->dev, p->fn, 0x14 + bar * 4) << 32; bar++; }
            if (base && base == fb_info.physical) found = true;
        }
        if (found) { match = p; matches++; }
    }
    return matches == 1 ? match : !matches && displays == 1 ? sole : NULL;
}

void video_init(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    const pci_dev_t *boot = boot_adapter(devs, n);
    bool basic = basic_requested();
    for (int i = 0; i < n && drv == DRV_NONE; i++) {
        pci_dev_t *p = &devs[i];
        if (basic || p != boot) continue;
        if ((p->vendor == 0x1234 && p->device == 0x1111) || (p->vendor == 0x80EE && p->device == 0xBEEF)) {
            uint16_t id = dr(D_ID);
            if (id < 0xB0C0 || id > 0xB0CF) continue;
            lfb = pci_cfg_read(p->bus, p->dev, p->fn, 0x10) & ~0xFu;
            uint16_t en = dr(D_ENABLE);
            dw(D_ENABLE, en | D_GETCAPS);            /* while GETCAPS is set, XRES/YRES read back the maximum */
            max_w = dr(D_XRES); max_h = dr(D_YRES);
            dw(D_ENABLE, en);
            vram = (uint32_t)dr(D_VRAM64K) * 65536;
            if (!vram || vram > 512u * 1024 * 1024) vram = 16u * 1024 * 1024;
            if (!max_w || max_w > 8192) { max_w = 2560; max_h = 1600; }
            drv = DRV_BGA;
            snprintf(name, sizeof name, p->vendor == 0x80EE ? "VirtualBox Graphics Adapter (BGA %04x)" : "Standard VGA Graphics Adapter (Bochs BGA %04x)", id);
            dev_claim(p, &drv_bga, name);
        } else if (p->vendor == 0x15AD && p->device == 0x0405) {
            uint32_t bar0 = pci_cfg_read(p->bus, p->dev, p->fn, 0x10);
            if (!(bar0 & 1)) continue;
            pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (pci_cfg_read(p->bus, p->dev, p->fn, 0x04) & 0xFFFF) | 0x07);
            svga_io = bar0 & ~3u;
            sw(S_ID, 0x90000002);
            if (sr(S_ID) != 0x90000002) { klog("video: VMware SVGA II did not accept SVGA_ID_2"); continue; }
            lfb = sr(S_FB_START);
            vram = sr(S_VRAM);
            fifo = (volatile uint32_t *)(uintptr_t)(pci_cfg_read(p->bus, p->dev, p->fn, 0x18) & ~0xFu);
            paging_set_uncached((uint32_t)(uintptr_t)fifo);
            max_w = sr(S_MAX_W); max_h = sr(S_MAX_H);
            fifo_size = sr(S_MEM_SIZE);
            if (fifo_size < F_NUM_REGS * 4 + 4096 || fifo_size > 16u * 1024 * 1024) { fifo = NULL; continue; }
            for (uint32_t off = 0; off < fifo_size; off += 2u * 1024 * 1024) paging_set_uncached((uintptr_t)fifo + off);
            svga_caps = sr(S_CAPS); svga_fifo_caps = fifo[F_CAPS];
            svga_hwversion = fifo[(svga_fifo_caps & (1u << 8)) ? F_HWVERSION_REVISED : F_HWVERSION];
            /* Nothing else is touched until a mode is set: while the adapter stays
             * in VGA/VESA mode the host keeps showing the boot framebuffer. Saying
             * CONFIG_DONE now would make VMware wait for FIFO updates that a
             * VESA-mode desktop never sends - a screen that looks frozen. */
            drv = DRV_SVGA;
            snprintf(name, sizeof name, "VMware SVGA II Adapter");
            dev_claim(p, &drv_vmsvga, name);
        }
    }
    if (drv == DRV_NONE) {
        snprintf(name, sizeof name, "WRLD Basic Display Driver (%s)", bootinfo.is_efi ? "UEFI GOP" : "VESA");
        if (boot) { char nm[96]; snprintf(nm, sizeof nm, "WRLD Basic Display (%04x:%04x, %ux%u)", boot->vendor, boot->device, fb_info.width, fb_info.height); dev_claim(boot, &drv_basic_display, nm); }
        klog("basicdisplay: %s; firmware mode retained, no vendor GPU register writes", basic ? "explicit recovery selection" : "automatic firmware fallback");
    }
    if (drv == DRV_NONE) klog("video: %s, firmware mode %ux%u; VRAM size unknown; CPU rendering", name, fb_info.width, fb_info.height);
    else klog("video: %s, max %ux%u, %u MB video memory", name, max_w, max_h, vram >> 20);
    if (drv == DRV_SVGA && !boot_safe_mode) gpu3d_init();
}
bool video_svga_start(void) {
    if (drv != DRV_SVGA) return false;
    if (!svga_on) {
        if (!video_set_mode(fb_info.width, fb_info.height)) return false;
        gfx_refresh_framebuffer();
    }
    video_svga_info(NULL, NULL, NULL);
    return true;
}
const char *video_driver_name(void) { return name; }
bool video_can_set_mode(void) { return drv != DRV_NONE; }
bool video_mode_ok(int w, int h) {
    if (drv == DRV_NONE) return w == (int)fb_info.width && h == (int)fb_info.height;
    return w >= 1024 && h >= 768 && (uint32_t)w <= max_w && (uint32_t)h <= max_h && (uint64_t)w * h * 4 <= vram;
}
/* switch the hardware and update fb_info; the caller reallocates its buffers */
bool video_set_mode(int w, int h) {
    if (!video_mode_ok(w, h)) return false;
    if (drv == DRV_BGA) {
        dw(D_ENABLE, 0);
        dw(D_XRES, w); dw(D_YRES, h); dw(D_BPP, 32);
        dw(D_VIRT_W, w); dw(D_XOFF, 0); dw(D_YOFF, 0);
        dw(D_ENABLE, D_ENABLED | D_LFB);
        if (dr(D_XRES) != w || dr(D_YRES) != h) { klog("video: BGA refused %dx%d", w, h); return false; }
        fb_info.addr = lfb; fb_info.pitch = dr(D_VIRT_W) * 4;
    } else if (drv == DRV_SVGA) {
        sw(S_WIDTH, w); sw(S_HEIGHT, h); sw(S_BPP, 32);
        sw(S_ENABLE, 1);
        if (!svga_on) {                    /* VMware's order: enable, set up the FIFO, then CONFIG_DONE */
            fifo[F_MIN] = F_NUM_REGS * 4; fifo[F_MAX] = fifo_size;
            fifo[F_NEXT] = F_NUM_REGS * 4; fifo[F_STOP] = F_NUM_REGS * 4;
            fifo[F_FENCE] = 0;
            fifo[F_RESERVED] = 0;
            if (svga_caps & 0x4000) fifo[288] = 0x00020001; /* negotiate 3D before CONFIG_DONE */
            __sync_synchronize();
            sw(S_CONFIG_DONE, 1);
        }
        svga_on = true;
        video_svga_info(NULL, NULL, NULL);   /* also refresh after an ordinary 2D mode change */
        fb_info.addr = sr(S_FB_START) + sr(S_FB_OFFSET); fb_info.pitch = sr(S_BPL);
    } else return false;
    fb_info.physical = fb_info.addr;
    basicfb_mode_t mode = { w, h, fb_info.pitch, 32, 16, 8, 8, 8, 0, 8 };
    if (!basicfb_mode_valid(&mode, NULL)) return false;
    bool wc = false; uintptr_t mapped = paging_map_framebuffer(fb_info.physical, mode.pitch * mode.height, &wc);
    if (!mapped) return false;
    fb_info.addr = mapped; fb_info.write_combining = wc;
    fb_info.width = w; fb_info.height = h; fb_info.bpp = 32;
    fb_info.red_pos = 16; fb_info.green_pos = 8; fb_info.blue_pos = 0;
    fb_info.red_bits = fb_info.green_bits = fb_info.blue_bits = 8;
    klog("video: mode %dx%d, pitch %u", w, h, fb_info.pitch);
    return true;
}
/* tell the adapter which part of the framebuffer changed (only SVGA needs this) */
void video_update(int x, int y, int w, int h) {
    if (!svga_on || w <= 0 || h <= 0) return;
    uint32_t update[] = { 1, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    video_svga_submit(update, 5);
}
