/* Pipes, sockets, epoll, eventfd and timerfd.
 *
 * Everything here is polled from the cooperative scheduler: a call that would
 * block sleeps for a millisecond and looks again. That is enough for an event
 * loop, and it keeps the kernel free of wait queues for now.
 *
 * AF_UNIX sockets are local to this machine and move messages, not bytes, so
 * SOCK_STREAM reads may return less than was written; SOCK_DGRAM and
 * SOCK_SEQPACKET keep boundaries.  AF_INET sockets (TCP and UDP) hand their
 * bytes to the stack in src/net; their ports and addresses are in host order
 * here and converted at the sockaddr boundary.
 */
#ifdef __x86_64__
#include "lx.h"
#include "net.h"

#define PIPE_CAP      (64 * 1024)
#define SOCK_QLEN     (256 * 1024)
#define MAX_MSG_FD    8

#define O_NONBLOCK_F  04000
#define O_CLOEXEC_F   02000000
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define SOCK_CLOEXEC2 0x80000
#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  04000
#define EFD_CLOEXEC   02000000
#define TFD_NONBLOCK  04000
#define TFD_CLOEXEC   02000000

#define AF_UNIX_      1
#define AF_INET_      2
#define SOCK_STREAM_  1
#define SOCK_DGRAM_   2
#define SOCK_SEQPACKET_ 5
#define IPPROTO_TCP_  6
#define IPPROTO_UDP_  17

#define POLLIN_  0x001
#define POLLOUT_ 0x004
#define POLLERR_ 0x008
#define POLLHUP_ 0x010
#define POLLNVAL_ 0x020

typedef struct lxobj { int refs; } lxobj_t;

typedef struct lxpipe {
    lxobj_t o;
    uint8_t *ring;
    uint32_t cap, head, len;
    int readers, writers;
} lxpipe_t;

typedef struct lxmsg {
    struct lxmsg *next;
    uint32_t len, off;
    uint8_t *data;
    int nfds;
    lxfile_t *fds[MAX_MSG_FD];
    uint32_t src_ip;              /* AF_INET datagrams */
    uint16_t src_port;
} lxmsg_t;

typedef struct lxsock {
    lxobj_t o;
    int type;
    bool bound, listening, connected, peer_gone;
    char path[108];
    struct lxsock *peer;
    lxmsg_t *qhead, *qtail;
    uint32_t qlen;
    uint64_t dropped;
    struct lxsock *accept_head, *accept_next;
    struct lxsock *reg_next;

    /* AF_INET: addresses and ports are in host order, like the network stack */
    int family;
    int proto;
    uint32_t laddr, raddr;
    uint16_t lport, rport;
    bool udp_bound;
    bool reuseaddr, nodelay;
    int so_error;
    tcb_t *tcb;                   /* stream socket */
    struct lxsock *inet_next;
} lxsock_t;

typedef struct lxepitem { int fd; uint32_t events; uint64_t data; } lxepitem_t;
typedef struct lxepoll { lxobj_t o; int n; lxepitem_t items[64]; } lxepoll_t;

typedef struct lxevfd { lxobj_t o; uint64_t count; bool sem; } lxevfd_t;

typedef struct lxtfd {
    lxobj_t o;
    bool armed;
    uint32_t next, interval;
    uint64_t exp;
} lxtfd_t;

static lxsock_t *bound_list;
static lxsock_t *inet_socks;

static lxproc_t *cur_proc(void) { lxthread_t *t = lx_current(); return t ? t->proc : NULL; }
static bool put(lxproc_t *p, uint64_t dst, const void *src, uint64_t n) {
    if (!uvm_check(p->vm, dst, n, true)) return false;
    memcpy((void *)(uintptr_t)dst, src, n);
    return true;
}
static bool get(lxproc_t *p, void *dst, uint64_t src, uint64_t n) {
    if (!uvm_check(p->vm, src, n, false)) return false;
    memcpy(dst, (const void *)(uintptr_t)src, n);
    return true;
}
static void idle(int64_t timeout_ms) { if (timeout_ms < 0) task_sleep(1); else if (timeout_ms > 0) task_sleep((uint32_t)timeout_ms); else task_yield(); }

/* ---------------- wrapping ---------------- */
static lxfile_t *wrap(int type, void *obj) {
    lxfile_t *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->type = type;
    f->refs = 1;
    f->obj = obj;
    ((lxobj_t *)obj)->refs++;
    return f;
}
static int install(lxproc_t *p, lxfile_t *f, int flags) {
    int fd = lx_fd_alloc(p, f, 0);
    if (fd < 0) { lx_file_put(f); return fd; }
    if (flags & (O_CLOEXEC_F | SOCK_CLOEXEC | SOCK_CLOEXEC2)) p->fdflags[fd] = 1;
    return fd;
}

/* ---------------- pipes ---------------- */
static lxpipe_t *pipe_new(void) {
    lxpipe_t *pp = kzalloc(sizeof *pp);
    if (!pp) return NULL;
    pp->ring = kmalloc(PIPE_CAP);
    if (!pp->ring) { kfree(pp); return NULL; }
    pp->cap = PIPE_CAP;
    return pp;
}
static int64_t pipe_read(lxpipe_t *pp, void *dst, uint64_t n, bool nb) {
    uint8_t *d = dst;
    uint64_t got = 0;
    if (!n) return 0;
    while (got < n) {
        while (!pp->len) {
            if (!pp->writers) return (int64_t)got;
            if (nb) return got ? (int64_t)got : -EAGAIN;
            if (lx_block_interrupted(lx_current())) return -EINTR;
            task_sleep(1);
        }
        uint32_t k = (uint32_t)MIN(n - got, (uint64_t)pp->len);
        uint32_t tail = pp->cap - pp->head;
        if (k > tail) k = tail;
        memcpy(d + got, pp->ring + pp->head, k);
        pp->head = (pp->head + k) % pp->cap;
        pp->len -= k;
        got += k;
    }
    return (int64_t)got;
}
static int64_t pipe_write(lxpipe_t *pp, const void *src, uint64_t n, bool nb) {
    const uint8_t *s = src;
    uint64_t done = 0;
    while (done < n) {
        while (pp->len == pp->cap) {
            if (!pp->readers) return done ? (int64_t)done : -EPIPE;
            if (nb) return done ? (int64_t)done : -EAGAIN;
            if (lx_block_interrupted(lx_current())) return -EINTR;
            task_sleep(1);
        }
        if (!pp->readers) return done ? (int64_t)done : -EPIPE;
        uint32_t widx = (pp->head + pp->len) % pp->cap;
        uint32_t k = (uint32_t)MIN(n - done, (uint64_t)(pp->cap - pp->len));
        uint32_t tail = pp->cap - widx;
        if (k > tail) k = tail;
        memcpy(pp->ring + widx, s + done, k);
        pp->len += k;
        done += k;
    }
    return (int64_t)done;
}

/* ---------------- sockets ---------------- */
static lxmsg_t *msg_new(const void *src, uint64_t n) {
    lxmsg_t *m = kzalloc(sizeof *m);
    if (!m) return NULL;
    m->data = kmalloc(n ? n : 1);
    if (!m->data) { kfree(m); return NULL; }
    if (n) memcpy(m->data, src, n);
    m->len = (uint32_t)n;
    return m;
}
static void msg_free(lxmsg_t *m) {
    for (int i = 0; i < m->nfds; i++) if (m->fds[i]) lx_file_put(m->fds[i]);
    if (m->data) kfree(m->data);
    kfree(m);
}
static void q_push(lxsock_t *s, lxmsg_t *m) {
    m->next = NULL;
    if (s->qtail) s->qtail->next = m; else s->qhead = m;
    s->qtail = m;
    s->qlen += m->len;
}
static void q_clear(lxsock_t *s) {
    while (s->qhead) { lxmsg_t *m = s->qhead; s->qhead = m->next; msg_free(m); }
    s->qtail = NULL; s->qlen = 0;
}
static lxsock_t *sock_new(int type) {
    lxsock_t *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->type = type;
    return s;
}
static lxsock_t *sock_lookup(const char *path) {
    for (lxsock_t *s = bound_list; s; s = s->reg_next) if (!strcmp(s->path, path)) return s;
    return NULL;
}
static void sock_unregister(lxsock_t *s) {
    for (lxsock_t **pp = &bound_list; *pp; pp = &(*pp)->reg_next)
        if (*pp == s) { *pp = s->reg_next; break; }
}
static bool sock_readable(lxsock_t *s) { return s->qhead != NULL || s->peer_gone || (!s->peer && s->connected); }
static bool sock_writable(lxsock_t *s) { return s->peer && !s->peer_gone && !s->peer->peer_gone; }

