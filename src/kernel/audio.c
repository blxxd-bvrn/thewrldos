/* Sound: Intel High Definition Audio and AC'97 drivers, a small software mixer
 * and a WAV loader. Output is always 48 kHz, 16-bit stereo. A ring of DMA
 * buffers is kept filled ~170 ms ahead of the hardware by the audio task.
 *   - Intel HDA: QEMU (intel-hda), VirtualBox (default), VMware, real PCs
 *   - AC'97 (Intel ICH): QEMU (AC97), VirtualBox (Windows XP-type VMs) */
#include "kernel.h"
#include "audio.h"

#define RATE 48000
#define NBUF 32
#define BUFFRAMES 512                     /* 10.7 ms per buffer */
#define BUFBYTES (BUFFRAMES * 4)
#define LEAD 16                           /* buffers kept ahead of the hardware */
#define NVOICE 4

enum { DEV_NONE, DEV_HDA, DEV_AC97 };
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
    hda = (volatile uint8_t *)bar;
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
    R32(0x40) = (uint32_t)corb; R32(0x44) = 0;
    R8(0x4E) = (R8(0x4E) & 0xF0) | 2;
    R16(0x4A) = 0x8000; for (int i = 0; i < 1000 && !(R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x4A) = 0; for (int i = 0; i < 1000 && (R16(0x4A) & 0x8000); i++) spin(1);
    R16(0x48) = 0;
    R32(0x50) = (uint32_t)rirb; R32(0x54) = 0;
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
    for (int i = 0; i < NBUF; i++) { bdl[i * 4] = (uint32_t)(ring + i * BUFFRAMES * 2); bdl[i * 4 + 1] = 0; bdl[i * 4 + 2] = BUFBYTES; bdl[i * 4 + 3] = 0; }
    R32(sd + 0x08) = NBUF * BUFBYTES;
    R16(sd + 0x0C) = NBUF - 1;
    R16(sd + 0x12) = 0x0011;
    R32(sd + 0x18) = (uint32_t)bdl; R32(sd + 0x1C) = 0;
    R32(sd) = (R32(sd) & 0xFF0FFFFF) | (1u << 20);          /* stream tag 1 */
    R8(sd) |= 2;                                             /* run */
    snprintf(devname, sizeof devname, "High Definition Audio (%04x:%04x, codec %08x)", p->vendor, p->device, vid);
    klog("hda: %s, DAC %d, %d output pin(s)%s", devname, dac, pins, use_immediate ? ", immediate commands" : "");
    return true;
}
static int hda_position(void) { return (R32(sd + 0x04) / BUFBYTES) % NBUF; }

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
    outw(nam + 0x02, 0x0000);                                /* master: 0 dB, unmuted */
    outw(nam + 0x04, 0x0000);                                /* headphones */
    outw(nam + 0x18, 0x0808);                                /* PCM out */
    if (inw(nam + 0x28) & 1) { outw(nam + 0x2A, inw(nam + 0x2A) | 1); outw(nam + 0x2C, RATE); }   /* variable rate: pin to 48 kHz */
    outb(nabm + 0x1B, 0x02);                                 /* reset PCM-out box */
    for (int i = 0; i < 1000 && (inb(nabm + 0x1B) & 2); i++) io_wait();
    ac_bdl = kmalloc_aligned(NBUF * 8, 8);
    for (int i = 0; i < NBUF; i++) { ac_bdl[i * 2] = (uint32_t)(ring + i * BUFFRAMES * 2); ac_bdl[i * 2 + 1] = BUFFRAMES * 2; }
    outl(nabm + 0x10, (uint32_t)ac_bdl);
    snprintf(devname, sizeof devname, "AC'97 Audio Controller (%04x:%04x)", p->vendor, p->device);
    klog("ac97: %s, mixer %x, bus master %x", devname, nam, nabm);
    return true;
}
static int ac97_position(void) { return inb(nabm + 0x14) % NBUF; }

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
static void refill(void) {
    int cur = dev == DEV_HDA ? hda_position() : ac97_position();
    while (((next_fill - cur + NBUF) % NBUF) < LEAD) {
        mix_into(ring + next_fill * BUFFRAMES * 2, BUFFRAMES);
        next_fill = (next_fill + 1) % NBUF;
    }
    if (dev == DEV_AC97) {
        outb(nabm + 0x15, (next_fill + NBUF - 1) % NBUF);     /* last valid buffer */
        if (!(inb(nabm + 0x1B) & 1)) outb(nabm + 0x1B, 0x01);   /* (re)start */
    }
}
static void audio_task(void *arg) { for (;;) { refill(); task_sleep(4); } }

void audio_init(void) {
    pci_dev_t devs[48]; int n = pci_scan(devs, 48);
    ring = kmalloc_aligned(NBUF * BUFBYTES, 128);
    memset(ring, 0, NBUF * BUFBYTES);
    for (int i = 0; i < n && !dev; i++) if (devs[i].cls == 0x04 && devs[i].sub == 0x03 && hda_init(&devs[i])) dev = DEV_HDA;
    for (int i = 0; i < n && !dev; i++) if (devs[i].cls == 0x04 && devs[i].sub == 0x01 && devs[i].vendor == 0x8086 && ac97_init(&devs[i])) dev = DEV_AC97;
    if (!dev) { klog("audio: no supported sound device (Intel HDA or AC'97)"); return; }
    next_fill = 0;
    refill();
    task_create("audiod", audio_task, NULL, 16384);
}
bool audio_present(void) { return dev != DEV_NONE; }
const char *audio_device_name(void) { return devname; }
int audio_play(const sound_t *s) {
    if (!dev || !s || !s->frames) return -1;
    for (int v = 0; v < NVOICE; v++) if (!voices[v].active) { voices[v].s = s; voices[v].pos = 0; voices[v].active = true; return v; }
    voices[0].s = s; voices[0].pos = 0; voices[0].active = true;     /* all busy: replace the oldest */
    return 0;
}
void audio_stop_all(void) { for (int v = 0; v < NVOICE; v++) voices[v].active = false; }
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
            for (int v = 0; v < NVOICE; v++) if (voices[v].s == c->s) voices[v].active = false;
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
