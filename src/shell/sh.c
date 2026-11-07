/* A POSIX-flavoured shell interpreter: tokenizer, recursive-descent parser to an AST,
 * expansion ($var, ${..}, $(..), `..`, $((..)), ~, globs), pipelines, redirection,
 * if/while/until/for/case, functions, and a large set of builtins. */
#include "kernel.h"
#include "jsapi.h"
#include "wm.h"
#include "apps.h"
#include "theme.h"
#include "disk.h"
#include "net.h"
#include "audio.h"

bool console_interrupted(console_t *c);
void console_clear_interrupt(console_t *c);
const char *terminal_cwd_ptr(console_t *c, char **cwd);

/* ======================= buffer console (pipes, $(..), redirection) ======================= */
typedef struct { console_t c; char *buf; int len, cap, rpos; console_t *parent; } bufcon_t;
static void bc_write(console_t *c, const char *s, int n) {
    bufcon_t *b = (bufcon_t *)c;
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2 + 64; b->buf = krealloc(b->buf, b->cap); }
    memcpy(b->buf + b->len, s, n); b->len += n; b->buf[b->len] = 0;
}
static int bc_readline(console_t *c, char *out, int cap) {
    bufcon_t *b = (bufcon_t *)c;
    if (b->rpos >= b->len) return -1;
    int n = 0;
    while (b->rpos < b->len && b->buf[b->rpos] != '\n') { if (n < cap - 1) out[n++] = b->buf[b->rpos]; b->rpos++; }
    if (b->rpos < b->len) b->rpos++;
    out[n] = 0;
    return n;
}
static int bc_getkey(console_t *c) { bufcon_t *b = (bufcon_t *)c; return b->rpos < b->len ? (uint8_t)b->buf[b->rpos++] : -1; }
static void bc_noop(console_t *c) { (void)c; }
static void bc_title(console_t *c, const char *t) { (void)c; (void)t; }
static void bc_color(console_t *c, int f, int b) { (void)c; (void)f; (void)b; }
static bool bc_alive(console_t *c) { bufcon_t *b = (bufcon_t *)c; return b->parent ? b->parent->alive(b->parent) : true; }
static bufcon_t *bufcon_new(console_t *parent) {
    bufcon_t *b = kzalloc(sizeof *b);
    b->c.write = bc_write; b->c.readline = bc_readline; b->c.getkey = bc_getkey; b->c.clear = bc_noop;
    b->c.set_title = bc_title; b->c.set_color = bc_color; b->c.alive = bc_alive; b->c.impl = b;
    b->parent = parent;
    b->cap = 256; b->buf = kmalloc(b->cap); b->buf[0] = 0;
    return b;
}
static void bufcon_free(bufcon_t *b) { if (b) { kfree(b->buf); kfree(b); } }

/* ======================= shell state ======================= */
typedef struct node node_t;
typedef struct var { char *name, *value; bool exported; struct var *next; } var_t;
typedef struct func { char *name; node_t *body; struct func *next; } func_t;
typedef struct sh {
    console_t *con;
    char cwd[256];
    var_t *vars;
    func_t *funcs;
    int status;
    int argc; char **argv;
    int brk, cont;
    bool ret, exiting;
    int exit_code;
    int depth;
    char *script_name;
} sh_t;
typedef struct { console_t *in, *out, *err; } io_t;

static const char *var_get(sh_t *s, const char *n) { for (var_t *v = s->vars; v; v = v->next) if (!strcmp(v->name, n)) return v->value; return NULL; }
static void var_set(sh_t *s, const char *n, const char *val) {
    for (var_t *v = s->vars; v; v = v->next) if (!strcmp(v->name, n)) { kfree(v->value); v->value = strdup(val); return; }
    var_t *v = kzalloc(sizeof *v); v->name = strdup(n); v->value = strdup(val); v->next = s->vars; s->vars = v;
}
static void var_unset(sh_t *s, const char *n) {
    for (var_t **pp = &s->vars; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->name, n)) { var_t *v = *pp; *pp = v->next; kfree(v->name); kfree(v->value); kfree(v); return; }
}
static bool interrupted(sh_t *s) { return console_interrupted(s->con); }
static void sh_sync_cwd(sh_t *s) { char *p; if (s->con) { terminal_cwd_ptr(s->con, &p); if (p && s->con->write) {} } var_set(s, "PWD", s->cwd); }

static void out_s(io_t *io, const char *s) { io->out->write(io->out, s, strlen(s)); }
static void out_f(io_t *io, const char *fmt, ...) { char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); io->out->write(io->out, b, MIN(n, 1023)); }
static void err_f(io_t *io, const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); io->err->write(io->err, "\x1b[91m", 5); io->err->write(io->err, b, MIN(n, 511)); io->err->write(io->err, "\x1b[0m", 4); }

/* ======================= tokenizer ======================= */
enum { T_WORD, T_SEMI, T_NL, T_AND, T_OR, T_PIPE, T_AMP, T_GT, T_GTGT, T_LT, T_LP, T_RP, T_DSEMI, T_2GT, T_2TO1, T_EOF };
typedef struct { int type; char *s; } tok_t;
typedef struct { tok_t *t; int n, cap, pos; bool incomplete; char err[96]; } toks_t;
static void tpush(toks_t *ts, int type, char *s) {
    if (ts->n == ts->cap) { ts->cap = ts->cap ? ts->cap * 2 : 64; ts->t = krealloc(ts->t, ts->cap * sizeof(tok_t)); }
    ts->t[ts->n].type = type; ts->t[ts->n].s = s; ts->n++;
}
static int scan_balanced(const char *p, char open, char close) {
    /* p points after the opening char; returns length up to matching close (exclusive) or -1 */
    int depth = 1, i = 0;
    while (p[i]) {
        if (p[i] == '\\' && p[i + 1]) { i += 2; continue; }
        if (p[i] == '\'') { i++; while (p[i] && p[i] != '\'') i++; if (p[i]) i++; continue; }
        if (p[i] == '"') { i++; while (p[i] && p[i] != '"') { if (p[i] == '\\' && p[i + 1]) i++; i++; } if (p[i]) i++; continue; }
        if (p[i] == open) depth++;
        else if (p[i] == close) { if (--depth == 0) return i; }
        i++;
    }
    return -1;
}
static void tokenize(const char *src, toks_t *ts) {
    const char *p = src;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
        if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (*p == '\n') { tpush(ts, T_NL, NULL); p++; continue; }
        if (*p == ';') { if (p[1] == ';') { tpush(ts, T_DSEMI, NULL); p += 2; } else { tpush(ts, T_SEMI, NULL); p++; } continue; }
        if (*p == '&') { if (p[1] == '&') { tpush(ts, T_AND, NULL); p += 2; } else { tpush(ts, T_AMP, NULL); p++; } continue; }
        if (*p == '|') { if (p[1] == '|') { tpush(ts, T_OR, NULL); p += 2; } else { tpush(ts, T_PIPE, NULL); p++; } continue; }
        if (*p == '>') { if (p[1] == '>') { tpush(ts, T_GTGT, NULL); p += 2; } else { tpush(ts, T_GT, NULL); p++; } continue; }
        if (*p == '<') { tpush(ts, T_LT, NULL); p++; continue; }
        if (*p == '(') { tpush(ts, T_LP, NULL); p++; continue; }
        if (*p == ')') { tpush(ts, T_RP, NULL); p++; continue; }
        if (*p == '2' && p[1] == '>') {
            if (p[2] == '&' && p[3] == '1') { tpush(ts, T_2TO1, NULL); p += 4; } else { tpush(ts, T_2GT, NULL); p += 2; }
            continue;
        }
        /* word */
        const char *st = p;
        while (*p && !strchr(" \t\r\n;&|<>()", *p)) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '\'') { p++; while (*p && *p != '\'') p++; if (!*p) { ts->incomplete = true; break; } p++; continue; }
            if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; else if (*p == '$' && p[1] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l < 0) { ts->incomplete = true; break; } p += l + 2; } p++; } if (!*p) { ts->incomplete = true; break; } p++; continue; }
            if (*p == '`') { p++; while (*p && *p != '`') p++; if (!*p) { ts->incomplete = true; break; } p++; continue; }
            if (*p == '$' && p[1] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l < 0) { ts->incomplete = true; p += strlen(p); break; } p += l + 3; continue; }
            if (*p == '$' && p[1] == '{') { int l = scan_balanced(p + 2, '{', '}'); if (l < 0) { ts->incomplete = true; p += strlen(p); break; } p += l + 3; continue; }
            p++;
        }
        tpush(ts, T_WORD, strndup(st, p - st));
    }
    tpush(ts, T_EOF, NULL);
}

/* ======================= AST ======================= */
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_LIST, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_FUNC, N_GROUP, N_SUBSHELL, N_NOT };
typedef struct { int type; char *target; } redir_t;
struct node {
    int type;
    node_t *a, *b, *c;          /* children */
    node_t *next;               /* list chaining */
    char **words; int nwords;
    redir_t *redirs; int nredir;
    char *name;
    char ***pats; int *npats; node_t **bodies; int ncase;
};
static node_t *nnew(int t) { node_t *n = kzalloc(sizeof *n); n->type = t; return n; }
static void nfree(node_t *n) {
    while (n) {
        node_t *nx = n->next;
        nfree(n->a); nfree(n->b); nfree(n->c);
        for (int i = 0; i < n->nwords; i++) kfree(n->words[i]);
        kfree(n->words);
        for (int i = 0; i < n->nredir; i++) kfree(n->redirs[i].target);
        kfree(n->redirs); kfree(n->name);
        for (int i = 0; i < n->ncase; i++) { for (int k = 0; k < n->npats[i]; k++) kfree(n->pats[i][k]); kfree(n->pats[i]); nfree(n->bodies[i]); }
        kfree(n->pats); kfree(n->npats); kfree(n->bodies);
        kfree(n);
        n = nx;
    }
}

/* ======================= parser ======================= */
static tok_t *peek(toks_t *ts) { return &ts->t[ts->pos]; }
static bool is_kw(toks_t *ts, const char *kw) { tok_t *t = peek(ts); return t->type == T_WORD && !strcmp(t->s, kw); }
static void skip_nl(toks_t *ts) { while (peek(ts)->type == T_NL || peek(ts)->type == T_SEMI) ts->pos++; }
static void skip_newlines(toks_t *ts) { while (peek(ts)->type == T_NL) ts->pos++; }
static bool expect_kw(toks_t *ts, const char *kw) {
    skip_nl(ts);
    if (is_kw(ts, kw)) { ts->pos++; return true; }
    if (peek(ts)->type == T_EOF) ts->incomplete = true;
    else if (!ts->err[0]) snprintf(ts->err, sizeof ts->err, "syntax error: expected '%s'", kw);
    return false;
}
static node_t *parse_list(toks_t *ts);
static bool is_terminator(toks_t *ts) {
    tok_t *t = peek(ts);
    if (t->type == T_EOF || t->type == T_RP || t->type == T_DSEMI) return true;
    if (t->type != T_WORD) return false;
    static const char *kws[] = { "then", "elif", "else", "fi", "do", "done", "esac", "}", NULL };
    for (int i = 0; kws[i]; i++) if (!strcmp(t->s, kws[i])) return true;
    return false;
}
static void add_word(node_t *n, char *w) { n->words = krealloc(n->words, (n->nwords + 1) * sizeof(char *)); n->words[n->nwords++] = w; }
static node_t *parse_command(toks_t *ts);
static node_t *parse_compound_body(toks_t *ts, const char *endkw) {
    node_t *b = parse_list(ts);
    if (!expect_kw(ts, endkw)) { nfree(b); return NULL; }
    return b;
}
static node_t *parse_command(toks_t *ts) {
    tok_t *t = peek(ts);
    if (t->type == T_WORD) {
        if (!strcmp(t->s, "!")) { ts->pos++; node_t *n = nnew(N_NOT); n->a = parse_command(ts); return n; }
        if (!strcmp(t->s, "if")) {
            ts->pos++;
            node_t *n = nnew(N_IF);
            n->a = parse_list(ts);
            if (!expect_kw(ts, "then")) { nfree(n); return NULL; }
            n->b = parse_list(ts);
            skip_nl(ts);
            if (is_kw(ts, "elif")) { ts->t[ts->pos].s[0] = 'i'; ts->t[ts->pos].s[1] = 'f'; ts->t[ts->pos].s[2] = 0; n->c = parse_command(ts); return n; }
            if (is_kw(ts, "else")) { ts->pos++; n->c = parse_list(ts); }
            if (!expect_kw(ts, "fi")) { nfree(n); return NULL; }
            return n;
        }
        if (!strcmp(t->s, "while") || !strcmp(t->s, "until")) {
            node_t *n = nnew(!strcmp(t->s, "while") ? N_WHILE : N_UNTIL); ts->pos++;
            n->a = parse_list(ts);
            if (!expect_kw(ts, "do")) { nfree(n); return NULL; }
            n->b = parse_compound_body(ts, "done");
            if (!n->b && (ts->incomplete || ts->err[0])) { nfree(n); return NULL; }
            return n;
        }
        if (!strcmp(t->s, "for")) {
            ts->pos++;
            node_t *n = nnew(N_FOR);
            if (peek(ts)->type != T_WORD) { snprintf(ts->err, sizeof ts->err, "syntax error after 'for'"); nfree(n); return NULL; }
            n->name = strdup(peek(ts)->s); ts->pos++;
            skip_newlines(ts);
            if (is_kw(ts, "in")) { ts->pos++; while (peek(ts)->type == T_WORD) { add_word(n, strdup(peek(ts)->s)); ts->pos++; } }
            else { add_word(n, strdup("\"$@\"")); }
            if (!expect_kw(ts, "do")) { nfree(n); return NULL; }
            n->a = parse_compound_body(ts, "done");
            if (!n->a && (ts->incomplete || ts->err[0])) { nfree(n); return NULL; }
            return n;
        }
        if (!strcmp(t->s, "case")) {
            ts->pos++;
            node_t *n = nnew(N_CASE);
            if (peek(ts)->type != T_WORD) { snprintf(ts->err, sizeof ts->err, "syntax error after 'case'"); nfree(n); return NULL; }
            n->name = strdup(peek(ts)->s); ts->pos++;
            if (!expect_kw(ts, "in")) { nfree(n); return NULL; }
            for (;;) {
                skip_nl(ts);
                if (is_kw(ts, "esac")) { ts->pos++; break; }
                if (peek(ts)->type == T_EOF) { ts->incomplete = true; nfree(n); return NULL; }
                if (peek(ts)->type == T_LP) ts->pos++;
                char **pats = NULL; int np = 0;
                while (peek(ts)->type == T_WORD) {
                    pats = krealloc(pats, (np + 1) * sizeof(char *)); pats[np++] = strdup(peek(ts)->s); ts->pos++;
                    if (peek(ts)->type == T_PIPE) ts->pos++; else break;
                }
                if (peek(ts)->type != T_RP) { snprintf(ts->err, sizeof ts->err, "syntax error in case pattern"); nfree(n); return NULL; }
                ts->pos++;
                node_t *body = parse_list(ts);
                n->pats = krealloc(n->pats, (n->ncase + 1) * sizeof(char **));
                n->npats = krealloc(n->npats, (n->ncase + 1) * sizeof(int));
                n->bodies = krealloc(n->bodies, (n->ncase + 1) * sizeof(node_t *));
                n->pats[n->ncase] = pats; n->npats[n->ncase] = np; n->bodies[n->ncase] = body; n->ncase++;
                skip_newlines(ts);
                if (peek(ts)->type == T_DSEMI) ts->pos++;
            }
            return n;
        }
        if (!strcmp(t->s, "{")) { ts->pos++; node_t *n = nnew(N_GROUP); n->a = parse_compound_body(ts, "}"); if (!n->a && (ts->incomplete || ts->err[0])) { nfree(n); return NULL; } return n; }
        if (!strcmp(t->s, "function")) {
            ts->pos++;
            node_t *n = nnew(N_FUNC); n->name = strdup(peek(ts)->s ? peek(ts)->s : "f"); ts->pos++;
            if (peek(ts)->type == T_LP && ts->t[ts->pos + 1].type == T_RP) ts->pos += 2;
            skip_newlines(ts);
            n->a = parse_command(ts);
            return n;
        }
        /* name() { ... } */
        if (ts->t[ts->pos + 1].type == T_LP && ts->t[ts->pos + 2].type == T_RP) {
            node_t *n = nnew(N_FUNC); n->name = strdup(t->s); ts->pos += 3;
            skip_newlines(ts);
            n->a = parse_command(ts);
            if (!n->a) { nfree(n); return NULL; }
            return n;
        }
    }
    if (t->type == T_LP) {
        ts->pos++;
        node_t *n = nnew(N_SUBSHELL); n->a = parse_list(ts);
        if (peek(ts)->type != T_RP) { if (peek(ts)->type == T_EOF) ts->incomplete = true; nfree(n); return NULL; }
        ts->pos++;
        return n;
    }
    /* simple command */
    node_t *n = nnew(N_SIMPLE);
    for (;;) {
        t = peek(ts);
        if (t->type == T_WORD) {
            if (n->nwords == 0 && is_terminator(ts)) break;
            add_word(n, strdup(t->s)); ts->pos++;
        } else if (t->type == T_GT || t->type == T_GTGT || t->type == T_LT || t->type == T_2GT) {
            ts->pos++;
            if (peek(ts)->type != T_WORD) { snprintf(ts->err, sizeof ts->err, "syntax error: missing redirection target"); nfree(n); return NULL; }
            n->redirs = krealloc(n->redirs, (n->nredir + 1) * sizeof(redir_t));
            n->redirs[n->nredir].type = t->type; n->redirs[n->nredir].target = strdup(peek(ts)->s); n->nredir++; ts->pos++;
        } else if (t->type == T_2TO1) {
            ts->pos++;
            n->redirs = krealloc(n->redirs, (n->nredir + 1) * sizeof(redir_t));
            n->redirs[n->nredir].type = T_2TO1; n->redirs[n->nredir].target = NULL; n->nredir++;
        } else break;
    }
    if (!n->nwords && !n->nredir) { nfree(n); return NULL; }
    return n;
}
static node_t *parse_pipeline(toks_t *ts) {
    node_t *first = parse_command(ts);
    if (!first) return NULL;
    if (peek(ts)->type != T_PIPE) return first;
    node_t *p = nnew(N_PIPE); p->a = first;
    node_t *last = first;
    while (peek(ts)->type == T_PIPE) {
        ts->pos++; skip_newlines(ts);
        node_t *c = parse_command(ts);
        if (!c) { if (peek(ts)->type == T_EOF) ts->incomplete = true; nfree(p); return NULL; }
        last->next = c; last = c;
    }
    return p;
}
static node_t *parse_andor(toks_t *ts) {
    node_t *l = parse_pipeline(ts);
    if (!l) return NULL;
    while (peek(ts)->type == T_AND || peek(ts)->type == T_OR) {
        int ty = peek(ts)->type == T_AND ? N_AND : N_OR;
        ts->pos++; skip_newlines(ts);
        node_t *r = parse_pipeline(ts);
        if (!r) { if (peek(ts)->type == T_EOF) ts->incomplete = true; nfree(l); return NULL; }
        node_t *n = nnew(ty); n->a = l; n->b = r; l = n;
    }
    return l;
}
static node_t *parse_list(toks_t *ts) {
    node_t *head = NULL, *tail = NULL;
    for (;;) {
        skip_nl(ts);
        while (peek(ts)->type == T_AMP) ts->pos++;
        if (is_terminator(ts) || ts->err[0] || ts->incomplete) break;
        int before = ts->pos;
        node_t *n = parse_andor(ts);
        if (!n) {
            if (ts->incomplete || ts->err[0]) break;
            if (ts->pos == before) { snprintf(ts->err, sizeof ts->err, "syntax error near unexpected token"); break; }
            continue;
        }
        node_t *wrap = nnew(N_LIST); wrap->a = n;
        if (tail) tail->next = wrap; else head = wrap;
        tail = wrap;
    }
    return head;
}

