//! Expressions for gen.rs: operators, constructors, swizzles, indexing and the GLSL
//! built-in functions.

use super::*;

fn fk(v: &Val) -> Vec<f32> { Gen::kcomps(v).iter().map(|&x| f32::from_bits(x)).collect() }

impl Gen {
    pub fn ex(&mut self, e: &E) -> R<Val> {
        self.line = e.line;
        match &e.k {
            Ex::Int(v, u) => Ok(if *u { Val { ty: Ty::S(B::U, 1), ..ki(*v as i32) } } else { ki(*v as i32) }),
            Ex::Float(f) => Ok(kf(*f)),
            Ex::Bool(b) => Ok(Val { ty: Ty::S(B::Bo, 1), loc: Loc::K(vec![if *b { !0 } else { 0 }]), swz: [0; 4], lv: false }),
            Ex::Var(n) => self.lookup(n),
            Ex::Comma(a, b) => { self.ex(a)?; self.ex(b) }
            Ex::Un(op, a) => { let v = self.ex(a)?; self.unary(*op, v) }
            Ex::Bin(op, a, b) => {
                if matches!(op, Bin::And | Bin::Or) && Self::side_effects(b) { return self.short_circuit(*op, a, b); }
                let x = self.ex(a)?; let y = self.ex(b)?;
                self.line = e.line;
                self.binary(*op, x, y)
            }
            Ex::Assign(op, l, r) => {
                let d = self.lvalue(l)?;
                let v = self.ex(r)?;
                self.line = e.line;
                let v = match op { Some(o) => { let cur = self.read(&d)?; self.binary(*o, cur, v)? } None => v };
                self.assign(&d, v.clone())?;
                Ok(Val { lv: false, ..self.read(&d)? })
            }
            Ex::IncDec(pre, inc, a) => {
                let d = self.lvalue(a)?;
                let cur = self.read(&d)?;
                let old = if *pre { None } else { let t = self.temp(&cur.ty)?; self.copy(&t, &cur)?; Some(Val { lv: false, ..t }) };
                let one = match cur.ty.b() { B::F => kf(1.0), _ => ki(1) };
                let n = self.binary(if *inc { Bin::Add } else { Bin::Sub }, cur, one)?;
                self.assign(&d, n)?;
                match old { Some(o) => Ok(o), None => self.read(&d) }
            }
            Ex::Cond(c, a, b) => {
                let cv = self.ex(c)?; let cv = self.to_bool(cv)?;
                if let Loc::K(k) = &cv.loc { return if k[0] != 0 { self.ex(a) } else { self.ex(b) }; }
                if !Self::side_effects(a) && !Self::side_effects(b) {
                    let x = self.ex(a)?; let y = self.ex(b)?;
                    if let (Ty::S(xb, xn), Ty::S(yb, yn)) = (x.ty.clone(), y.ty.clone()) {
                        if xn == yn {
                            let bt = if xb == yb { xb } else { B::F };
                            let x = self.conv(x, bt)?; let y = self.conv(y, bt)?;
                            let cs = self.comp(&cv, 0);
                            return self.alu(OP_MOVC, Ty::S(bt, xn), &[&cs, &x, &y]);
                        }
                    }
                }
                let o = self.src(&cv)?;
                self.emit_nz(OP_IF, 1, &[o])?;
                let x = self.ex(a)?;
                let t = self.temp(&x.ty)?;
                self.store(&t, x)?;
                self.emit(OP_ELSE, &[])?;
                let y = self.ex(b)?;
                self.store(&t, y)?;
                self.emit(OP_ENDIF, &[])?;
                Ok(Val { lv: false, ..t })
            }
            Ex::Call(n, arr, args) => self.call(n, arr.as_deref(), args),
            Ex::Field(a, f) => {
                /* "block.member" of a named interface block */
                if let Ex::Var(bn) = &a.k {
                    let key = format!("\u{2}{}", bn);
                    if self.scopes[0].contains_key(&key) { return self.lookup(&format!("{}.{}", bn, f)); }
                }
                let v = self.ex(a)?;
                self.line = e.line;
                self.field(v, f)
            }
            Ex::Index(a, i) => {
                let v = self.ex(a)?;
                let iv = self.ex(i)?;
                self.line = e.line;
                self.index(v, iv)
            }
            Ex::Length(a) => {
                let v = self.ex(a)?;
                match v.ty { Ty::A(_, n) => Ok(ki(n as i32)), Ty::S(_, n) => Ok(ki(n as i32)), Ty::M(c, _) => Ok(ki(c as i32)), _ => self.err("length() of what?") }
            }
        }
    }
    fn side_effects(e: &E) -> bool {
        match &e.k {
            Ex::Assign(..) | Ex::IncDec(..) => true,
            Ex::Call(n, _, a) => { let builtin = BUILTIN_FNS.contains(&n.as_str()) || n.starts_with('\u{1}'); !builtin || a.iter().any(Self::side_effects) }
            Ex::Bin(_, a, b) | Ex::Comma(a, b) | Ex::Index(a, b) => Self::side_effects(a) || Self::side_effects(b),
            Ex::Un(_, a) | Ex::Field(a, _) | Ex::Length(a) => Self::side_effects(a),
            Ex::Cond(a, b, c) => Self::side_effects(a) || Self::side_effects(b) || Self::side_effects(c),
            _ => false,
        }
    }
    fn short_circuit(&mut self, op: Bin, a: &E, b: &E) -> R<Val> {
        let x = self.ex(a)?; let x = self.to_bool(x)?;
        let t = self.temp(&Ty::S(B::Bo, 1))?;
        self.copy(&t, &x)?;
        let c = if op == Bin::And { Val { lv: false, ..t.clone() } } else { self.alu(OP_NOT, Ty::S(B::Bo, 1), &[&t])? };
        let o = self.src(&c)?;
        self.emit_nz(OP_IF, 1, &[o])?;
        let y = self.ex(b)?; let y = self.to_bool(y)?;
        self.copy(&t, &y)?;
        self.emit(OP_ENDIF, &[])?;
        Ok(Val { lv: false, ..t })
    }

    /* ---------------- lvalues ---------------- */
    /// Assignments to v[i] with a dynamic i on a vector go component by component.
    fn lvalue(&mut self, e: &E) -> R<Val> {
        if let Ex::Index(a, i) = &e.k {
            let v = self.ex(a)?;
            if let Ty::S(..) = v.ty {
                let iv = self.ex(i)?;
                if !Self::is_const(&iv) {
                    if !v.lv { return self.err("cannot assign to this"); }
                    let iv = self.coerce(iv, &Ty::S(B::I, 1))?;
                    let iv = self.materialize(&iv)?;
                    /* encode as a Null-located marker carrying the vector and index */
                    self.dyn_lv.push((v.clone(), iv));
                    return Ok(Val { ty: Ty::S(v.ty.b(), 1), loc: Loc::Null, swz: [(self.dyn_lv.len() - 1) as u8; 4], lv: true });
                }
                return self.index(v, iv);
            }
        }
        let v = self.ex(e)?;
        if !v.lv { return self.err("cannot assign to this (it is read-only)"); }
        Ok(v)
    }
    fn read(&mut self, d: &Val) -> R<Val> {
        if let Loc::Null = d.loc { if d.lv { let (v, i) = self.dyn_lv[d.swz[0] as usize].clone(); return self.dyn_comp(&v, &i); } }
        Ok(d.clone())
    }
    fn assign(&mut self, d: &Val, v: Val) -> R<()> {
        if let Loc::Null = d.loc {
            if d.lv {
                let (vec_, i) = self.dyn_lv[d.swz[0] as usize].clone();
                let v = self.coerce(v, &d.ty)?;
                let n = vec_.ty.n();
                for k in 0..n {
                    let c = self.comp(&vec_, k as usize);
                    let eq = self.alu(OP_IEQ, Ty::S(B::Bo, 1), &[&i, &ki(k as i32)])?;
                    let m = self.alu(OP_MOVC, c.ty.clone(), &[&eq, &v, &c])?;
                    self.copy(&c, &m)?;
                }
                return Ok(());
            }
        }
        self.store(d, v)
    }
    fn dyn_comp(&mut self, v: &Val, i: &Val) -> R<Val> {
        let n = v.ty.n();
        let mut r = self.comp(v, 0);
        for k in 1..n {
            let c = self.comp(v, k as usize);
            let eq = self.alu(OP_IEQ, Ty::S(B::Bo, 1), &[i, &ki(k as i32)])?;
            r = self.alu(OP_MOVC, c.ty.clone(), &[&eq, &c, &r])?;
        }
        Ok(r)
    }

