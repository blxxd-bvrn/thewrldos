/* Signals on THE WRLD OS: handlers on a real rt_sigframe, a blocked signal and
 * sigsuspend, an alternate stack, SIGPIPE, SIGCHLD, alarm() and SA_RESTART.
 * Build with the other samples and run C:/Linux/usr/local/bin/signals. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t counts[32];
static volatile sig_atomic_t on_alt_stack;

/* SIGSTKSZ stopped being a constant in glibc 2.34 (it is sysconf() now), so a
 * fixed size is the portable way to get a static alternate stack. */
#define ALTSTACK_SIZE (32 * 1024)
static unsigned char altstack[ALTSTACK_SIZE];

static void handler(int sig) { counts[sig]++; }
static void handler_alt(int sig) {
    char probe;
    uintptr_t here = (uintptr_t)&probe;
    counts[sig]++;
    /* the handler's own frame tells whether it runs on the alternate stack */
    if (here >= (uintptr_t)altstack && here < (uintptr_t)altstack + sizeof altstack) on_alt_stack = 1;
}
static void handler_segv(int sig, siginfo_t *info, void *ucp) {
    (void)info;
    counts[sig]++;
    ucontext_t *uc = ucp;
    uc->uc_mcontext.gregs[REG_RIP] += 3;        /* the fault is reported at "movb $1,(%rax)" itself: skip its 3 bytes */
}
static void handler_chld(int sig) { counts[sig]++; }

int main(void) {
    struct sigaction sa;
    int ok = 1;

    printf("signals: start (pid %d)\n", (int)getpid());

    /* 1. a plain handler */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL)) { perror("sigaction"); return 1; }
    kill(getpid(), SIGUSR1);
    printf("  kill(SIGUSR1): handler ran %d time(s)\n", (int)counts[SIGUSR1]);
    ok &= counts[SIGUSR1] == 1;

    /* 2. blocked, then released by sigsuspend */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    kill(getpid(), SIGUSR1);
    sigset_t pending;
    sigpending(&pending);
    printf("  blocked SIGUSR1 is %s while a handler waits\n", sigismember(&pending, SIGUSR1) ? "pending" : "missing");
    ok &= sigismember(&pending, SIGUSR1) == 1;
    sigsuspend(&old);
    printf("  sigsuspend: handler ran %d time(s) in total\n", (int)counts[SIGUSR1]);
    ok &= counts[SIGUSR1] == 2;
    sigprocmask(SIG_SETMASK, &old, NULL);

    /* 3. an alternate signal stack */
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    if (sigaltstack(&ss, NULL)) perror("sigaltstack");
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler_alt;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    kill(getpid(), SIGUSR2);
    printf("  alt stack: SIGUSR2 ran %d time(s)%s\n", (int)counts[SIGUSR2], on_alt_stack ? ", inside the handler" : "");
    ok &= counts[SIGUSR2] == 1;

    /* 4. a fault reaches a handler, which resumes after the instruction */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = NULL;
    sa.sa_sigaction = handler_segv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    __asm__ volatile("xor %%rax, %%rax\n\tmovb $1, (%%rax)\n\t" ::: "rax", "memory");
    printf("  SIGSEGV: handler ran %d time(s), execution continued\n", (int)counts[SIGSEGV]);
    ok &= counts[SIGSEGV] == 1;

    /* 5. writes to a pipe with no reader raise SIGPIPE */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
    int fds[2];
    if (!pipe(fds)) {
        close(fds[0]);
        errno = 0;
        ssize_t r = write(fds[1], "x", 1);
        printf("  broken pipe: write returned %ld (errno %d), SIGPIPE %d time(s)\n",
               (long)r, errno, (int)counts[SIGPIPE]);
        ok &= r == -1 && errno == EPIPE && counts[SIGPIPE] == 1;
        close(fds[1]);
    }

    /* 6. SIGCHLD, and wait4 collects the status */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler_chld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    pid_t c = fork();
    if (c == 0) { usleep(200000); _exit(7); }
    int st = 0;
    pid_t w = waitpid(c, &st, 0);
    printf("  SIGCHLD: wait4 got %d, exit status %d, handler ran %d time(s)\n",
           (int)w, WIFEXITED(st) ? WEXITSTATUS(st) : -1, (int)counts[SIGCHLD]);
    ok &= w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7;

    /* 7. alarm() interrupts pause() */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);
    int r = pause();
    printf("  alarm(1): pause returned %d, SIGALRM ran %d time(s)\n", r, (int)counts[SIGALRM]);
    ok &= counts[SIGALRM] == 1;

    /* 8. SA_RESTART: the read() waits again after the handler */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    if (!pipe(fds)) {
        pid_t k = fork();
        if (k == 0) { usleep(300000); kill(getppid(), SIGUSR1); _exit(0); }
        close(fds[1]);
        char b[8];
        ssize_t n = read(fds[0], b, sizeof b);      /* restarted; the writer is gone */
        printf("  SA_RESTART: read returned %ld after the handler\n", (long)n);
        ok &= n == 0;
        close(fds[0]);
        waitpid(k, NULL, 0);
    }

    printf("signals: %s\n", ok ? "all checks passed" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
