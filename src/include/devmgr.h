/* Device manager: every driver in the kernel registers itself here, and
 * devmgr_scan() lists every device the computer has (PCI, USB, disks and the
 * fixed PC devices), which driver matches it, and whether that driver started.
 *
 * Drivers are built into the kernel, so "installing" a driver means matching
 * it to a device. A PCI driver describes itself with DRIVER_PCI() and calls
 * dev_claim() when it has brought a device up; a device whose driver matched
 * but never claimed it is shown as "the driver could not start". */
#ifndef DEVMGR_H
#define DEVMGR_H
#include "kernel.h"

enum { DC_COMPUTER, DC_PROCESSOR, DC_DISPLAY, DC_STORAGE, DC_DISK, DC_NETWORK, DC_SOUND, DC_USBCTL, DC_USBDEV,
       DC_KEYBOARD, DC_MOUSE, DC_PORTS, DC_SYSTEM, DC_OTHER, DC_COUNT };
const char *devclass_name(int cls);            /* "Network adapters" */

enum {
    DS_WORKING,                                /* a driver is running the device */
    DS_BUILTIN,                                /* needs no driver (bridges, the motherboard's own parts) */
    DS_NOT_STARTED,                            /* a driver matches, but the device is not in use */
    DS_FAILED,                                 /* a driver matches and could not start the device */
    DS_NO_DRIVER,                              /* nothing in the kernel drives this */
};

typedef struct driver {
    const char *name;                          /* short name: "e1000" */
    const char *desc;                          /* "Intel PRO/1000 Gigabit Ethernet" */
    int cls;                                   /* DC_* */
    const char *bus;                           /* "PCI", "USB", "ISA", "ACPI" */
    bool (*match)(const pci_dev_t *p);         /* PCI drivers: would this driver take the device? */
} driver_t;
/* put a driver in the kernel's driver list (a table the linker collects) */
#define DRIVER_REGISTER(var) \
    static const driver_t *const var##_entry __attribute__((section("drivers"), used)) = &(var)
#define DRIVER_PCI(var, name_, desc_, cls_, match_) \
    static const driver_t var = { name_, desc_, cls_, "PCI", match_ }; DRIVER_REGISTER(var)

/* a driver has a PCI device working; `name` (may be NULL) is what it found */
void dev_claim(const pci_dev_t *p, const driver_t *drv, const char *name);

typedef struct device {
    char hwid[48];                             /* PCI\VEN_8086&DEV_100E, USB\VID_046D&PID_C077, ACPI\PNP0303 */
    char name[96];
    char location[48];                         /* "PCI bus 0, device 3, function 0" */
    char detail[128];                          /* why it is in this state, in a sentence */
    int cls, status;
    const driver_t *drv;                       /* NULL when there is none */
    bool is_new;                               /* not in the saved hardware profile */
} device_t;

void devmgr_scan(void);                        /* build the list again (hotplug, or a first look) */
int devmgr_count(void);
device_t *devmgr_get(int i);
int devmgr_drivers(const driver_t *const **out);   /* every driver in the kernel */
int devmgr_problem_count(void);                /* devices with no driver or a failed one */
void devmgr_summary(char *buf, int cap);       /* "23 devices, 21 working, 2 without a driver" */

/* The hardware profile (C:\WRLD\System32\config\hardware.ini) remembers what
 * the computer had. Returns how many devices are new since it was written,
 * and marks them is_new; -1 if there is no profile yet. */
int devmgr_compare_profile(void);
bool devmgr_save_profile(void);
void devmgr_boot_check(void);                 /* desktop is up: announce new hardware, then watch for hotplug */
#define HARDWARE_PROFILE "C:\\WRLD\\System32\\config\\hardware.ini"
#endif
