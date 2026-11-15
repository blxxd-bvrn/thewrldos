/* fakewin: run wrldcomp (and its Wayland clients) on a Linux build machine,
 * without THE WRLD OS: an LD_PRELOAD library that stands in for
 * /dev/wrld/window. Opening it gives a pipe (so poll/epoll work), mmap gives
 * memory, the ioctls are answered here, and a thread replays scripted input:
 *
 *   WRLD_FAKE_EVENTS="2000 move 100 80; 2500 down 100 80 1; 2600 up 100 80 1; 3000 key 65 65; 4000 dump /tmp/f.ppm"
 *   LD_PRELOAD=./fakewin.so wrldcomp -v ./gtkhello
 *
 * Times are milliseconds after the window is created; "dump" writes the window
 * as a PPM picture. Build: gcc -shared -fPIC -o fakewin.so tools/fakewin.c -ldl -lpthread */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../samples/linux-gnome/wrldwin.h"

static int rfd = -1, wfd = -1, W, H;
static uint32_t *px;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void push(uint32_t type, int x, int y, uint32_t code, uint32_t ch, uint32_t mods) {
    struct wrldwin_event e = { type, x, y, code, ch, mods };
    if (write(wfd, &e, sizeof e) < 0) perror("fakewin: write");
}
static void dump(const char *path) {
    pthread_mutex_lock(&lock);
    FILE *f = fopen(path, "wb");
    if (f && px) {
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; i++) { uint32_t c = px[i]; if (!(c >> 24)) c = 0x404040; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); }
        fprintf(stderr, "fakewin: wrote %s (%dx%d)\n", path, W, H);
    }
    if (f) fclose(f);
    pthread_mutex_unlock(&lock);
}
static void *script(void *arg) {
    (void)arg;
    const char *s = getenv("WRLD_FAKE_EVENTS");
    if (!s) return NULL;
    char *copy = strdup(s), *save = NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (char *step = strtok_r(copy, ";", &save); step; step = strtok_r(NULL, ";", &save)) {
        long at; char what[16], path[200] = ""; int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(step, " %ld %15s", &at, what) < 2) continue;
        for (;;) {
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            long ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
            if (ms >= at) break;
            usleep(5000);
        }
        const char *rest = strstr(step, what) + strlen(what);
        if (!strcmp(what, "dump")) { sscanf(rest, " %199s", path); dump(path); continue; }
        sscanf(rest, " %d %d %d %d", &a, &b, &c, &d);
        fprintf(stderr, "fakewin: %s %d %d %d\n", what, a, b, c);
        if (!strcmp(what, "move")) push(WRLDWIN_EV_MOTION, a, b, 0, 0, 0);
        else if (!strcmp(what, "down")) push(WRLDWIN_EV_BUTTON_DOWN, a, b, c ? c : 1, 0, 0);
        else if (!strcmp(what, "up")) push(WRLDWIN_EV_BUTTON_UP, a, b, c ? c : 1, 0, 0);
        else if (!strcmp(what, "key")) { push(WRLDWIN_EV_KEY_DOWN, 0, 0, a, b, c); push(WRLDWIN_EV_KEY_UP, 0, 0, a, b, c); }
        else if (!strcmp(what, "wheel")) push(WRLDWIN_EV_WHEEL, a, b, c, 0, 0);
        else if (!strcmp(what, "resize")) push(WRLDWIN_EV_RESIZE, a, b, 0, 0, 0);
        else if (!strcmp(what, "close")) push(WRLDWIN_EV_CLOSE, 0, 0, 0, 0, 0);
    }
    free(copy);
    return NULL;
}

int open(const char *path, int flags, ...) {
    static int (*real)(const char *, int, ...);
    if (!real) real = dlsym(RTLD_NEXT, "open");
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (path && !strcmp(path, "/dev/wrld/window")) {
        int p[2];
        if (pipe2(p, O_CLOEXEC)) return -1;
        rfd = p[0]; wfd = p[1];
        return rfd;
    }
    return real(path, flags, mode);
}
int ioctl(int fd, unsigned long req, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (fd != rfd || rfd < 0) return real(fd, req, arg);
    switch (req) {
    case WRLDWIN_CREATE: {
        struct wrldwin_create *c = arg; W = c->width; H = c->height;
        fprintf(stderr, "fakewin: window %dx%d flags %u '%s'\n", W, H, c->flags, c->title);
        pthread_t t; pthread_create(&t, NULL, script, NULL); pthread_detach(t);
        return 0;
    }
    case WRLDWIN_RESIZE: case WRLDWIN_SETSIZE: { struct wrldwin_size *s = arg; pthread_mutex_lock(&lock); W = s->width; H = s->height; pthread_mutex_unlock(&lock); fprintf(stderr, "fakewin: size %dx%d\n", W, H); return 0; }
    case WRLDWIN_ACTION: fprintf(stderr, "fakewin: action %lu\n", (unsigned long)(uintptr_t)arg); return 0;
    default: return 0;
    }
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real)(void *, size_t, int, int, int, off_t);
    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    if (fd >= 0 && fd == rfd) {
        void *p = real(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        pthread_mutex_lock(&lock); px = p; pthread_mutex_unlock(&lock);
        return p;
    }
    return real(addr, len, prot, flags, fd, off);
}
