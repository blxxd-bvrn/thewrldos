/* Device manager: what hardware the computer has, which built-in driver
 * matches each device, and whether that driver started. See devmgr.h.
 *
 * The list is rebuilt from the sources of truth each time it is asked for:
 * PCI configuration space, the drivers' claims, the block layer, the USB
 * stack and a few probes of the fixed PC devices. */
#include "kernel.h"
#include "devmgr.h"
#include "disk.h"
#include "fs.h"
#include "net.h"
#include "audio.h"
#include "usb.h"
#include "bootinfo.h"
#include "wm.h"

/* ---------------- the driver list ---------------- */
extern const driver_t *const drivers_start[], *const drivers_end[];   /* collected by the linker script */
int devmgr_drivers(const driver_t *const **out) { *out = drivers_start; return (int)(drivers_end - drivers_start); }

/* drivers for devices that are not on the PCI bus */
static const driver_t drv_i8042kbd = { "i8042kbd", "PS/2 keyboard", DC_KEYBOARD, "ISA", NULL }; DRIVER_REGISTER(drv_i8042kbd);
static const driver_t drv_i8042mou = { "i8042mou", "PS/2 mouse (3-button and wheel)", DC_MOUSE, "ISA", NULL }; DRIVER_REGISTER(drv_i8042mou);
static const driver_t drv_pit = { "pit", "System timer (8253/8254)", DC_SYSTEM, "ISA", NULL }; DRIVER_REGISTER(drv_pit);
static const driver_t drv_rtc = { "rtc", "CMOS real-time clock", DC_SYSTEM, "ISA", NULL }; DRIVER_REGISTER(drv_rtc);
static const driver_t drv_serial = { "serial", "16550 serial port (kernel log)", DC_PORTS, "ISA", NULL }; DRIVER_REGISTER(drv_serial);
static const driver_t drv_pcspk = { "pcspk", "PC speaker", DC_SOUND, "ISA", NULL }; DRIVER_REGISTER(drv_pcspk);
static const driver_t drv_acpi = { "acpi", "ACPI power management (power off, restart)", DC_SYSTEM, "ACPI", NULL }; DRIVER_REGISTER(drv_acpi);
static const driver_t drv_sb16 = { "sb16", "Sound Blaster 16", DC_SOUND, "ISA", NULL }; DRIVER_REGISTER(drv_sb16);
static const driver_t drv_wss = { "wss", "Windows Sound System (Crystal CS4231A, AD1848)", DC_SOUND, "ISA", NULL }; DRIVER_REGISTER(drv_wss);
static const driver_t drv_disk = { "disk", "Hard disks (IDE, SATA, NVMe, SCSI, USB)", DC_DISK, "storage", NULL }; DRIVER_REGISTER(drv_disk);
#ifdef __x86_64__
static const driver_t drv_usbhub = { "usbhub", "USB hubs", DC_USBDEV, "USB", NULL }; DRIVER_REGISTER(drv_usbhub);
static const driver_t drv_usbhid = { "usbhid", "USB keyboards and mice (HID)", DC_KEYBOARD, "USB", NULL }; DRIVER_REGISTER(drv_usbhid);
static const driver_t drv_usbstor = { "usbstor", "USB mass storage (bulk-only)", DC_USBDEV, "USB", NULL }; DRIVER_REGISTER(drv_usbstor);
static const driver_t drv_usbaudio = { "usbaudio", "USB audio class 1 and 2 playback", DC_SOUND, "USB", NULL }; DRIVER_REGISTER(drv_usbaudio);
#endif

const char *devclass_name(int cls) {
    static const char *n[DC_COUNT] = { "Computer", "Processors", "Display adapters", "Storage controllers", "Disk drives",
        "Network adapters", "Sound, video and game controllers", "Universal Serial Bus controllers", "USB devices",
        "Keyboards", "Mice and other pointing devices", "Ports (COM)", "System devices", "Other devices" };
    return cls >= 0 && cls < DC_COUNT ? n[cls] : "Other devices";
}

