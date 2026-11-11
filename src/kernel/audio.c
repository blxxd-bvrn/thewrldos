/* Sound: Intel High Definition Audio and AC'97 drivers, a small software mixer
 * and a WAV loader. Output is always 48 kHz, 16-bit stereo. A ring of DMA
 * buffers is kept filled ~170 ms ahead of the hardware from the timer
 * interrupt, so a busy desktop (or a slow VM) cannot starve it.
 *   - Intel High Definition Audio (any vendor's controller): QEMU (intel-hda,
 *     ich9-intel-hda), VirtualBox (default), VMware, real PCs
 *   - AC'97 with the Intel ICH bus master: Intel, nVidia nForce, AMD, SiS;
 *     QEMU (AC97), VirtualBox (ICH AC97)
 *   - Ensoniq AudioPCI ES1370 (QEMU es1370) and ES1371/ES1373/CT5880
 *     (VMware's "Ensoniq AudioPCI")
 *   - Creative Sound Blaster 16 (ISA, 16-bit auto-init DMA): QEMU (sb16),
 *     VirtualBox (SoundBlaster 16)
 *   - USB audio class 1 and 2 playback, paced by the device's packets
 * When no audio controller is on the bus (only a USB headset is present) the
 * mixer also exposes a software HDA controller whose stream feeds the USB
 * packets.
 * The output rate is 48 kHz, or 44.1 kHz on cards that cannot do 48. */
#include "kernel.h"
#include "audio.h"
#include "usb.h"

static uint32_t out_rate = 48000;            /* the mixer's (and the card's) sample rate */
#define RATE out_rate
#define NBUF 32
#define BUFFRAMES 512                     /* 10.7 ms per buffer */
#define BUFBYTES (BUFFRAMES * 4)
#define LEAD 16                           /* buffers kept ahead of the hardware */
#define NVOICE 4

enum { DEV_NONE, DEV_HDA, DEV_AC97, DEV_ES1370, DEV_ES1371, DEV_SB16, DEV_USB };
static int dev;
static char hda_why[96];                     /* why the HDA probe gave up (shown on the log-on screen) */
static char devname[80] = "No sound device";
static int16_t *ring;                     /* NBUF * BUFFRAMES stereo frames */
static int next_fill;
int audio_volume = 80;
bool audio_muted;

/* Playback endpoints shown by the Sound page: the card's own output pins, the
 * USB device, and (on a soft HDA controller) the virtual jacks. */
#define MAX_EP 10
#define MAX_PINS 8
static struct { int nid, devtype; bool digital; } hda_pins[MAX_PINS];
static int hda_npins, hda_cad;
static struct { char name[56]; int kind, pin, devtype; } eps[MAX_EP];
static int neps, ep_sel;
enum { EP_CARD, EP_HDA, EP_USB, EP_VJACK };

typedef struct { const sound_t *s; uint32_t pos; bool active; } voice_t;
static voice_t voices[NVOICE];
void audio_disable(const char *why);

/* ======================= Intel HDA ======================= */
static volatile uint8_t *hda;
static uint32_t *corb; static uint64_t *rirb;
static uint16_t rirb_rp;
static uint32_t sd;                       /* output stream descriptor offset */
static bool use_immediate;
static volatile uint32_t *posbuf;
static int sd_index;                      /* stream descriptor number (for the position buffer) */
#define R8(o) (*(volatile uint8_t *)(hda + (o)))
#define R16(o) (*(volatile uint16_t *)(hda + (o)))
#define R32(o) (*(volatile uint32_t *)(hda + (o)))
static void spin(uint32_t us) { for (uint32_t i = 0; i < us; i++) io_wait(); }
static bool hda_dead;                     /* neither command interface answers */
static uint32_t imm_cmd(uint32_t c, bool *ok) {
    for (int i = 0; i < 10000 && (R16(0x68) & 1); i++) spin(1);
    R32(0x60) = c; R16(0x68) = 2; R16(0x68) = 1;           /* clear "result valid", then send */
    for (int i = 0; i < 20000; i++) { if (R16(0x68) & 2) { R16(0x68) = 2; *ok = true; return R32(0x64); } spin(1); }
    *ok = false;
    return 0xFFFFFFFF;
}
static uint32_t corb_cmd(uint32_t c, bool *ok) {
    uint16_t wp = (R16(0x48) + 1) & 0xFF;
    corb[wp] = c;
    __asm__ volatile("" ::: "memory");
    R16(0x48) = wp;
    for (int i = 0; i < 20000; i++) {
        uint16_t w = R16(0x58) & 0xFF;
        if (w != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 0xFF;
            uint64_t e = rirb[rirb_rp];
            R8(0x5D) = 0x05;                                   /* ack RIRB status so the controller keeps going */
            if ((e >> 32) & 0x10) { i = 0; continue; }         /* unsolicited response: not ours */
            *ok = true;
            return (uint32_t)e;
        }
        spin(1);
    }
    *ok = false;
    return 0xFFFFFFFF;
}
static uint32_t hda_cmd(int cad, int nid, uint32_t verb_payload) {
    uint32_t c = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | (verb_payload & 0xFFFFF);
    bool ok = false;
    uint32_t r;
    if (hda_dead) return 0;
    if (!use_immediate) {
        r = corb_cmd(c, &ok);
        if (ok) return r;
        /* some emulations (and chips) stop answering through the rings: use the immediate interface */
        klog("hda: CORB/RIRB stopped answering, switching to immediate commands");
        R8(0x4C) = 0; R8(0x5C) = 0;
        use_immediate = true;
    }
    r = imm_cmd(c, &ok);
    if (!ok) { klog("hda: the codec does not answer commands"); hda_dead = true; return 0; }
    return r;
}
#define VERB(v, p) (((uint32_t)(v) << 8) | (p))         /* 12-bit verb, 8-bit payload */
#define VERB4(v, p) (((uint32_t)(v) << 16) | (p))       /* 4-bit verb, 16-bit payload */
static uint32_t param(int cad, int nid, int p) { return hda_cmd(cad, nid, VERB(0xF00, p)); }

typedef struct { uint32_t caps; uint8_t type; uint8_t nconn; uint8_t conn[16]; } widget_t;
static widget_t wg[128];
static int wstart, wcount;
static widget_t *W(int nid) { return nid >= wstart && nid < wstart + wcount ? &wg[nid - wstart] : NULL; }
static int path[8], plen;
static uint32_t afg_in_amp;                  /* the function group's default input amp caps */
/* `digital` picks a display-audio converter (HDMI/DP) instead of an analog one. */
static bool find_dac_kind(int cad, int nid, int depth, bool digital) {
    widget_t *w = W(nid);
    if (!w || depth > 6) return false;
    path[depth] = nid; plen = depth + 1;
    if (w->type == 0)                                     /* audio output converter */
        return ((w->caps & (1 << 9)) != 0) == digital;
    if (w->type != 2 && w->type != 3 && w->type != 4) return false;   /* only mixers, selectors, pins lead to DACs */
    for (int i = 0; i < w->nconn; i++) {
        if (find_dac_kind(cad, w->conn[i], depth + 1, digital)) {
            if (w->type != 2 && w->nconn > 1) hda_cmd(cad, nid, VERB(0x701, i));   /* select that input */
            if (w->type == 2 && (w->caps & (1 << 1))) {                           /* mixer input amp: that input at 0 dB, unmuted */
                uint32_t ic = (w->caps & (1 << 3)) ? param(cad, nid, 0x0D) : afg_in_amp;
                int g = (ic & 0x7F) ? (ic & 0x7F) : ((ic >> 8) & 0x7F);
                hda_cmd(cad, nid, VERB4(0x3, 0x7000 | (i << 8) | g));
            }
            return true;
        }
    }
    return false;
}
static bool find_dac(int cad, int nid, int depth) { return find_dac_kind(cad, nid, depth, false); }
static void amp_unmute(int cad, int nid, uint32_t afg_ampcaps) {
    widget_t *w = W(nid);
    if (!w || !(w->caps & (1 << 2))) return;              /* no output amplifier */
    uint32_t caps = (w->caps & (1 << 3)) ? param(cad, nid, 0x12) : afg_ampcaps;
    int steps = (caps >> 8) & 0x7F, offset = caps & 0x7F;
    int gain = offset ? offset : steps;                     /* 0 dB if known, else the top of the range */
    hda_cmd(cad, nid, VERB4(0x3, 0xB000 | gain));           /* output amp, left + right, unmuted */
}
char hda_diag[600];
static void diag(const char *fmt, ...) {
    size_t n = strlen(hda_diag);
    va_list ap; va_start(ap, fmt);
    vsnprintf(hda_diag + n, sizeof hda_diag - n, fmt, ap);
    va_end(ap);
}
static const char *pin_kind(int devtype) {
    static const char *k[16] = { "line out", "speaker", "headphones", "CD", "S/PDIF out", "digital out", "modem line", "modem handset",
                                 "line in", "aux", "mic", "telephony", "S/PDIF in", "digital in", "other", "other" };
    return k[devtype & 15];
}
/* Read one audio function group's widgets and route its output pins to analog
 * DACs. Jacks the codec reports as connected come first (line out, speaker,
 * headphones, then the rest); if it reports none, every output-capable pin.
 * Returns the number of DACs found (0 when this group has no analog output). */
