/* ws2_32.dll / wsock32.dll: Windows sockets on the kernel's TCP/IP stack (src/net).
 * IPv4 TCP (connect, listen/accept) and UDP; blocking and non-blocking sockets;
 * select(); the two event models Windows programs use, WSAEventSelect (an event object
 * is signalled, WSAEnumNetworkEvents says why) and WSAAsyncSelect (a window message
 * arrives); getaddrinfo/gethostbyname through the stack's DNS resolver. A kernel task
 * polls the sockets that have an event model attached, every 10 ms.
 * Not done: IPv6 (AF_INET6 is refused), overlapped I/O, raw sockets, OOB data. */
#include "w32.h"
#include "net.h"

#define P w32_cur()
extern w32_proc_t *pe_find(int pid);

enum { WSAEINTR = 10004, WSAEBADF = 10009, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035,
       WSAEINPROGRESS = 10036, WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEMSGSIZE = 10040, WSAEPROTONOSUPPORT = 10043,
       WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048, WSAENETDOWN = 10050, WSAECONNABORTED = 10053,
       WSAECONNRESET = 10054, WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061,
       WSANOTINITIALISED = 10093, WSAHOST_NOT_FOUND = 11001, WSATRY_AGAIN = 11002, WSANO_DATA = 11004 };
enum { FD_READ = 1, FD_WRITE = 2, FD_OOB = 4, FD_ACCEPT = 8, FD_CONNECT = 16, FD_CLOSE = 32 };
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET ((uintptr_t)~0)
#define AF_INET 2
#define AF_INET6 23
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_BASE 0x4000u                   /* socket values: SOCK_BASE + 4 * slot (apart from kernel32's handles) */
#define NSOCK 64
#define UDPQ 16

typedef struct { uint32_t ip; uint16_t port; uint16_t len; uint8_t data[1472]; } dgram_t;
typedef struct {
    bool used; int pid; int type;
    tcb_t *t;
    bool nonblock, connecting, was_connected, listening, shut, closed_seen;
    int err;                                 /* pending error (SO_ERROR) */
    uint32_t lip, rip; uint16_t lport, rport;
    /* WSAEventSelect */
    uintptr_t ev; long evmask, ev_pending; int ev_err[10];
    /* WSAAsyncSelect */
    HWND aw; UINT amsg; long amask, asent;
    /* UDP */
    bool udp_bound; dgram_t *q; int qh, qt;
} wsock_t;
static wsock_t socks[NSOCK];
static bool poller_running;

static void wsa_err(int e) { w32_proc_t *p = P; if (p) p->last_error = e; }
static wsock_t *sget(uintptr_t s) {
    if (s < SOCK_BASE || ((s - SOCK_BASE) & 3)) return NULL;
    uint32_t i = (s - SOCK_BASE) / 4;
    w32_proc_t *p = P;
    return i < NSOCK && socks[i].used && p && socks[i].pid == p->pid ? &socks[i] : NULL;
}
static uintptr_t sid(wsock_t *k) { return SOCK_BASE + 4 * (uintptr_t)(k - socks); }

/* sockaddr_in: family, port (network order), address (network order), 8 zero bytes */
typedef struct { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } sockaddr_in_t;
static bool read_sa(const void *sa, int len, uint32_t *ip, uint16_t *port) {
    if (!sa || len < (int)sizeof(sockaddr_in_t)) return false;
    const sockaddr_in_t *a = sa;
    if (a->family != AF_INET) return false;
    *ip = ntohl(a->addr); *port = ntohs(a->port);
    return true;
}
static void write_sa(void *sa, int *len, uint32_t ip, uint16_t port) {
    if (!sa || !len) return;
    sockaddr_in_t a = { AF_INET, htons(port), htonl(ip), { 0 } };
    memcpy(sa, &a, MIN((size_t)*len, sizeof a)); *len = sizeof a;
}

/* ---------------------------------------------------------------- readiness */
static bool tcp_closed(wsock_t *k) { return k->t && k->was_connected && (tcp_hup(k->t) || tcp_status(k->t) < 0) && !tcp_recv_ready(k->t); }
static bool can_read(wsock_t *k) {
    if (k->type == SOCK_DGRAM) return k->qh != k->qt;
    if (k->listening) return tcp_accept_ready(k->lport);
    return k->t && (tcp_recv_ready(k->t) || tcp_closed(k));
}
static int connect_state(wsock_t *k) {          /* 1 connected, 0 in progress, -1 failed */
    if (!k->t) return -1;
    int st = tcp_status(k->t);
    if (st == 1) { k->connecting = false; k->was_connected = true; return 1; }
    if (st < 0) { if (k->connecting) { int e = tcp_error(k->t); k->err = e == 1 ? WSAECONNREFUSED : e == 3 ? WSAETIMEDOUT : WSAECONNRESET; } k->connecting = false; return -1; }
    return k->was_connected ? 1 : 0;
}
static bool can_write(wsock_t *k) {
    if (k->type == SOCK_DGRAM) return true;
    return k->t && connect_state(k) == 1 && tcp_send_ready(k->t);
}

