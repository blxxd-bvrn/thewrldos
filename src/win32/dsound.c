/* dsound.dll: DirectSound for Win32 programs, on the kernel's sound streams.
 *
 * IDirectSound(8) makes sound buffers. Secondary buffers hold 8- or 16-bit PCM, mono or
 * stereo, at any rate; a kernel mixer task mixes the playing ones (resampled, with volume,
 * pan and frequency) into one stream at the sound card's rate, advancing each buffer's play
 * cursor, looping or stopping at the end, and signalling position notifications
 * (IDirectSoundNotify). The primary buffer only takes the format and volume. 3D buffers
 * and the listener (IDirectSound3DBuffer/Listener) accept every call and play as plain
 * stereo. There is no capture. Method tables: tools/gen_com.py from mingw-w64's dsound.h. */
#include "w32.h"
#include "audio.h"

#define P w32_cur()
typedef int32_t HRESULT;
#define DS_OK 0
#define DSERR_INVALIDPARAM ((HRESULT)0x80070057)
#define DSERR_NOINTERFACE ((HRESULT)0x80004002)
#define DSERR_UNSUPPORTED ((HRESULT)0x80004001)
#define DSERR_NODRIVER ((HRESULT)0x88780078)
#define DSERR_OUTOFMEMORY ((HRESULT)0x8007000E)
#define DSERR_INVALIDCALL ((HRESULT)0x88780032)

/* ---------------- method tables ---------------- */
enum { IF_DS, IF_BUF, IF_3DBUF, IF_LISTENER, IF_NOTIFY, IF_COUNT };
typedef struct { int iface, index, nargs; const char *name; } method_t;
static const method_t methods[] = {
#define IDirectSound8 IF_DS
#define IDirectSoundBuffer8 IF_BUF
#define IDirectSound3DBuffer IF_3DBUF
#define IDirectSound3DListener IF_LISTENER
#define IDirectSoundNotify IF_NOTIFY
#define M(i, idx, name, n) { i, idx, n, #name },
#include "dsound_methods.h"
#undef M
};
typedef uintptr_t A;
/* what is not implemented succeeds (3D parameters and the like), with the right argument count */
#define STUB(n, ...) static HRESULT WINAPI stub##n(void *self, ##__VA_ARGS__) { (void)self; return DS_OK; }
STUB(0) STUB(1, A a) STUB(2, A a, A b) STUB(3, A a, A b, A c) STUB(4, A a, A b, A c, A d) STUB(5, A a, A b, A c, A d, A e)
STUB(6, A a, A b, A c, A d, A e, A f) STUB(7, A a, A b, A c, A d, A e, A f, A g)
static void *const stubs[] = { stub0, stub1, stub2, stub3, stub4, stub5, stub6, stub7 };
typedef struct { int iface; const char *name; void *fn; } impl_t;
static void *vtables[IF_COUNT][32];
static bool vt_ready;
static void build_vtables(void);

/* ---------------- objects ---------------- */
typedef struct { void **vt; int refs; } ds_t;
typedef struct { uint32_t ofs; uintptr_t event; } notify_t;
typedef struct buf {
    void **vt; int refs;
    struct buf *next;                       /* the mixer's list */
    int pid;
    bool primary, playing, looping, dead;
    uint8_t *data; uint32_t size;
    int ch, bits; uint32_t rate, freq;      /* the format; freq can be changed */
    uint32_t align;
    int32_t volume, pan;                    /* hundredths of a decibel */
    int gain_l, gain_r;                     /* 0..4096 */
    uint64_t pos;                           /* play position in source frames, 32.32 fixed point */
    uint32_t flags;
    notify_t notes[32]; int nnotes;
    void *sub[2];                           /* its 3D-buffer and notify interfaces */
} buf_t;
typedef struct { void **vt; int refs; buf_t *owner; } sub_t;

static buf_t *bufs;                         /* every secondary buffer of every program */
static int mixer_stream = -1, mixer_cap;
static bool mixer_running;