static int route_out(int cad, int afg, int *dacs, int *pins_out, bool *digital_out)
{
    hda_npins = 0;
    hda_cad = cad;
    hda_cmd(cad, afg, VERB(0x705, 0));                      /* power D0 */
    for (int t = 0; t < 20 && ((hda_cmd(cad, afg, VERB(0xF05, 0)) >> 4) & 0xF) != 0; t++) sleep_ms(5);
    uint32_t afg_amp = param(cad, afg, 0x12);
    afg_in_amp = param(cad, afg, 0x0D);
    uint32_t sub = param(cad, afg, 0x04);
    wstart = (sub >> 16) & 0xFF; wcount = MIN((int)(sub & 0xFF), 128);
    for (int i = 0; i < wcount; i++) {
        int nid = wstart + i; widget_t *w = &wg[i];
        w->caps = param(cad, nid, 0x09); w->type = (w->caps >> 20) & 0xF;
        uint32_t cl = param(cad, nid, 0x0E); int n = cl & 0x7F; bool lng = cl & 0x80;
        w->nconn = 0;
        if ((w->caps & (1 << 8)) && n && !lng)
            for (int o = 0; o < n && w->nconn < 16; o += 4) {
                uint32_t e = hda_cmd(cad, nid, VERB(0xF02, o));
                for (int b = 0; b < 4 && o + b < n && w->nconn < 16; b++) w->conn[w->nconn++] = (e >> (8 * b)) & 0xFF;
            }
        if (w->caps & (1 << 10)) hda_cmd(cad, nid, VERB(0x705, 0));   /* widgets with power control: D0 */
    }
    int ndacs = 0, pins = 0;
    bool digital = false;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 2 && pins) break;
        for (int i = 0; i < wcount; i++) {
            widget_t *w = &wg[i]; int nid = wstart + i;
            if (w->type != 4) continue;
            uint32_t pcaps = param(cad, nid, 0x0C), cfg = hda_cmd(cad, nid, VERB(0xF1C, 0));
            int conn = cfg >> 30, devtype = (cfg >> 20) & 0xF;
            if (!(pcaps & (1 << 4))) continue;                        /* not output capable */
            if (devtype == 4 || devtype == 5 || (w->caps & (1 << 9))) continue;   /* digital */
            bool preferred = devtype <= 2;
            if (pass == 0 && (conn == 1 || !preferred)) continue;
            if (pass == 1 && (conn == 1 || preferred)) continue;
            if (pass == 2 && conn != 1) continue;
            plen = 0;
            if (!find_dac(cad, nid, 0)) continue;
            int d = path[plen - 1], k;
            for (k = 0; k < ndacs && dacs[k] != d; k++) {}
            if (k == ndacs && ndacs < 8) dacs[ndacs++] = d;
            pins++;
            for (int j = 0; j < plen; j++) amp_unmute(cad, path[j], afg_amp);
            hda_cmd(cad, nid, VERB(0x707, 0x40 | (devtype == 2 ? 0x80 : 0)));   /* output enable (+ headphone amp) */
            if (pcaps & (1 << 16)) hda_cmd(cad, nid, VERB(0x70C, 2));        /* external amplifier (EAPD) on */
            diag("pin %d (%s%s) -> DAC %d\n", nid, pin_kind(devtype), conn == 1 ? ", reported unconnected" : "", d);
            if (hda_npins < MAX_PINS) { hda_pins[hda_npins].nid = nid; hda_pins[hda_npins].devtype = devtype; hda_pins[hda_npins].digital = false; hda_npins++; }
        }
    }
    if (!ndacs)   /* no pin path: use the first analog DAC anyway */
        for (int i = 0; i < wcount; i++)
            if (wg[i].type == 0 && !(wg[i].caps & (1 << 9))) {
                dacs[ndacs++] = wstart + i;
                amp_unmute(cad, wstart + i, afg_amp);
                break;
            }
    /* A display-audio codec (HDMI/DP) has only digital converters and pins. Use
     * one when there is no analog output on this function group: a monitor's
     * speakers or its headphone jack are the output then. */
    if (!ndacs)
        for (int i = 0; i < wcount && !ndacs; i++) {
            widget_t *w = &wg[i]; int nid = wstart + i;
            if (w->type != 4) continue;
            uint32_t pcaps = param(cad, nid, 0x0C), cfg = hda_cmd(cad, nid, VERB(0xF1C, 0));
            int devtype = (cfg >> 20) & 0xF;
            if (!(pcaps & (1 << 4))) continue;
            if (devtype != 4 && devtype != 5 && !(w->caps & (1 << 9))) continue;
            plen = 0;
            if (!find_dac_kind(cad, nid, 0, true)) continue;
            int d = path[plen - 1], k;
            for (k = 0; k < ndacs && dacs[k] != d; k++) {}
            if (k == ndacs && ndacs < 8) dacs[ndacs++] = d;
            pins++;
            digital = true;
            for (int j = 0; j < plen; j++) amp_unmute(cad, path[j], afg_amp);
            hda_cmd(cad, nid, VERB(0x707, 0x40));                /* output enable */
            diag("pin %d (%s) -> digital DAC %d\n", nid, pin_kind(devtype), d);
            if (hda_npins < MAX_PINS) { hda_pins[hda_npins].nid = nid; hda_pins[hda_npins].devtype = devtype; hda_pins[hda_npins].digital = true; hda_npins++; }
        }
    if (!ndacs)   /* a display converter with no pin */
        for (int i = 0; i < wcount; i++)
            if (wg[i].type == 0 && (wg[i].caps & (1 << 9))) {
                dacs[ndacs++] = wstart + i;
                amp_unmute(cad, wstart + i, afg_amp);
                digital = true;
                break;
            }
    if (digital_out) *digital_out = digital;
    *pins_out = pins;
    return ndacs;
}

