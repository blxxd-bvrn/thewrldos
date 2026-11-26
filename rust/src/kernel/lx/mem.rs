//! Memory for Linux processes (were src/linux/pmm.c, shm.c and uvm.c): physical pages,
//! shared objects, and user address spaces.
//!
//! Physical pages are 4 KiB cut from 2 MiB chunks of the kernel heap, which is
//! identity-mapped, so a page's address is both its physical and its kernel-visible
//! address. Freed pages go on a list and are reused; chunks are not given back.
//!
//! An address space is a 4-level page table whose slot 0 is the kernel's identity map, a
//! sorted list of mappings (VMAs), and demand paging: a page is allocated, zeroed or filled
//! from its file, on first touch.
//!
//! A shared object is a reference-counted array of physical pages. Mapping it MAP_SHARED
//! puts those same pages in the page tables (marked PTE_SHARED, so the address space never
//! frees them), and fork() keeps the mapping shared; each mapping, open descriptor and
//! /dev/shm name holds a reference, and the pages are freed with the last one.

use super::*;
use core::ptr::{self, null_mut};

/* ---------------- physical pages ---------------- */

const CHUNK: usize = 2 << 20;
struct FreePg { next: *mut FreePg }
static mut FREE_LIST: *mut FreePg = null_mut();
static mut USED: u32 = 0;
static mut TOTAL: u32 = 0;

unsafe fn add_pages(c: *mut u8, bytes: usize) {
    let mut o = 0;
    while o < bytes {
        let f = c.add(o) as *mut FreePg;
        (*f).next = FREE_LIST; FREE_LIST = f; TOTAL += 1;
        o += 4096;
    }
}
/// One zeroed 4 KiB page (identity-mapped), or null.
pub unsafe fn pmm_alloc() -> *mut u8 {
    if FREE_LIST.is_null() {
        let c = kmalloc_aligned(CHUNK, 4096) as *mut u8;
        if c.is_null() {
            let c = kmalloc_aligned(256 * 1024, 4096) as *mut u8;   /* the heap is fragmented or nearly full */
            if c.is_null() { return null_mut(); }
            add_pages(c, 256 * 1024);
        } else {
            add_pages(c, CHUNK);
        }
    }
    let p = FREE_LIST;
    FREE_LIST = (*p).next;
    USED += 1;
    ptr::write_bytes(p as *mut u8, 0, 4096);
    p as *mut u8
}
pub unsafe fn pmm_free(p: *mut u8) {
    if p.is_null() { return; }
    let f = p as *mut FreePg;
    (*f).next = FREE_LIST; FREE_LIST = f;
    USED = USED.wrapping_sub(1);
}
pub fn pmm_used_pages() -> u32 { unsafe { USED } }
extern "C" { fn mm_phys_total() -> u64; fn mm_used() -> u32; }
/// The computer's memory as Linux programs see it (sysinfo, /proc/meminfo): its real size, and
/// what the kernel heap and the programs' pages leave free. (It said 256 MB whatever the
/// machine had, so WebKit sized its caches for a small computer.)
pub fn sys_mem() -> (u64, u64) {
    let total = unsafe { mm_phys_total() }.max(64 << 20);
    let used = unsafe { mm_used() } as u64 + pmm_used_pages() as u64 * 4096;
    (total, total.saturating_sub(used))
}

/* ---------------- shared objects ---------------- */

static mut NAMED: *mut ShmObj = null_mut();          /* /dev/shm/<name> objects */

