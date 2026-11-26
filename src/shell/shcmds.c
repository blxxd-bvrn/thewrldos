/* The shell's commands that front other parts of the system (network, downloads,
 * JavaScript, display, sound, processes, disks, devices, packages, themes, power): the
 * shell itself is in Rust (rust/src/kernel/shk.rs), which calls these through
 * sh_c_command with the working folder and the command's standard input, output and error. */
#include "kernel.h"
#include "devmgr.h"
#include "jsapi.h"
#include "wm.h"
#include "apps.h"
#include "pkg.h"
#include "theme.h"
#include "disk.h"
#include "net.h"
#include "audio.h"

typedef struct { console_t *in, *out, *err; } io_t;

static void out_s(io_t *io, const char *s) { io->out->write(io->out, s, strlen(s)); }
static void out_f(io_t *io, const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); io->out->write(io->out, b, MIN(n, 1023)); }
static void err_f(io_t *io, const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); io->err->write(io->err, "\x1b[91m", 5); io->err->write(io->err, b, MIN(n, 511)); io->err->write(io->err, "\x1b[0m", 4); }

static int b_uname(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc > 1 && !strcmp(argv[1], "-a")) out_f(io, "WRLD %s %s.%s WRLD-Kernel " OS_MACHINE " %s\n", computer_name, OS_VERSION, OS_BUILD, OS_NAME);
    else if (argc > 1 && !strcmp(argv[1], "-r")) out_f(io, "%s.%s\n", OS_VERSION, OS_BUILD);
    else if (argc > 1 && !strcmp(argv[1], "-m")) out_s(io, OS_MACHINE "\n");
    else out_s(io, "WRLD\n");
    return 0;
}
static int b_ver(const char *cwd, int argc, char **argv, io_t *io) { out_f(io, "\n%s %s [Version %s.%s]\n\n", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD); return 0; }
static int b_whoami(const char *cwd, int argc, char **argv, io_t *io) { out_f(io, "%s\n", user_name); return 0; }
static int b_ipconfig(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc > 1 && (!strcasecmp(argv[1], "/renew") || !strcmp(argv[1], "-r"))) {
        out_s(io, "Renewing the address (DHCP)...\n");
        dhcp_renew();
        if (!net_wait_ready(10000)) { err_f(io, "ipconfig: no DHCP server answered\n"); return 1; }
    }
    if (argc > 1 && (!strcasecmp(argv[1], "/dns") || !strcmp(argv[1], "-dns"))) {
        /* set the DNS server by hand (until the next DHCP renewal), like netsh's "set dns" */
        uint32_t ip;
        if (argc < 3 || !str_to_ip(argv[2], &ip)) { err_f(io, "usage: ipconfig /dns A.B.C.D\n"); return 1; }
        netif.dns = ip;
    }
    const char *cn = settings_get("net_name"); if (!cn || !*cn) cn = "Local Area Connection";
    out_f(io, "\n" OS_NAME " IP Configuration\n\n   Host Name . . . . . . . . : %s\n\nEthernet adapter %s:\n\n", computer_name, cn);
    if (!netif.present) { out_s(io, "   Media State . . . . . . . : No supported network adapter was found\n"); return 1; }
    char a[16], m[16], g[16], d[16], sv[16];
    ip_to_str(netif.ip, a); ip_to_str(netif.mask, m); ip_to_str(netif.gw, g); ip_to_str(netif.dns, d); ip_to_str(netif.dhcp_server, sv);
    out_f(io, "   Description . . . . . . . : %s\n", netif.driver);
    out_f(io, "   Physical Address. . . . . : %02X-%02X-%02X-%02X-%02X-%02X\n", netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
    if (!net_enabled()) { out_s(io, "   Media State . . . . . . . : Disabled\n"); return 0; }
    if (!netif.link) { out_s(io, "   Media State . . . . . . . : Media disconnected\n"); return 0; }
    bool st = net_is_static();
    out_f(io, "   DHCP Enabled. . . . . . . : %s\n   IP Address. . . . . . . . : %s\n   Subnet Mask . . . . . . . : %s\n   Default Gateway . . . . . : %s\n",
          st ? "No" : "Yes", netif.configured ? a : "(acquiring...)", m, g);
    if (!st) out_f(io, "   DHCP Server . . . . . . . : %s\n", sv);
    out_f(io, "   DNS Server. . . . . . . . : %s\n", d);
    out_f(io, "   Packets sent/received . . : %u / %u (%u KB / %u KB)\n", netif.tx_packets, netif.rx_packets, netif.tx_bytes / 1024, netif.rx_bytes / 1024);
    return 0;
}
static int b_ping(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: ping HOST [-n COUNT]\n"); return 1; }
    int count = 4; const char *host = NULL;
    for (int i = 1; i < argc; i++) { if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-c")) && i + 1 < argc) count = atoi(argv[++i]); else host = argv[i]; }
    uint32_t ip; char err[160];
    if (!host || !dns_resolve(host, &ip, err, sizeof err)) { err_f(io, "ping: %s\n", host ? err : "no host"); return 1; }
    char a[16]; ip_to_str(ip, a);
    out_f(io, "\nPinging %s [%s] with 32 bytes of data:\n", host, a);
    int ok = 0, mn = 1 << 30, mx = 0, sum = 0;
    for (int i = 0; i < count; i++) {
        int ms = icmp_ping(ip, i + 1, 3000);
        if (ms < 0) out_s(io, "Request timed out.\n");
        else { out_f(io, "Reply from %s: bytes=32 time=%dms TTL=64\n", a, ms); ok++; mn = MIN(mn, ms); mx = MAX(mx, ms); sum += ms; }
        if (i + 1 < count) task_sleep(1000 - MIN(ms < 0 ? 0 : ms, 900));
    }
    out_f(io, "\nPing statistics for %s:\n    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss)\n", a, count, ok, count - ok, (count - ok) * 100 / MAX(1, count));
    if (ok) out_f(io, "Approximate round trip times: Minimum = %dms, Maximum = %dms, Average = %dms\n", mn, mx, sum / ok);
    return ok ? 0 : 1;
}
static int b_nslookup(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: nslookup NAME\n"); return 1; }
    char d[16]; ip_to_str(netif.dns, d);
    out_f(io, "Server:  %s\n\n", d);
    uint32_t ip; char err[160];
    if (!dns_resolve(argv[1], &ip, err, sizeof err)) { err_f(io, "*** %s\n", err); return 1; }
    char a[16]; ip_to_str(ip, a);
    out_f(io, "Name:    %s\nAddress: %s\n", argv[1], a);
    return 0;
}
typedef struct { io_t *io; uint32_t last; } dlprog_t;
static void dl_progress(void *ctx, const char *status, uint32_t got, uint32_t total) {
    dlprog_t *p = ctx;
    if (got && timer_ticks() - p->last < 500) return;
    p->last = timer_ticks();
    if (got) { if (total) out_f(p->io, "  %u / %u KB\n", got / 1024, total / 1024); else out_f(p->io, "  %u KB\n", got / 1024); }
    else out_f(p->io, "%s\n", status);
}
static int b_wget(const char *cwd, int argc, char **argv, io_t *io) {
    bool curl = !strcmp(argv[0], "curl");
    const char *url = NULL, *outf = NULL; bool headers_only = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-O") || !strcmp(argv[i], "-o")) && i + 1 < argc) outf = argv[++i];
        else if (!strcmp(argv[i], "-I")) headers_only = true;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "-s")) quiet = true;
        else url = argv[i];
    }
    if (!url) { err_f(io, "usage: %s URL [-O FILE]%s\n", argv[0], curl ? " [-I] [-s]" : " [-q]"); return 1; }
    char full[1024];
    if (!strstr(url, "://")) snprintf(full, sizeof full, "http://%s", url); else strlcpy(full, url, sizeof full);
    http_response_t r;
    dlprog_t pg = { io, 0 };
    bool ok = http_get(full, &r, quiet || curl ? NULL : dl_progress, &pg);
    if (!ok) { err_f(io, "%s: %s\n", argv[0], r.error); http_free(&r); return 1; }
    if (!quiet && (!curl || headers_only)) {
        out_f(io, "HTTP %d, %u bytes, %s%s%s\n", r.status, r.body_len, r.content_type[0] ? r.content_type : "unknown type",
              r.secure ? ", " : "", r.secure ? r.tls_info : "");
        if (strcmp(r.url, full)) out_f(io, "Redirected to %s\n", r.url);
    }
    if (headers_only) { http_free(&r); return 0; }
    if (curl && !outf) { io->out->write(io->out, (const char *)r.body, r.body_len); if (r.body_len && r.body[r.body_len - 1] != '\n') out_s(io, "\n"); http_free(&r); return r.status >= 400; }
    char name[256];
    if (outf) strlcpy(name, outf, sizeof name);
    else {
        char sch[8], host[256], path[1024]; uint16_t port;
        url_parse(r.url, sch, host, sizeof host, &port, path, sizeof path);
        char *q = strchr(path, '?'); if (q) *q = 0;
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        strlcpy(name, *b ? b : "index.html", sizeof name);
    }
    char dst[512];
    if (isalpha(name[0]) && name[1] == ':') strlcpy(dst, name, sizeof dst); else path_join(dst, sizeof dst, cwd, name);
    path_normalize(dst);
    if (!vfs_write_file(dst, r.body, r.body_len)) { err_f(io, "%s: cannot write %s\n", argv[0], dst); http_free(&r); return 1; }
    if (!quiet) out_f(io, "Saved %s (%u bytes)\n", dst, r.body_len);
    http_free(&r);
    return 0;
}
static int b_sha256sum(const char *cwd, int argc, char **argv, io_t *io) {
    for (int i = 1; i < argc; i++) {
        vnode_t *n = vfs_resolve(argv[i], cwd);
        if (!n || n->dir) { err_f(io, "sha256sum: %s: not found\n", argv[i]); continue; }
        char h[65]; sha256_hex(n->data, n->size, h);
        out_f(io, "%s  %s\n", h, argv[i]);
    }
    return 0;
}
/* The console stores one byte per cell and draws a byte above 0x7F as that
 * Latin-1 character, so UTF-8 from QuickJS is narrowed here. Characters with
 * no Latin-1 form are transliterated when they have an obvious ASCII (or
 * punctuation) spelling, and '?' otherwise. The caller sizes the buffer for
 * n * 4 bytes: U+2122 becomes "(TM)", which is wider than its UTF-8 form. */