static bool hda_init(pci_dev_t *p) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x06);
    if (p->vendor != 0x1002 && p->vendor != 0x1022) {        /* TCSEL = TC0 (as Linux does; avoids static on some codecs) */
        uint32_t v = pci_cfg_read(p->bus, p->dev, p->fn, 0x44);
        pci_cfg_write(p->bus, p->dev, p->fn, 0x44, v & ~7u);
    }
    uint32_t bar = pci_cfg_read(p->bus, p->dev, p->fn, 0x10) & ~0xFu;
    if (!bar) return false;
    paging_set_uncached(bar);
    hda = (volatile uint8_t *)(uintptr_t)bar;
    hda_diag[0] = 0;
    diag("controller %04x:%04x\n", p->vendor, p->device);
    /* controller reset */
    R8(0x4C) = 0; R8(0x5C) = 0;                              /* stop the rings before resetting */
    R32(0x08) &= ~1u;
    for (int i = 0; i < 1000 && (R32(0x08) & 1); i++) spin(10);
    spin(100);
    R32(0x08) |= 1;
    for (int i = 0; i < 1000 && !(R32(0x08) & 1); i++) spin(10);
    uint16_t codecs = 0;
    for (int t = 0; t < 10 && !(codecs = R16(0x0E)); t++) sleep_ms(10);   /* codecs report in within ~1 ms; allow 100 */
    if (!codecs) { strlcpy(hda_why, "HDA controller found, no codec answered", sizeof hda_why); klog("hda: no codec found"); return false; }
    R32(0x20) = 0;                                          /* no interrupts: we poll */
    /* CORB / RIRB rings */
    corb = kmalloc_aligned(1024, 128); memset(corb, 0, 1024);
    rirb = kmalloc_aligned(2048, 128); memset(rirb, 0, 2048);
    spin(100);
    R32(0x40) = (uint32_t)(uintptr_t)corb; R32(0x44) = 0;
    R8(0x4E) = (R8(0x4E) & 0xF0) | 2;
    R16(0x4A) = 0x8000; for (int i = 0; i < 1000 && !(R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x4A) = 0; for (int i = 0; i < 1000 && (R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x48) = 0;
    R32(0x50) = (uint32_t)(uintptr_t)rirb; R32(0x54) = 0;
    R8(0x5E) = (R8(0x5E) & 0xF0) | 2;
    R16(0x58) = 0x8000; rirb_rp = 0;
    /* As Linux does: a response "interrupt" after every response, acknowledged in
     * corb_cmd. Controllers count responses and stop taking commands when the count
     * is reached until the flag is acknowledged, and the flag only exists with RIRB
     * interrupts enabled (no CPU interrupt: INTCTL stays 0). Without this the codec
     * stops answering after RINTCNT responses: enough for QEMU's small codec, not for
     * big ones like VMware's Realtek-style codec. */
    R16(0x5A) = 1;
    R8(0x4C) = 2; R8(0x5C) = 3;                              /* CORB run; RIRB DMA + response flag */
    /* A codec can be an analog one, an HDMI/DP one, or both on different
     * function groups. Try every codec present and every audio function group
     * in it, and use the first that has an analog output converter: the first
     * codec is not necessarily the one the speakers are on. */
    int cad = -1, afg = 0, dacs[8], ndacs = 0, pins = 0;
    bool chosen_digital = false;
    uint32_t vid = 0;
    bool queried = false;
    for (int c = 0; c < 15 && !ndacs; c++) {
        if (!(codecs & (1 << c))) continue;
        uint32_t v = param(c, 0, 0x00);
        if (!queried) {
            queried = true;
            if (v == 0xFFFFFFFF || !v) {                     /* rings answer nonsense: immediate commands */
                R8(0x4C) = 0; R8(0x5C) = 0; use_immediate = true; hda_dead = false;
                v = param(c, 0, 0x00);
            }
        }
        if (hda_dead || !v || v == 0xFFFFFFFF) continue;
        diag("codec %d: %08x, %s commands\n", c, v, use_immediate ? "immediate" : "CORB/RIRB");
        uint32_t sub = param(c, 0, 0x04);
        for (int n = (sub >> 16) & 0xFF, k = 0; k < (int)(sub & 0xFF); k++, n++) {
            if ((param(c, n, 0x05) & 0xFF) != 1) continue;   /* audio function group */
            bool digital = false;
            int nd = route_out(c, n, dacs, &pins, &digital);
            if (nd) { ndacs = nd; cad = c; afg = n; vid = v; chosen_digital = digital; break; }
        }
    }
    if (!ndacs) {
        char ids[64]; int len = 0;
        ids[0] = 0;
        for (int c = 0; c < 15 && len < (int)sizeof ids - 10; c++) {
            if (!(codecs & (1 << c))) continue;
            uint32_t v = param(c, 0, 0x00);
            if (!v || v == 0xFFFFFFFFu) continue;
            len += snprintf(ids + len, sizeof ids - len, "%s%d:%08x", len ? " " : "", c, v);
        }
        snprintf(hda_why, sizeof hda_why, "no analog output (codecs %s)", ids[0] ? ids : "none");
        klog("hda: no codec with an analog output converter (%s)", ids[0] ? ids : "none");
        return false;
    }
    diag("using codec %d, function group %d: %d DAC(s), %d pin(s)\n", cad, afg, ndacs, pins);

    /* output stream 1 -> every DAC used */
    for (int k = 0; k < ndacs; k++) {
        hda_cmd(cad, dacs[k], VERB(0x706, 1 << 4));         /* stream 1, channel 0 */
        hda_cmd(cad, dacs[k], VERB4(0x2, 0x0011));          /* 48 kHz, 16-bit, 2 channels */
    }
    uint16_t gcap = R16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { strlcpy(hda_why, "HDA controller has no output streams", sizeof hda_why); klog("hda: no output streams"); return false; }
    sd = 0x80 + iss * 0x20;
    R8(sd) = 0;                                              /* stop */
    R8(sd) |= 1; for (int i = 0; i < 1000 && !(R8(sd) & 1); i++) spin(1);
    R8(sd) &= ~1; for (int i = 0; i < 1000 && (R8(sd) & 1); i++) spin(1);
    R8(sd + 3) = 0x1C;                                       /* clear stream status */
    uint32_t *bdl = kmalloc_aligned(NBUF * 16, 128);
    for (int i = 0; i < NBUF; i++) { bdl[i * 4] = (uint32_t)(uintptr_t)(ring + i * BUFFRAMES * 2); bdl[i * 4 + 1] = 0; bdl[i * 4 + 2] = BUFBYTES; bdl[i * 4 + 3] = 0; }
    R32(sd + 0x08) = NBUF * BUFBYTES;
    R16(sd + 0x0C) = NBUF - 1;
    R16(sd + 0x12) = 0x0011;
    R32(sd + 0x18) = (uint32_t)(uintptr_t)bdl; R32(sd + 0x1C) = 0;
    R8(sd + 2) = (R8(sd + 2) & 0x0F) | (1 << 4);             /* stream tag 1 */
    sd_index = iss;
    posbuf = kmalloc_aligned(256, 128); memset((void *)posbuf, 0, 256);
    R32(0x74) = 0; R32(0x70) = (uint32_t)(uintptr_t)posbuf | 1;   /* DMA position buffer on */
    R8(sd) |= 2;                                             /* run */
    /* is the DMA engine moving? */
    uint32_t l0 = R32(sd + 0x04), p0 = posbuf[sd_index * 2];
    sleep_ms(40);
    uint32_t l1 = R32(sd + 0x04), p1 = posbuf[sd_index * 2];
    bool moving = l1 != l0 || p1 != p0;
    diag("start: LPIB %x -> %x, position buffer %x -> %x\n", l0, l1, p0, p1);
    diag("output stream %d (registers at 0x%x), DMA %s\n", iss + 1, sd, moving ? "running" : "NOT moving");
    snprintf(devname, sizeof devname, "%s (%04x:%04x, codec %08x)", chosen_digital ? "HDMI/DP audio" : "High Definition Audio", p->vendor, p->device, vid);
    klog("hda: %s, %d DAC(s), %d output pin(s)%s, DMA %s", devname, ndacs, pins, use_immediate ? ", immediate commands" : "", moving ? "running" : "not moving");
    return true;
}
/* Where is the stream? Two sources: the link position register (LPIB) and the
 * DMA position buffer the controller writes to memory (what Linux prefers).
 * A source counts only while it reads inside the buffer; if both are valid,
 * one that has stopped changing loses to one that moves. */
static int pos_src = -1;                  /* 0 LPIB, 1 position buffer, -1 none (timer estimate) */
static uint32_t last_lp = ~0u, last_pb = ~0u, lp_tick, pb_tick;
static int hda_position(void) {
    uint32_t cbl = NBUF * BUFBYTES, now = timer_ticks();
    uint32_t lp = R32(sd + 0x04), pb = posbuf ? posbuf[sd_index * 2] : ~0u;
    if (lp != last_lp) { last_lp = lp; lp_tick = now; }
    if (pb != last_pb) { last_pb = pb; pb_tick = now; }
    bool lp_ok = lp < cbl, pb_ok = pb < cbl;
    bool lp_live = lp_ok && now - lp_tick < 100, pb_live = pb_ok && now - pb_tick < 100;
    if (lp_ok && (lp_live || !pb_live)) { pos_src = 0; return lp / BUFBYTES; }
    if (pb_ok) { pos_src = 1; return pb / BUFBYTES; }
    pos_src = -1;
    return -1;
}
static void hda_regs(char *b, int cap) {
    snprintf(b, cap, "SDCTL %06x STS %02x LPIB %x posbuf %x CBL %x (from %s)\n",
             R32(sd) & 0xFFFFFF, R8(sd + 3), R32(sd + 0x04), posbuf ? posbuf[sd_index * 2] : 0, R32(sd + 0x08),
             pos_src == 0 ? "LPIB" : pos_src == 1 ? "posbuf" : "timer");
}

/* ======================= AC'97 ======================= */
static uint16_t nam, nabm;
static uint32_t *ac_bdl;
static bool ac97_init(pci_dev_t *p) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x05);
    nam = pci_cfg_read(p->bus, p->dev, p->fn, 0x10) & ~1u;
    nabm = pci_cfg_read(p->bus, p->dev, p->fn, 0x14) & ~1u;
    if (!nam || !nabm) return false;
    outl(nabm + 0x2C, 0x00000002);                           /* leave cold reset */
    sleep_ms(50);
    for (int i = 0; i < 100 && !(inl(nabm + 0x30) & 0x100); i++) sleep_ms(2);
    outw(nam + 0x00, 0);                                     /* reset mixer */
    outw(nam + 0x26, 0);                                     /* power up every section */
    for (int i = 0; i < 100 && (inw(nam + 0x26) & 0x0F) != 0x0F; i++) sleep_ms(1);
    outw(nam + 0x02, 0x0000);                                /* master: 0 dB, unmuted */
    outw(nam + 0x04, 0x0000);                                /* headphones */
    outw(nam + 0x18, 0x0808);                                /* PCM out */
    if (inw(nam + 0x28) & 1) { outw(nam + 0x2A, inw(nam + 0x2A) | 1); outw(nam + 0x2C, RATE); }   /* variable rate: pin to 48 kHz */
    outb(nabm + 0x1B, 0x02);                                 /* reset PCM-out box */
    for (int i = 0; i < 1000 && (inb(nabm + 0x1B) & 2); i++) io_wait();
    ac_bdl = kmalloc_aligned(NBUF * 8, 8);
    for (int i = 0; i < NBUF; i++) { ac_bdl[i * 2] = (uint32_t)(uintptr_t)(ring + i * BUFFRAMES * 2); ac_bdl[i * 2 + 1] = BUFFRAMES * 2; }
    outl(nabm + 0x10, (uint32_t)(uintptr_t)ac_bdl);
    static const struct { uint16_t v; const char *n; } vn[] = { { 0x8086, "Intel" }, { 0x10DE, "nVidia nForce" }, { 0x1022, "AMD" }, { 0x1039, "SiS" } };
    const char *vend = "";
    for (unsigned i = 0; i < ARRAY_LEN(vn); i++) if (vn[i].v == p->vendor) vend = vn[i].n;
    snprintf(devname, sizeof devname, "%s%sAC'97 Audio Controller (%04x:%04x)", vend, *vend ? " " : "", p->vendor, p->device);
    klog("ac97: %s, mixer %x, bus master %x", devname, nam, nabm);
    return true;
}
static int ac97_position(void) { return inb(nabm + 0x14) & (NBUF - 1); }


