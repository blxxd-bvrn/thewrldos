/* THE WRLD Media Player, after the media players of the XP era:
 * a skinned window with its own rounded frame, the feature taskbar down the
 * left (Now Playing, Media Guide, Copy from CD, Media Library, Radio Tuner,
 * Copy to CD or Device, Premium Services, Skin Chooser), the video or
 * visualisation in the middle with its toolbar underneath, the playlist pane on
 * the right and the round transport controls along the bottom. Skins change all
 * of it; the Classic skin goes back to the system frame and grey 3D controls.
 *
 * Files are decoded on their own task through src/media (MP3, MP2, Ogg Vorbis,
 * FLAC, WAV in its common codecs, AIFF, .au and MPEG-1 video); the mixer in
 * audio.c plays the sound and MPEG-1 pictures are decoded in step with it.
 * The visualisations are in mpviz.c. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "audio.h"
#include "raster.h"
#include "disk.h"
#include "mediadec.h"
#include "mp3glue.h"
#include "mpviz.h"

#define MAXT 256
#define MUSIC_DIR USER_DOCS "\\My Music"
#define APP_NAME "THE WRLD Media Player"

typedef struct {
    char path[200], title[80], artist[64], album[64], format[40];
    uint32_t ms; int kbps; bool video;
} track_t;

/* A decode on its own task, straight into a sound of the full length that
 * starts playing once half a second is ready. Whoever finishes last frees the
 * job: the player once the task is done (state 2 or 3), else the task once the
 * player has let go (`abandoned`). Tasks are cooperative, so the task checks
 * `abandoned` after every yield before touching the sound again. */
typedef struct {
    volatile int state;                 /* 0 opening, 1 playable and decoding, 2 finished, 3 failed */
    volatile bool abandoned, taken;
    volatile uint32_t ready;
    char path[200];
    sound_t *snd;
    char err[96];
} job_t;

enum { ST_STOP, ST_LOAD, ST_PLAY, ST_PAUSE };
enum { P_NOW, P_GUIDE, P_RIP, P_LIB, P_RADIO, P_COPY, P_PREMIUM, P_SKIN, P_COUNT };
static const char *feature_names[P_COUNT] = { "Now Playing", "Media Guide", "Copy from CD", "Media Library", "Radio Tuner",
                                              "Copy to CD or Device", "Premium Services", "Skin Chooser" };

/* ---------------- skins ---------------- */
typedef struct {
    const char *name, *desc;
    bool classic;                                   /* system frame, grey 3D controls */
    uint32_t fr_hi, fr_top, fr_bot, fr_edge;        /* frame */
    uint32_t tb_top, tb_bot, tb_text, tb_sel, tb_line, tb_hot;   /* feature taskbar */
    uint32_t pn_top, pn_bot, pn_art, pn_text, pn_dim, pn_line, pn_sel;   /* playlist pane */
    uint32_t bar_top, bar_bot, bar_text;            /* toolbar under the video */
    uint32_t bt_hi, bt_lo, bt_edge, bt_glyph;       /* round buttons */
    uint32_t thumb, groove;                         /* sliders */
    uint32_t pg_bg, pg_text, pg_head, pg_head_text, pg_row, pg_sel;   /* pages */
    uint32_t lcd, lcd_text;                         /* status box */
} skin_t;
static const skin_t skins[] = {
    { "WRLD Blue", "The default skin: blue frame, white taskbar, glassy buttons.", false,
      0xFF8CB0F0, 0xFF4A6FD0, 0xFF2A4AA8, 0xFF1A2E78,
      0xFFFFFFFF, 0xFFC6D6F4, 0xFF10246A, 0xFFDCE7FB, 0xFF9CB4E4, 0xFFEAF0FC,
      0xFF4C5EAE, 0xFF2C3C8E, 0xFF3A4C9E, 0xFFFFFFFF, 0xFFB8C8F0, 0xFF6A7CC6, 0xFF4A64C4,
      0xFF2A4290, 0xFF14256A, 0xFFFFFFFF,
      0xFFF8FBFF, 0xFF7C9CDC, 0xFF2E4AA0, 0xFF10246A,
      0xFF4CC04C, 0xFFE4ECFA,
      0xFFFFFFFF, 0xFF10246A, 0xFF3A5CC0, 0xFFFFFFFF, 0xFFEEF3FC, 0xFFC4D6F6,
      0xFF14256A, 0xFF9CE0FF },
    { "Silver", "Brushed silver, like the Silver visual style.", false,
      0xFFF4F4F8, 0xFFBDBECE, 0xFF8E90A6, 0xFF5C5E74,
      0xFFFFFFFF, 0xFFDCDCE6, 0xFF2C2E40, 0xFFE8E8F0, 0xFFB4B6C6, 0xFFF2F2F8,
      0xFF8E90A8, 0xFF6A6C84, 0xFF7C7E96, 0xFFFFFFFF, 0xFFDCDCE8, 0xFF9C9EB4, 0xFF8486A2,
      0xFF6E7088, 0xFF4A4C60, 0xFFFFFFFF,
      0xFFFFFFFF, 0xFFA6A8BC, 0xFF606278, 0xFF2C2E40,
      0xFF4CC04C, 0xFFF0F0F6,
      0xFFFFFFFF, 0xFF2C2E40, 0xFF8486A2, 0xFFFFFFFF, 0xFFF4F4F8, 0xFFDCDCEA,
      0xFF3C3E52, 0xFFC8F0FF },
    { "Olive Green", "Olive and khaki, like the Olive Green visual style.", false,
      0xFFD8E4A8, 0xFF8EA45C, 0xFF5E7438, 0xFF3C4C22,
      0xFFFFFFF4, 0xFFE2E8C4, 0xFF34401A, 0xFFEEF2D8, 0xFFB8C494, 0xFFF6F8E8,
      0xFF7A8E4C, 0xFF566A30, 0xFF68803E, 0xFFFFFFFF, 0xFFE0E8C0, 0xFF92A46A, 0xFF7E9450,
      0xFF5A6E32, 0xFF3C4C20, 0xFFFFFFFF,
      0xFFFFFFF4, 0xFFB4C47C, 0xFF5C7034, 0xFF34401A,
      0xFFF0A030, 0xFFF2F4E4,
      0xFFFFFFFA, 0xFF34401A, 0xFF7A8E4C, 0xFFFFFFFF, 0xFFF4F6E8, 0xFFE0E8C0,
      0xFF2E3A18, 0xFFE0FFA0 },
    { "Classic", "The WRLD Classic look: the system frame and grey 3D controls.", true,
      0xFFD4D0C8, 0xFFD4D0C8, 0xFFD4D0C8, 0xFF808080,
      0xFFD4D0C8, 0xFFD4D0C8, 0xFF000000, 0xFF0A246A, 0xFF808080, 0xFFE0DCD4,
      0xFFD4D0C8, 0xFFD4D0C8, 0xFFFFFFFF, 0xFF000000, 0xFF404040, 0xFF808080, 0xFF0A246A,
      0xFFD4D0C8, 0xFFD4D0C8, 0xFF000000,
      0xFFD4D0C8, 0xFFD4D0C8, 0xFF404040, 0xFF000000,
      0xFFD4D0C8, 0xFFFFFFFF,
      0xFFFFFFFF, 0xFF000000, 0xFF0A246A, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF0A246A,
      0xFF000000, 0xFF00FF00 },
    { "Midnight", "Black and steel with cyan light, for watching in the dark.", false,
      0xFF5A6478, 0xFF262C3A, 0xFF10141C, 0xFF000000,
      0xFF2A3040, 0xFF141822, 0xFFC8D6EC, 0xFF34405A, 0xFF3A4458, 0xFF242A38,
      0xFF1C2230, 0xFF0C1018, 0xFF202838, 0xFFDCE6F8, 0xFF7C8AA4, 0xFF2C3446, 0xFF1C4C78,
      0xFF1E2432, 0xFF0A0E16, 0xFF7CDCFF,
      0xFF6A7488, 0xFF1A1E2A, 0xFF000000, 0xFF7CDCFF,
      0xFF3CC8FF, 0xFF2A3242,
      0xFF141822, 0xFFDCE6F8, 0xFF243048, 0xFF7CDCFF, 0xFF1A1E2A, 0xFF1C4C78,
      0xFF000000, 0xFF3CC8FF },
    { "Tangerine", "Warm orange and cream.", false,
      0xFFFFD49C, 0xFFF08A2C, 0xFFC45A10, 0xFF7A3404,
      0xFFFFFCF4, 0xFFFDE2C2, 0xFF5A2A04, 0xFFFFEEDC, 0xFFF0B888, 0xFFFFF6EC,
      0xFFD8701C, 0xFFA84C0C, 0xFFC0601A, 0xFFFFFFFF, 0xFFFFE0C0, 0xFFE89050, 0xFFE07C28,
      0xFFB4520E, 0xFF7C3404, 0xFFFFFFFF,
      0xFFFFF8F0, 0xFFF4A860, 0xFF9C4808, 0xFF5A2A04,
      0xFF4CC04C, 0xFFFFF0E2,
      0xFFFFFDF8, 0xFF5A2A04, 0xFFE07C28, 0xFFFFFFFF, 0xFFFFF4EA, 0xFFFFDCBC,
      0xFF5A2A04, 0xFFFFD49C },
    { "Lavender", "Soft purple glass.", false,
      0xFFE4D4FC, 0xFF9A78DA, 0xFF6C48B0, 0xFF3E2470,
      0xFFFFFFFF, 0xFFE6DCF8, 0xFF3A1E6A, 0xFFF0EAFC, 0xFFC4B0EC, 0xFFF8F4FE,
      0xFF8462C4, 0xFF5E3E9C, 0xFF7252B4, 0xFFFFFFFF, 0xFFE8DCFC, 0xFFA088DC, 0xFF8A6AD0,
      0xFF60409E, 0xFF3E2470, 0xFFFFFFFF,
      0xFFFCF8FF, 0xFFB49AE6, 0xFF5A3A98, 0xFF3A1E6A,
      0xFF4CC04C, 0xFFF4EEFE,
      0xFFFFFFFF, 0xFF3A1E6A, 0xFF8A6AD0, 0xFFFFFFFF, 0xFFF6F2FE, 0xFFE4D8FA,
      0xFF2A1450, 0xFFE4D4FC },
};
#define NSKINS ((int)ARRAY_LEN(skins))

typedef struct {
    int page;
    track_t *lib; int nlib; bool scanned;
    track_t *list; int nlist, cur;
    int state; sound_t *snd; int voice; job_t *job;
    bool shuffle, repeat;
    int skin, skin_sel;
    bool taskbar_hidden;
    int viz_c, viz_p;
    viz_audio_t au;
    float bass_avg; uint32_t last_beat;
    /* video */
    mvid_t *vid; canvas_t *vframe; int vid_track;
    bool seek_drag;
    int lib_sel, lib_scroll, list_sel, list_scroll, lib_view, lib_tree_scroll;
    char lib_filter[64];
    char search[48];
    uint32_t anim;
    int quiet;                      /* ticks since anything last moved: no repaints after a second */
    char status[120];
    int last_click_row, last_click_where; uint32_t last_click_ms;
    canvas_t *art; char art_key[140];
    bool copy_sel[MAXT]; int copy_dest; char copy_msg[120];
    window_t *full;
    uint32_t viz_gen;               /* bumped by the player's tick: one visualizer step per tick */
    struct { canvas_t *cv; uint32_t gen; int c, p; } vcache[2];   /* the last frame, window and full screen */
} mp_t;
static window_t *mp_win;
#define SK (&skins[CLAMP(m->skin, 0, NSKINS - 1)])

