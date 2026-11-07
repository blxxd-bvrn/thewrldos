/* The WRLD Xplorer: the document tree as seen from JavaScript (jsapi.h dom_*).
 * Nodes live in the page's arena and are never freed before the page, so a
 * node pointer stays valid for the page's lifetime. Strings to and from the
 * engine are UTF-8; the tree itself stores Latin-1. */
#include "kernel.h"
#include "fs.h"
#include "net.h"
#include "wm.h"
#include "browser.h"
#include "jsapi.h"

#define PG ((page_t *)page)
#define ND(x) ((node_t *)(x))

/* ---------------- strings ---------------- */
static char *to_utf8(const char *s, int n) {
    if (!s) return NULL;
    if (n < 0) n = strlen(s);
    char *o = kmalloc(n * 2 + 1); int k = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c < 0x80) o[k++] = c; else { o[k++] = 0xC0 | (c >> 6); o[k++] = 0x80 | (c & 0x3F); }
    }
    o[k] = 0;
    return o;
}
static char *to_latin1(page_t *p, const char *u) {      /* into the page arena */
    int n = strlen(u);
    char *tmp = kmalloc(n * 3 + 1);
    int l = latin1_from_utf8((const uint8_t *)u, n, tmp);
    char *r = arena_strdup(&p->doc->arena, tmp, l);
    kfree(tmp);
    return r;
}
void dom_free(char *s) { kfree(s); }
void dom_changed(void *page) { if (page) PG->tree_dirty = true; }

/* ---------------- navigation ---------------- */
void *dom_root(void *page) { return PG->doc->root; }
/* the tree's root stands in for <body> (the parser folds html/head/body away) */
int dom_node_type(void *n) { return ND(n)->type == 1 ? 3 : 1; }
const char *dom_tag(void *n) { return ND(n)->type == 1 ? "#text" : !strcmp(ND(n)->tag, "#doc") ? "body" : ND(n)->tag; }
void *dom_parent(void *n) { return ND(n)->parent; }
void *dom_first(void *n) { return ND(n)->first; }
void *dom_last(void *n) { return ND(n)->last; }
void *dom_next(void *n) { return ND(n)->next; }
void *dom_prev(void *n) {
    node_t *x = ND(n), *p = x->parent;
    if (!p) return NULL;
    node_t *prev = NULL;
    for (node_t *c = p->first; c && c != x; c = c->next) prev = c;
    return prev;
}
/* next node in document order inside root (NULL at the end) */
static node_t *walk_next(node_t *n, node_t *root) {
    if (n->first) return n->first;
    while (n && n != root) { if (n->next) return n->next; n = n->parent; }
    return NULL;
}
void *dom_by_id(void *page, const char *id) {
    char *want = to_latin1(PG, id);
    node_t *root = PG->doc->root;
    for (node_t *n = root->first; n; n = walk_next(n, root)) {
        if (n->type) continue;
        const char *v = node_attr(n, "id");
        if (v && !strcmp(v, want)) return n;
    }
    return NULL;
}

/* ---------------- attributes ---------------- */
char *dom_get_attr(void *n, const char *name) {
    if (ND(n)->type) return NULL;
    const char *v = node_attr(ND(n), name);
    return v ? to_utf8(v, -1) : NULL;
}
static void set_attr_l1(page_t *p, node_t *n, const char *name, char *val) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attr[i * 2], name)) { n->attr[i * 2 + 1] = val; return; }
    char **na = arena_alloc(&p->doc->arena, sizeof(char *) * (n->nattr * 2 + 4));
    if (n->nattr) memcpy(na, n->attr, sizeof(char *) * n->nattr * 2);
    na[n->nattr * 2] = arena_strdup(&p->doc->arena, name, -1);
    na[n->nattr * 2 + 1] = val;
    n->attr = na; n->nattr++;
}
static void attr_changed(page_t *p, node_t *n, const char *name) {
    if (!strcmp(name, "class") || !strcmp(name, "style") || !strcmp(name, "id") || !strcmp(name, "hidden")) html_restyle(p->doc, n);
    if (n->aux > 0 && !strcmp(n->tag, "input") && (!strcmp(name, "value") || !strcmp(name, "checked"))) {
        ctrl_t *c = n->aux - 1 < p->nctrls ? &p->ctrls[n->aux - 1] : NULL;
        if (c && c->node == n) {
            const char *v = node_attr(n, "value");
            if (!strcmp(name, "value") && v) strlcpy(c->value, v, sizeof c->value);
            if (!strcmp(name, "checked")) c->checked = node_attr(n, "checked") != NULL;
        }
    }
    p->tree_dirty = true;
}
void dom_set_attr(void *page, void *n, const char *name, const char *value) {
    if (ND(n)->type || !name[0]) return;
    char nm[40]; int k = 0;
    for (; name[k] && k < 39; k++) nm[k] = tolower(name[k]);
    nm[k] = 0;
    set_attr_l1(PG, ND(n), nm, to_latin1(PG, value));
    attr_changed(PG, ND(n), nm);
}
void dom_remove_attr(void *page, void *n, const char *name) {
    node_t *x = ND(n);
    for (int i = 0; i < x->nattr; i++) if (!strcasecmp(x->attr[i * 2], name)) {
        memmove(&x->attr[i * 2], &x->attr[i * 2 + 2], sizeof(char *) * (x->nattr - i - 1) * 2);
        x->nattr--;
        attr_changed(PG, x, name);
        return;
    }
}
int dom_attr_count(void *n) { return ND(n)->type ? 0 : ND(n)->nattr; }
char *dom_attr_name(void *n, int i) { return i >= 0 && i < ND(n)->nattr ? to_utf8(ND(n)->attr[i * 2], -1) : NULL; }

