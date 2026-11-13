/* PE loader and Win32 process model. The 32-bit edition loads PE32 (x86)
 * programs into a per-process 4..16 MiB window; the 64-bit edition loads PE32+
 * (x64) programs into a private 4-level address space at their preferred base.
 * Relocations, import binding, TLS, TEB/PEB. */
#include "w32.h"

typedef struct PACKED { uint16_t e_magic; uint8_t pad[58]; uint32_t e_lfanew; } dos_hdr_t;
typedef struct PACKED { uint16_t Machine, NumberOfSections; uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; uint16_t SizeOfOptionalHeader, Characteristics; } file_hdr_t;
typedef struct PACKED { uint32_t VirtualAddress, Size; } data_dir_t;
typedef struct PACKED {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker; uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode, BaseOfData, ImageBase, SectionAlignment, FileAlignment;
    uint16_t MajorOS, MinorOS, MajorImage, MinorImage, MajorSubsystem, MinorSubsystem; uint32_t Win32Version, SizeOfImage, SizeOfHeaders, CheckSum; uint16_t Subsystem, DllCharacteristics;
    uint32_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit, LoaderFlags, NumberOfRvaAndSizes; data_dir_t dir[16];
} opt_hdr_t;
typedef struct PACKED { char Name[8]; uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers; uint16_t NumberOfRelocations, NumberOfLinenumbers; uint32_t Characteristics; } sect_hdr_t;
typedef struct PACKED { uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } import_desc_t;
typedef struct PACKED {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker; uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode; uint64_t ImageBase; uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOS, MinorOS, MajorImage, MinorImage, MajorSubsystem, MinorSubsystem; uint32_t Win32Version, SizeOfImage, SizeOfHeaders, CheckSum; uint16_t Subsystem, DllCharacteristics;
    uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit; uint32_t LoaderFlags, NumberOfRvaAndSizes; data_dir_t dir[16];
} opt_hdr64_t;
typedef struct PACKED { uint32_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks, SizeOfZeroFill, Characteristics; } tls_dir32_t;
typedef struct PACKED { uint64_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks; uint32_t SizeOfZeroFill, Characteristics; } tls_dir64_t;
/* the optional header fields we use, whichever format the file has */
typedef struct { uint64_t image_base; uint32_t entry, size_image, size_headers, nrva; uint16_t subsystem; uint64_t stack_reserve; data_dir_t dir[16]; } pe_info_t;

#define WIN_LO 0x00400000u
#define WIN_HI 0x01000000u
static w32_proc_t *procs[64];
#ifdef __x86_64__
#define PE_MAGIC 0x20B
#define PE_FMT "PE32+ (x64)"
#else
#define PE_MAGIC 0x10B
#define PE_FMT "PE32 (x86)"
#endif

w32_proc_t *w32_cur(void) { task_t *t = task_current(); return t ? (w32_proc_t *)t->proc : NULL; }

/* ---------------- process heap ---------------- */
#define AMAGIC 0x57334841
void *w32_alloc(w32_proc_t *p, uint32_t n) {
    alloc_hdr_t *h = kmalloc(n + sizeof(alloc_hdr_t));
    if (!h) return NULL;
    h->size = n; h->magic = AMAGIC;
    if (!p) { h->next = h->prev = NULL; return h + 1; }
    h->next = p->heap.next; h->prev = &p->heap;
    if (p->heap.next) p->heap.next->prev = h;
    p->heap.next = h;
    p->heap_bytes += n;
    return h + 1;
}
void w32_free(w32_proc_t *p, void *ptr) {
    if (!ptr) return;
    alloc_hdr_t *h = (alloc_hdr_t *)ptr - 1;
    if (h->magic != AMAGIC) return;
    h->magic = 0;
    if (h->prev) h->prev->next = h->next;
    if (h->next) h->next->prev = h->prev;
    if (p && p->heap_bytes >= h->size) p->heap_bytes -= h->size;
    kfree(h);
}
uint32_t w32_msize(void *ptr) { if (!ptr) return 0; alloc_hdr_t *h = (alloc_hdr_t *)ptr - 1; return h->magic == AMAGIC ? h->size : 0; }
void *w32_realloc(w32_proc_t *p, void *ptr, uint32_t n) {
    if (!ptr) return w32_alloc(p, n);
    if (!n) { w32_free(p, ptr); return NULL; }
    uint32_t old = w32_msize(ptr);
    if (old >= n) return ptr;
    void *q = w32_alloc(p, n);
    if (q) { memcpy(q, ptr, old); w32_free(p, ptr); }
    return q;
}

