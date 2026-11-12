/* USB audio, class 1 and 2 playback. The audio control interface says the device is
 * an audio device; the streaming interface's alternate setting carries the
 * format (channels, sample size) and an isochronous OUT endpoint. The samples
 * come from the kernel mixer at 48 kHz; this file chops them into one packet
 * per service interval, keeps a few packets queued on the endpoint, and reaps
 * them as the controller reports them sent. A device that offers both classes
 * uses its class 2 stream. Only the format the mixer produces (stereo, 16-bit,
 * 48 kHz or 44.1 kHz) is used; a device that cannot do either is left alone. */
#ifdef __x86_64__
#include "usb.h"
#include "audio.h"
#include "uac.h"

#define UA_PKTS    64                     /* packets kept in flight (8 ms at HS) */
#define UA_MAXPKT  1024

typedef struct {
    usb_dev_t *dev;                       /* the audio control interface */
    bool started;
    int stream_if, alt, ep, maxpkt, slot_size, interval_exp;
    int rate, channels, frame_bytes;
    int pkt_bytes, pkt_frames;            /* one service interval */
    int hi_speed;
    uint8_t *slots;                       /* UA_PKTS * slot_size of packet data */
    bool used[UA_PKTS];
    uint64_t trb[UA_PKTS];
    int queued;
    uint32_t played, errors, starved;
    bool said, heard;
} ua_t;
static ua_t ua;

static int ua_log2(int v)
{
    int e = 0;
    while (v > 1) { v >>= 1; e++; }
    return e;
}

static int ua_uac2_rate(usb_dev_t *d, int ac, int clock, int wanted)
{
    uint32_t r = (uint32_t)wanted;
    if (!clock) return 0;
    if (usb_control(d, 0x21, 0x01, 0x0100, (uint16_t)(clock | (ac << 8)), &r, 4) == 4) return wanted;
    uint32_t cur = 0;                                       /* not settable: use what it has */
    if (usb_control(d, 0xA1, 0x01, 0x0100, (uint16_t)(clock | (ac << 8)), &cur, 4) == 4 &&
        cur && cur != 0xFFFFFFFFu)
        return (int)cur;
    return 0;
}

/* A packet is one service interval: 1 ms on a full-speed bus, and 2^(bInterval-1)
 * microframes on a high-speed one. */
static void ua_packet_size(void)
{
    int exp = ua_log2(ua.hi_speed && ua.interval_exp ? (1 << ua.interval_exp) : 1);
    int si = ua.hi_speed ? 1 << exp : 8;                 /* microframes per interval */
    if (si > 8) si = 8;
    int pkts_per_ms = 8 / si;
    if (pkts_per_ms < 1) pkts_per_ms = 1;
    ua.pkt_frames = ua.rate / 1000 / pkts_per_ms;
    if (ua.pkt_frames < 1) ua.pkt_frames = 1;
    ua.pkt_bytes = ua.pkt_frames * ua.frame_bytes;
}

bool usb_audio_attach(usb_hcd_t *h, usb_dev_t *tmpl, const uint8_t *cfg, int len)
{
    if (ua.started) return false;                       /* one USB output */
    if (!h->iso_open || !h->iso_send || !h->iso_done) {
        klog("usb-audio: this controller cannot play isochronous audio yet");
        return false;
    }
    int addr = tmpl->addr, speed = tmpl->speed;
    uac_fmt_t f;
    if (!uac_parse(cfg, len, &f)) {
        klog("usb-audio: %04x:%04x: no playback stream in the configuration", tmpl->vendor, tmpl->product);
        usb_diag("audio %04x:%04x: no playback stream", tmpl->vendor, tmpl->product);
        return false;
    }
    int ac = f.ac, stream = f.stream, alt = f.alt, ep = f.ep, maxpkt = f.maxpkt;
    int interval = f.interval, rate = f.rate, ch = f.channels, fb = f.frame_bytes;
    if (f.assumed)
        klog("usb-audio: %04x:%04x: format not readable, assuming 48 kHz stereo 16-bit",
             tmpl->vendor, tmpl->product);
    if (ch != 2 || fb != 2) {
        klog("usb-audio: %d channels of %d bytes is not a format the mixer has", ch, fb);
        usb_diag("audio %04x:%04x: %dch %dB", tmpl->vendor, tmpl->product, ch, fb);
        return false;
    }
    if (f.class2) {                                     /* class 2: set the clock source */
        int r2 = ua_uac2_rate(tmpl, ac, f.clock, 48000);
        if (r2) rate = r2;
        else if (f.assumed) rate = 48000;               /* nothing to ask: use the mixer's rate */
        else {
            klog("usb-audio: the device has no clock source that takes a sample rate");
            usb_diag("audio %04x:%04x: no clock source", tmpl->vendor, tmpl->product);
            return false;
        }
    }
    if (rate != 48000 && rate != 44100) {
        klog("usb-audio: %d Hz is not a rate the mixer has", rate);
        return false;
    }
    bool class2 = f.class2;
    memset(&ua, 0, sizeof ua);
    ua.hi_speed = speed >= 3;
    /* The controller schedules periodic transfers in microframes: a full-speed
     * packet is one per millisecond, which is 8 of them. */
    ua.interval_exp = ua.hi_speed ? ua_log2(interval ? interval : 1) : 3;
    ua.maxpkt = maxpkt > 0 && maxpkt <= UA_MAXPKT ? maxpkt : UA_MAXPKT;
    ua.rate = rate; ua.channels = ch; ua.frame_bytes = ch * fb;
    ua_packet_size();
    if (ua.pkt_bytes > ua.maxpkt) {
        klog("usb-audio: %d-byte packets do not fit a %d-byte endpoint", ua.pkt_bytes, ua.maxpkt);
        return false;
    }

    usb_dev_t *d = tmpl;
    ua.slot_size = (ua.maxpkt + 7) & ~7;
    ua.slots = kmalloc((size_t)UA_PKTS * ua.slot_size);
    if (!ua.slots) return false;
    d->cls = 1; d->subcls = 1; d->iface = ac; d->kind = USB_KIND_AUDIO;

    /* The streaming interface starts at alternate setting 0 (no bandwidth); the
     * one with the format has to be selected before the endpoint exists. */
    if (usb_control(d, 0x01, 11, (uint16_t)alt, (uint16_t)stream, NULL, 0) < 0) {
        klog("usb-audio: the streaming interface would not start");
        kfree(ua.slots); ua.slots = NULL;
        return false;
    }
    if (!class2) {
        uint8_t fr[3] = { (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16) };
        /* The sampling frequency control is addressed to the endpoint; a device
         * that does not take it there sometimes takes it on the interface. */
        int rr = usb_control(d, 0x22, 0x01, 0x0100, (uint16_t)(ep | (stream << 8)), fr, 3);
        if (rr < 0) rr = usb_control(d, 0x21, 0x01, 0x0100, (uint16_t)stream, fr, 3);
        if (rr < 0) klog("usb-audio: the device refused %d Hz, using what it has", rate);
    }
    if (h->iso_open(h, addr, ep, ua.maxpkt, ua.interval_exp) < 0) {
        klog("usb-audio: the isochronous endpoint would not open");
        kfree(ua.slots); ua.slots = NULL;
        return false;
    }
    ua.dev = d;
    ua.stream_if = stream;
    ua.alt = alt;
    ua.ep = ep;
    ua.started = true;
    char name[64];
    /* The mixer treats this as a normal output device; say which USB Audio
     * class it negotiated, since class 2 is the high-speed path. */
    snprintf(name, sizeof name, "USB Audio %s (%s, %d Hz stereo)", class2 ? "2.0" : "1.0",
             ua.hi_speed ? "high speed" : "full speed", rate);
    klog("usb-audio: %s at address %d, %d bytes every %s", name, addr, ua.pkt_bytes,
         ua.hi_speed ? "125 us" : "ms");
    audio_usb_ready(rate, name);
    return true;
}