static void *com_new(int iface, size_t size) {
    build_vtables();
    ds_t *o = kzalloc(size);
    if (!o) return NULL;
    o->vt = vtables[iface]; o->refs = 1;
    return o;
}
/* decibels (hundredths) to a linear gain out of 4096: 10^(v/2000) */
static int db_gain(int32_t v) {
    if (v >= 0) return 4096;
    if (v <= -10000) return 0;
    float y = v * 0.0011512925f / 1024;                          /* ln(10)/2000, spread over 2^10 squarings */
    float g = 1 + y;
    for (int i = 0; i < 10; i++) g *= g;
    return (int)(g * 4096);
}
static void set_gains(buf_t *b) {
    int g = db_gain(b->volume);
    int l = b->pan > 0 ? db_gain(-b->pan) : 4096, r = b->pan < 0 ? db_gain(b->pan) : 4096;
    b->gain_l = g * l / 4096; b->gain_r = g * r / 4096;
}
static uint32_t play_byte(buf_t *b) { uint32_t f = (uint32_t)(b->pos >> 32); return (f * b->align) % (b->size ? b->size : 1); }

/* ---------------- the mixer ---------------- */
static void signal_note(buf_t *b, uintptr_t ev) { w32_proc_t *p = pe_find(b->pid); if (p) w32_event_signal(p, ev); }
static void mixer_task(void *arg) {
    (void)arg;
    int32_t *acc = kmalloc(sizeof(int32_t) * 2 * 2048);
    int16_t *out = kmalloc(sizeof(int16_t) * 2 * 2048);
    for (;;) {
        bool any = false;
        uintptr_t fl = irq_save();
        for (buf_t **pp = &bufs; *pp; ) {                        /* drop the buffers of programs that are gone */
            buf_t *b = *pp;
            if (b->dead || !pe_find(b->pid)) { *pp = b->next; kfree(b->data); kfree(b->sub[0]); kfree(b->sub[1]); kfree(b); continue; }
            if (b->playing) any = true;
            pp = &b->next;
        }
        irq_restore(fl);
        if (!any || mixer_stream < 0) { task_sleep(10); continue; }
        int rate = audio_rate();
        /* keep about 40 ms in our own ring; audio_stream_queued would also count the
         * card's fixed lead, which alone is longer than that */
        int room = audio_stream_room(mixer_stream);
        int want = rate * 40 / 1000 - (mixer_cap - room);
        int n = MIN(MIN(want, room), 2048);
        if (n < rate / 200) { task_sleep(4); continue; }
        memset(acc, 0, sizeof(int32_t) * 2 * n);
        for (buf_t *b = bufs; b; b = b->next) {
            if (!b->playing || !b->size || b->dead) continue;
            uint32_t frames = b->size / b->align;
            uint64_t step = ((uint64_t)b->freq << 32) / (uint32_t)rate;
            uint32_t before = play_byte(b);
            bool stopped = false;
            for (int i = 0; i < n; i++) {
                uint32_t f = (uint32_t)(b->pos >> 32);
                if (f >= frames) {
                    if (b->looping) { b->pos -= (uint64_t)frames << 32; f -= frames; }
                    else { b->playing = false; b->pos = 0; stopped = true; break; }
                }
                const uint8_t *s = b->data + f * b->align;
                int l, r;
                if (b->bits == 16) { l = ((const int16_t *)s)[0]; r = b->ch > 1 ? ((const int16_t *)s)[1] : l; }
                else { l = (s[0] - 128) << 8; r = b->ch > 1 ? (s[1] - 128) << 8 : l; }
                acc[2 * i] += l * b->gain_l >> 12; acc[2 * i + 1] += r * b->gain_r >> 12;
                b->pos += step;
            }
            uint32_t after = play_byte(b);
            for (int k = 0; k < b->nnotes; k++) {                /* notifications passed in this slice */
                uint32_t o = b->notes[k].ofs;
                if (o == 0xFFFFFFFF) { if (stopped) signal_note(b, b->notes[k].event); continue; }
                bool hit = before <= after ? (o >= before && o < after) : (o >= before || o < after);
                if (hit) signal_note(b, b->notes[k].event);
            }
        }
        for (int i = 0; i < 2 * n; i++) out[i] = (int16_t)CLAMP(acc[i], -32768, 32767);
        audio_stream_write(mixer_stream, out, n);
    }
}
static bool mixer_start(void) {
    if (!audio_present()) return false;
    if (mixer_stream < 0) { mixer_stream = audio_stream_open(); mixer_cap = audio_stream_room(mixer_stream); }
    if (mixer_stream < 0) return false;
    if (!mixer_running) { mixer_running = true; task_create("dsound mixer", mixer_task, NULL, 16384); }
    return true;
}

