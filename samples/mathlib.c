/* mathlib.dll: a small DLL for dlldemo.exe, to show THE WRLD OS loading a program's own DLLs */
#include <windows.h>
static int attached;
static char greeting[64] = "not initialised";
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    if (why == DLL_PROCESS_ATTACH) { attached++; lstrcpyA(greeting, "Hello from mathlib.dll"); }
    return TRUE;
}
__declspec(dllexport) int mathlib_add(int a, int b) { return a + b; }
__declspec(dllexport) long long mathlib_factorial(int n) { long long r = 1; while (n > 1) r *= n--; return r; }
__declspec(dllexport) const char *mathlib_greeting(void) { return greeting; }
__declspec(dllexport) int mathlib_attach_count(void) { return attached; }
