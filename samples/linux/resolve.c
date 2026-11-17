/* A Linux test program for THE WRLD OS: name lookups the way GLib, libsoup and
 * WebKit make them (getaddrinfo with AF_UNSPEC, so glibc asks for A and AAAA
 * records at once, and with AI_ADDRCONFIG, which makes glibc ask the kernel over
 * netlink which address families are configured), plus an IPv4-only lookup.
 *   C:/Linux/usr/local/bin/resolve [name...]      (default: pypi.org example.com) */
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int lookup(const char *name, int family, int flags) {
    struct addrinfo hints, *ai, *a;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    int g = getaddrinfo(name, "443", &hints, &ai);
    printf("%s (%s%s): ", name, family == AF_INET ? "IPv4" : "any", flags & AI_ADDRCONFIG ? ", AI_ADDRCONFIG as GLib asks" : "");
    if (g) { printf("%s\n", gai_strerror(g)); return 1; }
    for (a = ai; a; a = a->ai_next) {
        char ip[64] = "?";
        if (a->ai_family == AF_INET) inet_ntop(AF_INET, &((struct sockaddr_in *)a->ai_addr)->sin_addr, ip, sizeof ip);
        else if (a->ai_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, ip, sizeof ip);
        printf("%s ", ip);
    }
    printf("\n");
    freeaddrinfo(ai);
    return 0;
}
int main(int argc, char **argv) {
    const char *def[] = { "pypi.org", "example.com" };
    int n = argc > 1 ? argc - 1 : 2, bad = 0;
    for (int i = 0; i < n; i++) {
        const char *name = argc > 1 ? argv[i + 1] : def[i];
        bad += lookup(name, AF_UNSPEC, 0);
        bad += lookup(name, AF_INET, 0);
        bad += lookup(name, AF_UNSPEC, AI_ADDRCONFIG);
    }
    return bad != 0;
}
