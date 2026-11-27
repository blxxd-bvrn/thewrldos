//! PPMd variant H, the text model of RAR 2.9 - 4.x archives, with RAR's range decoder.
//!
//! Dmitry Shkarin's PPMd algorithm (public domain) as RAR uses it: a context model of
//! up to 64 bytes of history, kept in a sub-allocator of 12-byte units inside one block of
//! memory whose size the archive chooses (1 to 256 MB). Every pointer is a 32-bit offset
//! into that block; offset 0 is "none". The allocation order matters: the encoder and the
//! decoder must run out of memory (and restart the model) at the same symbol, so the
//! allocator follows the original one step for step.
use alloc::vec::Vec;
use crate::unarc::Bits;

const UNIT: u32 = 12;
const N_INDEXES: usize = 38;
const MAX_FREQ: u32 = 124;
const INT_BITS: u32 = 7;
const PERIOD_BITS: u32 = 7;
const BIN_SCALE: u32 = 1 << (INT_BITS + PERIOD_BITS);
const MAX_O: usize = 64;
const TOP: u32 = 1 << 24;
const BOT: u32 = 1 << 15;
const EXP_ESCAPE: [u8; 16] = [25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2];
const INIT_BIN_ESC: [u16; 8] = [0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051];
/// Where the heap's units start: offset 0 is "none", and its unit is the free-block list head
const START: u32 = UNIT;

#[derive(Clone, Copy, Default)]
struct See { summ: u16, shift: u8, count: u8 }
impl See {
    fn mean(&mut self) -> u32 {
        let r = (self.summ >> self.shift) as u32;
        self.summ = self.summ.wrapping_sub(r as u16);
        r + (r == 0) as u32
    }
    fn update(&mut self) {
        if (self.shift as u32) < PERIOD_BITS {
            self.count = self.count.wrapping_sub(1);
            if self.count == 0 { self.summ = self.summ.wrapping_add(self.summ); self.count = (3u32 << self.shift) as u8; self.shift += 1; }
        }
    }
}

pub struct Model {
    h: Vec<u8>,
    size: u32,
    text: u32, units_start: u32, lo: u32, hi: u32, glue: u32,
    free: [u32; N_INDEXES],
    i2u: [u8; N_INDEXES], u2i: [u8; 128],
    ns2indx: [u8; 256], ns2bs: [u8; 256], hb2flag: [u8; 256],
    see: [[See; 16]; 25], dummy: See,
    bin: [[u16; 64]; 128],
    min: u32, max: u32, found: u32,
    order_fall: u32, init_esc: u32, prev_success: u32, max_order: u32, hibits: u32,
    run_length: i32, init_rl: i32,
    low: u32, code: u32, range: u32,
    ready: bool,
}

impl Model {
    pub fn new() -> Model {
        let mut m = Model {
            h: Vec::new(), size: 0, text: 0, units_start: 0, lo: 0, hi: 0, glue: 0,
            free: [0; N_INDEXES], i2u: [0; N_INDEXES], u2i: [0; 128],
            ns2indx: [0; 256], ns2bs: [0; 256], hb2flag: [0; 256],
            see: [[See::default(); 16]; 25], dummy: See::default(), bin: [[0; 64]; 128],
            min: 0, max: 0, found: 0, order_fall: 0, init_esc: 0, prev_success: 0, max_order: 0, hibits: 0,
            run_length: 0, init_rl: 0, low: 0, code: 0, range: 0, ready: false,
        };
        // Indx2Units: 1,2,3,4, 6,8,10,12, 15,18,21,24, 28,32,...,128
        let mut k = 1u32;
        for idx in 0..N_INDEXES {
            m.i2u[idx] = k as u8;
            k += if idx < 3 { 1 } else if idx < 7 { 2 } else if idx < 11 { 3 } else { 4 };
        }
        let mut i = 0usize;
        for k in 0..128 {
            if (m.i2u[i] as usize) < k + 1 { i += 1; }
            m.u2i[k] = i as u8;
        }
        m.ns2bs[0] = 0; m.ns2bs[1] = 2;
        for x in &mut m.ns2bs[2..11] { *x = 4; }
        for x in &mut m.ns2bs[11..] { *x = 6; }
        for i in 0..3 { m.ns2indx[i] = i as u8; }
        let (mut mm, mut k, mut step) = (3u32, 1u32, 1u32);
        for i in 3..256 {
            m.ns2indx[i] = mm as u8;
            k -= 1;
            if k == 0 { step += 1; k = step; mm += 1; }
        }
        for i in 0x40..0x100 { m.hb2flag[i] = 8; }
        m.dummy.shift = PERIOD_BITS as u8;
        m
    }

