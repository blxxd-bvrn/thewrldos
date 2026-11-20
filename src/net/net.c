/* TCP/IP stack core: Ethernet, ARP, IPv4, ICMP, UDP, DHCP client, DNS
 * resolver and the network task that polls the card and runs timers. */
#include "kernel.h"
#include "net.h"
#include "fs.h"

netif_t netif;
static nic_ops_t nic;
static task_t *net_task;
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ip_to_str(uint32_t ip, char *b) { snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }
/* str_to_ip, ip_checksum and the packet parsers (net_parse_*) are in rust/src/net.rs */
/* the parser structs' sizes, for rust_init's layout check */
const uint32_t rust_net_layout[5] = { sizeof(net_arp_t), sizeof(net_ipv4_t), sizeof(net_udp_t), sizeof(net_tcp_t), sizeof(net_dhcp_t) };
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* ---------------- Ethernet ---------------- */
static bool eth_send(const uint8_t *dst, uint16_t type, const void *payload, uint32_t len) {
    static uint8_t f[1600];
    if (!nic.send || len > 1500) return false;
    memcpy(f, dst, 6); memcpy(f + 6, netif.mac, 6); put16(f + 12, type);
    memcpy(f + 14, payload, len);
    netif.tx_packets++; netif.tx_bytes += len + 14;
    return nic.send(f, len + 14);
}

/* ---------------- ARP ---------------- */
typedef struct { uint32_t ip; uint8_t mac[6]; uint32_t time; } arp_entry_t;
static arp_entry_t arp_cache[32];
static void arp_store(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    int slot = 0; uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < 32; i++) {
        if (arp_cache[i].ip == ip) { slot = i; break; }
        if (arp_cache[i].time < oldest) { oldest = arp_cache[i].time; slot = i; }
    }
    arp_cache[slot].ip = ip; memcpy(arp_cache[slot].mac, mac, 6); arp_cache[slot].time = timer_ticks() | 1;
}
static bool arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 32; i++) if (arp_cache[i].ip == ip && arp_cache[i].time) { memcpy(mac, arp_cache[i].mac, 6); return true; }
    return false;
}
static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip) {
    uint8_t a[28];
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, op);
    memcpy(a + 8, netif.mac, 6); put32(a + 14, netif.ip);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : tmac, 6); put32(a + 24, tip);
    eth_send(op == 1 ? BCAST : tmac, 0x0806, a, 28);
}
static void arp_input(const uint8_t *a, uint32_t len) {
    net_arp_t r;
    if (!net_parse_arp(a, len, &r)) return;
    arp_store(r.spa, r.sha);
    if (r.op == 1 && netif.ip && r.tpa == netif.ip) arp_send(2, r.sha, r.spa);
}
static bool in_net_task(void) { return task_current() == net_task; }
static bool arp_resolve(uint32_t ip, uint8_t *mac) {
    if (arp_lookup(ip, mac)) return true;
    if (in_net_task()) { arp_send(1, NULL, ip); return false; }     /* can't block here: drop, TCP will retransmit */
    for (int tries = 0; tries < 4; tries++) {
        arp_send(1, NULL, ip);
        for (int t = 0; t < 250; t += 5) { task_sleep(5); if (arp_lookup(ip, mac)) return true; }
    }
    return false;
}

/* ---------------- IPv4 ---------------- */
static uint16_t ip_id;
bool ip_send(uint32_t dst, uint8_t proto, const void *payload, uint32_t len) {
    static uint8_t pkt[1500];
    if (len > 1480) return false;
    uint8_t mac[6];
    bool bcast = dst == 0xFFFFFFFF || (netif.mask && (dst | netif.mask) == 0xFFFFFFFF);
    if (bcast) memcpy(mac, BCAST, 6);
    else {
        uint32_t hop = (netif.mask && (dst & netif.mask) != (netif.ip & netif.mask) && netif.gw) ? netif.gw : dst;
        if (!arp_resolve(hop, mac)) return false;
    }
    uint8_t *h = pkt;
    h[0] = 0x45; h[1] = 0; put16(h + 2, 20 + len); put16(h + 4, ip_id++); put16(h + 6, 0x4000);
    h[8] = 64; h[9] = proto; put16(h + 10, 0); put32(h + 12, netif.ip); put32(h + 16, dst);
    put16(h + 10, ip_checksum(h, 20, 0));
    memcpy(pkt + 20, payload, len);
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

/* ---------------- ICMP ---------------- */
static volatile int ping_seq_seen = -1;
static void icmp_input(uint32_t src, const uint8_t *p, uint32_t len) {
    if (len < 8) return;
    if (p[0] == 8) {                       /* echo request -> reply */
        static uint8_t r[1480];
        if (len > sizeof r) return;
        memcpy(r, p, len); r[0] = 0; put16(r + 2, 0); put16(r + 2, ip_checksum(r, len, 0));
        ip_send(src, IPPROTO_ICMP, r, len);
    } else if (p[0] == 0 && get16(p + 4) == 0x5752) ping_seq_seen = get16(p + 6);
}
int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ms) {
    uint8_t p[40];
    p[0] = 8; p[1] = 0; put16(p + 2, 0); put16(p + 4, 0x5752); put16(p + 6, seq);
    for (int i = 8; i < 40; i++) p[i] = 'a' + i % 26;
    put16(p + 2, ip_checksum(p, 40, 0));
    ping_seq_seen = -1;
    uint32_t t0 = timer_ticks();
    if (!ip_send(dst, IPPROTO_ICMP, p, 40)) return -1;
    while (timer_ticks() - t0 < timeout_ms) {
        if (ping_seq_seen == seq) return timer_ticks() - t0;
        task_sleep(1);
    }
    return -1;
}

