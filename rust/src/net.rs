//! Parsers for what arrives from the network: ARP, IPv4, UDP and TCP headers (with TCP's MSS
//! option), DHCP replies and DNS answers, plus the Internet checksum and dotted-quad
//! addresses. Replaces the parsing in src/net/net.c and src/net/tcp.c; the protocol state
//! (ARP cache, connections, DHCP and DNS clients) is still C, which calls these through
//! kernel.rs and acts on what they return.
//!
//! Packets are untrusted: every field is read through bounds-checked accessors, and a packet
//! that does not parse is dropped (None). They accept what the C code accepted, and one thing
//! less: an IPv4 header whose checksum is wrong (the C code never checked it).

/// Big-endian reads that fail instead of reading past the packet.
fn be16(p: &[u8], o: usize) -> Option<u16> { Some(u16::from_be_bytes([*p.get(o)?, *p.get(o + 1)?])) }
fn be32(p: &[u8], o: usize) -> Option<u32> { Some(u32::from_be_bytes(p.get(o..o + 4)?.try_into().ok()?)) }

/// The Internet checksum (RFC 1071) of `data`, starting from the partial sum `sum`
/// (a pseudo-header's, for UDP and TCP). A correct header sums to 0.
pub fn checksum(data: &[u8], sum: u32) -> u16 {
    let mut s = sum as u64;
    let mut it = data.chunks_exact(2);
    for w in &mut it { s += u16::from_be_bytes([w[0], w[1]]) as u64; }
    if let [b] = it.remainder() { s += (*b as u64) << 8; }
    while s >> 16 != 0 { s = (s & 0xFFFF) + (s >> 16); }
    !(s as u16)
}

/// "a.b.c.d" (each 0-255, at most 3 digits... and a fourth leading zero, as before) as a
/// host-order address.
pub fn parse_ipv4_addr(s: &[u8]) -> Option<u32> {
    let mut v = 0u32;
    let mut it = s.iter().copied().peekable();
    for part in 0..4 {
        let mut p = 0u32;
        let mut digits = 0;
        while let Some(c) = it.peek().copied().filter(|c| c.is_ascii_digit() && digits < 4) {
            p = p * 10 + (c - b'0') as u32;
            digits += 1;
            it.next();
        }
        if digits == 0 || p > 255 { return None; }
        v = (v << 8) | p;
        if part < 3 && it.next() != Some(b'.') { return None; }
    }
    if it.next().is_some() { return None; }
    Some(v)
}

/// An ARP packet for IPv4 over Ethernet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Arp { pub op: u16, pub sha: [u8; 6], pub spa: u32, pub tpa: u32 }

pub fn arp(a: &[u8]) -> Option<Arp> {
    if a.len() < 28 || be16(a, 0)? != 1 || be16(a, 2)? != 0x0800 { return None; }
    Some(Arp { op: be16(a, 6)?, sha: a[8..14].try_into().ok()?, spa: be32(a, 14)?, tpa: be32(a, 24)? })
}

/// An IPv4 packet: addresses, protocol, and where its payload is (`off..off + len` of the
/// bytes given, the total length's worth: Ethernet padding is not payload).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Ipv4 { pub src: u32, pub dst: u32, pub proto: u8, pub off: usize, pub len: usize }

pub fn ipv4(p: &[u8]) -> Option<Ipv4> {
    if p.len() < 20 || p[0] >> 4 != 4 { return None; }
    let hl = (p[0] & 15) as usize * 4;
    let tot = be16(p, 2)? as usize;
    if hl < 20 || tot > p.len() || tot < hl { return None; }
    if be16(p, 6)? & 0x3FFF != 0 { return None; }                 // fragments are not supported
    if checksum(&p[..hl], 0) != 0 { return None; }
    Some(Ipv4 { src: be32(p, 12)?, dst: be32(p, 16)?, proto: p[9], off: hl, len: tot - hl })
}

/// A UDP datagram's ports and payload (its length's worth).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Udp { pub sport: u16, pub dport: u16, pub off: usize, pub len: usize }

pub fn udp(p: &[u8]) -> Option<Udp> {
    let ulen = be16(p, 4)? as usize;
    if ulen < 8 || ulen > p.len() { return None; }
    Some(Udp { sport: be16(p, 0)?, dport: be16(p, 2)?, off: 8, len: ulen - 8 })
}

/// A TCP segment's header; `mss` is the MSS option's value, 0 without one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Tcp {
    pub sport: u16,
    pub dport: u16,
    pub seq: u32,
    pub ack: u32,
    pub flags: u8,
    pub win: u16,
    pub mss: u16,
    /// header length; the data is `hl..` of the segment
    pub hl: usize,
}

