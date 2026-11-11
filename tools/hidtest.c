/* Checks for src/usb/hid.c, run on the build machine: `make check-hid`.
 *
 * QEMU only has a boot keyboard and a plain three-button mouse, so the report
 * formats of real devices (report IDs, 16-bit and packed 12-bit axes, N-key
 * rollover keyboards, several HID interfaces in one device) are checked here
 * against descriptors of that shape instead. hid.c touches no hardware, so it
 * is compiled as it is. */
#include "hid.h"
int printf(const char *fmt, ...);

static int fails, checks;
static void check(const char *what, int got, int want)
{
    checks++;
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
}

/* QEMU's usb-mouse: three buttons, five bits of padding, X, Y, wheel. */
static const uint8_t boot_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06,
    0xC0, 0xC0
};

/* A gaming mouse: report ID 2, sixteen buttons, 16-bit X and Y, a wheel, and a
 * horizontal wheel on the consumer page; then a consumer-control report (ID 3)
 * in a second top-level collection that must not be read as a mouse. */
static const uint8_t wide_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x02, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x10, 0x15,0x00, 0x25,0x01, 0x95,0x10, 0x75,0x01, 0x81,0x02,
    0x05,0x01, 0x16,0x01,0x80, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x09,0x30, 0x09,0x31, 0x81,0x06,
    0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x09,0x38, 0x81,0x06,
    0x05,0x0C, 0x0A,0x38,0x02, 0x95,0x01, 0x81,0x06,
    0xC0, 0xC0,
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x03,
    0x75,0x10, 0x95,0x02, 0x15,0x01, 0x26,0x8C,0x02, 0x19,0x01, 0x2A,0x8C,0x02, 0x81,0x00,
    0xC0
};

/* 12-bit X and Y packed into three bytes, no report ID. */
static const uint8_t packed_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x08, 0x15,0x00, 0x25,0x01, 0x95,0x08, 0x75,0x01, 0x81,0x02,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x01,0xF8, 0x26,0xFF,0x07, 0x75,0x0C, 0x95,0x02, 0x81,0x06,
    0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0xC0, 0xC0
};

/* A tablet: absolute X and Y. Not a mouse we can drive as relative. */
static const uint8_t tablet[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x02,
    0xC0, 0xC0
};

/* The standard boot keyboard: modifiers, a reserved byte, LEDs out, six keys. */
static const uint8_t boot_kbd[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65, 0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,
    0xC0
};

/* An N-key rollover keyboard: report ID 4, modifiers, then one bit per usage
 * from 0x04 (A) for 120 keys. */
static const uint8_t nkro_kbd[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x04,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x05,0x07, 0x19,0x04, 0x29,0x7B, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x78, 0x81,0x02,
    0xC0
};

/* A wireless receiver: interface 0 boot keyboard on 0x81, interface 1 boot
 * mouse on 0x82 (with an OUT endpoint first, to be skipped), and interface 2
 * vendor-specific, which is not HID at all. */
static const uint8_t composite[] = {
    0x09,0x02, 0x54,0x00, 0x03,0x01,0x00,0xA0,0x31,
    0x09,0x04, 0x00,0x00,0x01, 0x03,0x01,0x01, 0x00,
    0x09,0x21, 0x11,0x01,0x00,0x01,0x22, 0x3B,0x00,
    0x07,0x05, 0x81,0x03, 0x08,0x00, 0x08,
    0x09,0x04, 0x01,0x00,0x02, 0x03,0x01,0x02, 0x00,
    0x09,0x21, 0x11,0x01,0x00,0x01,0x22, 0x94,0x00,
    0x07,0x05, 0x02,0x03, 0x20,0x00, 0x02,
    0x07,0x05, 0x82,0x03, 0x14,0x00, 0x02,
    0x09,0x04, 0x02,0x00,0x01, 0xFF,0x00,0x00, 0x00,
    0x07,0x05, 0x83,0x03, 0x20,0x00, 0x02
};

