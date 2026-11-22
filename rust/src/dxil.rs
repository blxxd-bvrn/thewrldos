//! A small DXIL 1.0 / Shader Model 6.0 reader (was `src/win32/dxil.c`), not an LLVM runtime.
//!
//! It reads the LLVM 3.7 bitstream encoding DXIL uses, then lowers a bounded vertex and
//! pixel subset into the same instruction list as DXBC (`dxbc.rs`): scalar f32/i32
//! arithmetic, comparisons and selects, branches without loops and two-input phis, input
//! and output signatures, legacy constant buffers and Texture2D sampling. Anything else
//! (other LLVM instructions or DXIL intrinsics, waves, atomics, UAVs, pointers, helper
//! functions, other stages, doubles, newer profiles) is refused when the shader is created.
//! Format references: Microsoft's DxilContainer.h, LLVM's LLVMBitCodes.h and BitCodes.h.
//!
//! Tables are fixed-size and allocated with the C allocator, as the C code's were; a value,
//! type or metadata reference that is out of range fails the shader and reads entry 0, so
//! the reader never indexes past a table.

use crate::dxbc::*;
use core::ffi::{c_char, c_int, c_void};
use core::fmt::Write;

const BC_TYPES: usize = 512;
const BC_VALUES: usize = 8192;
const BC_METADATA: usize = 4096;
const BC_RECORD: usize = 2048;
const BC_ABBREVS: usize = 512;
const BC_ABOPS: usize = 32;
const BC_BLOCK_ABBREVS: usize = 64;
const BC_BLOCKS: usize = 64;
const BC_BBS: usize = 128;

const TY_VOID: u16 = 2;
const TY_FLOAT: u16 = 3;
const TY_INT: u16 = 7;
const TY_PTR: u16 = 8;
const TY_ARRAY: u16 = 11;
const TY_VECTOR: u16 = 12;
const TY_STRUCT: u16 = 20;
const TY_FUNCTION: u16 = 21;

const V_CONSTANT: u16 = 1;
const V_UNDEF: u16 = 2;
const V_FUNCTION: u16 = 3;
const V_OPERAND: u16 = 4;
const V_HANDLE: u16 = 5;
const V_OPAQUE: u16 = 6;

extern "C" {
    fn kzalloc(n: usize) -> *mut c_void;
    fn krealloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn kfree(p: *mut c_void);
}

#[derive(Clone, Copy, Default)]
struct Ty { kind: u16, bits: u16, element: u16, count: u16, result: u16, params: [u16; 16] }
#[derive(Clone, Copy)]
struct Value {
    ty: u16, kind: u16, fn_type: u16,
    declaration: bool,
    integer: u64,
    operand: Opnd,
    resource_class: i32, binding: i32,
    block: i32, range_id: i32,
    name: [u8; 64],
}
#[derive(Clone, Copy)]
struct Md { kind: u8, n: u16, value: i32, refs: [u32; 16], text: [u8; 96] }
#[derive(Clone, Copy, Default)]
struct Abop { value: u64, encoding: u8, literal: u8 }
#[derive(Clone, Copy)]
struct Abbrev { n: i32, op: [Abop; BC_ABOPS] }
#[derive(Clone, Copy)]
struct Ablist { n: i32, id: [u16; BC_BLOCK_ABBREVS] }
#[derive(Clone, Copy, Default)]
struct Resource { valid: bool, lower: u32, count: u32, rows: u32 }

/// A zero-filled table from the C allocator. Only for types whose all-zero bytes are a
/// valid value (integers, bools, arrays of them).
struct KArr<T> { p: *mut T, n: usize }
impl<T> KArr<T> {
    fn new(n: usize) -> KArr<T> {
        let p = unsafe { kzalloc(core::mem::size_of::<T>() * n) } as *mut T;
        KArr { p, n: if p.is_null() { 0 } else { n } }
    }
    fn ok(&self) -> bool { !self.p.is_null() }
}
impl<T> core::ops::Deref for KArr<T> { type Target = [T]; fn deref(&self) -> &[T] { if self.p.is_null() { &[] } else { unsafe { core::slice::from_raw_parts(self.p, self.n) } } } }
impl<T> core::ops::DerefMut for KArr<T> { fn deref_mut(&mut self) -> &mut [T] { if self.p.is_null() { &mut [] } else { unsafe { core::slice::from_raw_parts_mut(self.p, self.n) } } } }
impl<T> Drop for KArr<T> { fn drop(&mut self) { unsafe { kfree(self.p as *mut c_void) } } }

struct Parser<'a> {
    bytes: &'a [u8], bit: u32, limit: u32,
    s: *mut Shader,
    err: *mut c_char, errlen: c_int, bad: bool,
    ab: KArr<Abbrev>, nab: usize,
    info: KArr<Ablist>,
    rec: KArr<u64>, nr: usize,
    types: KArr<Ty>, ntypes: i32, expected_types: i32, constant_type: i32,
    values: KArr<Value>, nvalues: i32, module_values: i32, entry: i32, defined_functions: i32,
    md: KArr<Md>, nmd: i32, entry_metadata: i32, version_metadata: i32, model_metadata: i32,
    resources: [[Resource; 16]; 4],
    sig_map: [[i32; DX_MAX_SIG]; 2], sig_col: [[i32; DX_MAX_SIG]; 2], sig_columns: [[i32; DX_MAX_SIG]; 2],
    metadata_name: [u8; 96],
    body_start: u32, body_end: u32, body_width: u32,
    pass: i32, nblocks: i32, block: i32, block_pc: [i32; BC_BBS], branch_a: [i32; BC_BBS], branch_b: [i32; BC_BBS],
    ins_cap: i32, pool_cap: i32,
    use_block: i32,
    dependencies: KArr<[u32; BC_BBS / 32]>,
    triple: bool, version: bool, module_seen: bool,
}

fn signed_value(v: u64) -> u64 { if v & 1 != 0 { 0u64.wrapping_sub(v >> 1) } else { v >> 1 } }
fn char6(v: u32) -> u8 {
    if v < 26 { b'a' + v as u8 } else if v < 52 { b'A' + (v - 26) as u8 } else if v < 62 { b'0' + (v - 52) as u8 } else if v == 62 { b'.' } else { b'_' }
}
fn u32at(d: &[u8], at: usize) -> u32 { match d.get(at..at + 4) { Some(b) => u32::from_le_bytes([b[0], b[1], b[2], b[3]]), None => 0 } }
fn operand(kind: u8, index: i32, component: i32, dest: bool) -> Opnd {
    let mut o = Opnd::default();
    o.ty = kind; o.dim = if kind == T_IMM32 || kind == T_NULL || kind == T_ODEPTH { 0 } else { 1 };
    o.idx[0] = index; o.mask = if dest { 1u8.wrapping_shl(component as u32) } else { 15 }; o.mode = if dest { 0 } else { 1 };
    o.rel = [-1; 3];
    o.swz = [component as u8; 4];
    o
}
fn immediate(bits: u32) -> Opnd { let mut o = operand(T_IMM32, 0, 0, false); o.imm = [bits; 4]; o }

