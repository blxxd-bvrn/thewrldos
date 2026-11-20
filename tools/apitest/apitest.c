/* Calls the functions of the smaller Win32 API files (ole32/oleaut32 and the other "misc"
 * DLLs, more of kernel32, shlwapi/shell32/version/winmm) and reports each result with
 * OutputDebugString, which THE WRLD OS writes to its serial log; run it on two builds and
 * compare the "AT " lines. Build: tools/apitest/build.sh. */
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <winspool.h>
#include <stdio.h>
#include <string.h>

static void say(const char *fmt, ...) {
    char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    char line[720]; snprintf(line, sizeof line, "AT %s", b);
    OutputDebugStringA(line); printf("%s\n", line);
}
static const char *hx(const void *p, int n) {
    static char b[600]; const BYTE *d = p; int k = 0;
    if (n > 250) n = 250;
    for (int i = 0; i < n; i++) k += sprintf(b + k, "%02x", d[i]);
    if (!n) strcpy(b, "-");
    return b;
}
static const char *w2s(const WCHAR *w) { static char b[600]; int i = 0; if (!w) return "(null)"; for (; w[i] && i < 590; i++) b[i] = w[i] < 128 ? (char)w[i] : '?'; b[i] = 0; return b; }

static void misc(void) {
    GUID g; WCHAR w[64]; LPOLESTR s;
    HRESULT h = CLSIDFromString(L"{6B29FC40-CA47-1067-B31D-00DD010662DA}", &g);
    say("clsid %08lx %s", h, hx(&g, 16));
    say("clsid bad %08lx", CLSIDFromString(L"{6B29FC40-CA47", &g));
    { int r2 = StringFromGUID2(&g, w, 64); say("guid2 %d %s", r2, w2s(w)); }
    say("guid2 short %d", StringFromGUID2(&g, w, 38));
    h = StringFromCLSID(&g, &s); say("fromclsid %08lx %s", h, w2s(s));
    GUID r1, r2; CoCreateGuid(&r1); CoCreateGuid(&r2);
    say("cocreate version %x variant %x differ %d", r1.Data3 >> 12, r1.Data4[0] >> 6, !IsEqualGUID(&r1, &r2));
    SAFEARRAYBOUND b[2] = { { 3, 1 }, { 4, 0 } };
    SAFEARRAY *a = SafeArrayCreate(VT_I4, 2, b);
    LONG lb, ub; SafeArrayGetLBound(a, 1, &lb); SafeArrayGetUBound(a, 1, &ub);
    say("sa dims %u elem %u d1 %ld..%ld", SafeArrayGetDim(a), SafeArrayGetElemsize(a), lb, ub);
    SafeArrayGetLBound(a, 2, &lb); SafeArrayGetUBound(a, 2, &ub); { LONG l0 = lb, u0 = ub; HRESULT hb = SafeArrayGetLBound(a, 3, &lb); say("sa d2 %ld..%ld bad %08lx", l0, u0, hb); }
    for (LONG i = 1; i <= 3; i++) for (LONG j = 0; j < 4; j++) { LONG idx[2] = { i, j }, v = i * 10 + j; SafeArrayPutElement(a, idx, &v); }
    void *data; SafeArrayAccessData(a, &data); say("sa data %s", hx(data, 48)); say("sa destroy locked %08lx", SafeArrayDestroy(a)); SafeArrayUnaccessData(a);
    LONG bad[2] = { 0, 0 }, v; say("sa bad index %08lx", SafeArrayGetElement(a, bad, &v));
    SAFEARRAY *c; SafeArrayCopy(a, &c); LONG idx[2] = { 3, 2 }; SafeArrayGetElement(c, idx, &v); say("sa copy elem %ld", v);
    { HRESULT d1 = SafeArrayDestroy(a), d2 = SafeArrayDestroy(c); say("sa destroy %08lx %08lx", d1, d2); }
    SAFEARRAY *bs = SafeArrayCreateVector(VT_BSTR, 0, 2); LONG i0 = 1; SafeArrayPutElement(bs, &i0, L"hello"); BSTR got; SafeArrayGetElement(bs, &i0, &got); say("sa bstr %s", w2s(got));
    VARIANT va, vb; VariantInit(&va); VariantInit(&vb); va.vt = VT_I4; va.lVal = -7;
    h = VariantChangeType(&vb, &va, 0, VT_R8); say("vct r8 %08lx vt %d %.1f", h, vb.vt, vb.dblVal);
    h = VariantChangeType(&vb, &va, 0, VT_BOOL); say("vct bool %08lx vt %d %d", h, vb.vt, vb.boolVal);
    h = VariantChangeType(&vb, &va, 0, VT_BSTR); say("vct bstr %08lx vt %d", h, vb.vt);
    SYSTEMTIME st = { 2024, 2, 0, 29, 13, 30, 15, 0 }; double d; SystemTimeToVariantTime(&st, &d); say("vtime %.6f", d);
    int r = IS_TEXT_UNICODE_STATISTICS; { BOOL t1 = IsTextUnicode(L"plain unicode text here", 46, &r); say("textunicode %d %d", t1, r); }
    r = IS_TEXT_UNICODE_STATISTICS; { BOOL t2 = IsTextUnicode("plain ansi text here.......", 26, &r); say("textansi %d %d", t2, r); }
    WCHAR o[64]; { int i1 = IdnToAscii(0, L"example.com", -1, o, 64); say("idn %d %s", i1, w2s(o)); int i2 = IdnToAscii(0, L"caf\x00e9.com", -1, o, 64); say("idn bad %d err %lu", i2, GetLastError()); }
    DWORD n = 0; { BOOL g1 = GetProfilesDirectoryW(NULL, &n); say("profiles %d need %lu", g1, n); } n = 64; GetUserProfileDirectoryW(NULL, o, &n); say("userprofile %s %lu", w2s(o), n);
    { BOOL p1 = OpenPrinterA("x", (HANDLE *)&n, NULL); say("printer %d err %lu", p1, GetLastError()); }
    say("bstrcmp %ld %ld", VarBstrCmp(L"abc", L"abd", 0, 0), VarBstrCmp(L"b", L"a", 0, 0));
}

