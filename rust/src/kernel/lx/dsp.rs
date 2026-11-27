//! /dev/dsp (was src/linux/dsp.c): sound for Linux programs through the Open Sound System
//! interface (what GStreamer's osssink and many older programs speak). A program picks a
//! format, channel count and rate with ioctls and writes samples; they are converted to the
//! mixer's 16-bit stereo at audio_rate() and handed to a stream of the OS's mixer
//! (src/kernel/audio.c), so they play alongside the desktop's own sounds. The device exists
//! when the OS has a sound card.

use super::*;
use super::mem::*;
use super::sig::interrupted;
use core::ptr;

extern "C" {
    fn audio_present() -> CBool;
    fn audio_stream_open() -> c_int;
    fn audio_stream_write(id: c_int, pcm: *const i16, frames: c_int) -> c_int;
    fn audio_stream_room(id: c_int) -> c_int;
    fn audio_stream_queued(id: c_int) -> c_int;
    fn audio_stream_close(id: c_int);
    fn audio_rate() -> c_int;
}

const AFMT_U8: c_int = 0x08;
const AFMT_S16_LE: c_int = 0x10;
#[repr(C)]
struct Dsp {
    stream: c_int,
    rate: c_int, chans: c_int, fmt: c_int,           /* what the program writes */
    phase: i64,                                      /* resampler position, in input frames (32.32 fixed point) */
    last: [i16; 2],                                  /* the previous input frame, for interpolation */
    part: [u8; 4], npart: c_int,                     /* the bytes of a frame split across two writes */
    written: u64,                                    /* bytes taken from the program */
    /* with every mixer stream taken (stream < 0) the device still opens, silent: frames queue
     * here and drain at the mixer's rate, so a player keeps real time instead of failing */
    silent_q: i64,
    silent_t: u32,
}
const SILENT_RING: i64 = 16384;
unsafe fn silent_drain(d: &mut Dsp) {
    let now = timer_ticks();
    let ms = now.wrapping_sub(d.silent_t) as i64;
    if ms > 0 { d.silent_q = (d.silent_q - ms * audio_rate() as i64 / 1000).max(0); d.silent_t = now; }
}
unsafe fn st_write(d: &mut Dsp, pcm: *const i16, frames: c_int) -> c_int {
    if d.stream >= 0 { return audio_stream_write(d.stream, pcm, frames); }
    silent_drain(d);
    let n = (frames as i64).min(SILENT_RING - d.silent_q).max(0);
    d.silent_q += n;
    n as c_int
}
unsafe fn st_room(d: &mut Dsp) -> c_int {
    if d.stream >= 0 { return audio_stream_room(d.stream); }
    silent_drain(d);
    (SILENT_RING - d.silent_q) as c_int
}
unsafe fn st_queued(d: &mut Dsp) -> c_int {
    if d.stream >= 0 { return audio_stream_queued(d.stream); }
    silent_drain(d);
    d.silent_q as c_int
}
fn bpf(d: &Dsp) -> c_int { (if d.fmt == AFMT_U8 { 1 } else { 2 }) * d.chans }

pub unsafe fn dsp_open(err: &mut i64) -> *mut c_void {
    if audio_present() == 0 { *err = -ENODEV; return ptr::null_mut(); }
    let d = znew::<Dsp>();
    if d.is_null() { *err = -ENOMEM; return ptr::null_mut(); }
    (*d).stream = audio_stream_open();
    if (*d).stream < 0 {
        /* every stream is in use (a web page can hold several paused players): open silent
         * rather than fail, which would stop the video that wanted it */
        klog(b"dsp: every mixer stream is busy, this player is silent\0".as_ptr() as *const c_char);
        (*d).silent_t = timer_ticks();
    }
    (*d).rate = 8000; (*d).chans = 1; (*d).fmt = AFMT_U8;   /* OSS's defaults */
    d as *mut c_void
}
pub unsafe fn dsp_release(o: *mut c_void) { let d = o as *mut Dsp; if d.is_null() { return; } if (*d).stream >= 0 { audio_stream_close((*d).stream); } zfree(d); }
pub unsafe fn dsp_writable(o: *mut c_void) -> bool { let d = o as *mut Dsp; !d.is_null() && st_room(&mut *d) >= 512 }