impl<'a> Parser<'a> {
    #[allow(clippy::mut_from_ref)]
    fn s<'x>(&self) -> &'x mut Shader { unsafe { &mut *self.s } }
    fn fail(&mut self, why: &str) {
        if !self.bad { set_err_parts(self.err, self.errlen, &[b"DXIL: ", why.as_bytes()]); }
        self.bad = true;
    }
    fn bits(&mut self, n: u32) -> u64 {
        if n > 64 || self.bit > self.limit || n > self.limit - self.bit { self.fail("truncated bitstream"); return 0; }
        let mut v = 0u64;
        for i in 0..n {
            let byte = self.bytes[(self.bit >> 3) as usize];
            v |= (((byte >> (self.bit & 7)) & 1) as u64) << i;
            self.bit += 1;
        }
        v
    }
    fn vbr(&mut self, width: u32) -> u64 {
        if !(2..=32).contains(&width) { self.fail("invalid VBR width"); return 0; }
        let mut value = 0u64;
        let mut shift = 0u32;
        let mut chunks = 0;
        while chunks < 64 && !self.bad {
            let x = self.bits(width);
            let lo = x & ((1u64 << (width - 1)) - 1);
            if shift >= 64 || (lo != 0 && lo > (!0u64 >> shift)) { self.fail("VBR overflow"); return 0; }
            value |= lo << shift;
            if x >> (width - 1) == 0 { return value; }
            shift += width - 1;
            chunks += 1;
        }
        self.fail("unterminated VBR"); 0
    }
    fn align32(&mut self) {
        if self.bit > self.limit || self.limit - self.bit < (0u32.wrapping_sub(self.bit) & 31) { self.fail("bad block alignment"); }
        else { self.bit = self.bit.wrapping_add(31) & !31u32; }
    }
    fn string_record(&mut self, first: usize, out: &mut [u8]) {
        let n = self.nr as isize - first as isize;
        if n < 0 || n as usize >= out.len() { self.fail("name is too long"); return; }
        for i in 0..n as usize {
            let c = self.rec[first + i];
            if c > 255 { self.fail("invalid string"); return; }
            out[i] = c as u8;
        }
        out[n as usize] = 0;
    }
    fn read_abbrev(&mut self) -> usize {
        if self.nab == BC_ABBREVS { self.fail("too many bitstream abbreviations"); return 0; }
        let id = self.nab; self.nab += 1;
        let n = self.vbr(5) as i32;
        self.ab[id].n = n;
        if !(1..=BC_ABOPS as i32).contains(&n) { self.fail("abbreviation is too large"); return id; }
        let mut i = 0;
        while i < n as usize && !self.bad {
            let literal = self.bits(1) as u8;
            self.ab[id].op[i].literal = literal;
            if literal != 0 { let v = self.vbr(8); self.ab[id].op[i].value = v; }
            else {
                let enc = self.bits(3) as u8;
                self.ab[id].op[i].encoding = enc;
                if enc == 1 || enc == 2 { let v = self.vbr(5); self.ab[id].op[i].value = v; }
                let o = self.ab[id].op[i];
                if !(1..=5).contains(&o.encoding) || o.value > 32 { self.fail("invalid abbreviation encoding"); }
            }
            i += 1;
        }
        id
    }
    fn abvalue(&mut self, o: Abop) -> u64 {
        if o.literal != 0 { return o.value; }
        match o.encoding {
            1 => self.bits(o.value as u32),
            2 => self.vbr(o.value as u32),
            4 => { let c = self.bits(6) as u32; char6(c) as u64 }
            _ => { self.fail("invalid scalar abbreviation"); 0 }
        }
    }
    fn append(&mut self, v: u64) { if self.nr == BC_RECORD { self.fail("record is too large"); } else { self.rec[self.nr] = v; self.nr += 1; } }
    fn record(&mut self, list: &Ablist, id: u32) -> u32 {
        self.nr = 0;
        if id == 3 {
            let code = self.vbr(6) as u32;
            let n = self.vbr(6) as u32;
            if n as usize > BC_RECORD { self.fail("record is too large"); return 0; }
            let mut i = 0;
            while i < n && !self.bad { let v = self.vbr(6); self.append(v); i += 1; }
            return code;
        }
        if id < 4 || id - 4 >= list.n as u32 { self.fail("unknown abbreviation"); return 0; }
        let a = self.ab[list.id[(id - 4) as usize] as usize];
        let code = self.abvalue(a.op[0]);
        let mut i = 1usize;
        while (i as i32) < a.n && !self.bad {
            let o = a.op[i];
            if o.literal == 0 && o.encoding == 3 {
                let n = self.vbr(6);
                i += 1;
                if i as i32 >= a.n || n > (BC_RECORD - self.nr) as u64 { self.fail("invalid abbreviated array"); break; }
                let mut j = 0u64;
                while j < n && !self.bad { let v = self.abvalue(a.op[i]); self.append(v); j += 1; }
            } else if o.literal == 0 && o.encoding == 5 {
                let n = self.vbr(6);
                self.align32();
                if n > (BC_RECORD - self.nr) as u64 || n > (self.limit.wrapping_sub(self.bit) / 8) as u64 { self.fail("blob is too large"); break; }
                let mut j = 0u64;
                while j < n && !self.bad { let v = self.bits(8); self.append(v); j += 1; }
                self.align32();
            } else { let v = self.abvalue(o); self.append(v); }
            i += 1;
        }
        code as u32
    }

    fn type_at(&mut self, id: u64) -> usize {
        if id >= self.ntypes as i64 as u64 { self.fail("type reference out of range"); return 0; }
        id as usize
    }
    fn ty(&mut self, id: u64) -> Ty { let k = self.type_at(id); self.types[k] }
    fn new_value(&mut self, ty: i32) -> usize {
        if self.nvalues as usize == BC_VALUES { self.fail("too many SSA values"); return 0; }
        self.type_at(ty as i64 as u64);
        let k = self.nvalues as usize; self.nvalues += 1;
        let block = if self.pass != 0 { self.block } else { -1 };
        let v = &mut self.values[k];
        *v = Value { ty: ty as u16, kind: 0, fn_type: 0, declaration: false, integer: 0, operand: Opnd::default(), resource_class: 0, binding: 0, block, range_id: 0, name: [0; 64] };
        k
    }
    fn pool_add(&mut self, o: &Opnd) -> i32 {
        let s = self.s();
        if s.npool == SHADER_MAX_OPERANDS { self.fail("too many native operands"); return 0; }
        if s.npool == self.pool_cap {
            let cap = core::cmp::min(SHADER_MAX_OPERANDS, if self.pool_cap != 0 { self.pool_cap * 2 } else { 128 });
            let a = unsafe { krealloc(s.pool as *mut c_void, cap as usize * core::mem::size_of::<Opnd>()) } as *mut Opnd;
            if a.is_null() { self.fail("out of memory"); return 0; }
            s.pool = a; self.pool_cap = cap;
        }
        unsafe { *s.pool.add(s.npool as usize) = *o; }
        s.npool += 1;
        s.npool - 1
    }
    /// Append an instruction; None only when memory ran out.
    fn emit(&mut self, op: u16, n: usize, args: &[Opnd]) -> Option<usize> {
        let s = self.s();
        if s.nins == SHADER_MAX_INS { self.fail("too many native instructions"); return Some(0); }
        if s.nins == self.ins_cap {
            let cap = core::cmp::min(SHADER_MAX_INS, if self.ins_cap != 0 { self.ins_cap * 2 } else { 128 });
            let a = unsafe { krealloc(s.ins as *mut c_void, cap as usize * core::mem::size_of::<Ins>()) } as *mut Ins;
            if a.is_null() { self.fail("out of memory"); return None; }
            s.ins = a; self.ins_cap = cap;
        }
        let k = s.nins as usize; s.nins += 1;
        let mut ins = Ins { op, nops: n as u8, jump: -1, alt: -1, ..Default::default() };
        let mut i = 0;
        while i < n && !self.bad { ins.o[i] = self.pool_add(&args[i]) as i16; i += 1; }
        self.s().ins_mut()[k] = ins;
        Some(k)
    }
    fn set_ins(&mut self, k: Option<usize>, f: impl FnOnce(&mut Ins)) { if let Some(k) = k { if let Some(i) = self.s().ins_mut().get_mut(k) { f(i) } } }
    fn temporary(&mut self, vector: bool) -> Opnd {
        let s = self.s();
        if s.ntemps == SHADER_MAX_TEMPS { self.fail("too many native temporaries"); return immediate(0); }
        let mut o = operand(T_TEMP, s.ntemps, 0, true); s.ntemps += 1;
        o.mask = if vector { 15 } else { 1 };
        o
    }
    fn source(&mut self, v: usize) -> Opnd {
        let val = self.values[v];
        let t = self.ty(val.ty as u64);
        if val.kind == V_OPERAND || val.kind == V_CONSTANT || val.kind == V_UNDEF {
            if t.kind != TY_FLOAT && !(t.kind == TY_INT && (t.bits == 32 || t.bits == 1)) { self.fail("only scalar f32/i32/boolean arithmetic is implemented"); }
            return val.operand;
        }
        self.fail("unsupported SSA operand"); immediate(0)
    }
    fn reference(&mut self, encoded: u64, sign: bool) -> usize {
        if encoded > 0xFFFF_FFFF { self.fail("SSA reference overflow"); return 0; }
        let distance: i64 = if sign { signed_value(encoded) as i64 } else { encoded as u32 as i64 };
        let id = self.nvalues as i64 - distance;
        if id < 0 || id >= self.nvalues as i64 { self.fail("forward/out-of-range SSA reference"); return 0; }
        let vb = self.values[id as usize].block;
        let used = if self.use_block >= 0 { self.use_block } else { self.block };
        if vb >= 0 && self.pass != 0 && used >= 0 && (used as usize) < BC_BBS {
            if let Some(d) = self.dependencies.get_mut(used as usize) { if let Some(w) = d.get_mut(vb as usize / 32) { *w |= 1u32 << (vb & 31); } }
        }
        id as usize
    }
    fn pair(&mut self, slot: &mut usize) -> usize {
        if *slot >= self.nr { self.fail("missing value/type pair"); return 0; }
        let e = self.rec[*slot]; *slot += 1;
        self.reference(e, false)   /* forward values deliberately unsupported */
    }
    fn constant(&mut self, v: usize) -> u32 {
        let val = self.values[v];
        let t = self.ty(val.ty as u64);
        if val.kind != V_CONSTANT || t.kind != TY_INT || t.bits > 32 { self.fail("dynamic binding/index is not implemented"); return 0; }
        val.integer as u32
    }
    fn result(&mut self, ty: i32, op: u16, nsrc: usize, srcs: &[Opnd], vector: bool) -> usize {
        let mut args = [Opnd::default(); 5];
        args[0] = self.temporary(vector);
        args[1..=nsrc].copy_from_slice(&srcs[..nsrc]);
        if !self.bad { self.emit(op, nsrc + 1, &args); }
        let v = self.new_value(ty);
        let val = &mut self.values[v];
        val.kind = V_OPERAND; val.operand = args[0];
        val.operand.mode = 1; val.operand.mask = 15;
        val.operand.swz = if vector { [0, 1, 2, 3] } else { [0; 4] };
        v
    }