/* Returns a message, or NULL when the queue is empty. Consumes it unless peek. */
static lxmsg_t *sock_take(lxsock_t *s, bool peek) {
    lxmsg_t *m = s->qhead;
    if (!m) return NULL;
    s->qlen -= (m->len - m->off);
    if (!peek) { s->qhead = m->next; if (!s->qhead) s->qtail = NULL; }
    return m;
}
static int64_t sock_recv(lxsock_t *s, void *dst, uint64_t n, bool nb, bool peek) {
    uint8_t *d = dst;
    uint64_t got = 0;
    while (got < n) {
        lxmsg_t *m = sock_take(s, peek);
        if (!m) {
            if (s->peer_gone || !s->peer) return (int64_t)got;
            if (nb) return got ? (int64_t)got : -EAGAIN;
            if (lx_block_interrupted(lx_current())) return -EINTR;
            task_sleep(1);
            continue;
        }
        uint32_t avail = m->len - m->off;
        uint32_t k = (uint32_t)MIN(n - got, (uint64_t)avail);
        memcpy(d + got, m->data + m->off, k);
        m->off += k;
        got += k;
        bool done = m->off >= m->len;
        if (peek) return (int64_t)k;
        if (done) msg_free(m);
        else { s->qhead = m; s->qlen += (m->len - m->off); if (!s->qtail) s->qtail = m; }
        if (k == 0) break;
    }
    return (int64_t)got;
}
static int64_t sock_send(lxsock_t *s, const void *src, uint64_t n, bool nb, lxfile_t **fds, int nfds) {
    if (!s->peer || s->peer_gone) return -EPIPE;
    while (s->peer->qlen > SOCK_QLEN) {
        if (nb) return -EAGAIN;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
    lxmsg_t *m = msg_new(src, n);
    if (!m) return -ENOMEM;
    for (int i = 0; i < nfds && i < MAX_MSG_FD; i++) { m->fds[m->nfds++] = fds[i]; }
    q_push(s->peer, m);
    return (int64_t)n;
}

/* ---------------- AF_INET sockets ---------------- */
/* Linux sockaddr_in, as it appears in memory: 16 bytes, network byte order. */
typedef struct PACKED { uint16_t family, port; uint32_t addr; uint8_t zero[8]; } sin_t;

static bool inet_port_used(uint16_t port, lxsock_t *except) {
    for (lxsock_t *o = inet_socks; o; o = o->inet_next)
        if (o != except && o->bound && o->lport == port) return true;
    return false;
}
static void inet_link(lxsock_t *s) { s->inet_next = inet_socks; inet_socks = s; }
static void inet_unlink(lxsock_t *s) {
    for (lxsock_t **pp = &inet_socks; *pp; pp = &(*pp)->inet_next)
        if (*pp == s) { *pp = s->inet_next; s->inet_next = NULL; return; }
}
static lxsock_t *inet_new(int type) {
    lxsock_t *s = sock_new(type);
    if (!s) return NULL;
    s->family = AF_INET_;
    s->proto = type == SOCK_STREAM_ ? IPPROTO_TCP_ : IPPROTO_UDP_;
    inet_link(s);
    return s;
}
static int in_parse(lxproc_t *p, uint64_t uaddr, int len, uint32_t *ip, uint16_t *port) {
    if (len < 8) return -EINVAL;
    sin_t sa; memset(&sa, 0, sizeof sa);
    if (!get(p, &sa, uaddr, MIN((int)sizeof sa, len))) return -EFAULT;
    if (sa.family != AF_INET_) return -EAFNOSUPPORT;
    if (port) *port = ntohs(sa.port);
    if (ip) *ip = ntohl(sa.addr);
    return 0;
}
static int in_put(lxproc_t *p, uint64_t uaddr, uint64_t ualen, uint32_t ip, uint16_t port) {
    if (!uaddr || !ualen) return 0;
    int32_t cap = 0;
    if (!get(p, &cap, ualen, 4)) return -EFAULT;
    sin_t sa; memset(&sa, 0, sizeof sa);
    sa.family = AF_INET_; sa.port = htons(port); sa.addr = htonl(ip);
    if (!put(p, uaddr, &sa, MIN((uint64_t)MAX(cap, 0), (uint64_t)sizeof sa))) return -EFAULT;
    int32_t l = sizeof sa;
    return put(p, ualen, &l, 4) ? 0 : -EFAULT;
}
static int inet_map_error(int e) { return e == 1 ? ECONNREFUSED : e == 2 ? ECONNRESET : ETIMEDOUT; }

/* A connecting socket becomes usable as soon as the stack says so. */
static void inet_sync(lxsock_t *s) {
    if (s->family != AF_INET_ || s->type != SOCK_STREAM_ || !s->tcb || s->connected) return;
    int st = tcp_status(s->tcb);
    if (st == 1) { s->connected = true; s->lport = tcp_local_port(s->tcb); }
    else if (st < 0 && !s->so_error) s->so_error = inet_map_error(tcp_error(s->tcb));
}

/* Datagrams arrive on the network task; queue them for recv(). */
static void inet_udp_input(uint32_t src, uint16_t sport, const uint8_t *data, uint32_t len, void *ctx) {
    lxsock_t *s = ctx;
    if (!s || s->family != AF_INET_ || s->type != SOCK_DGRAM_) return;
    if (s->qlen + len > SOCK_QLEN) { s->dropped++; return; }
    lxmsg_t *m = msg_new(data, len);
    if (!m) { s->dropped++; return; }
    m->src_ip = src; m->src_port = sport;
    q_push(s, m);
}
static int inet_auto_bind(lxsock_t *s) {
    if (s->family != AF_INET_ || s->bound) return 0;
    for (int i = 0; i < 128; i++) {
        uint16_t port = net_ephemeral_port();
        if (inet_port_used(port, s)) continue;
        if (s->type == SOCK_DGRAM_) {
            if (!udp_bind(port, inet_udp_input, s)) continue;
            s->udp_bound = true;
        }
        s->lport = port; s->bound = true;
        return 0;
    }
    return -EADDRINUSE;
}
static int64_t inet_udp_send(lxsock_t *s, const void *buf, uint64_t n, uint32_t dip, uint16_t dport, bool have_dest) {
    if (!have_dest) {
        if (!s->connected) return -EDESTADDRREQ;
        dip = s->raddr; dport = s->rport;
    }
    if (n > 1472) return -EMSGSIZE;                      /* no IP fragmentation in this stack */
    int r = inet_auto_bind(s);
    if (r < 0) return r;
    if (!udp_send(dip, s->lport, dport, buf, (uint32_t)n)) return -ENOBUFS;
    return (int64_t)n;
}
static int64_t inet_tcp_send(lxsock_t *s, const void *buf, uint64_t n, bool nb) {
    if (!s->tcb) return -ENOTCONN;
    inet_sync(s);
    if (tcp_status(s->tcb) == 0) {
        if (nb) return -EAGAIN;
        while (tcp_status(s->tcb) == 0) {
            if (lx_block_interrupted(lx_current())) return -EINTR;
            task_sleep(1);
        }
        inet_sync(s);
    }
    if (tcp_status(s->tcb) < 0)
        return s->so_error ? -s->so_error : -ECONNRESET;
    uint64_t done = 0;
    while (done < n) {
        int r = tcp_send_nb(s->tcb, (const uint8_t *)buf + done, (uint32_t)MIN(n - done, 0x7FFFFFFFu));
        if (r < 0) return done ? (int64_t)done : (s->so_error ? -s->so_error : -EPIPE);
        if (!r) {
            if (nb) return done ? (int64_t)done : -EAGAIN;
            if (lx_block_interrupted(lx_current())) return done ? (int64_t)done : -EINTR;
            task_sleep(1);
            continue;
        }
        done += (uint32_t)r;
    }
    return (int64_t)done;
}
static int64_t inet_tcp_recv(lxsock_t *s, void *buf, uint64_t n, bool nb, bool peek) {
    (void)nb;
    if (!s->tcb) return -ENOTCONN;
    inet_sync(s);
    if (tcp_status(s->tcb) < 0 && !tcp_recv_ready(s->tcb))
        return s->so_error ? -s->so_error : -ECONNRESET;
    int r = peek ? tcp_peek(s->tcb, buf, (uint32_t)MIN(n, 0x7FFFFFFFu))
                 : tcp_recv(s->tcb, buf, (uint32_t)MIN(n, 0x7FFFFFFFu), 0);
    if (r == -2) return -EAGAIN;
    if (r < 0) {
        int e = tcp_error(s->tcb);
        s->so_error = e ? inet_map_error(e) : ECONNRESET;
        return -s->so_error;
    }
    return r;
}
static int64_t inet_udp_recv(lxsock_t *s, void *buf, uint64_t n, bool peek,
                             uint32_t *sip, uint16_t *sport, uint32_t *full) {
    lxmsg_t *m = s->qhead;
    if (!m) return -EAGAIN;
    uint32_t len = m->len;
    uint64_t k = MIN(n, (uint64_t)len);
    if (k) memcpy(buf, m->data, k);
    if (sip) *sip = m->src_ip;
    if (sport) *sport = m->src_port;
    if (full) *full = len;
    if (!peek) {
        s->qhead = m->next;
        if (!s->qhead) s->qtail = NULL;
        s->qlen -= len;
        msg_free(m);
    }
    return (int64_t)k;
}
static int64_t inet_read(lxsock_t *s, void *buf, uint64_t n, bool nb,
                         uint32_t *sip, uint16_t *sport, uint32_t *full, bool peek) {
    if (!n) return 0;
    for (;;) {
        int64_t r = s->type == SOCK_DGRAM_
                  ? inet_udp_recv(s, buf, n, peek, sip, sport, full)
                  : inet_tcp_recv(s, buf, n, nb, peek);
        if (r != -EAGAIN) return r;
        if (nb) return -EAGAIN;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
}
static int64_t inet_write(lxsock_t *s, const void *buf, uint64_t n, bool nb,
                          uint32_t dip, uint16_t dport, bool have_dest) {
    if (s->type == SOCK_DGRAM_) return inet_udp_send(s, buf, n, dip, dport, have_dest);
    if (have_dest) return -EISCONN;
    return inet_tcp_send(s, buf, n, nb);
}

/* ---------------- eventfd and timerfd ---------------- */
static void tfd_tick(lxtfd_t *t) {
    if (!t->armed) return;
    uint32_t now = timer_ticks();
    if ((int32_t)(now - t->next) < 0) return;
    uint32_t iv = t->interval ? t->interval : 1;
    uint32_t laps = 1 + (now - t->next) / iv;
    t->exp += laps;
    if (t->interval) t->next += laps * t->interval;
    else t->armed = false;
}

/* ---------------- readiness ---------------- */
bool lx_ipc_ready(lxfile_t *f, uint32_t want) {
    if (!f) return false;
    switch (f->type) {
    case FD_PIPE: {
        lxpipe_t *pp = f->obj;
        if (want & POLLIN_) return pp->len > 0 || pp->writers == 0;
        if (want & POLLOUT_) return pp->readers > 0 && pp->len < pp->cap;
        return false;
    }
    case FD_SOCKET: {
        lxsock_t *s = f->obj;
        if (s->family == AF_INET_) {
            inet_sync(s);
            if (want & POLLIN_) {
                if (s->type == SOCK_DGRAM_) return s->qhead != NULL;
                if (s->listening) return tcp_accept_ready(s->lport);
                if (!s->tcb) return true;             /* unconnected: read reports ENOTCONN */
                if (!s->connected) return tcp_status(s->tcb) != 0;   /* failed connect is readable so read() reports it */
                return tcp_recv_ready(s->tcb);
            }
            if (want & POLLOUT_) {
                if (s->type == SOCK_DGRAM_) return true;
                if (!s->tcb) return false;
                if (!s->connected) return tcp_status(s->tcb) != 0;
                return tcp_send_ready(s->tcb);
            }
            if (want & POLLERR_) return s->so_error != 0 || (s->tcb && tcp_status(s->tcb) < 0);
            if (want & POLLHUP_) return s->tcb && tcp_hup(s->tcb);
            return false;
        }
        if (want & POLLIN_) return sock_readable(s);
        if (want & POLLOUT_) return sock_writable(s) && s->qlen < SOCK_QLEN;
        if (want & POLLHUP_) return s->peer_gone;
        return false;
    }
    case FD_EVENTFD: { lxevfd_t *e = f->obj; if (want & POLLIN_) return e->count > 0; if (want & POLLOUT_) return e->count < ~0ull - 1; return false; }
    case FD_TIMERFD: { lxtfd_t *t = f->obj; tfd_tick(t); if (want & POLLIN_) return t->exp > 0; return false; }
    case FD_WINDOW: return (want & POLLIN_) && lxwin_ready(f->obj);
    default: return false;
    }
}

int64_t lx_ipc_read(lxfile_t *f, void *dst, uint64_t n, bool nonblock) {
    switch (f->type) {
    case FD_PIPE: return pipe_read(f->obj, dst, n, nonblock);
    case FD_SOCKET: {
        lxsock_t *s = f->obj;
        if (s->family == AF_INET_) return inet_read(s, dst, n, nonblock, NULL, NULL, NULL, false);
        return sock_recv(s, dst, n, nonblock, false);
    }
    case FD_EVENTFD: {
        lxevfd_t *e = f->obj;
        if (n < 8) return -EINVAL;
        while (!e->count) {
            if (nonblock) return -EAGAIN;
            if (lx_block_interrupted(lx_current())) return -EINTR;
            task_sleep(1);
        }
        uint64_t v = e->sem ? 1 : e->count;
        if (e->sem) e->count--; else e->count = 0;
        memcpy(dst, &v, 8);
        return 8;
    }
    case FD_TIMERFD: {
        lxtfd_t *t = f->obj;
        if (n < 8) return -EINVAL;
        for (;;) { tfd_tick(t); if (t->exp) break; if (nonblock) return -EAGAIN; if (lx_block_interrupted(lx_current())) return -EINTR; task_sleep(1); }
        uint64_t v = t->exp; t->exp = 0;
        memcpy(dst, &v, 8);
        return 8;
    }
    }
    return -ENOTTY;
}
int64_t lx_ipc_write(lxfile_t *f, const void *src, uint64_t n, bool nonblock) {
    int64_t r;
    switch (f->type) {
    case FD_PIPE: r = pipe_write(f->obj, src, n, nonblock); break;
    case FD_SOCKET: {
        lxsock_t *s = f->obj;
        if (s->family == AF_INET_) r = inet_write(s, src, n, nonblock, 0, 0, false);
        else r = sock_send(s, src, n, nonblock, NULL, 0);
        break;
    }
    case FD_EVENTFD: {
        lxevfd_t *e = f->obj;
        uint64_t v;
        if (n != 8) return -EINVAL;
        memcpy(&v, src, 8);
        if (v == ~0ull) return -EINVAL;
        if (e->count + v < e->count) return -EAGAIN;
        e->count += v;
        return 8;
    }
    case FD_TIMERFD: return -EINVAL;
    default: return -ENOTTY;
    }
    if (r == -EPIPE) {                                   /* a pipe or socket with nobody on the other end */
        lxthread_t *t = lx_current();
        if (t) lx_signal_raise(t->proc, 13);             /* SIGPIPE */
    }
    return r;
}

void lx_ipc_release(lxfile_t *f) {
    if (!f || !f->obj) return;
    lxobj_t *o = f->obj;
    switch (f->type) {
    case FD_PIPE: {
        lxpipe_t *pp = f->obj;
        int acc = f->oflags & 3;
        if (acc == 0) { if (pp->readers > 0) pp->readers--; }
        else if (acc == 1) { if (pp->writers > 0) pp->writers--; }
        else { if (pp->readers > 0) pp->readers--; if (pp->writers > 0) pp->writers--; }
        if (--o->refs <= 0) { kfree(pp->ring); kfree(pp); }
        return;
    }
    case FD_SOCKET: {
        lxsock_t *s = f->obj;
        if (s->family == AF_INET_) {
            inet_unlink(s);
            if (s->type == SOCK_DGRAM_) {
                if (s->udp_bound) { udp_unbind(s->lport); s->udp_bound = false; }
            } else {
                if (s->listening) { tcp_unlisten(s->lport); s->listening = false; }
                if (s->tcb) { tcp_close(s->tcb); s->tcb = NULL; }
            }
            q_clear(s);
            if (--o->refs <= 0) kfree(s);
            return;
        }
        if (s->bound) { sock_unregister(s); s->bound = false; }
        if (s->peer && s->peer->peer == s) { s->peer->peer = NULL; s->peer->peer_gone = true; }
        q_clear(s);
        if (--o->refs <= 0) { kfree(s); }
        return;
    }
    default:
        if (--o->refs <= 0) kfree(o);
        return;
    }
}
void lx_ipc_wake_all(void) { }

/* ---------------- syscalls ---------------- */
typedef struct PACKED { uint64_t base, len; } iovec_t;
typedef struct PACKED {
    uint64_t name; uint32_t namelen, pad;
    uint64_t iov; uint64_t iovlen;
    uint64_t control; uint64_t controllen;
    int32_t flags;
} msghdr_t;
typedef struct PACKED { uint16_t family; char path[108]; } sun_t;
typedef struct PACKED { uint32_t events; uint64_t data; } epevent_t;

#define SOL_SOCKET_ 1
#define SCM_RIGHTS_ 1
#define MSG_PEEK_   2
#define MSG_DONTWAIT_ 0x40
#define MSG_TRUNC_  0x20
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* cmsg_len is a size_t; cmsgs are 8-byte aligned and start with len, level, type */
typedef struct PACKED { uint64_t len; int32_t level, type; } cmsghdr_t;
#define CMSG_ALIGN(n) (((n) + 7ull) & ~7ull)

static lxsock_t *sock_fd(lxproc_t *p, int fd) {
    lxfile_t *f = lx_fd_get(p, fd);
    return f && f->type == FD_SOCKET ? f->obj : NULL;
}

static int64_t do_socket(lxproc_t *p, int domain, int type, int proto) {
    int base = type & 0xF;
    if (domain == AF_UNIX_) {
        if (proto != 0) return -EPROTONOSUPPORT;
        if (base != SOCK_STREAM_ && base != SOCK_DGRAM_ && base != SOCK_SEQPACKET_) return -EPROTOTYPE;
        lxsock_t *s = sock_new(base);
        if (!s) return -ENOMEM;
        lxfile_t *f = wrap(FD_SOCKET, s);
        if (!f) { kfree(s); return -ENOMEM; }
        f->oflags = (type & SOCK_NONBLOCK) ? O_NONBLOCK_F : 0;
        return install(p, f, type);
    }
    if (domain == AF_INET_) {
        if (base != SOCK_STREAM_ && base != SOCK_DGRAM_) return -EPROTOTYPE;
        int want = base == SOCK_STREAM_ ? IPPROTO_TCP_ : IPPROTO_UDP_;
        if (proto != 0 && proto != want) return -EPROTONOSUPPORT;
        lxsock_t *s = inet_new(base);
        if (!s) return -ENOMEM;
        s->proto = proto ? proto : want;
        lxfile_t *f = wrap(FD_SOCKET, s);
        if (!f) { inet_unlink(s); kfree(s); return -ENOMEM; }
        f->oflags = (type & SOCK_NONBLOCK) ? O_NONBLOCK_F : 0;
        return install(p, f, type);
    }
    return -EAFNOSUPPORT;
}

static int64_t do_socketpair(lxproc_t *p, int domain, int type, int proto, uint64_t usv) {
    if (domain != AF_UNIX_) return -EAFNOSUPPORT;
    if (proto != 0) return -EPROTONOSUPPORT;
    int base = type & 0xF;
    if (base != SOCK_STREAM_ && base != SOCK_DGRAM_ && base != SOCK_SEQPACKET_) return -EPROTOTYPE;
    lxsock_t *a = sock_new(base), *b = sock_new(base);
    if (!a || !b) { kfree(a); kfree(b); return -ENOMEM; }
    a->peer = b; b->peer = a;
    a->connected = b->connected = true;
    lxfile_t *fa = wrap(FD_SOCKET, a), *fb = wrap(FD_SOCKET, b);
    if (!fa || !fb) {
        if (fa) lx_file_put(fa); else kfree(a);
        if (fb) lx_file_put(fb); else kfree(b);
        return -ENOMEM;
    }
    fa->oflags = fb->oflags = (type & SOCK_NONBLOCK) ? O_NONBLOCK_F : 0;
    int fd0 = install(p, fa, type), fd1 = install(p, fb, type);
    if (fd0 < 0 || fd1 < 0) {
        if (fd0 >= 0) { lx_file_put(p->fds[fd0]); p->fds[fd0] = NULL; }
        if (fd1 >= 0) { lx_file_put(p->fds[fd1]); p->fds[fd1] = NULL; }
        return -EMFILE;
    }
    int32_t sv[2] = { fd0, fd1 };
    if (!put(p, usv, sv, 8)) return -EFAULT;
    return 0;
}

static int sun_parse(lxproc_t *p, uint64_t uaddr, int len, char *path, int cap) {
    sun_t sa;
    if (len < 2 || len > (int)sizeof sa) return -EINVAL;
    memset(&sa, 0, sizeof sa);
    if (!get(p, &sa, uaddr, len)) return -EFAULT;
    if (sa.family != AF_UNIX_) return -EAFNOSUPPORT;
    sa.path[sizeof sa.path - 1] = 0;
    strlcpy(path, sa.path, cap);
    return 0;
}

static int64_t do_bind(lxproc_t *p, int fd, uint64_t uaddr, int len) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->bound) return -EINVAL;
    if (s->family == AF_INET_) {
        uint32_t ip = 0; uint16_t port = 0;
        int r = in_parse(p, uaddr, len, &ip, &port);
        if (r < 0) return r;
        if (ip && netif.ip && ip != netif.ip && ip != 0x7F000001) return -EADDRNOTAVAIL;
        if (!port) {
            for (int i = 0; i < 128 && !port; i++) {
                uint16_t cand = net_ephemeral_port();
                if (!inet_port_used(cand, s)) port = cand;
            }
            if (!port) return -EADDRINUSE;
        } else if (inet_port_used(port, s) && !s->reuseaddr) {
            return -EADDRINUSE;
        }
        if (s->type == SOCK_DGRAM_) {
            if (!udp_bind(port, inet_udp_input, s)) return -EADDRINUSE;
            s->udp_bound = true;
        }
        s->laddr = ip; s->lport = port; s->bound = true;
        return 0;
    }
    char path[108];
    int r = sun_parse(p, uaddr, len, path, sizeof path);
    if (r < 0) return r;
    if (!path[0]) return -EADDRINUSE;                          /* abstract names are not supported */
    if (sock_lookup(path)) return -EADDRINUSE;
    strlcpy(s->path, path, sizeof s->path);
    s->bound = true;
    s->reg_next = bound_list;
    bound_list = s;
    return 0;
}