/* ======================= Ensoniq AudioPCI (ES1370, ES1371/ES1373) =======================
 * DAC2 plays our ring as one looping "frame"; its position is read from the
 * frame count register (page 0x0C, offset 0x3C, upper half in longwords). */
static uint16_t es_io;
#define ES_CTRL 0x00
#define ES_STATUS 0x04
#define ES_PAGE 0x0C
#define ES1370_CODEC 0x10
#define ES1371_SRC 0x10
#define ES1371_CODEC 0x14
#define ES1371_LEGACY 0x18
#define ES_SCTRL 0x20
#define ES_DAC2_COUNT 0x28
#define ES_DAC2_FRAMEADR 0x38
#define ES_DAC2_FRAMECNT 0x3C
#define ES_DAC2_EN 0x20
#define ES_SCTRL_P2_16BIT 0x08
#define ES_SCTRL_P2_STEREO 0x04
#define ES_SCTRL_P2_INT_EN (1u << 9)
#define ES_STATUS_DAC2 (1u << 1)
static uint32_t es_ctrl, es_sctrl;
static uint32_t es_restarts, es_acks;
/* DAC2 plays the whole ring in a loop. Its sample counter is set to the whole ring
 * and its end-of-count interrupt enabled (as Linux does) but acknowledged by
 * polling (es_service): VMware's ES1371 stops at the end of a count until that
 * interrupt has been acknowledged. The PCI interrupt line itself is disabled. */
static void es_start_dac2(uint32_t ctrl_bits) {
    es_ctrl = ctrl_bits;
    outl(es_io + ES_CTRL, es_ctrl);                        /* DAC2 off while it is set up */
    outl(es_io + ES_PAGE, 0x0C);
    outl(es_io + ES_DAC2_FRAMEADR, (uint32_t)(uintptr_t)ring);
    outl(es_io + ES_DAC2_FRAMECNT, NBUF * BUFBYTES / 4 - 1);
    es_sctrl = ES_SCTRL_P2_16BIT | ES_SCTRL_P2_STEREO | (2u << 19) | ES_SCTRL_P2_INT_EN;   /* end increment 2 for 16-bit */
    outl(es_io + ES_SCTRL, es_sctrl);
    outl(es_io + ES_DAC2_COUNT, NBUF * BUFFRAMES - 1);     /* one end-of-count per trip round the ring */
    outl(es_io + ES_CTRL, es_ctrl | ES_DAC2_EN);
}
static void es_service(void) {                            /* timer interrupt: acknowledge DAC2's end of count */
    if (inl(es_io + ES_STATUS) & ES_STATUS_DAC2) {
        outl(es_io + ES_SCTRL, es_sctrl & ~ES_SCTRL_P2_INT_EN);
        outl(es_io + ES_SCTRL, es_sctrl);
        es_acks++;
    }
}
static uint32_t es_raw;
static int es_position(void) {
    outl(es_io + ES_PAGE, 0x0C);
    uint32_t v = inl(es_io + ES_DAC2_FRAMECNT);
    es_raw = v;
    uint32_t bytes = (v >> 16) * 4;
    if (bytes >= NBUF * BUFBYTES) return -1;
    return bytes / BUFBYTES;
}
static void es1370_codec(uint8_t reg, uint8_t val) {
    for (int i = 0; i < 2000 && (inl(es_io + ES_STATUS) & 0x400); i++) io_wait();   /* codec write in progress */
    outw(es_io + ES1370_CODEC, (reg << 8) | val);
}
static bool es1370_init(pci_dev_t *p) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x05 | 0x400);   /* I/O, bus master, INTx off: we poll */
    uint32_t bar = pci_cfg_read(p->bus, p->dev, p->fn, 0x10);
    if (!(bar & 1)) return false;
    es_io = bar & ~3u;
    out_rate = 44100;                                      /* DAC2 clock: 1411200 / (div + 2) */
    uint32_t ctrl = 0x02 | (30u << 16);                    /* codec enable, divider 30 = 44100 Hz */
    outl(es_io + ES_CTRL, ctrl); outl(es_io + ES_SCTRL, 0);
    /* AK4531 codec: out of reset, master and voice (wave) at 0 dB, voice routed to the output */
    es1370_codec(0x16, 0x03); sleep_ms(2); es1370_codec(0x17, 0x00);
    es1370_codec(0x00, 0x00); es1370_codec(0x01, 0x00);
    es1370_codec(0x02, 0x06); es1370_codec(0x03, 0x06);
    es1370_codec(0x10, 0x00); es1370_codec(0x11, 0x0C);
    es_start_dac2(ctrl);
    snprintf(devname, sizeof devname, "Ensoniq AudioPCI ES1370 (%04x:%04x)", p->vendor, p->device);
    klog("es1370: %s at %x, 44.1 kHz", devname, es_io);
    return true;
}
/* ES1371/ES1373: an AC'97 codec plus a sample rate converter in front of the DACs */
#define SRC_RAM_WE (1u << 24)
#define SRC_RAM_BUSY (1u << 23)
#define SRC_DISABLE (1u << 22)
#define SRC_DIS_P1 (1u << 21)
#define SRC_DIS_P2 (1u << 20)
#define SRC_DIS_R1 (1u << 19)
#define SRC_KEEP (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_P2 | SRC_DIS_R1)
#define SMP_DAC1 0x70
#define SMP_DAC2 0x74
#define SMP_ADC 0x78
#define SMP_VOL_ADC 0x6C
#define SMP_VOL_DAC1 0x7C
#define SMP_VOL_DAC2 0x7E
#define SMP_TRUNC_N 0x00
#define SMP_INT_REGS 0x01
#define SMP_VFREQ_FRAC 0x03
static uint32_t src_ready(void) {
    uint32_t v = 0;
    for (int i = 0; i < 10000; i++) { v = inl(es_io + ES1371_SRC); if (!(v & SRC_RAM_BUSY)) break; io_wait(); }
    return v;
}
static void src_write(uint8_t addr, uint16_t data) {
    uint32_t r = src_ready() & SRC_KEEP;
    outl(es_io + ES1371_SRC, r | ((uint32_t)(addr & 0x7F) << 25) | SRC_RAM_WE | data);
}
/* AC'97 codec write, the way Linux does it: wait for the codec interface, then
 * write while the sample rate converter is in a safe state (writes at other
 * moments can be lost on real chips) */
