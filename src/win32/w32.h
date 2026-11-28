#ifndef W32_H
#define W32_H
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"

/* x86: stdcall/cdecl. x64: one convention, Microsoft's (rcx, rdx, r8, r9, 32-byte
 * shadow space), which GCC produces for functions marked ms_abi. Variadic
 * exports read their arguments through the matching va_list. */
#ifdef __x86_64__
#define WINAPI __attribute__((ms_abi))
#define CDECL __attribute__((ms_abi))
typedef __builtin_ms_va_list w32_va_list;
#define w32_va_start(ap, last) __builtin_ms_va_start(ap, last)
#define w32_va_end(ap) __builtin_ms_va_end(ap)
#define w32_va_copy(d, s) __builtin_ms_va_copy(d, s)
#define W32_MACHINE 0x8664
#else
#define WINAPI __attribute__((stdcall))
#define CDECL __attribute__((cdecl))
typedef va_list w32_va_list;
#define w32_va_start(ap, last) va_start(ap, last)
#define w32_va_end(ap) va_end(ap)
#define w32_va_copy(d, s) __builtin_va_copy(d, s)
#define W32_MACHINE 0x014C
#endif
/* LLP64: handles and WPARAM/LPARAM/LRESULT are pointer-sized, DWORD/LONG stay 32-bit */
typedef uint32_t DWORD, UINT, COLORREF;
typedef uint16_t ATOM;
typedef uintptr_t HANDLE, HWND, HDC, HGDIOBJ, HINSTANCE, HMODULE, WPARAM, LPARAM, LRESULT, SIZE_T, ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef int BOOL, LONG, INT;
typedef uint16_t WORD, WCHAR;
typedef uint8_t BYTE;

/* ---- the PE file format (rust/src/pe.rs) ----
 * Everything read from a program or DLL is checked against the file or image first. */
typedef struct {
    uint64_t image_base, stack_reserve;
    uint32_t entry, size_image, size_headers, subsystem, characteristics, nsections, nrva, machine;
    uint32_t dir_rva[16], dir_size[16];                 /* 0 exports, 1 imports, 2 resources, 3 .pdata, 5 relocations, 9 TLS */
} pe_hdr_t;
typedef struct { uint64_t callbacks; uint32_t data_off, data_len, zero_fill, index_off; } pe_tls_t;   /* index_off ~0: none */
bool pe_parse(const uint8_t *file, uint32_t n, bool dll, pe_hdr_t *out, char *err, int ecap);   /* this edition's kind only */
void pe_load_image(const uint8_t *file, uint32_t n, bool dll, uint8_t *img, uint32_t size);      /* headers + sections */
void pe_relocate(uint8_t *img, uint32_t size, const pe_hdr_t *h, int64_t delta);
/* walks the imports: dll(ctx, name) once per DLL, entry(ctx, that, dll, name or NULL, ordinal) per
 * function; their results fill the import address table. Returns the entries bound. */
int pe_bind_imports(uint8_t *img, uint32_t size, const pe_hdr_t *h, void *ctx,
                    uintptr_t (*dll)(void *ctx, const char *name),
                    uintptr_t (*entry)(void *ctx, uintptr_t dllh, const char *dll, const char *name, uint32_t ord));
/* an export's RVA, 0 when there is none, ~0 for a forwarder (then "DLL.Name" in fwd) */
uint32_t pe_export(const uint8_t *img, uint32_t size, uint32_t exp_rva, uint32_t exp_size, const char *name, uint32_t ord, char *fwd, int fcap);
/* type and name: numbers below 0x10000, else (wide or narrow) strings, "#123" a number */
uint32_t pe_find_resource(const uint8_t *img, uint32_t size, uint32_t res_rva, uintptr_t type, uintptr_t name, bool wide, int lang);
bool pe_resource_data(const uint8_t *img, uint32_t size, uint32_t entry, uint32_t *rva, uint32_t *len);
bool pe_tls(const uint8_t *img, uint32_t size, const pe_hdr_t *h, uint64_t base, pe_tls_t *out);

