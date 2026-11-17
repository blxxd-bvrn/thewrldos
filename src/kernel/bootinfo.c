/* Normalise Multiboot 1 and Multiboot 2 boot information.
 *
 * Multiboot 2 is what GRUB uses on UEFI: it can carry a 64-bit framebuffer
 * address, the ACPI RSDP (which EFI firmware may keep only in its
 * configuration table) and a note that the firmware is EFI.  Multiboot 1
 * stays supported, so an older GRUB or a hand-written grub.cfg still boots. */
#include "kernel.h"
#include "bootinfo.h"

bootinfo_t bootinfo;

#define MB1_MAGIC 0x2BADB002
#define MB2_MAGIC 0x36D76289

typedef struct PACKED {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr, drives_length, drives_addr, config_table, boot_loader_name, apm_table;
    uint32_t vbe_control_info, vbe_mode_info; uint16_t vbe_mode, vbe_if_seg, vbe_if_off, vbe_if_len;
    uint64_t fb_addr; uint32_t fb_pitch, fb_width, fb_height; uint8_t fb_bpp, fb_type;
} mb1_info_t;

typedef struct PACKED { uint32_t size; uint64_t addr, len; uint32_t type; } mb1_mmap_t;
typedef struct PACKED { uint32_t start, end, cmdline, pad; } mb1_mod_t;

typedef struct PACKED { uint32_t type, size; } mb2_tag_t;
typedef struct PACKED { uint32_t type, size; uint32_t start, end; } mb2_mod_t;
typedef struct PACKED { uint32_t type, size; uint64_t addr; uint32_t pitch, width, height; uint8_t bpp, fb_type; uint16_t reserved; } mb2_fb_t;
typedef struct PACKED { uint64_t addr, len; uint32_t type, zero; } mb2_mmap_t;
typedef struct PACKED { uint32_t type, size, entry_size, version; } mb2_mmap_tag_t;

static void copy_cmdline(const char **dst, const char *src) {
    if (!src) return;
    if ((uintptr_t)src >= 0xFFFFFFFFu) return;      /* not addressable in the identity map */
    *dst = src;
}
static void framebuffer_rgb(const uint8_t *p) {
    bootinfo.fb_red_pos = p[0]; bootinfo.fb_red_bits = p[1];
    bootinfo.fb_green_pos = p[2]; bootinfo.fb_green_bits = p[3];
    bootinfo.fb_blue_pos = p[4]; bootinfo.fb_blue_bits = p[5];
    bootinfo.fb_rgb_valid = true;
}

static void parse_mb1(uintptr_t addr) {
    const mb1_info_t *mb = (const mb1_info_t *)addr;
    bootinfo.valid = true;

    if (mb->flags & (1u << 2)) copy_cmdline(&bootinfo.cmdline, (const char *)(uintptr_t)mb->cmdline);
    if (mb->flags & (1u << 9)) copy_cmdline(&bootinfo.loader_name, (const char *)(uintptr_t)mb->boot_loader_name);
    if (mb->flags & 1u) {
        bootinfo.have_mem = true;
        bootinfo.mem_lower = mb->mem_lower;
        bootinfo.mem_upper = mb->mem_upper;
    }

    if ((mb->flags & (1u << 6)) && mb->mmap_addr && mb->mmap_length < 0x100000) {
        for (uint32_t p = mb->mmap_addr; p + sizeof(mb1_mmap_t) <= mb->mmap_addr + mb->mmap_length &&
             bootinfo.mmap_count < BOOTINFO_MAX_MMAP; ) {
            const mb1_mmap_t *e = (const mb1_mmap_t *)(uintptr_t)p;
            if (e->size < sizeof(mb1_mmap_t) - 4) break;
            boot_mmap_t *o = &bootinfo.mmap[bootinfo.mmap_count++];
            o->addr = e->addr; o->len = e->len; o->type = e->type;
            p += e->size + 4;
        }
    }

    if ((mb->flags & (1u << 3)) && mb->mods_count && mb->mods_addr && mb->mods_count < 64) {
        const mb1_mod_t *m = (const mb1_mod_t *)(uintptr_t)mb->mods_addr;
        for (uint32_t i = 0; i < mb->mods_count && bootinfo.module_count < BOOTINFO_MAX_MODULES; i++) {
            boot_module_t *o = &bootinfo.modules[bootinfo.module_count++];
            o->start = m[i].start; o->end = m[i].end;
            copy_cmdline(&o->cmdline, (const char *)(uintptr_t)m[i].cmdline);
        }
    }

    if (mb->flags & (1u << 12)) {
        bootinfo.have_fb = true;
        bootinfo.fb_addr = mb->fb_addr;
        bootinfo.fb_pitch = mb->fb_pitch;
        bootinfo.fb_width = mb->fb_width;
        bootinfo.fb_height = mb->fb_height;
        bootinfo.fb_bpp = mb->fb_bpp;
        bootinfo.fb_type = mb->fb_type;
        if (mb->fb_type == 1) framebuffer_rgb((const uint8_t *)mb + sizeof *mb);
    }
}

