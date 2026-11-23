/* The WRLD Browser's DOM: the parts that stay C. The document tree interface (jsapi.h
 * dom_*) is rust/src/dom.rs; fetching talks to the HTTP client, and the wall clock is
 * returned as a double, which the 64-bit kernel's soft-float Rust code cannot hand to C. */
#include "kernel.h"
#include "fs.h"
#include "net.h"
#include "wm.h"
#include "browser.h"
#include "jsapi.h"

#define PG ((page_t *)page)

/* The layout of ctrl_t and page_t for the Rust copies in rust/src/dom.rs, which rust_init
 * checks at boot. */
const uint32_t rust_dom_layout[12] = {
    sizeof(ctrl_t), offsetof(ctrl_t, node), offsetof(ctrl_t, checked), offsetof(ctrl_t, optvals),
    offsetof(page_t, title), offsetof(page_t, referrer), offsetof(page_t, ctrls), offsetof(page_t, nctrls),
    offsetof(page_t, blocked), offsetof(page_t, tree_dirty), offsetof(page_t, title_dirty), offsetof(page_t, scripts_pending),
};

/* ---------------- network ---------------- */
int dom_fetch(void *page, const char *url, const char *method, const char *body, char **out, int *outlen, int *status, char **ctype) {
    *out = NULL; *ctype = NULL; *outlen = 0; *status = 0;
    if (!strncasecmp(url, "file:", 5)) {
        const char *pth = url + 5; while (*pth == '/') pth++;
        uint32_t l; uint8_t *d = vfs_read_file(pth, &l);
        if (!d) return 0;
        *out = kmalloc(l + 1); memcpy(*out, d, l); (*out)[l] = 0; *outlen = l; kfree(d);
        *status = 200; *ctype = strdup("text/plain");
        return 1;
    }
    if (adblock_blocks(url, PG->url)) { PG->blocked++; return 0; }
    http_response_t r;
    bool ok = body || strcasecmp(method, "GET") ? http_request(method, url, "application/x-www-form-urlencoded", body ? body : "", body ? strlen(body) : 0, &r, NULL, NULL)
                                                : http_get(url, &r, NULL, NULL);
    if (!ok) { http_free(&r); return 0; }
    /* the engine wants UTF-8 text; pass the bytes through unchanged */
    *out = kmalloc(r.body_len + 1); memcpy(*out, r.body, r.body_len); (*out)[r.body_len] = 0; *outlen = r.body_len;
    *status = r.status; *ctype = strdup(r.content_type);
    http_free(&r);
    return 1;
}
double dom_epoch_ms(void) {
    rtc_time_t t; rtc_read(&t);
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (double)((days * 86400 + t.hour * 3600 + t.minute * 60 + t.second) * 1000);
}