/* ---------------- IUnknown ---------------- */
static uint32_t WINAPI AddRef(ds_t *s) { return ++s->refs; }
static uint32_t WINAPI ReleaseDS(ds_t *s) { if (--s->refs > 0) return s->refs; s->vt = NULL; kfree(s); return 0; }
static uint32_t WINAPI ReleaseBuf(buf_t *b) {
    if (--b->refs > 0) return b->refs;
    if (b->primary) { kfree(b); return 0; }
    b->playing = false; b->dead = true;                          /* the mixer frees it */
    return 0;
}
static uint32_t WINAPI SubAddRef(sub_t *s) { return AddRef((ds_t *)s->owner); }
static uint32_t WINAPI SubRelease(sub_t *s) { return ReleaseBuf(s->owner); }
static HRESULT WINAPI QueryInterfaceDS(ds_t *s, const uint32_t *iid, void **out) { if (!out) return DSERR_INVALIDPARAM; s->refs++; *out = s; (void)iid; return DS_OK; }
/* a buffer answers for IDirectSoundBuffer(8), IDirectSound3DBuffer, IDirectSound3DListener and IDirectSoundNotify */
static HRESULT WINAPI QueryInterfaceBuf(buf_t *b, const uint32_t *iid, void **out) {
    if (!out || !iid) return DSERR_INVALIDPARAM;
    *out = NULL;
    uint32_t d1 = iid[0];
    if (d1 == 0x279AFA85 || d1 == 0x6825A449 || d1 == 0) { b->refs++; *out = b; return DS_OK; }    /* IDirectSoundBuffer, IDirectSoundBuffer8, IUnknown */
    int which = d1 == 0x279AFA86 ? IF_3DBUF : d1 == 0x279AFA84 ? IF_LISTENER : d1 == 0xB0210783 ? IF_NOTIFY : -1;
    if (which < 0) return DSERR_NOINTERFACE;
    int slot = which == IF_NOTIFY ? 1 : 0;
    if (!b->sub[slot]) { sub_t *s = com_new(which, sizeof(sub_t)); if (!s) return DSERR_OUTOFMEMORY; s->owner = b; b->sub[slot] = s; }
    sub_t *s = b->sub[slot]; s->vt = vtables[which];
    b->refs++; *out = s;
    return DS_OK;
}
static HRESULT WINAPI SubQueryInterface(sub_t *s, const uint32_t *iid, void **out) { return QueryInterfaceBuf(s->owner, iid, out); }

