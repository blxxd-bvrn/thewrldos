//! The network stack's state and protocols: Ethernet, ARP, IPv4, ICMP, UDP, the inbound
//! firewall, the DHCP client, the DNS client and TCP. Replaces src/net/net.c and tcp.c; the
//! C interface (src/include/net.h) is in kernel.rs, and the packet parsers in net.rs.
//!
//! Everything here is plain state machines over a [`Link`] (clock, randomness, frame
//! output), so the protocols run and are tested on the host. The kernel glue owns the one
//! [`Stack`], runs [`Stack::rx`] for each received frame and [`Stack::tick`] from the
//! network task, and turns the waiting calls (connect, receive, resolve) into short
//! check-then-sleep loops that never hold the stack while sleeping.
//!
//! Differences from the C version, all on purpose:
//! - Sending to an address whose MAC is not known yet queues the packet and asks by ARP; it
//!   goes out when the answer comes and is dropped after a second. (C slept in `ip_send`,
//!   sometimes with other state half-updated, or dropped the packet on the network task.)
//! - UDP handlers registered from C are not called from inside the stack: [`Stack::rx`]
//!   returns the delivery and the caller makes it, so a handler may send in reply.
//! - TCP connections are owned here; the handles C holds are checked before use, so a stale
//!   one is ignored instead of pointing into freed memory.
//! - Memory that depends on traffic (connection buffers, the ARP queue) is reserved with
//!   `try_reserve`, so running out of memory refuses a connection instead of panicking.

use alloc::boxed::Box;
use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use crate::net;
use crate::CBool;

// ---------------------------------------------------------------- the interface

/// netif_t (src/include/net.h): the interface's settings and counters, shared with C (the
/// drivers fill in `mac` and `driver`; the shell and Security Center read the rest).
#[repr(C)]
#[derive(Clone)]
pub struct Netif {
    pub present: CBool,
    pub link: CBool,
    pub configured: CBool,
    pub driver: [u8; 48],
    pub mac: [u8; 6],
    pub ip: u32,
    pub mask: u32,
    pub gw: u32,
    pub dns: u32,
    pub dhcp_server: u32,
    pub lease_s: u32,
    pub rx_packets: u32,
    pub tx_packets: u32,
    pub rx_bytes: u32,
    pub tx_bytes: u32,
    pub rx_dropped: u32,
}

impl Netif {
    pub const fn new() -> Self {
        Netif { present: 0, link: 0, configured: 0, driver: [0; 48], mac: [0; 6], ip: 0, mask: 0, gw: 0, dns: 0,
                dhcp_server: 0, lease_s: 0, rx_packets: 0, tx_packets: 0, rx_bytes: 0, tx_bytes: 0, rx_dropped: 0 }
    }
    pub fn ready(&self) -> bool { self.present != 0 && self.link != 0 && self.configured != 0 }
}

/// What the stack needs from its surroundings.
pub trait Link {
    /// milliseconds, wrapping
    fn now(&self) -> u32;
    fn rand(&mut self) -> u32;
    /// sends one Ethernet frame; false when there is no card or it is busy
    fn send(&mut self, frame: &[u8]) -> bool;
}

/// One call's view of the world: the interface settings (C's netif), the link, and the
/// computer's name (for DHCP).
pub struct Io<'a> {
    pub nif: &'a mut Netif,
    pub link: &'a mut dyn Link,
    pub hostname: &'a [u8],
}

/// The inbound firewall's switches (C globals) for one received frame.
#[derive(Clone, Copy, Default)]
pub struct FwConfig { pub enabled: bool, pub block_tcp: bool, pub block_udp: bool }

/// fw_event_t: a blocked attempt, for Security Center.
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct FwEvent { pub when: u32, pub src: u32, pub port: u16, pub proto: u8 }

/// A C UDP handler: void (*)(uint32_t src, uint16_t sport, const uint8_t *data, uint32_t len, void *ctx).
pub type UdpFn = unsafe extern "C" fn(u32, u16, *const u8, u32, *mut core::ffi::c_void);

/// A datagram for a C handler, to be delivered by the caller of [`Stack::rx`]: the payload
/// is `frame[off..off + len]`.
#[derive(Clone, Copy)]
pub struct Delivery { pub f: UdpFn, pub ctx: usize, pub src: u32, pub sport: u16, pub off: usize, pub len: usize }

/// What a received frame led to.
#[derive(Default)]
pub struct RxResult { pub delivery: Option<Delivery>, pub blocked: u32 }

pub const IPPROTO_ICMP: u8 = 1;
pub const IPPROTO_TCP: u8 = 6;
pub const IPPROTO_UDP: u8 = 17;
const BCAST: [u8; 6] = [0xFF; 6];
const PING_ID: u16 = 0x5752;

fn put16(b: &mut [u8], o: usize, v: u16) { b[o..o + 2].copy_from_slice(&v.to_be_bytes()); }
fn put32(b: &mut [u8], o: usize, v: u32) { b[o..o + 4].copy_from_slice(&v.to_be_bytes()); }
fn after(now: u32, t: u32) -> bool { now.wrapping_sub(t) as i32 >= 0 }

// ---------------------------------------------------------------- Ethernet, ARP, IPv4

#[derive(Clone, Copy, Default)]
struct ArpEntry { ip: u32, mac: [u8; 6], time: u32 }

/// A packet waiting for its next hop's MAC.
struct Pending { hop: u32, asked: u32, tries: u32, pkt: Vec<u8> }

/// Ethernet, ARP and IPv4 output: what every protocol sends through.
pub struct Core {
    arp: [ArpEntry; 32],
    pending: Vec<Pending>,
    ip_id: u16,
}

const ARP_RETRY_MS: u32 = 250;
const ARP_TRIES: u32 = 4;
const MAX_PENDING: usize = 64;

impl Core {
    const fn new() -> Self { Core { arp: [ArpEntry { ip: 0, mac: [0; 6], time: 0 }; 32], pending: Vec::new(), ip_id: 0 } }

    fn eth_send(&mut self, io: &mut Io, dst: &[u8; 6], kind: u16, payload: &[u8]) -> bool {
        if payload.len() > 1500 { return false; }
        let mut f = [0u8; 1514];
        f[..6].copy_from_slice(dst);
        f[6..12].copy_from_slice(&io.nif.mac);
        put16(&mut f, 12, kind);
        f[14..14 + payload.len()].copy_from_slice(payload);
        io.nif.tx_packets = io.nif.tx_packets.wrapping_add(1);
        io.nif.tx_bytes = io.nif.tx_bytes.wrapping_add(payload.len() as u32 + 14);
        io.link.send(&f[..14 + payload.len()])
    }

    fn arp_store(&mut self, io: &mut Io, ip: u32, mac: [u8; 6]) {
        if ip == 0 { return; }
        let slot = self.arp.iter().position(|e| e.ip == ip)
            .unwrap_or_else(|| (0..32).min_by_key(|&i| self.arp[i].time).unwrap_or(0));
        self.arp[slot] = ArpEntry { ip, mac, time: io.link.now() | 1 };
        // send what was waiting for this address
        let mut i = 0;
        while i < self.pending.len() {
            if self.pending[i].hop == ip {
                let p = self.pending.remove(i);
                self.eth_send(io, &mac, 0x0800, &p.pkt);
            } else {
                i += 1;
            }
        }
    }

    fn arp_lookup(&self, ip: u32) -> Option<[u8; 6]> {
        self.arp.iter().find(|e| e.ip == ip && e.time != 0).map(|e| e.mac)
    }

    fn arp_send(&mut self, io: &mut Io, op: u16, tmac: &[u8; 6], tip: u32) {
        let mut a = [0u8; 28];
        put16(&mut a, 0, 1); put16(&mut a, 2, 0x0800); a[4] = 6; a[5] = 4; put16(&mut a, 6, op);
        a[8..14].copy_from_slice(&io.nif.mac); put32(&mut a, 14, io.nif.ip);
        if op == 2 { a[18..24].copy_from_slice(tmac); }
        put32(&mut a, 24, tip);
        self.eth_send(io, if op == 1 { &BCAST } else { tmac }, 0x0806, &a);
    }

    /// Sends an IPv4 packet: at once to a known or broadcast address, else after ARP.
    /// False when it cannot be sent (too big, no memory to queue it, or the card refused).
    pub fn ip_send(&mut self, io: &mut Io, dst: u32, proto: u8, payload: &[u8]) -> bool {
        if payload.len() > 1480 { return false; }
        let mut pkt = [0u8; 1500];
        let h = &mut pkt[..20];
        h[0] = 0x45; put16(h, 2, 20 + payload.len() as u16); put16(h, 4, self.ip_id); put16(h, 6, 0x4000);
        h[8] = 64; h[9] = proto; put32(h, 12, io.nif.ip); put32(h, 16, dst);
        let c = net::checksum(h, 0); put16(h, 10, c);
        self.ip_id = self.ip_id.wrapping_add(1);
        pkt[20..20 + payload.len()].copy_from_slice(payload);
        let pkt = &pkt[..20 + payload.len()];
        let nif = &io.nif;
        let bcast = dst == 0xFFFF_FFFF || (nif.mask != 0 && dst | nif.mask == 0xFFFF_FFFF);
        if bcast { return self.eth_send(io, &BCAST, 0x0800, pkt); }
        let off_link = nif.mask != 0 && dst & nif.mask != nif.ip & nif.mask && nif.gw != 0;
        let hop = if off_link { nif.gw } else { dst };
        if let Some(mac) = self.arp_lookup(hop) { return self.eth_send(io, &mac, 0x0800, pkt); }
        // ask, and keep the packet until the answer
        if self.pending.len() >= MAX_PENDING { self.pending.remove(0); }
        let mut v = Vec::new();
        if v.try_reserve_exact(pkt.len()).is_err() || self.pending.try_reserve(1).is_err() { return false; }
        v.extend_from_slice(pkt);
        let now = io.link.now();
        let asked_already = self.pending.iter().any(|p| p.hop == hop);
        self.pending.push(Pending { hop, asked: now, tries: 1, pkt: v });
        if !asked_already { self.arp_send(io, 1, &[0; 6], hop); }
        true
    }

