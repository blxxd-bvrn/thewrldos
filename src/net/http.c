/* HTTP/1.1 client over TCP or TLS: GET/POST, redirects, chunked transfer
 * encoding and gzip/deflate content encoding. */
#include "kernel.h"
#include "net.h"
#include "fs.h"

#define MAX_BODY (24 * 1024 * 1024)
#define USER_AGENT "Mozilla/5.0 (compatible; WRLDBrowser/2.0; THE WRLD OS 1.0)"

bool url_parse(const char *url, char *scheme, char *host, int hostcap, uint16_t *port, char *path, int pathcap) {
    const char *p = strstr(url, "://");
    if (!p) return false;
    int sl = p - url; if (sl > 7) return false;
    for (int i = 0; i < sl; i++) scheme[i] = tolower(url[i]);
    scheme[sl] = 0;
    p += 3;
    const char *h = p;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
    int hl = p - h; if (!hl || hl >= hostcap) return false;
    memcpy(host, h, hl); host[hl] = 0;
    for (char *c = host; *c; c++) *c = tolower(*c);
    *port = !strcmp(scheme, "https") ? 443 : 80;
    if (*p == ':') { p++; *port = atoi(p); while (isdigit(*p)) p++; }
    if (*p == '/' || *p == '?') strlcpy(path, *p == '?' ? "/" : p, pathcap);
    else strlcpy(path, "/", pathcap);
    if (*p == '?') strlcat(path, p, pathcap);
    char *frag = strchr(path, '#'); if (frag) *frag = 0;
    return true;
}
/* resolves rel against base (RFC 3986, simplified) */
bool url_resolve(const char *base, const char *rel, char *out, int cap) {
    while (*rel == ' ' || *rel == '\t' || *rel == '\n' || *rel == '\r') rel++;
    if (strstr(rel, "://") && strstr(rel, "://") < rel + 10) { strlcpy(out, rel, cap); return true; }
    if (!strncasecmp(rel, "about:", 6) || !strncasecmp(rel, "wrld:", 5) || !strncasecmp(rel, "mailto:", 7) || !strncasecmp(rel, "javascript:", 11) || !strncasecmp(rel, "data:", 5)) { strlcpy(out, rel, cap); return true; }
    const char *sch = strstr(base, "://");
    if (!sch) { strlcpy(out, rel, cap); return false; }
    if (rel[0] == '/' && rel[1] == '/') { int l = sch - base + 1; snprintf(out, cap, "%.*s%s", l, base, rel); return true; }
    const char *hs = sch + 3, *pe = hs;
    while (*pe && *pe != '/' && *pe != '?' && *pe != '#') pe++;
    char root[512]; snprintf(root, sizeof root, "%.*s", (int)(pe - base), base);
    if (rel[0] == '/') { snprintf(out, cap, "%s%s", root, rel); return true; }
    if (rel[0] == '#' || !rel[0]) { strlcpy(out, base, cap); char *f = strchr(out, '#'); if (f) *f = 0; strlcat(out, rel, cap); return true; }
    /* directory of the base path */
    char path[1024];
    snprintf(path, sizeof path, "%s", *pe == '/' ? pe : "/");
    char *q = strpbrk(path, "?#"); if (q) *q = 0;
    if (rel[0] == '?') { snprintf(out, cap, "%s%s%s", root, path, rel); return true; }
    char *slash = strrchr(path, '/'); if (slash) slash[1] = 0;
    char joined[2048]; snprintf(joined, sizeof joined, "%s%s", path, rel);
    /* remove dot segments */
    char res[2048]; int rl = 0;
    char *qs = strpbrk(joined, "?#"); char tail[1024] = ""; if (qs) { strlcpy(tail, qs, sizeof tail); *qs = 0; }
    for (char *seg = joined; *seg;) {
        while (*seg == '/') seg++;
        char *e = seg; while (*e && *e != '/') e++;
        int n = e - seg; bool dir = *e == '/';
        if (n == 1 && seg[0] == '.') { if (!dir) res[rl++] = '/'; }
        else if (n == 2 && seg[0] == '.' && seg[1] == '.') { while (rl > 0 && res[rl - 1] != '/') rl--; if (rl > 0) rl--; while (rl > 0 && res[rl - 1] != '/') rl--; if (!dir && (!rl || res[rl - 1] != '/')) res[rl++] = '/'; }
        else if (n) { if (!rl || res[rl - 1] != '/') res[rl++] = '/'; if (rl + n + 1 < (int)sizeof res) { memcpy(res + rl, seg, n); rl += n; } if (dir) res[rl++] = '/'; }
        seg = e;
    }
    if (!rl) res[rl++] = '/';
    res[rl] = 0;
    snprintf(out, cap, "%s%s%s", root, res[0] == '/' ? "" : "/", res);
    strlcat(out, tail, cap);
    return true;
}

