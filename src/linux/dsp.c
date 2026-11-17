/* /dev/dsp: sound for Linux programs, through the Open Sound System interface
 * (what GStreamer's osssink and many older programs speak). A program picks a
 * format, channel count and rate with ioctls and writes samples; they are
 * converted to the mixer's 16-bit stereo at audio_rate() and handed to a
 * stream of THE WRLD OS's mixer (src/kernel/audio.c), so they play alongside
 * the desktop's own sounds. The device exists when the OS has a sound card. */
#include "kernel.h"
#ifdef __x86_64__
#include "audio.h"
#include "lx.h"

enum { AFMT_U8 = 0x08, AFMT_S16_LE = 0x10 };
typedef struct {
    int stream;
    int rate, chans, fmt;            /* what the program writes */
    int64_t phase;                   /* resampler position, in input frames (32.32 fixed point) */
    int16_t last[2];                 /* the previous input frame, for interpolation */
    uint8_t part[4]; int npart;      /* the bytes of a frame split across two writes */
    uint64_t written;                /* bytes taken from the program */
} dsp_t;

static int bpf(const dsp_t *d) { return (d->fmt == AFMT_U8 ? 1 : 2) * d->chans; }

void *dsp_open(int *err) {
    if (!audio_present()) { *err = -ENODEV; return NULL; }
    dsp_t *d = kzalloc(sizeof *d);
    if (!d) { *err = -ENOMEM; return NULL; }
    d->stream = audio_stream_open();
    if (d->stream < 0) { kfree(d); *err = -EBUSY; return NULL; }
    d->rate = 8000; d->chans = 1; d->fmt = AFMT_U8;   /* OSS's defaults */
    return d;
}
void dsp_release(void *o) { dsp_t *d = o; if (!d) return; audio_stream_close(d->stream); kfree(d); }
bool dsp_writable(void *o) { dsp_t *d = o; return d && audio_stream_room(d->stream) >= 512; }

/* one input frame as 16-bit stereo */
static void frame_at(const dsp_t *d, const uint8_t *src, int i, int16_t out[2]) {
    if (d->fmt == AFMT_U8) {
        const uint8_t *p = src + i * d->chans;
        out[0] = (int16_t)((p[0] - 128) << 8); out[1] = d->chans > 1 ? (int16_t)((p[1] - 128) << 8) : out[0];
    } else {
        const int16_t *p = (const int16_t *)(src + i * 2 * d->chans);
        out[0] = p[0]; out[1] = d->chans > 1 ? p[1] : p[0];
    }
}
/* convert whole input frames to the mixer's rate and queue them, waiting for room */
static int64_t put_frames(dsp_t *d, const uint8_t *src, int n, bool nonblock, bool *stopped) {
    int out_rate = audio_rate();
    int64_t step = ((int64_t)d->rate << 32) / out_rate;
    int16_t buf[512 * 2]; int nb = 0;
    int64_t limit = (int64_t)n << 32;
    while (d->phase < limit) {
        int i = (int)(d->phase >> 32); uint32_t f = (uint32_t)d->phase >> 16;   /* 16-bit fraction */
        int16_t a[2], b[2];
        if (i == 0) { a[0] = d->last[0]; a[1] = d->last[1]; } else frame_at(d, src, i - 1, a);
        frame_at(d, src, i, b);
        buf[nb * 2] = (int16_t)(a[0] + (((int32_t)(b[0] - a[0]) * (int32_t)f) >> 16));
        buf[nb * 2 + 1] = (int16_t)(a[1] + (((int32_t)(b[1] - a[1]) * (int32_t)f) >> 16));
        d->phase += step;
        if (++nb == 512 || d->phase >= limit) {
            int at = 0;
            while (at < nb) {
                int k = audio_stream_write(d->stream, buf + at * 2, nb - at);
                if (k < 0) { *stopped = true; return -EIO; }
                at += k;
                if (at < nb) {
                    if (nonblock) { *stopped = true; return 0; }   /* the rest of this frame batch is dropped */
                    if (lx_block_interrupted(lx_current())) { *stopped = true; return -EINTR; }
                    task_sleep(5);
                }
            }
            nb = 0;
        }
    }
    d->phase -= limit;
    if (n) frame_at(d, src, n - 1, d->last);
    return 0;
}
int64_t dsp_write(void *o, const uint8_t *src, uint64_t n, bool nonblock) {
    dsp_t *d = o;
    if (!d) return -EBADF;
    if (nonblock && audio_stream_room(d->stream) < 512) return -EAGAIN;
    int fb = bpf(d);
    uint64_t done = 0;
    if (d->npart) {                                    /* finish a frame begun in the last write */
        while (d->npart < fb && done < n) d->part[d->npart++] = src[done++];
        if (d->npart < fb) { d->written += done; return (int64_t)done; }
        bool stop = false; put_frames(d, d->part, 1, nonblock, &stop); d->npart = 0;
    }
    while (done + fb <= n) {
        int frames = (int)MIN((n - done) / fb, (uint64_t)4096);
        bool stop = false;
        int64_t r = put_frames(d, src + done, frames, nonblock, &stop);
        if (r < 0 && !done) return r;
        done += (uint64_t)frames * fb;
        if (stop) break;
    }
    while (done < n && n - done < (uint64_t)fb && d->npart < fb) d->part[d->npart++] = src[done++];
    d->written += done;
    return (int64_t)done;
}