    // ---- heap access (out-of-range reads give 0, writes are dropped: no panics on damaged data)
    #[inline] fn r8(&self, o: u32) -> u32 { self.h.get(o as usize).map_or(0, |&b| b as u32) }
    #[inline] fn w8(&mut self, o: u32, v: u32) { if let Some(b) = self.h.get_mut(o as usize) { *b = v as u8; } }
    #[inline] fn r16(&self, o: u32) -> u32 { self.r8(o) | self.r8(o + 1) << 8 }
    #[inline] fn w16(&mut self, o: u32, v: u32) { self.w8(o, v); self.w8(o + 1, v >> 8); }
    #[inline] fn r32(&self, o: u32) -> u32 { self.r16(o) | self.r16(o + 2) << 16 }
    #[inline] fn w32(&mut self, o: u32, v: u32) { self.w16(o, v); self.w16(o + 2, v >> 16); }
    fn copy(&mut self, dst: u32, src: u32, n: u32) {
        let (d, s, n) = (dst as usize, src as usize, n as usize);
        if s + n <= self.h.len() && d + n <= self.h.len() { self.h.copy_within(s..s + n, d); }
    }

    // context: NumStats u16 @0, SummFreq u16 @2, Stats @4, Suffix @8; its one state sits @2
    #[inline] fn ns(&self, c: u32) -> u32 { self.r16(c) }
    #[inline] fn set_ns(&mut self, c: u32, v: u32) { self.w16(c, v) }
    #[inline] fn summ(&self, c: u32) -> u32 { self.r16(c + 2) }
    #[inline] fn set_summ(&mut self, c: u32, v: u32) { self.w16(c + 2, v) }
    #[inline] fn stats(&self, c: u32) -> u32 { self.r32(c + 4) }
    #[inline] fn set_stats(&mut self, c: u32, v: u32) { self.w32(c + 4, v) }
    #[inline] fn suffix(&self, c: u32) -> u32 { self.r32(c + 8) }
    #[inline] fn set_suffix(&mut self, c: u32, v: u32) { self.w32(c + 8, v) }
    #[inline] fn one(c: u32) -> u32 { c + 2 }
    // state: Symbol @0, Freq @1, Successor @2 (6 bytes)
    #[inline] fn sym(&self, s: u32) -> u32 { self.r8(s) }
    #[inline] fn freq(&self, s: u32) -> u32 { self.r8(s + 1) }
    #[inline] fn set_freq(&mut self, s: u32, v: u32) { self.w8(s + 1, v) }
    #[inline] fn succ(&self, s: u32) -> u32 { self.r32(s + 2) }
    #[inline] fn set_succ(&mut self, s: u32, v: u32) { self.w32(s + 2, v) }
    fn get_state(&self, s: u32) -> [u8; 6] { let mut t = [0u8; 6]; for (k, b) in t.iter_mut().enumerate() { *b = self.r8(s + k as u32) as u8; } t }
    fn put_state(&mut self, s: u32, t: [u8; 6]) { for (k, &b) in t.iter().enumerate() { self.w8(s + k as u32, b as u32); } }
    fn swap(&mut self, a: u32, b: u32) { let (x, y) = (self.get_state(a), self.get_state(b)); self.put_state(a, y); self.put_state(b, x); }
    fn valid(&self, c: u32) -> bool { c > self.text && (c as usize) + UNIT as usize <= self.h.len() }

