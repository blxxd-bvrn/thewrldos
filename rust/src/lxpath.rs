//! The parts of the Linux layer (rust/src/kernel/lx) that touch nothing but bytes: Linux
//! paths and how they map onto the WRLD file system, the built-in /dev, /proc and /sys
//! names, "#!" script lines and the checks on an ELF program's headers.
//!
//! Paths are byte strings, as Linux's are. The C code (src/linux/fs.c, proc.c) built them
//! in 512-byte buffers; the same limits apply here, so a path that was cut short is cut at
//! the same place.

use alloc::vec::Vec;

/// What a Linux path names (the LP_ values of the C code, in the same order).
pub mod kind {
    pub const FILE: i32 = 0;
    pub const DEV_NULL: i32 = 1;
    pub const DEV_ZERO: i32 = 2;
    pub const DEV_RANDOM: i32 = 3;
    pub const DEV_TTY: i32 = 4;
    pub const PROC_SELF_EXE: i32 = 5;
    pub const PROC_SELF_FD: i32 = 6;
    pub const PROC_DIR: i32 = 7;
    pub const PROC_SELF_DIR: i32 = 8;
    pub const PROC_FD_DIR: i32 = 9;
    pub const PROC_MAPS: i32 = 10;
    pub const PROC_STAT: i32 = 11;
    pub const PROC_STATUS: i32 = 12;
    pub const PROC_CMDLINE: i32 = 13;
    pub const PROC_MEMINFO: i32 = 14;
    pub const PROC_CPUINFO: i32 = 15;
    pub const PROC_UPTIME: i32 = 16;
    pub const PROC_VERSION: i32 = 17;
    pub const SYS: i32 = 18;
    pub const SYS_DEVICES: i32 = 19;
    pub const SYS_DEV_SYSTEM: i32 = 20;
    pub const SYS_CPU: i32 = 21;
    pub const SYS_KERNEL: i32 = 22;
    pub const SYS_CPU_ONLINE: i32 = 23;
    pub const SYS_CPU_POSSIBLE: i32 = 24;
    pub const SYS_CPU_PRESENT: i32 = 25;
    pub const SYS_OSRELEASE: i32 = 26;
    pub const SYS_OSTYPE: i32 = 27;
    pub const SYS_VERSION: i32 = 28;
    pub const DEV_SHM: i32 = 29;
    pub const DEV_SHM_DIR: i32 = 30;
    pub const DEV_WRLDWIN: i32 = 31;
    pub const DEV_DSP: i32 = 32;
}

/// The C code's path buffers: 512 bytes with the terminating NUL.
pub const PATH_MAX: usize = 511;

/// The bytes before the first NUL.
pub fn cstr(b: &[u8]) -> &[u8] { &b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())] }

/// snprintf/strlcpy into a buffer of `cap` bytes: at most cap - 1 bytes survive.
pub fn cut(mut v: Vec<u8>, cap: usize) -> Vec<u8> { v.truncate(cap.saturating_sub(1)); v }

fn cat(parts: &[&[u8]], cap: usize) -> Vec<u8> {
    let mut v = Vec::new();
    for p in parts { v.extend_from_slice(p); }
    cut(v, cap)
}

/// Collapse "//", "." and ".." in an absolute path (fs.c normalize).
pub fn normalize(p: &[u8]) -> Vec<u8> {
    let p = cstr(p);
    let mut out: Vec<u8> = Vec::with_capacity(p.len() + 1);
    let mut s = 0;
    while s < p.len() {
        while s < p.len() && p[s] == b'/' { s += 1; }
        if s >= p.len() { break; }
        let mut e = s;
        while e < p.len() && p[e] != b'/' { e += 1; }
        let seg = &p[s..e];
        if seg == b"." { s = e; continue; }
        if seg == b".." {
            while !out.is_empty() && *out.last().unwrap() != b'/' { out.pop(); }
            out.pop();
            s = e;
            continue;
        }
        if out.len() + seg.len() + 2 >= 512 { break; }
        out.push(b'/');
        out.extend_from_slice(seg);
        s = e;
    }
    if out.is_empty() { out.push(b'/'); }
    out
}

/// A path as the program gave it, made absolute against `base` (its working directory,
/// or the directory a descriptor names) and normalised.
pub fn absolute(base: &[u8], upath: &[u8]) -> Vec<u8> {
    let upath = cstr(upath);
    let tmp = if upath.first() == Some(&b'/') { cut(upath.to_vec(), 512) } else { cat(&[cstr(base), b"/", upath], 512) };
    normalize(&tmp)
}

