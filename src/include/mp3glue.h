#ifndef MP3GLUE_H
#define MP3GLUE_H
/* MP3 decoding (minimp3) for the kernel; plain C types only, since the decoder
 * is built against the C library shim rather than the kernel headers. */
#include <stdint.h>

/* Streaming decode: open, then read one MP3 frame at a time into `pcm`
 * (room for 1152 stereo frames). Returns the frames decoded, 0 at the end;
 * channels and rate describe that frame. The state lives on the kernel heap. */
typedef struct mp3s mp3s_t;
mp3s_t *mp3s_open(const uint8_t *data, uint32_t len);
int mp3s_read(mp3s_t *s, int16_t *pcm, int *channels, int *rate);
void mp3s_close(mp3s_t *s);
/* Duration and format from the frame headers alone (fast, no decoding). */
int mp3_probe(const uint8_t *data, uint32_t len, uint32_t *ms, int *rate, int *kbps);
#endif