    // ---- sub-allocator
    fn u2b(nu: u32) -> u32 { nu * UNIT }
    fn insert_node(&mut self, p: u32, indx: usize) { let head = self.free[indx]; self.w32(p + 4, head); self.free[indx] = p; }
    fn remove_node(&mut self, indx: usize) -> u32 { let p = self.free[indx]; self.free[indx] = self.r32(p + 4); p }
    fn split_block(&mut self, p: u32, old: usize, new: usize) {
        let mut udiff = self.i2u[old] as u32 - self.i2u[new] as u32;
        let mut p = p + Self::u2b(self.i2u[new] as u32);
        let mut i = self.u2i[(udiff - 1) as usize] as usize;
        if self.i2u[i] as u32 != udiff {
            i -= 1;
            self.insert_node(p, i);
            let k = self.i2u[i] as u32;
            p += Self::u2b(k);
            udiff -= k;
        }
        let ix = self.u2i[(udiff - 1) as usize] as usize;
        self.insert_node(p, ix);
    }
    // free blocks while gluing: Stamp u16 @0 (0xFFFF), NU u16 @2, next @4, prev @8; head at 0
    fn glue_free_blocks(&mut self) {
        if self.lo != self.hi { self.w16(self.lo, 0); }
        self.w32(4, 0); self.w32(8, 0);
        for i in 0..N_INDEXES {
            while self.free[i] != 0 {
                let p = self.remove_node(i);
                // insert after the head
                let next = self.r32(4);
                self.w32(p + 4, next); self.w32(p + 8, 0);
                self.w32(next + 8, p); self.w32(4, p);
                self.w16(p, 0xFFFF);
                self.w16(p + 2, self.i2u[i] as u32);
            }
        }
        let mut p = self.r32(4);
        while p != 0 {
            loop {
                let nu = self.r16(p + 2);
                let p1 = p + nu * UNIT;
                if self.r16(p1) == 0xFFFF && nu + self.r16(p1 + 2) < 0x10000 {
                    let (n1, pr1) = (self.r32(p1 + 4), self.r32(p1 + 8));
                    self.w32(pr1 + 4, n1); self.w32(n1 + 8, pr1);
                    self.w16(p + 2, nu + self.r16(p1 + 2));
                } else { break; }
            }
            p = self.r32(p + 4);
        }
        loop {
            let mut p = self.r32(4);
            if p == 0 { break; }
            let (n, pr) = (self.r32(p + 4), self.r32(p + 8));
            self.w32(pr + 4, n); self.w32(n + 8, pr);
            let mut sz = self.r16(p + 2);
            while sz > 128 { self.insert_node(p, N_INDEXES - 1); p += Self::u2b(128); sz -= 128; }
            if sz == 0 { continue; }
            let mut i = self.u2i[(sz - 1) as usize] as usize;
            if self.i2u[i] as u32 != sz {
                i -= 1;
                let k = sz - self.i2u[i] as u32;
                self.insert_node(p + Self::u2b(sz - k), (k - 1) as usize);
            }
            self.insert_node(p, i);
        }
    }
    fn alloc_units_rare(&mut self, indx: usize) -> u32 {
        if self.glue == 0 {
            self.glue = 255;
            self.glue_free_blocks();
            if self.free[indx] != 0 { return self.remove_node(indx); }
        }
        let mut i = indx;
        loop {
            i += 1;
            if i == N_INDEXES {
                self.glue = self.glue.wrapping_sub(1);
                let n = Self::u2b(self.i2u[indx] as u32);
                if self.units_start - self.text > n { self.units_start -= n; return self.units_start; }
                return 0;
            }
            if self.free[i] != 0 { break; }
        }
        let r = self.remove_node(i);
        self.split_block(r, i, indx);
        r
    }
    fn alloc_units(&mut self, nu: u32) -> u32 {
        let indx = self.u2i[(nu - 1) as usize] as usize;
        if self.free[indx] != 0 { return self.remove_node(indx); }
        let n = Self::u2b(self.i2u[indx] as u32);
        if self.lo + n <= self.hi { let r = self.lo; self.lo += n; return r; }
        self.alloc_units_rare(indx)
    }
    fn alloc_context(&mut self) -> u32 {
        if self.hi != self.lo { self.hi -= UNIT; return self.hi; }
        if self.free[0] != 0 { return self.remove_node(0); }
        self.alloc_units_rare(0)
    }
    fn expand_units(&mut self, old: u32, old_nu: u32) -> u32 {
        let i0 = self.u2i[(old_nu - 1) as usize] as usize;
        let i1 = self.u2i[old_nu as usize] as usize;
        if i0 == i1 { return old; }
        let p = self.alloc_units(old_nu + 1);
        if p != 0 { self.copy(p, old, Self::u2b(old_nu)); self.insert_node(old, i0); }
        p
    }
    fn shrink_units(&mut self, old: u32, old_nu: u32, new_nu: u32) -> u32 {
        let i0 = self.u2i[(old_nu - 1) as usize] as usize;
        let i1 = self.u2i[(new_nu - 1) as usize] as usize;
        if i0 == i1 { return old; }
        if self.free[i1] != 0 {
            let p = self.remove_node(i1);
            self.copy(p, old, Self::u2b(new_nu));
            self.insert_node(old, i0);
            return p;
        }
        self.split_block(old, i0, i1);
        old
    }
    fn free_units(&mut self, p: u32, nu: u32) { let i = self.u2i[(nu - 1) as usize] as usize; self.insert_node(p, i); }