/* ---------------- UDP ---------------- */
typedef struct { uint16_t port; udp_handler_t h; void *ctx; } udp_sock_t;
static udp_sock_t udps[16];
static uint16_t next_port;
uint16_t net_ephemeral_port(void) { if (!next_port) next_port = 49152 + rand_next() % 8000; return next_port = next_port >= 65000 ? 49152 : next_port + 1; }
bool udp_bind(uint16_t port, udp_handler_t h, void *ctx) {
    for (int i = 0; i < 16; i++) if (udps[i].port == port && port) return false;   /* one owner per port */
    for (int i = 0; i < 16; i++) if (!udps[i].port) { udps[i] = (udp_sock_t){ port, h, ctx }; return true; }
    return false;
}
void udp_unbind(uint16_t port) { for (int i = 0; i < 16; i++) if (udps[i].port == port) udps[i].port = 0; }
bool udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    static uint8_t u[1480];
    if (len + 8 > sizeof u) return false;
    put16(u, sport); put16(u + 2, dport); put16(u + 4, 8 + len); put16(u + 6, 0);
    memcpy(u + 8, data, len);
    uint8_t ph[12]; put32(ph, netif.ip); put32(ph + 4, dst); ph[8] = 0; ph[9] = IPPROTO_UDP; put16(ph + 10, 8 + len);
    uint32_t s = 0; for (int i = 0; i < 12; i += 2) s += get16(ph + i);
    uint16_t ck = ip_checksum(u, 8 + len, s); put16(u + 6, ck ? ck : 0xFFFF);
    return ip_send(dst, IPPROTO_UDP, u, 8 + len);
}
static void udp_input(uint32_t src, const uint8_t *p, uint32_t len) {
    net_udp_t u;
    if (!net_parse_udp(p, len, &u)) return;
    for (int i = 0; i < 16; i++) if (udps[i].port == u.dport && udps[i].h) { udps[i].h(src, u.sport, p + u.off, u.len, udps[i].ctx); return; }
}
static bool udp_is_bound(uint16_t port) { for (int i = 0; i < 16; i++) if (udps[i].port == port) return true; return false; }

/* ---- simple inbound firewall ----
 * Outbound traffic is always allowed.  The filter blocks unsolicited new TCP
 * connections and UDP datagrams to ports nothing has bound.  Replies that
 * belong to a connection (TCP ACK) and bound UDP sockets pass. */
bool fw_enabled, fw_block_tcp = true, fw_block_udp = true;
uint32_t fw_blocked;
static uint16_t fw_ports[16]; static int fw_nports;
bool fw_port_allowed(uint16_t p) { for (int i = 0; i < fw_nports; i++) if (fw_ports[i] == p) return true; return false; }
bool fw_allow_port(uint16_t p) { if (fw_port_allowed(p)) return true; if (fw_nports >= 16) return false; fw_ports[fw_nports++] = p; return true; }
void fw_clear_ports(void) { fw_nports = 0; }
int fw_port_count(void) { return fw_nports; }
uint16_t fw_port_at(int i) { return i >= 0 && i < fw_nports ? fw_ports[i] : 0; }
bool fw_remove_port(uint16_t p) {
    for (int i = 0; i < fw_nports; i++) if (fw_ports[i] == p) { fw_ports[i] = fw_ports[--fw_nports]; return true; }
    return false;
}
/* the last blocked attempts, newest first, for Security Center */
#define FW_LOG 32
static fw_event_t fw_log[FW_LOG]; static int fw_log_n, fw_log_head;
static void fw_note(uint32_t src, uint16_t port, uint8_t proto) {
    fw_blocked++;
    fw_event_t *e = &fw_log[fw_log_head]; fw_log_head = (fw_log_head + 1) % FW_LOG;
    if (fw_log_n < FW_LOG) fw_log_n++;
    e->when = timer_ticks(); e->src = src; e->port = port; e->proto = proto;
}
int fw_log_count(void) { return fw_log_n; }
const fw_event_t *fw_log_at(int i) { return i >= 0 && i < fw_log_n ? &fw_log[(fw_log_head - 1 - i + FW_LOG) % FW_LOG] : NULL; }