    /// Asks again for the addresses packets wait on, and drops packets after ARP_TRIES asks.
    fn arp_tick(&mut self, io: &mut Io) {
        let now = io.link.now();
        let mut ask: Vec<u32> = Vec::new();
        self.pending.retain_mut(|p| {
            if now.wrapping_sub(p.asked) < ARP_RETRY_MS { return true; }
            if p.tries >= ARP_TRIES { return false; }
            p.tries += 1; p.asked = now;
            if !ask.contains(&p.hop) { ask.push(p.hop); }
            true
        });
        for hop in ask { self.arp_send(io, 1, &[0; 6], hop); }
    }

    #[cfg(test)]
    pub fn pending_count(&self) -> usize { self.pending.len() }
}

// ---------------------------------------------------------------- UDP, firewall, DHCP, DNS

#[derive(Clone, Copy)]
enum UdpHandler { C(UdpFn, usize), Dhcp, Dns(usize) }

#[derive(Clone, Copy)]
struct UdpSock { port: u16, h: UdpHandler }

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DhcpState { Idle, Discover, Request, Bound }

#[derive(Clone, Copy, Default)]
struct DnsQuery { active: bool, id: u16, port: u16, ip: u32, done: bool, failed: bool, rcode: u8 }

/// A DNS lookup's progress.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DnsState { Waiting, Address(u32), Failed { rcode: u8 } }

struct DnsCache { name: String, ip: u32, expires: u32 }

const FW_LOG: usize = 32;
const DNS_SLOTS: usize = 8;

/// The whole stack.
pub struct Stack {
    pub core: Core,
    udp: [Option<UdpSock>; 16],
    next_port: u16,
    ping_seen: i32,
    fw_ports: Vec<u16>,
    fw_log: [FwEvent; FW_LOG],
    fw_log_n: usize,
    fw_log_head: usize,
    dhcp: DhcpState,
    dhcp_xid: u32,
    dhcp_offer: u32,
    dhcp_server: u32,
    dhcp_next: u32,
    dhcp_tries: u32,
    dhcp_bound_at: u32,
    dns: [DnsQuery; DNS_SLOTS],
    dns_cache: Vec<DnsCache>,
    pub tcp: Tcp,
    /// set when the DHCP client configured the interface (for the log)
    pub dhcp_bound_event: bool,
}

impl Default for Stack { fn default() -> Self { Self::new() } }

impl Stack {
    pub const fn new() -> Self {
        Stack {
            core: Core::new(), udp: [None; 16], next_port: 0, ping_seen: -1,
            fw_ports: Vec::new(), fw_log: [FwEvent { when: 0, src: 0, port: 0, proto: 0 }; FW_LOG], fw_log_n: 0, fw_log_head: 0,
            dhcp: DhcpState::Idle, dhcp_xid: 0, dhcp_offer: 0, dhcp_server: 0, dhcp_next: 0, dhcp_tries: 0, dhcp_bound_at: 0,
            dns: [DnsQuery { active: false, id: 0, port: 0, ip: 0, done: false, failed: false, rcode: 0 }; DNS_SLOTS],
            dns_cache: Vec::new(), tcp: Tcp::new(), dhcp_bound_event: false,
        }
    }

    // ---- UDP

    pub fn ephemeral_port(&mut self, io: &mut Io) -> u16 {
        if self.next_port == 0 { self.next_port = 49152 + (io.link.rand() % 8000) as u16; }
        self.next_port = if self.next_port >= 65000 { 49152 } else { self.next_port + 1 };
        self.next_port
    }

    fn bind(&mut self, port: u16, h: UdpHandler) -> bool {
        if port != 0 && self.udp.iter().flatten().any(|s| s.port == port) { return false; }     // one owner per port
        match self.udp.iter_mut().find(|s| s.is_none()) {
            Some(slot) => { *slot = Some(UdpSock { port, h }); true }
            None => false,
        }
    }
    pub fn udp_bind(&mut self, port: u16, f: UdpFn, ctx: usize) -> bool { self.bind(port, UdpHandler::C(f, ctx)) }
    pub fn udp_unbind(&mut self, port: u16) { for s in self.udp.iter_mut() { if s.is_some_and(|s| s.port == port) { *s = None; } } }
    fn udp_bound(&self, port: u16) -> bool { self.udp.iter().flatten().any(|s| s.port == port) }

    pub fn udp_send(&mut self, io: &mut Io, dst: u32, sport: u16, dport: u16, data: &[u8]) -> bool {
        if data.len() + 8 > 1480 { return false; }
        let mut u = [0u8; 1480];
        let n = 8 + data.len();
        put16(&mut u, 0, sport); put16(&mut u, 2, dport); put16(&mut u, 4, n as u16);
        u[8..n].copy_from_slice(data);
        let sum = pseudo_sum(io.nif.ip, dst, IPPROTO_UDP, n);
        let ck = net::checksum(&u[..n], sum);
        put16(&mut u, 6, if ck == 0 { 0xFFFF } else { ck });
        self.core.ip_send(io, dst, IPPROTO_UDP, &u[..n])
    }

    // ---- ICMP

    /// Sends ping `seq`; [`Stack::ping_answered`] says when its reply came.
    pub fn ping_send(&mut self, io: &mut Io, dst: u32, seq: u16) -> bool {
        let mut p = [0u8; 40];
        p[0] = 8; put16(&mut p, 4, PING_ID); put16(&mut p, 6, seq);
        for (i, b) in p.iter_mut().enumerate().skip(8) { *b = b'a' + (i % 26) as u8; }
        let c = net::checksum(&p, 0); put16(&mut p, 2, c);
        self.ping_seen = -1;
        self.core.ip_send(io, dst, IPPROTO_ICMP, &p)
    }
    pub fn ping_answered(&self, seq: u16) -> bool { self.ping_seen == seq as i32 }

    fn icmp_input(&mut self, io: &mut Io, src: u32, p: &[u8]) {
        if p.len() < 8 { return; }
        if p[0] == 8 {
            // echo request: the same back as a reply
            let mut r = [0u8; 1480];
            if p.len() > r.len() { return; }
            let r = &mut r[..p.len()];
            r.copy_from_slice(p);
            r[0] = 0; put16(r, 2, 0);
            let c = net::checksum(r, 0); put16(r, 2, c);
            self.core.ip_send(io, src, IPPROTO_ICMP, r);
        } else if p[0] == 0 && u16::from_be_bytes([p[4], p[5]]) == PING_ID {
            self.ping_seen = u16::from_be_bytes([p[6], p[7]]) as i32;
        }
    }

    // ---- firewall

    pub fn fw_port_allowed(&self, p: u16) -> bool { self.fw_ports.contains(&p) }
    pub fn fw_allow_port(&mut self, p: u16) -> bool {
        if self.fw_port_allowed(p) { return true; }
        if self.fw_ports.len() >= 16 || self.fw_ports.try_reserve(1).is_err() { return false; }
        self.fw_ports.push(p);
        true
    }
    pub fn fw_remove_port(&mut self, p: u16) -> bool {
        match self.fw_ports.iter().position(|&x| x == p) { Some(i) => { self.fw_ports.swap_remove(i); true } None => false }
    }
    pub fn fw_clear_ports(&mut self) { self.fw_ports.clear(); }
    pub fn fw_ports(&self) -> &[u16] { &self.fw_ports }
    pub fn fw_log_count(&self) -> usize { self.fw_log_n }
    /// The `i`-th most recent blocked attempt (0 = newest).
    pub fn fw_log_at(&self, i: usize) -> Option<&FwEvent> {
        (i < self.fw_log_n).then(|| &self.fw_log[(self.fw_log_head + FW_LOG - 1 - i) % FW_LOG])
    }
    fn fw_note(&mut self, now: u32, src: u32, port: u16, proto: u8) {
        self.fw_log[self.fw_log_head] = FwEvent { when: now, src, port, proto };
        self.fw_log_head = (self.fw_log_head + 1) % FW_LOG;
        if self.fw_log_n < FW_LOG { self.fw_log_n += 1; }
    }

    // ---- receiving

    /// Handles one received Ethernet frame.
    pub fn rx(&mut self, io: &mut Io, f: &[u8], fw: FwConfig) -> RxResult {
        let mut res = RxResult::default();
        if f.len() < 14 { return res; }
        io.nif.rx_packets = io.nif.rx_packets.wrapping_add(1);
        io.nif.rx_bytes = io.nif.rx_bytes.wrapping_add(f.len() as u32);
        match u16::from_be_bytes([f[12], f[13]]) {
            0x0806 => {
                if let Some(a) = net::arp(&f[14..]) {
                    self.core.arp_store(io, a.spa, a.sha);
                    if a.op == 1 && io.nif.ip != 0 && a.tpa == io.nif.ip { self.core.arp_send(io, 2, &a.sha, a.spa); }
                }
            }
            0x0800 => {
                // learn the sender's MAC for on-link hosts (saves ARP round trips)
                if f.len() >= 34 && io.nif.mask != 0 {
                    let s = u32::from_be_bytes([f[26], f[27], f[28], f[29]]);
                    if s & io.nif.mask == io.nif.ip & io.nif.mask {
                        let mac: [u8; 6] = f[6..12].try_into().unwrap_or([0; 6]);
                        self.core.arp_store(io, s, mac);
                    }
                }
                self.ip_input(io, f, 14, fw, &mut res);
            }
            _ => {}
        }
        res
    }