pub unsafe fn shm_new(name: Option<&[u8]>) -> *mut ShmObj {
    let o = znew::<ShmObj>();
    if o.is_null() { return null_mut(); }
    (*o).refs = 1;
    if let Some(n) = name { setstr(&mut (*o).name, n); }
    o
}
pub unsafe fn shm_ref(o: *mut ShmObj) { if !o.is_null() { (*o).refs += 1; } }
pub unsafe fn shm_unref(o: *mut ShmObj) {
    if o.is_null() { return; }
    (*o).refs -= 1;
    if (*o).refs > 0 { return; }
    for i in 0..(*o).npages as usize { let pg = *(*o).pages.add(i); if !pg.is_null() { pmm_free(pg); } }
    kfree((*o).pages as *mut c_void);
    zfree(o);
}
/// Resize to `size` bytes; pages past the end are kept until the object goes, because
/// another process may still have them mapped.
pub unsafe fn shm_resize(o: *mut ShmObj, size: u64) -> i64 {
    let ob = &mut *o;
    if size > (1u64 << 36) { return -EFBIG; }
    if ob.seals & SHM_SEAL_SHRINK != 0 && size < ob.size { return -EPERM; }
    if ob.seals & SHM_SEAL_GROW != 0 && size > ob.size { return -EPERM; }
    let need = palign(size) / PAGE;
    if need > ob.npages {
        let np = krealloc(ob.pages as *mut c_void, 8 * need as usize) as *mut *mut u8;
        if np.is_null() { return -ENOMEM; }
        ptr::write_bytes(np.add(ob.npages as usize), 0, (need - ob.npages) as usize);
        ob.pages = np; ob.npages = need;
    }
    if size < ob.size {                                  /* bytes past the new end read as zero later */
        let first = palign(size) / PAGE;
        if size & (PAGE - 1) != 0 {
            let pg = *ob.pages.add((size / PAGE) as usize);
            if !pg.is_null() { ptr::write_bytes(pg.add((size & (PAGE - 1)) as usize), 0, (PAGE - (size & (PAGE - 1))) as usize); }
        }
        let mut i = first;
        while i < ob.npages && i < palign(ob.size) / PAGE {
            let pg = *ob.pages.add(i as usize);
            if !pg.is_null() { ptr::write_bytes(pg, 0, PAGE as usize); }
            i += 1;
        }
    }
    ob.size = size;
    0
}
pub unsafe fn shm_page(o: *mut ShmObj, idx: u64, create: bool) -> *mut u8 {
    if idx >= (*o).npages { return null_mut(); }
    let slot = (*o).pages.add(idx as usize);
    if (*slot).is_null() && create { *slot = pmm_alloc(); }    /* zeroed */
    *slot
}
/// Read or write through the descriptor (read, write, pread, pwrite).
pub unsafe fn shm_rw(o: *mut ShmObj, buf: *mut u8, mut n: u64, off: u64, write: bool) -> i64 {
    if write {
        if (*o).seals & SHM_SEAL_WRITE != 0 { return -EPERM; }
        if off.wrapping_add(n) > (*o).size { let r = shm_resize(o, off.wrapping_add(n)); if r != 0 { return r; } }
    } else {
        if off >= (*o).size { return 0; }
        n = n.min((*o).size - off);
    }
    let mut done = 0u64;
    while done < n {
        let at = off + done;
        let k = (n - done).min(PAGE - (at & (PAGE - 1)));
        let pg = shm_page(o, at / PAGE, write);
        let po = (at & (PAGE - 1)) as usize;
        if write {
            if pg.is_null() { return if done != 0 { done as i64 } else { -ENOMEM }; }
            ptr::copy(buf.add(done as usize), pg.add(po), k as usize);
        } else if !pg.is_null() { ptr::copy(pg.add(po), buf.add(done as usize), k as usize); }
        else { ptr::write_bytes(buf.add(done as usize), 0, k as usize); }
        done += k;
    }
    done as i64
}
/// /dev/shm/<name>, with a reference.
pub unsafe fn shm_lookup(name: &[u8]) -> *mut ShmObj {
    let mut o = NAMED;
    while !o.is_null() { if cstr(&(*o).name) == cstr(name) { shm_ref(o); return o; } o = (*o).next; }
    null_mut()
}
pub unsafe fn shm_create_named(name: &[u8]) -> *mut ShmObj {
    let o = shm_new(Some(name));
    if o.is_null() { return null_mut(); }
    (*o).is_named = true;
    (*o).next = NAMED; NAMED = o;                        /* the name holds the first reference */
    shm_ref(o);                                          /* and the caller one more */
    o
}
pub unsafe fn shm_unlink_name(name: &[u8]) -> i64 {
    let mut pp: *mut *mut ShmObj = ptr::addr_of_mut!(NAMED);
    while !(*pp).is_null() {
        let o = *pp;
        if cstr(&(*o).name) != cstr(name) { pp = ptr::addr_of_mut!((*o).next); continue; }
        *pp = (*o).next; (*o).is_named = false;
        shm_unref(o);
        return 0;
    }
    -ENOENT
}