static int64_t do_listen(lxproc_t *p, int fd, int backlog) {
    (void)backlog;
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->family == AF_INET_) {
        if (s->type != SOCK_STREAM_) return -EOPNOTSUPP;
        if (s->listening) return 0;
        if (!s->bound) {
            int r = inet_auto_bind(s);
            if (r < 0) return r;
        }
        if (tcp_listen(s->lport) != 0) return -EADDRINUSE;
        s->listening = true;
        return 0;
    }
    if (!s->bound) return -EINVAL;
    s->listening = true;
    return 0;
}

static int64_t do_connect(lxproc_t *p, int fd, uint64_t uaddr, int len) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    lxfile_t *self = lx_fd_get(p, fd);
    if (s->family == AF_INET_) {
        uint32_t ip = 0; uint16_t port = 0;
        int r = in_parse(p, uaddr, len, &ip, &port);
        if (r < 0) return r;
        if (!port) return -ECONNREFUSED;
        if (!ip) return -EADDRNOTAVAIL;
        if (s->type == SOCK_DGRAM_) { s->raddr = ip; s->rport = port; s->connected = true; return 0; }
        if (s->connected) return -EISCONN;
        if (s->tcb) return -EALREADY;
        bool nb = (self->oflags & O_NONBLOCK_F) != 0;
        if (!net_ready()) {
            if (!nb && !net_wait_ready(8000)) return -ENETUNREACH;
            if (nb) return -ENETUNREACH;
        }
        if (!s->bound) {
            r = inet_auto_bind(s);                 /* reserve the local port once */
            if (r < 0) return r;
        }
        tcb_t *t = tcp_open_from(ip, port, s->lport);
        if (!t) return -ENOMEM;
        s->tcb = t; s->raddr = ip; s->rport = port; s->so_error = 0;
        s->lport = tcp_local_port(t);
        if (nb) return -EINPROGRESS;
        uint32_t t0 = timer_ticks();
        while (tcp_status(t) == 0) {
            if (lx_block_interrupted(lx_current())) { tcp_close(t); s->tcb = NULL; return -EINTR; }
            if (timer_ticks() - t0 > 30000) { tcp_close(t); s->tcb = NULL; return -ETIMEDOUT; }
            task_sleep(2);
        }
        if (tcp_status(t) < 0) {
            int e = inet_map_error(tcp_error(t));
            tcp_close(t); s->tcb = NULL;
            return -e;
        }
        s->connected = true;
        s->lport = tcp_local_port(t);
        return 0;
    }
    if (s->connected) return -EISCONN;
    char path[108];
    int r = sun_parse(p, uaddr, len, path, sizeof path);
    if (r < 0) return r;
    lxsock_t *l = sock_lookup(path);
    if (!l || !l->listening) return -ECONNREFUSED;
    lxsock_t *srv = sock_new(s->type);
    if (!srv) return -ENOMEM;
    srv->connected = true; s->connected = true;
    srv->peer = s; s->peer = srv;
    srv->accept_next = NULL;
    if (!l->accept_head) l->accept_head = srv;
    else { lxsock_t *q = l->accept_head; while (q->accept_next) q = q->accept_next; q->accept_next = srv; }
    return 0;
}