/* ---------------- tags and library ---------------- */
static void tag_text(const uint8_t *p, uint32_t n, char *out, int cap) {
    if (!n) return;
    int enc = p[0], k = 0; p++; n--;
    if (enc == 1 || enc == 2) {
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
            if (enc == 3 && p[i] >= 0x80) { if ((p[i] & 0xC0) == 0xC0) out[k++] = '?'; continue; }
            out[k++] = p[i] < 128 ? (char)p[i] : '?';
        }
    }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ') out[--k] = 0;
}
static void read_id3(track_t *t, const uint8_t *d, uint32_t n) {
    if (n < 10 || memcmp(d, "ID3", 3)) {
        if (n >= 128 && !memcmp(d + n - 128, "TAG", 3)) {
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
    if (d[5] & 0x40) {
        uint32_t ex = ver >= 4 ? (((d[10] & 0x7F) << 21) | ((d[11] & 0x7F) << 14) | ((d[12] & 0x7F) << 7) | (d[13] & 0x7F))
                               : (uint32_t)((d[10] << 24) | (d[11] << 16) | (d[12] << 8) | d[13]) + 4;
        o += ex;
    }
    if (ver < 3) {
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
static bool is_media(const char *name) { return mdec_known_ext(vfs_ext(name)); }
static bool track_from(track_t *t, vnode_t *n) {
    memset(t, 0, sizeof *t);
    vfs_path(n, t->path, sizeof t->path);
    strlcpy(t->title, n->name, sizeof t->title);
    char *dot = strrchr(t->title, '.'); if (dot) *dot = 0;
    if (!n->data || !n->size) return false;
    mdec_info_t info;
    if (mdec_probe(n->data, n->size, vfs_ext(n->name), &info)) return false;
    t->ms = info.ms; t->kbps = info.kbps; t->video = info.video;
    strlcpy(t->format, info.format[0] ? info.format : "Audio", sizeof t->format);
    if (info.title[0]) strlcpy(t->title, info.title, sizeof t->title);
    if (info.artist[0]) strlcpy(t->artist, info.artist, sizeof t->artist);
    if (info.album[0]) strlcpy(t->album, info.album, sizeof t->album);
    if (!strncmp(t->format, "MP", 2) && t->format[2] >= '1' && t->format[2] <= '3') read_id3(t, n->data, n->size);
    if (!t->artist[0]) strlcpy(t->artist, t->video ? "Video" : "Unknown Artist", sizeof t->artist);
    if (!t->album[0]) strlcpy(t->album, t->video ? "Videos" : "Unknown Album", sizeof t->album);
    return true;
}
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
    for (int i = 1; i < m->nlib; i++) {                 /* My Music first, then by artist and title */
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
static void job_quit(job_t *j, mdec_t *d) {
    if (d) mdec_close(d);
    if (!j->taken) sound_free(j->snd);
    kfree(j);
}
static void decode_task(void *arg) {
    job_t *j = arg;
    if (j->abandoned) { job_quit(j, NULL); return; }
    vnode_t *n = vfs_resolve(j->path, NULL);
    if (!n || n->dir || !n->data) { snprintf(j->err, sizeof j->err, "the file could not be found"); j->state = 3; return; }
    mdec_info_t info;
    mdec_t *d = mdec_open(n->data, n->size, vfs_ext(n->name), &info, j->err, sizeof j->err);
    if (!d) { j->state = 3; return; }
    uint32_t R = (uint32_t)audio_rate(), total = (uint32_t)((uint64_t)info.ms * R / 1000) + R / 20;
    sound_t *s = kzalloc(sizeof *s);
    s->pcm = s ? kmalloc((size_t)total * 4 + 8) : NULL;
    if (!s || !s->pcm) { kfree(s); mdec_close(d); snprintf(j->err, sizeof j->err, "not enough memory"); j->state = 3; return; }
    memset(s->pcm, 0, (size_t)total * 4 + 8);
    s->frames = total; j->snd = s;
    /* resample chunk by chunk: `pos` is 16.16 in input frames, `base` the input
     * index of in[0], `last` the input frame before it */
    static int16_t in[4096 * 2];
    int16_t last[2] = { 0, 0 };
    uint64_t pos = 0, step = ((uint64_t)MAX(1, info.rate) << 16) / R;
    uint32_t base = 0, out = 0;
    int nf, count = 0;
    while ((nf = mdec_read(d, in, 4096)) > 0) {
        #define IN(k, c) ((k) < base ? last[c] : in[((k) - base) * 2 + (c)])
        while (out < total && (uint32_t)(pos >> 16) + 1 < base + (uint32_t)nf) {
            uint32_t f = (uint32_t)(pos >> 16), fr = (uint32_t)(pos & 0xFFFF);
            for (int c = 0; c < 2; c++) { int a = IN(f, c), b = IN(f + 1, c); s->pcm[out * 2 + c] = (int16_t)(a + (int)(((int64_t)(b - a) * fr) >> 16)); }
            out++; pos += step;
        }
        #undef IN
        last[0] = in[(nf - 1) * 2]; last[1] = in[(nf - 1) * 2 + 1];
        base += nf;
        j->ready = out;
        if (j->state == 0 && out >= R / 2) j->state = 1;
        if (++count % 2 == 0) {
            task_yield();
            if (j->abandoned) { job_quit(j, d); return; }
        }
    }
    mdec_close(d);
    j->ready = s->frames;
    j->state = 2;
}
static void drop_job(mp_t *m) {
    job_t *j = m->job;
    if (!j) return;
    m->job = NULL;
    if (j->state >= 2) { if (!j->taken) sound_free(j->snd); kfree(j); }
    else j->abandoned = true;
}
static void close_video(mp_t *m) {
    if (m->vid) { mvid_close(m->vid); m->vid = NULL; }
    if (m->vframe) { canvas_free(m->vframe); m->vframe = NULL; }
    m->vid_track = -1;
}
static void unload(mp_t *m) {
    drop_job(m);
    if (m->snd) { audio_voice_stop(m->voice, m->snd); sound_free(m->snd); m->snd = NULL; }
    m->voice = -1;
    close_video(m);
}
static void open_video(mp_t *m) {
    close_video(m);
    if (m->cur < 0 || m->cur >= m->nlist || !m->list[m->cur].video) return;
    vnode_t *n = vfs_resolve(m->list[m->cur].path, NULL);
    if (!n || !n->data) return;
    int w, h; float fps;
    m->vid = mvid_open(n->data, n->size, &w, &h, &fps);
    if (m->vid) { m->vframe = canvas_new(w, h); if (m->vframe) gfx_clear(m->vframe, 0xFF000000); m->vid_track = m->cur; }
}
static void play_index(mp_t *m, int i) {
    if (i < 0 || i >= m->nlist) return;
    unload(m);
    m->cur = i; m->list_sel = i;
    viz_reset();
    if (!audio_present()) { m->state = ST_STOP; snprintf(m->status, sizeof m->status, "There is no sound device to play on."); return; }
    job_t *j = kzalloc(sizeof *j);
    strlcpy(j->path, m->list[i].path, sizeof j->path);
    m->job = j; m->state = ST_LOAD;
    snprintf(m->status, sizeof m->status, "Opening %s...", m->list[i].title);
    open_video(m);
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
static uint32_t pos_frames(mp_t *m) { return m->snd && m->state != ST_STOP ? audio_voice_position(m->voice, m->snd) : 0; }
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
    if (m->job && m->job->snd == m->snd && m->job->state < 2 && f > m->job->ready) f = m->job->ready;
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
    if (!track_from(&t, n)) { snprintf(m->status, sizeof m->status, "%s is not a file the player can open.", n->name); return; }
    add_track(m, &t);
    if (play) play_index(m, m->nlist - 1);
}

/* ---------------- listening: spectrum, levels, beats ---------------- */
#define FFTN 512
static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < FFTN; i++) {
        int bit = FFTN >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    static float sn[FFTN]; static bool init;
    if (!init) {
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
static int ilog2x8(uint32_t v) {
    if (!v) return 0;
    int b = 31 - __builtin_clz(v);
    uint32_t frac = b >= 3 ? (v >> (b - 3)) & 7 : (v << (3 - b)) & 7;
    return b * 8 + (int)frac;
}
static void analyse(mp_t *m) {
    viz_audio_t *a = &m->au;
    a->ms = timer_ticks();
    a->beat = false;
    bool live = m->snd && m->state == ST_PLAY;
    a->playing = live;
    if (!live) {
        for (int b = 0; b < VIZ_BANDS; b++) a->band[b] *= 0.85f;
        for (int i = 0; i < 512; i++) a->wave[i] = (int16_t)(a->wave[i] * 7 / 8);
        a->bass *= 0.85f; a->mid *= 0.85f; a->treble *= 0.85f; a->level *= 0.85f;
        return;
    }
    static float re[FFTN], im[FFTN];
    uint32_t p = pos_frames(m);
    long sum = 0;
    for (int i = 0; i < FFTN; i++) {
        uint32_t f = p + i;
        int v = f < m->snd->frames ? (m->snd->pcm[f * 2] + m->snd->pcm[f * 2 + 1]) / 2 : 0;
        a->wave[i] = (int16_t)v;
        sum += v < 0 ? -v : v;
        float x = (float)i / (FFTN - 1), w = 4 * x * (1 - x);
        re[i] = v * w; im[i] = 0;
    }
    fft(re, im);
    /* 32 log-spaced bands over bins 1..230 */
    static const uint8_t edge[VIZ_BANDS + 1] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20, 23, 26, 30, 34, 39, 45, 52, 60, 70, 82, 96, 112, 132, 156, 190, 230 };
    for (int b = 0; b < VIZ_BANDS; b++) {
        float e = 0;
        for (int k = edge[b]; k < edge[b + 1]; k++) e += re[k] * re[k] + im[k] * im[k];
        e /= (edge[b + 1] - edge[b]);
        uint32_t ev = e > 4e18f ? 0xFFFFFFFFu : (uint32_t)(e / 1e9f * 4096 + 1);
        int db = ilog2x8(ev);
        float v = db ? (db + b * 3.5f - 70) / 110.0f : 0;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        a->band[b] = v > a->band[b] ? v : a->band[b] * 0.8f + v * 0.2f;
    }
    float bs = 0, md = 0, tr = 0;
    for (int b = 0; b < 6; b++) bs += a->band[b];
    for (int b = 6; b < 20; b++) md += a->band[b];
    for (int b = 20; b < VIZ_BANDS; b++) tr += a->band[b];
    a->bass = bs / 6; a->mid = md / 14; a->treble = tr / 12;
    a->level = MIN(1.0f, sum / (float)FFTN / 9000.0f);
    /* a beat: the bass well above its running average */
    m->bass_avg = m->bass_avg * 0.93f + a->bass * 0.07f;
    if (a->bass > m->bass_avg * 1.18f && a->bass > 0.3f && a->ms - m->last_beat > 260) { a->beat = true; m->last_beat = a->ms; }
}

/* ---------------- drawing helpers ---------------- */
static void fmt_time(uint32_t ms, char *b, int cap) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, cap, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, cap, "%02u:%02u", s / 60, s % 60);
}
static void tri(canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t col) {
    raster_path_t *p = raster_path_new(); if (!p) return;
    raster_move_to(p, RASTER_FIX(x0), RASTER_FIX(y0)); raster_line_to(p, RASTER_FIX(x1), RASTER_FIX(y1));
    raster_line_to(p, RASTER_FIX(x2), RASTER_FIX(y2)); raster_close(p);
    raster_fill(c, p, col, false);
    raster_path_free(p);
}
static void bevel3d(canvas_t *c, rect_t r, bool raised) {
    uint32_t a = raised ? 0xFFFFFFFF : 0xFF808080, b = raised ? 0xFF404040 : 0xFFFFFFFF, a2 = raised ? 0xFFE0DCD4 : 0xFF404040, b2 = raised ? 0xFF808080 : 0xFFE0DCD4;
    hline(c, r.x, r.y, r.w, a); vline(c, r.x, r.y, r.h, a);
    hline(c, r.x, r.y + r.h - 1, r.w, b); vline(c, r.x + r.w - 1, r.y, r.h, b);
    hline(c, r.x + 1, r.y + 1, r.w - 2, a2); vline(c, r.x + 1, r.y + 1, r.h - 2, a2);
    hline(c, r.x + 1, r.y + r.h - 2, r.w - 2, b2); vline(c, r.x + r.w - 2, r.y + 1, r.h - 2, b2);
}
/* a glassy sphere (or, in Classic, a square 3D button); true when clicked */
static bool sphere(window_t *w, mp_t *m, int cx, int cy, int r, bool on) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    rect_t rr = R(cx - r, cy - r, r * 2, r * 2);
    bool hot = ui_hover(w, rr), held = hot && w->ui.down, click = ui_click_area(w, rr);
    if (s->classic) {
        fill_rect(c, rr.x, rr.y, rr.w, rr.h, on ? 0xFFE8E4DC : 0xFFD4D0C8);
        bevel3d(c, rr, !(held || on));
        return click;
    }
    uint32_t hi = s->bt_hi, lo = s->bt_lo;
    if (on) { hi = mix(hi, s->thumb, 90); lo = mix(lo, s->thumb, 140); }
    if (hot) { hi = lighten(hi, 20); lo = lighten(lo, 30); }
    if (held) { uint32_t t = hi; hi = lo; lo = t; }
    fill_circle(c, cx, cy + 1, r + 1, mix(s->bt_edge, 0xFF000000, 120));
    fill_circle(c, cx, cy, r, s->bt_edge);
    for (int yy = -r + 1; yy < r; yy++) {                     /* vertical shading inside the rim */
        int hw = 0; while ((hw + 1) * (hw + 1) + yy * yy < (r - 1) * (r - 1)) hw++;
        hline(c, cx - hw, cy + yy, hw * 2 + 1, mix(hi, lo, (yy + r) * 255 / (2 * r)));
    }
    for (int yy = -r + 2; yy < -r / 5; yy++) {                 /* the glint */
        int hw = 0; while ((hw + 1) * (hw + 1) * 4 / 9 + (yy + r / 2) * (yy + r / 2) * 2 < r * r / 2 && hw < r) hw++;
        if (hw > 1) blend_rect(c, cx - hw, cy + yy, hw * 2, 1, 0xFFFFFFFF, 120 - (yy + r) * 140 / r);
    }
    return click;
}
static uint32_t glyph_col(mp_t *m) { return SK->bt_glyph; }
enum { G_PLAY, G_PAUSE, G_STOP, G_PREV, G_NEXT, G_MUTE, G_SHUFFLE, G_REPEAT, G_FULL, G_REW, G_FFW, G_LEFT, G_RIGHT, G_VIZ, G_MENU, G_EQ };
static void glyph(canvas_t *c, int g, int cx, int cy, int s, uint32_t col) {
    switch (g) {
    case G_PLAY: tri(c, cx - s / 2 + 1, cy - s * 2 / 3, cx - s / 2 + 1, cy + s * 2 / 3, cx + s * 2 / 3 + 1, cy, col); break;
    case G_PAUSE: fill_rect(c, cx - s / 2, cy - s / 2, s / 3 + 1, s, col); fill_rect(c, cx + s / 6, cy - s / 2, s / 3 + 1, s, col); break;
    case G_STOP: fill_rect(c, cx - s / 2, cy - s / 2, s, s, col); break;
    case G_PREV: fill_rect(c, cx - s / 2, cy - s / 2, 2, s, col); tri(c, cx + s / 2, cy - s / 2, cx + s / 2, cy + s / 2, cx - s / 2 + 2, cy, col); break;
    case G_NEXT: tri(c, cx - s / 2, cy - s / 2, cx - s / 2, cy + s / 2, cx + s / 2 - 2, cy, col); fill_rect(c, cx + s / 2 - 2, cy - s / 2, 2, s, col); break;
    case G_REW: tri(c, cx, cy - s / 2, cx, cy + s / 2, cx - s / 2, cy, col); tri(c, cx + s / 2, cy - s / 2, cx + s / 2, cy + s / 2, cx, cy, col); break;
    case G_FFW: tri(c, cx - s / 2, cy - s / 2, cx - s / 2, cy + s / 2, cx, cy, col); tri(c, cx, cy - s / 2, cx, cy + s / 2, cx + s / 2, cy, col); break;
    case G_LEFT: tri(c, cx + s / 3, cy - s / 2, cx + s / 3, cy + s / 2, cx - s / 3, cy, col); break;
    case G_RIGHT: tri(c, cx - s / 3, cy - s / 2, cx - s / 3, cy + s / 2, cx + s / 3, cy, col); break;
    case G_MUTE:
        fill_rect(c, cx - s / 2, cy - s / 5, s / 3, s * 2 / 5 + 1, col);
        tri(c, cx - s / 5, cy - s / 5, cx - s / 5, cy + s / 5, cx + s / 6, cy - s / 2, col);
        tri(c, cx - s / 5, cy + s / 5, cx + s / 6, cy + s / 2, cx + s / 6, cy - s / 2, col);
        vline(c, cx + s / 3, cy - s / 4, s / 2, col); vline(c, cx + s / 2, cy - s / 3, s * 2 / 3, col);
        break;
    case G_SHUFFLE:
        draw_line(c, cx - s / 2, cy - s / 3, cx + s / 3, cy + s / 3, col); draw_line(c, cx - s / 2, cy + s / 3, cx + s / 3, cy - s / 3, col);
        tri(c, cx + s / 3, cy - s / 2, cx + s / 3, cy - s / 6, cx + s / 2 + 1, cy - s / 3, col);
        tri(c, cx + s / 3, cy + s / 6, cx + s / 3, cy + s / 2, cx + s / 2 + 1, cy + s / 3, col);
        break;
    case G_REPEAT:
        draw_rect(c, cx - s / 2, cy - s / 3, s, s * 2 / 3, col);
        tri(c, cx - 1, cy - s / 3 - 3, cx - 1, cy - s / 3 + 3, cx + 3, cy - s / 3, col);
        break;
    case G_FULL:
        draw_rect(c, cx - s / 2, cy - s / 3, s, s * 2 / 3 + 1, col); fill_rect(c, cx - s / 2 + 2, cy - s / 3 + 2, s - 4, s * 2 / 3 - 3, col);
        break;
    case G_VIZ: for (int i = 0; i < 4; i++) { int h = (i * 5 + 3) % 7 + 2; fill_rect(c, cx - s / 2 + i * (s / 4 + 1), cy + s / 2 - h, s / 4, h, col); } break;
    case G_MENU: tri(c, cx - s / 2, cy - s / 4, cx + s / 2, cy - s / 4, cx, cy + s / 3, col); break;
    case G_EQ: for (int i = 0; i < 3; i++) { vline(c, cx - s / 2 + i * s / 2, cy - s / 2, s, col); fill_rect(c, cx - s / 2 + i * s / 2 - 2, cy - s / 2 + (i * 7 + 2) % s, 5, 3, col); } break;
    }
}
/* a small square tool button (toolbar under the video) */
static bool tool_btn(window_t *w, mp_t *m, rect_t r, int g) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    bool hot = ui_hover(w, r), held = hot && w->ui.down, click = ui_click_area(w, r);
    if (s->classic) { fill_rect(c, r.x, r.y, r.w, r.h, 0xFFD4D0C8); if (hot) bevel3d(c, r, !held); glyph(c, g, r.x + r.w / 2, r.y + r.h / 2, 7, 0xFF000000); return click; }
    if (hot) { fill_round_rect(c, r.x, r.y, r.w, r.h, 3, held ? mix(s->bar_bot, 0xFF000000, 80) : lighten(s->bar_top, 40)); draw_round_rect(c, r.x, r.y, r.w, r.h, 3, lighten(s->bar_top, 90)); }
    glyph(c, g, r.x + r.w / 2, r.y + r.h / 2, 7, s->bar_text);
    return click;
}
/* a horizontal slider with a pill-shaped thumb; returns the new value while dragged */
static int slider(window_t *w, mp_t *m, rect_t r, int v, int max, bool *dragging, bool enabled) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    int cy = r.y + r.h / 2;
    bool hot = ui_hover(w, r);
    (void)ui_click_area(w, r);
    if (enabled && hot && w->ui.pressed) *dragging = true;
    if (*dragging) { v = (int)((int64_t)CLAMP(w->ui.mx - r.x, 0, r.w) * max / MAX(1, r.w)); if (!w->ui.down) *dragging = false; }
    int x = max ? r.x + (int)((int64_t)v * r.w / max) : r.x;
    if (s->classic) {
        rect_t g = R(r.x, cy - 2, r.w, 4); fill_rect(c, g.x, g.y, g.w, g.h, 0xFFFFFFFF); bevel3d(c, g, false);
        rect_t t = R(x - 5, cy - 9, 11, 18); fill_rect(c, t.x, t.y, t.w, t.h, 0xFFD4D0C8); bevel3d(c, t, true);
        return v;
    }
    fill_round_rect(c, r.x, cy - 3, r.w, 6, 3, mix(s->groove, 0xFF000000, 60));
    fill_round_rect(c, r.x + 1, cy - 2, r.w - 2, 4, 2, s->groove);
    if (x > r.x + 2) fill_round_rect(c, r.x + 1, cy - 2, x - r.x, 4, 2, mix(s->groove, s->thumb, 110));
    if (enabled) {
        uint32_t th = hot || *dragging ? lighten(s->thumb, 30) : s->thumb;
        fill_round_rect(c, x - 7, cy - 6, 14, 12, 6, mix(th, 0xFF000000, 110));
        gradient_round_rect(c, x - 6, cy - 5, 12, 10, 5, lighten(th, 70), th);
        blend_rect(c, x - 4, cy - 4, 8, 2, 0xFFFFFFFF, 110);
    }
    return v;
}

