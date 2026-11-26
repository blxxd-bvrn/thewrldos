/* The WRLD Web (included by wrldweb.c): the browser's own search page, wrld://web?q=...
 * The search box, words typed in the address bar and the home page search here. The browser
 * asks DuckDuckGo (its HTML results page) and Brave Search for the words at the same time,
 * reads the results out of both pages, merges them (one from each in turn, without repeats)
 * and shows them on a page of its own. When neither answers with results (they sometimes ask
 * for a check only a person can pass), the page says so and offers both sites directly. */
#include <libsoup/soup.h>

#define WRLD_WEB "wrld://web?q="
static SoupSession *web_soup;
static char *web_proxy;                       /* the proxy the browser uses, if any */

typedef struct { char *title, *url, *snippet, *from; } web_hit;
typedef struct {
    WebKitURISchemeRequest *req;
    char *q;
    char *page[2];                            /* DuckDuckGo's and Brave's HTML */
    int pending;
} web_job;

static char *html_text(const char *s, gssize n) {             /* tags out, entities decoded, spaces folded */
    GString *o = g_string_new(NULL);
    gboolean tag = FALSE, sp = FALSE;
    for (gssize i = 0; n < 0 ? s[i] != 0 : i < n; i++) {
        char c = s[i];
        if (c == '<') { tag = TRUE; continue; }
        if (c == '>') { tag = FALSE; continue; }
        if (tag) continue;
        if (c == '&') {
            static const struct { const char *e, *r; } ents[] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#x27;", "'" }, { "&#39;", "'" }, { "&nbsp;", " " }, { "&#x2F;", "/" } };
            gboolean done = FALSE;
            for (unsigned k = 0; k < G_N_ELEMENTS(ents) && !done; k++)
                if (!strncmp(s + i, ents[k].e, strlen(ents[k].e))) { g_string_append(o, ents[k].r); i += strlen(ents[k].e) - 1; done = TRUE; }
            if (done) { sp = FALSE; continue; }
        }
        if (g_ascii_isspace(c)) { if (!sp && o->len) g_string_append_c(o, ' '); sp = TRUE; continue; }
        g_string_append_c(o, c); sp = FALSE;
    }
    while (o->len && o->str[o->len - 1] == ' ') g_string_truncate(o, o->len - 1);
    return g_string_free(o, FALSE);
}
static char *esc(const char *s) { return g_markup_escape_text(s ? s : "", -1); }

/* DuckDuckGo's HTML page: <a class="result__a" href="//duckduckgo.com/l/?uddg=URL&...">Title</a>
 * ... <a class="result__snippet" ...>snippet</a> */
static void parse_ddg(const char *page, GPtrArray *hits) {
    if (!page) return;
    GRegex *re = g_regex_new("class=\"result__a\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>(.*?)(?=class=\"result__a\"|$)", G_REGEX_DOTALL, 0, NULL);
    GRegex *sn = g_regex_new("class=\"result__snippet\"[^>]*>(.*?)</(a|td|div)>", G_REGEX_DOTALL, 0, NULL);
    GMatchInfo *mi;
    g_regex_match(re, page, 0, &mi);
    while (g_match_info_matches(mi) && hits->len < 40) {
        char *href = g_match_info_fetch(mi, 1), *title = g_match_info_fetch(mi, 2), *rest = g_match_info_fetch(mi, 3);
        char *url = NULL;
        const char *u = strstr(href, "uddg=");
        if (u) { char *e = strchr(u, '&'); char *enc = e ? g_strndup(u + 5, e - u - 5) : g_strdup(u + 5); url = g_uri_unescape_string(enc, NULL); g_free(enc); }
        else url = g_str_has_prefix(href, "//") ? g_strconcat("https:", href, NULL) : g_strdup(href);
        if (url && g_str_has_prefix(url, "http") && !strstr(url, "duckduckgo.com/y.js")) {   /* y.js: DuckDuckGo's ads */
            web_hit *h = g_new0(web_hit, 1);
            h->url = url; url = NULL;
            h->title = html_text(title, -1);
            GMatchInfo *m2;
            if (g_regex_match(sn, rest, 0, &m2)) { char *s = g_match_info_fetch(m2, 1); h->snippet = html_text(s, -1); g_free(s); }
            g_match_info_free(m2);
            h->from = "DuckDuckGo";
            g_ptr_array_add(hits, h);
        }
        g_free(url); g_free(href); g_free(title); g_free(rest);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi); g_regex_unref(re); g_regex_unref(sn);
}
/* Brave Search: each web result is a block with data-type="web": its first https link, the
 * title (class "title") and the description (class "snippet-description" or "content") */
