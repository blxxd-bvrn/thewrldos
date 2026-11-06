/* Console Win32 program built with mingw-w64; runs unmodified on THE WRLD OS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char **argv) {
    char name[64];
    SYSTEMTIME st;
    MEMORYSTATUS ms;
    printf("Hello from a real Win32 PE executable!\n");
    printf("argc = %d, argv[0] = %s\n", argc, argv[0]);
    GetLocalTime(&st);
    printf("Local time: %04d-%02d-%02d %02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    printf("Memory: %lu KB total, %lu KB free\n", (unsigned long)(ms.dwTotalPhys / 1024), (unsigned long)(ms.dwAvailPhys / 1024));
    printf("Tick count: %lu ms since boot\n", (unsigned long)GetTickCount());

    char *buf = malloc(1000);
    strcpy(buf, "heap allocation works");
    printf("malloc/free: %s\n", buf);
    free(buf);

    HANDLE f = CreateFileA("C:\\tmp\\from_exe.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w; const char *msg = "This file was written by hello.exe via CreateFileA/WriteFile.\r\n";
        WriteFile(f, msg, (DWORD)strlen(msg), &w, NULL);
        CloseHandle(f);
        printf("Wrote C:\\tmp\\from_exe.txt (%lu bytes)\n", (unsigned long)w);
    }

    printf("\nWhat is your name? ");
    fflush(stdout);
    if (fgets(name, sizeof name, stdin)) {
        name[strcspn(name, "\r\n")] = 0;
        printf("Nice to meet you, %s!\n", name[0] ? name : "stranger");
    }
    int sum = 0;
    for (int i = 1; i <= 100; i++) sum += i;
    printf("Sum 1..100 = %d, 7/3 = %.3f\n", sum, 7.0 / 3.0);
    return 0;
}
