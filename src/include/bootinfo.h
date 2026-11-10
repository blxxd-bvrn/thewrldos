#ifndef BOOTINFO_H
#define BOOTINFO_H
#include "types.h"

/* What the boot loader told us, in one shape whichever protocol it used.
 *
 * The kernel accepts both Multiboot 1 and Multiboot 2.  GRUB's BIOS boot path
 * uses either; its EFI path uses Multiboot 2, and through it we also learn
 * where the ACPI tables are (EFI firmware does not have to leave an RSDP in
 * the legacy 0xE0000..0xFFFFF window) and that the firmware is EFI. */

#define BOOTINFO_MAX_MODULES 8
#define BOOTINFO_MAX_MMAP    192

typedef struct {
    uint64_t addr, len;
    uint32_t type;                      /* 1 = available RAM */
} boot_mmap_t;

typedef struct {
    uint64_t start, end;
    const char *cmdline;
} boot_module_t;

typedef struct {
    bool valid;
    bool from_mb2;
    bool is_efi;

    const char *cmdline;
    const char *loader_name;

    bool have_mem;
    uint32_t mem_lower, mem_upper;      /* KiB */

    int mmap_count;
    boot_mmap_t mmap[BOOTINFO_MAX_MMAP];

    int module_count;
    boot_module_t modules[BOOTINFO_MAX_MODULES];

    bool have_fb;
    uint64_t fb_addr;
    uint32_t fb_pitch, fb_width, fb_height;
    uint8_t fb_bpp, fb_type;

    uint64_t acpi_rsdp;                 /* address of the RSDP, 0 if absent */
    uint8_t acpi_rsdp_data[36];         /* a copy, so later heap use cannot overwrite it */
    uint8_t acpi_rsdp_len;
    uint64_t efi_system_table;
} bootinfo_t;

extern bootinfo_t bootinfo;

void bootinfo_init(uint32_t magic, uint32_t addr);

#endif