/// The built-in names (fs.c classify).
pub fn classify(a: &[u8]) -> i32 {
    use kind::*;
    let a = cstr(a);
    match a {
        b"/dev/null" => return DEV_NULL,
        b"/dev/shm" => return DEV_SHM_DIR,
        b"/dev/wrld/window" => return DEV_WRLDWIN,
        b"/dev/dsp" | b"/dev/dsp0" => return DEV_DSP,
        _ => {}
    }
    if a.len() > 9 && a.starts_with(b"/dev/shm/") && !a[9..].contains(&b'/') { return DEV_SHM; }
    match a {
        b"/dev/zero" => DEV_ZERO,
        b"/dev/urandom" | b"/dev/random" => DEV_RANDOM,
        b"/dev/tty" | b"/dev/console" | b"/dev/stdout" | b"/dev/stderr" | b"/dev/stdin" => DEV_TTY,
        b"/proc" => PROC_DIR,
        b"/proc/self" => PROC_SELF_DIR,
        b"/proc/self/exe" => PROC_SELF_EXE,
        b"/proc/self/maps" => PROC_MAPS,
        b"/proc/self/stat" => PROC_STAT,
        b"/proc/self/status" => PROC_STATUS,
        b"/proc/self/cmdline" => PROC_CMDLINE,
        b"/proc/self/fd" => PROC_FD_DIR,
        _ if a.starts_with(b"/proc/self/fd/") => PROC_SELF_FD,
        b"/proc/meminfo" => PROC_MEMINFO,
        b"/proc/cpuinfo" => PROC_CPUINFO,
        b"/proc/uptime" => PROC_UPTIME,
        b"/proc/version" => PROC_VERSION,
        b"/sys" => SYS,
        b"/sys/devices" => SYS_DEVICES,
        b"/sys/devices/system" => SYS_DEV_SYSTEM,
        b"/sys/devices/system/cpu" => SYS_CPU,
        b"/sys/kernel" => SYS_KERNEL,
        b"/sys/devices/system/cpu/online" => SYS_CPU_ONLINE,
        b"/sys/devices/system/cpu/possible" => SYS_CPU_POSSIBLE,
        b"/sys/devices/system/cpu/present" => SYS_CPU_PRESENT,
        b"/sys/kernel/osrelease" => SYS_OSRELEASE,
        b"/sys/kernel/ostype" => SYS_OSTYPE,
        b"/sys/kernel/version" => SYS_VERSION,
        _ => FILE,
    }
}

/// An absolute Linux path as a WRLD path: "/" is C:\Linux, /mnt/<letter> is that drive,
/// and /lib, /lib64, /bin and /sbin are the merged-/usr links (fs.c to_wrld).
pub fn to_wrld(a: &[u8], cap: usize) -> Vec<u8> {
    let a = cstr(a);
    let mut tmp = cut(a.to_vec(), 512);
    for m in [&b"/lib"[..], b"/lib64", b"/bin", b"/sbin"] {
        if a.starts_with(m) && (a.len() == m.len() || a[m.len()] == b'/') { tmp = cat(&[b"/usr", a], 512); break; }
    }
    let mut out = if tmp.len() >= 6 && tmp.starts_with(b"/mnt/") && tmp[5].is_ascii_alphabetic() && (tmp.len() == 6 || tmp[6] == b'/') {
        let rest: &[u8] = if tmp.len() > 6 { &tmp[7..] } else { b"" };
        cat(&[&[tmp[5].to_ascii_uppercase(), b':', b'\\'], rest], cap)
    } else {
        let rest: &[u8] = if tmp.as_slice() != b"/" { &tmp } else { b"" };
        cat(&[b"C:\\Linux", rest], cap)
    };
    for c in out.iter_mut() { if *c == b'/' { *c = b'\\'; } }
    out
}

/// A WRLD path as the Linux programs see it (proc.c wrld_to_linux).
pub fn wrld_to_linux(wp: &[u8], cap: usize) -> Vec<u8> {
    let wp = cstr(wp);
    let mut out = if wp.len() >= 8 && wp[..8].eq_ignore_ascii_case(b"C:\\Linux") && (wp.len() == 8 || wp[8] == b'\\') {
        cut(if wp.len() > 8 { wp[8..].to_vec() } else { b"/".to_vec() }, cap)
    } else if wp.len() >= 2 && wp[1] == b':' {
        cat(&[b"/mnt/", &[wp[0].to_ascii_lowercase()], &wp[2..]], cap)
    } else {
        cut(wp.to_vec(), cap)
    };
    for c in out.iter_mut() { if *c == b'\\' { *c = b'/'; } }
    out
}

/// "#!interpreter [arg]" on a script's first line (proc.c parse_shebang): the interpreter
/// and the rest of the line as one argument, each as a 256-byte buffer would hold it.
pub fn shebang(d: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    if d.len() < 3 || d[0] != b'#' || d[1] != b'!' { return None; }
    let mut line: Vec<u8> = Vec::new();
    let mut i = 2;
    while i < d.len() && line.len() < 255 && d[i] != b'\n' { line.push(d[i]); i += 1; }
    let line = cstr(&line).to_vec();                 /* the C code worked on a C string */
    let mut s = 0;
    while s < line.len() && (line[s] == b' ' || line[s] == b'\t') { s += 1; }
    let mut e = s;
    while e < line.len() && line[e] != b' ' && line[e] != b'\t' && line[e] != b'\r' { e += 1; }
    let interp = &line[s..e];
    let mut a = e;
    if a < line.len() {
        a += 1;
        while a < line.len() && (line[a] == b' ' || line[a] == b'\t') { a += 1; }
    }
    let mut arg: &[u8] = if a <= line.len() { &line[a..] } else { b"" };
    while let Some(&c) = arg.last() { if c == b' ' || c == b'\t' || c == b'\r' { arg = &arg[..arg.len() - 1]; } else { break; } }
    if interp.is_empty() { return None; }
    Some((cut(interp.to_vec(), 256), cut(arg.to_vec(), 256)))
}