    // ---- the model
    fn restart(&mut self) {
        self.free = [0; N_INDEXES];
        self.text = START;
        let size2 = UNIT * (self.size / 8 / UNIT * 7);
        let size1 = self.size - size2;
        self.lo = START + size1;
        self.units_start = self.lo;
        self.hi = self.lo + size2;
        self.glue = 0;
        self.init_rl = -(self.max_order.min(12) as i32) - 1;
        let c = self.alloc_context();
        self.min = c; self.max = c;
        self.set_suffix(c, 0);
        self.order_fall = self.max_order;
        self.set_ns(c, 256);
        self.set_summ(c, 257);
        let st = self.alloc_units(128);
        self.set_stats(c, st);
        self.found = st;
        self.run_length = self.init_rl;
        self.prev_success = 0;
        for i in 0..256u32 { let s = st + i * 6; self.w8(s, i); self.w8(s + 1, 1); self.set_succ(s, 0); }
        for i in 0..128 {
            for k in 0..8 {
                let v = (BIN_SCALE - INIT_BIN_ESC[k] as u32 / (i as u32 + 2)) as u16;
                let mut m = 0;
                while m < 64 { self.bin[i][k + m] = v; m += 8; }
            }
        }
        for i in 0..25 {
            for k in 0..16 { self.see[i][k] = See { summ: ((5 * i as u32 + 10) << (PERIOD_BITS - 4)) as u16, shift: (PERIOD_BITS - 4) as u8, count: 4 }; }
        }
    }

    /// Starts a PPM block: model settings from the stream, then the range decoder.
    pub fn init(&mut self, b: &mut Bits, esc: &mut u32) -> Result<(), ()> {
        let mo = b.byte_in();
        let reset = mo & 0x20 != 0;
        let mb = if reset { b.byte_in() } else { if !self.ready { return Err(()); } 0 };
        if mo & 0x40 != 0 { *esc = b.byte_in(); }
        self.low = 0; self.code = 0; self.range = u32::MAX;
        for _ in 0..4 { self.code = (self.code << 8) | b.byte_in(); }
        if reset {
            let mut order = (mo & 0x1f) + 1;
            if order > 16 { order = 16 + (order - 16) * 3; }
            if order == 1 { self.ready = false; return Err(()); }
            let size = (mb + 1) << 20;
            if self.size != size || self.h.is_empty() {
                self.h = Vec::new();
                let total = (START + size + 2 * UNIT) as usize;
                if self.h.try_reserve_exact(total).is_err() { self.ready = false; self.size = 0; return Err(()); }
                self.h.resize(total, 0);
                self.size = size;
            }
            self.max_order = order;
            self.restart();
            self.ready = true;
        }
        if self.ready { Ok(()) } else { Err(()) }
    }

