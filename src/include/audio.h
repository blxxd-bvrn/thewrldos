#ifndef AUDIO_H
#define AUDIO_H
#include "kernel.h"

/* decoded sound: 48 kHz, 16-bit, stereo interleaved */
typedef struct { int16_t *pcm; uint32_t frames; } sound_t;

void audio_init(void);
bool audio_present(void);
const char *audio_device_name(void);
const char *audio_controller_name(void);    /* HDA (virtual) when the kernel emulates one */
void audio_diag(char *buf, int cap);        /* device, codec route, whether playback advances */
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

/* a USB audio device became the output (called by the USB audio driver) */
void audio_usb_ready(int rate, const char *name);
void audio_usb_gone(void);                  /* the USB device was unplugged */
const char *audio_summary(void);            /* one line for the log-on screen */
/* pull mixed audio for the USB isochronous stream; 48 kHz, stereo interleaved */
int audio_usb_pull(int16_t *out, int frames);
#endif