/* ---------------- ELF ---------------- */

pub const PT_LOAD: u32 = 1;
pub const PT_INTERP: u32 = 3;
pub const PT_PHDR: u32 = 6;
pub const ET_EXEC: u16 = 2;
pub const ET_DYN: u16 = 3;
pub const PHDR_SIZE: u64 = 56;

/// The fields of an ELF64 header the loader uses.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Ehdr { pub ty: u16, pub entry: u64, pub phoff: u64, pub phnum: u16 }
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Phdr { pub ty: u32, pub flags: u32, pub offset: u64, pub vaddr: u64, pub filesz: u64, pub memsz: u64 }

/// Why a file cannot be loaded, in the order the C loader checked.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ElfError { NotElf, NotX86_64, NotExecutable, BadHeaders }

fn u16le(d: &[u8], o: usize) -> u16 { u16::from_le_bytes([d[o], d[o + 1]]) }
fn u32le(d: &[u8], o: usize) -> u32 { u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) }
fn u64le(d: &[u8], o: usize) -> u64 { let mut b = [0u8; 8]; b.copy_from_slice(&d[o..o + 8]); u64::from_le_bytes(b) }

pub fn is_elf(d: &[u8]) -> bool { d.len() >= 64 && &d[..4] == b"\x7FELF" }

/// The header and program headers of a 64-bit x86-64 executable or shared object.
pub fn elf_headers(d: &[u8]) -> Result<(Ehdr, Vec<Phdr>), ElfError> {
    if !is_elf(d) { return Err(ElfError::NotElf); }
    if d[4] != 2 || d[5] != 1 || u16le(d, 18) != 62 { return Err(ElfError::NotX86_64); }
    let eh = Ehdr { ty: u16le(d, 16), entry: u64le(d, 24), phoff: u64le(d, 32), phnum: u16le(d, 56) };
    if eh.ty != ET_EXEC && eh.ty != ET_DYN { return Err(ElfError::NotExecutable); }
    let end = eh.phoff.checked_add(eh.phnum as u64 * PHDR_SIZE);
    if end.map_or(true, |e| e > d.len() as u64) || u16le(d, 54) as u64 != PHDR_SIZE { return Err(ElfError::BadHeaders); }
    let mut ph = Vec::with_capacity(eh.phnum as usize);
    for i in 0..eh.phnum as usize {
        let o = eh.phoff as usize + i * PHDR_SIZE as usize;
        ph.push(Phdr { ty: u32le(d, o), flags: u32le(d, o + 4), offset: u64le(d, o + 8), vaddr: u64le(d, o + 16),
                       filesz: u64le(d, o + 32), memsz: u64le(d, o + 40) });
    }
    Ok((eh, ph))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn s(v: Vec<u8>) -> std::string::String { std::string::String::from_utf8(v).unwrap() }

    #[test]
    fn paths() {
        assert_eq!(s(normalize(b"/a//b/./c/../d/")), "/a/b/d");
        assert_eq!(s(normalize(b"/../..")), "/");
        assert_eq!(s(absolute(b"/home/user", b"../x")), "/home/x");
        assert_eq!(s(to_wrld(b"/", 512)), "C:\\Linux");
        assert_eq!(s(to_wrld(b"/bin/sh", 512)), "C:\\Linux\\usr\\bin\\sh");
        assert_eq!(s(to_wrld(b"/mnt/d/x/y", 512)), "D:\\x\\y");
        assert_eq!(s(to_wrld(b"/mnt/d", 512)), "D:\\");
        assert_eq!(s(wrld_to_linux(b"C:\\Linux\\usr\\bin\\sh", 512)), "/usr/bin/sh");
        assert_eq!(s(wrld_to_linux(b"D:\\x", 512)), "/mnt/d/x");
        assert_eq!(classify(b"/dev/shm/x"), kind::DEV_SHM);
        assert_eq!(classify(b"/dev/shm/x/y"), kind::FILE);
        assert_eq!(classify(b"/proc/self/fd/3"), kind::PROC_SELF_FD);
        let (i, a) = shebang(b"#! /bin/sh -e \r\nrest").unwrap();
        assert_eq!((s(i), s(a)), ("/bin/sh".into(), "-e".into()));
        assert!(shebang(b"#!   \n").is_none());
        assert_eq!(elf_headers(b"nope"), Err(ElfError::NotElf));
    }
}