static int64_t do_accept(lxproc_t *p, int fd, uint64_t uaddr, uint64_t ualen, int flags) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    lxfile_t *self = lx_fd_get(p, fd);
    bool nb = (flags & SOCK_NONBLOCK) || (self->oflags & O_NONBLOCK_F);
    for (;;) {
        if (s->family == AF_INET_) {
            if (!s->listening) return -EINVAL;
            tcb_t *c = tcp_accept(s->lport);
            if (c) {
                lxsock_t *ns = inet_new(SOCK_STREAM_);
                if (!ns) { tcp_close(c); return -ENOMEM; }
                ns->tcb = c; ns->connected = true;
                ns->laddr = s->laddr; ns->lport = s->lport;
                ns->raddr = tcp_remote_ip(c); ns->rport = tcp_remote_port(c);
                lxfile_t *f = wrap(FD_SOCKET, ns);
                if (!f) { inet_unlink(ns); kfree(ns); tcp_close(c); return -ENOMEM; }
                f->oflags = (flags & SOCK_NONBLOCK) ? O_NONBLOCK_F : 0;
                int nfd = install(p, f, flags);
                if (nfd < 0) return nfd;
                int rr = in_put(p, uaddr, ualen, ns->raddr, ns->rport);
                if (rr < 0) return rr;
                return nfd;
            }
        } else if (s->accept_head) {
            lxsock_t *c = s->accept_head;
            s->accept_head = c->accept_next;
            c->accept_next = NULL;
            lxfile_t *f = wrap(FD_SOCKET, c);
            if (!f) return -ENOMEM;
            f->oflags = (flags & SOCK_NONBLOCK) ? O_NONBLOCK_F : 0;
            int nfd = install(p, f, flags);
            if (nfd < 0) return nfd;
            if (uaddr && ualen) {
                sun_t sa; memset(&sa, 0, sizeof sa);
                sa.family = AF_UNIX_;
                if (c->peer) strlcpy(sa.path, c->peer->path, sizeof sa.path);
                int32_t l = sizeof sa;
                put(p, uaddr, &sa, sizeof sa);
                put(p, ualen, &l, 4);
            }
            return nfd;
        }
        if (!s->listening) return -EINVAL;
        if (nb) return -EAGAIN;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
}