    fn md_at(&mut self, r: u32) -> usize {
        if r == 0 || r - 1 >= self.nmd as u32 { self.fail("metadata reference out of range"); return 0; }
        (r - 1) as usize
    }
    fn md_number(&mut self, r: u32) -> u32 {
        let m = self.md_at(r);
        let md = self.md[m];
        if md.kind != 2 || md.value < 0 || md.value >= self.module_values { self.fail("invalid typed metadata value"); return 0; }
        self.constant(md.value as usize)
    }
    fn node(&mut self, r: u32, min: u16) -> Md {
        let m = self.md_at(r);
        let md = self.md[m];
        if md.kind != 3 || md.n < min { self.fail("invalid metadata node"); }
        md
    }
    fn scalar_type(&mut self, id: i64, floating: bool, width: u16) -> bool {
        let t = self.ty(id as u64);
        if floating { t.kind == TY_FLOAT } else { t.kind == TY_INT && t.bits == width }
    }
    fn four_components(&mut self, id: i64, floating: bool, status: bool) -> bool {
        let t = self.ty(id as u64);
        if t.kind != TY_STRUCT || t.count != if status { 5 } else { 4 } { return false; }
        for i in 0..4 { if !self.scalar_type(t.params[i] as i64, floating, 32) { return false; } }
        !status || self.scalar_type(t.params[4] as i64, false, 32)
    }
    fn resource_metadata(&mut self, root: u32) {
        if root == 0 { return; }
        let all = self.node(root, 4);
        if self.bad { return; }
        let mut cls = 0usize;
        while cls < 4 && !self.bad {
            if all.refs[cls] == 0 { cls += 1; continue; }
            let list = self.node(all.refs[cls], 0);
            if cls == 1 && list.n != 0 { self.fail("UAV resources are not implemented"); return; }
            let mut i = 0usize;
            while i < list.n as usize && !self.bad {
                let r = self.node(list.refs[i], if cls == 0 { 9 } else { 8 });
                if self.bad { return; }
                let id = self.md_number(r.refs[0]);
                let space = self.md_number(r.refs[3]);
                let lower = self.md_number(r.refs[4]);
                let count = self.md_number(r.refs[5]);
                let limit = if cls == 2 { 14u32 } else { 16 };
                if space != 0 || id >= 16 || count == 0 || lower >= limit || count > limit - lower || self.resources[cls][id as usize].valid {
                    self.fail("resource spaces/unbounded or out-of-range bindings are not implemented"); return;
                }
                let mut rows = 0;
                if cls == 0 {
                    if self.md_number(r.refs[6]) != 2 || self.md_number(r.refs[7]) != 0 { self.fail("only non-MSAA Texture2D resources are implemented"); return; }
                    let props = self.node(r.refs[8], 2);
                    if props.n != 2 || self.md_number(props.refs[0]) != 0 || self.md_number(props.refs[1]) != 9 { self.fail("only float Texture2D resources are implemented"); return; }
                } else if cls == 2 {
                    let bytes = self.md_number(r.refs[6]);
                    if bytes == 0 || bytes > 65536 || r.refs[7] != 0 { self.fail("constant-buffer metadata is not implemented"); return; }
                    rows = (bytes + 15) / 16;
                } else if cls == 3 && (self.md_number(r.refs[6]) != 0 || r.refs[7] != 0) { self.fail("comparison/extended samplers are not implemented"); return; }
                for k in 0..16 {
                    let q = self.resources[cls][k];
                    if q.valid && lower < q.lower + q.count && q.lower < lower + count { self.fail("overlapping resource bindings"); return; }
                }
                self.resources[cls][id as usize] = Resource { valid: true, lower, count, rows };
                i += 1;
            }
            cls += 1;
        }
    }
    fn signature_metadata(&mut self) {
        if self.entry_metadata < 0 || self.entry_metadata >= self.nmd { self.fail("entry-point metadata is missing"); return; }
        let entry = self.md[self.entry_metadata as usize];
        if entry.kind != 3 || entry.n != 5 { self.fail("invalid entry-point metadata"); return; }
        let f = self.md_at(entry.refs[0]);
        if self.md[f].kind != 2 || self.md[f].value != self.entry { self.fail("entry-point function mismatch"); return; }
        self.resource_metadata(entry.refs[3]);
        if entry.refs[4] != 0 {
            let props = self.node(entry.refs[4], 0);
            /* shader flags beyond ordinary f32/int arithmetic need pipeline support */
            if props.n != 0 { self.fail("entry-point flags/properties are not implemented"); return; }
        }
        let (nin, nout) = (self.s().nin, self.s().nout);
        if entry.refs[2] == 0 { if nin != 0 || nout != 0 { self.fail("signature metadata is missing"); } return; }
        let sm = self.md_at(entry.refs[2]);
        let signatures = self.md[sm];
        if signatures.kind != 3 || signatures.n < 2 { self.fail("invalid signature metadata"); return; }
        let mut direction = 0usize;
        while direction < 2 && !self.bad {
            if signatures.refs[direction] == 0 { direction += 1; continue; }
            let lm = self.md_at(signatures.refs[direction]);
            let list = self.md[lm];
            let count = if direction != 0 { nout } else { nin };
            if list.kind != 3 || list.n as i32 != count { self.fail("signature list/container mismatch"); return; }
            let mut i = 0usize;
            while i < list.n as usize && !self.bad {
                let em = self.md_at(list.refs[i]);
                let element = self.md[em];
                if element.kind != 3 || element.n < 10 { self.fail("invalid signature element"); return; }
                let id = self.md_number(element.refs[0]);
                let mode = self.md_number(element.refs[5]);
                let rows = self.md_number(element.refs[6]);
                let columns = self.md_number(element.refs[7]);
                let reg = self.md_number(element.refs[8]);
                let mut col = self.md_number(element.refs[9]);
                let component = self.md_number(element.refs[2]);
                if reg == !0 && col == 255 { col = 0; }  /* unpacked scalar SV_Depth */
                let nm = self.md_at(element.refs[1]);
                let name = self.md[nm];
                let indices = self.node(element.refs[4], 1);
                if id as usize >= DX_MAX_SIG || self.sig_map[direction][id as usize] >= 0 || rows != 1 || columns == 0 || columns > 4 || col > 4 - columns || name.kind != 1 || indices.n != 1 {
                    self.fail("signature arrays/packing are not implemented"); return;
                }
                let semantic_index = self.md_number(indices.refs[0]);
                let s = self.s();
                let sigs = if direction != 0 { &s.out } else { &s.inp };
                let mut k = 0usize;
                while (k as i32) < count { if ieq(&sigs[k].name, &name.text) && sigs[k].index == semantic_index as i32 { break; } k += 1; }
                if k as i32 == count { self.fail("signature semantic mismatch"); return; }
                let sig = sigs[k];
                let ctype = if component == 9 { 3 } else if component == 4 { 2 } else if component == 5 { 1 } else { 0 };
                if reg != sig.reg as u32 || sig.mask as u32 != (((1u32 << columns) - 1) << col) || ctype != sig.component_type { self.fail("signature register/component mismatch"); return; }
                self.sig_map[direction][id as usize] = k as i32; self.sig_col[direction][id as usize] = col as i32; self.sig_columns[direction][id as usize] = columns as i32;
                if direction == 0 && sig.reg >= 0 {
                    if mode > 5 { self.fail("sample-frequency interpolation is not implemented"); return; }
                    let m = if mode == 1 { 1 } else if mode >= 4 { 4 } else { 2 };
                    let r = sig.reg as usize;
                    let s = self.s();
                    if s.interpolation_declared & (1 << r) != 0 && s.interpolation[r] != m { self.fail("mixed interpolation within a register is not implemented"); return; }
                    s.interpolation_declared |= 1 << r; s.interpolation[r] = m;
                }
                i += 1;
            }
            direction += 1;
        }
    }

