/* HTTPS: TLS 1.0-1.2 client on top of our TCP, using BearSSL.
 * Server certificates are validated against the Mozilla root store compiled
 * into the kernel (trust_anchors.c) plus any certificates the user puts in
 * C:\Windows\Certificates (*.crt, *.cer, *.pem). */
#include "kernel.h"
#include "net.h"
#include "fs.h"
#include "bearssl.h"

extern const br_x509_trust_anchor builtin_tas[];
extern const unsigned builtin_tas_num;
#define USER_CERT_DIR "C:\\Windows\\Certificates"

struct tls_conn {
    tcb_t *t;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    char err[200];
    unsigned char buf[BR_SSL_BUFSIZE_BIDI];
};

/* ---------------- entropy ---------------- */
static br_sha256_context pool;
static bool pool_init;
static void pool_mix(const void *d, size_t n) { if (!pool_init) { br_sha256_init(&pool); pool_init = true; } br_sha256_update(&pool, d, n); }
void entropy_add(uint32_t v) { uint64_t t = rdtsc(); pool_mix(&t, sizeof t); pool_mix(&v, sizeof v); }
static bool rdrand32(uint32_t *v) {
    unsigned char ok;
    __asm__ volatile(".byte 0x0f, 0xc7, 0xf0; setc %1" : "=a"(*v), "=qm"(ok));   /* rdrand eax */
    return ok;
}
static void gather_seed(unsigned char out[32]) {
    rtc_time_t t; rtc_read(&t);
    pool_mix(&t, sizeof t);
    pool_mix(netif.mac, 6);
    uint32_t tk = timer_ticks(); pool_mix(&tk, 4);
    for (int i = 0; i < 64; i++) {                 /* timing jitter */
        uint64_t a = rdtsc();
        for (volatile int k = 0; k < 50 + (int)(a & 63); k++) {}
        uint64_t b = rdtsc(); uint32_t d = (uint32_t)(b - a); pool_mix(&d, 4);
    }
    if (cpu_has_rdrand()) for (int i = 0; i < 16; i++) { uint32_t v; if (rdrand32(&v)) pool_mix(&v, 4); }
    br_sha256_context c = pool;
    br_sha256_out(&c, out);
    pool_mix(out, 32);                             /* never hand out the same seed twice */
}

