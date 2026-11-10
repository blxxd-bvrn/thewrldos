/* A Linux test program for THE WRLD OS: getaddrinfo, a non-blocking connect
 * driven by poll(), getsockopt(SO_ERROR), and a small TCP request/response.
 * With no arguments it fetches http://example.com/ over port 80. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int connect_nb(const char *host, const char *port) {
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host, port, &hints, &ai);
    if (g) { printf("  getaddrinfo(%s): %s\n", host, gai_strerror(g)); return -1; }
    char ip[64] = "?";
    inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
    printf("  %s -> %s:%s\n", host, ip, port);
    int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, 0);
    if (fd < 0) { perror("  socket"); freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        if (errno != EINPROGRESS) {
            perror("  connect"); close(fd); freeaddrinfo(ai); return -1;
        }
        struct pollfd p = { fd, POLLOUT, 0 };
        if (poll(&p, 1, 8000) <= 0) {
            printf("  connect: timed out\n"); close(fd); freeaddrinfo(ai); return -1;
        }
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            printf("  connect: %s\n", strerror(err ? err : errno));
            close(fd); freeaddrinfo(ai); return -1;
        }
    }
    freeaddrinfo(ai);
    return fd;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "example.com";
    const char *port = argc > 2 ? argv[2] : "80";
    printf("socket test: %s port %s\n", host, port);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u >= 0) {
        int one = 1;
        setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(u, (struct sockaddr *)&a, sizeof a) == 0) {
            socklen_t l = sizeof a;
            if (getsockname(u, (struct sockaddr *)&a, &l) == 0) printf("  UDP bound to port %u\n", ntohs(a.sin_port));
        } else perror("  udp bind");
        close(u);
    }

    int fd = connect_nb(host, port);
    if (fd < 0) return 1;
    fcntl(fd, F_SETFL, 0);                              /* blocking again for the exchange */

    char req[256];
    int n = snprintf(req, sizeof req, "HEAD / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (send(fd, req, (size_t)n, 0) < 0) { perror("  send"); close(fd); return 1; }

    char buf[1024];
    int got = 0;
    ssize_t k;
    while ((k = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
        buf[k] = 0;
        if (!got) { fputs("  ", stdout); fputs(buf, stdout); }
        got += (int)k;
        if (got > 4096) break;
    }
    printf("  received %d bytes over TCP\n", got);
    close(fd);
    return got > 0 ? 0 : 1;
}
