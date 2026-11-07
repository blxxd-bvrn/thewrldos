/* TLS 1.3 client (RFC 8446), built on BearSSL's primitives: X25519 / P-256 /
 * P-384 key exchange, AES-GCM and ChaCha20-Poly1305 record protection, SHA-256
 * and SHA-384 key schedules, and BearSSL's X.509 engine for the certificate
 * chain. RSA-PSS signature checking (which BearSSL 0.6 lacks) is done here on
 * top of the raw RSA public-key operation.
 * Not supported (not needed by a browser): session resumption / PSK, 0-RTT,
 * client certificates (an empty Certificate is sent if one is requested). */
#include "kernel.h"
#include "net.h"
#include "tls13.h"

#define MAXREC 16384
#define MAXCIPHER (MAXREC + 256)

typedef struct {
    bool on;
    uint8_t key[32], iv[12], secret[48];
    uint64_t seq;
    br_aes_ct_ctr_keys aes;
    br_gcm_context gcm;
} keys_t;

struct tls13 {
    tcb_t *t;
    char host[256];
    char err[200];
    char info[64];
    /* negotiated */
    uint16_t suite, group;
    const br_hash_class *hc;
    size_t hlen, klen;
    br_hash_compat_context th;               /* transcript hash */
    /* key exchange */
    uint8_t random[32], sid[32];
    uint8_t kpriv[48], kpub[97];
    size_t kprivlen, kpublen;
    /* certificate checking */
    br_x509_minimal_context xc;
    br_x509_pkey pkey;
    uint8_t pkbuf[1024];                     /* copy of the server key's modulus/exponent or point */
    bool have_pkey;
    /* records */
    keys_t rk, wk;
    uint8_t ibuf[4096]; size_t ioff, ilen;   /* TCP staging */
    uint8_t rec[5 + MAXCIPHER];              /* one incoming record */
    uint8_t out[5 + MAXREC + 1 + 16];        /* one outgoing record */
    uint8_t *hs; size_t hs_len, hs_cap;      /* handshake message reassembly */
    uint8_t *pt; size_t pt_off, pt_len;      /* decrypted application data not yet read */
    bool closed, failed;
};

/* ---------------- helpers ---------------- */
static void fail(tls13_t *c, const char *msg) { if (!c->err[0]) strlcpy(c->err, msg, sizeof c->err); c->failed = true; }
static inline void put16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static inline void put24(uint8_t *p, unsigned v) { p[0] = v >> 16; p[1] = v >> 8; p[2] = v; }

typedef struct { const uint8_t *p, *e; bool bad; } rd_t;
static unsigned r8(rd_t *r) { if (r->p + 1 > r->e) { r->bad = true; return 0; } return *r->p++; }
static unsigned r16(rd_t *r) { if (r->p + 2 > r->e) { r->bad = true; return 0; } unsigned v = r->p[0] << 8 | r->p[1]; r->p += 2; return v; }
static unsigned r24(rd_t *r) { if (r->p + 3 > r->e) { r->bad = true; return 0; } unsigned v = r->p[0] << 16 | r->p[1] << 8 | r->p[2]; r->p += 3; return v; }
static const uint8_t *rbytes(rd_t *r, size_t n) { if ((size_t)(r->e - r->p) < n) { r->bad = true; return NULL; } const uint8_t *q = r->p; r->p += n; return q; }

