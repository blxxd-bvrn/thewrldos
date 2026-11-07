/* Kernel side of HTTPS: the entropy pool and the user's certificate folder,
 * for the Mbed TLS glue in src/tls (see tlsglue.h). */
#include "kernel.h"
#include "net.h"
#include "fs.h"
#include "tlsglue.h"

#define USER_CERT_DIR "C:\\WRLD\\Certificates"

/* ---- entropy: interrupt timing mixed into a pool; Mbed TLS conditions it ---- */
static uint32_t pool[32];
static unsigned pool_pos;
static inline uint32_t rotl(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
void entropy_add(uint32_t v) {
    uint32_t t = (uint32_t)rdtsc();
    unsigned i = pool_pos++ & 31;
    pool[i] = rotl(pool[i] ^ t, 7) + (v * 0x9E3779B1u) + pool[(i + 13) & 31];
}
static bool rdrand32(uint32_t *v) {
    unsigned char ok;
    __asm__ volatile(".byte 0x0f, 0xc7, 0xf0; setc %1" : "=a"(*v), "=qm"(ok));   /* rdrand eax */
    return ok;
}
void tlsk_entropy(unsigned char *out, unsigned len, unsigned *bits) {
    bool hw = cpu_has_rdrand();
    rtc_time_t t; rtc_read(&t);
    entropy_add(t.second | t.minute << 8 | t.hour << 16); entropy_add(timer_ticks());
    entropy_add(netif.mac[2] | netif.mac[3] << 8 | netif.mac[4] << 16 | netif.mac[5] << 24);
    for (unsigned i = 0; i < len; i += 4) {
        uint32_t w = 0;
        if (hw) { for (int k = 0; k < 10 && !rdrand32(&w); k++) {} }
        /* timing jitter of a short busy loop */
        for (int j = 0; j < 4; j++) {
            uint64_t a = rdtsc();
            for (volatile int k = 0; k < 40 + (int)(a & 31); k++) {}
            entropy_add((uint32_t)(rdtsc() - a));
        }
        w ^= pool[(i / 4) & 31] ^ rotl(pool[(i / 4 + 7) & 31], 11);
        for (unsigned b = 0; b < 4 && i + b < len; b++) out[i + b] = w >> (8 * b);
    }
    *bits = hw ? len * 8 : len * 2;     /* without RDRAND, claim 2 bits per byte of jitter */
}

/* ---- C:\WRLD\Certificates ---- */
unsigned tlsk_cert_key(void) {
    vnode_t *d = vfs_resolve(USER_CERT_DIR, NULL);
    uint32_t k = 1;
    for (vnode_t *n = d ? d->child : NULL; n; n = n->next) { k = k * 31 + n->size; for (const char *p = n->name; *p; p++) k = k * 31 + (uint8_t)*p; }
    return k;
}
void tlsk_cert_files(void (*cb)(void *ctx, const unsigned char *data, unsigned len, const char *name), void *ctx) {
    vnode_t *d = vfs_resolve(USER_CERT_DIR, NULL);
    for (vnode_t *n = d ? d->child : NULL; n; n = n->next) {
        const char *e = vfs_ext(n->name);
        if (n->dir || (strcasecmp(e, "crt") && strcasecmp(e, "cer") && strcasecmp(e, "pem") && strcasecmp(e, "der"))) continue;
        cb(ctx, n->data, n->size, n->name);
    }
}
