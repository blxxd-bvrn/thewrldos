//! USB descriptors and reports, the parts that read what a device sends (was
//! `src/usb/hid.c`, `src/usb/audioclass.c` and the descriptor walks in `src/usb/usb.c`): the
//! HID interfaces of a configuration, HID report descriptors and the input reports they
//! describe, the USB audio class playback stream, the class and endpoints of the interface a
//! storage or hub driver uses, and mass storage's status word, capacity and model.
//!
//! A device can send anything, so every read is checked against the buffer it came in;
//! where the C code read past a descriptor it now reads 0 past the configuration. Results
//! otherwise match it exactly. The C interfaces (`hid.h`, `uac.h`, `usb.h`) are kept, and
//! nothing here needs the kernel: `make check-hid` and `make check-audio` link the host
//! build. The host controller drivers, enumeration and the input state stay in C.

use core::ffi::{c_char, c_int};
use crate::CBool;

#[inline] fn at(d: &[u8], i: usize) -> u8 { d.get(i).copied().unwrap_or(0) }
#[inline] fn le16(d: &[u8], i: usize) -> u16 { at(d, i) as u16 | (at(d, i + 1) as u16) << 8 }
/// The descriptors of a configuration, as (offset, bytes from there on): each at least 2
/// bytes long and inside the buffer; the walk stops at the first that is not.
fn descriptors(cfg: &[u8], start: usize) -> impl Iterator<Item = (usize, &[u8])> {
    let mut i = start;
    core::iter::from_fn(move || {
        if i + 2 > cfg.len() || cfg[i] < 2 || i + cfg[i] as usize > cfg.len() { return None; }
        // the rest of the configuration: the C code read past short descriptors
        let (o, p) = (i, &cfg[i..]);
        i += cfg[i] as usize;
        Some((o, p))
    })
}

// ---------------------------------------------------------------- HID

/// hid_iface_t
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct HidIface { pub num: u8, pub subcls: u8, pub proto: u8, pub ep: u8, pub interval: u8, pub maxpkt: u16, pub rlen: u16 }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct HidMouse { pub present: CBool, pub id: c_int, pub len: c_int, pub btn_off: c_int, pub btn_bits: c_int, pub x_off: c_int, pub x_bits: c_int, pub y_off: c_int, pub y_bits: c_int, pub w_off: c_int, pub w_bits: c_int }
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct HidKeys { pub present: CBool, pub id: c_int, pub len: c_int, pub mod_off: c_int, pub key_off: c_int, pub key_bits: c_int, pub key_count: c_int, pub map_off: c_int, pub map_min: c_int, pub map_count: c_int }
/// hid_format_t
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct HidFormat { pub ids: CBool, pub mouse: HidMouse, pub keys: HidKeys }

/// The HID interfaces (alternate setting 0) of a configuration, with their interrupt IN
/// endpoints, at most `max`.
pub fn find_interfaces(cfg: &[u8], max: usize) -> alloc::vec::Vec<HidIface> {
    let mut out = alloc::vec::Vec::new();
    let mut open = false;
    let mut cur = HidIface::default();
    for (_, p) in descriptors(cfg, 0) {
        if at(p, 1) == 4 && at(p, 0) >= 9 {
            if open && cur.ep != 0 && out.len() < max { out.push(cur); }
            open = at(p, 5) == 3 && at(p, 3) == 0;
            cur = HidIface { num: at(p, 2), subcls: at(p, 6), proto: at(p, 7), ..Default::default() };
        } else if open && at(p, 1) == 0x21 && at(p, 0) >= 9 {
            if at(p, 6) == 0x22 { cur.rlen = le16(p, 7); }
        } else if open && at(p, 1) == 5 && at(p, 0) >= 7 && at(p, 2) & 0x80 != 0 && at(p, 3) & 3 == 3 && cur.ep == 0 {
            cur.ep = at(p, 2) & 0x0F;
            cur.maxpkt = le16(p, 4) & 0x7FF;
            cur.interval = at(p, 6);
        }
    }
    if open && cur.ep != 0 && out.len() < max { out.push(cur); }
    out
}
/// A bit field of a report, sign-extended if asked.
pub fn field(p: &[u8], off: i32, bits: i32, sign: bool) -> i32 {
    if bits <= 0 || bits > 32 || off < 0 { return 0; }
    let mut v: u32 = 0;
    for i in 0..bits {
        let b = off.wrapping_add(i);
        if b < 0 || (b >> 3) as usize >= p.len() { break; }
        v |= ((p[(b >> 3) as usize] >> (b & 7)) as u32 & 1) << i;
    }
    if sign && bits < 32 && v & (1 << (bits - 1)) != 0 { v |= !((1u32 << bits) - 1); }
    v as i32
}