/// One input frame as 16-bit stereo.
unsafe fn frame_at(d: &Dsp, src: *const u8, i: c_int) -> [i16; 2] {
    if d.fmt == AFMT_U8 {
        let p = src.add((i * d.chans) as usize);
        let l = (((*p as c_int) - 128) << 8) as i16;
        [l, if d.chans > 1 { (((*p.add(1) as c_int) - 128) << 8) as i16 } else { l }]
    } else {
        let p = src.add((i * 2 * d.chans) as usize) as *const i16;
        let l = ptr::read_unaligned(p);
        [l, if d.chans > 1 { ptr::read_unaligned(p.add(1)) } else { l }]
    }
}
/// Convert whole input frames to the mixer's rate and queue them, waiting for room.
unsafe fn put_frames(d: *mut Dsp, src: *const u8, n: c_int, nonblock: bool, stopped: &mut bool) -> i64 {
    let d = &mut *d;
    let out_rate = audio_rate();
    let step = ((d.rate as i64) << 32) / out_rate as i64;
    let mut buf = [0i16; 512 * 2];
    let mut nb = 0usize;
    let limit = (n as i64) << 32;
    while d.phase < limit {
        let i = (d.phase >> 32) as c_int;
        let f = (d.phase as u32 >> 16) as i32;           /* 16-bit fraction */
        let a = if i == 0 { d.last } else { frame_at(d, src, i - 1) };
        let b = frame_at(d, src, i);
        buf[nb * 2] = (a[0] as i32 + (((b[0] as i32 - a[0] as i32) * f) >> 16)) as i16;
        buf[nb * 2 + 1] = (a[1] as i32 + (((b[1] as i32 - a[1] as i32) * f) >> 16)) as i16;
        d.phase += step;
        nb += 1;
        if nb == 512 || d.phase >= limit {
            let mut at = 0usize;
            while at < nb {
                let k = st_write(d, buf.as_ptr().add(at * 2), (nb - at) as c_int);
                if k < 0 { *stopped = true; return -EIO; }
                at += k as usize;
                if at < nb {
                    if nonblock { *stopped = true; return 0; }   /* the rest of this frame batch is dropped */
                    if interrupted() { *stopped = true; return -EINTR; }
                    task_sleep(5);
                }
            }
            nb = 0;
        }
    }
    d.phase -= limit;
    if n != 0 { d.last = frame_at(d, src, n - 1); }
    0
}
pub unsafe fn dsp_write(o: *mut c_void, src: *const u8, n: u64, nonblock: bool) -> i64 {
    let d = o as *mut Dsp;
    if d.is_null() { return -EBADF; }
    if nonblock && st_room(&mut *d) < 512 { return -EAGAIN; }
    let fb = bpf(&*d);
    let mut done = 0u64;
    if (*d).npart != 0 {                                 /* finish a frame begun in the last write */
        while (*d).npart < fb && done < n { (*d).part[(*d).npart as usize] = *src.add(done as usize); (*d).npart += 1; done += 1; }
        if (*d).npart < fb { (*d).written += done; return done as i64; }
        let mut stop = false;
        let part = (*d).part;
        put_frames(d, part.as_ptr(), 1, nonblock, &mut stop);
        (*d).npart = 0;
    }
    while done + fb as u64 <= n {
        let frames = ((n - done) / fb as u64).min(4096) as c_int;
        let mut stop = false;
        let r = put_frames(d, src.add(done as usize), frames, nonblock, &mut stop);
        if r < 0 && done == 0 { return r; }
        done += frames as u64 * fb as u64;
        if stop { break; }
    }
    while done < n && n - done < fb as u64 && (*d).npart < fb { (*d).part[(*d).npart as usize] = *src.add(done as usize); (*d).npart += 1; done += 1; }
    (*d).written += done;
    done as i64
}