/* the event models: what has happened since the program last looked */
static long ready_events(wsock_t *k, int *errs) {
    long ev = 0;
    if (k->type == SOCK_DGRAM) { if (k->qh != k->qt) ev |= FD_READ; ev |= FD_WRITE; return ev; }
    if (k->listening) { if (tcp_accept_ready(k->lport)) ev |= FD_ACCEPT; return ev; }
    if (!k->t) return 0;
    bool was = k->was_connected, pending = k->connecting;
    int cs = connect_state(k);
    if (pending && cs != 0) { ev |= FD_CONNECT; errs[4] = cs < 0 ? k->err : 0; }
    if (cs == 1) {
        if (!was || pending) ev |= FD_WRITE;
        if (tcp_recv_ready(k->t)) ev |= FD_READ;
        if (tcp_closed(k) && !k->closed_seen) { ev |= FD_CLOSE; errs[5] = tcp_error(k->t) == 2 ? WSAECONNRESET : 0; }
    }
    return ev;
}
static void poll_socket(wsock_t *k, w32_proc_t *p) {
    int errs[10] = { 0 };
    if (k->ev && k->evmask) {
        long ev = ready_events(k, errs) & k->evmask;
        long fresh = ev & ~k->ev_pending;
        if (fresh) {
            for (int b = 0; b < 10; b++) if (fresh & (1 << b)) k->ev_err[b] = errs[b];
            k->ev_pending |= fresh;
            if (fresh & FD_CLOSE) k->closed_seen = true;
            w32_event_signal(p, k->ev);
        }
    }
    if (k->aw && k->amask) {
        long ev = ready_events(k, errs) & k->amask & ~k->asent;
        for (int b = 0; b < 10; b++) if (ev & (1 << b)) {
            w32_post(p, k->aw, k->amsg, sid(k), (LPARAM)((uint32_t)(1 << b) | ((uint32_t)errs[b] << 16)));
            k->asent |= 1 << b;
            if ((1 << b) == FD_CLOSE) k->closed_seen = true;
        }
    }
}
static void sock_free(wsock_t *k) {
    if (k->t) tcp_close(k->t);
    if (k->listening) tcp_unlisten(k->lport);
    if (k->udp_bound) udp_unbind(k->lport);
    if (k->q) kfree(k->q);
    memset(k, 0, sizeof *k);
}
/* the kernel task behind the event models; it also closes the sockets of processes that ended */
static void poller(void *arg) {
    for (;;) {
        bool any = false;
        for (int i = 0; i < NSOCK; i++) {
            wsock_t *k = &socks[i];
            if (!k->used) continue;
            any = true;
            w32_proc_t *p = pe_find(k->pid);
            if (!p) { sock_free(k); continue; }
            if ((k->ev && k->evmask) || (k->aw && k->amask)) poll_socket(k, p);
        }
        if (!any) { poller_running = false; return; }
        task_sleep(10);
    }
}
static void start_poller(void) { if (!poller_running) { poller_running = true; task_create("winsock", poller, NULL, 16 * 1024); } }

/* ---------------------------------------------------------------- startup */
static int WINAPI WSAStartup(WORD ver, void *data) {
    if (data) {
        uint8_t *d = data; memset(d, 0, sizeof(void *) == 8 ? 408 : 400);
        WORD v = 0x0202; memcpy(d, &v, 2); memcpy(d + 2, &v, 2);
        char *desc = sizeof(void *) == 8 ? (char *)d + 16 : (char *)d + 4;
        strcpy(desc, "WRLD Sockets 2.2");
        WORD maxs = 64; memcpy(sizeof(void *) == 8 ? d + 4 : d + 390, &maxs, 2);
    }
    return 0;
}
static int WINAPI WSACleanup(void) { return 0; }
static int WINAPI WSAGetLastError(void) { w32_proc_t *p = P; return p ? (int)p->last_error : 0; }
static void WINAPI WSASetLastError(int e) { wsa_err(e); }

