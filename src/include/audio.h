#ifndef AUDIO_H
#define AUDIO_H
#include "kernel.h"

/* decoded sound: 48 kHz, 16-bit, stereo interleaved */
typedef struct { int16_t *pcm; uint32_t frames; } sound_t;

void audio_init(void);
void audio_rescan(void);                        /* re-scan the bus / refresh the endpoint list */
bool audio_present(void);
const char *audio_device_name(void);
const char *audio_controller_name(void);    /* HDA (virtual) when the kernel emulates one */
int audio_device_count(void);               /* playback endpoints the Sound page lists */
const char *audio_device_entry(int i);
int audio_device_active(void);
void audio_device_select(int i);            /* makes endpoint i the default */
void audio_diag(char *buf, int cap);        /* device, codec route, whether playback advances */
int audio_play(const sound_t *s);           /* voice number, or -1 */
/* a stream of 16-bit stereo frames at audio_rate(), written as it is produced (/dev/dsp) */
int audio_stream_open(void);                 /* -1: no sound device */
int audio_stream_write(int id, const int16_t *pcm, int frames);   /* frames taken (what fits) */
int audio_stream_room(int id);               /* frames that would fit now */
int audio_stream_queued(int id);             /* frames written and not yet heard */
void audio_stream_close(int id);
void audio_stop_all(void);
int audio_rate(void);                        /* frames per second of a sound_t */
bool audio_voice_active(int v, const sound_t *s);
uint32_t audio_voice_position(int v, const sound_t *s);  /* frame being heard */
void audio_voice_seek(int v, const sound_t *s, uint32_t frame);
void audio_voice_pause(int v, const sound_t *s, bool paused);
void audio_voice_stop(int v, const sound_t *s);
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
