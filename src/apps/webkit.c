/* The WRLD Browser on WebKit (64-bit edition): browser_open() starts WebKitGTK's
 * browser, samples/linux-webkitgtk/wrldweb.c, through the wrldcomp Wayland
 * compositor when the image carries it and the machine has the memory; otherwise
 * (and on the 32-bit edition, or with "browser = classic" in the settings) it opens
 * the built-in browser. One WebKit browser runs at a time: later requests are
 * written to a file it watches, and its window is brought to the front. */
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"
#include "disk.h"

void browser_classic_open(const char *url);

#ifdef __x86_64__
#define WK_COMP "C:\\Linux\\usr\\local\\bin\\wrldcomp"
#define WK_WEB  "C:\\Linux\\usr\\local\\bin\\wrldweb"
#define WK_REQ_DIR "C:\\WRLD\\Temp"
#define WK_REQ  WK_REQ_DIR "\\wrldweb-open.txt"
#define WK_MIN_MB 1600                          /* the kernel heap WebKit needs to start and load a page */
extern int lx_launch(const char *wrld_path, int argc, char **argv, char **envp, char *err, int ecap);
extern int lxwin_browser_pid;
extern window_t *lxwin_window_of(int pid);
static int wk_pid;

/* can the WebKit browser run here? why not, for the log */
bool webkit_browser_available(char *why, int cap) {
    const char *pref = settings_get("browser");
    if (pref && !strcasecmp(pref, "classic")) { snprintf(why, cap, "the classic browser is chosen in the settings"); return false; }
    if (!vfs_resolve(WK_WEB, NULL) || !vfs_resolve(WK_COMP, NULL)) { snprintf(why, cap, "this image was built without WebKit"); return false; }
    if (mm_total() / (1024 * 1024) < WK_MIN_MB) { snprintf(why, cap, "it needs about %d MB of memory", WK_MIN_MB + 400); return false; }
    return true;
}
static bool running(void) { return wk_pid && task_find(wk_pid); }

/* Internet Options for WebKit: a key=value file it reads at start and again when told
 * ("wrld:options" in the request file) */
#define WK_OPTS WK_REQ_DIR "\\wrldweb-options.txt"
#define WK_PROFILE "C:\\Documents and Settings\\User\\Application Data\\WRLD Browser"
extern bool js_enabled, browser_show_images, adblock_enabled;
static const char *opt(const char *k, const char *def) { const char *v = settings_get(k); return v && *v ? v : def; }
static void write_options(void) {
    char b[1200];
    bool proxy = atoi(opt("proxy_on", "0")) != 0;
    int n = snprintf(b, sizeof b, "home=%s\njavascript=%d\nimages=%d\nadblock=%d\npopups=%s\ncookies=%s\nproxy=%s\nsmooth=%s\n",
                     opt("home_page", "wrld:home"), js_enabled, browser_show_images, adblock_enabled, opt("popups", "0"), opt("cookies", "no-third-party"),
                     proxy ? opt("proxy", "") : "", opt("smooth_scroll", "0"));
    vfs_mkdir_p(WK_REQ_DIR);
    vfs_write_file(WK_OPTS, b, MIN(n, (int)sizeof b - 1));
}
/* append a line to the request file without bringing the window forward */
static void tell(const char *line) {
    vnode_t *n = vfs_resolve(WK_REQ, NULL);
    if (n && !n->dir) vfs_append_node(n, line, strlen(line));
    else vfs_write_file(WK_REQ, line, strlen(line));
}
void webkit_options_changed(void) { write_options(); if (running()) tell("wrld:options\n"); }
/* Delete Browsing History: a running browser clears its own data (WebKit's website data
 * manager); otherwise the profile's files go: cookies and saved site data (the cache is in
 * memory and goes when the browser exits). False if there is no WebKit here. */
bool webkit_clear_data(void) {
    if (!vfs_resolve(WK_WEB, NULL)) return false;
    if (running()) { tell("wrld:clear-data\n"); return true; }
    vnode_t *d = vfs_resolve(WK_PROFILE, NULL);
    if (d && d->dir) while (d->child) if (!vfs_delete(d->child)) break;
    return true;
}