/* ======================= arithmetic ======================= */
typedef struct { const char *p; sh_t *s; bool err; } ar_t;
static long ar_expr(ar_t *a);
static void ar_ws(ar_t *a) { while (isspace(*a->p)) a->p++; }
static long ar_primary(ar_t *a) {
    ar_ws(a);
    if (*a->p == '(') { a->p++; long v = ar_expr(a); ar_ws(a); if (*a->p == ')') a->p++; return v; }
    if (*a->p == '-') { a->p++; return -ar_primary(a); }
    if (*a->p == '+') { a->p++; return ar_primary(a); }
    if (*a->p == '!') { a->p++; return !ar_primary(a); }
    if (*a->p == '~') { a->p++; return ~ar_primary(a); }
    if (isdigit(*a->p)) { char *e; long v = strtol(a->p, &e, 0); a->p = e; return v; }
    if (*a->p == '$') a->p++;
    if (isalpha(*a->p) || *a->p == '_') {
        char name[64]; int n = 0;
        while ((isalnum(*a->p) || *a->p == '_') && n < 63) name[n++] = *a->p++;
        name[n] = 0;
        const char *v = var_get(a->s, name);
        return v ? strtol(v, NULL, 0) : 0;
    }
    a->err = true; return 0;
}
static long ar_bin(ar_t *a, int prec) {
    long l = ar_primary(a);
    for (;;) {
        ar_ws(a);
        const char *p = a->p; int op = 0, pr = 0, len = 1;
        if (p[0] == '|' && p[1] == '|') { op = 'o'; pr = 1; len = 2; }
        else if (p[0] == '&' && p[1] == '&') { op = 'a'; pr = 2; len = 2; }
        else if (p[0] == '|') { op = '|'; pr = 3; }
        else if (p[0] == '^') { op = '^'; pr = 4; }
        else if (p[0] == '&') { op = '&'; pr = 5; }
        else if (p[0] == '=' && p[1] == '=') { op = 'e'; pr = 6; len = 2; }
        else if (p[0] == '!' && p[1] == '=') { op = 'n'; pr = 6; len = 2; }
        else if (p[0] == '<' && p[1] == '=') { op = 'l'; pr = 7; len = 2; }
        else if (p[0] == '>' && p[1] == '=') { op = 'g'; pr = 7; len = 2; }
        else if (p[0] == '<' && p[1] == '<') { op = 'L'; pr = 8; len = 2; }
        else if (p[0] == '>' && p[1] == '>') { op = 'R'; pr = 8; len = 2; }
        else if (p[0] == '<') { op = '<'; pr = 7; }
        else if (p[0] == '>') { op = '>'; pr = 7; }
        else if (p[0] == '+') { op = '+'; pr = 9; }
        else if (p[0] == '-') { op = '-'; pr = 9; }
        else if (p[0] == '*') { op = '*'; pr = 10; }
        else if (p[0] == '/') { op = '/'; pr = 10; }
        else if (p[0] == '%') { op = '%'; pr = 10; }
        if (!op || pr < prec) return l;
        a->p += len;
        long r = ar_bin(a, pr + 1);
        switch (op) {
        case 'o': l = l || r; break; case 'a': l = l && r; break; case '|': l |= r; break; case '^': l ^= r; break; case '&': l &= r; break;
        case 'e': l = l == r; break; case 'n': l = l != r; break; case 'l': l = l <= r; break; case 'g': l = l >= r; break;
        case '<': l = l < r; break; case '>': l = l > r; break; case 'L': l <<= r; break; case 'R': l >>= r; break;
        case '+': l += r; break; case '-': l -= r; break; case '*': l *= r; break;
        case '/': l = r ? l / r : 0; if (!r) a->err = true; break; case '%': l = r ? l % r : 0; break;
        }
    }
}
static long ar_expr(ar_t *a) {
    long v = ar_bin(a, 1);
    ar_ws(a);
    if (*a->p == '?') { a->p++; long x = ar_expr(a); ar_ws(a); if (*a->p == ':') a->p++; long y = ar_expr(a); return v ? x : y; }
    return v;
}
static long arith(sh_t *s, const char *e) {
    /* support simple assignment forms: name=expr, name+=expr, name++ */
    const char *p = e; while (isspace(*p)) p++;
    char name[64]; int n = 0;
    while ((isalnum(p[n]) || p[n] == '_') && n < 63) { name[n] = p[n]; n++; }
    name[n] = 0;
    if (n && !isdigit(name[0])) {
        const char *q = p + n; while (isspace(*q)) q++;
        if ((q[0] == '+' && q[1] == '+') || (q[0] == '-' && q[1] == '-')) {
            const char *v = var_get(s, name); long cur = v ? strtol(v, NULL, 0) : 0;
            char b[24]; snprintf(b, sizeof b, "%ld", cur + (q[0] == '+' ? 1 : -1)); var_set(s, name, b); return cur;
        }
        if ((q[0] == '=' && q[1] != '=') || ((q[0] == '+' || q[0] == '-' || q[0] == '*' || q[0] == '/') && q[1] == '=')) {
            ar_t a = { q + (q[0] == '=' ? 1 : 2), s, false };
            long r = ar_expr(&a);
            const char *v = var_get(s, name); long cur = v ? strtol(v, NULL, 0) : 0;
            if (q[0] == '+') r = cur + r; else if (q[0] == '-' && q[1] == '=') r = cur - r; else if (q[0] == '*') r = cur * r; else if (q[0] == '/') r = r ? cur / r : 0;
            char b[24]; snprintf(b, sizeof b, "%ld", r); var_set(s, name, b); return r;
        }
    }
    ar_t a = { e, s, false };
    return ar_expr(&a);
}

/* ======================= expansion ======================= */
typedef struct { char **v; int n, cap; } strv_t;
static void sv_push(strv_t *sv, char *s) { if (sv->n + 1 >= sv->cap) { sv->cap = sv->cap ? sv->cap * 2 : 8; sv->v = krealloc(sv->v, sv->cap * sizeof(char *)); } sv->v[sv->n++] = s; sv->v[sv->n] = NULL; }
static void sv_free(strv_t *sv) { for (int i = 0; i < sv->n; i++) kfree(sv->v[i]); kfree(sv->v); sv->v = NULL; sv->n = sv->cap = 0; }
typedef struct { char *b; int n, cap; } sbuf_t;
static void sb_putc(sbuf_t *s, char c) { if (s->n + 2 > s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->b = krealloc(s->b, s->cap); } s->b[s->n++] = c; s->b[s->n] = 0; }
static void sb_puts(sbuf_t *s, const char *t) { while (*t) sb_putc(s, *t++); }

