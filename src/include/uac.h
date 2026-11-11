/* USB audio: read a device's configuration descriptor to find the playback
 * path it offers. This is only the descriptor work (no hardware), so the same
 * code is checked on the build machine.
 *
 * A class 1 device lists its sample rates in the streaming interface's format
 * type I descriptor, and the rate is set on the endpoint. A class 2 device has
 * no list there: its clock source entity (an output terminal points at it)
 * carries the rate, and the rate is set on that entity. */
#ifndef UAC_H
#define UAC_H
#include "types.h"

typedef struct {
    int ac;                 /* the audio control interface number */
    int stream;             /* the streaming interface number */
    int alt;                /* its alternate setting with the stream */
    int ep;                 /* isochronous OUT endpoint number */
    int maxpkt, interval;   /* its descriptor's packet size and bInterval */
    int rate;               /* from the class 1 format descriptor, 0 for class 2 */
    int channels;           /* bNrChannels */
    int frame_bytes;        /* bytes per sample (bSubframeSize) */
    int clock, link;        /* class 2: clock source to set, terminal it feeds */
    bool class2;            /* the rate is set on the clock source, not the format */
    bool assumed;           /* the format was not readable: assumed 48 kHz stereo 16-bit */
} uac_fmt_t;

/* true when the configuration has an audio control interface and a streaming
 * alternate setting with an isochronous OUT endpoint and a known format */
bool uac_parse(const uint8_t *cfg, int len, uac_fmt_t *o);

#endif