static const char *latin1_translit(uint32_t cp) {
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2013: case 0x2212: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2026: return "...";
    case 0x2022: case 0x25CF: case 0x2219: case 0x25AA: return "\xB7";
    case 0x2122: return "(TM)";
    case 0x20AC: return "EUR";
    case 0x2190: return "<-"; case 0x2192: return "->"; case 0x2191: return "^"; case 0x2193: return "v";
    case 0x2039: return "<"; case 0x203A: return ">";
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: case 0x205F: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: return "";
    case 0x2264: return "<="; case 0x2265: return ">="; case 0x2260: return "!=";
    case 0x2713: case 0x2714: return "v"; case 0x2715: case 0x2716: case 0x2717: return "x";
    case 0x2605: case 0x2606: return "*";
    case 0x25B2: case 0x25B4: return "^"; case 0x25BC: case 0x25BE: return "v";
    case 0x00A0: return "\xA0";
    }
    if (cp >= 0x1F000) return "";                       /* emoji and pictographs */
    return "?";
}
/* UTF-8 to Latin-1. A byte that starts no valid sequence is taken as
 * Windows-1252, which is what a mislabelled document means by it. */
static int latin1_from_utf8(const uint8_t *s, uint32_t n, char *out) {
    static const uint16_t cp1252[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    int o = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80 && (s[i + 3] & 0xC0) == 0x80)
            { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = c >= 0x80 && c < 0xA0 ? (cp1252[c - 0x80] ? cp1252[c - 0x80] : '?') : c; len = 1; }
        i += len;
        if (cp == '\r') continue;
        if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) out[o++] = (char)cp;
        else { const char *t = latin1_translit(cp); while (*t) out[o++] = *t++; }
    }
    out[o] = 0;
    return o;
}
/* js: run JavaScript (QuickJS) */
static void js_out(void *u, const char *t) {
    io_t *io = u;
    int n = strlen(t); char *l1 = kmalloc(n * 4 + 1);
    int l = latin1_from_utf8((const uint8_t *)t, n, l1); l1[l] = 0;
    out_s(io, l1); kfree(l1);
}
static int b_js(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        out_f(io, "JavaScript engine: QuickJS-ng %s\n\nusage: js FILE.js        run a script file\n       js -e \"CODE\"      run code and print the result, e.g. js -e \"[1,2,3].map(x => x * x)\"\n", js_engine_version());
        return 0;
    }
    if (!strcmp(argv[1], "-e")) {
        char code[2048]; code[0] = 0;
        for (int i = 2; i < argc; i++) { if (i > 2) strlcat(code, " ", sizeof code); strlcat(code, argv[i], sizeof code); }
        return js_shell_run(code, strlen(code), "<command line>", js_out, io);
    }
    vnode_t *n = vfs_resolve(argv[1], cwd);
    if (!n || n->dir) { err_f(io, "js: %s: not found\n", argv[1]); return 1; }
    char path[300]; vfs_path(n, path, sizeof path);
    return js_shell_run((const char *)n->data, n->size, path, js_out, io);
}
static int b_display(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        out_f(io, "Adapter:    %s\nResolution: %d x %d, %u-bit scanout (32-bit rendering)\n", video_driver_name(), SCREEN_W, SCREEN_H, fb_info.bpp);
        char diag[640]; gfx_display_diagnostics(diag, sizeof diag); out_s(io, diag);
        if (!video_can_set_mode()) { out_s(io, "This adapter cannot change resolution.\n"); return 0; }
        static const int mw[] = { 1024, 1152, 1280, 1280, 1280, 1280, 1366, 1440, 1600, 1600, 1680, 1920, 1920, 2560 }, mh[] = { 768, 864, 720, 800, 960, 1024, 768, 900, 900, 1200, 1050, 1080, 1200, 1440 };
        out_s(io, "Available:  ");
        for (unsigned i = 0; i < ARRAY_LEN(mw); i++) if (video_mode_ok(mw[i], mh[i])) out_f(io, "%dx%d ", mw[i], mh[i]);
        out_s(io, "\nusage: display WIDTHxHEIGHT\n");
        return 0;
    }
    int w = atoi(argv[1]); const char *x = strchr(argv[1], 'x'); int h = x ? atoi(x + 1) : 0;
    if (!w || !h) { err_f(io, "display: use WIDTHxHEIGHT, e.g. display 1280x800\n"); return 1; }
    if (!gfx_set_resolution(w, h)) { err_f(io, "display: %dx%d is not supported by %s\n", w, h, video_driver_name()); return 1; }
    out_f(io, "Resolution set to %d x %d\n", SCREEN_W, SCREEN_H);
    return 0;
}
static int b_play(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: play FILE [-w]   (WAV, AIFF, .au, FLAC, MP3, Ogg; -w waits until it has finished)\n"); return 1; }
    char full[512]; vnode_t *n = vfs_resolve(argv[1], cwd);
    if (!n) { err_f(io, "play: %s: not found\n", argv[1]); return 1; }
    vfs_path(n, full, sizeof full);
    char err[96];
    if (!audio_play_file(full, err, sizeof err)) { err_f(io, "play: %s\n", err); return 1; }
    if (argc > 2 && !strcmp(argv[2], "-w")) audio_wait(600000);
    return 0;
}
static int b_sndinfo(const char *cwd, int argc, char **argv, io_t *io) {
    char b[1024]; audio_diag(b, sizeof b);
    out_s(io, b);
    return 0;
}
static int b_volume(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc > 1) { if (!strcmp(argv[1], "mute")) audio_muted = true; else if (!strcmp(argv[1], "unmute")) audio_muted = false; else audio_volume = CLAMP(atoi(argv[1]), 0, 100); }
    out_f(io, "Sound device: %s\nVolume: %d%%%s\n", audio_device_name(), audio_volume, audio_muted ? " (muted)" : "");
    return 0;
}
static int b_hostname(const char *cwd, int argc, char **argv, io_t *io) { out_f(io, "%s\n", computer_name); return 0; }
static int b_ps(const char *cwd, int argc, char **argv, io_t *io) {
    static const char *kinds[] = { "kernel", "shell", "win32", "app" };
    out_f(io, "  PID  STATE     CPU(ms)   MEM(KB)  TYPE    NAME\n");
    for (task_t *t = task_list(); t; t = t->next) {
        const char *st = t->state == TASK_READY ? "running" : t->state == TASK_SLEEPING ? "sleeping" : t->state == TASK_BLOCKED ? "blocked" : "dead";
        out_f(io, "%5d  %-8s %8u %9u  %-6s  %s\n", t->pid, st, t->cpu_ms, t->mem_bytes / 1024, kinds[t->kind & 3], t->name);
    }
    return 0;
}
static int b_kill(const char *cwd, int argc, char **argv, io_t *io) {
    int rc = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') continue; int pid = atoi(argv[i]); if (!task_kill(pid)) { err_f(io, "kill: (%d) - No such process\n", pid); rc = 1; } }
    return rc;
}
static int b_sync(const char *cwd, int argc, char **argv, io_t *io) {
    if (!system_volume) { out_s(io, "sync: this is the live session from the CD; nothing is saved to a disk.\nInstall " OS_NAME " (desktop icon or Control Panel > System) to keep your files.\n"); return 0; }
    int n = persist_pending();
    persist_flush();
    char st[120]; persist_status(st, sizeof st);
    out_f(io, "sync: %d changed file(s) written. %s\n", n, st);
    return 0;
}
static int b_free(const char *cwd, int argc, char **argv, io_t *io) {
    out_f(io, "              total        used        free\nMem:     %10u  %10u  %10u  (KB)\n", mm_total() / 1024, mm_used() / 1024, (mm_total() - mm_used()) / 1024);
    out_f(io, "Physical: %llu MB installed\n", (unsigned long long)(mm_phys_total() / (1024 * 1024)));
    return 0;
}
static int b_uptime(const char *cwd, int argc, char **argv, io_t *io) {
    uint32_t t = timer_ticks() / 1000;
    out_f(io, "up %u:%02u:%02u, %d tasks, load: %u%% idle\n", t / 3600, (t / 60) % 60, t % 60, task_count(), timer_ticks() ? (uint32_t)((uint64_t)idle_ticks * 100 / timer_ticks()) : 0);
    return 0;
}
static int b_dmesg(const char *cwd, int argc, char **argv, io_t *io) { out_s(io, klog_buffer()); return 0; }
static int b_beep(const char *cwd, int argc, char **argv, io_t *io) { speaker_beep(argc > 1 ? atoi(argv[1]) : 880, argc > 2 ? atoi(argv[2]) : 150); return 0; }
static int b_mount(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        for (char l = 'C'; l <= 'Z'; l++) if (vfs_drive_kind(l)) out_f(io, "%c:  %-24s %s\n", l, vfs_drive_label(l), vfs_drive_kind(l) == 2 ? "(disk image, read-only)" : "(ramdisk)");
        return 0;
    }
    vnode_t *n = vfs_resolve(argv[1], cwd);
    if (!n || n->dir) { err_f(io, "mount: %s: not found\n", argv[1]); return 1; }
    char msg[400]; char l = dmg_mount(n, msg, sizeof msg);
    if (!l) { err_f(io, "mount: %s\n", msg); return 1; }
    out_f(io, "%s\n", msg); return 0;
}
static int b_hdiutil(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc >= 3 && (!strcmp(argv[1], "attach") || !strcmp(argv[1], "mount"))) { char *a[2] = { "mount", argv[2] }; return b_mount(cwd, 2, a, io); }
    if (argc >= 3 && (!strcmp(argv[1], "detach") || !strcmp(argv[1], "eject"))) {
        char l = toupper(argv[2][0]);
        if (!vfs_unmount(l)) { err_f(io, "hdiutil: cannot detach %s\n", argv[2]); return 1; }
        out_f(io, "\"%c:\" ejected.\n", l); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "imageinfo")) {
        vnode_t *n = vfs_resolve(argv[2], cwd);
        if (!n || n->dir) { err_f(io, "hdiutil: not found\n"); return 1; }
        char info[200]; uint32_t sz; uint8_t *d = dmg_extract(n->data, n->size, &sz, info, sizeof info);
        out_f(io, "%s\n", info); if (d) { out_f(io, "Decoded size: %u bytes\n", sz); kfree(d); }
        return d ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "info")) { char *a[1] = { "mount" }; return b_mount(cwd, 1, a, io); }
    out_s(io, "usage: hdiutil attach IMAGE.dmg | detach D: | imageinfo IMAGE.dmg | info\n");
    return 1;
}
typedef struct { char app[32]; char arg[256]; } launch_req_t;
static void do_launch(void *p) { launch_req_t *r = p; if (!strcmp(r->app, "::open")) shell_open(r->arg, NULL); else app_launch(r->app, r->arg[0] ? r->arg : NULL); kfree(r); }
static void gui_launch(const char *app, const char *arg) {
    launch_req_t *r = kzalloc(sizeof *r); strlcpy(r->app, app, sizeof r->app); if (arg) strlcpy(r->arg, arg, sizeof r->arg);
    wm_defer(do_launch, r);
}
static int b_open(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: open FILE|FOLDER|APP\n"); return 1; }
    vnode_t *n = vfs_resolve(argv[1], cwd);
    if (!n) { if (app_find(argv[1])) { gui_launch(argv[1], argc > 2 ? argv[2] : NULL); return 0; } err_f(io, "open: %s: not found\n", argv[1]); return 1; }
    char full[512]; vfs_path(n, full, sizeof full);
    gui_launch("::open", full);
    return 0;
}
#ifdef __x86_64__
/* dpkg, rpm, apt, apt-get: Linux software packages (src/apps/pkg.c) */
static void pkg_out(void *ctx, const char *line) { io_t *io = ctx; out_s(io, line); out_s(io, "\n"); }
static int b_pkg(const char *cwd, int argc, char **argv, io_t *io) { return pkg_cli(argc, argv, cwd, pkg_out, io); }
static int b_lxtrace(const char *cwd, int argc, char **argv, io_t *io) {
    (void)cwd;
    extern bool lx_strace;
    if (argc > 1) lx_strace = !strcasecmp(argv[1], "on");
    out_s(io, lx_strace ? "Linux system calls are logged (serial port and the boot log).\n" : "Linux system call logging is off.\n");
    return 0;
}
#endif
static int b_guiprof(const char *cwd, int argc, char **argv, io_t *io) {
    (void)cwd;
    extern bool gui_profile;
    if (argc > 1) gui_profile = !strcasecmp(argv[1], "on");
    out_s(io, gui_profile ? "Painting times are logged every 2 seconds (serial port and the boot log).\n" : "Painting times are not logged.\n");
    return 0;
}
static int b_theme(const char *cwd, int argc, char **argv, io_t *io) {
    if (argc < 2 || !strcmp(argv[1], "list")) {
        styles_load();
        out_f(io, "Current style: %s, hue %d, saturation %d, lightness %d\n\nVisual styles (" STYLE_DIR "\\*.wrldstyle):\n", style_name(theme_id), theme_hue, theme_sat, theme_light);
        for (int i = 0; i < THEME_COUNT; i++) out_f(io, "  %c %-18s %s\n", i == theme_id ? '*' : ' ', style_file(i), style_desc(i));
        out_s(io, "\nusage: theme STYLE [HUE 0-359]   e.g. theme aero, theme plex 200, theme \"Luna Blue\"\n"
                  "       theme saturation -100..100   theme lightness -60..60\n");
        return 0;
    }
    if (argc > 2 && (!strcasecmp(argv[1], "saturation") || !strcasecmp(argv[1], "lightness"))) {
        if (tolower(argv[1][0]) == 's') theme_set_tone(atoi(argv[2]), theme_light); else theme_set_tone(theme_sat, atoi(argv[2]));
        wm_redraw_all();
        out_f(io, "Saturation %d, lightness %d\n", theme_sat, theme_light);
        return 0;
    }
    static const struct { const char *alias, *file; } aliases[] = { { "blue", "Luna Blue" }, { "luna", "Luna Blue" }, { "olive", "Olive Green" }, { "classic", "WRLD Classic" } };
    const char *want = argv[1];
    for (unsigned i = 0; i < ARRAY_LEN(aliases); i++) if (!strcasecmp(want, aliases[i].alias)) want = aliases[i].file;
    styles_load();
    int id = style_find(want);
    if (id < 0) { err_f(io, "theme: no visual style called '%s' (try: theme list)\n", argv[1]); return 1; }
    theme_apply(id, argc > 2 ? atoi(argv[2]) : 0);
    wm_redraw_all();
    out_f(io, "Visual style set to %s (hue %d)\n", style_name(theme_id), theme_hue);
    return 0;
}
static int b_shutdown(const char *cwd, int argc, char **argv, io_t *io) {
    bool r = argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "/r"));
    out_s(io, r ? "Restarting...\n" : "Shutting down...\n");
    wm_defer(shell_power_action, r ? (void *)1 : NULL);
    return 0;
}
static int b_passwd(const char *cwd, int argc, char **argv, io_t *io) {
    (void)cwd;
    if (argc < 2) { out_s(io, "usage: passwd <new password>   (passwd -c clears it)\n"); return 1; }
    if (!strcmp(argv[1], "-c")) { password_set(""); settings_save(); out_s(io, "Password cleared.\n"); return 0; }
    password_set(argv[1]); settings_save(); out_s(io, "Password changed.\n"); return 0;
}
static int b_logoff(const char *cwd, int argc, char **argv, io_t *io) {
    (void)cwd; (void)argc; (void)argv;
    out_s(io, "Logging off...\n");
    wm_defer((wm_deferred_fn)shell_logoff, NULL);
    return 0;
}
static int b_lsblk(const char *cwd, int argc, char **argv, io_t *io) {
    if (!blk_count()) { out_s(io, "No hard disks.\n"); return 0; }
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        char b[128]; blk_describe(d, b, sizeof b);
        out_f(io, "Disk %d  %s\n", i, b);
        part_t p[4];
        if (!mbr_read(d, p)) { out_s(io, "        (no partition table)\n"); continue; }
        for (int k = 0; k < 4; k++)
            if (p[k].type) out_f(io, "        Partition %d  %-12s %8u MB%s\n", k + 1, part_type_name(p[k].type), p[k].sectors / 2048, p[k].boot ? "  active" : "");
    }
    return 0;
}
static int b_lspci(const char *cwd, int argc, char **argv, io_t *io) {
    pci_dev_t *d; int n = pci_list(&d);
    out_s(io, "BUS:DEV.FN  VENDOR:DEVICE  CLASS\n");
    for (int i = 0; i < n; i++) out_f(io, "%02x:%02x.%x     %04x:%04x      %s (%s)\n", d[i].bus, d[i].dev, d[i].fn, d[i].vendor, d[i].device, pci_class_name(d[i].cls, d[i].sub), pci_vendor_name(d[i].vendor));
    return 0;
}
/* devices: every device found, with the driver that runs it */
static int b_devices(const char *cwd, int argc, char **argv, io_t *io) {
    bool problems = argc > 1 && (!strcmp(argv[1], "-p") || !strcmp(argv[1], "--problems"));
    bool ids = argc > 1 && (!strcmp(argv[1], "-i") || !strcmp(argv[1], "--ids"));
    devmgr_scan();
    for (int cls = 0; cls < DC_COUNT; cls++) {
        bool head = false;
        for (int i = 0; i < devmgr_count(); i++) {
            device_t *d = devmgr_get(i);
            if (d->cls != cls) continue;
            if (problems && d->status != DS_NO_DRIVER && d->status != DS_FAILED) continue;
            if (!head) { out_f(io, "\x1b[93m%s\x1b[0m\n", devclass_name(cls)); head = true; }
            const char *st = d->status == DS_WORKING ? "\x1b[92mworking\x1b[0m    " : d->status == DS_BUILTIN ? "built in   " :
                             d->status == DS_NOT_STARTED ? "not in use " : d->status == DS_FAILED ? "\x1b[91mnot started\x1b[0m" : "\x1b[91mno driver\x1b[0m  ";
            out_f(io, "  %s  %-10s %s\n", st, d->drv ? d->drv->name : "-", d->name);
            if (ids || problems) out_f(io, "               %s%s%s\n", d->hwid, d->location[0] ? "  at " : "", d->location);
            if (problems) out_f(io, "               %s\n", d->detail);
        }
    }
    char sum[128]; devmgr_summary(sum, sizeof sum);
    out_f(io, "%s.\n", sum);
    return devmgr_problem_count() ? 1 : 0;
}
/* driverquery: every driver built into the kernel, and how many devices each one runs */
static int b_driverquery(const char *cwd, int argc, char **argv, io_t *io) {
    const driver_t *const *drv; int n = devmgr_drivers(&drv);
    devmgr_scan();
    out_s(io, "DRIVER      BUS      IN USE  DESCRIPTION\n");
    int used = 0;
    for (int cls = 0; cls < DC_COUNT; cls++)
        for (int k = 0; k < n; k++) {
            if (drv[k]->cls != cls) continue;
            int c = 0;
            for (int i = 0; i < devmgr_count(); i++) { device_t *d = devmgr_get(i); if (d->drv == drv[k] && d->status == DS_WORKING) c++; }
            if (c) used++;
            char cnt[12]; if (c) snprintf(cnt, sizeof cnt, "%d", c); else strcpy(cnt, "-");
            out_f(io, "%-11s %-8s %-7s %s\n", drv[k]->name, drv[k]->bus, cnt, drv[k]->desc);
        }
    out_f(io, "%d drivers are built into %s; %d are running a device on this computer.\n", n, OS_NAME, used);
    return 0;
}
static int b_lsusb(const char *cwd, int argc, char **argv, io_t *io) {
    int n = 0;
    devmgr_scan();
    for (int i = 0; i < devmgr_count(); i++) {
        device_t *d = devmgr_get(i);
        if (strncmp(d->hwid, "USB\\", 4)) continue;
        out_f(io, "%s  %-10s %s  (%s)\n", d->hwid + 4, d->drv ? d->drv->name : "no driver", d->name, d->location);
        n++;
    }
    if (!n) out_s(io, "No USB devices are connected.\n");
    return 0;
}
static int b_cpuinfo(const char *cwd, int argc, char **argv, io_t *io) { char v[16], b[52]; cpu_get_info(v, b); out_f(io, "vendor: %s\nmodel:  %s\n", v, b); return 0; }

