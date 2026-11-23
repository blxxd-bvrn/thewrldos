#ifndef MEDIADEC_H
#define MEDIADEC_H
/* Media decoding for THE WRLD Media Player (src/media). Plain C types only:
 * the decoders are built against the C library shim, not the kernel headers.
 *
 * Audio: MP3, MP2 and MP1 (minimp3), Ogg Vorbis (stb_vorbis), FLAC and Ogg FLAC
 * (dr_flac), WAV (PCM 8/16/24/32-bit, 32/64-bit float, A-law, mu-law, IMA and
 * Microsoft ADPCM, WAVE_FORMAT_EXTENSIBLE), AIFF/AIFC and Sun .au (rust/src/pcm.rs).
 * Video: MPEG-1 program streams with MP2 audio (.mpg, .mpeg; pl_mpeg). */
#include <stdint.h>

typedef struct {
    int rate, channels;                 /* source rate; mdec_read always gives stereo */
    uint32_t ms; int kbps;
    int video, width, height; float fps;
    char format[40];                    /* "MP3", "Ogg Vorbis", "WAV (IMA ADPCM)"... */
    char title[80], artist[64], album[64];
} mdec_info_t;

typedef struct mdec mdec_t;
/* fills `info` from the headers (and tags) without decoding everything; 0 on success */
int mdec_probe(const uint8_t *data, uint32_t len, const char *ext, mdec_info_t *info);
mdec_t *mdec_open(const uint8_t *data, uint32_t len, const char *ext, mdec_info_t *info, char *err, int errcap);
/* up to max_frames (at least 4096) stereo frames at info->rate; 0 at the end */
int mdec_read(mdec_t *d, int16_t *pcm, int max_frames);
void mdec_close(mdec_t *d);
/* does the extension name a format the player opens? */
int mdec_known_ext(const char *ext);

/* MPEG-1 video: decode the frame shown at time t (seconds) into 32-bit pixels
 * (0xAARRGGBB, width*height, `stride` pixels per row); 1 when the picture changed */
typedef struct mvid mvid_t;
mvid_t *mvid_open(const uint8_t *data, uint32_t len, int *width, int *height, float *fps);
int mvid_frame(mvid_t *v, double t, uint32_t *px, int stride);
void mvid_close(mvid_t *v);

/* format glue, one file each (src/media) */
typedef struct mfmt mfmt_t;
mfmt_t *vorbis_open(const uint8_t *data, uint32_t len, mdec_info_t *info);
int vorbis_read(mfmt_t *f, int16_t *pcm, int max_frames);
void vorbis_close(mfmt_t *f);
mfmt_t *flac_open(const uint8_t *data, uint32_t len, mdec_info_t *info);
int flac_read(mfmt_t *f, int16_t *pcm, int max_frames);
void flac_close(mfmt_t *f);
mfmt_t *mpeg_open(const uint8_t *data, uint32_t len, mdec_info_t *info);
int mpeg_read(mfmt_t *f, int16_t *pcm, int max_frames);
void mpeg_close(mfmt_t *f);
mfmt_t *pcm_open(const uint8_t *data, uint32_t len, mdec_info_t *info, char *err, int errcap);
int pcm_read(mfmt_t *f, int16_t *pcm, int max_frames);
void pcm_close(mfmt_t *f);
#endif
