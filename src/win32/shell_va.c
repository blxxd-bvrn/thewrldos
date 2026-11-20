/* shlwapi's printf functions: C-variadic, which Rust (stable) cannot define, so they stay in
 * C; the rest of shlwapi is rust/src/kernel/w32shell.rs. */
#include "w32.h"

static int WINAPI wvnsprintfW(WCHAR *b, int n, const WCHAR *fmt, w32_va_list ap) {
    void *fn = NULL; w32_lookup("msvcrt.dll", "_vsnwprintf", &fn);
    int r = ((int (CDECL *)(WCHAR *, size_t, const WCHAR *, w32_va_list))fn)(b, n, fmt, ap);
    if (r < 0 && n > 0) { b[n - 1] = 0; r = n - 1; }
    return r;
}
static int CDECL wnsprintfW(WCHAR *b, int n, const WCHAR *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = wvnsprintfW(b, n, fmt, ap); w32_va_end(ap); return r; }
static int WINAPI wvnsprintfA(char *b, int n, const char *fmt, w32_va_list ap) { int r = w32_vformat(b, n, fmt, ap); return r >= n ? n - 1 : r; }
static int CDECL wnsprintfA(char *b, int n, const char *fmt, ...) { w32_va_list ap; w32_va_start(ap, fmt); int r = wvnsprintfA(b, n, fmt, ap); w32_va_end(ap); return r; }

#define G(n) { "shlwapi", #n, (void *)n }
const w32_api_t shellva_api[] = { G(wvnsprintfW), G(wnsprintfW), G(wvnsprintfA), G(wnsprintfA), { NULL, NULL, NULL } };