/* ---------------- import resolution ---------------- */
typedef struct { const char *dll; const w32_export_t *tab; const int *count; } dll_t;
static const dll_t dlls[] = {
    { "kernel32.dll", kernel32_exports, &kernel32_count }, { "msvcrt.dll", msvcrt_exports, &msvcrt_count },
    { "user32.dll", user32_exports, &user32_count }, { "gdi32.dll", gdi32_exports, &gdi32_count },
    { "advapi32.dll", advapi32_exports, &advapi32_count }, { "shell32.dll", shell32_exports, &shell32_count },
    { "ucrtbase.dll", msvcrt_exports, &msvcrt_count }, { "msvcr100.dll", msvcrt_exports, &msvcrt_count },
    { "msvcr120.dll", msvcrt_exports, &msvcrt_count }, { "crtdll.dll", msvcrt_exports, &msvcrt_count },
    { "kernelbase.dll", kernel32_exports, &kernel32_count }, { "ntdll.dll", kernel32_exports, &kernel32_count },
    { "opengl32.dll", opengl32_exports, &opengl32_count }, { "glu32.dll", glu32_exports, &glu32_count },
    { "d3d9.dll", d3d9_exports, &d3d9_count }, { "d3dx9_43.dll", d3dx9_exports, &d3dx9_count },
    { "vulkan-1.dll", vulkan_exports, &vulkan_count },
};
static const dll_t *find_dll(const char *name) {
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) if (!strcasecmp(dlls[i].dll, name)) return &dlls[i];
    if (!strncasecmp(name, "api-ms-win-crt-", 15)) return &dlls[1];
    if (!strncasecmp(name, "d3dx9_", 6)) return find_dll("d3dx9_43.dll");
    if (!strncasecmp(name, "api-ms-win-core-", 16)) return &dlls[0];
    return NULL;
}
bool w32_lookup(const char *dll, const char *name, void **out) {
    const dll_t *d = dll ? find_dll(dll) : NULL;
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) {
        const dll_t *x = d ? d : &dlls[i];
        for (int k = 0; k < *x->count; k++) if (!strcmp(x->tab[k].name, name)) { *out = x->tab[k].fn; return true; }
        if (d) break;
    }
    return false;
}
const char *w32_api_list(int dll, int idx) {
    if (dll < 0 || dll >= 6) return NULL;
    if (idx < 0 || idx >= *dlls[dll].count) return NULL;
    return dlls[dll].tab[idx].name;
}
typedef struct { char dll[32]; char name[64]; } unimpl_t;
static void __attribute__((used)) w32_unimplemented(unimpl_t *u) {
    w32_proc_t *p = w32_cur();
    char msg[200];
    snprintf(msg, sizeof msg, "The program called %s!%s, which is not implemented by the WRLD Win32 subsystem yet.", u->dll, u->name);
    klog("w32: pid %d: unimplemented %s!%s", p ? p->pid : -1, u->dll, u->name);
    if (p && p->con) { con_printf(p->con, "\n\x1b[91m%s\x1b[0m\n", msg); }
    else msgbox(p ? path_basename(p->path) : "Win32", msg, MBI_ERROR, MBX_OK, NULL, NULL);
    w32_exit_process(0xC0000139);
}
static void *make_thunk(const char *dll, const char *name) {
    unimpl_t *u = kzalloc(sizeof *u);
    strlcpy(u->dll, dll, sizeof u->dll); strlcpy(u->name, name, sizeof u->name);
#ifdef __x86_64__
    uint8_t *t = kmalloc(32);
    t[0] = 0x48; t[1] = 0xBF; *(uint64_t *)(t + 2) = (uintptr_t)u;                  /* mov rdi, u */
    t[10] = 0x48; t[11] = 0xB8; *(uint64_t *)(t + 12) = (uintptr_t)w32_unimplemented; /* mov rax, fn */
    t[20] = 0x48; t[21] = 0x83; t[22] = 0xE4; t[23] = 0xF0;                        /* and rsp, -16 */
    t[24] = 0xFF; t[25] = 0xD0;                                                    /* call rax */
    t[26] = 0xCC;
#else
    uint8_t *t = kmalloc(16);
    t[0] = 0xB8; *(uint32_t *)(t + 1) = (uint32_t)u;          /* mov eax, u */
    t[5] = 0x50;                                               /* push eax */
    t[6] = 0xB9; *(uint32_t *)(t + 7) = (uint32_t)w32_unimplemented; /* mov ecx, fn */
    t[11] = 0xFF; t[12] = 0xD1;                                /* call ecx */
    t[13] = 0xCC;
#endif
    return t;
}