static int run_node(sh_t *s, node_t *n, io_t *io);
static int run_string(sh_t *s, const char *src, io_t *io);
static char *cmd_subst(sh_t *s, const char *cmd) {
    bufcon_t *b = bufcon_new(s->con);
    io_t io = { s->con, &b->c, &b->c };
    char save[256]; strcpy(save, s->cwd);
    run_string(s, cmd, &io);
    strcpy(s->cwd, save);
    char *r = strdup(b->buf);
    int l = strlen(r); while (l && (r[l - 1] == '\n' || r[l - 1] == '\r')) r[--l] = 0;
    bufcon_free(b);
    return r;
}
static bool glob_match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') { for (;;) { if (glob_match(p + 1, s)) return true; if (!*s) return false; s++; } }
    if (*p == '[') {
        const char *e = strchr(p, ']');
        if (e && *s) {
            bool neg = p[1] == '!' || p[1] == '^', ok = false;
            for (const char *q = p + 1 + neg; q < e; q++) { if (q[1] == '-' && q + 2 < e) { if (tolower(*s) >= tolower(q[0]) && tolower(*s) <= tolower(q[2])) ok = true; q += 2; } else if (tolower(*q) == tolower(*s)) ok = true; }
            if (ok != neg) return glob_match(e + 1, s + 1);
            return false;
        }
    }
    if (!*s) return false;
    if (*p == '?' || tolower(*p) == tolower(*s)) return glob_match(p + 1, s + 1);
    return false;
}
static void expand_param(sh_t *s, const char *expr, sbuf_t *out) {
    /* expr: inside ${...} or a bare name */
    char name[64]; int n = 0;
    bool len = false;
    if (*expr == '#' && expr[1]) { len = true; expr++; }
    while ((isalnum(*expr) || *expr == '_' || (n == 0 && strchr("?#$@*!", *expr))) && n < 63) { name[n++] = *expr++; if (n == 1 && strchr("?#$@*!0123456789", name[0]) && !isalpha(name[0]) && name[0] != '_') break; }
    name[n] = 0;
    char tmp[512]; const char *val = NULL;
    if (!strcmp(name, "?")) { snprintf(tmp, sizeof tmp, "%d", s->status); val = tmp; }
    else if (!strcmp(name, "#")) { snprintf(tmp, sizeof tmp, "%d", s->argc > 0 ? s->argc - 1 : 0); val = tmp; }
    else if (!strcmp(name, "$")) { snprintf(tmp, sizeof tmp, "%d", task_current()->pid); val = tmp; }
    else if (!strcmp(name, "@") || !strcmp(name, "*")) { tmp[0] = 0; for (int i = 1; i < s->argc; i++) { if (i > 1) strlcat(tmp, " ", sizeof tmp); strlcat(tmp, s->argv[i], sizeof tmp); } val = tmp; }
    else if (isdigit(name[0])) { int k = atoi(name); val = k < s->argc ? s->argv[k] : ""; if (k == 0) val = s->script_name ? s->script_name : "sh"; }
    else if (!strcmp(name, "RANDOM")) { snprintf(tmp, sizeof tmp, "%u", rand_next() % 32768); val = tmp; }
    else if (!strcmp(name, "SECONDS")) { snprintf(tmp, sizeof tmp, "%u", timer_ticks() / 1000); val = tmp; }
    else if (!strcmp(name, "PWD")) val = s->cwd;
    else val = var_get(s, name);
    if (len) { snprintf(tmp, sizeof tmp, "%d", val ? (int)strlen(val) : 0); sb_puts(out, tmp); return; }
    if (*expr == ':' && (expr[1] == '-' || expr[1] == '=' || expr[1] == '+')) {
        bool empty = !val || !*val;
        char op = expr[1]; const char *word = expr + 2;
        if (op == '-') { sb_puts(out, empty ? word : val); return; }
        if (op == '=') { if (empty) { var_set(s, name, word); val = word; } sb_puts(out, val); return; }
        if (op == '+') { if (!empty) sb_puts(out, word); return; }
    }
    if (*expr == ':' && isdigit(expr[1])) {
        char *e; int off = strtol(expr + 1, &e, 10), l = -1; if (*e == ':') l = strtol(e + 1, NULL, 10);
        if (val) { int vl = strlen(val); if (off > vl) off = vl; const char *st = val + off; int m = l < 0 ? (int)strlen(st) : MIN(l, (int)strlen(st)); for (int i = 0; i < m; i++) sb_putc(out, st[i]); }
        return;
    }
    if ((*expr == '#' || *expr == '%') && val) {
        bool longest = expr[1] == expr[0]; const char *pat = expr + (longest ? 2 : 1);
        int vl = strlen(val);
        char cp[512]; strlcpy(cp, val, sizeof cp);
        if (*expr == '#') {
            int best = -1;
            for (int i = 0; i <= vl; i++) { char c = cp[i]; cp[i] = 0; if (glob_match(pat, cp)) { best = i; if (!longest) { cp[i] = c; break; } } cp[i] = c; }
            sb_puts(out, best >= 0 ? val + best : val);
        } else {
            int best = -1;
            for (int i = vl; i >= 0; i--) if (glob_match(pat, val + i)) { best = i; if (!longest) break; }
            if (best >= 0) { cp[best] = 0; sb_puts(out, cp); } else sb_puts(out, val);
        }
        return;
    }
    if (*expr == '/' && val) {
        const char *pat = expr + 1; bool all = false; if (*pat == '/') { all = true; pat++; }
        const char *sl = strchr(pat, '/'); char p2[128]; const char *rep = "";
        if (sl) { strlcpy(p2, pat, MIN((int)(sl - pat) + 1, 128)); rep = sl + 1; } else strlcpy(p2, pat, 128);
        int pl = strlen(p2);
        for (const char *v = val; *v;) { if (pl && !strncmp(v, p2, pl)) { sb_puts(out, rep); v += pl; if (!all) { sb_puts(out, v); break; } } else sb_putc(out, *v++); }
        return;
    }
    if (val) sb_puts(out, val);
}
/* expand one raw word into fields; split: perform field splitting; glob: perform pathname expansion */
static void expand_word(sh_t *s, const char *w, strv_t *out, bool split) {
    sbuf_t cur = { 0 }; bool have = false, globbable = false, quoted_any = false;
    const char *p = w;
    if (*p == '~' && (p[1] == 0 || p[1] == '/' || p[1] == '\\')) { sb_puts(&cur, USER_HOME); have = true; p++; }
    while (*p) {
        char c = *p;
        if (c == '\\') { if (p[1]) { sb_putc(&cur, p[1]); p += 2; } else p++; have = true; continue; }
        if (c == '\'') { p++; while (*p && *p != '\'') sb_putc(&cur, *p++); if (*p) p++; have = true; quoted_any = true; continue; }
        if (c == '"') {
            p++; have = true; quoted_any = true;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1] && strchr("\"\\$`", p[1])) { sb_putc(&cur, p[1]); p += 2; continue; }
                if (*p == '$' || *p == '`') {
                    /* expand without splitting */
                    sbuf_t tmp = { 0 };
                    if (*p == '`') { const char *e = strchr(p + 1, '`'); if (!e) e = p + strlen(p); char *cmd = strndup(p + 1, e - p - 1); char *r = cmd_subst(s, cmd); sb_puts(&cur, r); kfree(r); kfree(cmd); p = *e ? e + 1 : e; continue; }
                    if (p[1] == '(' && p[2] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l > 0) { char *e = strndup(p + 3, l - 1); char b[24]; snprintf(b, sizeof b, "%ld", arith(s, e)); kfree(e); sb_puts(&cur, b); p += l + 4; if (*p == ')') p++; continue; } }
                    if (p[1] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l >= 0) { char *cmd = strndup(p + 2, l); char *r = cmd_subst(s, cmd); sb_puts(&cur, r); kfree(r); kfree(cmd); p += l + 3; continue; } }
                    if (p[1] == '{') { int l = scan_balanced(p + 2, '{', '}'); if (l >= 0) { char *e = strndup(p + 2, l); expand_param(s, e, &tmp); kfree(e); p += l + 3; if (tmp.b) sb_puts(&cur, tmp.b); kfree(tmp.b); continue; } }
                    if (isalnum(p[1]) || p[1] == '_' || strchr("?#$@*!", p[1])) {
                        char nm[64]; int n = 0; p++;
                        if (strchr("?#$@*!", *p) || isdigit(*p)) nm[n++] = *p++;
                        else while ((isalnum(*p) || *p == '_') && n < 63) nm[n++] = *p++;
                        nm[n] = 0; expand_param(s, nm, &tmp); if (tmp.b) sb_puts(&cur, tmp.b); kfree(tmp.b); continue;
                    }
                    sb_putc(&cur, '$'); p++; continue;
                }
                sb_putc(&cur, *p++);
            }
            if (*p) p++;
            continue;
        }
        if (c == '$' || c == '`') {
            sbuf_t tmp = { 0 }; bool did = true;
            if (c == '`') { const char *e = strchr(p + 1, '`'); if (!e) e = p + strlen(p); char *cmd = strndup(p + 1, e - p - 1); char *r = cmd_subst(s, cmd); sb_puts(&tmp, r); kfree(r); kfree(cmd); p = *e ? e + 1 : e; }
            else if (p[1] == '(' && p[2] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l > 0) { char *e = strndup(p + 3, l - 1); char b[24]; snprintf(b, sizeof b, "%ld", arith(s, e)); kfree(e); sb_puts(&tmp, b); p += l + 4; if (*p == ')') p++; } else { p++; did = false; } }
            else if (p[1] == '(') { int l = scan_balanced(p + 2, '(', ')'); if (l >= 0) { char *cmd = strndup(p + 2, l); char *r = cmd_subst(s, cmd); sb_puts(&tmp, r); kfree(r); kfree(cmd); p += l + 3; } else { p++; did = false; } }
            else if (p[1] == '{') { int l = scan_balanced(p + 2, '{', '}'); if (l >= 0) { char *e = strndup(p + 2, l); expand_param(s, e, &tmp); kfree(e); p += l + 3; } else { p++; did = false; } }
            else if (isalnum(p[1]) || p[1] == '_' || (p[1] && strchr("?#$@*!", p[1]))) {
                char nm[64]; int n = 0; p++;
                if (strchr("?#$@*!", *p) || isdigit(*p)) nm[n++] = *p++;
                else while ((isalnum(*p) || *p == '_') && n < 63) nm[n++] = *p++;
                nm[n] = 0; expand_param(s, nm, &tmp);
            } else { sb_putc(&cur, '$'); p++; have = true; continue; }
            if (!did) { sb_putc(&cur, '$'); have = true; continue; }
            /* unquoted expansion result: field splitting */
            if (tmp.b) {
                if (!split) { sb_puts(&cur, tmp.b); have = true; }
                else {
                    for (char *q = tmp.b; *q; q++) {
                        if (*q == ' ' || *q == '\t' || *q == '\n') { if (have) { sv_push(out, cur.b ? cur.b : strdup("")); cur.b = NULL; cur.n = cur.cap = 0; have = false; } }
                        else { sb_putc(&cur, *q); have = true; }
                    }
                }
                kfree(tmp.b);
            }
            continue;
        }
        if (c == '*' || c == '?' || c == '[') globbable = true;
        sb_putc(&cur, c); have = true; p++;
    }
    if (!have) { kfree(cur.b); return; }
    if (!cur.b) cur.b = strdup("");
    if (globbable && split && !quoted_any) {
        /* pathname expansion: pattern may contain a directory part */
        char dir[256] = "", *pat = cur.b;
        char *sl = strrchr(cur.b, '/'), *bs = strrchr(cur.b, '\\'); if (bs > sl) sl = bs;
        if (sl) { int l = sl - cur.b + 1; memcpy(dir, cur.b, l); dir[l] = 0; pat = sl + 1; }
        vnode_t *d = vfs_resolve(dir[0] ? dir : ".", s->cwd);
        int before = out->n;
        if (d && d->dir) {
            vfs_sort_children(d);
            for (vnode_t *c2 = d->child; c2; c2 = c2->next)
                if (glob_match(pat, c2->name) && (c2->name[0] != '.' || pat[0] == '.')) { char full[300]; snprintf(full, sizeof full, "%s%s", dir, c2->name); sv_push(out, strdup(full)); }
        }
        if (out->n > before) { kfree(cur.b); return; }
    }
    sv_push(out, cur.b);
}
static void expand_words(sh_t *s, char **words, int n, strv_t *out) { for (int i = 0; i < n; i++) expand_word(s, words[i], out, true); }
static char *expand_single(sh_t *s, const char *w) {
    strv_t v = { 0 }; expand_word(s, w, &v, false);
    char *r = v.n ? strdup(v.v[0]) : strdup("");
    sv_free(&v);
    return r;
}

/* ======================= builtins ======================= */
static sh_t *sh_new(console_t *con, const char *cwd);
static void sh_free(sh_t *s);
int sh_run_file_io(sh_t *parent, const char *path, int argc, char **argv, io_t *io);
typedef int (*builtin_fn)(sh_t *s, int argc, char **argv, io_t *io);
typedef struct { const char *name; builtin_fn fn; const char *help; } builtin_t;
static const builtin_t *find_builtin(const char *name);

static int read_input_all(io_t *io, sbuf_t *sb) {
    char line[1024]; int n;
    while ((n = io->in->readline(io->in, line, sizeof line)) >= 0) { sb_puts(sb, line); sb_putc(sb, '\n'); if (!io->in->alive(io->in)) break; }
    return sb->n;
}
static bool is_stdin_tty(sh_t *s, io_t *io) { return io->in == s->con; }
/* read data from file args or stdin */
static char *gather_input(sh_t *s, int argc, char **argv, int first, io_t *io, bool *ok) {
    sbuf_t sb = { 0 }; *ok = true;
    if (first >= argc) { if (!is_stdin_tty(s, io)) read_input_all(io, &sb); }
    else for (int i = first; i < argc; i++) {
        vnode_t *n = vfs_resolve(argv[i], s->cwd);
        if (!n || n->dir) { err_f(io, "%s: %s: No such file\n", argv[0], argv[i]); *ok = false; continue; }
        for (uint32_t k = 0; k < n->size; k++) sb_putc(&sb, n->data[k]);
    }
    if (!sb.b) sb.b = strdup("");
    return sb.b;
}
static int b_echo(sh_t *s, int argc, char **argv, io_t *io) {
    bool nl = true, esc = false; int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool okf = true; for (char *f = argv[i] + 1; *f; f++) if (!strchr("neE", *f)) okf = false;
        if (!okf) break;
        for (char *f = argv[i] + 1; *f; f++) { if (*f == 'n') nl = false; if (*f == 'e') esc = true; if (*f == 'E') esc = false; }
    }
    sbuf_t sb = { 0 };
    for (int k = i; k < argc; k++) {
        if (k > i) sb_putc(&sb, ' ');
        for (char *p = argv[k]; *p; p++) {
            if (esc && *p == '\\' && p[1]) {
                p++;
                switch (*p) { case 'n': sb_putc(&sb, '\n'); break; case 't': sb_putc(&sb, '\t'); break; case 'e': case 'E': sb_putc(&sb, 27); break; case '\\': sb_putc(&sb, '\\'); break; case 'a': break; case 'c': nl = false; goto out;
                case '0': { int v = 0; for (int d = 0; d < 3 && p[1] >= '0' && p[1] <= '7'; d++) v = v * 8 + (*++p - '0'); sb_putc(&sb, v); break; }
                default: sb_putc(&sb, '\\'); sb_putc(&sb, *p); }
            } else sb_putc(&sb, *p);
        }
    }