void usb_audio_detach(usb_dev_t *d)
{
    if (!ua.started || ua.dev != d) return;
    ua.started = false;
    ua.dev = NULL;
    kfree(ua.slots);
    ua.slots = NULL;
    ua.queued = 0;
    memset(ua.used, 0, sizeof ua.used);
    memset(ua.trb, 0, sizeof ua.trb);
    ua.said = false;
    audio_usb_gone();
    klog("usb-audio: the device is gone");
}

/* Keep the stream fed: reap what the controller has sent, then fill the queue
 * with the next packets from the mixer. */
void usb_audio_poll(void)
{
    if (!ua.started || !ua.dev) return;
    for (int i = 0; i < UA_PKTS; i++) {
        if (!ua.trb[i]) continue;
        int r = ua.dev->hcd->iso_done(ua.dev->hcd, ua.dev->addr, ua.ep, ua.trb[i]);
        if (r == 0) continue;
        ua.trb[i] = 0;
        ua.used[i] = false;
        if (ua.queued > 0) ua.queued--;
        if (r < 0) ua.errors++;
        else ua.played++;
    }
    int want = ua.hi_speed ? 60 : 20;                   /* 7.5 ms (HS) or 20 ms (FS) queued */
    while (ua.queued < want) {
        int slot = -1;
        for (int i = 0; i < UA_PKTS; i++) if (!ua.used[i]) { slot = i; break; }
        if (slot < 0) break;
        uint8_t *pkt = ua.slots + (size_t)slot * ua.slot_size;
        audio_usb_pull((int16_t *)pkt, ua.pkt_frames);
        if (!ua.heard) {
            for (int k = 0; k < ua.pkt_bytes; k++)
                if (pkt[k]) { ua.heard = true; klog("usb-audio: sound is flowing"); break; }
        }
        uint64_t trb = ua.dev->hcd->iso_send(ua.dev->hcd, ua.dev->addr, ua.ep, pkt, ua.pkt_bytes);
        if (!trb) { ua.errors++; break; }
        ua.trb[slot] = trb;
        ua.used[slot] = true;
        ua.queued++;
    }
    if (!ua.said && ua.played >= 100) {
        ua.said = true;
        klog("usb-audio: streaming (%u packets sent, %u errors)", ua.played, ua.errors);
    }
    if (!ua.said && ua.errors >= 10) {
        ua.said = true;
        klog("usb-audio: the isochronous stream is failing (%u errors, %u sent)", ua.errors, ua.played);
    }
}

bool usb_audio_present(void) { return ua.started && ua.dev; }

void usb_audio_diag(char *buf, int cap)
{
    if (!ua.started) { snprintf(buf, cap, "USB audio: no stream\n"); return; }
    snprintf(buf, cap, "USB audio: %u packets/s expected, %u sent, %u errors, %d queued, %d bytes each\n",
             ua.pkt_frames ? (unsigned)(ua.rate / ua.pkt_frames) : 0, ua.played, ua.errors, ua.queued, ua.pkt_bytes);
}
#else
#include "usb.h"
void usb_audio_poll(void) {}
bool usb_audio_present(void) { return false; }
void usb_audio_detach(usb_dev_t *d) { (void)d; }
void usb_audio_diag(char *buf, int cap) { if (cap > 0) buf[0] = 0; }
bool usb_audio_attach(usb_hcd_t *h, usb_dev_t *tmpl, const uint8_t *cfg, int len)
{ (void)h; (void)tmpl; (void)cfg; (void)len; return false; }
#endif
