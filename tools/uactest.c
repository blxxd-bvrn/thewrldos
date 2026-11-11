/* Host-side checks for the USB audio descriptor parser (used by
 * make check-audio). A class 1 device lists its rates in the format
 * descriptor; a class 2 one leaves the rate to its clock source entity. */
#include "uac.h"
#include <stdio.h>

static int fails;
static void check(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: got %ld, wanted %ld\n", what, got, want); fails++; }
}

/* A class 1 configuration: control interface 0, streaming interface 1 with an
 * empty alternate setting 0 and a 48 kHz stereo 16-bit alternate setting 1. */
static const unsigned char uac1[] = {
    9, 2, 0x4A, 0, 2, 1, 0, 0x80, 50,

    9, 4, 0, 0, 0, 1, 1, 0, 0,                       /* interface 0: audio control */
    9, 0x24, 1, 0x00, 0x01, 0x1C, 0x00, 1, 1,        /* AC header, bcdADC 1.00 */

    9, 4, 1, 0, 0, 1, 2, 0, 0,                       /* interface 1 alt 0: streaming */
    7, 0x24, 1, 1, 0, 0x01, 0x00,                     /* AS header, terminal 1 */

    9, 4, 1, 1, 1, 1, 2, 0, 0,                       /* interface 1 alt 1: one endpoint */
    7, 0x24, 1, 1, 0, 0x01, 0x00,
    11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xBB, 0x00,   /* format 1: 2 ch, 2 B, 16 bit, 48 kHz */
    9, 5, 0x01, 1, 0xC0, 0x00, 1, 0, 0               /* ISO OUT, 192 bytes, every frame */
};

/* A class 2 configuration: the format has no rate list; the output terminal 4
 * is fed by clock source 41, and the stream is linked to terminal 4. */
static const unsigned char uac2[] = {
    9, 2, 0x46, 0, 2, 1, 0, 0x80, 50,

    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0x00, 0x02, 1, 0x1C, 0x00, 0,
    8, 0x24, 10, 41, 3, 3, 0, 0,                     /* clock source 41, programmable */
    12, 0x24, 3, 4, 0x01, 0x03, 0, 5, 41, 0, 0, 0,   /* output terminal 4, clock 41 */

    9, 4, 1, 0, 0, 1, 2, 0, 0,
    16, 0x24, 1, 4, 0, 1, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0,   /* AS header, terminal 4 */

    9, 4, 1, 1, 1, 1, 2, 0, 0,
    16, 0x24, 1, 4, 0, 1, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0,
    7, 0x24, 2, 1, 2, 2, 16,                          /* format 1, no rates (class 2) */
    9, 5, 0x01, 1, 0x18, 0x00, 1, 0, 0               /* ISO OUT, 24 bytes per microframe */
};

int main(void)
{
    uac_fmt_t f;

    check("uac1 parse", uac_parse(uac1, sizeof uac1, &f), 1);
    check("uac1 control interface", f.ac, 0);
    check("uac1 stream interface", f.stream, 1);
    check("uac1 alternate setting", f.alt, 1);
    check("uac1 endpoint", f.ep, 1);
    check("uac1 max packet", f.maxpkt, 192);
    check("uac1 interval", f.interval, 1);
    check("uac1 rate", f.rate, 48000);
    check("uac1 channels", f.channels, 2);
    check("uac1 frame bytes", f.frame_bytes, 2);
    check("uac1 is class 1", f.class2, 0);

    check("uac2 parse", uac_parse(uac2, sizeof uac2, &f), 1);
    check("uac2 control interface", f.ac, 0);
    check("uac2 stream interface", f.stream, 1);
    check("uac2 alternate setting", f.alt, 1);
    check("uac2 endpoint", f.ep, 1);
    check("uac2 max packet", f.maxpkt, 24);
    check("uac2 interval", f.interval, 1);
    check("uac2 rate comes from the clock", f.rate, 0);
    check("uac2 is class 2", f.class2, 1);
    check("uac2 clock source", f.clock, 41);
    check("uac2 terminal link", f.link, 4);
    check("uac2 channels", f.channels, 2);
    check("uac2 frame bytes", f.frame_bytes, 2);

    {   /* a configuration with no audio interface at all */
        static const unsigned char none[] = { 9, 2, 9, 0, 1, 1, 0, 0x80, 50,
                                              9, 4, 0, 0, 0, 3, 1, 1, 0 };
        check("no audio is refused", uac_parse(none, sizeof none, &f), 0);
    }
    {   /* a streaming interface whose format is not 16-bit stereo is still read */
        static const unsigned char mono[] = {
            9, 2, 0x36, 0, 2, 1, 0, 0x80, 50,
            9, 4, 0, 0, 0, 1, 1, 0, 0,
            9, 0x24, 1, 0x00, 0x01, 0x14, 0x00, 1, 1,
            9, 4, 1, 1, 1, 1, 2, 0, 0,
            7, 0x24, 1, 1, 0, 0x01, 0x00,
            11, 0x24, 2, 1, 1, 2, 16, 1, 0x80, 0xBB, 0x00,
            9, 5, 0x01, 1, 0x60, 0x00, 1, 0, 0
        };
        check("mono parses", uac_parse(mono, sizeof mono, &f), 1);
        check("mono channels", f.channels, 1);
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("all 26 checks passed\n");
    return 0;
}
