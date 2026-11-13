/* QuickJS glue for THE WRLD OS: the "js" command and page scripting for
 * The WRLD Browser. Built against the QuickJS libc shim, not the kernel
 * headers; everything else is reached through jsapi.h. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "quickjs.h"
#include "jsapi.h"

/* a few kernel services (plain C types only) */
void task_yield(void);
unsigned timer_ticks(void);
unsigned mm_total(void);
unsigned mm_used(void);
void klog(const char *fmt, ...);
extern const char js_prelude[], js_prelude_end[];

struct jspage {
    JSRuntime *rt;
    JSContext *ctx;
    void *page;                 /* NULL in the shell */
    size_t mem;                 /* bytes currently allocated by this runtime */
    int busy, aborted;
    unsigned started, last_yield;
    unsigned stack;
    js_out_fn out; void *out_user;
};

/* ---------------- memory: counted per page, refused when the OS runs low ---------------- */
#define LOW_MEMORY_RESERVE (24u * 1024 * 1024)
static int heap_low(size_t want) { return mm_total() - mm_used() < LOW_MEMORY_RESERVE + want; }
static void *jm_malloc(void *op, size_t n) {
    jspage_t *j = op;
    if (heap_low(n)) return NULL;
    void *p = malloc(n);
    if (p) j->mem += malloc_usable_size(p);
    return p;
}
static void *jm_calloc(void *op, size_t c, size_t n) {
    if (n && c > 0x7FFF0000u / n) return NULL;
    void *p = jm_malloc(op, c * n);
    if (p) memset(p, 0, c * n);
    return p;
}
static void jm_free(void *op, void *p) {
    jspage_t *j = op;
    if (!p) return;
    j->mem -= malloc_usable_size(p);
    free(p);
}
static void *jm_realloc(void *op, void *p, size_t n) {
    jspage_t *j = op;
    if (!p) return jm_malloc(op, n);
    if (!n) { jm_free(op, p); return NULL; }
    size_t old = malloc_usable_size(p);
    if (n > old && heap_low(n - old)) return NULL;
    void *q = realloc(p, n);
    if (q) j->mem = j->mem - old + malloc_usable_size(q);
    return q;
}
static size_t jm_usable(const void *p) { return malloc_usable_size((void *)p); }
static const JSMallocFunctions jmf = { jm_calloc, jm_malloc, jm_free, jm_realloc, jm_usable };

/* ---------------- running scripts without freezing the desktop ---------------- */
static int interrupt(JSContext *ctx, void *op) {
    jspage_t *j = op;
    if (j->aborted) return 1;
    unsigned now = timer_ticks();
    if (now - j->last_yield >= 15) {          /* let the desktop and the network run */
        task_yield();
        j->last_yield = timer_ticks();
    }
    if (j->page) { if (dom_interrupt(j->page)) { j->aborted = 1; return 1; } }
    else if (now - j->started > 120000) return 1;
    return 0;
}

static jspage_t *new_runtime(void *page, unsigned stack) {
    jspage_t *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->page = page; j->stack = stack;
    j->rt = JS_NewRuntime2(&jmf, j);
    if (!j->rt) { free(j); return NULL; }
    JS_SetMaxStackSize(j->rt, stack);
    JS_SetInterruptHandler(j->rt, interrupt, j);
    JS_SetGCThreshold(j->rt, 4 * 1024 * 1024);
    j->ctx = JS_NewContext(j->rt);
    if (!j->ctx) { JS_FreeRuntime(j->rt); free(j); return NULL; }
    JS_SetContextOpaque(j->ctx, j);
    return j;
}
static void enter(jspage_t *j) { JS_UpdateStackTop(j->rt); j->busy++; j->started = j->last_yield = timer_ticks(); }
static void leave(jspage_t *j) {
    /* run promise jobs (then/await) queued by the code that just ran */
    JSContext *c;
    for (int i = 0; i < 10000 && !j->aborted; i++) if (JS_ExecutePendingJob(j->rt, &c) <= 0) break;
    j->busy--;
}

