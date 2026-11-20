/* A test DLL for rust/src/pe.rs: named, ordinal-only and forwarded exports, data, resources. */
#include <windows.h>
__declspec(dllexport) int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }                     /* exported by ordinal only (testdll.def) */
__declspec(dllexport) int counter = 42;
__declspec(dllexport) const char *greeting(void) { return "hello from testdll"; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) { return TRUE; }