static void es1371_codec(uint8_t reg, uint16_t val) {
    for (int i = 0; i < 10000 && (inl(es_io + ES1371_CODEC) & (1u << 30)); i++) io_wait();   /* write in progress */
    uint32_t x = src_ready();
    outl(es_io + ES1371_SRC, (x & SRC_KEEP) | 0x00010000);
    for (int i = 0; i < 10000 && (inl(es_io + ES1371_SRC) & 0x00870000) != 0; i++) {}
    for (int i = 0; i < 10000 && (inl(es_io + ES1371_SRC) & 0x00870000) != 0x00010000; i++) {}
    outl(es_io + ES1371_CODEC, ((uint32_t)(reg & 0x7F) << 16) | val);
    src_ready();
    outl(es_io + ES1371_SRC, x);
}
static void es1371_rate(uint8_t base, uint32_t rate) {
    uint32_t freq = ((rate << 15) + 1500) / 3000;
    uint32_t r = src_ready() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1);
    outl(es_io + ES1371_SRC, r | SRC_DIS_P2);              /* hold DAC2 while changing it */
    src_write(base + SMP_INT_REGS, (freq >> 5) & 0xFC00);  /* the low byte was cleared at init */
    src_write(base + SMP_VFREQ_FRAC, freq & 0x7FFF);
    r = src_ready() & (SRC_DISABLE | SRC_DIS_P1 | SRC_DIS_R1);
    outl(es_io + ES1371_SRC, r);
}
static bool es1371_init(pci_dev_t *p) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x05 | 0x400);   /* I/O, bus master, INTx off: we poll */
    uint32_t bar = pci_cfg_read(p->bus, p->dev, p->fn, 0x10);
    if (!(bar & 1)) return false;
    es_io = bar & ~3u;
    outl(es_io + ES_CTRL, 1u << 14); inl(es_io + ES_CTRL); sleep_ms(1);   /* AC-link warm reset */
    outl(es_io + ES_CTRL, 0); outl(es_io + ES_SCTRL, 0); outl(es_io + ES1371_LEGACY, 0);
    /* sample rate converter: clear its RAM, unity volumes, 48 kHz on DAC2 */
    outl(es_io + ES1371_SRC, SRC_DISABLE);
    for (int i = 0; i < 0x80; i++) src_write(i, 0);
    src_write(SMP_DAC1 + SMP_TRUNC_N, 16 << 4); src_write(SMP_DAC1 + SMP_INT_REGS, 16 << 10);
    src_write(SMP_DAC2 + SMP_TRUNC_N, 16 << 4); src_write(SMP_DAC2 + SMP_INT_REGS, 16 << 10);
    src_write(SMP_VOL_ADC, 1 << 12); src_write(SMP_VOL_ADC + 1, 1 << 12);
    src_write(SMP_VOL_DAC1, 1 << 12); src_write(SMP_VOL_DAC1 + 1, 1 << 12);
    src_write(SMP_VOL_DAC2, 1 << 12); src_write(SMP_VOL_DAC2 + 1, 1 << 12);
    es1371_rate(SMP_DAC2, 48000);
    src_ready(); outl(es_io + ES1371_SRC, 0);              /* SRC on */
    /* AC'97 codec: reset, power up, unmute master and PCM */
    es1371_codec(0x00, 0); sleep_ms(20);
    es1371_codec(0x26, 0x0000); sleep_ms(5);
    es1371_codec(0x02, 0x0000); es1371_codec(0x04, 0x0000); es1371_codec(0x18, 0x0808);
    out_rate = 48000;
    es_start_dac2(0);
    snprintf(devname, sizeof devname, "%s (%04x:%04x)", p->device == 0x1371 ? "Ensoniq AudioPCI ES1371" : p->vendor == 0x1102 ? "Creative Ectiva EV1938" : "Ensoniq AudioPCI ES1373", p->vendor, p->device);
    klog("es1371: %s at %x", devname, es_io);
    return true;
}

/* ======================= Sound Blaster 16 =======================
 * 16-bit stereo, auto-initialising ISA DMA over the whole ring; the position
 * comes from the DMA controller's count register. ISA DMA can only reach the
 * first 16 MiB and must not cross a 128 KiB boundary, so the ring lives in a
 * fixed low-memory area (2 MiB, below every other user). */
#define ISA_DMA_AREA 0x00200000u
static uint16_t sb_base;
static int sb_dma = 5, sb_irq = 5;
static bool sb_reset(uint16_t base) {
    outb(base + 6, 1); spin(10); outb(base + 6, 0);
    for (int i = 0; i < 2000; i++) { if ((inb(base + 0xE) & 0x80) && inb(base + 0xA) == 0xAA) return true; spin(5); }
    return false;
}
static void dsp_w(uint8_t v) { for (int i = 0; i < 20000 && (inb(sb_base + 0xC) & 0x80); i++) {} outb(sb_base + 0xC, v); }
static int dsp_r(void) { for (int i = 0; i < 20000; i++) if (inb(sb_base + 0xE) & 0x80) return inb(sb_base + 0xA); return -1; }
static void sb_mix(uint8_t reg, uint8_t val) { outb(sb_base + 4, reg); outb(sb_base + 5, val); }
static uint8_t sb_mix_r(uint8_t reg) { outb(sb_base + 4, reg); return inb(sb_base + 5); }
static void sb_irq_handler(regs_t *r) { (void)r; inb(sb_base + 0xF); inb(sb_base + 0xE); }   /* acknowledge 16- and 8-bit */
static const uint8_t dma_addr_port[8] = { 0x00, 0x02, 0x04, 0x06, 0xC0, 0xC4, 0xC8, 0xCC };
static const uint8_t dma_count_port[8] = { 0x01, 0x03, 0x05, 0x07, 0xC2, 0xC6, 0xCA, 0xCE };
static const uint8_t dma_page_port[8] = { 0x87, 0x83, 0x81, 0x82, 0x8F, 0x8B, 0x89, 0x8A };
static bool sb16_init(void) {
    static const uint16_t bases[] = { 0x220, 0x240, 0x260, 0x280 };
    for (unsigned i = 0; i < ARRAY_LEN(bases) && !sb_base; i++) if (sb_reset(bases[i])) sb_base = bases[i];
    if (!sb_base) return false;
    dsp_w(0xE1);
    int major = dsp_r(), minor = dsp_r();
    if (major < 4) { klog("sb16: DSP %d.%02d at %x is older than a Sound Blaster 16", major, minor, sb_base); sb_base = 0; return false; }
    uint8_t irqsel = sb_mix_r(0x80), dmasel = sb_mix_r(0x81);
    sb_irq = irqsel & 2 ? 5 : irqsel & 4 ? 7 : irqsel & 8 ? 10 : irqsel & 1 ? 9 : 5;
    sb_dma = dmasel & 0x20 ? 5 : dmasel & 0x40 ? 6 : dmasel & 0x80 ? 7 : 5;
    out_rate = 44100;
    ring = (int16_t *)(uintptr_t)ISA_DMA_AREA;
    memset(ring, 0, NBUF * BUFBYTES);
    /* 8237 slave controller: channel sb_dma, single mode, auto-init, memory -> device */
    int c = sb_dma & 3;
    uint32_t a = ISA_DMA_AREA, words = NBUF * BUFBYTES / 2;
    outb(0xD4, 0x04 | c);                                  /* mask */
    outb(0xD8, 0);                                         /* clear flip-flop */
    outb(0xD6, 0x58 | c);
    outb(dma_addr_port[sb_dma], (a >> 1) & 0xFF); outb(dma_addr_port[sb_dma], (a >> 9) & 0xFF);
    outb(dma_page_port[sb_dma], (a >> 16) & 0xFE);
    outb(dma_count_port[sb_dma], (words - 1) & 0xFF); outb(dma_count_port[sb_dma], ((words - 1) >> 8) & 0xFF);
    outb(0xD4, c);                                         /* unmask */
    irq_install(sb_irq, sb_irq_handler);
    sb_mix(0x22, 0xFF); sb_mix(0x30, 0xF8); sb_mix(0x31, 0xF8); sb_mix(0x32, 0xF8); sb_mix(0x33, 0xF8);
    dsp_w(0xD1);                                           /* speaker on */
    dsp_w(0x41); dsp_w(out_rate >> 8); dsp_w(out_rate & 0xFF);
    uint32_t blk = BUFFRAMES * 2 - 1;                      /* samples per interrupt - 1 */
    dsp_w(0xB6); dsp_w(0x30); dsp_w(blk & 0xFF); dsp_w(blk >> 8);   /* 16-bit, auto-init, FIFO; stereo signed */
    snprintf(devname, sizeof devname, "Creative Sound Blaster 16 (DSP %d.%02d, port %x, IRQ %d, DMA %d)", major, minor, sb_base, sb_irq, sb_dma);
    klog("sb16: %s", devname);
    return true;
}
static int sb16_position(void) {
    outb(0xD8, 0);
    uint16_t lo = inb(dma_count_port[sb_dma]), hi = inb(dma_count_port[sb_dma]);
    uint32_t left = ((hi << 8) | lo) + 1, words = NBUF * BUFBYTES / 2;
    if (left > words) left = words;
    uint32_t pos = (words - left) * 2;
    return pos / BUFBYTES;
}