/* report an exception: name, message and stack */
static void report(jspage_t *j) {
    JSValue e = JS_GetException(j->ctx);
    if (j->aborted) { JS_FreeValue(j->ctx, e); return; }
    const char *s = JS_ToCString(j->ctx, e);
    char buf[1024]; snprintf(buf, sizeof buf, "%s", s ? s : "exception");
    if (s) JS_FreeCString(j->ctx, s);
    if (JS_IsError(e)) {
        JSValue st = JS_GetPropertyStr(j->ctx, e, "stack");
        const char *ss = JS_IsString(st) ? JS_ToCString(j->ctx, st) : NULL;
        if (ss && *ss) { size_t l = strlen(buf); snprintf(buf + l, sizeof buf - l, "\n%s", ss); }
        if (ss) JS_FreeCString(j->ctx, ss);
        JS_FreeValue(j->ctx, st);
    }
    JS_FreeValue(j->ctx, e);
    if (j->page) dom_console(j->page, 3, buf);
    else if (j->out) { j->out(j->out_user, buf); j->out(j->out_user, "\n"); }
}

/* ---------------- helpers for native functions ---------------- */
#define J() ((jspage_t *)JS_GetContextOpaque(ctx))
#define PG() (J()->page)
#define FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
static void *node_arg(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    uint32_t v = 0;
    if (i < argc && !JS_IsNull(argv[i]) && !JS_IsUndefined(argv[i])) JS_ToUint32(ctx, &v, argv[i]);
    return (void *)(uintptr_t)v;
}
static int int_arg(JSContext *ctx, int argc, JSValueConst *argv, int i) { int32_t v = 0; if (i < argc) JS_ToInt32(ctx, &v, argv[i]); return v; }
/* strings: the caller frees with JS_FreeCString; NULL/undefined give NULL */
static const char *str_arg(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    if (i >= argc || JS_IsNull(argv[i]) || JS_IsUndefined(argv[i])) return NULL;
    return JS_ToCString(ctx, argv[i]);
}
#define FREE_S(s) do { if (s) JS_FreeCString(ctx, s); } while (0)
static JSValue node_val(JSContext *ctx, void *n) { return JS_NewUint32(ctx, (uint32_t)(uintptr_t)n); }
static JSValue own_str(JSContext *ctx, char *s) {     /* takes a dom_* string */
    if (!s) return JS_NULL;
    JSValue v = JS_NewString(ctx, s);
    dom_free(s);
    return v;
}