/* ---------------------------------------------------------------- sockets */
static uintptr_t WINAPI socket_(int af, int type, int proto) {
    if (af != AF_INET && af != 0) { wsa_err(WSAEAFNOSUPPORT); return INVALID_SOCKET; }
    type &= 0xFF;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) { wsa_err(WSAEPROTONOSUPPORT); return INVALID_SOCKET; }
    for (int i = 0; i < NSOCK; i++) if (!socks[i].used) {
        wsock_t *k = &socks[i];
        memset(k, 0, sizeof *k);
        k->used = true; k->pid = P->pid; k->type = type;
        start_poller();
        return sid(k);
    }
    wsa_err(WSAEMFILE); return INVALID_SOCKET;
}
static uintptr_t WINAPI WSASocketA(int af, int type, int proto, void *info, DWORD g, DWORD fl) { return socket_(af, type, proto); }
static int WINAPI closesocket(uintptr_t s) { wsock_t *k = sget(s); if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; } sock_free(k); return 0; }
static void udp_rx(uint32_t src, uint16_t sport, const uint8_t *data, uint32_t len, void *ctx) {
    wsock_t *k = ctx;
    if (!k->used || !k->q) return;
    int n = (k->qh + 1) % UDPQ;
    if (n == k->qt) return;                                           /* full: dropped, as UDP may */
    dgram_t *g = &k->q[k->qh]; g->ip = src; g->port = sport; g->len = MIN(len, (uint32_t)sizeof g->data); memcpy(g->data, data, g->len);
    k->qh = n;
}
static bool udp_ensure(wsock_t *k) {
    if (k->udp_bound) return true;
    if (!k->lport) k->lport = net_ephemeral_port();
    if (!k->q) k->q = kzalloc(sizeof(dgram_t) * UDPQ);
    if (!k->q || !udp_bind(k->lport, udp_rx, k)) { wsa_err(WSAEADDRINUSE); return false; }
    k->udp_bound = true;
    return true;
}
static int WINAPI bind_(uintptr_t s, const void *sa, int len) {
    wsock_t *k = sget(s); uint32_t ip; uint16_t port;
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (!read_sa(sa, len, &ip, &port)) { wsa_err(WSAEAFNOSUPPORT); return SOCKET_ERROR; }
    k->lip = ip; k->lport = port;
    if (k->type == SOCK_DGRAM && !udp_ensure(k)) return SOCKET_ERROR;
    return 0;
}
static int WINAPI listen_(uintptr_t s, int backlog) {
    wsock_t *k = sget(s);
    if (!k || k->type != SOCK_STREAM) { wsa_err(k ? WSAEOPNOTSUPP : WSAENOTSOCK); return SOCKET_ERROR; }
    if (!k->lport) k->lport = net_ephemeral_port();
    if (tcp_listen(k->lport) < 0) { wsa_err(WSAEADDRINUSE); return SOCKET_ERROR; }
    k->listening = true;
    return 0;
}
static uintptr_t WINAPI accept_(uintptr_t s, void *sa, int *len) {
    wsock_t *k = sget(s);
    if (!k || !k->listening) { wsa_err(k ? WSAEINVAL : WSAENOTSOCK); return INVALID_SOCKET; }
    while (!tcp_accept_ready(k->lport)) {
        if (k->nonblock) { wsa_err(WSAEWOULDBLOCK); return INVALID_SOCKET; }
        task_sleep(10);
        if (!k->used) { wsa_err(WSAEINTR); return INVALID_SOCKET; }
    }
    k->asent &= ~FD_ACCEPT; k->ev_pending &= ~FD_ACCEPT;
    tcb_t *t = tcp_accept(k->lport);
    if (!t) { wsa_err(WSAEWOULDBLOCK); return INVALID_SOCKET; }
    uintptr_t ns = socket_(AF_INET, SOCK_STREAM, 0);
    if (ns == INVALID_SOCKET) { tcp_close(t); return INVALID_SOCKET; }
    wsock_t *c = sget(ns);
    c->t = t; c->was_connected = true; c->rip = tcp_remote_ip(t); c->rport = tcp_remote_port(t); c->lport = tcp_local_port(t);
    c->nonblock = k->nonblock;
    if (k->ev) { c->ev = k->ev; c->evmask = k->evmask; }                 /* the listener's event selection is inherited */
    if (k->aw) { c->aw = k->aw; c->amsg = k->amsg; c->amask = k->amask; }
    write_sa(sa, len, c->rip, c->rport);
    return ns;
}
static int WINAPI connect_(uintptr_t s, const void *sa, int len) {
    wsock_t *k = sget(s); uint32_t ip; uint16_t port;
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (!read_sa(sa, len, &ip, &port)) { wsa_err(WSAEAFNOSUPPORT); return SOCKET_ERROR; }
    if (k->type == SOCK_DGRAM) { k->rip = ip; k->rport = port; return udp_ensure(k) ? 0 : SOCKET_ERROR; }
    if (k->t) { wsa_err(k->connecting ? WSAEALREADY : WSAEISCONN); return SOCKET_ERROR; }
    if (!net_ready()) { wsa_err(WSAENETDOWN); return SOCKET_ERROR; }
    k->rip = ip; k->rport = port;
    k->t = tcp_open_from(ip, port, k->lport);
    if (!k->t) { wsa_err(WSAENETDOWN); return SOCKET_ERROR; }
    k->connecting = true; k->err = 0;
    k->lport = tcp_local_port(k->t);
    if (k->nonblock || k->ev || k->aw) { wsa_err(WSAEWOULDBLOCK); return SOCKET_ERROR; }   /* the event models make a socket non-blocking */
    uint32_t end = timer_ticks() + 21000;
    for (;;) {
        int cs = connect_state(k);
        if (cs == 1) return 0;
        if (cs < 0) { int e = k->err ? k->err : WSAECONNREFUSED; tcp_close(k->t); k->t = NULL; wsa_err(e); return SOCKET_ERROR; }
        if ((int32_t)(timer_ticks() - end) >= 0) { tcp_close(k->t); k->t = NULL; wsa_err(WSAETIMEDOUT); return SOCKET_ERROR; }
        task_sleep(5);
    }
}
static int WINAPI send_(uintptr_t s, const char *buf, int len, int flags) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (k->type == SOCK_DGRAM) {
        if (!k->rport) { wsa_err(WSAENOTCONN); return SOCKET_ERROR; }
        if (!udp_ensure(k)) return SOCKET_ERROR;
        if (len > 1472) { wsa_err(WSAEMSGSIZE); return SOCKET_ERROR; }
        return udp_send(k->rip, k->lport, k->rport, buf, len) ? len : (wsa_err(WSAENETDOWN), SOCKET_ERROR);
    }
    if (!k->t || connect_state(k) != 1) { wsa_err(k->t && k->connecting ? WSAEWOULDBLOCK : WSAENOTCONN); return SOCKET_ERROR; }
    if (k->shut) { wsa_err(WSAECONNABORTED); return SOCKET_ERROR; }
    int done = 0;
    while (done < len) {
        int r = tcp_send_nb(k->t, buf + done, len - done);
        if (r < 0) { if (done) return done; wsa_err(WSAECONNRESET); return SOCKET_ERROR; }
        done += r;
        if (done < len) {
            if (k->nonblock || k->ev || k->aw) { if (done) return done; k->asent &= ~FD_WRITE; k->ev_pending &= ~FD_WRITE; wsa_err(WSAEWOULDBLOCK); return SOCKET_ERROR; }
            task_sleep(5);
        }
    }
    return done;
}
static int WINAPI recv_(uintptr_t s, char *buf, int len, int flags) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (k->type == SOCK_DGRAM) {
        while (k->qh == k->qt) { if (k->nonblock || k->ev || k->aw) { wsa_err(WSAEWOULDBLOCK); return SOCKET_ERROR; } task_sleep(5); }
        dgram_t *g = &k->q[k->qt]; int n = MIN(len, (int)g->len); memcpy(buf, g->data, n);
        if (!(flags & 2)) k->qt = (k->qt + 1) % UDPQ;
        k->asent &= ~FD_READ; k->ev_pending &= ~FD_READ;
        return n;
    }
    if (!k->t || connect_state(k) == 0) { wsa_err(k->t ? WSAEWOULDBLOCK : WSAENOTCONN); return SOCKET_ERROR; }
    for (;;) {
        int r = (flags & 2) ? tcp_peek(k->t, buf, len) : tcp_recv(k->t, buf, len, 0);    /* MSG_PEEK */
        if (r > 0 || r == 0) {
            k->asent &= ~FD_READ; k->ev_pending &= ~FD_READ;              /* FD_READ comes again while data remains */
            return r;
        }
        if (r == -1) { wsa_err(WSAECONNRESET); return SOCKET_ERROR; }
        if (tcp_closed(k)) return 0;
        if (k->nonblock || k->ev || k->aw) { k->asent &= ~FD_READ; k->ev_pending &= ~FD_READ; wsa_err(WSAEWOULDBLOCK); return SOCKET_ERROR; }
        task_sleep(5);
    }
}
static int WINAPI sendto_(uintptr_t s, const char *buf, int len, int flags, const void *to, int tolen) {
    wsock_t *k = sget(s); uint32_t ip; uint16_t port;
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (k->type != SOCK_DGRAM || !to) return send_(s, buf, len, flags);
    if (!read_sa(to, tolen, &ip, &port)) { wsa_err(WSAEAFNOSUPPORT); return SOCKET_ERROR; }
    if (!udp_ensure(k)) return SOCKET_ERROR;
    if (len > 1472) { wsa_err(WSAEMSGSIZE); return SOCKET_ERROR; }
    return udp_send(ip, k->lport, port, buf, len) ? len : (wsa_err(WSAENETDOWN), SOCKET_ERROR);
}
static int WINAPI recvfrom_(uintptr_t s, char *buf, int len, int flags, void *from, int *fromlen) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (k->type != SOCK_DGRAM) { int r = recv_(s, buf, len, flags); if (r >= 0) write_sa(from, fromlen, k->rip, k->rport); return r; }
    if (!udp_ensure(k)) return SOCKET_ERROR;
    while (k->qh == k->qt) { if (k->nonblock || k->ev || k->aw) { wsa_err(WSAEWOULDBLOCK); return SOCKET_ERROR; } task_sleep(5); }
    dgram_t *g = &k->q[k->qt]; int n = MIN(len, (int)g->len); memcpy(buf, g->data, n);
    write_sa(from, fromlen, g->ip, g->port);
    if (!(flags & 2)) k->qt = (k->qt + 1) % UDPQ;
    k->asent &= ~FD_READ; k->ev_pending &= ~FD_READ;
    return n;
}
static int WINAPI shutdown_(uintptr_t s, int how) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (k->t && how >= 1) { tcp_shutdown_wr(k->t); k->shut = true; }
    return 0;
}
static int WINAPI ioctlsocket(uintptr_t s, long cmd, uint32_t *arg) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if ((uint32_t)cmd == 0x8004667E) { k->nonblock = *arg != 0; return 0; }     /* FIONBIO */
    if ((uint32_t)cmd == 0x4004667F) {                                          /* FIONREAD */
        if (k->type == SOCK_DGRAM) { *arg = k->qh != k->qt ? k->q[k->qt].len : 0; return 0; }
        static uint8_t tmp[65536];
        int r = k->t ? tcp_peek(k->t, tmp, sizeof tmp) : 0;
        *arg = r > 0 ? r : 0;
        return 0;
    }
    if ((uint32_t)cmd == 0x40047307) { *arg = 0; return 0; }                    /* SIOCATMARK */
    wsa_err(WSAEINVAL); return SOCKET_ERROR;
}
static int WINAPI setsockopt_(uintptr_t s, int level, int opt, const char *val, int len) { if (!sget(s)) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; } return 0; }
static int WINAPI getsockopt_(uintptr_t s, int level, int opt, char *val, int *len) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    int v = 0;
    if (level == 0xFFFF && opt == 0x1007) { if (k->t) connect_state(k); v = k->err; k->err = 0; }          /* SO_ERROR */
    else if (level == 0xFFFF && opt == 0x1008) v = k->type;                                                /* SO_TYPE */
    else if (level == 0xFFFF && (opt == 0x1001 || opt == 0x1002)) v = 65536;                               /* SO_SNDBUF / SO_RCVBUF */
    else if (level == 0xFFFF && opt == 0x0002) v = k->listening;                                           /* SO_ACCEPTCONN */
    if (val && len && *len >= 4) { memcpy(val, &v, 4); *len = 4; }
    return 0;
}
static int WINAPI getpeername_(uintptr_t s, void *sa, int *len) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    if (!k->rport) { wsa_err(WSAENOTCONN); return SOCKET_ERROR; }
    write_sa(sa, len, k->rip, k->rport); return 0;
}
static int WINAPI getsockname_(uintptr_t s, void *sa, int *len) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    write_sa(sa, len, k->t || k->udp_bound ? netif.ip : k->lip, k->lport); return 0;
}

