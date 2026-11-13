/* THE WRLD Media Player: plays MP3 and WAV files, in the spirit of the
 * all-in-one players of the XP era. A dark glossy frame holds the feature
 * tabs (Now Playing, Library) at the top, the Now Playing list on the right
 * and the transport bar at the bottom; Now Playing shows the track's info
 * and a visualisation drawn from the samples being heard.
 *
 * Files are decoded on their own task (MP3 through minimp3) so the desktop
 * keeps running; the mixer in audio.c plays the result on one voice. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "audio.h"
#include "raster.h"
#include "mp3glue.h"

#define MAXT 256
#define MUSIC_DIR USER_DOCS "\\My Music"

typedef struct {
    char path[200], title[80], artist[64], album[64];
    uint32_t ms; int kbps; bool mp3;
} track_t;

/* A decode on its own task. MP3s are decoded straight into a sound of the
 * full length, which starts playing once half a second is ready while the
 * rest is decoded ahead of the speaker. Whoever finishes last frees the job:
 * the player once the task is done (state 2 or 3), else the task once the
 * player has let go (`abandoned`); the task frees the sound only if the
 * player never took it. Tasks are cooperative, so the task checks
 * `abandoned` after every yield before it touches the sound again. */
typedef struct {
    volatile int state;                 /* 0 opening, 1 playable and decoding, 2 finished, 3 failed */
    volatile bool abandoned, taken;
    volatile uint32_t ready;            /* frames decoded so far */
    char path[200];
    sound_t *snd;
    char err[96];
} job_t;

enum { ST_STOP, ST_LOAD, ST_PLAY, ST_PAUSE };
enum { TAB_NOW, TAB_LIB };
enum { VIZ_BARS, VIZ_SCOPE, VIZ_AURORA, VIZ_COUNT };
static const char *viz_names[VIZ_COUNT] = { "Bars and Waves: Spectrum", "Bars and Waves: Scope", "Ambience: Aurora" };
#define NBANDS 28

typedef struct {
    int tab;
    track_t *lib; int nlib; bool scanned;
    track_t *list; int nlist, cur;
    int state; sound_t *snd; int voice; job_t *job;
    bool shuffle, repeat;
    int viz;
    int band[NBANDS], peak[NBANDS];     /* 0..1000 */
    bool seek_drag; int drag_x;
    int lib_sel, lib_scroll, list_sel, list_scroll;
    char search[48];
    uint32_t anim;
    char status[120];
    int last_click_row, last_click_where; uint32_t last_click_ms;
} mp_t;

static window_t *mp_win;

/* ---------------- colours (the player's own skin, the same in every theme) ---------------- */
#define C_BG       0xFF0B1426
#define C_PANEL    0xFF111D35
#define C_PANEL2   0xFF16264A
#define C_LINE     0xFF243A66
#define C_ACCENT   0xFF3CC8FF
#define C_ACCENT2  0xFF1E78E6
#define C_TEXT     0xFFE8F0FA
#define C_DIM      0xFF8FA4C4
#define C_ROWSEL   0xFF1D3F78