/* the album picture for the current item, made from its album and artist names */
static canvas_t *art_for(mp_t *m, const track_t *t) {
    char key[140]; snprintf(key, sizeof key, "%s|%s", t ? t->album : "", t ? t->artist : "");
    if (m->art && !strcmp(key, m->art_key)) return m->art;
    if (!m->art) m->art = canvas_new(96, 96);
    if (!m->art) return NULL;
    strlcpy(m->art_key, key, sizeof m->art_key);
    canvas_t *c = m->art;
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    int hue = h % 360, s = 96;
    uint32_t a = hsv_to_rgb(hue, 190, 235), b = hsv_to_rgb((hue + 50) % 360, 220, 80);
    gradient_v(c, 0, 0, s, s, a, b);
    if (t && t->video) {                                        /* a film strip */
        fill_rect(c, 0, 18, s, 60, 0xFF101010);
        for (int x = 4; x < s; x += 12) { fill_rect(c, x, 22, 7, 6, 0xFFE0E0E0); fill_rect(c, x, 68, 7, 6, 0xFFE0E0E0); }
        gradient_v(c, 6, 32, s - 12, 32, a, b);
    } else {
        for (int k = 0; k < 6; k++) {
            int r = 10 + (int)((h >> (k * 5)) % 30), cx = (int)((h >> (k * 3)) % s), cy = (int)((h >> (k * 4 + 2)) % s);
            draw_circle(c, cx, cy, r, mix(a, 0xFFFFFFFF, 150));
            draw_circle(c, cx, cy, r + 1, mix(a, 0xFFFFFFFF, 200));
        }
        int nx = 40, ny = 62;                                   /* a note */
        fill_circle(c, nx, ny, 8, 0xE0FFFFFF);
        fill_rect(c, nx + 6, ny - 34, 3, 34, 0xE0FFFFFF);
        fill_rect(c, nx + 6, ny - 34, 16, 4, 0xE0FFFFFF);
    }
    blend_rect(c, 0, 0, s, s / 2, 0xFFFFFFFF, 30);
    return c;
}

/* ---------------- layout ---------------- */
#define FR_TOP 30                                                /* skinned frame: title band */
#define FR_SIDE 7
#define FR_BOT 66                                                /* seek bar and transport */
#define TB_W 86                                                  /* feature taskbar */
#define BAR_H 20                                                 /* toolbar under the video */
#define PANE_W 176
typedef struct { rect_t body, tb, content, title; int bottom_y; } layout_t;
static layout_t layout(window_t *w, mp_t *m) {
    canvas_t *c = w->surf;
    layout_t L;
    bool classic = SK->classic;
    int top = classic ? 4 : FR_TOP, side = classic ? 4 : FR_SIDE, bot = classic ? 62 : FR_BOT;
    L.title = R(0, 0, c->w, classic ? 0 : FR_TOP);
    L.body = R(side, top, c->w - 2 * side, c->h - top - bot);
    int tbw = m->taskbar_hidden ? 0 : TB_W;
    L.tb = R(L.body.x, L.body.y, tbw, L.body.h);
    L.content = R(L.body.x + tbw + (tbw ? 6 : 0), L.body.y, L.body.w - tbw - (tbw ? 6 : 0), L.body.h);
    L.bottom_y = c->h - bot;
    return L;
}

