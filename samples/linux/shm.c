/* Shared memory between processes on THE WRLD OS: what WebKit's processes use to
 * hand each other rendered tiles and IPC buffers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;
static void check(const char *what, int ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); fflush(stdout); if (ok) passed++; else failed++; }
static int wait_ok(pid_t c) { int st = 0; return waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == 0; }

static void send_fd(int sock, int fd) {
    char b = 'f'; struct iovec io = { &b, 1 };
    char ctl[CMSG_SPACE(sizeof(int))]; memset(ctl, 0, sizeof ctl);
    struct msghdr m = { 0 }; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    sendmsg(sock, &m, 0);
}
static int recv_fd(int sock) {
    char b; struct iovec io = { &b, 1 };
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { 0 }; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) <= 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m); int fd = -1;
    if (c && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

int main(void) {
    printf("THE WRLD OS - shared memory between processes\n");
    const size_t SZ = 1 << 20;

    /* 1. memfd shared with a child after fork */
    int fd = memfd_create("tiles", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check("memfd_create", fd >= 0);
    check("ftruncate to 1 MB", ftruncate(fd, SZ) == 0);
    unsigned char *a = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("mmap MAP_SHARED", a != MAP_FAILED);
    pid_t c = fork();
    if (c == 0) { for (size_t i = 0; i < SZ; i += 4096) a[i] = (unsigned char)(i / 4096 + 1); _exit(0); }
    int ok = wait_ok(c);
    for (size_t i = 0; i < SZ && ok; i += 4096) if (a[i] != (unsigned char)(i / 4096 + 1)) ok = 0;
    check("a child's writes to a memfd mapping reach the parent (256 pages)", ok);

    /* 2. two mappings of the same object alias each other */
    unsigned char *b = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    b[12345] = 0x5A;
    check("two mappings of one memfd are the same memory", a[12345] == 0x5A && b != a);

    /* 3. the descriptor and the mapping see each other */
    char buf[16] = { 0 };
    int r1 = pread(fd, buf, 5, 8192) == 5 && buf[0] == 3;     /* written by the child as page 2 + 1 */
    int r2 = pwrite(fd, "hello", 5, 4096 * 7 + 100) == 5;
    check("pread/pwrite and the mapping agree", r1 && r2 && !memcmp(a + 4096 * 7 + 100, "hello", 5));
    struct stat st; fstat(fd, &st);
    check("fstat reports the size", st.st_size == (off_t)SZ);

    /* 4. seals */
    check("F_ADD_SEALS", fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0 && (fcntl(fd, F_GET_SEALS) & F_SEAL_SHRINK));
    errno = 0;
    check("a sealed memfd cannot shrink", ftruncate(fd, 4096) == -1 && errno == EPERM);

    /* 5. MAP_PRIVATE stays private */
    unsigned char *p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    p[100] = 0xEE;
    check("MAP_PRIVATE of a memfd copies on write", p[100] == 0xEE && a[100] != 0xEE && p[4096] == a[4096]);
    munmap(p, SZ);

    /* 6. MAP_SHARED | MAP_ANONYMOUS across fork */
    volatile int *counter = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *counter = 40;
    c = fork();
    if (c == 0) { *counter += 2; _exit(0); }
    check("shared anonymous memory across fork", wait_ok(c) && *counter == 42);

    /* 7. shm_open by name in two processes */
    shm_unlink("/wrld-test");
    int s = shm_open("/wrld-test", O_RDWR | O_CREAT | O_EXCL, 0600);
    check("shm_open creates /dev/shm/wrld-test", s >= 0 && ftruncate(s, 65536) == 0);
    unsigned char *sm = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, s, 0);
    c = fork();
    if (c == 0) {
        int t = shm_open("/wrld-test", O_RDWR, 0);
        unsigned char *m = t >= 0 ? mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, t, 0) : MAP_FAILED;
        if (m == MAP_FAILED) _exit(1);
        strcpy((char *)m + 60000, "from the child");
        _exit(0);
    }
    check("a second process opens it by name and writes", wait_ok(c) && !strcmp((char *)sm + 60000, "from the child"));
    check("shm_unlink removes the name", shm_unlink("/wrld-test") == 0 && shm_open("/wrld-test", O_RDWR, 0) == -1 && errno == ENOENT);
    check("the mapping outlives the name", !strcmp((char *)sm + 60000, "from the child"));

    /* 8. a memfd sent over a Unix socket (how WebKit hands buffers between processes) */
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    c = fork();
    if (c == 0) {
        close(sv[0]);
        int got = recv_fd(sv[1]);
        unsigned char *m = got >= 0 ? mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, got, 0) : MAP_FAILED;
        if (m == MAP_FAILED) _exit(1);
        memset(m + 500000, 0x77, 1000);
        _exit(0);
    }
    close(sv[1]);
    int fd2 = memfd_create("passed", 0);
    if (ftruncate(fd2, SZ)) { check("ftruncate the memfd to pass", 0); return 1; }
    unsigned char *q = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    send_fd(sv[0], fd2);
    ok = wait_ok(c);
    for (int i = 0; i < 1000 && ok; i++) if (q[500000 + i] != 0x77) ok = 0;
    check("a memfd passed with SCM_RIGHTS is shared with the receiver", ok);

    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