    fn create_successors(&mut self, skip: bool, p1: u32) -> u32 {
        let mut pc = self.min;
        let up_branch = self.succ(self.found);
        let mut ps = [0u32; MAX_O];
        let mut n = 0usize;
        let fsym = self.sym(self.found);
        let mut no_loop = false;
        if !skip {
            ps[n] = self.found; n += 1;
            if self.suffix(pc) == 0 { no_loop = true; }
        }
        if !no_loop {
            let mut first = p1 != 0;
            loop {
                let p;
                if first {
                    first = false;
                    p = p1;
                    pc = self.suffix(pc);
                } else {
                    pc = self.suffix(pc);
                    if !self.valid(pc) { return 0; }
                    if self.ns(pc) != 1 {
                        let mut s = self.stats(pc);
                        let mut guard = 0;
                        while self.sym(s) != fsym { s += 6; guard += 1; if guard > 256 { return 0; } }
                        p = s;
                    } else { p = Self::one(pc); }
                }
                if self.succ(p) != up_branch { pc = self.succ(p); break; }
                if n >= MAX_O { return 0; }
                ps[n] = p; n += 1;
                if self.suffix(pc) == 0 { break; }
            }
        }
        if n == 0 { return pc; }
        let up_sym = self.r8(up_branch);
        let up_succ = up_branch + 1;
        let up_freq;
        if self.ns(pc) != 1 {
            if pc <= self.text { return 0; }
            let mut s = self.stats(pc);
            let mut guard = 0;
            while self.sym(s) != up_sym { s += 6; guard += 1; if guard > 256 { return 0; } }
            let cf = self.freq(s).wrapping_sub(1);
            let s0 = self.summ(pc).wrapping_sub(self.ns(pc)).wrapping_sub(cf);
            up_freq = 1 + if 2 * cf <= s0 { (5 * cf > s0) as u32 } else { (2 * cf + 3 * s0 - 1) / (2 * s0).max(1) };
        } else { up_freq = self.freq(Self::one(pc)); }
        while n > 0 {
            n -= 1;
            let c = self.alloc_context();
            if c == 0 { return 0; }
            self.set_ns(c, 1);
            let o = Self::one(c);
            self.w8(o, up_sym); self.w8(o + 1, up_freq); self.set_succ(o, up_succ);
            self.set_suffix(c, pc);
            self.set_succ(ps[n], c);
            pc = c;
        }
        pc
    }