    fn type_record(&mut self, code: u32) {
        if code == 1 {
            if self.nr != 1 || self.rec[0] > BC_TYPES as u64 { self.fail("type table is too large"); }
            else { self.expected_types = self.rec[0] as i32; }
            return;
        }
        if code == 19 { return; }             /* a name for the following named struct */
        if self.ntypes as usize >= BC_TYPES || self.ntypes >= self.expected_types { self.fail("too many types"); return; }
        let k = self.ntypes as usize; self.ntypes += 1;
        let mut t = self.types[k];
        t.kind = code as u16;
        let expected = self.expected_types as i64 as u64;
        let nr = self.nr;
        if code == TY_INT as u32 {
            if nr != 1 || self.rec[0] == 0 || self.rec[0] > 64 { self.fail("integer type width"); } else { t.bits = self.rec[0] as u16; }
        } else if code == TY_FLOAT as u32 { t.bits = 32; }
        else if code == TY_PTR as u32 {
            if nr < 1 || self.rec[0] >= expected { self.fail("pointer type"); } else { t.element = self.rec[0] as u16; }
        } else if code == TY_VECTOR as u32 || code == TY_ARRAY as u32 {
            if nr != 2 || self.rec[0] > 65535 || self.rec[1] >= expected { self.fail("array/vector type"); } else { t.count = self.rec[0] as u16; t.element = self.rec[1] as u16; }
        } else if code == TY_FUNCTION as u32 || code == 18 || code == 20 {
            let first = if code == TY_FUNCTION as u32 { 2 } else { 1 };
            if nr < first || nr - first > 16 || (code == TY_FUNCTION as u32 && self.rec[0] != 0) { self.types[k] = t; self.fail("complex/variadic type"); return; }
            t.kind = if code == TY_FUNCTION as u32 { TY_FUNCTION } else { TY_STRUCT };
            t.count = (nr - first) as u16;
            if code == TY_FUNCTION as u32 { if self.rec[1] >= expected { self.fail("invalid function result type"); } else { t.result = self.rec[1] as u16; } }
            for i in 0..t.count as usize { if self.rec[first + i] >= expected { self.fail("invalid member type"); } else { t.params[i] = self.rec[first + i] as u16; } }
        } else if ![TY_VOID as u32, 5, 6, 16, 4, 10].contains(&code) { self.fail("unsupported LLVM type"); }
        self.types[k] = t;
    }
    fn constant_record(&mut self, code: u32) {
        if code == 1 {
            if self.nr != 1 { self.fail("constant type record"); } else { self.constant_type = self.rec[0] as i32; self.type_at(self.constant_type as i64 as u64); }
            return;
        }
        let t = self.ty(self.constant_type as i64 as u64);
        let v = self.new_value(self.constant_type);
        let (nr, r0) = (self.nr, self.rec[0]);
        let val = &mut self.values[v];
        val.kind = V_CONSTANT; val.operand = immediate(0);
        val.block = -1;
        if code == 2 || code == 3 { val.kind = if code == 3 { V_UNDEF } else { V_CONSTANT }; return; }
        if code == 4 && nr == 1 && t.kind == TY_INT {
            val.integer = signed_value(r0);
            if t.bits < 64 { val.integer &= (1u64 << t.bits) - 1; }
            val.operand = immediate(if t.bits == 1 && val.integer != 0 { !0 } else { val.integer as u32 });
        } else if code == 6 && nr == 1 && t.kind == TY_FLOAT { val.operand = immediate(r0 as u32); }
        else { val.kind = V_OPAQUE; }            /* metadata arrays/strings, not executable values */
    }
    fn metadata_record(&mut self, code: u32) {
        if code == 4 { let mut name = [0u8; 96]; name.copy_from_slice(&self.metadata_name); self.string_record(0, &mut name); self.metadata_name = name; return; }
        if code == 10 {
            let name = cstr(&self.metadata_name);
            if name == b"dx.entryPoints" {
                if self.nr != 1 || self.rec[0] >= BC_METADATA as u64 { self.fail("multiple/invalid entry points"); } else { self.entry_metadata = self.rec[0] as i32; }
            } else if name == b"dx.version" || name == b"dx.shaderModel" {
                let version = name == b"dx.version";
                if self.nr != 1 || self.rec[0] >= BC_METADATA as u64 { self.fail("invalid shader version metadata"); }
                else if version { self.version_metadata = self.rec[0] as i32; }
                else { self.model_metadata = self.rec[0] as i32; }
            }
            return;
        }
        if code == 6 { return; }              /* metadata-kind names do not occupy value IDs */
        if self.nmd as usize == BC_METADATA { self.fail("metadata table is too large"); return; }
        let k = self.nmd as usize; self.nmd += 1;
        self.md[k].kind = code as u8;
        if code == 1 { let mut text = self.md[k].text; self.string_record(0, &mut text); self.md[k].text = text; }
        else if code == 2 {
            if self.nr != 2 || self.rec[1] >= self.nvalues as i64 as u64 { self.fail("metadata value"); } else { self.md[k].value = self.rec[1] as i32; }
        } else if code == 3 || code == 5 {
            self.md[k].kind = 3;
            if self.nr > 16 { self.fail("metadata node is too large"); return; }
            self.md[k].n = self.nr as u16;
            for i in 0..self.nr {
                if self.rec[i] > BC_METADATA as u64 { self.fail("metadata reference"); } else { self.md[k].refs[i] = self.rec[i] as u32; }
            }
        } else { self.fail("debug/extended metadata is not implemented"); }
    }