/* ======================= mixer + task ======================= */
static void mix_into(int16_t *out, int frames) {
    static int32_t acc[BUFFRAMES * 2];
    memset(acc, 0, sizeof(int32_t) * frames * 2);
    int vol = audio_muted ? 0 : audio_volume;
    for (int v = 0; v < NVOICE; v++) {
        voice_t *vc = &voices[v];
        if (!vc->active) continue;
        int n = MIN((uint32_t)frames, vc->s->frames - vc->pos);
        const int16_t *src = vc->s->pcm + vc->pos * 2;
        for (int i = 0; i < n * 2; i++) acc[i] += src[i];
        vc->pos += n;
        if (vc->pos >= vc->s->frames) vc->active = false;
    }
    for (int i = 0; i < frames * 2; i++) { int32_t s = acc[i] * vol / 100; out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s; }
}
/* Keep LEAD buffers queued in front of the buffer the hardware is playing.
 * Buffers it has finished are cleared, so if we ever fall behind it plays
 * silence instead of repeating stale sound; if it has overtaken us, start
 * again just ahead of it. Runs with interrupts off (timer IRQ or irq_save). */
static int cur_position(void) {
    return dev == DEV_HDA ? hda_position() : dev == DEV_AC97 ? ac97_position() : dev == DEV_SB16 ? sb16_position() : es_position();
}
static int last_cur = -1;
static uint32_t underruns, bad_positions;
/* When the card reports no usable position, estimate it from the time since the
 * last good reading at the card's sample rate, so the buffers keep being filled
 * (some emulated cards play fine but report nonsense positions). */
static uint32_t est_bytes, est_tick;
static bool est_set;
static void refill(void) {
    int cur = cur_position();
    uint32_t now = timer_ticks();
    if (dev == DEV_ES1370 || dev == DEV_ES1371) {
        es_service();                                         /* acknowledge DAC2's end of count */
        /* stalled DAC2: restart it, but only a few times (restarting a channel can be
         * slow on a VM: never let it eat the machine) */
        static uint32_t es_last = ~0u, es_last_tick, es_restart_tick;
        if (es_raw != es_last) { es_last = es_raw; es_last_tick = now; }
        else if (now - es_last_tick > 300) {
            if (es_restarts >= 3) { audio_disable("the card stopped playing and did not restart"); return; }
            if (!es_restarts || now - es_restart_tick >= 2000) {
                es_restarts++; es_restart_tick = now;
                es_start_dac2(es_ctrl);
            }
            es_last_tick = now;
        }
    }
    if (cur >= 0) { est_bytes = cur * BUFBYTES; est_tick = now; est_set = true; }
    else {
        bad_positions++;
        if (!est_set) { est_bytes = 0; est_tick = now; est_set = true; }
        uint64_t b = est_bytes + (uint64_t)(now - est_tick) * out_rate * 4 / 1000;
        cur = (int)((b / BUFBYTES) % NBUF);
    }
    if (last_cur >= 0)
        for (int b = last_cur; b != cur; b = (b + 1) % NBUF) memset(ring + b * BUFFRAMES * 2, 0, BUFBYTES);
    last_cur = cur;
    int ahead = (next_fill - cur + NBUF) % NBUF;          /* 1 + buffers queued after the playing one */
    if (ahead == 0 || ahead > LEAD + 1) {                     /* hardware caught up with / passed us */
        if (ahead) underruns++;
        next_fill = (cur + 1) % NBUF;
        ahead = 1;
    }
    for (; ahead <= LEAD; ahead++) {
        mix_into(ring + next_fill * BUFFRAMES * 2, BUFFRAMES);
        next_fill = (next_fill + 1) % NBUF;
    }
    if (dev == DEV_AC97) {
        outb(nabm + 0x15, (next_fill + NBUF - 1) % NBUF);     /* last valid buffer */
        if (!(inb(nabm + 0x1B) & 1)) outb(nabm + 0x1B, 0x01);   /* (re)start */
    }
}
/* Time budget: the sound work runs in the timer interrupt, so if the card makes it
 * slow (VM device models can block on the host's audio), sound is switched off
 * rather than letting it starve the desktop, keyboard and mouse. */
static uint64_t tsc_per_ms, budget_used, budget_start;
static void audio_tick(uint32_t t) {
    if ((t & 3) != 0 || !dev) return;
    uint64_t a = rdtsc();
    refill();
    if (!tsc_per_ms) return;
    uint64_t d = rdtsc() - a;
    if (d > tsc_per_ms * 20) { audio_disable("the sound card took too long to respond"); return; }
    budget_used += d;
    if (a - budget_start >= tsc_per_ms * 1000) {              /* each second: more than 15% of the CPU? */
        if (budget_used > tsc_per_ms * 150) { audio_disable("the sound card was using too much of the CPU"); return; }
        budget_used = 0; budget_start = a;
    }
}

static char off_reason[96];

/* ---- Virtual HDA controller ----
 * A machine can have no audio controller on the PCI bus and still play through
 * a USB headset. To keep the rest of the system on an HDA footing, the kernel
 * presents a software controller with one codec, one audio function group, a
 * DAC and a speaker pin. There is no register file behind it: its stream pulls
 * from the mixer on demand, and the USB audio driver hands those frames to its
 * isochronous endpoint. The physical jack stays whatever the hardware is. */
typedef struct {
    bool on;
    int rate;
    int codec, afg, dac, pin;
    uint32_t frames;
} vhda_t;
static vhda_t vhda;
static bool card_found, probed;

static void vhda_start(int rate)
{
    if (vhda.on) { vhda.rate = rate; return; }
    memset(&vhda, 0, sizeof vhda);
    vhda.on = true;
    vhda.rate = rate;
    vhda.codec = 0; vhda.afg = 1; vhda.dac = 2; vhda.pin = 3;
    klog("hda: virtual controller: codec %d, AFG %d, DAC %d -> pin %d (speaker), %d Hz",
         vhda.codec, vhda.afg, vhda.dac, vhda.pin, rate);
}
static void vhda_stop(void)
{
    if (!vhda.on) return;
    vhda.on = false;
    klog("hda: virtual controller gone");
}

static const char *pin_label(int devtype, bool digital)
{
    if (digital) return "HDMI/DP output";
    switch (devtype & 15) {
    case 0: return "Line output";
    case 1: return "Speakers";
    case 2: return "Headphones";
    case 4: return "S/PDIF output";
    case 5: return "Digital output";
    default: return pin_kind(devtype);
    }
}
static void ep_add(const char *name, int kind, int pin, int devtype)
{
    if (neps >= MAX_EP || !name || !name[0]) return;
    strlcpy(eps[neps].name, name, sizeof eps[neps].name);
    eps[neps].kind = kind; eps[neps].pin = pin; eps[neps].devtype = devtype;
    neps++;
}
/* Rebuild the playback endpoint list whenever the output or the USB device
 * changes: the card's pins, the USB device, and the soft controller's jacks. */