/* ---------------- tags and lengths ---------------- */
static void tag_text(const uint8_t *p, uint32_t n, char *out, int cap) {
    if (!n) return;
    int enc = p[0], k = 0; p++; n--;
    if (enc == 1 || enc == 2) {                                       /* UTF-16 */
        bool be = enc == 2;
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
        for (uint32_t i = 0; i + 1 < n && k < cap - 1; i += 2) {
            unsigned ch = be ? (p[i] << 8 | p[i + 1]) : (p[i] | p[i + 1] << 8);
            if (!ch) break;
            out[k++] = ch < 128 ? (char)ch : '?';
        }
    } else {
        for (uint32_t i = 0; i < n && k < cap - 1; i++) {
            if (!p[i]) break;
            if (enc == 3 && p[i] >= 0x80) { if ((p[i] & 0xC0) == 0xC0) out[k++] = '?'; continue; }   /* UTF-8 */
            out[k++] = p[i] < 128 ? (char)p[i] : '?';
        }
    }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
}
static void read_id3(track_t *t, const uint8_t *d, uint32_t n) {
    if (n < 10 || memcmp(d, "ID3", 3)) {
        if (n >= 128 && !memcmp(d + n - 128, "TAG", 3)) {                /* ID3v1 */
            const uint8_t *v = d + n - 128;
            char b[32];
            memcpy(b, v + 3, 30); b[30] = 0; for (int k = 29; k >= 0 && (b[k] == ' ' || !b[k]); k--) b[k] = 0; if (b[0]) strlcpy(t->title, b, sizeof t->title);
            memcpy(b, v + 33, 30); b[30] = 0; for (int k = 29; k >= 0 && (b[k] == ' ' || !b[k]); k--) b[k] = 0; if (b[0]) strlcpy(t->artist, b, sizeof t->artist);
            memcpy(b, v + 63, 30); b[30] = 0; for (int k = 29; k >= 0 && (b[k] == ' ' || !b[k]); k--) b[k] = 0; if (b[0]) strlcpy(t->album, b, sizeof t->album);
        }
        return;
    }
    int ver = d[3];
    uint32_t size = ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
    uint32_t o = 10, end = MIN(n, 10 + size);
    if (d[5] & 0x40) {                                                 /* extended header */
        uint32_t ex = ver >= 4 ? (((d[10] & 0x7F) << 21) | ((d[11] & 0x7F) << 14) | ((d[12] & 0x7F) << 7) | (d[13] & 0x7F))
                               : (uint32_t)((d[10] << 24) | (d[11] << 16) | (d[12] << 8) | d[13]) + 4;
        o += ex;
    }
    if (ver < 3) {                                                     /* ID3v2.2: three-letter frames */
        while (o + 6 <= end) {
            uint32_t fs = (d[o + 3] << 16) | (d[o + 4] << 8) | d[o + 5];
            if (!d[o] || o + 6 + fs > end) break;
            const uint8_t *f = d + o + 6;
            if (!memcmp(d + o, "TT2", 3)) tag_text(f, fs, t->title, sizeof t->title);
            else if (!memcmp(d + o, "TP1", 3)) tag_text(f, fs, t->artist, sizeof t->artist);
            else if (!memcmp(d + o, "TAL", 3)) tag_text(f, fs, t->album, sizeof t->album);
            o += 6 + fs;
        }
        return;
    }
    while (o + 10 <= end) {
        uint32_t fs = ver >= 4 ? (((d[o + 4] & 0x7F) << 21) | ((d[o + 5] & 0x7F) << 14) | ((d[o + 6] & 0x7F) << 7) | (d[o + 7] & 0x7F))
                               : (uint32_t)((d[o + 4] << 24) | (d[o + 5] << 16) | (d[o + 6] << 8) | d[o + 7]);
        if (!d[o] || o + 10 + fs > end) break;
        const uint8_t *f = d + o + 10;
        if (!memcmp(d + o, "TIT2", 4)) tag_text(f, fs, t->title, sizeof t->title);
        else if (!memcmp(d + o, "TPE1", 4)) tag_text(f, fs, t->artist, sizeof t->artist);
        else if (!memcmp(d + o, "TALB", 4)) tag_text(f, fs, t->album, sizeof t->album);
        o += 10 + fs;
    }
}
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static bool is_media(const char *name) {
    const char *e = vfs_ext(name);
    return !strcasecmp(e, "mp3") || !strcasecmp(e, "wav");
}
static bool track_from(track_t *t, vnode_t *n) {
    memset(t, 0, sizeof *t);
    vfs_path(n, t->path, sizeof t->path);
    strlcpy(t->title, n->name, sizeof t->title);
    char *dot = strrchr(t->title, '.'); if (dot) *dot = 0;
    t->mp3 = !strcasecmp(vfs_ext(n->name), "mp3");
    if (!n->data || !n->size) return false;
    if (t->mp3) {
        read_id3(t, n->data, n->size);
        int rate;
        if (mp3_probe(n->data, n->size, &t->ms, &rate, &t->kbps)) return false;
    } else {
        const uint8_t *d = n->data;
        if (n->size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
        uint32_t brate = 0, dlen = 0;
        for (uint32_t o = 12; o + 8 <= n->size;) {
            uint32_t cl = rd32(d + o + 4);
            if (!memcmp(d + o, "fmt ", 4) && cl >= 16) brate = rd32(d + o + 16);
            else if (!memcmp(d + o, "data", 4)) dlen = MIN(cl, n->size - o - 8);
            o += 8 + cl + (cl & 1);
        }
        if (!brate) return false;
        t->ms = (uint32_t)((uint64_t)dlen * 1000 / brate);
        t->kbps = (int)(brate * 8 / 1000);
    }
    if (!t->artist[0]) strlcpy(t->artist, "Unknown Artist", sizeof t->artist);
    if (!t->album[0]) strlcpy(t->album, "Unknown Album", sizeof t->album);
    return true;
}

/* ---------------- library ---------------- */
static void scan_dir(mp_t *m, vnode_t *d, int depth) {
    if (depth > 8 || m->nlib >= MAXT) return;
    for (vnode_t *c = d->child; c && m->nlib < MAXT; c = c->next) {
        if (c->dir) {
            if (depth == 0 && (!strcasecmp(c->name, "Linux") || !strcasecmp(c->name, "boot"))) continue;
            scan_dir(m, c, depth + 1);
        } else if (is_media(c->name) && track_from(&m->lib[m->nlib], c)) m->nlib++;
    }
}
static void library_scan(mp_t *m) {
    m->nlib = 0;
    for (char l = 'C'; l <= 'Z'; l++) { vnode_t *r = vfs_root(l); if (r) scan_dir(m, r, 0); }
    /* My Music first, then by artist and title */
    for (int i = 1; i < m->nlib; i++) {
        track_t t = m->lib[i]; int j = i - 1;
        bool tm = !strncasecmp(t.path, MUSIC_DIR, strlen(MUSIC_DIR));
        while (j >= 0) {
            bool jm = !strncasecmp(m->lib[j].path, MUSIC_DIR, strlen(MUSIC_DIR));
            int c = jm != tm ? (jm ? -1 : 1) : strcasecmp(m->lib[j].artist, t.artist);
            if (!c) c = strcasecmp(m->lib[j].title, t.title);
            if (c <= 0) break;
            m->lib[j + 1] = m->lib[j]; j--;
        }
        m->lib[j + 1] = t;
    }
    m->scanned = true;
}

/* ---------------- playback ---------------- */
static void job_quit(job_t *j, mp3s_t *d) {             /* the player let go mid-decode */
    if (d) mp3s_close(d);
    if (!j->taken) sound_free(j->snd);
    kfree(j);
}
static void decode_task(void *arg) {
    job_t *j = arg;
    if (j->abandoned) { job_quit(j, NULL); return; }
    vnode_t *n = vfs_resolve(j->path, NULL);
    if (!n || n->dir || !n->data) { snprintf(j->err, sizeof j->err, "the file could not be found"); j->state = 3; return; }
    if (strcasecmp(vfs_ext(n->name), "mp3")) {
        j->snd = sound_load_wav(n->data, n->size, j->err, sizeof j->err);
        if (j->snd) j->ready = j->snd->frames;
        j->state = j->snd ? 2 : 3;
        return;
    }
    uint32_t ms; int rate0, kbps;
    mp3s_t *d = mp3_probe(n->data, n->size, &ms, &rate0, &kbps) ? NULL : mp3s_open(n->data, n->size);
    if (!d) { snprintf(j->err, sizeof j->err, "not an MP3 file"); j->state = 3; return; }
    uint32_t R = (uint32_t)audio_rate(), total = (uint32_t)((uint64_t)ms * R / 1000) + R / 20;
    sound_t *s = kzalloc(sizeof *s);
    s->pcm = kmalloc((size_t)total * 4 + 8);
    if (!s->pcm) { kfree(s); mp3s_close(d); snprintf(j->err, sizeof j->err, "not enough memory"); j->state = 3; return; }
    memset(s->pcm, 0, (size_t)total * 4 + 8);
    s->frames = total; j->snd = s;
    /* resample frame by frame: `pos` is 16.16 in input frames, `base` the input
     * index of in[0], `last` the input frame before it */
    int16_t in[1152 * 2];                                  /* one MP3 frame */
    int16_t last[2] = { 0, 0 };
    uint64_t pos = 0; uint32_t base = 0, out = 0;
    int ch, rate, count = 0;
    int nf;
    while ((nf = mp3s_read(d, in, &ch, &rate)) > 0) {
        uint64_t step = ((uint64_t)rate << 16) / R;
        #define IN(k, c) ((k) < base ? last[c] : in[((k) - base) * ch + (ch == 1 ? 0 : (c))])
        while (out < total && (uint32_t)(pos >> 16) + 1 < base + (uint32_t)nf) {
            uint32_t f = (uint32_t)(pos >> 16), fr = (uint32_t)(pos & 0xFFFF);
            for (int c = 0; c < 2; c++) { int a = IN(f, c), b = IN(f + 1, c); s->pcm[out * 2 + c] = a + (int)(((int64_t)(b - a) * fr) >> 16); }
            out++; pos += step;
        }
        #undef IN
        last[0] = in[(nf - 1) * ch]; last[1] = in[(nf - 1) * ch + (ch == 1 ? 0 : 1)];
        base += nf;
        j->ready = out;
        if (j->state == 0 && out >= R / 2) j->state = 1;
        if (++count % 3 == 0) {
            task_yield();
            if (j->abandoned) { job_quit(j, d); return; }
        }
    }
    mp3s_close(d);
    j->ready = s->frames;
    j->state = 2;
}
static void drop_job(mp_t *m) {
    job_t *j = m->job;
    if (!j) return;
    m->job = NULL;
    if (j->state >= 2) { if (!j->taken) sound_free(j->snd); kfree(j); }   /* the task is done */
    else j->abandoned = true;                                             /* the task frees it */
}
static void unload(mp_t *m) {
    drop_job(m);
    if (m->snd) { audio_voice_stop(m->voice, m->snd); sound_free(m->snd); m->snd = NULL; }
    m->voice = -1;
}
static void play_index(mp_t *m, int i) {
    if (i < 0 || i >= m->nlist) return;
    unload(m);
    m->cur = i; m->list_sel = i;
    if (!audio_present()) { m->state = ST_STOP; snprintf(m->status, sizeof m->status, "There is no sound device to play on."); return; }
    job_t *j = kzalloc(sizeof *j);
    strlcpy(j->path, m->list[i].path, sizeof j->path);
    m->job = j; m->state = ST_LOAD;
    snprintf(m->status, sizeof m->status, "Opening %s...", m->list[i].title);
    task_create("wrldmp decode", decode_task, j, 64 * 1024);
}
static int next_index(mp_t *m, int dir) {
    if (!m->nlist) return -1;
    if (m->shuffle && m->nlist > 1) {
        int r; do r = (int)((timer_ticks() * 2654435761u + m->anim * 40503u) >> 7) % m->nlist; while (r == m->cur);
        return r;
    }
    int n = m->cur + dir;
    if (n >= m->nlist) return m->repeat ? 0 : -1;
    if (n < 0) return m->repeat ? m->nlist - 1 : 0;
    return n;
}
static uint32_t pos_frames(mp_t *m) {
    if (!m->snd) return 0;
    if (m->state == ST_STOP) return 0;
    return audio_voice_position(m->voice, m->snd);
}
static void toggle_play(mp_t *m) {
    if (m->state == ST_PLAY) { audio_voice_pause(m->voice, m->snd, true); m->state = ST_PAUSE; }
    else if (m->state == ST_PAUSE) { audio_voice_pause(m->voice, m->snd, false); m->state = ST_PLAY; }
    else if (m->state == ST_STOP) {
        if (m->snd) { m->voice = audio_play(m->snd); m->state = m->voice >= 0 ? ST_PLAY : ST_STOP; }
        else if (m->nlist) play_index(m, m->cur < m->nlist ? m->cur : 0);
    }
}
static void stop(mp_t *m) {
    if (m->state == ST_LOAD) { drop_job(m); m->state = ST_STOP; return; }
    if (m->snd) audio_voice_stop(m->voice, m->snd);
    m->state = ST_STOP;
}
static void seek_ms(mp_t *m, int64_t ms) {
    if (!m->snd || (m->state != ST_PLAY && m->state != ST_PAUSE)) return;
    if (ms < 0) ms = 0;
    uint64_t f = (uint64_t)ms * audio_rate() / 1000;
    if (f >= m->snd->frames) f = m->snd->frames - 1;
    if (m->job && m->job->snd == m->snd && m->job->state < 2 && f > m->job->ready) f = m->job->ready;   /* not decoded yet */
    audio_voice_seek(m->voice, m->snd, (uint32_t)f);
}
static void add_track(mp_t *m, const track_t *t) { if (m->nlist < MAXT) m->list[m->nlist++] = *t; }
static void add_path(mp_t *m, const char *path, bool play) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n) return;
    if (n->dir) {
        int first = m->nlist;
        for (vnode_t *c = n->child; c; c = c->next) {
            track_t t;
            if (!c->dir && is_media(c->name) && track_from(&t, c)) add_track(m, &t);
        }
        if (play && m->nlist > first) play_index(m, first);
        return;
    }
    track_t t;
    if (!track_from(&t, n)) { snprintf(m->status, sizeof m->status, "%s is not a sound file the player can open.", n->name); return; }
    add_track(m, &t);
    if (play) play_index(m, m->nlist - 1);
}