/* ---------------- key schedule ---------------- */
static void hmac(tls13_t *c, const void *key, size_t klen, const void *a, size_t alen, const void *b, size_t blen, uint8_t *out) {
    br_hmac_key_context kc; br_hmac_context h;
    br_hmac_key_init(&kc, c->hc, key, klen);
    br_hmac_init(&h, &kc, 0);
    br_hmac_update(&h, a, alen);
    if (blen) br_hmac_update(&h, b, blen);
    br_hmac_out(&h, out);
}
static void hkdf_extract(tls13_t *c, const uint8_t *salt, const uint8_t *ikm, size_t ikmlen, uint8_t *out) {
    uint8_t zero[48] = { 0 };
    hmac(c, salt ? salt : zero, c->hlen, ikm ? ikm : zero, ikm ? ikmlen : c->hlen, NULL, 0, out);
}
static void expand_label(tls13_t *c, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t ctxlen, uint8_t *out, size_t outlen) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    put16(info, outlen); n = 2;
    info[n++] = 6 + ll; memcpy(info + n, "tls13 ", 6); n += 6; memcpy(info + n, label, ll); n += ll;
    info[n++] = ctxlen; if (ctxlen) { memcpy(info + n, ctx, ctxlen); n += ctxlen; }
    uint8_t t[64]; size_t tlen = 0, done = 0;
    for (uint8_t i = 1; done < outlen; i++) {
        br_hmac_key_context kc; br_hmac_context h;
        br_hmac_key_init(&kc, c->hc, secret, c->hlen);
        br_hmac_init(&h, &kc, 0);
        br_hmac_update(&h, t, tlen); br_hmac_update(&h, info, n); br_hmac_update(&h, &i, 1);
        br_hmac_out(&h, t); tlen = c->hlen;
        size_t k = MIN(tlen, outlen - done);
        memcpy(out + done, t, k); done += k;
    }
}
static void transcript(tls13_t *c, uint8_t *out) {
    br_hash_compat_context cp = c->th;
    c->hc->out(&cp.vtable, out);
}
static void hash_of(tls13_t *c, const void *d, size_t n, uint8_t *out) {
    br_hash_compat_context h;
    c->hc->init(&h.vtable); c->hc->update(&h.vtable, d, n); c->hc->out(&h.vtable, out);
}
static void derive(tls13_t *c, const uint8_t *secret, const char *label, const uint8_t *thash, uint8_t *out) {
    uint8_t empty[64];
    if (!thash) { hash_of(c, "", 0, empty); thash = empty; }
    expand_label(c, secret, label, thash, c->hlen, out, c->hlen);
}
static void set_keys(tls13_t *c, keys_t *k, const uint8_t *secret) {
    memcpy(k->secret, secret, c->hlen);
    expand_label(c, secret, "key", NULL, 0, k->key, c->klen);
    expand_label(c, secret, "iv", NULL, 0, k->iv, 12);
    k->seq = 0;
    if (c->suite != 0x1303) {
        br_aes_ct_ctr_init(&k->aes, k->key, c->klen);
        br_gcm_init(&k->gcm, &k->aes.vtable, br_ghash_ctmul);
    }
    k->on = true;
}

/* ---------------- record protection ---------------- */
static bool aead(tls13_t *c, keys_t *k, bool enc, const uint8_t *hdr, uint8_t *data, size_t len, uint8_t *tag) {
    uint8_t nonce[12];
    memcpy(nonce, k->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(k->seq >> (8 * i));
    k->seq++;
    if (c->suite == 0x1303) {
        uint8_t t[16];
        br_poly1305_ctmul_run(k->key, nonce, data, len, hdr, 5, t, br_chacha20_ct_run, enc);
        if (enc) { memcpy(tag, t, 16); return true; }
        uint8_t d = 0; for (int i = 0; i < 16; i++) d |= t[i] ^ tag[i];
        return d == 0;
    }
    br_gcm_reset(&k->gcm, nonce, 12);
    br_gcm_aad_inject(&k->gcm, hdr, 5);
    br_gcm_flip(&k->gcm);
    br_gcm_run(&k->gcm, enc, data, len);
    if (enc) { br_gcm_get_tag(&k->gcm, tag); return true; }
    return br_gcm_check_tag(&k->gcm, tag);
}

static int rd_exact(tls13_t *c, uint8_t *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (c->ioff == c->ilen) {
            int r = tcp_recv(c->t, c->ibuf, sizeof c->ibuf, 30000);
            if (r <= 0) return r == 0 ? 0 : -1;
            c->ioff = 0; c->ilen = r;
        }
        size_t k = MIN(n - got, c->ilen - c->ioff);
        memcpy(dst + got, c->ibuf + c->ioff, k);
        c->ioff += k; got += k;
    }
    return 1;
}

/* next record: returns its content type (decrypted when keys are on), 0 = TCP closed, -1 error */
static int rec_read(tls13_t *c, uint8_t **data, size_t *len) {
    for (;;) {
        int r = rd_exact(c, c->rec, 5);
        if (r <= 0) { if (r < 0) fail(c, "The connection was closed or timed out during the secure handshake."); return r; }
        size_t n = c->rec[3] << 8 | c->rec[4];
        if (n > MAXCIPHER) { fail(c, "The website sent a malformed secure record."); return -1; }
        if ((r = rd_exact(c, c->rec + 5, n)) <= 0) { fail(c, "The connection was closed in the middle of a secure record."); return -1; }
        uint8_t type = c->rec[0], *p = c->rec + 5;
        if (type == 20) continue;                                 /* middlebox-compatibility ChangeCipherSpec */
        if (c->rk.on) {
            if (type != 23 || n < 17) { fail(c, "The website sent an unprotected record after the handshake began."); return -1; }
            if (!aead(c, &c->rk, false, c->rec, p, n - 16, p + n - 16)) { fail(c, "A secure record failed its integrity check (bad record MAC)."); return -1; }
            n -= 16;
            while (n && !p[n - 1]) n--;                               /* padding */
            if (!n) { fail(c, "The website sent a malformed secure record."); return -1; }
            type = p[--n];
        }
        *data = p; *len = n;
        return type;
    }
}

