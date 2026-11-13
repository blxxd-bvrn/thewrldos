/* HTTPS for The WRLD Browser, wget and curl: TLS 1.3 and 1.2 through Mbed TLS.
 * This file is built against Mbed TLS and the C library shim, not the kernel
 * headers (see tlsglue.h); it implements the TLS API declared in net.h.
 * Server certificates are checked against the Mozilla root store compiled in
 * (root_certs.c) plus any certificates in C:\WRLD\Certificates. Roots are
 * parsed only when a server's chain names them as its issuer. */
/* the entropy hook uses SHA-256 directly (PSA is not running yet when it is called) */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"
#include "mbedtls/build_info.h"
#include "psa/crypto.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/platform_util.h"
#include "tlsglue.h"

/* ---- the kernel (net.h / kernel.h, restated with plain types) ---- */
typedef struct tcb tcb_t;
typedef struct tls_conn tls_conn_t;
tcb_t *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms, char *err, int errcap);
int tcp_send(tcb_t *t, const void *data, uint32_t len);
int tcp_recv(tcb_t *t, void *buf, uint32_t max, uint32_t timeout_ms);
void tcp_close(tcb_t *t);
void klog(const char *fmt, ...);

/* ---- the root store (root_certs.c) ---- */
extern const unsigned char wrld_root_der[];
extern const struct { uint32_t off, len, subj_off, subj_len; } wrld_roots[];
extern const unsigned wrld_root_count;

/* ---- platform hooks Mbed TLS asks for (see wrld_crypto_config.h) ---- */
/* Mbed TLS wants full-entropy bytes: the kernel's raw material (RDRAND and/or
 * timing jitter, rated at least 2 bits per byte) is gathered four times over
 * and compressed with SHA-256, 128 raw bytes into each 32 output bytes. */
int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                 unsigned char *output, size_t output_size) {
    (void)flags;
    unsigned char raw[128], h[32];
    for (size_t done = 0; done < output_size;) {
        unsigned bits = 0;
        tlsk_entropy(raw, sizeof raw, &bits);
        if (bits < 256) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        mbedtls_sha256(raw, sizeof raw, h, 0);
        size_t k = output_size - done < 32 ? output_size - done : 32;
        memcpy(output + done, h, k);
        done += k;
    }
    mbedtls_platform_zeroize(raw, sizeof raw); mbedtls_platform_zeroize(h, sizeof h);
    *estimate_bits = 8 * output_size;
    return 0;
}
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf) {
    time_t t = (time_t)*tt;
    return gmtime_r(&t, tm_buf);
}
mbedtls_ms_time_t mbedtls_ms_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (mbedtls_ms_time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- trusted certificates ---- */
typedef struct { unsigned char *der; size_t len, subj_off, subj_len; } user_cert_t;
static user_cert_t *ucerts;
static int nucerts;
static unsigned ucert_key = 0xFFFFFFFFu;

static void add_user_file(void *ctx, const unsigned char *data, unsigned len, const char *name) {
    (void)ctx;
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    /* PEM needs a terminating NUL that the file may not have */
    unsigned char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, data, len); copy[len] = 0;
    int pem = len > 10 && !memcmp(copy, "-----BEGIN", 10);
    int r = mbedtls_x509_crt_parse(&chain, copy, pem ? len + 1 : len);
    free(copy);
    if (r < 0) klog("tls: %s is not a certificate Mbed TLS can read (-0x%04x)", name, -r);
    for (mbedtls_x509_crt *c = &chain; c && c->raw.p; c = c->next) {
        user_cert_t *n = realloc(ucerts, sizeof *ucerts * (nucerts + 1));
        if (!n) break;
        ucerts = n;
        user_cert_t *u = &ucerts[nucerts];
        u->der = malloc(c->raw.len);
        if (!u->der) break;
        memcpy(u->der, c->raw.p, c->raw.len);
        u->len = c->raw.len;
        u->subj_off = c->subject_raw.p - c->raw.p; u->subj_len = c->subject_raw.len;
        nucerts++;
    }
    mbedtls_x509_crt_free(&chain);
}
static void load_user_certs(void) {
    unsigned key = tlsk_cert_key();
    if (key == ucert_key) return;
    for (int i = 0; i < nucerts; i++) free(ucerts[i].der);
    free(ucerts); ucerts = NULL; nucerts = 0;
    tlsk_cert_files(add_user_file, NULL);
    if (nucerts) klog("tls: %d user certificate(s) trusted from C:\\WRLD\\Certificates", nucerts);
    ucert_key = key;
}
/* Mbed TLS asks for the CAs that could have issued a certificate */
static int find_issuers(void *ctx, const mbedtls_x509_crt *child, mbedtls_x509_crt **out) {
    (void)ctx;
    mbedtls_x509_crt *chain = NULL;
    const unsigned char *want = child->issuer_raw.p;
    size_t wl = child->issuer_raw.len;
    for (unsigned i = 0; i < wrld_root_count + (unsigned)nucerts; i++) {
        const unsigned char *der, *subj; size_t len, sl;
        if (i < wrld_root_count) {
            der = wrld_root_der + wrld_roots[i].off; len = wrld_roots[i].len;
            subj = wrld_root_der + wrld_roots[i].subj_off; sl = wrld_roots[i].subj_len;
        } else {
            user_cert_t *u = &ucerts[i - wrld_root_count];
            der = u->der; len = u->len; subj = u->der + u->subj_off; sl = u->subj_len;
        }
        if (sl != wl || memcmp(subj, want, wl)) continue;
        if (!chain) {
            chain = mbedtls_calloc(1, sizeof *chain);
            if (!chain) break;
            mbedtls_x509_crt_init(chain);
        }
        mbedtls_x509_crt_parse_der(chain, der, len);
    }
    *out = chain;
    return 0;
}

