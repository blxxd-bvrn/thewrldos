/* A Linux test program for THE WRLD OS: C library start-up, stdio, files,
 * the heap, time and the environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    struct utsname u; uname(&u);
    printf("Hello from Linux userland on THE WRLD OS! (%s %s, pid %d)\n", u.sysname, u.machine, getpid());
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    printf("  HOME=%s\n", getenv("HOME"));
    char cwd[256]; printf("  cwd: %s\n", getcwd(cwd, sizeof cwd) ? cwd : "?");
    size_t total = 0;
    for (int i = 0; i < 64; i++) { char *p = malloc(64 * 1024); memset(p, i, 64 * 1024); total += 64 * 1024; if (i % 2) free(p); }
    char *big = malloc(8 << 20); memset(big, 1, 8 << 20); free(big);
    printf("  malloc: %zu KB in small blocks + 8 MB in one block\n", total / 1024);
    FILE *f = fopen("/tmp/hello.txt", "w");
    if (f) { fprintf(f, "written by pid %d\n", getpid()); fclose(f); }
    char line[128] = "";
    f = fopen("/tmp/hello.txt", "r");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    printf("  file round trip: %s", line[0] ? line : "FAILED\n");
    time_t now = time(NULL);
    printf("  time: %s", ctime(&now));
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec d = { 0, 50 * 1000 * 1000 }; nanosleep(&d, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("  slept %ld ms\n", (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000));
    return 7;
}