    fn ip_input(&mut self, io: &mut Io, f: &[u8], at: usize, fw: FwConfig, res: &mut RxResult) {
        let p = &f[at..];
        let Some(ip) = net::ipv4(p) else { return };
        let nif = &io.nif;
        let mine = ip.dst == nif.ip || ip.dst == 0xFFFF_FFFF || (nif.mask != 0 && ip.dst | nif.mask == 0xFFFF_FFFF) || nif.ip == 0;
        if !mine { return; }
        let pl = &p[ip.off..ip.off + ip.len];
        if fw.enabled {
            let now = io.link.now();
            if ip.proto == IPPROTO_TCP && fw.block_tcp {
                // a pure SYN is a new inbound connection; ACK/SYN-ACK belongs to one we have
                if let Some(t) = net::tcp(pl) {
                    if t.flags & 0x02 != 0 && t.flags & 0x10 == 0 && !self.fw_port_allowed(t.dport) {
                        self.fw_note(now, ip.src, t.dport, IPPROTO_TCP); res.blocked += 1; return;
                    }
                }
            } else if ip.proto == IPPROTO_UDP && fw.block_udp {
                if let Some(u) = net::udp(pl) {
                    if !self.udp_bound(u.dport) && !self.fw_port_allowed(u.dport) && u.dport != 68 {
                        self.fw_note(now, ip.src, u.dport, IPPROTO_UDP); res.blocked += 1; return;
                    }
                }
            }
        }
        match ip.proto {
            IPPROTO_ICMP => self.icmp_input(io, ip.src, pl),
            IPPROTO_UDP => {
                let Some(u) = net::udp(pl) else { return };
                let data = &pl[u.off..u.off + u.len];
                let Some(sock) = self.udp.iter().flatten().find(|s| s.port == u.dport).copied() else { return };
                match sock.h {
                    UdpHandler::C(func, ctx) => {
                        res.delivery = Some(Delivery { f: func, ctx, src: ip.src, sport: u.sport, off: at + ip.off + u.off, len: u.len });
                    }
                    UdpHandler::Dhcp => self.dhcp_input(io, ip.src, data),
                    UdpHandler::Dns(slot) => self.dns_input(slot, data),
                }
            }
            IPPROTO_TCP => self.tcp.input(&mut self.core, io, ip.src, pl),
            _ => {}
        }
    }

    // ---- DHCP

    pub fn dhcp_state(&self) -> DhcpState { self.dhcp }

    /// Starts over: forgets the address and asks for one.
    pub fn dhcp_renew(&mut self, io: &mut Io) {
        if io.nif.present == 0 { return; }
        io.nif.configured = 0; io.nif.ip = 0;
        self.dhcp_xid = io.link.rand();
        self.dhcp = DhcpState::Discover; self.dhcp_tries = 0; self.dhcp_next = io.link.now();
    }

    pub fn dhcp_start(&mut self) -> bool { self.bind(68, UdpHandler::Dhcp) }

    /// Stops asking for addresses (a fixed address was set, or the adapter is disabled).
    pub fn dhcp_stop(&mut self) { self.dhcp = DhcpState::Idle; }

    fn dhcp_send(&mut self, io: &mut Io, kind: u8) {
        let mut m = [0u8; 400];
        m[0] = 1; m[1] = 1; m[2] = 6; put32(&mut m, 4, self.dhcp_xid); put16(&mut m, 10, 0x8000);
        m[28..34].copy_from_slice(&io.nif.mac);
        put32(&mut m, 236, 0x6382_5363);
        let mut o = 240;
        let mut opt = |m: &mut [u8], code: u8, v: &[u8]| { m[o] = code; m[o + 1] = v.len() as u8; m[o + 2..o + 2 + v.len()].copy_from_slice(v); o += 2 + v.len(); };
        opt(&mut m, 53, &[kind]);
        let mut cid = [1u8; 7]; cid[1..].copy_from_slice(&io.nif.mac);
        opt(&mut m, 61, &cid);
        let host = &io.hostname[..io.hostname.len().min(32)];
        opt(&mut m, 12, host);
        if kind == 3 {
            opt(&mut m, 50, &self.dhcp_offer.to_be_bytes());
            opt(&mut m, 54, &self.dhcp_server.to_be_bytes());
        }
        opt(&mut m, 55, &[1, 3, 6, 51]);
        m[o] = 255; o += 1;
        let save = io.nif.ip; io.nif.ip = 0;                  // DHCP is sent from 0.0.0.0
        self.udp_send(io, 0xFFFF_FFFF, 68, 67, &m[..o.max(300)]);
        io.nif.ip = save;
    }

    fn dhcp_input(&mut self, io: &mut Io, src: u32, m: &[u8]) {
        let Some(r) = net::dhcp(m, self.dhcp_xid) else { return };
        match (r.kind, self.dhcp) {
            (2, DhcpState::Discover) => {                         // OFFER
                self.dhcp_offer = r.yiaddr;
                self.dhcp_server = if r.server != 0 { r.server } else { src };
                self.dhcp = DhcpState::Request; self.dhcp_tries = 0; self.dhcp_next = io.link.now();
            }
            (5, DhcpState::Request) => {                          // ACK
                let n = &mut *io.nif;
                n.ip = r.yiaddr;
                n.mask = if r.mask != 0 { r.mask } else { 0xFFFF_FF00 };
                n.gw = r.gw;
                n.dns = if r.dns != 0 { r.dns } else { r.gw };
                n.dhcp_server = self.dhcp_server; n.lease_s = r.lease;
                n.configured = 1;
                self.dhcp = DhcpState::Bound; self.dhcp_bound_at = io.link.now();
                self.dhcp_bound_event = true;
            }
            (6, DhcpState::Request) => {                          // NAK
                self.dhcp = DhcpState::Discover; self.dhcp_tries = 0; self.dhcp_next = io.link.now().wrapping_add(1000);
            }
            _ => {}
        }
    }

    fn dhcp_tick(&mut self, io: &mut Io) {
        let now = io.link.now();
        if self.dhcp == DhcpState::Bound {
            // renew at half the lease
            let l = io.nif.lease_s;
            if l != 0 && l < 0x7FFF_FFFF && now.wrapping_sub(self.dhcp_bound_at) > l.saturating_mul(500) {
                self.dhcp = DhcpState::Request; self.dhcp_offer = io.nif.ip; self.dhcp_tries = 0; self.dhcp_next = now;
            }
            return;
        }
        if self.dhcp == DhcpState::Idle || !after(now, self.dhcp_next) { return; }
        if self.dhcp == DhcpState::Request && self.dhcp_tries >= 4 { self.dhcp = DhcpState::Discover; self.dhcp_tries = 0; }
        let kind = if self.dhcp == DhcpState::Discover { 1 } else { 3 };
        self.dhcp_send(io, kind);
        self.dhcp_tries += 1;
        self.dhcp_next = now.wrapping_add(16000u32.min(1000u32 << self.dhcp_tries.min(4)));
    }

    // ---- DNS

    pub fn dns_cache_get(&self, name: &str, now: u32) -> Option<u32> {
        self.dns_cache.iter().find(|c| c.name.eq_ignore_ascii_case(name) && !after(now, c.expires)).map(|c| c.ip)
    }
    pub fn dns_cache_put(&mut self, io: &mut Io, name: &str, ip: u32) {
        let now = io.link.now();
        let mut n = String::new();
        if n.try_reserve(name.len()).is_err() { return; }
        n.push_str(name);
        let e = DnsCache { name: n, ip, expires: now.wrapping_add(300_000) };
        if let Some(c) = self.dns_cache.iter_mut().find(|c| c.name.eq_ignore_ascii_case(name)) { *c = e; return; }
        if self.dns_cache.len() < 32 {
            if self.dns_cache.try_reserve(1).is_ok() { self.dns_cache.push(e); }
        } else {
            let k = (io.link.rand() % 32) as usize;
            self.dns_cache[k] = e;
        }
    }