static bool rec_write(tls13_t *c, uint8_t type, const uint8_t *data, size_t len) {
    do {
        size_t n = MIN(len, (size_t)MAXREC);
        uint8_t *o = c->out;
        if (!c->wk.on) {
            o[0] = type; o[1] = 3; o[2] = type == 22 ? 1 : 3; put16(o + 3, n);
            memcpy(o + 5, data, n);
            if (tcp_send(c->t, o, 5 + n) < 0) return false;
        } else {
            o[0] = 23; o[1] = 3; o[2] = 3; put16(o + 3, n + 1 + 16);
            memcpy(o + 5, data, n); o[5 + n] = type;
            aead(c, &c->wk, true, o, o + 5, n + 1, o + 5 + n + 1);
            if (tcp_send(c->t, o, 5 + n + 1 + 16) < 0) return false;
        }
        data += n; len -= n;
    } while (len);
    return true;
}

static void send_alert(tls13_t *c, uint8_t desc) {
    uint8_t a[2] = { desc ? 2 : 1, desc };
    rec_write(c, 21, a, 2);
}

static const char *alert_text(unsigned d) {
    switch (d) {
    case 40: return "The website refused the secure connection (handshake failure).";
    case 42: case 43: case 44: case 45: case 46: case 48: return "The website rejected the secure connection (certificate problem).";
    case 70: return "The website does not support this TLS version.";
    case 71: return "The website requires stronger security settings.";
    case 112: return "The website does not recognise this site name (SNI).";
    }
    return "The website refused the secure connection (TLS alert).";
}

/* the next complete handshake message (with its 4-byte header); returns its type or -1 */
static int hs_next(tls13_t *c, const uint8_t **msg, size_t *mlen, int *alert) {
    for (;;) {
        if (c->hs_len >= 4) {
            size_t L = 4 + (c->hs[1] << 16 | c->hs[2] << 8 | c->hs[3]);
            if (L > 262144 + 4) { fail(c, "The website sent an oversized handshake message."); return -1; }
            if (c->hs_len >= L) { *msg = c->hs; *mlen = L; return c->hs[0]; }
        }
        uint8_t *d; size_t n;
        int type = rec_read(c, &d, &n);
        if (type <= 0) { if (!type) fail(c, "The connection was closed during the secure handshake."); return -1; }
        if (type == 21) {
            if (alert) *alert = n >= 2 ? d[1] : 0;
            fail(c, alert_text(n >= 2 ? d[1] : 0));
            return -1;
        }
        if (type != 22) { fail(c, "The website sent unexpected data during the secure handshake."); return -1; }
        if (c->hs_len + n > c->hs_cap) { c->hs_cap = c->hs_len + n + 4096; c->hs = krealloc(c->hs, c->hs_cap); }
        memcpy(c->hs + c->hs_len, d, n); c->hs_len += n;
    }
}
static void hs_consume(tls13_t *c, size_t n) { memmove(c->hs, c->hs + n, c->hs_len - n); c->hs_len -= n; }
static void th_add(tls13_t *c, const void *d, size_t n) { c->hc->update(&c->th.vtable, d, n); }

/* ---------------- key shares ---------------- */
static bool make_share(tls13_t *c, uint16_t group) {
    const br_ec_impl *ec = &br_ec_all_m31;
    int curve; size_t plen;
    switch (group) {
    case 0x001D: curve = BR_EC_curve25519; plen = 32; break;
    case 0x0017: curve = BR_EC_secp256r1; plen = 32; break;
    case 0x0018: curve = BR_EC_secp384r1; plen = 48; break;
    default: return false;
    }
    tls_random(c->kpriv, plen);
    if (curve != BR_EC_curve25519) { c->kpriv[0] &= 0x7F; c->kpriv[plen - 1] |= 1; }   /* 0 < key < n */
    c->kprivlen = plen;
    c->kpublen = ec->mulgen(c->kpub, c->kpriv, plen, curve);
    c->group = group;
    return c->kpublen != 0;
}
static bool shared_secret(tls13_t *c, const uint8_t *peer, size_t plen, uint8_t *out, size_t *outlen) {
    const br_ec_impl *ec = &br_ec_all_m31;
    uint8_t pt[97];
    if (plen != c->kpublen || plen > sizeof pt) return false;
    memcpy(pt, peer, plen);
    int curve = c->group == 0x001D ? BR_EC_curve25519 : c->group == 0x0017 ? BR_EC_secp256r1 : BR_EC_secp384r1;
    if (!ec->mul(pt, plen, c->kpriv, c->kprivlen, curve)) return false;
    if (curve == BR_EC_curve25519) {
        uint8_t z = 0; for (int i = 0; i < 32; i++) z |= pt[i];
        if (!z) return false;                                     /* low-order point */
        memcpy(out, pt, 32); *outlen = 32;
    } else {
        size_t xl = (plen - 1) / 2;
        memcpy(out, pt + 1, xl); *outlen = xl;
    }
    return true;
}

