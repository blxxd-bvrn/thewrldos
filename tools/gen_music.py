#!/usr/bin/env python3
"""Synthesise the sample tracks in My Music (original compositions, made from
oscillators and noise here) and encode them as MP3 with lame.

    python3 tools/gen_music.py
"""
import os, subprocess, tempfile, wave
import numpy as np

SR = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "rootfs/Documents and Settings/User/My Documents/My Music")
rng = np.random.default_rng(7)


def hz(n):                                   # MIDI note -> Hz
    return 440.0 * 2 ** ((n - 69) / 12)


def env(n, a, d, s, r):                      # ADSR over n samples (seconds for a, d, r)
    a, d, r = int(a * SR), int(d * SR), int(r * SR)
    e = np.full(n, s, dtype=np.float64)
    a = min(a, n); e[:a] = np.linspace(0, 1, a, endpoint=False)
    d2 = min(d, max(0, n - a)); e[a:a + d2] = np.linspace(1, s, d2, endpoint=False)
    r = min(r, n); e[n - r:] *= np.linspace(1, 0, r)
    return e


def osc(kind, f, n, phase=0.0, detune=0.0):
    t = np.arange(n) / SR
    ph = (f * (1 + detune) * t + phase) % 1.0
    if kind == "sine": return np.sin(2 * np.pi * ph)
    if kind == "saw": return 2 * ph - 1
    if kind == "square": return np.where(ph < 0.5, 1.0, -1.0)
    if kind == "tri": return 4 * np.abs(ph - 0.5) - 1
    raise ValueError(kind)


def lowpass(x, cutoff):                       # one-pole, run twice
    a = np.exp(-2 * np.pi * cutoff / SR)
    for _ in range(2):
        y = np.empty_like(x); acc = 0.0
        for i in range(len(x)):
            acc = (1 - a) * x[i] + a * acc; y[i] = acc
        x = y
    return x


def lowpass_fast(x, cutoff):                  # FFT brick-ish low-pass with a soft edge
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / SR)
    X *= 1 / (1 + (f / cutoff) ** 4)
    return np.fft.irfft(X, len(x))


def add(buf, at, sig, pan=0.0, gain=1.0):
    i = int(at * SR); j = min(len(buf), i + len(sig))
    if j <= i: return
    l, r = np.cos((pan + 1) * np.pi / 4), np.sin((pan + 1) * np.pi / 4)
    buf[i:j, 0] += sig[:j - i] * gain * l * 1.414
    buf[i:j, 1] += sig[:j - i] * gain * r * 1.414


def reverb(buf, mix=0.25):
    out = buf.copy()
    for k, (dl, g) in enumerate([(0.031, .5), (0.047, .45), (0.071, .4), (0.113, .33), (0.167, .27), (0.229, .2), (0.311, .14)]):
        d = int(dl * SR)
        ch = k % 2
        out[d:, ch] += buf[:-d, 1 - ch] * g * mix
        out[d:, 1 - ch] += buf[:-d, ch] * g * mix * 0.6
    return out


def kick(n=0.35):
    m = int(n * SR); t = np.arange(m) / SR
    f = 50 + 110 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 9)


def snare(n=0.25):
    m = int(n * SR); t = np.arange(m) / SR
    noise = lowpass_fast(rng.standard_normal(m), 6000) * np.exp(-t * 18)
    return 0.6 * noise + 0.4 * np.sin(2 * np.pi * 190 * t) * np.exp(-t * 25)


def hat(n=0.06):
    m = int(n * SR); t = np.arange(m) / SR
    x = rng.standard_normal(m); x = x - lowpass_fast(x, 7000)
    return x * np.exp(-t * 60)


def master(buf, gain=0.8):
    buf = buf / max(1e-9, np.max(np.abs(buf))) * gain
    fade = int(2.5 * SR); buf[-fade:] *= np.linspace(1, 0, fade)[:, None]
    buf[:int(0.02 * SR)] *= np.linspace(0, 1, int(0.02 * SR))[:, None]
    return np.tanh(buf * 1.1) / np.tanh(1.1)