int main(void)
{
    hid_format_t f;
    int dx, dy, btn, wheel;

    hid_parse_format(&f, boot_mouse, sizeof boot_mouse);
    check("boot mouse present", f.mouse.present, 1);
    check("boot mouse ids", f.ids, 0);
    check("boot mouse btn bits", f.mouse.btn_bits, 3);
    check("boot mouse x off", f.mouse.x_off, 8);
    check("boot mouse y off", f.mouse.y_off, 16);
    check("boot mouse w off", f.mouse.w_off, 24);
    check("boot mouse len", f.mouse.len, 4);
    check("boot mouse keys", f.keys.present, 0);
    {
        const uint8_t r[4] = { 0x05, 0x05, 0xFB, 0xFF };        /* left+middle, right 5, up 5, wheel -1 */
        check("boot mouse decode", hid_decode_mouse(&f, r, 4, &dx, &dy, &btn, &wheel), 1);
        check("boot mouse dx", dx, 5);
        check("boot mouse dy", dy, -5);
        check("boot mouse btn", btn, 5);
        check("boot mouse wheel", wheel, -1);
        check("boot mouse too short", hid_decode_mouse(&f, r, 2, &dx, &dy, &btn, &wheel), 0);
        check("boot mouse takes its own report", hid_report_kind(&f, 1, HID_REPORT_MOUSE, r, 4), HID_REPORT_MOUSE);
        const uint8_t kbd[8] = { 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };   /* a keyboard's report */
        check("boot mouse refuses a keyboard report", hid_report_kind(&f, 1, HID_REPORT_MOUSE, kbd, 8), 0);
        check("boot keyboard refuses a mouse report", hid_report_kind(&f, 1, HID_REPORT_KEYBOARD, r, 4), 0);
    }

    hid_parse_format(&f, wide_mouse, sizeof wide_mouse);
    check("wide present", f.mouse.present, 1);
    check("wide ids", f.ids, 1);
    check("wide id", f.mouse.id, 2);
    check("wide btn bits", f.mouse.btn_bits, 16);
    check("wide x off", f.mouse.x_off, 16);
    check("wide x bits", f.mouse.x_bits, 16);
    check("wide y off", f.mouse.y_off, 32);
    check("wide w off", f.mouse.w_off, 48);
    check("wide w bits", f.mouse.w_bits, 8);
    check("wide len", f.mouse.len, 8);
    {
        /* after the ID byte: buttons 0x0002, X = -300, Y = +1000, wheel +1, pan 0 */
        const uint8_t r[8] = { 0x02,0x00, 0xD4,0xFE, 0xE8,0x03, 0x01, 0x00 };
        check("wide decode", hid_decode_mouse(&f, r, 8, &dx, &dy, &btn, &wheel), 1);
        check("wide dx", dx, -300);
        check("wide dy", dy, 1000);
        check("wide btn", btn, 2);
        check("wide wheel", wheel, 1);
        const uint8_t idr[9] = { 0x02, 0x02,0x00, 0xD4,0xFE, 0xE8,0x03, 0x01, 0x00 };
        check("wide report kind", hid_report_kind(&f, 0, HID_REPORT_MOUSE, idr, 9), HID_REPORT_MOUSE);
        const uint8_t other[9] = { 0x03, 0x02,0x00, 0xD4,0xFE, 0xE8,0x03, 0x01, 0x00 };
        check("wide other id refused", hid_report_kind(&f, 0, HID_REPORT_MOUSE, other, 9), 0);
        check("wide wrong length refused", hid_report_kind(&f, 0, HID_REPORT_MOUSE, idr, 8), 0);
    }

    hid_parse_format(&f, packed_mouse, sizeof packed_mouse);
    check("packed present", f.mouse.present, 1);
    check("packed x off", f.mouse.x_off, 8);
    check("packed x bits", f.mouse.x_bits, 12);
    check("packed y off", f.mouse.y_off, 20);
    check("packed w off", f.mouse.w_off, 32);
    check("packed len", f.mouse.len, 5);
    {
        /* X = -2 (0xFFE), Y = +3 (0x003): bytes FE 3F 00 */
        const uint8_t r[5] = { 0x01, 0xFE, 0x3F, 0x00, 0xFF };
        check("packed decode", hid_decode_mouse(&f, r, 5, &dx, &dy, &btn, &wheel), 1);
        check("packed dx", dx, -2);
        check("packed dy", dy, 3);
        check("packed btn", btn, 1);
        check("packed wheel", wheel, -1);
    }

    hid_parse_format(&f, tablet, sizeof tablet);
    check("tablet is not a relative mouse", f.mouse.present, 0);

    hid_parse_format(&f, boot_kbd, sizeof boot_kbd);
    check("kbd present", f.keys.present, 1);
    check("kbd no mouse", f.mouse.present, 0);
    check("kbd ids", f.ids, 0);
    check("kbd mod off", f.keys.mod_off, 0);
    check("kbd key off", f.keys.key_off, 16);
    check("kbd key bits", f.keys.key_bits, 8);
    check("kbd key count", f.keys.key_count, 6);
    check("kbd no map", f.keys.map_count, 0);
    check("kbd len", f.keys.len, 8);
    {
        uint8_t out[8];
        const uint8_t r[8] = { 0x22, 0x00, 0x04, 0x1D, 0x00, 0x00, 0x00, 0x00 };   /* LShift+RShift, A, Z */
        check("kbd decode", hid_decode_keys(&f, r, 8, out), 1);
        check("kbd mods", out[0], 0x22);
        check("kbd key 0", out[2], 0x04);
        check("kbd key 1", out[3], 0x1D);
        check("kbd key 2", out[4], 0x00);
        const uint8_t over[8] = { 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
        check("kbd rollover is refused", hid_decode_keys(&f, over, 8, out), 0);
        const uint8_t shortr[4] = { 0x00, 0x04, 0x00, 0x00 };      /* some other device's report */
        check("kbd short report is not a key report", hid_report_kind(&f, 0, HID_REPORT_KEYBOARD, shortr, 4), 0);
        const uint8_t longer[9] = { 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        check("kbd padded report is still its own", hid_report_kind(&f, 0, HID_REPORT_KEYBOARD, longer, 9), HID_REPORT_KEYBOARD);
        check("kbd report kind", hid_report_kind(&f, 0, HID_REPORT_KEYBOARD, r, 8), HID_REPORT_KEYBOARD);
    }

    hid_parse_format(&f, nkro_kbd, sizeof nkro_kbd);
    check("nkro present", f.keys.present, 1);
    check("nkro ids", f.ids, 1);
    check("nkro id", f.keys.id, 4);
    check("nkro mod off", f.keys.mod_off, 0);
    check("nkro map off", f.keys.map_off, 8);
    check("nkro map min", f.keys.map_min, 4);
    check("nkro map count", f.keys.map_count, 120);
    check("nkro no array", f.keys.key_count, 0);
    check("nkro len", f.keys.len, 16);
    {
        uint8_t out[8];
        uint8_t r[16];
        for (int i = 0; i < 16; i++) r[i] = 0;
        r[0] = 0x01;                                     /* left Ctrl */
        r[1] = 0x01 | 0x04;                              /* usages 0x04 (A) and 0x06 (C) */
        r[1 + (0x4C - 4) / 8] |= (uint8_t)(1u << ((0x4C - 4) % 8));   /* Delete */
        check("nkro decode", hid_decode_keys(&f, r, 16, out), 1);
        check("nkro mods", out[0], 0x01);
        check("nkro key 0", out[2], 0x04);
        check("nkro key 1", out[3], 0x06);
        check("nkro key 2", out[4], 0x4C);
        check("nkro key 3", out[5], 0x00);
    }

    {
        hid_iface_t hi[4];
        int n = hid_find_interfaces(composite, sizeof composite, hi, 4);
        check("composite count", n, 2);
        check("composite kbd iface", hi[0].num, 0);
        check("composite kbd proto", hi[0].proto, 1);
        check("composite kbd ep", hi[0].ep, 1);
        check("composite kbd maxpkt", hi[0].maxpkt, 8);
        check("composite kbd rlen", hi[0].rlen, 0x3B);
        check("composite mouse iface", hi[1].num, 1);
        check("composite mouse proto", hi[1].proto, 2);
        check("composite mouse ep", hi[1].ep, 2);
        check("composite mouse maxpkt", hi[1].maxpkt, 20);
        check("composite mouse rlen", hi[1].rlen, 0x94);
        check("composite max respected", hid_find_interfaces(composite, sizeof composite, hi, 1), 1);
        check("truncated descriptor", hid_find_interfaces(composite, 20, hi, 4), 0);
    }

    check("field sign", hid_field((const uint8_t[]){ 0xF0, 0xFF }, 2, 4, 12, true), -1);
    check("field past the end", hid_field((const uint8_t[]){ 0xFF }, 1, 8, 8, false), 0);

    printf(fails ? "%d of %d checks FAILED\n" : "all %d checks passed\n", fails ? fails : checks, checks);
    return fails != 0;
}
