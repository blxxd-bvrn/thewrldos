/* ACPI power management: finds the FADT and the \_S5_ (soft-off) sleep type in
 * the DSDT so the machine really powers off on any ACPI PC or hypervisor
 * (QEMU, VirtualBox, VMware, Hyper-V, real hardware). Also implements the
 * ACPI reset register for restarts. All of physical memory below 4 GiB is
 * identity-mapped, so the tables can be read in place. */
#include "kernel.h"

typedef struct PACKED { char sig[8]; uint8_t sum; char oem[6]; uint8_t rev; uint32_t rsdt; uint32_t len; uint64_t xsdt; uint8_t xsum; uint8_t r[3]; } rsdp_t;
typedef struct PACKED { char sig[4]; uint32_t len; uint8_t rev, sum; char oem[6], oemtab[8]; uint32_t oemrev, creator, crev; } sdt_t;
typedef struct PACKED { uint8_t space, width, offset, size; uint64_t addr; } gas_t;

static bool acpi_ok;
static uint32_t smi_cmd, pm1a_cnt, pm1b_cnt;
static uint8_t acpi_enable_val, s5a, s5b;
static bool have_s5, have_reset;
static gas_t reset_reg;
static uint8_t reset_val;
static char acpi_oem[7];

static bool checksum(const void *p, uint32_t n) { uint8_t s = 0; for (uint32_t i = 0; i < n; i++) s += ((const uint8_t *)p)[i]; return s == 0; }
static rsdp_t *find_rsdp(void) {
    uintptr_t bda = 0x40E;
    __asm__("" : "+r"(bda));                       /* BIOS data area: EBDA segment */
    uint32_t ebda = (uint32_t)(*(volatile uint16_t *)bda) << 4;
    uint32_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
    for (int r = 0; r < 2; r++) {
        if (!ranges[r][0] || ranges[r][0] >= 0x100000) continue;
        for (uint32_t a = ranges[r][0] & ~15u; a < ranges[r][1]; a += 16)
            if (!memcmp((void *)a, "RSD PTR ", 8) && checksum((void *)a, 20)) return (rsdp_t *)a;
    }
    return NULL;
}
static bool table_ok(uint32_t a) { return a && a < 0xFFFF0000u && ((sdt_t *)a)->len >= sizeof(sdt_t) && ((sdt_t *)a)->len < 0x1000000; }