/* ---------------- __host: DOM primitives used by prelude.js ---------------- */
FN(h_root) { return node_val(ctx, dom_root(PG())); }
FN(h_by_id) { const char *s = str_arg(ctx, argc, argv, 0); void *n = s ? dom_by_id(PG(), s) : NULL; FREE_S(s); return node_val(ctx, n); }
FN(h_query) {
    void *root = node_arg(ctx, argc, argv, 0);
    const char *sel = str_arg(ctx, argc, argv, 1);
    int first = int_arg(ctx, argc, argv, 2);
    JSValue arr = JS_NewArray(ctx);
    if (sel) {
        int cap = first ? 1 : 4096;
        void **out = malloc(sizeof(void *) * cap);
        int n = out ? dom_query(PG(), root, sel, out, cap, first) : 0;
        for (int i = 0; i < n; i++) JS_SetPropertyUint32(ctx, arr, i, node_val(ctx, out[i]));
        free(out);
        if (n < 0) { FREE_S(sel); JS_FreeValue(ctx, arr); return JS_ThrowSyntaxError(ctx, "unsupported selector"); }
    }
    FREE_S(sel);
    return arr;
}
FN(h_matches) { const char *s = str_arg(ctx, argc, argv, 1); int r = s ? dom_matches(node_arg(ctx, argc, argv, 0), s) : 0; FREE_S(s); return JS_NewBool(ctx, r > 0); }
FN(h_type) { void *n = node_arg(ctx, argc, argv, 0); return JS_NewInt32(ctx, n ? dom_node_type(n) : 0); }
FN(h_tag) { void *n = node_arg(ctx, argc, argv, 0); return JS_NewString(ctx, n ? dom_tag(n) : ""); }
FN(h_parent) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_parent(n) : NULL); }
FN(h_first) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_first(n) : NULL); }
FN(h_last) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_last(n) : NULL); }
FN(h_next) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_next(n) : NULL); }
FN(h_prev) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_prev(n) : NULL); }
FN(h_get_attr) { void *n = node_arg(ctx, argc, argv, 0); const char *a = str_arg(ctx, argc, argv, 1); char *v = n && a ? dom_get_attr(n, a) : NULL; FREE_S(a); return own_str(ctx, v); }
FN(h_set_attr) {
    void *n = node_arg(ctx, argc, argv, 0); const char *a = str_arg(ctx, argc, argv, 1), *v = str_arg(ctx, argc, argv, 2);
    if (n && a) dom_set_attr(PG(), n, a, v ? v : "");
    FREE_S(a); FREE_S(v); return JS_UNDEFINED;
}
FN(h_remove_attr) { void *n = node_arg(ctx, argc, argv, 0); const char *a = str_arg(ctx, argc, argv, 1); if (n && a) dom_remove_attr(PG(), n, a); FREE_S(a); return JS_UNDEFINED; }
FN(h_attr_names) {
    void *n = node_arg(ctx, argc, argv, 0);
    JSValue arr = JS_NewArray(ctx);
    int c = n ? dom_attr_count(n) : 0;
    for (int i = 0; i < c; i++) JS_SetPropertyUint32(ctx, arr, i, own_str(ctx, dom_attr_name(n, i)));
    return arr;
}
FN(h_text) { void *n = node_arg(ctx, argc, argv, 0); JSValue v = own_str(ctx, n ? dom_text(n) : NULL); return JS_IsNull(v) ? JS_NewString(ctx, "") : v; }
FN(h_set_text) { void *n = node_arg(ctx, argc, argv, 0); const char *s = str_arg(ctx, argc, argv, 1); if (n) dom_set_text(PG(), n, s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_html) { void *n = node_arg(ctx, argc, argv, 0); JSValue v = own_str(ctx, n ? dom_inner_html(n, int_arg(ctx, argc, argv, 1)) : NULL); return JS_IsNull(v) ? JS_NewString(ctx, "") : v; }
FN(h_set_html) { void *n = node_arg(ctx, argc, argv, 0); const char *s = str_arg(ctx, argc, argv, 1); if (n) dom_set_inner_html(PG(), n, s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_insert_html) {
    void *n = node_arg(ctx, argc, argv, 0); const char *w = str_arg(ctx, argc, argv, 1), *s = str_arg(ctx, argc, argv, 2);
    if (n && w) dom_insert_html(PG(), n, w, s ? s : "");
    FREE_S(w); FREE_S(s); return JS_UNDEFINED;
}
FN(h_create) { const char *t = str_arg(ctx, argc, argv, 0); void *n = dom_create(PG(), t ? t : "div"); FREE_S(t); return node_val(ctx, n); }
FN(h_create_text) { const char *t = str_arg(ctx, argc, argv, 0); void *n = dom_create_text(PG(), t ? t : ""); FREE_S(t); return node_val(ctx, n); }
FN(h_clone) { void *n = node_arg(ctx, argc, argv, 0); return node_val(ctx, n ? dom_clone(PG(), n, int_arg(ctx, argc, argv, 1)) : NULL); }
FN(h_insert) {
    void *p = node_arg(ctx, argc, argv, 0), *c = node_arg(ctx, argc, argv, 1), *b = node_arg(ctx, argc, argv, 2);
    if (!p || !c) return JS_ThrowTypeError(ctx, "insertBefore: not a node");
    if (dom_insert(PG(), p, c, b) < 0) return JS_ThrowTypeError(ctx, "The new child element contains the parent.");
    return JS_UNDEFINED;
}
FN(h_remove) { void *c = node_arg(ctx, argc, argv, 0); if (c) dom_remove(PG(), c); return JS_UNDEFINED; }
FN(h_set_style) {
    void *n = node_arg(ctx, argc, argv, 0); const char *p = str_arg(ctx, argc, argv, 1), *v = str_arg(ctx, argc, argv, 2);
    if (n && p) dom_set_style(PG(), n, p, v ? v : "");
    FREE_S(p); FREE_S(v); return JS_UNDEFINED;
}
FN(h_get_style) { void *n = node_arg(ctx, argc, argv, 0); const char *p = str_arg(ctx, argc, argv, 1); char *v = n && p ? dom_get_style(n, p) : NULL; FREE_S(p); JSValue r = own_str(ctx, v); return JS_IsNull(r) ? JS_NewString(ctx, "") : r; }
FN(h_get_value) { void *n = node_arg(ctx, argc, argv, 0); JSValue v = own_str(ctx, n ? dom_get_value(PG(), n) : NULL); return JS_IsNull(v) ? JS_NewString(ctx, "") : v; }
FN(h_set_value) { void *n = node_arg(ctx, argc, argv, 0); const char *s = str_arg(ctx, argc, argv, 1); if (n) dom_set_value(PG(), n, s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_get_checked) { void *n = node_arg(ctx, argc, argv, 0); return JS_NewBool(ctx, n && dom_get_checked(PG(), n)); }
FN(h_set_checked) { void *n = node_arg(ctx, argc, argv, 0); if (n) dom_set_checked(PG(), n, int_arg(ctx, argc, argv, 1)); return JS_UNDEFINED; }
FN(h_get_sel) { void *n = node_arg(ctx, argc, argv, 0); return JS_NewInt32(ctx, n ? dom_get_selected_index(PG(), n) : -1); }
FN(h_set_sel) { void *n = node_arg(ctx, argc, argv, 0); if (n) dom_set_selected_index(PG(), n, int_arg(ctx, argc, argv, 1)); return JS_UNDEFINED; }
FN(h_focus) { void *n = node_arg(ctx, argc, argv, 0); if (n) dom_focus(PG(), n); return JS_UNDEFINED; }
FN(h_submit) { void *n = node_arg(ctx, argc, argv, 0); if (n) dom_submit(PG(), n); return JS_UNDEFINED; }
FN(h_default_action) { void *n = node_arg(ctx, argc, argv, 0); const char *t = str_arg(ctx, argc, argv, 1); if (n && t) dom_default_action(PG(), n, t); FREE_S(t); return JS_UNDEFINED; }
FN(h_title) { return own_str(ctx, dom_title(PG())); }
FN(h_set_title) { const char *s = str_arg(ctx, argc, argv, 0); dom_set_title(PG(), s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_url) { return own_str(ctx, dom_url(PG())); }
FN(h_referrer) { JSValue v = own_str(ctx, dom_referrer(PG())); return JS_IsNull(v) ? JS_NewString(ctx, "") : v; }
FN(h_resolve) { const char *s = str_arg(ctx, argc, argv, 0); JSValue v = own_str(ctx, dom_resolve(PG(), NULL, s ? s : "")); FREE_S(s); return v; }
FN(h_resolve_against) {
    const char *b = str_arg(ctx, argc, argv, 0), *s = str_arg(ctx, argc, argv, 1);
    JSValue v = own_str(ctx, dom_resolve(PG(), b ? b : "", s ? s : ""));
    FREE_S(b); FREE_S(s); return v;
}
FN(h_navigate) { const char *s = str_arg(ctx, argc, argv, 0); if (s) dom_navigate(PG(), s, int_arg(ctx, argc, argv, 1)); FREE_S(s); return JS_UNDEFINED; }
FN(h_history) { dom_history(PG(), int_arg(ctx, argc, argv, 0)); return JS_UNDEFINED; }
FN(h_reload) { dom_reload(PG()); return JS_UNDEFINED; }
FN(h_alert) {
    const char *k = str_arg(ctx, argc, argv, 0), *s = str_arg(ctx, argc, argv, 1);
    dom_alert(PG(), k ? k : "alert", s ? s : "");
    FREE_S(k); FREE_S(s); return JS_UNDEFINED;
}
FN(h_console) { const char *s = str_arg(ctx, argc, argv, 1); dom_console(PG(), int_arg(ctx, argc, argv, 0), s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_status) { const char *s = str_arg(ctx, argc, argv, 0); dom_status(PG(), s ? s : ""); FREE_S(s); return JS_UNDEFINED; }
FN(h_cookie) { JSValue v = own_str(ctx, dom_cookie(PG())); return JS_IsNull(v) ? JS_NewString(ctx, "") : v; }
FN(h_set_cookie) { const char *s = str_arg(ctx, argc, argv, 0); if (s) dom_set_cookie(PG(), s); FREE_S(s); return JS_UNDEFINED; }
FN(h_sget) { const char *k = str_arg(ctx, argc, argv, 0); char *v = k ? dom_storage_get(PG(), k) : NULL; FREE_S(k); return own_str(ctx, v); }
FN(h_sset) { const char *k = str_arg(ctx, argc, argv, 0), *v = str_arg(ctx, argc, argv, 1); if (k) dom_storage_set(PG(), k, v); FREE_S(k); FREE_S(v); return JS_UNDEFINED; }
FN(h_sclear) { dom_storage_clear(PG()); return JS_UNDEFINED; }
FN(h_storage_keys) {
    char *s = dom_storage_keys(PG());
    JSValue arr = JS_NewArray(ctx);
    int i = 0;
    for (char *p = s; p && *p;) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, p, l));
        p = e ? e + 1 : p + l;
    }
    if (s) dom_free(s);
    return arr;
}
FN(h_viewport) { return JS_NewInt32(ctx, dom_viewport(PG(), int_arg(ctx, argc, argv, 0))); }
FN(h_scroll_to) { dom_scroll_to(PG(), int_arg(ctx, argc, argv, 0), int_arg(ctx, argc, argv, 1)); return JS_UNDEFINED; }
FN(h_rect) {
    void *n = node_arg(ctx, argc, argv, 0); int x, y, w, h;
    if (!n || !dom_rect(PG(), n, &x, &y, &w, &h)) return JS_NULL;
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewInt32(ctx, x)); JS_SetPropertyUint32(ctx, a, 1, JS_NewInt32(ctx, y));
    JS_SetPropertyUint32(ctx, a, 2, JS_NewInt32(ctx, w)); JS_SetPropertyUint32(ctx, a, 3, JS_NewInt32(ctx, h));
    return a;
}
FN(h_fetch) {
    const char *u = str_arg(ctx, argc, argv, 0), *m = str_arg(ctx, argc, argv, 1), *b = str_arg(ctx, argc, argv, 2);
    char *out = NULL, *ct = NULL; int len = 0, status = 0;
    int ok = u ? dom_fetch(PG(), u, m ? m : "GET", b, &out, &len, &status, &ct) : 0;
    FREE_S(u); FREE_S(m); FREE_S(b);
    if (!ok) { if (out) dom_free(out); if (ct) dom_free(ct); return JS_NULL; }
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, r, "text", JS_NewStringLen(ctx, out ? out : "", out ? len : 0));
    JS_SetPropertyStr(ctx, r, "type", JS_NewString(ctx, ct ? ct : ""));
    if (out) dom_free(out);
    if (ct) dom_free(ct);
    return r;
}
FN(h_now) { return JS_NewFloat64(ctx, (double)dom_now_ms()); }
FN(h_epoch) { return JS_NewFloat64(ctx, dom_epoch_ms()); }