const MAX_USAGES: usize = 16;
const MAX_IDS: usize = 16;
const MAX_STACK: usize = 4;
const APP_NONE: u8 = 0; const APP_KEYBOARD: u8 = 1; const APP_MOUSE: u8 = 2;
#[derive(Clone, Copy, Default)]
struct Globals { page: u32, rsize: i32, rcount: i32, id: i32 }
struct Parse {
    g: Globals, stack: [Globals; MAX_STACK], depth_stack: usize,
    usage: [u32; MAX_USAGES], wide: [bool; MAX_USAGES], nusage: usize,
    umin: u32, umax: u32, umin_wide: bool, umax_wide: bool, range: bool,
    ids: [i32; MAX_IDS], bits: [i32; MAX_IDS + 1], nids: usize,
}
const SPARE: usize = MAX_IDS;
impl Parse {
    /// A short usage belongs to the page in force at the main item.
    fn full(&self, u: u32, wide: bool) -> u32 { if wide { u } else { self.g.page << 16 | (u & 0xFFFF) } }
    fn field_usage(&self, f: usize) -> u32 {
        if self.nusage != 0 { let k = f.min(self.nusage - 1); return self.full(self.usage[k], self.wide[k]); }
        if self.range {
            let (lo, hi) = (self.full(self.umin, self.umin_wide), self.full(self.umax, self.umax_wide));
            let u = lo.wrapping_add(f as u32);
            return if u > hi { hi } else { u };
        }
        0
    }
    fn clear_locals(&mut self) { self.nusage = 0; self.range = false; self.umin = 0; self.umax = 0; }
    /// Where the next field of this report ID starts (a scratch slot past 16 IDs).
    fn bit_pos(&mut self, id: i32) -> usize {
        if let Some(i) = (0..self.nids).find(|&i| self.ids[i] == id) { return i; }
        if self.nids >= MAX_IDS { self.bits[SPARE] = 0; return SPARE; }
        self.ids[self.nids] = id; self.bits[self.nids] = 0;
        self.nids += 1;
        self.nids - 1
    }
}
/// Read a report descriptor: where the mouse's axes, buttons and wheel are, and the
/// keyboard's modifiers and keys.
pub fn parse_format(p: &[u8]) -> HidFormat {
    let mut s = Parse { g: Globals::default(), stack: [Globals::default(); MAX_STACK], depth_stack: 0, usage: [0; MAX_USAGES], wide: [false; MAX_USAGES], nusage: 0,
        umin: 0, umax: 0, umin_wide: false, umax_wide: false, range: false, ids: [0; MAX_IDS], bits: [0; MAX_IDS + 1], nids: 0 };
    let mut f = HidFormat::default();
    f.keys.mod_off = -1;
    let (mut depth, mut app) = (0i32, APP_NONE);
    let (mut x_id, mut y_id, mut btn_id, mut w_id, mut key_id) = (-1i32, -1i32, -1i32, -1i32, -1i32);
    let mut absolute = false;
    let n = p.len();
    let mut i = 0usize;
    while i < n {
        let b = p[i];
        if b == 0xFE {                                   // long item: skip it
            if i + 3 > n { break; }
            i += 3 + p[i + 1] as usize;
            continue;
        }
        let sz = if b & 3 == 3 { 4 } else { (b & 3) as usize };
        if i + 1 + sz > n { break; }
        let val = (0..sz).fold(0u32, |v, k| v | (p[i + 1 + k] as u32) << (8 * k));
        i += 1 + sz;
        match b & 0xFC {
            // global items
            0x04 => s.g.page = val & 0xFFFF,
            0x74 => s.g.rsize = val as i32,
            0x94 => s.g.rcount = val as i32,
            0x84 => { s.g.id = val as i32; f.ids = 1; }
            0xA4 => { if s.depth_stack < MAX_STACK { s.stack[s.depth_stack] = s.g; s.depth_stack += 1; } }
            0xB4 => { if s.depth_stack > 0 { s.depth_stack -= 1; s.g = s.stack[s.depth_stack]; } }
            // local items
            0x08 => { if s.nusage < MAX_USAGES { s.usage[s.nusage] = val; s.wide[s.nusage] = sz == 4; s.nusage += 1; } }
            0x18 => { s.umin = val; s.umin_wide = sz == 4; s.range = true; }
            0x28 => { s.umax = val; s.umax_wide = sz == 4; s.range = true; }
            // main items
            0xA0 => {                                    // Collection
                if depth == 0 && val == 1 {              // a top-level application
                    let u = if s.nusage != 0 { s.full(s.usage[s.nusage - 1], s.wide[s.nusage - 1]) } else { 0 };
                    app = if u == 0x10006 || u == 0x10007 { APP_KEYBOARD } else if u == 0x10002 || u == 0x10001 { APP_MOUSE } else { APP_NONE };
                }
                depth += 1;
                s.clear_locals();
            }
            0xC0 => {                                    // End Collection
                if depth > 0 { depth -= 1; }
                if depth == 0 { app = APP_NONE; }
                s.clear_locals();
            }
            0x80 => {                                    // Input
                let pos = s.bit_pos(s.g.id);
                let (size, count, id) = (s.g.rsize, s.g.rcount, s.g.id);
                let (constant, variable, relative) = (val & 1 != 0, val & 2 != 0, val & 4 != 0);
                if size <= 0 || count <= 0 || size > 32 || count > 4096 { s.clear_locals(); continue; }
                if !constant && app == APP_MOUSE {
                    for k in 0..count.min(64) {
                        let u = s.field_usage(k as usize);
                        let at_ = s.bits[pos].wrapping_add(k * size);
                        if u == 0x10030 || u == 0x10031 {
                            if !relative { absolute = true; }
                            else if u == 0x10030 && f.mouse.x_bits == 0 { f.mouse.x_off = at_; f.mouse.x_bits = size; x_id = id; }
                            else if u == 0x10031 && f.mouse.y_bits == 0 { f.mouse.y_off = at_; f.mouse.y_bits = size; y_id = id; }
                        } else if u == 0x10038 && relative && f.mouse.w_bits == 0 {
                            f.mouse.w_off = at_; f.mouse.w_bits = size; w_id = id;
                        } else if u >> 16 == 0x09 && variable && size == 1 {
                            if f.mouse.btn_bits == 0 { f.mouse.btn_off = at_; f.mouse.btn_bits = 1; btn_id = id; }
                            else if id == btn_id && at_ == f.mouse.btn_off.wrapping_add(f.mouse.btn_bits) { f.mouse.btn_bits += 1; }
                        }
                    }
                } else if !constant && app == APP_KEYBOARD && s.field_usage(0) >> 16 == 0x07 && (key_id < 0 || key_id == id) {
                    let first = (s.field_usage(0) & 0xFFFF) as i32;
                    if variable && size == 1 && first == 0xE0 && count == 8 && f.keys.mod_off < 0 {
                        f.keys.mod_off = s.bits[pos]; key_id = id;
                    } else if variable && size == 1 && s.range && s.nusage == 0 && f.keys.map_count == 0 {
                        let span = ((s.umax & 0xFFFF) as i32 - (s.umin & 0xFFFF) as i32) + 1;
                        f.keys.map_off = s.bits[pos]; f.keys.map_min = first;
                        f.keys.map_count = if count < span { count } else { span };
                        key_id = id;
                    } else if !variable && f.keys.key_count == 0 {
                        f.keys.key_off = s.bits[pos]; f.keys.key_bits = size;
                        f.keys.key_count = count.min(32);
                        key_id = id;
                    }
                }
                s.bits[pos] = s.bits[pos].wrapping_add(size * count);
                s.clear_locals();
            }
            0x90 | 0xB0 => s.clear_locals(),             // Output, Feature
            _ => {}
        }
    }
    f.mouse.present = (f.mouse.x_bits != 0 && f.mouse.y_bits != 0 && x_id == y_id && !absolute) as CBool;
    f.mouse.id = if x_id < 0 { 0 } else { x_id };
    if btn_id != x_id { f.mouse.btn_bits = 0; }
    if w_id != x_id { f.mouse.w_bits = 0; }
    f.keys.present = (f.keys.key_count > 0 || f.keys.map_count > 0) as CBool;
    f.keys.id = if key_id < 0 { 0 } else { key_id };
    let mp = s.bit_pos(f.mouse.id);
    f.mouse.len = s.bits[mp].wrapping_add(7) / 8;
    let kp = s.bit_pos(f.keys.id);
    f.keys.len = s.bits[kp].wrapping_add(7) / 8;
    f
}
pub const HID_REPORT_NONE: i32 = 0;
pub const HID_REPORT_KEYBOARD: i32 = 1;
pub const HID_REPORT_MOUSE: i32 = 2;
/// Which interface's format a report is in (see hid.h).
pub fn report_kind(f: &HidFormat, boot: bool, kind: i32, p: &[u8]) -> i32 {
    let n = p.len();
    if n < 1 { return HID_REPORT_NONE; }
    let (mut id, mut len) = (0i32, n as i32);
    if !boot && f.ids != 0 {
        if n < 2 { return HID_REPORT_NONE; }
        id = at(p, 0) as i32;
        len = n as i32 - 1;
    }
    if kind == HID_REPORT_KEYBOARD {
        if boot { return if n == 8 || (n == 9 && at(p, 0) != 0) { HID_REPORT_KEYBOARD } else { HID_REPORT_NONE }; }
        if f.keys.present == 0 { return HID_REPORT_NONE; }
        if f.ids != 0 && (id != f.keys.id || (f.keys.len != 0 && len != f.keys.len)) { return HID_REPORT_NONE; }
        return if len >= 8 { HID_REPORT_KEYBOARD } else { HID_REPORT_NONE };   // at least the boot layout's eight bytes
    }
    if boot { return if n == 3 || n == 4 { HID_REPORT_MOUSE } else { HID_REPORT_NONE }; }
    if f.mouse.present == 0 { return HID_REPORT_NONE; }
    if f.ids != 0 && (id != f.mouse.id || (f.mouse.len != 0 && len != f.mouse.len)) { return HID_REPORT_NONE; }
    if len >= 3 { HID_REPORT_MOUSE } else { HID_REPORT_NONE }
}
/// (dx, dy, buttons, wheel), HID's way up (Y down, wheel away from the user).
pub fn decode_mouse(f: &HidFormat, p: &[u8]) -> Option<(i32, i32, i32, i32)> {
    let m = &f.mouse;
    let bits = (p.len() as i32).wrapping_mul(8);
    if m.present == 0 || bits < m.x_off.wrapping_add(m.x_bits) || bits < m.y_off.wrapping_add(m.y_bits) { return None; }
    Some((field(p, m.x_off, m.x_bits, true), field(p, m.y_off, m.y_bits, true),
          if m.btn_bits != 0 { field(p, m.btn_off, m.btn_bits, false) } else { 0 },
          if m.w_bits != 0 { field(p, m.w_off, m.w_bits, true) } else { 0 }))
}
fn add_key(out: &mut [u8; 8], nkeys: &mut usize, u: i32) {
    if (0xE0..=0xE7).contains(&u) { out[0] |= 1 << (u - 0xE0); return; }
    if !(4..=0xFF).contains(&u) { return; }              // 1..3 are error codes
    if out[2..2 + *nkeys].contains(&(u as u8)) { return; }
    if *nkeys < 6 { out[2 + *nkeys] = u as u8; *nkeys += 1; }
}
/// The keys held down, in the boot keyboard layout; false on ErrorRollOver or no keyboard.
pub fn decode_keys(f: &HidFormat, p: &[u8]) -> (bool, [u8; 8]) {
    let mut out = [0u8; 8];
    let k = &f.keys;
    if k.present == 0 { return (false, out); }
    let mut nkeys = 0;
    if k.mod_off >= 0 { out[0] = field(p, k.mod_off, 8, false) as u8; }
    for i in 0..k.key_count {
        let u = field(p, k.key_off.wrapping_add(i.wrapping_mul(k.key_bits)), k.key_bits, false);
        if u == 1 { return (false, out); }               // ErrorRollOver: too many keys
        add_key(&mut out, &mut nkeys, u);
    }
    for i in 0..k.map_count { if field(p, k.map_off.wrapping_add(i), 1, false) != 0 { add_key(&mut out, &mut nkeys, k.map_min.wrapping_add(i)); } }
    (true, out)
}