out:
    if (nl) sb_putc(&sb, '\n');
    if (sb.b) { io->out->write(io->out, sb.b, sb.n); kfree(sb.b); }
    return 0;
}
static int b_printf(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) return 1;
    const char *f = argv[1]; int ai = 2;
    sbuf_t sb = { 0 };
    do {
        for (const char *p = f; *p; p++) {
            if (*p == '\\' && p[1]) { p++; sb_putc(&sb, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'e' ? 27 : *p); continue; }
            if (*p == '%' && p[1]) {
                char spec[16]; int k = 0; spec[k++] = '%'; p++;
                while ((*p == '-' || *p == '0' || isdigit(*p) || *p == '.') && k < 12) spec[k++] = *p++;
                char conv = *p; spec[k++] = conv == 'i' ? 'd' : conv; spec[k] = 0;
                char b[256]; const char *a = ai < argc ? argv[ai++] : "";
                if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'u' || conv == 'c') snprintf(b, sizeof b, spec, conv == 'c' ? a[0] : (int)strtol(a, NULL, 0));
                else if (conv == 's') snprintf(b, sizeof b, spec, a);
                else if (conv == '%') { strcpy(b, "%"); ai--; }
                else strcpy(b, "");
                sb_puts(&sb, b);
                continue;
            }
            sb_putc(&sb, *p);
        }
    } while (ai < argc && strchr(f, '%'));
    if (sb.b) { io->out->write(io->out, sb.b, sb.n); kfree(sb.b); }
    return 0;
}
static int b_cd(sh_t *s, int argc, char **argv, io_t *io) {
    const char *t = argc > 1 ? argv[1] : USER_HOME;
    if (!strcmp(t, "-")) t = var_get(s, "OLDPWD") ? var_get(s, "OLDPWD") : s->cwd;
    vnode_t *n = vfs_resolve(t, s->cwd);
    if (!n || !n->dir) { err_f(io, "cd: %s: No such directory\n", t); return 1; }
    var_set(s, "OLDPWD", s->cwd);
    vfs_path(n, s->cwd, sizeof s->cwd);
    char *p; terminal_cwd_ptr(s->con, &p); if (p && s->con->set_title) strlcpy(p, s->cwd, 256);
    sh_sync_cwd(s);
    return 0;
}
static int b_pwd(sh_t *s, int argc, char **argv, io_t *io) { out_f(io, "%s\n", s->cwd); return 0; }
static void fmt_time(rtc_time_t *t, char *b, int n) { snprintf(b, n, "%04d-%02d-%02d %02d:%02d", t->year, t->month, t->day, t->hour, t->minute); }
static int b_ls(sh_t *s, int argc, char **argv, io_t *io) {
    bool lng = false, all = false; int first = argc;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-' && argv[i][1]) { if (strchr(argv[i], 'l')) lng = true; if (strchr(argv[i], 'a')) all = true; } else { first = i; break; } }
    int rc = 0;
    for (int i = first; i <= argc; i++) {
        if (i == argc && first != argc) break;
        const char *path = i < argc ? argv[i] : ".";
        vnode_t *d = vfs_resolve(path, s->cwd);
        if (!d) { err_f(io, "ls: cannot access '%s': No such file or directory\n", path); rc = 1; continue; }
        if (argc - first > 1 && d->dir) out_f(io, "%s:\n", path);
        vnode_t single = *d; single.next = NULL;
        vnode_t *list = d->dir ? (vfs_sort_children(d), d->child) : &single;
        int col = 0;
        for (vnode_t *c = list; c; c = c->next) {
            if (!all && c->name[0] == '.') continue;
            const char *color = c->dir ? "\x1b[94m" : (!strcasecmp(vfs_ext(c->name), "exe") || !strcasecmp(vfs_ext(c->name), "sh")) ? "\x1b[92m" : !strcasecmp(vfs_ext(c->name), "dmg") ? "\x1b[95m" : "";
            if (lng) { char tb[32]; fmt_time(&c->mtime, tb, sizeof tb); out_f(io, "%s%s %8u %s %s%s%s\x1b[0m\n", c->dir ? "d" : "-", c->readonly ? "r-x" : "rwx", c->dir ? vfs_count_children(c) : c->size, tb, color, c->name, c->dir ? "/" : ""); }
            else {
                char nm[110]; snprintf(nm, sizeof nm, "%s%s", c->name, c->dir ? "/" : "");
                if (io->out != s->con) { out_f(io, "%s\n", nm); continue; }
                int w = strlen(nm) < 24 ? 26 : ((strlen(nm) + 2 + 25) / 26) * 26;
                if (col + w > 78 && col) { out_s(io, "\n"); col = 0; }
                out_f(io, "%s%s\x1b[0m%*s", color, nm, (int)(w - strlen(nm)), "");
                col += w;
            }
        }
        if (col) out_s(io, "\n");
    }
    return rc;
}
static int b_dir(sh_t *s, int argc, char **argv, io_t *io) {
    const char *path = argc > 1 ? argv[1] : ".";
    vnode_t *d = vfs_resolve(path, s->cwd);
    if (!d || !d->dir) { err_f(io, "File Not Found\n"); return 1; }
    char full[256]; vfs_path(d, full, sizeof full);
    out_f(io, " Volume in drive %c is %s\n\n Directory of %s\n\n", vfs_drive_letter(d), vfs_drive_label(vfs_drive_letter(d)), full);
    vfs_sort_children(d);
    int nf = 0, nd = 0; uint32_t total = 0;
    for (vnode_t *c = d->child; c; c = c->next) {
        char tb[32]; snprintf(tb, sizeof tb, "%02d/%02d/%04d  %02d:%02d", c->mtime.month, c->mtime.day, c->mtime.year, c->mtime.hour, c->mtime.minute);
        if (c->dir) { out_f(io, "%s    <DIR>          %s\n", tb, c->name); nd++; }
        else { out_f(io, "%s    %14u %s\n", tb, c->size, c->name); nf++; total += c->size; }
    }
    out_f(io, "%16d File(s) %14u bytes\n%16d Dir(s)  %14u bytes free\n", nf, total, nd, mm_total() - mm_used());
    return 0;
}
static int b_cat(sh_t *s, int argc, char **argv, io_t *io) {
    bool num = false; int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) { num = true; first = 2; }
    bool ok; char *d = gather_input(s, argc, argv, first, io, &ok);
    if (num) { int ln = 1; char *p = d; while (*p) { char *e = strchr(p, '\n'); int l = e ? e - p : (int)strlen(p); out_f(io, "%6d  ", ln++); io->out->write(io->out, p, l); out_s(io, "\n"); p += l + (e ? 1 : 0); } }
    else io->out->write(io->out, d, strlen(d));
    kfree(d);
    return ok ? 0 : 1;
}
static int b_mkdir(sh_t *s, int argc, char **argv, io_t *io) {
    bool p = false; int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { p = true; continue; }
        if (vfs_resolve(argv[i], s->cwd)) { if (!p) { err_f(io, "mkdir: '%s' already exists\n", argv[i]); rc = 1; } continue; }
        if (p) { char full[512]; if (isalpha(argv[i][0]) && argv[i][1] == ':') strlcpy(full, argv[i], sizeof full); else path_join(full, sizeof full, s->cwd, argv[i]); path_normalize(full); vfs_mkdir_p(full); }
        else if (!vfs_create(argv[i], s->cwd, true)) { err_f(io, "mkdir: cannot create '%s'\n", argv[i]); rc = 1; }
    }
    return rc;
}
static int b_rm(sh_t *s, int argc, char **argv, io_t *io) {
    bool r = false, f = false; int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) { if (strchr(argv[i], 'r') || strchr(argv[i], 'R')) r = true; if (strchr(argv[i], 'f')) f = true; continue; }
        vnode_t *n = vfs_resolve(argv[i], s->cwd);
        if (!n) { if (!f) { err_f(io, "rm: '%s': No such file\n", argv[i]); rc = 1; } continue; }
        if (n->dir && !r && n->child) { err_f(io, "rm: '%s' is a directory (use -r)\n", argv[i]); rc = 1; continue; }
        if (!vfs_delete(n)) { err_f(io, "rm: cannot remove '%s': read-only\n", argv[i]); rc = 1; }
    }
    return rc;
}
static int b_touch(sh_t *s, int argc, char **argv, io_t *io) {
    for (int i = 1; i < argc; i++) { vnode_t *n = vfs_resolve(argv[i], s->cwd); if (n) rtc_now(&n->mtime); else if (!vfs_create(argv[i], s->cwd, false)) { err_f(io, "touch: cannot create '%s'\n", argv[i]); return 1; } }
    return 0;
}
static int cp_mv(sh_t *s, int argc, char **argv, io_t *io, bool move) {
    int first = 1; while (first < argc && argv[first][0] == '-') first++;
    if (argc - first < 2) { err_f(io, "usage: %s SOURCE... DEST\n", argv[0]); return 1; }
    vnode_t *dst = vfs_resolve(argv[argc - 1], s->cwd);
    int rc = 0;
    for (int i = first; i < argc - 1; i++) {
        vnode_t *src = vfs_resolve(argv[i], s->cwd);
        if (!src) { err_f(io, "%s: '%s': No such file\n", argv[0], argv[i]); rc = 1; continue; }
        if (dst && dst->dir) {
            vnode_t *ex = vfs_lookup_child(dst, src->name); if (ex && ex != src) vfs_delete(ex);
            if (move) { if (!vfs_move(src, dst)) { err_f(io, "mv: failed\n"); rc = 1; } }
            else if (!vfs_copy(src, dst, NULL)) { err_f(io, "cp: failed\n"); rc = 1; }
        } else {
            if (argc - first > 2) { err_f(io, "%s: target is not a directory\n", argv[0]); return 1; }
            if (src->dir && !move) { char p[512]; path_dirname(argv[argc - 1], p, sizeof p); vnode_t *pd = vfs_resolve(p[0] ? p : ".", s->cwd); if (pd) vfs_copy(src, pd, path_basename(argv[argc - 1])); continue; }
            if (move) {
                char p[512]; path_dirname(argv[argc - 1], p, sizeof p);
                vnode_t *pd = vfs_resolve(p[0] ? p : ".", s->cwd);
                if (dst) vfs_delete(dst);
                if (pd && pd != src->parent) vfs_move(src, pd);
                vfs_rename(src, path_basename(argv[argc - 1]));
            } else {
                vnode_t *n = vfs_create(argv[argc - 1], s->cwd, false);
                if (!n || !vfs_write_node(n, src->data ? src->data : (uint8_t *)"", src->size)) { err_f(io, "cp: cannot write '%s'\n", argv[argc - 1]); rc = 1; }
            }
        }
    }
    return rc;
}
static int b_cp(sh_t *s, int argc, char **argv, io_t *io) { return cp_mv(s, argc, argv, io, false); }
static int b_mv(sh_t *s, int argc, char **argv, io_t *io) { return cp_mv(s, argc, argv, io, true); }
static int b_headtail(sh_t *s, int argc, char **argv, io_t *io) {
    bool tail = !strcmp(argv[0], "tail"); int n = 10, first = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) { n = atoi(argv[2]); first = 3; } else if (argc > 1 && argv[1][0] == '-' && isdigit(argv[1][1])) { n = atoi(argv[1] + 1); first = 2; }
    bool ok; char *d = gather_input(s, argc, argv, first, io, &ok);
    int lines = 0; for (char *p = d; *p; p++) if (*p == '\n') lines++;
    if (*d && d[strlen(d) - 1] != '\n') lines++;
    int start = tail ? MAX(0, lines - n) : 0, end = tail ? lines : MIN(n, lines);
    int ln = 0; char *p = d;
    while (*p && ln < end) { char *e = strchr(p, '\n'); int l = e ? e - p + 1 : (int)strlen(p); if (ln >= start) io->out->write(io->out, p, l); ln++; p += l; }
    if (*d && d[strlen(d) - 1] != '\n' && end == lines && end > start) out_s(io, "\n");
    kfree(d);
    return 0;
}
static int b_wc(sh_t *s, int argc, char **argv, io_t *io) {
    int first = 1; char mode = 0; if (argc > 1 && argv[1][0] == '-') { mode = argv[1][1]; first = 2; }
    bool ok; char *d = gather_input(s, argc, argv, first, io, &ok);
    int l = 0, w = 0, c = strlen(d); bool inw = false;
    for (char *p = d; *p; p++) { if (*p == '\n') l++; if (isspace(*p)) inw = false; else if (!inw) { inw = true; w++; } }
    if (mode == 'l') out_f(io, "%d\n", l); else if (mode == 'w') out_f(io, "%d\n", w); else if (mode == 'c') out_f(io, "%d\n", c);
    else out_f(io, "%7d %7d %7d\n", l, w, c);
    kfree(d);
    return 0;
}
static bool ci_contains(const char *h, const char *n, bool ci) { int l = strlen(n); for (; *h; h++) if (ci ? !strncasecmp(h, n, l) : !strncmp(h, n, l)) return true; return !*n; }
static int b_grep(sh_t *s, int argc, char **argv, io_t *io) {
    bool ci = false, num = false, inv = false, cnt = false, quiet = false; int i = 1, after_n = 0, after = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (argv[i][1] == 'A') { after_n = argv[i][2] ? atoi(argv[i] + 2) : (i + 1 < argc ? atoi(argv[++i]) : 0); continue; }
        for (char *f = argv[i] + 1; *f; f++) { if (*f == 'i') ci = true; if (*f == 'n') num = true; if (*f == 'v') inv = true; if (*f == 'c') cnt = true; if (*f == 'q') quiet = true; }
    }
    if (i >= argc) { err_f(io, "usage: grep [-invc] PATTERN [FILE...]\n"); return 2; }
    const char *pat = argv[i++];
    bool ok; char *d = gather_input(s, argc, argv, i, io, &ok);
    int ln = 0, matches = 0; char *p = d;
    while (*p) {
        char *e = strchr(p, '\n'); int l = e ? e - p : (int)strlen(p);
        char save = p[l]; p[l] = 0; ln++;
        bool m = ci_contains(p, pat, ci) != inv;
        if (m) { matches++; after = after_n; if (!cnt && !quiet) { if (num) out_f(io, "\x1b[92m%d:\x1b[0m", ln); out_s(io, p); out_s(io, "\n"); } }
        else if (after > 0) { after--; if (!cnt && !quiet) { if (num) out_f(io, "%d-", ln); out_s(io, p); out_s(io, "\n"); } }
        p[l] = save; p += l + (e ? 1 : 0);
    }
    if (cnt) out_f(io, "%d\n", matches);
    kfree(d);
    return matches ? 0 : 1;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }
static int b_sort(sh_t *s, int argc, char **argv, io_t *io) {
    bool rev = false, num = false, uniq = false; int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) { if (strchr(argv[first], 'r')) rev = true; if (strchr(argv[first], 'n')) num = true; if (strchr(argv[first], 'u')) uniq = true; }
    bool ok; char *d = gather_input(s, argc, argv, first, io, &ok);
    strv_t v = { 0 }; char *p = d;
    while (*p) { char *e = strchr(p, '\n'); int l = e ? e - p : (int)strlen(p); sv_push(&v, strndup(p, l)); p += l + (e ? 1 : 0); }
    for (int a = 1; a < v.n; a++) for (int b = a; b > 0; b--) {
        int c = num ? (atoi(v.v[b - 1]) > atoi(v.v[b]) ? 1 : atoi(v.v[b - 1]) < atoi(v.v[b]) ? -1 : 0) : strcmp(v.v[b - 1], v.v[b]);
        if (rev) c = -c;
        if (c > 0) { char *t = v.v[b]; v.v[b] = v.v[b - 1]; v.v[b - 1] = t; } else break;
    }
    for (int k = 0; k < v.n; k++) { if (uniq && k && !strcmp(v.v[k], v.v[k - 1])) continue; out_s(io, v.v[k]); out_s(io, "\n"); }
    sv_free(&v); kfree(d); (void)cmpstr;
    return 0;
}
static int b_uniq(sh_t *s, int argc, char **argv, io_t *io) {
    bool ok; char *d = gather_input(s, argc, argv, 1, io, &ok);
    char *p = d, *prev = NULL;
    while (*p) { char *e = strchr(p, '\n'); int l = e ? e - p : (int)strlen(p); char *ln = strndup(p, l); if (!prev || strcmp(prev, ln)) { out_s(io, ln); out_s(io, "\n"); } kfree(prev); prev = ln; p += l + (e ? 1 : 0); }
    kfree(prev); kfree(d); return 0;
}
static int b_rev(sh_t *s, int argc, char **argv, io_t *io) {
    bool ok; char *d = gather_input(s, argc, argv, 1, io, &ok); char *p = d;
    while (*p) { char *e = strchr(p, '\n'); int l = e ? e - p : (int)strlen(p); for (int i = l - 1; i >= 0; i--) io->out->write(io->out, p + i, 1); out_s(io, "\n"); p += l + (e ? 1 : 0); }
    kfree(d); return 0;
}
static int b_tr(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc >= 2 && !strcmp(argv[1], "-d")) {
        if (argc < 3) return 1;
        bool ok; char *d = gather_input(s, argc, argv, 3, io, &ok);
        char *o = d; for (char *q = d; *q; q++) if (!strchr(argv[2], *q)) *o++ = *q; *o = 0;
        out_s(io, d); kfree(d); return 0;
    }
    if (argc < 3) { err_f(io, "usage: tr SET1 SET2 | tr -d SET\n"); return 1; }
    char set1[256], set2[256]; int n1 = 0, n2 = 0;
    for (int k = 0; k < 2; k++) {
        const char *src = argv[1 + k]; char *dst = k ? set2 : set1; int *n = k ? &n2 : &n1;
        if (!strcmp(src, "[:lower:]") || !strcmp(src, "a-z")) { for (char c = 'a'; c <= 'z'; c++) dst[(*n)++] = c; continue; }
        if (!strcmp(src, "[:upper:]") || !strcmp(src, "A-Z")) { for (char c = 'A'; c <= 'Z'; c++) dst[(*n)++] = c; continue; }
        for (; *src && *n < 255; src++) dst[(*n)++] = *src;
    }
    bool ok; char *d = gather_input(s, argc, argv, 3, io, &ok);
    for (char *p = d; *p; p++) for (int i = 0; i < n1; i++) if (*p == set1[i]) { *p = set2[MIN(i, n2 - 1)]; break; }
    out_s(io, d); kfree(d); return 0;
}
static int b_clear(sh_t *s, int argc, char **argv, io_t *io) { if (io->out->clear) io->out->clear(io->out); return 0; }
static int b_exit(sh_t *s, int argc, char **argv, io_t *io) { s->exiting = true; s->exit_code = argc > 1 ? atoi(argv[1]) : s->status; return s->exit_code; }
static int b_true(sh_t *s, int argc, char **argv, io_t *io) { return 0; }
static int b_false(sh_t *s, int argc, char **argv, io_t *io) { return 1; }
static int b_sleep(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) return 1;
    uint32_t ms = 0; const char *p = argv[1]; while (isdigit(*p)) ms = ms * 10 + (*p++ - '0'); ms *= 1000;
    if (*p == '.') { int scale = 100; p++; while (isdigit(*p) && scale) { ms += (*p++ - '0') * scale; scale /= 10; } }
    uint32_t end = timer_ticks() + ms;
    while ((int32_t)(timer_ticks() - end) < 0) { if (interrupted(s)) return 130; task_sleep(MIN(50u, end - timer_ticks())); }
    return 0;
}
static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int b_date(sh_t *s, int argc, char **argv, io_t *io) {
    rtc_time_t t; rtc_now(&t);
    if (argc > 1 && argv[1][0] == '+') {
        sbuf_t sb = { 0 };
        for (const char *p = argv[1] + 1; *p; p++) {
            if (*p != '%' || !p[1]) { sb_putc(&sb, *p); continue; }
            char b[16]; p++;
            switch (*p) { case 'Y': snprintf(b, 16, "%04d", t.year); break; case 'm': snprintf(b, 16, "%02d", t.month); break; case 'd': snprintf(b, 16, "%02d", t.day); break;
            case 'H': snprintf(b, 16, "%02d", t.hour); break; case 'M': snprintf(b, 16, "%02d", t.minute); break; case 'S': snprintf(b, 16, "%02d", t.second); break;
            case 'a': strcpy(b, days[t.weekday]); break; case 'b': strcpy(b, mons[(t.month + 11) % 12]); break; default: b[0] = '%'; b[1] = *p; b[2] = 0; }
            sb_puts(&sb, b);
        }
        out_f(io, "%s\n", sb.b ? sb.b : ""); kfree(sb.b); return 0;
    }
    out_f(io, "%s %s %2d %02d:%02d:%02d %04d\n", days[t.weekday], mons[(t.month + 11) % 12], t.day, t.hour, t.minute, t.second, t.year);
    return 0;
}
static int b_uname(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc > 1 && !strcmp(argv[1], "-a")) out_f(io, "WRLD %s %s.%s WRLD-Kernel " OS_MACHINE " %s\n", computer_name, OS_VERSION, OS_BUILD, OS_NAME);
    else if (argc > 1 && !strcmp(argv[1], "-r")) out_f(io, "%s.%s\n", OS_VERSION, OS_BUILD);
    else if (argc > 1 && !strcmp(argv[1], "-m")) out_s(io, OS_MACHINE "\n");
    else out_s(io, "WRLD\n");
    return 0;
}
static int b_ver(sh_t *s, int argc, char **argv, io_t *io) { out_f(io, "\n%s %s [Version %s.%s]\n\n", OS_NAME, OS_EDITION, OS_VERSION, OS_BUILD); return 0; }
static int b_whoami(sh_t *s, int argc, char **argv, io_t *io) { out_f(io, "%s\n", user_name); return 0; }
static int b_ipconfig(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc > 1 && (!strcasecmp(argv[1], "/renew") || !strcmp(argv[1], "-r"))) {
        out_s(io, "Renewing the address (DHCP)...\n");
        dhcp_renew();
        if (!net_wait_ready(10000)) { err_f(io, "ipconfig: no DHCP server answered\n"); return 1; }
    }
    out_f(io, "\n" OS_NAME " IP Configuration\n\n   Host Name . . . . . . . . : %s\n\nEthernet adapter Local Area Connection:\n\n", computer_name);
    if (!netif.present) { out_s(io, "   Media State . . . . . . . : No supported network adapter was found\n"); return 1; }
    char a[16], m[16], g[16], d[16], sv[16];
    ip_to_str(netif.ip, a); ip_to_str(netif.mask, m); ip_to_str(netif.gw, g); ip_to_str(netif.dns, d); ip_to_str(netif.dhcp_server, sv);
    out_f(io, "   Description . . . . . . . : %s\n", netif.driver);
    out_f(io, "   Physical Address. . . . . : %02X-%02X-%02X-%02X-%02X-%02X\n", netif.mac[0], netif.mac[1], netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
    if (!netif.link) { out_s(io, "   Media State . . . . . . . : Media disconnected\n"); return 0; }
    out_f(io, "   DHCP Enabled. . . . . . . : Yes\n   IP Address. . . . . . . . : %s\n   Subnet Mask . . . . . . . : %s\n   Default Gateway . . . . . : %s\n   DHCP Server . . . . . . . : %s\n   DNS Server. . . . . . . . : %s\n",
          netif.configured ? a : "(acquiring...)", m, g, sv, d);
    out_f(io, "   Packets sent/received . . : %u / %u (%u KB / %u KB)\n", netif.tx_packets, netif.rx_packets, netif.tx_bytes / 1024, netif.rx_bytes / 1024);
    return 0;
}
static int b_ping(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: ping HOST [-n COUNT]\n"); return 1; }
    int count = 4; const char *host = NULL;
    for (int i = 1; i < argc; i++) { if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-c")) && i + 1 < argc) count = atoi(argv[++i]); else host = argv[i]; }
    uint32_t ip; char err[160];
    if (!host || !dns_resolve(host, &ip, err, sizeof err)) { err_f(io, "ping: %s\n", host ? err : "no host"); return 1; }
    char a[16]; ip_to_str(ip, a);
    out_f(io, "\nPinging %s [%s] with 32 bytes of data:\n", host, a);
    int ok = 0, mn = 1 << 30, mx = 0, sum = 0;
    for (int i = 0; i < count; i++) {
        int ms = icmp_ping(ip, i + 1, 3000);
        if (ms < 0) out_s(io, "Request timed out.\n");
        else { out_f(io, "Reply from %s: bytes=32 time=%dms TTL=64\n", a, ms); ok++; mn = MIN(mn, ms); mx = MAX(mx, ms); sum += ms; }
        if (i + 1 < count) task_sleep(1000 - MIN(ms < 0 ? 0 : ms, 900));
    }
    out_f(io, "\nPing statistics for %s:\n    Packets: Sent = %d, Received = %d, Lost = %d (%d%% loss)\n", a, count, ok, count - ok, (count - ok) * 100 / MAX(1, count));
    if (ok) out_f(io, "Approximate round trip times: Minimum = %dms, Maximum = %dms, Average = %dms\n", mn, mx, sum / ok);
    return ok ? 0 : 1;
}
static int b_nslookup(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: nslookup NAME\n"); return 1; }
    char d[16]; ip_to_str(netif.dns, d);
    out_f(io, "Server:  %s\n\n", d);
    uint32_t ip; char err[160];
    if (!dns_resolve(argv[1], &ip, err, sizeof err)) { err_f(io, "*** %s\n", err); return 1; }
    char a[16]; ip_to_str(ip, a);
    out_f(io, "Name:    %s\nAddress: %s\n", argv[1], a);
    return 0;
}
typedef struct { io_t *io; uint32_t last; } dlprog_t;
static void dl_progress(void *ctx, const char *status, uint32_t got, uint32_t total) {
    dlprog_t *p = ctx;
    if (got && timer_ticks() - p->last < 500) return;
    p->last = timer_ticks();
    if (got) { if (total) out_f(p->io, "  %u / %u KB\n", got / 1024, total / 1024); else out_f(p->io, "  %u KB\n", got / 1024); }
    else out_f(p->io, "%s\n", status);
}
static int b_wget(sh_t *s, int argc, char **argv, io_t *io) {
    bool curl = !strcmp(argv[0], "curl");
    const char *url = NULL, *outf = NULL; bool headers_only = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-O") || !strcmp(argv[i], "-o")) && i + 1 < argc) outf = argv[++i];
        else if (!strcmp(argv[i], "-I")) headers_only = true;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "-s")) quiet = true;
        else url = argv[i];
    }
    if (!url) { err_f(io, "usage: %s URL [-O FILE]%s\n", argv[0], curl ? " [-I] [-s]" : " [-q]"); return 1; }
    char full[1024];
    if (!strstr(url, "://")) snprintf(full, sizeof full, "http://%s", url); else strlcpy(full, url, sizeof full);
    http_response_t r;
    dlprog_t pg = { io, 0 };
    bool ok = http_get(full, &r, quiet || curl ? NULL : dl_progress, &pg);
    if (!ok) { err_f(io, "%s: %s\n", argv[0], r.error); http_free(&r); return 1; }
    if (!quiet && (!curl || headers_only)) {
        out_f(io, "HTTP %d, %u bytes, %s%s%s\n", r.status, r.body_len, r.content_type[0] ? r.content_type : "unknown type",
              r.secure ? ", " : "", r.secure ? r.tls_info : "");
        if (strcmp(r.url, full)) out_f(io, "Redirected to %s\n", r.url);
    }
    if (headers_only) { http_free(&r); return 0; }
    if (curl && !outf) { io->out->write(io->out, (const char *)r.body, r.body_len); if (r.body_len && r.body[r.body_len - 1] != '\n') out_s(io, "\n"); http_free(&r); return r.status >= 400; }
    char name[256];
    if (outf) strlcpy(name, outf, sizeof name);
    else {
        char sch[8], host[256], path[1024]; uint16_t port;
        url_parse(r.url, sch, host, sizeof host, &port, path, sizeof path);
        char *q = strchr(path, '?'); if (q) *q = 0;
        const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
        strlcpy(name, *b ? b : "index.html", sizeof name);
    }
    char dst[512];
    if (isalpha(name[0]) && name[1] == ':') strlcpy(dst, name, sizeof dst); else path_join(dst, sizeof dst, s->cwd, name);
    path_normalize(dst);
    if (!vfs_write_file(dst, r.body, r.body_len)) { err_f(io, "%s: cannot write %s\n", argv[0], dst); http_free(&r); return 1; }
    if (!quiet) out_f(io, "Saved %s (%u bytes)\n", dst, r.body_len);
    http_free(&r);
    return 0;
}
static int b_sha256sum(sh_t *s, int argc, char **argv, io_t *io) {
    for (int i = 1; i < argc; i++) {
        vnode_t *n = vfs_resolve(argv[i], s->cwd);
        if (!n || n->dir) { err_f(io, "sha256sum: %s: not found\n", argv[i]); continue; }
        char h[65]; sha256_hex(n->data, n->size, h);
        out_f(io, "%s  %s\n", h, argv[i]);
    }
    return 0;
}
/* js: run JavaScript (QuickJS) */
static void js_out(void *u, const char *t) {
    io_t *io = u;
    int n = strlen(t); char *l1 = kmalloc(n * 3 + 1);
    extern int latin1_from_utf8(const uint8_t *s, uint32_t n, char *out);
    int l = latin1_from_utf8((const uint8_t *)t, n, l1); l1[l] = 0;
    out_s(io, l1); kfree(l1);
}
static int b_js(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        out_f(io, "JavaScript engine: QuickJS-ng %s\n\nusage: js FILE.js        run a script file\n       js -e \"CODE\"      run code and print the result, e.g. js -e \"[1,2,3].map(x => x * x)\"\n", js_engine_version());
        return 0;
    }
    if (!strcmp(argv[1], "-e")) {
        char code[2048]; code[0] = 0;
        for (int i = 2; i < argc; i++) { if (i > 2) strlcat(code, " ", sizeof code); strlcat(code, argv[i], sizeof code); }
        return js_shell_run(code, strlen(code), "<command line>", js_out, io);
    }
    vnode_t *n = vfs_resolve(argv[1], s->cwd);
    if (!n || n->dir) { err_f(io, "js: %s: not found\n", argv[1]); return 1; }
    char path[300]; vfs_path(n, path, sizeof path);
    return js_shell_run((const char *)n->data, n->size, path, js_out, io);
}
static int b_display(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        out_f(io, "Adapter:    %s\nResolution: %d x %d, 32-bit colour\n", video_driver_name(), SCREEN_W, SCREEN_H);
        if (!video_can_set_mode()) { out_s(io, "This adapter cannot change resolution.\n"); return 0; }
        static const int mw[] = { 1024, 1152, 1280, 1280, 1280, 1280, 1366, 1440, 1600, 1600, 1680, 1920, 1920, 2560 }, mh[] = { 768, 864, 720, 800, 960, 1024, 768, 900, 900, 1200, 1050, 1080, 1200, 1440 };
        out_s(io, "Available:  ");
        for (unsigned i = 0; i < ARRAY_LEN(mw); i++) if (video_mode_ok(mw[i], mh[i])) out_f(io, "%dx%d ", mw[i], mh[i]);
        out_s(io, "\nusage: display WIDTHxHEIGHT\n");
        return 0;
    }
    int w = atoi(argv[1]); const char *x = strchr(argv[1], 'x'); int h = x ? atoi(x + 1) : 0;
    if (!w || !h) { err_f(io, "display: use WIDTHxHEIGHT, e.g. display 1280x800\n"); return 1; }
    if (!gfx_set_resolution(w, h)) { err_f(io, "display: %dx%d is not supported by %s\n", w, h, video_driver_name()); return 1; }
    out_f(io, "Resolution set to %d x %d\n", SCREEN_W, SCREEN_H);
    return 0;
}
static int b_play(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: play FILE.wav [-w]   (-w waits until it has finished)\n"); return 1; }
    char full[512]; vnode_t *n = vfs_resolve(argv[1], s->cwd);
    if (!n) { err_f(io, "play: %s: not found\n", argv[1]); return 1; }
    vfs_path(n, full, sizeof full);
    char err[96];
    if (!audio_play_file(full, err, sizeof err)) { err_f(io, "play: %s\n", err); return 1; }
    if (argc > 2 && !strcmp(argv[2], "-w")) audio_wait(600000);
    return 0;
}
static int b_volume(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc > 1) { if (!strcmp(argv[1], "mute")) audio_muted = true; else if (!strcmp(argv[1], "unmute")) audio_muted = false; else audio_volume = CLAMP(atoi(argv[1]), 0, 100); }
    out_f(io, "Sound device: %s\nVolume: %d%%%s\n", audio_device_name(), audio_volume, audio_muted ? " (muted)" : "");
    return 0;
}
static int b_hostname(sh_t *s, int argc, char **argv, io_t *io) { out_f(io, "%s\n", computer_name); return 0; }
static int b_ps(sh_t *s, int argc, char **argv, io_t *io) {
    static const char *kinds[] = { "kernel", "shell", "win32", "app" };
    out_f(io, "  PID  STATE     CPU(ms)   MEM(KB)  TYPE    NAME\n");
    for (task_t *t = task_list(); t; t = t->next) {
        const char *st = t->state == TASK_READY ? "running" : t->state == TASK_SLEEPING ? "sleeping" : t->state == TASK_BLOCKED ? "blocked" : "dead";
        out_f(io, "%5d  %-8s %8u %9u  %-6s  %s\n", t->pid, st, t->cpu_ms, t->mem_bytes / 1024, kinds[t->kind & 3], t->name);
    }
    return 0;
}
static int b_kill(sh_t *s, int argc, char **argv, io_t *io) {
    int rc = 0;
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') continue; int pid = atoi(argv[i]); if (!task_kill(pid)) { err_f(io, "kill: (%d) - No such process\n", pid); rc = 1; } }
    return rc;
}
static int b_sync(sh_t *s, int argc, char **argv, io_t *io) {
    if (!system_volume) { out_s(io, "sync: this is the live session from the CD; nothing is saved to a disk.\nInstall " OS_NAME " (desktop icon or Control Panel > System) to keep your files.\n"); return 0; }
    int n = persist_pending();
    persist_flush();
    char st[120]; persist_status(st, sizeof st);
    out_f(io, "sync: %d changed file(s) written. %s\n", n, st);
    return 0;
}
static int b_free(sh_t *s, int argc, char **argv, io_t *io) {
    out_f(io, "              total        used        free\nMem:     %10u  %10u  %10u  (KB)\n", mm_total() / 1024, mm_used() / 1024, (mm_total() - mm_used()) / 1024);
    out_f(io, "Physical: %u MB installed\n", mm_phys_total() / (1024 * 1024));
    return 0;
}
static int b_uptime(sh_t *s, int argc, char **argv, io_t *io) {
    uint32_t t = timer_ticks() / 1000;
    out_f(io, "up %u:%02u:%02u, %d tasks, load: %u%% idle\n", t / 3600, (t / 60) % 60, t % 60, task_count(), timer_ticks() ? (uint32_t)((uint64_t)idle_ticks * 100 / timer_ticks()) : 0);
    return 0;
}
static int b_dmesg(sh_t *s, int argc, char **argv, io_t *io) { out_s(io, klog_buffer()); return 0; }
static int b_export(sh_t *s, int argc, char **argv, io_t *io) {
    for (int i = 1; i < argc; i++) { char *eq = strchr(argv[i], '='); if (eq) { *eq = 0; var_set(s, argv[i], eq + 1); *eq = '='; } }
    return 0;
}
static int b_unset(sh_t *s, int argc, char **argv, io_t *io) { for (int i = 1; i < argc; i++) var_unset(s, argv[i]); return 0; }
static int b_env(sh_t *s, int argc, char **argv, io_t *io) { for (var_t *v = s->vars; v; v = v->next) out_f(io, "%s=%s\n", v->name, v->value); return 0; }
static int b_read(sh_t *s, int argc, char **argv, io_t *io) {
    int i = 1; const char *prompt = NULL;
    while (i < argc && argv[i][0] == '-') { if (!strcmp(argv[i], "-p") && i + 1 < argc) { prompt = argv[i + 1]; i += 2; } else i++; }
    if (prompt) out_s(io, prompt);
    char line[512];
    int n = io->in->readline(io->in, line, sizeof line);
    if (n < 0) return 1;
    if (i >= argc) { var_set(s, "REPLY", line); return 0; }
    char *p = line;
    for (; i < argc; i++) {
        while (*p == ' ' || *p == '\t') p++;
        if (i == argc - 1) { var_set(s, argv[i], p); break; }
        char *e = p; while (*e && *e != ' ' && *e != '\t') e++;
        char save = *e; *e = 0; var_set(s, argv[i], p); *e = save; p = e;
    }
    return 0;
}
static bool test_eval(sh_t *s, int argc, char **argv) {
    if (argc == 0) return false;
    if (argv[0][0] == '!' && !argv[0][1]) return !test_eval(s, argc - 1, argv + 1);
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-a")) return test_eval(s, i, argv) && test_eval(s, argc - i - 1, argv + i + 1);
        if (!strcmp(argv[i], "-o")) return test_eval(s, i, argv) || test_eval(s, argc - i - 1, argv + i + 1);
    }
    if (argc == 1) return argv[0][0] != 0;
    if (argc == 2) {
        const char *op = argv[0], *a = argv[1];
        if (!strcmp(op, "-z")) return !*a;
        if (!strcmp(op, "-n")) return *a;
        vnode_t *n = vfs_resolve(a, s->cwd);
        if (!strcmp(op, "-e")) return n != NULL;
        if (!strcmp(op, "-f")) return n && !n->dir;
        if (!strcmp(op, "-d")) return n && n->dir;
        if (!strcmp(op, "-s")) return n && n->size > 0;
        if (!strcmp(op, "-r") || !strcmp(op, "-x")) return n != NULL;
        if (!strcmp(op, "-w")) return n && !n->readonly;
        return false;
    }
    if (argc == 3) {
        const char *a = argv[0], *op = argv[1], *b = argv[2];
        if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
        if (!strcmp(op, "!=")) return strcmp(a, b);
        long x = strtol(a, NULL, 10), y = strtol(b, NULL, 10);
        if (!strcmp(op, "-eq")) return x == y; if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y; if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y; if (!strcmp(op, "-ge")) return x >= y;
        if (!strcmp(op, "<")) return strcmp(a, b) < 0; if (!strcmp(op, ">")) return strcmp(a, b) > 0;
    }
    return false;
}
static int b_test(sh_t *s, int argc, char **argv, io_t *io) {
    int n = argc - 1;
    if (!strcmp(argv[0], "[") || !strcmp(argv[0], "[[")) { if (n < 1 || (strcmp(argv[n], "]") && strcmp(argv[n], "]]"))) { err_f(io, "[: missing ']'\n"); return 2; } n--; }
    return test_eval(s, n, argv + 1) ? 0 : 1;
}
static int b_seq(sh_t *s, int argc, char **argv, io_t *io) {
    long a = 1, b = 1, st = 1;
    if (argc == 2) b = atoi(argv[1]); else if (argc == 3) { a = atoi(argv[1]); b = atoi(argv[2]); } else if (argc >= 4) { a = atoi(argv[1]); st = atoi(argv[2]); b = atoi(argv[3]); }
    if (!st) return 1;
    for (long i = a; st > 0 ? i <= b : i >= b; i += st) out_f(io, "%ld\n", i);
    return 0;
}
static int b_expr(sh_t *s, int argc, char **argv, io_t *io) {
    char buf[512] = ""; for (int i = 1; i < argc; i++) { strlcat(buf, argv[i], sizeof buf); strlcat(buf, " ", sizeof buf); }
    long v = arith(s, buf); out_f(io, "%ld\n", v); return v == 0;
}
static int b_let(sh_t *s, int argc, char **argv, io_t *io) { long v = 0; for (int i = 1; i < argc; i++) v = arith(s, argv[i]); return v == 0; }
static int b_basename(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) return 1;
    char b[256]; strlcpy(b, path_basename(argv[1]), sizeof b);
    if (argc > 2) { const char *e = str_ends_with_ci(b, argv[2]); if (e && e != b) b[e - b] = 0; }
    out_f(io, "%s\n", b); return 0;
}
static int b_dirname(sh_t *s, int argc, char **argv, io_t *io) { if (argc < 2) return 1; char b[256]; path_dirname(argv[1], b, sizeof b); out_f(io, "%s\n", b[0] ? b : "."); return 0; }
static int b_help(sh_t *s, int argc, char **argv, io_t *io);
static int b_type(sh_t *s, int argc, char **argv, io_t *io);
static int b_history(sh_t *s, int argc, char **argv, io_t *io) { out_s(io, "Use the Up/Down arrow keys to browse command history.\n"); return 0; }
static int b_source(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) return 1;
    vnode_t *n = vfs_resolve(argv[1], s->cwd);
    if (!n || n->dir) { err_f(io, "source: %s: not found\n", argv[1]); return 1; }
    char *src = strndup((char *)n->data, n->size);
    int r = run_string(s, src, io); kfree(src); return r;
}
static int b_beep(sh_t *s, int argc, char **argv, io_t *io) { speaker_beep(argc > 1 ? atoi(argv[1]) : 880, argc > 2 ? atoi(argv[2]) : 150); return 0; }
static int b_color(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) { io->out->set_color(io->out, 7, 0); return 0; }
    int v = strtol(argv[1], NULL, 16); io->out->set_color(io->out, v & 15, (v >> 4) & 15); return 0;
}
static int b_title(sh_t *s, int argc, char **argv, io_t *io) {
    char b[128] = ""; for (int i = 1; i < argc; i++) { if (i > 1) strlcat(b, " ", sizeof b); strlcat(b, argv[i], sizeof b); }
    s->con->set_title(s->con, b); return 0;
}
static void tree_rec(io_t *io, vnode_t *d, char *prefix, int depth) {
    if (depth > 12) return;
    vfs_sort_children(d);
    for (vnode_t *c = d->child; c; c = c->next) {
        bool last = !c->next;
        out_f(io, "%s%s%s%s\n", prefix, last ? "\\---" : "+---", c->name, c->dir ? "" : "");
        if (c->dir) { int l = strlen(prefix); strcat(prefix, last ? "    " : "|   "); tree_rec(io, c, prefix, depth + 1); prefix[l] = 0; }
    }
}
static int b_tree(sh_t *s, int argc, char **argv, io_t *io) {
    vnode_t *d = vfs_resolve(argc > 1 ? argv[1] : ".", s->cwd);
    if (!d || !d->dir) { err_f(io, "tree: invalid path\n"); return 1; }
    char p[256]; vfs_path(d, p, sizeof p); out_f(io, "%s\n", p);
    char prefix[256] = ""; tree_rec(io, d, prefix, 0); return 0;
}
static void find_rec(sh_t *s, io_t *io, vnode_t *d, const char *base, const char *pat) {
    for (vnode_t *c = d->child; c; c = c->next) {
        char p[512]; snprintf(p, sizeof p, "%s\\%s", base, c->name);
        if (!pat || glob_match(pat, c->name)) out_f(io, "%s\n", p);
        if (c->dir) find_rec(s, io, c, p, pat);
    }
}
static int b_find(sh_t *s, int argc, char **argv, io_t *io) {
    const char *start = "."; const char *pat = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-name") && i + 1 < argc) pat = argv[++i]; else start = argv[i]; }
    vnode_t *d = vfs_resolve(start, s->cwd);
    if (!d) { err_f(io, "find: '%s': No such file or directory\n", start); return 1; }
    find_rec(s, io, d, start, pat); return 0;
}
static int b_hexdump(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) return 1;
    vnode_t *n = vfs_resolve(argv[argc - 1], s->cwd);
    if (!n || n->dir) { err_f(io, "hexdump: cannot open\n"); return 1; }
    uint32_t lim = MIN(n->size, 512u);
    for (uint32_t o = 0; o < lim; o += 16) {
        char line[100]; int k = snprintf(line, sizeof line, "%08x  ", o);
        for (int i = 0; i < 16; i++) k += o + i < lim ? snprintf(line + k, sizeof line - k, "%02x ", n->data[o + i]) : snprintf(line + k, sizeof line - k, "   ");
        k += snprintf(line + k, sizeof line - k, " |");
        for (int i = 0; i < 16 && o + i < lim; i++) { char c = n->data[o + i]; line[k++] = (c >= 32 && c < 127) ? c : '.'; }
        line[k++] = '|'; line[k] = 0;
        out_f(io, "%s\n", line);
    }
    if (n->size > lim) out_f(io, "... (%u more bytes)\n", n->size - lim);
    return 0;
}
static int b_mount(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) {
        for (char l = 'C'; l <= 'Z'; l++) if (vfs_drive_kind(l)) out_f(io, "%c:  %-24s %s\n", l, vfs_drive_label(l), vfs_drive_kind(l) == 2 ? "(disk image, read-only)" : "(ramdisk)");
        return 0;
    }
    vnode_t *n = vfs_resolve(argv[1], s->cwd);
    if (!n || n->dir) { err_f(io, "mount: %s: not found\n", argv[1]); return 1; }
    char msg[400]; char l = dmg_mount(n, msg, sizeof msg);
    if (!l) { err_f(io, "mount: %s\n", msg); return 1; }
    out_f(io, "%s\n", msg); return 0;
}
static int b_hdiutil(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc >= 3 && (!strcmp(argv[1], "attach") || !strcmp(argv[1], "mount"))) { char *a[2] = { "mount", argv[2] }; return b_mount(s, 2, a, io); }
    if (argc >= 3 && (!strcmp(argv[1], "detach") || !strcmp(argv[1], "eject"))) {
        char l = toupper(argv[2][0]);
        if (!vfs_unmount(l)) { err_f(io, "hdiutil: cannot detach %s\n", argv[2]); return 1; }
        out_f(io, "\"%c:\" ejected.\n", l); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "imageinfo")) {
        vnode_t *n = vfs_resolve(argv[2], s->cwd);
        if (!n || n->dir) { err_f(io, "hdiutil: not found\n"); return 1; }
        char info[200]; uint32_t sz; uint8_t *d = dmg_extract(n->data, n->size, &sz, info, sizeof info);
        out_f(io, "%s\n", info); if (d) { out_f(io, "Decoded size: %u bytes\n", sz); kfree(d); }
        return d ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "info")) { char *a[1] = { "mount" }; return b_mount(s, 1, a, io); }
    out_s(io, "usage: hdiutil attach IMAGE.dmg | detach D: | imageinfo IMAGE.dmg | info\n");
    return 1;
}
typedef struct { char app[32]; char arg[256]; } launch_req_t;
static void do_launch(void *p) { launch_req_t *r = p; if (!strcmp(r->app, "::open")) shell_open(r->arg, NULL); else app_launch(r->app, r->arg[0] ? r->arg : NULL); kfree(r); }
static void gui_launch(const char *app, const char *arg) {
    launch_req_t *r = kzalloc(sizeof *r); strlcpy(r->app, app, sizeof r->app); if (arg) strlcpy(r->arg, arg, sizeof r->arg);
    wm_defer(do_launch, r);
}
static int b_open(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2) { err_f(io, "usage: open FILE|FOLDER|APP\n"); return 1; }
    vnode_t *n = vfs_resolve(argv[1], s->cwd);
    if (!n) { if (app_find(argv[1])) { gui_launch(argv[1], argc > 2 ? argv[2] : NULL); return 0; } err_f(io, "open: %s: not found\n", argv[1]); return 1; }
    char full[512]; vfs_path(n, full, sizeof full);
    gui_launch("::open", full);
    return 0;
}
static int b_theme(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc < 2 || !strcmp(argv[1], "list")) {
        styles_load();
        out_f(io, "Current style: %s, hue %d, saturation %d, lightness %d\n\nVisual styles (" STYLE_DIR "\\*.wrldstyle):\n", style_name(theme_id), theme_hue, theme_sat, theme_light);
        for (int i = 0; i < THEME_COUNT; i++) out_f(io, "  %c %-18s %s\n", i == theme_id ? '*' : ' ', style_file(i), style_desc(i));
        out_s(io, "\nusage: theme STYLE [HUE 0-359]   e.g. theme aero, theme plex 200, theme \"Luna Blue\"\n"
                  "       theme saturation -100..100   theme lightness -60..60\n");
        return 0;
    }
    if (argc > 2 && (!strcasecmp(argv[1], "saturation") || !strcasecmp(argv[1], "lightness"))) {
        if (tolower(argv[1][0]) == 's') theme_set_tone(atoi(argv[2]), theme_light); else theme_set_tone(theme_sat, atoi(argv[2]));
        wm_redraw_all();
        out_f(io, "Saturation %d, lightness %d\n", theme_sat, theme_light);
        return 0;
    }
    static const struct { const char *alias, *file; } aliases[] = { { "blue", "Luna Blue" }, { "luna", "Luna Blue" }, { "olive", "Olive Green" }, { "classic", "WRLD Classic" } };
    const char *want = argv[1];
    for (unsigned i = 0; i < ARRAY_LEN(aliases); i++) if (!strcasecmp(want, aliases[i].alias)) want = aliases[i].file;
    styles_load();
    int id = style_find(want);
    if (id < 0) { err_f(io, "theme: no visual style called '%s' (try: theme list)\n", argv[1]); return 1; }
    theme_apply(id, argc > 2 ? atoi(argv[2]) : 0);
    wm_redraw_all();
    out_f(io, "Visual style set to %s (hue %d)\n", style_name(theme_id), theme_hue);
    return 0;
}
static int b_pause(sh_t *s, int argc, char **argv, io_t *io) { out_s(io, "Press any key to continue . . . "); s->con->getkey(s->con); out_s(io, "\n"); return 0; }
static int b_shutdown(sh_t *s, int argc, char **argv, io_t *io) {
    bool r = argc > 1 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "/r"));
    out_s(io, r ? "Restarting...\n" : "Shutting down...\n");
    wm_defer(shell_power_action, r ? (void *)1 : NULL);
    return 0;
}
static int b_xargs(sh_t *s, int argc, char **argv, io_t *io);
static int b_lsblk(sh_t *s, int argc, char **argv, io_t *io) {
    if (!blk_count()) { out_s(io, "No hard disks.\n"); return 0; }
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        char b[128]; blk_describe(d, b, sizeof b);
        out_f(io, "Disk %d  %s\n", i, b);
        part_t p[4];
        if (!mbr_read(d, p)) { out_s(io, "        (no partition table)\n"); continue; }
        for (int k = 0; k < 4; k++)
            if (p[k].type) out_f(io, "        Partition %d  %-12s %8u MB%s\n", k + 1, part_type_name(p[k].type), p[k].sectors / 2048, p[k].boot ? "  active" : "");
    }
    return 0;
}
static int b_lspci(sh_t *s, int argc, char **argv, io_t *io) {
    pci_dev_t *d; int n = pci_list(&d);
    out_s(io, "BUS:DEV.FN  VENDOR:DEVICE  CLASS\n");
    for (int i = 0; i < n; i++) out_f(io, "%02x:%02x.%x     %04x:%04x      %s (%s)\n", d[i].bus, d[i].dev, d[i].fn, d[i].vendor, d[i].device, pci_class_name(d[i].cls, d[i].sub), pci_vendor_name(d[i].vendor));
    return 0;
}
static int b_return(sh_t *s, int argc, char **argv, io_t *io) { s->ret = true; return argc > 1 ? atoi(argv[1]) : s->status; }
static int b_break(sh_t *s, int argc, char **argv, io_t *io) { s->brk = argc > 1 ? atoi(argv[1]) : 1; return 0; }
static int b_continue(sh_t *s, int argc, char **argv, io_t *io) { s->cont = 1; return 0; }
static int b_shift(sh_t *s, int argc, char **argv, io_t *io) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    while (n-- > 0 && s->argc > 1) { memmove(s->argv + 1, s->argv + 2, (s->argc - 2) * sizeof(char *)); s->argc--; }
    return 0;
}
static int b_cpuinfo(sh_t *s, int argc, char **argv, io_t *io) { char v[16], b[52]; cpu_get_info(v, b); out_f(io, "vendor: %s\nmodel:  %s\n", v, b); return 0; }

