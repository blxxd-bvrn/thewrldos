#ifndef W32_H
#define W32_H
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"

#define WINAPI __attribute__((stdcall))
typedef uint32_t DWORD, UINT, HANDLE, HWND, HDC, HGDIOBJ, HINSTANCE, HMODULE, WPARAM, LPARAM, LRESULT, ATOM, COLORREF;
typedef int BOOL, LONG, INT;
typedef uint16_t WORD, WCHAR;
typedef uint8_t BYTE;

#define MAX_HANDLES 128
enum { H_NONE, H_FILE, H_THREAD, H_FIND, H_EVENT, H_PROCESS };
typedef struct { int kind; vnode_t *node; uint32_t pos; bool write; int tid; vnode_t *find_dir, *find_cur; char find_pat[96]; bool signaled; } w32_handle_t;

typedef struct { uint32_t hwnd, message, wParam, lParam, time; int32_t ptx, pty; } MSG32;
#define MSGQ 256

typedef struct alloc_hdr { struct alloc_hdr *next, *prev; uint32_t size, magic; } alloc_hdr_t;

typedef struct w32_proc {
    int pid;
    task_t *task;
    char path[256], cmdline[512], cwd[256];
    uint32_t base, size, entry;
    uint32_t *pagedir;
    uint8_t *frames;
    uint8_t *teb, *peb;
    uint32_t tls_index_addr; uint8_t *tls_block; uint32_t *tls_array;
    uint32_t tls_callbacks;
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
    uint32_t tls_slots[64]; uint64_t tls_used;
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
void w32_post(w32_proc_t *p, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
int w32_console_write(w32_proc_t *p, int fd, const char *s, int n);
int w32_stdin_getc(w32_proc_t *p);
int w32_stdin_read(w32_proc_t *p, char *buf, int n);
int w32_vformat(char *out, int cap, const char *fmt, va_list ap);
int w32_vsscanf(const char *str, const char *fmt, va_list ap);
bool w32_lookup(const char *dll, const char *name, void **out);
void w32_user_cleanup(w32_proc_t *p);
bool w32_pump_timers(w32_proc_t *p);

typedef struct { const char *name; void *fn; } w32_export_t;
extern const w32_export_t kernel32_exports[], msvcrt_exports[], user32_exports[], gdi32_exports[], advapi32_exports[], shell32_exports[];
extern const int kernel32_count, msvcrt_count, user32_count, gdi32_count, advapi32_count, shell32_count;
#endif