// ---------------------------------------------------------------- USB audio class

/// uac_fmt_t
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct UacFmt {
    pub ac: c_int, pub stream: c_int, pub alt: c_int, pub ep: c_int, pub maxpkt: c_int, pub interval: c_int,
    pub rate: c_int, pub channels: c_int, pub frame_bytes: c_int, pub clock: c_int, pub link: c_int,
    pub class2: CBool, pub assumed: CBool,
}
/// 48 kHz if listed, else 44.1 kHz, else the first rate.
fn uac_rate(cfg: &[u8], freqs: usize, n: usize) -> i32 {
    let mut want = 0;
    for k in 0..n {
        let o = freqs + k * 3;
        let r = at(cfg, o) as i32 | (at(cfg, o + 1) as i32) << 8 | (at(cfg, o + 2) as i32) << 16;
        if r == 48000 { return r; }
        if want == 0 || r == 44100 { want = r; }
    }
    want
}
fn iso_out_ep(p: &[u8], o: &mut UacFmt) {
    let a = at(p, 2);
    if at(p, 3) & 3 == 1 && a & 0x80 == 0 && o.ep == 0 { o.ep = (a & 0x0F) as c_int; o.maxpkt = le16(p, 4) as c_int; o.interval = at(p, 6) as c_int; }
}
/// One alternate setting: its endpoint and its format.
fn alt_format(cfg: &[u8], start: usize, o: &mut UacFmt) -> bool {
    let (mut freqs, mut nfreq, mut ft, mut ch, mut sub, mut bits, mut have) = (None, 0usize, 0, 0, 0, 0, false);
    o.ep = 0; o.link = 0;
    for (i, p) in descriptors(cfg, start) {
        if at(p, 1) == 4 && i != start { break; }            // the next interface
        if at(p, 1) == 0x24 && at(p, 2) == 1 && at(p, 0) >= 4 && o.link == 0 { o.link = at(p, 3) as c_int; }
        else if at(p, 1) == 0x24 && at(p, 2) == 2 && at(p, 0) >= 7 {  // format type I
            ft = at(p, 3); ch = at(p, 4) as i32; sub = at(p, 5) as i32; bits = at(p, 6) as i32;
            if at(p, 0) >= 8 && at(p, 7) != 0 { nfreq = at(p, 7) as usize; if i + 8 + nfreq * 3 <= cfg.len() { freqs = Some(i + 8); } }
            have = true;
        } else if at(p, 1) == 5 && at(p, 0) >= 7 { iso_out_ep(p, o); }
    }
    if o.ep == 0 || !have || ft != 1 || ch < 1 || sub < 1 || bits < 1 || bits > sub * 8 { return false; }
    o.channels = ch; o.frame_bytes = sub; o.rate = 0;
    if let Some(fq) = freqs { o.rate = uac_rate(cfg, fq, nfreq); return o.rate != 0; }   // class 1 lists its rates
    o.class2 = 1;                                        // class 2: the clock sets it
    true
}
/// A streaming interface with an isochronous OUT endpoint but no readable format: assumed
/// 48 kHz stereo 16-bit.
fn alt_loose(cfg: &[u8], start: usize, o: &mut UacFmt) -> bool {
    o.ep = 0; o.link = 0;
    for (i, p) in descriptors(cfg, start) {
        if at(p, 1) == 4 && i != start { break; }
        if at(p, 1) == 0x24 && at(p, 2) == 1 && at(p, 0) >= 4 && o.link == 0 { o.link = at(p, 3) as c_int; }
        else if at(p, 1) == 5 && at(p, 0) >= 7 { iso_out_ep(p, o); }
    }
    if o.ep == 0 { return false; }
    o.channels = 2; o.frame_bytes = 2; o.rate = 0; o.class2 = 1; o.assumed = 1;
    true
}
/// The clock a class 2 stream takes its rate from.
fn clock_of(cfg: &[u8], link: c_int) -> c_int {
    let (mut clock, mut term, mut first) = (0, 0, 0);
    for (_, p) in descriptors(cfg, 0) {
        if at(p, 1) != 0x24 || at(p, 0) < 4 { continue; }
        if at(p, 2) == 10 && clock == 0 { clock = at(p, 3) as c_int; }            // clock source
        else if at(p, 2) == 3 && at(p, 0) >= 12 {                                // output terminal
            if first == 0 { first = at(p, 8) as c_int; }
            if link != 0 && at(p, 3) as c_int == link && term == 0 { term = at(p, 8) as c_int; }
        }
    }
    if term != 0 { term } else if first != 0 { first } else { clock }
}
// ---------------------------------------------------------------- the interface a driver uses