    /* ---------------- fields, swizzles, indexing ---------------- */
    fn field(&mut self, v: Val, f: &str) -> R<Val> {
        match &v.ty {
            Ty::St(i) => {
                let fields = self.structs[*i].fields.clone();
                let mut off = 0;
                for (n, t) in fields { if n == f { return self.part(&v, off, t); } off += self.regs(&t); }
                self.err(&format!("no field '{}'", f))
            }
            Ty::S(b, n) => {
                let b = *b; let n = *n;
                if f.len() > 4 { return self.err("swizzle too long"); }
                let mut sel = [0u8; 4];
                for (k, c) in f.bytes().enumerate() {
                    let idx = match c { b'x' | b'r' | b's' => 0, b'y' | b'g' | b't' => 1, b'z' | b'b' | b'p' => 2, b'w' | b'a' | b'q' => 3, _ => return self.err(&format!("bad swizzle '{}'", f)) };
                    if idx >= n { return self.err(&format!("swizzle '{}' is out of range", f)); }
                    sel[k] = idx;
                }
                let m = f.len() as u8;
                let dup = (0..m as usize).any(|a| (a + 1..m as usize).any(|c| sel[a] == sel[c]));
                match &v.loc {
                    Loc::K(c) => Ok(kvec(b, (0..m as usize).map(|k| c[(sel[k] as usize).min(c.len() - 1)]).collect())).map(|x| if m == 1 { Val { ty: Ty::S(b, 1), ..x } } else { x }),
                    _ => {
                        let mut s = [0u8; 4];
                        for k in 0..4 { s[k] = v.swz[sel[(k).min(m as usize - 1)] as usize]; }
                        Ok(Val { ty: Ty::S(b, m), loc: v.loc.clone(), swz: s, lv: v.lv && !dup })
                    }
                }
            }
            _ => self.err(&format!("'{}' is not a field of {}", f, self.tyname(&v.ty))),
        }
    }
    pub fn index(&mut self, v: Val, i: Val) -> R<Val> {
        let i = match i.ty { Ty::S(B::I, 1) | Ty::S(B::U, 1) => i, Ty::S(_, 1) => self.conv(i, B::I)?, _ => return self.err("an index must be an integer") };
        let (et, n) = match &v.ty {
            Ty::A(e, n) => ((**e).clone(), *n as i32),
            Ty::M(c, r) => (Ty::S(B::F, *r), *c as i32),
            Ty::S(_, n) => {
                if let Loc::K(k) = &i.loc { let k = k[0] as i32; if k < 0 || k >= *n as i32 { return self.err("index out of range"); } return Ok(self.comp(&v, k as usize)); }
                return self.dyn_comp(&v, &i);
            }
            _ => return self.err("this cannot be indexed"),
        };
        let er = self.regs(&et);
        if let Loc::K(k) = &i.loc {
            let k = k[0] as i32;
            if k < 0 || k >= n { return self.err("index out of range"); }
            return self.part(&v, k * er, et);
        }
        /* a dynamic index: a register offset in a temporary */
        let v = if let Loc::K(_) = v.loc { self.materialize(&v)? } else { v };
        if let Loc::Smp(_) = v.loc { return self.err("samplers can only be indexed with constants"); }
        let scaled = if er == 1 { self.materialize(&i)? } else { self.alu(OP_IMAD, Ty::S(B::I, 1), &[&i, &ki(er), &ki(0)])? };
        let rel = match &v.loc {
            Loc::R { rel: Some((r, l)), .. } => {
                let prev = Val { ty: Ty::S(B::I, 1), loc: Loc::R { f: T_TEMP, base: *r, rel: None }, swz: [*l; 4], lv: false };
                let s = self.alu(OP_IADD, Ty::S(B::I, 1), &[&prev, &scaled])?;
                (match s.loc { Loc::R { base, .. } => base, _ => 0 }, 0u8)
            }
            _ => match &scaled.loc { Loc::R { f: T_TEMP, base, rel: None } => (*base, scaled.swz[0]), _ => { let m = self.materialize(&scaled)?; (match m.loc { Loc::R { base, .. } => base, _ => 0 }, 0) } },
        };
        match v.loc {
            Loc::R { f, base, .. } => Ok(Val { ty: et, loc: Loc::R { f, base, rel: Some(rel) }, swz: [0, 1, 2, 3], lv: v.lv }),
            _ => self.err("this cannot be indexed"),
        }
    }