/* ---------------- IDirectSound8 ---------------- */
typedef struct { uint16_t tag, ch; uint32_t rate, avg; uint16_t align, bits, cb; } WFX;
static HRESULT WINAPI CreateSoundBuffer(ds_t *s, const uint32_t *desc, buf_t **out, void *outer) {
    if (!desc || !out || desc[0] < 20) return DSERR_INVALIDPARAM;
    *out = NULL;
    uint32_t flags = desc[1], bytes = desc[2];
    const WFX *wf = *(const WFX *const *)(desc + 4);
    buf_t *b = com_new(IF_BUF, sizeof(buf_t));
    if (!b) return DSERR_OUTOFMEMORY;
    b->pid = P->pid; b->flags = flags; b->volume = 0; b->pan = 0; set_gains(b);
    if (flags & 1) {                                             /* DSBCAPS_PRIMARYBUFFER */
        b->primary = true; b->ch = 2; b->bits = 16; b->rate = b->freq = 44100; b->align = 4;
        *out = b; return DS_OK;
    }
    if (!wf || (wf->tag != 1 && wf->tag != 0xFFFE) || (wf->bits != 8 && wf->bits != 16) || !wf->ch || wf->ch > 2 || !wf->rate || bytes < 4 || bytes > (64u << 20)) { kfree(b); return DSERR_INVALIDPARAM; }
    b->ch = wf->ch; b->bits = wf->bits; b->rate = b->freq = wf->rate; b->align = wf->ch * wf->bits / 8;
    b->size = bytes - bytes % b->align;
    b->data = kzalloc(b->size);
    if (!b->data) { kfree(b); return DSERR_OUTOFMEMORY; }
    if (b->bits == 8) memset(b->data, 0x80, b->size);
    mixer_start();
    uintptr_t fl = irq_save(); b->next = bufs; bufs = b; irq_restore(fl);
    *out = b;
    return DS_OK;
}
/* DSCAPS */
static HRESULT WINAPI GetCapsDS(ds_t *s, uint32_t *c) {
    if (!c || c[0] < 8) return DSERR_INVALIDPARAM;
    uint32_t sz = c[0]; memset(c + 1, 0, sz - 4);
    c[1] = 0x1 | 0x4 | 0x8 | 0x400 | 0x800 | 0x20;               /* continuous rate, mono/stereo, 8/16-bit, secondary... */
    c[2] = 100; c[3] = 200000;                                   /* min/max secondary sample rate */
    c[4] = 1; c[5] = 64; c[6] = 64; c[7] = 64;                   /* primary buffers, max hw mixing buffers */
    return DS_OK;
}
static HRESULT WINAPI DuplicateSoundBuffer(ds_t *s, buf_t *src, buf_t **out) {
    if (!src || !out || src->primary) return DSERR_INVALIDPARAM;
    buf_t *b = com_new(IF_BUF, sizeof(buf_t));
    if (!b) return DSERR_OUTOFMEMORY;
    *b = *src; b->vt = vtables[IF_BUF]; b->refs = 1; b->playing = false; b->pos = 0; b->nnotes = 0; b->sub[0] = b->sub[1] = NULL; b->pid = P->pid;
    b->data = kmalloc(src->size); if (!b->data) { kfree(b); return DSERR_OUTOFMEMORY; }
    memcpy(b->data, src->data, src->size);
    uintptr_t fl = irq_save(); b->next = bufs; bufs = b; irq_restore(fl);
    *out = b;
    return DS_OK;
}
static HRESULT WINAPI SetCooperativeLevel(ds_t *s, uintptr_t hwnd, uint32_t level) { (void)s; (void)hwnd; (void)level; return DS_OK; }
static HRESULT WINAPI Compact(ds_t *s) { (void)s; return DS_OK; }
static HRESULT WINAPI GetSpeakerConfig(ds_t *s, uint32_t *c) { (void)s; if (!c) return DSERR_INVALIDPARAM; *c = 4; return DS_OK; }   /* DSSPEAKER_STEREO */
static HRESULT WINAPI SetSpeakerConfig(ds_t *s, uint32_t c) { (void)s; (void)c; return DS_OK; }
static HRESULT WINAPI InitializeDS(ds_t *s, const void *g) { (void)s; (void)g; return DS_OK; }
static HRESULT WINAPI VerifyCertification(ds_t *s, uint32_t *c) { (void)s; if (c) *c = 1; return DS_OK; }   /* DS_CERTIFIED */