/// usb_cfg_t: the class, and the bulk and interrupt endpoints, of the interface a storage
/// or hub driver will use (the first one of the device's class).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct UsbCfg { pub cls: u8, pub subcls: u8, pub proto: u8, pub iface: c_int, pub ep_int: c_int, pub ep_in: c_int, pub ep_out: c_int, pub maxpkt_int: u16, pub maxpkt_in: u16, pub maxpkt_out: u16 }
/// `c` comes in with the device descriptor's class (0: take the first interface's).
pub fn parse_config(cfg: &[u8], c: &mut UsbCfg) {
    let (mut mine, mut have) = (false, false);
    for (_, p) in descriptors(cfg, 0) {
        if at(p, 1) == 4 && at(p, 0) >= 9 {
            if c.cls == 0 { c.cls = at(p, 5); c.subcls = at(p, 6); c.proto = at(p, 7); }
            mine = !have && at(p, 5) == c.cls && at(p, 3) == 0;
            if mine { c.iface = at(p, 2) as c_int; have = true; }
        } else if at(p, 1) == 5 && at(p, 0) >= 7 && mine {
            let (ep, attr, maxpkt) = ((at(p, 2) & 0x0F) as c_int, at(p, 3) & 3, le16(p, 4) & 0x7FF);
            if attr == 3 && at(p, 2) & 0x80 != 0 && c.ep_int == 0 { c.ep_int = ep; c.maxpkt_int = maxpkt; }
            else if attr == 2 {
                if at(p, 2) & 0x80 != 0 { c.ep_in = ep; c.maxpkt_in = maxpkt; } else { c.ep_out = ep; c.maxpkt_out = maxpkt; }
            }
        }
    }
}