/* the OSS ioctls a player uses */
const SNDCTL_DSP_RESET: u64 = 0x5000;
const SNDCTL_DSP_SYNC: u64 = 0x5001;
const SNDCTL_DSP_SPEED: u64 = 0xC004_5002;
const SNDCTL_DSP_STEREO: u64 = 0xC004_5003;
const SNDCTL_DSP_GETBLKSIZE: u64 = 0xC004_5004;
const SNDCTL_DSP_SETFMT: u64 = 0xC004_5005;
const SNDCTL_DSP_CHANNELS: u64 = 0xC004_5006;
const SNDCTL_DSP_POST: u64 = 0x5008;
const SNDCTL_DSP_SUBDIVIDE: u64 = 0xC004_5009;
const SNDCTL_DSP_SETFRAGMENT: u64 = 0xC004_500A;
const SNDCTL_DSP_GETFMTS: u64 = 0x8004_500B;
const SNDCTL_DSP_GETOSPACE: u64 = 0x8010_500C;
const SNDCTL_DSP_NONBLOCK: u64 = 0x500E;
const SNDCTL_DSP_GETCAPS: u64 = 0x8004_500F;
const SNDCTL_DSP_GETTRIGGER: u64 = 0x8004_5010;
const SNDCTL_DSP_SETTRIGGER: u64 = 0x4004_5010;
const SNDCTL_DSP_GETOPTR: u64 = 0x800C_5012;
const SNDCTL_DSP_GETODELAY: u64 = 0x8004_5017;
const FRAGSIZE: i32 = 4096;
/// Bytes of the program's format that mixer frames stand for.
unsafe fn to_bytes(d: &Dsp, frames: c_int) -> c_int { ((frames as i64 * d.rate as i64 / audio_rate() as i64) as c_int).wrapping_mul(bpf(d)) }

pub unsafe fn dsp_ioctl(o: *mut c_void, cmd: u64, arg: u64, vm: *mut Uvm) -> i64 {
    let dp = o as *mut Dsp;
    if dp.is_null() { return -EBADF; }
    let d = &mut *dp;
    let put = |v: &[i32]| if uout(vm, arg, v.as_ptr(), v.len() as u64 * 4) { 0 } else { -EFAULT };
    let get = || uget::<i32>(vm, arg);
    match cmd {
        SNDCTL_DSP_RESET | SNDCTL_DSP_POST | SNDCTL_DSP_NONBLOCK => { d.npart = 0; 0 }
        SNDCTL_DSP_SYNC => {                             /* until what was written has played (at most 2 s) */
            let t0 = timer_ticks();
            while st_queued(d) > 16 * 512 && timer_ticks().wrapping_sub(t0) < 2000 { task_sleep(10); }
            0
        }
        SNDCTL_DSP_SPEED => {
            let Some(v) = get() else { return -EFAULT };
            if v > 0 { d.rate = v.clamp(4000, 192000); }
            put(&[d.rate])
        }
        SNDCTL_DSP_STEREO => {
            let Some(v) = get() else { return -EFAULT };
            d.chans = if v != 0 { 2 } else { 1 };
            put(&[d.chans - 1])
        }
        SNDCTL_DSP_CHANNELS => {
            let Some(v) = get() else { return -EFAULT };
            if v > 0 { d.chans = if v >= 2 { 2 } else { 1 }; }
            put(&[d.chans])
        }
        SNDCTL_DSP_SETFMT => {
            let Some(v) = get() else { return -EFAULT };
            if v == AFMT_U8 || v == AFMT_S16_LE { d.fmt = v; } else if v != 0 { d.fmt = AFMT_S16_LE; }   /* 0 asks what is set */
            d.npart = 0;
            put(&[d.fmt])
        }
        SNDCTL_DSP_GETFMTS => put(&[AFMT_U8 | AFMT_S16_LE]),
        SNDCTL_DSP_GETBLKSIZE => put(&[FRAGSIZE]),
        SNDCTL_DSP_SETFRAGMENT | SNDCTL_DSP_SUBDIVIDE | SNDCTL_DSP_SETTRIGGER => 0,
        SNDCTL_DSP_GETTRIGGER => put(&[2]),              /* output on */
        SNDCTL_DSP_GETCAPS => put(&[1 | 0x1000 | 0x2000]),
        SNDCTL_DSP_GETOSPACE => {
            let r = st_room(d);
            let room = to_bytes(d, r);
            put(&[room / FRAGSIZE, 16384 * 4 / FRAGSIZE, FRAGSIZE, room])
        }
        SNDCTL_DSP_GETODELAY => { let q = st_queued(d); put(&[to_bytes(d, q)]) }
        SNDCTL_DSP_GETOPTR => {
            let q = st_queued(d);
            let played = d.written as i64 - to_bytes(d, q) as i64;
            let pl = if played > 0 { played } else { 0 };
            put(&[pl as i32, 0, (pl % (FRAGSIZE as i64 * 16)) as i32])
        }
        _ => -EINVAL,
    }
}
