/* Glue between the kernel and minimp3 (third_party/minimp3). */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include <stdlib.h>
#include <string.h>
#include "../../third_party/minimp3/minimp3.h"
#include "mp3glue.h"

static uint32_t id3_skip(const uint8_t *d, uint32_t n) {
    if (n >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        uint32_t sz = ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
        sz += 10 + ((d[5] & 0x10) ? 10 : 0);
        return sz < n ? sz : n;
    }
    return 0;
}

struct mp3s {
    mp3dec_t dec;
    const uint8_t *data;
    uint32_t off, len;
};
mp3s_t *mp3s_open(const uint8_t *data, uint32_t len) {
    mp3s_t *s = malloc(sizeof *s);
    if (!s) return NULL;
    mp3dec_init(&s->dec);
    s->data = data; s->off = id3_skip(data, len);
    if (len >= 128 && !memcmp(data + len - 128, "TAG", 3)) len -= 128;    /* ID3v1 */
    s->len = len;
    return s;
}
int mp3s_read(mp3s_t *s, int16_t *pcm, int *channels, int *rate) {
    while (s->off < s->len) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&s->dec, s->data + s->off, (int)(s->len - s->off), pcm, &info);
        if (!info.frame_bytes) { s->off = s->len; break; }
        s->off += info.frame_bytes;
        if (n) { *channels = info.channels; *rate = info.hz; return n; }
    }
    return 0;
}
void mp3s_close(mp3s_t *s) { free(s); }

int mp3_probe(const uint8_t *data, uint32_t len, uint32_t *ms, int *rate, int *kbps) {
    uint32_t off = id3_skip(data, len);
    uint64_t samples = 0; int hz = 0, frames = 0; uint64_t bits = 0;
    while (off + 4 <= len) {
        int fb, h, br;
        const uint8_t *p = data + off;
        if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0 || !hdr_valid(p)) { off++; continue; }
        h = hdr_sample_rate_hz(p); fb = hdr_frame_bytes(p, 0) + hdr_padding(p);
        br = hdr_bitrate_kbps(p);
        if (fb < 4 || off + fb > len) break;
        samples += hdr_frame_samples(p); bits += br; frames++;
        if (!hz) hz = h;
        off += fb;
    }
    if (!frames || !hz) return -1;
    *ms = (uint32_t)(samples * 1000 / hz); *rate = hz; *kbps = (int)(bits / frames);
    return 0;
}