static void k32(void) {
    char b[600]; WCHAR w[300]; DWORD n;
    SetEnvironmentVariableA("APITEST", "value one");
    say("env %lu %s", GetEnvironmentVariableA("apitest", b, sizeof b), b);
    say("env small %lu", GetEnvironmentVariableA("APITEST", b, 3));
    n = GetEnvironmentVariableW(L"APITEST", w, 300); say("env W %lu %s", n, w2s(w));
    SetEnvironmentVariableA("APITEST", NULL); n = GetEnvironmentVariableA("APITEST", b, sizeof b); say("env gone %lu err %lu", n, GetLastError());
    { BOOL e3 = SetEnvironmentVariableA("A=B", "x"); say("env bad %d err %lu", e3, GetLastError()); }
    SetEnvironmentVariableA("APIDIR", "C:\\WRLD");
    say("expand %lu %s", ExpandEnvironmentStringsA("%APIDIR%\\x %NOPE% 50%", b, sizeof b), b);
    n = ExpandEnvironmentStringsW(L"[%apidir%]", w, 300); say("expand W %lu %s", n, w2s(w));
    char *blk = GetEnvironmentStringsA(); int found = 0; for (char *p = blk; *p; p += strlen(p) + 1) if (!strncmp(p, "APIDIR=", 7)) found = 1; say("envblock has %d", found); FreeEnvironmentStringsA(blk);
    OSVERSIONINFOEXA vi = { sizeof vi }; GetVersionExA((OSVERSIONINFOA *)&vi); say("version %lu.%lu.%lu %s sp %u suite %x type %u", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, vi.szCSDVersion, vi.wServicePackMajor, vi.wSuiteMask, vi.wProductType);
    OSVERSIONINFOA vs = { 10 }; { BOOL v1 = GetVersionExA(&vs); say("version small %d err %lu", v1, GetLastError()); }
    ULONGLONG m = VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL), VER_MINORVERSION, VER_GREATER_EQUAL);
    OSVERSIONINFOEXW want = { sizeof want }; want.dwMajorVersion = 6; want.dwMinorVersion = 2;
    { BOOL v2 = VerifyVersionInfoW(&want, VER_MAJORVERSION | VER_MINORVERSION, m); say("verify 6.2 %d err %lu mask %llx", v2, GetLastError(), m); }
    want.dwMinorVersion = 0; say("verify 6.0 %d", VerifyVersionInfoW(&want, VER_MAJORVERSION | VER_MINORVERSION, m));
    say("getversion %08lx", GetVersion());
    /* INI files */
    const char *ini = "C:\\apitest.ini";
    DeleteFileA(ini);
    WritePrivateProfileStringA("Main", "Name", "First value", ini);
    WritePrivateProfileStringA("Main", "Count", "0x2A", ini);
    WritePrivateProfileStringA("Other", "key", "x", ini);
    WritePrivateProfileStringW(L"Main", L"Wide", L"w value", L"C:\\apitest.ini");
    say("ini get %lu %s", GetPrivateProfileStringA("main", "NAME", "def", b, sizeof b, ini), b);
    say("ini def %lu %s", GetPrivateProfileStringA("Main", "nope", "def  ", b, sizeof b, ini), b);
    say("ini int %u %u", GetPrivateProfileIntA("Main", "Count", 5, ini), GetPrivateProfileIntA("Main", "nope", 5, ini));
    n = GetPrivateProfileStringA(NULL, NULL, NULL, b, sizeof b, ini); say("ini sections %lu %s", n, hx(b, n + 1));
    n = GetPrivateProfileStringA("Main", NULL, NULL, b, sizeof b, ini); say("ini keys %lu %s", n, hx(b, n + 1));
    n = GetPrivateProfileSectionA("Main", b, sizeof b, ini); say("ini section %lu %s", n, hx(b, n + 1));
    n = GetPrivateProfileStringW(L"Main", L"Wide", L"", w, 300, L"C:\\apitest.ini"); say("ini W %lu %s", n, w2s(w));
    n = GetPrivateProfileSectionNamesW(w, 300, L"C:\\apitest.ini"); say("ini names W %lu %s", n, hx(w, (n + 1) * 2));
    WritePrivateProfileStringA("Main", "Count", NULL, ini);
    WritePrivateProfileStringA("Other", NULL, NULL, ini);
    WritePrivateProfileSectionA("New", "a=1\0b=2\0", ini);
    BYTE st[4] = { 1, 2, 3, 0xFE }, st2[4] = { 0 }; WritePrivateProfileStructA("New", "s", st, 4, ini);
    say("ini struct %d %s", GetPrivateProfileStructA("New", "s", st2, 4, ini), hx(st2, 4));
    HANDLE f = CreateFileA(ini, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); n = 0; ReadFile(f, b, sizeof b - 1, &n, NULL); CloseHandle(f); b[n] = 0;
    say("ini file %lu %s", n, hx(b, n));
    /* atoms */
    ATOM a1 = GlobalAddAtomA("ApiTestAtom"), a2 = GlobalAddAtomA("apitestatom");
    say("atom %d %d find %d", a1 >= 0xC000, a1 == a2, GlobalFindAtomA("APITESTATOM") == a1);
    say("atom name %u %s", GlobalGetAtomNameA(a1, b, sizeof b), b);
    say("atom int %u %u %s", GlobalAddAtomA("#123"), GlobalGetAtomNameA(77, b, sizeof b), b);
    GlobalDeleteAtom(a1); GlobalDeleteAtom(a1); say("atom gone %d", GlobalFindAtomA("ApiTestAtom"));
    /* Tool Help */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPMODULE, 0);
    PROCESSENTRY32 pe = { sizeof pe }; int np = 0, self = 0; for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) { np++; if (strstr(pe.szExeFile, "apitest")) self = 1; }
    say("procs %d self %d", np > 0, self);
    MODULEENTRY32 me = { sizeof me }; BOOL ok = Module32First(snap, &me); say("module %d %s", ok, me.szModule); CloseHandle(snap);
    { BOOL s1 = Process32First((HANDLE)0x1234, &pe); say("snap bad %d err %lu", s1, GetLastError()); }
    /* paths, files, console, misc */
    char *part; n = SearchPathA(NULL, "notepad", ".exe", sizeof b, b, &part); say("searchpath %lu %s part %s", n, b, n ? part : "");
    n = SearchPathA(NULL, "nosuchfile.xyz", NULL, sizeof b, b, &part); say("searchpath none %lu err %lu", n, GetLastError());
    f = CreateFileA(ini, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    FILE_STANDARD_INFO fsi; { BOOL ok2 = GetFileInformationByHandleEx(f, FileStandardInfo, &fsi, sizeof fsi); say("fileinfo %d size %lld dir %d", ok2, fsi.EndOfFile.QuadPart, fsi.Directory); }
    n = GetFinalPathNameByHandleA(f, b, sizeof b, 0); say("finalpath %lu %s", n, b); CloseHandle(f);
    char vol[64], fs[16]; DWORD ser, ml, fl; { BOOL ok3 = GetVolumeInformationA("C:\\", vol, 64, &ser, &ml, &fl, fs, 16); say("volume %d %s %08lx %lu %lx %s", ok3, vol, ser, ml, fl, fs); }
    WORD ty[8]; GetStringTypeA(0, CT_CTYPE1, "aZ5 !\t\xe9", 7, ty); say("stringtype %s", hx(ty, 14));
    CPINFOEXW cpi; GetCPInfoExW(1252, 0, &cpi); say("cpinfo %u %u %s", cpi.MaxCharSize, cpi.CodePage, w2s(cpi.CodePageName));
    { int l1 = lstrlenW(L"hello"), l2 = lstrcmpW(L"abc", L"abd"), l3 = lstrcmpiW(L"ABC", L"abc"), l4 = lstrcmpW(NULL, L""); say("lstr %d %d %d %d", l1, l2, l3, l4); }
    lstrcpynW(w, L"truncate me", 6); say("lstrcpyn %s", w2s(w));
    INPUT_RECORD ir = { 0 }; ir.EventType = KEY_EVENT; ir.Event.KeyEvent.bKeyDown = 1; ir.Event.KeyEvent.uChar.UnicodeChar = 'q'; DWORD wr; WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &ir, 1, &wr);
    INPUT_RECORD ri; ReadConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &ri, 1, &wr); say("consoleinput %lu vk %x ch %c", wr, ri.Event.KeyEvent.wVirtualKeyCode, ri.Event.KeyEvent.uChar.UnicodeChar);
    { UINT e1 = SetErrorMode(5), e2 = SetErrorMode(0); say("errormode %u %u", e1, e2); }
    n = sizeof b; { BOOL ok5 = GetComputerNameExA(ComputerNameNetBIOS, b, &n); say("computername %d %lu", ok5, n); }
    int x = 41; SIZE_T done; { BOOL ok4 = ReadProcessMemory(GetCurrentProcess(), &x, &n, 4, &done); say("rpm %d %d", ok4, (int)n); }
    SYSTEM_POWER_STATUS ps; GetSystemPowerStatus(&ps); say("power %u %u %u", ps.ACLineStatus, ps.BatteryFlag, ps.BatteryLifePercent);
}