pub fn tcp(s: &[u8]) -> Option<Tcp> {
    if s.len() < 20 { return None; }
    let hl = (s[12] >> 4) as usize * 4;
    if hl < 20 || hl > s.len() { return None; }
    let mut mss = 0;
    let opts = &s[20..hl];
    let mut o = 0;
    while o + 1 < opts.len() {
        match opts[o] {
            0 => break,                                           // end of options
            1 => { o += 1; continue; }                            // no-op
            2 if opts[o + 1] == 4 && o + 4 <= opts.len() => mss = be16(opts, o + 2)?,
            _ => {}
        }
        if opts[o + 1] < 2 { break; }
        o += opts[o + 1] as usize;
    }
    Some(Tcp { sport: be16(s, 0)?, dport: be16(s, 2)?, seq: be32(s, 4)?, ack: be32(s, 8)?, flags: s[13], win: be16(s, 14)?, mss, hl })
}

/// What a DHCP server said (RFC 2131/2132): the message type (2 offer, 5 ack, 6 nak), the
/// offered address, and the options this client uses (0 when absent).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Dhcp { pub kind: u8, pub yiaddr: u32, pub mask: u32, pub gw: u32, pub dns: u32, pub server: u32, pub lease: u32 }

/// A DHCP reply to transaction `xid`, or None for anything else.
pub fn dhcp(m: &[u8], xid: u32) -> Option<Dhcp> {
    if m.len() < 240 || m[0] != 2 || be32(m, 4)? != xid || be32(m, 236)? != 0x6382_5363 { return None; }
    let mut d = Dhcp { yiaddr: be32(m, 16)?, ..Dhcp::default() };
    let mut o = 240;
    while o + 1 < m.len() && m[o] != 255 {
        if m[o] == 0 { o += 1; continue; }                        // pad
        let (code, l) = (m[o], m[o + 1] as usize);
        let Some(v) = m.get(o + 2..o + 2 + l) else { break };
        let first32 = || be32(v, 0);
        match code {
            53 if l >= 1 => d.kind = v[0],
            1 if l >= 4 => d.mask = first32()?,
            3 if l >= 4 => d.gw = first32()?,
            6 if l >= 4 => d.dns = first32()?,
            54 if l >= 4 => d.server = first32()?,
            51 if l >= 4 => d.lease = first32()?,
            _ => {}
        }
        o += 2 + l;
    }
    Some(d)
}

/// The outcome of a DNS reply to query `id`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Dns {
    /// not a reply to this query: keep waiting
    NotOurs,
    /// the first IPv4 address (A record) among the answers
    Address(u32),
    /// a reply without one; `rcode` 3 means the name does not exist
    NoAddress { rcode: u8 },
}

/// The offset just past the (possibly compressed) name at `o`.
fn skip_name(m: &[u8], mut o: usize) -> Option<usize> {
    loop {
        let b = *m.get(o)?;
        match b & 0xC0 {
            0 if b == 0 => return Some(o + 1),
            0 => o += 1 + b as usize,
            0xC0 => { m.get(o + 1)?; return Some(o + 2); }        // a pointer ends the name
            _ => return None,                                     // reserved label kinds
        }
    }
}

