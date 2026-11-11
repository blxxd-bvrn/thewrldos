/* USB: a small host stack for the 64-bit edition. A host controller driver
 * (UHCI, OHCI, EHCI or xHCI) moves transfers; this layer enumerates devices,
 * then hands each one to a class driver: HID boot keyboards and mice feed the
 * PS/2 input queues, and mass storage registers a SCSI disk in the block
 * layer. */
#ifndef USB_H
#define USB_H
#include "kernel.h"

typedef struct usb_hcd usb_hcd_t;
typedef struct usb_dev usb_dev_t;

enum { USB_KIND_NONE = 0, USB_KIND_KEYBOARD, USB_KIND_MOUSE, USB_KIND_STORAGE };

/* one device on a bus, after enumeration */
struct usb_dev {
    usb_hcd_t *hcd;
    int addr;
    bool low_speed;
    uint16_t vendor, product;
    uint8_t cls, subcls, proto;
    int kind;
    int ep_in, ep_out, ep_int;                 /* endpoint numbers, 0 = none */
    uint16_t maxpkt_in, maxpkt_out, maxpkt_int;
    uint8_t toggle_in, toggle_out, toggle_int;
    uint8_t report[16];                        /* last HID report, for key-up detection */
    bool seen;                                 /* has this device sent a report yet? */
    int root_port, route;                      /* where it sits, for xHCI's slot context */
    int id;
    char name[40];
    usb_dev_t *next;
};

/* A host controller driver implements the two transfer primitives; the rest of
 * the stack is written against them. */
struct usb_hcd {
    const char *name;
    /* control transfer to endpoint 0; returns bytes transferred or -1 */
    int (*control)(usb_hcd_t *h, int addr, bool low_speed, uint8_t type, uint8_t req,
                   uint16_t value, uint16_t index, void *buf, int len);
    /* one IN or OUT transfer on a non-zero endpoint; returns bytes, -2 on a
     * time-out with the transfer still pending, or -1 on an error */
    int (*transfer)(usb_hcd_t *h, int addr, bool low_speed, int ep, bool in,
                    uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms);
    /* reset a root port and report whether a device is attached, and its speed */
    bool (*port_reset)(usb_hcd_t *h, int port, bool *low_speed);
    /* a device appeared (root_port is 1-based, route is the hub path): xHCI has
     * to allocate a slot before endpoint 0 can be used. `hub_addr`/`hub_port`
     * are zero for a root device; `speed` is an xHCI speed code or 0 to derive
     * it from the root port. May be NULL. */
    bool (*new_device)(usb_hcd_t *h, int root_port, int route, bool low_speed,
                       int hub_addr, int hub_port, int speed);
    int ports;
    void *priv;
};

void usb_init(void);
void usb_poll(void);                            /* poll HID devices for reports */
int usb_count(void);
usb_dev_t *usb_first(void);
const char *usb_boot_summary(void);             /* controller list and device count, for the logon screen */
int usb_has_input(void);                        /* a USB keyboard or mouse was enumerated */
int usb_input_active(void);                     /* at least one HID report has arrived */
void usb_note_controller(const pci_dev_t *pd, int ok, int ports, int connected);
void usb_diag(const char *fmt, ...);            /* last USB failures, shown on the log-on screen */
const char *usb_diag_text(void);
extern int usb_unsupported;                    /* USB controllers this kernel has no driver for */
void usb_attach(usb_hcd_t *h);                  /* a controller enumerates its root ports */
int usb_control(usb_dev_t *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, int len);
int usb_in(usb_dev_t *d, int ep, uint16_t maxpkt, uint8_t *toggle, void *buf, int len, int timeout_ms);
int usb_out(usb_dev_t *d, int ep, uint16_t maxpkt, uint8_t *toggle, const void *buf, int len, int timeout_ms);

/* host controllers */
void uhci_probe(void);
void ohci_probe(void);
void ehci_probe(void);
void xhci_probe(void);

#endif