/* ---------------- frame, title band, taskbar ---------------- */
static int corner_in(int j, int rad) { if (j >= rad) return 0; int dy = rad - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++; return rad - dx; }
static void draw_frame(window_t *w, mp_t *m) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    if (s->classic) { gfx_clear(c, 0xFFD4D0C8); return; }
    gfx_clear(c, 0x00000000);                                    /* see-through outside the rounded frame */
    int rad = w->state == WS_MAXIMIZED ? 0 : 12;
    for (int y = 0; y < c->h; y++) {
        int in = MAX(corner_in(y, rad), corner_in(c->h - 1 - y, rad));
        uint32_t col = y < FR_TOP ? mix(s->fr_hi, s->fr_top, y * 255 / FR_TOP) : mix(s->fr_top, s->fr_bot, (y - FR_TOP) * 255 / MAX(1, c->h - FR_TOP));
        if (y >= c->h - FR_BOT) col = mix(s->fr_top, s->fr_bot, (y - (c->h - FR_BOT)) * 255 / FR_BOT);
        hline(c, in, y, c->w - 2 * in, col);
        put_pixel(c, in, y, s->fr_edge); put_pixel(c, c->w - 1 - in, y, s->fr_edge);
    }
    hline(c, rad, 0, c->w - 2 * rad, s->fr_edge); hline(c, rad, c->h - 1, c->w - 2 * rad, s->fr_edge);
    blend_rect(c, rad, 1, c->w - 2 * rad, 1, 0xFFFFFFFF, 150);
    /* a gloss over the top band */
    for (int y = 2; y < FR_TOP / 2; y++) blend_rect(c, corner_in(y, rad) + 1, y, c->w - 2 * corner_in(y, rad) - 2, 1, 0xFFFFFFFF, 60 - y * 3);
    /* the bottom band's lighter strip behind the controls */
    int by = c->h - FR_BOT + 4;
    fill_round_rect(c, 8, by, c->w - 16, FR_BOT - 12, 10, mix(s->fr_top, 0xFFFFFFFF, 70));
    fill_round_rect_blend(c, 9, by + 1, c->w - 18, (FR_BOT - 12) / 2, 9, 0xFFFFFFFF, 60);
    draw_round_rect(c, 8, by, c->w - 16, FR_BOT - 12, 10, mix(s->fr_bot, 0xFF000000, 60));
}
static void draw_title(window_t *w, mp_t *m, layout_t *L) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    if (s->classic) return;
    draw_icon(c, ICON_MEDIA_PLAYER, 16, 12, 7);
    draw_text(c, FONT_UI_BOLD, 33, 9, APP_NAME, mix(s->fr_edge, 0xFF000000, 60));
    draw_text(c, FONT_UI_BOLD, 32, 8, APP_NAME, 0xFFFFFFFF);
    /* caption buttons */
    static const char *cap = "_\x01x";
    for (int i = 0; i < 3; i++) {
        rect_t r = R(c->w - 26 - (2 - i) * 22, 6, 20, 17);
        bool hot = ui_hover(w, r);
        if (hot) { fill_round_rect(c, r.x, r.y, r.w, r.h, 4, i == 2 ? 0xFFD8402C : mix(s->fr_top, 0xFFFFFFFF, 120)); draw_round_rect(c, r.x, r.y, r.w, r.h, 4, 0xFFFFFFFF); }
        uint32_t g = hot ? 0xFFFFFFFF : mix(s->fr_edge, 0xFF000000, 30);
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (i == 0) fill_rect(c, cx - 4, cy + 3, 8, 2, g);
        else if (i == 1) { draw_rect(c, cx - 4, cy - 4, 9, 8, g); hline(c, cx - 4, cy - 3, 9, g); }
        else for (int k = 0; k < 7; k++) { put_pixel(c, cx - 3 + k, cy - 3 + k, g); put_pixel(c, cx - 2 + k, cy - 3 + k, g); put_pixel(c, cx + 3 - k, cy - 3 + k, g); put_pixel(c, cx + 4 - k, cy - 3 + k, g); }
        if (ui_click_area(w, r)) {
            if (i == 0) wm_minimize(w);
            else if (i == 1) wm_maximize(w, w->state != WS_MAXIMIZED);
            else wm_close(w);
        }
        (void)cap;
    }
    /* the playlist selector */
    int dw = 168;
    rect_t d = R(c->w - 26 - 2 * 22 - dw - 12, 6, dw, 18);
    fill_round_rect(c, d.x, d.y, d.w, d.h, 3, 0xFFFFFFFF);
    draw_round_rect(c, d.x, d.y, d.w, d.h, 3, mix(s->fr_edge, 0xFF000000, 30));
    draw_icon(c, ICON_MUSIC_FILE, 16, d.x + 2, d.y + 1);
    draw_text_ellipsis(c, FONT_UI, d.x + 22, d.y + 2, d.w - 40, m->page == P_LIB ? "Media Library" : "Now Playing", 0xFF000000);
    glyph(c, G_MENU, d.x + d.w - 10, d.y + d.h / 2, 8, 0xFF203060);
    if (ui_click_area(w, d)) {
        static const menu_item_t items[] = { { "Now Playing", 1, 0 }, { "-", 0, 0 }, { "All Music", 2, 0 }, { "All Video", 3, 0 }, { "My Music folder", 4, 0 }, { "-", 0, 0 }, { "Clear the list", 5, 0 } };
        rect_t cr = wm_client_rect(w);
        extern void mp_playlist_menu(void *ctx, int id);
        popup_menu(cr.x + d.x, cr.y + d.y + d.h, items, ARRAY_LEN(items), mp_playlist_menu, m);
    }
    (void)L;
}
static void draw_taskbar(window_t *w, mp_t *m, layout_t *L) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    rect_t r = L->tb;
    if (r.w <= 0) {                                              /* hidden: just the grip to show it again */
        rect_t g = R(L->body.x, L->body.y + L->body.h / 2 - 20, 6, 40);
        fill_round_rect(c, g.x, g.y, g.w, g.h, 2, s->classic ? 0xFFD4D0C8 : s->tb_bot);
        glyph(c, G_RIGHT, g.x + 3, g.y + 20, 6, s->tb_text);
        if (ui_click_area(w, g)) m->taskbar_hidden = false;
        return;
    }
    if (s->classic) { fill_rect(c, r.x, r.y, r.w, r.h, 0xFFD4D0C8); bevel3d(c, r, false); }
    else {
        fill_round_rect(c, r.x, r.y, r.w, r.h, 8, mix(s->tb_bot, 0xFF000000, 50));
        gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 7, s->tb_top, s->tb_bot);
    }
    int y = r.y + 4;
    for (int i = 0; i < P_COUNT; i++) {
        /* one or two lines, like "Copy to CD / or Device" */
        char l1[32], l2[32]; l2[0] = 0;
        strlcpy(l1, feature_names[i], sizeof l1);
        if (text_width(FONT_UI_BOLD, l1) > r.w - 18) {
            char *sp = NULL;
            for (char *q = l1; *q; q++) if (*q == ' ' && text_width_n(FONT_UI_BOLD, l1, (int)(q - l1)) <= r.w - 18) sp = q;
            if (sp) { strlcpy(l2, sp + 1, sizeof l2); *sp = 0; }
        }
        int h = l2[0] ? 34 : 24;
        rect_t b = R(r.x + 3, y, r.w - 6, h);
        bool on = m->page == i, hot = ui_hover(w, b);
        if (s->classic) {
            if (on) fill_rect(c, b.x, b.y, b.w, b.h, s->tb_sel);
            else if (hot) bevel3d(c, b, true);
        } else {
            if (on) { fill_round_rect(c, b.x, b.y, b.w, b.h, 4, s->tb_sel); draw_round_rect(c, b.x, b.y, b.w, b.h, 4, s->tb_line); }
            else if (hot) fill_round_rect(c, b.x, b.y, b.w, b.h, 4, s->tb_hot);
        }
        uint32_t tc = s->classic && on ? 0xFFFFFFFF : s->tb_text;
        draw_text(c, FONT_UI_BOLD, b.x + 4, b.y + 4, l1, tc);
        if (l2[0]) draw_text(c, FONT_UI_BOLD, b.x + 4, b.y + 18, l2, tc);
        if (on) glyph(c, G_RIGHT, b.x + b.w - 6, b.y + b.h / 2, 7, tc);
        if (ui_click_area(w, b)) { m->page = i; w->dirty = true; if (i == P_LIB && !m->scanned) library_scan(m); if (i == P_SKIN) m->skin_sel = m->skin; }
        y += h + 1;
        if (!s->classic && i < P_COUNT - 1) hline(c, b.x + 4, y - 1, b.w - 8, s->tb_line);
    }
    /* the logo at the bottom */
    if (r.y + r.h - 50 > y) draw_icon(c, ICON_LOGO, 32, r.x + (r.w - 32) / 2, r.y + r.h - 44);
    /* the grip that hides the taskbar */
    rect_t g = R(r.x + r.w, r.y + r.h / 2 - 20, 6, 40);
    fill_round_rect(c, g.x, g.y, g.w, g.h, 2, s->classic ? 0xFFD4D0C8 : s->tb_bot);
    glyph(c, G_LEFT, g.x + 3, g.y + 20, 6, s->tb_text);
    if (ui_click_area(w, g)) m->taskbar_hidden = true;
}