/* select(): fd_set is { u_int count; SOCKET array[64]; } with SOCKET pointer-sized */
static uintptr_t *fd_arr(void *set) { return set ? (uintptr_t *)((uint8_t *)set + sizeof(uintptr_t)) : NULL; }
static uint32_t *fd_cnt(void *set) { return (uint32_t *)set; }
typedef struct { int32_t sec, usec; } timeval_t;                       /* long tv_sec, tv_usec (32-bit on Windows) */
static int WINAPI select_(int nfds, void *rd, void *wr, void *ex, const timeval_t *tv) {
    uint32_t wait = tv ? (uint32_t)tv->sec * 1000 + (uint32_t)tv->usec / 1000 : 0xFFFFFFFF;
    uint32_t end = timer_ticks() + wait;
    uintptr_t in_r[64], in_w[64], in_e[64]; uint32_t nr = rd ? MIN(*fd_cnt(rd), 64u) : 0, nw = wr ? MIN(*fd_cnt(wr), 64u) : 0, ne = ex ? MIN(*fd_cnt(ex), 64u) : 0;
    if (nr) memcpy(in_r, fd_arr(rd), nr * sizeof(uintptr_t));
    if (nw) memcpy(in_w, fd_arr(wr), nw * sizeof(uintptr_t));
    if (ne) memcpy(in_e, fd_arr(ex), ne * sizeof(uintptr_t));
    if (!nr && !nw && !ne) { if (wait != 0xFFFFFFFF) task_sleep(wait); return 0; }
    for (;;) {
        int total = 0; uint32_t cr = 0, cw = 0, ce = 0;
        for (uint32_t i = 0; i < nr; i++) { wsock_t *k = sget(in_r[i]); if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; } if (can_read(k)) { fd_arr(rd)[cr++] = in_r[i]; total++; } }
        for (uint32_t i = 0; i < nw; i++) { wsock_t *k = sget(in_w[i]); if (k && can_write(k)) { fd_arr(wr)[cw++] = in_w[i]; total++; } }
        for (uint32_t i = 0; i < ne; i++) { wsock_t *k = sget(in_e[i]); if (k && k->t && k->connecting && connect_state(k) < 0) { fd_arr(ex)[ce++] = in_e[i]; total++; } }
        if (total || (wait != 0xFFFFFFFF && (int32_t)(timer_ticks() - end) >= 0)) {
            if (rd) *fd_cnt(rd) = cr; if (wr) *fd_cnt(wr) = cw; if (ex) *fd_cnt(ex) = ce;
            return total;
        }
        task_sleep(5);
    }
}
static int WINAPI WSAFDIsSet(uintptr_t s, void *set) { for (uint32_t i = 0; set && i < *fd_cnt(set); i++) if (fd_arr(set)[i] == s) return 1; return 0; }

