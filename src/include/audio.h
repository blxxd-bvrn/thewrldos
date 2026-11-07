#ifndef AUDIO_H
#define AUDIO_H
#include "kernel.h"

/* decoded sound: 48 kHz, 16-bit, stereo interleaved */
typedef struct { int16_t *pcm; uint32_t frames; } sound_t;

void audio_init(void);
bool audio_present(void);
const char *audio_device_name(void);
int audio_play(const sound_t *s);           /* voice number, or -1 */
void audio_stop_all(void);
bool audio_busy(void);
extern int audio_volume;                    /* 0..100 */
extern bool audio_muted;

sound_t *sound_load_wav(const uint8_t *data, uint32_t len, char *err, int errcap);
void sound_free(sound_t *s);

/* files and system sounds */
#define SOUND_STARTUP  "C:\\WRLD\\Media\\WRLD Startup.wav"
#define SOUND_SHUTDOWN "C:\\WRLD\\Media\\WRLD Shutdown.wav"
extern bool system_sounds;                  /* play startup / shutdown sounds */
bool audio_play_file(const char *path, char *err, int errcap);
void audio_wait(uint32_t max_ms);           /* until everything has finished playing */
#endif
