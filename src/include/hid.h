/* HID report formats. A HID device describes its input reports in a report
 * descriptor: which bits are the buttons, the axes and the keys, and whether
 * each report starts with a report ID. This is the part of the USB stack that
 * reads that description and takes a report apart; it touches no hardware. */
#ifndef HID_H
#define HID_H
#include "types.h"

/* one HID interface of a configuration, with its interrupt IN endpoint */
typedef struct {
    uint8_t num;                            /* bInterfaceNumber */
    uint8_t subcls, proto;                  /* 1/1 = boot keyboard, 1/2 = boot mouse */
    uint8_t ep;                             /* interrupt IN endpoint number, 0 = none */
    uint8_t interval;
    uint16_t maxpkt;
    uint16_t rlen;                          /* length of the report descriptor */
} hid_iface_t;

typedef struct {
    bool ids;                               /* every report starts with a report ID byte */
    struct {
        bool present;                       /* a relative X and Y were found */
        int id;                             /* the report they are in */
        int btn_off, btn_bits;              /* bit offsets and widths, after the ID byte */
        int x_off, x_bits, y_off, y_bits;
        int w_off, w_bits;
    } mouse;
    struct {
        bool present;
        int id;
        int mod_off;                        /* the eight modifier bits, -1 = none */
        int key_off, key_bits, key_count;   /* an array of the usages held down */
        int map_off, map_min, map_count;    /* or one bit per usage (N-key rollover) */
    } keys;
} hid_format_t;

/* the HID interfaces in a configuration descriptor; returns how many */
int hid_find_interfaces(const uint8_t *cfg, int len, hid_iface_t *out, int max);
/* read a report descriptor */
void hid_parse_format(hid_format_t *f, const uint8_t *desc, int n);
/* a bit field of a report, sign-extended if asked */
int hid_field(const uint8_t *p, int n, int off, int bits, bool sign);
/* take a report apart; `p` is the report after its ID byte. HID's Y grows
 * downwards and its wheel is positive away from the user. */
bool hid_decode_mouse(const hid_format_t *f, const uint8_t *p, int n,
                      int *dx, int *dy, int *buttons, int *wheel);
/* the keys held down, in the 8-byte boot keyboard layout (modifiers, 0, six usages) */
bool hid_decode_keys(const hid_format_t *f, const uint8_t *p, int n, uint8_t out[8]);

#endif
