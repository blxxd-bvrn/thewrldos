/* TCP (client side): active open with MSS, in-order receive with a 64 KiB
 * window, go-back-N retransmission with exponential back-off, graceful and
 * abortive close. Segments are processed on the network task; applications
 * block in tcp_recv()/tcp_connect() by sleeping, which lets other tasks run. */
#include "kernel.h"
#include "net.h"

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, LAST_ACK, TIME_WAIT, SYN_RCVD };
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
#define RCAP (64 * 1024)
#define SCAP (64 * 1024)

struct tcb {
    int state;
    uint32_t rip; uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t rcv_nxt;
    uint16_t mss;
    uint8_t *rbuf; uint32_t rlen;          /* received, not yet read by the application */
    uint8_t *sbuf; uint32_t slen;          /* bytes from snd_una on: sent-unacked + unsent */
    bool fin_pending, fin_sent, fin_rcvd, app_closed;
    uint32_t rto, rtx_at; int retries;
    uint32_t last_wnd_adv;
    uint32_t dead_at;
    int err;                               /* 0, or reason the connection failed */
    struct tcb *next;
    struct tcb *accept_next;               /* queued on a listener, waiting for accept() */
};
static tcb_t *conns;

/* Passive opens: a listening port holds connections that finished the
 * handshake until an application accepts them. */
typedef struct tcp_listener {
    struct tcp_listener *next;
    uint16_t port;
    tcb_t *head, *tail;
} tcp_listener_t;
static tcp_listener_t *listeners;

