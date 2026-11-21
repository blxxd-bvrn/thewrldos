/* The WRLD Browser: page memory. Documents themselves (the tree built from Lexbor's
 * HTML5 parser, CSS parsing and the cascade) are in Rust: rust/src/html.rs and
 * rust/src/css.rs. Everything one page allocates comes from an arena freed at once. */
#include "browser.h"

void *arena_alloc(arena_t *a, uint32_t n) {
    n = (n + 7) & ~7u;
    if (!a->head || a->head->used + n > a->head->cap) {
        uint32_t cap = MAX(n, 64u * 1024);
        arena_chunk_t *c = kmalloc(sizeof *c + cap);
        c->next = a->head; c->used = 0; c->cap = cap; a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}
char *arena_strdup(arena_t *a, const char *s, int n) { if (n < 0) n = strlen(s); char *d = arena_alloc(a, n + 1); memcpy(d, s, n); d[n] = 0; return d; }
void arena_free(arena_t *a) { while (a->head) { arena_chunk_t *n = a->head->next; kfree(a->head); a->head = n; } }

/* The layout of css_t, node_t and doc_t for the Rust copies in rust/src/html.rs and
 * rust/src/css.rs, which rust_init checks at boot. */
const uint32_t rust_html_layout[12] = {
    sizeof(css_t), offsetof(css_t, bsz), offsetof(css_t, fill_cur), sizeof(node_t),
    offsetof(node_t, css), offsetof(node_t, next), sizeof(doc_t), offsetof(doc_t, refresh_s),
    offsetof(doc_t, links), offsetof(doc_t, nlinks), offsetof(doc_t, css), offsetof(doc_t, nrules),
};