// ---------------------------------------------------------------- the C interfaces

unsafe fn sl<'a>(p: *const u8, n: c_int) -> &'a [u8] { if p.is_null() || n <= 0 { &[] } else { core::slice::from_raw_parts(p, n as usize) } }
#[no_mangle]
pub unsafe extern "C" fn hid_find_interfaces(cfg: *const u8, len: c_int, out: *mut HidIface, max: c_int) -> c_int {
    let v = find_interfaces(sl(cfg, len), max.max(0) as usize);
    for (i, x) in v.iter().enumerate() { out.add(i).write(*x); }
    v.len() as c_int
}
#[no_mangle]
pub unsafe extern "C" fn hid_parse_format(f: *mut HidFormat, desc: *const u8, n: c_int) { f.write(parse_format(sl(desc, n))); }
#[no_mangle]
pub unsafe extern "C" fn hid_field(p: *const u8, n: c_int, off: c_int, bits: c_int, sign: CBool) -> c_int { field(sl(p, n), off, bits, sign != 0) }
#[no_mangle]
pub unsafe extern "C" fn hid_report_kind(f: *const HidFormat, boot: CBool, kind: c_int, p: *const u8, n: c_int) -> c_int { report_kind(&*f, boot != 0, kind, sl(p, n)) }
#[no_mangle]
pub unsafe extern "C" fn hid_decode_mouse(f: *const HidFormat, p: *const u8, n: c_int, dx: *mut c_int, dy: *mut c_int, buttons: *mut c_int, wheel: *mut c_int) -> CBool {
    match decode_mouse(&*f, sl(p, n)) { Some((a, b, c, d)) => { *dx = a; *dy = b; *buttons = c; *wheel = d; 1 } None => 0 }
}
#[no_mangle]
pub unsafe extern "C" fn hid_decode_keys(f: *const HidFormat, p: *const u8, n: c_int, out: *mut u8) -> CBool {
    let (ok, k) = decode_keys(&*f, sl(p, n));
    core::ptr::copy_nonoverlapping(k.as_ptr(), out, 8);
    ok as CBool
}
#[no_mangle]
pub unsafe extern "C" fn uac_parse(cfg: *const u8, len: c_int, o: *mut UacFmt) -> CBool {
    let (ok, f) = uac_parse_any(sl(cfg, len));
    o.write(f);
    ok as CBool
}
/// The playback stream of a configuration (see uac.h): whether one was found, and what
/// uac_parse leaves in its result either way.
pub fn uac_parse_any(cfg: &[u8]) -> (bool, UacFmt) {
    let mut o = UacFmt { ac: -1, stream: -1, ..Default::default() };
    let r = uac_scan(cfg, &mut o);
    (r, o)
}
fn uac_scan(cfg: &[u8], o: &mut UacFmt) -> bool {
    let audio = |p: &[u8]| p[1] == 4 && p[0] >= 9 && p[5] == 1;
    for (i, p) in descriptors(cfg, 0) {
        if !audio(p) { continue; }
        if p[6] == 1 && o.ac < 0 { o.ac = p[2] as c_int; continue; }
        if p[6] != 2 || p[4] == 0 { continue; }
        let mut one = *o;
        if !alt_format(cfg, i, &mut one) { continue; }
        one.stream = p[2] as c_int; one.alt = p[3] as c_int;
        if o.stream < 0 || (one.class2 != 0 && o.class2 == 0) { *o = one; }
    }
    if o.ac >= 0 && o.stream < 0 {
        for (i, p) in descriptors(cfg, 0) {
            if !audio(p) || p[6] != 2 || p[4] == 0 { continue; }
            let mut one = *o;
            if !alt_loose(cfg, i, &mut one) { continue; }
            one.stream = p[2] as c_int; one.alt = p[3] as c_int;
            *o = one;
        }
    }
    if o.ac < 0 || o.stream < 0 || o.ep == 0 { return false; }
    if o.class2 != 0 { o.clock = clock_of(cfg, o.link); }
    true
}
/// void usb_parse_config(usb_cfg_t *c, const uint8_t *cfg, int len)
#[no_mangle]
pub unsafe extern "C" fn usb_parse_config(c: *mut UsbCfg, cfg: *const u8, len: c_int) { parse_config(sl(cfg, len), &mut *c); }
/// int usb_csw_check(const uint8_t *csw, int n, uint32_t tag): 0 passed, -2 the device
/// refused the command (retry), -1 no valid status.
#[no_mangle]
pub unsafe extern "C" fn usb_csw_check(csw: *const u8, n: c_int, tag: u32) -> c_int {
    let c = sl(csw, n);
    if c.len() < 13 { return -1; }
    let rd = |o: usize| u32::from_le_bytes([c[o], c[o + 1], c[o + 2], c[o + 3]]);
    let (sig, t, status) = (rd(0), rd(4), c[12]);
    if sig != 0x5342_5355 || t != tag || status != 0 { return if sig == 0x5342_5355 && t == tag { -2 } else { -1 }; }
    0
}
/// bool usb_read_capacity(const uint8_t cap[8], uint32_t *blocks, uint32_t *bsize): false for
/// a block size the disk layer cannot use.
#[no_mangle]
pub unsafe extern "C" fn usb_read_capacity(cap: *const u8, blocks: *mut u32, bsize: *mut u32) -> CBool {
    let c = sl(cap, 8);
    let (last, bs) = (u32::from_be_bytes([c[0], c[1], c[2], c[3]]), u32::from_be_bytes([c[4], c[5], c[6], c[7]]));
    *bsize = bs;
    *blocks = last.wrapping_add(1);
    matches!(bs, 512 | 1024 | 2048 | 4096) as CBool
}
/// void usb_inquiry_model(const uint8_t inq[36], char *out, int cap): vendor and product.
#[no_mangle]
pub unsafe extern "C" fn usb_inquiry_model(inq: *const u8, out: *mut c_char, cap: c_int) {
    if out.is_null() || cap <= 0 { return; }
    let q = sl(inq, 36);
    let s = |a: usize, n: usize| { let f = &q[a..a + n]; &f[..f.iter().position(|&x| x == 0).unwrap_or(n)] };
    let mut v = alloc::vec::Vec::new();
    v.extend_from_slice(s(8, 8)); v.extend_from_slice(s(16, 16));
    let n = v.len().min(cap as usize - 1);
    core::ptr::copy_nonoverlapping(v.as_ptr(), out as *mut u8, n);
    *out.add(n) = 0;
}

