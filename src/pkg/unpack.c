/* Decompression for software packages: the members of a .deb and the payload of
 * an .rpm are gzip, xz, LZMA ("lzma_alone", old .debs) or Zstandard streams.
 * xz and LZMA go through XZ Embedded (third_party/xz-embedded), Zstandard through
 * the zstd reference decoder (third_party/zstd-dec), gzip through the kernel's
 * inflate (rust/src/pkgfmt.rs). Built against the C library shim, whose malloc is the kernel heap. */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "xz.h"
#include "zstd_decompress.h"
#include "unpack.h"


int unpack_kind(const uint8_t *s, uint32_t n) {
    if (n >= 3 && s[0] == 0x1F && s[1] == 0x8B && s[2] == 8) return UNPACK_GZIP;
    if (n >= 6 && !memcmp(s, "\xFD" "7zXZ\0", 6)) return UNPACK_XZ;
    if (n >= 4 && !memcmp(s, "\x28\xB5\x2F\xFD", 4)) return UNPACK_ZSTD;
    if (n >= 3 && !memcmp(s, "BZh", 3)) return UNPACK_BZIP2;
    if (n >= 13 && s[0] == 0x5D && s[1] == 0 && s[2] == 0) return UNPACK_LZMA;   /* lc=3 lp=0 pb=2, the usual properties */
    return UNPACK_NONE;
}
const char *unpack_kind_name(int k) {
    static const char *names[] = { "none", "gzip", "xz", "zstd", "bzip2", "lzma" };
    return k >= 0 && k <= UNPACK_LZMA ? names[k] : "?";
}

static void say(char *err, int ecap, const char *m) { if (err && ecap) snprintf(err, ecap, "%s", m); }

/* gzip: one or more members (RFC 1952), each a raw deflate stream (rust/src/pkgfmt.rs) */
uint8_t *unpack_gunzip(const uint8_t *s, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap);
static uint8_t *gunzip(const uint8_t *s, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap) { return unpack_gunzip(s, n, max_out, outlen, err, ecap); }

/* xz and LZMA ("lzma_alone"), in pieces, into a growing buffer */
static uint8_t *unxz(const uint8_t *s, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap) {
    xz_crc32_init();
#ifdef XZ_USE_CRC64
    xz_crc64_init();
#endif
    struct xz_dec *d = xz_dec_init(XZ_DYNALLOC, 1u << 27);     /* dictionaries up to 128 MiB (xz -9 uses 64) */
    if (!d) { say(err, ecap, "not enough memory to unpack"); return NULL; }
    uint32_t cap = n * 4 < (1u << 20) ? (1u << 20) : n * 4;
    if (cap > max_out) cap = max_out;
    uint8_t *out = malloc(cap);
    if (!out) { xz_dec_end(d); say(err, ecap, "not enough memory to unpack"); return NULL; }
    struct xz_buf b = { .in = s, .in_pos = 0, .in_size = n, .out = out, .out_pos = 0, .out_size = cap };
    for (;;) {
        size_t full = b.out_size;
        if (full - b.out_pos > (1u << 20)) b.out_size = b.out_pos + (1u << 20);   /* a megabyte at a time */
        enum xz_ret r = xz_dec_catrun(d, &b, 1);
        bool chunk_full = r == XZ_OK && b.out_pos == b.out_size && b.out_size < full;
        b.out_size = full;
        unpack_yield();
        if (r == XZ_STREAM_END) break;
        if (chunk_full) continue;
        if (r == XZ_OK && b.out_pos == b.out_size) {
            if (cap >= max_out) { say(err, ecap, "the unpacked data is too large"); goto fail; }
            uint32_t ncap = cap > max_out / 2 ? max_out : cap * 2;
            uint8_t *no = malloc(ncap);
            if (!no) { say(err, ecap, "not enough memory to unpack"); goto fail; }
            memcpy(no, out, b.out_pos); free(out); out = no; cap = ncap;
            b.out = out; b.out_size = cap;
            continue;
        }
        if (r == XZ_OK) continue;
        say(err, ecap, r == XZ_MEMLIMIT_ERROR ? "the xz data needs a dictionary over 128 MB" :
                       r == XZ_OPTIONS_ERROR ? "the xz data uses options this decoder lacks" :
                       r == XZ_MEM_ERROR ? "not enough memory to unpack" : "the xz data is damaged");
        goto fail;
    }
    xz_dec_end(d);
    *outlen = (uint32_t)b.out_pos;
    return out;
fail:
    xz_dec_end(d); free(out);
    return NULL;
}

/* The kernel is not preempted: long decodes give other tasks (the desktop) a turn */
uint32_t timer_ticks(void);
void task_yield(void);
static uint32_t last_yield;
void unpack_yield(void) { uint32_t t = timer_ticks(); if (t - last_yield >= 15) { last_yield = t; task_yield(); } }

/* The reference zstd decoder stops the program on an error (exit(1)): here exit is
 * zstd_dec_fail (Makefile), which jumps back into unzstd. It keeps its state in
 * globals, so one decode runs at a time. */
static void *zstd_jmp[5];
static int zstd_active;
void zstd_dec_fail(int code) { (void)code; if (zstd_active) __builtin_longjmp(zstd_jmp, 1); for (;;) ; }
static uint8_t *unzstd(const uint8_t *s, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap) {
    static uint8_t *volatile out;
    static volatile uint32_t cap;
    while (zstd_active) task_yield();                 /* another task is decoding */
    out = NULL;
    size_t known = (size_t)-1;
    zstd_active = 1;
    if (!__builtin_setjmp(zstd_jmp)) known = ZSTD_get_decompressed_size(s, n);
    /* the size in the first frame's header is a guess when there are several frames */
    cap = known != (size_t)-1 && known >= n && known <= max_out ? (uint32_t)known : (n * 5 < (1u << 20) ? (1u << 20) : n * 5);
    for (int tries = 0; tries < 8; tries++) {
        if (cap > max_out) cap = max_out;
        out = malloc(cap ? cap : 1);
        if (!out) break;
        if (!__builtin_setjmp(zstd_jmp)) {
            size_t got = ZSTD_decompress(out, cap, s, n);
            zstd_active = 0;
            *outlen = (uint32_t)got;
            return out;
        }
        /* failed: damaged, or the output did not fit; memory the decoder held is lost */
        free(out); out = NULL;
        if (cap >= max_out) break;
        cap = cap > max_out / 2 ? max_out : cap * 2;
    }
    zstd_active = 0;
    say(err, ecap, "the Zstandard data is damaged or too large");
    return NULL;
}

uint8_t *unpack_all(const uint8_t *src, uint32_t n, uint32_t max_out, uint32_t *outlen, char *err, int ecap) {
    switch (unpack_kind(src, n)) {
    case UNPACK_GZIP: return gunzip(src, n, max_out, outlen, err, ecap);
    case UNPACK_XZ: return unxz(src, n, max_out, outlen, err, ecap);
    case UNPACK_ZSTD: return unzstd(src, n, max_out, outlen, err, ecap);
    case UNPACK_LZMA: say(err, ecap, "LZMA (lzma_alone) packages are not supported; xz, gzip and Zstandard are"); return NULL;
    case UNPACK_BZIP2: say(err, ecap, "bzip2-compressed packages are not supported; xz, gzip and Zstandard are"); return NULL;
    default: {
        uint8_t *o = malloc(n ? n : 1);
        if (!o) { say(err, ecap, "not enough memory"); return NULL; }
        memcpy(o, src, n); *outlen = n; return o;
    }
    }
}