/* ---------------- claims ---------------- */
#define MAX_CLAIMS 64
static struct { uint8_t bus, dev, fn; const driver_t *drv; char name[72]; } claims[MAX_CLAIMS];
static int nclaims;
void dev_claim(const pci_dev_t *p, const driver_t *drv, const char *name) {
    int i;
    for (i = 0; i < nclaims; i++) if (claims[i].bus == p->bus && claims[i].dev == p->dev && claims[i].fn == p->fn) break;
    if (i == nclaims) { if (nclaims == MAX_CLAIMS) return; nclaims++; }
    claims[i].bus = p->bus; claims[i].dev = p->dev; claims[i].fn = p->fn; claims[i].drv = drv;
    strlcpy(claims[i].name, name && *name ? name : drv->desc, sizeof claims[i].name);
}
static int claim_find(const pci_dev_t *p) {
    for (int i = 0; i < nclaims; i++) if (claims[i].bus == p->bus && claims[i].dev == p->dev && claims[i].fn == p->fn) return i;
    return -1;
}
static bool class_claimed(int cls) { for (int i = 0; i < nclaims; i++) if (claims[i].drv->cls == cls) return true; return false; }

/* ---------------- names for hardware nothing drives ---------------- */
static const char *vendor_name(uint16_t v) {
    static const struct { uint16_t id; const char *n; } t[] = {
        { 0x8086, "Intel" }, { 0x1022, "AMD" }, { 0x1002, "AMD/ATI" }, { 0x10DE, "NVIDIA" }, { 0x10EC, "Realtek" },
        { 0x14E4, "Broadcom" }, { 0x168C, "Qualcomm Atheros" }, { 0x17CB, "Qualcomm" }, { 0x14C3, "MediaTek" },
        { 0x1814, "Ralink" }, { 0x11AB, "Marvell" }, { 0x1B4B, "Marvell" }, { 0x1969, "Qualcomm Atheros" },
        { 0x1106, "VIA" }, { 0x1039, "SiS" }, { 0x10B9, "ALi" }, { 0x1B21, "ASMedia" }, { 0x1912, "Renesas" },
        { 0x1033, "NEC" }, { 0x104C, "Texas Instruments" }, { 0x1180, "Ricoh" }, { 0x197B, "JMicron" },
        { 0x1217, "O2 Micro" }, { 0x10B7, "3Com" }, { 0x1000, "LSI Logic" }, { 0x9005, "Adaptec" }, { 0x1077, "QLogic" },
        { 0x15B3, "Mellanox" }, { 0x144D, "Samsung" }, { 0x15B7, "SanDisk" }, { 0x1C5C, "SK hynix" }, { 0x1E0F, "Kioxia" },
        { 0x1987, "Phison" }, { 0x126F, "Silicon Motion" }, { 0xC0A9, "Micron" }, { 0x1344, "Micron" }, { 0x2646, "Kingston" },
        { 0x1179, "Toshiba" }, { 0x1102, "Creative Labs" }, { 0x1274, "Ensoniq" }, { 0x13F6, "C-Media" }, { 0x1013, "Cirrus Logic" },
        { 0x5333, "S3" }, { 0x102B, "Matrox" }, { 0x1A03, "ASPEED" }, { 0x1234, "QEMU" }, { 0x1B36, "Red Hat (QEMU)" },
        { 0x1AF4, "Red Hat (VirtIO)" }, { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" }, { 0x1414, "Microsoft (Hyper-V)" },
        { 0x1AB8, "Parallels" }, { 0x106B, "Apple" }, { 0x8087, "Intel" }, { 0x1D6A, "Aquantia" }, { 0x1FC9, "Tehuti" }, { 0x1011, "DEC" }, { 0x10B5, "PLX" }, { 0x1283, "ITE" } };
    for (unsigned i = 0; i < ARRAY_LEN(t); i++) if (t[i].id == v) return t[i].n;
    return NULL;
}
/* what kind of device a PCI class code describes, and where it belongs in the tree */
static const char *pci_kind(const pci_dev_t *p, int *cls, bool *builtin) {
    *builtin = false; *cls = DC_OTHER;
    switch (p->cls) {
    case 0x00: return p->sub == 1 ? "VGA-compatible device" : "Device";
    case 0x01: *cls = DC_STORAGE;
        switch (p->sub) { case 0: return "SCSI controller"; case 1: return "IDE controller"; case 2: return "Floppy controller";
        case 4: return "RAID controller"; case 5: return "ATA controller"; case 6: return p->prog == 1 ? "SATA AHCI controller" : "SATA controller";
        case 7: return "SAS controller"; case 8: return p->prog == 2 ? "NVMe controller" : "Non-volatile memory controller"; default: return "Storage controller"; }
    case 0x02: *cls = DC_NETWORK; return p->sub == 0 ? "Ethernet adapter" : p->sub == 0x80 ? "Network controller (wireless or other)" : "Network adapter";
    case 0x03: *cls = DC_DISPLAY; return p->sub == 0 ? "VGA display adapter" : p->sub == 2 ? "3D controller" : "Display adapter";
    case 0x04: *cls = DC_SOUND; return p->sub == 3 ? "High Definition Audio controller" : p->sub == 1 ? "Audio device" : p->sub == 0 ? "Video capture device" : "Multimedia device";
    case 0x05: *cls = DC_SYSTEM; *builtin = true; return "Memory controller";
    case 0x06: *cls = DC_SYSTEM; *builtin = true;
        switch (p->sub) { case 0: return "PCI host bridge"; case 1: return "PCI-to-ISA bridge"; case 4: return "PCI-to-PCI bridge";
        case 7: return "CardBus bridge"; default: return "PCI bridge device"; }
    case 0x07: *cls = DC_PORTS; return p->sub == 0 ? "PCI serial port" : p->sub == 1 ? "PCI parallel port" : p->sub == 3 ? "Modem" : "Communication controller";
    case 0x08: *cls = DC_SYSTEM; if (p->sub == 5) return "SD host controller"; *builtin = true;
        return p->sub == 0 ? "Interrupt controller" : p->sub == 2 ? "System timer" : p->sub == 3 ? "Real-time clock" : "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0B: *cls = DC_SYSTEM; *builtin = true; return "Processor";
    case 0x0C:
        if (p->sub == 3) { *cls = DC_USBCTL; return p->prog == 0x00 ? "USB 1.1 UHCI host controller" : p->prog == 0x10 ? "USB 1.1 OHCI host controller" :
            p->prog == 0x20 ? "USB 2.0 EHCI host controller" : p->prog == 0x30 ? "USB 3 xHCI host controller" : p->prog == 0x40 ? "USB4 host interface" : "USB controller"; }
        if (p->sub == 5) { *cls = DC_SYSTEM; *builtin = true; return "SMBus controller"; }
        return p->sub == 0 ? "IEEE 1394 (FireWire) controller" : p->sub == 0x80 ? "Serial bus controller" : "Serial bus controller";
    case 0x0D: *cls = DC_NETWORK; return p->sub == 0x11 ? "Bluetooth controller" : p->sub == 0x20 || p->sub == 0x21 ? "Wireless network adapter" : "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: *cls = DC_SYSTEM; *builtin = true; return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    case 0x13: *cls = DC_SYSTEM; *builtin = true; return "Non-essential instrumentation";
    default: return "PCI device";
    }
}

/* ---------------- the device list ---------------- */
#define MAX_DEVICES 192
static device_t devices[MAX_DEVICES];
static int ndevices;
int devmgr_count(void) { if (!ndevices) devmgr_scan(); return ndevices; }
device_t *devmgr_get(int i) { return i >= 0 && i < ndevices ? &devices[i] : NULL; }

/* devices found to be new since the saved profile; a rescan keeps the mark */
static char new_keys[24][100]; static int nnew;
static void profile_key(const device_t *d, char *out, int cap) { snprintf(out, cap, "%s\t%s", d->hwid, d->location); }
static device_t *add(int cls, int status, const driver_t *drv, const char *hwid, const char *name, const char *location, const char *detail) {
    if (ndevices == MAX_DEVICES) return NULL;
    device_t *d = &devices[ndevices++];
    memset(d, 0, sizeof *d);
    d->cls = cls; d->status = status; d->drv = drv;
    strlcpy(d->hwid, hwid, sizeof d->hwid); strlcpy(d->name, name, sizeof d->name);
    strlcpy(d->location, location ? location : "", sizeof d->location);
    strlcpy(d->detail, detail ? detail : "", sizeof d->detail);
    char key[100]; profile_key(d, key, sizeof key);
    for (int i = 0; i < nnew; i++) if (!strcmp(new_keys[i], key)) d->is_new = true;
    return d;
}

static void scan_pci(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    const driver_t *const *drv; int nd = devmgr_drivers(&drv);
    for (int i = 0; i < n; i++) {
        pci_dev_t *p = &devs[i];
        char hwid[48], loc[48], name[96];
        snprintf(hwid, sizeof hwid, "PCI\\VEN_%04X&DEV_%04X", p->vendor, p->device);
        snprintf(loc, sizeof loc, "PCI bus %d, device %d, function %d", p->bus, p->dev, p->fn);
        int cls; bool builtin;
        const char *kind = pci_kind(p, &cls, &builtin);
        const char *ven = vendor_name(p->vendor);
        if (ven) snprintf(name, sizeof name, "%s %s", ven, kind); else snprintf(name, sizeof name, "%s", kind);
        int c = claim_find(p);
        if (c >= 0) {
            add(claims[c].drv->cls, DS_WORKING, claims[c].drv, hwid, claims[c].name, loc, "This device is working properly.");
            continue;
        }
        const driver_t *m = NULL;
        for (int k = 0; k < nd && !m; k++) if (drv[k]->match && drv[k]->match(p)) m = drv[k];
        if (m) {
            /* a driver knows this device but is not running it */
            char why[128];
            int st = DS_FAILED;
            if (m->cls == DC_NETWORK && class_claimed(DC_NETWORK)) { st = DS_NOT_STARTED; snprintf(why, sizeof why, "The %s driver supports this adapter, but only one network adapter is used at a time.", m->name); }
            else if (m->cls == DC_SOUND && (class_claimed(DC_SOUND) || audio_present())) { st = DS_NOT_STARTED; snprintf(why, sizeof why, "The %s driver supports this device, but another sound device is in use.", m->name); }
            else snprintf(why, sizeof why, "The %s driver could not start this device. The kernel log (dmesg) has the details.", m->name);
            add(m->cls, st, m, hwid, name, loc, why);
        } else if (builtin) {
            add(cls, DS_BUILTIN, NULL, hwid, name, loc, OS_NAME " does not need a driver for this device.");
        } else {
            add(cls, DS_NO_DRIVER, NULL, hwid, name, loc, "No driver for this device is included in " OS_NAME ".");
        }
    }
}

static bool uart_present(uint16_t base) {
    uint8_t old = inb(base + 7);
    outb(base + 7, 0x5A); if (inb(base + 7) != 0x5A) return false;
    outb(base + 7, 0xA5); if (inb(base + 7) != 0xA5) return false;
    outb(base + 7, old);
    return true;
}
static void scan_fixed(void) {
    extern int ps2_detected(void);             /* drivers.c: 1 = controller, 2 = mouse answered, 4 = wheel */
    extern bool acpi_present(void);
    char vendor[16], brand[52]; cpu_get_info(vendor, brand);
    add(DC_COMPUTER, DS_BUILTIN, NULL, ARCH_BITS == 64 ? "ROOT\\ACPI_X64_PC" : "ROOT\\ACPI_X86_PC",
        ARCH_BITS == 64 ? "ACPI x64-based PC" : "ACPI x86-based PC", "", bootinfo.is_efi ? "Started by UEFI firmware." : "Started by BIOS (legacy) firmware.");
    char hw[48]; snprintf(hw, sizeof hw, "CPU\\%s", vendor);
    add(DC_PROCESSOR, DS_BUILTIN, NULL, hw, brand, "", "The kernel runs on one processor.");
    int ps2 = ps2_detected();
    if (ps2 & 1) {
        add(DC_KEYBOARD, DS_WORKING, &drv_i8042kbd, "ACPI\\PNP0303", "Standard PS/2 keyboard", "Keyboard controller (ports 60h/64h), IRQ 1", "This device is working properly.");
        if (ps2 & 2) add(DC_MOUSE, DS_WORKING, &drv_i8042mou, "ACPI\\PNP0F13", (ps2 & 4) ? "PS/2 wheel mouse (IntelliMouse-compatible)" : "PS/2-compatible mouse",
                         "Keyboard controller (ports 60h/64h), IRQ 12", "This device is working properly.");
    }
    add(DC_SYSTEM, DS_WORKING, &drv_pit, "ACPI\\PNP0100", "System timer", "Ports 40h-43h, IRQ 0", "Ticks 1000 times a second.");
    add(DC_SYSTEM, DS_WORKING, &drv_rtc, "ACPI\\PNP0B00", "System CMOS/real-time clock", "Ports 70h-71h", "This device is working properly.");
    add(DC_SOUND, DS_WORKING, &drv_pcspk, "ACPI\\PNP0800", "PC speaker", "Port 61h", "This device is working properly.");
    if (uart_present(0x3F8)) add(DC_PORTS, DS_WORKING, &drv_serial, "ACPI\\PNP0501", "Communications port (COM1)", "Ports 3F8h-3FFh", "Carries the kernel log.");
    if (acpi_present()) add(DC_SYSTEM, DS_WORKING, &drv_acpi, "ACPI\\PNP0C08", "ACPI power management", "", "Power off and restart go through ACPI.");
    /* an ISA sound card, when that is what plays */
    if (audio_present() && !class_claimed(DC_SOUND) && strstr(audio_controller_name(), "Sound Blaster"))
        add(DC_SOUND, DS_WORKING, &drv_sb16, "ISAPNP\\CTL0031", "Sound Blaster 16", "Port 220h, IRQ 5, DMA 1/5", "This device is working properly.");
    if (audio_present() && !class_claimed(DC_SOUND) && strstr(audio_controller_name(), "Windows Sound System"))
        add(DC_SOUND, DS_WORKING, &drv_wss, "ISAPNP\\CSC0000", "Crystal CS4231A Windows Sound System", "Port 530h, IRQ 9, DMA 3", "This device is working properly.");
}

static void scan_disks(void) {
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *b = blk_get(i);
        if (!b || b->dead) continue;
        char hwid[48], name[96], loc[48], model[41];
        strlcpy(model, b->model[0] ? b->model : "Disk", sizeof model);
        for (int l = strlen(model); l > 0 && model[l - 1] == ' '; l--) model[l - 1] = 0;
        { char *o = model; for (char *c = model; *c; c++) if (*c != ' ' || (o > model && o[-1] != ' ')) *o++ = *c; *o = 0; }   /* "QEMU    HARDDISK" */
        snprintf(hwid, sizeof hwid, "%s\\DISK", b->kind ? b->kind : "DISK");
        for (char *c = hwid; *c; c++) if (*c != '\\') *c = toupper(*c);
        snprintf(name, sizeof name, "%s (%u MB)", model, b->sectors / 2048);
        snprintf(loc, sizeof loc, "Disk %d on %s", b->index, b->driver ? b->driver : "the storage driver");
        add(DC_DISK, DS_WORKING, &drv_disk, hwid, name, loc, "This device is working properly.");
    }
}

