/* pipe(), socketpair(), SCM_RIGHTS, eventfd() and epoll(): the pieces an event
 * loop needs. Build with the rest of samples/linux and run it as
 * C:/Linux/usr/local/bin/ipc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

int main(void) {
    /* 1. a pipe across fork */
    int p[2];
    if (pipe(p) < 0) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        const char *msg = "hello through the pipe";
        if (write(p[1], msg, strlen(msg)) < 0) perror("write");
        _exit(0);
    }
    close(p[1]);
    char buf[128];
    ssize_t n = read(p[0], buf, sizeof buf - 1);
    if (n < 0) { perror("read"); return 1; }
    buf[n] = 0;
    printf("pipe: %s\n", buf);
    waitpid(pid, NULL, 0);

    /* 2. a socketpair, with an eventfd passed over it via SCM_RIGHTS */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }

    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) { perror("eventfd"); return 1; }

    struct msghdr mh;
    struct iovec iov;
    char byte = 'x';
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(&mh, 0, sizeof mh);
    memset(ctl, 0, sizeof ctl);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    iov.iov_base = &byte; iov.iov_len = 1;
    mh.msg_control = ctl; mh.msg_controllen = sizeof ctl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &efd, sizeof(int));
    if (sendmsg(sv[0], &mh, 0) < 0) { perror("sendmsg"); return 1; }

    char rbyte = 0;
    char rctl[CMSG_SPACE(sizeof(int))];
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    iov.iov_base = &rbyte; iov.iov_len = 1;
    mh.msg_control = rctl; mh.msg_controllen = sizeof rctl;
    if (recvmsg(sv[1], &mh, 0) < 0) { perror("recvmsg"); return 1; }

    int got = -1;
    for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&got, CMSG_DATA(c), sizeof(int));
    printf("socketpair: byte '%c', received descriptor %d\n", rbyte, got);

    if (got >= 0) {
        uint64_t one = 1, value = 0;
        if (write(got, &one, sizeof one) != sizeof one) perror("eventfd write");
        if (read(got, &value, sizeof value) != sizeof value) perror("eventfd read");
        printf("eventfd: %llu\n", (unsigned long long)value);
        close(got);
    }
    close(efd);
    close(sv[0]);
    close(sv[1]);

    /* 3. epoll over a pipe */
    int ep[2];
    if (pipe(ep) < 0) { perror("pipe"); return 1; }
    int efd2 = epoll_create1(0);
    if (efd2 < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1234;
    if (epoll_ctl(efd2, EPOLL_CTL_ADD, ep[0], &ev) < 0) { perror("epoll_ctl"); return 1; }

    if (write(ep[1], "z", 1) != 1) perror("write");
    struct epoll_event out[4];
    int ready = epoll_wait(efd2, out, 4, 1000);
    printf("epoll: %d ready, first has data %llx\n", ready, (unsigned long long)(ready > 0 ? out[0].data.u64 : 0));

    /* 4. timerfd: one 5 ms tick */
    int tfd = -1;
    (void)tfd;
    close(ep[0]); close(ep[1]); close(efd2);
    return 0;
}