    fn bool_type(&mut self) -> i32 {
        for i in 0..self.ntypes as usize { if self.types[i].kind == TY_INT && self.types[i].bits == 1 { return i as i32; } }
        self.fail("missing boolean type"); 0
    }
    fn binary_record(&mut self) {
        let mut at = 0; let a = self.pair(&mut at);
        if at + 2 > self.nr { self.fail("binary instruction"); return; }
        let b = self.reference(self.rec[at], false); at += 1;
        let op = self.rec[at] as i32; at += 1;
        let _ = at;
        let ty = self.values[a].ty as i32;
        let t = self.ty(ty as i64 as u64);
        let mut args = [self.source(a), self.source(b), Opnd::default()];
        if ty != self.values[b].ty as i32 || (t.kind != TY_FLOAT && (t.kind != TY_INT || (t.bits != 32 && !(t.bits == 1 && (10..=12).contains(&op))))) { self.fail("binary operand types"); return; }
        let mut native: i32 = -1;
        if t.kind == TY_FLOAT {
            if op == 0 { native = OP_ADD as i32; }
            else if op == 1 { native = OP_ADD as i32; args[1].md ^= 1; }
            else if op == 2 { native = OP_MUL as i32; }
            else if op == 4 { native = OP_DIV as i32; }
        } else {
            const MAP: [i32; 13] = [OP_IADD as i32, OP_IADD as i32, OP_IMAD as i32, -1, -1, -1, -1, OP_ISHL as i32, OP_USHR as i32, OP_ISHR as i32, OP_AND as i32, OP_OR as i32, OP_XOR as i32];
            if (0..13).contains(&op) { native = MAP[op as usize]; }
            if op == 1 { args[1].md ^= 1; }
            if op == 2 { args[2] = immediate(0); }
        }
        if native < 0 { self.fail("binary operation is not implemented"); return; }
        self.result(ty, native as u16, if op == 2 && t.kind == TY_INT { 3 } else { 2 }, &args, false);
    }
    fn compare_record(&mut self) {
        let mut at = 0; let a = self.pair(&mut at);
        if at + 2 > self.nr { self.fail("comparison instruction"); return; }
        let b = self.reference(self.rec[at], false); at += 1;
        let pred = self.rec[at] as i32;
        let t = self.ty(self.values[a].ty as u64);
        let mut args = [self.source(a), self.source(b)];
        let mut op: i32 = -1;
        let mut invert = false;
        if self.values[a].ty != self.values[b].ty { self.fail("comparison types"); return; }
        if t.kind == TY_FLOAT {
            match pred {
                1 => op = OP_EQ as i32, 2 => { op = OP_LT as i32; invert = true; } 3 => op = OP_GE as i32,
                4 => op = OP_LT as i32, 5 => { op = OP_GE as i32; invert = true; } 14 => op = OP_NE as i32,
                _ => {}
            }
        } else if t.kind == TY_INT && t.bits <= 32 {
            match pred {
                32 => op = OP_IEQ as i32, 33 => op = OP_INE as i32, 34 => { op = OP_ULT as i32; invert = true; }
                35 => op = OP_UGE as i32, 36 => op = OP_ULT as i32, 37 => { op = OP_UGE as i32; invert = true; }
                38 => { op = OP_ILT as i32; invert = true; } 39 => op = OP_IGE as i32,
                40 => op = OP_ILT as i32, 41 => { op = OP_IGE as i32; invert = true; }
                _ => {}
            }
        }
        if op < 0 { self.fail("comparison predicate is not implemented"); return; }
        if invert { args.swap(0, 1); }
        let bt = self.bool_type();
        self.result(bt, op as u16, 2, &args, false);
    }
    fn cast_record(&mut self) {
        let mut at = 0; let v = self.pair(&mut at);
        if at + 2 != self.nr { self.fail("cast instruction"); return; }
        let ty = self.rec[at] as i32; at += 1;
        let cast = self.rec[at] as i32;
        let from = self.ty(self.values[v].ty as u64);
        let to = self.ty(ty as i64 as u64);
        let mut args = [self.source(v), immediate(1)];
        let mut op: i32 = -1;
        if cast == 3 && from.kind == TY_FLOAT && to.kind == TY_INT && to.bits == 32 { op = OP_FTOU as i32; }
        else if cast == 4 && from.kind == TY_FLOAT && to.kind == TY_INT && to.bits == 32 { op = OP_FTOI as i32; }
        else if cast == 5 && from.kind == TY_INT && from.bits == 32 && to.kind == TY_FLOAT { op = OP_UTOF as i32; }
        else if cast == 6 && from.kind == TY_INT && from.bits == 32 && to.kind == TY_FLOAT { op = OP_ITOF as i32; }
        else if cast == 11 && from.bits == 32 && to.bits == 32 { op = OP_MOV as i32; }
        else if (cast == 0 || cast == 1 || cast == 2) && from.kind == TY_INT && to.kind == TY_INT && to.bits <= 32 && from.bits <= 32 {
            if cast == 0 || from.bits == 1 {
                let width = if cast == 0 { to.bits } else { from.bits } as u32;
                args[1] = immediate(if width == 32 { !0 } else { (1u32 << width) - 1 });
                op = OP_AND as i32;
            } else if cast == 1 { op = OP_MOV as i32; }
            else { self.fail("sign extension width is not implemented"); return; }
        }
        if op < 0 { self.fail("cast is not implemented"); return; }
        let out = self.result(ty, op as u16, if op == OP_AND as i32 { 2 } else { 1 }, &args, false);
        if (cast == 2 && from.bits == 1) || (cast == 0 && to.bits == 1) {
            let o = self.values[out].operand;
            let mut negate = [o, o]; negate[0].mode = 0; negate[0].mask = 1;
            self.emit(OP_INEG, 2, &negate);
        }
    }
    fn call_record(&mut self) {
        if self.nr < 3 { self.fail("call record"); return; }
        let mut at = 2usize;
        let mut fn_type: i32 = -1;
        let cc = self.rec[1] as u32;
        if cc & (1 << 15) != 0 { if at >= self.nr { self.fail("call type"); return; } fn_type = self.rec[at] as i32; at += 1; }
        let f = self.pair(&mut at);
        let fv = self.values[f];
        if fv.kind != V_FUNCTION || !fv.declaration || !cstr(&fv.name).starts_with(b"dx.op.") { self.fail("helper/external functions are not implemented"); return; }
        if fn_type < 0 { fn_type = fv.fn_type as i32; }
        let ft = self.ty(fn_type as i64 as u64);
        if ft.kind != TY_FUNCTION || ft.count > 16 || at + ft.count as usize != self.nr { self.fail("call signature"); return; }
        let mut arg = [0usize; 16];
        for i in 0..ft.count as usize {
            arg[i] = self.reference(self.rec[at], false); at += 1;
            if self.values[arg[i]].ty != ft.params[i] { self.fail("call argument type"); return; }
        }
        if self.bad { return; }
        if ft.count == 0 { self.fail("call opcode argument is missing"); return; }
        let opcode = self.constant(arg[0]) as i32;
        let mut namebuf = [0u8; 64];
        namebuf[..58].copy_from_slice(&fv.name[6..64]);
        let name = cstr(&namebuf);
        let rt = ft.result as i32;
        let n = ft.count;
        let a = |p: &Self, i: usize| p.values[arg[i]];
        if opcode == 57 && name == b"createHandle" && n == 5 {
            let cls = self.constant(arg[1]); let index = self.constant(arg[3]);
            let id = self.constant(arg[2]); let nonuniform = self.constant(arg[4]);
            if (cls != 0 && cls != 2 && cls != 3) || id >= 16 || nonuniform != 0 { self.fail("resource class/binding is not implemented"); return; }
            let r = self.resources[cls as usize][id as usize];
            if !r.valid || index < r.lower || index - r.lower >= r.count { self.fail("handle does not match its resource binding range"); return; }
            let v = self.new_value(rt);
            let val = &mut self.values[v]; val.kind = V_HANDLE; val.resource_class = cls as i32; val.binding = index as i32; val.range_id = id as i32;
            return;
        }
        if opcode == 4 && (name == b"loadInput.f32" || name == b"loadInput.i32") && n == 5 {
            let id = self.constant(arg[1]); let row = self.constant(arg[2]); let col = self.constant(arg[3]);
            if id as usize >= DX_MAX_SIG || self.sig_map[0][id as usize] < 0 || row != 0 || col >= self.sig_columns[0][id as usize] as u32 || a(self, 4).kind != V_UNDEF { self.fail("input signature indexing is not implemented"); return; }
            let sig = self.s().inp[self.sig_map[0][id as usize] as usize];
            if sig.reg < 0 || !self.scalar_type(rt as i64, sig.component_type == 3, 32) { self.fail("input signature type mismatch"); return; }
            let o = operand(T_INPUT, sig.reg, col as i32 + self.sig_col[0][id as usize], false);
            let v = self.new_value(rt);
            let val = &mut self.values[v]; val.kind = V_OPERAND; val.operand = o;
            return;
        }
        if opcode == 5 && (name == b"storeOutput.f32" || name == b"storeOutput.i32") && n == 5 {
            let id = self.constant(arg[1]); let row = self.constant(arg[2]); let col = self.constant(arg[3]);
            if id as usize >= DX_MAX_SIG || self.sig_map[1][id as usize] < 0 || row != 0 || col >= self.sig_columns[1][id as usize] as u32 { self.fail("output signature indexing is not implemented"); return; }
            let sig = self.s().out[self.sig_map[1][id as usize] as usize];
            let a4 = a(self, 4);
            if self.ty(rt as i64 as u64).kind != TY_VOID || !self.scalar_type(a4.ty as i64, sig.component_type == 3, 32) { self.fail("output signature type mismatch"); return; }
            if sig.sysval == 67 || sig.sysval == 68 || (sig.reg < 0 && sig.sysval != 65) { self.fail("output semantic is not implemented"); return; }
            let args = [operand(if sig.sysval == 65 { T_ODEPTH } else { T_OUTPUT }, sig.reg, col as i32 + self.sig_col[1][id as usize], true), self.source(arg[4])];
            self.emit(OP_MOV, 2, &args);
            return;
        }
        if opcode == 59 && (name == b"cbufferLoadLegacy.f32" || name == b"cbufferLoadLegacy.i32") && n == 3 {
            let h = a(self, 1);
            if h.kind != V_HANDLE || h.resource_class != 2 || !(self.four_components(rt as i64, true, false) || self.four_components(rt as i64, false, false)) { self.fail("constant-buffer load type"); return; }
            let row = self.constant(arg[2]);
            if row >= self.resources[2][h.range_id as usize & 15].rows { self.fail("constant-buffer row out of range"); return; }
            let mut o = operand(T_CB, h.binding, 0, false);
            o.dim = 2; o.idx[1] = row as i32; o.swz = [0, 1, 2, 3];
            let v = self.new_value(rt);
            let val = &mut self.values[v]; val.kind = V_OPERAND; val.operand = o;
            return;
        }
        if (opcode == 60 && name == b"sample.f32") || (opcode == 62 && name == b"sampleLevel.f32") {
            if n != 11 || !self.four_components(rt as i64, true, true) || a(self, 1).kind != V_HANDLE || a(self, 1).resource_class != 0 || a(self, 2).kind != V_HANDLE || a(self, 2).resource_class != 3 { self.fail("Texture2D sampling signature"); return; }
            for i in 5..=6 { if a(self, i).kind != V_UNDEF { self.fail("only two-dimensional texture sampling is implemented"); return; } }
            if self.constant(arg[7]) != 0 || self.constant(arg[8]) != 0 || a(self, 9).kind != V_UNDEF { self.fail("texture offsets are not implemented"); return; }
            let mut uv = self.temporary(true);
            let mut mv = [uv, self.source(arg[3])]; mv[0].mask = 1; self.emit(OP_MOV, 2, &mv);
            mv[0].mask = 2; mv[1] = self.source(arg[4]); self.emit(OP_MOV, 2, &mv);
            uv.mode = 1; uv.swz = [0, 1, 2, 3];
            let mut args = [uv, operand(T_RESOURCE, a(self, 1).binding, 0, false), operand(T_SAMPLER, a(self, 2).binding, 0, false), self.source(arg[10])];
            args[1].swz = [0, 1, 2, 3];
            if opcode == 60 && a(self, 10).kind != V_UNDEF { self.fail("sample clamp is not implemented"); return; }
            self.result(rt, if opcode == 62 { OP_SAMPLE_L } else { OP_SAMPLE }, if opcode == 62 { 4 } else { 3 }, &args, true);
            return;
        }
        if n == 2 && ((opcode == 130 && name == b"legacyF32ToF16") || (opcode == 131 && name == b"legacyF16ToF32")) {
            let a1 = a(self, 1);
            if !self.scalar_type(rt as i64, opcode == 131, 32) || !self.scalar_type(a1.ty as i64, opcode == 130, 32) { self.fail("legacy half-conversion type"); return; }
            let src = [self.source(arg[1])];
            self.result(rt, if opcode == 130 { OP_F32TOF16 } else { OP_F16TOF32 }, 1, &src, false);
            return;
        }
        if (opcode == 54 && name == b"dot2.f32") || (opcode == 55 && name == b"dot3.f32") || (opcode == 56 && name == b"dot4.f32") {
            let count = (opcode - 52) as usize;
            if n as usize != 1 + 2 * count || !self.scalar_type(rt as i64, true, 32) { self.fail("dot-product signature"); return; }
            let mut args = [Opnd::default(); 2];
            for v in 0..2 {
                args[v] = self.temporary(true);
                for c in 0..count {
                    let k = arg[1 + v * count + c];
                    if self.values[k].ty as i32 != rt { self.fail("dot-product operand type"); return; }
                    let mut mv = [args[v], self.source(k)]; mv[0].mask = 1 << c; self.emit(OP_MOV, 2, &mv);
                }
                args[v].mode = 1; args[v].swz = [0, 1, 2, 3];
            }
            self.result(rt, if count == 2 { OP_DP2 } else if count == 3 { OP_DP3 } else { OP_DP4 }, 2, &args, false);
            return;
        }
        if opcode == 82 && name == b"discard" && n == 2 && self.ty(rt as i64 as u64).kind == TY_VOID && { let t = a(self, 1).ty; self.scalar_type(t as i64, false, 1) } {
            let condition = [self.source(arg[1])];
            let k = self.emit(OP_DISCARD, 1, &condition);
            self.set_ins(k, |i| i.nonzero = 1);
            return;
        }
        if (name == b"unary.f32" || name == b"unary.i32" || name == b"unaryBits.i32") && n == 2 {
            let mut sat = false;
            let op: i32 = match opcode {
                6 => OP_MOV as i32, 7 => { sat = true; OP_MOV as i32 }
                12 | 13 => OP_SINCOS as i32,
                21 => OP_EXP as i32, 22 => OP_FRC as i32, 23 => OP_LOG as i32,
                24 => OP_SQRT as i32, 25 => OP_RSQ as i32,
                26 => OP_ROUND_NE as i32, 27 => OP_ROUND_NI as i32, 28 => OP_ROUND_PI as i32, 29 => OP_ROUND_Z as i32,
                30 => OP_BFREV as i32, 31 => OP_COUNTBITS as i32, 32 => OP_FIRSTBIT_LO as i32,
                33 => OP_FIRSTBIT_HI as i32, 34 => OP_FIRSTBIT_SHI as i32,
                _ => -1,
            };
            if op < 0 || !self.scalar_type(rt as i64, opcode < 30, 32) || a(self, 1).ty as i32 != rt { self.fail("unary intrinsic/type is not implemented"); return; }
            let mut src = self.source(arg[1]);
            if opcode == 6 { src.md |= 2; }
            if op == OP_SINCOS as i32 {
                let dest = self.temporary(false);
                let mut args = [operand(T_NULL, 0, 0, true), operand(T_NULL, 0, 0, true), src];
                args[if opcode == 13 { 0 } else { 1 }] = dest;
                self.emit(OP_SINCOS, 3, &args);
                let v = self.new_value(rt);
                let val = &mut self.values[v]; val.kind = V_OPERAND; val.operand = dest; val.operand.mode = 1;
                return;
            }
            self.result(rt, op as u16, 1, &[src], false);
            let s = self.s();
            if sat && s.nins != 0 { let k = s.nins as usize - 1; s.ins_mut()[k].sat = 1; }
            return;
        }
        if (name == b"binary.f32" || name == b"binary.i32") && n == 3 {
            let mut op: i32 = if opcode == 35 || opcode == 37 || opcode == 39 { OP_MAX as i32 } else if opcode == 36 || opcode == 38 || opcode == 40 { OP_MIN as i32 } else { -1 };
            if opcode == 37 { op = OP_IMAX as i32; }
            if opcode == 38 { op = OP_IMIN as i32; }
            if opcode == 39 { op = OP_UMAX as i32; }
            if opcode == 40 { op = OP_UMIN as i32; }
            if op < 0 || !self.scalar_type(rt as i64, opcode <= 36, 32) || a(self, 1).ty as i32 != rt || a(self, 2).ty as i32 != rt { self.fail("binary intrinsic/type is not implemented"); return; }
            let args = [self.source(arg[1]), self.source(arg[2])];
            self.result(rt, op as u16, 2, &args, false);
            return;
        }
        if (name == b"tertiary.f32" || name == b"tertiary.i32") && n == 4 && (opcode == 46 || opcode == 48 || opcode == 49) {
            if !self.scalar_type(rt as i64, opcode == 46, 32) || a(self, 1).ty as i32 != rt || a(self, 2).ty as i32 != rt || a(self, 3).ty as i32 != rt { self.fail("tertiary intrinsic type is not implemented"); return; }
            let args = [self.source(arg[1]), self.source(arg[2]), self.source(arg[3])];
            self.result(rt, if opcode == 46 { OP_MAD } else if opcode == 48 { OP_IMAD } else { OP_UMAD }, 3, &args, false);
            return;
        }
        if let Some(mut b) = ErrBuf::new(self.err, self.errlen) {
            b.bytes(b"DXIL: intrinsic "); b.bytes(name);
            let _ = write!(b, " (opcode {}) is not implemented", opcode);
            b.finish();
        }
        self.bad = true;
    }

