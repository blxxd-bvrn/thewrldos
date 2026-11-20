/* A test program for rust/src/pe.rs: imports by name and by ordinal, a TLS variable and callback. */
#include <windows.h>
#include <stdio.h>
__declspec(dllimport) int add(int, int);
int mul(int, int);                                           /* imported by ordinal through testdll_ord.def */
__declspec(dllimport) const char *greeting(void);
__thread int per_thread = 5;
static void NTAPI tls_cb(PVOID h, DWORD why, PVOID r) { per_thread++; }
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK tls_cb_ptr = tls_cb;
static const char *table[] = { "pointers", "the", "loader", "relocates" };   /* absolute addresses: relocations */
int main(void) {
    Sleep(0);
    printf("%d %d %s %s %u\n", add(2, 3), mul(4, 5), greeting(), table[per_thread % 4], (unsigned)GetTickCount());
    return 0;
}