/* ---------------- probing ---------------- */
static bool parse_headers(const uint8_t *d, uint32_t n, file_hdr_t **fh, pe_info_t *pi, sect_hdr_t **sh, char *err, int ecap) {
    if (n < 64 || d[0] != 'M' || d[1] != 'Z') { snprintf(err, ecap, "Not a valid Win32 application (no MZ header)."); return false; }
    uint32_t lf = ((const dos_hdr_t *)d)->e_lfanew;
    if (lf + 24 > n || memcmp(d + lf, "PE\0\0", 4)) { snprintf(err, ecap, "This is an MS-DOS program; 16-bit DOS executables are not supported."); return false; }
    *fh = (file_hdr_t *)(d + lf + 4);
    uint16_t m = (*fh)->Machine;
    if (m == 0xAA64) { snprintf(err, ecap, "This is an ARM64 program; only x86 and x64 programs are supported."); return false; }
#ifdef __x86_64__
    if (m == 0x14C) { snprintf(err, ecap, "This is a 32-bit (x86) program. This is the 64-bit edition of " OS_NAME ", which runs 64-bit (x64) programs; use the 32-bit edition for this one, or rebuild it with x86_64-w64-mingw32-gcc."); return false; }
#else
    if (m == 0x8664) { snprintf(err, ecap, "This is a 64-bit (x64) program. Run it on the 64-bit edition of " OS_NAME " (wrldos64.iso), or rebuild it for i686 (i686-w64-mingw32-gcc)."); return false; }
#endif
    if (m != W32_MACHINE) { snprintf(err, ecap, "Unsupported machine type 0x%04x.", m); return false; }
    const uint8_t *oh = (const uint8_t *)*fh + sizeof(file_hdr_t);
    if (lf + 24 + (*fh)->SizeOfOptionalHeader > n || *(const uint16_t *)oh != PE_MAGIC) { snprintf(err, ecap, "Unsupported optional header (expected " PE_FMT ")."); return false; }
    memset(pi, 0, sizeof *pi);
#ifdef __x86_64__
    const opt_hdr64_t *o = (const opt_hdr64_t *)oh;
#else
    const opt_hdr_t *o = (const opt_hdr_t *)oh;
#endif
    pi->image_base = o->ImageBase; pi->entry = o->AddressOfEntryPoint; pi->size_image = o->SizeOfImage; pi->size_headers = o->SizeOfHeaders;
    pi->subsystem = o->Subsystem; pi->stack_reserve = o->SizeOfStackReserve; pi->nrva = MIN(o->NumberOfRvaAndSizes, 16u);
    memcpy(pi->dir, o->dir, sizeof(data_dir_t) * pi->nrva);
    *sh = (sect_hdr_t *)(oh + (*fh)->SizeOfOptionalHeader);
    if ((*fh)->Characteristics & 0x2000) { snprintf(err, ecap, "This file is a DLL, not a program."); return false; }
    if (pi->nrva > 14 && pi->dir[14].Size) { snprintf(err, ecap, "This is a .NET (CLR) assembly; the .NET runtime is not available."); return false; }
    return true;
}
bool pe_probe(vnode_t *n, char *desc, int cap) {
    file_hdr_t *fh; pe_info_t pi; sect_hdr_t *sh; char err[300];
    if (!n || n->dir || !parse_headers(n->data, n->size, &fh, &pi, &sh, err, sizeof err)) { if (desc) strlcpy(desc, err, cap); return false; }
    if (desc) snprintf(desc, cap, "Win32 %s %s application, %d sections, image base 0x%llx", ARCH_NAME, pi.subsystem == 2 ? "GUI" : "console", fh->NumberOfSections, (unsigned long long)pi.image_base);
    return true;
}
bool pe_is_gui(vnode_t *n) {
    file_hdr_t *fh; pe_info_t pi; sect_hdr_t *sh; char err[300];
    if (!parse_headers(n->data, n->size, &fh, &pi, &sh, err, sizeof err)) return false;
    return pi.subsystem == 2;
}