/* ---------------- for the Rust shell ---------------- */
typedef int (*shcmd_fn)(const char *cwd, int argc, char **argv, io_t *io);
static const struct { const char *name; shcmd_fn fn; } cmds[] = {
    { "uname", b_uname }, { "ver", b_ver }, { "whoami", b_whoami }, { "hostname", b_hostname },
    { "ipconfig", b_ipconfig }, { "ifconfig", b_ipconfig }, { "ping", b_ping }, { "nslookup", b_nslookup },
    { "wget", b_wget }, { "curl", b_wget }, { "sha256sum", b_sha256sum }, { "js", b_js }, { "display", b_display },
    { "play", b_play }, { "volume", b_volume }, { "sndinfo", b_sndinfo }, { "ps", b_ps }, { "tasklist", b_ps },
    { "kill", b_kill }, { "taskkill", b_kill }, { "free", b_free }, { "mem", b_free }, { "sync", b_sync },
    { "uptime", b_uptime }, { "dmesg", b_dmesg }, { "beep", b_beep }, { "mount", b_mount }, { "hdiutil", b_hdiutil },
    { "open", b_open }, { "start", b_open },
#ifdef __x86_64__
    { "lxtrace", b_lxtrace }, { "dpkg", b_pkg }, { "rpm", b_pkg }, { "apt", b_pkg }, { "apt-get", b_pkg },
#endif
    { "guiprof", b_guiprof }, { "theme", b_theme }, { "shutdown", b_shutdown }, { "passwd", b_passwd }, { "logoff", b_logoff },
    { "lspci", b_lspci }, { "devices", b_devices }, { "lsdev", b_devices }, { "driverquery", b_driverquery },
    { "lsusb", b_lsusb }, { "lsblk", b_lsblk }, { "cpuinfo", b_cpuinfo },
};
int sh_c_command(const char *name, const char *cwd, int argc, char **argv, io_t *io) {
    for (unsigned i = 0; i < ARRAY_LEN(cmds); i++) if (!strcmp(cmds[i].name, name)) return cmds[i].fn(cwd, argc, argv, io);
    err_f(io, "%s: command not found\n", name);
    return 127;
}
/* GUI applications start on the window manager's task */
void sh_gui_launch(const char *app, const char *arg) { gui_launch(app, arg); }
int sh_task_pid(void) { task_t *t = task_current(); return t ? t->pid : 0; }