    /* ---------------- operators ---------------- */
    fn unary(&mut self, op: Un, v: Val) -> R<Val> {
        match op {
            Un::Plus => Ok(v),
            Un::Neg => match &v.ty {
                Ty::S(B::F, n) => { let n = *n; if Self::is_const(&v) { return Ok(kvec(B::F, fk(&v).iter().map(|x| (-x).to_bits()).collect())).map(|x| Val { ty: Ty::S(B::F, n), ..x }); } self.alu_md(OP_MOV, Ty::S(B::F, n), &[&v], &[1]) }
                Ty::S(B::I, n) | Ty::S(B::U, n) => { let (b, n) = (v.ty.b(), *n); if Self::is_const(&v) { return Ok(Val { ty: Ty::S(b, n), ..kvec(b, Self::kcomps(&v).iter().map(|x| x.wrapping_neg()).collect()) }); } self.alu(OP_INEG, Ty::S(b, n), &[&v]) }
                Ty::M(c, r) => { let (c, r) = (*c, *r); self.per_column(&v, None, Ty::M(c, r), |g, a, _| g.alu_md(OP_MOV, a.ty.clone(), &[a], &[1])) }
                _ => self.err("cannot negate this"),
            },
            Un::Not => { let n = v.ty.n(); if v.ty.b() != B::Bo { return self.err("! needs a bool"); } if Self::is_const(&v) { return Ok(Val { ty: Ty::S(B::Bo, n), ..kvec(B::Bo, Self::kcomps(&v).iter().map(|x| !x).collect()) }); } self.alu(OP_NOT, Ty::S(B::Bo, n), &[&v]) }
            Un::BNot => { let t = v.ty.clone(); self.alu(OP_NOT, t, &[&v]) }
        }
    }
    fn per_column<F: FnMut(&mut Gen, &Val, Option<&Val>) -> R<Val>>(&mut self, a: &Val, b: Option<&Val>, rt: Ty, mut f: F) -> R<Val> {
        let (c, r) = match rt { Ty::M(c, r) => (c, r), _ => return self.err("matrix expected") };
        let out = self.temp(&rt)?;
        for k in 0..c as i32 {
            let ca = if let Ty::M(..) = a.ty { self.part(a, k, Ty::S(B::F, r))? } else { a.clone() };
            let cb = match b { Some(b) => Some(if let Ty::M(..) = b.ty { self.part(b, k, Ty::S(B::F, r))? } else { b.clone() }), None => None };
            let v = f(self, &ca, cb.as_ref())?;
            let d = self.part(&out, k, Ty::S(B::F, r))?;
            self.copy(&d, &v)?;
        }
        Ok(Val { lv: false, ..out })
    }
    fn fold(&self, op: Bin, b: B, x: &Val, y: &Val, n: u8) -> Option<Val> {
        if !Self::is_const(x) || !Self::is_const(y) { return None; }
        let xa = Self::kcomps(x); let ya = Self::kcomps(y);
        let get = |a: &Vec<u32>, i: usize| a[i.min(a.len() - 1)];
        let mut o = Vec::new();
        for i in 0..n as usize {
            let (p, q) = (get(&xa, i), get(&ya, i));
            let r = match b {
                B::F => { let (p, q) = (f32::from_bits(p), f32::from_bits(q)); match op { Bin::Add => (p + q).to_bits(), Bin::Sub => (p - q).to_bits(), Bin::Mul => (p * q).to_bits(), Bin::Div => (p / q).to_bits(), _ => return None } }
                B::I => { let (p, q) = (p as i32, q as i32); (match op { Bin::Add => p.wrapping_add(q), Bin::Sub => p.wrapping_sub(q), Bin::Mul => p.wrapping_mul(q), Bin::Div => if q == 0 { return None } else { p.wrapping_div(q) }, Bin::Mod => if q == 0 { return None } else { p.wrapping_rem(q) }, Bin::Shl => p.wrapping_shl(q as u32), Bin::Shr => p.wrapping_shr(q as u32), Bin::BAnd => p & q, Bin::BOr => p | q, Bin::BXor => p ^ q, _ => return None }) as u32 }
                B::U => match op { Bin::Add => p.wrapping_add(q), Bin::Sub => p.wrapping_sub(q), Bin::Mul => p.wrapping_mul(q), Bin::Div => if q == 0 { return None } else { p / q }, Bin::Mod => if q == 0 { return None } else { p % q }, Bin::Shl => p.wrapping_shl(q), Bin::Shr => p.wrapping_shr(q), Bin::BAnd => p & q, Bin::BOr => p | q, Bin::BXor => p ^ q, _ => return None },
                B::Bo => return None,
            };
            o.push(r);
        }
        Some(Val { ty: Ty::S(b, n), ..kvec(b, o) })
    }
    pub fn binary(&mut self, op: Bin, x: Val, y: Val) -> R<Val> {
        use Bin::*;
        match op {
            And | Or | Xor => {
                let x = self.to_bool(x)?; let y = self.to_bool(y)?;
                if Self::is_const(&x) && Self::is_const(&y) { let (a, b) = (Self::kcomps(&x)[0] != 0, Self::kcomps(&y)[0] != 0); return Ok(kb_(match op { And => a && b, Or => a || b, _ => a != b })); }
                let o = match op { And => OP_AND, Or => OP_OR, _ => OP_XOR };
                return self.alu(o, Ty::S(B::Bo, 1), &[&x, &y]);
            }
            Eq | Ne => return self.equality(op == Eq, x, y),
            _ => {}
        }
        /* matrices */
        if matches!(x.ty, Ty::M(..)) || matches!(y.ty, Ty::M(..)) { return self.mat_binary(op, x, y); }
        let (xb, xn) = match x.ty { Ty::S(b, n) => (b, n), _ => return self.err(&format!("cannot use {} in arithmetic", self.tyname(&x.ty))) };
        let (yb, yn) = match y.ty { Ty::S(b, n) => (b, n), _ => return self.err(&format!("cannot use {} in arithmetic", self.tyname(&y.ty))) };
        if xn != yn && xn != 1 && yn != 1 { return self.err(&format!("cannot combine {} and {}", self.tyname(&x.ty), self.tyname(&y.ty))); }
        let n = xn.max(yn);
        let b = if xb == yb { xb } else if xb == B::F || yb == B::F { B::F } else if matches!((xb, yb), (B::I, B::U) | (B::U, B::I)) { B::U } else { return self.err("cannot combine bool and number") };
        if b == B::Bo { return self.err("arithmetic on bool"); }
        let x = self.conv(x, b)?; let y = self.conv(y, b)?;
        if let Some(v) = self.fold(op, b, &x, &y, n) { return Ok(v); }
        let t = Ty::S(b, n);
        let bt = Ty::S(B::Bo, n);
        match (op, b) {
            (Add, B::F) => self.alu(OP_ADD, t, &[&x, &y]),
            (Sub, B::F) => self.alu_md(OP_ADD, t, &[&x, &y], &[0, 1]),
            (Mul, B::F) => self.alu(OP_MUL, t, &[&x, &y]),
            (Div, B::F) => self.alu(OP_DIV, t, &[&x, &y]),
            (Mod, B::F) => self.fmod(&x, &y, n),
            (Add, _) => self.alu(OP_IADD, t, &[&x, &y]),
            (Sub, _) => self.alu_md(OP_IADD, t, &[&x, &y], &[0, 1]),
            (Mul, _) => { let d = self.temp(&t)?; let nl = self.null()?; let dd = self.dst(&d)?; let a = self.src(&x)?; let c = self.src(&y)?; self.emit(OP_IMUL, &[nl, dd, a, c])?; Ok(Val { lv: false, ..d }) }
            (Div, B::U) | (Mod, B::U) => { let q = self.temp(&t)?; let r = self.temp(&t)?; let dq = self.dst(&q)?; let dr = self.dst(&r)?; let a = self.src(&x)?; let c = self.src(&y)?; self.emit(OP_UDIV, &[dq, dr, a, c])?; Ok(Val { lv: false, ..if op == Div { q } else { r } }) }
            (Div, B::I) | (Mod, B::I) => {
                let q = self.temp(&t)?; let r = self.temp(&t)?;
                let dq = self.dst(&q)?; let dr = self.dst(&r)?; let a = self.src_md(&x, 2)?; let c = self.src_md(&y, 2)?;
                self.emit(OP_UDIV, &[dq, dr, a, c])?;
                if op == Div {
                    let s = self.alu(OP_XOR, t.clone(), &[&x, &y])?;
                    let neg = self.alu(OP_ILT, bt, &[&s, &ki(0)])?;
                    let nq = self.alu(OP_INEG, t.clone(), &[&q])?;
                    self.alu(OP_MOVC, t, &[&neg, &nq, &q])
                } else {
                    let neg = self.alu(OP_ILT, bt, &[&x, &ki(0)])?;
                    let nr = self.alu(OP_INEG, t.clone(), &[&r])?;
                    self.alu(OP_MOVC, t, &[&neg, &nr, &r])
                }
            }
            (Shl, _) => self.alu(OP_ISHL, t, &[&x, &y]),
            (Shr, B::I) => self.alu(OP_ISHR, t, &[&x, &y]),
            (Shr, _) => self.alu(OP_USHR, t, &[&x, &y]),
            (BAnd, _) => self.alu(OP_AND, t, &[&x, &y]),
            (BOr, _) => self.alu(OP_OR, t, &[&x, &y]),
            (BXor, _) => self.alu(OP_XOR, t, &[&x, &y]),
            (Lt | Gt | Le | Ge, _) => {
                if n != 1 { return self.err("comparisons need scalars (use lessThan and friends for vectors)"); }
                if Self::is_const(&x) && Self::is_const(&y) {
                    let (p, q) = (Self::kcomps(&x)[0], Self::kcomps(&y)[0]);
                    let r = match b { B::F => { let (p, q) = (f32::from_bits(p), f32::from_bits(q)); match op { Lt => p < q, Gt => p > q, Le => p <= q, _ => p >= q } }
                                      B::I => { let (p, q) = (p as i32, q as i32); match op { Lt => p < q, Gt => p > q, Le => p <= q, _ => p >= q } }
                                      _ => match op { Lt => p < q, Gt => p > q, Le => p <= q, _ => p >= q } };
                    return Ok(kb_(r));
                }
                let (lt, ge) = match b { B::F => (OP_LT, OP_GE), B::I => (OP_ILT, OP_IGE), _ => (OP_ULT, OP_UGE) };
                match op { Lt => self.alu(lt, bt, &[&x, &y]), Gt => self.alu(lt, bt, &[&y, &x]), Le => self.alu(ge, bt, &[&y, &x]), _ => self.alu(ge, bt, &[&x, &y]) }
            }
            _ => self.err("this operator does not apply here"),
        }
    }
    fn fmod(&mut self, x: &Val, y: &Val, n: u8) -> R<Val> {
        let t = Ty::S(B::F, n);
        let q = self.alu(OP_DIV, t.clone(), &[x, y])?;
        let f = self.alu(OP_ROUND_NI, t.clone(), &[&q])?;
        self.alu_md(OP_MAD, t, &[y, &f, x], &[1, 0, 0])
    }
    fn equality(&mut self, eq: bool, x: Val, y: Val) -> R<Val> {
        let (x, y) = match (&x.ty, &y.ty) {
            (Ty::S(a, n), Ty::S(b, m)) if n == m && a != b => { let t = if *a == B::F || *b == B::F { B::F } else { *a }; (self.conv(x, t)?, self.conv(y, t)?) }
            _ => (x, y),
        };
        if x.ty != y.ty { return self.err(&format!("cannot compare {} and {}", self.tyname(&x.ty), self.tyname(&y.ty))); }
        if Self::is_const(&x) && Self::is_const(&y) {
            let same = if x.ty.b() == B::F { fk(&x) == fk(&y) } else { Self::kcomps(&x) == Self::kcomps(&y) };
            return Ok(kb_(same == eq));
        }
        let all = self.eq_all(&x, &y)?;
        if eq { Ok(all) } else { self.alu(OP_NOT, Ty::S(B::Bo, 1), &[&all]) }
    }
    /// One bool: every component of x equals y's.
    fn eq_all(&mut self, x: &Val, y: &Val) -> R<Val> {
        match &x.ty {
            Ty::S(b, n) => {
                let (b, n) = (*b, *n);
                let e = self.alu(if b == B::F { OP_EQ } else { OP_IEQ }, Ty::S(B::Bo, n), &[x, y])?;
                let mut r = self.comp(&e, 0);
                for k in 1..n as usize { let c = self.comp(&e, k); r = self.alu(OP_AND, Ty::S(B::Bo, 1), &[&r, &c])?; }
                Ok(r)
            }
            Ty::M(c, rr) => { let (c, rr) = (*c, *rr); let mut r = kb(true); for k in 0..c as i32 { let a = self.part(x, k, Ty::S(B::F, rr))?; let b = self.part(y, k, Ty::S(B::F, rr))?; let e = self.eq_all(&a, &b)?; r = if k == 0 { e } else { self.alu(OP_AND, Ty::S(B::Bo, 1), &[&r, &e])? }; } Ok(r) }
            Ty::A(e, n) => { let (e, n) = ((**e).clone(), *n); let er = self.regs(&e); let mut r = kb(true); for k in 0..n as i32 { let a = self.part(x, k * er, e.clone())?; let b = self.part(y, k * er, e.clone())?; let q = self.eq_all(&a, &b)?; r = if k == 0 { q } else { self.alu(OP_AND, Ty::S(B::Bo, 1), &[&r, &q])? }; } Ok(r) }
            Ty::St(i) => {
                let f = self.structs[*i].fields.clone(); let mut off = 0; let mut r = kb(true); let mut first = true;
                for (_, ft) in f { let a = self.part(x, off, ft.clone())?; let b = self.part(y, off, ft.clone())?; let q = self.eq_all(&a, &b)?; r = if first { q } else { self.alu(OP_AND, Ty::S(B::Bo, 1), &[&r, &q])? }; first = false; off += self.regs(&ft); }
                Ok(r)
            }
            _ => self.err("cannot compare this"),
        }
    }
    fn mat_binary(&mut self, op: Bin, x: Val, y: Val) -> R<Val> {
        let x = if let Ty::S(B::I, _) | Ty::S(B::U, _) = x.ty { self.conv(x, B::F)? } else { x };
        let y = if let Ty::S(B::I, _) | Ty::S(B::U, _) = y.ty { self.conv(y, B::F)? } else { y };
        match (op, x.ty.clone(), y.ty.clone()) {
            (Bin::Mul, Ty::M(c, r), Ty::S(B::F, n)) if n == c => self.mat_vec(&x, &y, c, r),
            (Bin::Mul, Ty::S(B::F, n), Ty::M(c, r)) if n == r => {
                /* v * M: component k is dot(v, column k) */
                let out = self.temp(&Ty::S(B::F, c))?;
                for k in 0..c as usize {
                    let col = self.part(&x_or(&y), k as i32, Ty::S(B::F, r))?;
                    let d = self.comp(&out, k);
                    let dd = self.dst(&d)?; let a = self.src(&x)?; let b = self.src(&col)?;
                    let op = match r { 2 => OP_DP2, 3 => OP_DP3, _ => OP_DP4 };
                    self.emit(op, &[dd, a, b])?;
                }
                Ok(Val { lv: false, ..out })
            }
            (Bin::Mul, Ty::M(c1, r1), Ty::M(c2, r2)) if c1 == r2 => {
                let out = self.temp(&Ty::M(c2, r1))?;
                for k in 0..c2 as i32 {
                    let col = self.part(&y, k, Ty::S(B::F, r2))?;
                    let v = self.mat_vec(&x, &col, c1, r1)?;
                    let d = self.part(&out, k, Ty::S(B::F, r1))?;
                    self.copy(&d, &v)?;
                }
                Ok(Val { lv: false, ..out })
            }
            (Bin::Add | Bin::Sub | Bin::Mul | Bin::Div, Ty::M(c, r), _) | (Bin::Add | Bin::Sub | Bin::Mul | Bin::Div, _, Ty::M(c, r)) => {
                let ok = match (&x.ty, &y.ty) { (Ty::M(a, b), Ty::M(cc, d)) => a == cc && b == d && op != Bin::Mul, (Ty::M(..), Ty::S(B::F, 1)) | (Ty::S(B::F, 1), Ty::M(..)) => true, _ => false };
                if !ok { return self.err("these matrix types do not combine"); }
                self.per_column(&x, Some(&y), Ty::M(c, r), |g, a, b| g.binary(op, a.clone(), b.unwrap().clone()))
            }
            _ => self.err(&format!("cannot combine {} and {}", self.tyname(&x.ty), self.tyname(&y.ty))),
        }
    }
    fn mat_vec(&mut self, m: &Val, v: &Val, c: u8, r: u8) -> R<Val> {
        let t = Ty::S(B::F, r);
        let c0 = self.part(m, 0, t.clone())?;
        let v0 = self.comp(v, 0);
        let mut acc = self.alu(OP_MUL, t.clone(), &[&c0, &v0])?;
        for k in 1..c as usize {
            let ck = self.part(m, k as i32, t.clone())?;
            let vk = self.comp(v, k);
            acc = self.alu(OP_MAD, t.clone(), &[&ck, &vk, &acc])?;
        }
        Ok(acc)
    }