/* ---------------- IDirectSoundBuffer8 ---------------- */
static HRESULT WINAPI GetCapsBuf(buf_t *b, uint32_t *c) {
    if (!c || c[0] < 20) return DSERR_INVALIDPARAM;
    c[1] = b->flags | 0x8; c[2] = b->size; c[3] = 0; c[4] = 0;    /* DSBCAPS_LOCSOFTWARE */
    return DS_OK;
}
static HRESULT WINAPI GetCurrentPosition(buf_t *b, uint32_t *play, uint32_t *write) {
    uint32_t p = b->primary ? 0 : play_byte(b);
    uint32_t lead = (b->freq * 20 / 1000) * b->align;            /* the write cursor: 20 ms past the play cursor */
    if (play) *play = p;
    if (write) *write = b->size ? (p + (b->playing ? lead : 0)) % b->size : 0;
    return DS_OK;
}
static HRESULT WINAPI GetFormat(buf_t *b, WFX *w, uint32_t n, uint32_t *written) {
    if (written) *written = sizeof(WFX);
    if (!w) return DS_OK;
    if (n < 16) return DSERR_INVALIDPARAM;
    WFX f = { 1, (uint16_t)b->ch, b->rate, b->rate * b->align, (uint16_t)b->align, (uint16_t)b->bits, 0 };
    memcpy(w, &f, MIN(n, (uint32_t)sizeof f));
    return DS_OK;
}
static HRESULT WINAPI GetVolume(buf_t *b, int32_t *v) { if (!v) return DSERR_INVALIDPARAM; *v = b->volume; return DS_OK; }
static HRESULT WINAPI GetPan(buf_t *b, int32_t *v) { if (!v) return DSERR_INVALIDPARAM; *v = b->pan; return DS_OK; }
static HRESULT WINAPI GetFrequency(buf_t *b, uint32_t *v) { if (!v) return DSERR_INVALIDPARAM; *v = b->freq; return DS_OK; }
static HRESULT WINAPI GetStatus(buf_t *b, uint32_t *s) { if (!s) return DSERR_INVALIDPARAM; *s = (b->playing || b->primary ? 1 : 0) | (b->looping ? 4 : 0); return DS_OK; }
static HRESULT WINAPI InitializeBuf(buf_t *b, void *ds, const void *desc) { (void)b; (void)ds; (void)desc; return 0x88780082; }   /* DSERR_ALREADYINITIALIZED */
/* Lock: up to two pieces when the region wraps past the end */
static HRESULT WINAPI Lock(buf_t *b, uint32_t ofs, uint32_t bytes, void **p1, uint32_t *n1, void **p2, uint32_t *n2, uint32_t flags) {
    if (b->primary || !b->size || !p1 || !n1) return DSERR_INVALIDCALL;
    if (flags & 1) GetCurrentPosition(b, NULL, &ofs);            /* DSBLOCK_FROMWRITECURSOR */
    if (flags & 2) { ofs = 0; bytes = b->size; }                 /* DSBLOCK_ENTIREBUFFER */
    if (ofs >= b->size || bytes > b->size || !bytes) return DSERR_INVALIDPARAM;
    uint32_t first = MIN(bytes, b->size - ofs);
    *p1 = b->data + ofs; *n1 = first;
    if (p2) *p2 = first < bytes ? b->data : NULL;
    if (n2) *n2 = first < bytes ? bytes - first : 0;
    return DS_OK;
}
static HRESULT WINAPI Unlock(buf_t *b, void *p1, uint32_t n1, void *p2, uint32_t n2) { (void)b; (void)p1; (void)n1; (void)p2; (void)n2; return DS_OK; }
static HRESULT WINAPI Play(buf_t *b, uint32_t res, uint32_t prio, uint32_t flags) {
    if (b->primary) return DS_OK;
    if (!mixer_start()) return DSERR_NODRIVER;
    b->looping = flags & 1;                                      /* DSBPLAY_LOOPING */
    b->playing = true;
    return DS_OK;
}
static HRESULT WINAPI SetCurrentPosition(buf_t *b, uint32_t pos) {
    if (b->primary || !b->align) return DSERR_INVALIDCALL;
    b->pos = (uint64_t)((pos % (b->size ? b->size : 1)) / b->align) << 32;
    return DS_OK;
}
static HRESULT WINAPI SetFormat(buf_t *b, const WFX *w) { (void)b; return w ? DS_OK : DSERR_INVALIDPARAM; }   /* the primary format: the mixer converts anyway */
static HRESULT WINAPI SetVolume(buf_t *b, int32_t v) { if (v > 0 || v < -10000) return DSERR_INVALIDPARAM; b->volume = v; set_gains(b); return DS_OK; }
static HRESULT WINAPI SetPan(buf_t *b, int32_t v) { if (v > 10000 || v < -10000) return DSERR_INVALIDPARAM; b->pan = v; set_gains(b); return DS_OK; }
static HRESULT WINAPI SetFrequency(buf_t *b, uint32_t f) { if (b->primary) return DSERR_INVALIDCALL; b->freq = f ? CLAMP(f, 100u, 200000u) : b->rate; return DS_OK; }
static HRESULT WINAPI Stop(buf_t *b) {
    bool was = b->playing; b->playing = false;
    if (was) for (int k = 0; k < b->nnotes; k++) if (b->notes[k].ofs == 0xFFFFFFFF) signal_note(b, b->notes[k].event);   /* DSBPN_OFFSETSTOP */
    return DS_OK;
}
static HRESULT WINAPI Restore(buf_t *b) { (void)b; return DS_OK; }
static HRESULT WINAPI SetFX(buf_t *b, uint32_t n, const void *d, uint32_t *r) { (void)b; (void)d; if (r) for (uint32_t i = 0; i < n; i++) r[i] = 0; return n ? DSERR_UNSUPPORTED : DS_OK; }
static HRESULT WINAPI AcquireResources(buf_t *b, uint32_t f, uint32_t n, uint32_t *r) { (void)b; (void)f; if (r) for (uint32_t i = 0; i < n; i++) r[i] = 0; return DS_OK; }
static HRESULT WINAPI GetObjectInPath(buf_t *b, const void *g, uint32_t i, const void *iid, void **out) { (void)b; (void)g; (void)i; (void)iid; if (out) *out = NULL; return DSERR_UNSUPPORTED; }

