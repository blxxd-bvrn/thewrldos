/* Display drivers. GRUB starts us in a VESA linear framebuffer; these drivers
 * take over the adapter so the resolution can be changed at runtime:
 *  - Bochs/BGA "DISPI" interface: QEMU standard VGA (1234:1111), Bochs, and
 *    VirtualBox's VBoxVGA (80ee:beef)
 *  - VMware SVGA II (15ad:0405): VMware, and VirtualBox's VMSVGA. Screen
 *    updates are reported through the command FIFO.
 * Without a supported adapter the boot framebuffer is used as-is. */
#include "kernel.h"

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
enum { F_MIN, F_MAX, F_NEXT, F_STOP };
static uint16_t svga_io;
static volatile uint32_t *fifo;
static bool svga_on;
static void sw(int r, uint32_t v) { outl(svga_io, r); outl(svga_io + 1, v); }
static uint32_t sr(int r) { outl(svga_io, r); return inl(svga_io + 1); }
static void fifo_sync(void) { sw(S_SYNC, 1); for (int i = 0; i < 1000000 && sr(S_BUSY); i++) {} }
static void fifo_put(uint32_t v) {
    uint32_t next = fifo[F_NEXT], n2 = next + 4 == fifo[F_MAX] ? fifo[F_MIN] : next + 4;
    if (n2 == fifo[F_STOP]) fifo_sync();
    fifo[next / 4] = v;
    fifo[F_NEXT] = n2;
}

enum { DRV_NONE, DRV_BGA, DRV_SVGA };
static int drv;
static char name[64] = "Standard VESA display (boot framebuffer)";
static uint32_t lfb, vram = 0, max_w = 0, max_h = 0;

void video_init(void) {
    pci_dev_t devs[48];
    int n = pci_scan(devs, 48);
    for (int i = 0; i < n && drv == DRV_NONE; i++) {
        pci_dev_t *p = &devs[i];
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
        } else if (p->vendor == 0x15AD && p->device == 0x0405) {
            uint32_t bar0 = pci_cfg_read(p->bus, p->dev, p->fn, 0x10);
            if (!(bar0 & 1)) continue;
            pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (pci_cfg_read(p->bus, p->dev, p->fn, 0x04) & 0xFFFF) | 0x07);
            svga_io = bar0 & ~3u;
            sw(S_ID, 0x90000002);
            if (sr(S_ID) != 0x90000002) { klog("video: VMware SVGA II did not accept SVGA_ID_2"); continue; }
            lfb = sr(S_FB_START);
            vram = sr(S_VRAM);
            fifo = (volatile uint32_t *)(pci_cfg_read(p->bus, p->dev, p->fn, 0x18) & ~0xFu);
            paging_set_uncached((uint32_t)fifo);
            max_w = sr(S_MAX_W); max_h = sr(S_MAX_H);
            uint32_t fsize = sr(S_MEM_SIZE);
            fifo[F_MIN] = 293 * 4; fifo[F_MAX] = fsize; fifo[F_NEXT] = 293 * 4; fifo[F_STOP] = 293 * 4;
            sw(S_CONFIG_DONE, 1);
            drv = DRV_SVGA;
            snprintf(name, sizeof name, "VMware SVGA II Adapter");
        }
    }
    klog("video: %s, max %ux%u, %u MB video memory", name, max_w, max_h, vram >> 20);
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
        svga_on = true;
        fb_info.addr = sr(S_FB_START) + sr(S_FB_OFFSET); fb_info.pitch = sr(S_BPL);
    } else return false;
    fb_info.width = w; fb_info.height = h; fb_info.bpp = 32;
    klog("video: mode %dx%d, pitch %u", w, h, fb_info.pitch);
    return true;
}
/* tell the adapter which part of the framebuffer changed (only SVGA needs this) */
void video_update(int x, int y, int w, int h) {
    if (!svga_on || w <= 0 || h <= 0) return;
    fifo_put(1); fifo_put(x); fifo_put(y); fifo_put(w); fifo_put(h);   /* SVGA_CMD_UPDATE */
}
