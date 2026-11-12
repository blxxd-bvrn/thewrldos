/* USB audio class 1 and 2: which interface is the playback stream, what format
 * it carries, and (class 2) which clock source sets the rate. Nothing here
 * touches hardware, so tools/uactest checks it with made-up descriptors.
 *
 * The descriptors this walks:
 *   - an audio interface: bInterfaceClass 1, bInterfaceSubClass 1 (control) or
 *     2 (streaming); the streaming one with endpoints is an alternate setting;
 *   - class-specific (0x24) descriptors inside them:
 *       subtype 1: the control interface's header (bcdADC), or the streaming
 *                  interface's header (its bTerminalLink is the terminal fed)
 *       subtype 2: format type I: bFormatType, channels, subframe size, bits,
 *                  and for class 1 a list of 24-bit sample rates
 *       subtype 3: an output terminal (class 2: its clock source is p[8])
 *       subtype 10: a clock source (its ID is p[3])
 *   - an endpoint descriptor (5) that is isochronous (bmAttributes & 3 == 1)
 *     and OUT (address bit 7 clear): the stream's packet size and interval. */
#include "uac.h"

static int uac_rate(const uint8_t *freqs, int n)
{
    int want = 0;
    for (int k = 0; k < n; k++) {
        int r = freqs[k * 3] | (freqs[k * 3 + 1] << 8) | (freqs[k * 3 + 2] << 16);
        if (r == 48000) return r;
        if (!want || r == 44100) want = r;
    }
    return want;
}

/* One alternate setting: its endpoint and its format. */
static bool alt_format(const uint8_t *cfg, int len, int start, uac_fmt_t *o)
{
    const uint8_t *freqs = NULL;
    int nfreq = 0, ft = 0, ch = 0, sub = 0, bits = 0, have = 0;
    o->ep = 0;
    o->link = 0;
    for (int i = start; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
        const uint8_t *p = cfg + i;
        if (p[1] == 4 && i != start) break;                      /* the next interface */
        if (p[1] == 0x24 && p[2] == 1 && p[0] >= 4 && !o->link) o->link = p[3];
        else if (p[1] == 0x24 && p[2] == 2 && p[0] >= 7) {       /* format type I */
            ft = p[3]; ch = p[4]; sub = p[5]; bits = p[6];
            if (p[0] >= 8 && p[7]) { nfreq = p[7]; if (i + 8 + nfreq * 3 <= len) freqs = p + 8; }
            have = 1;
        } else if (p[1] == 5 && p[0] >= 7) {
            int a = p[2];
            if ((p[3] & 3) == 1 && !(a & 0x80) && !o->ep) {
                o->ep = a & 0x0F;
                o->maxpkt = p[4] | (p[5] << 8);
                o->interval = p[6];
            }
        }
    }
    if (!o->ep || !have || ft != 1 || ch < 1 || sub < 1 || bits < 1 || bits > sub * 8) return false;
    o->channels = ch;
    o->frame_bytes = sub;
    o->rate = 0;
    if (freqs) {                                                 /* class 1 lists its rates */
        o->rate = uac_rate(freqs, nfreq);
        return o->rate != 0;
    }
    o->class2 = true;                                            /* class 2: the clock sets it */
    return true;
}

/* When a streaming interface has an isochronous OUT endpoint but no format
 * descriptor this parser understands, still try it as 48 kHz stereo 16-bit:
 * a device is better used with an assumed format than not at all. */
static bool alt_loose(const uint8_t *cfg, int len, int start, uac_fmt_t *o)
{
    o->ep = 0;
    o->link = 0;
    for (int i = start; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
        const uint8_t *p = cfg + i;
        if (p[1] == 4 && i != start) break;
        if (p[1] == 0x24 && p[2] == 1 && p[0] >= 4 && !o->link) o->link = p[3];
        else if (p[1] == 5 && p[0] >= 7) {
            int a = p[2];
            if ((p[3] & 3) == 1 && !(a & 0x80) && !o->ep) {
                o->ep = a & 0x0F;
                o->maxpkt = p[4] | (p[5] << 8);
                o->interval = p[6];
            }
        }
    }
    if (!o->ep) return false;
    o->channels = 2;
    o->frame_bytes = 2;
    o->rate = 0;
    o->class2 = true;
    o->assumed = true;
    return true;
}

/* The clock a class 2 stream takes its rate from: the clock of the output
 * terminal it is linked to, else the first terminal's, else the first clock
 * source. */
static int clock_of(const uint8_t *cfg, int len, int link)
{
    int clock = 0, term = 0, first = 0;
    for (int i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
        const uint8_t *p = cfg + i;
        if (p[1] != 0x24 || p[0] < 4) continue;
        if (p[2] == 10 && !clock) clock = p[3];                  /* clock source */
        else if (p[2] == 3 && p[0] >= 12) {                      /* output terminal */
            if (!first) first = p[8];
            if (link && p[3] == link && !term) term = p[8];
        }
    }
    return term ? term : first ? first : clock;
}

bool uac_parse(const uint8_t *cfg, int len, uac_fmt_t *o)
{
    for (int i = 0; i < (int)sizeof *o; i++) ((uint8_t *)o)[i] = 0;
    o->ac = -1;
    o->stream = -1;
    for (int i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
        const uint8_t *p = cfg + i;
        if (p[1] != 4 || p[0] < 9 || p[5] != 1) continue;        /* not an audio interface */
        if (p[6] == 1 && o->ac < 0) { o->ac = p[2]; continue; }
        if (p[6] != 2 || p[4] == 0) continue;                     /* streaming with endpoints */
        uac_fmt_t one = *o;
        if (!alt_format(cfg, len, i, &one)) continue;
        one.stream = p[2];
        one.alt = p[3];
        /* Prefer a class 2 stream when the device offers both: it carries
         * 24-bit/high-speed formats and is the current standard. */
        if (o->stream < 0 || (one.class2 && !o->class2)) *o = one;
    }
    if (o->ac >= 0 && o->stream < 0) {          /* a stream we could not read: assume the usual */
        for (int i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i]) {
            const uint8_t *p = cfg + i;
            if (p[1] != 4 || p[0] < 9 || p[5] != 1 || p[6] != 2 || p[4] == 0) continue;
            uac_fmt_t one = *o;
            if (!alt_loose(cfg, len, i, &one)) continue;
            one.stream = p[2];
            one.alt = p[3];
            *o = one;
        }
    }
    if (o->ac < 0 || o->stream < 0 || !o->ep) return false;
    if (o->class2) o->clock = clock_of(cfg, len, o->link);
    return true;
}