    /* ---------------- constructors ---------------- */
    pub fn ctor(&mut self, t: &Ty, args: Vec<Val>) -> R<Val> {
        match t {
            Ty::S(b, n) => {
                let (b, n) = (*b, *n);
                if args.len() == 1 { if let Ty::S(_, 1) = args[0].ty {
                    let s = self.conv(args[0].clone(), b)?;
                    if let Loc::K(c) = &s.loc { return Ok(Val { ty: Ty::S(b, n), ..kvec(b, vec![c[0]; n as usize]) }); }
                    let mut v = s; v.ty = Ty::S(b, n); v.lv = false; v.swz = [v.swz[0]; 4]; return Ok(v);
                } }
                /* the components of the arguments, in order */
                let mut comps: Vec<Val> = Vec::new();
                for a in &args {
                    match &a.ty {
                        Ty::S(_, m) => { let m = *m; let a = self.conv(a.clone(), b)?; for k in 0..m as usize { comps.push(self.comp(&a, k)); } }
                        Ty::M(c, r) => { let (c, r) = (*c, *r); for k in 0..c as i32 { let col = self.part(a, k, Ty::S(B::F, r))?; let col = self.conv(col, b)?; for j in 0..r as usize { comps.push(self.comp(&col, j)); } } }
                        _ => return self.err(&format!("cannot build {} from {}", self.tyname(t), self.tyname(&a.ty))),
                    }
                }
                if comps.len() < n as usize { return self.err(&format!("not enough values for {}", self.tyname(t))); }
                comps.truncate(n as usize);
                if comps.iter().all(Self::is_const) { return Ok(Val { ty: Ty::S(b, n), ..kvec(b, comps.iter().map(|c| Self::kcomps(c)[0]).collect()) }); }
                /* all from one register in one move when possible */
                let out = self.temp(&Ty::S(b, n))?;
                let mut k = 0;
                while k < comps.len() {
                    let c = comps[k].clone();
                    let mut j = k + 1;
                    let same = |a: &Val, b: &Val| match (&a.loc, &b.loc) { (Loc::R { f, base, rel }, Loc::R { f: f2, base: b2, rel: r2 }) => f == f2 && base == b2 && rel == r2, _ => false };
                    while j < comps.len() && same(&comps[j], &c) { j += 1; }
                    let mut src = c.clone();
                    if let Loc::R { .. } = c.loc { src.ty = Ty::S(b, (j - k) as u8); for q in 0..(j - k) { src.swz[q] = comps[k + q].swz[0]; } for q in (j - k)..4 { src.swz[q] = src.swz[j - k - 1]; } }
                    else { j = k + 1; }
                    let mut d = out.clone();
                    d.ty = Ty::S(b, (j - k) as u8);
                    for q in 0..4 { d.swz[q] = ((k + q.min(j - k - 1)) as u8).min(3); }
                    self.mov_to(&d, &src)?;
                    k = j;
                }
                Ok(Val { lv: false, ..out })
            }
            Ty::M(c, r) => {
                let (c, r) = (*c, *r);
                let out = self.temp(t)?;
                if args.len() == 1 {
                    match args[0].ty.clone() {
                        Ty::S(_, 1) => {
                            let s = self.conv(args[0].clone(), B::F)?;
                            for k in 0..c as usize { let col = self.part(&out, k as i32, Ty::S(B::F, r))?; let mut v = vec![kf(0.0); r as usize]; if k < r as usize { v[k] = s.clone(); } let cv = self.ctor(&Ty::S(B::F, r), v)?; self.copy(&col, &cv)?; }
                            return Ok(Val { lv: false, ..out });
                        }
                        Ty::M(c2, r2) => {
                            for k in 0..c as usize {
                                let col = self.part(&out, k as i32, Ty::S(B::F, r))?;
                                let mut v = Vec::new();
                                for j in 0..r as usize {
                                    if k < c2 as usize && j < r2 as usize { let sc = self.part(&args[0], k as i32, Ty::S(B::F, r2))?; v.push(self.comp(&sc, j)); }
                                    else { v.push(kf(if j == k { 1.0 } else { 0.0 })); }
                                }
                                let cv = self.ctor(&Ty::S(B::F, r), v)?; self.copy(&col, &cv)?;
                            }
                            return Ok(Val { lv: false, ..out });
                        }
                        _ => {}
                    }
                }
                let mut comps = Vec::new();
                for a in &args {
                    match &a.ty {
                        Ty::S(_, m) => { let m = *m; let a = self.conv(a.clone(), B::F)?; for k in 0..m as usize { comps.push(self.comp(&a, k)); } }
                        Ty::M(c2, r2) => { let (c2, r2) = (*c2, *r2); for k in 0..c2 as i32 { let col = self.part(a, k, Ty::S(B::F, r2))?; for j in 0..r2 as usize { comps.push(self.comp(&col, j)); } } }
                        _ => return self.err("bad matrix constructor"),
                    }
                }
                if comps.len() < (c * r) as usize { return self.err("not enough values for the matrix"); }
                for k in 0..c as usize {
                    let col = self.part(&out, k as i32, Ty::S(B::F, r))?;
                    let cv = self.ctor(&Ty::S(B::F, r), comps[k * r as usize..(k + 1) * r as usize].to_vec())?;
                    self.copy(&col, &cv)?;
                }
                Ok(Val { lv: false, ..out })
            }
            Ty::St(i) => {
                let f = self.structs[*i].fields.clone();
                if f.len() != args.len() { return self.err(&format!("{} needs {} values", self.structs[*i].name, f.len())); }
                let out = self.temp(t)?;
                let mut off = 0;
                for (k, (_, ft)) in f.iter().enumerate() { let d = self.part(&out, off, ft.clone())?; self.store(&d, args[k].clone())?; off += self.regs(ft); }
                Ok(Val { lv: false, ..out })
            }
            Ty::A(e, n) => {
                if args.len() != *n as usize { return self.err("wrong number of array elements"); }
                let out = self.temp(t)?;
                let er = self.regs(e);
                for (k, a) in args.into_iter().enumerate() { let d = self.part(&out, k as i32 * er, (**e).clone())?; self.store(&d, a)?; }
                Ok(Val { lv: false, ..out })
            }
            _ => self.err("cannot construct this type"),
        }
    }

