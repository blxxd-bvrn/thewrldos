/* Lexbor glue (src/lexbor/lexglue.c): the HTML5 parser and the CSS selector
 * parser for XplorerLite. Only plain C types cross this boundary: the kernel's
 * bool is not C's, so neither side includes the other's headers. */
#ifndef LEXGLUE_H
#define LEXGLUE_H

/* ---- HTML: parse, then replay the finished tree as start/text/end events ---- */
typedef struct { const char *name; unsigned name_len; const char *value; unsigned value_len; } lxg_attr_t;
typedef struct {
    void *ctx;
    void (*start)(void *ctx, const char *name, unsigned name_len, const lxg_attr_t *attrs, int nattr);
    void (*end)(void *ctx);
    void (*text)(void *ctx, const char *utf8, unsigned len);
} lxg_sink_t;
/* utf8: the whole document as UTF-8. Returns 0 on success. Element names are
 * lower case for HTML; text and attribute values are UTF-8 with character
 * references already decoded. Template contents are not replayed. */
int lxg_parse_html(const char *utf8, unsigned len, const lxg_sink_t *sink);
/* innerHTML-style: parse utf8 as the contents of a <context_tag> element and
 * replay only the resulting children */
int lxg_parse_fragment(const char *context_tag, const char *utf8, unsigned len, const lxg_sink_t *sink);

/* ---- CSS selectors ---- */
/* The caller's document tree, as seen by the matcher. Nodes are opaque. */
typedef struct {
    void *(*parent)(void *n);                      /* parent element, NULL above the top */
    void *(*prev)(void *n);                        /* previous / next sibling element */
    void *(*next)(void *n);
    const char *(*tag)(void *n);                   /* lower case */
    const char *(*attr)(void *n, const char *name); /* value (UTF-8) or NULL */
    int (*is_empty)(void *n);                      /* no element children and no text */
    int (*is_root)(void *n);                       /* matches :root */
} lxg_tree_t;

/* Selectors are parsed into a pool that holds every selector of one page. */
void *lxg_pool_new(void);
void lxg_pool_free(void *pool);

/* Key used to bucket a selector by its right-most compound selector. */
enum { LXG_KEY_ANY = 0, LXG_KEY_TAG, LXG_KEY_CLASS, LXG_KEY_ID };
typedef void (*lxg_each_f)(void *ctx, void *sel, unsigned specificity, int key_kind, const char *key, unsigned key_len);
/* Parses a selector list (UTF-8) and calls each() once per complex selector
 * that could ever match an element (pseudo-element selectors are left out).
 * Returns the number of selectors passed to each(), or -1 if the list is invalid. */
int lxg_selectors(void *pool, const char *sel, unsigned len, lxg_each_f each, void *ctx);
int lxg_match(void *sel, void *node, const lxg_tree_t *tree);    /* 1 if node matches */

const char *lxg_version(void);
#endif