/* ---------------- ClientHello ---------------- */
static size_t build_hello(tls13_t *c, uint8_t *b, const uint8_t *cookie, size_t cookielen) {
    size_t n = 4;
    put16(b + n, 0x0303); n += 2;
    memcpy(b + n, c->random, 32); n += 32;
    b[n++] = 32; memcpy(b + n, c->sid, 32); n += 32;
    static const uint8_t suites[] = { 0x13, 0x01, 0x13, 0x03, 0x13, 0x02 };
    put16(b + n, sizeof suites); n += 2; memcpy(b + n, suites, sizeof suites); n += sizeof suites;
    b[n++] = 1; b[n++] = 0;                                       /* null compression */
    size_t ext = n; n += 2;
    /* server_name (an IP address is not sent) */
    bool ip = true;
    for (const char *s = c->host; *s; s++) if (!(isdigit(*s) || *s == '.')) ip = false;
    size_t hl = strlen(c->host);
    if (!ip && hl) {
        put16(b + n, 0); put16(b + n + 2, hl + 5); put16(b + n + 4, hl + 3); b[n + 6] = 0; put16(b + n + 7, hl);
        memcpy(b + n + 9, c->host, hl); n += 9 + hl;
    }
    static const uint8_t fixed[] = {
        0x00, 0x0A, 0x00, 0x08, 0x00, 0x06, 0x00, 0x1D, 0x00, 0x17, 0x00, 0x18,           /* supported_groups */
        0x00, 0x0D, 0x00, 0x14, 0x00, 0x12, 0x04, 0x03, 0x08, 0x04, 0x05, 0x03, 0x08, 0x05, /* signature_algorithms */
        0x08, 0x06, 0x06, 0x03, 0x04, 0x01, 0x05, 0x01, 0x06, 0x01,
        0x00, 0x2B, 0x00, 0x03, 0x02, 0x03, 0x04,                                           /* supported_versions: 1.3 */
        0x00, 0x2D, 0x00, 0x02, 0x01, 0x01,                                                 /* psk_key_exchange_modes */
        0x00, 0x10, 0x00, 0x0B, 0x00, 0x09, 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1',   /* ALPN */
    };
    memcpy(b + n, fixed, sizeof fixed); n += sizeof fixed;
    /* key_share */
    put16(b + n, 51); put16(b + n + 2, c->kpublen + 6); put16(b + n + 4, c->kpublen + 4);
    put16(b + n + 6, c->group); put16(b + n + 8, c->kpublen); memcpy(b + n + 10, c->kpub, c->kpublen);
    n += 10 + c->kpublen;
    if (cookie) { put16(b + n, 44); put16(b + n + 2, cookielen); memcpy(b + n + 4, cookie, cookielen); n += 4 + cookielen; }
    put16(b + ext, n - ext - 2);
    b[0] = 1; put24(b + 1, n - 4);
    return n;
}

/* ---------------- ServerHello ---------------- */
static const uint8_t hrr_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };

typedef struct { bool tls13, hrr, sid_ok; uint16_t suite, group; const uint8_t *share; size_t sharelen; const uint8_t *cookie; size_t cookielen; } sh_t;
static bool parse_sh(tls13_t *c, const uint8_t *m, size_t len, sh_t *s) {
    memset(s, 0, sizeof *s);
    rd_t r = { m + 4, m + len, false };
    r16(&r);
    const uint8_t *rnd = rbytes(&r, 32);
    unsigned sidl = r8(&r);
    const uint8_t *sid = rbytes(&r, sidl);
    s->sid_ok = sid && sidl == 32 && !memcmp(sid, c->sid, 32);
    s->suite = r16(&r);
    r8(&r);
    if (r.bad || !rnd) return false;
    s->hrr = !memcmp(rnd, hrr_random, 32);
    if (r.p == r.e) return true;                                  /* no extensions: TLS 1.2 or older */
    unsigned el = r16(&r);
    rd_t e = { r.p, r.p + el, false };
    if (r.p + el > r.e) return false;
    while (e.p < e.e && !e.bad) {
        unsigned type = r16(&e), l = r16(&e);
        const uint8_t *d = rbytes(&e, l);
        if (!d) break;
        rd_t x = { d, d + l, false };
        if (type == 43) s->tls13 = r16(&x) == 0x0304;
        else if (type == 51) {
            s->group = r16(&x);
            if (!s->hrr) { s->sharelen = r16(&x); s->share = rbytes(&x, s->sharelen); }
        } else if (type == 44) { s->cookielen = r16(&x); s->cookie = rbytes(&x, s->cookielen); }
    }
    return !e.bad;
}