static int64_t do_sendto(lxproc_t *p, int fd, uint64_t buf, uint64_t n, int flags, uint64_t uaddr, uint64_t ulen) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    lxfile_t *self = lx_fd_get(p, fd);
    bool nb = (flags & MSG_DONTWAIT_) || (self->oflags & O_NONBLOCK_F);
    if (s->type == SOCK_STREAM_) {
        if (n > SOCK_QLEN) n = SOCK_QLEN;       /* a stream write may be short */
    } else if (n > SOCK_QLEN) return -EMSGSIZE;
    uint8_t *tmp = kmalloc(n ? n : 1);
    if (!tmp) return -ENOMEM;
    if (n && !get(p, tmp, buf, n)) { kfree(tmp); return -EFAULT; }
    int64_t r;
    if (s->family == AF_INET_) {
        uint32_t dip = 0; uint16_t dport = 0;
        bool have = uaddr && ulen;
        if (have) {
            int e = in_parse(p, uaddr, (int)ulen, &dip, &dport);
            if (e < 0) { kfree(tmp); return e; }
        }
        r = inet_write(s, tmp, n, nb, dip, dport, have);
    } else r = sock_send(s, tmp, n, nb, NULL, 0);
    kfree(tmp);
    return r;
}

static int64_t do_recvfrom(lxproc_t *p, int fd, uint64_t buf, uint64_t n, int flags, uint64_t uaddr, uint64_t ualen) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    lxfile_t *self = lx_fd_get(p, fd);
    bool nb = (flags & MSG_DONTWAIT_) || (self->oflags & O_NONBLOCK_F);
    uint64_t want = MIN(n, (uint64_t)SOCK_QLEN);
    uint8_t *tmp = kmalloc(want ? want : 1);
    if (!tmp) return -ENOMEM;
    int64_t r;
    if (s->family == AF_INET_) {
        uint32_t sip = s->raddr, full = 0; uint16_t sport = s->rport;
        r = inet_read(s, tmp, want, nb, &sip, &sport, &full, (flags & MSG_PEEK_) != 0);
        if (r > 0 && !put(p, buf, tmp, r)) { kfree(tmp); return -EFAULT; }
        kfree(tmp);
        if (r >= 0 && uaddr && ualen) {
            int e = in_put(p, uaddr, ualen, sip, sport);
            if (e < 0) return e;
        }
        if ((flags & MSG_TRUNC_) && s->type == SOCK_DGRAM_ && r >= 0) return full;
        return r;
    }
    r = sock_recv(s, tmp, want, nb, (flags & MSG_PEEK_) != 0);
    if (r > 0 && !put(p, buf, tmp, r)) { kfree(tmp); return -EFAULT; }
    kfree(tmp);
    if (r >= 0 && uaddr && ualen) {
        sun_t sa; memset(&sa, 0, sizeof sa);
        sa.family = AF_UNIX_;
        if (s->peer) strlcpy(sa.path, s->peer->path, sizeof sa.path);
        int32_t l = sizeof sa;
        put(p, uaddr, &sa, sizeof sa);
        put(p, ualen, &l, 4);
    }
    return r;
}

