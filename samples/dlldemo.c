/* dlldemo.exe: uses mathlib.dll through an import and through LoadLibrary/GetProcAddress */
#include <windows.h>
#include <stdio.h>
__declspec(dllimport) int mathlib_add(int a, int b);
__declspec(dllimport) const char *mathlib_greeting(void);
__declspec(dllimport) int mathlib_attach_count(void);
int main(void) {
    printf("Imported:  mathlib_add(2, 40) = %d\n", mathlib_add(2, 40));
    printf("Imported:  greeting = \"%s\", DllMain ran %d time(s)\n", mathlib_greeting(), mathlib_attach_count());
    HMODULE h = LoadLibraryA("mathlib.dll");
    char path[MAX_PATH] = "";
    GetModuleFileNameA(h, path, sizeof path);
    printf("LoadLibrary(\"mathlib.dll\") = %p (%s)\n", (void *)h, path);
    long long (*fact)(int) = (long long (*)(int))GetProcAddress(h, "mathlib_factorial");
    if (fact) printf("GetProcAddress: mathlib_factorial(20) = %lld\n", fact(20));
    else printf("GetProcAddress failed (%lu)\n", GetLastError());
    printf("GetProcAddress(\"no_such_function\") = %p\n", (void *)GetProcAddress(h, "no_such_function"));
    HMODULE none = LoadLibraryA("missing.dll");
    DWORD err = GetLastError();
    printf("LoadLibrary(\"missing.dll\") = %p, error %lu\n", (void *)none, err);
    FreeLibrary(h);
    return 0;
}