static void parse_brave(const char *page, GPtrArray *hits) {
    if (!page) return;
    const char *p = page;
    GRegex *href = g_regex_new("<a[^>]*href=\"(https?://[^\"]+)\"", 0, 0, NULL);
    GRegex *title = g_regex_new("class=\"[^\"]*title[^\"]*\"[^>]*>(.*?)</(div|span)>", G_REGEX_DOTALL, 0, NULL);
    GRegex *desc = g_regex_new("class=\"[^\"]*(snippet-description|snippet-content|generic-snippet)[^\"]*\"[^>]*>(.*?)</div>", G_REGEX_DOTALL, 0, NULL);
    while ((p = strstr(p, "data-type=\"web\"")) && hits->len < 40) {
        const char *next = strstr(p + 15, "data-type=\"web\"");
        char *block = next ? g_strndup(p, next - p) : g_strndup(p, MIN(strlen(p), 20000));
        GMatchInfo *m;
        char *url = NULL, *t = NULL, *d = NULL;
        if (g_regex_match(href, block, 0, &m)) { url = g_match_info_fetch(m, 1); }
        g_match_info_free(m);
        if (g_regex_match(title, block, 0, &m)) { char *x = g_match_info_fetch(m, 1); t = html_text(x, -1); g_free(x); }
        g_match_info_free(m);
        if (g_regex_match(desc, block, 0, &m)) { char *x = g_match_info_fetch(m, 2); d = html_text(x, -1); g_free(x); }
        g_match_info_free(m);
        if (url && t && *t && !strstr(url, "brave.com/")) {
            web_hit *h = g_new0(web_hit, 1);
            char *un = html_text(url, -1);
            h->url = un; h->title = t; h->snippet = d; h->from = "Brave"; t = d = NULL;
            g_ptr_array_add(hits, h);
        }
        g_free(url); g_free(t); g_free(d); g_free(block);
        p += 15;
    }
    g_regex_unref(href); g_regex_unref(title); g_regex_unref(desc);
}
static char *url_key(const char *u) {                         /* for spotting the same page twice */
    const char *s = strstr(u, "://"); s = s ? s + 3 : u;
    if (!strncmp(s, "www.", 4)) s += 4;
    char *k = g_ascii_strdown(s, -1);
    size_t n = strlen(k); while (n && (k[n - 1] == '/')) k[--n] = 0;
    return k;
}
static const char web_style[] =
    "<style>body{margin:0;font-family:'Liberation Sans',Arial,sans-serif;color:#222;background:#fff}"
    ".top{background:linear-gradient(#0b3f78,#1f6fb8);padding:14px 20px;display:flex;align-items:center;gap:16px;border-bottom:1px solid #8fb6de}"
    ".top a.logo{color:#fff;font-size:22px;text-decoration:none;text-shadow:0 1px 2px #0006;white-space:nowrap}"
    "form{display:flex;flex:1;max-width:620px;border:1px solid #7f9db9;background:#fff}input{flex:1;border:0;padding:6px 8px;font-size:15px;outline:none}"
    "button{border:0;border-left:1px solid #7f9db9;background:linear-gradient(#fff,#cfe3f7);padding:0 14px;cursor:pointer;font-size:13px}"
    ".info{color:#666;font-size:12px;padding:10px 20px 0}.r{max-width:680px;padding:12px 20px 4px}"
    ".r a.t{font-size:17px;color:#1a0dab;text-decoration:none}.r a.t:hover{text-decoration:underline}"
    ".r .u{color:#0a7b35;font-size:13px;margin:2px 0;word-break:break-all}.r .s{font-size:13.5px;line-height:1.4;color:#333}"
    ".r .f{font-size:11px;color:#999;margin-left:6px}.none{padding:30px 20px;max-width:640px;font-size:14px;line-height:1.5}"
    ".none a{display:inline-block;margin:10px 12px 0 0;padding:6px 14px;border:1px solid #7f9db9;background:linear-gradient(#fff,#e4eef9);color:#123;text-decoration:none}"
    ".home{text-align:center;margin-top:90px}.home h1{font-weight:normal;font-size:32px;color:#0b3f78}.home form{margin:18px auto;max-width:520px}"
    ".home p{color:#777;font-size:13px}</style>";
