/* TCP (client side): active open with MSS, in-order receive with a 64 KiB
 * window, go-back-N retransmission with exponential back-off, graceful and
 * abortive close. Segments are processed on the network task; applications
 * block in tcp_recv()/tcp_connect() by sleeping, which lets other tasks run. */
#include "kernel.h"
#include "net.h"

enum { CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, LAST_ACK, TIME_WAIT };
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
};
static tcb_t *conns;

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
static void destroy(tcb_t *t) {
    for (tcb_t **pp = &conns; *pp; pp = &(*pp)->next) if (*pp == t) { *pp = t->next; break; }
    kfree(t->rbuf); kfree(t->sbuf); kfree(t);
}
static void fail(tcb_t *t, int err) { t->err = err; t->state = CLOSED; t->rtx_at = 0; if (t->app_closed) t->dead_at = timer_ticks(); }

tcb_t *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms, char *err, int errcap) {
    if (!net_wait_ready(8000)) { snprintf(err, errcap, "The network is not connected."); return NULL; }
    tcb_t *t = kzalloc(sizeof *t);
    t->rbuf = kmalloc(RCAP); t->sbuf = kmalloc(SCAP);
    t->rip = ip; t->rport = port; t->lport = net_ephemeral_port();
    t->iss = rand_next() ^ (uint32_t)rdtsc();
    t->snd_una = t->iss; t->snd_nxt = t->iss + 1; t->mss = 536; t->rto = 1000;
    t->state = SYN_SENT;
    t->next = conns; conns = t;
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
void tcp_close(tcb_t *t) {
    if (!t) return;
    t->app_closed = true;
    if (t->state == ESTABLISHED || t->state == CLOSE_WAIT) { t->fin_pending = true; output(t); t->dead_at = timer_ticks() + 10000; }
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
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *s, uint32_t len) {
    if (len < 20) return;
    uint32_t hl = (s[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    uint16_t sport = get16(s), dport = get16(s + 2);
    uint32_t seq = get32(s + 4), ack = get32(s + 8);
    uint8_t fl = s[13];
    uint32_t win = get16(s + 14);
    const uint8_t *data = s + hl; uint32_t dlen = len - hl;
    tcb_t *t = conns;
    while (t && !(t->rip == src && t->rport == sport && t->lport == dport)) t = t->next;
    if (!t || t->state == CLOSED) { if (!(fl & F_RST)) rst_reply(src, s, len); return; }

    if (t->state == SYN_SENT) {
        if ((fl & F_ACK) && ack != t->iss + 1) { if (!(fl & F_RST)) rst_reply(src, s, len); return; }
        if (fl & F_RST) { if (fl & F_ACK) fail(t, 1); return; }
        if (!(fl & F_SYN)) return;
        t->rcv_nxt = seq + 1;
        for (uint32_t o = 20; o + 1 < hl;) {                 /* options: MSS */
            uint8_t k = s[o];
            if (k == 0) break;
            if (k == 1) { o++; continue; }
            if (k == 2 && s[o + 1] == 4 && o + 4 <= hl) t->mss = MIN(1460, get16(s + o + 2));
            if (s[o + 1] < 2) break;
            o += s[o + 1];
        }
        if (fl & F_ACK) {
            t->snd_una = ack; t->snd_wnd = win; t->state = ESTABLISHED; t->rtx_at = 0; t->retries = 0;
            send_ack(t);
        }
        return;
    }
    if (fl & F_RST) { if (seq_le(t->rcv_nxt, seq) && seq_lt(seq, t->rcv_nxt + RCAP)) fail(t, 2); return; }
    if (fl & F_SYN) { send_ack(t); return; }                 /* retransmitted SYN-ACK */

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