/* ---------------- trust anchors ---------------- */
static br_x509_trust_anchor *tas;
static size_t ntas;
static uint32_t tas_gen = 0xFFFFFFFF;
typedef struct { uint8_t *b; size_t len, cap; } dnbuf_t;
static void dn_append(void *ctx, const void *d, size_t n) {
    dnbuf_t *x = ctx;
    if (x->len + n > x->cap) { x->cap = (x->len + n) * 2; x->b = krealloc(x->b, x->cap); }
    memcpy(x->b + x->len, d, n); x->len += n;
}
static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1; }
static uint32_t b64_decode(const char *s, const char *e, uint8_t *out) {
    uint32_t n = 0, acc = 0; int bits = 0;
    for (; s < e; s++) { int v = b64v(*s); if (v < 0) continue; acc = (acc << 6) | v; bits += 6; if (bits >= 8) { bits -= 8; out[n++] = acc >> bits; } }
    return n;
}
static void *dup(const void *p, size_t n) { void *d = kmalloc(n ? n : 1); memcpy(d, p, n); return d; }
static bool add_user_cert(const uint8_t *der, uint32_t len) {
    dnbuf_t dn = { 0 };
    br_x509_decoder_context dc;
    br_x509_decoder_init(&dc, dn_append, &dn);
    br_x509_decoder_push(&dc, der, len);
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) { kfree(dn.b); return false; }
    br_x509_trust_anchor *ta = &tas[ntas];
    ta->dn.data = dn.b; ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = dup(pk->key.rsa.n, pk->key.rsa.nlen); ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.e = dup(pk->key.rsa.e, pk->key.rsa.elen); ta->pkey.key.rsa.elen = pk->key.rsa.elen;
    } else {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = dup(pk->key.ec.q, pk->key.ec.qlen); ta->pkey.key.ec.qlen = pk->key.ec.qlen;
    }
    ntas++;
    return true;
}
static uint32_t cert_dir_key(void) {
    vnode_t *d = vfs_resolve(USER_CERT_DIR, NULL);
    uint32_t k = 1;
    for (vnode_t *n = d ? d->child : NULL; n; n = n->next) { k = k * 31 + n->size; for (const char *p = n->name; *p; p++) k = k * 31 + (uint8_t)*p; }
    return k;
}
static void load_trust_anchors(void) {
    uint32_t key = cert_dir_key();
    if (tas && tas_gen == key) return;
    /* the previous array is intentionally kept: a handshake in another task may still use it */
    vnode_t *d = vfs_resolve(USER_CERT_DIR, NULL);
    int extra = 0;
    for (vnode_t *n = d ? d->child : NULL; n; n = n->next) extra += 4;
    tas = kmalloc(sizeof(br_x509_trust_anchor) * (builtin_tas_num + extra + 1));
    memcpy(tas, builtin_tas, sizeof(br_x509_trust_anchor) * builtin_tas_num);
    ntas = builtin_tas_num;
    for (vnode_t *n = d ? d->child : NULL; n; n = n->next) {
        const char *e = vfs_ext(n->name);
        if (n->dir || (strcasecmp(e, "crt") && strcasecmp(e, "cer") && strcasecmp(e, "pem") && strcasecmp(e, "der"))) continue;
        if (n->size > 10 && !memcmp(n->data, "-----BEGIN", 10)) {
            const char *p = (const char *)n->data, *end = p + n->size;
            int added = 0;
            while (added < 4 && (p = strstr(p, "-----BEGIN CERTIFICATE-----")) && p < end) {
                const char *q = strstr(p, "-----END CERTIFICATE-----");
                if (!q) break;
                uint8_t *der = kmalloc(q - p);
                uint32_t l = b64_decode(p + 27, q, der);
                if (add_user_cert(der, l)) added++;
                kfree(der);
                p = q + 25;
            }
        } else add_user_cert(n->data, n->size);
    }
    if (ntas > builtin_tas_num) klog("tls: %u user certificate(s) trusted from " USER_CERT_DIR, (unsigned)(ntas - builtin_tas_num));
    tas_gen = key;
}
int tls_trust_anchor_count(void) { load_trust_anchors(); return ntas; }