/* hand an address (or "" for a new tab with the home page) to the running browser */
static void request(const char *url) {
    vfs_mkdir_p(WK_REQ_DIR);
    char line[1100]; snprintf(line, sizeof line, "%s\n", url ? url : "");
    vnode_t *n = vfs_resolve(WK_REQ, NULL);
    if (n && !n->dir) vfs_append_node(n, line, strlen(line));
    else vfs_write_file(WK_REQ, line, strlen(line));
    window_t *w = lxwin_window_of(wk_pid);
    if (w) wm_focus(w);
}

static bool webkit_open(const char *url) {
    char file[1100];
    if (url && isalpha((unsigned char)url[0]) && url[1] == ':' && (url[2] == '\\' || url[2] == '/')) {   /* C:\x.html -> file:///mnt/c/x.html */
        int o = snprintf(file, sizeof file, "file:///mnt/%c/", tolower((unsigned char)url[0]));
        for (const char *p = url + 3; *p && o < (int)sizeof file - 1; p++) file[o++] = *p == '\\' ? '/' : *p;
        file[o] = 0; url = file;
    }
    if (running()) { request(url); return true; }
    vfs_mkdir_p(WK_REQ_DIR);
    vfs_write_file(WK_REQ, "", 0);                 /* nothing pending from an earlier run */
    write_options();
    char docs[200], prof[200], who[64];
    snprintf(docs, sizeof docs, "WRLD_DOCS=/mnt/c/Documents and Settings/User/My Documents");
    snprintf(prof, sizeof prof, "WRLD_PROFILE=/mnt/c/Documents and Settings/User/Application Data/WRLD Browser");
    snprintf(who, sizeof who, "WRLD_USER=%s", user_name[0] ? user_name : "User");
    /* WebKit's JavaScript JIT writes SSE4.1 instructions (2008 and later processors); on an older
     * processor, or a virtual one without them, scripts run in its interpreter instead */
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    bool sse41 = (c >> 19) & 1;
    if (!sse41) klog("browser: the processor has no SSE4.1: WebKit runs JavaScript without its JIT");
    char *env[] = { "PATH=/usr/local/bin:/usr/bin:/bin", "HOME=/home/user", "USER=user", "LOGNAME=user", "SHELL=/bin/sh",
                    "LANG=C.UTF-8", "PWD=/", docs, prof, who, "WRLD_REQUEST=/mnt/c/WRLD/Temp/wrldweb-open.txt", "WRLD_OPTIONS=/mnt/c/WRLD/Temp/wrldweb-options.txt",
                    sse41 ? NULL : "JSC_useJIT=false", NULL };
    char *argv[4] = { "wrldcomp", "/usr/local/bin/wrldweb", NULL, NULL };
    int argc = 2;
    if (url && *url) argv[argc++] = (char *)url;
    char err[160];
    int pid = lx_launch(WK_COMP, argc, argv, env, err, sizeof err);
    if (pid < 0) { klog("browser: WebKit did not start: %s", err); return false; }
    wk_pid = lxwin_browser_pid = pid;
    klog("browser: WebKit started (pid %d)", pid);
    return true;
}
#else
bool webkit_browser_available(char *why, int cap) { snprintf(why, cap, "WebKit needs the 64-bit edition"); return false; }
void webkit_options_changed(void) {}
bool webkit_clear_data(void) { return false; }
static bool webkit_open(const char *url) { (void)url; return false; }
#endif

void browser_open(const char *url) {
    char why[96] = "it did not start";
    if (webkit_browser_available(why, sizeof why) && webkit_open(url)) return;
    /* the classic engine stands in: say why, once, unless the user chose it */
    static bool told;
    const char *pref = settings_get("browser");
    if (!told && !(pref && !strcasecmp(pref, "classic"))) {
        char m[200]; snprintf(m, sizeof m, "The WRLD Browser is using its classic engine: WebKit could not start (%s).", why);
        notify_balloon("The WRLD Browser", m);
        told = true;
    }
    browser_classic_open(url);
}