    /// Starts looking up `name` (A record) at the interface's DNS server; the slot to follow
    /// it with, or None when no query slot or port is free or the name is not a DNS name.
    pub fn dns_begin(&mut self, io: &mut Io, name: &str) -> Option<usize> {
        let q = dns_query_packet(name, 0)?;
        let slot = self.dns.iter().position(|q| !q.active)?;
        let port = self.ephemeral_port(io);
        if !self.bind(port, UdpHandler::Dns(slot)) { return None; }
        let id = io.link.rand() as u16;
        self.dns[slot] = DnsQuery { active: true, id, port, ..DnsQuery::default() };
        let _ = q;
        self.dns_send(io, slot, name);
        Some(slot)
    }
    /// Sends the query (again).
    pub fn dns_send(&mut self, io: &mut Io, slot: usize, name: &str) {
        let Some(q) = self.dns.get(slot).copied().filter(|q| q.active) else { return };
        if let Some(pkt) = dns_query_packet(name, q.id) {
            let dns = io.nif.dns;
            self.udp_send(io, dns, q.port, 53, &pkt.0[..pkt.1]);
        }
    }
    pub fn dns_state(&self, slot: usize) -> DnsState {
        match self.dns.get(slot) {
            Some(q) if q.done => DnsState::Address(q.ip),
            Some(q) if q.failed => DnsState::Failed { rcode: q.rcode },
            Some(q) if q.active => DnsState::Waiting,
            _ => DnsState::Failed { rcode: 0 },
        }
    }
    /// The last answer's rcode so far (3: the name does not exist).
    pub fn dns_rcode(&self, slot: usize) -> u8 { self.dns.get(slot).map_or(0, |q| q.rcode) }
    pub fn dns_end(&mut self, slot: usize) {
        if let Some(q) = self.dns.get_mut(slot) {
            if q.active { let port = q.port; *q = DnsQuery::default(); self.udp_unbind(port); }
        }
    }
    fn dns_input(&mut self, slot: usize, m: &[u8]) {
        let Some(q) = self.dns.get_mut(slot).filter(|q| q.active) else { return };
        match net::dns(m, q.id) {
            net::Dns::NotOurs => {}
            net::Dns::Address(ip) => { q.ip = ip; q.done = true; }
            net::Dns::NoAddress { rcode } => { q.rcode = rcode; q.failed = true; }
        }
    }

    // ---- timers

    /// Called by the network task every few milliseconds.
    pub fn tick(&mut self, io: &mut Io) {
        self.core.arp_tick(io);
        self.dhcp_tick(io);
        self.tcp.timers(&mut self.core, io);
    }
}