/* ---------------- process lifetime ---------------- */
static void free_proc(w32_proc_t *p) {
    w32_user_cleanup(p);
    for (int i = 0; i < MAX_HANDLES; i++) p->handles[i].kind = H_NONE;
    alloc_hdr_t *h = p->heap.next;
    while (h) { alloc_hdr_t *nx = h->next; h->magic = 0; kfree(h); h = nx; }
    p->heap.next = NULL;
    for (int i = 0; i < 64; i++) if (procs[i] == p) procs[i] = NULL;
    kfree_aligned(p->frames);
#ifdef __x86_64__
    vm_space_free(p->space);
#else
    paging_free_dir(p->pagedir);
    for (int i = 0; i < p->nptabs; i++) kfree_aligned(p->ptabs[i]);
#endif
    kfree(p->teb); kfree(p->peb); kfree(p->tls_block); kfree(p->tls_array);
    if (p->argv) { for (int i = 0; i < p->argc; i++) kfree(p->argv[i]); kfree(p->argv); }
    kfree(p->envp); kfree(p->acmdln);
    kfree(p);
}
void pe_task_cleanup(task_t *t) {
    w32_proc_t *p = t->proc;
    if (!p) return;
    t->proc = NULL;
    if (p->task != t) { p->threads--; return; }   /* secondary thread */
    /* kill other threads of this process */
    for (task_t *o = task_list(); o; o = o->next) if (o != t && o->proc == p) { o->proc = NULL; task_kill(o->pid); }
    if (p->own_console && p->con) { con_printf(p->con, "\n[%s exited with code %d]\n", path_basename(p->path), t->exit_code); }
    free_proc(p);
}
void w32_exit_process(int code) {
    w32_proc_t *p = w32_cur();
    if (p && !p->exiting) { p->exiting = true; for (int i = p->natexit - 1; i >= 0; i--) p->atexit_fns[i](); }
    task_exit(code);
    for (;;) {}
}
void pe_set_stdin(int pid, console_t *in) {
    for (int i = 0; i < 64; i++) if (procs[i] && procs[i]->pid == pid) procs[i]->in = in;
}
w32_proc_t *pe_find(int pid) { for (int i = 0; i < 64; i++) if (procs[i] && procs[i]->pid == pid) return procs[i]; return NULL; }

static void pe_main(void *arg) {
    w32_proc_t *p = arg;
    if (p->tls_callbacks) {
        uintptr_t *cb = (uintptr_t *)p->tls_callbacks;
        for (int i = 0; i < 16 && cb[i]; i++) call_tls_cb((void *)cb[i], p->base, 1, 0);
    }
    uint32_t r = call_pe_entry((void *)(p->base + p->entry), 0);
    w32_exit_process(r);
}

static void build_args(w32_proc_t *p) {
    /* split cmdline (Windows rules, simplified) */
    char *s = p->cmdline; int cap = 8;
    p->argv = kzalloc(cap * sizeof(char *));
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char buf[512]; int n = 0; bool q = false;
        while (*s && (q || (*s != ' ' && *s != '\t'))) { if (*s == '"') { q = !q; s++; continue; } if (n < 511) buf[n++] = *s; s++; }
        buf[n] = 0;
        if (p->argc + 2 >= cap) { cap *= 2; p->argv = krealloc(p->argv, cap * sizeof(char *)); }
        p->argv[p->argc++] = strdup(buf);
        p->argv[p->argc] = NULL;
    }
    p->envp = kzalloc(4 * sizeof(char *));
    static char env0[] = "OS=THE_WRLD_OS", env1[] = "USERPROFILE=" USER_HOME, env2[] = "PATH=C:\\WRLD\\System32;C:\\WRLD";
    p->envp[0] = env0; p->envp[1] = env1; p->envp[2] = env2;
    p->acmdln = strdup(p->cmdline);
}