typedef struct { tcb_t *t; tls_conn_t *tls; } conn_t;
static int c_write(conn_t *c, const void *d, uint32_t n) { return c->tls ? tls_write(c->tls, d, n) : tcp_send(c->t, d, n); }
static int c_read(conn_t *c, void *b, uint32_t n) {
    if (c->tls) return tls_read(c->tls, b, n);
    int r = tcp_recv(c->t, b, n, 30000);
    return r == -2 ? -1 : r;
}
static void c_close(conn_t *c) { if (c->tls) tls_close(c->tls); else if (c->t) tcp_close(c->t); }

static const char *hdr_find(const char *hdrs, const char *name, char *out, int cap) {
    int nl = strlen(name);
    for (const char *p = hdrs; *p;) {
        const char *e = strstr(p, "\r\n"); if (!e) e = p + strlen(p);
        if (e - p > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1; while (*v == ' ' || *v == '\t') v++;
            int l = MIN((int)(e - v), cap - 1); memcpy(out, v, l); out[l] = 0;
            return out;
        }
        p = *e ? e + 2 : e;
    }
    return NULL;
}
static uint8_t *dechunk(const uint8_t *b, uint32_t len, uint32_t *outlen) {
    uint8_t *o = kmalloc(len + 1); uint32_t ol = 0, i = 0;
    while (i < len) {
        uint32_t sz = 0; bool any = false;
        while (i < len && isxdigit(b[i])) { int c = tolower(b[i]); sz = sz * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); i++; any = true; }
        while (i < len && b[i] != '\n') i++;
        i++;
        if (!any || !sz) break;
        uint32_t n = MIN(sz, len - MIN(i, len));
        memcpy(o + ol, b + i, n); ol += n; i += n;
        if (i < len && b[i] == '\r') i++;
        if (i < len && b[i] == '\n') i++;
    }
    *outlen = ol;
    return o;
}
static uint8_t *ungzip(const uint8_t *b, uint32_t len, uint32_t *outlen) {
    if (len < 18 || b[0] != 0x1F || b[1] != 0x8B || b[2] != 8) return NULL;
    uint8_t fl = b[3]; uint32_t i = 10;
    if (fl & 4) { if (i + 2 > len) return NULL; i += 2 + (b[i] | (b[i + 1] << 8)); }
    if (fl & 8) { while (i < len && b[i]) i++; i++; }
    if (fl & 16) { while (i < len && b[i]) i++; i++; }
    if (fl & 2) i += 2;
    if (i >= len) return NULL;
    uint32_t isize = b[len - 4] | (b[len - 3] << 8) | (b[len - 2] << 16) | ((uint32_t)b[len - 1] << 24);
    uint32_t cap = MIN(MAX(isize + 64, len * 4u), (uint32_t)MAX_BODY);
    uint8_t *o = kmalloc(cap + 1);
    if (inflate_raw(b + i, len - i, o, cap, outlen) < 0 && !*outlen) { kfree(o); return NULL; }
    return o;
}

