/* Lexbor glue for XplorerLite: HTML5 parsing (lexbor/html) and CSS selector
 * parsing (lexbor/css), with a selector matcher that runs on XplorerLite's own
 * document tree through the callbacks in lexglue.h. Compiled against the C
 * library shim and Lexbor's headers only; see lexglue.h for the boundary. */
#include "lexbor/html/html.h"
#include "lexbor/css/css.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexglue.h"

/* ---------------- HTML ---------------- */
#define MAX_ATTRS 64

static void emit_start(lxb_dom_element_t *el, const lxg_sink_t *s) {
    lxg_attr_t at[MAX_ATTRS];
    int n = 0;
    for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(el); a && n < MAX_ATTRS; a = lxb_dom_element_next_attribute(a)) {
        size_t nl = 0, vl = 0;
        const lxb_char_t *nm = lxb_dom_attr_qualified_name(a, &nl);
        const lxb_char_t *v = lxb_dom_attr_value(a, &vl);
        if (!nm || !nl) continue;
        at[n].name = (const char *)nm; at[n].name_len = (unsigned)nl;
        at[n].value = v ? (const char *)v : ""; at[n].value_len = v ? (unsigned)vl : 0;
        n++;
    }
    size_t tl = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &tl);
    s->start(s->ctx, tag ? (const char *)tag : "", tag ? (unsigned)tl : 0, at, n);
}

/* replay the children of top as start/text/end events (iterative: pages can
 * nest far deeper than a task stack allows recursion) */
static void walk(lxb_dom_node_t *top, const lxg_sink_t *s) {
    lxb_dom_node_t *n = top->first_child;
    while (n) {
        bool descend = false;
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            emit_start(lxb_dom_interface_element(n), s);
            if (n->first_child && n->local_name != LXB_TAG_TEMPLATE) descend = true;
            else s->end(s->ctx);
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_text_t *t = lxb_dom_interface_text(n);
            if (t->char_data.data.length) s->text(s->ctx, (const char *)t->char_data.data.data, (unsigned)t->char_data.data.length);
        }
        if (descend) { n = n->first_child; continue; }
        while (n && n != top && !n->next) {
            n = n->parent;
            if (n && n != top && n->type == LXB_DOM_NODE_TYPE_ELEMENT) s->end(s->ctx);
        }
        n = (n && n != top) ? n->next : NULL;
    }
}

int lxg_parse_html(const char *utf8, unsigned len, const lxg_sink_t *s) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return -1;
    /* scripting off: <noscript> content is parsed as markup, as XplorerLite always did;
     * the layout decides whether to show it */
    lxb_html_document_scripting_set(doc, false);
    if (lxb_html_document_parse(doc, (const lxb_char_t *)utf8, len) != LXB_STATUS_OK) { lxb_html_document_destroy(doc); return -1; }
    walk(lxb_dom_interface_node(doc), s);
    lxb_html_document_destroy(doc);
    return 0;
}

int lxg_parse_fragment(const char *context_tag, const char *utf8, unsigned len, const lxg_sink_t *s) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return -1;
    lxb_html_document_scripting_set(doc, false);
    const char *ct = context_tag && *context_tag ? context_tag : "body";
    lxb_dom_element_t *ctx = lxb_dom_document_create_element(lxb_dom_interface_document(doc), (const lxb_char_t *)ct, strlen(ct), NULL);
    lxb_dom_node_t *root = ctx ? lxb_html_document_parse_fragment(doc, ctx, (const lxb_char_t *)utf8, len) : NULL;
    if (!root) { lxb_html_document_destroy(doc); return -1; }
    walk(root, s);
    lxb_html_document_destroy(doc);
    return 0;
}

/* ---------------- selectors: parsing ---------------- */
static lxb_css_parser_t *parser;

void *lxg_pool_new(void) {
    lxb_css_memory_t *m = lxb_css_memory_create();
    if (!m) return NULL;
    if (lxb_css_memory_init(m, 128) != LXB_STATUS_OK) { lxb_css_memory_destroy(m, true); return NULL; }
    return m;
}
void lxg_pool_free(void *pool) { if (pool) lxb_css_memory_destroy(pool, true); }

