/* wsget: a Windows sockets sample for THE WRLD OS. Fetches a web page over HTTP and
 * prints the reply's first lines, using one of the three ways Windows programs use
 * sockets:
 *   wsget HOST [PORT] [PATH]            blocking connect, send and recv
 *   wsget -s HOST [PORT] [PATH]         non-blocking, waiting with select()
 *   wsget -e HOST [PORT] [PATH]         WSAEventSelect + WaitForSingleObject
 * Example: wsget example.com 80 /  */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what) { printf("wsget: %s failed (error %d)\n", what, WSAGetLastError()); return 1; }

int main(int argc, char **argv) {
    int mode = 0, a = 1;
    if (argc > 1 && !strcmp(argv[1], "-s")) { mode = 1; a++; }
    else if (argc > 1 && !strcmp(argv[1], "-e")) { mode = 2; a++; }
    if (argc <= a) { printf("usage: wsget [-s|-e] HOST [PORT] [PATH]\n"); return 2; }
    const char *host = argv[a], *port = argc > a + 1 ? argv[a + 1] : "80", *path = argc > a + 2 ? argv[a + 2] : "/";

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) return fail("WSAStartup");
    printf("Winsock %d.%d: %s\n", LOBYTE(wd.wVersion), HIBYTE(wd.wVersion), wd.szDescription);

    struct addrinfo hints = { 0 }, *ai;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(host, port, &hints, &ai);
    if (r) { printf("wsget: getaddrinfo(%s) failed (error %d)\n", host, r); return 1; }
    struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
    char ip[16]; inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
    printf("%s is %s, port %u (%s)\n", host, ip, ntohs(sa->sin_port), mode == 0 ? "blocking" : mode == 1 ? "select" : "WSAEventSelect");

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return fail("socket");
    WSAEVENT ev = NULL;
    if (mode == 1) { u_long nb = 1; ioctlsocket(s, FIONBIO, &nb); }
    if (mode == 2) { ev = WSACreateEvent(); WSAEventSelect(s, ev, FD_CONNECT | FD_READ | FD_CLOSE); }

    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR) {
        if (mode == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return fail("connect");
        if (mode == 1) {
            fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
            struct timeval tv = { 10, 0 };
            if (select(0, NULL, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { int err = 0, l = sizeof err; getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &l); printf("wsget: connect failed (error %d)\n", err); return 1; }
        } else {
            WSANETWORKEVENTS ne;
            if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("wsget: no connection within 10 s\n"); return 1; }
            WSAEnumNetworkEvents(s, ev, &ne);
            if (!(ne.lNetworkEvents & FD_CONNECT) || ne.iErrorCode[FD_CONNECT_BIT]) { printf("wsget: connect failed (error %d)\n", ne.iErrorCode[FD_CONNECT_BIT]); return 1; }
        }
    }
    printf("connected\n");

    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: wsget (THE WRLD OS)\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, n, 0) != n) return fail("send");

    char buf[4096]; int total = 0, lines = 0, printed = 0;
    for (;;) {
        if (mode == 1) {
            fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd); struct timeval tv = { 10, 0 };
            if (select(0, &rd, NULL, NULL, &tv) <= 0) { printf("wsget: timed out\n"); break; }
        } else if (mode == 2) {
            WSANETWORKEVENTS ne;
            if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) { printf("wsget: timed out\n"); break; }
            WSAEnumNetworkEvents(s, ev, &ne);
        }
        int k = recv(s, buf, sizeof buf - 1, 0);
        if (k == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (k <= 0) break;
        buf[k] = 0; total += k;
        for (char *p = buf; *p && lines < 12; p++) { if (!printed) printed = 1; putchar(*p == '\r' ? ' ' : *p); if (*p == '\n') lines++; }
    }
    printf("\n%d bytes received\n", total);
    closesocket(s);
    freeaddrinfo(ai);
    if (ev) WSACloseEvent(ev);
    WSACleanup();
    return total > 0 ? 0 : 1;
}