/* ---------------- signatures ---------------- */
static void mgf1_xor(const br_hash_class *hc, const uint8_t *seed, size_t slen, uint8_t *out, size_t olen) {
    size_t hl = (hc->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    uint8_t h[64];
    for (uint32_t ctr = 0, done = 0; done < olen; ctr++) {
        br_hash_compat_context hx; uint8_t cb[4] = { ctr >> 24, ctr >> 16, ctr >> 8, ctr };
        hc->init(&hx.vtable); hc->update(&hx.vtable, seed, slen); hc->update(&hx.vtable, cb, 4); hc->out(&hx.vtable, h);
        for (size_t i = 0; i < hl && done < olen; i++) out[done++] ^= h[i];
    }
}
/* RSASSA-PSS verification (RFC 8017 9.1.2), salt length = hash length */
static bool rsa_pss_verify(const br_rsa_public_key *pk, const br_hash_class *hc, const uint8_t *mhash, const uint8_t *sig, size_t siglen) {
    size_t hl = (hc->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    const uint8_t *n = pk->n; size_t nlen = pk->nlen;
    while (nlen && !*n) { n++; nlen--; }
    if (!nlen || siglen != nlen || nlen > 512) return false;
    unsigned top = 8; while (top && !(n[0] & (1u << (top - 1)))) top--;
    size_t modbits = (nlen - 1) * 8 + top, emlen = (modbits - 1 + 7) / 8;
    uint8_t x[512];
    memcpy(x, sig, siglen);
    if (!br_rsa_i31_public(x, siglen, pk)) return false;
    uint8_t *em = x + (siglen - emlen);
    if (siglen > emlen && x[0]) return false;
    if (emlen < 2 * hl + 2 || em[emlen - 1] != 0xBC) return false;
    size_t dblen = emlen - hl - 1;
    uint8_t *db = em, *h = em + dblen;
    unsigned unused = 8 * emlen - (modbits - 1);
    if (unused && (db[0] >> (8 - unused))) return false;
    mgf1_xor(hc, h, hl, db, dblen);
    if (unused) db[0] &= 0xFF >> unused;
    size_t ps = dblen - hl - 1;
    for (size_t i = 0; i < ps; i++) if (db[i]) return false;
    if (db[ps] != 0x01) return false;
    br_hash_compat_context hx; uint8_t zeros[8] = { 0 }, h2[64];
    hc->init(&hx.vtable); hc->update(&hx.vtable, zeros, 8); hc->update(&hx.vtable, mhash, hl);
    hc->update(&hx.vtable, db + ps + 1, hl); hc->out(&hx.vtable, h2);
    return !memcmp(h2, h, hl);
}
static bool verify_cv(tls13_t *c, unsigned alg, const uint8_t *sig, size_t siglen, const uint8_t *thash) {
    uint8_t content[64 + 34 + 64];
    memset(content, 0x20, 64);
    memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
    content[97] = 0;
    memcpy(content + 98, thash, c->hlen);
    size_t clen = 98 + c->hlen;
    const br_hash_class *hc;
    bool ecdsa;
    switch (alg) {
    case 0x0403: hc = &br_sha256_vtable; ecdsa = true; break;      /* ecdsa_secp256r1_sha256 */
    case 0x0503: hc = &br_sha384_vtable; ecdsa = true; break;      /* ecdsa_secp384r1_sha384 */
    case 0x0603: hc = &br_sha512_vtable; ecdsa = true; break;      /* ecdsa_secp521r1_sha512 */
    case 0x0804: hc = &br_sha256_vtable; ecdsa = false; break;     /* rsa_pss_rsae_sha256 */
    case 0x0805: hc = &br_sha384_vtable; ecdsa = false; break;
    case 0x0806: hc = &br_sha512_vtable; ecdsa = false; break;
    default: return false;
    }
    uint8_t h[64]; br_hash_compat_context hx;
    hc->init(&hx.vtable); hc->update(&hx.vtable, content, clen); hc->out(&hx.vtable, h);
    size_t hl = (hc->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
    if (ecdsa) return c->pkey.key_type == BR_KEYTYPE_EC &&
                      br_ecdsa_vrfy_asn1_get_default()(br_ec_get_default(), h, hl, &c->pkey.key.ec, sig, siglen) == 1;
    return c->pkey.key_type == BR_KEYTYPE_RSA && rsa_pss_verify(&c->pkey.key.rsa, hc, h, sig, siglen);
}

/* keep a copy of the server's public key (the X.509 engine's buffers are reused) */
static void save_pkey(tls13_t *c, const br_x509_pkey *pk) {
    c->pkey = *pk;
    if (pk->key_type == BR_KEYTYPE_RSA && pk->key.rsa.nlen + pk->key.rsa.elen <= sizeof c->pkbuf) {
        memcpy(c->pkbuf, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(c->pkbuf + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
        c->pkey.key.rsa.n = c->pkbuf; c->pkey.key.rsa.e = c->pkbuf + pk->key.rsa.nlen;
        c->have_pkey = true;
    } else if (pk->key_type == BR_KEYTYPE_EC && pk->key.ec.qlen <= sizeof c->pkbuf) {
        memcpy(c->pkbuf, pk->key.ec.q, pk->key.ec.qlen);
        c->pkey.key.ec.q = c->pkbuf;
        c->have_pkey = true;
    }
}

static bool check_certificates(tls13_t *c, const uint8_t *m, size_t len) {
    rd_t r = { m + 4, m + len, false };
    unsigned ctxl = r8(&r); rbytes(&r, ctxl);
    unsigned total = r24(&r);
    rd_t l = { r.p, r.p + total, false };
    if (r.bad || r.p + total > r.e) { fail(c, "The website sent a malformed certificate message."); return false; }
    const br_x509_class **xv = &c->xc.vtable;
    (*xv)->start_chain(xv, c->host);
    int count = 0;
    while (l.p < l.e) {
        unsigned cl = r24(&l);
        const uint8_t *der = rbytes(&l, cl);
        unsigned el = r16(&l); rbytes(&l, el);
        if (l.bad || !der) { fail(c, "The website sent a malformed certificate message."); return false; }
        (*xv)->start_cert(xv, cl);
        (*xv)->append(xv, der, cl);
        (*xv)->end_cert(xv);
        count++;
    }
    if (!count) { fail(c, "The website did not send a security certificate."); return false; }
    unsigned e = (*xv)->end_chain(xv);
    if (e) {
        const char *t = tls_x509_error_text(e);
        if (t) fail(c, t); else { char b[64]; snprintf(b, sizeof b, "The website's certificate was rejected (X.509 error %u).", e); fail(c, b); }
        return false;
    }
    unsigned usages = 0;
    const br_x509_pkey *pk = (*xv)->get_pkey(xv, &usages);
    if (!pk || !(usages & BR_KEYTYPE_SIGN)) { fail(c, "The website's certificate may not be used for signing."); return false; }
    save_pkey(c, pk);
    if (!c->have_pkey) { fail(c, "The website's certificate key is too large."); return false; }
    return true;
}

/* ---------------- handshake ---------------- */
static void choose_suite(tls13_t *c, uint16_t suite) {
    c->suite = suite;
    c->hc = suite == 0x1302 ? &br_sha384_vtable : &br_sha256_vtable;
    c->hlen = suite == 0x1302 ? 48 : 32;
    c->klen = suite == 0x1301 ? 16 : 32;
}

static void x509_setup(tls13_t *c, const br_x509_trust_anchor *tas, size_t ntas, uint32_t days, uint32_t secs) {
    static const br_hash_class *hashes[] = { &br_sha1_vtable, &br_sha224_vtable, &br_sha256_vtable, &br_sha384_vtable, &br_sha512_vtable };
    br_x509_minimal_init(&c->xc, &br_sha256_vtable, tas, ntas);
    br_x509_minimal_set_rsa(&c->xc, br_rsa_pkcs1_vrfy_get_default());
    br_x509_minimal_set_ecdsa(&c->xc, br_ec_get_default(), br_ecdsa_vrfy_asn1_get_default());
    for (unsigned i = 0; i < ARRAY_LEN(hashes); i++)
        br_x509_minimal_set_hash(&c->xc, (hashes[i]->desc >> BR_HASHDESC_ID_OFF) & BR_HASHDESC_ID_MASK, hashes[i]);
    br_x509_minimal_set_time(&c->xc, days, secs);
}

static bool handshake(tls13_t *c, int *fallback) {
    uint8_t ch[1024];
    tls_random(c->random, 32); tls_random(c->sid, 32);
    if (!make_share(c, 0x001D)) { fail(c, "Key generation failed."); return false; }
    size_t chlen = build_hello(c, ch, NULL, 0);
    if (!rec_write(c, 22, ch, chlen)) { fail(c, "The connection was closed during the secure handshake."); *fallback = 1; return false; }

    const uint8_t *m; size_t ml; int alert = -1;
    int type = hs_next(c, &m, &ml, &alert);
    sh_t sh;
    if (type != 2 || !parse_sh(c, m, ml, &sh) || !sh.tls13) { *fallback = 1; return false; }
    if (sh.suite != 0x1301 && sh.suite != 0x1302 && sh.suite != 0x1303) { fail(c, "The website chose an encryption method this browser did not offer."); return false; }
    if (!sh.sid_ok) { fail(c, "The website's ServerHello is invalid (session ID mismatch)."); return false; }
    choose_suite(c, sh.suite);
    c->hc->init(&c->th.vtable);
    if (sh.hrr) {
        /* HelloRetryRequest: the transcript restarts with a hash of the first hello */
        uint8_t h[64], mh[4 + 64];
        hash_of(c, ch, chlen, h);
        mh[0] = 254; mh[1] = 0; mh[2] = 0; mh[3] = c->hlen; memcpy(mh + 4, h, c->hlen);
        th_add(c, mh, 4 + c->hlen);
        th_add(c, m, ml);
        uint8_t cookie[512]; size_t cookielen = 0;
        if (sh.cookie && sh.cookielen <= sizeof cookie) { memcpy(cookie, sh.cookie, sh.cookielen); cookielen = sh.cookielen; }
        uint16_t want = sh.group ? sh.group : c->group;
        hs_consume(c, ml);
        if (want == c->group && !cookielen) { fail(c, "The website asked for the key exchange it was already given."); return false; }
        if (want != c->group && !make_share(c, want)) { fail(c, "The website wants a key exchange this browser does not support."); return false; }
        chlen = build_hello(c, ch, cookielen ? cookie : NULL, cookielen);
        if (!rec_write(c, 22, ch, chlen)) { fail(c, "The connection was closed during the secure handshake."); return false; }
        th_add(c, ch, chlen);
        uint16_t suite = sh.suite;
        type = hs_next(c, &m, &ml, NULL);
        if (type != 2 || !parse_sh(c, m, ml, &sh) || !sh.tls13 || sh.hrr || sh.suite != suite) { fail(c, "The website's second ServerHello was invalid."); return false; }
    } else th_add(c, ch, chlen);
    th_add(c, m, ml);
    hs_consume(c, ml);
    if (sh.group != c->group || !sh.share) { fail(c, "The website's key share does not match."); return false; }
    uint8_t ecdhe[48]; size_t elen;
    if (!shared_secret(c, sh.share, sh.sharelen, ecdhe, &elen)) { fail(c, "The website's key share is invalid."); return false; }

    /* key schedule up to the handshake traffic secrets */
    uint8_t early[48], dsec[48], hs[48], th[64], chs[48], shs[48];
    hkdf_extract(c, NULL, NULL, 0, early);
    derive(c, early, "derived", NULL, dsec);
    hkdf_extract(c, dsec, ecdhe, elen, hs);
    transcript(c, th);
    derive(c, hs, "c hs traffic", th, chs);
    derive(c, hs, "s hs traffic", th, shs);
    set_keys(c, &c->rk, shs);

    /* EncryptedExtensions, [CertificateRequest], Certificate, CertificateVerify, Finished */
    bool cert_req = false, got_cert = false, got_cv = false;
    uint8_t req_ctx[256]; size_t req_ctx_len = 0;
    for (;;) {
        type = hs_next(c, &m, &ml, NULL);
        if (type < 0) return false;
        if (type == 8) { th_add(c, m, ml); }
        else if (type == 13) {
            cert_req = true;
            req_ctx_len = ml > 4 ? MIN((size_t)m[4], MIN(ml - 5, sizeof req_ctx)) : 0;
            memcpy(req_ctx, m + 5, req_ctx_len);
            th_add(c, m, ml);
        } else if (type == 11) {
            th_add(c, m, ml);
            if (!check_certificates(c, m, ml)) { send_alert(c, 42); return false; }
            got_cert = true;
        } else if (type == 15) {
            if (!got_cert) { fail(c, "The website skipped its certificate."); return false; }
            transcript(c, th);
            rd_t r = { m + 4, m + ml, false };
            unsigned alg = r16(&r), sl = r16(&r);
            const uint8_t *sig = rbytes(&r, sl);
            if (r.bad || !verify_cv(c, alg, sig, sl, th)) { send_alert(c, 51); fail(c, "The website could not prove it owns its certificate (bad signature)."); return false; }
            th_add(c, m, ml);
            got_cv = true;
        } else if (type == 20) {
            if (!got_cv) { fail(c, "The website did not authenticate itself."); return false; }
            uint8_t fk[48], expect[48];
            transcript(c, th);
            expand_label(c, shs, "finished", NULL, 0, fk, c->hlen);
            hmac(c, fk, c->hlen, th, c->hlen, NULL, 0, expect);
            if (ml != 4 + c->hlen || memcmp(m + 4, expect, c->hlen)) { send_alert(c, 51); fail(c, "The secure handshake failed its integrity check."); return false; }
            th_add(c, m, ml);
            hs_consume(c, ml);
            break;
        } else { fail(c, "The website sent an unexpected handshake message."); return false; }
        hs_consume(c, ml);
    }

    /* application secrets (transcript through the server's Finished) */
    uint8_t master[48], cap[48], sap[48];
    derive(c, hs, "derived", NULL, dsec);
    hkdf_extract(c, dsec, NULL, 0, master);
    transcript(c, th);
    derive(c, master, "c ap traffic", th, cap);
    derive(c, master, "s ap traffic", th, sap);

    /* our flight: compatibility CCS, [empty Certificate], Finished */
    static const uint8_t ccs[6] = { 20, 3, 3, 0, 1, 1 };
    if (tcp_send(c->t, ccs, 6) < 0) { fail(c, "The connection was closed during the secure handshake."); return false; }
    set_keys(c, &c->wk, chs);
    if (cert_req) {
        uint8_t cm[4 + 1 + 255 + 3]; size_t n = 4;
        cm[n++] = req_ctx_len; memcpy(cm + n, req_ctx, req_ctx_len); n += req_ctx_len;
        put24(cm + n, 0); n += 3;
        cm[0] = 11; put24(cm + 1, n - 4);
        th_add(c, cm, n);
        if (!rec_write(c, 22, cm, n)) return false;
    }
    uint8_t fk[48], fin[4 + 48];
    transcript(c, th);
    expand_label(c, chs, "finished", NULL, 0, fk, c->hlen);
    fin[0] = 20; put24(fin + 1, c->hlen);
    hmac(c, fk, c->hlen, th, c->hlen, NULL, 0, fin + 4);
    if (!rec_write(c, 22, fin, 4 + c->hlen)) { fail(c, "The connection was closed during the secure handshake."); return false; }
    set_keys(c, &c->wk, cap);
    set_keys(c, &c->rk, sap);
    memset(ecdhe, 0, sizeof ecdhe); memset(hs, 0, sizeof hs); memset(master, 0, sizeof master);
    memset(c->kpriv, 0, sizeof c->kpriv);
    snprintf(c->info, sizeof c->info, "TLS 1.3, %s, %s",
             c->suite == 0x1301 ? "AES-128-GCM" : c->suite == 0x1302 ? "AES-256-GCM" : "ChaCha20-Poly1305",
             c->group == 0x001D ? "X25519" : c->group == 0x0017 ? "P-256" : "P-384");
    return true;
}

tls13_t *tls13_connect(tcb_t *t, const char *host, const br_x509_trust_anchor *tas, size_t ntas,
                       uint32_t days, uint32_t secs, char *err, int errcap, int *fallback) {
    tls13_t *c = kzalloc(sizeof *c);
    c->t = t;
    *fallback = 0;
    strlcpy(c->host, host, sizeof c->host);
    x509_setup(c, tas, ntas, days, secs);
    if (!handshake(c, fallback)) {
        if (err) strlcpy(err, c->err[0] ? c->err : "The secure handshake failed.", errcap);
        kfree(c->hs); memset(c, 0, sizeof *c); kfree(c);
        return NULL;
    }
    return c;
}

int tls13_write(tls13_t *c, const void *data, uint32_t len) {
    if (c->failed || c->closed) return -1;
    if (!rec_write(c, 23, data, len)) { fail(c, "The connection was closed."); return -1; }
    return len;
}

static void key_update(tls13_t *c, keys_t *k) {
    uint8_t next[48];
    expand_label(c, k->secret, "traffic upd", NULL, 0, next, c->hlen);
    set_keys(c, k, next);
}

int tls13_read(tls13_t *c, void *buf, uint32_t max) {
    for (;;) {
        if (c->pt_off < c->pt_len) {
            uint32_t n = MIN(max, (uint32_t)(c->pt_len - c->pt_off));
            memcpy(buf, c->pt + c->pt_off, n); c->pt_off += n;
            return n;
        }
        if (c->closed) return 0;
        if (c->failed) return -1;
        uint8_t *d; size_t n;
        int type = rec_read(c, &d, &n);
        if (type == 0) { c->closed = true; return 0; }           /* TCP closed without close_notify */
        if (type < 0) return -1;
        if (type == 23) {
            if (!c->pt) c->pt = kmalloc(MAXREC + 256);
            memcpy(c->pt, d, n); c->pt_off = 0; c->pt_len = n;
        } else if (type == 21) {
            if (n >= 2 && d[1] == 0) { c->closed = true; return 0; }
            fail(c, alert_text(n >= 2 ? d[1] : 0));
            return -1;
        } else if (type == 22) {
            if (c->hs_len + n > c->hs_cap) { c->hs_cap = c->hs_len + n + 4096; c->hs = krealloc(c->hs, c->hs_cap); }
            memcpy(c->hs + c->hs_len, d, n); c->hs_len += n;
            while (c->hs_len >= 4) {
                size_t L = 4 + (c->hs[1] << 16 | c->hs[2] << 8 | c->hs[3]);
                if (c->hs_len < L) break;
                if (c->hs[0] == 24 && L == 5) {                      /* KeyUpdate */
                    bool requested = c->hs[4] == 1;
                    key_update(c, &c->rk);
                    if (requested) {
                        uint8_t ku[5] = { 24, 0, 0, 1, 0 };
                        rec_write(c, 22, ku, 5);
                        key_update(c, &c->wk);
                    }
                }
                /* NewSessionTicket (4) and anything else: ignored */
                hs_consume(c, L);
            }
        }
    }
}

void tls13_free(tls13_t *c) {
    if (!c) return;
    if (!c->failed && !c->closed) send_alert(c, 0);               /* close_notify */
    kfree(c->hs); kfree(c->pt);
    memset(c, 0, sizeof *c);
    kfree(c);
}
const char *tls13_info(tls13_t *c) { return c->info; }
const char *tls13_error(tls13_t *c) { return c->err; }