static int b_sh(sh_t *s, int argc, char **argv, io_t *io) {
    if (argc > 2 && !strcmp(argv[1], "-c")) { sh_t *c = sh_new(s->con, s->cwd); for (var_t *v = s->vars; v; v = v->next) var_set(c, v->name, v->value); int r = run_string(c, argv[2], io); sh_free(c); return r; }
    if (argc > 1) return sh_run_file_io(s, argv[1], argc - 1, argv + 1, io);
    out_s(io, "(nested shells are not needed: you are already in sh)\n");
    return 0;
}
static const builtin_t builtins[] = {
    { "sh", b_sh, "run a script (sh FILE / sh -c CMD)" }, { "bash", b_sh, NULL },
    { "echo", b_echo, "print arguments" }, { "printf", b_printf, "formatted print" }, { "cd", b_cd, "change directory" },
    { "pwd", b_pwd, "print working directory" }, { "ls", b_ls, "list files (-l -a)" }, { "dir", b_dir, "list files (DOS style)" },
    { "cat", b_cat, "print files" }, { "type", b_type, "describe a command / print file" }, { "mkdir", b_mkdir, "make directories (-p)" }, { "md", b_mkdir, NULL },
    { "rmdir", b_rm, NULL }, { "rd", b_rm, NULL }, { "rm", b_rm, "remove files (-r -f)" }, { "del", b_rm, NULL }, { "touch", b_touch, "create empty file" },
    { "cp", b_cp, "copy files" }, { "copy", b_cp, NULL }, { "mv", b_mv, "move/rename" }, { "move", b_mv, NULL }, { "ren", b_mv, NULL },
    { "head", b_headtail, "first lines" }, { "tail", b_headtail, "last lines" }, { "wc", b_wc, "count lines/words" },
    { "grep", b_grep, "search text (-i -n -v -c)" }, { "sort", b_sort, "sort lines (-r -n -u)" }, { "uniq", b_uniq, "drop repeated lines" },
    { "rev", b_rev, "reverse lines" }, { "tr", b_tr, "translate characters" },
    { "clear", b_clear, "clear the screen" }, { "cls", b_clear, NULL }, { "exit", b_exit, "exit the shell" },
    { "true", b_true, NULL }, { "false", b_false, NULL }, { ":", b_true, NULL }, { "sleep", b_sleep, "wait N seconds" },
    { "date", b_date, "show date (+FORMAT)" }, { "uname", b_uname, "system name (-a)" }, { "ver", b_ver, "OS version" },
    { "whoami", b_whoami, NULL }, { "hostname", b_hostname, NULL }, { "ipconfig", b_ipconfig, "network configuration (/renew)" }, { "ifconfig", b_ipconfig, NULL },
    { "ping", b_ping, "send ICMP echo requests" }, { "nslookup", b_nslookup, "look up a host name" },
    { "wget", b_wget, "download a URL (http/https)" }, { "sha256sum", b_sha256sum, "SHA-256 of files" }, { "js", b_js, "run JavaScript (js FILE or js -e CODE)" }, { "display", b_display, "show or change the screen resolution" },
    { "play", b_play, "play a .wav file" }, { "volume", b_volume, "show or set the volume (0-100, mute)" }, { "curl", b_wget, "print a URL" }, { "ps", b_ps, "list processes" }, { "tasklist", b_ps, NULL },
    { "kill", b_kill, "terminate a process" }, { "taskkill", b_kill, NULL }, { "free", b_free, "memory usage" }, { "sync", b_sync, "write changes to the system disk now" }, { "mem", b_free, NULL },
    { "uptime", b_uptime, NULL }, { "dmesg", b_dmesg, "kernel log" }, { "export", b_export, "set variables" }, { "set", b_env, NULL },
    { "unset", b_unset, NULL }, { "env", b_env, "list variables" }, { "read", b_read, "read a line into variables" },
    { "test", b_test, "evaluate conditions" }, { "[", b_test, NULL }, { "[[", b_test, NULL }, { "seq", b_seq, "number sequences" },
    { "expr", b_expr, "arithmetic" }, { "let", b_let, NULL }, { "basename", b_basename, NULL }, { "dirname", b_dirname, NULL },
    { "help", b_help, "this help" }, { "history", b_history, NULL }, { "source", b_source, "run script in this shell" }, { ".", b_source, NULL },
    { "beep", b_beep, "PC speaker beep" }, { "color", b_color, "console colours (hex)" }, { "title", b_title, "window title" },
    { "tree", b_tree, "directory tree" }, { "find", b_find, "find files (-name PAT)" }, { "hexdump", b_hexdump, "hex dump a file" }, { "xxd", b_hexdump, NULL },
    { "mount", b_mount, "mount a .dmg / list drives" }, { "hdiutil", b_hdiutil, "disk image utility" }, { "open", b_open, "open with default app" }, { "start", b_open, NULL },
    { "theme", b_theme, "change the visual style" }, { "pause", b_pause, NULL }, { "shutdown", b_shutdown, "power off (-r reboot)" },
    { "lspci", b_lspci, "list PCI devices" }, { "lsblk", b_lsblk, "list disks and partitions" }, { "return", b_return, NULL }, { "break", b_break, NULL }, { "continue", b_continue, NULL },
    { "shift", b_shift, NULL }, { "cpuinfo", b_cpuinfo, NULL }, { "xargs", b_xargs, "build commands from input" },
};
static const builtin_t *find_builtin(const char *name) { for (unsigned i = 0; i < ARRAY_LEN(builtins); i++) if (!strcmp(builtins[i].name, name)) return &builtins[i]; return NULL; }
static int b_help(sh_t *s, int argc, char **argv, io_t *io) {
    out_s(io, "\x1b[93mWRLD shell - built-in commands:\x1b[0m\n");
    int col = 0;
    for (unsigned i = 0; i < ARRAY_LEN(builtins); i++) {
        if (!builtins[i].help) continue;
        char b[80]; snprintf(b, sizeof b, "  \x1b[96m%-9s\x1b[0m %-28s", builtins[i].name, builtins[i].help);
        out_s(io, b); if (++col == 2) { out_s(io, "\n"); col = 0; }
    }
    if (col) out_s(io, "\n");
    out_s(io, "\n\x1b[93mApplications:\x1b[0m notepad calc paint explorer control taskmgr snake winver\n");
    out_s(io, "\x1b[93mPrograms:\x1b[0m run FILE.exe (Win32 PE), FILE.sh (shell script), open FILE.dmg (disk image)\n");
    out_s(io, "\x1b[93mSyntax:\x1b[0m VAR=x  $VAR ${VAR:-d}  $(cmd)  $((1+2))  a | b  > >> < 2>&1  && ||\n");
    out_s(io, "        if/elif/else/fi  for x in ...; do ... done  while/until  case ... esac  f() { ... }\n");
    return 0;
}
static int resolve_external(sh_t *s, const char *name, char *out, int cap);
int sh_run_file_io(sh_t *parent, const char *path, int argc, char **argv, io_t *io);
static int b_type(sh_t *s, int argc, char **argv, io_t *io) {
    if (!strcmp(argv[0], "type") && argc > 1 && vfs_resolve(argv[1], s->cwd) && !find_builtin(argv[1])) { char *a[2] = { "cat", argv[1] }; return b_cat(s, 2, a, io); }
    for (int i = 1; i < argc; i++) {
        char p[256];
        bool fn = false; for (func_t *f = s->funcs; f; f = f->next) if (!strcmp(f->name, argv[i])) fn = true;
        if (fn) out_f(io, "%s is a function\n", argv[i]);
        else if (find_builtin(argv[i])) out_f(io, "%s is a shell builtin\n", argv[i]);
        else if (app_find(argv[i])) out_f(io, "%s is a graphical application\n", argv[i]);
        else if (resolve_external(s, argv[i], p, sizeof p)) out_f(io, "%s is %s\n", argv[i], p);
        else { out_f(io, "%s: not found\n", argv[i]); return 1; }
    }
    return 0;
}