/* TEB for a thread; the PEB is shared by the process */
uint8_t *w32_new_teb(w32_proc_t *p, task_t *t) {
    uint8_t *teb = kzalloc(4096);
    uintptr_t top = (uintptr_t)t->stack + t->stack_size, bottom = (uintptr_t)t->stack;
#ifdef __x86_64__
    uint64_t *q = (uint64_t *)teb;                     /* reached through GS */
    q[0x08 / 8] = top; q[0x10 / 8] = bottom;
    q[0x30 / 8] = (uintptr_t)teb;                     /* self */
    q[0x40 / 8] = p->pid; q[0x48 / 8] = t->pid;
    q[0x58 / 8] = (uintptr_t)p->tls_array;
    q[0x60 / 8] = (uintptr_t)p->peb;
#else
    uint32_t *q = (uint32_t *)teb;                     /* reached through FS */
    q[0] = 0xFFFFFFFF;                                 /* SEH chain end */
    q[1] = top; q[2] = bottom;
    q[0x18 / 4] = (uint32_t)teb;                       /* self */
    q[0x20 / 4] = p->pid; q[0x24 / 4] = t->pid;
    q[0x2C / 4] = (uint32_t)p->tls_array;
    q[0x30 / 4] = (uint32_t)p->peb;
#endif
    return teb;
}