#[cfg(test)]
mod tests {
    use super::*;
    // the boot keyboard and a typical wheel mouse
    const KBD: &[u8] = &[0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
        0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x06,0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0];
    const MOUSE: &[u8] = &[0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,
        0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,0x81,0x06,0xC0,0xC0];
    #[test]
    fn hid() {
        let k = parse_format(KBD);
        assert_eq!((k.keys.present, k.keys.mod_off, k.keys.key_off, k.keys.key_count, k.keys.len), (1, 0, 16, 6, 8));
        let (ok, keys) = decode_keys(&k, &[0x02, 0, 0x04, 0x05, 0, 0, 0, 0]);
        assert!(ok && keys == [0x02, 0, 4, 5, 0, 0, 0, 0]);
        let m = parse_format(MOUSE);
        assert_eq!((m.mouse.present, m.mouse.btn_bits, m.mouse.x_off, m.mouse.y_off, m.mouse.w_off, m.mouse.len), (1, 3, 8, 16, 24, 4));
        assert_eq!(decode_mouse(&m, &[0x01, 0xFF, 0x02, 0x01]), Some((-1, 2, 1, 1)));
        assert_eq!(field(&[0xF0], 4, 4, true), -1);
    }
    #[test]
    fn junk_does_not_panic() {
        let mut seed = 0xC0FF_EE11u32;
        let mut next = || { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; seed };
        for _ in 0..20000 {
            let d: alloc::vec::Vec<u8> = (0..next() % 300).map(|_| next() as u8).collect();
            let f = parse_format(&d);
            let _ = (decode_mouse(&f, &d), decode_keys(&f, &d), report_kind(&f, false, 1, &d), find_interfaces(&d, 4), uac_parse_any(&d));
            let mut c = UsbCfg::default(); parse_config(&d, &mut c);
        }
    }
}
