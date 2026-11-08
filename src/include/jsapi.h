/* JavaScript engine interface (QuickJS glue in src/js <-> the rest of the OS).
 * The two sides are compiled against different headers (the kernel's bool is
 * an int, QuickJS uses C99 _Bool), so only plain C types cross this boundary:
 * int, unsigned, double, char pointers and opaque pointers. Strings are UTF-8
 * unless noted. Implemented in src/js/engine.c unless noted. */
#ifndef JSAPI_H
#define JSAPI_H

typedef struct jspage jspage_t;

/* ---- the "js" shell command: run code in a fresh context ---- */
typedef void (*js_out_fn)(void *user, const char *text);
int js_shell_run(const char *src, int len, const char *name, js_out_fn out, void *user);   /* 0 = ok */
const char *js_engine_version(void);

/* ---- page scripting (XplorerLite) ---- */
jspage_t *js_page_new(void *page);                 /* page = the browser's page_t */
void js_page_free(jspage_t *j);
int js_page_eval(jspage_t *j, const char *src, int len, const char *name);   /* 0 ok, -1 exception, -2 aborted */
int js_page_dispatch(jspage_t *j, void *node, const char *type, int key);    /* 1 = default prevented */
void js_page_fire(jspage_t *j, const char *type);  /* DOMContentLoaded / load on document and window */
int js_page_run_timers(jspage_t *j);               /* runs due timers; returns ms until the next (-1 none) */
char *js_page_take_written(jspage_t *j);           /* document.write output since the last call (free with js_free_str) */
unsigned js_page_mem_kb(jspage_t *j);
int js_page_busy(jspage_t *j);
void js_page_abort(jspage_t *j);                   /* stop all scripts on this page for good */
int js_page_aborted(jspage_t *j);
void js_free_str(char *s);

/* ---- implemented by the browser (src/apps/dom.c), called from the glue ---- */
/* nodes */
void *dom_root(void *page);
void *dom_by_id(void *page, const char *id);
int dom_query(void *page, void *root, const char *selector, void **out, int max, int first_only);
int dom_node_type(void *node);                     /* 1 element, 3 text, 9 document */
const char *dom_tag(void *node);                   /* lower case; "#doc" for the root */
void *dom_parent(void *node);
void *dom_first(void *node);
void *dom_last(void *node);
void *dom_next(void *node);
void *dom_prev(void *node);
int dom_matches(void *node, const char *selector);
/* attributes and content (returned strings are UTF-8 and must be freed with dom_free) */
char *dom_get_attr(void *node, const char *name);  /* NULL if absent */
void dom_set_attr(void *page, void *node, const char *name, const char *value);
void dom_remove_attr(void *page, void *node, const char *name);
int dom_attr_count(void *node);
char *dom_attr_name(void *node, int i);
char *dom_text(void *node);
void dom_set_text(void *page, void *node, const char *text);
char *dom_inner_html(void *node, int outer);
int dom_set_inner_html(void *page, void *node, const char *html);
int dom_insert_html(void *page, void *node, const char *where, const char *html);  /* beforebegin/afterbegin/beforeend/afterend */
void *dom_create(void *page, const char *tag);
void *dom_create_text(void *page, const char *text);
void *dom_clone(void *page, void *node, int deep);
int dom_insert(void *page, void *parent, void *child, void *before);     /* append when before is NULL */
int dom_remove(void *page, void *child);
void dom_set_style(void *page, void *node, const char *prop, const char *value);
char *dom_get_style(void *node, const char *prop);
/* form controls */
char *dom_get_value(void *page, void *node);
void dom_set_value(void *page, void *node, const char *value);
int dom_get_checked(void *page, void *node);
void dom_set_checked(void *page, void *node, int on);
int dom_get_selected_index(void *page, void *node);
void dom_set_selected_index(void *page, void *node, int index);
void dom_focus(void *page, void *node);
int dom_submit(void *page, void *form);
/* the page and the window */
char *dom_title(void *page);
void dom_set_title(void *page, const char *title);
char *dom_url(void *page);
char *dom_referrer(void *page);
void dom_navigate(void *page, const char *url, int replace);
void dom_history(void *page, int delta);
void dom_reload(void *page);
void dom_alert(void *page, const char *kind, const char *text);
void dom_console(void *page, int level, const char *text);
void dom_status(void *page, const char *text);
char *dom_cookie(void *page);
void dom_set_cookie(void *page, const char *cookie);
char *dom_storage_get(void *page, const char *key);
void dom_storage_set(void *page, const char *key, const char *value);   /* value NULL removes */
void dom_storage_clear(void *page);
int dom_viewport(void *page, int which);           /* 0 width, 1 height, 2 scrollY, 3 page height, 4/5 screen */
void dom_scroll_to(void *page, int x, int y);
int dom_rect(void *page, void *node, int *x, int *y, int *w, int *h);
int dom_fetch(void *page, const char *url, const char *method, const char *body, char **out, int *outlen, int *status, char **ctype);
char *dom_resolve(void *page, const char *base, const char *url);   /* base NULL = the page URL */
char *dom_storage_keys(void *page);                /* keys separated by '\n' */
void dom_default_action(void *page, void *node, const char *type); /* el.click(): follow link, submit... */
void dom_free(char *s);
unsigned dom_now_ms(void);
double dom_epoch_ms(void);
int dom_interrupt(void *page);                     /* called while scripts run: yields, memory check; 1 = abort */
void dom_changed(void *page);                      /* the tree changed: lay out again */
#endif