/* Find "_S5_" Name(Package{SLP_TYPa, SLP_TYPb, ...}) in AML */
static bool parse_s5(const uint8_t *aml, uint32_t len) {
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4)) continue;
        if (!(i >= 1 && (aml[i - 1] == 0x08 || (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\')))) continue;
        const uint8_t *p = aml + i + 4;
        if (*p++ != 0x12) continue;                  /* PackageOp */
        p += ((*p & 0xC0) >> 6) + 1;                 /* PkgLength */
        p++;                                         /* NumElements */
        uint8_t v[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) { p++; v[k] = *p++; }    /* BytePrefix */
            else if (*p == 0x00 || *p == 0x01) v[k] = *p++;   /* ZeroOp / OneOp */
            else if (*p == 0xFF) { v[k] = 0; p++; }  /* OnesOp */
            else v[k] = *p++;
        }
        s5a = v[0] & 7; s5b = v[1] & 7;
        return true;
    }
    return false;
}

void acpi_init(void) {
    rsdp_t *rsdp = find_rsdp();
    if (!rsdp) { klog("acpi: no RSDP found (no ACPI)"); return; }
    memcpy(acpi_oem, rsdp->oem, 6); acpi_oem[6] = 0;
    bool x = rsdp->rev >= 2 && rsdp->xsdt && rsdp->xsdt < 0xFFFFFFFFull;
    uint32_t root = x ? (uint32_t)rsdp->xsdt : rsdp->rsdt;
    if (!table_ok(root)) { klog("acpi: bad root table"); return; }
    sdt_t *rt = (sdt_t *)root;
    int n = (rt->len - sizeof(sdt_t)) / (x ? 8 : 4);
    uint8_t *fadt = NULL;
    for (int i = 0; i < n; i++) {
        uint32_t a = x ? (uint32_t)((uint64_t *)(rt + 1))[i] : ((uint32_t *)(rt + 1))[i];
        if (table_ok(a) && !memcmp(((sdt_t *)a)->sig, "FACP", 4)) { fadt = (uint8_t *)a; break; }
    }
    if (!fadt) { klog("acpi: no FADT"); return; }
    uint32_t flen = ((sdt_t *)fadt)->len;
    uint32_t dsdt = *(uint32_t *)(fadt + 40);
    if (flen >= 148 && *(uint64_t *)(fadt + 140) && *(uint64_t *)(fadt + 140) < 0xFFFFFFFFull) dsdt = (uint32_t)*(uint64_t *)(fadt + 140);
    smi_cmd = *(uint32_t *)(fadt + 48);
    acpi_enable_val = fadt[52];
    pm1a_cnt = *(uint32_t *)(fadt + 64);
    pm1b_cnt = *(uint32_t *)(fadt + 68);
    if (flen >= 129 && (*(uint32_t *)(fadt + 112) & (1 << 10))) {
        memcpy(&reset_reg, fadt + 116, sizeof reset_reg); reset_val = fadt[128];
        have_reset = reset_reg.addr && (reset_reg.space == 0 || reset_reg.space == 1);
    }
    if (table_ok(dsdt) && !memcmp(((sdt_t *)dsdt)->sig, "DSDT", 4))
        have_s5 = parse_s5((uint8_t *)dsdt + sizeof(sdt_t), ((sdt_t *)dsdt)->len - sizeof(sdt_t));
    acpi_ok = pm1a_cnt != 0;
    klog("acpi: %s rev %d, PM1a_CNT=%x PM1b_CNT=%x SMI_CMD=%x, S5 %s (%d/%d), reset reg %s",
         acpi_oem, rsdp->rev, pm1a_cnt, pm1b_cnt, smi_cmd, have_s5 ? "found" : "missing", s5a, s5b, have_reset ? "yes" : "no");
}
const char *acpi_describe(void) {
    static char b[96];
    if (!acpi_ok) return "Not available";
    snprintf(b, sizeof b, "ACPI (%s), soft-off %s", acpi_oem, have_s5 ? "supported" : "not described");
    return b;
}

static void acpi_enable(void) {
    if ((inw(pm1a_cnt) & 1) || !smi_cmd || !acpi_enable_val) return;   /* SCI_EN already set, or hardware-reduced */
    outb(smi_cmd, acpi_enable_val);
    for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++) sleep_ms(10);
}
static void spin_ms(uint32_t ms) { for (uint32_t i = 0; i < ms * 1000; i++) io_wait(); }   /* works with interrupts off */

bool acpi_poweroff(void) {
    if (acpi_ok && have_s5) {
        acpi_enable();
        cli();
        outw(pm1a_cnt, (s5a << 10) | (1 << 13));
        if (pm1b_cnt) outw(pm1b_cnt, (s5b << 10) | (1 << 13));
        spin_ms(500);
    }
    cli();
    /* well-known emulator ports, in case the tables were unusable */
    outw(0x604, 0x2000);    /* QEMU (PIIX4 / ICH9) */
    outw(0xB004, 0x2000);   /* Bochs, old QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    outw(0x600, 0x34);      /* Cloud Hypervisor */
    spin_ms(500);
    sti();
    return false;           /* still running: power-off not supported */
}
void acpi_reset(void) {
    if (acpi_ok && have_reset) {
        if (reset_reg.space == 1) outb((uint16_t)reset_reg.addr, reset_val);
        else *(volatile uint8_t *)(uint32_t)reset_reg.addr = reset_val;
        spin_ms(200);
    }
    /* PCI reset control register (most chipsets) */
    outb(0xCF9, 0x02); io_wait(); outb(0xCF9, 0x06);
    spin_ms(200);
}