#ifdef __x86_64__
static void scan_usb(void) {
    for (usb_dev_t *u = usb_first(); u; u = u->next) {
        if (u->gone) continue;
        char hwid[48], loc[48], name[96];
        snprintf(hwid, sizeof hwid, "USB\\VID_%04X&PID_%04X", u->vendor, u->product);
        if (u->hub_addr) snprintf(loc, sizeof loc, "%s, hub %d port %d, address %d", u->hcd ? u->hcd->name : "USB", u->hub_addr, u->hub_port, u->addr);
        else snprintf(loc, sizeof loc, "%s, root port %d, address %d", u->hcd ? u->hcd->name : "USB", u->root_port, u->addr);
        const char *speed = u->speed == 4 ? "SuperSpeed" : u->speed == 3 ? "high speed" : u->speed == 2 ? "low speed" : "full speed";
        char detail[128];
        const driver_t *drv = NULL; int cls = DC_USBDEV; const char *fallback = "USB device";
        if (u->ports) { drv = &drv_usbhub; fallback = "USB hub"; }
        else switch (u->kind) {
            case USB_KIND_KEYBOARD: drv = &drv_usbhid; cls = DC_KEYBOARD; fallback = "USB keyboard"; break;
            case USB_KIND_MOUSE: drv = &drv_usbhid; cls = DC_MOUSE; fallback = "USB mouse"; break;
            case USB_KIND_STORAGE: drv = &drv_usbstor; fallback = "USB mass storage device"; break;   /* its disk is under Disk drives */
            case USB_KIND_AUDIO: drv = &drv_usbaudio; cls = DC_SOUND; fallback = "USB audio device"; break;
        }
        snprintf(name, sizeof name, "%s", u->name[0] && u->kind != USB_KIND_STORAGE && !u->ports ? u->name : fallback);
        if (drv) { snprintf(detail, sizeof detail, "This device is working properly (%s).", speed); add(cls, DS_WORKING, drv, hwid, name, loc, detail); }
        else {
            snprintf(detail, sizeof detail, "No driver for this USB device class (%02X/%02X/%02X) is included in " OS_NAME ".", u->cls, u->subcls, u->proto);
            add(DC_USBDEV, DS_NO_DRIVER, NULL, hwid, name, loc, detail);
        }
    }
    for (int i = 0; i < 8; i++) {
        const char *where; uint16_t ven, prod; uint8_t c[3];
        if (!usb_undriven(i, &where, &ven, &prod, c)) continue;
        char hwid[48], detail[128];
        snprintf(hwid, sizeof hwid, "USB\\VID_%04X&PID_%04X", ven, prod);
        if (c[0]) snprintf(detail, sizeof detail, "No driver for this USB device class (%02X/%02X/%02X) is included in " OS_NAME ".", c[0], c[1], c[2]);
        else snprintf(detail, sizeof detail, "None of this USB device's interfaces has a driver in " OS_NAME ".");
        const char *nm = c[0] == 2 ? "USB communications device" : c[0] == 7 ? "USB printer" : c[0] == 0x0E ? "USB video device" :
                         c[0] == 0xE0 ? "USB wireless controller (Bluetooth or other)" : c[0] == 0xFF ? "USB device (vendor-specific)" : "USB device";
        add(DC_USBDEV, DS_NO_DRIVER, NULL, hwid, nm, where, detail);
    }
}
#endif