/* ======================= execution ======================= */
static int resolve_external(sh_t *s, const char *name, char *out, int cap) {
    vnode_t *n = NULL;
    if (strchr(name, '/') || strchr(name, '\\') || (name[1] == ':')) n = vfs_resolve(name, s->cwd);
    else {
        const char *path = var_get(s, "PATH");
        char pbuf[512]; strlcpy(pbuf, path ? path : ".", sizeof pbuf);
        const char *exts[] = { "", ".exe", ".sh", ".app" };
        for (char *d = pbuf; d && !n;) {
            char *e = strchr(d, ';'); if (e) *e = 0;
            for (unsigned k = 0; k < ARRAY_LEN(exts) && !n; k++) {
                char p[512]; snprintf(p, sizeof p, "%s\\%s%s", d, name, exts[k]);
                vnode_t *c = vfs_resolve(p, s->cwd);
                if (c && (!c->dir || str_ends_with_ci(c->name, ".app"))) n = c;
            }
            d = e ? e + 1 : NULL;
        }
    }
    if (!n) return 0;
    vfs_path(n, out, cap);
    return 1;
}
static int wait_pid(sh_t *s, int pid) {
    while (task_find(pid)) {
        if (interrupted(s)) { task_kill(pid); console_clear_interrupt(s->con); return 130; }
        task_sleep(15);
    }
    return task_exit_code(pid);
}
int sh_run_file_io(sh_t *parent, const char *path, int argc, char **argv, io_t *io);
static int run_external(sh_t *s, int argc, char **argv, io_t *io) {
    char path[256];
    if (!resolve_external(s, argv[0], path, sizeof path)) {
        if (app_find(argv[0]) || !strcasecmp(argv[0], "notepad") || !strcasecmp(argv[0], "mspaint")) {
            char full[512] = "";
            if (argc > 1) { vnode_t *n = vfs_resolve(argv[1], s->cwd); if (n) vfs_path(n, full, sizeof full); else if (strcasecmp(argv[0], "control")) { if (isalpha(argv[1][0]) && argv[1][1] == ':') strlcpy(full, argv[1], sizeof full); else path_join(full, sizeof full, s->cwd, argv[1]); } else strlcpy(full, argv[1], sizeof full); }
            if (!strcasecmp(argv[0], "explorer") && argc == 1) strlcpy(full, s->cwd, sizeof full);
            gui_launch(argv[0], full[0] ? full : NULL);
            return 0;
        }
        err_f(io, "%s: command not found\n", argv[0]);
        return 127;
    }
    vnode_t *n = vfs_resolve(path, NULL);
    const char *ext = vfs_ext(n->name);
    if (n->dir) { if (str_ends_with_ci(n->name, ".app")) { gui_launch("::open", path); return 0; } err_f(io, "%s: is a directory\n", argv[0]); return 126; }
    if (!strcasecmp(ext, "exe") || !strcasecmp(ext, "com") || (n->size > 2 && n->data[0] == 'M' && n->data[1] == 'Z')) {
        char args[512] = "";
        for (int i = 1; i < argc; i++) { if (i > 1) strlcat(args, " ", sizeof args); bool q = strchr(argv[i], ' ') != NULL; if (q) strlcat(args, "\"", sizeof args); strlcat(args, argv[i], sizeof args); if (q) strlcat(args, "\"", sizeof args); }
        char err[256];
        int pid = pe_spawn(path, args, io->out, err, sizeof err);
        if (pid < 0) { err_f(io, "%s: %s\n", argv[0], err); return 126; }
        extern void pe_set_stdin(int pid, console_t *in);
        extern bool pe_is_gui(vnode_t *n);
        pe_set_stdin(pid, io->in);
        if (pe_is_gui(n)) { out_f(io, "Started %s (pid %d)\n", n->name, pid); return 0; }
        return wait_pid(s, pid);
    }
    if (!strcasecmp(ext, "sh") || !strcasecmp(ext, "bash") || (n->size > 2 && n->data[0] == '#' && n->data[1] == '!')) return sh_run_file_io(s, path, argc, argv, io);
    if (!strcasecmp(ext, "dmg")) { char *a[2] = { "mount", path }; return b_mount(s, 2, a, io); }
    char desc[128];
    if (macho_probe(n->data, n->size, desc, sizeof desc)) { err_f(io, "%s: cannot execute: %s (native macOS binaries are not supported)\n", argv[0], desc); return 126; }
    if (n->size >= 4 && !memcmp(n->data, "\x7F" "ELF", 4)) { err_f(io, "%s: cannot execute: Linux ELF binary\n", argv[0]); return 126; }
    gui_launch("::open", path);
    return 0;
}
static int b_xargs(sh_t *s, int argc, char **argv, io_t *io) {
    bool ok; char *d = gather_input(s, 1, argv, 1, io, &ok);
    strv_t v = { 0 };
    for (int i = 1; i < argc; i++) sv_push(&v, strdup(argv[i]));
    if (!v.n) sv_push(&v, strdup("echo"));
    char *p = d;
    while (*p) { while (isspace(*p)) p++; if (!*p) break; char *e = p; while (*e && !isspace(*e)) e++; sv_push(&v, strndup(p, e - p)); p = e; }
    const builtin_t *b = find_builtin(v.v[0]);
    int r = b ? b->fn(s, v.n, v.v, io) : run_external(s, v.n, v.v, io);
    sv_free(&v); kfree(d);
    return r;
}