static void shell(void) {
    WCHAR w[600], w2[600]; char b[600]; int n;
    const WCHAR *paths[] = { L"C:\\dir\\sub\\file.txt", L"C:\\", L"\\\\server\\share\\x", L"file", L"C:\\dir\\", L"a.b\\c d.e.f", L"" };
    for (int i = 0; i < 7; i++) {
        const WCHAR *p = paths[i];
        say("path %d name %d ext %d rel %d root %d unc %d spec %d drive %d", i, (int)(PathFindFileNameW(p) - p), (int)(PathFindExtensionW(p) - p), PathIsRelativeW(p), PathIsRootW(p), PathIsUNCW(p), PathIsFileSpecW(p), PathGetDriveNumberW(p));
        lstrcpyW(w, p); BOOL r = PathRemoveFileSpecW(w); say("  removefilespec %d %s", r, w2s(w));
        lstrcpyW(w, p); PathAddBackslashW(w); say("  addbackslash %s", w2s(w));
        lstrcpyW(w, p); PathRemoveBackslashW(w); say("  removebackslash %s", w2s(w));
        lstrcpyW(w, p); PathRemoveExtensionW(w); say("  removeext %s", w2s(w));
        lstrcpyW(w, p); PathStripPathW(w); say("  strippath %s", w2s(w));
        lstrcpyW(w, p); r = PathStripToRootW(w); say("  striptoroot %d %s", r, w2s(w));
    }
    const WCHAR *canon[] = { L"C:\\a\\.\\b\\..\\c\\", L"\\\\srv\\x\\..\\y", L"a\\..\\..\\b", L"C:..\\x", L"..", L"C:\\a\\b\\..\\..\\..\\c" };
    for (int i = 0; i < 6; i++) { PathCanonicalizeW(w, canon[i]); say("canon %d %s", i, w2s(w)); }
    WCHAR *r = PathCombineW(w, L"C:\\dir", L"..\\f.txt"); say("combine %d %s", r != NULL, w2s(w));
    r = PathCombineW(w, L"C:\\dir", L"D:\\abs"); say("combine abs %s", w2s(w));
    r = PathCombineW(w, NULL, NULL); say("combine null %d", r != NULL);
    char a[300] = "C:\\base"; PathAppendA(a, "\\more\\x.txt"); say("appendA %s", a);
    n = PathCommonPrefixW(L"C:\\a\\b\\c", L"c:\\A\\b\\d", w); say("prefix %d %s", n, w2s(w));
    BOOL ok = PathRelativePathToW(w, L"C:\\a\\b\\f.txt", 0, L"C:\\a\\c\\g.txt", 0); say("relative %d %s", ok, w2s(w));
    say("match %d %d %d", PathMatchSpecW(L"C:\\x\\Photo.JPG", L"*.txt;*.jpg"), PathMatchSpecW(L"a.png", L"*.jpg"), PathMatchSpecA("abc", "a?c"));
    lstrcpyW(w, L"with space"); PathQuoteSpacesW(w); say("quote %s", w2s(w)); PathUnquoteSpacesW(w); say("unquote %s", w2s(w));
    PathCompactPathExW(w, L"C:\\Program Files\\Some Folder\\file.txt", 20, 0); say("compact %s", w2s(w));
    PathCompactPathExW(w, L"C:\\verylongfilename_without_folder_part.txt", 12, 0); say("compact2 %s", w2s(w));
    lstrcpyW(w, L"C:\\x\\file.txt"); PathRenameExtensionW(w, L".bak"); say("renameext %s", w2s(w));
    say("isurl %d %d", PathIsURLW(L"http://x"), PathIsURLW(L"C:\\x"));
    say("exists %d %d dir %d", PathFileExistsA("C:\\WRLD"), PathFileExistsA("C:\\nope"), PathIsDirectoryA("C:\\WRLD"));
    /* strings */
    say("strchr %d strrchr %d strstri %d", (int)(StrChrW(L"hello", 'l') - L"hello"), (int)(StrRChrW(L"hello", NULL, 'l') - L"hello"), StrStrIW(L"Hello World", L"WORLD") ? 1 : 0);
    say("cmp %d %d %d %d logical %d %d", StrCmpW(L"a", L"b"), StrCmpIW(L"ABC", L"abc"), StrCmpNW(L"abcx", L"abcy", 3), StrCmpNIA("ABx", "aby", 2), StrCmpLogicalW(L"file9", L"file10"), StrCmpLogicalW(L"x2y", L"X2Y"));
    lstrcpyW(w, L"  trim me\t "); StrTrimW(w, L" \t"); say("trim [%s]", w2s(w));
    int v = 0; ok = StrToIntExW(L"  -0x1f", STIF_SUPPORT_HEX, &v); say("strtointex %d %d", ok, v);
    say("strtoint %d %d", StrToIntW(L"-123abc"), StrToIntA("  42"));
    StrFormatByteSizeW(1536, w, 64); say("bytesize %s", w2s(w)); StrFormatByteSizeA(52428800, b, 64); say("bytesizeA %s", b);
    StrFormatByteSize64A(532, b, 64); say("bytesize64 %s", b); StrFormatKBSizeW(5000, w, 64); say("kbsize %s", w2s(w));
    DWORD un = 600; HRESULT h = UrlEscapeW(L"a b/c?d#e\x00e9", w, &un, 0); say("urlescape %08lx %lu %s", h, un, w2s(w));
    un = 600; h = UrlUnescapeW(L"a%20b%zz%41", w, &un, 0); say("urlunescape %08lx %lu %s", h, un, w2s(w));
    WORD hh, ll, ss; ColorRGBToHLS(0x336699, &hh, &ll, &ss); say("hls %u %u %u back %06lx", hh, ll, ss, ColorHLSToRGB(hh, ll, ss));
    n = wnsprintfW(w, 600, L"%d-%s", 42, L"wide"); say("wnsprintf %d %s", n, w2s(w));
    /* command lines */
    LPWSTR *argv = CommandLineToArgvW(L"\"C:\\My App\\x.exe\" a\\\\\"b c\" \"d\"\"e\" f\\g", &n);
    for (int i = 0; i < n; i++) say("argv %d %s", i, w2s(argv[i])); LocalFree(argv);
    argv = CommandLineToArgvW(L"", &n); say("argv empty %d %s", n, w2s(argv[0])); LocalFree(argv);
    /* folders and item ID lists */
    int csidl[] = { CSIDL_PERSONAL, CSIDL_APPDATA, CSIDL_WINDOWS, CSIDL_PROGRAM_FILES, CSIDL_MYMUSIC, 0x77 };
    for (int i = 0; i < 6; i++) { w[0] = 'x'; w[1] = 0; HRESULT hr = SHGetFolderPathW(NULL, csidl[i], NULL, 0, w); say("folder %x %08lx %s", csidl[i], hr, w2s(w)); }
    LPITEMIDLIST pidl; SHGetSpecialFolderLocation(NULL, CSIDL_PERSONAL, &pidl); SHGetPathFromIDListA(pidl, b); say("pidl %u %s", ILGetSize(pidl), b); ILFree(pidl);
    PWSTR kf; { static const GUID roaming = { 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } }; h = SHGetKnownFolderPath(&roaming, 0, NULL, &kf); } say("knownfolder %08lx %s", h, w2s(kf));
    say("mkdir %d %d", SHCreateDirectoryExA(NULL, "C:\\apitest_dir\\a\\b", NULL), SHCreateDirectoryExA(NULL, "C:\\apitest_dir\\a\\b", NULL));
    CopyFileA("C:\\apitest.ini", "C:\\apitest_dir\\a\\copy.ini", 0);
    SHFILEINFOA fi; DWORD_PTR rr = SHGetFileInfoA("C:\\apitest_dir\\a\\copy.ini", 0, &fi, sizeof fi, SHGFI_TYPENAME | SHGFI_DISPLAYNAME); say("fileinfo %d %s %s %lx", (int)rr, fi.szDisplayName, fi.szTypeName, fi.dwAttributes);
    rr = SHGetFileInfoA("C:\\apitest_dir", 0, &fi, sizeof fi, SHGFI_TYPENAME); say("fileinfo dir %d %s", (int)rr, fi.szTypeName);
    rr = SHGetFileInfoA("x.exe", 0, NULL, 0, SHGFI_EXETYPE | SHGFI_USEFILEATTRIBUTES); say("exetype %lx", (unsigned long)rr);
    SHFILEOPSTRUCTA op = { 0 }; op.wFunc = FO_DELETE; op.pFrom = "C:\\apitest_dir\0"; op.fFlags = FOF_NOCONFIRMATION;
    say("fileop delete %d exists %d", SHFileOperationA(&op), PathFileExistsA("C:\\apitest_dir"));
    /* version resources */
    char self[300]; GetModuleFileNameA(NULL, self, sizeof self);
    DWORD vh, vs = GetFileVersionInfoSizeA(self, &vh); say("versize %d", vs > 0);
    void *vb = malloc(vs); GetFileVersionInfoA(self, 0, vs, vb);
    VS_FIXEDFILEINFO *ffi; UINT fl; ok = VerQueryValueA(vb, "\\", (void **)&ffi, &fl); say("fixed %d %u %08lx %08lx %08lx", ok, fl, ffi->dwSignature, ffi->dwFileVersionMS, ffi->dwFileVersionLS);
    char *sv; ok = VerQueryValueA(vb, "\\StringFileInfo\\040904B0\\FileDescription", (void **)&sv, &fl); say("verstring %d %u %s", ok, fl, ok ? sv : "");
    WCHAR *wv; ok = VerQueryValueW(vb, L"\\StringFileInfo\\040904b0\\CompanyName", (void **)&wv, &fl); say("verstringW %d %u %s", ok, fl, ok ? w2s(wv) : "");
    DWORD *tr; ok = VerQueryValueA(vb, "\\VarFileInfo\\Translation", (void **)&tr, &fl); say("translation %d %u %08lx", ok, fl, ok ? *tr : 0);
    say("vermissing %d", VerQueryValueA(vb, "\\StringFileInfo\\040904b0\\Nope", (void **)&sv, &fl));
    { DWORD s2 = GetFileVersionInfoSizeA("C:\\nosuch.exe", &vh); say("versize none %lu err %lu", s2, GetLastError()); }
    { DWORD l2 = VerLanguageNameA(0x409, b, sizeof b); say("langname %lu %s", l2, b); }
    /* winmm */
    DWORD t1 = timeGetTime(); Sleep(50); DWORD t2 = timeGetTime(); say("timegettime %d", t2 - t1 >= 40 && t2 - t1 < 1000);
    say("waveout devs %u", waveOutGetNumDevs());
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0 }; say("waveout query %u bad %u", waveOutOpen(NULL, WAVE_MAPPER, &wf, 0, 0, WAVE_FORMAT_QUERY), (wf.wBitsPerSample = 12, waveOutOpen(NULL, WAVE_MAPPER, &wf, 0, 0, WAVE_FORMAT_QUERY)));
    say("mci %u %u", mciSendStringA("open cdaudio", NULL, 0, NULL), mciSendStringA("stop all", NULL, 0, NULL));
    say("playsound missing %d", PlaySoundA("C:\\nosuch.wav", NULL, SND_FILENAME | SND_NODEFAULT));
    say("joy %u %u", joyGetNumDevs(), midiOutGetNumDevs());
}

int main(void) {
    misc();
    k32();
    shell();
    say("done");
    return 0;
}
