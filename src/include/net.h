#ifndef NET_H
#define NET_H
#include "kernel.h"

/* IPv4 addresses are kept in host byte order: 10.0.2.15 = 0x0A00020F */
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
static inline uint16_t htons(uint16_t v) { return (v >> 8) | (v << 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
void ip_to_str(uint32_t ip, char *buf);                  /* buf >= 16 */
bool str_to_ip(const char *s, uint32_t *ip);

/* ---- packet parsers (rust/src/net.rs) ----
 * Each checks a received packet and returns false (or -1) for one to drop. Offsets are into
 * the bytes given. All fields are 32-bit so that C and Rust lay the structs out alike. */
typedef struct { uint32_t op, spa, tpa; uint8_t sha[8]; } net_arp_t;
typedef struct { uint32_t src, dst, proto, off, len; } net_ipv4_t;        /* payload: off..off+len */
typedef struct { uint32_t sport, dport, off, len; } net_udp_t;
typedef struct { uint32_t sport, dport, seq, ack, flags, win, mss, hl; } net_tcp_t;   /* mss 0: no option */
typedef struct { uint32_t kind, yiaddr, mask, gw, dns, server, lease; } net_dhcp_t;  /* 0: option absent */
bool net_parse_arp(const uint8_t *p, uint32_t len, net_arp_t *out);
bool net_parse_ipv4(const uint8_t *p, uint32_t len, net_ipv4_t *out);   /* also checks the header checksum */
bool net_parse_udp(const uint8_t *p, uint32_t len, net_udp_t *out);
bool net_parse_tcp(const uint8_t *p, uint32_t len, net_tcp_t *out);
bool net_parse_dhcp(const uint8_t *m, uint32_t len, uint32_t xid, net_dhcp_t *out);
/* a DNS reply to query id: 1 with *ip its first A record, 0 with *rcode for none, -1 not ours */
int net_parse_dns(const uint8_t *m, uint32_t len, uint16_t id, uint32_t *ip, int *rcode);

/* ---- network interface ---- */
typedef struct {
    bool present, link, configured;
    char driver[48];
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns, dhcp_server;
    uint32_t lease_s;
    uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
} netif_t;
extern netif_t netif;

/* NIC driver interface */
typedef struct {
    const char *name;
    bool (*send)(const void *frame, uint32_t len);
    void (*poll)(void);                                   /* deliver received frames to net_rx() */
    bool (*link_up)(void);
} nic_ops_t;
bool e1000_probe(nic_ops_t *ops);       /* Intel PRO/1000, 82574 (VMware e1000e), I217/I218/I219 */
bool igb_probe(nic_ops_t *ops);         /* Intel 82575/82576/I210/I211/I350 */
bool virtionet_probe(nic_ops_t *ops);   /* virtio-net (QEMU/KVM, Proxmox, VirtualBox) */
bool vmxnet3_probe(nic_ops_t *ops);     /* VMware VMXNET3 */
bool pcnet_probe(nic_ops_t *ops);       /* AMD PCnet (VMware vlance, VirtualBox PCnet) */
bool rtl8169_probe(nic_ops_t *ops);     /* Realtek RTL8111/8168/8169/810x/8125 */
bool rtl8139_probe(nic_ops_t *ops);     /* Realtek RTL8139 */
void net_rx(const uint8_t *frame, uint32_t len);

void net_init(void);                                      /* probe NIC, start the net task and DHCP */
bool net_ready(void);                                     /* link up and address configured */
bool net_wait_ready(uint32_t timeout_ms);
void dhcp_renew(void);
const char *net_status_text(void);                        /* one line for the UI */

/* ---- IP / ICMP / UDP ---- */
enum { IPPROTO_ICMP = 1, IPPROTO_TCP = 6, IPPROTO_UDP = 17 };
bool ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len);
/* inbound firewall */
extern bool fw_enabled, fw_block_tcp, fw_block_udp;
extern uint32_t fw_blocked;
bool fw_port_allowed(uint16_t p);
bool fw_allow_port(uint16_t p);
void fw_clear_ports(void);
int fw_port_count(void);
uint16_t fw_port_at(int i);
bool fw_remove_port(uint16_t p);
typedef struct { uint32_t when, src; uint16_t port; uint8_t proto; } fw_event_t;
int fw_log_count(void);
const fw_event_t *fw_log_at(int i);             /* 0 = the most recent blocked attempt */
uint16_t ip_checksum(const void *data, uint32_t len, uint32_t initial);
int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ms);   /* round-trip ms, or -1 */
typedef void (*udp_handler_t)(uint32_t src, uint16_t sport, const uint8_t *data, uint32_t len, void *ctx);
bool udp_bind(uint16_t port, udp_handler_t h, void *ctx);
void udp_unbind(uint16_t port);
bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len);
uint16_t net_ephemeral_port(void);