static const struct { const char *name; JSCFunction *fn; int nargs; } host_fns[] = {
    { "root", h_root, 0 }, { "byId", h_by_id, 1 }, { "query", h_query, 3 }, { "matches", h_matches, 2 }, { "type", h_type, 1 },
    { "tag", h_tag, 1 }, { "parent", h_parent, 1 }, { "first", h_first, 1 }, { "last", h_last, 1 }, { "next", h_next, 1 },
    { "prev", h_prev, 1 }, { "getAttr", h_get_attr, 2 }, { "setAttr", h_set_attr, 3 }, { "removeAttr", h_remove_attr, 2 },
    { "attrNames", h_attr_names, 1 }, { "text", h_text, 1 }, { "setText", h_set_text, 2 }, { "html", h_html, 2 },
    { "setHtml", h_set_html, 2 }, { "insertHtml", h_insert_html, 3 }, { "create", h_create, 1 }, { "createText", h_create_text, 1 },
    { "clone", h_clone, 2 }, { "insert", h_insert, 3 }, { "remove", h_remove, 1 }, { "setStyle", h_set_style, 3 },
    { "getStyle", h_get_style, 2 }, { "getValue", h_get_value, 1 }, { "setValue", h_set_value, 2 }, { "getChecked", h_get_checked, 1 },
    { "setChecked", h_set_checked, 2 }, { "getSel", h_get_sel, 1 }, { "setSel", h_set_sel, 2 }, { "focus", h_focus, 1 },
    { "submit", h_submit, 1 }, { "defaultAction", h_default_action, 2 }, { "title", h_title, 0 }, { "setTitle", h_set_title, 1 },
    { "url", h_url, 0 }, { "referrer", h_referrer, 0 }, { "resolve", h_resolve, 1 }, { "resolveAgainst", h_resolve_against, 2 },
    { "navigate", h_navigate, 2 }, { "history", h_history, 1 }, { "reload", h_reload, 0 }, { "alert", h_alert, 2 },
    { "console", h_console, 2 }, { "status", h_status, 1 }, { "cookie", h_cookie, 0 }, { "setCookie", h_set_cookie, 1 },
    { "sget", h_sget, 1 }, { "sset", h_sset, 2 }, { "sclear", h_sclear, 0 }, { "storageKeys", h_storage_keys, 0 },
    { "viewport", h_viewport, 1 }, { "scrollTo", h_scroll_to, 2 }, { "rect", h_rect, 1 }, { "fetch", h_fetch, 3 },
    { "now", h_now, 0 }, { "epoch", h_epoch, 0 },
};

