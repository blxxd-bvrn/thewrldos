/* Exercises the registry functions (advapi32 and shlwapi) the way programs call them and
 * reports each result with OutputDebugString, which THE WRLD OS writes to its serial log;
 * run it on two builds and compare the "RT " lines. Build: tools/regtest/build.sh. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

static void say(const char *fmt, ...) {
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    char line[620]; snprintf(line, sizeof line, "RT %s", b);
    OutputDebugStringA(line); printf("%s\n", line);
}
static const char *hexs(const void *p, DWORD n) {
    static char b[400]; const BYTE *d = p; int k = 0;
    if (n > 150) n = 150;
    for (DWORD i = 0; i < n; i++) k += snprintf(b + k, sizeof b - k, "%02x", d[i]);
    if (!n) strcpy(b, "-");
    return b;
}
#define T "Software\\RegTest"

int main(void) {
    HKEY k = 0, k2 = 0; DWORD disp = 9, t, n, cc; LONG e; BYTE buf[256]; char name[64]; WCHAR wname[64];
    SHDeleteKeyA(HKEY_CURRENT_USER, T);                       /* start from scratch */

    e = RegCreateKeyExA(HKEY_CURRENT_USER, T, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    say("create %ld disp %lu handle-ok %d", e, disp, k != 0);
    e = RegCreateKeyExA(HKEY_CURRENT_USER, T, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k2, &disp);
    say("create again %ld disp %lu", e, disp);
    RegCloseKey(k2);

    /* values of each kind */
    say("set sz %ld", RegSetValueExA(k, "Str", 0, REG_SZ, (const BYTE *)"hello", 6));
    say("set sz unterminated %ld", RegSetValueExA(k, "Raw", 0, REG_SZ, (const BYTE *)"abc", 3));
    DWORD dw = 0x12345678; say("set dword %ld", RegSetValueExA(k, "Num", 0, REG_DWORD, (const BYTE *)&dw, 4));
    ULONGLONG qw = 0x1122334455667788ull; say("set qword %ld", RegSetValueExA(k, "Big", 0, REG_QWORD, (const BYTE *)&qw, 8));
    say("set binary %ld", RegSetValueExA(k, "Bin", 0, REG_BINARY, (const BYTE *)"\x01\x00\xff", 3));
    say("set multi %ld", RegSetValueExA(k, "Multi", 0, REG_MULTI_SZ, (const BYTE *)"one\0two\0", 9));
    say("set wide %ld", RegSetValueExW(k, L"Wide", 0, REG_SZ, (const BYTE *)L"w\x00e9\x4e2d", 6));    /* not terminated */
    say("set expand %ld", RegSetValueExW(k, L"Path", 0, REG_EXPAND_SZ, (const BYTE *)L"%TEMP%\\x", 18));
    say("set default %ld", RegSetValueExA(k, NULL, 0, REG_SZ, (const BYTE *)"dflt", 5));

    /* reading back: sizes, short buffers, conversions */
    const char *names[] = { "Str", "Raw", "Num", "Big", "Bin", "Multi", "Wide", "Path", "", "Missing" };
    for (int i = 0; i < 10; i++) {
        n = 0; t = 99; e = RegQueryValueExA(k, names[i], NULL, &t, NULL, &n);
        say("size A %s: %ld type %lu size %lu", names[i], e, t, n);
        n = 2; e = RegQueryValueExA(k, names[i], NULL, &t, buf, &n);
        say("short A %s: %ld size %lu", names[i], e, n);
        n = sizeof buf; memset(buf, 0xcc, sizeof buf); e = RegQueryValueExA(k, names[i], NULL, &t, buf, &n);
        say("read A %s: %ld size %lu %s", names[i], e, n, e ? "" : hexs(buf, n));
        WCHAR wn[32]; MultiByteToWideChar(CP_ACP, 0, names[i], -1, wn, 32);
        n = sizeof buf; memset(buf, 0xcc, sizeof buf); e = RegQueryValueExW(k, wn, NULL, &t, buf, &n);
        say("read W %s: %ld size %lu %s", names[i], e, n, e ? "" : hexs(buf, n));
    }
    say("data without size %ld", RegQueryValueExA(k, "Str", NULL, NULL, buf, NULL));
    say("case-insensitive %ld", RegQueryValueExA(k, "sTR", NULL, NULL, NULL, &n));

    /* RegGetValue with type filters, from the parent with a subkey path */
    n = 4; e = RegGetValueA(HKEY_CURRENT_USER, T, "Num", RRF_RT_REG_DWORD, &t, buf, &n); say("getvalue dword %ld %lu %s", e, n, hexs(buf, n));
    n = sizeof buf; e = RegGetValueA(HKEY_CURRENT_USER, T, "Num", RRF_RT_REG_SZ, &t, buf, &n); say("getvalue wrong type %ld type %lu", e, t);
    n = sizeof buf; e = RegGetValueW(HKEY_CURRENT_USER, L"Software\\RegTest", L"Str", RRF_RT_ANY, &t, buf, &n); say("getvalue W %ld %lu %s", e, n, hexs(buf, n));
    n = sizeof buf; e = RegGetValueA(HKEY_CURRENT_USER, T "\\Nope", "Num", RRF_RT_ANY, &t, buf, &n); say("getvalue no key %ld", e);

    /* subkeys: order, enumeration, short buffers, info */
    const char *subs[] = { "beta", "Alpha", "gamma", "ALPHA", "delta\\deep\\deeper" };
    for (int i = 0; i < 5; i++) { e = RegCreateKeyA(k, subs[i], &k2); say("subkey %s %ld", subs[i], e); RegCloseKey(k2); }
    for (DWORD i = 0;; i++) {
        cc = sizeof name; e = RegEnumKeyExA(k, i, name, &cc, NULL, NULL, NULL, NULL);
        say("enumkey A %lu: %ld %lu %s", i, e, cc, e ? "" : name);
        if (e) break;
        cc = 64; e = RegEnumKeyExW(k, i, wname, &cc, NULL, NULL, NULL, NULL);
        say("enumkey W %lu: %ld %lu %s", i, e, cc, e ? "" : hexs(wname, (cc + 1) * 2));
    }
    cc = 3; e = RegEnumKeyExA(k, 0, name, &cc, NULL, NULL, NULL, NULL); say("enumkey short %ld %lu", e, cc);
    e = RegEnumKeyA(k, 1, name, sizeof name); say("enumkey old %ld %s", e, e ? "" : name);
    for (DWORD i = 0;; i++) {
        cc = sizeof name; n = sizeof buf; t = 99; e = RegEnumValueA(k, i, name, &cc, NULL, &t, buf, &n);
        say("enumvalue A %lu: %ld %lu '%s' type %lu size %lu %s", i, e, cc, e ? "" : name, t, n, e ? "" : hexs(buf, n));
        if (e) break;
        cc = 64; n = 2; e = RegEnumValueW(k, i, wname, &cc, NULL, &t, buf, &n);
        say("enumvalue W short %lu: %ld %lu size %lu", i, e, cc, n);
    }
    DWORD ns, ms, nv, mn, md; FILETIME ft;
    e = RegQueryInfoKeyA(k, NULL, NULL, NULL, &ns, &ms, NULL, &nv, &mn, &md, NULL, &ft);
    say("info A %ld subkeys %lu maxsub %lu values %lu maxname %lu maxdata %lu ft %08lx%08lx", e, ns, ms, nv, mn, md, ft.dwHighDateTime, ft.dwLowDateTime);
    e = RegQueryInfoKeyW(k, NULL, NULL, NULL, &ns, NULL, NULL, &nv, NULL, NULL, NULL, NULL); say("info W %ld %lu %lu", e, ns, nv);

    /* the old default-value functions */
    say("setvalue A %ld", RegSetValueA(k, "beta", REG_SZ, "bdef", 4));
    LONG ln = sizeof name; e = RegQueryValueA(k, "beta", name, &ln); say("queryvalue A %ld %ld %s", e, ln, e ? "" : name);
    ln = sizeof name; e = RegQueryValueA(k, "gamma", name, &ln); say("queryvalue empty %ld %ld '%s'", e, ln, name);
    say("setvalue W %ld", RegSetValueW(k, L"gamma", REG_SZ, L"g", 2));
    ln = sizeof wname; e = RegQueryValueW(k, L"gamma", wname, &ln); say("queryvalue W %ld %ld %s", e, ln, hexs(wname, ln));
    ln = 0; e = RegQueryValueA(k, "beta", NULL, &ln); say("queryvalue size %ld %ld", e, ln);

    /* wide paths with characters ANSI cannot hold */
    e = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\RegTest\\\x4e2d\x6587", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k2, &disp);
    say("wide key %ld disp %lu", e, disp); RegCloseKey(k2);
    e = RegOpenKeyExA(HKEY_CURRENT_USER, T "\\??", 0, KEY_READ, &k2); say("open as ?? %ld", e); RegCloseKey(k2);

    /* HKEY_CLASSES_ROOT is HKLM\SOFTWARE\Classes */
    e = RegCreateKeyA(HKEY_CLASSES_ROOT, ".rtx", &k2); say("hkcr create %ld", e);
    say("hkcr set %ld", RegSetValueExA(k2, NULL, 0, REG_SZ, (const BYTE *)"rtxfile", 8)); RegCloseKey(k2);
    n = sizeof buf; e = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Classes\\.rtx", NULL, RRF_RT_ANY, &t, buf, &n); say("via hklm %ld %s", e, e ? "" : (char *)buf);
    say("hkcr delete %ld", RegDeleteKeyA(HKEY_CLASSES_ROOT, ".rtx"));

    /* what programs look up */
    n = sizeof buf; e = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, &t, buf, &n);
    say("productname %ld %s", e, e ? "" : (char *)buf);
    n = sizeof buf; e = RegGetValueA(HKEY_CURRENT_USER, "Control Panel\\International", "sShortDate", RRF_RT_ANY, &t, buf, &n); say("shortdate %ld %s", e, e ? "" : (char *)buf);

    /* shlwapi */
    say("shset A %ld", SHSetValueA(HKEY_CURRENT_USER, T "\\sh", "v", REG_DWORD, &dw, 4));
    say("shset W %ld", SHSetValueW(HKEY_CURRENT_USER, L"Software\\RegTest\\sh", L"w", REG_SZ, L"x", 4));
    n = sizeof buf; e = SHGetValueA(HKEY_CURRENT_USER, T "\\sh", "v", &t, buf, &n); say("shget A %ld %lu %s", e, t, hexs(buf, n));
    n = sizeof buf; e = SHGetValueW(HKEY_CURRENT_USER, L"Software\\RegTest\\sh", L"w", &t, buf, &n); say("shget W %ld %lu %s", e, t, hexs(buf, n));
    n = sizeof buf; e = SHQueryValueExA(k, "Num", NULL, &t, buf, &n); say("shquery %ld %s", e, hexs(buf, n));
    cc = sizeof name; e = SHEnumKeyExA(k, 0, name, &cc); say("shenum %ld %s", e, e ? "" : name);
    say("shdelvalue %ld", SHDeleteValueA(HKEY_CURRENT_USER, T "\\sh", "v"));
    say("shdelvalue again %ld", SHDeleteValueA(HKEY_CURRENT_USER, T "\\sh", "v"));

    /* deleting */
    say("delvalue A %ld", RegDeleteValueA(k, "Raw"));
    say("delvalue W %ld", RegDeleteValueW(k, L"Raw"));
    e = RegOpenKeyA(k, "delta\\deep", &k2); say("open deep %ld", e);
    say("delkey with subkeys %ld", RegDeleteKeyA(k, "delta"));
    say("deltree %ld", RegDeleteTreeA(k, "delta"));
    say("deep handle after delete %ld", RegSetValueExA(k2, "x", 0, REG_DWORD, (const BYTE *)&dw, 4));
    say("delkey root %ld", RegDeleteKeyA(HKEY_CURRENT_USER, ""));
    say("deltree W contents %ld", RegDeleteTreeW(k, NULL));
    e = RegQueryInfoKeyA(k, NULL, NULL, NULL, &ns, NULL, NULL, &nv, NULL, NULL, NULL, NULL); say("after contents %ld %lu %lu", e, ns, nv);

    /* handles */
    say("close %ld", RegCloseKey(k));
    say("closed handle %ld", RegSetValueExA(k, "x", 0, REG_DWORD, (const BYTE *)&dw, 4));
    say("bad handle %ld", RegOpenKeyExA((HKEY)0x1234, "x", 0, KEY_READ, &k2));
    say("null result %ld", RegOpenKeyExA(HKEY_CURRENT_USER, "Software", 0, KEY_READ, NULL));
    say("missing %ld", RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\NoSuchKey", 0, KEY_READ, &k2));
    e = RegOpenCurrentUser(KEY_READ, &k2); say("current user %ld %d", e, k2 == HKEY_CURRENT_USER);
    say("connect local %ld remote %ld", RegConnectRegistryA(NULL, HKEY_LOCAL_MACHINE, &k2), RegConnectRegistryA("\\\\far", HKEY_LOCAL_MACHINE, &k2));
    HKEY many[300]; int ok = 0;
    for (int i = 0; i < 300; i++) ok += RegOpenKeyExA(HKEY_CURRENT_USER, "Software", 0, KEY_READ, &many[i]) == 0;
    say("300 opens %d last-works %ld", ok, RegQueryInfoKeyA(many[299], NULL, NULL, NULL, &ns, NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    for (int i = 0; i < 300; i++) RegCloseKey(many[i]);

    /* leave something behind, and show what registry.dat holds for it */
    e = RegCreateKeyA(HKEY_CURRENT_USER, T "\\Saved %stuff", &k); RegSetValueExA(k, "a b%", 0, REG_SZ, (const BYTE *)"v", 2); RegCloseKey(k);
    FILE *f = fopen("C:\\WRLD\\registry.dat", "rb");
    char line[700];
    if (f) { while (fgets(line, sizeof line, f)) if (strstr(line, "RegTest")) { line[strcspn(line, "\r\n")] = 0; say("file %s", line); } fclose(f); }
    else say("no registry.dat");
    say("done");
    return 0;
}
