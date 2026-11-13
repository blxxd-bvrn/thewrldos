/* XplorerLite ad blocker.
 *  - requests for pictures and scripts on known ad / tracking servers are
 *    never made (built-in list plus C:\WRLD\adblock.txt)
 *  - page elements whose class or id looks like an ad slot are hidden
 *  - sites can be allowed ("@@example.com" in the list file)
 * Top-level navigation (typing an address, following a link) is never blocked. */
#include "kernel.h"
#include "fs.h"
#include "net.h"
#include "browser.h"

bool adblock_enabled = true;
uint32_t adblock_total;                       /* requests refused since start-up */

/* matched against the end of the host name: "doubleclick.net" also blocks
 * "ad.doubleclick.net" but not "notdoubleclick.net" */
static const char *const builtin_hosts[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com", "google-analytics.com", "googletagmanager.com",
    "googletagservices.com", "adservice.google.com", "pagead2.googlesyndication.com", "imasdk.googleapis.com",
    "amazon-adsystem.com", "adnxs.com", "adsrvr.org", "taboola.com", "outbrain.com", "criteo.com", "criteo.net",
    "scorecardresearch.com", "quantserve.com", "quantcount.com", "moatads.com", "pubmatic.com", "rubiconproject.com",
    "openx.net", "casalemedia.com", "advertising.com", "bidswitch.net", "smartadserver.com", "media.net", "zedo.com",
    "popads.net", "propellerads.com", "adform.net", "serving-sys.com", "hotjar.com", "mixpanel.com", "chartbeat.com",
    "chartbeat.net", "krxd.net", "bluekai.com", "demdex.net", "everesttech.net", "mathtag.com", "adsafeprotected.com",
    "doubleverify.com", "teads.tv", "yieldmo.com", "sharethrough.com", "33across.com", "indexww.com", "lijit.com",
    "sovrn.com", "contextweb.com", "spotxchange.com", "springserve.com", "tremorhub.com", "yahoo-mbcdn.com",
    "ads.yahoo.com", "analytics.yahoo.com", "adtechus.com", "atdmt.com", "adroll.com", "revcontent.com", "mgid.com",
    "zergnet.com", "exoclick.com", "juicyads.com", "trafficjunky.net", "adcash.com", "hilltopads.net", "onclickads.net",
    "adsterra.com", "infolinks.com", "buysellads.com", "carbonads.com", "carbonads.net", "intellitxt.com",
    "connect.facebook.net", "pixel.facebook.com", "ads-twitter.com", "static.ads-twitter.com", "analytics.twitter.com",
    "ads.linkedin.com", "px.ads.linkedin.com", "bat.bing.com", "clarity.ms", "newrelic.com", "nr-data.net",
    "segment.io", "segment.com", "optimizely.com", "crazyegg.com", "mouseflow.com", "fullstory.com", "omtrdc.net",
    "2o7.net", "coremetrics.com", "imrworldwide.com", "statcounter.com", "histats.com", "addthis.com", "sharethis.com",
    "adition.com", "adhigh.net", "advertserve.com", "adzerk.net", "ad.gt", "adlightning.com", "admixer.net",
    "appnexus.com", "ib-ibi.com", "rlcdn.com", "tapad.com", "agkn.com", "turn.com", "w55c.net", "eyeota.net",
    "lotame.com", "crwdcntrl.net", "id5-sync.com", "liadm.com", "permutive.com", "gumgum.com", "undertone.com",
};
/* path fragments that only ad servers use */
static const char *const builtin_paths[] = {
    "/pagead/", "/adserver", "/ads.js", "/adsbygoogle", "/doubleclick/", "/ad_banner", "/adframe", "/advert.", "/banner_ad",
    "/gampad/", "/prebid", "/adview", "/adclick", "/ads/banner", "/sponsor-ads",
};
/* class / id names of ad slots (whole word, case-insensitive) */
static const char *const builtin_hide[] = {
    "ad", "ads", "adv", "advert", "adverts", "advertisement", "advertising", "ad-banner", "ad-container", "ad-slot", "ad-unit",
    "ad-wrapper", "ad-box", "ad_box", "adbox", "adbanner", "adsbygoogle", "ad-leaderboard", "leaderboard-ad", "banner-ad",
    "banner_ad", "sidebar-ad", "top-ad", "bottom-ad", "sponsored", "sponsored-content", "sponsor", "dfp-ad", "gpt-ad",
    "google-ad", "google-ads", "adsense", "native-ad", "outbrain", "taboola", "OUTBRAIN", "ad-placeholder", "adplaceholder",
};
static const char *const builtin_hide_prefix[] = { "div-gpt-ad", "google_ads_", "ad-slot-", "adslot", "taboola-", "ezoic-ad" };

#define USER_LIST "C:\\WRLD\\adblock.txt"
static char **u_hosts, **u_hide, **u_allow;
static int n_hosts, n_hide, n_allow;
static uint32_t list_gen = 0xFFFFFFFF, list_size;

static void push(char ***arr, int *n, const char *s, int len) {
    *arr = krealloc(*arr, sizeof(char *) * (*n + 1));
    char *d = kmalloc(len + 1); memcpy(d, s, len); d[len] = 0;
    for (char *q = d; *q; q++) *q = tolower(*q);
    (*arr)[(*n)++] = d;
}
static void freelist(char ***arr, int *n) { for (int i = 0; i < *n; i++) kfree((*arr)[i]); kfree(*arr); *arr = NULL; *n = 0; }

