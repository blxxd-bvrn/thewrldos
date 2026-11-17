/* A Linux test program for THE WRLD OS: sound through /dev/dsp (Open Sound System).
 * Plays a 440 Hz (triangle-wave) tone for two seconds at 44.1 kHz, 16-bit stereo, and reports what
 * the device agreed to.  C:/Linux/usr/local/bin/dsptone [seconds] [hz] */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
/* the Open Sound System numbers (linux/soundcard.h, not in every C library) */
#define AFMT_S16_LE 0x10
#define SNDCTL_DSP_SYNC 0x5001
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_GETODELAY 0x80045017

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 2.0, hz = argc > 2 ? atof(argv[2]) : 440.0;
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("/dev/dsp"); return 1; }
    int fmt = AFMT_S16_LE, ch = 2, rate = 44100;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) < 0 || ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0) { perror("ioctl"); return 1; }
    printf("dsptone: format %#x, %d channels, %d Hz\n", fmt, ch, rate);
    int frames = (int)(secs * rate);
    short buf[2048 * 2];
    for (int done = 0; done < frames; ) {
        int n = frames - done < 2048 ? frames - done : 2048;
        for (int i = 0; i < n; i++) {
            double t = (done + i) * hz / rate; t -= (long)t;            /* position in the cycle, 0..1 */
            short v = (short)(12000 * (t < 0.5 ? 4 * t - 1 : 3 - 4 * t));
            buf[i * 2] = v; buf[i * 2 + 1] = v;
        }
        if (write(fd, buf, (size_t)n * 4) < 0) { perror("write"); return 1; }
        done += n;
    }
    int delay = 0; ioctl(fd, SNDCTL_DSP_GETODELAY, &delay);
    printf("dsptone: %d frames written, %d bytes still queued\n", frames, delay);
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    close(fd);
    printf("dsptone: done\n");
    return 0;
}