/* IDirectSoundNotify: DSBPOSITIONNOTIFY { dwOffset, hEventNotify } */
static HRESULT WINAPI SetNotificationPositions(sub_t *s, uint32_t n, const uint8_t *pn) {
    buf_t *b = s->owner;
    if (n && !pn) return DSERR_INVALIDPARAM;
    if (b->playing) return DSERR_INVALIDCALL;
    b->nnotes = 0;
    size_t sz = 4 + sizeof(uintptr_t) + (sizeof(uintptr_t) == 8 ? 4 : 0);
    for (uint32_t i = 0; i < n && b->nnotes < 32; i++) {
        const uint8_t *e = pn + i * sz;
        b->notes[b->nnotes].ofs = *(const uint32_t *)e;
        b->notes[b->nnotes].event = *(const uintptr_t *)(e + sz - sizeof(uintptr_t));
        b->nnotes++;
    }
    return DS_OK;
}

static const impl_t impls[] = {
    { IF_DS, "QueryInterface", QueryInterfaceDS }, { IF_DS, "AddRef", AddRef }, { IF_DS, "Release", ReleaseDS },
    { IF_DS, "CreateSoundBuffer", CreateSoundBuffer }, { IF_DS, "GetCaps", GetCapsDS }, { IF_DS, "DuplicateSoundBuffer", DuplicateSoundBuffer },
    { IF_DS, "SetCooperativeLevel", SetCooperativeLevel }, { IF_DS, "Compact", Compact }, { IF_DS, "GetSpeakerConfig", GetSpeakerConfig },
    { IF_DS, "SetSpeakerConfig", SetSpeakerConfig }, { IF_DS, "Initialize", InitializeDS }, { IF_DS, "VerifyCertification", VerifyCertification },
    { IF_BUF, "QueryInterface", QueryInterfaceBuf }, { IF_BUF, "AddRef", AddRef }, { IF_BUF, "Release", ReleaseBuf },
    { IF_BUF, "GetCaps", GetCapsBuf }, { IF_BUF, "GetCurrentPosition", GetCurrentPosition }, { IF_BUF, "GetFormat", GetFormat },
    { IF_BUF, "GetVolume", GetVolume }, { IF_BUF, "GetPan", GetPan }, { IF_BUF, "GetFrequency", GetFrequency }, { IF_BUF, "GetStatus", GetStatus },
    { IF_BUF, "Initialize", InitializeBuf }, { IF_BUF, "Lock", Lock }, { IF_BUF, "Play", Play }, { IF_BUF, "SetCurrentPosition", SetCurrentPosition },
    { IF_BUF, "SetFormat", SetFormat }, { IF_BUF, "SetVolume", SetVolume }, { IF_BUF, "SetPan", SetPan }, { IF_BUF, "SetFrequency", SetFrequency },
    { IF_BUF, "Stop", Stop }, { IF_BUF, "Unlock", Unlock }, { IF_BUF, "Restore", Restore }, { IF_BUF, "SetFX", SetFX },
    { IF_BUF, "AcquireResources", AcquireResources }, { IF_BUF, "GetObjectInPath", GetObjectInPath },
    { IF_3DBUF, "QueryInterface", SubQueryInterface }, { IF_3DBUF, "AddRef", SubAddRef }, { IF_3DBUF, "Release", SubRelease },
    { IF_LISTENER, "QueryInterface", SubQueryInterface }, { IF_LISTENER, "AddRef", SubAddRef }, { IF_LISTENER, "Release", SubRelease },
    { IF_NOTIFY, "QueryInterface", SubQueryInterface }, { IF_NOTIFY, "AddRef", SubAddRef }, { IF_NOTIFY, "Release", SubRelease },
    { IF_NOTIFY, "SetNotificationPositions", SetNotificationPositions },
};
static void build_vtables(void) {
    if (vt_ready) return;
    for (unsigned i = 0; i < ARRAY_LEN(methods); i++) {
        const method_t *m = &methods[i];
        void *fn = stubs[MIN(m->nargs, 7)];
        for (unsigned k = 0; k < ARRAY_LEN(impls); k++) if (impls[k].iface == m->iface && !strcmp(impls[k].name, m->name)) { fn = impls[k].fn; break; }
        vtables[m->iface][m->index] = fn;
    }
    vt_ready = true;
}