/* ---------------- page API ---------------- */
static int eval_in(jspage_t *j, const char *src, int len, const char *name) {
    if (j->aborted) return -2;
    enter(j);
    JSValue r = JS_Eval(j->ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(r)) { report(j); rc = j->aborted ? -2 : -1; }
    JS_FreeValue(j->ctx, r);
    leave(j);
    return rc;
}
jspage_t *js_page_new(void *page) {
    jspage_t *j = new_runtime(page, 160 * 1024);
    if (!j) return NULL;
    JSValue g = JS_GetGlobalObject(j->ctx), h = JS_NewObject(j->ctx);
    for (unsigned i = 0; i < sizeof host_fns / sizeof host_fns[0]; i++)
        JS_SetPropertyStr(j->ctx, h, host_fns[i].name, JS_NewCFunction(j->ctx, host_fns[i].fn, host_fns[i].name, host_fns[i].nargs));
    JS_SetPropertyStr(j->ctx, g, "__host", h);
    JS_FreeValue(j->ctx, g);
    if (eval_in(j, js_prelude, (int)(js_prelude_end - js_prelude), "wrld:prelude.js") != 0) {
        klog("js: the browser prelude failed to load");
        js_page_free(j);
        return NULL;
    }
    return j;
}
void js_page_free(jspage_t *j) {
    if (!j) return;
    JS_FreeContext(j->ctx);
    JS_FreeRuntime(j->rt);
    free(j);
}
int js_page_eval(jspage_t *j, const char *src, int len, const char *name) { return j ? eval_in(j, src, len, name) : -1; }