static void eps_build(void)
{
    neps = 0;
    if (dev == DEV_USB) ep_add(devname, EP_USB, 0, 0);
    if (dev == DEV_HDA || (dev == DEV_USB && card_found))
        for (int i = 0; i < hda_npins; i++)
            ep_add(pin_label(hda_pins[i].devtype, hda_pins[i].digital), EP_HDA, hda_pins[i].nid, hda_pins[i].devtype);
    if (dev == DEV_USB) {
        if (vhda.on) {
            ep_add("Line output (virtual)", EP_VJACK, 3, 0);
            ep_add("Speakers (virtual)", EP_VJACK, 4, 1);
            ep_add("Headphones (virtual)", EP_VJACK, 5, 2);
        }
    } else if (dev != DEV_HDA && dev != DEV_NONE) {
        ep_add(devname, EP_CARD, 0, 0);
    }
    if (!neps) ep_add(devname, EP_CARD, 0, 0);
    if (ep_sel < 0 || ep_sel >= neps) ep_sel = 0;
}
int audio_device_count(void) { return neps; }
const char *audio_device_entry(int i) { return i >= 0 && i < neps ? eps[i].name : ""; }
int audio_device_active(void) { return ep_sel < neps ? ep_sel : 0; }
void audio_device_select(int i)
{
    if (i < 0 || i >= neps) return;
    ep_sel = i;
    if (dev == DEV_HDA)
        for (int k = 0; k < neps; k++)
            if (eps[k].kind == EP_HDA)
                hda_cmd(hda_cad, eps[k].pin, VERB(0x707, k == i ? (0x40u | (eps[k].devtype == 2 ? 0x80u : 0)) : 0));
    if (vhda.on && eps[i].kind == EP_VJACK) vhda.pin = eps[i].pin;
    klog("audio: playback device is %s", eps[i].name);
}

static bool usb_card;
static int usb_card_rate;
static char usb_card_name[64];
static int dev_other;                         /* the card USB audio took over from */
static int other_rate;
static char other_name[64];
void audio_usb_ready(int rate, const char *name)
{
    usb_card = true;
    usb_card_rate = rate;
    strlcpy(usb_card_name, name, sizeof usb_card_name);
    if (dev == DEV_USB) { if (probed && !card_found) vhda_start(rate); eps_build(); return; }
    if (dev) {                                /* a card was playing: keep it for later */
        dev_other = dev;
        other_rate = out_rate;
        strlcpy(other_name, devname, sizeof other_name);
    }
    dev = DEV_USB;
    out_rate = rate;
    strlcpy(devname, name, sizeof devname);
    timer_tick_hook = NULL;                   /* the packets pace the mixer */
    klog("audio: output is %s, %d Hz", name, rate);
    if (probed && !card_found) vhda_start(rate);
    eps_build();
}
void audio_usb_gone(void)
{
    usb_card = false;
    if (dev != DEV_USB) return;
    if (dev_other) {
        dev = dev_other;
        out_rate = other_rate;
        strlcpy(devname, other_name, sizeof devname);
        uint32_t f = irq_save(); refill(); irq_restore(f);
        timer_tick_hook = audio_tick;
        klog("audio: output is back to %s", devname);
        dev_other = 0;
    } else {
        dev = DEV_NONE;
        strlcpy(devname, "No sound device", sizeof devname);
        vhda_stop();
        klog("audio: the USB audio device was unplugged; no sound device now");
    }
    eps_build();
}
int audio_usb_pull(int16_t *out, int frames)
{
    if (frames <= 0 || !out) return 0;
    if (frames > BUFFRAMES) frames = BUFFRAMES;
    uint32_t f = irq_save();
    mix_into(out, frames);
    if (vhda.on) vhda.frames += frames;       /* the virtual controller's stream ran */
    irq_restore(f);
    return frames;
}

void audio_disable(const char *why) {
    timer_tick_hook = NULL;
    if (dev == DEV_ES1370 || dev == DEV_ES1371) outl(es_io + ES_CTRL, es_ctrl);   /* DAC2 off */
    else if (dev == DEV_HDA) R8(sd) &= ~2;
    else if (dev == DEV_AC97) outb(nabm + 0x1B, 0);
    strlcpy(off_reason, why, sizeof off_reason);
    for (int v = 0; v < NVOICE; v++) voices[v].active = false;
    klog("audio: sound switched off: %s", why);
}
void audio_init(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    ring = kmalloc_aligned(NBUF * BUFBYTES, 128);
    memset(ring, 0, NBUF * BUFBYTES);
    /* Probe the PCI cards even when a USB audio device is already playing, so
     * that the card is there to go back to when the USB one is unplugged. */
    bool usb_was = dev == DEV_USB;
    if (usb_was) dev = DEV_NONE;
    for (int i = 0; i < n && !dev; i++) if (devs[i].cls == 0x04 && devs[i].sub == 0x03 && hda_init(&devs[i])) dev = DEV_HDA;
    for (int i = 0; i < n && !dev; i++) {
        pci_dev_t *d = &devs[i];
        if (d->vendor == 0x1274 && d->device == 0x5000) { if (es1370_init(d)) dev = DEV_ES1370; }
        else if ((d->vendor == 0x1274 && (d->device == 0x1371 || d->device == 0x5880)) || (d->vendor == 0x1102 && d->device == 0x8938)) { if (es1371_init(d)) dev = DEV_ES1371; }
        else if (d->cls == 0x04 && d->sub == 0x01 && (d->vendor == 0x8086 || d->vendor == 0x10DE || d->vendor == 0x1022 || d->vendor == 0x1039) && ac97_init(d)) dev = DEV_AC97;
    }
    if (!dev && sb16_init()) dev = DEV_SB16;            /* ISA: probed last */
    card_found = dev != DEV_NONE;
    probed = true;
    if (usb_was) {                                      /* a USB device is the output */
        if (dev) { dev_other = dev; other_rate = out_rate; strlcpy(other_name, devname, sizeof other_name); }
        dev = DEV_USB;
        out_rate = usb_card_rate;
        strlcpy(devname, usb_card_name, sizeof devname);
    } else if (!dev && usb_card) {
        dev = DEV_USB;
        out_rate = usb_card_rate;
        strlcpy(devname, usb_card_name, sizeof devname);
    }
    if (!dev) { klog("audio: no supported sound device (HD Audio, AC'97, AudioPCI, Sound Blaster 16 or USB audio)"); return; }
    if (dev == DEV_USB && !card_found) vhda_start(out_rate);
    else vhda_stop();
    eps_build();
    next_fill = 0;
    { uint64_t a = rdtsc(); sleep_ms(20); tsc_per_ms = (rdtsc() - a) / 20; budget_start = rdtsc(); }
    if (dev != DEV_USB) {
        uint32_t f = irq_save(); refill(); irq_restore(f);
        timer_tick_hook = audio_tick;                   /* USB audio is paced by its packets */
    }
}
/* Some device names already carry the rate (the USB audio one does), so the
 * status lines do not append it twice. */
static bool name_has_rate(const char *s)
{
    for (; s[0]; s++) if (s[0] == 'H' && s[1] == 'z') return true;
    return false;
}
/* One line for the log-on screen: the sound device that is playing, or the
 * audio hardware that was found and why it is not. */