static const char *default_list =
    "; XplorerLite ad blocker - your own rules, one per line.\r\n"
    ";   example.com        block pictures and scripts from example.com and its subdomains\r\n"
    ";   ##.promo-box       hide elements with class=\"promo-box\"\r\n"
    ";   ###top-banner      hide the element with id=\"top-banner\"\r\n"
    ";   @@example.org      allow everything on pages from example.org\r\n"
    "; Lines starting with ; or ! are comments. Changes apply to the next page you open.\r\n"
    "\r\n";

static void load_user_list(void) {
    vnode_t *n = vfs_resolve(USER_LIST, NULL);
    if (!n) { vfs_write_file(USER_LIST, default_list, strlen(default_list)); n = vfs_resolve(USER_LIST, NULL); }
    if (n && list_gen == vfs_generation && list_size == n->size) return;
    list_gen = vfs_generation; list_size = n ? n->size : 0;
    freelist(&u_hosts, &n_hosts); freelist(&u_hide, &n_hide); freelist(&u_allow, &n_allow);
    if (!n || n->dir) return;
    const char *s = (const char *)n->data, *end = s + n->size;
    while (s < end) {
        const char *e = s; while (e < end && *e != '\n' && *e != '\r') e++;
        const char *a = s, *b = e;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (b > a && *a != ';' && *a != '!' && *a != '#' ) {
            if (b - a > 2 && a[0] == '@' && a[1] == '@') push(&u_allow, &n_allow, a + 2, b - a - 2);
            else {                                    /* also accept "||example.com^" from adblock lists */
                if (b - a > 2 && a[0] == '|' && a[1] == '|') a += 2;
                if (b > a && b[-1] == '^') b--;
                if (b > a && !memchr(a, '/', b - a)) push(&u_hosts, &n_hosts, a, b - a);
            }
        } else if (b - a > 3 && a[0] == '#' && a[1] == '#' && (a[2] == '.' || a[2] == '#')) push(&u_hide, &n_hide, a + 2, b - a - 2);
        s = e; while (s < end && (*s == '\n' || *s == '\r')) s++;
    }
}

static bool host_matches(const char *host, const char *rule) {
    int hl = strlen(host), rl = strlen(rule);
    if (rl > hl) return false;
    if (strcasecmp(host + hl - rl, rule)) return false;
    return hl == rl || host[hl - rl - 1] == '.';
}
static void host_of(const char *url, char *host, int cap) {
    char sch[8], path[8]; uint16_t port;
    host[0] = 0;
    url_parse(url, sch, host, cap, &port, path, sizeof path);
}

/* is the page itself on the allow list? */
bool adblock_page_allowed(const char *page_url) {
    if (!adblock_enabled) return true;
    load_user_list();
    char h[256]; host_of(page_url, h, sizeof h);
    for (int i = 0; i < n_allow; i++) if (host_matches(h, u_allow[i])) return true;
    return false;
}

/* should a sub-resource (picture, script, frame) be refused? */
bool adblock_blocks(const char *url, const char *page_url) {
    if (!adblock_enabled) return false;
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return false;
    if (page_url && adblock_page_allowed(page_url)) return false;
    load_user_list();
    char h[256]; host_of(url, h, sizeof h);
    if (!h[0]) return false;
    for (unsigned i = 0; i < ARRAY_LEN(builtin_hosts); i++) if (host_matches(h, builtin_hosts[i])) { adblock_total++; return true; }
    for (int i = 0; i < n_hosts; i++) if (host_matches(h, u_hosts[i])) { adblock_total++; return true; }
    const char *path = strchr(strstr(url, "://") + 3, '/');
    if (path) {
        char low[512]; int k = 0;
        for (; path[k] && k < 511; k++) low[k] = tolower(path[k]);
        low[k] = 0;
        for (unsigned i = 0; i < ARRAY_LEN(builtin_paths); i++) if (strstr(low, builtin_paths[i])) { adblock_total++; return true; }
    }
    return false;
}

/* does a class/id word name an ad slot? */
static bool word_is_ad(const char *w, int n, bool is_id) {
    char low[64]; if (n <= 0 || n >= 64) return false;
    for (int i = 0; i < n; i++) low[i] = tolower(w[i]);
    low[n] = 0;
    for (unsigned i = 0; i < ARRAY_LEN(builtin_hide); i++) if (!strcasecmp(low, builtin_hide[i])) return true;
    for (unsigned i = 0; i < ARRAY_LEN(builtin_hide_prefix); i++) if (!strncasecmp(low, builtin_hide_prefix[i], strlen(builtin_hide_prefix[i]))) return true;
    for (int i = 0; i < n_hide; i++) if ((u_hide[i][0] == (is_id ? '#' : '.')) && !strcmp(low, u_hide[i] + 1)) return true;
    return false;
}
bool adblock_hides(const char *cls, const char *id) {
    if (!adblock_enabled) return false;
    if (id && word_is_ad(id, strlen(id), true)) return true;
    for (const char *s = cls; s && *s;) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        const char *e = s; while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (e > s && word_is_ad(s, e - s, false)) return true;
        s = e;
    }
    return false;
}
int adblock_rule_count(void) { load_user_list(); return (int)(ARRAY_LEN(builtin_hosts) + ARRAY_LEN(builtin_paths) + ARRAY_LEN(builtin_hide)) + n_hosts + n_hide; }
void adblock_reload(void) { list_gen = 0xFFFFFFFF; load_user_list(); }