    fn function_record(&mut self, code: u32) {
        if code == 1 {
            if self.nr != 1 || self.rec[0] == 0 || self.rec[0] > BC_BBS as u64 || self.nblocks != 0 { self.fail("basic block declaration"); return; }
            self.nblocks = self.rec[0] as i32; self.block = 0; self.block_pc[0] = self.s().nins;
            let k = self.emit(OP_BLOCK, 0, &[]); self.set_ins(k, |i| i.jump = 0);
            return;
        }
        if self.nblocks == 0 || self.block >= self.nblocks { self.fail("instruction outside a basic block"); return; }
        if code == 2 { self.binary_record(); }
        else if code == 3 { self.cast_record(); }
        else if code == 9 || code == 28 { self.compare_record(); }
        else if code == 34 { self.call_record(); }
        else if code == 26 {
            let mut at = 0; let agg = self.pair(&mut at);
            let av = self.values[agg];
            let t = self.ty(av.ty as u64);
            if at + 1 != self.nr || t.kind != TY_STRUCT || self.rec[at] >= t.count as u64 || self.rec[at] >= 4 || av.kind != V_OPERAND { self.fail("aggregate extraction is not implemented"); return; }
            let component = self.rec[at] as usize;
            let v = self.new_value(t.params[component] as i32);
            let val = &mut self.values[v]; val.kind = V_OPERAND; val.operand = av.operand;
            val.operand.swz = [component as u8; 4];
        } else if code == 5 || code == 29 {
            let mut at = 0; let yes = self.pair(&mut at);
            if at + 2 != self.nr { self.fail("select instruction"); return; }
            let no = self.reference(self.rec[at], false); at += 1;
            let condition = self.reference(self.rec[at], false);
            let cty = self.values[condition].ty;
            if self.values[yes].ty != self.values[no].ty || self.ty(cty as u64).bits != 1 { self.fail("select operand types"); return; }
            let args = [self.source(condition), self.source(yes), self.source(no)];
            let yt = self.values[yes].ty as i32;
            self.result(yt, OP_MOVC, 3, &args, false);
        } else if code == 16 {
            if self.nr != 5 { self.fail("only two-input phi nodes are implemented"); return; }
            let ty = self.rec[0] as i32; let block_a = self.rec[2] as i32; let block_b = self.rec[4] as i32;
            if block_a < 0 || block_a >= self.block || block_b < 0 || block_b >= self.block || block_a == block_b { self.fail("cyclic/invalid phi node"); return; }
            self.use_block = block_a; let a = self.reference(self.rec[1], true);
            self.use_block = block_b; let b = self.reference(self.rec[3], true); self.use_block = -1;
            if self.values[a].ty as i32 != ty || self.values[b].ty as i32 != ty { self.fail("phi operand types"); return; }
            let args = [self.source(a), self.source(b)];
            self.result(ty, OP_PHI, 2, &args, false);
            if !self.bad { let k = self.s().nins as usize - 1; let i = &mut self.s().ins_mut()[k]; i.jump = block_a; i.alt = block_b; }
        } else if code == 10 || code == 11 {
            if code == 10 { if self.nr != 0 { self.fail("entry-point return type"); return; } self.emit(OP_RET, 0, &[]); }
            else {
                if self.nr != 1 && self.nr != 3 { self.fail("branch record"); return; }
                let a = self.rec[0] as i32;
                let b = if self.nr == 3 { self.rec[1] as i32 } else { a };
                if a <= self.block || b <= self.block || a >= self.nblocks || b >= self.nblocks { self.fail("backward/cyclic branches are not implemented"); return; }
                let cond = if self.nr == 3 { Some(self.reference(self.rec[2], false)) } else { None };
                if let Some(c) = cond { let t = self.values[c].ty; if !self.scalar_type(t as i64, false, 1) { self.fail("branch condition type"); return; } }
                let condition = match cond { Some(c) => self.source(c), None => immediate(0) };
                let k = self.emit(OP_BRANCH, if self.nr == 3 { 1 } else { 0 }, &[condition]);
                self.set_ins(k, |i| { i.jump = a; i.alt = b; });
                self.branch_a[self.block as usize] = a; self.branch_b[self.block as usize] = b;
            }
            self.block += 1;
            if self.block < self.nblocks {
                self.block_pc[self.block as usize] = self.s().nins;
                let blk = self.block;
                let k = self.emit(OP_BLOCK, 0, &[]); self.set_ins(k, |i| i.jump = blk);
            }
        } else {
            seterr!(self.err, self.errlen, "DXIL: LLVM instruction record {} is not implemented", code);
            self.bad = true;
        }
    }