/* ---------------------------------------------------------------- the event models */
static uintptr_t WINAPI WSACreateEvent(void) { void *fn = NULL; w32_lookup("kernel32.dll", "CreateEventA", &fn); return ((uintptr_t (WINAPI *)(void *, BOOL, BOOL, const char *))fn)(NULL, 1, 0, NULL); }
static BOOL WINAPI WSACloseEvent(uintptr_t e) { void *fn = NULL; w32_lookup("kernel32.dll", "CloseHandle", &fn); return ((BOOL (WINAPI *)(uintptr_t))fn)(e); }
static BOOL WINAPI WSASetEvent(uintptr_t e) { w32_event_signal(P, e); return 1; }
static BOOL WINAPI WSAResetEvent(uintptr_t e) { void *fn = NULL; w32_lookup("kernel32.dll", "ResetEvent", &fn); return ((BOOL (WINAPI *)(uintptr_t))fn)(e); }
static DWORD WINAPI WSAWaitForMultipleEvents(DWORD n, uintptr_t *evs, BOOL all, DWORD ms, BOOL al) {
    void *fn = NULL; w32_lookup("kernel32.dll", "WaitForMultipleObjects", &fn);
    return ((DWORD (WINAPI *)(DWORD, uintptr_t *, BOOL, DWORD))fn)(n, evs, all, ms);
}
static int WINAPI WSAEventSelect(uintptr_t s, uintptr_t ev, long mask) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    k->ev = ev; k->evmask = mask; k->ev_pending = 0; k->nonblock = true;
    k->aw = 0; k->amask = 0;
    if (ev && mask) poll_socket(k, P);
    return 0;
}
static int WINAPI WSAEnumNetworkEvents(uintptr_t s, uintptr_t ev, void *out) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    poll_socket(k, P);
    struct { long events; int errs[10]; } *ne = out;
    ne->events = k->ev_pending;
    memcpy(ne->errs, k->ev_err, sizeof ne->errs);
    k->ev_pending = 0; memset(k->ev_err, 0, sizeof k->ev_err);
    if (ev) { void *fn = NULL; w32_lookup("kernel32.dll", "ResetEvent", &fn); ((BOOL (WINAPI *)(uintptr_t))fn)(ev); }
    return 0;
}
static int WINAPI WSAAsyncSelect(uintptr_t s, HWND w, UINT msg, long mask) {
    wsock_t *k = sget(s);
    if (!k) { wsa_err(WSAENOTSOCK); return SOCKET_ERROR; }
    k->aw = w; k->amsg = msg; k->amask = mask; k->asent = 0; k->nonblock = true;
    k->ev = 0; k->evmask = 0;
    return 0;
}