static void web_finish(web_job *j) {
    GPtrArray *a = g_ptr_array_new(), *b = g_ptr_array_new();
    parse_ddg(j->page[0], a);
    parse_brave(j->page[1], b);
    GString *h = g_string_new("<!doctype html><html><head><meta charset=utf-8><title>");
    char *qe = esc(j->q);
    g_string_append_printf(h, "%s - The WRLD Web</title>%s</head><body><div class=top><a class=logo href='wrld://web'>The WRLD Web</a>"
                           "<form action='wrld://web'><input name=q value=\"%s\" autofocus><button>Search</button></form></div>", qe, web_style, qe);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int shown = 0;
    for (guint i = 0; i < MAX(a->len, b->len); i++) {
        for (int k = 0; k < 2; k++) {
            GPtrArray *src = k ? b : a;
            if (i >= src->len) continue;
            web_hit *x = src->pdata[i];
            char *key = url_key(x->url);
            if (g_hash_table_contains(seen, key)) { g_free(key); continue; }
            g_hash_table_add(seen, key);
            char *t = esc(x->title), *u = esc(x->url), *s = esc(x->snippet ? x->snippet : "");
            g_string_append_printf(h, "<div class=r><a class=t href=\"%s\">%s</a><div class=u>%s<span class=f>%s</span></div><div class=s>%s</div></div>", u, t, u, x->from, s);
            g_free(t); g_free(u); g_free(s);
            shown++;
        }
    }
    char *qq = g_uri_escape_string(j->q, NULL, TRUE);
    if (!shown)
        g_string_append_printf(h, "<div class=none>No results came back from DuckDuckGo or Brave Search for <b>%s</b>. They sometimes ask for a check "
                               "that only a person can pass, or the connection is down. Search on them directly:<br>"
                               "<a href='https://duckduckgo.com/?q=%s'>DuckDuckGo</a><a href='https://search.brave.com/search?q=%s'>Brave Search</a></div>", qe, qq, qq);
    else
        g_string_append_printf(h, "<div class=info>%d results from DuckDuckGo (%u) and Brave Search (%u) &middot; <a href='https://duckduckgo.com/?q=%s'>more on DuckDuckGo</a>"
                               " &middot; <a href='https://search.brave.com/search?q=%s'>more on Brave</a></div>", shown, a->len, b->len, qq, qq);
    g_string_append(h, "</body></html>");
    gsize len = h->len;
    GInputStream *in = g_memory_input_stream_new_from_data(g_string_free(h, FALSE), len, g_free);
    webkit_uri_scheme_request_finish(j->req, in, len, "text/html");
    g_object_unref(in);
    g_free(qe); g_free(qq);
    g_hash_table_destroy(seen);
    for (guint i = 0; i < a->len; i++) { web_hit *x = a->pdata[i]; g_free(x->title); g_free(x->url); g_free(x->snippet); g_free(x); }
    for (guint i = 0; i < b->len; i++) { web_hit *x = b->pdata[i]; g_free(x->title); g_free(x->url); g_free(x->snippet); g_free(x); }
    g_ptr_array_free(a, TRUE); g_ptr_array_free(b, TRUE);
    g_object_unref(j->req); g_free(j->q); g_free(j->page[0]); g_free(j->page[1]); g_free(j);
}
typedef struct { web_job *j; int which; } web_part;
static void web_got(GObject *src, GAsyncResult *res, gpointer p) {
    web_part *wp = p;
    GError *e = NULL;
    GBytes *b = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &e);
    if (b) { gsize n; const char *d = g_bytes_get_data(b, &n); wp->j->page[wp->which] = g_strndup(d, n); g_bytes_unref(b); }
    else { g_printerr("wrldweb: The WRLD Web: %s: %s\n", wp->which ? "Brave" : "DuckDuckGo", e ? e->message : "?"); g_clear_error(&e); }
    if (--wp->j->pending == 0) web_finish(wp->j);
    g_free(wp);
}
static void web_fetch(web_job *j, int which, const char *url) {
    SoupMessage *m = soup_message_new("GET", url);
    if (!m) { j->pending--; return; }
    SoupMessageHeaders *hd = soup_message_get_request_headers(m);
    soup_message_headers_replace(hd, "Accept", "text/html,application/xhtml+xml");
    soup_message_headers_replace(hd, "Accept-Language", "en-US,en;q=0.9");
    web_part *wp = g_new0(web_part, 1); wp->j = j; wp->which = which;
    soup_session_send_and_read_async(web_soup, m, G_PRIORITY_DEFAULT, NULL, web_got, wp);
    g_object_unref(m);
}
static void wrld_web(WebKitURISchemeRequest *r, const char *q) {
    if (!q || !*q) {                                           /* The WRLD Web's own front page */
        static const char head[] = "<!doctype html><html><head><meta charset=utf-8><title>The WRLD Web</title>";
        char *page = g_strconcat(head, web_style, "</head><body><div class=home><h1>The WRLD Web</h1>"
                                 "<form action='wrld://web'><input name=q autofocus><button>Search</button></form>"
                                 "<p>Results from DuckDuckGo and Brave Search, together on one page.</p></div></body></html>", NULL);
        gsize len = strlen(page);
        GInputStream *in = g_memory_input_stream_new_from_data(page, len, g_free);
        webkit_uri_scheme_request_finish(r, in, len, "text/html");
        g_object_unref(in);
        return;
    }
    if (!web_soup) {
        web_soup = soup_session_new_with_options("user-agent", webkit_settings_get_user_agent(wsettings), "timeout", 20, NULL);
        if (web_proxy && *web_proxy) {
            GProxyResolver *pr = g_simple_proxy_resolver_new(web_proxy, NULL);
            soup_session_set_proxy_resolver(web_soup, pr);
            g_object_unref(pr);
        }
    }
    web_job *j = g_new0(web_job, 1);
    j->req = g_object_ref(r); j->q = g_strdup(q); j->pending = 2;
    char *qq = g_uri_escape_string(q, NULL, TRUE);
    char *u1 = g_strconcat("https://html.duckduckgo.com/html/?q=", qq, NULL), *u2 = g_strconcat("https://search.brave.com/search?source=web&q=", qq, NULL);
    web_fetch(j, 0, u1); web_fetch(j, 1, u2);
    g_free(u1); g_free(u2); g_free(qq);
}