/* ---------------- frame dispatch ---------------- */
static void ip_input(const uint8_t *p, uint32_t len) {
    net_ipv4_t ip;
    if (!net_parse_ipv4(p, len, &ip)) return;                /* bad header, bad checksum, or a fragment */
    uint32_t src = ip.src, dst = ip.dst, hl = ip.off, tot = ip.off + ip.len;
    bool mine = dst == netif.ip || dst == 0xFFFFFFFF || (netif.mask && (dst | netif.mask) == 0xFFFFFFFF) || !netif.ip;
    if (!mine) return;
    if (fw_enabled) {
        net_tcp_t t; net_udp_t u;
        if (ip.proto == IPPROTO_TCP && fw_block_tcp && net_parse_tcp(p + hl, ip.len, &t)) {
            /* a pure SYN is a new inbound connection; ACK/SYN-ACK belongs to
             * an existing connection and is allowed (a malformed segment: tcp_input drops it) */
            if ((t.flags & 0x02) && !(t.flags & 0x10) && !fw_port_allowed(t.dport)) { fw_note(src, t.dport, IPPROTO_TCP); return; }
        } else if (ip.proto == IPPROTO_UDP && fw_block_udp && net_parse_udp(p + hl, ip.len, &u)) {
            if (!udp_is_bound(u.dport) && !fw_port_allowed(u.dport) && u.dport != 68) { fw_note(src, u.dport, IPPROTO_UDP); return; }
        }
    }
    switch (ip.proto) {
    case IPPROTO_ICMP: icmp_input(src, p + hl, tot - hl); break;
    case IPPROTO_UDP: udp_input(src, p + hl, tot - hl); break;
    case IPPROTO_TCP: tcp_input(src, dst, p + hl, tot - hl); break;
    }
}
void net_rx(const uint8_t *f, uint32_t len) {
    if (len < 14) return;
    netif.rx_packets++; netif.rx_bytes += len;
    uint16_t type = get16(f + 12);
    if (type == 0x0806) arp_input(f + 14, len - 14);
    else if (type == 0x0800) {
        /* learn the sender's MAC for on-link hosts (saves ARP round trips) */
        if (len >= 34 && netif.mask) { uint32_t s = get32(f + 26); if ((s & netif.mask) == (netif.ip & netif.mask)) arp_store(s, f + 6); }
        ip_input(f + 14, len - 14);
    }
}