/* WSASend / WSARecv without overlapped I/O: WSABUF { u_long len; char *buf; } */
typedef struct { uint32_t len; char *buf; } wsabuf_t;
static int WINAPI WSASend(uintptr_t s, wsabuf_t *b, DWORD n, DWORD *sent, DWORD fl, void *ov, void *cr) {
    DWORD total = 0;
    for (DWORD i = 0; i < n; i++) { int r = send_(s, b[i].buf, b[i].len, fl); if (r < 0) { if (total) break; return SOCKET_ERROR; } total += r; if ((DWORD)r < b[i].len) break; }
    if (sent) *sent = total;
    return 0;
}
static int WINAPI WSARecv(uintptr_t s, wsabuf_t *b, DWORD n, DWORD *got, DWORD *fl, void *ov, void *cr) {
    int r = recv_(s, b[0].buf, b[0].len, fl ? (int)*fl : 0);
    if (r < 0) return SOCKET_ERROR;
    if (got) *got = r; if (fl) *fl = 0;
    return 0;
}
static int WINAPI WSAIoctl(uintptr_t s, DWORD code, void *in, DWORD ni, void *out, DWORD no, DWORD *ret, void *ov, void *cr) { wsa_err(WSAEOPNOTSUPP); return SOCKET_ERROR; }