/* ---------------- address spaces ---------------- */

const PTE_P: u64 = 0x001;
const PTE_W: u64 = 0x002;
const PTE_U: u64 = 0x004;
const PTE_NX: u64 = 1 << 63;
const PTE_SHARED: u64 = 1 << 9;                      /* software bit: a shared object's page, not ours to free */
pub const ADDR_MASK: u64 = 0x000F_FFFF_FFFF_F000;
#[inline] fn addr(e: u64) -> u64 { e & ADDR_MASK }

pub unsafe fn uvm_new() -> *mut Uvm {
    let vm = znew::<Uvm>();
    if vm.is_null() { return null_mut(); }
    (*vm).pml4 = pmm_alloc() as *mut u64;
    if (*vm).pml4.is_null() { zfree(vm); return null_mut(); }
    *(*vm).pml4 = boot_pml4[0];                      /* the kernel's 0..512 GiB (identity map of the low 4 GiB) */
    vm
}
pub unsafe fn uvm_cr3(vm: *mut Uvm) -> usize { (*vm).pml4 as usize }

/// The PTE for va, creating the tables on the way if asked.
pub unsafe fn pte_of(vm: *mut Uvm, va: u64, create: bool) -> *mut u64 {
    let mut t = (*vm).pml4;
    for shift in [39u32, 30, 21] {
        let e = t.add(((va >> shift) & 511) as usize);
        if *e & PTE_P == 0 {
            if !create { return null_mut(); }
            let nt = pmm_alloc();
            if nt.is_null() { return null_mut(); }
            *e = nt as u64 | PTE_P | PTE_W | PTE_U;
        }
        t = addr(*e) as *mut u64;
    }
    t.add(((va >> 12) & 511) as usize)
}
/// The PTE of a resident page.
pub unsafe fn pte_lookup(vm: *mut Uvm, va: u64) -> *mut u64 {
    let e = pte_of(vm, va, false);
    if !e.is_null() && *e & PTE_P != 0 { e } else { null_mut() }
}