# ---------------- 1. Blue Horizon: slow pads and a bell melody ----------------
def blue_horizon():
    bpm = 72; beat = 60 / bpm; bars = 14
    buf = np.zeros((int((bars * 4 + 4) * beat * SR), 2))
    chords = [[50, 57, 62, 66, 69], [47, 54, 59, 62, 66], [43, 50, 55, 59, 62], [45, 52, 57, 61, 64]]   # D, Bm, G, A
    for b in range(bars):
        ch = chords[b % 4]; n = int(4 * beat * SR * 1.15)
        for k, note in enumerate(ch):
            s = sum(osc("saw", hz(note), n, phase=rng.random(), detune=d) for d in (-0.004, 0, 0.005)) / 3
            s = lowpass_fast(s, 900 + 300 * np.sin(b)) * env(n, 1.2, 0.5, 0.8, 1.5)
            add(buf, b * 4 * beat, s, pan=(k - 2) * 0.3, gain=0.10)
        bass = osc("sine", hz(ch[0] - 12), int(4 * beat * SR)) * env(int(4 * beat * SR), 0.05, 0.5, 0.7, 0.4)
        add(buf, b * 4 * beat, bass, gain=0.22)
    melody = [74, 76, 78, 81, 78, 76, 74, None, 71, 73, 74, 78, 76, None, 73, 69,
              74, 76, 78, 81, 83, 81, 78, None, 79, 78, 76, 74, 76, None, 73, 74]
    for k, note in enumerate(melody * 2):
        if note is None: continue
        at = (8 + k) * beat
        if at > (bars * 4 - 2) * beat: break
        n = int(2.5 * SR); t = np.arange(n) / SR
        bell = (np.sin(2 * np.pi * hz(note) * t) + 0.35 * np.sin(2 * np.pi * hz(note) * 2.76 * t) * np.exp(-t * 4)
                + 0.15 * np.sin(2 * np.pi * hz(note) * 5.4 * t) * np.exp(-t * 8)) * np.exp(-t * 2.2)
        add(buf, at, bell, pan=0.25 * np.sin(k), gain=0.14)
    return master(reverb(buf, 0.45))


# ---------------- 2. Neon Skyline: a driving synth-pop groove ----------------
def neon_skyline():
    bpm = 112; beat = 60 / bpm; bars = 20
    buf = np.zeros((int((bars * 4 + 3) * beat * SR), 2))
    prog = [45, 41, 36, 43]                                     # Am F C G roots
    tri = {45: [57, 60, 64], 41: [53, 57, 60], 36: [55, 60, 64], 43: [55, 59, 62]}
    for b in range(bars):
        root = prog[b % 4]
        for e in range(8):                                      # bass eighths
            n = int(beat / 2 * SR * 0.9)
            s = lowpass_fast(osc("saw", hz(root - 12 + (12 if e % 4 == 3 else 0)), n), 700) * env(n, 0.005, 0.1, 0.6, 0.05)
            add(buf, (b * 4 + e / 2) * beat, s, gain=0.30)
        if b >= 2:                                              # arpeggio sixteenths
            notes = tri[root] + [tri[root][0] + 12]
            for e in range(16):
                n = int(beat / 4 * SR * 0.8)
                s = osc("square", hz(notes[(e * 3) % 4] + 12), n) * env(n, 0.002, 0.08, 0.3, 0.04)
                add(buf, (b * 4 + e / 4) * beat, lowpass_fast(s, 3500), pan=0.4 if e % 2 else -0.4, gain=0.06)
        if b >= 4:                                              # pad
            n = int(4 * beat * SR)
            for k, note in enumerate(tri[root]):
                s = sum(osc("saw", hz(note), n, phase=rng.random(), detune=d) for d in (-0.006, 0.006)) / 2
                add(buf, b * 4 * beat, lowpass_fast(s, 1600) * env(n, 0.3, 0.3, 0.7, 0.4), pan=(k - 1) * 0.5, gain=0.06)
        for q in range(4):                                      # drums
            at = (b * 4 + q) * beat
            add(buf, at, kick(), gain=0.55)
            if q % 2 == 1 and b >= 1: add(buf, at, snare(), gain=0.32)
            if b >= 1:
                add(buf, at + beat / 2, hat(), pan=0.3, gain=0.12)
                add(buf, at, hat(), pan=0.3, gain=0.06)
    lead = [69, None, 72, 71, 69, None, 67, 64, 65, None, 69, 67, 64, None, 60, 62,
            64, None, 67, 69, 72, None, 71, 69, 67, None, 64, 67, 69, None, None, None]
    for rep in range(2):
        for k, note in enumerate(lead):
            if note is None: continue
            at = ((8 + rep * 8) * 4 + k * 0.5 * 2) * beat
            n = int(beat * 0.95 * SR)
            s = sum(osc("saw", hz(note + 12), n, detune=d) for d in (-0.003, 0.004)) / 2
            vib = 1 + 0.002 * np.sin(2 * np.pi * 5.5 * np.arange(n) / SR)
            add(buf, at, lowpass_fast(s * vib, 2800) * env(n, 0.01, 0.15, 0.6, 0.1), gain=0.12)
    return master(reverb(buf, 0.22))