typedef struct w32_proc w32_proc_t;
/* a DLL loaded into a process from a file (pe.c); the program itself is mods[0] */
typedef struct w32_mod {
    char name[64], path[256];
    uintptr_t base; uint32_t size, entry;
    uint32_t exp_rva, exp_size, res_rva, res_size, pdata_rva, pdata_size;
    uint8_t *img;               /* kernel-visible alias of the image (the program's: p->frames) */
    int refs; bool inited, is_exe;
} w32_mod_t;
#define W32_MAX_MODS 48
#define MAX_HANDLES 128
enum { H_NONE, H_FILE, H_THREAD, H_FIND, H_EVENT, H_PROCESS, H_SEM, H_MUTEX };
typedef struct { int kind; vnode_t *node; uint32_t pos; bool write; int tid; vnode_t *find_dir, *find_cur; char find_pat[96]; bool signaled, manual; int32_t count, max; int owner, recursion; } w32_handle_t;
/* waiting (kernel32.c): is the object signalled? an auto-reset event is reset by a successful wait */
bool w32_handle_signaled(w32_proc_t *p, uintptr_t h, bool consume);
void w32_event_signal(w32_proc_t *p, uintptr_t h);              /* SetEvent for another task (Winsock's poller) */

typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; int32_t ptx, pty; } MSG32;
#define MSGQ 256

typedef struct alloc_hdr { struct alloc_hdr *next, *prev; uint32_t size, magic; } alloc_hdr_t;

struct w32_proc {
    int pid;
    task_t *task;
    char path[256], cmdline[512], cwd[256];
    uintptr_t base;             /* where the image is mapped in the process */
    uint32_t size, entry;
    uintptr_t space;            /* CR3 of the process address space */
    uint32_t *pagedir;          /* x86 */
    uint32_t *ptabs[40]; int nptabs;  /* x86: page tables for the image window and DLLs */
    uint8_t *frames;
    uint8_t *teb, *peb;
    uintptr_t tls_index_addr; uint8_t *tls_block; uintptr_t *tls_array;
    uintptr_t tls_callbacks;
    console_t *con, *in;
    bool own_console;
    bool gui;
    alloc_hdr_t heap;
    uint32_t heap_bytes;
    void (*atexit_fns[64])(void); int natexit;
    w32_handle_t handles[MAX_HANDLES];
    uint32_t last_error;
    MSG32 q[MSGQ]; int qh, qt;
    bool quit_posted; int quit_code;
    uint32_t idle_since;            /* waiting in GetMessage with nothing to do since then (0: busy) */
    char stdin_buf[1024]; int stdin_len, stdin_pos;
    int argc; char **argv; char **envp; char *acmdln;
    uintptr_t tls_slots[64]; uint64_t tls_used;
    int errno_val;
    int threads;
    bool exiting;
    char title[96];
    uint32_t fmode, commode;
    uint32_t res_rva, res_size;   /* the image's resource directory */
    w32_mod_t *mods[W32_MAX_MODS]; int nmods;   /* the program, then its DLLs in load order */
    w32_mod_t *initq[W32_MAX_MODS]; int ninitq; /* DLLs whose DllMain is still to run (dependencies first) */
    uintptr_t dll_next;           /* where the next DLL goes */
    uintptr_t fdh[64]; uint8_t fdfl[64];        /* C runtime file descriptors: kernel32 handles (0-2: the console) */
    WCHAR **wargv, **wenvp; WCHAR *wcmdln;      /* wide copies, made when a program asks */
    int umask;
};