static int64_t do_sendmsg(lxproc_t *p, int fd, uint64_t umsg, int flags) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    msghdr_t mh;
    if (!get(p, &mh, umsg, sizeof mh)) return -EFAULT;
    uint64_t total = 0;
    for (uint64_t i = 0; i < mh.iovlen && i < 1024; i++) {
        iovec_t v;
        if (!get(p, &v, mh.iov + i * 16, 16)) return -EFAULT;
        if (v.len > SOCK_QLEN || total + v.len > SOCK_QLEN) {
            if (s->type == SOCK_STREAM_) break;    /* a stream write may be short */
            return -EMSGSIZE;
        }
        total += v.len;
    }
    uint8_t *buf = kmalloc(total ? total : 1);
    if (!buf) return -ENOMEM;
    uint64_t o = 0;
    for (uint64_t i = 0; i < mh.iovlen && i < 1024 && o < total; i++) {
        iovec_t v;
        if (!get(p, &v, mh.iov + i * 16, 16)) { kfree(buf); return -EFAULT; }
        uint64_t k = MIN(v.len, total - o);
        if (k && !get(p, buf + o, v.base, k)) { kfree(buf); return -EFAULT; }
        o += k;
    }
    lxfile_t *fds[MAX_MSG_FD];
    int nfds = 0;
    if (mh.control && mh.controllen) {
        uint8_t ctl[256];
        uint64_t cl = MIN(mh.controllen, (uint64_t)sizeof ctl);
        if (!get(p, ctl, mh.control, cl)) { kfree(buf); return -EFAULT; }
        uint64_t off = 0;
        while (off + 16 <= cl) {
            cmsghdr_t *c = (cmsghdr_t *)(ctl + off);
            if (c->len < 16 || off + c->len > cl) break;
            if (c->level == SOL_SOCKET_ && c->type == SCM_RIGHTS_) {
                int cnt = (int)((c->len - 16) / 4);
                int32_t *fp = (int32_t *)(ctl + off + 16);
                for (int i = 0; i < cnt && nfds < MAX_MSG_FD; i++) {
                    lxfile_t *f = lx_fd_get(p, fp[i]);
                    if (!f) break;
                    f->refs++;
                    fds[nfds++] = f;
                }
            }
            off += CMSG_ALIGN(c->len);
        }
    }
    lxfile_t *self = lx_fd_get(p, fd);
    bool nb = (flags & MSG_DONTWAIT_) || (self->oflags & O_NONBLOCK_F);
    if (s->family == AF_INET_) {
        for (int i = 0; i < nfds; i++) lx_file_put(fds[i]);
        uint32_t dip = 0; uint16_t dport = 0;
        bool have = mh.name && mh.namelen;
        if (have) {
            int e = in_parse(p, mh.name, (int)mh.namelen, &dip, &dport);
            if (e < 0) { kfree(buf); return e; }
        }
        int64_t ir = inet_write(s, buf, total, nb, dip, dport, have);
        kfree(buf);
        return ir;
    }
    int64_t r = sock_send(s, buf, total, nb, fds, nfds);
    if (r < 0) for (int i = 0; i < nfds; i++) lx_file_put(fds[i]);
    kfree(buf);
    return r;
}

static int64_t do_recvmsg(lxproc_t *p, int fd, uint64_t umsg, int flags) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    msghdr_t mh;
    if (!get(p, &mh, umsg, sizeof mh)) return -EFAULT;
    lxfile_t *self = lx_fd_get(p, fd);
    bool nb = (flags & MSG_DONTWAIT_) || (self->oflags & O_NONBLOCK_F);
    if (s->family == AF_INET_) {
        uint64_t cap = 0;
        for (uint64_t i = 0; i < mh.iovlen && i < 1024; i++) {
            iovec_t v;
            if (!get(p, &v, mh.iov + i * 16, 16)) return -EFAULT;
            if (v.len > SOCK_QLEN || cap + v.len > SOCK_QLEN) {
                if (s->type == SOCK_STREAM_) break;
                return -EMSGSIZE;
            }
            cap += v.len;
        }
        uint8_t *buf = kmalloc(cap ? cap : 1);
        if (!buf) return -ENOMEM;
        uint32_t sip = s->raddr, full = 0; uint16_t sport = s->rport;
        int64_t r = inet_read(s, buf, cap, nb, &sip, &sport, &full,
                              (flags & MSG_PEEK_) != 0);
        if (r < 0) { kfree(buf); return r; }
        uint64_t copied = 0;
        for (uint64_t i = 0; i < mh.iovlen && i < 1024 && copied < (uint64_t)r; i++) {
            iovec_t v;
            if (!get(p, &v, mh.iov + i * 16, 16)) { kfree(buf); return -EFAULT; }
            uint32_t k = (uint32_t)MIN(v.len, (uint64_t)r - copied);
            if (k && !put(p, v.base, buf + copied, k)) { kfree(buf); return -EFAULT; }
            copied += k;
        }
        kfree(buf);
        mh.flags = (s->type == SOCK_DGRAM_ && (uint64_t)full > copied) ? MSG_TRUNC_ : 0;
        mh.controllen = 0;
        if (mh.name && mh.namelen) {
            sin_t sa; memset(&sa, 0, sizeof sa);
            sa.family = AF_INET_; sa.port = htons(sport); sa.addr = htonl(sip);
            uint64_t k = MIN((uint64_t)mh.namelen, (uint64_t)sizeof sa);
            if (!put(p, mh.name, &sa, k)) return -EFAULT;
            mh.namelen = sizeof sa;
        }
        if (!put(p, umsg, &mh, sizeof mh)) return -EFAULT;
        if ((flags & MSG_TRUNC_) && s->type == SOCK_DGRAM_) return full;
        return r;
    }
    for (;;) {
        if (s->qhead) break;
        if (s->peer_gone || !s->peer) { mh.flags = 0; put(p, umsg, &mh, sizeof mh); return 0; }
        if (nb) return -EAGAIN;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
    lxmsg_t *m = s->qhead;
    bool peek = (flags & MSG_PEEK_) != 0;
    uint32_t avail = m->len - m->off, copied = 0;
    for (uint64_t i = 0; i < mh.iovlen && i < 1024; i++) {
        iovec_t v;
        if (!get(p, &v, mh.iov + i * 16, 16)) return -EFAULT;
        if (copied >= avail) break;
        uint32_t k = (uint32_t)MIN(v.len, (uint64_t)(avail - copied));
        if (k && !put(p, v.base, m->data + m->off + copied, k)) return -EFAULT;
        copied += k;
    }
    mh.flags = copied < avail ? MSG_TRUNC_ : 0;
    if (mh.name && mh.namelen) {
        sun_t sa; memset(&sa, 0, sizeof sa);
        sa.family = AF_UNIX_;
        if (s->peer) strlcpy(sa.path, s->peer->path, sizeof sa.path);
        put(p, mh.name, &sa, MIN((uint64_t)mh.namelen, (uint64_t)sizeof sa));
    }
    if (m->nfds && mh.control && mh.controllen >= 24) {
        uint8_t ctl[64]; memset(ctl, 0, sizeof ctl);
        uint64_t need = 16 + (uint64_t)m->nfds * 4;
        cmsghdr_t *c = (cmsghdr_t *)ctl;
        c->len = need; c->level = SOL_SOCKET_; c->type = SCM_RIGHTS_;
        int32_t *fp = (int32_t *)(ctl + 16);
        int passed = 0;
        for (int i = 0; i < m->nfds; i++) {
            m->fds[i]->refs++;
            int nfd = lx_fd_alloc(p, m->fds[i], 0);
            if (nfd < 0) { m->fds[i]->refs--; break; }
            p->fdflags[nfd] = 1;
            fp[passed++] = nfd;
        }
        c->len = 16 + (uint64_t)passed * 4;
        put(p, mh.control, ctl, CMSG_ALIGN(c->len));
        mh.controllen = CMSG_ALIGN(c->len);
    } else if (m->nfds) {
        mh.controllen = 0;
    }
    put(p, umsg, &mh, sizeof mh);
    if (!peek) {
        m->off += copied;
        s->qlen -= copied;
        if (m->off >= m->len) { s->qhead = m->next; if (!s->qhead) s->qtail = NULL; msg_free(m); }
    }
    return (int64_t)copied;
}