/* ---------------- selectors ---------------- */
/* compound: tag? (#id | .class | [attr op "v"] | :pseudo(arg))*  ; combinators ' ' '>' '+' '~' */
static bool has_class_l1(node_t *n, const char *cls, int len) {
    const char *c = node_attr(n, "class");
    while (c && *c) {
        while (*c == ' ' || *c == '\t' || *c == '\n') c++;
        const char *e = c; while (*e && *e != ' ' && *e != '\t' && *e != '\n') e++;
        if (e - c == len && !strncmp(c, cls, len)) return true;
        c = e;
    }
    return false;
}
static int elem_index(node_t *n, int from_end, bool same_type) {
    int i = 0;
    if (!n->parent) return 1;
    if (!from_end) { for (node_t *c = n->parent->first; c; c = c->next) { if (c->type || (same_type && strcmp(c->tag, n->tag))) continue; i++; if (c == n) return i; } }
    else { bool after = false; for (node_t *c = n->parent->first; c; c = c->next) { if (c == n) after = true; if (!after || c->type || (same_type && strcmp(c->tag, n->tag))) continue; i++; } return i; }
    return i;
}
static bool nth_match(int idx, const char *a, int alen) {
    char arg[24]; int k = 0;
    for (int i = 0; i < alen && k < 23; i++) if (a[i] != ' ') arg[k++] = tolower(a[i]);
    arg[k] = 0;
    if (!strcmp(arg, "odd")) return idx % 2 == 1;
    if (!strcmp(arg, "even")) return idx % 2 == 0;
    char *nx = strchr(arg, 'n');
    if (!nx) return idx == atoi(arg);
    int A = nx == arg ? 1 : (nx == arg + 1 && arg[0] == '-') ? -1 : (nx == arg + 1 && arg[0] == '+') ? 1 : atoi(arg);
    int B = nx[1] ? atoi(nx + 1) : 0;
    if (A == 0) return idx == B;
    return (idx - B) % A == 0 && (idx - B) / A >= 0;
}
static bool match_list(node_t *n, const char *sel, int len);
static bool match_compound(node_t *n, const char *s, int len) {
    if (n->type || !strcmp(n->tag, "#doc")) return false;
    const char *e = s + len;
    if (s < e && (isalpha(*s) || *s == '*')) {
        const char *t = s; while (s < e && (isalnum(*s) || *s == '-' || *s == '_' || *s == '*')) s++;
        if (!(s - t == 1 && *t == '*') && (strncasecmp(n->tag, t, s - t) || n->tag[s - t])) return false;
    }
    while (s < e) {
        char k = *s++;
        if (k == '#' || k == '.') {
            const char *t = s; while (s < e && (isalnum(*s) || *s == '-' || *s == '_' || (uint8_t)*s >= 0x80 || *s == '\\')) s++;
            if (k == '#') { const char *id = node_attr(n, "id"); if (!id || (int)strlen(id) != s - t || strncmp(id, t, s - t)) return false; }
            else if (!has_class_l1(n, t, s - t)) return false;
        } else if (k == '[') {
            const char *an = s; while (s < e && *s != ']' && *s != '=' && *s != '~' && *s != '^' && *s != '$' && *s != '*' && *s != '|') s++;
            char name[40]; int nl = MIN((int)(s - an), 39); for (int i = 0; i < nl; i++) name[i] = tolower(an[i]);
            while (nl > 0 && name[nl - 1] == ' ') nl--;
            name[nl] = 0;
            char op = 0;
            if (s < e && *s != ']' && *s != '=') op = *s++;
            if (s < e && *s == '=') { s++; if (!op) op = '='; }
            char val[256]; int vl = 0;
            if (op) {
                while (s < e && *s == ' ') s++;
                char q = (s < e && (*s == '"' || *s == '\'')) ? *s++ : 0;
                while (s < e && (q ? *s != q : (*s != ']' && *s != ' ')) && vl < 255) val[vl++] = *s++;
                if (q && s < e) s++;
            }
            val[vl] = 0;
            while (s < e && *s != ']') s++;
            if (s < e) s++;
            const char *v = node_attr(n, name);
            if (!v) return false;
            int L = strlen(v);
            switch (op) {
            case 0: break;
            case '=': if (strcmp(v, val)) return false; break;
            case '^': if (strncmp(v, val, vl)) return false; break;
            case '$': if (L < vl || strcmp(v + L - vl, val)) return false; break;
            case '*': if (!strstr(v, val)) return false; break;
            case '~': { node_t tmp = *n; char *cv[2] = { "class", (char *)v }; tmp.attr = cv; tmp.nattr = 1; if (!has_class_l1(&tmp, val, vl)) return false; break; }
            case '|': if (strncmp(v, val, vl) || (v[vl] && v[vl] != '-')) return false; break;
            }
        } else if (k == ':') {
            if (s < e && *s == ':') s++;
            const char *pn = s; while (s < e && (isalnum(*s) || *s == '-')) s++;
            int pl = s - pn;
            const char *arg = NULL; int al = 0;
            if (s < e && *s == '(') {
                int depth = 1; arg = ++s;
                while (s < e && depth) { if (*s == '(') depth++; else if (*s == ')') depth--; if (depth) s++; }
                al = s - arg; if (s < e) s++;
            }
            #define PIS(x) (pl == (int)strlen(x) && !strncasecmp(pn, x, pl))
            if (PIS("first-child")) { if (elem_index(n, 0, false) != 1) return false; }
            else if (PIS("last-child")) { if (elem_index(n, 1, false) != 1) return false; }
            else if (PIS("only-child")) { if (elem_index(n, 0, false) != 1 || elem_index(n, 1, false) != 1) return false; }
            else if (PIS("first-of-type")) { if (elem_index(n, 0, true) != 1) return false; }
            else if (PIS("last-of-type")) { if (elem_index(n, 1, true) != 1) return false; }
            else if (PIS("nth-child")) { if (!arg || !nth_match(elem_index(n, 0, false), arg, al)) return false; }
            else if (PIS("nth-last-child")) { if (!arg || !nth_match(elem_index(n, 1, false), arg, al)) return false; }
            else if (PIS("nth-of-type")) { if (!arg || !nth_match(elem_index(n, 0, true), arg, al)) return false; }
            else if (PIS("not")) { if (arg && match_list(n, arg, al)) return false; }
            else if (PIS("is") || PIS("where") || PIS("matches")) { if (!arg || !match_list(n, arg, al)) return false; }
            else if (PIS("checked")) { if (!node_attr(n, "checked") && !node_attr(n, "selected")) return false; }
            else if (PIS("disabled")) { if (!node_attr(n, "disabled")) return false; }
            else if (PIS("enabled")) { if (node_attr(n, "disabled")) return false; }
            else if (PIS("empty")) { if (n->first) return false; }
            else if (PIS("root")) { if (n->parent && strcmp(n->parent->tag, "#doc")) return false; }
            else if (PIS("link") || PIS("any-link")) { if (strcmp(n->tag, "a") || !node_attr(n, "href")) return false; }
            else if (PIS("hover") || PIS("active") || PIS("focus") || PIS("visited") || PIS("focus-within") || PIS("focus-visible")) return false;
            /* other pseudo-classes and pseudo-elements: ignored */
            #undef PIS
        } else if (k == ' ') continue;
        else return false;
    }
    return true;
}
/* one complex selector (no commas) */
static bool match_complex(node_t *n, const char *s, int len) {
    while (len > 0 && s[len - 1] == ' ') len--;
    while (len > 0 && *s == ' ') { s++; len--; }
    if (len <= 0) return false;
    /* find the last combinator outside brackets/parentheses */
    int depth = 0, cut = -1; char comb = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '[' || c == '(') depth++;
        else if (c == ']' || c == ')') depth--;
        else if (!depth && (c == '>' || c == '+' || c == '~')) { cut = i; comb = c; }
        else if (!depth && c == ' ') {
            int j = i; while (j < len && s[j] == ' ') j++;
            if (j < len && s[j] != '>' && s[j] != '+' && s[j] != '~' && i > 0 && s[i - 1] != '>' && s[i - 1] != '+' && s[i - 1] != '~') { cut = i; comb = ' '; }
            i = j - 1;
        }
    }
    if (cut < 0) return match_compound(n, s, len);
    const char *last = s + cut + 1; int ll = len - cut - 1;
    while (ll > 0 && *last == ' ') { last++; ll--; }
    if (!match_compound(n, last, ll)) return false;
    int pl = cut;
    switch (comb) {
    case '>': return n->parent && match_complex(n->parent, s, pl);
    case ' ': for (node_t *a = n->parent; a; a = a->parent) if (match_complex(a, s, pl)) return true; return false;
    case '+': { node_t *pv = dom_prev(n); while (pv && pv->type) pv = dom_prev(pv); return pv && match_complex(pv, s, pl); }
    case '~': for (node_t *pv = dom_prev(n); pv; pv = dom_prev(pv)) if (!pv->type && match_complex(pv, s, pl)) return true; return false;
    }
    return false;
}
static bool match_list(node_t *n, const char *sel, int len) {
    int depth = 0, start = 0;
    for (int i = 0; i <= len; i++) {
        char c = i < len ? sel[i] : ',';
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        else if (c == ',' && !depth) { if (match_complex(n, sel + start, i - start)) return true; start = i + 1; }
    }
    return false;
}
int dom_matches(void *n, const char *sel) {
    if (ND(n)->type) return 0;
    char *l1 = kmalloc(strlen(sel) * 3 + 1);
    int l = latin1_from_utf8((const uint8_t *)sel, strlen(sel), l1);
    bool r = match_list(ND(n), l1, l);
    kfree(l1);
    return r;
}
int dom_query(void *page, void *root, const char *sel, void **out, int max, int first_only) {
    node_t *r = root ? ND(root) : PG->doc->root;
    char *l1 = kmalloc(strlen(sel) * 3 + 1);
    int l = latin1_from_utf8((const uint8_t *)sel, strlen(sel), l1);
    int cnt = 0;
    for (node_t *n = r->first; n && cnt < max; n = walk_next(n, r)) {
        if (n->type) continue;
        if (match_list(n, l1, l)) { out[cnt++] = n; if (first_only) break; }
    }
    kfree(l1);
    return cnt;
}