    fn update_model(&mut self) {
        let fs = self.found;
        let (fsym, ffreq, fsucc0) = (self.sym(fs), self.freq(fs), self.succ(fs));
        let mut fsucc = fsucc0;
        let mut p = 0u32;
        let pc = self.suffix(self.min);
        if ffreq < MAX_FREQ / 4 && pc != 0 {
            if self.ns(pc) != 1 {
                p = self.stats(pc);
                if self.sym(p) != fsym {
                    let mut guard = 0;
                    loop { p += 6; guard += 1; if self.sym(p) == fsym || guard > 256 { break; } }
                    if self.freq(p) >= self.freq(p - 6) { self.swap(p, p - 6); p -= 6; }
                }
                if self.freq(p) < MAX_FREQ - 9 { self.set_freq(p, self.freq(p) + 2); self.set_summ(pc, self.summ(pc) + 2); }
            } else {
                p = Self::one(pc);
                if self.freq(p) < 32 { self.set_freq(p, self.freq(p) + 1); }
            }
        }
        if self.order_fall == 0 {
            let c = self.create_successors(true, p);
            self.min = c; self.max = c;
            if c == 0 { self.restart(); return; }
            self.set_succ(self.found, c);
            return;
        }
        self.w8(self.text, fsym);
        self.text += 1;
        let mut successor = self.text;
        if self.text >= self.units_start { self.restart(); return; }
        if fsucc != 0 {
            if fsucc <= self.text {
                fsucc = self.create_successors(false, p);
                if fsucc == 0 { self.restart(); return; }
            }
            self.order_fall -= 1;
            if self.order_fall == 0 {
                successor = fsucc;
                if self.max != self.min { self.text -= 1; }
            }
        } else {
            self.set_succ(self.found, successor);
            fsucc = self.min;
        }
        let ns = self.ns(self.min);
        let s0 = self.summ(self.min).wrapping_sub(ns).wrapping_sub(ffreq.wrapping_sub(1));
        let mut pc = self.max;
        while pc != self.min {
            let mut ns1 = self.ns(pc);
            if ns1 != 1 {
                if ns1 & 1 == 0 {
                    let st = self.expand_units(self.stats(pc), ns1 >> 1);
                    if st == 0 { self.restart(); return; }
                    self.set_stats(pc, st);
                }
                let sf = self.summ(pc);
                self.set_summ(pc, sf + (2 * ns1 < ns) as u32 + 2 * ((4 * ns1 <= ns) as u32 & (sf <= 8 * ns1) as u32));
            } else {
                let s = self.alloc_units(1);
                if s == 0 { self.restart(); return; }
                let t = self.get_state(Self::one(pc));
                self.put_state(s, t);
                self.set_stats(pc, s);
                let f = self.freq(s);
                self.set_freq(s, if f < MAX_FREQ / 4 - 1 { f + f } else { MAX_FREQ - 4 });
                self.set_summ(pc, self.freq(s) + self.init_esc + (ns > 3) as u32);
            }
            let mut cf = 2 * ffreq * (self.summ(pc) + 6);
            let sf = s0.wrapping_add(self.summ(pc));
            if cf < 6 * sf {
                cf = 1 + (cf > sf) as u32 + (cf >= 4 * sf) as u32;
                self.set_summ(pc, self.summ(pc) + 3);
            } else {
                cf = 4 + (cf >= 9 * sf) as u32 + (cf >= 12 * sf) as u32 + (cf >= 15 * sf) as u32;
                self.set_summ(pc, self.summ(pc) + cf);
            }
            let s = self.stats(pc) + ns1 * 6;
            self.set_succ(s, successor);
            self.w8(s, fsym);
            self.set_freq(s, cf);
            ns1 += 1;
            self.set_ns(pc, ns1);
            pc = self.suffix(pc);
            if pc == 0 { break; }
        }
        self.max = fsucc; self.min = fsucc;
    }

    fn rescale(&mut self) {
        let mc = self.min;
        let old_ns = self.ns(mc);
        let stats = self.stats(mc);
        let mut p = self.found;
        while p > stats { self.swap(p, p - 6); p -= 6; }
        self.set_freq(stats, self.freq(stats) + 4);
        self.set_summ(mc, self.summ(mc) + 4);
        let mut esc = self.summ(mc).wrapping_sub(self.freq(p));
        let adder = (self.order_fall != 0) as u32;
        let f0 = (self.freq(p) + adder) >> 1;
        self.set_freq(p, f0);
        let mut sum = f0;
        let mut i = old_ns - 1;
        while i > 0 {
            p += 6;
            esc = esc.wrapping_sub(self.freq(p));
            let f = (self.freq(p) + adder) >> 1;
            self.set_freq(p, f);
            sum += f;
            if f > self.freq(p - 6) {
                let tmp = self.get_state(p);
                let mut p1 = p;
                loop {
                    let prev = self.get_state(p1 - 6);
                    self.put_state(p1, prev);
                    p1 -= 6;
                    if p1 == stats || tmp[1] as u32 <= self.freq(p1 - 6) { break; }
                }
                self.put_state(p1, tmp);
            }
            i -= 1;
        }
        if self.freq(p) == 0 {
            let mut i = 0;
            loop { i += 1; p -= 6; if self.freq(p) != 0 || p <= stats { break; } }
            esc = esc.wrapping_add(i);
            let new_ns = old_ns - i;
            self.set_ns(mc, new_ns);
            if new_ns == 1 {
                let mut tmp = self.get_state(stats);
                loop { tmp[1] -= tmp[1] >> 1; esc >>= 1; if esc <= 1 { break; } }
                self.free_units(stats, (old_ns + 1) >> 1);
                let o = Self::one(mc);
                self.put_state(o, tmp);
                self.found = o;
                return;
            }
        }
        esc -= esc >> 1;
        self.set_summ(mc, sum + esc);
        let (n0, n1) = ((old_ns + 1) >> 1, (self.ns(mc) + 1) >> 1);
        if n0 != n1 { let st = self.shrink_units(stats, n0, n1); self.set_stats(mc, st); }
        self.found = self.stats(mc);
    }