static int64_t do_shutdown(lxproc_t *p, int fd, int how) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->family == AF_INET_) {
        if (s->type == SOCK_DGRAM_) {
            if (how != 0) s->connected = false;            /* stop using the default peer */
            return 0;
        }
        if (!s->tcb) return -ENOTCONN;
        if (how == 1 || how == 2) tcp_shutdown_wr(s->tcb); /* SHUT_WR, SHUT_RDWR */
        return 0;                                          /* discarding input is not modelled */
    }
    if (how == 1 || how == 2) s->peer_gone = true;                 /* SHUT_WR, SHUT_RDWR */
    if (how == 0 || how == 2) { if (s->peer) s->peer->peer_gone = true; }
    return 0;
}

static int64_t do_getname(lxproc_t *p, int fd, uint64_t uaddr, uint64_t ualen, bool peer) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->family == AF_INET_) {
        if (peer && !s->connected) return -ENOTCONN;
        return in_put(p, uaddr, ualen, peer ? s->raddr : s->laddr,
                      peer ? s->rport : s->lport);
    }
    if (!uaddr || !ualen) return 0;
    sun_t sa; memset(&sa, 0, sizeof sa);
    sa.family = AF_UNIX_;
    lxsock_t *src = peer ? s->peer : s;
    if (src) strlcpy(sa.path, src->path, sizeof sa.path);
    int32_t cap = 0;
    get(p, &cap, ualen, 4);
    int32_t l = sizeof sa;
    if (!put(p, uaddr, &sa, MIN((uint64_t)cap, (uint64_t)sizeof sa))) return -EFAULT;
    return put(p, ualen, &l, 4) ? 0 : -EFAULT;
}