fn pseudo_sum(src: u32, dst: u32, proto: u8, len: usize) -> u32 {
    (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto as u32 + len as u32
}

/// A DNS query for `name` (type A, class IN, recursion desired): the bytes and their length.
/// None if a label is empty or longer than 63, or the name is too long.
pub fn dns_query_packet(name: &str, id: u16) -> Option<([u8; 300], usize)> {
    let mut m = [0u8; 300];
    put16(&mut m, 0, id); m[2] = 1; put16(&mut m, 4, 1);
    let mut o = 12;
    for label in name.trim_end_matches('.').split('.') {
        let l = label.len();
        if l == 0 || l > 63 || o + 1 + l + 5 > 270 { return None; }
        m[o] = l as u8;
        m[o + 1..o + 1 + l].copy_from_slice(label.as_bytes());
        o += 1 + l;
    }
    m[o] = 0; put16(&mut m, o + 1, 1); put16(&mut m, o + 3, 1);
    Some((m, o + 5))
}

/// Looks `name` up in a hosts file ("ADDRESS NAME... # comment" lines, first match wins).
pub fn hosts_lookup(text: &[u8], name: &str) -> Option<u32> {
    for line in text.split(|&c| c == b'\n' || c == b'\r') {
        let line = match line.iter().position(|&c| c == b'#') { Some(i) => &line[..i], None => line };
        let mut words = line.split(|&c| c == b' ' || c == b'\t').filter(|w| !w.is_empty());
        let Some(addr) = words.next() else { continue };
        if addr.len() > 31 { continue; }
        if words.any(|w| w.eq_ignore_ascii_case(name.as_bytes())) {
            if let Some(ip) = net::parse_ipv4_addr(addr) { return Some(ip); }
        }
    }
    None
}

// ---------------------------------------------------------------- TCP

/// Connection states (RFC 793).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum St { Closed, SynSent, Established, FinWait1, FinWait2, CloseWait, LastAck, TimeWait, SynRcvd }

const F_FIN: u8 = 0x01;
const F_SYN: u8 = 0x02;
const F_RST: u8 = 0x04;
const F_PSH: u8 = 0x08;
const F_ACK: u8 = 0x10;
pub const RCAP: usize = 64 * 1024;
pub const SCAP: usize = 64 * 1024;

fn seq_lt(a: u32, b: u32) -> bool { (a.wrapping_sub(b) as i32) < 0 }
fn seq_le(a: u32, b: u32) -> bool { (a.wrapping_sub(b) as i32) <= 0 }

/// One connection. C holds `*mut Tcb` handles (tcb_t); they stay valid until the
/// connection is destroyed, which happens only after the application closed it.
pub struct Tcb {
    pub state: St,
    pub rip: u32,
    pub lport: u16,
    pub rport: u16,
    iss: u32,
    snd_una: u32,
    snd_nxt: u32,
    snd_wnd: u32,
    rcv_nxt: u32,
    mss: u16,
    /// received, not yet read by the application
    rbuf: VecDeque<u8>,
    /// from snd_una on: sent but unacknowledged, then unsent
    sbuf: VecDeque<u8>,
    fin_pending: bool,
    fin_sent: bool,
    pub fin_rcvd: bool,
    app_closed: bool,
    rto: u32,
    rtx_at: u32,
    retries: u32,
    last_wnd_adv: u32,
    dead_at: u32,
    /// 0, or why it failed: 1 refused, 2 reset, 3 timed out
    pub err: i32,
}

impl Tcb {
    fn rwnd(&self) -> u32 { ((RCAP - self.rbuf.len()) as u32).min(65535) }
    pub fn rlen(&self) -> usize { self.rbuf.len() }
    pub fn slen(&self) -> usize { self.sbuf.len() }
    fn arm(&mut self, now: u32) { self.rtx_at = now.wrapping_add(self.rto); }
    /// 1 established (data can flow), 0 opening, -1 failed or closed (tcp_status)
    pub fn status(&self) -> i32 {
        match self.state {
            St::Established | St::CloseWait | St::FinWait1 | St::FinWait2 | St::LastAck => 1,
            St::SynSent | St::SynRcvd => 0,
            _ => -1,
        }
    }
    pub fn recv_ready(&self) -> bool { !self.rbuf.is_empty() || self.fin_rcvd || self.state == St::Closed }
    pub fn send_ready(&self) -> bool {
        match self.state { St::Closed => true, St::Established | St::CloseWait => self.sbuf.len() < SCAP, _ => false }
    }
    pub fn hup(&self) -> bool { self.fin_rcvd || self.state == St::Closed }
    fn open(&self) -> bool { matches!(self.state, St::Established | St::CloseWait) }
    fn acceptable(&self) -> bool { matches!(self.state, St::Established | St::CloseWait | St::FinWait1 | St::FinWait2) }
    /// Copies up to buf.len() received bytes; `consume` removes them.
    pub fn read(&mut self, buf: &mut [u8], consume: bool) -> usize {
        let n = buf.len().min(self.rbuf.len());
        for (d, s) in buf[..n].iter_mut().zip(self.rbuf.iter()) { *d = *s; }
        if consume { self.rbuf.drain(..n); }
        n
    }
}

struct Listener { port: u16, queue: VecDeque<*const Tcb> }

/// The connections and listening ports.
pub struct Tcp {
    conns: Vec<Box<Tcb>>,
    listeners: Vec<Listener>,
}

impl Tcp {
    const fn new() -> Self { Tcp { conns: Vec::new(), listeners: Vec::new() } }

    pub fn count(&self) -> usize { self.conns.len() }

    /// The connection behind a handle from C, if it is still one of ours.
    pub fn get(&mut self, h: *const Tcb) -> Option<&mut Tcb> {
        self.conns.iter_mut().map(|b| &mut **b).find(|t| core::ptr::eq(*t as *const Tcb, h))
    }
    fn index(&self, h: *const Tcb) -> Option<usize> { self.conns.iter().position(|b| core::ptr::eq(&**b, h)) }

    fn send_seg(core: &mut Core, io: &mut Io, t: &mut Tcb, seq: u32, flags: u8, data_from: Option<(usize, usize)>) -> bool {
        let mut s = [0u8; 1480];
        let hl = if flags & F_SYN != 0 { 24 } else { 20 };
        let len = data_from.map_or(0, |d| d.1);
        if hl + len > s.len() { return false; }
        put16(&mut s, 0, t.lport); put16(&mut s, 2, t.rport); put32(&mut s, 4, seq);
        put32(&mut s, 8, if flags & F_ACK != 0 { t.rcv_nxt } else { 0 });
        s[12] = ((hl / 4) << 4) as u8; s[13] = flags; put16(&mut s, 14, t.rwnd() as u16);
        if flags & F_SYN != 0 { s[20] = 2; s[21] = 4; put16(&mut s, 22, 1460); }
        if let Some((from, n)) = data_from {
            for (d, b) in s[hl..hl + n].iter_mut().zip(t.sbuf.range(from..from + n)) { *d = *b; }
        }
        let sum = pseudo_sum(io.nif.ip, t.rip, IPPROTO_TCP, hl + len);
        let c = net::checksum(&s[..hl + len], sum); put16(&mut s, 16, c);
        if flags & F_ACK != 0 { t.last_wnd_adv = t.rwnd(); }
        core.ip_send(io, t.rip, IPPROTO_TCP, &s[..hl + len])
    }
    fn send_ack(core: &mut Core, io: &mut Io, t: &mut Tcb) { let n = t.snd_nxt; Self::send_seg(core, io, t, n, F_ACK, None); }

    /// Sends whatever the peer's window allows, then the FIN if one is due.
    fn output(core: &mut Core, io: &mut Io, t: &mut Tcb) {
        if !matches!(t.state, St::Established | St::CloseWait | St::FinWait1 | St::LastAck) { return; }
        let wnd = if t.snd_wnd != 0 { t.snd_wnd } else { 1 };
        let inflight = |t: &Tcb| t.snd_nxt.wrapping_sub(t.snd_una).wrapping_sub(t.fin_sent as u32) as usize;
        loop {
            let fl = inflight(t);
            if fl >= t.sbuf.len() || fl >= wnd as usize { break; }
            let n = (t.mss as usize).min(t.sbuf.len() - fl).min(wnd as usize - fl);
            let seq = t.snd_nxt;
            if !Self::send_seg(core, io, t, seq, F_ACK | F_PSH, Some((fl, n))) { break; }
            t.snd_nxt = t.snd_nxt.wrapping_add(n as u32);
            if t.rtx_at == 0 { t.arm(io.link.now()); }
        }
        if t.fin_pending && !t.fin_sent && inflight(t) >= t.sbuf.len() {
            let seq = t.snd_nxt;
            Self::send_seg(core, io, t, seq, F_FIN | F_ACK, None);
            t.snd_nxt = t.snd_nxt.wrapping_add(1); t.fin_sent = true;
            t.state = if t.state == St::CloseWait { St::LastAck } else { St::FinWait1 };
            if t.rtx_at == 0 { t.arm(io.link.now()); }
        }
    }

    fn accept_remove(&mut self, h: *const Tcb) -> bool {
        for l in self.listeners.iter_mut() {
            if let Some(i) = l.queue.iter().position(|&q| core::ptr::eq(q, h)) { l.queue.remove(i); return true; }
        }
        false
    }
    fn destroy(&mut self, h: *const Tcb) {
        self.accept_remove(h);
        if let Some(i) = self.index(h) { self.conns.swap_remove(i); }
    }
    /// The connection (taken out of `conns`) failed: true when it should be freed now
    /// (nobody accepted it yet); one an application holds stays until it closes it, so it
    /// can still see the error.
    fn fail(&mut self, t: &mut Tcb, err: i32, now: u32) -> bool {
        t.err = err; t.state = St::Closed; t.rtx_at = 0;
        if self.accept_remove(t as *const Tcb) { return true; }
        if t.app_closed { t.dead_at = now | 1; }
        false
    }

    fn conn_new(&mut self, io: &mut Io, lport: u16, ip: u32, port: u16, state: St) -> Option<*mut Tcb> {
        let mut rbuf = VecDeque::new();
        let mut sbuf = VecDeque::new();
        if rbuf.try_reserve_exact(RCAP).is_err() || sbuf.try_reserve_exact(SCAP).is_err() || self.conns.try_reserve(1).is_err() { return None; }
        let iss = io.link.rand();
        let mut t = Box::new(Tcb {
            state, rip: ip, lport, rport: port, iss, snd_una: iss, snd_nxt: iss.wrapping_add(1), snd_wnd: 0,
            rcv_nxt: 0, mss: 536, rbuf, sbuf, fin_pending: false, fin_sent: false, fin_rcvd: false, app_closed: false,
            rto: 1000, rtx_at: 0, retries: 0, last_wnd_adv: 0, dead_at: 0, err: 0,
        });
        let p: *mut Tcb = &mut *t;
        self.conns.push(t);
        Some(p)
    }

    /// Begins an active open (SYN) from local port `lport`; the handle, or None without memory.
    pub fn open(&mut self, core: &mut Core, io: &mut Io, lport: u16, ip: u32, port: u16) -> Option<*mut Tcb> {
        let h = self.conn_new(io, lport, ip, port, St::SynSent)?;
        let t = self.get(h)?;
        let iss = t.iss;
        Self::send_seg(core, io, t, iss, F_SYN, None);
        t.arm(io.link.now());
        Some(h)
    }

    /// Queues what fits of `data` and sends what the window allows: bytes taken, or -1 when
    /// the connection cannot send.
    pub fn send(&mut self, core: &mut Core, io: &mut Io, h: *const Tcb, data: &[u8]) -> i32 {
        let Some(t) = self.get(h) else { return -1 };
        if !t.open() { return -1; }
        let n = (SCAP - t.sbuf.len()).min(data.len());
        if n > 0 { t.sbuf.extend(&data[..n]); Self::output(core, io, t); }
        n as i32
    }

    /// Reads received data; sends a window update once enough space has opened.
    pub fn recv(&mut self, core: &mut Core, io: &mut Io, h: *const Tcb, buf: &mut [u8]) -> usize {
        let Some(t) = self.get(h) else { return 0 };
        let n = t.read(buf, true);
        if n > 0 && t.state != St::Closed && t.rwnd() >= t.last_wnd_adv + 8192 { Self::send_ack(core, io, t); }
        n
    }

    pub fn shutdown_wr(&mut self, core: &mut Core, io: &mut Io, h: *const Tcb) {
        let Some(t) = self.get(h) else { return };
        if t.fin_sent || t.fin_pending || !t.open() { return; }
        t.fin_pending = true;
        Self::output(core, io, t);
    }

    /// The application is done with the connection (graceful when it can be).
    pub fn close(&mut self, core: &mut Core, io: &mut Io, h: *const Tcb) {
        let now = io.link.now();
        if self.get(h).is_none() { return; }
        self.accept_remove(h);
        let Some(t) = self.get(h) else { return };
        t.app_closed = true;
        match t.state {
            St::Established | St::CloseWait => { t.fin_pending = true; Self::output(core, io, t); t.dead_at = now.wrapping_add(10_000); }
            St::SynSent | St::SynRcvd => {
                // never completed: reset instead of retransmitting to the grave
                let n = t.snd_nxt;
                Self::send_seg(core, io, t, n, F_RST, None);
                t.state = St::Closed; t.rtx_at = 0; t.dead_at = now;
            }
            St::Closed => t.dead_at = now,
            _ => t.dead_at = now.wrapping_add(5000),
        }
        if t.dead_at == 0 { t.dead_at = 1; }
    }

    /// Frees a connection that never got to an application (tcp_connect giving up).
    pub fn abort(&mut self, core: &mut Core, io: &mut Io, h: *const Tcb) {
        if let Some(t) = self.get(h) {
            if t.state == St::SynSent { let n = t.snd_nxt; Self::send_seg(core, io, t, n, F_RST, None); }
        }
        self.destroy(h);
    }

    pub fn listen(&mut self, port: u16) -> i32 {
        if port == 0 || self.listeners.iter().any(|l| l.port == port) || self.listeners.try_reserve(1).is_err() { return -1; }
        self.listeners.push(Listener { port, queue: VecDeque::new() });
        0
    }
    pub fn unlisten(&mut self, now: u32, port: u16) {
        let Some(i) = self.listeners.iter().position(|l| l.port == port) else { return };
        let l = self.listeners.swap_remove(i);
        for h in l.queue {
            if let Some(t) = self.get(h) {
                t.state = St::Closed; t.rtx_at = 0;
                if !t.app_closed { t.app_closed = true; t.dead_at = now | 1; }
            }
        }
    }
    pub fn accept_ready(&mut self, port: u16) -> bool {
        let Some(l) = self.listeners.iter().position(|l| l.port == port) else { return false };
        let q: Vec<*const Tcb> = self.listeners[l].queue.iter().copied().collect();
        q.into_iter().any(|h| self.get(h).is_some_and(|t| t.acceptable()))
    }
    pub fn accept(&mut self, now: u32, port: u16) -> Option<*mut Tcb> {
        loop {
            let l = self.listeners.iter_mut().find(|l| l.port == port)?;
            let h = l.queue.pop_front()?;
            let t = self.get(h)?;
            if t.acceptable() { return Some(h as *mut Tcb); }
            // reset before the program got to it: let the timers free it
            if !t.app_closed { t.app_closed = true; t.dead_at = now | 1; }
        }
    }

    fn rst_reply(core: &mut Core, io: &mut Io, src: u32, h: &net::Tcp, dlen: usize) {
        let mut tmp = Tcb {
            state: St::Closed, rip: src, lport: h.dport, rport: h.sport, iss: 0, snd_una: 0, snd_nxt: 0, snd_wnd: 0, rcv_nxt: 0,
            mss: 536, rbuf: VecDeque::new(), sbuf: VecDeque::new(), fin_pending: false, fin_sent: false, fin_rcvd: false,
            app_closed: true, rto: 0, rtx_at: 0, retries: 0, last_wnd_adv: 0, dead_at: 0, err: 0,
        };
        if h.flags & F_ACK != 0 {
            Self::send_seg(core, io, &mut tmp, h.ack, F_RST, None);
        } else {
            tmp.rcv_nxt = h.seq.wrapping_add(dlen as u32).wrapping_add((h.flags & F_SYN != 0) as u32).wrapping_add((h.flags & F_FIN != 0) as u32);
            Self::send_seg(core, io, &mut tmp, 0, F_RST | F_ACK, None);
        }
    }

    /// A received segment.
    pub fn input(&mut self, core: &mut Core, io: &mut Io, src: u32, s: &[u8]) {
        let Some(h) = net::tcp(s) else { return };
        let data = &s[h.hl..];
        let now = io.link.now();
        let fl = h.flags;
        let found = self.conns.iter().position(|t| t.rip == src && t.rport == h.sport && t.lport == h.dport && t.state != St::Closed);
        let Some(i) = found else {
            if fl & F_SYN != 0 && fl & F_ACK == 0 && self.listeners.iter().any(|l| l.port == h.dport) && self.accept_syn(core, io, src, &h) { return; }
            if fl & F_RST == 0 { Self::rst_reply(core, io, src, &h, data.len()); }
            return;
        };
        // work on the connection outside the list (its Box, and so the handle, stays put)
        let mut b = self.conns.swap_remove(i);
        let keep = self.segment(core, io, &mut b, src, &h, data, now, fl);
        if keep { self.conns.push(b); }
    }

    /// `input` for a connection taken out of the list: false when it should be freed.
    #[allow(clippy::too_many_arguments)]
    fn segment(&mut self, core: &mut Core, io: &mut Io, t: &mut Tcb, src: u32, h: &net::Tcp, data: &[u8], now: u32, mut fl: u8) -> bool {
        let dlen = data.len() as u32;
        let hp = t as *const Tcb;

        if t.state == St::SynSent {
            if fl & F_ACK != 0 && h.ack != t.iss.wrapping_add(1) { if fl & F_RST == 0 { Self::rst_reply(core, io, src, h, data.len()); } return true; }
            if fl & F_RST != 0 { return !(fl & F_ACK != 0 && self.fail(t, 1, now)); }
            if fl & F_SYN == 0 { return true; }
            t.rcv_nxt = h.seq.wrapping_add(1);
            if h.mss != 0 { t.mss = h.mss.min(1460); }
            if fl & F_ACK != 0 {
                t.snd_una = h.ack; t.snd_wnd = h.win as u32; t.state = St::Established; t.rtx_at = 0; t.retries = 0;
                Self::send_ack(core, io, t);
            }
            return true;
        }
        if fl & F_RST != 0 {
            if seq_le(t.rcv_nxt, h.seq) && seq_lt(h.seq, t.rcv_nxt.wrapping_add(RCAP as u32)) { return !self.fail(t, 2, now); }
            return true;
        }
        if t.state == St::SynRcvd {
            if fl & F_SYN != 0 { let iss = t.iss; Self::send_seg(core, io, t, iss, F_SYN | F_ACK, None); t.arm(now); return true; }   // SYN-ACK lost
            if fl & F_ACK == 0 || h.ack != t.iss.wrapping_add(1) { return true; }
            t.snd_una = h.ack; t.snd_wnd = h.win as u32; t.retries = 0; t.rto = 1000; t.rtx_at = 0;
            t.state = St::Established;
            let lport = t.lport;
            let queued = match self.listeners.iter_mut().find(|l| l.port == lport) {
                Some(l) => l.queue.try_reserve(1).is_ok() && { l.queue.push_back(hp); true },
                None => false,
            };
            if !queued { return !self.fail(t, 1, now); }
            // the ACK may carry the first data: fall through
        } else if fl & F_SYN != 0 {
            Self::send_ack(core, io, t);                         // a retransmitted SYN-ACK
            return true;
        }

        // acknowledgements
        if fl & F_ACK != 0 {
            if seq_lt(t.snd_una, h.ack) && seq_le(h.ack, t.snd_nxt) {
                let acked = h.ack.wrapping_sub(t.snd_una);
                let fin_acked = t.fin_sent && h.ack == t.snd_nxt;
                let data_acked = ((acked - fin_acked as u32) as usize).min(t.sbuf.len());
                t.sbuf.drain(..data_acked);
                t.snd_una = h.ack;
                t.retries = 0; t.rto = 1000;
                t.rtx_at = if t.snd_una == t.snd_nxt { 0 } else { now.wrapping_add(t.rto) };
                if fin_acked {
                    if t.state == St::FinWait1 { t.state = St::FinWait2; }
                    else if t.state == St::LastAck { t.state = St::Closed; t.dead_at = now | 1; }
                }
            }
            t.snd_wnd = h.win as u32;
        }
        // data, in order only
        if dlen > 0 {
            if h.seq == t.rcv_nxt && !t.fin_rcvd {
                let n = (dlen as usize).min(RCAP - t.rbuf.len());
                t.rbuf.extend(&data[..n]);
                t.rcv_nxt = t.rcv_nxt.wrapping_add(n as u32);
                if n < dlen as usize { fl &= !F_FIN; }               // the FIN comes again with the rest
            } else {
                fl &= !F_FIN;
            }
            Self::send_ack(core, io, t);
        }
        if fl & F_FIN != 0 && h.seq.wrapping_add(dlen) == t.rcv_nxt && !t.fin_rcvd {
            t.rcv_nxt = t.rcv_nxt.wrapping_add(1); t.fin_rcvd = true;
            Self::send_ack(core, io, t);
            if t.state == St::Established { t.state = St::CloseWait; }
            else if matches!(t.state, St::FinWait1 | St::FinWait2) { t.state = St::TimeWait; t.dead_at = now.wrapping_add(1000) | 1; }
        }
        Self::output(core, io, t);
        true
    }

    /// A SYN for a listening port: a connection in SYN_RCVD, answered with SYN-ACK.
    fn accept_syn(&mut self, core: &mut Core, io: &mut Io, src: u32, h: &net::Tcp) -> bool {
        let lport = h.dport;
        let Some(p) = self.conn_new(io, lport, src, h.sport, St::SynRcvd) else { return false };
        let t = self.get(p).expect("just made");
        t.rcv_nxt = h.seq.wrapping_add(1);
        t.snd_wnd = h.win as u32;
        if h.mss != 0 { t.mss = h.mss.min(1460); }
        let iss = t.iss;
        Self::send_seg(core, io, t, iss, F_SYN | F_ACK, None);
        t.arm(io.link.now());
        true
    }

    /// Retransmission, give-up and clean-up timers.
    pub fn timers(&mut self, core: &mut Core, io: &mut Io) {
        let now = io.link.now();
        let all = core::mem::take(&mut self.conns);
        let mut kept = Vec::new();
        if kept.try_reserve_exact(all.len()).is_err() { self.conns = all; return; }   // no memory: try next tick
        for mut b in all {
            if self.timer(core, io, &mut b, now) { kept.push(b); } else { self.accept_remove(&*b); }
        }
        self.conns = kept;
    }

    /// One connection's timers (taken out of the list): false when it should be freed.
    fn timer(&mut self, core: &mut Core, io: &mut Io, t: &mut Tcb, now: u32) -> bool {
        {
            if t.dead_at != 0 && t.app_closed && after(now, t.dead_at) { return false; }
            if t.rtx_at == 0 || !after(now, t.rtx_at) { return true; }
            t.retries += 1;
            if t.retries > 8 { return !self.fail(t, 3, now); }
            t.rto = (t.rto * 2).min(16_000);
            match t.state {
                St::SynSent => { let i = t.iss; Self::send_seg(core, io, t, i, F_SYN, None); t.arm(now); return true; }
                St::SynRcvd => { let i = t.iss; Self::send_seg(core, io, t, i, F_SYN | F_ACK, None); t.arm(now); return true; }
                _ => {}
            }
            // go back to the oldest unacknowledged byte
            t.snd_nxt = t.snd_una;
            let fin = t.fin_sent; t.fin_sent = false;
            if fin && t.state == St::FinWait1 { t.state = St::Established; }
            if fin && t.state == St::LastAck { t.state = St::CloseWait; }
            t.rtx_at = 0;
            if t.sbuf.is_empty() && !fin && t.snd_wnd == 0 { Self::send_ack(core, io, t); t.arm(now); return true; }   // window probe
            Self::output(core, io, t);
            if t.rtx_at == 0 { t.arm(now); }
        }
        true
    }
}

// The C side holds raw handles; the stack is only touched by one task at a time.
unsafe impl Send for Stack {}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec;

    /// A link that records what is sent; the test plays the other hosts.
    struct FakeLink { now: u32, seed: u32, sent: Vec<Vec<u8>> }
    impl Link for FakeLink {
        fn now(&self) -> u32 { self.now }
        fn rand(&mut self) -> u32 { self.seed = self.seed.wrapping_mul(1103515245).wrapping_add(12345); self.seed }
        fn send(&mut self, f: &[u8]) -> bool { self.sent.push(f.to_vec()); true }
    }

    const ME: u32 = 0x0A00_020F;      // 10.0.2.15
    const GW: u32 = 0x0A00_0202;      // 10.0.2.2
    const MYMAC: [u8; 6] = [0x52, 0x54, 0, 0x12, 0x34, 0x56];
    const GWMAC: [u8; 6] = [0x52, 0x55, 10, 0, 2, 2];

    struct World { st: Stack, nif: Netif, link: FakeLink }
    impl World {
        fn new(configured: bool) -> Self {
            let mut nif = Netif::new();
            nif.present = 1; nif.link = 1; nif.mac = MYMAC;
            if configured { nif.ip = ME; nif.mask = 0xFFFF_FF00; nif.gw = GW; nif.dns = 0x0A00_0203; nif.configured = 1; }
            World { st: Stack::new(), nif, link: FakeLink { now: 1000, seed: 7, sent: vec![] } }
        }
        fn io(&mut self) -> (&mut Stack, Io<'_>) {
            (&mut self.st, Io { nif: &mut self.nif, link: &mut self.link, hostname: b"wrld" })
        }
        fn rx(&mut self, f: &[u8]) -> RxResult { let (st, mut io) = self.io(); st.rx(&mut io, f, FwConfig::default()) }
        fn rx_fw(&mut self, f: &[u8], fw: FwConfig) -> RxResult { let (st, mut io) = self.io(); st.rx(&mut io, f, fw) }
        fn tick(&mut self, ms: u32) { self.link.now += ms; let (st, mut io) = self.io(); st.tick(&mut io); }
        fn take(&mut self) -> Vec<Vec<u8>> { core::mem::take(&mut self.link.sent) }
        /// Teach the stack the gateway's MAC.
        fn know_gw(&mut self) { let a = arp_frame(2, GWMAC, GW, MYMAC, ME); self.rx(&a); self.take(); }
    }

    fn arp_frame(op: u16, sha: [u8; 6], spa: u32, tha: [u8; 6], tpa: u32) -> Vec<u8> {
        let mut f = vec![0u8; 42];
        f[..6].copy_from_slice(&if op == 1 { BCAST } else { tha }); f[6..12].copy_from_slice(&sha); put16(&mut f, 12, 0x0806);
        put16(&mut f, 14, 1); put16(&mut f, 16, 0x0800); f[18] = 6; f[19] = 4; put16(&mut f, 20, op);
        f[22..28].copy_from_slice(&sha); put32(&mut f, 28, spa); f[32..38].copy_from_slice(&tha); put32(&mut f, 38, tpa);
        f
    }

    fn ip_frame(src: u32, dst: u32, proto: u8, payload: &[u8]) -> Vec<u8> {
        let mut f = vec![0u8; 34];
        f[..6].copy_from_slice(&MYMAC); f[6..12].copy_from_slice(&GWMAC); put16(&mut f, 12, 0x0800);
        let h = &mut f[14..34];
        h[0] = 0x45; put16(h, 2, 20 + payload.len() as u16); h[8] = 64; h[9] = proto; put32(h, 12, src); put32(h, 16, dst);
        let c = net::checksum(h, 0); put16(h, 10, c);
        f.extend_from_slice(payload);
        f
    }

    fn tcp_frame(src: u32, sport: u16, dport: u16, seq: u32, ack: u32, flags: u8, data: &[u8]) -> Vec<u8> {
        let mut s = vec![0u8; 20];
        put16(&mut s, 0, sport); put16(&mut s, 2, dport); put32(&mut s, 4, seq); put32(&mut s, 8, ack);
        s[12] = 0x50; s[13] = flags; put16(&mut s, 14, 65535);
        s.extend_from_slice(data);
        ip_frame(src, ME, IPPROTO_TCP, &s)
    }

    /// (dst ip, proto, payload) of a sent IPv4 frame
    fn sent_ip(f: &[u8]) -> (u32, u8, Vec<u8>) {
        assert_eq!(&f[12..14], &[8, 0]);
        let ip = net::ipv4(&f[14..]).expect("valid IPv4 out");
        (ip.dst, ip.proto, f[14 + ip.off..14 + ip.off + ip.len].to_vec())
    }
    fn sent_tcp(f: &[u8]) -> (net::Tcp, Vec<u8>) {
        let (_, proto, p) = sent_ip(f);
        assert_eq!(proto, IPPROTO_TCP);
        let t = net::tcp(&p).unwrap();
        (t, p[t.hl..].to_vec())
    }

    #[test]
    fn arp_then_send() {
        let mut w = World::new(true);
        let (st, mut io) = w.io();
        assert!(st.core.ip_send(&mut io, 0x0A00_0263, IPPROTO_ICMP, b"payload"));
        assert_eq!(st.core.pending_count(), 1);
        let out = w.take();
        assert_eq!(out.len(), 1);
        assert_eq!(&out[0][12..14], &[8, 6]);                   // an ARP request first
        // the answer releases the packet
        let mac = [2, 0, 0, 0, 0, 0x63];
        w.rx(&arp_frame(2, mac, 0x0A00_0263, MYMAC, ME));
        let out = w.take();
        assert_eq!(out.len(), 1);
        assert_eq!(&out[0][..6], &mac);
        assert_eq!(sent_ip(&out[0]).2, b"payload");
        // no answer: asked again, then dropped
        let (st, mut io) = w.io();
        st.core.ip_send(&mut io, 0x0A00_0264, IPPROTO_ICMP, b"x");
        w.take();
        for _ in 0..3 { w.tick(ARP_RETRY_MS); assert_eq!(w.take().len(), 1); }
        w.tick(ARP_RETRY_MS);
        assert_eq!(w.st.core.pending_count(), 0);
        // off-link addresses go to the gateway's MAC
        w.know_gw();
        let (st, mut io) = w.io();
        st.core.ip_send(&mut io, 0x0808_0808, IPPROTO_UDP, b"y");
        assert_eq!(&w.take()[0][..6], &GWMAC);
    }

    #[test]
    fn answers_arp_and_ping() {
        let mut w = World::new(true);
        w.rx(&arp_frame(1, GWMAC, GW, [0; 6], ME));
        let out = w.take();
        assert_eq!(out.len(), 1);
        assert_eq!(&out[0][..6], &GWMAC);
        assert_eq!(net::arp(&out[0][14..]).unwrap().op, 2);
        let mut echo = vec![8, 0, 0, 0, 0, 1, 0, 1, b'h', b'i'];
        let c = net::checksum(&echo, 0); put16(&mut echo, 2, c);
        w.rx(&ip_frame(GW, ME, IPPROTO_ICMP, &echo));
        let (dst, proto, reply) = sent_ip(&w.take()[0]);
        assert_eq!((dst, proto, reply[0], &reply[8..]), (GW, IPPROTO_ICMP, 0, &b"hi"[..]));
        assert_eq!(net::checksum(&reply, 0), 0);
        // our own ping and its reply
        let (st, mut io) = w.io();
        st.ping_send(&mut io, GW, 9);
        let (_, _, req) = sent_ip(&w.take()[0]);
        let mut rep = req.clone(); rep[0] = 0;
        assert!(!w.st.ping_answered(9));
        w.rx(&ip_frame(GW, ME, IPPROTO_ICMP, &rep));
        assert!(w.st.ping_answered(9));
    }

    #[test]
    fn dhcp_exchange() {
        let mut w = World::new(false);
        assert!(w.st.dhcp_start());
        let (st, mut io) = w.io();
        st.dhcp_renew(&mut io);
        w.tick(1);
        let out = w.take();
        let (dst, _, udp) = sent_ip(&out[0]);
        assert_eq!(dst, 0xFFFF_FFFF);
        let disc = &udp[8..];
        assert_eq!(&disc[236..240], &[0x63, 0x82, 0x53, 0x63]);
        assert_eq!(&disc[240..243], &[53, 1, 1]);              // DISCOVER
        let xid = u32::from_be_bytes(disc[4..8].try_into().unwrap());
        let reply = |kind: u8| {
            let mut m = net::tests::dhcp_reply(xid, &[53, 1, kind, 1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 2, 2, 6, 4, 10, 0, 2, 3, 54, 4, 10, 0, 2, 2, 51, 4, 0, 1, 0x51, 0x80, 255]);
            let mut u = vec![0, 67, 0, 68, 0, 0, 0, 0]; put16(&mut u, 4, 8 + m.len() as u16); u.append(&mut m);
            ip_frame(GW, 0xFFFF_FFFF, IPPROTO_UDP, &u)
        };
        w.rx(&reply(2));                                         // OFFER
        assert_eq!(w.st.dhcp_state(), DhcpState::Request);
        w.tick(1);
        let req = sent_ip(&w.take()[0]).2;
        assert_eq!(&req[8 + 240..8 + 243], &[53, 1, 3]);         // REQUEST
        w.rx(&reply(5));                                         // ACK
        assert_eq!(w.st.dhcp_state(), DhcpState::Bound);
        assert_eq!((w.nif.ip, w.nif.mask, w.nif.gw, w.nif.dns, w.nif.lease_s, w.nif.configured), (ME, 0xFFFFFF00, GW, 0x0A000203, 86400, 1));
    }

    #[test]
    fn dns_lookup() {
        let mut w = World::new(true);
        w.know_gw();
        w.rx(&arp_frame(2, [9; 6], 0x0A00_0203, MYMAC, ME));
        let (st, mut io) = w.io();
        let slot = st.dns_begin(&mut io, "www.example.com").unwrap();
        let (dst, _, udp) = sent_ip(&w.take()[0]);
        assert_eq!(dst, 0x0A00_0203);
        let (port, id) = (u16::from_be_bytes([udp[0], udp[1]]), u16::from_be_bytes([udp[8], udp[9]]));
        assert_eq!(w.st.dns_state(slot), DnsState::Waiting);
        let ans = net::tests::dns_reply(id, 0, &[(&[0xC0, 12], 1, &[93, 184, 216, 34])]);
        let mut u = vec![0, 53, 0, 0, 0, 0, 0, 0]; put16(&mut u, 2, port); put16(&mut u, 4, 8 + ans.len() as u16); u.extend_from_slice(&ans);
        w.rx(&ip_frame(0x0A00_0203, ME, IPPROTO_UDP, &u));
        assert_eq!(w.st.dns_state(slot), DnsState::Address(0x5DB8_D822));
        w.st.dns_end(slot);
        assert!(!w.st.udp_bound(port));
        assert_eq!(dns_query_packet("a..b", 1).map(|p| p.1), None);
    }

    #[test]
    fn hosts_file() {
        let t = b"# comment\r\n127.0.0.1 localhost\n10.1.2.3\tintranet  Wiki.Local # the wiki\n\n10.9.9.9 intranet\n";
        assert_eq!(hosts_lookup(t, "wiki.local"), Some(0x0A01_0203));
        assert_eq!(hosts_lookup(t, "intranet"), Some(0x0A01_0203));        // first match wins
        assert_eq!(hosts_lookup(t, "comment"), None);
        assert_eq!(hosts_lookup(t, "the"), None);
    }

    extern "C" fn record(src: u32, sport: u16, data: *const u8, len: u32, ctx: *mut core::ffi::c_void) {
        let v = unsafe { &mut *(ctx as *mut Vec<(u32, u16, Vec<u8>)>) };
        v.push((src, sport, unsafe { core::slice::from_raw_parts(data, len as usize) }.to_vec()));
    }

    #[test]
    fn udp_to_c_handler_and_firewall() {
        let mut w = World::new(true);
        let mut got: Vec<(u32, u16, Vec<u8>)> = vec![];
        assert!(w.st.udp_bind(5000, record, &mut got as *mut _ as usize));
        assert!(!w.st.udp_bind(5000, record, 0));                // one owner per port
        let f = ip_frame(GW, ME, IPPROTO_UDP, &[0x30, 0x39, 0x13, 0x88, 0, 13, 0, 0, b'h', b'e', b'l', b'l', b'o']);
        let r = w.rx(&f);
        let d = r.delivery.expect("delivered");
        unsafe { (d.f)(d.src, d.sport, f[d.off..].as_ptr(), d.len as u32, d.ctx as *mut _) };
        assert_eq!(got, vec![(GW, 12345, b"hello".to_vec())]);
        // the firewall: a datagram to an unbound port, and a SYN to a closed one
        let fw = FwConfig { enabled: true, block_tcp: true, block_udp: true };
        let r = w.rx_fw(&ip_frame(GW, ME, IPPROTO_UDP, &[0, 1, 0x1F, 0x90, 0, 8, 0, 0]), fw);
        assert_eq!(r.blocked, 1);
        let r = w.rx_fw(&tcp_frame(GW, 4000, 8080, 1, 0, F_SYN, b""), fw);
        assert_eq!(r.blocked, 1);
        assert_eq!(w.st.fw_log_count(), 2);
        assert_eq!(w.st.fw_log_at(0).map(|e| (e.port, e.proto)), Some((8080, IPPROTO_TCP)));
        assert!(w.st.fw_allow_port(8080));
        assert_eq!(w.rx_fw(&tcp_frame(GW, 4000, 8080, 1, 0, F_SYN, b""), fw).blocked, 0);
    }

    fn open_conn(w: &mut World) -> (*mut Tcb, u32, u16) {
        w.know_gw();
        let (st, mut io) = w.io();
        let h = st.tcp.open(&mut st.core, &mut io, 50001, 0x5DB8_D822, 80).unwrap();
        let (syn, _) = sent_tcp(&w.take()[0]);
        assert_eq!(syn.flags, F_SYN);
        assert_eq!(syn.mss, 1460);
        w.rx(&tcp_frame(0x5DB8_D822, 80, syn.sport, 7000, syn.seq.wrapping_add(1), F_SYN | F_ACK, b""));
        let (ack, _) = sent_tcp(&w.take()[0]);
        assert_eq!((ack.flags, ack.ack), (F_ACK, 7001));
        assert_eq!(w.st.tcp.get(h).unwrap().state, St::Established);
        (h, syn.seq, syn.sport)
    }

    #[test]
    fn tcp_client_conversation() {
        let mut w = World::new(true);
        let (h, iss, port) = open_conn(&mut w);
        const PEER: u32 = 0x5DB8_D822;
        // send a request; it goes out at once
        let (st, mut io) = w.io();
        assert_eq!(st.tcp.send(&mut st.core, &mut io, h, b"GET / HTTP/1.0\r\n\r\n"), 18);
        let (seg, data) = sent_tcp(&w.take()[0]);
        assert_eq!((seg.seq, &data[..]), (iss.wrapping_add(1), &b"GET / HTTP/1.0\r\n\r\n"[..]));
        // the reply, with data and FIN, acknowledging our request
        w.rx(&tcp_frame(PEER, 80, port, 7001, iss.wrapping_add(19), F_ACK | F_PSH | F_FIN, b"HTTP/1.0 200 OK\r\n"));
        let t = w.st.tcp.get(h).unwrap();
        assert_eq!((t.state, t.slen(), t.rlen(), t.fin_rcvd), (St::CloseWait, 0, 17, true));
        let mut buf = [0u8; 64];
        let (st, mut io) = w.io();
        let n = st.tcp.recv(&mut st.core, &mut io, h, &mut buf);
        assert_eq!(&buf[..n], b"HTTP/1.0 200 OK\r\n");
        // we close: FIN, LAST_ACK, then the peer's ACK ends it
        w.take();
        let (st, mut io) = w.io();
        st.tcp.close(&mut st.core, &mut io, h);
        let (fin, _) = sent_tcp(&w.take()[0]);
        assert_eq!(fin.flags & F_FIN, F_FIN);
        assert_eq!(w.st.tcp.get(h).unwrap().state, St::LastAck);
        w.rx(&tcp_frame(PEER, 80, port, 7019, fin.seq.wrapping_add(1), F_ACK, b""));
        assert_eq!(w.st.tcp.get(h).unwrap().state, St::Closed);
        w.tick(1);
        assert_eq!(w.st.tcp.count(), 0);                         // freed
        assert!(w.st.tcp.get(h).is_none());                     // and the old handle is refused
    }

    #[test]
    fn tcp_retransmits_and_gives_up() {
        let mut w = World::new(true);
        let (h, iss, _) = open_conn(&mut w);
        let (st, mut io) = w.io();
        st.tcp.send(&mut st.core, &mut io, h, b"lost?");
        w.take();
        w.tick(1000);                                            // the RTO
        let (seg, data) = sent_tcp(&w.take()[0]);
        assert_eq!((seg.seq, &data[..]), (iss.wrapping_add(1), &b"lost?"[..]));
        for _ in 0..12 { w.tick(16_000); }
        let t = w.st.tcp.get(h).unwrap();
        assert_eq!((t.state, t.err), (St::Closed, 3));          // timed out, kept for the app to see
    }

    #[test]
    fn tcp_server_accepts() {
        let mut w = World::new(true);
        w.know_gw();
        assert_eq!(w.st.tcp.listen(8080), 0);
        assert_eq!(w.st.tcp.listen(8080), -1);
        let mut syn = tcp_frame(GW, 40000, 8080, 100, 0, F_SYN, b"");
        // with an MSS option
        syn[14 + 20 + 12] = 0x60;
        let ip_len = u16::from_be_bytes([syn[16], syn[17]]) + 4;
        syn.extend_from_slice(&[2, 4, 0x02, 0x18]);
        put16(&mut syn, 16, ip_len); put16(&mut syn, 24, 0); let c = net::checksum(&syn[14..34], 0); put16(&mut syn, 24, c);
        w.rx(&syn);
        let (synack, _) = sent_tcp(&w.take()[0]);
        assert_eq!((synack.flags, synack.ack), (F_SYN | F_ACK, 101));
        assert!(!w.st.tcp.accept_ready(8080));
        w.rx(&tcp_frame(GW, 40000, 8080, 101, synack.seq.wrapping_add(1), F_ACK | F_PSH, b"hello"));
        assert!(w.st.tcp.accept_ready(8080));
        let h = w.st.tcp.accept(w.link.now, 8080).unwrap();
        assert_eq!(w.st.tcp.get(h).unwrap().rlen(), 5);
        // a segment for nothing gets a reset
        w.take();
        w.rx(&tcp_frame(GW, 40001, 9999, 5, 0, F_ACK, b"?"));
        let (rst, _) = sent_tcp(&w.take()[0]);
        assert_eq!(rst.flags & F_RST, F_RST);
        w.st.tcp.unlisten(w.link.now, 8080);
    }

    #[test]
    fn tcp_reset_and_refused() {
        let mut w = World::new(true);
        let (h, iss, port) = open_conn(&mut w);
        w.rx(&tcp_frame(0x5DB8_D822, 80, port, 7001, iss.wrapping_add(1), F_RST, b""));
        assert_eq!(w.st.tcp.get(h).unwrap().err, 2);
        // refused: RST+ACK to our SYN
        let (st, mut io) = w.io();
        let h2 = st.tcp.open(&mut st.core, &mut io, 50100, 0x5DB8_D822, 81).unwrap();
        let (syn, _) = sent_tcp(&w.take()[0]);
        w.rx(&tcp_frame(0x5DB8_D822, 81, 50100, 0, syn.seq.wrapping_add(1), F_RST | F_ACK, b""));
        let t = w.st.tcp.get(h2).unwrap();
        assert_eq!((t.state, t.err), (St::Closed, 1));
    }

    /// Random frames at a busy stack: no panics, and the stack still works afterwards.
    #[test]
    fn fuzz_no_panic() {
        let mut w = World::new(true);
        let (h, iss, port) = open_conn(&mut w);
        w.st.tcp.listen(8080);
        let mut seed = 0xABCDu32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        let bases = [
            tcp_frame(0x5DB8_D822, 80, port, 7001, iss.wrapping_add(1), F_ACK, b"data"),
            tcp_frame(GW, 40000, 8080, 1, 0, F_SYN, b""),
            arp_frame(1, GWMAC, GW, [0; 6], ME),
            ip_frame(GW, ME, IPPROTO_UDP, &[0, 67, 0, 68, 0, 8, 0, 0]),
            ip_frame(GW, ME, IPPROTO_ICMP, &[8, 0, 0, 0, 0, 0, 0, 0]),
        ];
        for round in 0..50_000 {
            let mut f = bases[round % bases.len()].clone();
            for _ in 0..rnd() % 4 { let k = rnd() as usize % f.len(); f[k] = rnd() as u8; }
            if round % 3 == 0 { let c = net::checksum(&f[14..34.min(f.len())], 0); if f.len() >= 34 { put16(&mut f, 24, 0); let c2 = net::checksum(&f[14..34], 0); put16(&mut f, 24, c2); let _ = c; } }
            w.rx(&f);
            if round % 100 == 0 { w.tick(50); w.take(); }
        }
        let _ = h;
        let (st, mut io) = w.io();
        assert!(st.core.ip_send(&mut io, GW, IPPROTO_ICMP, b"still alive"));
    }
}