/* ---------------- Now Playing ---------------- */
static void viz_menu_cb(void *ctx, int id) {
    mp_t *m = ctx;
    if (id <= 0) return;
    m->viz_c = id / 100; m->viz_p = id % 100;
    viz_reset();
    char b[16]; snprintf(b, sizeof b, "%d,%d", m->viz_c, m->viz_p); settings_set("mp_viz", b); settings_save();
}
static void open_viz_menu(window_t *w, mp_t *m, int x, int y) {
    static menu_item_t items[64]; static char labels[64][48];
    int n = 0;
    for (int c = 0; c < viz_collections() && n < 60; c++) {
        if (c) items[n++] = (menu_item_t){ "-", 0, 0 };
        for (int p = 0; p < viz_presets(c) && n < 63; p++) {
            snprintf(labels[n], sizeof labels[n], "%s: %s", viz_collection(c), viz_preset(c, p));
            items[n] = (menu_item_t){ labels[n], c * 100 + p, (c == m->viz_c && p == m->viz_p) ? MI_CHECKED : 0 };
            n++;
        }
    }
    rect_t cr = wm_client_rect(w);
    popup_menu(cr.x + x, cr.y + y, items, n, viz_menu_cb, m);
}
static void step_viz(mp_t *m, int dir) {
    m->viz_p += dir;
    if (m->viz_p >= viz_presets(m->viz_c)) { m->viz_c = (m->viz_c + 1) % viz_collections(); m->viz_p = 0; }
    if (m->viz_p < 0) { m->viz_c = (m->viz_c + viz_collections() - 1) % viz_collections(); m->viz_p = viz_presets(m->viz_c) - 1; }
    viz_reset();
    char b[16]; snprintf(b, sizeof b, "%d,%d", m->viz_c, m->viz_p); settings_set("mp_viz", b);
}
static void open_full(mp_t *m);
/* the picture: video when the item has one, else the visualisation */
static void draw_picture(canvas_t *c, mp_t *m, rect_t r) {
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    if (m->vframe && m->vid_track == m->cur && m->state != ST_STOP) {
        fill_rect(c, r.x, r.y, r.w, r.h, 0xFF000000);
        int vw = m->vframe->w, vh = m->vframe->h;
        int dw = r.w, dh = vh * r.w / MAX(1, vw);
        if (dh > r.h) { dh = r.h; dw = vw * r.h / MAX(1, vh); }
        rect_t old = canvas_push_clip(c, r);
        blit_scaled_alpha(c, R(r.x + (r.w - dw) / 2, r.y + (r.h - dh) / 2, dw, dh), m->vframe, 255);
        c->clip = old;
        return;
    }
    /* The visualizer steps once per player tick, not once per paint: moving the mouse over the
     * window repaints it, and a step per paint made the picture race and jump with the cursor
     * (and move after Stop). A paint between ticks shows the last frame again. */
    int slot = mp_win && c == mp_win->surf ? 0 : 1;
    if (r.w <= 0 || r.h <= 0) return;
    __typeof__(m->vcache[0]) *vc = &m->vcache[slot];
    if (!vc->cv || vc->cv->w != r.w || vc->cv->h != r.h) {
        if (vc->cv) canvas_free(vc->cv);
        vc->cv = canvas_new(r.w, r.h);
        vc->gen = m->viz_gen - 1;
        if (!vc->cv) { viz_draw(c, r, m->viz_c, m->viz_p, &m->au, art_for(m, t)); return; }
    }
    if (vc->gen != m->viz_gen || vc->c != m->viz_c || vc->p != m->viz_p) {
        viz_draw(vc->cv, R(0, 0, r.w, r.h), m->viz_c, m->viz_p, &m->au, art_for(m, t));
        vc->gen = m->viz_gen; vc->c = m->viz_c; vc->p = m->viz_p;
    }
    blit(c, r.x, r.y, vc->cv, 0, 0, r.w, r.h);
}
static void draw_now(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    int pw = a.w >= 520 ? PANE_W : 0;
    rect_t vr = R(a.x, a.y, a.w - pw - (pw ? 2 : 0), a.h - BAR_H);
    draw_picture(c, m, vr);
    if (s->classic) bevel3d(c, R(vr.x - 2, vr.y - 2, vr.w + 4, vr.h + 4), false);
    if (m->state == ST_LOAD) {
        const char *msg = "Opening media...";
        int tw = text_width(FONT_UI_BOLD, msg) + 30;
        fill_round_rect_blend(c, vr.x + (vr.w - tw) / 2, vr.y + vr.h / 2 - 14, tw, 28, 14, 0xFF000000, 170);
        draw_text_centered(c, FONT_UI_BOLD, R(vr.x + (vr.w - tw) / 2, vr.y + vr.h / 2 - 14, tw, 28), msg, 0xFFFFFFFF);
    }
    if (ui_hover(w, vr) && w->ui.dblclick) open_full(m);
    /* the toolbar under the picture */
    rect_t br = R(a.x, a.y + a.h - BAR_H, vr.w, BAR_H);
    if (s->classic) { fill_rect(c, br.x, br.y, br.w, br.h, 0xFFD4D0C8); bevel3d(c, br, true); }
    else { gradient_v(c, br.x, br.y, br.w, br.h, s->bar_top, s->bar_bot); hline(c, br.x, br.y, br.w, lighten(s->bar_top, 60)); }
    int x = br.x + 3;
    bool video = m->vframe && m->vid_track == m->cur && m->state != ST_STOP;
    if (tool_btn(w, m, R(x, br.y + 2, 18, 16), G_VIZ)) open_viz_menu(w, m, x, br.y + 18);
    x += 20;
    if (tool_btn(w, m, R(x, br.y + 2, 16, 16), G_LEFT)) step_viz(m, -1);
    x += 17;
    if (tool_btn(w, m, R(x, br.y + 2, 16, 16), G_RIGHT)) step_viz(m, 1);
    x += 20;
    char label[96];
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    if (video) snprintf(label, sizeof label, "Video: %s", t ? t->format : "");
    else if (m->viz_c == 1 && m->viz_p == 0 && viz_current_name()[0]) snprintf(label, sizeof label, "%s : %s (%s)", viz_collection(m->viz_c), viz_preset(m->viz_c, m->viz_p), viz_current_name());
    else snprintf(label, sizeof label, "%s : %s", viz_collection(m->viz_c), viz_preset(m->viz_c, m->viz_p));
    draw_text_ellipsis(c, FONT_SMALL, x, br.y + 4, br.x + br.w - 48 - x, label, s->classic ? 0xFF000000 : s->bar_text);
    if (tool_btn(w, m, R(br.x + br.w - 40, br.y + 2, 18, 16), G_FULL)) open_full(m);
    if (tool_btn(w, m, R(br.x + br.w - 20, br.y + 2, 18, 16), G_MENU)) open_viz_menu(w, m, br.x + br.w - 200, br.y + 18);
    if (!pw) return;
    /* the playlist pane: picture and title, then the list */
    rect_t p = R(a.x + a.w - pw, a.y, pw, a.h);
    if (s->classic) { fill_rect(c, p.x, p.y, p.w, p.h, 0xFFFFFFFF); bevel3d(c, p, false); }
    else gradient_v(c, p.x, p.y, p.w, p.h, s->pn_top, s->pn_bot);
    int ah = 112;
    if (!s->classic) fill_rect(c, p.x, p.y, p.w, ah, s->pn_art);
    canvas_t *art = art_for(m, t);
    if (art) {
        int asz = 84;
        rect_t ar = R(p.x + (p.w - asz) / 2, p.y + 6, asz, asz);
        fill_round_rect(c, ar.x - 2, ar.y - 2, ar.w + 4, ar.h + 4, 4, s->classic ? 0xFF808080 : s->pn_line);
        blit_scaled_alpha(c, ar, art, 255);
    }
    draw_text_ellipsis(c, FONT_SMALL, p.x + 6, p.y + ah - 18, p.w - 12, t ? t->album : "", s->pn_dim);
    hline(c, p.x, p.y + ah, p.w, s->pn_line);
    int y = p.y + ah + 2, rowh = 18, rows = (p.y + p.h - y - 20) / rowh;
    if (ui_hover(w, R(p.x, y, p.w, rows * rowh)) && w->ui.wheel) m->list_scroll += w->ui.wheel * 3;
    m->list_scroll = CLAMP(m->list_scroll, 0, MAX(0, m->nlist - rows));
    int lsb = m->nlist > rows ? 16 : 0;                     /* a scrollbar when the list is longer than the pane */
    if (lsb) ui_vscroll(w, R(p.x + p.w - lsb - 1, y, lsb, rows * rowh), &m->list_scroll, m->nlist, rows);
    for (int r = 0; r < rows && m->list_scroll + r < m->nlist; r++) {
        int i = m->list_scroll + r, ry = y + r * rowh;
        rect_t rr = R(p.x + 2, ry, p.w - 4 - lsb, rowh);
        bool cur = i == m->cur && m->state != ST_STOP;
        if (i == m->list_sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pn_sel);
        char len[16]; fmt_time(m->list[i].ms, len, sizeof len);
        int lw = text_width(FONT_SMALL, len);
        uint32_t col = s->classic && i == m->list_sel ? 0xFFFFFFFF : cur ? (s->classic ? 0xFF0000C0 : s->lcd_text) : s->pn_text;
        draw_text_ellipsis(c, cur ? FONT_UI_BOLD : FONT_SMALL, rr.x + 4, ry + 2, rr.w - 12 - lw, m->list[i].title, col);
        draw_text(c, FONT_SMALL, rr.x + rr.w - 4 - lw, ry + 3, len, s->classic && i == m->list_sel ? 0xFFFFFFFF : s->pn_dim);
        if (ui_click_area(w, rr)) {
            m->list_sel = i;
            uint32_t now = timer_ticks();
            if (m->last_click_where == 2 && m->last_click_row == i && now - m->last_click_ms < wm_dblclick_ms) { play_index(m, i); m->last_click_ms = 0; }
            else { m->last_click_where = 2; m->last_click_row = i; m->last_click_ms = now; }
        }
    }
    if (!m->nlist) draw_text_wrapped(c, FONT_SMALL, p.x + 6, y + 6, p.w - 12, "The playlist is empty. Pick music in the Media Library.", s->pn_dim, 2);
    uint32_t total = 0; for (int i = 0; i < m->nlist; i++) total += m->list[i].ms;
    char b[48], tt[16]; fmt_time(total, tt, sizeof tt);
    snprintf(b, sizeof b, "Total Time: %s", tt);
    draw_text(c, FONT_SMALL, p.x + 6, p.y + p.h - 16, b, s->pn_dim);
}