/* call a global function set up by the prelude */
static JSValue call_global(jspage_t *j, const char *fn, int argc, JSValue *argv) {
    JSValue g = JS_GetGlobalObject(j->ctx), f = JS_GetPropertyStr(j->ctx, g, fn), r = JS_UNDEFINED;
    if (JS_IsFunction(j->ctx, f)) {
        enter(j);
        r = JS_Call(j->ctx, f, g, argc, argv);
        if (JS_IsException(r)) { report(j); r = JS_UNDEFINED; }
        leave(j);
    }
    JS_FreeValue(j->ctx, f); JS_FreeValue(j->ctx, g);
    return r;
}
int js_page_dispatch(jspage_t *j, void *node, const char *type, int key) {
    if (!j || j->aborted) return 0;
    JSValue a[3] = { node_val(j->ctx, node), JS_NewString(j->ctx, type), JS_NewInt32(j->ctx, key) };
    JSValue r = call_global(j, "__wrld_dispatch", 3, a);
    int prevented = JS_ToBool(j->ctx, r) > 0;
    JS_FreeValue(j->ctx, r); JS_FreeValue(j->ctx, a[1]);
    return prevented;
}
void js_page_fire(jspage_t *j, const char *type) {
    if (!j || j->aborted) return;
    JSValue a = JS_NewString(j->ctx, type);
    JS_FreeValue(j->ctx, call_global(j, "__wrld_fire", 1, &a));
    JS_FreeValue(j->ctx, a);
}
int js_page_run_timers(jspage_t *j) {
    if (!j || j->aborted) return -1;
    JSValue r = call_global(j, "__wrld_timers", 0, NULL);
    int32_t next = -1;
    if (JS_IsNumber(r)) JS_ToInt32(j->ctx, &next, r);
    JS_FreeValue(j->ctx, r);
    return next;
}
char *js_page_take_written(jspage_t *j) {
    if (!j || j->aborted) return NULL;
    JSValue r = call_global(j, "__wrld_take_written", 0, NULL);
    char *out = NULL;
    if (JS_IsString(r)) {
        size_t l; const char *s = JS_ToCStringLen(j->ctx, &l, r);
        if (s && l) { out = malloc(l + 1); if (out) { memcpy(out, s, l); out[l] = 0; } }
        if (s) JS_FreeCString(j->ctx, s);
    }
    JS_FreeValue(j->ctx, r);
    return out;
}
unsigned js_page_mem_kb(jspage_t *j) { return j ? (unsigned)(j->mem / 1024) : 0; }
int js_page_busy(jspage_t *j) { return j && j->busy; }
void js_page_abort(jspage_t *j) { if (j) j->aborted = 1; }
int js_page_aborted(jspage_t *j) { return !j || j->aborted; }
void js_free_str(char *s) { free(s); }
const char *js_engine_version(void) { return JS_GetVersion(); }