    fn on_record(&mut self, block: u32, code: u32) {
        if block == 17 && self.pass == 0 { self.type_record(code); }
        else if block == 11 { self.constant_record(code); }
        else if block == 15 && self.pass == 0 { self.metadata_record(code); }
        else if block == 14 && self.pass == 0 && code == 1 {
            if self.nr == 0 || self.rec[0] >= self.nvalues as i64 as u64 || self.values[self.rec[0] as usize].kind != V_FUNCTION { self.fail("value symbol table"); return; }
            let k = self.rec[0] as usize;
            let mut name = self.values[k].name;
            self.string_record(1, &mut name);
            self.values[k].name = name;
        } else if block == 12 && self.pass != 0 { self.function_record(code); }
        else if block == 8 && self.pass == 0 {
            if code == 1 { if self.nr != 1 || self.rec[0] != 1 { self.fail("only LLVM bitcode version 1 is implemented"); } else { self.version = true; } }
            else if code == 2 {
                let mut triple = [0u8; 64];
                self.string_record(0, &mut triple);
                if cstr(&triple) != b"dxil-ms-dx" { self.fail("not a DXIL target triple"); } else { self.triple = true; }
            } else if code == 4 || code == 7 || code == 9 { self.fail("assembly/global memory is not implemented"); }
            else if code == 8 {
                if self.nr < 8 { self.fail("function declaration"); return; }
                let mut ty = self.rec[0] as i32;
                let mut t = self.ty(ty as i64 as u64);
                if t.kind == TY_PTR { ty = t.element as i32; t = self.ty(ty as i64 as u64); }
                if t.kind != TY_FUNCTION { self.fail("function type"); return; }
                let v = self.new_value(self.rec[0] as i32);
                let declaration = self.rec[2] != 0;
                let val = &mut self.values[v]; val.kind = V_FUNCTION; val.fn_type = ty as u16; val.declaration = declaration;
                if !declaration {
                    self.entry = self.nvalues - 1;
                    self.defined_functions += 1;
                    if self.defined_functions != 1 || t.count != 0 || self.ty(t.result as u64).kind != TY_VOID { self.fail("only one void entry-point function is implemented"); }
                }
            }
        }
    }
    fn wanted_block(&self, id: u32) -> bool {
        if self.pass == 0 { return id == 0 || id == 8 || id == 11 || id == 14 || id == 15 || id == 17; }
        id == 11 || id == 12
    }
    fn block(&mut self, id: u32, width: u32, end: u32, depth: i32) {
        if self.bad { return; }
        if depth > 16 || id as usize >= BC_BLOCKS || !(2..=16).contains(&width) || end > self.limit { self.fail("invalid block header"); return; }
        let parent_limit = self.limit; self.limit = end;
        let mut list = self.info[id as usize];
        let mut info_target: i32 = -1;
        while self.bit < end && !self.bad {
            let entry = self.bits(width) as u32;
            if entry == 0 { self.align32(); if self.bit != end { self.fail("block length mismatch"); } break; }
            if entry == 1 {
                let child = self.vbr(8) as u32;
                let cw = self.vbr(4) as u32;
                self.align32();
                let words = self.bits(32);
                let stop = self.bit as u64 + words * 32;
                if child as usize >= BC_BLOCKS || !(2..=16).contains(&cw) || stop > end as u64 { self.fail("sub-block is out of bounds"); break; }
                if child == 12 && self.pass == 0 {
                    if self.body_start != 0 { self.fail("multiple function bodies are not implemented"); break; }
                    self.body_start = self.bit; self.body_end = stop as u32; self.body_width = cw;
                } else if self.wanted_block(child) {
                    if child == 8 { if self.module_seen { self.fail("duplicate module"); } self.module_seen = true; }
                    self.block(child, cw, stop as u32, depth + 1);
                }
                self.bit = stop as u32;
            } else if entry == 2 {
                let ab = self.read_abbrev();
                let target: &mut Ablist = if id == 0 && info_target >= 0 { &mut self.info[info_target as usize] } else { &mut list };
                if target.n as usize == BC_BLOCK_ABBREVS { self.fail("too many block abbreviations"); break; }
                target.id[target.n as usize] = ab as u16; target.n += 1;
            } else {
                let code = self.record(&list, entry);
                if self.bad { break; }
                if id == 0 {
                    if code == 1 { if self.nr != 1 || self.rec[0] >= BC_BLOCKS as u64 { self.fail("invalid block-info target"); } else { info_target = self.rec[0] as i32; } }
                } else { self.on_record(id, code); }
            }
        }
        if self.bit > end { self.fail("block overrun"); }
        self.limit = parent_limit;
    }
}

