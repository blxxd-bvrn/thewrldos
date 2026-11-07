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
 * The output rate is 48 kHz, or 44.1 kHz on cards that cannot do 48. */
#include "kernel.h"
#include "audio.h"

static uint32_t out_rate = 48000;            /* the mixer's (and the card's) sample rate */
#define RATE out_rate
#define NBUF 32
#define BUFFRAMES 512                     /* 10.7 ms per buffer */
#define BUFBYTES (BUFFRAMES * 4)
#define LEAD 16                           /* buffers kept ahead of the hardware */
#define NVOICE 4

enum { DEV_NONE, DEV_HDA, DEV_AC97, DEV_ES1370, DEV_ES1371, DEV_SB16 };
static int dev;
static char devname[80] = "No sound device";
static int16_t *ring;                     /* NBUF * BUFFRAMES stereo frames */
static int next_fill;
int audio_volume = 80;
bool audio_muted;

typedef struct { const sound_t *s; uint32_t pos; bool active; } voice_t;
static voice_t voices[NVOICE];

/* ======================= Intel HDA ======================= */
static volatile uint8_t *hda;
static uint32_t *corb; static uint64_t *rirb;
static uint16_t rirb_rp;
static uint32_t sd;                       /* output stream descriptor offset */
static bool use_immediate;
#define R8(o) (*(volatile uint8_t *)(hda + (o)))
#define R16(o) (*(volatile uint16_t *)(hda + (o)))
#define R32(o) (*(volatile uint32_t *)(hda + (o)))
static void spin(uint32_t us) { for (uint32_t i = 0; i < us; i++) io_wait(); }
static uint32_t hda_cmd(int cad, int nid, uint32_t verb_payload) {
    uint32_t c = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | (verb_payload & 0xFFFFF);
    if (use_immediate) {
        for (int i = 0; i < 10000 && (R16(0x68) & 1); i++) spin(1);
        R32(0x60) = c; R16(0x68) = 2; R16(0x68) = 1;
        for (int i = 0; i < 10000; i++) { if (R16(0x68) & 2) { R16(0x68) = 2; return R32(0x64); } spin(1); }
        return 0;
    }
    uint16_t wp = (R16(0x48) + 1) & 0xFF;
    corb[wp] = c;
    R16(0x48) = wp;
    for (int i = 0; i < 20000; i++) {
        uint16_t w = R16(0x58) & 0xFF;
        if (w != rirb_rp) { rirb_rp = (rirb_rp + 1) & 0xFF; R8(0x5D) = 0x05; return (uint32_t)rirb[rirb_rp]; }   /* ack RIRB status so the controller keeps going */
        spin(1);
    }
    return 0xFFFFFFFF;
}
#define VERB(v, p) (((uint32_t)(v) << 8) | (p))         /* 12-bit verb, 8-bit payload */
#define VERB4(v, p) (((uint32_t)(v) << 16) | (p))       /* 4-bit verb, 16-bit payload */
static uint32_t param(int cad, int nid, int p) { return hda_cmd(cad, nid, VERB(0xF00, p)); }