static bool http_once(const char *method, const char *url, const char *ctype, const void *body, uint32_t blen,
                      http_response_t *r, http_progress_t cb, void *ctx) {
    char scheme[8], host[256], path[2048]; uint16_t port;
    if (!url_parse(url, scheme, host, sizeof host, &port, path, sizeof path)) { snprintf(r->error, sizeof r->error, "The address is not valid."); return false; }
    bool https = !strcmp(scheme, "https");
    if (!https && strcmp(scheme, "http")) { snprintf(r->error, sizeof r->error, "The '%s' protocol is not supported.", scheme); return false; }
    if (cb) { char s[300]; snprintf(s, sizeof s, "Finding site: %s", host); cb(ctx, s, 0, 0); }
    uint32_t ip;
    if (!dns_resolve(host, &ip, r->error, sizeof r->error)) return false;
    if (cb) { char s[300]; snprintf(s, sizeof s, "Connecting to %s...", host); cb(ctx, s, 0, 0); }
    conn_t c = { 0 };
    c.t = tcp_connect(ip, port, 15000, r->error, sizeof r->error);
    if (!c.t) return false;
    if (https) {
        if (cb) cb(ctx, "Establishing a secure connection...", 0, 0);
        c.tls = tls_open(c.t, host, ip, port, r->error, sizeof r->error);
        if (!c.tls) return false;                         /* tls_open closed the TCP connection */
        r->secure = true;
        strlcpy(r->tls_info, tls_cipher_name(c.tls), sizeof r->tls_info);
    }
    char req[3072], hosthdr[300];
    bool defport = (https && port == 443) || (!https && port == 80);
    if (defport) strlcpy(hosthdr, host, sizeof hosthdr); else snprintf(hosthdr, sizeof hosthdr, "%s:%u", host, port);
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
                     "Accept: text/html,application/xhtml+xml,text/plain;q=0.9,image/*;q=0.8,*/*;q=0.5\r\nAccept-Language: en\r\n"
                     "Accept-Encoding: gzip, deflate\r\nConnection: close\r\n", method, path, hosthdr);
    if (body) n += snprintf(req + n, sizeof req - n, "Content-Type: %s\r\nContent-Length: %u\r\n", ctype ? ctype : "application/x-www-form-urlencoded", blen);
    n += snprintf(req + n, sizeof req - n, "\r\n");
    if (c_write(&c, req, n) < 0 || (body && blen && c_write(&c, body, blen) < 0)) {
        snprintf(r->error, sizeof r->error, "%s", c.tls ? tls_error_text(c.tls) : "The connection was reset while sending the request.");
        c_close(&c); return false;
    }
    if (cb) { char s[300]; snprintf(s, sizeof s, "Waiting for %s...", host); cb(ctx, s, 0, 0); }
    /* read everything (Connection: close) */
    uint32_t cap = 64 * 1024, len = 0;
    uint8_t *buf = kmalloc(cap);
    uint32_t hdr_end = 0, total = 0;
    for (;;) {
        if (len + 16384 > cap) { if (cap >= MAX_BODY) break; cap *= 2; buf = krealloc(buf, cap); }
        int k = c_read(&c, buf + len, cap - len - 1);
        if (k < 0) {
            if (!len) { snprintf(r->error, sizeof r->error, "%s", c.tls && tls_error_text(c.tls)[0] ? tls_error_text(c.tls) : "The server closed the connection without answering."); kfree(buf); c_close(&c); return false; }
            break;
        }
        if (!k) break;
        len += k;
        if (!hdr_end) {
            buf[len] = 0;
            char *e = strstr((char *)buf, "\r\n\r\n");
            if (e) { hdr_end = e - (char *)buf + 4; char cl[32]; *e = 0; if (hdr_find((char *)buf, "Content-Length", cl, sizeof cl)) total = strtoul(cl, NULL, 10); *e = '\r'; }
        }
        if (hdr_end && total && len - hdr_end >= total) break;   /* complete even if the server keeps the socket open */
        if (cb) cb(ctx, "Downloading...", hdr_end ? len - hdr_end : 0, total);
    }
    c_close(&c);
    buf[len] = 0;
    if (!hdr_end) { char *e = strstr((char *)buf, "\n\n"); if (!e) { snprintf(r->error, sizeof r->error, "The server sent an invalid response."); kfree(buf); return false; } hdr_end = e - (char *)buf + 2; }
    char *hdrs = (char *)buf; hdrs[hdr_end - 2] = 0;
    if (strncmp(hdrs, "HTTP/", 5)) { snprintf(r->error, sizeof r->error, "The server did not answer with HTTP."); kfree(buf); return false; }
    r->status = atoi(strchr(hdrs, ' ') ? strchr(hdrs, ' ') + 1 : "0");
    char v[256];
    if (hdr_find(hdrs, "Content-Type", v, sizeof v)) {
        strlcpy(r->content_type, v, sizeof r->content_type);
        char *semi = strchr(r->content_type, ';'); if (semi) *semi = 0;
        char *cs = strstr(v, "charset="); if (cs) strlcpy(r->charset, cs + 8, sizeof r->charset);
    }
    if (hdr_find(hdrs, "Location", v, sizeof v)) url_resolve(url, v, r->location, sizeof r->location);
    uint8_t *b = buf + hdr_end; uint32_t bl = len - hdr_end;
    if (total && bl > total) bl = total;
    uint8_t *dec = NULL; uint32_t dl = 0;
    if (hdr_find(hdrs, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) { dec = dechunk(b, bl, &dl); b = dec; bl = dl; }
    uint8_t *unz = NULL; uint32_t ul = 0;
    if (hdr_find(hdrs, "Content-Encoding", v, sizeof v)) {
        if (strstr(v, "gzip")) unz = ungzip(b, bl, &ul);
        else if (strstr(v, "deflate")) {
            uint32_t cap2 = MIN(bl * 8u + 1024, (uint32_t)MAX_BODY); unz = kmalloc(cap2 + 1);
            if (inflate_zlib(b, bl, unz, cap2, &ul) < 0 && inflate_raw(b, bl, unz, cap2, &ul) < 0) { kfree(unz); unz = NULL; }
        }
        if (unz) { b = unz; bl = ul; }
    }
    r->body = kmalloc(bl + 1); memcpy(r->body, b, bl); r->body[bl] = 0; r->body_len = bl;
    kfree(dec); kfree(unz); kfree(buf);
    return true;
}
bool http_request(const char *method, const char *url, const char *ctype, const void *body, uint32_t blen,
                  http_response_t *r, http_progress_t cb, void *ctx) {
    memset(r, 0, sizeof *r);
    char cur[1024]; strlcpy(cur, url, sizeof cur);
    for (int hop = 0; hop < 8; hop++) {
        http_free(r);
        memset(r, 0, sizeof *r);
        strlcpy(r->url, cur, sizeof r->url);
        if (!http_once(method, cur, ctype, body, blen, r, cb, ctx)) return false;
        if (r->status >= 300 && r->status < 400 && r->location[0] && r->status != 304) {
            strlcpy(cur, r->location, sizeof cur);
            if (r->status == 303 || ((r->status == 301 || r->status == 302) && strcmp(method, "GET"))) { method = "GET"; body = NULL; blen = 0; }
            continue;
        }
        return true;
    }
    snprintf(r->error, sizeof r->error, "The page redirected too many times.");
    return false;
}
bool http_get(const char *url, http_response_t *r, http_progress_t cb, void *ctx) { return http_request("GET", url, NULL, NULL, 0, r, cb, ctx); }
void http_free(http_response_t *r) { kfree(r->body); r->body = NULL; r->body_len = 0; }