/* ---------------- pages ---------------- */
static void page_head(window_t *w, mp_t *m, rect_t a, const char *title, const char *sub) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    fill_rect(c, a.x, a.y, a.w, a.h, s->pg_bg);
    if (s->classic) { bevel3d(c, a, false); fill_rect(c, a.x + 2, a.y + 2, a.w - 4, 34, 0xFF0A246A); }
    else gradient_v(c, a.x, a.y, a.w, 36, s->pg_head, mix(s->pg_head, 0xFF000000, 60));
    draw_text(c, FONT_LARGE_BOLD, a.x + 12, a.y + 9, title, s->classic ? 0xFFFFFFFF : s->pg_head_text);
    if (sub) draw_text_ellipsis(c, FONT_SMALL, a.x + 14 + text_width(FONT_LARGE_BOLD, title) + 10, a.y + 14, a.w - 40 - text_width(FONT_LARGE_BOLD, title), sub, s->classic ? 0xFFD0D0FF : mix(s->pg_head_text, s->pg_head, 90));
}
static bool page_button(window_t *w, mp_t *m, rect_t r, const char *label, bool enabled) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    if (s->classic) return ui_button_ex(w, r, label, enabled, false);
    bool hot = enabled && ui_hover(w, r), click = enabled && ui_click_area(w, r);
    gradient_round_rect(c, r.x, r.y, r.w, r.h, 9, enabled ? (hot ? lighten(s->pg_head, 50) : lighten(s->pg_head, 20)) : 0xFFC8C8C8, enabled ? mix(s->pg_head, 0xFF000000, 40) : 0xFFA0A0A0);
    fill_round_rect_blend(c, r.x + 1, r.y + 1, r.w - 2, r.h / 2, 8, 0xFFFFFFFF, 50);
    draw_round_rect(c, r.x, r.y, r.w, r.h, 9, mix(s->pg_head, 0xFF000000, 90));
    draw_text_centered(c, FONT_UI_BOLD, r, label, 0xFFFFFFFF);
    return click;
}
static bool has_ci(const char *h, const char *q) { size_t n = strlen(q); for (; *h; h++) if (!strncasecmp(h, q, n)) return true; return false; }
static bool lib_match(mp_t *m, const track_t *t) {
    switch (m->lib_view) {
    case 1: if (t->video) return false; break;                    /* All Music */
    case 2: if (!t->video) return false; break;                   /* All Video */
    case 3: if (strcasecmp(t->artist, m->lib_filter)) return false; break;
    case 4: if (strcasecmp(t->album, m->lib_filter)) return false; break;
    }
    if (m->search[0] && !has_ci(t->title, m->search) && !has_ci(t->artist, m->search) && !has_ci(t->album, m->search)) return false;
    return true;
}
static void draw_library(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, "Media Library", NULL);
    /* tool row */
    int y = a.y + 42;
    if (page_button(w, m, R(a.x + 10, y, 74, 22), "Play", m->nlib > 0)) {
        m->nlist = 0;
        for (int i = 0; i < m->nlib; i++) if (lib_match(m, &m->lib[i])) add_track(m, &m->lib[i]);
        int at = 0; if (m->lib_sel >= 0) for (int i = 0; i < m->nlist; i++) if (!strcmp(m->list[i].path, m->lib[m->lib_sel].path)) at = i;
        if (m->nlist) { play_index(m, at); m->page = P_NOW; }
    }
    if (page_button(w, m, R(a.x + 90, y, 140, 22), "Add to Now Playing", m->lib_sel >= 0 && m->lib_sel < m->nlib)) {
        add_track(m, &m->lib[m->lib_sel]); snprintf(m->status, sizeof m->status, "Added %s to Now Playing.", m->lib[m->lib_sel].title);
    }
    if (page_button(w, m, R(a.x + 236, y, 90, 22), "Search...", true)) library_scan(m);
    int sw = MIN(190, a.x + a.w - (a.x + 340) - 10);
    if (sw > 70) {
        rect_t sb = R(a.x + a.w - sw - 10, y, sw, 22);
        fill_rect(c, sb.x, sb.y, sb.w, sb.h, 0xFFFFFFFF); draw_rect(c, sb.x, sb.y, sb.w, sb.h, s->classic ? 0xFF808080 : s->pg_head);
        ui_textbox_ex(w, sb, m->search, sizeof m->search, true, 0xFF000000, NULL);
        if (!m->search[0]) draw_text(c, FONT_UI, sb.x + 5, sb.y + 4, "Find in library", 0xFF909090);
    }
    y += 30;
    /* the tree on the left */
    int tw = MIN(170, a.w / 3);
    rect_t tr = R(a.x + 8, y, tw, a.y + a.h - y - 8);
    fill_rect(c, tr.x, tr.y, tr.w, tr.h, 0xFFFFFFFF); draw_rect(c, tr.x, tr.y, tr.w, tr.h, s->classic ? 0xFF808080 : s->pg_sel);
    struct { const char *label; int view, depth; const char *filter; } nodes[64]; int nn = 0;
    nodes[nn++] = (typeof(nodes[0])){ "All Media", 0, 0, NULL };
    nodes[nn++] = (typeof(nodes[0])){ "All Music", 1, 1, NULL };
    nodes[nn++] = (typeof(nodes[0])){ "Artist", -1, 1, NULL };
    for (int i = 0; i < m->nlib && nn < 40; i++) {               /* each artist once */
        if (m->lib[i].video) continue;
        bool seen = false; for (int k = 0; k < nn; k++) if (nodes[k].view == 3 && !strcasecmp(nodes[k].filter, m->lib[i].artist)) seen = true;
        if (!seen) nodes[nn++] = (typeof(nodes[0])){ m->lib[i].artist, 3, 2, m->lib[i].artist };
    }
    nodes[nn++] = (typeof(nodes[0])){ "Album", -1, 1, NULL };
    for (int i = 0; i < m->nlib && nn < 60; i++) {
        if (m->lib[i].video) continue;
        bool seen = false; for (int k = 0; k < nn; k++) if (nodes[k].view == 4 && !strcasecmp(nodes[k].filter, m->lib[i].album)) seen = true;
        if (!seen) nodes[nn++] = (typeof(nodes[0])){ m->lib[i].album, 4, 2, m->lib[i].album };
    }
    if (nn < 63) nodes[nn++] = (typeof(nodes[0])){ "All Video", 2, 1, NULL };
    rect_t old = canvas_push_clip(c, tr);
    for (int k = 0; k < nn; k++) {
        rect_t rr = R(tr.x + 1, tr.y + 2 + k * 18, tr.w - 2, 18);
        if (rr.y + rr.h > tr.y + tr.h) break;
        bool sel = nodes[k].view == m->lib_view && (nodes[k].view < 3 || !strcasecmp(nodes[k].filter, m->lib_filter));
        if (sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pg_sel);
        int ix = rr.x + 4 + nodes[k].depth * 12;
        draw_icon(c, nodes[k].view == 2 ? ICON_MEDIA_PLAYER : nodes[k].depth < 2 ? ICON_FOLDER : ICON_MUSIC_FILE, 16, ix, rr.y + 1);
        draw_text_ellipsis(c, nodes[k].depth == 1 ? FONT_UI_BOLD : FONT_UI, ix + 20, rr.y + 2, rr.x + rr.w - ix - 22, nodes[k].label, s->classic && sel ? 0xFFFFFFFF : s->pg_text);
        if (nodes[k].view >= 0 && ui_click_area(w, rr)) { m->lib_view = nodes[k].view; strlcpy(m->lib_filter, nodes[k].filter ? nodes[k].filter : "", sizeof m->lib_filter); m->lib_scroll = 0; }
    }
    c->clip = old;
    /* the table */
    rect_t t = R(tr.x + tr.w + 6, y, a.x + a.w - (tr.x + tr.w + 6) - 8, tr.h);
    fill_rect(c, t.x, t.y, t.w, t.h, 0xFFFFFFFF); draw_rect(c, t.x, t.y, t.w, t.h, s->classic ? 0xFF808080 : s->pg_sel);
    int cx[5] = { t.x + 6, t.x + t.w * 38 / 100, t.x + t.w * 60 / 100, t.x + t.w * 80 / 100, t.x + t.w * 89 / 100 };
    static const char *heads[5] = { "Title", "Artist", "Album", "Length", "Type" };
    if (s->classic) { fill_rect(c, t.x + 1, t.y + 1, t.w - 2, 20, 0xFFD4D0C8); bevel3d(c, R(t.x + 1, t.y + 1, t.w - 2, 20), true); }
    else gradient_v(c, t.x + 1, t.y + 1, t.w - 2, 20, 0xFFFFFFFF, s->pg_row);
    for (int i = 0; i < 5; i++) { draw_text(c, FONT_UI, cx[i], t.y + 4, heads[i], s->pg_text); if (i) vline(c, cx[i] - 5, t.y + 3, 16, s->pg_sel); }
    int rowh = 18, ty = t.y + 22, rows = (t.y + t.h - ty - 18) / rowh;
    int idx[MAXT], n = 0;
    for (int i = 0; i < m->nlib; i++) if (lib_match(m, &m->lib[i])) idx[n++] = i;
    if (ui_hover(w, R(t.x, ty, t.w, rows * rowh)) && w->ui.wheel) m->lib_scroll += w->ui.wheel * 3;
    m->lib_scroll = CLAMP(m->lib_scroll, 0, MAX(0, n - rows));
    int lsb = n > rows ? 17 : 0;
    if (lsb) ui_vscroll(w, R(t.x + t.w - lsb - 1, ty, lsb, rows * rowh), &m->lib_scroll, n, rows);
    old = canvas_push_clip(c, R(t.x + 1, ty, t.w - 2 - lsb, rows * rowh));
    for (int r = 0; r < rows && m->lib_scroll + r < n; r++) {
        int i = idx[m->lib_scroll + r], ry = ty + r * rowh;
        const track_t *tk = &m->lib[i];
        rect_t rr = R(t.x + 1, ry, t.w - 2 - lsb, rowh);
        bool sel = m->lib_sel == i;
        if (sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pg_sel);
        else if (r & 1) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pg_row);
        uint32_t col = s->classic && sel ? 0xFFFFFFFF : s->pg_text;
        char len[16]; fmt_time(tk->ms, len, sizeof len);
        draw_text_ellipsis(c, FONT_UI, cx[0], ry + 2, cx[1] - cx[0] - 8, tk->title, col);
        draw_text_ellipsis(c, FONT_UI, cx[1], ry + 2, cx[2] - cx[1] - 8, tk->artist, col);
        draw_text_ellipsis(c, FONT_UI, cx[2], ry + 2, cx[3] - cx[2] - 8, tk->album, col);
        draw_text(c, FONT_UI, cx[3], ry + 2, len, col);
        draw_text_ellipsis(c, FONT_SMALL, cx[4], ry + 3, t.x + t.w - cx[4] - 4, tk->format, col);
        if (ui_click_area(w, rr)) {
            m->lib_sel = i;
            uint32_t now = timer_ticks();
            if (m->last_click_where == 1 && m->last_click_row == i && now - m->last_click_ms < wm_dblclick_ms) {
                m->nlist = 0; for (int q = 0; q < n; q++) add_track(m, &m->lib[idx[q]]);
                int at = 0; for (int q = 0; q < n; q++) if (idx[q] == i) at = q;
                play_index(m, at); m->last_click_ms = 0;
            } else { m->last_click_where = 1; m->last_click_row = i; m->last_click_ms = now; }
        }
    }
    c->clip = old;
    if (!n) draw_text(c, FONT_UI, t.x + 8, ty + 6, m->nlib ? "Nothing here matches." : "No media found. Copy music or videos to My Music, then click Search.", 0xFF808080);
    uint32_t total = 0; for (int q = 0; q < n; q++) total += m->lib[idx[q]].ms;
    char b[120], tt[16]; fmt_time(total, tt, sizeof tt);
    snprintf(b, sizeof b, "%d item%s, total time %s. Double-click an item to play it.", n, n == 1 ? "" : "s", tt);
    draw_text(c, FONT_SMALL, t.x + 6, t.y + t.h - 15, b, 0xFF707070);
}
/* a heading/link colour that reads on the page background, light or dark */
static uint32_t page_accent(const skin_t *s) {
    if (s->classic) return 0xFF0A246A;
    int l = (int)((s->pg_bg >> 16 & 255) * 3 + (s->pg_bg >> 8 & 255) * 6 + (s->pg_bg & 255)) / 10;
    return l < 128 ? s->pg_head_text : s->pg_head;
}
static void draw_guide(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, "Media Guide", "On this computer");
    if (!m->scanned) library_scan(m);
    int music = 0, video = 0; uint32_t ms = 0;
    for (int i = 0; i < m->nlib; i++) { if (m->lib[i].video) video++; else music++; ms += m->lib[i].ms; }
    int y = a.y + 48, x = a.x + 14, cw = a.w - 28;
    char b[160], tt[16]; fmt_time(ms, tt, sizeof tt);
    snprintf(b, sizeof b, "Your library holds %d song%s and %d video%s, %s in all.", music, music == 1 ? "" : "s", video, video == 1 ? "" : "s", tt);
    draw_text(c, FONT_UI_BOLD, x, y, b, s->pg_text); y += 24;
    draw_text(c, FONT_UI_BOLD, x, y, "Featured on this computer", page_accent(s)); y += 20;
    for (int i = 0, shown = 0; i < m->nlib && shown < 6; i++, shown++) {
        rect_t r = R(x, y, cw, 40);
        bool hot = ui_hover(w, r);
        fill_round_rect(c, r.x, r.y, r.w, r.h, 4, hot ? s->pg_sel : s->pg_row);
        canvas_t *art = art_for(m, &m->lib[i]);
        if (art) blit_scaled_alpha(c, R(r.x + 4, r.y + 4, 32, 32), art, 255);
        draw_text_ellipsis(c, FONT_UI_BOLD, r.x + 44, r.y + 5, r.w - 120, m->lib[i].title, s->pg_text);
        snprintf(b, sizeof b, "%s - %s", m->lib[i].artist, m->lib[i].format);
        draw_text_ellipsis(c, FONT_SMALL, r.x + 44, r.y + 22, r.w - 120, b, mix(s->pg_text, s->pg_bg, 100));
        draw_text(c, FONT_UI, r.x + r.w - 60, r.y + 12, "Play", page_accent(s));
        if (ui_click_area(w, r)) { m->nlist = 0; add_track(m, &m->lib[i]); play_index(m, 0); m->page = P_NOW; }
        y += 44;
        art_for(m, m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL);   /* keep the pane's picture */
    }
    y += 8;
    draw_text_wrapped(c, FONT_SMALL, x, y, cw, "The online media guide service of the XP era no longer exists, so the guide shows what is on this computer. "
        "Put music and videos in My Music; the player opens MP3, MP2, Ogg Vorbis, FLAC, WAV, AIFF, .au and MPEG-1 video files.", mix(s->pg_text, s->pg_bg, 80), 2);
}
static void draw_info_page(window_t *w, mp_t *m, rect_t a, const char *title, const char *sub, const char *text) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, title, sub);
    draw_text_wrapped(c, FONT_UI, a.x + 16, a.y + 52, a.w - 32, text, s->pg_text, 3);
}
static void draw_rip(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, "Copy from CD", NULL);
    int y = a.y + 50;
    draw_text(c, FONT_UI_BOLD, a.x + 16, y, "Drives", s->pg_text); y += 20;
    int found = 0;
    for (char l = 'D'; l <= 'Z'; l++) {
        if (!vfs_root(l)) continue;
        char b[80]; snprintf(b, sizeof b, "%c:  %s", l, vfs_drive_label(l) ? vfs_drive_label(l) : "");
        draw_icon(c, ICON_DRIVE, 16, a.x + 20, y); draw_text(c, FONT_UI, a.x + 42, y + 1, b, s->pg_text);
        y += 20; found++;
    }
    if (!found) { draw_text(c, FONT_UI, a.x + 20, y, "No CD drives or other disks were found.", s->pg_text); y += 20; }
    y += 10;
    draw_text_wrapped(c, FONT_UI, a.x + 16, y, a.w - 32, "Copying tracks from an audio CD needs the drive to be read in its audio mode, which THE WRLD OS "
        "does not do yet. Music files on a data CD or another disk can be played directly: open them from the Media Library or the File Explorer.", s->pg_text, 3);
}
static void copy_dest_name(int i, char *b, int cap) {
    if (i == 0) { strlcpy(b, USER_DOCS "\\My Music\\Copied", cap); return; }
    int k = 0;
    for (char l = 'D'; l <= 'Z'; l++) if (vfs_root(l) && ++k == i) { snprintf(b, cap, "%c:\\", l); return; }
    b[0] = 0;
}
static void draw_copy(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, "Copy to CD or Device", NULL);
    int half = a.w / 2 - 12, y = a.y + 46;
    draw_text(c, FONT_UI_BOLD, a.x + 12, y, "Items to copy (the Now Playing list)", s->pg_text);
    draw_text(c, FONT_UI_BOLD, a.x + half + 24, y, "Copy to", s->pg_text);
    y += 20;
    rect_t lr = R(a.x + 10, y, half, a.y + a.h - y - 44);
    fill_rect(c, lr.x, lr.y, lr.w, lr.h, 0xFFFFFFFF); draw_rect(c, lr.x, lr.y, lr.w, lr.h, s->classic ? 0xFF808080 : s->pg_sel);
    for (int i = 0; i < m->nlist && 4 + i * 20 + 20 < lr.h; i++) {
        ui_checkbox(w, lr.x + 6, lr.y + 4 + i * 20, m->list[i].title, &m->copy_sel[i]);
    }
    if (!m->nlist) draw_text(c, FONT_UI, lr.x + 8, lr.y + 8, "The Now Playing list is empty.", 0xFF808080);
    /* destinations */
    rect_t dr = R(a.x + half + 22, y, half, lr.h);
    fill_rect(c, dr.x, dr.y, dr.w, dr.h, 0xFFFFFFFF); draw_rect(c, dr.x, dr.y, dr.w, dr.h, s->classic ? 0xFF808080 : s->pg_sel);
    for (int i = 0; i < 12; i++) {
        char b[80]; copy_dest_name(i, b, sizeof b);
        if (!b[0]) break;
        rect_t rr = R(dr.x + 1, dr.y + 2 + i * 22, dr.w - 2, 22);
        if (m->copy_dest == i) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pg_sel);
        draw_icon(c, i ? ICON_DRIVE : ICON_FOLDER, 16, rr.x + 4, rr.y + 3);
        draw_text_ellipsis(c, FONT_UI, rr.x + 24, rr.y + 4, rr.w - 28, b, s->classic && m->copy_dest == i ? 0xFFFFFFFF : s->pg_text);
        if (ui_click_area(w, rr)) m->copy_dest = i;
    }
    int nsel = 0; for (int i = 0; i < m->nlist; i++) if (m->copy_sel[i]) nsel++;
    if (page_button(w, m, R(a.x + a.w - 120, a.y + a.h - 34, 108, 24), "Copy", nsel > 0)) {
        char dest[120]; copy_dest_name(m->copy_dest, dest, sizeof dest);
        vnode_t *dd = m->copy_dest == 0 ? vfs_mkdir_p(dest) : vfs_resolve(dest, NULL);
        int ok = 0;
        for (int i = 0; dd && i < m->nlist; i++) if (m->copy_sel[i]) {
            vnode_t *src = vfs_resolve(m->list[i].path, NULL);
            if (src && vfs_copy(src, dd, NULL)) ok++;
        }
        snprintf(m->copy_msg, sizeof m->copy_msg, dd ? "Copied %d of %d item%s to %s." : "%d of %d item%s copied: %s cannot be written.", ok, nsel, nsel == 1 ? "" : "s", dest);
    }
    if (m->copy_msg[0]) draw_text_ellipsis(c, FONT_UI, a.x + 12, a.y + a.h - 29, a.w - 150, m->copy_msg, s->pg_text);
}
static void skin_preview(canvas_t *c, rect_t r, const skin_t *s) {
    if (s->classic) {
        fill_rect(c, r.x, r.y, r.w, r.h, 0xFFD4D0C8); bevel3d(c, r, true);
        gradient_h(c, r.x + 3, r.y + 3, r.w - 6, 16, 0xFF0A246A, 0xFFA6CAF0);
        draw_text(c, FONT_SMALL, r.x + 6, r.y + 5, APP_NAME, 0xFFFFFFFF);
        fill_rect(c, r.x + 50, r.y + 24, r.w - 58, r.h - 70, 0xFF000000);
        for (int i = 0; i < 5; i++) { rect_t b = R(r.x + 6, r.y + 24 + i * 18, 40, 16); bevel3d(c, b, true); }
        for (int i = 0; i < 5; i++) { rect_t b = R(r.x + 12 + i * 24, r.y + r.h - 34, 20, 20); bevel3d(c, b, true); }
        return;
    }
    fill_round_rect(c, r.x, r.y, r.w, r.h, 10, s->fr_edge);
    gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 9, s->fr_hi, s->fr_bot);
    fill_round_rect(c, r.x + 6, r.y + 22, 44, r.h - 72, 5, s->tb_top);
    for (int i = 0; i < 6; i++) hline(c, r.x + 10, r.y + 32 + i * 14, 34, s->tb_text);
    fill_rect(c, r.x + 54, r.y + 22, r.w - 60 - 44, r.h - 72, 0xFF000000);
    for (int i = 0; i < 12; i++) { int h = (i * 37 % 23) + 6; fill_rect(c, r.x + 60 + i * 8, r.y + r.h - 54 - h, 6, h, s->thumb); }
    fill_rect(c, r.x + r.w - 48, r.y + 22, 42, r.h - 72, s->pn_bot);
    fill_round_rect(c, r.x + 6, r.y + r.h - 44, r.w - 12, 38, 8, mix(s->fr_top, 0xFFFFFFFF, 70));
    fill_circle(c, r.x + 26, r.y + r.h - 25, 13, s->bt_edge); fill_circle(c, r.x + 26, r.y + r.h - 25, 12, s->bt_hi);
    for (int i = 0; i < 3; i++) { fill_circle(c, r.x + 52 + i * 20, r.y + r.h - 25, 8, s->bt_edge); fill_circle(c, r.x + 52 + i * 20, r.y + r.h - 25, 7, s->bt_lo); }
    fill_round_rect(c, r.x + 116, r.y + r.h - 28, 60, 6, 3, s->groove);
    fill_round_rect(c, r.x + 150, r.y + r.h - 31, 12, 12, 6, s->thumb);
}
static void draw_skins(window_t *w, mp_t *m, rect_t a) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    page_head(w, m, a, "Skin Chooser", NULL);
    int y = a.y + 46;
    if (page_button(w, m, R(a.x + 10, y, 100, 22), "Apply Skin", true)) {
        m->skin = m->skin_sel;
        wm_set_skinned(w, !skins[m->skin].classic);
        char b[8]; snprintf(b, sizeof b, "%d", m->skin); settings_set("mp_skin", b); settings_save();
        m->page = P_NOW;
        return;
    }
    y += 30;
    int lw = MIN(170, a.w / 3);
    rect_t lr = R(a.x + 10, y, lw, a.y + a.h - y - 10);
    fill_rect(c, lr.x, lr.y, lr.w, lr.h, 0xFFFFFFFF); draw_rect(c, lr.x, lr.y, lr.w, lr.h, s->classic ? 0xFF808080 : s->pg_sel);
    for (int i = 0; i < NSKINS; i++) {
        rect_t rr = R(lr.x + 1, lr.y + 2 + i * 22, lr.w - 2, 22);
        bool sel = m->skin_sel == i;
        if (sel) fill_rect(c, rr.x, rr.y, rr.w, rr.h, s->pg_sel);
        draw_text(c, i == m->skin ? FONT_UI_BOLD : FONT_UI, rr.x + 8, rr.y + 4, skins[i].name, s->classic && sel ? 0xFFFFFFFF : s->pg_text);
        if (ui_click_area(w, rr)) {
            uint32_t now = timer_ticks();
            if (m->skin_sel == i && m->last_click_where == 3 && now - m->last_click_ms < wm_dblclick_ms) {
                m->skin = i; wm_set_skinned(w, !skins[i].classic);
                char b[8]; snprintf(b, sizeof b, "%d", m->skin); settings_set("mp_skin", b); settings_save();
            }
            m->skin_sel = i; m->last_click_where = 3; m->last_click_ms = now;
        }
    }
    /* preview and description */
    int px = lr.x + lr.w + 20, pw = MIN(a.x + a.w - px - 14, 300), ph = pw * 3 / 4;
    if (pw > 120) {
        skin_preview(c, R(px, y, pw, ph), &skins[m->skin_sel]);
        draw_text(c, FONT_UI_BOLD, px, y + ph + 10, skins[m->skin_sel].name, s->pg_text);
        draw_text_wrapped(c, FONT_UI, px, y + ph + 28, pw, skins[m->skin_sel].desc, mix(s->pg_text, s->pg_bg, 70), 2);
    }
}