/* the OSS ioctls a player uses */
#define SNDCTL_DSP_RESET       0x5000
#define SNDCTL_DSP_SYNC        0x5001
#define SNDCTL_DSP_SPEED       0xC0045002
#define SNDCTL_DSP_STEREO      0xC0045003
#define SNDCTL_DSP_GETBLKSIZE  0xC0045004
#define SNDCTL_DSP_SETFMT      0xC0045005
#define SNDCTL_DSP_CHANNELS    0xC0045006
#define SNDCTL_DSP_POST        0x5008
#define SNDCTL_DSP_SUBDIVIDE   0xC0045009
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define SNDCTL_DSP_GETFMTS     0x8004500B
#define SNDCTL_DSP_GETOSPACE   0x8010500C
#define SNDCTL_DSP_NONBLOCK    0x500E
#define SNDCTL_DSP_GETCAPS     0x8004500F
#define SNDCTL_DSP_GETTRIGGER  0x80045010
#define SNDCTL_DSP_SETTRIGGER  0x40045010
#define SNDCTL_DSP_GETOPTR     0x800C5012
#define SNDCTL_DSP_GETODELAY   0x80045017
#define FRAGSIZE 4096
static bool get_int(struct uvm *vm, uint64_t a, int32_t *v) { if (!uvm_check(vm, a, 4, false)) return false; memcpy(v, (void *)(uintptr_t)a, 4); return true; }
static bool put_bytes(struct uvm *vm, uint64_t a, const void *v, int n) { if (!uvm_check(vm, a, n, true)) return false; memcpy((void *)(uintptr_t)a, v, n); return true; }
/* bytes of the program's format that mixer frames stand for */
static int to_bytes(const dsp_t *d, int frames) { return (int)((int64_t)frames * d->rate / audio_rate()) * bpf(d); }

int64_t dsp_ioctl(void *o, uint64_t cmd, uint64_t arg, struct uvm *vm) {
    dsp_t *d = o;
    if (!d) return -EBADF;
    int32_t v = 0;
    switch (cmd) {
    case SNDCTL_DSP_RESET: case SNDCTL_DSP_POST: case SNDCTL_DSP_NONBLOCK: d->npart = 0; return 0;
    case SNDCTL_DSP_SYNC: {                               /* until what was written has played (at most 2 s) */
        uint32_t t0 = timer_ticks();
        while (audio_stream_queued(d->stream) > 16 * 512 && timer_ticks() - t0 < 2000) task_sleep(10);
        return 0;
    }
    case SNDCTL_DSP_SPEED:
        if (!get_int(vm, arg, &v)) return -EFAULT;
        if (v > 0) d->rate = CLAMP(v, 4000, 192000);
        return put_bytes(vm, arg, &d->rate, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_STEREO:
        if (!get_int(vm, arg, &v)) return -EFAULT;
        d->chans = v ? 2 : 1; v = d->chans - 1;
        return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_CHANNELS:
        if (!get_int(vm, arg, &v)) return -EFAULT;
        if (v > 0) d->chans = v >= 2 ? 2 : 1;
        return put_bytes(vm, arg, &d->chans, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_SETFMT:
        if (!get_int(vm, arg, &v)) return -EFAULT;
        if (v == AFMT_U8 || v == AFMT_S16_LE) d->fmt = v; else if (v) d->fmt = AFMT_S16_LE;   /* 0 asks what is set */
        d->npart = 0;
        return put_bytes(vm, arg, &d->fmt, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_GETFMTS: v = AFMT_U8 | AFMT_S16_LE; return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_GETBLKSIZE: v = FRAGSIZE; return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_SETFRAGMENT: case SNDCTL_DSP_SUBDIVIDE: case SNDCTL_DSP_SETTRIGGER: return 0;
    case SNDCTL_DSP_GETTRIGGER: v = 2; return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;   /* output on */
    case SNDCTL_DSP_GETCAPS: v = 1 | 0x1000 | 0x2000; return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;   /* revision 1, TRIGGER, MMAP-less */
    case SNDCTL_DSP_GETOSPACE: {
        int room = to_bytes(d, audio_stream_room(d->stream));
        int32_t info[4] = { room / FRAGSIZE, 16384 * 4 / FRAGSIZE, FRAGSIZE, room };
        return put_bytes(vm, arg, info, 16) ? 0 : -EFAULT;
    }
    case SNDCTL_DSP_GETODELAY: v = to_bytes(d, audio_stream_queued(d->stream)); return put_bytes(vm, arg, &v, 4) ? 0 : -EFAULT;
    case SNDCTL_DSP_GETOPTR: {
        int64_t played = (int64_t)d->written - to_bytes(d, audio_stream_queued(d->stream));
        int32_t info[3] = { (int32_t)(played > 0 ? played : 0), 0, (int32_t)((played > 0 ? played : 0) % (FRAGSIZE * 16)) };
        return put_bytes(vm, arg, info, 12) ? 0 : -EFAULT;
    }
    default: return -EINVAL;
    }
}
#endif