static int run_simple(sh_t *s, node_t *n, io_t *io) {
    strv_t argv = { 0 };
    /* leading assignments */
    int wi = 0;
    for (; wi < n->nwords; wi++) {
        char *w = n->words[wi]; char *eq = strchr(w, '=');
        if (!eq || eq == w) break;
        bool ok = true; for (char *c = w; c < eq; c++) if (!isalnum(*c) && *c != '_') ok = false;
        if (!ok || isdigit(w[0])) break;
    }
    expand_words(s, n->words + wi, n->nwords - wi, &argv);
    if (argv.n == 0) {
        for (int i = 0; i < wi; i++) { char *w = n->words[i]; char *eq = strchr(w, '='); *eq = 0; char *v = expand_single(s, eq + 1); var_set(s, w, v); kfree(v); *eq = '='; }
        sv_free(&argv);
        return 0;
    }
    for (int i = 0; i < wi; i++) { char *w = n->words[i]; char *eq = strchr(w, '='); *eq = 0; char *v = expand_single(s, eq + 1); var_set(s, w, v); kfree(v); *eq = '='; }
    /* redirections */
    io_t rio = *io;
    bufcon_t *outbuf = NULL, *inbuf = NULL, *errbuf = NULL;
    vnode_t *outfile = NULL, *errfile = NULL; bool append = false, eappend = false;
    for (int i = 0; i < n->nredir; i++) {
        redir_t *r = &n->redirs[i];
        if (r->type == T_2TO1) { rio.err = rio.out; continue; }
        char *t = expand_single(s, r->target);
        if (r->type == T_LT && (!strcmp(t, "/dev/null") || !strcasecmp(t, "nul"))) { inbuf = bufcon_new(s->con); rio.in = &inbuf->c; }
        else if (r->type == T_LT) {
            vnode_t *f = vfs_resolve(t, s->cwd);
            if (!f || f->dir) { err_f(io, "%s: No such file\n", t); kfree(t); sv_free(&argv); return 1; }
            inbuf = bufcon_new(s->con); bc_write(&inbuf->c, (char *)f->data, f->size); rio.in = &inbuf->c;
        } else if (!strcmp(t, "/dev/null") || !strcasecmp(t, "nul")) {
            bufcon_t *b = bufcon_new(s->con);
            if (r->type == T_2GT) { errbuf = b; rio.err = &b->c; errfile = NULL; } else { outbuf = b; rio.out = &b->c; outfile = NULL; }
        } else {
            vnode_t *f = vfs_create(t, s->cwd, false);
            if (!f || f->dir || f->readonly) { err_f(io, "%s: cannot write\n", t); kfree(t); sv_free(&argv); return 1; }
            bufcon_t *b = bufcon_new(s->con);
            if (r->type == T_2GT) { errbuf = b; errfile = f; rio.err = &b->c; }
            else { outbuf = b; outfile = f; append = r->type == T_GTGT; rio.out = &b->c; }
        }
        kfree(t);
    }
    int rc;
    func_t *fn = NULL; for (func_t *f = s->funcs; f; f = f->next) if (!strcmp(f->name, argv.v[0])) fn = f;
    const builtin_t *b = fn ? NULL : find_builtin(argv.v[0]);
    if (fn) {
        int oargc = s->argc; char **oargv = s->argv;
        s->argc = argv.n; s->argv = argv.v;
        rc = run_node(s, fn->body, &rio);
        s->ret = false;
        s->argc = oargc; s->argv = oargv;
    } else if (b) rc = b->fn(s, argv.n, argv.v, &rio);
    else rc = run_external(s, argv.n, argv.v, &rio);
    if (outbuf) { if (outfile) { if (append) vfs_append_node(outfile, outbuf->buf, outbuf->len); else vfs_write_node(outfile, outbuf->buf, outbuf->len); } bufcon_free(outbuf); }
    if (errbuf) { if (errfile) vfs_write_node(errfile, errbuf->buf, errbuf->len); bufcon_free(errbuf); (void)eappend; }
    bufcon_free(inbuf);
    sv_free(&argv);
    return rc;
}
static int run_node(sh_t *s, node_t *n, io_t *io) {
    if (!n) return 0;
    if (s->exiting || interrupted(s)) return s->status;
    if (++s->depth > 200) { err_f(io, "sh: maximum recursion depth exceeded\n"); s->depth--; return 1; }
    int rc = 0;
    switch (n->type) {
    case N_LIST:
        for (node_t *c = n; c; c = c->next) {
            rc = run_node(s, c->a, io); s->status = rc;
            if (s->exiting || s->brk || s->cont || s->ret || interrupted(s)) break;
            task_yield();
        }
        break;
    case N_SIMPLE: rc = run_simple(s, n, io); break;
    case N_NOT: rc = !run_node(s, n->a, io); break;
    case N_AND: rc = run_node(s, n->a, io); s->status = rc; if (!rc) rc = run_node(s, n->b, io); break;
    case N_OR: rc = run_node(s, n->a, io); s->status = rc; if (rc) rc = run_node(s, n->b, io); break;
    case N_PIPE: {
        bufcon_t *prev = NULL;
        for (node_t *c = n->a; c; c = c->next) {
            io_t pio = *io;
            if (prev) { pio.in = &prev->c; }
            bufcon_t *cur = NULL;
            if (c->next) { cur = bufcon_new(s->con); pio.out = &cur->c; }
            node_t save = *c; save.next = NULL;
            node_t *one = &save;
            rc = run_node(s, one, &pio);
            bufcon_free(prev);
            prev = cur;
        }
        bufcon_free(prev);
        break;
    }
    case N_IF:
        if (!run_node(s, n->a, io)) rc = run_node(s, n->b, io);
        else if (n->c) rc = run_node(s, n->c, io);
        else rc = 0;
        break;
    case N_WHILE: case N_UNTIL: {
        int guard = 0;
        for (;;) {
            int c = run_node(s, n->a, io);
            if (s->exiting || interrupted(s)) break;
            if ((n->type == N_WHILE) ? c != 0 : c == 0) break;
            rc = run_node(s, n->b, io);
            if (s->brk) { s->brk--; break; }
            if (s->cont) s->cont = 0;
            if (s->ret || s->exiting) break;
            if (++guard % 64 == 0) task_yield();
        }
        break;
    }
    case N_FOR: {
        strv_t items = { 0 };
        expand_words(s, n->words, n->nwords, &items);
        for (int i = 0; i < items.n; i++) {
            var_set(s, n->name, items.v[i]);
            rc = run_node(s, n->a, io);
            if (s->brk) { s->brk--; break; }
            if (s->cont) s->cont = 0;
            if (s->ret || s->exiting || interrupted(s)) break;
        }
        sv_free(&items);
        break;
    }
    case N_CASE: {
        char *w = expand_single(s, n->name);
        for (int i = 0; i < n->ncase; i++) {
            bool m = false;
            for (int k = 0; k < n->npats[i] && !m; k++) { char *p = expand_single(s, n->pats[i][k]); m = glob_match(p, w); kfree(p); }
            if (m) { rc = run_node(s, n->bodies[i], io); break; }
        }
        kfree(w);
        break;
    }
    case N_FUNC: {
        func_t *f = NULL; for (func_t *x = s->funcs; x; x = x->next) if (!strcmp(x->name, n->name)) f = x;
        if (!f) { f = kzalloc(sizeof *f); f->name = strdup(n->name); f->next = s->funcs; s->funcs = f; }
        f->body = n->a; n->a = NULL;   /* take ownership */
        rc = 0;
        break;
    }
    case N_GROUP: rc = run_node(s, n->a, io); break;
    case N_SUBSHELL: { char save[256]; strcpy(save, s->cwd); rc = run_node(s, n->a, io); strcpy(s->cwd, save); break; }
    }
    s->depth--;
    s->status = rc;
    return rc;
}
static int parse_source(const char *src, node_t **out, char *err, int ecap) {
    toks_t ts = { 0 };
    tokenize(src, &ts);
    *out = NULL;
    int r = 0;
    if (ts.incomplete) r = 1;
    else {
        node_t *n = parse_list(&ts);
        if (ts.incomplete) { r = 1; nfree(n); }
        else if (ts.err[0]) { r = 2; strlcpy(err, ts.err, ecap); nfree(n); }
        else if (peek(&ts)->type != T_EOF) {
            tok_t *t = peek(&ts);
            snprintf(err, ecap, "syntax error near unexpected token '%s'", t->type == T_WORD ? t->s : t->type == T_RP ? ")" : t->type == T_DSEMI ? ";;" : "?");
            r = 2; nfree(n);
        } else *out = n;
    }
    for (int i = 0; i < ts.n; i++) kfree(ts.t[i].s);
    kfree(ts.t);
    return r;
}
static int run_string(sh_t *s, const char *src, io_t *io) {
    node_t *n; char err[128];
    int r = parse_source(src, &n, err, sizeof err);
    if (r == 1) { err_f(io, "sh: syntax error: unexpected end of file\n"); return 2; }
    if (r == 2) { err_f(io, "sh: %s\n", err); return 2; }
    int rc = run_node(s, n, io);
    nfree(n);
    return rc;
}
static sh_t *sh_new(console_t *con, const char *cwd) {
    sh_t *s = kzalloc(sizeof *s);
    s->con = con;
    vnode_t *d = cwd ? vfs_resolve(cwd, NULL) : NULL;
    if (d && d->dir) vfs_path(d, s->cwd, sizeof s->cwd); else strcpy(s->cwd, USER_HOME);
    var_set(s, "HOME", USER_HOME);
    var_set(s, "USER", user_name);
    var_set(s, "SHELL", "C:\\WRLD\\System32\\sh.exe");
    var_set(s, "OS", OS_NAME);
    var_set(s, "PATH", ".;C:\\WRLD\\System32;C:\\WRLD;C:\\Samples;C:\\Program Files");
    var_set(s, "PS1", "");
    var_set(s, "PWD", s->cwd);
    return s;
}
static void sh_free(sh_t *s) {
    while (s->vars) { var_t *v = s->vars; s->vars = v->next; kfree(v->name); kfree(v->value); kfree(v); }
    while (s->funcs) { func_t *f = s->funcs; s->funcs = f->next; nfree(f->body); kfree(f->name); kfree(f); }
    kfree(s);
}
int sh_run_file_io(sh_t *parent, const char *path, int argc, char **argv, io_t *io) {
    vnode_t *n = vfs_resolve(path, parent ? parent->cwd : NULL);
    if (!n || n->dir) { err_f(io, "sh: %s: No such file\n", path); return 127; }
    char *src = strndup((char *)n->data, n->size);
    sh_t *s = sh_new(io->out == parent->con ? parent->con : parent->con, parent->cwd);
    for (var_t *v = parent->vars; v; v = v->next) var_set(s, v->name, v->value);
    char full[256]; vfs_path(n, full, sizeof full);
    s->script_name = full;
    char **av = kzalloc((argc + 2) * sizeof(char *));
    av[0] = full; for (int i = 1; i < argc; i++) av[i] = argv[i];
    s->argc = argc > 0 ? argc : 1; s->argv = av;
    int rc = run_string(s, src, io);
    if (s->exiting) rc = s->exit_code;
    kfree(av); kfree(src);
    sh_free(s);
    return rc;
}
int sh_run_file(console_t *con, const char *path, int argc, char **argv, const char *cwd) {
    sh_t *p = sh_new(con, cwd);
    io_t io = { con, con, con };
    int rc = sh_run_file_io(p, path, argc, argv, &io);
    sh_free(p);
    return rc;
}
int sh_run_string(console_t *con, const char *script, const char *cwd) {
    sh_t *s = sh_new(con, cwd);
    io_t io = { con, con, con };
    int rc = run_string(s, script, &io);
    sh_free(s);
    return rc;
}
void sh_interactive(console_t *con, const char *cwd) {
    sh_t *s = sh_new(con, cwd);
    io_t io = { con, con, con };
    char *p; terminal_cwd_ptr(con, &p); strlcpy(p, s->cwd, 256);
    /* run startup profile if present */
    vnode_t *rc = vfs_resolve(USER_HOME "\\.profile", NULL);
    if (rc && !rc->dir) { char *src = strndup((char *)rc->data, rc->size); run_string(s, src, &io); kfree(src); }
    sbuf_t acc = { 0 };
    while (!s->exiting) {
        if (!acc.n) con_printf(con, "\x1b[92m%s\x1b[0m@wrld \x1b[93m%s\x1b[0m$ ", user_name, s->cwd);
        else con_puts(con, "> ");
        char line[512];
        int n = con->readline(con, line, sizeof line);
        if (n < 0) break;
        console_clear_interrupt(con);
        if (acc.n) sb_putc(&acc, '\n');
        sb_puts(&acc, line);
        if (!acc.b) continue;
        node_t *tree; char err[128];
        int r = parse_source(acc.b, &tree, err, sizeof err);
        if (r == 1) continue;   /* need more input */
        if (r == 2) err_f(&io, "sh: %s\n", err);
        else { s->status = run_node(s, tree, &io); nfree(tree); }
        console_clear_interrupt(con);
        kfree(acc.b); acc.b = NULL; acc.n = acc.cap = 0;
        strlcpy(p, s->cwd, 256);
    }
    sh_free(s);
}
void sh_spawn_script(const char *path, const char *cwd) { terminal_open(path, cwd); }