/* ---------------- DHCP ---------------- */
enum { DHCP_IDLE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };
static int dhcp_state;
static uint32_t dhcp_xid, dhcp_offer, dhcp_server, dhcp_next, dhcp_tries, dhcp_bound_at;
static void dhcp_send(int type) {
    uint8_t m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6; put32(m + 4, dhcp_xid); put16(m + 10, 0x8000);
    memcpy(m + 28, netif.mac, 6);
    put32(m + 236, 0x63825363);
    int o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = type;
    m[o++] = 61; m[o++] = 7; m[o++] = 1; memcpy(m + o, netif.mac, 6); o += 6;
    int hl = MIN((int)strlen(computer_name), 32); m[o++] = 12; m[o++] = hl; memcpy(m + o, computer_name, hl); o += hl;
    if (type == 3) { m[o++] = 50; m[o++] = 4; put32(m + o, dhcp_offer); o += 4; m[o++] = 54; m[o++] = 4; put32(m + o, dhcp_server); o += 4; }
    m[o++] = 55; m[o++] = 4; m[o++] = 1; m[o++] = 3; m[o++] = 6; m[o++] = 51;
    m[o++] = 255;
    uint32_t save = netif.ip; netif.ip = 0;
    udp_send(0xFFFFFFFF, 68, 67, m, MAX(o, 300));
    netif.ip = save;
}
static void dhcp_input(uint32_t src, uint16_t sport, const uint8_t *m, uint32_t len, void *ctx) {
    net_dhcp_t r;
    if (!net_parse_dhcp(m, len, dhcp_xid, &r)) return;
    int type = r.kind; uint32_t mask = r.mask, gw = r.gw, dns = r.dns, server = r.server, lease = r.lease;
    if (type == 2 && dhcp_state == DHCP_DISCOVER) {           /* OFFER */
        dhcp_offer = r.yiaddr; dhcp_server = server ? server : src;
        dhcp_state = DHCP_REQUEST; dhcp_tries = 0; dhcp_next = 0;
    } else if (type == 5 && dhcp_state == DHCP_REQUEST) {    /* ACK */
        netif.ip = r.yiaddr; netif.mask = mask ? mask : IP4(255, 255, 255, 0); netif.gw = gw; netif.dns = dns ? dns : gw;
        netif.dhcp_server = dhcp_server; netif.lease_s = lease;
        netif.configured = true; dhcp_state = DHCP_BOUND; dhcp_bound_at = timer_ticks();
        char a[16], g[16], d[16]; ip_to_str(netif.ip, a); ip_to_str(netif.gw, g); ip_to_str(netif.dns, d);
        klog("dhcp: address %s, gateway %s, DNS %s, lease %us", a, g, d, lease);
    } else if (type == 6 && dhcp_state == DHCP_REQUEST) {    /* NAK */
        dhcp_state = DHCP_DISCOVER; dhcp_tries = 0; dhcp_next = timer_ticks() + 1000;
    }
}
void dhcp_renew(void) {
    if (!netif.present) return;
    netif.configured = false; netif.ip = 0;
    dhcp_xid = rand_next(); dhcp_state = DHCP_DISCOVER; dhcp_tries = 0; dhcp_next = 0;
}
static void dhcp_tick(void) {
    uint32_t now = timer_ticks();
    if (dhcp_state == DHCP_BOUND) {
        if (netif.lease_s && netif.lease_s < 0x7FFFFFFF && now - dhcp_bound_at > netif.lease_s * 500u) { dhcp_state = DHCP_REQUEST; dhcp_offer = netif.ip; dhcp_tries = 0; dhcp_next = 0; }
        return;
    }
    if (dhcp_state == DHCP_IDLE || (int32_t)(now - dhcp_next) < 0) return;
    if (dhcp_state == DHCP_REQUEST && dhcp_tries >= 4) { dhcp_state = DHCP_DISCOVER; dhcp_tries = 0; }
    dhcp_send(dhcp_state == DHCP_DISCOVER ? 1 : 3);
    dhcp_tries++;
    dhcp_next = now + MIN(16000u, 1000u << MIN(dhcp_tries, 4u));
}