void devmgr_scan(void) {
    ndevices = 0;
    scan_fixed();
    scan_pci();
    scan_disks();
#ifdef __x86_64__
    scan_usb();
#endif
}

int devmgr_problem_count(void) {
    int n = 0;
    for (int i = 0; i < devmgr_count(); i++) if (devices[i].status == DS_NO_DRIVER || devices[i].status == DS_FAILED) n++;
    return n;
}
void devmgr_summary(char *buf, int cap) {
    int total = devmgr_count(), ok = 0, nodrv = 0, failed = 0;
    for (int i = 0; i < total; i++) {
        int s = devices[i].status;
        if (s == DS_WORKING || s == DS_BUILTIN || s == DS_NOT_STARTED) ok++; else if (s == DS_NO_DRIVER) nodrv++; else failed++;
    }
    int o = snprintf(buf, cap, "%d devices found, %d ready", total, ok);
    if (nodrv) o += snprintf(buf + o, cap - o, ", %d without a driver", nodrv);
    if (failed) snprintf(buf + o, cap - o, ", %d not started", failed);
}

/* ---------------- hardware profile ---------------- */
/* one line per device: hardware ID, tab, location. A device is "new" when no
 * line has both its ID and its location. */
static bool profile_differs;
int devmgr_compare_profile(void) {
    uint32_t len; char *txt = (char *)vfs_read_file(HARDWARE_PROFILE, &len);
    profile_differs = true;
    nnew = 0;
    devmgr_scan();
    if (!txt) return -1;
    int fresh = 0;
    for (int i = 0; i < ndevices; i++) {
        char key[100]; profile_key(&devices[i], key, sizeof key);
        int kl = strlen(key); bool known = false;
        for (const char *p = txt; p && *p && !known;) {
            const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p);
            if (l > 0 && p[l - 1] == '\r') l--;
            if (l >= kl && !strncmp(p, key, kl) && (l == kl || p[kl] == '\t')) known = true;
            p = e ? e + 1 : NULL;
        }
        if (!known) {
            devices[i].is_new = true; fresh++;
            if (nnew < (int)ARRAY_LEN(new_keys)) strlcpy(new_keys[nnew++], key, sizeof new_keys[0]);
        }
    }
    int lines = 0;
    for (const char *p = txt; *p; p++) if (*p == '\n') lines++;
    profile_differs = fresh > 0 || lines != ndevices;
    kfree(txt);
    return fresh;
}