/* ---- shared configuration ---- */
static mbedtls_ssl_config conf;
static int ready;
static const char *alpn[] = { "http/1.1", NULL };
static int init_once(char *err, int errcap) {
    if (ready) return 1;
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) { snprintf(err, errcap, "The encryption library could not start (PSA error %d).", (int)st); return 0; }
    mbedtls_ssl_config_init(&conf);
    int r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r) { snprintf(err, errcap, "TLS setup failed (-0x%04x).", -r); return 0; }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(&conf, find_issuers, NULL);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_alpn_protocols(&conf, alpn);
    ready = 1;
    klog("tls: Mbed TLS %s ready, %u root certificates", MBEDTLS_VERSION_STRING, wrld_root_count);
    return 1;
}

/* ---- connections ---- */
struct tls_conn {
    tcb_t *t;
    mbedtls_ssl_context ssl;
    int closed;
    char err[200];
    char info[96];
};
#define BIO_SEND_FAILED (-0x004E)
#define BIO_RECV_FAILED (-0x004C)
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    tls_conn_t *c = ctx;
    int n = tcp_send(c->t, buf, (uint32_t)len);
    return n > 0 ? n : BIO_SEND_FAILED;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    tls_conn_t *c = ctx;
    int n = tcp_recv(c->t, buf, (uint32_t)len, 30000);
    if (n > 0) return n;
    return n == 0 ? 0 : BIO_RECV_FAILED;                  /* 0: the server closed the connection */
}