/* ---------------------------------------------------------------- names and addresses */
static uint16_t WINAPI htons_(uint16_t v) { return htons(v); }
static uint32_t WINAPI htonl_(uint32_t v) { return htonl(v); }
static uint16_t WINAPI ntohs_(uint16_t v) { return ntohs(v); }
static uint32_t WINAPI ntohl_(uint32_t v) { return ntohl(v); }
static uint32_t WINAPI inet_addr_(const char *s) { uint32_t ip; return s && str_to_ip(s, &ip) ? htonl(ip) : 0xFFFFFFFFu; }
static char ntoa_buf[16];
static char *WINAPI inet_ntoa_(uint32_t a) { ip_to_str(ntohl(a), ntoa_buf); return ntoa_buf; }
static int WINAPI inet_pton_(int af, const char *s, void *dst) { uint32_t ip; if (af != AF_INET) { wsa_err(WSAEAFNOSUPPORT); return -1; } if (!str_to_ip(s, &ip)) return 0; ip = htonl(ip); memcpy(dst, &ip, 4); return 1; }
static const char *WINAPI inet_ntop_(int af, const void *src, char *dst, size_t n) { if (af != AF_INET) { wsa_err(WSAEAFNOSUPPORT); return NULL; } uint32_t ip; memcpy(&ip, src, 4); char b[16]; ip_to_str(ntohl(ip), b); strlcpy(dst, b, n); return dst; }
static int WINAPI gethostname_(char *name, int len) { strlcpy(name, "wrld", len); return 0; }
static bool resolve(const char *name, uint32_t *ip) {
    if (!name || !*name) { *ip = 0; return true; }
    if (str_to_ip(name, ip)) return true;
    if (!strcasecmp(name, "localhost")) { *ip = netif.ip; return true; }   /* no loopback interface: our own address */
    char err[100];
    return net_ready() && dns_resolve(name, ip, err, sizeof err);
}
/* hostent (one per process at a time, as on Windows): h_name, h_aliases, h_addrtype, h_length, h_addr_list */
typedef struct { char *name; char **aliases; int16_t type, len; char **addrs; } hostent_t;
static struct { hostent_t h; char name[256]; char *aliases[1]; char *addrs[2]; uint32_t addr; } hostbuf;
static hostent_t *WINAPI gethostbyname_(const char *name) {
    uint32_t ip;
    if (!resolve(name, &ip)) { wsa_err(WSAHOST_NOT_FOUND); return NULL; }
    strlcpy(hostbuf.name, name ? name : "wrld", sizeof hostbuf.name);
    hostbuf.addr = htonl(ip); hostbuf.aliases[0] = NULL; hostbuf.addrs[0] = (char *)&hostbuf.addr; hostbuf.addrs[1] = NULL;
    hostbuf.h = (hostent_t){ hostbuf.name, hostbuf.aliases, AF_INET, 4, hostbuf.addrs };
    return &hostbuf.h;
}
static hostent_t *WINAPI gethostbyaddr_(const char *addr, int len, int type) {
    if (type != AF_INET || len < 4) { wsa_err(WSANO_DATA); return NULL; }
    uint32_t ip; memcpy(&ip, addr, 4); char b[16]; ip_to_str(ntohl(ip), b);
    return gethostbyname_(b);
}
static int service_port(const char *svc) {
    if (!svc || !*svc) return 0;
    if (isdigit((unsigned char)svc[0])) return atoi(svc);
    static const struct { const char *n; int p; } k[] = { { "http", 80 }, { "https", 443 }, { "ssh", 22 }, { "telnet", 23 }, { "ftp", 21 }, { "smtp", 25 }, { "domain", 53 }, { "pop3", 110 }, { "imap", 143 } };
    for (unsigned i = 0; i < ARRAY_LEN(k); i++) if (!strcasecmp(svc, k[i].n)) return k[i].p;
    return -1;
}
static struct { char name[16]; char *aliases[1]; char proto[4]; uint8_t s[32]; } servbuf;
static void *WINAPI getservbyname_(const char *name, const char *proto) {
    int p = service_port(name);
    if (p <= 0) { wsa_err(WSANO_DATA); return NULL; }
    strlcpy(servbuf.name, name, sizeof servbuf.name); servbuf.aliases[0] = NULL; strcpy(servbuf.proto, "tcp");
    /* struct servent: char *s_name; char **s_aliases; then x86: short s_port; char *s_proto / x64: char *s_proto; short s_port */
    uint8_t *s = servbuf.s; memset(s, 0, sizeof servbuf.s);
    char *n = servbuf.name; char **al = servbuf.aliases; char *pr = servbuf.proto; uint16_t port = htons((uint16_t)p);
    memcpy(s, &n, sizeof n); memcpy(s + sizeof n, &al, sizeof al);
    if (sizeof(void *) == 8) { memcpy(s + 16, &pr, 8); memcpy(s + 24, &port, 2); }
    else { memcpy(s + 8, &port, 2); memcpy(s + 12, &pr, 4); }
    return s;
}
/* addrinfo: ai_flags, ai_family, ai_socktype, ai_protocol, ai_addrlen (size_t), ai_canonname, ai_addr, ai_next */
typedef struct addrinfo_s { int flags, family, socktype, protocol; size_t addrlen; char *canonname; void *addr; struct addrinfo_s *next; } addrinfo_t;
static int WINAPI getaddrinfo_(const char *node, const char *service, const addrinfo_t *hints, addrinfo_t **res) {
    *res = NULL;
    int family = hints ? hints->family : 0, socktype = hints ? hints->socktype : 0, flags = hints ? hints->flags : 0;
    if (family != 0 && family != AF_INET) return WSAEAFNOSUPPORT;
    int port = service_port(service);
    if (port < 0) return 10109;                                          /* WSATYPE_NOT_FOUND */
    uint32_t ip;
    if (!node) ip = (flags & 1) ? 0 : netif.ip;                       /* AI_PASSIVE: any address */
    else if ((flags & 4) && !str_to_ip(node, &ip)) return WSAHOST_NOT_FOUND;   /* AI_NUMERICHOST */
    else if (!resolve(node, &ip)) return WSAHOST_NOT_FOUND;
    w32_proc_t *p = P;
    addrinfo_t *ai = w32_alloc(p, sizeof *ai + sizeof(sockaddr_in_t) + 256);
    if (!ai) return 8;
    memset(ai, 0, sizeof *ai + sizeof(sockaddr_in_t) + 256);
    sockaddr_in_t *sa = (sockaddr_in_t *)(ai + 1);
    sa->family = AF_INET; sa->port = htons((uint16_t)port); sa->addr = htonl(ip);
    ai->family = AF_INET; ai->socktype = socktype ? socktype : SOCK_STREAM;
    ai->protocol = ai->socktype == SOCK_DGRAM ? 17 : 6;
    ai->addrlen = sizeof *sa; ai->addr = sa;
    if ((flags & 2) && node) { ai->canonname = (char *)(sa + 1); strlcpy(ai->canonname, node, 256); }   /* AI_CANONNAME */
    *res = ai;
    return 0;
}
static void WINAPI freeaddrinfo_(addrinfo_t *ai) { while (ai) { addrinfo_t *n = ai->next; w32_free(P, ai); ai = n; } }
/* ADDRINFOW: the same, with a wide canonical name */
static void w2a_s(const WCHAR *w, char *a, int cap) { int i = 0; if (!w) { a[0] = 0; return; } for (; w[i] && i < cap - 1; i++) a[i] = w[i] < 128 ? (char)w[i] : '?'; a[i] = 0; }
static int WINAPI GetAddrInfoW(const WCHAR *node, const WCHAR *service, const addrinfo_t *hints, addrinfo_t **res) {
    char n[256], s[32]; w2a_s(node, n, sizeof n); w2a_s(service, s, sizeof s);
    int r = getaddrinfo_(node ? n : NULL, service ? s : NULL, hints, res);
    if (!r && *res && (*res)->canonname) { char tmp[256]; strlcpy(tmp, (*res)->canonname, sizeof tmp); WCHAR *w = (WCHAR *)(*res)->canonname; int i = 0; for (; tmp[i] && i < 127; i++) w[i] = (uint8_t)tmp[i]; w[i] = 0; }
    return r;
}
static int WINAPI getnameinfo_(const void *sa, int salen, char *host, DWORD hl, char *serv, DWORD sl, int flags) {
    uint32_t ip; uint16_t port;
    if (!read_sa(sa, salen, &ip, &port)) return WSAEAFNOSUPPORT;
    if (host && hl) { char b[16]; ip_to_str(ip, b); strlcpy(host, b, hl); }
    if (serv && sl) snprintf(serv, sl, "%u", port);
    return 0;
}
static int WINAPI WSAAddressToStringA(const void *sa, DWORD len, void *info, char *out, DWORD *outlen) {
    uint32_t ip; uint16_t port;
    if (!read_sa(sa, len, &ip, &port)) { wsa_err(WSAEINVAL); return SOCKET_ERROR; }
    char b[32], ips[16]; ip_to_str(ip, ips);
    if (port) snprintf(b, sizeof b, "%s:%u", ips, port); else strlcpy(b, ips, sizeof b);
    if (*outlen <= strlen(b)) { *outlen = strlen(b) + 1; wsa_err(WSAEFAULT); return SOCKET_ERROR; }
    strcpy(out, b); *outlen = strlen(b) + 1;
    return 0;
}
static int WINAPI WSAAddressToStringW(const void *sa, DWORD len, void *info, WCHAR *out, DWORD *outlen) {
    char b[40]; DWORD l = sizeof b;
    if (WSAAddressToStringA(sa, len, info, b, &l)) return SOCKET_ERROR;
    if (*outlen < l) { *outlen = l; wsa_err(WSAEFAULT); return SOCKET_ERROR; }
    for (DWORD i = 0; i < l; i++) out[i] = (uint8_t)b[i];
    *outlen = l;
    return 0;
}
static int WINAPI WSAStringToAddressA(char *s, int af, void *info, void *sa, int *len) {
    char b[64]; strlcpy(b, s, sizeof b);
    char *c = strchr(b, ':'); int port = 0; if (c) { *c = 0; port = atoi(c + 1); }
    uint32_t ip; if (af != AF_INET || !str_to_ip(b, &ip)) { wsa_err(WSAEINVAL); return SOCKET_ERROR; }
    write_sa(sa, len, ip, (uint16_t)port);
    return 0;
}
static int WINAPI WSAStringToAddressW(WCHAR *s, int af, void *info, void *sa, int *len) { char b[64]; w2a_s(s, b, sizeof b); return WSAStringToAddressA(b, af, info, sa, len); }
static BOOL WINAPI WSAIsBlocking(void) { return 0; }
static int WINAPI WSACancelBlockingCall(void) { return 0; }