/* ---------------- spectrum ---------------- */
#define FFTN 512
static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < FFTN; i++) {
        int bit = FFTN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    /* twiddles from a quarter-wave sine table, built once */
    static float sn[FFTN]; static bool init;
    if (!init) {
        /* sin by recurrence: s[k+1] = 2cos(d)s[k] - s[k-1] */
        double d = 2 * 3.14159265358979 / FFTN, c2 = 2 * (1 - d * d / 2 + d * d * d * d / 24 - d * d * d * d * d * d / 720);
        double a = 0, b = d - d * d * d / 6 + d * d * d * d * d / 120;
        sn[0] = 0; sn[1] = (float)b;
        for (int k = 2; k < FFTN; k++) { double nx = c2 * b - a; a = b; b = nx; sn[k] = (float)nx; }
        init = true;
    }
    for (int len = 2; len <= FFTN; len <<= 1) {
        int step = FFTN / len;
        for (int i = 0; i < FFTN; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = sn[(k * step + FFTN / 4) % FFTN], wi = -sn[k * step];
                float xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                float xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}
static int ilog2x8(uint32_t v) {                                   /* 8 * log2(v), roughly */
    if (!v) return 0;
    int b = 31 - __builtin_clz(v);
    uint32_t frac = b >= 3 ? (v >> (b - 3)) & 7 : (v << (3 - b)) & 7;
    return b * 8 + (int)frac;
}
static void analyse(mp_t *m) {
    bool live = m->snd && m->state == ST_PLAY;
    if (!live) {
        for (int b = 0; b < NBANDS; b++) { m->band[b] = m->band[b] * 85 / 100; if (m->peak[b] > 0) m->peak[b] -= 12; if (m->peak[b] < m->band[b]) m->peak[b] = m->band[b]; }
        return;
    }
    static float re[FFTN], im[FFTN];
    uint32_t p = pos_frames(m);
    for (int i = 0; i < FFTN; i++) {
        uint32_t f = p + i;
        int v = f < m->snd->frames ? (m->snd->pcm[f * 2] + m->snd->pcm[f * 2 + 1]) / 2 : 0;
        /* Hann window, from the sine table's shape: 0.5 - 0.5cos */
        float x = (float)i / (FFTN - 1), w = 4 * x * (1 - x);
        re[i] = v * w; im[i] = 0;
    }
    fft(re, im);
    /* log-spaced bands over FFT bins 1..200 (about 86 Hz to 17 kHz) */
    static const uint8_t edge[NBANDS + 1] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 17, 21, 25, 29, 35, 41, 49, 59, 70, 83, 99, 118, 141, 168, 200 };
    for (int b = 0; b < NBANDS; b++) {
        int k0 = edge[b], k1 = edge[b + 1];
        float e = 0;
        for (int k = k0; k < k1; k++) e += re[k] * re[k] + im[k] * im[k];
        e /= (k1 - k0);
        uint32_t ev = e > 4e18f ? 0xFFFFFFFFu : (uint32_t)(e / 1e9f * 4096 + 1);
        int db = ilog2x8(ev);                                         /* 0..256 */
        /* music has less energy up high: tilt about 3 dB per band (measured on the sample tracks) */
        int v = db ? (db + b * 4 - 70) * 1000 / 110 : 0; v = CLAMP(v, 0, 1000);
        m->band[b] = v > m->band[b] ? v : m->band[b] * 82 / 100 + v * 18 / 100;
        if (v > m->peak[b]) m->peak[b] = v; else if (m->peak[b] > 0) m->peak[b] -= 10;
    }
}

/* ---------------- drawing helpers ---------------- */
static void fmt_time(uint32_t ms, char *b, int cap) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%02u:%02u", s / 60, s % 60);
}
static void gloss_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t top, uint32_t bot) {
    gradient_round_rect(c, x, y, w, h, r, top, bot);
    fill_round_rect_blend(c, x + 1, y + 1, w - 2, h / 2, MAX(0, r - 1), 0xFFFFFFFF, 26);
}
static void tri(canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    raster_path_t *p = raster_path_new(); if (!p) return;
    raster_move_to(p, RASTER_FIX(x0), RASTER_FIX(y0)); raster_line_to(p, RASTER_FIX(x1), RASTER_FIX(y1));
    raster_line_to(p, RASTER_FIX(x2), RASTER_FIX(y2)); raster_close(p);
    raster_fill(c, p, col, false);
    raster_path_free(p);
}
/* a round transport button; returns true when clicked */
static bool round_btn(window_t *w, int cx, int cy, int r, bool big) {
    canvas_t *c = w->surf;
    rect_t rr = R(cx - r, cy - r, r * 2, r * 2);
    bool hot = ui_hover(w, rr), held = hot && w->ui.down;
    bool click = ui_click_area(w, rr);
    uint32_t a = big ? 0xFF5AD8FF : 0xFF3A4E74, b = big ? 0xFF0B5FC8 : 0xFF141F38;
    if (hot) { a = lighten(a, 30); b = lighten(b, 25); }
    if (held) { uint32_t t = a; a = b; b = t; }
    fill_circle(c, cx, cy + 1, r + 1, 0xFF050A14);
    fill_circle(c, cx, cy, r, b);
    for (int i = 0; i < r; i++) {                                     /* vertical gradient by rings of rows */
        int yy = cy - r + i * 2;
        int hw = 0; while ((hw + 1) * (hw + 1) + (yy - cy) * (yy - cy) <= r * r) hw++;
        hline(c, cx - hw, yy, hw * 2 + 1, mix(a, b, i * 255 / r));
        if (yy + 1 <= cy + r) { int hw2 = 0; while ((hw2 + 1) * (hw2 + 1) + (yy + 1 - cy) * (yy + 1 - cy) <= r * r) hw2++; hline(c, cx - hw2, yy + 1, hw2 * 2 + 1, mix(a, b, i * 255 / r)); }
    }
    draw_circle(c, cx, cy, r, big ? 0xFF8EE8FF : 0xFF5C7299);
    /* gloss */
    for (int yy = cy - r + 2; yy < cy - r / 6; yy++) {
        int dy = yy - (cy - r * 2 / 5), hw = 0, rr2 = r * 3 / 4;
        while ((hw + 1) * (hw + 1) * 100 / 150 + dy * dy * 100 / 60 < rr2 * rr2 * 100 / 100 && hw < r) hw++;
        if (hw > 1) blend_rect(c, cx - hw, yy, hw * 2, 1, 0xFFFFFFFF, 60 - (yy - (cy - r)) * 50 / r);
    }
    return click;
}