/// fork(): a private copy of every mapping, and of every page that is resident. Mappings of
/// a shared object keep pointing at its pages, so parent and child go on sharing them.
/// MAP_SHARED of an ordinary file maps the file's shared pages (locks.rs), so it is shared too.
pub unsafe fn uvm_clone(src: *mut Uvm) -> *mut Uvm {
    let dst = uvm_new();
    if dst.is_null() { return null_mut(); }
    let mut v = (*src).vmas;
    while !v.is_null() {
        let n = kmalloc(core::mem::size_of::<Vma>()) as *mut Vma;
        if n.is_null() { uvm_free(dst); return null_mut(); }
        ptr::copy_nonoverlapping(v, n, 1); (*n).next = null_mut();
        if !(*n).shm.is_null() { shm_ref((*n).shm); }
        let mut pp: *mut *mut Vma = ptr::addr_of_mut!((*dst).vmas);
        while !(*pp).is_null() { pp = ptr::addr_of_mut!((**pp).next); }
        *pp = n;
        v = (*v).next;
    }
    (*dst).brk_start = (*src).brk_start;
    (*dst).brk = (*src).brk;
    let mut v = (*dst).vmas;
    while !v.is_null() {
        let mut va = (*v).start;
        while va < (*v).end {
            let sp = pte_of(src, va, false);
            if sp.is_null() { va = (va | 0x1F_FFFF) + PAGE; continue; }   /* no page table: skip its 2 MiB */
            if *sp & PTE_P == 0 { va += PAGE; continue; }
            if *sp & PTE_SHARED != 0 {                                   /* MAP_SHARED: the same page */
                let dp = pte_of(dst, va, true);
                if dp.is_null() { uvm_free(dst); return null_mut(); }
                *dp = *sp;
                va += PAGE;
                continue;
            }
            let dp = pte_of(dst, va, true);
            if dp.is_null() { uvm_free(dst); return null_mut(); }
            let pg = pmm_alloc();
            if pg.is_null() { uvm_free(dst); return null_mut(); }
            ptr::copy_nonoverlapping(addr(*sp) as *const u8, pg, PAGE as usize);
            *dp = pg as u64 | (*sp & !ADDR_MASK);
            (*dst).pages += 1;
            /* the kernel is not preempted: a big process (a web browser holds hundreds of
             * megabytes) would otherwise stop the desktop for the whole copy */
            if (*dst).pages & 255 == 0 { task_yield(); }
            va += PAGE;
        }
        v = (*v).next;
    }
    dst
}
fn pte_flags(prot: c_int) -> u64 {
    let mut f = PTE_P | PTE_U;
    if prot & VM_WRITE != 0 { f |= PTE_W; }
    if prot & VM_EXEC == 0 { f |= PTE_NX; }
    f
}
unsafe fn free_range_pages(vm: *mut Uvm, start: u64, end: u64) {
    let mut va = start;
    while va < end {
        let pte = pte_of(vm, va, false);
        if pte.is_null() { va = (va | 0x1F_FFFF).wrapping_add(PAGE); continue; }   /* no page table: skip its 2 MiB */
        if *pte & PTE_P != 0 {
            if *pte & PTE_SHARED == 0 { pmm_free(addr(*pte) as *mut u8); (*vm).pages = (*vm).pages.wrapping_sub(1); }
            *pte = 0;
            invlpg(va);
        }
        va += PAGE;
    }
}
unsafe fn free_tables(t: *mut u64, level: i32) {
    for i in 0..512 {
        let e = *t.add(i);
        if e & PTE_P == 0 { continue; }
        if level > 1 { free_tables(addr(e) as *mut u64, level - 1); }
        else if e & PTE_SHARED == 0 { pmm_free(addr(e) as *mut u8); }
    }
    pmm_free(t as *mut u8);
}
pub unsafe fn uvm_free(vm: *mut Uvm) {
    if vm.is_null() { return; }
    for i in 1..256 { let e = *(*vm).pml4.add(i); if e & PTE_P != 0 { free_tables(addr(e) as *mut u64, 3); } }
    pmm_free((*vm).pml4 as *mut u8);
    while !(*vm).vmas.is_null() {
        let v = (*vm).vmas;
        let n = (*v).next;
        if !(*v).shm.is_null() { let o = (*v).shm; shm_unref(o); if !(*v).fnode.is_null() { super::locks::filemap_unmapped((*v).fnode, o); } }
        zfree(v);
        (*vm).vmas = n;
    }
    zfree(vm);
}
pub unsafe fn uvm_find(vm: *mut Uvm, a: u64) -> *mut Vma {
    let mut v = (*vm).vmas;
    while !v.is_null() && (*v).start <= a { if a < (*v).end { return v; } v = (*v).next; }
    null_mut()
}
unsafe fn insert_vma(vm: *mut Uvm, n: *mut Vma) {
    let mut pp: *mut *mut Vma = ptr::addr_of_mut!((*vm).vmas);
    while !(*pp).is_null() && (**pp).start < (*n).start { pp = ptr::addr_of_mut!((**pp).next); }
    (*n).next = *pp; *pp = n;
}
/// Split v at a (strictly inside it); v keeps the lower part.
unsafe fn split_vma(v: *mut Vma, a: u64) {
    let n = kmalloc(core::mem::size_of::<Vma>()) as *mut Vma;
    ptr::copy_nonoverlapping(v, n, 1);
    (*n).start = a;
    if !(*n).fdata.is_null() || !(*n).shm.is_null() { (*n).foff = (*n).foff.wrapping_add(a - (*v).start); }
    if !(*n).shm.is_null() { shm_ref((*n).shm); }
    (*v).end = a;
    (*v).next = n;
}
/// The highest free range of `len` bytes ending at or below `below` (0: none). One pass over
/// the regions, which are kept sorted and apart: the candidates are the ends of the gaps between
/// them. (It used to rescan every region for each candidate, which made each mmap in a large
/// program like WebKit cost millions of steps.)
pub unsafe fn uvm_find_free(vm: *mut Uvm, len: u64, below: u64) -> u64 {
    if below < USER_LO.wrapping_add(len) { return 0; }
    let mut best = 0u64;
    let mut gap_lo = USER_LO;                             /* start of the gap before v */
    let mut v = (*vm).vmas;
    loop {
        let gap_hi = if v.is_null() { below } else { (*v).start.min(below) };
        if gap_hi >= gap_lo.saturating_add(len) { best = gap_hi - len; }
        if v.is_null() || (*v).start >= below { break; }
        gap_lo = gap_lo.max((*v).end);
        v = (*v).next;
    }
    best
}
pub unsafe fn uvm_unmap(vm: *mut Uvm, start: u64, len: u64) -> i64 {
    let end = palign(start.wrapping_add(len));
    let start = start & !(PAGE - 1);
    if end <= start { return 0; }
    let mut v = (*vm).vmas;
    while !v.is_null() {
        if !((*v).end <= start || (*v).start >= end) {
            if (*v).start < start { split_vma(v, start); v = (*v).next; continue; }   /* the next iteration sees the upper part */
            if (*v).end > end { split_vma(v, end); }
        }
        v = (*v).next;
    }
    let mut pp: *mut *mut Vma = ptr::addr_of_mut!((*vm).vmas);
    while !(*pp).is_null() {
        let v = *pp;
        if (*v).start >= start && (*v).end <= end {
            *pp = (*v).next;
            free_range_pages(vm, (*v).start, (*v).end);
            if !(*v).shm.is_null() { let o = (*v).shm; shm_unref(o); if !(*v).fnode.is_null() { super::locks::filemap_unmapped((*v).fnode, o); } }
            zfree(v);
        } else { pp = ptr::addr_of_mut!((*v).next); }
    }
    0
}
/// Map a shared object; the mapping holds a reference to it.
pub unsafe fn uvm_map_shm(vm: *mut Uvm, hint: u64, len: u64, prot: c_int, shared: bool, fixed: bool, o: *mut ShmObj, off: u64) -> i64 {
    let r = uvm_map(vm, hint, len, prot, if shared { VMF_SHARED } else { VMF_PRIVATE }, fixed, ptr::null(), 0, off, null_mut());
    if r < 0 { return r; }
    let v = uvm_find(vm, r as u64);
    if !v.is_null() { (*v).shm = o; shm_ref(o); }
    r
}
/// Map [start, start+len): fixed placement replaces what was there.
pub unsafe fn uvm_map(vm: *mut Uvm, hint: u64, len: u64, prot: c_int, flags: c_int, fixed: bool,
                      fdata: *const u8, fsize: u64, foff: u64, fnode: *mut Vnode) -> i64 {
    let len = palign(len);
    if len == 0 { return -EINVAL; }
    let start;
    if fixed {
        if hint & (PAGE - 1) != 0 || hint < USER_LO || hint.wrapping_add(len) > USER_HI || hint.wrapping_add(len) < hint { return -EINVAL; }
        uvm_unmap(vm, hint, len);
        start = hint;
    } else {
        let mut s = 0;
        let hint = hint & !(PAGE - 1);
        if hint >= USER_LO && hint.wrapping_add(len) <= USER_HI {
            let mut free_ = true;
            let mut v = (*vm).vmas;
            while !v.is_null() { if (*v).start < hint.wrapping_add(len) && (*v).end > hint { free_ = false; break; } v = (*v).next; }
            if free_ { s = hint; }
        }
        if s == 0 { s = uvm_find_free(vm, len, MMAP_TOP); }
        if s == 0 { return -ENOMEM; }
        start = s;
    }
    let n = znew::<Vma>();
    if n.is_null() { return -ENOMEM; }
    (*n).start = start; (*n).end = start.wrapping_add(len); (*n).prot = prot; (*n).flags = flags;
    (*n).fdata = fdata; (*n).fsize = fsize; (*n).foff = foff; (*n).fnode = fnode;
    insert_vma(vm, n);
    start as i64
}
pub unsafe fn uvm_protect(vm: *mut Uvm, start: u64, len: u64, prot: c_int) -> i64 {
    let end = palign(start.wrapping_add(len));
    if start & (PAGE - 1) != 0 { return -EINVAL; }
    /* every page must be mapped */
    let mut a = start;
    while a < end { let v = uvm_find(vm, a); if v.is_null() { return -ENOMEM; } a = (*v).end; }
    let mut v = (*vm).vmas;
    while !v.is_null() {
        if !((*v).end <= start || (*v).start >= end) {
            if (*v).start < start { split_vma(v, start); v = (*v).next; continue; }
            if (*v).end > end { split_vma(v, end); }
            (*v).prot = prot;
            let mut va = (*v).start;
            while va < (*v).end {
                let pte = pte_of(vm, va, false);
                if pte.is_null() { va = (va | 0x1F_FFFF).wrapping_add(PAGE); continue; }
                if *pte & PTE_P != 0 { *pte = addr(*pte) | pte_flags(prot) | (*pte & PTE_SHARED); invlpg(va); }
                va += PAGE;
            }
        }
        v = (*v).next;
    }
    0
}
/// Demand paging; false = segmentation fault.
pub unsafe fn uvm_fault(vm: *mut Uvm, a: u64, write: bool) -> bool {
    if !(USER_LO..USER_HI).contains(&a) { return false; }
    let v = uvm_find(vm, a);
    if v.is_null() || (*v).prot == 0 { return false; }
    if write && (*v).prot & VM_WRITE == 0 { return false; }
    let va = a & !(PAGE - 1);
    let pte = pte_of(vm, va, true);
    if pte.is_null() { return false; }
    if *pte & PTE_P != 0 { return !write || *pte & PTE_W != 0; }   /* another thread got there first */
    if !(*v).shm.is_null() {
        let idx = ((*v).foff.wrapping_add(va - (*v).start)) / PAGE;
        if idx.wrapping_mul(PAGE) >= palign((*(*v).shm).size) { return false; }   /* past the object's end (SIGBUS on Linux) */
        let sp = shm_page((*v).shm, idx, true);
        if sp.is_null() { lxlog!("linux: out of memory for a shared page"); return false; }
        if (*v).flags & VMF_SHARED != 0 { *pte = sp as u64 | pte_flags((*v).prot) | PTE_SHARED; return true; }
        let pg = pmm_alloc();                                      /* MAP_PRIVATE of a shared object: a copy */
        if pg.is_null() { return false; }
        ptr::copy_nonoverlapping(sp, pg, PAGE as usize);
        *pte = pg as u64 | pte_flags((*v).prot);
        (*vm).pages += 1;
        return true;
    }
    let pg = pmm_alloc();
    if pg.is_null() { lxlog!("linux: out of memory for a page at {:x}", va); return false; }
    if !(*v).fdata.is_null() {
        let off = (*v).foff.wrapping_add(va - (*v).start);
        if off < (*v).fsize { ptr::copy_nonoverlapping((*v).fdata.add(off as usize), pg, PAGE.min((*v).fsize - off) as usize); }
    }
    *pte = pg as u64 | pte_flags((*v).prot);
    (*vm).pages += 1;
    true
}
/// MADV_DONTNEED: the resident pages of [start, end) in mappings without file data go, so
/// they read as zeros on the next touch. The C code looked every page up on its own; a
/// browser drops 64 GiB reservations this way, so only the mappings that overlap the range
/// are visited, and 2 MiB is skipped where there is no page table. The same pages go.
pub unsafe fn uvm_dontneed(vm: *mut Uvm, start: u64, end: u64) {
    let mut v = (*vm).vmas;
    while !v.is_null() {
        if (*v).start < end && (*v).end > start && (*v).fdata.is_null() {
            let mut va = (*v).start.max(start);
            let stop = (*v).end.min(end);
            while va < stop {
                let pte = pte_of(vm, va, false);
                if pte.is_null() { va = (va | 0x1F_FFFF).wrapping_add(1); continue; }   /* no page table: its 2 MiB */
                if *pte & PTE_P != 0 { pmm_free(addr(*pte) as *mut u8); *pte = 0; (*vm).pages = (*vm).pages.wrapping_sub(1); invlpg(va); }
                va += PAGE;
            }
        }
        v = (*v).next;
    }
}
/// Write 4 bytes into another process's memory (it need not be the current one).
pub unsafe fn uvm_poke32(vm: *mut Uvm, va: u64, val: u32) -> bool {
    if va & (PAGE - 1) > PAGE - 4 { return false; }
    let mut e = pte_of(vm, va & !(PAGE - 1), false);
    if e.is_null() || *e & PTE_P == 0 {
        if !uvm_fault(vm, va, true) { return false; }
        e = pte_of(vm, va & !(PAGE - 1), false);
    }
    if e.is_null() || *e & PTE_P == 0 { return false; }
    ptr::write_volatile((addr(*e) + (va & (PAGE - 1))) as *mut u32, val);
    true
}
/// Covered by mappings that allow it.
pub unsafe fn uvm_check(vm: *mut Uvm, mut a: u64, len: u64, write: bool) -> bool {
    if len == 0 { return true; }
    if a < USER_LO || a.wrapping_add(len) > USER_HI || a.wrapping_add(len) < a { return false; }
    let end = a + len;
    while a < end {
        let v = uvm_find(vm, a);
        if v.is_null() || (*v).prot & VM_READ == 0 || (write && (*v).prot & VM_WRITE == 0) { return false; }
        a = (*v).end;
    }
    true
}

