/* Interface between the kernel and the Mbed TLS glue (src/tls). The two sides
 * are compiled against different headers (the kernel's bool is an int; the
 * glue sees the C library shim), so only plain C types cross. The TLS API the
 * rest of the OS uses (tls_open, tls_read, ...) is declared in net.h. */
#ifndef TLSGLUE_H
#define TLSGLUE_H

/* implemented by the kernel (src/net/tlsk.c) */
/* fills out[len] with seed material; *bits = how much real entropy it holds */
void tlsk_entropy(unsigned char *out, unsigned len, unsigned *bits);
/* changes whenever C:\WRLD\Certificates changes */
unsigned tlsk_cert_key(void);
/* calls cb for every .crt/.cer/.pem/.der file in C:\WRLD\Certificates */
void tlsk_cert_files(void (*cb)(void *ctx, const unsigned char *data, unsigned len, const char *name), void *ctx);
#endif