/* ---------------- exports ---------------- */
static HRESULT WINAPI DirectSoundCreate8(const void *guid, ds_t **out, void *outer) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (!audio_present()) return DSERR_NODRIVER;
    ds_t *d = com_new(IF_DS, sizeof(ds_t));
    if (!d) return DSERR_OUTOFMEMORY;
    *out = d;
    return DS_OK;
}
static HRESULT WINAPI DirectSoundCreate(const void *guid, ds_t **out, void *outer) { return DirectSoundCreate8(guid, out, outer); }
/* the callback sees the primary sound driver, then the sound card */
static HRESULT WINAPI DirectSoundEnumerateA(void *cb, void *ctx) {
    if (!cb || !audio_present()) return DS_OK;
    static const uint32_t dev[4] = { 0xDEF00000, 0x11D29A2B, 0xD13CFC8A, 0x5751F10C };
    if (!((int (WINAPI *)(const void *, const char *, const char *, void *))cb)(NULL, "Primary Sound Driver", "", ctx)) return DS_OK;
    ((int (WINAPI *)(const void *, const char *, const char *, void *))cb)(dev, audio_device_name(), "wrldsnd.sys", ctx);
    return DS_OK;
}
static HRESULT WINAPI DirectSoundEnumerateW(void *cb, void *ctx) {
    if (!cb || !audio_present()) return DS_OK;
    static const uint32_t dev[4] = { 0xDEF00000, 0x11D29A2B, 0xD13CFC8A, 0x5751F10C };
    uint16_t a[64], b[64], e[1] = { 0 }; const char *n = audio_device_name(); int i = 0;
    const char *p = "Primary Sound Driver"; for (i = 0; p[i]; i++) a[i] = p[i]; a[i] = 0;
    for (i = 0; n[i] && i < 63; i++) b[i] = (uint8_t)n[i]; b[i] = 0;
    if (!((int (WINAPI *)(const void *, const uint16_t *, const uint16_t *, void *))cb)(NULL, a, e, ctx)) return DS_OK;
    ((int (WINAPI *)(const void *, const uint16_t *, const uint16_t *, void *))cb)(dev, b, e, ctx);
    return DS_OK;
}
static HRESULT WINAPI DirectSoundCaptureCreate(const void *g, void **out, void *outer) { if (out) *out = NULL; return DSERR_NODRIVER; }
static HRESULT WINAPI DirectSoundCaptureEnumerateA(void *cb, void *ctx) { (void)cb; (void)ctx; return DS_OK; }
static HRESULT WINAPI GetDeviceID(const void *in, uint32_t *out) { static const uint32_t dev[4] = { 0xDEF00000, 0x11D29A2B, 0xD13CFC8A, 0x5751F10C }; if (!out) return DSERR_INVALIDPARAM; memcpy(out, dev, 16); return DS_OK; }
static HRESULT WINAPI DllCanUnloadNow(void) { return 1; }

#define X(n) { "dsound", #n, (void *)n }
const w32_api_t dsound_api[] = {
    X(DirectSoundCreate), X(DirectSoundCreate8), X(DirectSoundEnumerateA), X(DirectSoundEnumerateW), X(DirectSoundCaptureCreate),
    { "dsound", "DirectSoundCaptureCreate8", (void *)DirectSoundCaptureCreate }, X(DirectSoundCaptureEnumerateA),
    { "dsound", "DirectSoundCaptureEnumerateW", (void *)DirectSoundCaptureEnumerateA }, X(GetDeviceID), X(DllCanUnloadNow),
    { "dsound", "Ordinal1", (void *)DirectSoundCreate }, { "dsound", "Ordinal2", (void *)DirectSoundEnumerateA }, { "dsound", "Ordinal3", (void *)DirectSoundEnumerateW },
    { "dsound", "Ordinal11", (void *)DirectSoundCreate8 },
    { NULL, NULL, NULL }
};
