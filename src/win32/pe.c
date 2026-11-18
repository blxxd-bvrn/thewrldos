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
extern const w32_export_t ws2_exports[]; extern const int ws2_count;   /* ws2_32.c */
extern const w32_export_t imm32_exports[], ole32_exports[], comdlg32_exports[], comctl32_exports[];   /* user32_dlg.inc */
extern const int imm32_count, ole32_count, comdlg32_count, comctl32_count;
extern const w32_export_t winspool_exports[]; extern const int winspool_count;
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
    { "d3d10.dll", d3d10_exports, &d3d10_count }, { "dxgi.dll", dxgi_exports, &dxgi_count },
    { "d3dcompiler_47.dll", d3dcompiler_exports, &d3dcompiler_count },
    { "ws2_32.dll", ws2_exports, &ws2_count }, { "wsock32.dll", ws2_exports, &ws2_count },
    { "imm32.dll", imm32_exports, &imm32_count }, { "ole32.dll", ole32_exports, &ole32_count },
    { "comdlg32.dll", comdlg32_exports, &comdlg32_count }, { "comctl32.dll", comctl32_exports, &comctl32_count },
    { "winspool.drv", winspool_exports, &winspool_count },
};
static const dll_t *find_dll(const char *name) {
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) if (!strcasecmp(dlls[i].dll, name)) return &dlls[i];
    if (!strncasecmp(name, "api-ms-win-crt-", 15)) return &dlls[1];
    if (!strncasecmp(name, "d3dx9_", 6)) return find_dll("d3dx9_43.dll");
    if (!strncasecmp(name, "d3dcompiler_", 12)) return find_dll("d3dcompiler_47.dll");
    if (!strncasecmp(name, "api-ms-win-core-", 16)) return &dlls[0];
    return NULL;
}
void *w32_compat_find(const char *dll, const char *name);   /* compat.c */
bool w32_lookup(const char *dll, const char *name, void **out) {
    void *xf = w32_extra_find(dll, name);
    if (xf) { *out = xf; return true; }
    const dll_t *d = dll ? find_dll(dll) : NULL;
    for (unsigned i = 0; i < ARRAY_LEN(dlls); i++) {
        const dll_t *x = d ? d : &dlls[i];
        for (int k = 0; k < *x->count; k++) if (!strcmp(x->tab[k].name, name)) { *out = x->tab[k].fn; return true; }
        if (d) break;
    }
    void *fn = w32_compat_find(dll, name);
    if (fn) { *out = fn; return true; }
    return false;
}
const char *w32_api_list(int dll, int idx) {
    if (dll < 0 || dll >= 6) return NULL;
    if (idx < 0 || idx >= *dlls[dll].count) return NULL;
    return dlls[dll].tab[idx].name;
}
/* ---------------- resources ----------------
 * The resource directory is a three-level tree (type, name, language) of
 * IMAGE_RESOURCE_DIRECTORY tables; entries are named (a counted UTF-16 string,
 * high bit set) or numbered. A leaf is an IMAGE_RESOURCE_DATA_ENTRY. */