/* ---------------- new hardware ---------------- */
static void announce(int fresh, const device_t *first) {
    char text[200];
    const char *state = first->status == DS_WORKING || first->status == DS_BUILTIN ? "Its driver is installed and it is ready to use." :
                        first->status == DS_NOT_STARTED ? "A driver is installed; the device is not in use." :
                        first->status == DS_FAILED ? "Its driver could not start it. See Device Manager." :
                        "No driver for it is included. See Device Manager.";
    if (fresh > 1) snprintf(text, sizeof text, "%s and %d more. %s", first->name, fresh - 1, fresh > 2 ? "Device Manager lists them all." : state);
    else snprintf(text, sizeof text, "%s. %s", first->name, state);
    notify_balloon("Found New Hardware", text);
}
/* what was there at the last look, to notice a device plugged in while running */
static char (*snap)[100]; static int nsnap;
static void snapshot(void) {
    if (!snap) snap = kmalloc(MAX_DEVICES * 100);
    nsnap = 0;
    if (!snap) return;
    for (int i = 0; i < ndevices; i++) profile_key(&devices[i], snap[nsnap++], 100);
}
static void watch_task(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep(2000);
        if (!snap) continue;
        devmgr_scan();
        int fresh = 0; const device_t *first = NULL;
        for (int i = 0; i < ndevices; i++) {
            char key[100]; profile_key(&devices[i], key, sizeof key);
            bool known = false;
            for (int k = 0; k < nsnap && !known; k++) if (!strcmp(snap[k], key)) known = true;
            if (known) continue;
            if (devices[i].cls == DC_DISK && fresh) continue;       /* the disk of a USB stick just announced */
            if (!first) first = &devices[i];
            fresh++;
        }
        bool changed = fresh || nsnap != ndevices;
        if (fresh) { klog("devmgr: new hardware: %s (%s)", first->name, first->drv ? first->drv->name : "no driver"); announce(fresh, first); }
        if (changed) { snapshot(); if (system_volume) devmgr_save_profile(); }
    }
}
/* once, when the desktop is up: say what is new since the last start, keep the
 * profile current, and start watching for devices plugged in later */