int pe_spawn(const char *path, const char *args, console_t *con, char *err, int ecap) {
    vnode_t *n = vfs_resolve(path, NULL);
    if (!n || n->dir) { snprintf(err, ecap, "File not found: %s", path); return -1; }
    file_hdr_t *fh; pe_info_t pi; sect_hdr_t *sh;
    if (!parse_headers(n->data, n->size, &fh, &pi, &sh, err, ecap)) return -1;
    const uint8_t *d = n->data;
    uint32_t size = (pi.size_image + 0xFFF) & ~0xFFFu;
    uint64_t pref = pi.image_base, base = pref;
    bool has_reloc = pi.nrva > 5 && pi.dir[5].Size && !(fh->Characteristics & 1);
#ifdef __x86_64__
    /* the preferred base works if it is 2 MiB aligned and either in the program
     * window (4..16 MiB) or above the kernel's 4 GiB, inside the first 512 GiB */
    bool in_window = base >= WIN_LO && base + size <= WIN_HI;
    bool high_ok = base >= 0x100000000ull && base + size <= (1ull << 39);
    if (!((in_window || high_ok) && !(base & 0xFFF) && size <= 64u * 1024 * 1024)) {
        if (!has_reloc) { snprintf(err, ecap, "Image base 0x%llx cannot be used here and the file has no relocations.", (unsigned long long)base); return -1; }
        base = WIN_LO;
    }
    if (size > 64u * 1024 * 1024) { snprintf(err, ecap, "Program image too large (%u KB).", size / 1024); return -1; }
#else
    if (base < WIN_LO || base + size > WIN_HI) {
        if (!has_reloc) { snprintf(err, ecap, "Image base 0x%08x is outside the supported range and the file has no relocations.", (uint32_t)base); return -1; }
        base = WIN_LO;
    }
    if (size > WIN_HI - WIN_LO) { snprintf(err, ecap, "Program image too large (%u KB).", size / 1024); return -1; }
#endif
    int slot = -1; for (int i = 0; i < 64; i++) if (!procs[i]) { slot = i; break; }
    if (slot < 0) { snprintf(err, ecap, "Too many Win32 processes."); return -1; }

    w32_proc_t *p = kzalloc(sizeof(w32_proc_t));
    strlcpy(p->path, path, sizeof p->path);
    vfs_path(n, p->path, sizeof p->path);
    snprintf(p->cmdline, sizeof p->cmdline, "\"%s\"%s%s", p->path, args && *args ? " " : "", args ? args : "");
    path_dirname(p->path, p->cwd, sizeof p->cwd);
    p->gui = pi.subsystem == 2;
    p->con = con; p->in = con;
    p->fmode = 0x4000; /* _O_TEXT */
    uint8_t *img;
#ifdef __x86_64__
    /* private address space: the image in 4 KiB pages, only as much memory as it needs */
    p->frames = kmalloc_aligned(size, 4096);
    p->space = p->frames ? vm_space_new() : 0;
    if (!p->space) { kfree_aligned(p->frames); kfree(p); snprintf(err, ecap, "Not enough memory to load the program."); return -1; }
    memset(p->frames, 0, size);
    for (uint32_t off = 0; off < size; off += 4096)
        if (!vm_map_4k(p->space, base + off, (uintptr_t)p->frames + off)) {
            vm_space_free(p->space); kfree_aligned(p->frames); kfree(p);
            snprintf(err, ecap, "Not enough memory to map the program."); return -1;
        }
    img = p->frames;                                   /* kernel-visible alias of the image */
#else
    /* private page directory; the image is mapped in 4 KiB pages through page tables */
    if (base & 0xFFF) { kfree(p); snprintf(err, ecap, "Image base 0x%08x is not page aligned.", (uint32_t)base); return -1; }
    p->frames = kmalloc_aligned(size, 4096);
    if (!p->frames) { kfree(p); snprintf(err, ecap, "Not enough memory to load the program."); return -1; }
    memset(p->frames, 0, size);
    p->pagedir = paging_new_dir();
    uint32_t pde_first = (uint32_t)base >> 22, pde_last = (uint32_t)(base + size - 1) >> 22;
    for (uint32_t pd = pde_first; pd <= pde_last && p->nptabs < 4; pd++) {
        uint32_t *pt = kmalloc_aligned(4096, 4096);
        memset(pt, 0, 4096);
        p->ptabs[p->nptabs++] = pt;
        p->pagedir[pd] = (uint32_t)pt | 0x07;
    }
    for (uint32_t off = 0; off < size; off += 4096) {
        uint32_t va = (uint32_t)base + off;
        uint32_t *pt = (uint32_t *)(p->pagedir[va >> 22] & ~0xFFFu);
        pt[(va >> 12) & 1023] = ((uint32_t)p->frames + off) | 0x07;
    }
    p->space = (uintptr_t)p->pagedir;
    img = p->frames;
#endif
    p->base = base; p->size = size; p->entry = pi.entry;
    /* headers + sections */
    memcpy(img, d, MIN(pi.size_headers, n->size));
    for (int i = 0; i < fh->NumberOfSections; i++) {
        sect_hdr_t *s = &sh[i];
        if (s->VirtualAddress + MAX(s->VirtualSize, s->SizeOfRawData) > size) continue;
        uint32_t raw = MIN(s->SizeOfRawData, s->VirtualSize ? s->VirtualSize : s->SizeOfRawData);
        if (s->PointerToRawData + raw <= n->size) memcpy(img + s->VirtualAddress, d + s->PointerToRawData, raw);
    }
    /* relocations */
    int64_t delta = (int64_t)(base - pref);
    if (delta && has_reloc) {
        uint8_t *r = img + pi.dir[5].VirtualAddress, *re = r + pi.dir[5].Size;
        while (r < re) {
            uint32_t page = ((uint32_t *)r)[0], bsz = ((uint32_t *)r)[1];
            if (!bsz) break;
            uint16_t *e = (uint16_t *)(r + 8);
            for (uint32_t k = 0; k < (bsz - 8) / 2; k++) {
                int type = e[k] >> 12, off = e[k] & 0xFFF;
                if (page + off + 8 > size) continue;
                if (type == 10) *(uint64_t *)(img + page + off) += delta;          /* DIR64 */
                else if (type == 3) *(uint32_t *)(img + page + off) += (uint32_t)delta;
                else if (type == 1) *(uint16_t *)(img + page + off) += (uint16_t)(delta >> 16);
                else if (type == 2) *(uint16_t *)(img + page + off) += (uint16_t)delta;
            }
            r += bsz;
        }
    }
    /* imports (thunk entries are pointer-sized; bit 31 / 63 marks an ordinal) */
    int unresolved = 0;
    const uintptr_t ord_flag = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
    if (pi.nrva > 1 && pi.dir[1].Size) {
        for (import_desc_t *id = (import_desc_t *)(img + pi.dir[1].VirtualAddress); id->Name; id++) {
            const char *dll = (const char *)(img + id->Name);
            uintptr_t *ilt = (uintptr_t *)(img + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
            uintptr_t *iat = (uintptr_t *)(img + id->FirstThunk);
            const dll_t *dl = find_dll(dll);
            for (int k = 0; ilt[k]; k++) {
                void *fn = NULL;
                char name[64];
                if (ilt[k] & ord_flag) snprintf(name, sizeof name, "Ordinal%u", (uint32_t)(ilt[k] & 0xFFFF));
                else strlcpy(name, (const char *)(img + (uint32_t)ilt[k] + 2), sizeof name);
                if (dl) { for (int e = 0; e < *dl->count; e++) if (!strcmp(dl->tab[e].name, name)) { fn = dl->tab[e].fn; break; } }
                if (!fn && dl && dl->tab == msvcrt_exports) {   /* C runtime variables imported as data (x64 CRT) */
                    if (!strcmp(name, "_fmode")) fn = &p->fmode;
                    else if (!strcmp(name, "_commode")) fn = &p->commode;
                    else if (!strcmp(name, "_acmdln")) fn = &p->acmdln;
                    else if (!strcmp(name, "__argc")) fn = &p->argc;
                    else if (!strcmp(name, "__argv")) fn = &p->argv;
                    else if (!strcmp(name, "_environ") || !strcmp(name, "__initenv")) fn = &p->envp;
                }
                if (!fn) { fn = make_thunk(dll, name); unresolved++; klog("w32: %s: unresolved import %s!%s", path_basename(p->path), dll, name); }
                iat[k] = (uintptr_t)fn;
            }
        }
    }
    /* TLS */
    p->tls_array = kzalloc(64 * sizeof(uintptr_t));
    if (pi.nrva > 9 && pi.dir[9].Size) {
#ifdef __x86_64__
        tls_dir64_t *td = (tls_dir64_t *)(img + pi.dir[9].VirtualAddress);
#else
        tls_dir32_t *td = (tls_dir32_t *)(img + pi.dir[9].VirtualAddress);
#endif
        uint64_t start = td->StartAddressOfRawData, end = td->EndAddressOfRawData, idx = td->AddressOfIndex;
        uint64_t sh_ = pref != base ? base - pref : 0;  /* the directory's VAs were relocated already */
        (void)sh_;
        uint32_t len = end > start && end - start < 0x1000000 ? (uint32_t)(end - start) : 0;
        p->tls_block = kzalloc(len + td->SizeOfZeroFill + 16);
        if (start >= base && end <= base + size) memcpy(p->tls_block, img + (start - base), len);
        if (idx >= base && idx < base + size) *(uint32_t *)(img + (idx - base)) = 0;
        p->tls_array[0] = (uintptr_t)p->tls_block;
        p->tls_callbacks = (uintptr_t)td->AddressOfCallBacks;
    }
    build_args(p);
    /* PEB */
    p->peb = kzalloc(4096);
#ifdef __x86_64__
    ((uint64_t *)p->peb)[0x10 / 8] = base;
    ((uint64_t *)p->peb)[0x30 / 8] = 0x00ABC000;   /* process heap pseudo handle */
#else
    ((uint32_t *)p->peb)[0x08 / 4] = (uint32_t)base;
    ((uint32_t *)p->peb)[0x18 / 4] = 0x00ABC000;
#endif
    /* task */
    char tname[48]; strlcpy(tname, path_basename(p->path), sizeof tname);
    uint32_t stack = (uint32_t)MAX(MIN(pi.stack_reserve, 2ull * 1024 * 1024), 256ull * 1024);
    task_t *t = task_create(tname, pe_main, p, stack);
    t->kind = 2; t->proc = p; t->cr3 = p->space; t->console = con;
    p->pid = t->pid;
    p->teb = w32_new_teb(p, t);
    t->teb = (uintptr_t)p->teb;
    p->task = t; p->threads = 1;
    procs[slot] = p;
    klog("w32: loaded %s (%s) at 0x%llx (%u KB, %s, %d unresolved imports) as pid %d", p->path, ARCH_NAME, (unsigned long long)base, size / 1024, p->gui ? "GUI" : "console", unresolved, t->pid);
    return t->pid;
}
int pe_proc_list(int *pids, int max) { int n = 0; for (int i = 0; i < 64 && n < max; i++) if (procs[i]) pids[n++] = procs[i]->pid; return n; }
