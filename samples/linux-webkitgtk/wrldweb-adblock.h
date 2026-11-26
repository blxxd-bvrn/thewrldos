/* The ad blocker for the WebKit browser (included by wrldweb.c).
 *  - WebKit's content blocker: requests to about 165 ad and tracking servers, and to the paths
 *    YouTube serves its ads and ad reports from, are never made; elements that hold ads are
 *    hidden. The rules come from /usr/share/wrld/adblock.json (tools/gen_adblock_rules.py,
 *    the same lists as The WRLD Browser's own blocker) plus the user's C:\WRLD\adblock.txt:
 *    "example.com" blocks a server, "##.class" / "###id" hides, "@@example.org" allows a site.
 *  - On YouTube a page script takes the ads out of the player's data before the player reads
 *    them (the way uBlock Origin does), skips any ad that still starts, and removes the
 *    "ad blockers are not allowed" dialog, resuming the video it paused.
 * Internet Options' "Block advertisements and trackers" turns all of it on and off. */
#define ADBLOCK_RULES "/usr/share/wrld/adblock.json"
#define ADBLOCK_USER "/mnt/c/WRLD/adblock.txt"

static WebKitUserContentManager *ucm;          /* shared by every tab */
static WebKitUserContentFilterStore *ab_store;
static WebKitUserContentFilter *ab_filter;
static WebKitUserScript *ab_script;
static gboolean ab_on = TRUE, ab_applied;

static const char ab_youtube_js[] =
"(() => {\n"
"  if (window.__wrldAdblock) return; window.__wrldAdblock = true;\n"
"  const AD_KEYS = ['adPlacements', 'adSlots', 'playerAds', 'adBreakHeartbeatParams', 'adParams', 'adBreakParams'];\n"
"  const prune = (o, depth) => {\n"
"    if (!o || typeof o !== 'object' || depth > 4) return o;\n"
"    for (const k of AD_KEYS) if (k in o) delete o[k];\n"
"    if (o.auxiliaries && o.auxiliaries.enforcementMessageViewModel) delete o.auxiliaries.enforcementMessageViewModel;\n"
"    if (o.playerResponse) prune(o.playerResponse, depth + 1);\n"
"    if (o.response) prune(o.response, depth + 1);\n"
"    if (Array.isArray(o.onResponseReceivedEndpoints)) o.onResponseReceivedEndpoints.forEach(e => prune(e, depth + 1));\n"
"    return o;\n"
"  };\n"
"  /* the player's data arrives as page variables, through JSON.parse and through fetch() */\n"
"  const parse = JSON.parse;\n"
"  JSON.parse = function (text, reviver) { const r = parse.call(this, text, reviver); try { prune(r, 0); } catch (e) {} return r; };\n"
"  const rjson = Response.prototype.json;\n"
"  Response.prototype.json = function () { return rjson.call(this).then(r => { try { prune(r, 0); } catch (e) {} return r; }); };\n"
"  for (const name of ['ytInitialPlayerResponse', 'ytInitialData']) {\n"
"    let v;\n"
"    try { Object.defineProperty(window, name, { configurable: true, get() { return v; }, set(x) { v = prune(x, 0); } }); } catch (e) {}\n"
"  }\n"
"  /* an ad that still starts is run to its end at once, muted; the warning dialog goes */\n"
"  setInterval(() => {\n"
"    const ad = document.querySelector('.ad-showing video, .ad-interrupting video');\n"
"    if (ad && isFinite(ad.duration) && ad.duration > 0) { ad.muted = true; ad.currentTime = ad.duration; }\n"
"    const skip = document.querySelector('.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, button[id^=\"skip-button\"]');\n"
"    if (skip) skip.click();\n"
"    const warn = document.querySelector('ytd-enforcement-message-view-model');\n"
"    if (warn) {\n"
"      const box = warn.closest('tp-yt-paper-dialog') || warn;\n"
"      box.remove();\n"
"      document.querySelectorAll('tp-yt-iron-overlay-backdrop').forEach(b => b.remove());\n"
"      document.body && (document.body.style.overflow = '');\n"
"      const v = document.querySelector('#movie_player video');\n"
"      if (v && v.paused) v.play().catch(() => {});\n"
"    }\n"
"  }, 400);\n"
"})();\n";