void devmgr_boot_check(void) {
    int fresh = devmgr_compare_profile();
    char sum[128]; devmgr_summary(sum, sizeof sum);
    klog("devmgr: %s", sum);
    for (int i = 0; i < ndevices; i++)
        if (devices[i].status == DS_NO_DRIVER || devices[i].status == DS_FAILED)
            klog("devmgr: %s: %s [%s] %s", devices[i].status == DS_FAILED ? "not started" : "no driver", devices[i].name, devices[i].hwid, devices[i].location);
    if (system_volume) {
        if (fresh > 0) {
            const device_t *first = NULL;
            for (int i = 0; i < ndevices && !first; i++) if (devices[i].is_new) first = &devices[i];
            if (first) announce(fresh, first);
        }
        if (profile_differs) devmgr_save_profile();
    }
    snapshot();
    task_create("devmgr", watch_task, NULL, 16384);
}
bool devmgr_save_profile(void) {
    if (!ndevices) devmgr_scan();
    int cap = ndevices * 280 + 64, o = 0;
    char *b = kmalloc(cap);
    if (!b) return false;
    for (int i = 0; i < ndevices; i++) {
        device_t *d = &devices[i];
        o += snprintf(b + o, cap - o, "%s\t%s\t%s\t%s\r\n", d->hwid, d->location, d->drv ? d->drv->name : "-", d->name);
    }
    vfs_mkdir_p("C:\\WRLD\\System32\\config");
    bool ok = vfs_write_file(HARDWARE_PROFILE, b, o) != NULL;
    kfree(b);
    return ok;
}
