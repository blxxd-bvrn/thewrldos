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
#define W32_MACHINE 0x8664
#else
#define WINAPI __attribute__((stdcall))
#define CDECL __attribute__((cdecl))
typedef va_list w32_va_list;
#define w32_va_start(ap, last) va_start(ap, last)
#define w32_va_end(ap) va_end(ap)
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

#define MAX_HANDLES 128
enum { H_NONE, H_FILE, H_THREAD, H_FIND, H_EVENT, H_PROCESS };
typedef struct { int kind; vnode_t *node; uint32_t pos; bool write; int tid; vnode_t *find_dir, *find_cur; char find_pat[96]; bool signaled; } w32_handle_t;

typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; int32_t ptx, pty; } MSG32;
#define MSGQ 256

typedef struct alloc_hdr { struct alloc_hdr *next, *prev; uint32_t size, magic; } alloc_hdr_t;

typedef struct w32_proc {
    int pid;
    task_t *task;
    char path[256], cmdline[512], cwd[256];
    uintptr_t base;             /* where the image is mapped in the process */
    uint32_t size, entry;
    uintptr_t space;            /* CR3 of the process address space */
    uint32_t *pagedir;          /* x86 */
    uint32_t *ptabs[4]; int nptabs;   /* x86: page tables for the image window */
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
    char stdin_buf[1024]; int stdin_len, stdin_pos;
    int argc; char **argv; char **envp; char *acmdln;
    uintptr_t tls_slots[64]; uint64_t tls_used;
    int errno_val;
    int threads;
    bool exiting;
    char title[96];
    uint32_t fmode, commode;
} w32_proc_t;

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
void w32_user_cleanup(w32_proc_t *p);
bool w32_pump_timers(w32_proc_t *p);

typedef struct { const char *name; void *fn; } w32_export_t;

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
#endif