/* ---------------- DNS ---------------- */
typedef struct { char name[128]; uint32_t ip, expires; } dns_cache_t;
static dns_cache_t dcache[32];
typedef struct { uint16_t id; uint32_t ip; bool done, failed; int rcode; } dns_q_t;
static void dns_input(uint32_t src, uint16_t sport, const uint8_t *m, uint32_t len, void *ctx) {
    dns_q_t *q = ctx;
    uint32_t ip; int rcode = 0;
    int r = net_parse_dns(m, len, q->id, &ip, &rcode);
    if (r < 0) return;
    q->rcode = rcode;
    if (r > 0) { q->ip = ip; q->done = true; } else q->failed = true;
}
static bool hosts_lookup(const char *name, uint32_t *ip) {
    uint32_t len;
    char *t = (char *)vfs_read_file("C:\\WRLD\\hosts", &len);
    if (!t) return false;
    bool found = false;
    /* no strtok: the name loop inside would clobber the line loop's position */
    for (char *line = t, *next; line && *line && !found; line = next) {
        next = line; while (*next && *next != '\r' && *next != '\n') next++;
        if (*next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; } else next = NULL;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        char a[32]; int k = 0;
        while (*line && *line != ' ' && *line != '\t' && k < 31) a[k++] = *line++;
        a[k] = 0;
        while (*line && !found) {
            while (*line == ' ' || *line == '\t') line++;
            char *n = line;
            while (*line && *line != ' ' && *line != '\t' && *line != '#') line++;
            int nl = line - n;
            if (*line == '#') *line = 0;
            if (nl && (int)strlen(name) == nl && !strncasecmp(n, name, nl) && str_to_ip(a, ip)) found = true;
        }
    }
    kfree(t);
    return found;
}
bool dns_resolve(const char *name, uint32_t *ip, char *err, int errcap) {
    if (str_to_ip(name, ip)) return true;
    if (!strcasecmp(name, "localhost")) { *ip = IP4(127, 0, 0, 1); return true; }
    if (hosts_lookup(name, ip)) return true;
    for (int i = 0; i < 32; i++) if (dcache[i].ip && !strcasecmp(dcache[i].name, name) && (int32_t)(dcache[i].expires - timer_ticks()) > 0) { *ip = dcache[i].ip; return true; }
    if (!net_wait_ready(8000)) { snprintf(err, errcap, "The network is not connected."); return false; }
    if (!netif.dns) { snprintf(err, errcap, "No DNS server is configured."); return false; }
    uint8_t m[300]; int o = 12;
    dns_q_t q = { (uint16_t)rand_next(), 0, false, false, 0 };
    memset(m, 0, 12); put16(m, q.id); m[2] = 1; put16(m + 4, 1);
    const char *p = name;
    while (*p && o < 270) {
        const char *dot = strchr(p, '.'); int l = dot ? dot - p : (int)strlen(p);
        if (l <= 0 || l > 63) break;
        m[o++] = l; memcpy(m + o, p, l); o += l;
        p += l; if (*p == '.') p++;
    }
    m[o++] = 0; put16(m + o, 1); put16(m + o + 2, 1); o += 4;
    uint16_t port = net_ephemeral_port();
    udp_bind(port, dns_input, &q);
    for (int attempt = 0; attempt < 3 && !q.done && !q.failed; attempt++) {
        udp_send(netif.dns, port, 53, m, o);
        for (int t = 0; t < 1500 && !q.done && !q.failed; t += 5) task_sleep(5);
    }
    udp_unbind(port);
    if (!q.done) {
        if (q.rcode == 3) snprintf(err, errcap, "The server name \"%s\" could not be found.", name);
        else snprintf(err, errcap, "The DNS server did not answer for \"%s\".", name);
        return false;
    }
    int slot = rand_next() % 32;
    strlcpy(dcache[slot].name, name, sizeof dcache[slot].name); dcache[slot].ip = q.ip; dcache[slot].expires = timer_ticks() + 300000;
    *ip = q.ip;
    return true;
}

/* ---------------- network task ---------------- */
static void net_loop(void *arg) {
    bool was_up = false;
    for (;;) {
        if (nic.poll) nic.poll();
        bool up = nic.link_up && nic.link_up();
        if (up != was_up) {
            netif.link = up; was_up = up;
            klog("net: link %s", up ? "up" : "down");
            if (up) dhcp_renew(); else { netif.configured = false; }
        }
        dhcp_tick();
        tcp_timers();
        task_sleep(tcp_count() ? 1 : 3);
    }
}
void net_init(void) {
    memset(&netif, 0, sizeof netif);
    /* first match wins: paravirtual cards first, then the emulated/real ones */
    static bool (*const probes[])(nic_ops_t *) = { virtionet_probe, vmxnet3_probe, e1000_probe, igb_probe, rtl8169_probe, rtl8139_probe, pcnet_probe };
    bool found = false;
    for (unsigned i = 0; i < ARRAY_LEN(probes) && !found; i++) { memset(&nic, 0, sizeof nic); found = probes[i](&nic); }
    if (!found) {
        pci_dev_t *devs; int n = pci_list(&devs);
        for (int i = 0; i < n; i++) if (devs[i].cls == 0x02) klog("net: no driver for network adapter %04x:%04x", devs[i].vendor, devs[i].device);
        klog("net: no supported network adapter found"); strcpy(netif.driver, "No supported adapter"); return;
    }
    netif.present = true;
    udp_bind(68, dhcp_input, NULL);
    net_task = task_create("netd", net_loop, NULL, 32768);
}
bool net_ready(void) { return netif.present && netif.link && netif.configured; }
bool net_wait_ready(uint32_t ms) {
    uint32_t t0 = timer_ticks();
    while (!net_ready()) { if (!netif.present || timer_ticks() - t0 > ms) return false; task_sleep(20); }
    return true;
}
const char *net_status_text(void) {
    static char b[96];
    if (!netif.present) return "No network adapter";
    if (!netif.link) return "Network cable unplugged";
    if (!netif.configured) return "Acquiring network address...";
    char a[16]; ip_to_str(netif.ip, a);
    snprintf(b, sizeof b, "Connected - %s", a);
    return b;
}