/* album art: a tile made from the title, since the files carry no pictures we read */
static void album_art(canvas_t *c, int x, int y, int s, const track_t *t) {
    uint32_t h = 2166136261u;
    for (const char *p = t ? t->album : "x"; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (t) for (const char *p = t->artist; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    int hue = h % 360;
    uint32_t a = hsv_to_rgb(hue, 200, 230), b = hsv_to_rgb((hue + 60) % 360, 230, 90);
    gradient_round_rect(c, x, y, s, s, 6, a, b);
    for (int k = 0; k < 5; k++) {
        int r = s / 6 + (int)((h >> (k * 5)) % (s / 3));
        int cx = x + (int)((h >> (k * 3)) % s), cy = y + (int)((h >> (k * 4 + 2)) % s);
        rect_t old = canvas_push_clip(c, R(x, y, s, s));
        draw_circle(c, cx, cy, r, mix(a, 0xFFFFFFFF, 140));
        draw_circle(c, cx, cy, r + 1, mix(a, 0xFFFFFFFF, 200));
        c->clip = old;
    }
    fill_round_rect_blend(c, x, y, s, s / 2, 6, 0xFFFFFFFF, 30);
    draw_round_rect(c, x, y, s, s, 6, 0xFF000000);
    /* a note mark */
    int nx = x + s / 2 - s / 10, ny = y + s / 2 + s / 8;
    fill_circle(c, nx, ny, MAX(3, s / 12), 0xE0FFFFFF);
    fill_rect(c, nx + MAX(3, s / 12) - 2, ny - s / 3, MAX(2, s / 40), s / 3, 0xE0FFFFFF);
    fill_rect(c, nx + MAX(3, s / 12) - 2, ny - s / 3, s / 7, MAX(2, s / 40), 0xE0FFFFFF);
}

/* ---------------- the parts of the window ---------------- */
#define TOP_H 46
#define BAR_H 84
#define LIST_W 250

static void draw_top(window_t *w, mp_t *m) {
    canvas_t *c = w->surf;
    gradient_v(c, 0, 0, c->w, TOP_H, 0xFF1C2E52, 0xFF0A1428);
    hline(c, 0, 0, c->w, 0xFF3B5A94);
    hline(c, 0, TOP_H - 1, c->w, 0xFF050A14);
    draw_icon(c, ICON_MEDIA_PLAYER, 32, 10, 7);
    draw_text(c, FONT_UI_BOLD, 48, 8, "THE WRLD", C_ACCENT);
    draw_text(c, FONT_UI, 48, 24, "Media Player", C_TEXT);
    static const char *tabs[] = { "Now Playing", "Library" };
    int x = 170;
    for (int i = 0; i < 2; i++) {
        int tw = text_width(FONT_UI_BOLD, tabs[i]) + 34;
        rect_t r = R(x, 9, tw, 28);
        bool hot = ui_hover(w, r), on = m->tab == i;
        if (on) { gloss_rect(c, r.x, r.y, r.w, r.h, 14, 0xFF3D8CF0, 0xFF0E3E96); draw_round_rect(c, r.x, r.y, r.w, r.h, 14, 0xFF7CC8FF); }
        else if (hot) { gloss_rect(c, r.x, r.y, r.w, r.h, 14, 0xFF2A3E66, 0xFF15223E); draw_round_rect(c, r.x, r.y, r.w, r.h, 14, 0xFF3E5A8C); }
        draw_text_centered(c, FONT_UI_BOLD, r, tabs[i], on || hot ? 0xFFFFFFFF : C_DIM);
        if (ui_click_area(w, r)) { m->tab = i; if (i == TAB_LIB && !m->scanned) library_scan(m); }
        x += tw + 6;
    }
    /* right side: open a file or folder */
    rect_t ob = R(c->w - 104, 9, 94, 28);
    bool hot = ui_hover(w, ob);
    gloss_rect(c, ob.x, ob.y, ob.w, ob.h, 6, hot ? 0xFF2E4A7C : 0xFF213456, hot ? 0xFF16264A : 0xFF101A30);
    draw_round_rect(c, ob.x, ob.y, ob.w, ob.h, 6, 0xFF3E5A8C);
    draw_text_centered(c, FONT_UI, ob, "Open...", C_TEXT);
    if (ui_click_area(w, ob)) {
        extern void mediaplayer_open_prompt(void);
        mediaplayer_open_prompt();
    }
}

static void viz_bars(canvas_t *c, mp_t *m, rect_t a) {
    int gap = 3, bw = MAX(4, (a.w - 40 - gap * (NBANDS - 1)) / NBANDS);
    int total = bw * NBANDS + gap * (NBANDS - 1), x0 = a.x + (a.w - total) / 2, base = a.y + a.h - 40, hmax = a.h - 90;
    for (int b = 0; b < NBANDS; b++) {
        int x = x0 + b * (bw + gap), h = m->band[b] * hmax / 1000;
        uint32_t top = hsv_to_rgb(190 + b * 4, 200, 255), bot = hsv_to_rgb(215 + b * 3, 230, 170);
        for (int yy = 0; yy < h; yy += 4) {                              /* segmented LED look */
            int t = yy * 255 / MAX(1, hmax);
            fill_rect(c, x, base - yy - 3, bw, 3, mix(bot, top, t));
        }
        int py = base - m->peak[b] * hmax / 1000 - 4;
        fill_rect(c, x, py, bw, 2, 0xFFFFFFFF);
        /* reflection */
        for (int yy = 0; yy < h / 3 && yy < 34; yy += 4) blend_rect(c, x, base + 4 + yy, bw, 2, top, 70 - yy * 2);
    }
}
static void viz_scope(canvas_t *c, mp_t *m, rect_t a) {
    int cy = a.y + a.h / 2, amp = a.h / 3;
    hline(c, a.x + 20, cy, a.w - 40, 0xFF1A3460);
    if (!m->snd || m->state != ST_PLAY) { hline(c, a.x + 20, cy, a.w - 40, C_ACCENT); return; }
    uint32_t p = pos_frames(m);
    int n = a.w - 40, px = 0, py = cy;
    for (int i = 0; i < n; i++) {
        uint32_t f = p + (uint32_t)i * 2;
        int v = f < m->snd->frames ? (m->snd->pcm[f * 2] + m->snd->pcm[f * 2 + 1]) / 2 : 0;
        int x = a.x + 20 + i, y = cy - v * amp / 32768;
        if (i) {
            draw_thick_line(c, px, py, x, y, 4, 0xFF0C3C78);
            draw_line(c, px, py, x, y, C_ACCENT);
        }
        px = x; py = y;
    }
}
static void viz_aurora(canvas_t *c, mp_t *m, rect_t a) {
    int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    int lvl = 0; for (int b = 0; b < NBANDS; b++) lvl += m->band[b]; lvl /= NBANDS;
    for (int k = 0; k < 12; k++) {
        int ph = (int)(m->anim * (k + 3)) % 512, band = m->band[(k * 5) % NBANDS];
        int r = 30 + k * MIN(a.w, a.h) / 30 + band * 40 / 1000;
        /* points on an ellipse wobbling with the music */
        int ox = (int)((ph < 256 ? ph : 511 - ph) - 128) * (20 + lvl / 25) / 128;
        uint32_t col = hsv_to_rgb((180 + k * 14 + m->anim) % 360, 180, 120 + band * 135 / 1000);
        draw_circle(c, cx + ox, cy, r, col);
        draw_circle(c, cx - ox, cy, r + 1, mix(col, C_BG, 120));
    }
    for (int s = 0; s < 40; s++) {                                     /* drifting sparks */
        uint32_t h = (uint32_t)s * 2654435761u;
        int x = a.x + (int)((h >> 8) % (uint32_t)a.w), y = a.y + (int)(((h >> 16) + m->anim * (1 + s % 3)) % (uint32_t)a.h);
        fill_rect(c, x, y, 2, 2, mix(0xFFFFFFFF, C_BG, 255 - (40 + lvl * 215 / 1000)));
    }
}

static void draw_now_playing(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    gradient_v(c, a.x, a.y, a.w, a.h, 0xFF050B18, 0xFF0B1630);
    rect_t old = canvas_push_clip(c, a);
    if (m->viz == VIZ_BARS) viz_bars(c, m, a);
    else if (m->viz == VIZ_SCOPE) viz_scope(c, m, a);
    else viz_aurora(c, m, a);
    c->clip = old;
    /* info panel, top left */
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    fill_round_rect_blend(c, a.x + 12, a.y + 12, MIN(a.w - 24, 360), 84, 8, 0xFF000000, 120);
    album_art(c, a.x + 22, a.y + 22, 64, t);
    if (t) {
        draw_text_ellipsis(c, FONT_LARGE_BOLD, a.x + 98, a.y + 22, MIN(a.w - 24, 360) - 96, t->title, C_TEXT);
        draw_text_ellipsis(c, FONT_UI, a.x + 98, a.y + 48, MIN(a.w - 24, 360) - 96, t->artist, C_ACCENT);
        char b[96]; snprintf(b, sizeof b, "%s  -  %s %d kbps", t->album, t->mp3 ? "MP3" : "WAV", t->kbps);
        draw_text_ellipsis(c, FONT_SMALL, a.x + 98, a.y + 68, MIN(a.w - 24, 360) - 96, b, C_DIM);
    } else {
        draw_text(c, FONT_LARGE_BOLD, a.x + 98, a.y + 24, "Nothing playing", C_TEXT);
        draw_text(c, FONT_UI, a.x + 98, a.y + 50, "Pick something in the Library.", C_DIM);
    }
    /* visualisation chooser, bottom of the area */
    int by = a.y + a.h - 28;
    draw_text(c, FONT_SMALL, a.x + 36, by + 5, viz_names[m->viz], C_DIM);
    int lw = 0; for (int i = 0; i < VIZ_COUNT; i++) lw = MAX(lw, text_width(FONT_SMALL, viz_names[i]));
    rect_t lb = R(a.x + 10, by, 20, 22), rb = R(a.x + 40 + lw, by, 20, 22);
    tri(c, lb.x + 14, lb.y + 5, lb.x + 14, lb.y + 17, lb.x + 6, lb.y + 11, ui_hover(w, lb) ? C_ACCENT : C_DIM);
    tri(c, rb.x + 6, rb.y + 5, rb.x + 6, rb.y + 17, rb.x + 14, rb.y + 11, ui_hover(w, rb) ? C_ACCENT : C_DIM);
    if (ui_click_area(w, lb)) m->viz = (m->viz + VIZ_COUNT - 1) % VIZ_COUNT;
    if (ui_click_area(w, rb)) m->viz = (m->viz + 1) % VIZ_COUNT;
    if (m->state == ST_LOAD) {
        const char *s = "Opening the file...";
        int tw = text_width(FONT_UI_BOLD, s) + 30;
        fill_round_rect_blend(c, a.x + (a.w - tw) / 2, a.y + a.h / 2 - 16, tw, 32, 16, 0xFF000000, 160);
        draw_text_centered(c, FONT_UI_BOLD, R(a.x + (a.w - tw) / 2, a.y + a.h / 2 - 16, tw, 32), s, C_TEXT);
    }
}

static bool has_ci(const char *h, const char *q) {
    size_t n = strlen(q);
    for (; *h; h++) if (!strncasecmp(h, q, n)) return true;
    return false;
}
static bool matches(const track_t *t, const char *q) {
    if (!q[0]) return true;
    return has_ci(t->title, q) || has_ci(t->artist, q) || has_ci(t->album, q);
}
static bool dbl(mp_t *m, int where, int row) {
    uint32_t now = timer_ticks();
    bool d = m->last_click_where == where && m->last_click_row == row && now - m->last_click_ms < wm_dblclick_ms;
    m->last_click_where = where; m->last_click_row = row; m->last_click_ms = d ? 0 : now;
    return d;
}

static void dark_button(window_t *w, rect_t r, const char *label, bool *clicked) {
    canvas_t *c = w->surf;
    bool hot = ui_hover(w, r);
    gloss_rect(c, r.x, r.y, r.w, r.h, 5, hot ? 0xFF2E4A7C : 0xFF213456, hot ? 0xFF16264A : 0xFF101A30);
    draw_round_rect(c, r.x, r.y, r.w, r.h, 5, 0xFF3E5A8C);
    draw_text_centered(c, FONT_UI, r, label, C_TEXT);
    *clicked = ui_click_area(w, r);
}

static void draw_library(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    fill_rect(c, a.x, a.y, a.w, a.h, C_PANEL);
    /* tool row */
    int y = a.y + 10;
    draw_text(c, FONT_LARGE_BOLD, a.x + 14, y + 2, "Music", C_TEXT);
    bool k;
    int bx = a.x + 90;
    dark_button(w, R(bx, y, 82, 26), "Play all", &k);
    if (k) { m->nlist = 0; for (int i = 0; i < m->nlib; i++) if (matches(&m->lib[i], m->search)) add_track(m, &m->lib[i]); if (m->nlist) play_index(m, 0); }
    dark_button(w, R(bx + 88, y, 150, 26), "Add to Now Playing", &k);
    if (k && m->lib_sel >= 0 && m->lib_sel < m->nlib) { add_track(m, &m->lib[m->lib_sel]); snprintf(m->status, sizeof m->status, "Added %s to the Now Playing list.", m->lib[m->lib_sel].title); }
    dark_button(w, R(bx + 244, y, 74, 26), "Rescan", &k);
    if (k) library_scan(m);
    int sw = MIN(200, a.w - (bx + 330 - a.x) - 14);
    if (sw > 80) {
        rect_t sb = R(a.x + a.w - sw - 14, y + 1, sw, 24);
        fill_round_rect(c, sb.x, sb.y, sb.w, sb.h, 4, C_BG);
        draw_round_rect(c, sb.x, sb.y, sb.w, sb.h, 4, C_LINE);
        ui_textbox_ex(w, sb, m->search, sizeof m->search, true, C_TEXT, NULL);
        if (!m->search[0]) draw_text(c, FONT_UI, sb.x + 6, sb.y + 5, "Search music", C_DIM);
    }
    y += 40;
    /* columns */
    int cx[5] = { a.x + 14, a.x + a.w * 36 / 100, a.x + a.w * 58 / 100, a.x + a.w * 80 / 100, a.x + a.w * 90 / 100 };
    static const char *heads[5] = { "Title", "Artist", "Album", "Length", "Type" };
    gradient_v(c, a.x, y, a.w, 24, C_PANEL2, 0xFF101C34);
    for (int i = 0; i < 5; i++) { draw_text(c, FONT_UI_BOLD, cx[i], y + 5, heads[i], C_DIM); if (i) vline(c, cx[i] - 6, y + 4, 16, C_LINE); }
    y += 24;
    int rowh = 22, rows = (a.y + a.h - y - 26) / rowh;
    int idx[MAXT], n = 0;
    for (int i = 0; i < m->nlib; i++) if (matches(&m->lib[i], m->search)) idx[n++] = i;
    if (ui_hover(w, R(a.x, y, a.w, rows * rowh)) && w->ui.wheel) m->lib_scroll = CLAMP(m->lib_scroll + w->ui.wheel * 3, 0, MAX(0, n - rows));
    m->lib_scroll = CLAMP(m->lib_scroll, 0, MAX(0, n - rows));
    rect_t old = canvas_push_clip(c, R(a.x, y, a.w, rows * rowh));
    for (int r = 0; r < rows && m->lib_scroll + r < n; r++) {
        int i = idx[m->lib_scroll + r], ry = y + r * rowh;
        const track_t *t = &m->lib[i];
        rect_t rr = R(a.x, ry, a.w, rowh);
        bool sel = m->lib_sel == i, playing = m->nlist && m->cur < m->nlist && !strcmp(m->list[m->cur].path, t->path) && m->state != ST_STOP;
        if (sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, C_ROWSEL);
        else if (ui_hover(w, rr)) fill_rect(c, rr.x, rr.y, rr.w, rr.h, 0xFF172A4E);
        else if (r & 1) fill_rect(c, rr.x, rr.y, rr.w, rr.h, 0xFF0F1A30);
        uint32_t col = playing ? C_ACCENT : C_TEXT;
        char len[16]; fmt_time(t->ms, len, sizeof len);
        draw_text_ellipsis(c, playing ? FONT_UI_BOLD : FONT_UI, cx[0], ry + 4, cx[1] - cx[0] - 10, t->title, col);
        draw_text_ellipsis(c, FONT_UI, cx[1], ry + 4, cx[2] - cx[1] - 10, t->artist, col);
        draw_text_ellipsis(c, FONT_UI, cx[2], ry + 4, cx[3] - cx[2] - 10, t->album, col);
        draw_text(c, FONT_UI, cx[3], ry + 4, len, col);
        draw_text(c, FONT_UI, cx[4], ry + 4, t->mp3 ? "MP3" : "WAV", C_DIM);
        if (ui_click_area(w, rr)) {
            m->lib_sel = i;
            if (dbl(m, 1, i)) {                            /* play it, with the rest of the view after it */
                m->nlist = 0;
                for (int q = 0; q < n; q++) add_track(m, &m->lib[idx[q]]);
                int at = 0; for (int q = 0; q < n; q++) if (idx[q] == i) at = q;
                play_index(m, at);
            }
        }
    }
    c->clip = old;
    if (!n) draw_text(c, FONT_UI, a.x + 14, y + 10, m->nlib ? "No music matches your search." : "No music found. Copy .mp3 or .wav files to My Music, then click Rescan.", C_DIM);
    char b[120]; uint32_t total = 0; for (int q = 0; q < n; q++) total += m->lib[idx[q]].ms;
    char tt[16]; fmt_time(total, tt, sizeof tt);
    snprintf(b, sizeof b, "%d item%s, total time %s. Double-click a song to play it.", n, n == 1 ? "" : "s", tt);
    draw_text(c, FONT_SMALL, a.x + 14, a.y + a.h - 20, b, C_DIM);
}

static void draw_list_pane(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    fill_rect(c, a.x, a.y, a.w, a.h, C_PANEL);
    vline(c, a.x, a.y, a.h, 0xFF050A14);
    gradient_v(c, a.x + 1, a.y, a.w - 1, 30, C_PANEL2, C_PANEL);
    draw_text(c, FONT_UI_BOLD, a.x + 12, a.y + 8, "Now Playing", C_TEXT);
    bool k;
    dark_button(w, R(a.x + a.w - 58, a.y + 4, 50, 22), "Clear", &k);
    if (k) { stop(m); unload(m); m->nlist = 0; m->cur = 0; m->list_scroll = 0; }
    /* art and current title */
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    int y = a.y + 38;
    album_art(c, a.x + 12, y, 56, t);
    draw_text_ellipsis(c, FONT_UI_BOLD, a.x + 78, y + 6, a.w - 88, t ? t->album : "No album", C_TEXT);
    draw_text_ellipsis(c, FONT_SMALL, a.x + 78, y + 26, a.w - 88, t ? t->artist : "", C_DIM);
    y += 66;
    hline(c, a.x + 8, y, a.w - 16, C_LINE);
    y += 4;
    int rowh = 22, rows = (a.y + a.h - y - 28) / rowh;
    if (ui_hover(w, R(a.x, y, a.w, rows * rowh)) && w->ui.wheel) m->list_scroll += w->ui.wheel * 3;
    m->list_scroll = CLAMP(m->list_scroll, 0, MAX(0, m->nlist - rows));
    for (int r = 0; r < rows && m->list_scroll + r < m->nlist; r++) {
        int i = m->list_scroll + r, ry = y + r * rowh;
        rect_t rr = R(a.x + 1, ry, a.w - 1, rowh);
        bool cur = i == m->cur && m->state != ST_STOP;
        if (i == m->list_sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, C_ROWSEL);
        else if (ui_hover(w, rr)) fill_rect(c, rr.x, rr.y, rr.w, rr.h, 0xFF172A4E);
        char len[16]; fmt_time(m->list[i].ms, len, sizeof len);
        int lw = text_width(FONT_UI, len);
        if (cur) tri(c, a.x + 8, ry + 6, a.x + 8, ry + 16, a.x + 15, ry + 11, C_ACCENT);
        draw_text_ellipsis(c, cur ? FONT_UI_BOLD : FONT_UI, a.x + 20, ry + 4, a.w - 36 - lw, m->list[i].title, cur ? C_ACCENT : C_TEXT);
        draw_text(c, FONT_UI, a.x + a.w - 10 - lw, ry + 4, len, C_DIM);
        if (ui_click_area(w, rr)) { m->list_sel = i; if (dbl(m, 2, i)) play_index(m, i); }
    }
    if (!m->nlist) draw_text_wrapped(c, FONT_UI, a.x + 12, y + 8, a.w - 24, "The list is empty. Double-click a song in the Library, or click Open.", C_DIM, 2);
    uint32_t total = 0; for (int i = 0; i < m->nlist; i++) total += m->list[i].ms;
    char b[64], tt[16]; fmt_time(total, tt, sizeof tt);
    snprintf(b, sizeof b, "%d item%s  -  %s", m->nlist, m->nlist == 1 ? "" : "s", tt);
    hline(c, a.x + 8, a.y + a.h - 26, a.w - 16, C_LINE);
    draw_text(c, FONT_SMALL, a.x + 12, a.y + a.h - 19, b, C_DIM);
}

static void draw_bar(window_t *w, mp_t *m) {
    canvas_t *c = w->surf;
    int y0 = c->h - BAR_H;
    gradient_v(c, 0, y0, c->w, BAR_H, 0xFF1A2B4D, 0xFF070E1C);
    hline(c, 0, y0, c->w, 0xFF3B5A94);
    /* seek bar */
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    uint32_t total = m->snd ? m->snd->frames : 0, pos = pos_frames(m);
    rect_t sk = R(14, y0 + 8, c->w - 28, 12);
    bool can_seek = m->snd && (m->state == ST_PLAY || m->state == ST_PAUSE);
    if (can_seek && ui_hover(w, sk) && w->ui.pressed) m->seek_drag = true;
    if (m->seek_drag) {
        m->drag_x = CLAMP(w->ui.mx - sk.x, 0, sk.w);
        pos = (uint32_t)((uint64_t)m->drag_x * total / MAX(1, sk.w));
        if (!w->ui.down) { m->seek_drag = false; if (can_seek) seek_ms(m, (int64_t)pos * 1000 / audio_rate()); }
    }
    int fill = total ? (int)((uint64_t)pos * sk.w / total) : 0;
    fill_round_rect(c, sk.x, sk.y + 3, sk.w, 6, 3, 0xFF060B16);
    draw_round_rect(c, sk.x, sk.y + 3, sk.w, 6, 3, 0xFF2A3E66);
    if (fill > 2) gradient_round_rect(c, sk.x, sk.y + 3, fill, 6, 3, C_ACCENT, C_ACCENT2);
    if (can_seek) {
        int tx = sk.x + fill;
        fill_circle(c, tx, sk.y + 6, 7, 0xFF0A1428);
        fill_circle(c, tx, sk.y + 6, 6, ui_hover(w, sk) || m->seek_drag ? 0xFFFFFFFF : 0xFFD8E8FF);
        fill_circle(c, tx, sk.y + 6, 3, C_ACCENT2);
    }
    /* left: status and time */
    int cy = y0 + 52;
    const char *st = m->state == ST_PLAY ? "Playing" : m->state == ST_PAUSE ? "Paused" : m->state == ST_LOAD ? "Opening" : "Stopped";
    char b[160];
    if (t && m->state != ST_STOP) snprintf(b, sizeof b, "%s: %s", st, t->title); else snprintf(b, sizeof b, "%s", m->status[0] ? m->status : st);
    int ctl_x = c->w / 2;
    draw_text_ellipsis(c, FONT_UI, 16, cy - 16, MAX(40, ctl_x - 150 - 16), b, C_TEXT);
    char tp[16], tl[16];
    uint32_t rate = (uint32_t)audio_rate();
    fmt_time(rate ? (uint32_t)((uint64_t)pos * 1000 / rate) : 0, tp, sizeof tp);
    fmt_time(t ? (total && rate ? (uint32_t)((uint64_t)total * 1000 / rate) : t->ms) : 0, tl, sizeof tl);
    snprintf(b, sizeof b, "%s / %s", tp, tl);
    draw_text(c, FONT_UI_BOLD, 16, cy + 2, b, C_ACCENT);
    /* centre: transport */
    if (round_btn(w, ctl_x, cy, 22, true)) toggle_play(m);
    if (m->state == ST_PLAY) { fill_rect(c, ctl_x - 7, cy - 8, 5, 16, 0xFFFFFFFF); fill_rect(c, ctl_x + 2, cy - 8, 5, 16, 0xFFFFFFFF); }
    else tri(c, ctl_x - 5, cy - 9, ctl_x - 5, cy + 9, ctl_x + 9, cy, 0xFFFFFFFF);
    int sx = ctl_x - 48;
    if (round_btn(w, sx, cy, 14, false)) stop(m);
    fill_rect(c, sx - 5, cy - 5, 10, 10, C_TEXT);
    int px = ctl_x - 84;
    if (round_btn(w, px, cy, 14, false)) {
        if (pos_frames(m) > (uint32_t)audio_rate() * 3) seek_ms(m, 0);
        else { int n = next_index(m, -1); if (n >= 0) play_index(m, n); }
    }
    fill_rect(c, px - 6, cy - 5, 2, 10, C_TEXT); tri(c, px + 5, cy - 5, px + 5, cy + 5, px - 3, cy, C_TEXT);
    int nx = ctl_x + 48;
    if (round_btn(w, nx, cy, 14, false)) { int n = next_index(m, 1); if (n >= 0) play_index(m, n); }
    tri(c, nx - 5, cy - 5, nx - 5, cy + 5, nx + 3, cy, C_TEXT); fill_rect(c, nx + 4, cy - 5, 2, 10, C_TEXT);
    /* shuffle and repeat toggles */
    int tx = ctl_x + 84;
    static const char *tg[2] = { "Shuffle", "Repeat" };
    bool *tv[2] = { &m->shuffle, &m->repeat };
    for (int i = 0; i < 2; i++) {
        int tw = text_width(FONT_SMALL, tg[i]) + 16;
        rect_t r = R(tx, cy - 10, tw, 20);
        bool on = *tv[i], hot = ui_hover(w, r);
        fill_round_rect(c, r.x, r.y, r.w, r.h, 10, on ? 0xFF124A8E : hot ? 0xFF1C2E52 : 0xFF0F1A30);
        draw_round_rect(c, r.x, r.y, r.w, r.h, 10, on ? C_ACCENT : 0xFF2E4670);
        draw_text_centered(c, FONT_SMALL, r, tg[i], on ? 0xFFFFFFFF : C_DIM);
        if (ui_click_area(w, r)) *tv[i] = !*tv[i];
        tx += tw + 6;
    }
    /* right: mute and volume */
    int vx = c->w - 150;
    if (vx > tx + 10) {
        rect_t mb = R(vx - 30, cy - 10, 22, 20);
        uint32_t mc = ui_hover(w, mb) ? C_ACCENT : C_TEXT;
        fill_rect(c, mb.x + 2, cy - 3, 5, 6, mc); tri(c, mb.x + 6, cy - 3, mb.x + 6, cy + 3, mb.x + 12, cy - 8, mc); tri(c, mb.x + 6, cy + 3, mb.x + 12, cy + 8, mb.x + 12, cy - 8, mc);
        if (audio_muted) { draw_thick_line(c, mb.x + 14, cy - 5, mb.x + 21, cy + 5, 2, 0xFFFF5A4A); draw_thick_line(c, mb.x + 21, cy - 5, mb.x + 14, cy + 5, 2, 0xFFFF5A4A); }
        else { draw_line(c, mb.x + 15, cy - 3, mb.x + 15, cy + 3, mc); draw_line(c, mb.x + 18, cy - 6, mb.x + 18, cy + 6, mc); }
        if (ui_click_area(w, mb)) audio_muted = !audio_muted;
        rect_t vr = R(vx, cy - 8, 120, 16);
        bool vh = ui_hover(w, vr);
        if (vh && w->ui.down) audio_volume = CLAMP((w->ui.mx - vr.x) * 100 / vr.w, 0, 100);
        if (vh && w->ui.wheel) audio_volume = CLAMP(audio_volume - w->ui.wheel * 5, 0, 100);
        (void)ui_click_area(w, vr);
        int vf = audio_volume * vr.w / 100;
        fill_round_rect(c, vr.x, cy - 3, vr.w, 6, 3, 0xFF060B16);
        if (vf > 2) gradient_round_rect(c, vr.x, cy - 3, vf, 6, 3, audio_muted ? 0xFF5C6A80 : C_ACCENT, audio_muted ? 0xFF3A4458 : C_ACCENT2);
        fill_circle(c, vr.x + vf, cy, 6, vh ? 0xFFFFFFFF : 0xFFD8E8FF);
    }
}

static void mp_paint(window_t *w) {
    mp_t *m = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, C_BG);
    draw_top(w, m);
    int listw = c->w >= 720 ? LIST_W : 0;
    rect_t area = R(0, TOP_H, c->w - listw, c->h - TOP_H - BAR_H);
    if (m->tab == TAB_LIB) draw_library(w, m, area); else draw_now_playing(w, m, area);
    if (listw) draw_list_pane(w, m, R(c->w - listw, TOP_H, listw, c->h - TOP_H - BAR_H));
    draw_bar(w, m);
}

static void mp_tick(window_t *w) {
    mp_t *m = w->data;
    m->anim++;
    job_t *j = m->job;
    if (m->state == ST_LOAD && j && j->state == 3) {
        m->job = NULL; m->state = ST_STOP;
        snprintf(m->status, sizeof m->status, "Cannot play %s: %s.", m->list[m->cur].title, j->err);
        kfree(j);
    } else if (m->state == ST_LOAD && j && (j->state == 1 || j->state == 2)) {
        m->snd = j->snd; j->taken = true;
        m->voice = audio_play(m->snd);
        m->state = m->voice >= 0 ? ST_PLAY : ST_STOP;
        m->status[0] = 0;
        if (m->voice < 0) snprintf(m->status, sizeof m->status, "The sound device is not ready.");
    }
    if (m->job && m->job->taken && m->job->state == 2) { kfree(m->job); m->job = NULL; }   /* fully decoded */
    if (m->state == ST_PLAY && m->snd && !audio_voice_active(m->voice, m->snd)) {
        int n = next_index(m, 1);                                     /* the track ended */
        if (n >= 0 && !(n == m->cur && !m->repeat)) play_index(m, n);
        else if (n == m->cur && m->repeat) { m->voice = audio_play(m->snd); if (m->voice < 0) m->state = ST_STOP; }
        else m->state = ST_STOP;
    }
    analyse(m);
    w->dirty = true;
}

static void mp_key(window_t *w, key_event_t *e) {
    mp_t *m = w->data;
    if (!e->pressed) return;
    bool ctrl = e->mods & MOD_CTRL;
    if (ctrl && (e->key == 'p' || e->key == 'P')) toggle_play(m);
    else if (ctrl && (e->key == 's' || e->key == 'S')) stop(m);
    else if (ctrl && (e->key == 'f' || e->key == 'F')) { int n = next_index(m, 1); if (n >= 0) play_index(m, n); }
    else if (ctrl && (e->key == 'b' || e->key == 'B')) { int n = next_index(m, -1); if (n >= 0) play_index(m, n); }
    else if (ctrl && (e->key == 'h' || e->key == 'H')) m->shuffle = !m->shuffle;
    else if (ctrl && (e->key == 't' || e->key == 'T')) m->repeat = !m->repeat;
    else if (ctrl && (e->key == 'o' || e->key == 'O')) { extern void mediaplayer_open_prompt(void); mediaplayer_open_prompt(); }
    else if (ctrl && e->key == '1') m->tab = TAB_NOW;
    else if (ctrl && e->key == '2') { m->tab = TAB_LIB; if (!m->scanned) library_scan(m); }
    else if (e->key == KEY_F8) audio_muted = !audio_muted;
    else if (e->key == KEY_F9) audio_volume = MAX(0, audio_volume - 5);
    else if (e->key == KEY_F10) audio_volume = MIN(100, audio_volume + 5);
    else if (m->tab == TAB_NOW && e->key == ' ') toggle_play(m);
    else if (m->tab == TAB_NOW && (e->key == KEY_LEFT || e->key == KEY_RIGHT)) {
        int64_t now = (int64_t)pos_frames(m) * 1000 / audio_rate();
        seek_ms(m, now + (e->key == KEY_RIGHT ? 5000 : -5000));
    } else return;
    w->dirty = true;
}

static void mp_destroy(window_t *w) {
    mp_t *m = w->data;
    unload(m);
    kfree(m->lib); kfree(m->list); kfree(m);
    if (mp_win == w) mp_win = NULL;
}

static void open_cb(void *ctx, const char *text) {
    (void)ctx;
    if (!text || !text[0] || !mp_win || !wm_alive(mp_win)) return;
    mp_t *m = mp_win->data;
    char p[200]; strlcpy(p, text, sizeof p);
    size_t l = strlen(p); if (l >= 2 && p[0] == '"' && p[l - 1] == '"') { p[l - 1] = 0; memmove(p, p + 1, l - 1); }
    if (!vfs_resolve(p, NULL)) { msgbox("THE WRLD Media Player", "The file or folder could not be found.", MBI_ERROR, MBX_OK, NULL, NULL); return; }
    add_path(m, p, true);
    m->tab = TAB_NOW;
    mp_win->dirty = true;
}
void mediaplayer_open_prompt(void) {
    input_box("Open", "Type the path of a sound file or a folder of music:", MUSIC_DIR, open_cb, NULL);
}

void mediaplayer_open(const char *path) {
    if (mp_win && wm_alive(mp_win)) {
        wm_focus(mp_win);
        if (path) { mp_t *m = mp_win->data; add_path(m, path, true); m->tab = TAB_NOW; }
        return;
    }
    window_t *w = wm_create("THE WRLD Media Player", -1, -1, MIN(900, SCREEN_W - 40), MIN(600, SCREEN_H - 70), WF_RESIZABLE);
    mp_t *m = kzalloc(sizeof *m);
    m->lib = kzalloc(sizeof(track_t) * MAXT);
    m->list = kzalloc(sizeof(track_t) * MAXT);
    m->voice = -1; m->lib_sel = -1; m->list_sel = -1;
    w->data = m; w->paint = mp_paint; w->destroy = mp_destroy; w->key = mp_key;
    w->icon = ICON_MEDIA_PLAYER;
    w->min_w = 560; w->min_h = 420;
    mp_win = w;
    library_scan(m);
    if (path) add_path(m, path, true);
    else {
        /* start with My Music in the Now Playing list, ready to play */
        for (int i = 0; i < m->nlib && !strncasecmp(m->lib[i].path, MUSIC_DIR, strlen(MUSIC_DIR)); i++) add_track(m, &m->lib[i]);
        if (!m->nlist) m->tab = TAB_LIB;
    }
    wm_set_timer(w, 50, mp_tick);
    wm_center(w);
}