/* ---------------- connection ---------------- */
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    tls_conn_t *c = ctx;
    int n = tcp_recv(c->t, buf, len, 30000);
    return n > 0 ? n : -1;
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    tls_conn_t *c = ctx;
    int n = tcp_send(c->t, buf, len);
    return n > 0 ? n : -1;
}
static uint32_t days_since_0ad(int y, int m, int d) {
    /* Howard Hinnant's days_from_civil (relative to 1970-01-01), shifted to 0000-01-01 */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + (int)doe - 719468) + 719528;
}
static const char *err_text(int e) {
    switch (e) {
    case BR_ERR_X509_NOT_TRUSTED: return "The website's security certificate is not issued by a trusted certificate authority.";
    case BR_ERR_X509_EXPIRED: return "The website's security certificate has expired or is not yet valid (check the computer's date and time).";
    case BR_ERR_X509_BAD_SERVER_NAME: return "The website's security certificate was issued for a different website.";
    case BR_ERR_X509_NOT_CA: return "A certificate in the website's chain is not allowed to issue certificates.";
    case BR_ERR_X509_BAD_SIGNATURE: return "The website's certificate has an invalid signature.";
    case BR_ERR_X509_WEAK_PUBLIC_KEY: return "The website's certificate uses a key that is too weak.";
    case BR_ERR_X509_UNSUPPORTED: case BR_ERR_X509_WRONG_KEY_TYPE: return "The website's certificate uses an unsupported algorithm.";
    case BR_ERR_X509_TIME_UNKNOWN: return "The current date is unknown, so the certificate cannot be checked.";
    case BR_ERR_UNSUPPORTED_VERSION: case BR_ERR_BAD_VERSION: return "The website requires a TLS version this browser does not support (TLS 1.3 only).";
    case BR_ERR_BAD_CIPHER_SUITE: return "The website and this browser have no encryption method in common.";
    case BR_ERR_IO: return "The connection was closed or timed out during the secure handshake.";
    case BR_ERR_NO_RANDOM: return "No random seed was available for encryption.";
    }
    if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT) return "The website refused the secure connection (TLS alert).";
    if (e >= BR_ERR_SEND_FATAL_ALERT) return "The secure connection was aborted.";
    return NULL;
}
static void set_err(tls_conn_t *c) {
    int e = br_ssl_engine_last_error(&c->sc.eng);
    const char *t = err_text(e);
    if (t) strlcpy(c->err, t, sizeof c->err); else snprintf(c->err, sizeof c->err, "Secure connection failed (TLS error %d).", e);
}
tls_conn_t *tls_open(tcb_t *t, const char *host, char *err, int errcap) {
    load_trust_anchors();
    tls_conn_t *c = kzalloc(sizeof *c);
    c->t = t;
    br_ssl_client_init_full(&c->sc, &c->xc, tas, ntas);
    rtc_time_t now; rtc_read(&now);
    br_x509_minimal_set_time(&c->xc, days_since_0ad(now.year, now.month, now.day), now.hour * 3600 + now.minute * 60 + now.second);
    br_ssl_engine_set_buffer(&c->sc.eng, c->buf, sizeof c->buf, 1);
    unsigned char seed[32]; gather_seed(seed);
    br_ssl_engine_inject_entropy(&c->sc.eng, seed, sizeof seed);
    memset(seed, 0, sizeof seed);
    if (!br_ssl_client_reset(&c->sc, host, 0)) { set_err(c); strlcpy(err, c->err, errcap); tls_close(c); return NULL; }
    br_sslio_init(&c->io, &c->sc.eng, sock_read, c, sock_write, c);
    br_sslio_flush(&c->io);                         /* runs the handshake */
    if (br_ssl_engine_current_state(&c->sc.eng) == BR_SSL_CLOSED) {
        set_err(c); strlcpy(err, c->err, errcap);
        klog("tls: handshake with %s failed: %s", host, c->err);
        tls_close(c);
        return NULL;
    }
    return c;
}
int tls_write(tls_conn_t *c, const void *data, uint32_t len) {
    if (br_sslio_write_all(&c->io, data, len) < 0 || br_sslio_flush(&c->io) < 0) { set_err(c); return -1; }
    return len;
}
int tls_read(tls_conn_t *c, void *buf, uint32_t max) {
    int n = br_sslio_read(&c->io, buf, max);
    if (n > 0) return n;
    int e = br_ssl_engine_last_error(&c->sc.eng);
    if (e == BR_ERR_OK || e == BR_ERR_IO) return 0;   /* close_notify or the server simply closed TCP */
    set_err(c);
    return -1;
}
void tls_close(tls_conn_t *c) {
    if (!c) return;
    if (c->t) tcp_close(c->t);
    memset(c, 0, sizeof *c);
    kfree(c);
}
const char *tls_error_text(tls_conn_t *c) { return c->err; }
const char *tls_cipher_name(tls_conn_t *c) {
    static char b[80];
    br_ssl_session_parameters p;
    br_ssl_engine_get_session_parameters(&c->sc.eng, &p);
    const char *v = p.version == BR_TLS12 ? "TLS 1.2" : p.version == BR_TLS11 ? "TLS 1.1" : "TLS 1.0";
    uint16_t cs = p.cipher_suite;
    const char *name = "";
    switch (cs) {
    case BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256: case BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256: name = "ECDHE, AES-128-GCM"; break;
    case BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384: case BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384: name = "ECDHE, AES-256-GCM"; break;
    case BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256: case BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256: name = "ECDHE, ChaCha20-Poly1305"; break;
    default: name = (cs >> 8) == 0xC0 ? "ECDHE" : "RSA"; break;
    }
    snprintf(b, sizeof b, "%s, %s", v, name);
    return b;
}
void sha256_hex(const void *data, uint32_t len, char out[65]) {
    br_sha256_context c; unsigned char h[32];
    br_sha256_init(&c); br_sha256_update(&c, data, len); br_sha256_out(&c, h);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}
