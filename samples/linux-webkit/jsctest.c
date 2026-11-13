/* WebKit's JavaScript engine (JavaScriptCore, from WebKitGTK) on THE WRLD OS.
 * Runs modern JavaScript through the JSC C API and times a hot loop, which
 * JavaScriptCore compiles with its JIT tiers. */
#include <JavaScriptCore/JavaScript.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int passed, failed;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static char *eval(JSGlobalContextRef ctx, const char *src, char *buf, size_t cap) {
    JSStringRef s = JSStringCreateWithUTF8CString(src);
    JSValueRef exc = NULL;
    JSValueRef v = JSEvaluateScript(ctx, s, NULL, NULL, 1, &exc);
    JSStringRelease(s);
    JSStringRef r = JSValueToStringCopy(ctx, exc ? exc : v, NULL);
    JSStringGetUTF8CString(r, buf, cap); JSStringRelease(r);
    return buf;
}
static void check(JSGlobalContextRef ctx, const char *what, const char *src, const char *want) {
    char out[256]; eval(ctx, src, out, sizeof out);
    int ok = !strcmp(out, want);
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : "  got: ", ok ? "" : out); fflush(stdout);
    if (ok) passed++; else failed++;
}
static JSValueRef native_add(JSContextRef ctx, JSObjectRef f, JSObjectRef self, size_t argc, const JSValueRef argv[], JSValueRef *exc) {
    double s = 0; for (size_t i = 0; i < argc; i++) s += JSValueToNumber(ctx, argv[i], exc);
    return JSValueMakeNumber(ctx, s);
}
int main(void) {
    printf("THE WRLD OS - WebKit's JavaScriptCore\n");
    JSGlobalContextRef ctx = JSGlobalContextCreate(NULL);
    JSStringRef name = JSStringCreateWithUTF8CString("nativeAdd");
    JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, JSObjectMakeFunctionWithCallback(ctx, name, native_add), 0, NULL);
    JSStringRelease(name);
    check(ctx, "arithmetic and strings", "(6*7) + ' ' + 'web'.toUpperCase()", "42 WEB");
    check(ctx, "classes, inheritance, template literals", "class A{constructor(x){this.x=x}get d(){return this.x*2}} class B extends A{toString(){return `B(${this.d})`}} String(new B(21))", "B(42)");
    check(ctx, "closures, arrow functions, spread", "const mk=n=>()=>++n; const c=mk(40); c(); [c(), ...[1,2]].join(',')", "42,1,2");
    check(ctx, "JSON round trip", "JSON.stringify(JSON.parse('{\"a\":[1,2,{\"b\":null}],\"c\":\"\\u00e9\"}'))", "{\"a\":[1,2,{\"b\":null}],\"c\":\"\xc3\xa9\"}");
    check(ctx, "regular expressions with named groups", "'2026-11-13'.replace(/(?<y>\\d+)-(?<m>\\d+)-(?<d>\\d+)/, '$<d>/$<m>/$<y>')", "13/11/2026");
    check(ctx, "Map, Set, destructuring", "const m=new Map([[1,'a'],[2,'b']]); const s=new Set([1,1,2,3]); const {size}=s; `${m.get(2)}${size}`", "b3");
    check(ctx, "typed arrays", "const f=new Float64Array([1.5,2.5]); const u=new Uint8Array(f.buffer); u.length + ':' + f.reduce((a,b)=>a+b)", "16:4");
    check(ctx, "BigInt", "(2n**100n).toString().slice(0,10)", "1267650600");
    check(ctx, "generators and iterators", "function* g(){yield* [1,2,3]} [...g()].map(x=>x*x).join('')", "149");
    check(ctx, "calls into C", "nativeAdd(40, 1, 1)", "42");
    check(ctx, "exceptions", "try { null.x } catch (e) { e instanceof TypeError }", "true");
    /* a hot loop: run it a few times; later runs use optimised JIT code */
    const char *bench = "(() => { function fib(n){return n<2?n:fib(n-1)+fib(n-2)} let s=0; for (let i=0;i<300000;i++) s+=i%7; return fib(25)+s%1; })()";
    char out[64]; double best = 1e9, first = 0;
    for (int r = 0; r < 5; r++) {
        double t0 = now_ms(); eval(ctx, bench, out, sizeof out); double dt = now_ms() - t0;
        if (r == 0) first = dt;
        if (dt < best) best = dt;
    }
    printf("      fib(25) + 300,000-iteration loop: first run %.0f ms, best %.0f ms (result %s)\n", first, best, out);
    if (!strcmp(out, "75025")) passed++; else { failed++; printf("FAIL  benchmark result\n"); }
    JSGlobalContextRelease(ctx);
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