static const char *alert_text(int a) {
    switch (a) {
    case MBEDTLS_SSL_ALERT_MSG_HANDSHAKE_FAILURE: return "The website and this browser have no encryption method in common (handshake failure).";
    case MBEDTLS_SSL_ALERT_MSG_PROTOCOL_VERSION: return "The website does not support TLS 1.2 or 1.3.";
    case MBEDTLS_SSL_ALERT_MSG_INSUFFICIENT_SECURITY: return "The website requires stronger security settings.";
    case MBEDTLS_SSL_ALERT_MSG_UNRECOGNIZED_NAME: return "The website does not recognise this site name.";
    case MBEDTLS_SSL_ALERT_MSG_BAD_CERT: return "The website rejected the secure connection (certificate problem).";
    }
    return "The website refused the secure connection (TLS alert).";
}
static void set_err(tls_conn_t *c, int r) {
    const char *t = NULL;
    if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        uint32_t f = mbedtls_ssl_get_verify_result(&c->ssl);
        if (f & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
            t = "The website's security certificate has expired or is not yet valid (check the computer's date and time).";
        else if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) t = "The website's security certificate was issued for a different website.";
        else if (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED) t = "The website's security certificate is not issued by a trusted certificate authority.";
        else if (f & (MBEDTLS_X509_BADCERT_BAD_KEY | MBEDTLS_X509_BADCERT_BAD_MD | MBEDTLS_X509_BADCERT_BAD_PK))
            t = "The website's certificate uses an algorithm or key that is too weak or unsupported.";
        else if (f & (MBEDTLS_X509_BADCERT_KEY_USAGE | MBEDTLS_X509_BADCERT_EXT_KEY_USAGE)) t = "The website's certificate may not be used for a web server.";
        else t = "The website's security certificate could not be verified.";
    } else if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) t = alert_text(mbedtls_ssl_get_fatal_alert(&c->ssl));
    else if (r == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION) t = "The website does not support TLS 1.2 or 1.3.";
    else if (r == MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE) t = "The website and this browser have no encryption method in common.";
    else if (r == BIO_RECV_FAILED || r == BIO_SEND_FAILED || r == MBEDTLS_ERR_SSL_CONN_EOF)
        t = "The connection was closed or timed out during the secure handshake.";
    if (t) { snprintf(c->err, sizeof c->err, "%s", t); return; }
    char e[120];
    mbedtls_strerror(r, e, sizeof e);
    snprintf(c->err, sizeof c->err, "Secure connection failed: %s (-0x%04x).", e, -r);
}
static void describe(tls_conn_t *c) {
    const char *v = mbedtls_ssl_get_version(&c->ssl), *cs = mbedtls_ssl_get_ciphersuite(&c->ssl);
    const char *ver = !strcmp(v, "TLSv1.3") ? "TLS 1.3" : !strcmp(v, "TLSv1.2") ? "TLS 1.2" : v;
    const char *cipher = strstr(cs, "CHACHA20") ? "ChaCha20-Poly1305" : strstr(cs, "AES-256-GCM") ? "AES-256-GCM"
                       : strstr(cs, "AES-128-GCM") ? "AES-128-GCM" : strstr(cs, "AES-256") ? "AES-256" : strstr(cs, "AES-128") ? "AES-128" : cs;
    snprintf(c->info, sizeof c->info, "%s, %s%s, Mbed TLS %s", ver, strstr(cs, "ECDHE") ? "ECDHE, " : "", cipher, MBEDTLS_VERSION_STRING);
}

tls_conn_t *tls_open(tcb_t *t, const char *host, uint32_t ip, uint16_t port, char *err, int errcap) {
    (void)ip; (void)port;
    if (!init_once(err, errcap)) { tcp_close(t); return NULL; }
    load_user_certs();
    tls_conn_t *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errcap, "Out of memory."); tcp_close(t); return NULL; }
    c->t = t;
    mbedtls_ssl_init(&c->ssl);
    int r = mbedtls_ssl_setup(&c->ssl, &conf);
    if (!r) r = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (!r) {
        mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);
        do r = mbedtls_ssl_handshake(&c->ssl);
        while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (r) {
        set_err(c, r);
        snprintf(err, errcap, "%s", c->err);
        klog("tls: handshake with %s failed: %s", host, c->err);
        mbedtls_ssl_free(&c->ssl);
        tcp_close(t);
        free(c);
        return NULL;
    }
    describe(c);
    klog("tls: %s: %s", host, c->info);
    return c;
}
int tls_write(tls_conn_t *c, const void *data, uint32_t len) {
    const unsigned char *p = data;
    uint32_t done = 0;
    while (done < len) {
        int r = mbedtls_ssl_write(&c->ssl, p + done, len - done);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r < 0) { set_err(c, r); return -1; }
        done += r;
    }
    return (int)len;
}
int tls_read(tls_conn_t *c, void *buf, uint32_t max) {
    if (c->closed) return 0;
    for (;;) {
        int r = mbedtls_ssl_read(&c->ssl, buf, max);
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) { c->closed = 1; return 0; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        if (r == BIO_RECV_FAILED) { c->closed = 1; return 0; }      /* the server dropped the connection after its reply */
        set_err(c, r);
        return -1;
    }
}
void tls_close(tls_conn_t *c) {
    if (!c) return;
    if (!c->closed) mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    if (c->t) tcp_close(c->t);
    memset(c, 0, sizeof *c);
    free(c);
}
const char *tls_error_text(tls_conn_t *c) { return c->err; }
const char *tls_cipher_name(tls_conn_t *c) { return c->info; }
int tls_trust_anchor_count(void) { load_user_certs(); return (int)wrld_root_count + nucerts; }
const char *tls_library_version(void) { return "Mbed TLS " MBEDTLS_VERSION_STRING; }

void sha256_hex(const void *data, uint32_t len, char out[65]) {
    unsigned char h[32]; size_t hl = 0;
    char e[8];
    if (!init_once(e, sizeof e) || psa_hash_compute(PSA_ALG_SHA_256, data, len, h, sizeof h, &hl) != PSA_SUCCESS) { strcpy(out, "?"); return; }
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}