static tcp_listener_t *find_listener(uint16_t port) {
    for (tcp_listener_t *l = listeners; l; l = l->next) if (l->port == port) return l;
    return NULL;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
int tcp_count(void) { int n = 0; for (tcb_t *t = conns; t; t = t->next) n++; return n; }

static uint32_t rwnd(tcb_t *t) { uint32_t w = RCAP - t->rlen; return w > 65535 ? 65535 : w; }
static bool send_seg(tcb_t *t, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len) {
    uint8_t s[1480];
    int hl = (flags & F_SYN) ? 24 : 20;
    if (hl + len > sizeof s) return false;
    put16(s, t->lport); put16(s + 2, t->rport); put32(s + 4, seq); put32(s + 8, (flags & F_ACK) ? t->rcv_nxt : 0);
    s[12] = (hl / 4) << 4; s[13] = flags; put16(s + 14, rwnd(t)); put16(s + 16, 0); put16(s + 18, 0);
    if (flags & F_SYN) { s[20] = 2; s[21] = 4; put16(s + 22, 1460); }
    if (len) memcpy(s + hl, data, len);
    uint8_t ph[12]; put32(ph, netif.ip); put32(ph + 4, t->rip); ph[8] = 0; ph[9] = IPPROTO_TCP; put16(ph + 10, hl + len);
    uint32_t sum = 0; for (int i = 0; i < 12; i += 2) sum += get16(ph + i);
    put16(s + 16, ip_checksum(s, hl + len, sum));
    if (flags & F_ACK) t->last_wnd_adv = rwnd(t);
    return ip_send(t->rip, IPPROTO_TCP, s, hl + len);
}
static void send_ack(tcb_t *t) { send_seg(t, t->snd_nxt, F_ACK, NULL, 0); }
static void arm_timer(tcb_t *t) { t->rtx_at = timer_ticks() + t->rto; }

/* send whatever the peer's window allows */
static void output(tcb_t *t) {
    if (t->state != ESTABLISHED && t->state != CLOSE_WAIT && t->state != FIN_WAIT_1 && t->state != LAST_ACK) return;
    uint32_t wnd = t->snd_wnd ? t->snd_wnd : 1;
    for (;;) {
        uint32_t inflight = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);
        if (inflight >= t->slen || inflight >= wnd) break;
        uint32_t n = MIN(MIN((uint32_t)t->mss, t->slen - inflight), wnd - inflight);
        if (!send_seg(t, t->snd_nxt, F_ACK | F_PSH, t->sbuf + inflight, n)) break;
        t->snd_nxt += n;
        if (!t->rtx_at) arm_timer(t);
    }
    uint32_t inflight = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);
    if (t->fin_pending && !t->fin_sent && inflight >= t->slen) {
        send_seg(t, t->snd_nxt, F_FIN | F_ACK, NULL, 0);
        t->snd_nxt++; t->fin_sent = true;
        t->state = t->state == CLOSE_WAIT ? LAST_ACK : FIN_WAIT_1;
        if (!t->rtx_at) arm_timer(t);
    }
}
/* remove a tcb from any listener's accept queue; true if it was queued */
static bool accept_remove(tcb_t *t) {
    for (tcp_listener_t *l = listeners; l; l = l->next) {
        if (l->head == t) {
            l->head = t->accept_next;
            if (!l->head) l->tail = NULL;
            t->accept_next = NULL;
            return true;
        }
        for (tcb_t *q = l->head; q && q->accept_next; q = q->accept_next)
            if (q->accept_next == t) {
                q->accept_next = t->accept_next;
                if (l->tail == t) l->tail = q;
                t->accept_next = NULL;
                return true;
            }
    }
    return false;
}
static void destroy(tcb_t *t) {
    accept_remove(t);
    for (tcb_t **pp = &conns; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    kfree(t->rbuf); kfree(t->sbuf); kfree(t);
}
static void fail(tcb_t *t, int err) {
    t->err = err; t->state = CLOSED; t->rtx_at = 0;
    /* A connection that failed before anyone accepted it has no owner; free it
     * now.  One the application holds stays until it closes the socket, so it
     * can still report the error through getsockopt(). */
    if (accept_remove(t)) { destroy(t); return; }
    if (t->app_closed) t->dead_at = timer_ticks();
}

/* ---- entry points for the Linux socket layer ---- */
static tcb_t *conn_new(uint32_t ip, uint16_t port, int state) {
    tcb_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->rbuf = kmalloc(RCAP); t->sbuf = kmalloc(SCAP);
    if (!t->rbuf || !t->sbuf) { kfree(t->rbuf); kfree(t->sbuf); kfree(t); return NULL; }
    t->rip = ip; t->rport = port; t->lport = net_ephemeral_port();
    t->iss = rand_next() ^ (uint32_t)rdtsc();
    t->snd_una = t->iss; t->snd_nxt = t->iss + 1; t->mss = 536; t->rto = 1000;
    t->state = state;
    t->next = conns; conns = t;
    return t;
}
tcb_t *tcp_open_from(uint32_t ip, uint16_t port, uint16_t lport) {
    if (!net_ready()) return NULL;
    tcb_t *t = conn_new(ip, port, SYN_SENT);
    if (!t) return NULL;
    if (lport) t->lport = lport;             /* 0 means pick an ephemeral port */
    send_seg(t, t->iss, F_SYN, NULL, 0);
    arm_timer(t);
    return t;
}
tcb_t *tcp_open(uint32_t ip, uint16_t port) { return tcp_open_from(ip, port, 0); }
int tcp_status(tcb_t *t) {
    if (!t) return -1;
    switch (t->state) {
    case ESTABLISHED: case CLOSE_WAIT: case FIN_WAIT_1: case FIN_WAIT_2: case LAST_ACK: return 1;
    case SYN_SENT: case SYN_RCVD: return 0;
    default: return -1;
    }
}
int tcp_error(tcb_t *t) { return t ? t->err : 0; }
bool tcp_recv_ready(tcb_t *t) { return t && (t->rlen > 0 || t->fin_rcvd || t->state == CLOSED); }
bool tcp_send_ready(tcb_t *t) {
    if (!t) return false;
    if (t->state == CLOSED) return true;            /* poll wakes the program so it sees the error */
    if (t->state != ESTABLISHED && t->state != CLOSE_WAIT) return false;
    return t->slen < SCAP;
}
bool tcp_hup(tcb_t *t) { return t && (t->fin_rcvd || t->state == CLOSED); }
int tcp_send_nb(tcb_t *t, const void *data, uint32_t len) {
    if (!t) return -1;
    if (t->state != ESTABLISHED && t->state != CLOSE_WAIT) return -1;
    uint32_t space = SCAP - t->slen;
    uint32_t n = MIN(space, len);
    if (n) { memcpy(t->sbuf + t->slen, data, n); t->slen += n; output(t); }
    return (int)n;
}
uint16_t tcp_local_port(tcb_t *t) { return t ? t->lport : 0; }
uint16_t tcp_remote_port(tcb_t *t) { return t ? t->rport : 0; }
uint32_t tcp_remote_ip(tcb_t *t) { return t ? t->rip : 0; }

int tcp_listen(uint16_t port) {
    if (!port || find_listener(port)) return -1;
    tcp_listener_t *l = kzalloc(sizeof *l);
    if (!l) return -1;
    l->port = port;
    l->next = listeners;
    listeners = l;
    return 0;
}
void tcp_unlisten(uint16_t port) {
    tcp_listener_t **pp = &listeners;
    while (*pp && (*pp)->port != port) pp = &(*pp)->next;
    tcp_listener_t *l = *pp;
    if (!l) return;
    *pp = l->next;
    for (tcb_t *t = l->head; t; ) {
        tcb_t *nx = t->accept_next;
        t->accept_next = NULL;
        t->state = CLOSED; t->rtx_at = 0;
        if (!t->app_closed) { t->app_closed = true; t->dead_at = timer_ticks(); }
        t = nx;
    }
    kfree(l);
}
bool tcp_accept_ready(uint16_t port) {
    tcp_listener_t *l = find_listener(port);
    for (tcb_t *t = l ? l->head : NULL; t; t = t->accept_next)
        if (t->state == ESTABLISHED || t->state == CLOSE_WAIT ||
            t->state == FIN_WAIT_1 || t->state == FIN_WAIT_2) return true;
    return false;
}
tcb_t *tcp_accept(uint16_t port) {
    tcp_listener_t *l = find_listener(port);
    while (l && l->head) {
        tcb_t *t = l->head;
        l->head = t->accept_next;
        if (!l->head) l->tail = NULL;
        t->accept_next = NULL;
        if (t->state == ESTABLISHED || t->state == CLOSE_WAIT ||
            t->state == FIN_WAIT_1 || t->state == FIN_WAIT_2)
            return t;
        /* reset before the program got to it: let the timer task free it */
        if (!t->app_closed) { t->app_closed = true; t->dead_at = timer_ticks(); }
    }
    return NULL;
}

tcb_t *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms, char *err, int errcap) {
    if (!net_wait_ready(8000)) { snprintf(err, errcap, "The network is not connected."); return NULL; }
    tcb_t *t = conn_new(ip, port, SYN_SENT);
    if (!t) { snprintf(err, errcap, "Not enough memory for a connection."); return NULL; }
    send_seg(t, t->iss, F_SYN, NULL, 0);
    arm_timer(t);
    uint32_t t0 = timer_ticks();
    while (t->state == SYN_SENT && timer_ticks() - t0 < timeout_ms) task_sleep(2);
    if (t->state != ESTABLISHED) {
        char a[16]; ip_to_str(ip, a);
        if (t->err == 1) snprintf(err, errcap, "The connection to %s port %u was refused.", a, port);
        else snprintf(err, errcap, "The connection to %s port %u timed out.", a, port);
        if (t->state == SYN_SENT) send_seg(t, t->snd_nxt, F_RST, NULL, 0);
        destroy(t);
        return NULL;
    }
    return t;
}
int tcp_send(tcb_t *t, const void *data, uint32_t len) {
    const uint8_t *p = data; uint32_t done = 0;
    while (done < len) {
        if (t->state != ESTABLISHED && t->state != CLOSE_WAIT) return done ? (int)done : -1;
        uint32_t space = SCAP - t->slen;
        if (!space) { task_sleep(2); continue; }
        uint32_t n = MIN(space, len - done);
        memcpy(t->sbuf + t->slen, p + done, n); t->slen += n; done += n;
        output(t);
    }
    return done;
}
int tcp_recv(tcb_t *t, void *buf, uint32_t max, uint32_t timeout_ms) {
    uint32_t t0 = timer_ticks();
    while (!t->rlen) {
        if (t->fin_rcvd) return 0;
        if (t->state == CLOSED) return t->err ? -1 : 0;
        if (timer_ticks() - t0 > timeout_ms) return -2;
        task_sleep(1);
    }
    uint32_t n = MIN(max, t->rlen);
    memcpy(buf, t->rbuf, n);
    memmove(t->rbuf, t->rbuf + n, t->rlen - n);
    t->rlen -= n;
    /* window update once a useful amount of space has opened */
    if (t->state != CLOSED && rwnd(t) >= t->last_wnd_adv + 8192) send_ack(t);
    return n;
}
void tcp_shutdown_wr(tcb_t *t) {
    if (!t || t->fin_sent || t->fin_pending) return;
    if (t->state != ESTABLISHED && t->state != CLOSE_WAIT) return;
    t->fin_pending = true;                  /* output() sends the FIN; reads still drain */
    output(t);
}
int tcp_peek(tcb_t *t, void *buf, uint32_t max) {
    if (!t) return -1;
    if (!t->rlen) return (t->fin_rcvd || t->state == CLOSED) ? 0 : -2;
    uint32_t n = MIN(max, t->rlen);
    memcpy(buf, t->rbuf, n);
    return (int)n;
}
void tcp_close(tcb_t *t) {
    if (!t) return;
    t->app_closed = true;
    accept_remove(t);
    if (t->state == ESTABLISHED || t->state == CLOSE_WAIT) { t->fin_pending = true; output(t); t->dead_at = timer_ticks() + 10000; }
    else if (t->state == SYN_SENT || t->state == SYN_RCVD) {
        /* never completed: reset instead of retransmitting to the grave */
        send_seg(t, t->snd_nxt, F_RST, NULL, 0);
        t->state = CLOSED; t->rtx_at = 0; t->dead_at = timer_ticks();
    }
    else if (t->state == CLOSED) t->dead_at = timer_ticks();
    else t->dead_at = timer_ticks() + 5000;
}