/* ---- DNS ---- */
bool dns_resolve(const char *name, uint32_t *ip, char *err, int errcap);

/* ---- TCP ---- */
typedef struct tcb tcb_t;
tcb_t *tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms, char *err, int errcap);
int tcp_send(tcb_t *t, const void *data, uint32_t len);             /* bytes queued, -1 on error */
int tcp_recv(tcb_t *t, void *buf, uint32_t max, uint32_t timeout_ms); /* >0 bytes, 0 = closed by peer, -1 error, -2 timeout */
int tcp_peek(tcb_t *t, void *buf, uint32_t max);            /* same without consuming; -2 when empty */
void tcp_shutdown_wr(tcb_t *t);                                     /* send our FIN; reads still drain */
void tcp_close(tcb_t *t);                                            /* graceful close; t must not be used afterwards */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint32_t len);
void tcp_timers(void);
int tcp_count(void);

/* TCP for the Linux socket layer: non-blocking active opens and passive ones.
 * Addresses and ports are in host order, as everywhere else in this stack. */
tcb_t *tcp_open(uint32_t ip, uint16_t port);              /* begin an active open; NULL if no network/memory */
tcb_t *tcp_open_from(uint32_t ip, uint16_t port, uint16_t lport);   /* same, binding lport (0 = ephemeral) */
int tcp_status(tcb_t *t);                                 /* 1 established, 0 in progress, -1 failed/closed */
int tcp_error(tcb_t *t);                                  /* 1 refused, 2 reset, 3 timed out, 0 none */
bool tcp_recv_ready(tcb_t *t);
bool tcp_send_ready(tcb_t *t);
bool tcp_hup(tcb_t *t);
int tcp_send_nb(tcb_t *t, const void *data, uint32_t len);/* queue what fits; 0 when full, -1 error */
uint16_t tcp_local_port(tcb_t *t);
uint16_t tcp_remote_port(tcb_t *t);
uint32_t tcp_remote_ip(tcb_t *t);
int tcp_listen(uint16_t port);                            /* 0, or -1 when the port is in use */
void tcp_unlisten(uint16_t port);
bool tcp_accept_ready(uint16_t port);                      /* a completed connection waits on the listener */
tcb_t *tcp_accept(uint16_t port);                         /* an established connection, or NULL */

/* ---- TLS: Mbed TLS (src/tls/tls.c), TLS 1.3 and 1.2 ---- */
typedef struct tls_conn tls_conn_t;
tls_conn_t *tls_open(tcb_t *t, const char *host, uint32_t ip, uint16_t port, char *err, int errcap);   /* TLS 1.3, else 1.2 (may reconnect) */
int tls_write(tls_conn_t *c, const void *data, uint32_t len);       /* -1 on error */
int tls_read(tls_conn_t *c, void *buf, uint32_t max);                /* >0, 0 closed, -1 error */
void tls_close(tls_conn_t *c);                                       /* also closes the TCP connection */
const char *tls_error_text(tls_conn_t *c);
const char *tls_cipher_name(tls_conn_t *c);
int tls_trust_anchor_count(void);
const char *tls_library_version(void);                               /* "Mbed TLS 4.1.0" */
void entropy_add(uint32_t v);
void sha256_hex(const void *data, uint32_t len, char out[65]);                                        /* timing noise from IRQs */

/* ---- HTTP(S) client ---- */
typedef struct {
    int status;                 /* HTTP status, or 0 if the request failed */
    char url[1024];             /* final URL after redirects */
    char content_type[96];
    char charset[24];
    char location[1024];
    uint8_t *body; uint32_t body_len;
    char error[200];
    bool secure;
    char tls_info[80];
} http_response_t;
typedef void (*http_progress_t)(void *ctx, const char *status, uint32_t got, uint32_t total);
bool http_get(const char *url, http_response_t *r, http_progress_t cb, void *ctx);
bool http_request(const char *method, const char *url, const char *content_type, const void *body, uint32_t body_len,
                  http_response_t *r, http_progress_t cb, void *ctx);
void http_free(http_response_t *r);
bool url_resolve(const char *base, const char *rel, char *out, int cap);    /* relative -> absolute */
bool url_parse(const char *url, char *scheme, char *host, int hostcap, uint16_t *port, char *path, int pathcap);
#endif