/* ---------------------------------------------------------------- the table */
#define W(n) { #n, (void *)n }
#define WA(name, fn) { name, (void *)(fn) }
const w32_export_t ws2_exports[] = {
    W(WSAStartup), W(WSACleanup), W(WSAGetLastError), W(WSASetLastError), WA("socket", socket_), W(WSASocketA), WA("WSASocketW", WSASocketA),
    W(closesocket), WA("bind", bind_), WA("listen", listen_), WA("accept", accept_), WA("connect", connect_), WA("send", send_), WA("recv", recv_),
    WA("sendto", sendto_), WA("recvfrom", recvfrom_), WA("shutdown", shutdown_), W(ioctlsocket), WA("setsockopt", setsockopt_), WA("getsockopt", getsockopt_),
    WA("getpeername", getpeername_), WA("getsockname", getsockname_), WA("select", select_), WA("__WSAFDIsSet", WSAFDIsSet),
    W(WSACreateEvent), W(WSACloseEvent), W(WSASetEvent), W(WSAResetEvent), W(WSAWaitForMultipleEvents), W(WSAEventSelect), W(WSAEnumNetworkEvents),
    W(WSAAsyncSelect), W(WSASend), W(WSARecv), W(WSAIoctl),
    WA("htons", htons_), WA("htonl", htonl_), WA("ntohs", ntohs_), WA("ntohl", ntohl_), WA("inet_addr", inet_addr_), WA("inet_ntoa", inet_ntoa_),
    WA("inet_pton", inet_pton_), WA("InetPtonA", inet_pton_), WA("inet_ntop", inet_ntop_), WA("InetNtopA", inet_ntop_),
    WA("gethostname", gethostname_), WA("gethostbyname", gethostbyname_), WA("gethostbyaddr", gethostbyaddr_), WA("getservbyname", getservbyname_),
    WA("getaddrinfo", getaddrinfo_), WA("freeaddrinfo", freeaddrinfo_), W(GetAddrInfoW), WA("FreeAddrInfoW", freeaddrinfo_), WA("getnameinfo", getnameinfo_),
    W(WSAAddressToStringA), W(WSAAddressToStringW), W(WSAStringToAddressA), W(WSAStringToAddressW), W(WSAIsBlocking), W(WSACancelBlockingCall),
    /* the ordinals programs import ws2_32 by */
    WA("Ordinal1", accept_), WA("Ordinal2", bind_), WA("Ordinal3", closesocket), WA("Ordinal4", connect_), WA("Ordinal5", getpeername_),
    WA("Ordinal6", getsockname_), WA("Ordinal7", getsockopt_), WA("Ordinal8", htonl_), WA("Ordinal9", htons_), WA("Ordinal10", ioctlsocket),
    WA("Ordinal11", inet_addr_), WA("Ordinal12", inet_ntoa_), WA("Ordinal13", listen_), WA("Ordinal14", ntohl_), WA("Ordinal15", ntohs_),
    WA("Ordinal16", recv_), WA("Ordinal17", recvfrom_), WA("Ordinal18", select_), WA("Ordinal19", send_), WA("Ordinal20", sendto_),
    WA("Ordinal21", setsockopt_), WA("Ordinal22", shutdown_), WA("Ordinal23", socket_), WA("Ordinal51", gethostbyaddr_), WA("Ordinal52", gethostbyname_),
    WA("Ordinal55", getservbyname_), WA("Ordinal57", gethostname_), WA("Ordinal101", WSAAsyncSelect), WA("Ordinal111", WSAGetLastError),
    WA("Ordinal112", WSASetLastError), WA("Ordinal115", WSAStartup), WA("Ordinal116", WSACleanup), WA("Ordinal151", WSAFDIsSet),
};
const int ws2_count = ARRAY_LEN(ws2_exports);