static void parse_mb2(uintptr_t addr) {
    const uint32_t *hdr = (const uint32_t *)addr;
    uint32_t total = hdr[0];
    if (total < 16 || total > 16 * 1024 * 1024) return;      /* not a sane info block */
    bootinfo.valid = true;
    bootinfo.from_mb2 = true;

    uintptr_t p = addr + 8, end = addr + total;
    while (p + sizeof(mb2_tag_t) <= end) {
        const mb2_tag_t *t = (const mb2_tag_t *)p;
        if (t->type == 0 || t->size < 8) break;
        uintptr_t next = p + ((t->size + 7) & ~7u);
        if (next <= p || next > end) break;

        switch (t->type) {
        case 1:                                                     /* command line */
            copy_cmdline(&bootinfo.cmdline, (const char *)p + 8);
            break;
        case 2:                                                     /* boot loader name */
            copy_cmdline(&bootinfo.loader_name, (const char *)p + 8);
            break;
        case 3: {                                                   /* module */
            const mb2_mod_t *m = (const mb2_mod_t *)p;
            if (t->size >= sizeof *m && bootinfo.module_count < BOOTINFO_MAX_MODULES) {
                boot_module_t *o = &bootinfo.modules[bootinfo.module_count++];
                o->start = m->start; o->end = m->end;
                copy_cmdline(&o->cmdline, (const char *)p + 16);
            }
            break;
        }
        case 4:                                                     /* basic memory */
            if (t->size >= 16) {
                bootinfo.have_mem = true;
                bootinfo.mem_lower = *(const uint32_t *)(p + 8);
                bootinfo.mem_upper = *(const uint32_t *)(p + 12);
            }
            break;
        case 6: {                                                   /* full memory map */
            const mb2_mmap_tag_t *mt = (const mb2_mmap_tag_t *)p;
            if (t->size >= 16 && mt->entry_size >= sizeof(mb2_mmap_t)) {
                uintptr_t q = p + 16, qend = p + t->size;
                while (q + mt->entry_size <= qend && bootinfo.mmap_count < BOOTINFO_MAX_MMAP) {
                    const mb2_mmap_t *e = (const mb2_mmap_t *)q;
                    boot_mmap_t *o = &bootinfo.mmap[bootinfo.mmap_count++];
                    o->addr = e->addr; o->len = e->len; o->type = e->type;
                    q += mt->entry_size;
                }
            }
            break;
        }
        case 8: {                                                   /* framebuffer */
            const mb2_fb_t *fb = (const mb2_fb_t *)p;
            if (t->size >= sizeof *fb) {
                bootinfo.fb_rgb_valid = false;
                bootinfo.have_fb = true;
                bootinfo.fb_addr = fb->addr;
                bootinfo.fb_pitch = fb->pitch;
                bootinfo.fb_width = fb->width;
                bootinfo.fb_height = fb->height;
                bootinfo.fb_bpp = fb->bpp;
                bootinfo.fb_type = fb->fb_type;
                if (fb->fb_type == 1 && t->size >= sizeof *fb + 6) framebuffer_rgb((const uint8_t *)fb + sizeof *fb);
            }
            break;
        }
        case 11:                                                    /* EFI32 system table */
            if (t->size >= 12) { bootinfo.is_efi = true; bootinfo.efi_system_table = *(const uint32_t *)(p + 8); }
            break;
        case 12:                                                    /* EFI64 system table */
            if (t->size >= 16) { bootinfo.is_efi = true; bootinfo.efi_system_table = *(const uint64_t *)(p + 8); }
            break;
        case 18:                                                    /* EFI boot services not terminated */
            bootinfo.is_efi = true;
            break;
        case 14:                                                    /* ACPI 1.0 RSDP (copy) */
            if (t->size >= 8 + 20 && !memcmp((const void *)(p + 8), "RSD PTR ", 8) && bootinfo.acpi_rsdp_len < 36) {
                memcpy(bootinfo.acpi_rsdp_data, (const void *)(p + 8), 20);
                bootinfo.acpi_rsdp_len = 20;
                bootinfo.acpi_rsdp = (uintptr_t)bootinfo.acpi_rsdp_data;
            }
            break;
        case 15:                                                    /* ACPI 2.0+ RSDP (copy) */
            if (t->size >= 8 + 36 && !memcmp((const void *)(p + 8), "RSD PTR ", 8)) {
                memcpy(bootinfo.acpi_rsdp_data, (const void *)(p + 8), 36);
                bootinfo.acpi_rsdp_len = 36;
                bootinfo.acpi_rsdp = (uintptr_t)bootinfo.acpi_rsdp_data;
            }
            break;
        default:
            break;
        }
        p = next;
    }
}

void bootinfo_init(uint32_t magic, uint32_t addr) {
    memset(&bootinfo, 0, sizeof bootinfo);
    if (!addr) return;
    if (magic == MB1_MAGIC) parse_mb1(addr);
    else if (magic == MB2_MAGIC) parse_mb2(addr);
}