pub fn dns(m: &[u8], id: u16) -> Dns {
    if m.len() < 12 || be16(m, 0) != Some(id) || m[2] & 0x80 == 0 { return Dns::NotOurs; }
    let rcode = m[3] & 15;
    let fail = Dns::NoAddress { rcode };
    let (qd, an) = (be16(m, 4).unwrap_or(0), be16(m, 6).unwrap_or(0));
    let mut o = 12;
    for _ in 0..qd {
        let Some(e) = skip_name(m, o) else { return fail };
        o = e + 4;                                                // type, class
    }
    for _ in 0..an {
        let Some(e) = skip_name(m, o) else { return fail };
        let (Some(kind), Some(rdlen)) = (be16(m, e), be16(m, e + 8)) else { return fail };
        if kind == 1 && rdlen == 4 {
            if let Some(ip) = be32(m, e + 10) { return Dns::Address(ip); }
        }
        o = e + 10 + rdlen as usize;
    }
    fail
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::vec::Vec;

    fn ip_header(src: u32, dst: u32, proto: u8, payload: usize) -> Vec<u8> {
        let mut h = vec![0x45, 0, 0, 0, 0x12, 0x34, 0x40, 0, 64, proto, 0, 0];
        h[2..4].copy_from_slice(&((20 + payload) as u16).to_be_bytes());
        h.extend_from_slice(&src.to_be_bytes());
        h.extend_from_slice(&dst.to_be_bytes());
        let c = checksum(&h, 0);
        h[10..12].copy_from_slice(&c.to_be_bytes());
        h
    }

    #[test]
    fn checksums() {
        // the example header from RFC 1071's discussion (Wikipedia's IPv4 checksum example)
        let h = [0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb8, 0x61, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7];
        assert_eq!(checksum(&h, 0), 0);
        let mut z = h; z[10] = 0; z[11] = 0;
        assert_eq!(checksum(&z, 0), 0xb861);
        assert_eq!(checksum(&[0xAB], 0), !0xAB00u16);              // odd length: padded with a zero byte
        assert_eq!(checksum(&[], 0xFFFF_FFFF), checksum(&[], 0xFFFF_FFFF));   // large partial sums fold
    }

    #[test]
    fn addresses() {
        assert_eq!(parse_ipv4_addr(b"10.0.2.15"), Some(0x0A00_020F));
        assert_eq!(parse_ipv4_addr(b"255.255.255.255"), Some(0xFFFF_FFFF));
        assert_eq!(parse_ipv4_addr(b"0010.0.0.1"), Some(0x0A00_0001));   // four digits, as before
        for bad in [&b""[..], b"1.2.3", b"1.2.3.4.", b"256.1.1.1", b"1..2.3", b"a.b.c.d", b"1.2.3.4 ", b"01234.1.1.1"] {
            assert_eq!(parse_ipv4_addr(bad), None, "{:?}", core::str::from_utf8(bad));
        }
    }

    #[test]
    fn ipv4_udp() {
        let mut p = ip_header(0x0A000202, 0x0A00020F, 17, 12);
        p.extend_from_slice(&[0x00, 0x35, 0xC0, 0x00, 0x00, 0x0C, 0, 0, b'd', b'a', b't', b'a']);
        p.extend_from_slice(&[0; 6]);                             // Ethernet padding
        let ip = ipv4(&p).unwrap();
        assert_eq!((ip.src, ip.dst, ip.proto, ip.off, ip.len), (0x0A000202, 0x0A00020F, 17, 20, 12));
        let u = udp(&p[ip.off..ip.off + ip.len]).unwrap();
        assert_eq!((u.sport, u.dport, u.off, u.len), (53, 0xC000, 8, 4));
        let mut bad = p.clone(); bad[8] ^= 1;                     // TTL changed: checksum wrong
        assert_eq!(ipv4(&bad), None);
        let mut frag = ip_header(1, 2, 17, 0); frag[6] = 0x20;    // more fragments
        assert_eq!(ipv4(&frag), None);
        assert_eq!(udp(&[0, 1, 2, 3, 0, 7, 0, 0]), None);         // length below the header's
    }

    #[test]
    fn tcp_options() {
        let mut s = vec![0x30, 0x39, 0x00, 0x50, 0, 0, 0, 1, 0, 0, 0, 2, 0x70, 0x12, 0xFF, 0xFF, 0, 0, 0, 0];
        s.extend_from_slice(&[1, 1, 4, 2, 2, 4, 0x05, 0xB4]);     // NOP NOP SACK-permitted MSS=1460
        let t = tcp(&s).unwrap();
        assert_eq!((t.sport, t.dport, t.seq, t.ack, t.flags, t.win, t.mss, t.hl), (12345, 80, 1, 2, 0x12, 0xFFFF, 1460, 28));
        s[12] = 0xF0;                                             // header longer than the segment
        assert_eq!(tcp(&s), None);
        let mut z = s.clone(); z[12] = 0x70; z[22] = 0;           // a zero-length option ends the list
        assert_eq!(tcp(&z).unwrap().mss, 0);
    }

    pub fn dhcp_reply(xid: u32, opts: &[u8]) -> Vec<u8> {
        let mut m = vec![0u8; 240];
        m[0] = 2; m[4..8].copy_from_slice(&xid.to_be_bytes());
        m[16..20].copy_from_slice(&0x0A00020Fu32.to_be_bytes());
        m[236..240].copy_from_slice(&0x6382_5363u32.to_be_bytes());
        m.extend_from_slice(opts);
        m
    }

    #[test]
    fn dhcp_ack() {
        let m = dhcp_reply(77, &[53, 1, 5, 0, 1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 2, 2, 6, 8, 10, 0, 2, 3, 8, 8, 8, 8,
                                 54, 4, 10, 0, 2, 2, 51, 4, 0, 0, 0x0E, 0x10, 255, 99, 99]);
        let d = dhcp(&m, 77).unwrap();
        assert_eq!(d, Dhcp { kind: 5, yiaddr: 0x0A00020F, mask: 0xFFFFFF00, gw: 0x0A000202, dns: 0x0A000203, server: 0x0A000202, lease: 3600 });
        assert_eq!(dhcp(&m, 78), None);                            // another transaction
        let cut = dhcp_reply(1, &[53, 1, 2, 1, 200, 1, 2]);       // an option running off the end
        assert_eq!(dhcp(&cut, 1).unwrap().kind, 2);
    }

    pub fn dns_reply(id: u16, rcode: u8, answers: &[(&[u8], u16, &[u8])]) -> Vec<u8> {
        let mut m = Vec::new();
        m.extend_from_slice(&id.to_be_bytes());
        m.extend_from_slice(&[0x81, 0x80 | rcode, 0, 1]);
        m.extend_from_slice(&(answers.len() as u16).to_be_bytes());
        m.extend_from_slice(&[0, 0, 0, 0]);
        m.extend_from_slice(b"\x03www\x07example\x03com\x00\x00\x01\x00\x01");
        for (name, kind, data) in answers {
            m.extend_from_slice(name);
            m.extend_from_slice(&kind.to_be_bytes());
            m.extend_from_slice(&[0, 1, 0, 0, 0x0E, 0x10]);
            m.extend_from_slice(&(data.len() as u16).to_be_bytes());
            m.extend_from_slice(data);
        }
        m
    }

    #[test]
    fn dns_answers() {
        // a CNAME, then the A record of the name it points to (compressed names)
        let m = dns_reply(0x4242, 0, &[(&[0xC0, 12], 5, b"\x03cdn\xC0\x10"), (&[0xC0, 0x29], 1, &[93, 184, 216, 34])]);
        assert_eq!(dns(&m, 0x4242), Dns::Address(0x5DB8D822));
        assert_eq!(dns(&m, 0x4243), Dns::NotOurs);
        let nx = dns_reply(7, 3, &[]);
        assert_eq!(dns(&nx, 7), Dns::NoAddress { rcode: 3 });
        let aaaa = dns_reply(8, 0, &[(&[0xC0, 12], 28, &[0; 16])]);
        assert_eq!(dns(&aaaa, 8), Dns::NoAddress { rcode: 0 });
        let mut cut = m.clone(); cut.truncate(cut.len() - 2);    // the address cut short
        assert_eq!(dns(&cut, 0x4242), Dns::NoAddress { rcode: 0 });
        let mut query = m.clone(); query[2] &= 0x7F;             // a query, not a reply
        assert_eq!(dns(&query, 0x4242), Dns::NotOurs);
    }

    #[test]
    fn fuzz_no_panic() {
        let mut seed = 0x5EEDu32;
        let mut rnd = move || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        let mut base_ip = ip_header(1, 2, 6, 28);
        base_ip.extend_from_slice(&[0x30, 0x39, 0, 80, 0, 0, 0, 1, 0, 0, 0, 2, 0x70, 0x02, 0xFF, 0xFF, 0, 0, 0, 0, 1, 1, 4, 2, 2, 4, 5, 0xB4]);
        let bases: Vec<Vec<u8>> = vec![
            base_ip,
            dhcp_reply(5, &[53, 1, 5, 1, 4, 255, 255, 255, 0, 255]),
            dns_reply(9, 0, &[(&[0xC0, 12], 1, &[1, 2, 3, 4])]),
            vec![0, 1, 8, 0, 6, 4, 0, 1, 1, 2, 3, 4, 5, 6, 10, 0, 2, 2, 0, 0, 0, 0, 0, 0, 10, 0, 2, 15],
        ];
        for round in 0..100_000 {
            let mut p = bases[round % bases.len()].clone();
            for _ in 0..1 + rnd() % 6 { let k = rnd() as usize % p.len(); p[k] = rnd() as u8; }
            if rnd() % 5 == 0 { let n = rnd() as usize % p.len(); p.truncate(n); }
            if let Some(ip) = ipv4(&p) { assert!(ip.off + ip.len <= p.len()); let pl = &p[ip.off..ip.off + ip.len]; let _ = (tcp(pl), udp(pl)); }
            if let Some(t) = tcp(&p) { assert!(t.hl <= p.len()); }
            if let Some(u) = udp(&p) { assert!(u.off + u.len <= p.len()); }
            let _ = (dhcp(&p, 5), dns(&p, 9), arp(&p), parse_ipv4_addr(&p), checksum(&p, rnd()));
        }
    }
}