/* ---------------- text and markup ---------------- */
static void text_of(node_t *n, char **buf, int *len, int *cap) {
    for (node_t *c = n->first; c; c = c->next) {
        if (c->type == 1) {
            int l = strlen(c->text);
            if (*len + l + 1 > *cap) { *cap = (*len + l + 1) * 2; *buf = krealloc(*buf, *cap); }
            memcpy(*buf + *len, c->text, l); *len += l; (*buf)[*len] = 0;
        } else if (strcmp(c->tag, "script") && strcmp(c->tag, "style")) text_of(c, buf, len, cap);
        else if (n->type == 0 && (!strcmp(n->tag, "script"))) text_of(c, buf, len, cap);
    }
}
char *dom_text(void *n) {
    node_t *x = ND(n);
    if (x->type == 1) return to_utf8(x->text, -1);
    int len = 0, cap = 256; char *b = kmalloc(cap); b[0] = 0;
    if (!strcmp(x->tag, "script") || !strcmp(x->tag, "style")) { for (node_t *c = x->first; c; c = c->next) if (c->type == 1) { int l = strlen(c->text); if (len + l + 1 > cap) { cap = (len + l + 1) * 2; b = krealloc(b, cap); } memcpy(b + len, c->text, l); len += l; b[len] = 0; } }
    else text_of(x, &b, &len, &cap);
    char *u = to_utf8(b, len);
    kfree(b);
    return u;
}
static node_t *new_node(page_t *p, int type, const char *tag) {
    node_t *n = arena_alloc(&p->doc->arena, sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = type;
    if (tag) { int k = 0; for (; tag[k] && k < 11; k++) n->tag[k] = tolower(tag[k]); n->tag[k] = 0; }
    return n;
}
static void unlink(node_t *c) {
    node_t *p = c->parent;
    if (!p) return;
    node_t *prev = NULL;
    for (node_t *x = p->first; x && x != c; x = x->next) prev = x;
    if (prev) prev->next = c->next; else p->first = c->next;
    if (p->last == c) p->last = prev;
    c->parent = NULL; c->next = NULL;
}
static void link_before(node_t *p, node_t *c, node_t *before) {
    c->parent = p;
    if (!before || before->parent != p) {
        c->next = NULL;
        if (p->last) p->last->next = c; else p->first = c;
        p->last = c;
        return;
    }
    node_t *prev = NULL;
    for (node_t *x = p->first; x && x != before; x = x->next) prev = x;
    c->next = before;
    if (prev) prev->next = c; else p->first = c;
}
static void mark_scripts(page_t *p, node_t *n) {           /* script elements added later still run */
    if (n->type) return;
    if (!strcmp(n->tag, "script") && n->aux == 0) p->scripts_pending = true;
    for (node_t *c = n->first; c; c = c->next) mark_scripts(p, c);
}
void dom_set_text(void *page, void *n, const char *text) {
    node_t *x = ND(n);
    if (x->type == 1) { x->text = to_latin1(PG, text); PG->tree_dirty = true; return; }
    while (x->first) unlink(x->first);
    if (*text) { node_t *t = new_node(PG, 1, NULL); t->text = to_latin1(PG, text); link_before(x, t, NULL); }
    if (!strcmp(x->tag, "title") || (x->parent && !strcmp(x->tag, "title"))) { strlcpy(PG->title, x->first ? x->first->text : "", sizeof PG->title); PG->title_dirty = true; }
    PG->tree_dirty = true;
}
/* serialisation */
typedef struct { char *b; int len, cap; } sbuf_t;
static void sb_put(sbuf_t *s, const char *t, int n) {
    if (n < 0) n = strlen(t);
    if (s->len + n + 1 > s->cap) { s->cap = (s->len + n + 1) * 2; s->b = krealloc(s->b, s->cap); }
    memcpy(s->b + s->len, t, n); s->len += n; s->b[s->len] = 0;
}
static void sb_esc(sbuf_t *s, const char *t, bool attr) {
    for (; *t; t++) {
        if (*t == '&') sb_put(s, "&amp;", 5);
        else if (*t == '<') sb_put(s, "&lt;", 4);
        else if (*t == '>') sb_put(s, "&gt;", 4);
        else if (attr && *t == '"') sb_put(s, "&quot;", 6);
        else sb_put(s, t, 1);
    }
}
static void serialize(sbuf_t *s, node_t *n, bool raw) {
    if (n->type == 1) { if (raw) sb_put(s, n->text, -1); else sb_esc(s, n->text, false); return; }
    sb_put(s, "<", 1); sb_put(s, n->tag, -1);
    for (int i = 0; i < n->nattr; i++) { sb_put(s, " ", 1); sb_put(s, n->attr[i * 2], -1); sb_put(s, "=\"", 2); sb_esc(s, n->attr[i * 2 + 1], true); sb_put(s, "\"", 1); }
    sb_put(s, ">", 1);
    if (html_is_void(n->tag)) return;
    bool r = !strcmp(n->tag, "script") || !strcmp(n->tag, "style");
    for (node_t *c = n->first; c; c = c->next) serialize(s, c, r);
    sb_put(s, "</", 2); sb_put(s, n->tag, -1); sb_put(s, ">", 1);
}
char *dom_inner_html(void *n, int outer) {
    node_t *x = ND(n);
    sbuf_t s = { kmalloc(256), 0, 256 }; s.b[0] = 0;
    if (outer || x->type == 1) serialize(&s, x, false);
    else { bool r = !strcmp(x->tag, "script") || !strcmp(x->tag, "style"); for (node_t *c = x->first; c; c = c->next) serialize(&s, c, r); }
    char *u = to_utf8(s.b, s.len);
    kfree(s.b);
    return u;
}
/* copy a parsed fragment's nodes into the page's arena */
static node_t *copy_tree(page_t *p, node_t *src) {
    node_t *n = new_node(p, src->type, NULL);
    memcpy(n->tag, src->tag, sizeof n->tag);
    if (src->type == 1) n->text = arena_strdup(&p->doc->arena, src->text, -1);
    else if (src->nattr) {
        n->attr = arena_alloc(&p->doc->arena, sizeof(char *) * (src->nattr * 2 + 2)); n->nattr = src->nattr;
        for (int i = 0; i < src->nattr * 2; i++) n->attr[i] = arena_strdup(&p->doc->arena, src->attr[i], -1);
    }
    n->css = src->css;
    for (node_t *c = src->first; c; c = c->next) { node_t *k = copy_tree(p, c); link_before(n, k, NULL); }
    return n;
}
static int insert_fragment(page_t *p, node_t *parent, node_t *before, const char *html, bool run_scripts) {
    doc_t *d = html_parse(html, strlen(html), "utf-8", p->url);
    int cnt = 0;
    for (node_t *c = d->root->first; c; c = c->next) {
        node_t *k = copy_tree(p, c);
        if (!run_scripts) { /* scripts set through innerHTML never run */
            node_t *r = k; for (node_t *x = r; x; x = walk_next(x, r)) if (!x->type && !strcmp(x->tag, "script")) x->aux = -1;
        }
        link_before(parent, k, before);
        if (!k->type) { html_restyle(p->doc, k); mark_scripts(p, k); }
        cnt++;
    }
    /* <style> blocks in the fragment add rules to the page */
    if (d->nrules) {
        int total = p->doc->nrules + d->nrules, cap = (total + 63) / 64 * 64;
        css_rule_t *nr = arena_alloc(&p->doc->arena, sizeof(css_rule_t) * cap);
        if (p->doc->nrules) memcpy(nr, p->doc->rules, sizeof(css_rule_t) * p->doc->nrules);
        memcpy(nr + p->doc->nrules, d->rules, sizeof(css_rule_t) * d->nrules);
        p->doc->rules = nr; p->doc->nrules = total;
    }
    doc_free(d);
    p->tree_dirty = true;
    return cnt;
}
int dom_set_inner_html(void *page, void *n, const char *html) {
    node_t *x = ND(n);
    if (x->type == 1) { dom_set_text(page, n, html); return 0; }
    while (x->first) unlink(x->first);
    return insert_fragment(PG, x, NULL, html, false);
}
int dom_insert_html(void *page, void *n, const char *where, const char *html) {
    node_t *x = ND(n);
    bool from_write = !strcmp(where, "write");          /* document.write output runs its scripts */
    if (!strcmp(where, "beforebegin") && x->parent) return insert_fragment(PG, x->parent, x, html, false);
    if ((!strcmp(where, "afterend") || from_write) && x->parent) return insert_fragment(PG, x->parent, x->next, html, from_write);
    if (!strcmp(where, "afterbegin")) return insert_fragment(PG, x, x->first, html, false);
    if (!strcmp(where, "beforeend")) return insert_fragment(PG, x, NULL, html, false);
    return 0;
}
void *dom_create(void *page, const char *tag) {
    char t[12]; int k = 0;
    for (; tag[k] && k < 11; k++) t[k] = tolower(tag[k]);
    t[k] = 0;
    return new_node(PG, 0, t);
}
void *dom_create_text(void *page, const char *text) { node_t *n = new_node(PG, 1, NULL); n->text = to_latin1(PG, text); return n; }
static node_t *clone_node(page_t *p, node_t *src, bool deep) {
    node_t *n = new_node(p, src->type, NULL);
    memcpy(n->tag, src->tag, sizeof n->tag);
    n->text = src->text;
    if (src->nattr) { n->attr = arena_alloc(&p->doc->arena, sizeof(char *) * (src->nattr * 2 + 2)); memcpy(n->attr, src->attr, sizeof(char *) * src->nattr * 2); n->nattr = src->nattr; }
    n->css = src->css;
    if (deep) for (node_t *c = src->first; c; c = c->next) link_before(n, clone_node(p, c, true), NULL);
    return n;
}
void *dom_clone(void *page, void *n, int deep) { return clone_node(PG, ND(n), deep); }
int dom_insert(void *page, void *parent, void *child, void *before) {
    node_t *p = ND(parent), *c = ND(child), *b = ND(before);
    for (node_t *a = p; a; a = a->parent) if (a == c) return -1;     /* would make a cycle */
    if (p->type == 1) return -1;
    if (b == c) return 0;
    unlink(c);
    link_before(p, c, b);
    if (!c->type) { html_restyle(PG->doc, c); mark_scripts(PG, c); }
    PG->tree_dirty = true;
    return 0;
}
int dom_remove(void *page, void *child) { unlink(ND(child)); PG->tree_dirty = true; return 0; }

/* ---------------- inline style ---------------- */
static void style_rewrite(page_t *p, node_t *n, const char *prop, const char *value) {
    const char *old = node_attr(n, "style"); if (!old) old = "";
    char out[1024]; int o = 0; out[0] = 0;
    const char *s = old;
    while (*s) {
        const char *e = strchr(s, ';'); if (!e) e = s + strlen(s);
        const char *colon = memchr(s, ':', e - s);
        if (colon) {
            const char *a = s; while (a < colon && *a == ' ') a++;
            const char *b = colon; while (b > a && b[-1] == ' ') b--;
            bool same = (int)strlen(prop) == b - a && !strncasecmp(a, prop, b - a);
            if (!same && o + (e - s) + 2 < (int)sizeof out) { memcpy(out + o, a, e - a); o += e - a; out[o++] = ';'; out[o] = 0; }
        }
        s = *e ? e + 1 : e;
    }
    if (value && *value) o += snprintf(out + o, sizeof out - o, "%s:%s;", prop, value);
    set_attr_l1(p, n, "style", arena_strdup(&p->doc->arena, out, -1));
    html_restyle(p->doc, n);
    p->tree_dirty = true;
}
void dom_set_style(void *page, void *n, const char *prop, const char *value) {
    if (ND(n)->type) return;
    char *v = to_latin1(PG, value);
    char pr[48]; strlcpy(pr, prop, sizeof pr);
    style_rewrite(PG, ND(n), pr, v);
}
char *dom_get_style(void *n, const char *prop) {
    const char *s = ND(n)->type ? NULL : node_attr(ND(n), "style");
    if (!s) return NULL;
    while (*s) {
        const char *e = strchr(s, ';'); if (!e) e = s + strlen(s);
        const char *colon = memchr(s, ':', e - s);
        if (colon) {
            const char *a = s; while (a < colon && *a == ' ') a++;
            const char *b = colon; while (b > a && b[-1] == ' ') b--;
            if ((int)strlen(prop) == b - a && !strncasecmp(a, prop, b - a)) {
                const char *v = colon + 1; while (v < e && *v == ' ') v++;
                const char *ve = e; while (ve > v && ve[-1] == ' ') ve--;
                return to_utf8(v, ve - v);
            }
        }
        s = *e ? e + 1 : e;
    }
    return NULL;
}

/* ---------------- form controls ---------------- */
static ctrl_t *ctrl_of(page_t *p, node_t *n) {
    if (n->type || n->aux <= 0 || n->aux > p->nctrls) return NULL;
    ctrl_t *c = &p->ctrls[n->aux - 1];
    return c->node == n ? c : NULL;
}
char *dom_get_value(void *page, void *n) {
    node_t *x = ND(n);
    ctrl_t *c = ctrl_of(PG, x);
    if (c) {
        if (c->type == C_SELECT) return to_utf8(c->nopts ? c->optvals[c->sel] : "", -1);
        return to_utf8(c->value, -1);
    }
    if (!strcmp(x->tag, "textarea")) return dom_text(n);
    if (!strcmp(x->tag, "option") && !node_attr(x, "value")) return dom_text(n);
    if (!strcmp(x->tag, "select")) {   /* not collected yet: first selected option */
        node_t *first = NULL;
        for (node_t *o = x->first; o; o = walk_next(o, x)) if (!o->type && !strcmp(o->tag, "option")) { if (!first) first = o; if (node_attr(o, "selected")) { first = o; break; } }
        return first ? dom_get_value(page, first) : to_utf8("", 0);
    }
    const char *v = node_attr(x, "value");
    return to_utf8(v ? v : "", -1);
}
void dom_set_value(void *page, void *n, const char *value) {
    node_t *x = ND(n);
    ctrl_t *c = ctrl_of(PG, x);
    char *l1 = to_latin1(PG, value);
    if (c) {
        if (c->type == C_SELECT) { for (int i = 0; i < c->nopts; i++) if (!strcmp(c->optvals[i], l1)) c->sel = i; }
        else strlcpy(c->value, l1, sizeof c->value);
    } else if (!strcmp(x->tag, "textarea")) dom_set_text(page, n, value);
    else set_attr_l1(PG, x, "value", l1);
    PG->tree_dirty = true;
}
int dom_get_checked(void *page, void *n) { ctrl_t *c = ctrl_of(PG, ND(n)); return c ? c->checked : node_attr(ND(n), "checked") != NULL; }
void dom_set_checked(void *page, void *n, int on) {
    ctrl_t *c = ctrl_of(PG, ND(n));
    if (c) {
        if (on && c->type == C_RADIO) for (int i = 0; i < PG->nctrls; i++) { ctrl_t *o = &PG->ctrls[i]; if (o->type == C_RADIO && o->form == c->form && !strcmp(o->name, c->name)) o->checked = false; }
        c->checked = on;
    } else if (on) set_attr_l1(PG, ND(n), "checked", arena_strdup(&PG->doc->arena, "", 0));
    else dom_remove_attr(page, n, "checked");
    PG->tree_dirty = true;
}
int dom_get_selected_index(void *page, void *n) { ctrl_t *c = ctrl_of(PG, ND(n)); return c && c->type == C_SELECT ? c->sel : 0; }
void dom_set_selected_index(void *page, void *n, int i) { ctrl_t *c = ctrl_of(PG, ND(n)); if (c && c->type == C_SELECT && i >= 0 && i < c->nopts) { c->sel = i; PG->tree_dirty = true; } }

/* ---------------- title, URL ---------------- */
char *dom_title(void *page) { return to_utf8(PG->title, -1); }
void dom_set_title(void *page, const char *t) {
    char *l1 = kmalloc(strlen(t) * 3 + 1); latin1_from_utf8((const uint8_t *)t, strlen(t), l1);
    strlcpy(PG->title, l1, sizeof PG->title); kfree(l1);
    PG->title_dirty = true;
}
char *dom_url(void *page) { return to_utf8(PG->url, -1); }
char *dom_referrer(void *page) { return to_utf8(PG->referrer, -1); }
char *dom_resolve(void *page, const char *base, const char *url) {
    char out[2048];
    const char *b = base && *base ? base : (PG->doc->base[0] ? PG->doc->base : PG->url);
    if (!url_resolve(b, url, out, sizeof out)) strlcpy(out, url, sizeof out);
    return to_utf8(out, -1);
}

/* ---------------- cookies and storage (kept per site for this session) ---------------- */
typedef struct kv { char site[128]; char *k, *v; struct kv *next; } kv_t;
static kv_t *cookies, *storage;
static void site_of(page_t *p, char *out, int cap) {
    char sch[8], path[8]; uint16_t port;
    if (!url_parse(p->url, sch, out, cap, &port, path, sizeof path)) strlcpy(out, "local", cap);
}
static kv_t *kv_find(kv_t *l, const char *site, const char *k) { for (; l; l = l->next) if (!strcmp(l->site, site) && !strcmp(l->k, k)) return l; return NULL; }
static void kv_set(kv_t **l, const char *site, const char *k, const char *v) {
    kv_t *e = kv_find(*l, site, k);
    if (!v) {
        for (kv_t **pp = l; *pp; pp = &(*pp)->next) if (*pp == e) { *pp = e->next; kfree(e->k); kfree(e->v); kfree(e); break; }
        return;
    }
    if (!e) { e = kzalloc(sizeof *e); strlcpy(e->site, site, sizeof e->site); e->k = strdup(k); e->next = *l; *l = e; }
    else kfree(e->v);
    e->v = strdup(v);
}
char *dom_cookie(void *page) {
    char site[128]; site_of(PG, site, sizeof site);
    sbuf_t s = { kmalloc(64), 0, 64 }; s.b[0] = 0;
    for (kv_t *e = cookies; e; e = e->next) if (!strcmp(e->site, site)) { if (s.len) sb_put(&s, "; ", 2); sb_put(&s, e->k, -1); sb_put(&s, "=", 1); sb_put(&s, e->v, -1); }
    return s.b;
}
void dom_set_cookie(void *page, const char *c) {
    char site[128]; site_of(PG, site, sizeof site);
    const char *eq = strchr(c, '='), *semi = strchr(c, ';');
    if (!eq || (semi && semi < eq)) return;
    char k[128], v[1024];
    int kl = MIN((int)(eq - c), 127); memcpy(k, c, kl); k[kl] = 0;
    const char *ve = semi ? semi : c + strlen(c);
    int vl = MIN((int)(ve - eq - 1), 1023); memcpy(v, eq + 1, vl); v[vl] = 0;
    bool expire = semi && (strstr(semi, "expires=Thu, 01 Jan 1970") || strstr(semi, "max-age=0") || strstr(semi, "Max-Age=0"));
    kv_set(&cookies, site, k, expire ? NULL : v);
}
char *dom_storage_get(void *page, const char *key) { char site[128]; site_of(PG, site, sizeof site); kv_t *e = kv_find(storage, site, key); return e ? strdup(e->v) : NULL; }
void dom_storage_set(void *page, const char *key, const char *value) { char site[128]; site_of(PG, site, sizeof site); kv_set(&storage, site, key, value); }
void dom_storage_clear(void *page) {
    char site[128]; site_of(PG, site, sizeof site);
    for (kv_t **pp = &storage; *pp;) { kv_t *e = *pp; if (!strcmp(e->site, site)) { *pp = e->next; kfree(e->k); kfree(e->v); kfree(e); } else pp = &e->next; }
}
char *dom_storage_keys(void *page) {
    char site[128]; site_of(PG, site, sizeof site);
    sbuf_t s = { kmalloc(64), 0, 64 }; s.b[0] = 0;
    for (kv_t *e = storage; e; e = e->next) if (!strcmp(e->site, site)) { if (s.len) sb_put(&s, "\n", 1); sb_put(&s, e->k, -1); }
    return s.b;
}

/* ---------------- network ---------------- */
int dom_fetch(void *page, const char *url, const char *method, const char *body, char **out, int *outlen, int *status, char **ctype) {
    *out = NULL; *ctype = NULL; *outlen = 0; *status = 0;
    if (!strncasecmp(url, "file:", 5)) {
        const char *pth = url + 5; while (*pth == '/') pth++;
        uint32_t l; uint8_t *d = vfs_read_file(pth, &l);
        if (!d) return 0;
        *out = kmalloc(l + 1); memcpy(*out, d, l); (*out)[l] = 0; *outlen = l; kfree(d);
        *status = 200; *ctype = strdup("text/plain");
        return 1;
    }
    if (adblock_blocks(url, PG->url)) { PG->blocked++; return 0; }
    http_response_t r;
    bool ok = body || strcasecmp(method, "GET") ? http_request(method, url, "application/x-www-form-urlencoded", body ? body : "", body ? strlen(body) : 0, &r, NULL, NULL)
                                                : http_get(url, &r, NULL, NULL);
    if (!ok) { http_free(&r); return 0; }
    /* the engine wants UTF-8 text; pass the bytes through unchanged */
    *out = kmalloc(r.body_len + 1); memcpy(*out, r.body, r.body_len); (*out)[r.body_len] = 0; *outlen = r.body_len;
    *status = r.status; *ctype = strdup(r.content_type);
    http_free(&r);
    return 1;
}
unsigned dom_now_ms(void) { return timer_ticks(); }
double dom_epoch_ms(void) {
    rtc_time_t t; rtc_read(&t);
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    return (double)((days * 86400 + t.hour * 3600 + t.minute * 60 + t.second) * 1000);
}
