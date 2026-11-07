/* TLS 1.3 client (tls13.c), used by tls.c before falling back to BearSSL's TLS 1.2 */
#ifndef TLS13_H
#define TLS13_H
#include "bearssl.h"

typedef struct tls13 tls13_t;
/* Runs the handshake on an open TCP connection. Returns NULL on failure; then
 * *fallback is set when the server simply does not speak TLS 1.3, so the
 * caller should reconnect and try TLS 1.2. The TCP connection is not closed. */
tls13_t *tls13_connect(tcb_t *t, const char *host, const br_x509_trust_anchor *tas, size_t ntas,
                       uint32_t days, uint32_t secs, char *err, int errcap, int *fallback);
int tls13_write(tls13_t *c, const void *data, uint32_t len);     /* -1 on error */
int tls13_read(tls13_t *c, void *buf, uint32_t max);              /* >0, 0 closed, -1 error */
void tls13_free(tls13_t *c);                                      /* sends close_notify; TCP stays open */
const char *tls13_info(tls13_t *c);                               /* "TLS 1.3, AES-128-GCM, X25519" */
const char *tls13_error(tls13_t *c);

/* from tls.c */
void tls_random(void *out, size_t len);
const char *tls_x509_error_text(int err);
#endif