static bool never_matches(const lxb_css_selector_t *s) {
    if (s->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT || s->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT_FUNCTION) return true;
    if (s->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS) {
        switch (s->u.pseudo.type) {           /* states a static page is never in */
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_HOVER: case LXB_CSS_SELECTOR_PSEUDO_CLASS_ACTIVE: case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS_VISIBLE: case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS_WITHIN:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_VISITED: case LXB_CSS_SELECTOR_PSEUDO_CLASS_TARGET: case LXB_CSS_SELECTOR_PSEUDO_CLASS_TARGET_WITHIN:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FULLSCREEN: case LXB_CSS_SELECTOR_PSEUDO_CLASS_PLACEHOLDER_SHOWN:
            return true;
        }
    }
    return false;
}

int lxg_selectors(void *pool, const char *sel, unsigned len, lxg_each_f each, void *ctx) {
    if (!pool) return -1;
    if (!parser) {
        parser = lxb_css_parser_create();
        if (lxb_css_parser_init(parser, NULL) != LXB_STATUS_OK) { lxb_css_parser_destroy(parser, true); parser = NULL; return -1; }
    }
    lxb_css_parser_memory_set(parser, pool);
    lxb_css_selector_list_t *list = lxb_css_selectors_parse(parser, (const lxb_char_t *)sel, len);
    lxb_css_parser_memory_set(parser, NULL);
    if (!list || parser->status != LXB_STATUS_OK) return -1;
    int count = 0;
    for (lxb_css_selector_list_t *cx = list; cx; cx = cx->next) {
        if (!cx->last) continue;
        bool skip = false;
        for (lxb_css_selector_t *s = cx->first; s; s = s->next) if (never_matches(s)) { skip = true; break; }
        if (skip) continue;
        /* bucket key from the right-most compound: an id, else a class, else the tag */
        int kind = LXG_KEY_ANY; const char *key = ""; unsigned kl = 0;
        for (lxb_css_selector_t *s = cx->last; s; s = s->prev) {
            if (s->type == LXB_CSS_SELECTOR_TYPE_ID) { kind = LXG_KEY_ID; key = (const char *)s->name.data; kl = (unsigned)s->name.length; }
            else if (s->type == LXB_CSS_SELECTOR_TYPE_CLASS && kind < LXG_KEY_CLASS) { kind = LXG_KEY_CLASS; key = (const char *)s->name.data; kl = (unsigned)s->name.length; }
            else if (s->type == LXB_CSS_SELECTOR_TYPE_ELEMENT && kind < LXG_KEY_TAG) { kind = LXG_KEY_TAG; key = (const char *)s->name.data; kl = (unsigned)s->name.length; }
            if (s->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE) break;
        }
        each(ctx, cx, cx->specificity & 0x07FFFFFFu, kind, key, kl);
        count++;
    }
    return count;
}

/* ---------------- selectors: matching ---------------- */
/* selector strings are UTF-8; the tree's strings are Latin-1 */
static unsigned to_l1(const lexbor_str_t *s, char *out, unsigned cap) {
    unsigned o = 0;
    for (size_t i = 0; i < s->length && o + 1 < cap;) {
        unsigned char c = s->data[i];
        if (c < 0x80) { out[o++] = (char)c; i++; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s->length) { unsigned cp = ((c & 0x1Fu) << 6) | (s->data[i + 1] & 0x3Fu); out[o++] = cp <= 0xFF ? (char)cp : '?'; i += 2; }
        else { out[o++] = '?'; i++; while (i < s->length && (s->data[i] & 0xC0) == 0x80) i++; }
    }
    out[o] = 0;
    return o;
}
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool eq(const char *a, const char *b, unsigned n, bool ci) {
    for (unsigned i = 0; i < n; i++) if (ci ? lc((unsigned char)a[i]) != lc((unsigned char)b[i]) : a[i] != b[i]) return false;
    return true;
}
static bool has_word(const char *list, const char *w, unsigned wl, bool ci) {
    if (!list || !wl) return false;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r' && *e != '\f') e++;
        if ((unsigned)(e - p) == wl && eq(p, w, wl, ci)) return true;
        p = e;
    }
    return false;
}