/* ---------------- the "js" command ---------------- */
static JSValue sh_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    jspage_t *j = J();
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (i) j->out(j->out_user, " ");
        j->out(j->out_user, s ? s : "?");
        if (s) JS_FreeCString(ctx, s);
    }
    j->out(j->out_user, "\n");
    return JS_UNDEFINED;
}
int js_shell_run(const char *src, int len, const char *name, js_out_fn out, void *user) {
    jspage_t *j = new_runtime(NULL, 80 * 1024);
    if (!j) { out(user, "js: not enough memory\n"); return -1; }
    j->out = out; j->out_user = user;
    JSValue g = JS_GetGlobalObject(j->ctx), con = JS_NewObject(j->ctx);
    static const char *names[] = { "log", "info", "warn", "error", "debug" };
    for (unsigned i = 0; i < 5; i++) JS_SetPropertyStr(j->ctx, con, names[i], JS_NewCFunction(j->ctx, sh_print, names[i], 1));
    JS_SetPropertyStr(j->ctx, g, "console", con);
    JS_SetPropertyStr(j->ctx, g, "print", JS_NewCFunction(j->ctx, sh_print, "print", 1));
    JS_FreeValue(j->ctx, g);
    enter(j);
    JSValue r = JS_Eval(j->ctx, src, len, name, JS_EVAL_TYPE_GLOBAL);
    int rc = 0;
    if (JS_IsException(r)) { report(j); rc = 1; }
    else if (!JS_IsUndefined(r) && !strcmp(name, "<command line>")) {   /* show the value of an expression */
        const char *s = JS_ToCString(j->ctx, r);
        if (s) { out(user, s); out(user, "\n"); JS_FreeCString(j->ctx, s); }
    }
    JS_FreeValue(j->ctx, r);
    leave(j);
    js_page_free(j);
    return rc;
}