/* ---------------- copying to and from the current process ---------------- */

/// The current process's address space.
pub unsafe fn cur_vm() -> *mut Uvm { (*(*lx_current()).proc_).vm }
/// Copy in from user memory (the current address space is the process's).
pub unsafe fn uin<T>(vm: *mut Uvm, dst: *mut T, src: u64, n: u64) -> bool {
    if !uvm_check(vm, src, n, false) { return false; }
    ptr::copy(src as *const u8, dst as *mut u8, n as usize);
    true
}
pub unsafe fn uout<T>(vm: *mut Uvm, dst: u64, src: *const T, n: u64) -> bool {
    if !uvm_check(vm, dst, n, true) { return false; }
    ptr::copy(src as *const u8, dst as *mut u8, n as usize);
    true
}
/// A value from user memory.
pub unsafe fn uget<T: Copy>(vm: *mut Uvm, src: u64) -> Option<T> {
    let mut v = core::mem::MaybeUninit::<T>::zeroed();
    if uin(vm, v.as_mut_ptr(), src, core::mem::size_of::<T>() as u64) { Some(v.assume_init()) } else { None }
}
pub unsafe fn uput<T: Copy>(vm: *mut Uvm, dst: u64, v: &T) -> bool { uout(vm, dst, v, core::mem::size_of::<T>() as u64) }
