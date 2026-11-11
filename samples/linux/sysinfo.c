/* Reads the built-in /proc and /sys files, checks that a rlimit is enforced
 * and that prctl answers honestly. Build with musl-gcc -static-pie or any
 * distribution's gcc; run it from the Command Prompt as
 * C:/Linux/usr/local/bin/sysinfo. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/prctl.h>

static void show(const char *path, const char *tag, int maxlines)
{
    char buf[2048];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("%s: open failed (%d)\n", tag, errno); return; }
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) { printf("%s: read failed\n", tag); return; }
    buf[n] = 0;
    printf("--- %s\n", tag);
    for (int i = 0; i < n && maxlines > 0; i++) {
        putchar(buf[i]);
        if (buf[i] == '\n') maxlines--;
    }
}

static void pick(const char *path, const char *tag, const char *const *keys, int nk)
{
    char buf[4096];
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("%s: open failed (%d)\n", tag, errno); return; }
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) return;
    buf[n] = 0;
    printf("--- %s\n", tag);
    for (int k = 0; k < nk; k++) {
        char *p = strstr(buf, keys[k]);
        if (!p) { printf("%s missing\n", keys[k]); continue; }
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        printf("%s\n", p);
        if (e) *e = '\n';
    }
}

int main(void)
{
    static const char *const st[] = { "Name:", "Pid:", "Threads:", "VmRSS:", "NoNewPrivs:" };
    static const char *const ci[] = { "vendor_id", "model name" };
    printf("sysinfo: pid=%d\n", (int)getpid());
    show("/proc/self/stat", "stat", 1);
    pick("/proc/self/status", "status", st, 5);
    show("/proc/self/maps", "maps", 2);
    show("/proc/meminfo", "meminfo", 2);
    pick("/proc/cpuinfo", "cpuinfo", ci, 2);
    show("/proc/uptime", "uptime", 1);
    show("/proc/version", "version", 1);
    show("/sys/devices/system/cpu/online", "sys cpu online", 1);
    show("/sys/kernel/osrelease", "sys osrelease", 1);

    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        printf("proc:");
        while ((e = readdir(d))) if (e->d_name[0] != '.') printf(" %s", e->d_name);
        printf("\n");
        closedir(d);
    } else printf("opendir(/proc) failed (%d)\n", errno);

    d = opendir("/proc/self/fd");
    if (d) {
        struct dirent *e;
        printf("fds:");
        while ((e = readdir(d))) if (e->d_name[0] != '.') printf(" %s", e->d_name);
        printf("\n");
        closedir(d);
    } else printf("opendir(/proc/self/fd) failed (%d)\n", errno);

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
        printf("nofile: cur=%llu max=%llu\n", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
    struct rlimit nl = { 3, 3 };
    printf("setrlimit(nofile,3): %d\n", setrlimit(RLIMIT_NOFILE, &nl));
    getrlimit(RLIMIT_NOFILE, &rl);
    int fd = open("/dev/null", O_RDONLY);
    printf("nofile now %llu, open above it: %d (%s)\n", (unsigned long long)rl.rlim_cur, fd, fd < 0 ? "refused" : "UNEXPECTED");
    nl.rlim_cur = nl.rlim_max = 256;
    setrlimit(RLIMIT_NOFILE, &nl);

    printf("NNP set: %d, get: %d\n", prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
    printf("PR_SET_SECCOMP: %d\n", prctl(PR_SET_SECCOMP, 0, 0, 0, 0));
    printf("PR_SET_NAME: %d\n", prctl(PR_SET_NAME, "sysinfo", 0, 0, 0));
    return 0;
}