/* ---------------- the bottom: seek bar and transport ---------------- */
static void draw_transport(window_t *w, mp_t *m, layout_t *L) {
    canvas_t *c = w->surf;
    const skin_t *s = SK;
    int y0 = L->bottom_y;
    if (s->classic) { fill_rect(c, 4, y0 + 2, c->w - 8, c->h - y0 - 6, 0xFFD4D0C8); bevel3d(c, R(4, y0 + 2, c->w - 8, c->h - y0 - 6), true); }
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    uint32_t total = m->snd ? m->snd->frames : 0, pos = pos_frames(m), rate = (uint32_t)audio_rate();
    bool can_seek = m->snd && (m->state == ST_PLAY || m->state == ST_PAUSE);
    /* row 1: rewind, the seek bar, fast forward */
    int sy = y0 + (s->classic ? 12 : 16);
    rect_t rw = R(16, sy - 7, 22, 14), fw = R(c->w - 38, sy - 7, 22, 14);
    for (int k = 0; k < 2; k++) {
        rect_t r = k ? fw : rw;
        bool hot = ui_hover(w, r);
        if (s->classic) { fill_rect(c, r.x, r.y, r.w, r.h, 0xFFD4D0C8); bevel3d(c, r, !(hot && w->ui.down)); }
        else { fill_round_rect(c, r.x, r.y, r.w, r.h, 7, s->bt_edge); gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 6, hot ? lighten(s->bt_hi, 10) : s->bt_hi, s->bt_lo); }
        glyph(c, k ? G_FFW : G_REW, r.x + r.w / 2, r.y + r.h / 2, 8, s->classic ? 0xFF000000 : glyph_col(m));
        if (ui_click_area(w, r)) seek_ms(m, (int64_t)pos * 1000 / MAX(1u, rate) + (k ? 10000 : -10000));
    }
    int pv = slider(w, m, R(46, sy - 8, c->w - 92, 16), total ? (int)((uint64_t)pos * 1000 / total) : 0, 1000, &m->seek_drag, can_seek);
    static bool was_drag;
    /* while dragging, and on the paint that ends the drag (the slider has just let go, so the
     * target is still where the mouse was): the position under the thumb, not the playing one,
     * or a seek went back to where it started */
    if ((m->seek_drag || was_drag) && can_seek) pos = (uint32_t)((uint64_t)pv * total / 1000);
    if (was_drag && !m->seek_drag && can_seek) seek_ms(m, (int64_t)pos * 1000 / MAX(1u, rate));
    was_drag = m->seek_drag;
    /* row 2: play, stop, previous, next, mute, volume, shuffle, repeat ... status */
    int cy = c->h - (s->classic ? 22 : 24);
    int x = 30;
    if (sphere(w, m, x, cy, 15, false)) toggle_play(m);
    glyph(c, m->state == ST_PLAY ? G_PAUSE : G_PLAY, x, cy, 11, s->classic ? 0xFF000000 : glyph_col(m));
    x += 30;
    if (sphere(w, m, x, cy, 10, false)) stop(m);
    glyph(c, G_STOP, x, cy, 7, s->classic ? 0xFF000000 : glyph_col(m));
    x += 28;
    if (sphere(w, m, x, cy, 10, false)) { if (pos > rate * 3) seek_ms(m, 0); else { int n = next_index(m, -1); if (n >= 0) play_index(m, n); } }
    glyph(c, G_PREV, x, cy, 8, s->classic ? 0xFF000000 : glyph_col(m));
    x += 22;
    if (sphere(w, m, x, cy, 10, false)) { int n = next_index(m, 1); if (n >= 0) play_index(m, n); }
    glyph(c, G_NEXT, x, cy, 8, s->classic ? 0xFF000000 : glyph_col(m));
    x += 30;
    if (sphere(w, m, x, cy, 10, audio_muted)) audio_muted = !audio_muted;
    glyph(c, G_MUTE, x, cy, 10, s->classic ? 0xFF000000 : glyph_col(m));
    if (audio_muted) draw_thick_line(c, x - 6, cy + 6, x + 6, cy - 6, 2, 0xFFE03020);
    static bool vdrag;
    audio_volume = slider(w, m, R(x + 16, cy - 8, 70, 16), audio_volume, 100, &vdrag, true);
    if (ui_hover(w, R(x + 16, cy - 8, 70, 16)) && w->ui.wheel) audio_volume = CLAMP(audio_volume - w->ui.wheel * 5, 0, 100);
    x += 108;
    if (sphere(w, m, x, cy, 9, m->shuffle)) m->shuffle = !m->shuffle;
    glyph(c, G_SHUFFLE, x, cy, 9, s->classic ? 0xFF000000 : glyph_col(m));
    x += 22;
    if (sphere(w, m, x, cy, 9, m->repeat)) m->repeat = !m->repeat;
    glyph(c, G_REPEAT, x, cy, 9, s->classic ? 0xFF000000 : glyph_col(m));
    x += 18;
    /* the status box on the right */
    int bw = c->w - x - 22 - (s->classic ? 0 : 18);
    if (bw > 90) {
        rect_t sb = R(x + 6, cy - 10, bw - 6, 20);
        if (s->classic) { fill_rect(c, sb.x, sb.y, sb.w, sb.h, 0xFF000000); bevel3d(c, sb, false); }
        else { fill_round_rect(c, sb.x, sb.y, sb.w, sb.h, 5, s->lcd); draw_round_rect(c, sb.x, sb.y, sb.w, sb.h, 5, mix(s->fr_bot, 0xFF000000, 80)); }
        char tp[16], tl[16], b[200];
        fmt_time(rate ? (uint32_t)((uint64_t)pos * 1000 / rate) : 0, tp, sizeof tp);
        fmt_time(t ? (total && rate ? (uint32_t)((uint64_t)total * 1000 / rate) : t->ms) : 0, tl, sizeof tl);
        const char *st = m->state == ST_PLAY ? "Playing" : m->state == ST_PAUSE ? "Paused" : m->state == ST_LOAD ? "Opening" : "Stopped";
        int tw = text_width(FONT_SMALL, "00:00 / 00:00") + 10;
        if (t && m->state != ST_STOP) snprintf(b, sizeof b, "%s: %s", st, t->title); else snprintf(b, sizeof b, "%s", m->status[0] ? m->status : st);
        draw_text_ellipsis(c, FONT_SMALL, sb.x + 6, sb.y + 4, sb.w - tw - 10, b, s->classic ? 0xFF00FF00 : s->lcd_text);
        snprintf(b, sizeof b, "%s / %s", tp, tl);
        draw_text(c, FONT_SMALL, sb.x + sb.w - tw, sb.y + 4, b, s->classic ? 0xFF00FF00 : s->lcd_text);
    }
    /* resize grip */
    if (!s->classic && w->state != WS_MAXIMIZED) {
        rect_t g = R(c->w - 16, c->h - 16, 14, 14);
        for (int k = 0; k < 3; k++) for (int j = 0; j <= k; j++) fill_rect(c, g.x + 10 - j * 4, g.y + 2 + k * 4, 2, 2, mix(s->fr_edge, 0xFF000000, 30));
        if (ui_hover(w, g) && w->ui.pressed) wm_begin_resize(w, 2 | 8);
    }
}