# ---------------- 3. Morning Glow: a gentle plucked-keys waltz ----------------
def morning_glow():
    bpm = 96; beat = 60 / bpm; bars = 22
    buf = np.zeros((int((bars * 3 + 4) * beat * SR), 2))
    prog = [[48, 64, 67, 72], [45, 60, 64, 69], [41, 60, 65, 69], [43, 59, 62, 67]]   # C Am F G
    def key(f, n):
        t = np.arange(n) / SR
        x = sum(np.sin(2 * np.pi * f * h * t) * np.exp(-t * (1.8 + h * 1.6)) / h for h in (1, 2, 3, 4))
        return x * env(n, 0.004, 0.2, 1.0, 0.2)
    for b in range(bars):
        ch = prog[b % 4]
        add(buf, b * 3 * beat, key(hz(ch[0] - 12), int(3 * beat * SR)), gain=0.30)
        for q in range(3):
            for k, note in enumerate(ch[1:]):
                add(buf, (b * 3 + q + k * 0.04) * beat, key(hz(note), int(1.5 * beat * SR)), pan=(k - 1) * 0.4, gain=0.07 if q else 0.10)
    tune = [76, 74, 72, 74, 76, 79, 77, 76, 74, 72, 71, 72, 74, None, None, 79, 81, 79, 77, 76, 74, 76, 72, None]
    for rep in range(2):
        for k, note in enumerate(tune):
            if note is None: continue
            at = ((4 + rep * 8) * 3 + k * 1.0) * beat
            add(buf, at, key(hz(note), int(2 * beat * SR)), pan=0.1, gain=0.20)
    return master(reverb(buf, 0.38))


TRACKS = [
    ("Blue Horizon", blue_horizon, 1),
    ("Neon Skyline", neon_skyline, 2),
    ("Morning Glow", morning_glow, 3),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    for title, fn, no in TRACKS:
        pcm = (fn() * 32000).astype("<i2")
        with tempfile.TemporaryDirectory() as td:
            wpath = os.path.join(td, "t.wav")
            with wave.open(wpath, "wb") as w:
                w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
            mp3 = os.path.join(OUT, f"{title}.mp3")
            subprocess.run(["lame", "--quiet", "-b", "128", "--id3v2-only", "--tt", title, "--ta", "THE WRLD OS",
                            "--tl", "WRLD Sounds", "--ty", "2026", "--tn", f"{no}/{len(TRACKS)}", "--tg", "Electronic",
                            wpath, mp3], check=True)
        print(f"{mp3}: {len(pcm) / SR:.1f} s, {os.path.getsize(mp3) // 1024} KB")


if __name__ == "__main__":
    main()