typedef struct { uint32_t caps; uint8_t type; uint8_t nconn; uint8_t conn[16]; } widget_t;
static widget_t wg[128];
static int wstart, wcount;
static widget_t *W(int nid) { return nid >= wstart && nid < wstart + wcount ? &wg[nid - wstart] : NULL; }
static int path[8], plen;
static bool find_dac(int cad, int nid, int depth) {
    widget_t *w = W(nid);
    if (!w || depth > 6) return false;
    path[depth] = nid; plen = depth + 1;
    if (w->type == 0) return true;                        /* audio output (DAC) */
    for (int i = 0; i < w->nconn; i++) {
        if (find_dac(cad, w->conn[i], depth + 1)) {
            if (w->type != 2 && w->nconn > 1) hda_cmd(cad, nid, VERB(0x701, i));   /* select that input */
            if (w->type == 2) hda_cmd(cad, nid, VERB4(0x3, 0x7000 | (i << 8) | 0x40)); /* mixer: unmute that input */
            plen = MAX(plen, depth + 2);
            return true;
        }
    }
    return false;
}
static void amp_unmute(int cad, int nid, uint32_t afg_ampcaps) {
    widget_t *w = W(nid);
    if (!w || !(w->caps & (1 << 2))) return;              /* no output amplifier */
    uint32_t caps = (w->caps & (1 << 3)) ? param(cad, nid, 0x12) : afg_ampcaps;
    int steps = (caps >> 8) & 0x7F, offset = caps & 0x7F;
    int gain = offset ? offset : steps;                     /* 0 dB if known, else the top of the range */
    hda_cmd(cad, nid, VERB4(0x3, 0xB000 | gain));
}
static bool hda_init(pci_dev_t *p) {
    uint32_t cmd = pci_cfg_read(p->bus, p->dev, p->fn, 0x04);
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x06);
    uint32_t bar = pci_cfg_read(p->bus, p->dev, p->fn, 0x10) & ~0xFu;
    paging_set_uncached(bar);
    hda = (volatile uint8_t *)(uintptr_t)bar;
    /* controller reset */
    R32(0x08) &= ~1u;
    for (int i = 0; i < 1000 && (R32(0x08) & 1); i++) spin(10);
    spin(100);
    R32(0x08) |= 1;
    for (int i = 0; i < 1000 && !(R32(0x08) & 1); i++) spin(10);
    sleep_ms(20);
    uint16_t codecs = R16(0x0E);
    if (!codecs) { klog("hda: no codec found"); return false; }
    R32(0x20) = 0;                                          /* no interrupts: we poll */
    /* CORB / RIRB rings */
    corb = kmalloc_aligned(1024, 128); memset(corb, 0, 1024);
    rirb = kmalloc_aligned(2048, 128); memset(rirb, 0, 2048);
    R8(0x4C) = 0; R8(0x5C) = 0; spin(100);
    R32(0x40) = (uint32_t)(uintptr_t)corb; R32(0x44) = 0;
    R8(0x4E) = (R8(0x4E) & 0xF0) | 2;
    R16(0x4A) = 0x8000; for (int i = 0; i < 1000 && !(R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x4A) = 0; for (int i = 0; i < 1000 && (R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x48) = 0;
    R32(0x50) = (uint32_t)(uintptr_t)rirb; R32(0x54) = 0;
    R8(0x5E) = (R8(0x5E) & 0xF0) | 2;
    R16(0x58) = 0x8000; rirb_rp = 0;
    R16(0x5A) = 0xFF;                                     /* response interrupt count */
    R8(0x4C) = 2; R8(0x5C) = 2;
    int cad = 0; while (cad < 15 && !(codecs & (1 << cad))) cad++;
    uint32_t vid = param(cad, 0, 0x00);
    if (vid == 0xFFFFFFFF || !vid) { use_immediate = true; R8(0x4C) = 0; R8(0x5C) = 0; vid = param(cad, 0, 0x00); }
    /* find the audio function group */
    uint32_t sub = param(cad, 0, 0x04);
    int afg = 0;
    for (int n = (sub >> 16) & 0xFF, k = 0; k < (int)(sub & 0xFF); k++, n++) if ((param(cad, n, 0x05) & 0xFF) == 1) { afg = n; break; }
    if (!afg) { klog("hda: codec %d has no audio function group", cad); return false; }
    hda_cmd(cad, afg, VERB(0x705, 0));                      /* power D0 */
    uint32_t afg_amp = param(cad, afg, 0x12);
    sub = param(cad, afg, 0x04);
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
        hda_cmd(cad, nid, VERB(0x705, 0));
    }
    /* route every connected output pin (speaker, line out, headphones) to a DAC */
    int dac = 0, pins = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < wcount; i++) {
            widget_t *w = &wg[i]; int nid = wstart + i;
            if (w->type != 4) continue;
            uint32_t pcaps = param(cad, nid, 0x0C), cfg = hda_cmd(cad, nid, VERB(0xF1C, 0));
            int conn = cfg >> 30, devtype = (cfg >> 20) & 0xF;
            if (!(pcaps & (1 << 4)) || conn == 1) continue;             /* not output capable / not connected */
            bool preferred = devtype == 0 || devtype == 1 || devtype == 2;
            if ((pass == 0) != preferred) continue;
            plen = 0;
            if (!find_dac(cad, nid, 0)) continue;
            int d = path[plen - 1];
            if (dac && d != dac) continue;
            dac = d; pins++;
            for (int k = 0; k < plen; k++) amp_unmute(cad, path[k], afg_amp);
            hda_cmd(cad, nid, VERB(0x707, 0x40 | (devtype == 2 ? 0x80 : 0)));
            if (pcaps & (1 << 16)) hda_cmd(cad, nid, VERB(0x70C, 2));
        }
    if (!dac) {   /* no pin path: use the first DAC anyway */
        for (int i = 0; i < wcount; i++) if (wg[i].type == 0) { dac = wstart + i; amp_unmute(cad, dac, afg_amp); break; }
    }
    if (!dac) { klog("hda: no output converter found"); return false; }
    /* output stream 1 -> DAC */
    hda_cmd(cad, dac, VERB(0x706, 1 << 4));
    hda_cmd(cad, dac, VERB4(0x2, 0x0011));                  /* 48 kHz, 16-bit, 2 channels */
    uint16_t gcap = R16(0x00);
    int iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { klog("hda: no output streams"); return false; }
    sd = 0x80 + iss * 0x20;
    R8(sd) |= 1; for (int i = 0; i < 1000 && !(R8(sd) & 1); i++) spin(1);
    R8(sd) &= ~1; for (int i = 0; i < 1000 && (R8(sd) & 1); i++) spin(1);
    uint32_t *bdl = kmalloc_aligned(NBUF * 16, 128);
    for (int i = 0; i < NBUF; i++) { bdl[i * 4] = (uint32_t)(uintptr_t)(ring + i * BUFFRAMES * 2); bdl[i * 4 + 1] = 0; bdl[i * 4 + 2] = BUFBYTES; bdl[i * 4 + 3] = 0; }
    R32(sd + 0x08) = NBUF * BUFBYTES;
    R16(sd + 0x0C) = NBUF - 1;
    R16(sd + 0x12) = 0x0011;
    R32(sd + 0x18) = (uint32_t)(uintptr_t)bdl; R32(sd + 0x1C) = 0;
    R32(sd) = (R32(sd) & 0xFF0FFFFF) | (1u << 20);          /* stream tag 1 */
    R8(sd) |= 2;                                             /* run */
    snprintf(devname, sizeof devname, "High Definition Audio (%04x:%04x, codec %08x)", p->vendor, p->device, vid);
    klog("hda: %s, DAC %d, %d output pin(s)%s", devname, dac, pins, use_immediate ? ", immediate commands" : "");
    return true;
}
static int hda_position(void) {
    uint32_t lpib = R32(sd + 0x04);
    if (lpib >= NBUF * BUFBYTES) return -1;               /* nonsense (device gone / not running) */
    return lpib / BUFBYTES;
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
static void es_start_dac2(uint32_t ctrl_bits) {
    outl(es_io + ES_PAGE, 0x0C);
    outl(es_io + ES_DAC2_FRAMEADR, (uint32_t)(uintptr_t)ring);
    outl(es_io + ES_DAC2_FRAMECNT, NBUF * BUFBYTES / 4 - 1);
    outl(es_io + ES_SCTRL, ES_SCTRL_P2_16BIT | ES_SCTRL_P2_STEREO | (2u << 19));   /* end increment 2 for 16-bit */
    outl(es_io + ES_DAC2_COUNT, BUFFRAMES - 1);
    outl(es_io + ES_CTRL, ctrl_bits | ES_DAC2_EN);
}
static int es_position(void) {
    outl(es_io + ES_PAGE, 0x0C);
    uint32_t v = inl(es_io + ES_DAC2_FRAMECNT);
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
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x05);
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
static void es1371_codec(uint8_t reg, uint16_t val) {
    for (int i = 0; i < 10000 && (inl(es_io + ES1371_CODEC) & (1u << 30)); i++) io_wait();   /* write in progress */
    outl(es_io + ES1371_CODEC, ((uint32_t)(reg & 0x7F) << 16) | val);
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
    pci_cfg_write(p->bus, p->dev, p->fn, 0x04, (cmd & 0xFFFF) | 0x05);
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
    es1371_codec(0x00, 0); sleep_ms(5);
    es1371_codec(0x26, 0x0000);
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
static int last_cur = -1;
static uint32_t underruns;
static void refill(void) {
    int cur = dev == DEV_HDA ? hda_position() : dev == DEV_AC97 ? ac97_position() : dev == DEV_SB16 ? sb16_position() : es_position();
    if (cur < 0) return;
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
static void audio_tick(uint32_t t) { if ((t & 3) == 0) refill(); }

void audio_init(void) {
    pci_dev_t *devs; int n = pci_list(&devs);
    ring = kmalloc_aligned(NBUF * BUFBYTES, 128);
    memset(ring, 0, NBUF * BUFBYTES);
    for (int i = 0; i < n && !dev; i++) if (devs[i].cls == 0x04 && devs[i].sub == 0x03 && hda_init(&devs[i])) dev = DEV_HDA;
    for (int i = 0; i < n && !dev; i++) {
        pci_dev_t *d = &devs[i];
        if (d->vendor == 0x1274 && d->device == 0x5000) { if (es1370_init(d)) dev = DEV_ES1370; }
        else if ((d->vendor == 0x1274 && (d->device == 0x1371 || d->device == 0x5880)) || (d->vendor == 0x1102 && d->device == 0x8938)) { if (es1371_init(d)) dev = DEV_ES1371; }
        else if (d->cls == 0x04 && d->sub == 0x01 && (d->vendor == 0x8086 || d->vendor == 0x10DE || d->vendor == 0x1022 || d->vendor == 0x1039) && ac97_init(d)) dev = DEV_AC97;
    }
    if (!dev && sb16_init()) dev = DEV_SB16;            /* ISA: probed last */
    if (!dev) { klog("audio: no supported sound device (HD Audio, AC'97, AudioPCI or Sound Blaster 16)"); return; }
    next_fill = 0;
    { uint32_t f = irq_save(); refill(); irq_restore(f); }
    timer_tick_hook = audio_tick;
}
bool audio_present(void) { return dev != DEV_NONE; }
const char *audio_device_name(void) { return devname; }
int audio_play(const sound_t *s) {
    if (!dev || !s || !s->frames) return -1;
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