static int64_t do_sockopt(lxproc_t *p, int fd, int level, int opt, uint64_t uval, uint64_t ulen, bool get_opt) {
    lxsock_t *s = sock_fd(p, fd);
    if (!s) return -ENOTSOCK;
    if (get_opt) {
        if (level == SOL_SOCKET_) {
            switch (opt) {
            case 2:  case 15: { int32_t v = s->reuseaddr; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            case 3:  { int32_t v = s->type; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            case 4:  { int32_t v = s->so_error; s->so_error = 0;
                       if (v && s->tcb && tcp_status(s->tcb) < 0) { tcp_close(s->tcb); s->tcb = NULL; }
                       return put(p, uval, &v, 4) ? 0 : -EFAULT; }   /* SO_ERROR clears on read */
            case 7:  case 8: { int32_t v = SOCK_QLEN; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            case 38: { int32_t v = s->proto; return put(p, uval, &v, 4) ? 0 : -EFAULT; }  /* SO_PROTOCOL */
            case 39: { int32_t v = s->family; return put(p, uval, &v, 4) ? 0 : -EFAULT; } /* SO_DOMAIN */
            case 18: case 19: { int32_t v = 1; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            case 20: case 21: {                       /* SO_RCVTIMEO, SO_SNDTIMEO: always no timeout */
                uint8_t tv[16]; memset(tv, 0, sizeof tv);
                return put(p, uval, tv, MIN(ulen, (uint64_t)sizeof tv)) ? 0 : -EFAULT;
            }
            case 9: case 13: { int32_t v = 0; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            default: return -ENOPROTOOPT;
            }
        }
        if (level == IPPROTO_TCP_) {
            if (opt == 1) { int32_t v = s->nodelay; return put(p, uval, &v, 4) ? 0 : -EFAULT; }
            return -ENOPROTOOPT;
        }
        return -ENOPROTOOPT;
    }
    int32_t v = 0;
    if (!uval || ulen < 4) return -EINVAL;
    if (!get(p, &v, uval, 4)) return -EFAULT;
    if (level == SOL_SOCKET_) {
        switch (opt) {
        case 2: case 15: s->reuseaddr = v != 0; return 0;   /* SO_REUSEADDR, SO_REUSEPORT */
        case 6: case 9: case 10: case 12: case 13: case 20: case 21: case 18: case 19:
            return 0;                                        /* accepted no-ops */
        default: return -ENOPROTOOPT;
        }
    }
    if (level == IPPROTO_TCP_) {
        if (opt == 1) { s->nodelay = v != 0; return 0; }     /* TCP_NODELAY */
        return -ENOPROTOOPT;
    }
    return -ENOPROTOOPT;
}

/* ---------------- pipes, eventfd, timerfd, epoll ---------------- */
static int64_t do_pipe(lxproc_t *p, uint64_t ufds, int flags) {
    lxpipe_t *pp = pipe_new();
    if (!pp) return -ENOMEM;
    lxfile_t *rf = wrap(FD_PIPE, pp), *wf = wrap(FD_PIPE, pp);
    if (!rf || !wf) {
        if (rf) lx_file_put(rf);
        if (wf) lx_file_put(wf);
        if (!rf && !wf) kfree(pp);
        return -ENOMEM;
    }
    rf->oflags = 0;
    wf->oflags = 1;
    pp->readers = 1;
    pp->writers = 1;
    int fd0 = install(p, rf, flags), fd1 = install(p, wf, flags);
    if (fd0 < 0 || fd1 < 0) {
        if (fd0 >= 0) { lx_file_put(p->fds[fd0]); p->fds[fd0] = NULL; }
        if (fd1 >= 0) { lx_file_put(p->fds[fd1]); p->fds[fd1] = NULL; }
        return -EMFILE;
    }
    int32_t fds[2] = { fd0, fd1 };
    if (!put(p, ufds, fds, 8)) return -EFAULT;
    return 0;
}

static int64_t do_eventfd(lxproc_t *p, uint32_t init, int flags) {
    lxevfd_t *e = kzalloc(sizeof *e);
    if (!e) return -ENOMEM;
    e->count = init;
    e->sem = (flags & EFD_SEMAPHORE) != 0;
    lxfile_t *f = wrap(FD_EVENTFD, e);
    if (!f) { kfree(e); return -ENOMEM; }
    f->oflags = (flags & EFD_NONBLOCK) ? O_NONBLOCK_F : 0;
    return install(p, f, flags);
}

static int64_t do_timerfd_create(lxproc_t *p, int clockid, int flags) {
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    lxtfd_t *t = kzalloc(sizeof *t);
    if (!t) return -ENOMEM;
    lxfile_t *f = wrap(FD_TIMERFD, t);
    if (!f) { kfree(t); return -ENOMEM; }
    f->oflags = (flags & TFD_NONBLOCK) ? O_NONBLOCK_F : 0;
    return install(p, f, flags);
}

static int64_t do_timerfd_settime(lxproc_t *p, int fd, int flags, uint64_t unew, uint64_t uold) {
    if (fd < 0 || fd >= LX_MAXFD || !p->fds[fd] || p->fds[fd]->type != FD_TIMERFD) return -EINVAL;
    lxtfd_t *t = p->fds[fd]->obj;
    uint64_t iv[4];
    if (!get(p, iv, unew, sizeof iv)) return -EFAULT;
    if (uold) { uint64_t zero[4] = { 0, 0, 0, 0 }; if (!put(p, uold, zero, sizeof zero)) return -EFAULT; }
    uint64_t iv_ms = iv[0] * 1000 + iv[1] / 1000000;                /* interval */
    uint64_t vl_ms = iv[2] * 1000 + iv[3] / 1000000;                /* value */
    if (!iv[2] && !iv[3]) { t->armed = false; t->interval = 0; return 0; }
    t->interval = (uint32_t)MIN(iv_ms, 0x7FFFFFFFull);
    t->next = (flags & 1) ? (uint32_t)MIN(vl_ms, 0xFFFFFFFFull) : timer_ticks() + (uint32_t)MIN(vl_ms, 0x7FFFFFFFull);
    t->armed = true;
    return 0;
}

static int64_t do_timerfd_gettime(lxproc_t *p, int fd, uint64_t ucur) {
    if (fd < 0 || fd >= LX_MAXFD || !p->fds[fd] || p->fds[fd]->type != FD_TIMERFD) return -EINVAL;
    lxtfd_t *t = p->fds[fd]->obj;
    uint64_t cur[4] = { 0, 0, 0, 0 };
    cur[0] = t->interval / 1000; cur[1] = (uint64_t)(t->interval % 1000) * 1000000;
    if (t->armed) {
        uint32_t now = timer_ticks();
        uint32_t left = (int32_t)(t->next - now) > 0 ? t->next - now : 0;
        cur[2] = left / 1000; cur[3] = (uint64_t)(left % 1000) * 1000000;
    }
    return put(p, ucur, cur, sizeof cur) ? 0 : -EFAULT;
}

static lxepoll_t *epoll_fd(lxproc_t *p, int fd) {
    lxfile_t *f = lx_fd_get(p, fd);
    return f && f->type == FD_EPOLL ? f->obj : NULL;
}

static int64_t do_epoll_create(lxproc_t *p, int flags) {
    (void)flags;
    lxepoll_t *e = kzalloc(sizeof *e);
    if (!e) return -ENOMEM;
    lxfile_t *f = wrap(FD_EPOLL, e);
    if (!f) { kfree(e); return -ENOMEM; }
    return install(p, f, flags);
}

static int64_t do_epoll_ctl(lxproc_t *p, int epfd, int op, int fd, uint64_t uev) {
    lxepoll_t *e = epoll_fd(p, epfd);
    if (!e) return -EBADF;
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL) return -EINVAL;
    if (!lx_fd_get(p, fd)) return -EBADF;
    epevent_t ev; memset(&ev, 0, sizeof ev);
    if (op != EPOLL_CTL_DEL && !get(p, &ev, uev, sizeof ev)) return -EFAULT;
    int i;
    for (i = 0; i < e->n; i++) if (e->items[i].fd == fd) break;
    if (op == EPOLL_CTL_ADD) {
        if (i < e->n) return -EEXIST;
        if (e->n >= 64) return -ENOSPC;
        e->items[e->n].fd = fd; e->items[e->n].events = ev.events; e->items[e->n].data = ev.data; e->n++;
        return 0;
    }
    if (i >= e->n) return -ENOENT;
    if (op == EPOLL_CTL_DEL) { e->items[i] = e->items[--e->n]; return 0; }
    e->items[i].events = ev.events; e->items[i].data = ev.data;
    return 0;
}

static int64_t do_epoll_wait(lxproc_t *p, int epfd, uint64_t uevents, int maxevents, int timeout) {
    lxepoll_t *e = epoll_fd(p, epfd);
    if (!e) return -EBADF;
    if (maxevents <= 0) return -EINVAL;
    uint32_t deadline = timer_ticks() + (timeout > 0 ? (uint32_t)timeout : 0);
    for (;;) {
        int n = 0;
        for (int i = 0; i < e->n && n < maxevents; i++) {
            lxfile_t *f = lx_fd_get(p, e->items[i].fd);
            uint32_t got = 0;
            if (!f) got = POLLNVAL_;
            else {
                if (lx_ipc_ready(f, POLLIN_)) got |= POLLIN_;
                if (lx_ipc_ready(f, POLLOUT_)) got |= POLLOUT_;
                if (lx_ipc_ready(f, POLLHUP_)) got |= POLLHUP_;
                else if (f->type == FD_PIPE && ((lxpipe_t *)f->obj)->writers == 0) got |= POLLHUP_;
                else if (f->type == FD_SOCKET && ((lxsock_t *)f->obj)->peer_gone) got |= POLLHUP_;
            }
            if (!got) continue;
            if (!(got & e->items[i].events) && !(got & (POLLERR_ | POLLHUP_ | POLLNVAL_))) continue;
            epevent_t out; out.events = got; out.data = e->items[i].data;
            if (!put(p, uevents + (uint64_t)n * sizeof out, &out, sizeof out)) return -EFAULT;
            n++;
        }
        if (n) return n;
        if (timeout == 0) return 0;
        if (timeout > 0 && (int32_t)(timer_ticks() - deadline) >= 0) return 0;
        if (lx_block_interrupted(lx_current())) return -EINTR;
        task_sleep(1);
    }
}

/* ---------------- dispatch ---------------- */
int64_t lx_sys_ipc(lxthread_t *t, int nr, regs_t *r, bool *handled) {
    lxproc_t *p = t->proc;
    uint64_t a0 = r->edi, a1 = r->esi, a2 = r->edx, a3 = r->r10, a4 = r->r8, a5 = r->r9;
    *handled = true;
    switch (nr) {
    case 22: return do_pipe(p, a0, 0);                                  /* pipe */
    case 293: return do_pipe(p, a0, (int)a1);                           /* pipe2 */
    case 41: return do_socket(p, (int)a0, (int)a1, (int)a2);            /* socket */
    case 53: return do_socketpair(p, (int)a0, (int)a1, (int)a2, a3);    /* socketpair */
    case 49: return do_bind(p, (int)a0, a1, (int)a2);                   /* bind */
    case 50: return do_listen(p, (int)a0, (int)a1);                     /* listen */
    case 42: return do_connect(p, (int)a0, a1, (int)a2);                /* connect */
    case 43: return do_accept(p, (int)a0, a1, a2, 0);                   /* accept */
    case 288: return do_accept(p, (int)a0, a1, a2, (int)a3);            /* accept4 */
    case 44: return do_sendto(p, (int)a0, a1, a2, (int)a3, a4, a5);      /* sendto */
    case 45: return do_recvfrom(p, (int)a0, a1, a2, (int)a3, a4, a5);   /* recvfrom */
    case 46: return do_sendmsg(p, (int)a0, a1, (int)a2);                /* sendmsg */
    case 47: return do_recvmsg(p, (int)a0, a1, (int)a2);                /* recvmsg */
    case 48: return do_shutdown(p, (int)a0, (int)a1);                   /* shutdown */
    case 51: return do_getname(p, (int)a0, a1, a2, false);              /* getsockname */
    case 52: return do_getname(p, (int)a0, a1, a2, true);               /* getpeername */
    case 54: return do_sockopt(p, (int)a0, (int)a1, (int)a2, a3, a4, false);   /* setsockopt */
    case 55: return do_sockopt(p, (int)a0, (int)a1, (int)a2, a3, a4, true);    /* getsockopt */
    case 290: return do_eventfd(p, (uint32_t)a0, (int)a1);              /* eventfd2 */
    case 284: return do_eventfd(p, (uint32_t)a0, 0);                    /* eventfd */
    case 283: return do_timerfd_create(p, (int)a0, (int)a1);            /* timerfd_create */
    case 286: return do_timerfd_settime(p, (int)a0, (int)a1, a2, a3);   /* timerfd_settime */
    case 287: return do_timerfd_gettime(p, (int)a0, a1);                /* timerfd_gettime */
    case 291: return do_epoll_create(p, (int)a0);                       /* epoll_create1 */
    case 233: return do_epoll_ctl(p, (int)a0, (int)a1, (int)a2, a3);    /* epoll_ctl */
    case 232: return do_epoll_wait(p, (int)a0, a1, (int)a2, (int)a3);   /* epoll_wait */
    case 281: return do_epoll_wait(p, (int)a0, a1, (int)a2, (int)a3);   /* epoll_pwait */
    }
    *handled = false;
    return -ENOSYS;
}
#endif