    /* ---------------- calls ---------------- */
    fn call(&mut self, n: &str, arr: Option<&E>, args: &[E]) -> R<Val> {
        let line = self.line;
        /* constructors */
        let is_type = n != "\u{1}list" && self.base_type(n).is_ok();
        if is_type {
            let mut t = self.base_type(n)?;
            let mut vals = Vec::new();
            for a in args { vals.push(self.ex(a)?); }
            self.line = line;
            if let Some(sz) = arr { let k = self.const_int(sz)?; let len = if k < 0 { vals.len() as u32 } else { k as u32 }; t = Ty::A(Box::new(t), len); }
            return self.ctor(&t, vals);
        }
        if let Some(v) = self.call_user(n, args)? { return Ok(v); }
        let mut vals = Vec::new();
        for a in args { vals.push(self.ex(a)?); }
        self.line = line;
        self.builtin(n, vals)
    }
    fn fl(&mut self, v: Val) -> R<Val> { match v.ty { Ty::S(B::F, _) => Ok(v), Ty::S(_, _) => self.conv(v, B::F), _ => self.err("a float or vector is needed") } }
    fn dot(&mut self, a: &Val, b: &Val) -> R<Val> {
        let n = a.ty.n();
        if n == 1 { return self.alu(OP_MUL, Ty::S(B::F, 1), &[a, b]); }
        let op = match n { 2 => OP_DP2, 3 => OP_DP3, _ => OP_DP4 };
        self.alu(op, Ty::S(B::F, 1), &[a, b])
    }
    fn atan1(&mut self, x: &Val) -> R<Val> {
        /* atan on [-1, 1] by a polynomial, with 1/x outside it */
        let t = x.ty.clone(); let bt = Ty::S(B::Bo, t.n());
        let a = self.alu_md(OP_MOV, t.clone(), &[x], &[2])?;
        let big = self.alu(OP_LT, bt, &[&kf(1.0), &a])?;
        let inv = self.alu(OP_DIV, t.clone(), &[&kf(1.0), &a])?;
        let z = self.alu(OP_MOVC, t.clone(), &[&big, &inv, &a])?;
        let z2 = self.alu(OP_MUL, t.clone(), &[&z, &z])?;
        let mut p = kf(0.020_835_1);
        for c in [-0.085_133, 0.180_141, -0.330_299_5, 0.999_866] { p = self.alu(OP_MAD, t.clone(), &[&p, &z2, &kf(c)])?; }
        let p = self.alu(OP_MUL, t.clone(), &[&p, &z])?;
        let q = self.alu_md(OP_ADD, t.clone(), &[&kf(PI / 2.0), &p], &[0, 1])?;
        let r = self.alu(OP_MOVC, t.clone(), &[&big, &q, &p])?;
        let neg = self.alu(OP_LT, Ty::S(B::Bo, t.n()), &[x, &kf(0.0)])?;
        let nr = self.alu_md(OP_MOV, t.clone(), &[&r], &[1])?;
        self.alu(OP_MOVC, t, &[&neg, &nr, &r])
    }
    fn atan2(&mut self, y: &Val, x: &Val) -> R<Val> {
        let t = y.ty.clone(); let n = t.n(); let bt = Ty::S(B::Bo, n);
        let q = self.alu(OP_DIV, t.clone(), &[y, x])?;
        let a = self.atan1(&q)?;
        let xneg = self.alu(OP_LT, bt.clone(), &[x, &kf(0.0)])?;
        let yneg = self.alu(OP_LT, bt.clone(), &[y, &kf(0.0)])?;
        let pi = self.alu(OP_MOVC, t.clone(), &[&yneg, &kf(-PI), &kf(PI)])?;
        let ap = self.alu(OP_ADD, t.clone(), &[&a, &pi])?;
        let r = self.alu(OP_MOVC, t.clone(), &[&xneg, &ap, &a])?;
        let xz = self.alu(OP_EQ, bt, &[x, &kf(0.0)])?;
        let h = self.alu(OP_MOVC, t.clone(), &[&yneg, &kf(-PI / 2.0), &kf(PI / 2.0)])?;
        self.alu(OP_MOVC, t, &[&xz, &h, &r])
    }
    fn sample(&mut self, s: &Val, uv: &Val, op: u16, extra: Option<&Val>) -> R<Val> {
        let slot = match s.loc { Loc::Smp(k) => k, _ => return self.err("the first argument must be a sampler") };
        if let Ty::Smp(3) = s.ty { /* shadow comparisons are not implemented: lit */ return Ok(kvec(B::F, vec![FONE; 4])); }
        let out = self.temp(&Ty::S(B::F, 4))?;
        let d = self.dst(&out)?;
        let c = self.src(uv)?;
        let r = self.push_op(Opnd { ty: T_RESOURCE, mode: 1, mask: 15, dim: 1, idx: [slot, 0, 0], rel: [-1; 3], swz: [0, 1, 2, 3], ..Default::default() })?;
        let sm = self.push_op(Opnd { ty: T_SAMPLER, mode: 0, mask: 15, dim: 1, idx: [slot, 0, 0], rel: [-1; 3], swz: [0, 1, 2, 3], ..Default::default() })?;
        match extra {
            Some(x) => { let x = self.fl(x.clone())?; let e = self.src(&x)?; self.emit(op, &[d, c, r, sm, e])?; }
            None => { self.emit(OP_SAMPLE, &[d, c, r, sm])?; }
        }
        Ok(Val { lv: false, ..out })
    }
    fn proj(&mut self, uv: &Val) -> R<Val> {
        let n = uv.ty.n();
        let w = self.comp(uv, (n - 1) as usize);
        let xy = self.field(uv.clone(), "xy")?;
        self.alu(OP_DIV, Ty::S(B::F, 2), &[&xy, &w])
    }
    fn builtin(&mut self, n: &str, a: Vec<Val>) -> R<Val> {
        let na = a.len();
        let need = |g: &Gen, k: usize| -> R<()> { if na != k { g.err(&format!("{} takes {} arguments", n, k)) } else { Ok(()) } };
        macro_rules! f1 { () => {{ need(self, 1)?; let x = self.fl(a[0].clone())?; x }} }
        let tf = |v: &Val| v.ty.clone();
        match n {
            "radians" | "degrees" => { let x = f1!(); let k = if n == "radians" { PI / 180.0 } else { 180.0 / PI }; self.alu(OP_MUL, tf(&x), &[&x, &kf(k)]) }
            "sin" | "cos" => {
                let x = f1!();
                if Self::is_const(&x) { let c = fk(&x); return Ok(Val { ty: x.ty.clone(), ..kvec(B::F, c.iter().map(|v| if n == "sin" { libm_sin(*v) } else { libm_sin(*v + PI / 2.0) }.to_bits()).collect()) }); }
                let out = self.temp(&x.ty)?;
                let d = self.dst(&out)?; let nl = self.null()?; let s = self.src(&x)?;
                if n == "sin" { self.emit(OP_SINCOS, &[d, nl, s])?; } else { self.emit(OP_SINCOS, &[nl, d, s])?; }
                Ok(Val { lv: false, ..out })
            }
            "tan" => { let x = f1!(); let s = self.builtin("sin", vec![x.clone()])?; let c = self.builtin("cos", vec![x.clone()])?; self.alu(OP_DIV, tf(&x), &[&s, &c]) }
            "atan" => {
                if na == 2 { let y = self.fl(a[0].clone())?; let x = self.fl(a[1].clone())?; return self.atan2(&y, &x); }
                let x = f1!(); self.atan1(&x)
            }
            "asin" | "acos" => {
                let x = f1!(); let t = tf(&x);
                let x2 = self.alu(OP_MUL, t.clone(), &[&x, &x])?;
                let c = self.alu_md(OP_ADD, t.clone(), &[&kf(1.0), &x2], &[0, 1])?;
                let c = self.alu(OP_MAX, t.clone(), &[&c, &kf(0.0)])?;
                let c = self.alu(OP_SQRT, t.clone(), &[&c])?;
                let s = self.atan2(&x, &c)?;
                if n == "asin" { Ok(s) } else { self.alu_md(OP_ADD, t, &[&kf(PI / 2.0), &s], &[0, 1]) }
            }
            "sinh" | "cosh" | "tanh" => {
                let x = f1!(); let t = tf(&x);
                let e = self.alu(OP_MUL, t.clone(), &[&x, &kf(core::f32::consts::LOG2_E)])?;
                let p = self.alu(OP_EXP, t.clone(), &[&e])?;
                let m = self.alu_md(OP_EXP, t.clone(), &[&e], &[1])?;
                match n {
                    "sinh" => { let d = self.alu_md(OP_ADD, t.clone(), &[&p, &m], &[0, 1])?; self.alu(OP_MUL, t, &[&d, &kf(0.5)]) }
                    "cosh" => { let d = self.alu(OP_ADD, t.clone(), &[&p, &m])?; self.alu(OP_MUL, t, &[&d, &kf(0.5)]) }
                    _ => { let d = self.alu_md(OP_ADD, t.clone(), &[&p, &m], &[0, 1])?; let s = self.alu(OP_ADD, t.clone(), &[&p, &m])?; self.alu(OP_DIV, t, &[&d, &s]) }
                }
            }
            "pow" => {
                need(self, 2)?; let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?; let t = tf(&x);
                if Self::is_const(&y) && fk(&y).iter().all(|v| *v == 2.0) { return self.alu(OP_MUL, t, &[&x, &x]); }
                let l = self.alu(OP_LOG, t.clone(), &[&x])?;
                let m = self.alu(OP_MUL, t.clone(), &[&l, &y])?;
                self.alu(OP_EXP, t, &[&m])
            }
            "exp" => { let x = f1!(); let m = self.alu(OP_MUL, tf(&x), &[&x, &kf(core::f32::consts::LOG2_E)])?; self.alu(OP_EXP, tf(&x), &[&m]) }
            "log" => { let x = f1!(); let l = self.alu(OP_LOG, tf(&x), &[&x])?; self.alu(OP_MUL, tf(&x), &[&l, &kf(core::f32::consts::LN_2)]) }
            "exp2" => { let x = f1!(); self.alu(OP_EXP, tf(&x), &[&x]) }
            "log2" => { let x = f1!(); self.alu(OP_LOG, tf(&x), &[&x]) }
            "sqrt" => { let x = f1!(); self.alu(OP_SQRT, tf(&x), &[&x]) }
            "inversesqrt" => { let x = f1!(); self.alu(OP_RSQ, tf(&x), &[&x]) }
            "abs" => {
                need(self, 1)?; let x = a[0].clone();
                match x.ty.b() { B::F => self.alu_md(OP_MOV, tf(&x), &[&x], &[2]), _ => self.alu_md(OP_IADD, tf(&x), &[&x, &ki(0)], &[2, 0]) }
            }
            "sign" => {
                need(self, 1)?; let x = a[0].clone(); let t = tf(&x); let bt = Ty::S(B::Bo, t.n());
                if x.ty.b() == B::F {
                    let gt = self.alu(OP_LT, bt.clone(), &[&kf(0.0), &x])?; let lt = self.alu(OP_LT, bt, &[&x, &kf(0.0)])?;
                    let p = self.alu(OP_AND, t.clone(), &[&gt, &kvec(B::U, vec![FONE])])?; let m = self.alu(OP_AND, t.clone(), &[&lt, &kvec(B::U, vec![FONE])])?;
                    self.alu_md(OP_ADD, t, &[&p, &m], &[0, 1])
                } else {
                    let gt = self.alu(OP_ILT, bt.clone(), &[&ki(0), &x])?; let lt = self.alu(OP_ILT, bt, &[&x, &ki(0)])?;
                    let p = self.alu(OP_AND, t.clone(), &[&gt, &ki(1)])?;
                    self.alu(OP_IADD, t, &[&p, &lt])
                }
            }
            "floor" => { let x = f1!(); self.alu(OP_ROUND_NI, tf(&x), &[&x]) }
            "ceil" => { let x = f1!(); self.alu(OP_ROUND_PI, tf(&x), &[&x]) }
            "trunc" => { let x = f1!(); self.alu(OP_ROUND_Z, tf(&x), &[&x]) }
            "round" | "roundEven" => { let x = f1!(); self.alu(OP_ROUND_NE, tf(&x), &[&x]) }
            "fract" => { let x = f1!(); self.alu(OP_FRC, tf(&x), &[&x]) }
            "mod" => { need(self, 2)?; let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?; let n = x.ty.n(); self.fmod(&x, &y, n) }
            "min" | "max" => {
                need(self, 2)?;
                let (x, y) = self.same_base(a[0].clone(), a[1].clone())?;
                let t = if x.ty.n() >= y.ty.n() { tf(&x) } else { tf(&y) };
                let op = match (n, t.b()) { ("min", B::F) => OP_MIN, ("max", B::F) => OP_MAX, ("min", B::I) => OP_IMIN, ("max", B::I) => OP_IMAX, ("min", _) => OP_UMIN, _ => OP_UMAX };
                self.alu(op, t, &[&x, &y])
            }
            "clamp" => {
                need(self, 3)?;
                let (x, lo) = self.same_base(a[0].clone(), a[1].clone())?;
                let (x, hi) = self.same_base(x, a[2].clone())?;
                let t = tf(&x);
                let (mx, mn) = match t.b() { B::F => (OP_MAX, OP_MIN), B::I => (OP_IMAX, OP_IMIN), _ => (OP_UMAX, OP_UMIN) };
                if t.b() == B::F && Self::is_const(&lo) && Self::is_const(&hi) && fk(&lo).iter().all(|v| *v == 0.0) && fk(&hi).iter().all(|v| *v == 1.0) { return self.alu_sat(OP_MOV, t, &[&x]); }
                let m = self.alu(mx, t.clone(), &[&x, &lo])?;
                self.alu(mn, t, &[&m, &hi])
            }
            "mix" => {
                need(self, 3)?;
                let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?;
                if a[2].ty.b() == B::Bo { return self.alu(OP_MOVC, tf(&x), &[&a[2], &y, &x]); }
                let w = self.fl(a[2].clone())?;
                let d = self.alu_md(OP_ADD, tf(&x), &[&y, &x], &[0, 1])?;
                self.alu(OP_MAD, tf(&x), &[&d, &w, &x])
            }
            "step" => {
                need(self, 2)?; let e = self.fl(a[0].clone())?; let x = self.fl(a[1].clone())?;
                let t = if x.ty.n() >= e.ty.n() { tf(&x) } else { tf(&e) };
                let ge = self.alu(OP_GE, Ty::S(B::Bo, t.n()), &[&x, &e])?;
                self.alu(OP_AND, t, &[&ge, &kvec(B::U, vec![FONE])])
            }
            "smoothstep" => {
                need(self, 3)?; let e0 = self.fl(a[0].clone())?; let e1 = self.fl(a[1].clone())?; let x = self.fl(a[2].clone())?;
                let t = tf(&x);
                let num = self.alu_md(OP_ADD, t.clone(), &[&x, &e0], &[0, 1])?;
                let den = self.alu_md(OP_ADD, if e1.ty.n() >= e0.ty.n() { tf(&e1) } else { tf(&e0) }, &[&e1, &e0], &[0, 1])?;
                let u = self.alu_sat(OP_DIV, t.clone(), &[&num, &den])?;
                let k = self.alu(OP_MAD, t.clone(), &[&u, &kf(-2.0), &kf(3.0)])?;
                let u2 = self.alu(OP_MUL, t.clone(), &[&u, &u])?;
                self.alu(OP_MUL, t, &[&u2, &k])
            }
            "length" => { let x = f1!(); let d = self.dot(&x, &x)?; self.alu(OP_SQRT, Ty::S(B::F, 1), &[&d]) }
            "distance" => { need(self, 2)?; let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?; let d = self.alu_md(OP_ADD, tf(&x), &[&x, &y], &[0, 1])?; let q = self.dot(&d, &d)?; self.alu(OP_SQRT, Ty::S(B::F, 1), &[&q]) }
            "dot" => { need(self, 2)?; let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?; if x.ty.n() != y.ty.n() { return self.err("dot needs vectors of one size"); } self.dot(&x, &y) }
            "cross" => {
                need(self, 2)?; let x = self.fl(a[0].clone())?; let y = self.fl(a[1].clone())?; let t = Ty::S(B::F, 3);
                let xa = self.field(x.clone(), "yzx")?; let yb = self.field(y.clone(), "zxy")?;
                let xc = self.field(x, "zxy")?; let yd = self.field(y, "yzx")?;
                let m = self.alu(OP_MUL, t.clone(), &[&xc, &yd])?;
                self.alu_md(OP_MAD, t, &[&xa, &yb, &m], &[0, 0, 1])
            }
            "normalize" => {
                let x = f1!();
                if x.ty.n() == 1 { return self.builtin("sign", vec![x]); }
                let d = self.dot(&x, &x)?; let r = self.alu(OP_RSQ, Ty::S(B::F, 1), &[&d])?; self.alu(OP_MUL, tf(&x), &[&x, &r])
            }
            "faceforward" => {
                need(self, 3)?; let nn = self.fl(a[0].clone())?; let i = self.fl(a[1].clone())?; let r = self.fl(a[2].clone())?;
                let d = self.dot(&r, &i)?; let lt = self.alu(OP_LT, Ty::S(B::Bo, 1), &[&d, &kf(0.0)])?;
                let neg = self.alu_md(OP_MOV, tf(&nn), &[&nn], &[1])?;
                self.alu(OP_MOVC, tf(&nn), &[&lt, &nn, &neg])
            }
            "reflect" => {
                need(self, 2)?; let i = self.fl(a[0].clone())?; let nn = self.fl(a[1].clone())?;
                let d = self.dot(&nn, &i)?; let d2 = self.alu(OP_ADD, Ty::S(B::F, 1), &[&d, &d])?;
                self.alu_md(OP_MAD, tf(&i), &[&nn, &d2, &i], &[1, 0, 0])
            }
            "refract" => {
                need(self, 3)?; let i = self.fl(a[0].clone())?; let nn = self.fl(a[1].clone())?; let eta = self.fl(a[2].clone())?;
                let s = Ty::S(B::F, 1);
                let d = self.dot(&nn, &i)?;
                let d2 = self.alu(OP_MUL, s.clone(), &[&d, &d])?;
                let o = self.alu_md(OP_ADD, s.clone(), &[&kf(1.0), &d2], &[0, 1])?;
                let e2 = self.alu(OP_MUL, s.clone(), &[&eta, &eta])?;
                let k = self.alu_md(OP_MAD, s.clone(), &[&e2, &o, &kf(1.0)], &[1, 0, 0])?;
                let sk = self.alu(OP_SQRT, s.clone(), &[&k])?;
                let m = self.alu(OP_MAD, s.clone(), &[&eta, &d, &sk])?;
                let ei = self.alu(OP_MUL, tf(&i), &[&i, &eta])?;
                let r = self.alu_md(OP_MAD, tf(&i), &[&nn, &m, &ei], &[1, 0, 0])?;
                let neg = self.alu(OP_LT, Ty::S(B::Bo, 1), &[&k, &kf(0.0)])?;
                self.alu(OP_MOVC, tf(&i), &[&neg, &kf(0.0), &r])
            }
            "matrixCompMult" => { need(self, 2)?; let t = tf(&a[0]); self.per_column(&a[0], Some(&a[1]), t, |g, x, y| g.alu(OP_MUL, x.ty.clone(), &[x, y.unwrap()])) }
            "transpose" => {
                need(self, 1)?;
                let (c, r) = match a[0].ty { Ty::M(c, r) => (c, r), _ => return self.err("transpose needs a matrix") };
                let mut comps = Vec::new();
                for j in 0..r as usize { for k in 0..c as i32 { let col = self.part(&a[0], k, Ty::S(B::F, r))?; comps.push(self.comp(&col, j)); } }
                self.ctor(&Ty::M(r, c), comps)
            }
            "determinant" => {
                need(self, 1)?;
                let m = a[0].clone();
                let e = |g: &mut Gen, c: i32, r: usize| -> R<Val> { let n = match m.ty { Ty::M(n, _) => n, _ => 2 }; let col = g.part(&m, c, Ty::S(B::F, n))?; Ok(g.comp(&col, r)) };
                match m.ty {
                    Ty::M(2, 2) => { let (a0, a1, b0, b1) = (e(self, 0, 0)?, e(self, 0, 1)?, e(self, 1, 0)?, e(self, 1, 1)?); let p = self.alu(OP_MUL, Ty::S(B::F, 1), &[&a0, &b1])?; self.alu_md(OP_MAD, Ty::S(B::F, 1), &[&b0, &a1, &p], &[1, 0, 0]) }
                    Ty::M(3, 3) => { let c0 = self.part(&m, 0, Ty::S(B::F, 3))?; let c1 = self.part(&m, 1, Ty::S(B::F, 3))?; let c2 = self.part(&m, 2, Ty::S(B::F, 3))?; let x = self.builtin("cross", vec![c1, c2])?; self.dot(&c0, &x) }
                    _ => self.err("determinant of this matrix is not supported"),
                }
            }
            "lessThan" | "lessThanEqual" | "greaterThan" | "greaterThanEqual" | "equal" | "notEqual" => {
                need(self, 2)?;
                let (x, y) = self.same_base(a[0].clone(), a[1].clone())?;
                let nn = x.ty.n(); let bt = Ty::S(B::Bo, nn);
                let (lt, ge, eq, ne) = match x.ty.b() { B::F => (OP_LT, OP_GE, OP_EQ, OP_NE), B::I => (OP_ILT, OP_IGE, OP_IEQ, OP_INE), _ => (OP_ULT, OP_UGE, OP_IEQ, OP_INE) };
                match n { "lessThan" => self.alu(lt, bt, &[&x, &y]), "lessThanEqual" => self.alu(ge, bt, &[&y, &x]), "greaterThan" => self.alu(lt, bt, &[&y, &x]),
                          "greaterThanEqual" => self.alu(ge, bt, &[&x, &y]), "equal" => self.alu(eq, bt, &[&x, &y]), _ => self.alu(ne, bt, &[&x, &y]) }
            }
            "any" | "all" => {
                need(self, 1)?; let x = a[0].clone(); let nn = x.ty.n();
                let mut r = self.comp(&x, 0);
                for k in 1..nn as usize { let c = self.comp(&x, k); r = self.alu(if n == "any" { OP_OR } else { OP_AND }, Ty::S(B::Bo, 1), &[&r, &c])?; }
                Ok(r)
            }
            "not" => { need(self, 1)?; let t = tf(&a[0]); self.alu(OP_NOT, t, &[&a[0]]) }
            "texture2D" | "texture" | "texture2DRect" | "textureCube" | "texture3D" | "texture1D" | "shadow2D" | "texture2DArray" => {
                if na < 2 || na > 3 { return self.err(&format!("{} takes 2 or 3 arguments", n)); }
                let uv = self.fl(a[1].clone())?;
                if n == "shadow2D" { return Ok(kvec(B::F, vec![FONE; 4])); }
                self.sample(&a[0], &uv, OP_SAMPLE_B, a.get(2))
            }
            "texture2DLod" | "textureLod" | "textureCubeLod" | "texture2DGrad" | "textureGrad" => {
                if na < 3 { return self.err(&format!("{} takes 3 arguments", n)); }
                let uv = self.fl(a[1].clone())?;
                if n.ends_with("Grad") { return self.sample(&a[0], &uv, OP_SAMPLE_L, Some(&kf(0.0))); }
                self.sample(&a[0], &uv, OP_SAMPLE_L, Some(&a[2]))
            }
            "texture2DProj" | "textureProj" => {
                if na < 2 { return self.err(&format!("{} takes 2 or 3 arguments", n)); }
                let uv = self.fl(a[1].clone())?; let p = self.proj(&uv)?;
                self.sample(&a[0], &p, OP_SAMPLE_B, a.get(2))
            }
            "texture2DProjLod" | "textureProjLod" => { need(self, 3)?; let uv = self.fl(a[1].clone())?; let p = self.proj(&uv)?; self.sample(&a[0], &p, OP_SAMPLE_L, Some(&a[2])) }
            "texelFetch" => {
                need(self, 3)?;
                let slot = match a[0].loc { Loc::Smp(k) => k, _ => return self.err("texelFetch needs a sampler") };
                let xy = self.conv(a[1].clone(), B::I)?;
                let out = self.temp(&Ty::S(B::F, 4))?;
                let d = self.dst(&out)?; let c = self.src(&xy)?;
                let r = self.push_op(Opnd { ty: T_RESOURCE, mode: 1, mask: 15, dim: 1, idx: [slot, 0, 0], rel: [-1; 3], swz: [0, 1, 2, 3], ..Default::default() })?;
                self.emit(OP_LD, &[d, c, r])?;
                Ok(Val { lv: false, ..out })
            }
            "textureSize" => {
                if na < 1 { return self.err("textureSize needs a sampler"); }
                let slot = match a[0].loc { Loc::Smp(k) => k, _ => return self.err("textureSize needs a sampler") };
                let out = self.temp(&Ty::S(B::I, 2))?;
                let d = self.dst(&out)?; let l = self.src(&ki(0))?;
                let r = self.push_op(Opnd { ty: T_RESOURCE, mode: 1, mask: 15, dim: 1, idx: [slot, 0, 0], rel: [-1; 3], swz: [0, 1, 2, 3], ..Default::default() })?;
                let i = self.emit(OP_RESINFO, &[d, l, r])?;
                self.ins[i].ret_type = 2;
                Ok(Val { lv: false, ..out })
            }
            "dFdx" | "dFdy" | "fwidth" | "dFdxFine" | "dFdyFine" | "dFdxCoarse" | "dFdyCoarse" => { let x = f1!(); Ok(Val { ty: tf(&x), ..kvec(B::F, vec![0; x.ty.n() as usize]) }) }
            "noise1" | "noise2" | "noise3" | "noise4" => { need(self, 1)?; let k = n.as_bytes()[5] - b'0'; Ok(Val { ty: Ty::S(B::F, k), ..kvec(B::F, vec![0; k as usize]) }) }
            "ftransform" => { let m = self.lookup("gl_ModelViewProjectionMatrix")?; let v = self.lookup("gl_Vertex")?; self.binary(Bin::Mul, m, v) }
            "floatBitsToInt" | "floatBitsToUint" | "intBitsToFloat" | "uintBitsToFloat" => {
                need(self, 1)?; let nn = a[0].ty.n();
                let b = match n { "floatBitsToInt" => B::I, "floatBitsToUint" => B::U, _ => B::F };
                Ok(Val { ty: Ty::S(b, nn), lv: false, ..a[0].clone() })
            }
            "isnan" => { let x = f1!(); let bt = Ty::S(B::Bo, x.ty.n()); self.alu(OP_NE, bt, &[&x, &x]) }
            "isinf" => { let x = f1!(); let bt = Ty::S(B::Bo, x.ty.n()); let ax = self.alu_md(OP_MOV, tf(&x), &[&x], &[2])?; self.alu(OP_EQ, bt, &[&ax, &kf(f32::INFINITY)]) }
            _ => self.err(&format!("'{}' is not a known function", n)),
        }
    }
    fn same_base(&mut self, x: Val, y: Val) -> R<(Val, Val)> {
        let (xb, yb) = (x.ty.b(), y.ty.b());
        if xb == yb { return Ok((x, y)); }
        let t = if xb == B::F || yb == B::F { B::F } else { B::I };
        Ok((self.conv(x, t)?, self.conv(y, t)?))
    }
}