static void ab_saved(GObject *src, GAsyncResult *res, gpointer d) {
    (void)d; GError *e = NULL;
    WebKitUserContentFilter *f = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(src), res, &e);
    if (!f) { g_printerr("wrldweb: the ad blocker's rules did not compile: %s\n", e ? e->message : "?"); g_clear_error(&e); return; }
    if (ab_filter) webkit_user_content_filter_unref(ab_filter);
    ab_filter = f;
    ab_applied = FALSE;
    if (ab_on) {
        webkit_user_content_manager_remove_all_filters(ucm);
        webkit_user_content_manager_add_filter(ucm, ab_filter);
        ab_applied = TRUE;
    }
}
static void json_str(GString *s, const char *t) {
    g_string_append_c(s, '"');
    for (; *t; t++) { if (*t == '"' || *t == '\\') g_string_append_c(s, '\\'); g_string_append_c(s, *t); }
    g_string_append_c(s, '"');
}
/* the user's own list: servers to block, elements to hide, sites to leave alone */
static void ab_user_rules(GString *j) {
    char *text = NULL;
    if (!g_file_get_contents(ADBLOCK_USER, &text, NULL, NULL)) return;
    char **lines = g_strsplit_set(text, "\r\n", -1);
    GString *allow = g_string_new(NULL), *hide = g_string_new(NULL);
    for (char **l = lines; *l; l++) {
        char *t = g_strstrip(*l);
        if (!*t || *t == ';' || *t == '!') continue;
        if (t[0] == '#' && t[1] == '#' && (t[2] == '.' || t[2] == '#') && t[3]) {
            if (hide->len) g_string_append(hide, ", ");
            g_string_append(hide, t + 2);
            continue;
        }
        if (*t == '#') continue;
        if (t[0] == '@' && t[1] == '@' && t[2]) {
            if (allow->len) g_string_append_c(allow, ',');
            char *dom = g_strconcat("*", g_ascii_strdown(t + 2, -1), NULL);
            json_str(allow, dom); g_free(dom);
            continue;
        }
        if (t[0] == '|' && t[1] == '|') t += 2;
        size_t n = strlen(t); if (n && t[n - 1] == '^') t[n - 1] = 0;
        if (!*t || strchr(t, '/')) continue;
        char *esc = g_regex_escape_string(g_ascii_strdown(t, -1), -1);
        char *re = g_strconcat("^[a-z]+://([^/:]*\\.)?", esc, "[/:]", NULL);
        g_string_append(j, ",{\"trigger\":{\"url-filter\":"); json_str(j, re); g_string_append(j, "},\"action\":{\"type\":\"block\"}}");
        g_free(re); g_free(esc);
    }
    if (hide->len) { g_string_append(j, ",{\"trigger\":{\"url-filter\":\".*\"},\"action\":{\"type\":\"css-display-none\",\"selector\":"); json_str(j, hide->str); g_string_append(j, "}}"); }
    if (allow->len) g_string_append_printf(j, ",{\"trigger\":{\"url-filter\":\".*\",\"if-top-url\":[%s]},\"action\":{\"type\":\"ignore-previous-rules\"}}", allow->str);
    g_string_free(allow, TRUE); g_string_free(hide, TRUE);
    g_strfreev(lines); g_free(text);
}
/* the rules again (at start, and when Internet Options change, which also covers an edited list) */
static void ab_load(void) {
    char *base = NULL; gsize n = 0;
    if (!g_file_get_contents(ADBLOCK_RULES, &base, &n, NULL) || n < 2) { g_free(base); base = g_strdup("[]"); n = 2; }
    GString *j = g_string_new_len(base, (gssize)n);
    while (j->len && j->str[j->len - 1] != ']') g_string_truncate(j, j->len - 1);
    if (j->len) g_string_truncate(j, j->len - 1);
    gsize mark = j->len;
    ab_user_rules(j);
    if (mark == 1 && j->len > 1 && j->str[1] == ',') g_string_erase(j, 1, 1);   /* no built-in rules: no leading comma */
    g_string_append_c(j, ']');
    gsize len = j->len;
    GBytes *b2 = g_bytes_new_take(g_string_free(j, FALSE), len);
    g_free(base);
    if (!ab_store) ab_store = webkit_user_content_filter_store_new("/tmp/wrldweb-filters");
    webkit_user_content_filter_store_save(ab_store, "wrld-adblock", b2, NULL, ab_saved, NULL);
    g_bytes_unref(b2);
}
static void ab_init(void) {
    ucm = webkit_user_content_manager_new();
    static const char *const yt[] = { "https://*.youtube.com/*", "https://youtube.com/*", "https://*.youtube-nocookie.com/*", NULL };
    ab_script = webkit_user_script_new(ab_youtube_js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, yt, NULL);
    if (ab_on) webkit_user_content_manager_add_script(ucm, ab_script);
    ab_load();
}
static void ab_set(gboolean on) {
    if (on == ab_on) return;
    ab_on = on;
    if (!ucm) return;
    if (on) { webkit_user_content_manager_add_script(ucm, ab_script); ab_load(); }
    else { webkit_user_content_manager_remove_all_filters(ucm); webkit_user_content_manager_remove_all_scripts(ucm); ab_applied = FALSE; }
}