static void rst_reply(uint32_t src, const uint8_t *s, uint32_t len) {
    tcb_t tmp = { 0 };
    tmp.rip = src; tmp.lport = get16(s + 2); tmp.rport = get16(s);
    uint8_t fl = s[13];
    uint32_t hl = (s[12] >> 4) * 4;
    if (fl & F_ACK) send_seg(&tmp, get32(s + 8), F_RST, NULL, 0);
    else { tmp.rcv_nxt = get32(s + 4) + (len - hl) + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0); send_seg(&tmp, 0, F_RST | F_ACK, NULL, 0); }
}

/* A SYN arrived for a listening port: create the connection in SYN_RCVD and
 * answer with SYN-ACK.  The third handshake segment moves it to the listener's
 * accept queue. */
static tcb_t *accept_syn(tcp_listener_t *l, uint32_t src, const net_tcp_t *h) {
    tcb_t *t = conn_new(src, h->sport, SYN_RCVD);
    if (!t) return NULL;
    t->lport = l->port;
    t->rcv_nxt = h->seq + 1;
    t->snd_wnd = h->win;
    if (h->mss) t->mss = MIN(1460, h->mss);
    send_seg(t, t->iss, F_SYN | F_ACK, NULL, 0);
    arm_timer(t);
    return t;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *s, uint32_t len) {
    net_tcp_t h;
    if (!net_parse_tcp(s, len, &h)) return;                  /* rust/src/net.rs */
    uint32_t hl = h.hl;
    uint16_t sport = h.sport, dport = h.dport;
    uint32_t seq = h.seq, ack = h.ack;
    uint8_t fl = h.flags;
    uint32_t win = h.win;
    const uint8_t *data = s + hl; uint32_t dlen = len - hl;
    tcb_t *t = conns;
    while (t && !(t->rip == src && t->rport == sport && t->lport == dport)) t = t->next;
    if (!t || t->state == CLOSED) {
        if ((fl & F_SYN) && !(fl & F_ACK)) {
            tcp_listener_t *l = find_listener(dport);
            if (l) { if (accept_syn(l, src, &h)) return; }
        }
        if (!(fl & F_RST)) rst_reply(src, s, len);
        return;
    }

    if (t->state == SYN_SENT) {
        if ((fl & F_ACK) && ack != t->iss + 1) { if (!(fl & F_RST)) rst_reply(src, s, len); return; }
        if (fl & F_RST) { if (fl & F_ACK) fail(t, 1); return; }
        if (!(fl & F_SYN)) return;
        t->rcv_nxt = seq + 1;
        if (h.mss) t->mss = MIN(1460, h.mss);
        if (fl & F_ACK) {
            t->snd_una = ack; t->snd_wnd = win; t->state = ESTABLISHED; t->rtx_at = 0; t->retries = 0;
            send_ack(t);
        }
        return;
    }
    if (fl & F_RST) { if (seq_le(t->rcv_nxt, seq) && seq_lt(seq, t->rcv_nxt + RCAP)) fail(t, 2); return; }
    if (t->state == SYN_RCVD) {
        if (fl & F_SYN) { send_seg(t, t->iss, F_SYN | F_ACK, NULL, 0); arm_timer(t); return; }   /* SYN-ACK lost */
        if (!(fl & F_ACK) || ack != t->iss + 1) return;
        t->snd_una = ack; t->snd_wnd = win; t->retries = 0; t->rto = 1000; t->rtx_at = 0;
        t->state = ESTABLISHED;
        tcp_listener_t *l = find_listener(t->lport);
        if (!l) { fail(t, 1); return; }
        t->accept_next = NULL;
        if (l->tail) l->tail->accept_next = t; else l->head = t;
        l->tail = t;
        /* the ACK may carry the first data segment: fall through */
    } else if (fl & F_SYN) { send_ack(t); return; }         /* retransmitted SYN-ACK */

    /* acknowledgements */
    if (fl & F_ACK) {
        if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_nxt)) {
            uint32_t acked = ack - t->snd_una;
            bool fin_acked = t->fin_sent && ack == t->snd_nxt;
            uint32_t data_acked = MIN(acked - (fin_acked ? 1 : 0), t->slen);
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
            t->snd_una = ack;
            t->retries = 0; t->rto = 1000;
            t->rtx_at = t->snd_una == t->snd_nxt ? 0 : timer_ticks() + t->rto;
            if (fin_acked) {
                if (t->state == FIN_WAIT_1) t->state = FIN_WAIT_2;
                else if (t->state == LAST_ACK) { t->state = CLOSED; t->dead_at = timer_ticks(); }
            }
        }
        t->snd_wnd = win;
    }
    /* data */
    if (dlen) {
        if (seq == t->rcv_nxt && !t->fin_rcvd) {
            uint32_t n = MIN(dlen, RCAP - t->rlen);
            memcpy(t->rbuf + t->rlen, data, n); t->rlen += n; t->rcv_nxt += n;
            if (n < dlen) fl &= ~F_FIN;                       /* didn't take it all: FIN comes later */
        } else fl &= ~F_FIN;
        send_ack(t);
    }
    if ((fl & F_FIN) && seq + dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++; t->fin_rcvd = true;
        send_ack(t);
        if (t->state == ESTABLISHED) t->state = CLOSE_WAIT;
        else if (t->state == FIN_WAIT_1 || t->state == FIN_WAIT_2) { t->state = TIME_WAIT; t->dead_at = timer_ticks() + 1000; }
    }
    output(t);
}
void tcp_timers(void) {
    uint32_t now = timer_ticks();
    for (tcb_t *t = conns, *nx; t; t = nx) {
        nx = t->next;
        if (t->dead_at && t->app_closed && (int32_t)(now - t->dead_at) >= 0) { destroy(t); continue; }
        if (!t->rtx_at || (int32_t)(now - t->rtx_at) < 0) continue;
        if (++t->retries > 8) { fail(t, 3); continue; }
        t->rto = MIN(t->rto * 2, 16000u);
        if (t->state == SYN_SENT) { send_seg(t, t->iss, F_SYN, NULL, 0); arm_timer(t); continue; }
        if (t->state == SYN_RCVD) { send_seg(t, t->iss, F_SYN | F_ACK, NULL, 0); arm_timer(t); continue; }
        /* go back to the oldest unacknowledged byte */
        t->snd_nxt = t->snd_una;
        bool fin = t->fin_sent; t->fin_sent = false;
        if (fin && t->state == FIN_WAIT_1) t->state = ESTABLISHED;
        if (fin && t->state == LAST_ACK) t->state = CLOSE_WAIT;
        t->rtx_at = 0;
        if (!t->slen && !fin && t->snd_wnd == 0) { send_ack(t); arm_timer(t); continue; }
        output(t);
        if (!t->rtx_at) arm_timer(t);
    }
}