fn x_or(v: &Val) -> Val { v.clone() }
fn kb_(v: bool) -> Val { Val { ty: Ty::S(B::Bo, 1), loc: Loc::K(vec![if v { !0 } else { 0 }]), swz: [0; 4], lv: false } }
/// sin for constant folding (a short Taylor series after range reduction; shaders get
/// the interpreter's library sine at run time)
fn libm_sin(x: f32) -> f32 {
    let tau = 2.0 * PI;
    let mut x = x - tau * ((x / tau) as i32 as f32);
    if x > PI { x -= tau; } else if x < -PI { x += tau; }
    let x2 = x * x;
    x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0)))))
}

pub const BUILTIN_FNS: [&str; 89] = [
    "radians", "degrees", "sin", "cos", "tan", "atan", "asin", "acos", "sinh", "cosh", "tanh", "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
    "abs", "sign", "floor", "ceil", "trunc", "round", "roundEven", "fract", "mod", "min", "max", "clamp", "mix", "step", "smoothstep",
    "length", "distance", "dot", "cross", "normalize", "faceforward", "reflect", "refract", "matrixCompMult", "transpose", "determinant",
    "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual", "any", "all", "not",
    "texture2D", "texture", "texture2DRect", "textureCube", "texture3D", "texture1D", "shadow2D", "texture2DArray", "texture2DLod", "textureLod", "textureCubeLod",
    "texture2DGrad", "textureGrad", "texture2DProj", "textureProj", "texture2DProjLod", "textureProjLod", "texelFetch", "textureSize",
    "dFdx", "dFdy", "fwidth", "dFdxFine", "dFdyFine", "dFdxCoarse", "dFdyCoarse", "noise1", "noise2", "noise3", "noise4", "ftransform",
    "floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat", "isnan",
];