const char *audio_summary(void)
{
    static char out[240];
    if (dev) {
        int len = snprintf(out, sizeof out, "Sound: %s", devname);
        if (!name_has_rate(devname)) len += snprintf(out + len, sizeof out - len, ", %u Hz", out_rate);
        if (vhda.on) len += snprintf(out + len, sizeof out - len, ", virtual HDA");
        /* Also say what the onboard codec search found: a USB device can be
         * playing while the line-out jack's codec is still missing. */
        if (hda_why[0]) snprintf(out + len, sizeof out - len, " (onboard: %s)", hda_why);
        return out;
    }
    int len = snprintf(out, sizeof out, "Sound: no device");
    pci_dev_t *devs; int n = pci_list(&devs);
    for (int i = 0; i < n && len + 30 < (int)sizeof out; i++) {
        if (devs[i].cls != 0x04) continue;
        len += snprintf(out + len, sizeof out - len, ", %04x:%04x %s", devs[i].vendor, devs[i].device,
                        devs[i].sub == 0x03 ? "HDA" : devs[i].sub == 0x01 ? "AC97" : "audio");
    }
    if (hda_why[0]) snprintf(out + len, sizeof out - len, " (%s)", hda_why);
    return out;
}
/* what the driver found and whether the card is really playing (for the "sndinfo" command) */
static int put_devices(char *buf, int cap, int off)
{
    if (neps <= 1) return off;
    off += snprintf(buf + off, cap - off, "Playback devices (%d):\n", neps);
    for (int i = 0; i < neps && off + 80 < cap; i++)
        off += snprintf(buf + off, cap - off, "  %c %s%s\n", i == ep_sel ? '*' : ' ', eps[i].name, i == ep_sel ? "  (selected)" : "");
    return off;
}
void audio_diag(char *buf, int cap) {
    if (!dev) { snprintf(buf, cap, "No sound device found (supported: HD Audio, AC'97, Ensoniq AudioPCI, Sound Blaster 16, USB audio).\n"); return; }
    if (dev == DEV_USB) {
        char u[160], v[200], hdr[96];
        usb_audio_diag(u, sizeof u);
        if (name_has_rate(devname)) snprintf(hdr, sizeof hdr, "%s", devname);
        else snprintf(hdr, sizeof hdr, "%s, %d Hz", devname, out_rate);
        if (vhda.on) snprintf(v, sizeof v, "HDA controller: virtual (software), codec %d, AFG %d, DAC %d, pin %d\n%u frames through the stream\n", vhda.codec, vhda.afg, vhda.dac, vhda.pin, vhda.frames);
        else v[0] = 0;
        int len = snprintf(buf, cap, "%s\n", hdr);
        len = put_devices(buf, cap, len);
        snprintf(buf + len, cap - len, "%s%sVolume %d%s\n", v, u, audio_volume,
                 audio_muted ? " (muted)" : "");
        return;
    }
    if (off_reason[0]) {
        snprintf(buf, cap, "%s\nSound is switched off: %s.\n%u end-of-count acks, %u restarts\n", devname, off_reason, es_acks, es_restarts);
        return;
    }
    char r0[160] = "", r1[160] = "";
    uint32_t f = irq_save(); int a = cur_position(); irq_restore(f);
    if (dev == DEV_HDA) hda_regs(r0, sizeof r0);
    else if (dev == DEV_ES1370 || dev == DEV_ES1371) snprintf(r0, sizeof r0, "CTRL %08x STATUS %08x SCTRL %08x frame %08x\n", inl(es_io + ES_CTRL), inl(es_io + ES_STATUS), inl(es_io + ES_SCTRL), es_raw);
    sleep_ms(60);
    f = irq_save(); int b = cur_position(); irq_restore(f);
    if (dev == DEV_HDA) hda_regs(r1, sizeof r1);
    else if (dev == DEV_ES1370 || dev == DEV_ES1371) snprintf(r1, sizeof r1, "CTRL %08x STATUS %08x SCTRL %08x frame %08x\n%u end-of-count acks, %u restarts\n", inl(es_io + ES_CTRL), inl(es_io + ES_STATUS), inl(es_io + ES_SCTRL), es_raw, es_acks, es_restarts);
    const char *st = a < 0 || b < 0 ? "UNREADABLE (using the timer estimate)" : a != b ? "advancing" : "NOT advancing (the card is not playing)";
    int len = snprintf(buf, cap, "%s, %d Hz\n", devname, out_rate);
    len = put_devices(buf, cap, len);
    snprintf(buf + len, cap - len, "%s%s%sPlayback position: %s (buffer %d -> %d)\n%u underruns, %u unusable position readings\nVolume %d%s\n",
             dev == DEV_HDA ? hda_diag : "", r0, r1, st, a, b, underruns, bad_positions, audio_volume, audio_muted ? " (muted)" : "");
}
bool audio_present(void) { return dev != DEV_NONE; }
const char *audio_device_name(void) { return neps ? eps[ep_sel].name : devname; }
const char *audio_controller_name(void) {
    if (vhda.on) return "HDA (virtual)";
    int d = dev == DEV_USB && dev_other ? dev_other : dev;
    switch (d) {
    case DEV_HDA: return "HDA";
    case DEV_AC97: return "AC'97";
    case DEV_ES1370: case DEV_ES1371: return "AudioPCI";
    case DEV_SB16: return "Sound Blaster 16";
    default: return "";
    }
}
int audio_play(const sound_t *s) {
    if (!dev || off_reason[0] || !s || !s->frames) return -1;
    uint32_t f = irq_save();
    int v = 0;
    while (v < NVOICE && voices[v].active) v++;
    if (v == NVOICE) v = 0;                                          /* all busy: replace the first */
    voices[v].s = s; voices[v].pos = 0; voices[v].active = true;
    irq_restore(f);
    return v;
}
void audio_stop_all(void) { uint32_t f = irq_save(); for (int v = 0; v < NVOICE; v++) voices[v].active = false; irq_restore(f); }
bool audio_busy(void) { for (int v = 0; v < NVOICE; v++) if (voices[v].active) return true; return false; }
bool audio_voice_active(int v, const sound_t *s) { return v >= 0 && v < NVOICE && voices[v].active && voices[v].s == s; }

/* ======================= WAV ======================= */
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
sound_t *sound_load_wav(const uint8_t *d, uint32_t n, char *err, int errcap) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { snprintf(err, errcap, "not a WAV file"); return NULL; }
    int fmt = 0, ch = 0, bits = 0; uint32_t rate = 0; const uint8_t *data = NULL; uint32_t dlen = 0;
    for (uint32_t o = 12; o + 8 <= n;) {
        uint32_t cl = le32(d + o + 4);
        if (!memcmp(d + o, "fmt ", 4) && cl >= 16) { fmt = le16(d + o + 8); ch = le16(d + o + 10); rate = le32(d + o + 12); bits = le16(d + o + 22); if (fmt == 0xFFFE && cl >= 40) fmt = le16(d + o + 32); }
        else if (!memcmp(d + o, "data", 4)) { data = d + o + 8; dlen = MIN(cl, n - o - 8); }
        o += 8 + cl + (cl & 1);
    }
    if (fmt != 1 || !data || (bits != 8 && bits != 16 && bits != 24) || ch < 1 || ch > 8 || rate < 4000 || rate > 192000) {
        snprintf(err, errcap, "unsupported WAV format (need PCM 8/16/24-bit)"); return NULL;
    }
    int bps = bits / 8, stride = bps * ch;
    uint32_t in_frames = dlen / stride;
    uint32_t out_frames = (uint32_t)((uint64_t)in_frames * RATE / rate);
    sound_t *s = kzalloc(sizeof *s);
    s->pcm = kmalloc(out_frames * 4 + 4); s->frames = out_frames;
    uint64_t step = ((uint64_t)rate << 16) / RATE, pos = 0;     /* 16.16 fixed point */
    #define SMP(f, c) (bps == 2 ? (int16_t)le16(data + (f) * stride + (c) * 2) : bps == 1 ? (int16_t)((data[(f) * stride + (c)] - 128) << 8) \
                       : (int16_t)le16(data + (f) * stride + (c) * 3 + 1))
    for (uint32_t i = 0; i < out_frames; i++, pos += step) {
        uint32_t f = pos >> 16, fr = pos & 0xFFFF, f2 = f + 1 < in_frames ? f + 1 : f;
        for (int c = 0; c < 2; c++) {
            int sc = ch == 1 ? 0 : c;
            int a = SMP(f, sc), b = SMP(f2, sc);
            s->pcm[i * 2 + c] = a + (int)(((int64_t)(b - a) * fr) >> 16);
        }
    }
    #undef SMP
    return s;
}
void sound_free(sound_t *s) { if (s) { kfree(s->pcm); kfree(s); } }

/* ======================= files ======================= */
#include "fs.h"
bool system_sounds = true;
typedef struct { char path[160]; uint32_t size; sound_t *s; uint32_t used; } cache_t;
static cache_t cache[4];
bool audio_play_file(const char *path, char *err, int errcap) {
    if (!dev) { snprintf(err, errcap, "no sound device"); return false; }
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir) { snprintf(err, errcap, "file not found"); return false; }
    char full[160]; vfs_path(n, full, sizeof full);
    cache_t *c = NULL;
    for (int i = 0; i < 4; i++) if (cache[i].s && !strcasecmp(cache[i].path, full) && cache[i].size == n->size) c = &cache[i];
    if (!c) {
        sound_t *s = sound_load_wav(n->data, n->size, err, errcap);
        if (!s) { klog("audio: %s: %s", full, err); return false; }
        int lru = 0; for (int i = 1; i < 4; i++) if (cache[i].used < cache[lru].used) lru = i;
        c = &cache[lru];
        if (c->s) {                                       /* evict: make sure nothing still plays it */
            uint32_t f = irq_save();
            for (int v = 0; v < NVOICE; v++) if (voices[v].s == c->s) voices[v].active = false;
            irq_restore(f);
            sound_free(c->s);
        }
        strlcpy(c->path, full, sizeof c->path); c->size = n->size; c->s = s;
    }
    c->used = timer_ticks();
    return audio_play(c->s) >= 0;
}
void audio_wait(uint32_t max_ms) {
    uint32_t t0 = timer_ticks();
    bool any = audio_busy();
    while (audio_busy() && timer_ticks() - t0 < max_ms) task_sleep(20);
    if (any) task_sleep(LEAD * BUFFRAMES * 1000 / RATE + 40);    /* let the buffered tail play out */
}