static bool match_complex(const lxb_css_selector_t *last, void *n, const lxg_tree_t *t, int depth);
static bool match_list(const lxb_css_selector_list_t *l, void *n, const lxg_tree_t *t, int depth) {
    for (; l; l = l->next) if (l->last && match_complex(l->last, n, t, depth + 1)) return true;
    return false;
}

/* 1-based position among element siblings; from_end counts from the last; of: only siblings matching it */
static long position(void *n, const lxg_tree_t *t, bool from_end, bool same_type, const lxb_css_selector_list_t *of, int depth) {
    long i = 1;
    const char *tag = same_type ? t->tag(n) : NULL;
    for (void *s = from_end ? t->next(n) : t->prev(n); s; s = from_end ? t->next(s) : t->prev(s)) {
        if (same_type && strcmp(t->tag(s), tag)) continue;
        if (of && !match_list(of, s, t, depth)) continue;
        i++;
    }
    return i;
}
static bool anb(long a, long b, long i) {
    if (a == 0) return i == b;
    long d = i - b;
    return d % a == 0 && d / a >= 0;
}

static bool match_simple(const lxb_css_selector_t *s, void *n, const lxg_tree_t *t, int depth) {
    char buf[256];
    switch (s->type) {
    case LXB_CSS_SELECTOR_TYPE_ANY: return true;
    case LXB_CSS_SELECTOR_TYPE_ELEMENT: {
        unsigned l = to_l1(&s->name, buf, sizeof buf);
        if (l == 1 && buf[0] == '*') return true;
        const char *tag = t->tag(n);
        return strlen(tag) == l && eq(tag, buf, l, true);
    }
    case LXB_CSS_SELECTOR_TYPE_ID: {
        unsigned l = to_l1(&s->name, buf, sizeof buf);
        const char *id = t->attr(n, "id");
        return id && strlen(id) == l && eq(id, buf, l, false);
    }
    case LXB_CSS_SELECTOR_TYPE_CLASS: {
        unsigned l = to_l1(&s->name, buf, sizeof buf);
        return has_word(t->attr(n, "class"), buf, l, false);
    }
    case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE: {
        char name[64], val[256];
        unsigned nl = to_l1(&s->name, name, sizeof name);
        for (unsigned i = 0; i < nl; i++) name[i] = (char)lc((unsigned char)name[i]);
        const char *v = t->attr(n, name);
        if (!v) return false;
        if (!s->u.attribute.value.data) return true;                 /* [name] */
        unsigned vl = to_l1(&s->u.attribute.value, val, sizeof val);
        bool ci = s->u.attribute.modifier == LXB_CSS_SELECTOR_MODIFIER_I;
        unsigned L = (unsigned)strlen(v);
        switch (s->u.attribute.match) {
        case LXB_CSS_SELECTOR_MATCH_EQUAL: return L == vl && eq(v, val, vl, ci);
        case LXB_CSS_SELECTOR_MATCH_INCLUDE: return has_word(v, val, vl, ci);
        case LXB_CSS_SELECTOR_MATCH_DASH: return L >= vl && eq(v, val, vl, ci) && (L == vl || v[vl] == '-');
        case LXB_CSS_SELECTOR_MATCH_PREFIX: return vl && L >= vl && eq(v, val, vl, ci);
        case LXB_CSS_SELECTOR_MATCH_SUFFIX: return vl && L >= vl && eq(v + L - vl, val, vl, ci);
        case LXB_CSS_SELECTOR_MATCH_SUBSTRING:
            if (!vl) return false;
            for (unsigned i = 0; i + vl <= L; i++) if (eq(v + i, val, vl, ci)) return true;
            return false;
        default: return false;
        }
    }
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS:
        switch (s->u.pseudo.type) {
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_CHILD: return !t->prev(n);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_CHILD: return !t->next(n);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_CHILD: return !t->prev(n) && !t->next(n);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_OF_TYPE: return position(n, t, false, true, NULL, depth) == 1;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_OF_TYPE: return position(n, t, true, true, NULL, depth) == 1;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_OF_TYPE: return position(n, t, false, true, NULL, depth) == 1 && position(n, t, true, true, NULL, depth) == 1;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_EMPTY: return t->is_empty(n);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ROOT: return t->is_root(n);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_LINK: case LXB_CSS_SELECTOR_PSEUDO_CLASS_ANY_LINK: {
            const char *tag = t->tag(n);
            return (!strcmp(tag, "a") || !strcmp(tag, "area")) && t->attr(n, "href");
        }
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_CHECKED: return t->attr(n, "checked") || t->attr(n, "selected");
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_DISABLED: return t->attr(n, "disabled") != NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_ENABLED: return t->attr(n, "disabled") == NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_REQUIRED: return t->attr(n, "required") != NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_OPTIONAL: return t->attr(n, "required") == NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_READ_ONLY: return t->attr(n, "readonly") != NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_READ_WRITE: return t->attr(n, "readonly") == NULL;
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_DEFAULT: return t->attr(n, "checked") || t->attr(n, "selected");
        default: return false;
        }
    case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS_FUNCTION: {
        if (depth > 16) return false;
        const lxb_css_selector_anb_of_t *ab = s->u.pseudo.data;
        switch (s->u.pseudo.type) {
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT: return !match_list(s->u.pseudo.data, n, t, depth);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE: return match_list(s->u.pseudo.data, n, t, depth);
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD: return ab && (!ab->of || match_list(ab->of, n, t, depth)) && anb(ab->anb.a, ab->anb.b, position(n, t, false, false, ab->of, depth));
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD: return ab && (!ab->of || match_list(ab->of, n, t, depth)) && anb(ab->anb.a, ab->anb.b, position(n, t, true, false, ab->of, depth));
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE: return ab && anb(ab->anb.a, ab->anb.b, position(n, t, false, true, NULL, depth));
        case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE: return ab && anb(ab->anb.a, ab->anb.b, position(n, t, true, true, NULL, depth));
        default: return false;                         /* :has(), :lang(), :dir() ... */
        }
    }
    default: return false;                             /* pseudo-elements never match an element */
    }
}