/* ---------------- painting ---------------- */
static void mp_paint(window_t *w) {
    mp_t *m = w->data;
    canvas_t *c = w->surf;
    if (!!(w->flags & WF_SKINNED) == SK->classic) wm_set_skinned(w, !SK->classic);
    draw_frame(w, m);
    layout_t L = layout(w, m);
    draw_title(w, m, &L);
    draw_taskbar(w, m, &L);
    rect_t a = L.content;
    switch (m->page) {
    case P_NOW: draw_now(w, m, a); break;
    case P_LIB: draw_library(w, m, a); break;
    case P_GUIDE: draw_guide(w, m, a); break;
    case P_RIP: draw_rip(w, m, a); break;
    case P_COPY: draw_copy(w, m, a); break;
    case P_SKIN: draw_skins(w, m, a); break;
    case P_RADIO: draw_info_page(w, m, a, "Radio Tuner", "Internet radio",
        "Internet radio stations stream their sound continuously over the network. THE WRLD OS can fetch web pages and files, "
        "but the player cannot yet play a stream while it is still arriving, so the Radio Tuner has no stations to offer.\n\n"
        "Saved recordings and podcasts that you download as MP3, Ogg Vorbis or FLAC files play from the Media Library."); break;
    case P_PREMIUM: draw_info_page(w, m, a, "Premium Services", NULL,
        "No premium music or video services are set up on this computer. THE WRLD Media Player plays the files you keep "
        "yourself and does not connect to any store or subscription service."); break;
    }
    draw_transport(w, m, &L);
    /* drag the window by its title band or frame when nothing else took the press */
    if (!SK->classic && w->ui.pressed && !w->ui.active && w->state != WS_MAXIMIZED &&
        (pt_in(L.title, w->ui.mx, w->ui.my) || w->ui.mx < FR_SIDE || w->ui.mx >= c->w - FR_SIDE)) {
        if (w->ui.dblclick) wm_maximize(w, true); else wm_begin_move(w);
    }
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
    if (m->job && m->job->taken && m->job->state == 2) { kfree(m->job); m->job = NULL; }
    if (m->state == ST_PLAY && m->snd && !audio_voice_active(m->voice, m->snd)) {
        int n = next_index(m, 1);
        if (n >= 0 && !(n == m->cur && !m->repeat)) play_index(m, n);
        else if (n == m->cur && m->repeat) { m->voice = audio_play(m->snd); if (m->voice < 0) m->state = ST_STOP; }
        else m->state = ST_STOP;
    }
    if (m->vid && m->vframe && m->vid_track == m->cur && (m->state == ST_PLAY || m->state == ST_PAUSE))
        mvid_frame(m->vid, (double)pos_frames(m) / audio_rate(), m->vframe->px, m->vframe->stride);
    /* repaint while something moves: playing, loading, or the visualizer settling
     * for a second after a stop; a stopped player costs nothing */
    if (m->state == ST_PLAY || m->state == ST_LOAD || m->job) m->quiet = 0; else if (m->quiet < 1000) m->quiet++;
    if (m->quiet > 20) return;
    analyse(m);
    m->viz_gen++;
    w->dirty = true;
    if (m->full && wm_alive(m->full)) m->full->dirty = true;
}

/* ---------------- full screen ---------------- */
static void full_paint(window_t *w) {
    if (!mp_win || !wm_alive(mp_win)) { gfx_clear(w->surf, 0xFF000000); return; }
    mp_t *m = mp_win->data;
    canvas_t *c = w->surf;
    draw_picture(c, m, R(0, 0, c->w, c->h));
    const track_t *t = m->nlist && m->cur < m->nlist ? &m->list[m->cur] : NULL;
    if (t && timer_ticks() - m->last_click_ms < 4000) {
        char b[160]; snprintf(b, sizeof b, "%s - %s", t->title, t->artist);
        fill_round_rect_blend(c, 20, c->h - 50, text_width(FONT_LARGE_BOLD, b) + 30, 34, 8, 0xFF000000, 150);
        draw_text(c, FONT_LARGE_BOLD, 35, c->h - 42, b, 0xFFFFFFFF);
    }
    if (w->ui.pressed || w->ui.rpressed) { m->full = NULL; wm_close(w); }
}
static void full_key(window_t *w, key_event_t *e) {
    if (!e->pressed) return;
    if (mp_win && wm_alive(mp_win)) {
        mp_t *m = mp_win->data;
        if (e->key == KEY_RIGHT || e->key == KEY_LEFT) { step_viz(m, e->key == KEY_RIGHT ? 1 : -1); return; }
        if (e->key == ' ') { toggle_play(m); return; }
        m->full = NULL;
    }
    wm_close(w);
}
static void full_destroy(window_t *w) { if (mp_win && wm_alive(mp_win)) { mp_t *m = mp_win->data; if (m->full == w) m->full = NULL; } }
static void open_full(mp_t *m) {
    if (m->full && wm_alive(m->full)) return;
    window_t *w = wm_create("Full Screen", 0, 0, SCREEN_W, SCREEN_H, WF_POPUP | WF_TOPMOST);
    if (!w) return;
    w->paint = full_paint; w->key = full_key; w->destroy = full_destroy;
    m->full = w; m->last_click_ms = timer_ticks();
    wm_focus(w);
}

/* ---------------- keys, menus, lifecycle ---------------- */
void mp_playlist_menu(void *ctx, int id) {
    mp_t *m = ctx;
    if (!mp_win || !wm_alive(mp_win) || mp_win->data != m) return;
    if (!m->scanned) library_scan(m);
    if (id == 5) { stop(m); unload(m); m->nlist = 0; m->cur = 0; return; }
    if (id >= 2 && id <= 4) {
        m->nlist = 0;
        for (int i = 0; i < m->nlib; i++) {
            const track_t *t = &m->lib[i];
            if ((id == 2 && !t->video) || (id == 3 && t->video) || (id == 4 && !strncasecmp(t->path, MUSIC_DIR, strlen(MUSIC_DIR)))) add_track(m, t);
        }
        if (m->nlist) play_index(m, 0);
    }
    m->page = P_NOW;
    mp_win->dirty = true;
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
    else if (ctrl && e->key >= '1' && e->key <= '8') { m->page = e->key - '1'; if (m->page == P_LIB && !m->scanned) library_scan(m); }
    else if ((e->mods & MOD_ALT) && e->key == KEY_ENTER) open_full(m);
    else if (e->key == KEY_F8) audio_muted = !audio_muted;
    else if (e->key == KEY_F9) audio_volume = MAX(0, audio_volume - 5);
    else if (e->key == KEY_F10) audio_volume = MIN(100, audio_volume + 5);
    else if (m->page == P_NOW && e->key == ' ') toggle_play(m);
    else if (m->page == P_NOW && (e->key == KEY_LEFT || e->key == KEY_RIGHT)) {
        int64_t now = (int64_t)pos_frames(m) * 1000 / audio_rate();
        seek_ms(m, now + (e->key == KEY_RIGHT ? 5000 : -5000));
    } else if (m->page == P_NOW && (e->key == KEY_UP || e->key == KEY_DOWN)) step_viz(m, e->key == KEY_DOWN ? 1 : -1);
    else return;
    w->dirty = true;
}
static void mp_destroy(window_t *w) {
    mp_t *m = w->data;
    if (m->full && wm_alive(m->full)) { window_t *f = m->full; m->full = NULL; wm_close(f); }
    unload(m);
    if (m->art) canvas_free(m->art);
    for (int i = 0; i < 2; i++) if (m->vcache[i].cv) canvas_free(m->vcache[i].cv);
    settings_save();
    kfree(m->lib); kfree(m->list); kfree(m);
    if (mp_win == w) mp_win = NULL;
}
static void open_cb(void *ctx, const char *text) {
    (void)ctx;
    if (!text || !text[0] || !mp_win || !wm_alive(mp_win)) return;
    mp_t *m = mp_win->data;
    char p[200]; strlcpy(p, text, sizeof p);
    size_t l = strlen(p); if (l >= 2 && p[0] == '"' && p[l - 1] == '"') { p[l - 1] = 0; memmove(p, p + 1, l - 1); }
    if (!vfs_resolve(p, NULL)) { msgbox(APP_NAME, "The file or folder could not be found.", MBI_ERROR, MBX_OK, NULL, NULL); return; }
    add_path(m, p, true);
    m->page = P_NOW;
    mp_win->dirty = true;
}
void mediaplayer_open_prompt(void) {
    input_box("Open", "Type the path of a media file or a folder:", MUSIC_DIR, open_cb, NULL);
}

static bool mp_drop(window_t *w, int x, int y, const char *paths, bool copy) {
    (void)w; (void)x; (void)y; (void)copy;
    char p[512]; const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
    snprintf(p, sizeof p, "%.*s", (int)MIN(n, sizeof p - 1), paths);
    if (!p[0]) return false;
    mediaplayer_open(p);
    return true;
}
void mediaplayer_open(const char *path) {
    if (path && *path && vfs_resolve(path, NULL)) { char full_[256]; vfs_path(vfs_resolve(path, NULL), full_, sizeof full_); recent_add(full_); }   /* for The WRLD Search */
    if (mp_win && wm_alive(mp_win)) {
        wm_focus(mp_win);
        if (path) { mp_t *m = mp_win->data; add_path(m, path, true); m->page = P_NOW; }
        return;
    }
    mp_t *m = kzalloc(sizeof *m);
    if (!m) return;
    const char *sk = settings_get("mp_skin");
    m->skin = sk ? CLAMP(atoi(sk), 0, NSKINS - 1) : 0;
    m->viz_c = 1; m->viz_p = 0;                                     /* Charge : Randomization */
    const char *vz = settings_get("mp_viz");
    if (vz) { int c = atoi(vz); const char *k = strchr(vz, ','); int p = k ? atoi(k + 1) : 0; if (c >= 0 && c < viz_collections() && p >= 0 && p < viz_presets(c)) { m->viz_c = c; m->viz_p = p; } }
    int cw = MIN(760, SCREEN_W - 40), ch = MIN(540, SCREEN_H - 70);
    window_t *w = wm_create(APP_NAME, -1, -1, cw, ch, WF_RESIZABLE | (skins[m->skin].classic ? 0 : WF_SKINNED));
    if (!w) { kfree(m); return; }
    m->lib = kzalloc(sizeof(track_t) * MAXT);
    m->list = kzalloc(sizeof(track_t) * MAXT);
    m->voice = -1; m->lib_sel = -1; m->list_sel = -1; m->vid_track = -1; m->skin_sel = m->skin; m->lib_view = 0;
    w->data = m; w->paint = mp_paint; w->destroy = mp_destroy; w->key = mp_key;
    w->drop = mp_drop;
    w->icon = ICON_MEDIA_PLAYER;
    w->min_w = 600; w->min_h = 430;
    mp_win = w;
    library_scan(m);
    if (path) add_path(m, path, true);
    else for (int i = 0; i < m->nlib && !strncasecmp(m->lib[i].path, MUSIC_DIR, strlen(MUSIC_DIR)); i++) add_track(m, &m->lib[i]);
    if (!m->nlist && !path) m->page = P_LIB;
    wm_set_timer(w, 50, mp_tick);
    wm_center(w);
}