typedef struct PACKED { uint32_t chars, stamp; uint16_t major, minor, named, ids; } resdir_t;
typedef struct PACKED { uint32_t name, off; } resent_t;
typedef struct PACKED { uint32_t rva, size, cp, res; } resdata_t;
static bool res_name_eq(const uint8_t *root, uint32_t nameoff, uintptr_t want, bool wide) {
    const uint16_t *s = (const uint16_t *)(root + (nameoff & 0x7FFFFFFF));
    uint16_t len = s[0];
    for (uint16_t i = 0; i < len; i++) {
        uint16_t c = s[1 + i], w = wide ? ((const uint16_t *)want)[i] : ((const uint8_t *)want)[i];
        if (!w) return false;
        if (toupper(c < 128 ? c : 0) != toupper(w < 128 ? w : 0) && c != w) return false;
    }
    return wide ? ((const uint16_t *)want)[len] == 0 : ((const uint8_t *)want)[len] == 0;
}
static uintptr_t res_id(uintptr_t v, bool wide) {          /* "#123" -> 123 */
    if (v < 0x10000) return v;
    if (wide) { const uint16_t *w = (const uint16_t *)v; if (w[0] != '#') return v; uintptr_t n = 0; for (int i = 1; w[i] >= '0' && w[i] <= '9'; i++) n = n * 10 + (w[i] - '0'); return n; }
    const char *a = (const char *)v; return a[0] == '#' ? (uintptr_t)atoi(a + 1) : v;
}
/* the entry of one directory level matching id (or the first entry when id is ~0) */
static const resent_t *res_level(const uint8_t *root, uint32_t diroff, uintptr_t id, bool wide) {
    const resdir_t *d = (const resdir_t *)(root + diroff);
    const resent_t *e = (const resent_t *)(d + 1);
    int n = d->named + d->ids;
    for (int i = 0; i < n; i++) {
        if (id == ~(uintptr_t)0) return &e[i];
        if (id < 0x10000) { if (!(e[i].name & 0x80000000) && e[i].name == id) return &e[i]; }
        else if ((e[i].name & 0x80000000) && res_name_eq(root, e[i].name, id, wide)) return &e[i];
    }
    return NULL;
}
w32_mod_t *w32_mod_by_base(w32_proc_t *p, uintptr_t base);
w32_mod_t *w32_mod_at(w32_proc_t *p, uintptr_t a);
uintptr_t w32_find_resource(w32_proc_t *p, uintptr_t type, uintptr_t name, bool wide, int lang) {
    return w32_find_resource_in(p, 0, type, name, wide, lang);
}
/* in the module hmod (0 or the program: the program's own resources) */
uintptr_t w32_find_resource_in(w32_proc_t *p, uintptr_t hmod, uintptr_t type, uintptr_t name, bool wide, int lang) {
    if (!p) return 0;
    uintptr_t mbase = p->base; uint32_t rva = p->res_rva;
    w32_mod_t *m = hmod && hmod != p->base ? w32_mod_by_base(p, hmod) : NULL;
    if (m) { mbase = m->base; rva = m->res_rva; }
    if (!rva) return 0;
    const uint8_t *root = (const uint8_t *)(mbase + rva);
    const resent_t *t = res_level(root, 0, res_id(type, wide), wide);
    if (!t || !(t->off & 0x80000000)) return 0;
    const resent_t *n = res_level(root, t->off & 0x7FFFFFFF, res_id(name, wide), wide);
    if (!n || !(n->off & 0x80000000)) return 0;
    const resent_t *l = lang ? res_level(root, n->off & 0x7FFFFFFF, (uintptr_t)lang, false) : NULL;
    if (!l) l = res_level(root, n->off & 0x7FFFFFFF, ~(uintptr_t)0, false);      /* any language */
    if (!l || (l->off & 0x80000000)) return 0;
    return (uintptr_t)(root + l->off);
}
void *w32_resource_data(w32_proc_t *p, uintptr_t hrsrc, uint32_t *size) {
    if (!p || !hrsrc) return NULL;
    const resdata_t *d = (const resdata_t *)hrsrc;
    if (size) *size = d->size;
    w32_mod_t *m = w32_mod_at(p, hrsrc);             /* the data entry lies in its module's image */
    return (void *)((m ? m->base : p->base) + d->rva);
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
static bool parse_headers_ex(const uint8_t *d, uint32_t n, file_hdr_t **fh, pe_info_t *pi, sect_hdr_t **sh, char *err, int ecap, bool dll) {
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
    if (!dll && ((*fh)->Characteristics & 0x2000)) { snprintf(err, ecap, "This file is a DLL, not a program."); return false; }
    if (dll && !((*fh)->Characteristics & 0x2000)) { snprintf(err, ecap, "This file is not a DLL."); return false; }
    if (pi->nrva > 14 && pi->dir[14].Size) { snprintf(err, ecap, "This is a .NET (CLR) assembly; the .NET runtime is not available."); return false; }
    return true;
}
static bool parse_headers(const uint8_t *d, uint32_t n, file_hdr_t **fh, pe_info_t *pi, sect_hdr_t **sh, char *err, int ecap) {
    return parse_headers_ex(d, n, fh, pi, sh, err, ecap, false);
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
    for (int i = 0; i < p->nmods; i++) { if (!p->mods[i]->is_exe) kfree_aligned(p->mods[i]->img); kfree(p->mods[i]); }
    p->nmods = 0;
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
    w32_run_dll_inits(p);
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
    /* the environment: what Windows programs expect to find (each its own string, so
     * SetEnvironmentVariable and _putenv can replace entries) */
    static const char *const fixed[] = {
        "OS=Windows_NT", "WRLD_OS=THE_WRLD_OS", "SystemDrive=C:", "SystemRoot=C:\\WRLD", "windir=C:\\WRLD", "ComSpec=C:\\WRLD\\System32\\cmd.exe",
        "PATH=C:\\WRLD\\System32;C:\\WRLD", "PATHEXT=.COM;.EXE;.BAT;.CMD", "TEMP=C:\\tmp", "TMP=C:\\tmp", "USERPROFILE=" USER_HOME,
        "HOMEDRIVE=C:", "APPDATA=" USER_HOME "\\Application Data", "LOCALAPPDATA=" USER_HOME "\\Local Settings\\Application Data",
        "ProgramFiles=C:\\Program Files", "CommonProgramFiles=C:\\Program Files\\Common Files", "ALLUSERSPROFILE=C:\\Documents and Settings\\All Users",
        "NUMBER_OF_PROCESSORS=1",
#ifdef __x86_64__
        "PROCESSOR_ARCHITECTURE=AMD64",
#else
        "PROCESSOR_ARCHITECTURE=x86",
#endif
    };
    int nf = ARRAY_LEN(fixed);
    p->envp = kzalloc((nf + 6) * sizeof(char *));
    for (int i = 0; i < nf; i++) p->envp[i] = (char *)fixed[i];
    char e[300];
    snprintf(e, sizeof e, "HOMEPATH=%s", USER_HOME + 2); p->envp[nf] = strdup(e);
    snprintf(e, sizeof e, "USERNAME=%s", user_name); p->envp[nf + 1] = strdup(e);
    snprintf(e, sizeof e, "COMPUTERNAME=%s", computer_name); p->envp[nf + 2] = strdup(e);
    snprintf(e, sizeof e, "USERDOMAIN=%s", computer_name); p->envp[nf + 3] = strdup(e);
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

/* ---------------- program DLLs ----------------
 * A DLL that is not one of the built-in system DLLs is loaded from a file: the
 * program's folder, the current folder, C:\WRLD\System32 or C:\WRLD. Each process
 * gets its own copy, mapped into its address space (x64: from 448 GiB up, or at the
 * DLL's preferred base; x86: in the 4..16 MiB window after the program, then in the
 * DLL area mm.c sets aside), relocated, with its own imports resolved the same way.
 * DllMain(DLL_PROCESS_ATTACH) runs in the process's task, dependencies first. */
#define DLL_BASE64 0x7000000000ull
#define DLL_TOP64  0x7F00000000ull
static void set_mod_dirs(w32_mod_t *m, const pe_info_t *pi) {
    if (pi->nrva > 0) { m->exp_rva = pi->dir[0].VirtualAddress; m->exp_size = pi->dir[0].Size; }
    if (pi->nrva > 2) { m->res_rva = pi->dir[2].VirtualAddress; m->res_size = pi->dir[2].Size; }
    if (pi->nrva > 3) { m->pdata_rva = pi->dir[3].VirtualAddress; m->pdata_size = pi->dir[3].Size; }
}
bool w32_is_system_dll(const char *name) {
    char b[64]; strlcpy(b, path_basename(name), sizeof b);
    if (!strchr(b, '.')) strlcat(b, ".dll", sizeof b);
    if (find_dll(b)) return true;
    if (!strncasecmp(b, "api-ms-", 7) || !strncasecmp(b, "ext-ms-", 7)) return true;
    static const char *sys[] = { "oleaut32.dll", "shlwapi.dll", "winmm.dll", "version.dll", "setupapi.dll", "crypt32.dll", "uxtheme.dll",
        "dwmapi.dll", "psapi.dll", "secur32.dll", "iphlpapi.dll", "bcrypt.dll", "userenv.dll", "netapi32.dll", "wtsapi32.dll", "oleacc.dll",
        "msimg32.dll", "powrprof.dll", "dbghelp.dll", "shcore.dll", "wldap32.dll", "normaliz.dll", "rpcrt4.dll", "mpr.dll", "dnsapi.dll",
        "winhttp.dll", "wininet.dll", "urlmon.dll", "msacm32.dll", "avrt.dll", "hid.dll", "dinput8.dll", "dinput.dll", "xinput1_3.dll",
        "xinput1_4.dll", "xinput9_1_0.dll", "dsound.dll", "ddraw.dll", "usp10.dll", "gdiplus.dll", "comctl32.dll", "msvcp60.dll", "imagehlp.dll", "cfgmgr32.dll", "bcryptprimitives.dll", "propsys.dll", "sechost.dll", "credui.dll", "dwrite.dll", "d2d1.dll", "winmm.dll" };
    for (unsigned i = 0; i < ARRAY_LEN(sys); i++) if (!strcasecmp(b, sys[i])) return true;
    return false;
}
w32_mod_t *w32_mod_by_base(w32_proc_t *p, uintptr_t base) {
    for (int i = 0; p && i < p->nmods; i++) if (p->mods[i]->base == base) return p->mods[i];
    return NULL;
}
w32_mod_t *w32_mod_by_name(w32_proc_t *p, const char *name) {
    char b[64]; strlcpy(b, path_basename(name), sizeof b);
    if (!strchr(b, '.')) strlcat(b, ".dll", sizeof b);
    for (int i = 0; p && i < p->nmods; i++) if (!strcasecmp(p->mods[i]->name, b)) return p->mods[i];
    return NULL;
}
w32_mod_t *w32_mod_at(w32_proc_t *p, uintptr_t a) {
    for (int i = 0; p && i < p->nmods; i++) if (a >= p->mods[i]->base && a < p->mods[i]->base + p->mods[i]->size) return p->mods[i];
    return NULL;
}
/* map size bytes of frames at va in the process */
static bool map_pages(w32_proc_t *p, uintptr_t va, uint8_t *frames, uint32_t size) {
#ifdef __x86_64__
    for (uint32_t off = 0; off < size; off += 4096) if (!vm_map_4k(p->space, va + off, (uintptr_t)frames + off)) return false;
#else
    for (uint32_t off = 0; off < size; off += 4096) {
        uint32_t a = (uint32_t)va + off, pd = a >> 22;
        uint32_t *pt = (uint32_t *)(p->pagedir[pd] & ~0xFFFu);
        bool own = false;
        for (int i = 0; i < p->nptabs; i++) if (p->ptabs[i] == pt && !(p->pagedir[pd] & 0x80)) own = true;
        if (!own) {
            if (p->nptabs >= (int)ARRAY_LEN(p->ptabs)) return false;
            pt = kmalloc_aligned(4096, 4096);
            if (!pt) return false;
            memset(pt, 0, 4096);
            p->ptabs[p->nptabs++] = pt;
            p->pagedir[pd] = (uint32_t)pt | 0x07;
        }
        pt[(a >> 12) & 1023] = ((uint32_t)frames + off) | 0x07;
    }
    uint32_t cr3; __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
    if (cr3 == (uint32_t)p->pagedir) __asm__ volatile("mov %0,%%cr3" :: "r"(cr3) : "memory");
#endif
    return true;
}
static bool range_free(w32_proc_t *p, uint64_t a, uint32_t size) {
    for (int i = 0; i < p->nmods; i++) { w32_mod_t *m = p->mods[i]; if (a < m->base + m->size && m->base < a + size) return false; }
    return true;
}
/* an address for a DLL of this size: its preferred base when that is usable, else the next free one */
static uintptr_t dll_address(w32_proc_t *p, uint64_t pref, uint32_t size, bool reloc) {
#ifdef __x86_64__
    if (pref >= 0x100000000ull && pref + size <= DLL_TOP64 && !(pref & 0xFFF) && range_free(p, pref, size)) return pref;
    if (!reloc) return 0;
    uintptr_t a = (p->dll_next + 0xFFFF) & ~(uintptr_t)0xFFFF;
    if (a + size > DLL_TOP64) return 0;
    p->dll_next = a + size;
    return a;
#else
    if (!reloc) {
        bool ok = (pref >= WIN_LO && pref + size <= WIN_HI) || (w32_dll_area && pref >= w32_dll_area && pref + size <= w32_dll_area + w32_dll_area_size);
        return ok && !(pref & 0xFFF) && range_free(p, pref, size) ? (uintptr_t)pref : 0;
    }
    uintptr_t a = (p->dll_next + 0xFFFF) & ~(uintptr_t)0xFFFF;
    if (a < WIN_HI && a + size > WIN_HI) a = w32_dll_area;            /* the window is full: the DLL area */
    if (!a || (a >= WIN_HI && (!w32_dll_area || a + size > w32_dll_area + w32_dll_area_size))) return 0;
    p->dll_next = a + size;
    return a;
#endif
}
static void relocate(uint8_t *img, uint32_t size, const pe_info_t *pi, int64_t delta) {
    if (!delta || pi->nrva <= 5 || !pi->dir[5].Size) return;
    uint8_t *r = img + pi->dir[5].VirtualAddress, *re = r + pi->dir[5].Size;
    while (r + 8 <= re) {
        uint32_t page = ((uint32_t *)r)[0], bsz = ((uint32_t *)r)[1];
        if (bsz < 8) break;
        uint16_t *e = (uint16_t *)(r + 8);
        for (uint32_t k = 0; k < (bsz - 8) / 2; k++) {
            int type = e[k] >> 12, off = e[k] & 0xFFF;
            if (page + off + 8 > size) continue;
            if (type == 10) *(uint64_t *)(img + page + off) += delta;
            else if (type == 3) *(uint32_t *)(img + page + off) += (uint32_t)delta;
            else if (type == 1) *(uint16_t *)(img + page + off) += (uint16_t)(delta >> 16);
            else if (type == 2) *(uint16_t *)(img + page + off) += (uint16_t)delta;
        }
        r += bsz;
    }
}
static bool find_dll_file(w32_proc_t *p, const char *name, char *out, int cap) {
    char b[96]; strlcpy(b, name, sizeof b);
    if (!strchr(path_basename(b), '.')) strlcat(b, ".dll", sizeof b);
    if (strchr(b, '\\') || strchr(b, '/') || strchr(b, ':')) {
        if (isalpha(b[0]) && b[1] == ':') strlcpy(out, b, cap);
        else path_join(out, cap, p->cwd, b);
        path_normalize(out);
        vnode_t *v = vfs_resolve(out, NULL);
        return v && !v->dir;
    }
    char dir[256]; path_dirname(p->mods[0] ? p->mods[0]->path : p->path, dir, sizeof dir);
    const char *dirs[] = { dir, p->cwd, "C:\\WRLD\\System32", "C:\\WRLD" };
    for (unsigned i = 0; i < ARRAY_LEN(dirs); i++) {
        path_join(out, cap, dirs[i], b); path_normalize(out);
        vnode_t *v = vfs_resolve(out, NULL);
        if (v && !v->dir) return true;
    }
    return false;
}
static int resolve_imports(w32_proc_t *p, uint8_t *img, const pe_info_t *pi);
static w32_mod_t *load_mod(w32_proc_t *p, const char *name, int depth) {
    w32_mod_t *m = w32_mod_by_name(p, name);
    if (m) { m->refs++; return m; }
    if (depth > 16 || p->nmods >= W32_MAX_MODS) return NULL;
    char path[300];
    if (!find_dll_file(p, name, path, sizeof path)) return NULL;
    vnode_t *n = vfs_resolve(path, NULL);
    file_hdr_t *fh; pe_info_t pi; sect_hdr_t *sh; char err[200];
    if (!parse_headers_ex(n->data, n->size, &fh, &pi, &sh, err, sizeof err, true)) { klog("w32: %s: %s", path, err); return NULL; }
    uint32_t size = (pi.size_image + 0xFFF) & ~0xFFFu;
    if (!size || size > 64u * 1024 * 1024) return NULL;
    bool reloc = pi.nrva > 5 && pi.dir[5].Size && !(fh->Characteristics & 1);
    uintptr_t base = dll_address(p, pi.image_base, size, reloc);
    if (!base) { klog("w32: no room for %s (%u KB)", path, size / 1024); return NULL; }
    uint8_t *img = kmalloc_aligned(size, 4096);
    if (!img) return NULL;
    memset(img, 0, size);
    if (!map_pages(p, base, img, size)) { kfree_aligned(img); return NULL; }
    memcpy(img, n->data, MIN(pi.size_headers, n->size));
    for (int i = 0; i < fh->NumberOfSections; i++) {
        sect_hdr_t *sc = &sh[i];
        if (sc->VirtualAddress + MAX(sc->VirtualSize, sc->SizeOfRawData) > size) continue;
        uint32_t raw = MIN(sc->SizeOfRawData, sc->VirtualSize ? sc->VirtualSize : sc->SizeOfRawData);
        if (sc->PointerToRawData + raw <= n->size) memcpy(img + sc->VirtualAddress, n->data + sc->PointerToRawData, raw);
    }
    relocate(img, size, &pi, (int64_t)(base - pi.image_base));
    m = kzalloc(sizeof *m);
    strlcpy(m->path, path, sizeof m->path); strlcpy(m->name, path_basename(path), sizeof m->name);
    m->base = base; m->size = size; m->entry = pi.entry; m->img = img; m->refs = 1;
    set_mod_dirs(m, &pi);
    p->mods[p->nmods++] = m;                       /* before its imports, so circular imports find it */
    int un = resolve_imports(p, img, &pi);
    if (p->ninitq < W32_MAX_MODS) p->initq[p->ninitq++] = m;
    klog("w32: %s: loaded %s at 0x%llx (%u KB, %d unresolved imports)", path_basename(p->path), path, (unsigned long long)base, size / 1024, un);
    return m;
}
uintptr_t w32_load_library(w32_proc_t *p, const char *name) {
    if (!p || !name || !*name) return 0;
    w32_mod_t *m = w32_mod_by_name(p, name);
    if (m) { m->refs++; return m->base; }
    char b[96]; strlcpy(b, name, sizeof b);
    for (char *c = b; *c; c++) if (*c == '/') *c = '\\';
    if (w32_is_system_dll(path_basename(b))) return 0;
    m = load_mod(p, b, 0);
    return m ? m->base : 0;
}
static void *builtin_import(w32_proc_t *p, const char *dll, const char *name);
void *w32_mod_export(w32_proc_t *p, w32_mod_t *m, const char *name, uint32_t ord) {
    if (!m || !m->exp_rva || m->exp_rva + 40 > m->size) return NULL;
    const uint8_t *img = m->img;
    const uint32_t *ed = (const uint32_t *)(img + m->exp_rva);
    uint32_t obase = ed[4], nfunc = ed[5], nname = ed[6];
    const uint32_t *funcs = (const uint32_t *)(img + ed[7]), *names = (const uint32_t *)(img + ed[8]);
    const uint16_t *ords = (const uint16_t *)(img + ed[9]);
    int64_t idx = -1;
    if (name && name[0] == '#') { ord = (uint32_t)atoi(name + 1); name = NULL; }
    if (name) {
        int lo = 0, hi = (int)nname - 1;                 /* names are sorted */
        while (lo <= hi) {
            int mid = (lo + hi) / 2, c = strcmp((const char *)(img + names[mid]), name);
            if (!c) { idx = ords[mid]; break; }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (idx < 0) for (uint32_t i = 0; i < nname; i++) if (!strcmp((const char *)(img + names[i]), name)) { idx = ords[i]; break; }
    } else if (ord >= obase) idx = ord - obase;
    if (idx < 0 || idx >= nfunc || !funcs[idx]) return NULL;
    uint32_t rva = funcs[idx];
    if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size) {    /* forwarder: "OTHER.Name" or "OTHER.#12" */
        char fw[128]; strlcpy(fw, (const char *)(img + rva), sizeof fw);
        char *dot = strrchr(fw, '.');
        if (!dot) return NULL;
        *dot = 0;
        char dll[96]; snprintf(dll, sizeof dll, "%s.dll", fw);
        if (w32_is_system_dll(dll)) return builtin_import(p, dll, dot + 1);
        w32_mod_t *o = load_mod(p, dll, 1);
        return o && o != m ? w32_mod_export(p, o, dot + 1, 0) : NULL;
    }
    return (void *)(m->base + rva);
}
static void *builtin_import(w32_proc_t *p, const char *dll, const char *name) {
    void *fn = w32_extra_find(dll, name);
    if (fn) return fn;
    const dll_t *dl = find_dll(dll);
    if (dl) { for (int e = 0; e < *dl->count; e++) if (!strcmp(dl->tab[e].name, name)) { fn = dl->tab[e].fn; break; } }
    if (!fn && dl && dl->tab == msvcrt_exports) {   /* C runtime variables imported as data (x64 CRT) */
        if (!strcmp(name, "_fmode")) fn = &p->fmode;
        else if (!strcmp(name, "_commode")) fn = &p->commode;
        else if (!strcmp(name, "_acmdln")) fn = &p->acmdln;
        else if (!strcmp(name, "__argc")) fn = &p->argc;
        else if (!strcmp(name, "__argv")) fn = &p->argv;
        else if (!strcmp(name, "_environ") || !strcmp(name, "__initenv")) fn = &p->envp;
    }
    if (!fn) fn = w32_compat_find(dll, name);
    return fn;
}
/* imports (thunk entries are pointer-sized; bit 31 / 63 marks an ordinal) */
static int resolve_imports(w32_proc_t *p, uint8_t *img, const pe_info_t *pi) {
    int unresolved = 0;
    const uintptr_t ord_flag = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
    if (pi->nrva <= 1 || !pi->dir[1].Size) return 0;
    for (import_desc_t *id = (import_desc_t *)(img + pi->dir[1].VirtualAddress); id->Name; id++) {
        const char *dll = (const char *)(img + id->Name);
        uintptr_t *ilt = (uintptr_t *)(img + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        uintptr_t *iat = (uintptr_t *)(img + id->FirstThunk);
        w32_mod_t *m = w32_is_system_dll(dll) ? NULL : load_mod(p, dll, 1);
        for (int k = 0; ilt[k]; k++) {
            void *fn = NULL;
            char name[64]; uint32_t ord = 0;
            if (ilt[k] & ord_flag) { ord = (uint32_t)(ilt[k] & 0xFFFF); snprintf(name, sizeof name, "Ordinal%u", ord); }
            else strlcpy(name, (const char *)(img + (uint32_t)ilt[k] + 2), sizeof name);
            if (m) fn = w32_mod_export(p, m, ord ? NULL : name, ord);
            else fn = builtin_import(p, dll, name);
            if (!fn) { fn = make_thunk(dll, name); unresolved++; klog("w32: %s: unresolved import %s!%s", path_basename(p->path), dll, name); }
            iat[k] = (uintptr_t)fn;
        }
    }
    return unresolved;
}
/* DllMain(hinst, DLL_PROCESS_ATTACH, NULL) for the DLLs loaded since the last call */
void w32_run_dll_inits(w32_proc_t *p) {
    for (int i = 0; i < p->ninitq; i++) {
        w32_mod_t *m = p->initq[i];
        if (m->inited) continue;
        m->inited = true;
        if (!m->entry) continue;
        uint32_t ok = call_tls_cb((void *)(m->base + m->entry), m->base, 1, 0);
        if (!(ok & 0xFF)) klog("w32: pid %d: DllMain of %s failed", p->pid, m->name);
    }
    p->ninitq = 0;
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
    for (uint32_t pd = pde_first; pd <= pde_last && p->nptabs < (int)ARRAY_LEN(p->ptabs); pd++) {
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
    if (pi.nrva > 2 && pi.dir[2].Size) { p->res_rva = pi.dir[2].VirtualAddress; p->res_size = pi.dir[2].Size; }
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
    /* the program is module 0; DLLs it imports from files are loaded after it */
    w32_mod_t *me = kzalloc(sizeof *me);
    strlcpy(me->path, p->path, sizeof me->path); strlcpy(me->name, path_basename(p->path), sizeof me->name);
    me->base = base; me->size = size; me->entry = pi.entry; me->img = img; me->refs = 1; me->inited = true; me->is_exe = true;
    set_mod_dirs(me, &pi);
    p->mods[p->nmods++] = me;
#ifdef __x86_64__
    p->dll_next = DLL_BASE64;
#else
    p->dll_next = (base + size + 0xFFFF) & ~(uintptr_t)0xFFFF;
#endif
    int unresolved = resolve_imports(p, img, &pi);
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