/* right to left: the compound ending at `last`, then the combinator before it */
static bool match_complex(const lxb_css_selector_t *last, void *n, const lxg_tree_t *t, int depth) {
    const lxb_css_selector_t *s = last;
    for (;;) {
        if (!match_simple(s, n, t, depth)) return false;
        if (s->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE || !s->prev) break;
        s = s->prev;
    }
    const lxb_css_selector_t *pl = s->prev;
    if (!pl) return true;
    switch (s->combinator) {
    case LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT:
        for (void *a = t->parent(n); a; a = t->parent(a)) if (match_complex(pl, a, t, depth)) return true;
        return false;
    case LXB_CSS_SELECTOR_COMBINATOR_CHILD: { void *a = t->parent(n); return a && match_complex(pl, a, t, depth); }
    case LXB_CSS_SELECTOR_COMBINATOR_SIBLING: { void *a = t->prev(n); return a && match_complex(pl, a, t, depth); }
    case LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING:
        for (void *a = t->prev(n); a; a = t->prev(a)) if (match_complex(pl, a, t, depth)) return true;
        return false;
    default: return false;
    }
}

int lxg_match(void *sel, void *node, const lxg_tree_t *tree) {
    const lxb_css_selector_list_t *cx = sel;
    return cx && cx->last && match_complex(cx->last, node, tree, 0);
}

const char *lxg_version(void) { return "Lexbor " LEXBOR_VERSION_STRING; }