    // ---- range decoder
    fn threshold(&mut self, scale: u32) -> Option<u32> {
        if scale == 0 { return None; }
        self.range /= scale;
        if self.range == 0 { return None; }
        Some(self.code.wrapping_sub(self.low) / self.range)
    }
    fn rc_decode(&mut self, lo: u32, hi: u32) {
        self.low = self.low.wrapping_add(self.range.wrapping_mul(lo));
        self.range = self.range.wrapping_mul(hi.wrapping_sub(lo));
    }
    fn normalize(&mut self, b: &mut Bits) {
        loop {
            if (self.low ^ self.low.wrapping_add(self.range)) >= TOP {
                if self.range >= BOT { break; }
                self.range = self.low.wrapping_neg() & (BOT - 1);
            }
            self.code = (self.code << 8) | b.byte_in();
            self.range <<= 8;
            self.low <<= 8;
        }
    }

    fn finish(&mut self, b: &mut Bits) -> Option<u32> {
        let sym = self.sym(self.found);
        let s = self.succ(self.found);
        if self.order_fall == 0 && s > self.text { self.min = s; self.max = s; } else { self.update_model(); }
        self.normalize(b);
        Some(sym)
    }

    /// The next byte, or None when the data is damaged (or the model has no symbol).
    pub fn decode(&mut self, b: &mut Bits) -> Option<u32> {
        if !self.ready { return None; }
        let mut mask = [false; 256];
        let mut num_masked;
        let mc = self.min;
        if !self.valid(mc) { return None; }
        let ns = self.ns(mc);
        if ns != 1 {
            let stats = self.stats(mc);
            if !self.valid(stats) { return None; }
            let summ = self.summ(mc);
            let count = self.threshold(summ)?;
            if count >= summ { return None; }
            let mut s = stats;
            let mut hi = self.freq(s);
            if count < hi {
                self.rc_decode(0, hi);
                self.prev_success = (2 * hi > summ) as u32;
                self.run_length += self.prev_success as i32;
                self.found = s;
                self.set_freq(s, hi + 4);
                self.set_summ(mc, summ + 4);
                if hi + 4 > MAX_FREQ { self.rescale(); }
                return self.finish(b);
            }
            self.prev_success = 0;
            let mut i = ns - 1;
            loop {
                s += 6;
                hi += self.freq(s);
                if hi > count {
                    let f = self.freq(s);
                    self.rc_decode(hi - f, hi);
                    // update1
                    self.found = s;
                    self.set_freq(s, f + 4);
                    self.set_summ(mc, self.summ(mc) + 4);
                    if f + 4 > self.freq(s - 6) {
                        self.swap(s, s - 6);
                        self.found = s - 6;
                        if self.freq(s - 6) > MAX_FREQ { self.rescale(); }
                    }
                    return self.finish(b);
                }
                i -= 1;
                if i == 0 { break; }
            }
            self.hibits = self.hb2flag[self.sym(self.found) as usize] as u32;
            self.rc_decode(hi, summ);
            let mut q = stats;
            for _ in 0..ns { mask[self.sym(q) as usize] = true; q += 6; }
            num_masked = ns;
        } else {
            let rs = Self::one(mc);
            self.hibits = self.hb2flag[self.sym(self.found) as usize] as u32;
            let suf = self.suffix(mc);
            if !self.valid(suf) { return None; }
            let i = (self.freq(rs) as usize).wrapping_sub(1).min(127);
            let j = self.prev_success as usize + self.ns2bs[(self.ns(suf) as usize).wrapping_sub(1) & 255] as usize + self.hibits as usize
                + 2 * self.hb2flag[self.sym(rs) as usize] as usize + ((self.run_length >> 26) & 0x20) as usize;
            let bs = self.bin[i][j & 63] as u32;
            self.range >>= 14;
            if self.range == 0 { return None; }
            let count = self.code.wrapping_sub(self.low) / self.range;
            if count < bs {
                self.found = rs;
                let f = self.freq(rs);
                self.set_freq(rs, f + (f < 128) as u32);
                self.rc_decode(0, bs);
                self.bin[i][j & 63] = (bs + (1 << INT_BITS) - ((bs + 32) >> 7)) as u16;
                self.prev_success = 1;
                self.run_length += 1;
                return self.finish(b);
            }
            self.rc_decode(bs, BIN_SCALE);
            let nb = bs - ((bs + 32) >> 7);
            self.bin[i][j & 63] = nb as u16;
            self.init_esc = EXP_ESCAPE[(nb >> 10) as usize & 15] as u32;
            num_masked = 1;
            mask[self.sym(rs) as usize] = true;
            self.prev_success = 0;
        }
        loop {
            self.normalize(b);
            let mut mc = self.min;
            loop {
                self.order_fall += 1;
                mc = self.suffix(mc);
                if !self.valid(mc) { return None; }
                if self.ns(mc) != num_masked { break; }
            }
            self.min = mc;
            let ns = self.ns(mc);
            if ns < num_masked { return None; }
            let diff = ns - num_masked;
            // the SEE context for the escape frequency
            let mut see_ix: Option<(usize, usize)> = None;
            let mut scale = if ns != 256 {
                let suf = self.suffix(mc);
                let a = self.ns2indx[((diff as usize).wrapping_sub(1)) & 255] as usize;
                let k = ((diff as i32) < self.ns(suf) as i32 - ns as i32) as usize
                    + 2 * (self.summ(mc) < 11 * ns) as usize
                    + 4 * (num_masked > diff) as usize
                    + self.hibits as usize;
                let (a, k) = (a.min(24), k.min(15));
                see_ix = Some((a, k));
                self.see[a][k].mean()
            } else { 1 };
            let mut ps = [0u32; 256];
            let mut hi = 0u32;
            let mut s = self.stats(mc);
            let mut n = 0usize;
            let mut guard = 0u32;
            while (n as u32) < diff {
                if guard >= ns { return None; }
                if !mask[self.sym(s) as usize] { hi += self.freq(s); ps[n] = s; n += 1; }
                s += 6;
                guard += 1;
            }
            scale += hi;
            let count = self.threshold(scale)?;
            if count >= scale { return None; }
            if count < hi {
                let mut h = 0u32;
                let mut k = 0usize;
                loop { h += self.freq(ps[k]); if h > count || k + 1 >= n { break; } k += 1; }
                let s = ps[k];
                let f = self.freq(s);
                self.rc_decode(h - f, h);
                if let Some((a, k)) = see_ix { self.see[a][k].update(); }
                // update2
                self.found = s;
                self.set_freq(s, f + 4);
                self.set_summ(mc, self.summ(mc) + 4);
                if f + 4 > MAX_FREQ { self.rescale(); }
                self.run_length = self.init_rl;
                return self.finish(b);
            }
            self.rc_decode(hi, scale);
            for &q in &ps[..n] { mask[self.sym(q) as usize] = true; }
            match see_ix { Some((a, k)) => self.see[a][k].summ = self.see[a][k].summ.wrapping_add(scale as u16), None => self.dummy.summ = self.dummy.summ.wrapping_add(scale as u16) }
            num_masked = ns;
        }
    }
}

impl Default for Model { fn default() -> Self { Self::new() } }