w32_proc_t *w32_cur(void);
void *w32_alloc(w32_proc_t *p, uint32_t n);
void w32_free(w32_proc_t *p, void *ptr);
void *w32_realloc(w32_proc_t *p, void *ptr, uint32_t n);
uint32_t w32_msize(void *ptr);
void w32_exit_process(int code) __attribute__((noreturn));
uint8_t *w32_new_teb(w32_proc_t *p, task_t *t);     /* TEB for a thread of p (layout per architecture) */
void w32_post(w32_proc_t *p, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
int w32_console_write(w32_proc_t *p, int fd, const char *s, int n);
int w32_stdin_getc(w32_proc_t *p);
int w32_stdin_read(w32_proc_t *p, char *buf, int n);
int w32_vformat(char *out, int cap, const char *fmt, w32_va_list ap);
int w32_vsscanf(const char *str, const char *fmt, w32_va_list ap);
bool w32_lookup(const char *dll, const char *name, void **out);
/* program DLLs (pe.c): load one by name or path (0 if there is no such file or it is
 * a built-in system DLL), find a loaded one, look up an export ("Name" or "#12"),
 * run pending DllMain(DLL_PROCESS_ATTACH) calls in the process's own task */
uintptr_t w32_load_library(w32_proc_t *p, const char *name);
w32_mod_t *w32_mod_by_base(w32_proc_t *p, uintptr_t base);
w32_mod_t *w32_mod_by_name(w32_proc_t *p, const char *name);
w32_mod_t *w32_mod_at(w32_proc_t *p, uintptr_t addr);
void *w32_mod_export(w32_proc_t *p, w32_mod_t *m, const char *name, uint32_t ord);
void w32_run_dll_inits(w32_proc_t *p);
bool w32_is_system_dll(const char *name);
uintptr_t w32_find_resource_in(w32_proc_t *p, uintptr_t hmod, uintptr_t type, uintptr_t name, bool wide, int lang);
/* resources (pe.c): type and name are MAKEINTRESOURCE ids (< 0x10000) or strings, ANSI or wide;
 * the result is the image's IMAGE_RESOURCE_DATA_ENTRY (the HRSRC), or 0 */
uintptr_t w32_find_resource(w32_proc_t *p, uintptr_t type, uintptr_t name, bool wide, int lang);
void *w32_resource_data(w32_proc_t *p, uintptr_t hrsrc, uint32_t *size);
void w32_user_cleanup(w32_proc_t *p);
vnode_t *w32_handle_node(w32_proc_t *p, uintptr_t h);         /* kernel32.c: a file handle's node, or NULL */
uint32_t w32_handle_pos(w32_proc_t *p, uintptr_t h);
w32_proc_t *pe_find(int pid);
int pe_proc_list(int *pids, int max);
bool w32_pump_timers(w32_proc_t *p);

typedef struct { const char *name; void *fn; } w32_export_t;
/* entries of the newer API files (registry.c, k32more.c, shlwapi.c...): a DLL group
 * ("kernel32", "shlwapi", ...), a name and the function. They are looked up before the
 * older tables, so a fuller implementation there replaces a stub elsewhere. */
typedef struct { const char *group, *name; void *fn; } w32_api_t;
void *w32_extra_find(const char *dll, const char *name);
void w32_dll_group(const char *dll, char *g, int cap);

/* calling into the program */
#ifdef __x86_64__
typedef LRESULT (WINAPI *w32_wndproc_t)(HWND, UINT, WPARAM, LPARAM);
static inline LRESULT call_wndproc(void *fn, HWND h, UINT m, WPARAM w, LPARAM l) { return ((w32_wndproc_t)fn)(h, m, w, l); }
static inline uintptr_t call_pe_entry(void *fn, uintptr_t unused) { (void)unused; return ((uintptr_t (WINAPI *)(void))fn)(); }
static inline uintptr_t call_tls_cb(void *fn, uintptr_t a, uintptr_t b, uintptr_t c) { return ((uintptr_t (WINAPI *)(uintptr_t, uintptr_t, uintptr_t))fn)(a, b, c); }
static inline uint32_t call_thread_proc(void *fn, void *arg) { return ((uint32_t (WINAPI *)(void *))fn)(arg); }
static inline void call_void_proc(void *fn) { ((void (WINAPI *)(void))fn)(); }
#else
/* assembly helpers (src/arch/i686/isr.asm) that survive stdcall/cdecl mix-ups */
extern uint32_t call_wndproc(void *fn, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
extern uint32_t call_pe_entry(void *fn, uint32_t unused);
extern uint32_t call_tls_cb(void *fn, uint32_t a, uint32_t b, uint32_t c);
static inline uint32_t call_thread_proc(void *fn, void *arg) { return call_tls_cb(fn, (uint32_t)arg, 0, 0); }
static inline void call_void_proc(void *fn) { call_pe_entry(fn, 0); }
#endif
extern const w32_export_t kernel32_exports[], msvcrt_exports[], user32_exports[], gdi32_exports[], advapi32_exports[], shell32_exports[];
extern const int kernel32_count, msvcrt_count, user32_count, gdi32_count, advapi32_count, shell32_count;
/* graphics APIs on the WRLD 3D engine: gl.c (OpenGL 1.1, GLU), d3d9.c (Direct3D 9), vulkan.c */
extern const w32_export_t opengl32_exports[], glu32_exports[], d3d9_exports[], d3dx9_exports[], vulkan_exports[], d3d8_exports[];
extern const int opengl32_count, glu32_count, d3d9_count, d3dx9_count, vulkan_count, d3d8_count;
/* d3d10.c: Direct3D 10, DXGI and the (absent) run-time HLSL compiler */
extern const w32_export_t d3d10_exports[], dxgi_exports[], d3dcompiler_exports[];
extern const int d3d10_count, dxgi_count, d3dcompiler_count;
#endif