/// Read a DXIL program part into `s` (signatures are already read from the container).
pub(crate) fn load(program: &[u8], s: &mut Shader, err: *mut c_char, errlen: c_int) -> bool {
    macro_rules! reject { ($m:expr) => {{ set_err_parts(err, errlen, &[b"DXIL: ", $m]); return false; }} }
    let size = program.len() as u32;
    if size < 24 || size & 3 != 0 || u32at(program, 8) != 0x4C49_5844 { reject!(b"bad program header"); }
    let (version, words, offset, length) = (u32at(program, 0), u32at(program, 4), u32at(program, 16), u32at(program, 20));
    if words < 6 || words > size / 4 || offset < 16 || offset > words * 4 - 8 || length > words * 4 - 8 - offset || length < 8 || length > 1024 * 1024 { reject!(b"bitcode is out of bounds"); }
    s.ty = (version >> 16) as i32; s.major = (version >> 4 & 15) as i32; s.minor = (version & 15) as i32; s.dxil = 1;
    if s.ty > 1 { reject!(b"only vertex/pixel stages are implemented; compute/geometry/tessellation are not"); }
    if s.major != 6 || s.minor != 0 || u32at(program, 12) != 0x100 { reject!(b"only the SM6.0 / DXIL 1.0 subset is implemented"); }
    let bytes = &program[(8 + offset) as usize..(8 + offset + length) as usize];
    if u32at(bytes, 0) != 0xDEC0_4342 { reject!(b"not LLVM bitcode"); }
    let mut p = Parser {
        bytes, bit: 32, limit: length * 8, s: s as *mut Shader, err, errlen, bad: false,
        ab: KArr::new(BC_ABBREVS), nab: 0, info: KArr::new(BC_BLOCKS), rec: KArr::new(BC_RECORD), nr: 0,
        types: KArr::new(BC_TYPES), ntypes: 0, expected_types: 0, constant_type: 0,
        values: KArr::new(BC_VALUES), nvalues: 0, module_values: 0, entry: -1, defined_functions: 0,
        md: KArr::new(BC_METADATA), nmd: 0, entry_metadata: -1, version_metadata: -1, model_metadata: -1,
        resources: [[Resource::default(); 16]; 4],
        sig_map: [[-1; DX_MAX_SIG]; 2], sig_col: [[0; DX_MAX_SIG]; 2], sig_columns: [[0; DX_MAX_SIG]; 2],
        metadata_name: [0; 96], body_start: 0, body_end: 0, body_width: 0,
        pass: 0, nblocks: 0, block: 0, block_pc: [0; BC_BBS], branch_a: [0; BC_BBS], branch_b: [0; BC_BBS],
        ins_cap: 0, pool_cap: 0, use_block: -1, dependencies: KArr::new(BC_BBS),
        triple: false, version: false, module_seen: false,
    };
    if !p.ab.ok() || !p.info.ok() || !p.rec.ok() || !p.types.ok() || !p.values.ok() || !p.md.ok() || !p.dependencies.ok() { p.fail("out of memory"); }
    if !p.bad { p.block(7, 2, length * 8, 0); }  /* the root is not a module block */
    if !p.bad && (!p.version || !p.triple || p.defined_functions != 1 || p.body_start == 0 || p.ntypes != p.expected_types) { p.fail("incomplete module"); }
    p.module_values = p.nvalues;
    if !p.bad {
        let version_md = p.node((p.version_metadata + 1) as u32, 2);
        let model_md = p.node((p.model_metadata + 1) as u32, 3);
        if !p.bad {
            let sm = p.md_at(model_md.refs[0]);
            let stage = p.md[sm];
            let want: &[u8] = if s_ty(&p) != 0 { b"vs" } else { b"ps" };
            if version_md.n != 2 || p.md_number(version_md.refs[0]) != 1 || p.md_number(version_md.refs[1]) != 0 ||
               model_md.n != 3 || stage.kind != 1 || cstr(&stage.text) != want ||
               p.md_number(model_md.refs[1]) != 6 || p.md_number(model_md.refs[2]) != 0 { p.fail("shader model/version metadata mismatch"); }
        }
    }
    if !p.bad { p.signature_metadata(); }
    if !p.bad {
        p.branch_a = [-1; BC_BBS]; p.branch_b = [-1; BC_BBS]; p.block_pc = [-1; BC_BBS];
        p.pass = 1; p.bit = p.body_start; p.limit = length * 8;
        let (w, e) = (p.body_width, p.body_end);
        p.block(12, w, e, 0);
        if p.block != p.nblocks { p.fail("unterminated function block"); }
    }
    if !p.bad {
        for i in 0..p.s().nins as usize {
            let ins = p.s().ins()[i];
            if ins.op == OP_BRANCH {
                let (j, a) = (p.block_pc[ins.jump as usize & (BC_BBS - 1)], p.block_pc[ins.alt as usize & (BC_BBS - 1)]);
                let x = &mut p.s().ins_mut()[i]; x.jump = j; x.alt = a;
            } else if ins.op == OP_PHI {
                let mut owner: i32 = -1;
                for b in 0..p.nblocks as usize { if p.block_pc[b] <= i as i32 { owner = b as i32; } }
                let (ja, jb) = (ins.jump as usize & (BC_BBS - 1), ins.alt as usize & (BC_BBS - 1));
                if owner < 0 || (p.branch_a[ja] != owner && p.branch_b[ja] != owner) || (p.branch_a[jb] != owner && p.branch_b[jb] != owner) { p.fail("phi predecessor mismatch"); }
                for b in 0..owner.max(0) as usize {
                    if (p.branch_a[b] == owner || p.branch_b[b] == owner) && b as i32 != ins.jump && b as i32 != ins.alt { p.fail("phi is missing a predecessor"); }
                }
            }
        }
    }
    if !p.bad {
        let mut dom = [[0u32; BC_BBS / 32]; BC_BBS];
        for b in 0..p.nblocks as usize {
            if p.bad { break; }
            let mut found = b == 0;
            for a in 0..b {
                if p.branch_a[a] == b as i32 || p.branch_b[a] == b as i32 {
                    if !found { dom[b] = dom[a]; } else { for k in 0..BC_BBS / 32 { dom[b][k] &= dom[a][k]; } }
                    found = true;
                }
            }
            if !found { p.fail("unreachable basic blocks are not implemented"); break; }
            dom[b][b / 32] |= 1 << (b & 31);
            for k in 0..BC_BBS / 32 { if p.dependencies[b][k] & !dom[b][k] != 0 { p.fail("SSA definition does not dominate its use"); } }
        }
    }
    if !p.bad {
        let mut written = [[0u8; DX_MAX_REG]; BC_BBS];
        let mut returns = [0u8; DX_MAX_REG];
        let mut have_return = false;
        let nins = p.s().nins;
        for b in 0..p.nblocks as usize {
            let mut first = true;
            for a in 0..b {
                if p.branch_a[a] == b as i32 || p.branch_b[a] == b as i32 {
                    if first { written[b] = written[a]; } else { for k in 0..DX_MAX_REG { written[b][k] &= written[a][k]; } }
                    first = false;
                }
            }
            let end = if b + 1 < p.nblocks as usize { p.block_pc[b + 1] } else { nins };
            for i in p.block_pc[b].max(0)..end {
                let ins = p.s().ins()[i as usize];
                if ins.op == OP_MOV {
                    let o = p.s().pool()[ins.o[0] as usize];
                    if o.ty == T_OUTPUT { if let Some(w) = written[b].get_mut(o.idx[0] as usize) { *w |= o.mask; } }
                    else if o.ty == T_ODEPTH { written[b][DX_MAX_REG - 1] |= 1; }
                } else if ins.op == OP_RET {
                    if !have_return { returns = written[b]; } else { for k in 0..DX_MAX_REG { returns[k] &= written[b][k]; } }
                    have_return = true;
                }
            }
        }
        p.s().definite_mask = returns;
    }
    !p.bad
}
fn s_ty(p: &Parser) -> i32 { p.s().ty }
