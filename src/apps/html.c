/* The WRLD Browser: documents. Lexbor's HTML5 parser builds the tree (see
 * lexglue.h); text is UTF-8 end to end, so any character the fonts cover
 * reaches the screen unchanged. Windows-1252 input is converted on the way in.
 * CSS: full selectors (parsed by Lexbor, matched here on The WRLD Browser's tree)
 * with the cascade by specificity and source order, simple @media queries, and
 * the small set of properties the layout uses (colour, background, display,
 * alignment, bold/italic/underline, size). */
#include "browser.h"
#include "fs.h"
#include "lexglue.h"

/* ---------------- arena ---------------- */
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

/* ---------------- characters ---------------- */
static const uint16_t cp1252[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
/* Text used to be narrowed to Latin-1 here, with typographic punctuation
 * transliterated. The browser now keeps UTF-8 all the way to the screen, so
 * this validates and normalises instead: stray Windows-1252 bytes become the
 * characters they stand for, and invisible control characters are dropped.
 * The caller sizes the buffer for n * 3 bytes, the worst case (0x80 -> U+20AC). */
int utf8_normalise(const uint8_t *s, uint32_t n, char *out) {
    int o = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = c >= 0x80 && c < 0xA0 ? (cp1252[c - 0x80] ? cp1252[c - 0x80] : '?') : c; len = 1; }   /* stray byte: Windows-1252 */
        i += len;
        if (!cp || cp == '\r' || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 || cp == 0xFEFF) continue;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = 0;
    return o;
}

/* ---------------- colours and CSS ---------------- */
static const struct { const char *n; uint32_t c; } named[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "navy", 0x000080 }, { "purple", 0x800080 },
    { "olive", 0x808000 }, { "teal", 0x008080 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "orange", 0xFFA500 },
    { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "lightblue", 0xADD8E6 }, { "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 },
    { "royalblue", 0x4169E1 }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "crimson", 0xDC143C }, { "orangered", 0xFF4500 },
    { "lightyellow", 0xFFFFE0 }, { "lightgreen", 0x90EE90 }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "tan", 0xD2B48C }, { "khaki", 0xF0E68C },
    { "salmon", 0xFA8072 }, { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 }, { "slategray", 0x708090 }, { "darkslategray", 0x2F4F4F }, { "cornflowerblue", 0x6495ED },
    { "aliceblue", 0xF0F8FF }, { "ghostwhite", 0xF8F8FF }, { "snow", 0xFFFAFA }, { "linen", 0xFAF0E6 }, { "wheat", 0xF5DEB3 }, { "midnightblue", 0x191970 },
};
bool parse_color(const char *s, uint32_t *out) {
    while (*s == ' ') s++;
    if (*s == '#') {
        s++; int n = 0; while (isxdigit(s[n])) n++;
        uint32_t v = strtoul(s, NULL, 16);
        if (n == 3 || n == 4) { if (n == 4) v >>= 4; v = ((v >> 8 & 15) * 0x110000) | ((v >> 4 & 15) * 0x1100) | ((v & 15) * 0x11); }
        else if (n == 8) v >>= 8;
        else if (n != 6) return false;
        *out = 0xFF000000u | v; return true;
    }
    if (!strncasecmp(s, "rgb", 3)) {
        const char *p = strchr(s, '('); if (!p) return false;
        int v[3] = { 0 };
        for (int i = 0; i < 3; i++) { p++; while (*p == ' ') p++; v[i] = atoi(p); if (strchr(p, '%') && strchr(p, '%') < strpbrk(p, ",)")) v[i] = v[i] * 255 / 100; p = strpbrk(p, ",) "); if (!p) break; while (*p == ' ') p++; if (*p != ',' && i < 2) { if (*p == ')') break; } }
        *out = RGB(CLAMP(v[0], 0, 255), CLAMP(v[1], 0, 255), CLAMP(v[2], 0, 255)); return true;
    }
    char w[24]; int k = 0; while (isalpha(*s) && k < 23) w[k++] = tolower(*s++); w[k] = 0;
    for (unsigned i = 0; i < ARRAY_LEN(named); i++) if (!strcmp(named[i].n, w)) { *out = 0xFF000000u | named[i].c; return true; }
    if (k && isxdigit(w[0]) && strlen(w) == 6) { *out = 0xFF000000u | strtoul(w, NULL, 16); return true; }   /* bgcolor="ffffff" */
    return false;
}
static const char *css_token_end(const char *p, const char *end) {
    int depth = 0;
    while (p < end) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (depth) depth--; }
        else if (*p == ',' && !depth) break;
        p++;
    }
    return p;
}

/* linear-gradient(to right, #123456, #abcdef) and radial-gradient(...):
 * the two-colour form a page uses for banners and buttons. */
static int parse_gradient(const char *s, css_t *c) {
    while (*s == ' ') s++;
    int kind = !strncasecmp(s, "linear-gradient", 15) ? 1
             : !strncasecmp(s, "radial-gradient", 15) ? 2 : 0;
    if (!kind) return 0;
    const char *p = strchr(s, '(');
    if (!p) return 0;
    p++;
    const char *end = strrchr(p, ')');
    if (!end) end = p + strlen(p);
    const char *t = css_token_end(p, end);
    char tok[96];
    int n = (int)(t - p); if (n > 95) n = 95;
    memcpy(tok, p, n); tok[n] = 0;
    for (char *q = tok; *q; q++) *q = (char)tolower(*q);
    for (char *q = tok + strlen(tok); q > tok && q[-1] == ' '; q--) q[-1] = 0;

    uint32_t first, second;
    int dir = 0;
    if (!parse_color(tok, &first)) {                   /* a direction, then the first colour */
        if (strstr(tok, "right") || strstr(tok, "left") || strstr(tok, "90deg") || strstr(tok, "270deg")) dir = 1;
        else if (strstr(tok, "diagonal") || strstr(tok, "45deg") || strstr(tok, "135deg")) dir = 2;
        p = t;
        if (p < end && *p == ',') p++;
        t = css_token_end(p, end);
        n = (int)(t - p); if (n > 95) n = 95;
        memcpy(tok, p, n); tok[n] = 0;
        if (!parse_color(tok, &first)) return 0;
    }
    p = t;
    if (p < end && *p == ',') p++;
    t = css_token_end(p, end);
    n = (int)(t - p); if (n > 95) n = 95;
    memcpy(tok, p, n); tok[n] = 0;
    if (!parse_color(tok, &second)) return 0;
    c->bg = first; c->bg2 = second;
    c->grad = (int8_t)kind;
    c->grad_dir = (int8_t)dir;
    c->set |= ST_BG | ST_GRAD;
    return 1;
}

/* a CSS length in px: px, em/rem (16 px), pt, or 0; *pct set for percentages */
static int css_len(const char *v, bool *pct, bool *is_auto) {
    while (*v == ' ') v++;
    if (is_auto) *is_auto = !strncmp(v, "auto", 4);
    if (pct) *pct = false;
    bool neg = *v == '-'; if (neg || *v == '+') v++;
    int whole = 0, frac = 0, fd = 0;
    while (*v >= '0' && *v <= '9') whole = whole * 10 + (*v++ - '0');
    if (*v == '.') { v++; while (*v >= '0' && *v <= '9') { if (fd < 2) { frac = frac * 10 + (*v - '0'); fd++; } v++; } }
    while (fd < 2) { frac *= 10; fd++; }
    int x100 = whole * 100 + frac, px;
    if (!strncmp(v, "em", 2) || !strncmp(v, "rem", 3)) px = x100 * 16 / 100;
    else if (!strncmp(v, "pt", 2)) px = x100 * 4 / 300;
    else if (*v == '%') { px = x100 / 100; if (pct) *pct = true; }
    else if (!strncmp(v, "vw", 2)) { px = x100 / 100; if (pct) *pct = true; }
    else px = x100 / 100;
    px = CLAMP(px, 0, 8000);
    return neg ? -px : px;
}
/* margin / padding shorthand with one to four values */
static void css_sides(const char *v, int16_t out[4], uint8_t *set, uint8_t *autos) {
    int vals[4], aut[4], k = 0;
    const char *p = v;
    while (*p && k < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '!') break;
        bool a; vals[k] = css_len(p, NULL, &a); aut[k] = a; k++;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (!k) return;
    int t = 0, r = k > 1 ? 1 : 0, b = k > 2 ? 2 : 0, l = k > 3 ? 3 : k > 1 ? 1 : 0;
    int idx[4] = { t, r, b, l };
    for (int i = 0; i < 4; i++) { out[i] = (int16_t)vals[idx[i]]; if (autos) { if (aut[idx[i]]) *autos |= 1 << i; else *autos &= ~(1 << i); } }
    *set = 0xF;
}
/* ---- CSS custom properties ---- */
static doc_t *var_doc;                        /* the document whose variables var() reads */
static const char *var_lookup(const char *name, int nl) {
    doc_t *d = var_doc;
    if (!d) return NULL;
    for (int i = d->nvars - 1; i >= 0; i--) if ((int)strlen(d->vnames[i]) == nl && !strncmp(d->vnames[i], name, nl)) return d->vvals[i];
    return NULL;
}
static void var_set(doc_t *d, const char *name, int nl, const char *val, int vl, int prio) {
    while (vl > 0 && isspace(val[vl - 1])) vl--;
    while (vl > 0 && isspace(*val)) { val++; vl--; }
    if (vl > 10 && !strncasecmp(val + vl - 10, "!important", 10)) { vl -= 10; while (vl > 0 && isspace(val[vl - 1])) vl--; }
    for (int i = 0; i < d->nvars; i++) if ((int)strlen(d->vnames[i]) == nl && !strncmp(d->vnames[i], name, nl)) {
        if (prio < d->vprio[i]) return;                  /* a component's value does not replace the page's */
        d->vvals[i] = arena_strdup(&d->arena, val, vl); d->vprio[i] = (uint8_t)prio; return;
    }
    if (d->nvars == d->capvars) {
        d->capvars = d->capvars ? d->capvars * 2 : 64;
        d->vnames = krealloc(d->vnames, sizeof(char *) * d->capvars); d->vvals = krealloc(d->vvals, sizeof(char *) * d->capvars);
        d->vprio = krealloc(d->vprio, d->capvars);
    }
    d->vnames[d->nvars] = arena_strdup(&d->arena, name, nl); d->vvals[d->nvars] = arena_strdup(&d->arena, val, vl); d->vprio[d->nvars] = (uint8_t)prio;
    d->nvars++;
}
/* copy v to out with every var(--name[, fallback]) replaced; false if a variable is unknown and has no fallback */
static bool subst_vars(const char *v, char *out, int cap, int depth) {
    int o = 0; bool ok = true;
    while (*v && o < cap - 1) {
        if (strncmp(v, "var(", 4)) { out[o++] = *v++; continue; }
        const char *p = v + 4; while (*p == ' ') p++;
        const char *ne = p; while (*ne && *ne != ',' && *ne != ')' && *ne != ' ') ne++;
        /* the matching ')' and the fallback after the first top-level comma */
        const char *q = ne, *fb = NULL; int dep = 1;
        while (*q && dep) { if (*q == '(') dep++; else if (*q == ')') { if (!--dep) break; } else if (*q == ',' && dep == 1 && !fb) fb = q + 1; q++; }
        const char *val = var_lookup(p, (int)(ne - p));
        char tmp[512];
        if (val && depth < 8) { if (!subst_vars(val, tmp, sizeof tmp, depth + 1)) ok = false; }
        else if (fb) { int fl = MIN((int)(q - fb), (int)sizeof tmp - 1); memcpy(tmp, fb, fl); tmp[fl] = 0; char t2[512]; if (!subst_vars(tmp, t2, sizeof t2, depth + 1)) ok = false; strlcpy(tmp, t2, sizeof tmp); }
        else { tmp[0] = 0; ok = false; }
        const char *t = tmp; while (*t == ' ') t++;
        while (*t && o < cap - 1) out[o++] = *t++;
        v = *q ? q + 1 : q;
    }
    out[o] = 0;
    return ok;
}
/* calc(): the common forms, a length plus or minus a length, or a length times a number */
static int css_calc(const char *v, int pct_base) {
    const char *p = strchr(v, '('); if (!p) return 0;
    p++;
    int acc = 0, sign = 1; bool first = true;
    while (*p && *p != ')') {
        while (*p == ' ') p++;
        if (*p == '+' && p[1] == ' ') { sign = 1; p++; continue; }
        if (*p == '-' && p[1] == ' ') { sign = -1; p++; continue; }
        if (*p == '*' || *p == '/') {
            char op = *p++; while (*p == ' ') p++;
            int whole = atoi(p), frac = 0; const char *dot = strchr(p, '.'); if (dot && dot < p + 8 && isdigit(dot[1])) frac = dot[1] - '0';
            int x10 = whole * 10 + frac;
            if (op == '*') acc = acc * x10 / 10; else if (x10) acc = acc * 10 / x10;
            while (*p && *p != ' ' && *p != ')') p++;
            continue;
        }
        bool pct = false; int whole = 0, frac = 0, fd = 0; bool neg = false;
        if (*p == '-') { neg = true; p++; }
        while (isdigit(*p)) whole = whole * 10 + (*p++ - '0');
        if (*p == '.') { p++; while (isdigit(*p)) { if (fd < 2) { frac = frac * 10 + (*p - '0'); fd++; } p++; } }
        while (fd < 2) { frac *= 10; fd++; }
        int x100 = whole * 100 + frac, px;
        if (!strncmp(p, "rem", 3) || !strncmp(p, "em", 2)) px = x100 * 16 / 100;
        else if (*p == '%' || !strncmp(p, "vw", 2)) { px = pct_base * x100 / 10000; pct = true; }
        else px = x100 / 100;
        (void)pct;
        if (neg) px = -px;
        acc = first ? px : acc + sign * px;
        first = false; sign = 1;
        while (*p && *p != ' ' && *p != ')') p++;
    }
    return acc;
}
static int8_t parse_justify(const char *v) {
    return !strncmp(v, "center", 6) ? FJ_CENTER : !strncmp(v, "flex-end", 8) || !strncmp(v, "end", 3) || !strncmp(v, "right", 5) ? FJ_END
         : !strncmp(v, "space-between", 13) ? FJ_BETWEEN : !strncmp(v, "space-around", 12) ? FJ_AROUND : !strncmp(v, "space-evenly", 12) ? FJ_EVENLY : FJ_START;
}
static int8_t parse_align(const char *v) {
    return !strncmp(v, "center", 6) ? FA_CENTER : !strncmp(v, "flex-end", 8) || !strncmp(v, "end", 3) || !strncmp(v, "self-end", 8) ? FA_END
         : !strncmp(v, "flex-start", 10) || !strncmp(v, "start", 5) || !strncmp(v, "self-start", 10) || !strncmp(v, "baseline", 8) ? FA_START : FA_STRETCH;
}
/* grid-template-columns: a count of tracks, or repeat(auto-fill|auto-fit, minmax(N, 1fr)) */
static void parse_grid_cols(const char *v, css_t *c) {
    c->gcols = 0; c->gmin = 0;
    if (!strncmp(v, "repeat(", 7)) {
        const char *p = v + 7; while (*p == ' ') p++;
        if (!strncmp(p, "auto-fill", 9) || !strncmp(p, "auto-fit", 8)) {
            const char *m = strstr(p, "minmax("); const char *len = m ? m + 7 : strchr(p, ',') ? strchr(p, ',') + 1 : NULL;
            if (len) { while (*len == ' ') len++; c->gmin = (int16_t)CLAMP(css_len(len, NULL, NULL), 20, 2000); }
            if (!c->gmin) c->gmin = 200;
        } else c->gcols = (int16_t)CLAMP(atoi(p), 1, 24);
    } else {
        int n = 0, dep = 0; bool in = false;
        for (const char *p = v; *p && *p != '!'; p++) {
            if (*p == '(') dep++; else if (*p == ')') dep--;
            else if (*p == '[') { while (*p && *p != ']') p++; if (!*p) break; continue; }   /* [line-names] */
            bool sp = *p == ' ' || *p == '\t';
            if (!sp && !in && !dep) { n++; in = true; } else if (sp && !dep) in = false;
        }
        c->gcols = (int16_t)CLAMP(n, 1, 24);
    }
    c->set |= ST_FLEX; c->fset |= F_GCOLS;
}
static void css_decls(const char *s, int n, css_t *c) {
    static char buf[8192]; n = MIN(n, (int)sizeof buf - 1); memcpy(buf, s, n); buf[n] = 0;
    for (char *d = strtok(buf, ";"); d; d = strtok(NULL, ";")) {
        char *colon = strchr(d, ':'); if (!colon) continue;
        *colon = 0; char *k = d, *v = colon + 1;
        while (*k == ' ' || *k == '\n' || *k == '\t' || *k == '\r') k++;
        for (char *e = k + strlen(k) - 1; e >= k && (*e == ' ' || *e == '\n' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *p = k; *p; p++) *p = tolower(*p);
        bool imp = strstr(v, "!important") != NULL;
        (void)imp;
        if (k[0] == '-' && k[1] == '-') continue;                  /* custom properties: collected per sheet */
        static char vbuf[1024];
        if (strstr(v, "var(")) { if (!subst_vars(v, vbuf, sizeof vbuf, 0)) continue; v = vbuf; }
        static char cbuf[32];
        if (!strncmp(v, "calc(", 5) || !strncmp(v, "min(", 4) || !strncmp(v, "max(", 4) || !strncmp(v, "clamp(", 6)) {
            /* lengths only: min/max/clamp take their first argument, calc is worked out */
            if (v[0] == 'c' && v[1] == 'a') snprintf(cbuf, sizeof cbuf, "%dpx", css_calc(v, 800));
            else if (v[1] == 'l') { const char *a = strchr(v, ','); snprintf(cbuf, sizeof cbuf, "%dpx", a ? css_len(a + 1, NULL, NULL) : 0); }
            else snprintf(cbuf, sizeof cbuf, "%dpx", css_len(strchr(v, '(') + 1, NULL, NULL));
            if (strchr(v, '%') && !strstr(v, "px") && !strstr(v, "em") && v[0] != 'c') snprintf(cbuf, sizeof cbuf, "%d%%", atoi(strchr(v, '(') + 1));
            v = cbuf;
        }
        if (!strcmp(k, "color")) { if (parse_color(v, &c->color)) c->set |= ST_COLOR; }
        else if (!strcmp(k, "background") || !strcmp(k, "background-image")) {
            if (!parse_gradient(v, c) && !strcmp(k, "background") && parse_color(v, &c->bg)) c->set |= ST_BG;
        }
        else if (!strcmp(k, "background-color")) { if (parse_color(v, &c->bg)) c->set |= ST_BG; }
        else if (!strcmp(k, "border-radius")) { c->set |= ST_RADIUS; c->radius = (int16_t)CLAMP(atoi(v), 0, 512); }
        else if (!strcmp(k, "border")) {
            c->set |= ST_BORDER;
            c->border = !strncmp(v, "none", 4) || (v[0] == '0' && (v[1] == ' ' || !v[1])) ? 0 : (int16_t)CLAMP(css_len(v, NULL, NULL), 0, 32);
            if (!c->border && strncmp(v, "none", 4) && strncmp(v, "0", 1) && (strstr(v, "solid") || strstr(v, "dashed") || strstr(v, "dotted"))) c->border = 3;   /* "solid #ccc": medium */
            c->border_color = BLACK;
            uint32_t bc;
            for (const char *w = v; *w; ) {                               /* the colour is any word that parses as one */
                while (*w == ' ') w++;
                const char *e = w; int depth = 0;
                while (*e && (depth || *e != ' ')) { if (*e == '(') depth++; else if (*e == ')') depth--; e++; }
                char word[48]; int l = MIN((int)(e - w), 47); memcpy(word, w, l); word[l] = 0;
                static const char *styles[] = { "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset", "none", "hidden" };
                bool kw = (word[0] >= '0' && word[0] <= '9') || word[0] == '.';          /* the width */
                for (unsigned q = 0; q < ARRAY_LEN(styles) && !kw; q++) if (!strcasecmp(word, styles[q])) kw = true;
                if (l && !kw && parse_color(word, &bc)) { c->border_color = bc; break; }
                w = e;
            }
        }
        else if (!strcmp(k, "margin")) { css_sides(v, c->m, &c->mset, &c->mauto); c->set |= ST_MARGIN; }
        else if (!strcmp(k, "padding")) { uint8_t dummy = 0; css_sides(v, c->pad, &c->pset, &dummy); c->set |= ST_PADDING; }
        else if (!strncmp(k, "margin-", 7) || !strncmp(k, "padding-", 8)) {
            bool marg = k[0] == 'm'; const char *side = k + (marg ? 7 : 8);
            int i = !strcmp(side, "top") ? 0 : !strcmp(side, "right") ? 1 : !strcmp(side, "bottom") ? 2 : !strcmp(side, "left") ? 3 : -1;
            if (i >= 0) {
                bool a; int px = css_len(v, NULL, &a);
                if (marg) { c->m[i] = (int16_t)px; c->mset |= 1 << i; if (a) c->mauto |= 1 << i; else c->mauto &= ~(1 << i); c->set |= ST_MARGIN; }
                else { c->pad[i] = (int16_t)MAX(0, px); c->pset |= 1 << i; c->set |= ST_PADDING; }
            }
        }
        else if (!strcmp(k, "width")) { bool a, pc; int px = css_len(v, &pc, &a); if (!a && px > 0) { c->width = (int16_t)px; c->wpct = pc; c->set |= ST_WIDTH; } }
        else if (!strcmp(k, "height")) { bool a, pc; int px = css_len(v, &pc, &a); if (!a && !pc && px > 0) { c->height = (int16_t)px; c->set |= ST_HEIGHT; } }
        else if (!strcmp(k, "box-sizing")) c->bsz = !strncmp(v, "border", 6) ? 2 : 1;
        else if (!strcmp(k, "min-height")) { bool a, pc; int px = css_len(v, &pc, &a); if (!a && !pc && px > 0) { c->minh = (int16_t)px; c->set |= ST_HEIGHT; } }
        else if (!strcmp(k, "max-width")) { bool a, pc; int px = css_len(v, &pc, &a); if (!a && px > 0) { c->maxw = (int16_t)px; c->mpct = pc; c->set |= ST_MAXW; } }
        else if (!strcmp(k, "border-width")) { c->set |= ST_BORDER; c->border = (int16_t)CLAMP(css_len(v, NULL, NULL), 0, 32); if (!c->border_color) c->border_color = BLACK; }
        else if (!strcmp(k, "border-color")) { uint32_t bc; if (parse_color(v, &bc)) { c->border_color = bc; c->set |= ST_BCOLOR; } }
        else if (!strcmp(k, "display")) {
            c->set |= ST_DISPLAY;
            c->display = !strncmp(v, "none", 4) ? DISP_NONE : strstr(v, "flex") ? DISP_FLEX : strstr(v, "grid") ? DISP_GRID
                       : !strncmp(v, "inline-block", 12) || !strncmp(v, "inline-table", 12) ? DISP_IBLOCK
                       : !strncmp(v, "inline", 6) || !strncmp(v, "contents", 8) ? DISP_INLINE : DISP_BLOCK;
        }
        else if (!strcmp(k, "position")) {
            c->set |= ST_POS; c->oset |= 0x10;             /* bit 4: the position property itself */
            c->pos = !strncmp(v, "relative", 8) ? POS_RELATIVE : !strncmp(v, "absolute", 8) ? POS_ABSOLUTE : !strncmp(v, "fixed", 5) ? POS_FIXED
                   : !strncmp(v, "sticky", 6) || !strncmp(v, "-webkit-sticky", 14) ? POS_STICKY : POS_STATIC;
        }
        else if (!strcmp(k, "top") || !strcmp(k, "right") || !strcmp(k, "bottom") || !strcmp(k, "left") || !strcmp(k, "inset")) {
            int i0 = k[0] == 't' ? 0 : k[0] == 'r' ? 1 : k[0] == 'b' ? 2 : k[0] == 'l' ? 3 : -1;
            for (int i = 0; i < 4; i++) {
                if (i0 >= 0 && i != i0) continue;
                bool a, pc; int px = css_len(v, &pc, &a);
                if (a) { c->oset &= ~(1 << i); continue; }
                c->off[i] = (int16_t)CLAMP(px, -10000, 10000); c->oset |= 1 << i;
                if (pc) c->opct |= 1 << i; else c->opct &= ~(1 << i);
            }
            c->set |= ST_POS;
        }
        else if (!strcmp(k, "fill")) {
            c->set |= ST_FILL; c->fill_cur = !strncasecmp(v, "currentcolor", 12);
            if (!c->fill_cur && !parse_color(v, &c->fill)) c->fill = 0;      /* none */
        }
        else if (!strcmp(k, "float")) { c->set |= ST_FLOAT; c->flt = !strncmp(v, "left", 4) || !strncmp(v, "inline-start", 12) ? 1 : !strncmp(v, "right", 5) || !strncmp(v, "inline-end", 10) ? 2 : 0; }
        else if (!strcmp(k, "clear")) { c->set |= ST_CLEAR; c->clr = !strncmp(v, "left", 4) ? 1 : !strncmp(v, "right", 5) ? 2 : !strncmp(v, "both", 4) ? 3 : 0; }
        else if (!strcmp(k, "flex-direction")) { c->set |= ST_FLEX; c->fset |= F_DIR; c->fdir = (int8_t)((strstr(v, "column") ? 1 : 0) | (strstr(v, "reverse") ? 2 : 0)); }
        else if (!strcmp(k, "flex-wrap")) { c->set |= ST_FLEX; c->fset |= F_WRAP; c->fwrap = !strncmp(v, "wrap", 4); }
        else if (!strcmp(k, "flex-flow")) {
            c->set |= ST_FLEX; c->fset |= F_DIR | F_WRAP;
            c->fdir = (int8_t)((strstr(v, "column") ? 1 : 0) | (strstr(v, "reverse") && !strstr(v, "wrap-reverse") ? 2 : 0));
            c->fwrap = strstr(v, "wrap") && !strstr(v, "nowrap");
        }
        else if (!strcmp(k, "justify-content")) { c->set |= ST_FLEX; c->fset |= F_JUST; c->fjust = parse_justify(v); }
        else if (!strcmp(k, "align-items")) { c->set |= ST_FLEX; c->fset |= F_ALIGN; c->falign = parse_align(v); }
        else if (!strcmp(k, "place-items")) { c->set |= ST_FLEX; c->fset |= F_ALIGN; c->falign = parse_align(v); }
        else if (!strcmp(k, "align-self")) { c->set |= ST_FLEX; c->fset |= F_ASELF; c->aself = !strncmp(v, "auto", 4) ? -1 : parse_align(v); }
        else if (!strcmp(k, "gap") || !strcmp(k, "grid-gap")) {
            c->set |= ST_FLEX; c->fset |= F_GAP | F_RGAP;
            c->rgap = (int16_t)CLAMP(css_len(v, NULL, NULL), 0, 400);
            const char *sp = strchr(v, ' '); c->gap = sp && sp[1] && sp[1] != '!' ? (int16_t)CLAMP(css_len(sp + 1, NULL, NULL), 0, 400) : c->rgap;
        }
        else if (!strcmp(k, "column-gap") || !strcmp(k, "grid-column-gap")) { c->set |= ST_FLEX; c->fset |= F_GAP; c->gap = (int16_t)CLAMP(css_len(v, NULL, NULL), 0, 400); }
        else if (!strcmp(k, "row-gap") || !strcmp(k, "grid-row-gap")) { c->set |= ST_FLEX; c->fset |= F_RGAP; c->rgap = (int16_t)CLAMP(css_len(v, NULL, NULL), 0, 400); }
        else if (!strcmp(k, "flex-grow")) { c->set |= ST_FLEX; c->fset |= F_GROW; c->grow = (int8_t)CLAMP(atoi(v), 0, 100); }
        else if (!strcmp(k, "flex-shrink")) { c->set |= ST_FLEX; c->fset |= F_SHRINK; c->shrink = (int8_t)CLAMP(atoi(v), 0, 100); }
        else if (!strcmp(k, "flex-basis")) { c->set |= ST_FLEX; c->fset |= F_BASIS; bool pc; c->basis = !strncmp(v, "auto", 4) || !strncmp(v, "content", 7) ? -1 : (int16_t)css_len(v, &pc, NULL); c->bpct = !strncmp(v, "auto", 4) ? false : pc; }
        else if (!strcmp(k, "flex")) {
            c->set |= ST_FLEX; c->fset |= F_GROW | F_SHRINK | F_BASIS; c->bpct = false;
            if (!strncmp(v, "none", 4)) { c->grow = 0; c->shrink = 0; c->basis = -1; }
            else if (!strncmp(v, "auto", 4)) { c->grow = 1; c->shrink = 1; c->basis = -1; }
            else {
                /* flex: <grow> [<shrink>] [<basis>]; a lone number means basis 0 */
                const char *p = v; int nums = 0; c->grow = 0; c->shrink = 1; c->basis = 0;
                while (*p && *p != '!') {
                    while (*p == ' ') p++;
                    if (!*p || *p == '!') break;
                    const char *e = p; while (*e && *e != ' ') e++;
                    bool unit = false; for (const char *q = p; q < e; q++) if (isalpha(*q) || *q == '%') unit = true;
                    if (!unit && nums == 0) { c->grow = (int8_t)CLAMP(atoi(p), 0, 100); nums++; }
                    else if (!unit && nums == 1) { c->shrink = (int8_t)CLAMP(atoi(p), 0, 100); nums++; }
                    else { bool pc; c->basis = !strncmp(p, "auto", 4) ? -1 : (int16_t)css_len(p, &pc, NULL); c->bpct = pc && strncmp(p, "auto", 4); }
                    p = e;
                }
            }
        }
        else if (!strcmp(k, "grid-template-columns")) parse_grid_cols(v, c);
        else if (!strcmp(k, "visibility")) { if (!strncmp(v, "hidden", 6)) { c->set |= ST_DISPLAY; c->display = 0; } }
        else if (!strcmp(k, "text-align")) { c->set |= ST_ALIGN; c->align = !strncmp(v, "center", 6) ? 1 : !strncmp(v, "right", 5) ? 2 : 0; }
        else if (!strcmp(k, "font-weight")) { c->set |= ST_BOLD; c->bold = !strncmp(v, "bold", 4) || atoi(v) >= 600; }
        else if (!strcmp(k, "font-style")) { c->set |= ST_ITALIC; c->italic = !strncmp(v, "italic", 6) || !strncmp(v, "oblique", 7); }
        else if (!strcmp(k, "text-decoration") || !strcmp(k, "text-decoration-line")) { c->set |= ST_UNDER; c->under = strstr(v, "underline") != NULL; }
        else if (!strcmp(k, "font-size")) {
            int px = atoi(v);
            if (strstr(v, "em") || strstr(v, "rem")) { int whole = atoi(v), frac = strchr(v, '.') ? v[strchr(v, '.') - v + 1] - '0' : 0; px = (whole * 10 + frac) * 16 / 10; }
            else if (strchr(v, '%')) px = px * 16 / 100;
            else if (strstr(v, "pt")) px = px * 4 / 3;
            else if (strstr(v, "small")) px = 12;
            else if (strstr(v, "large")) px = 20;
            if (px) { c->set |= ST_SIZE; c->size = px <= 13 ? -1 : px <= 18 ? 0 : px <= 21 ? 3 : px <= 26 ? 2 : 1; }
        }
    }
}
static void css_merge(css_t *dst, const css_t *src) {
    if (src->set & ST_COLOR) dst->color = src->color;
    if (src->set & ST_BG) dst->bg = src->bg;
    if (src->set & ST_DISPLAY) dst->display = src->display;
    if (src->set & ST_ALIGN) dst->align = src->align;
    if (src->set & ST_BOLD) dst->bold = src->bold;
    if (src->set & ST_ITALIC) dst->italic = src->italic;
    if (src->set & ST_UNDER) dst->under = src->under;
    if (src->set & ST_SIZE) dst->size = src->size;
    if (src->set & ST_RADIUS) dst->radius = src->radius;
    if (src->set & ST_GRAD) { dst->bg2 = src->bg2; dst->grad = src->grad; dst->grad_dir = src->grad_dir; }
    if (src->set & ST_BORDER) { dst->border = src->border; dst->border_color = src->border_color; }
    if (src->set & ST_BCOLOR) dst->border_color = src->border_color;
    for (int i = 0; i < 4; i++) {
        if (src->mset & (1 << i)) { dst->m[i] = src->m[i]; dst->mset |= 1 << i; dst->mauto = (dst->mauto & ~(1 << i)) | (src->mauto & (1 << i)); }
        if (src->pset & (1 << i)) { dst->pad[i] = src->pad[i]; dst->pset |= 1 << i; }
    }
    if (src->set & ST_WIDTH) { dst->width = src->width; dst->wpct = src->wpct; }
    if (src->set & ST_MAXW) { dst->maxw = src->maxw; dst->mpct = src->mpct; }
    if (src->set & ST_POS) {
        if (src->oset & 0x10) { dst->pos = src->pos; dst->oset |= 0x10; }
        for (int i = 0; i < 4; i++) if (src->oset & (1 << i)) { dst->off[i] = src->off[i]; dst->oset |= 1 << i; dst->opct = (dst->opct & ~(1 << i)) | (src->opct & (1 << i)); }
    }
    if (src->set & ST_FLOAT) dst->flt = src->flt;
    if (src->set & ST_FILL) { dst->fill = src->fill; dst->fill_cur = src->fill_cur; }
    if (src->set & ST_CLEAR) dst->clr = src->clr;
    if (src->bsz) dst->bsz = src->bsz;
    if (src->set & ST_HEIGHT) { if (src->height) dst->height = src->height; if (src->minh) dst->minh = src->minh; }
    if (src->set & ST_FLEX) {
        uint16_t f = src->fset;
        if (f & F_DIR) dst->fdir = src->fdir;
        if (f & F_WRAP) dst->fwrap = src->fwrap;
        if (f & F_JUST) dst->fjust = src->fjust;
        if (f & F_ALIGN) dst->falign = src->falign;
        if (f & F_ASELF) dst->aself = src->aself;
        if (f & F_GAP) dst->gap = src->gap;
        if (f & F_RGAP) dst->rgap = src->rgap;
        if (f & F_GROW) dst->grow = src->grow;
        if (f & F_SHRINK) dst->shrink = src->shrink;
        if (f & F_BASIS) { dst->basis = src->basis; dst->bpct = src->bpct; }
        if (f & F_GCOLS) { dst->gcols = src->gcols; dst->gmin = src->gmin; }
        dst->fset |= f;
    }
    dst->set |= src->set;
}
/* ---------------- tree building (Lexbor's HTML5 parser) ---------------- */
const char *node_attr(node_t *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attr[i * 2], name)) return n->attr[i * 2 + 1];
    return NULL;
}
static bool is_void(const char *t) {
    static const char *v[] = { "br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "source", "wbr", "param", "track", "keygen" };
    for (unsigned i = 0; i < ARRAY_LEN(v); i++) if (!strcmp(t, v[i])) return true;
    return false;
}
/* Windows-1252 / Latin-1 bytes to UTF-8, for Lexbor */
static uint32_t utf8_from_1252(const uint8_t *s, uint32_t n, char *out) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0x80 && cp < 0xA0) cp = cp1252[cp - 0x80] ? cp1252[cp - 0x80] : '?';
        if (cp < 0x80) out[o++] = cp;
        else if (cp < 0x800) { out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3F); }
        else { out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3F); out[o++] = 0x80 | (cp & 0x3F); }
    }
    out[o] = 0;
    return o;
}
static char *textdup(arena_t *a, const char *u, unsigned n) {
    char *t = arena_alloc(a, n * 4 + 1);            /* 3 bytes out per stray byte, 4 for a 4-byte sequence */
    utf8_normalise((const uint8_t *)u, n, t);
    return t;
}

static bool media_ok(const char *q, int n);
static const char *strcasestr_l(const char *h, const char *n) {
    int l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return h;
    return NULL;
}
#define MAX_DEPTH 512
enum { FR_NODE, FR_PASS, FR_TITLE, FR_STYLE, FR_SKIP };
typedef struct { node_t *n; uint8_t kind; } frame_t;
typedef struct {
    doc_t *d;
    doc_t *cssdoc;                                 /* where <style> rules go (the page, for fragments) */
    const char *url;
    frame_t st[MAX_DEPTH]; int sp, overflow;
    char *buf; uint32_t blen, bcap;               /* text of <title> / <style> being collected */
    bool in_body;
} builder_t;
static void parse_stylesheet(doc_t *d, const char *s, int n, int sheet);
static node_t *top_node(builder_t *b) { return b->st[b->sp - 1].n; }
static void append(node_t *p, node_t *n) { n->parent = p; if (p->last) p->last->next = n; else p->first = n; p->last = n; }
static void push(builder_t *b, node_t *n, int kind) {
    if (b->sp >= MAX_DEPTH) { b->overflow++; return; }    /* deeper levels are flattened into the last one */
    b->st[b->sp].n = n; b->st[b->sp].kind = kind; b->sp++;
}
static void collect_text(builder_t *b, const char *s, unsigned n) {
    if (b->blen + n + 1 > b->bcap) { b->bcap = (b->blen + n + 1) * 2; b->buf = krealloc(b->buf, b->bcap); }
    memcpy(b->buf + b->blen, s, n); b->blen += n; b->buf[b->blen] = 0;
}
static char **copy_attrs(doc_t *d, const lxg_attr_t *a, int na) {
    char **v = arena_alloc(&d->arena, sizeof(char *) * (na * 2 + 2));
    for (int i = 0; i < na; i++) {
        char *nm = arena_alloc(&d->arena, a[i].name_len + 1);
        for (unsigned k = 0; k < a[i].name_len; k++) nm[k] = tolower(a[i].name[k]);
        v[i * 2] = nm;
        v[i * 2 + 1] = textdup(&d->arena, a[i].value, a[i].value_len);
    }
    return v;
}
static void sink_start(void *ctx, const char *name, unsigned nl, const lxg_attr_t *a, int na) {
    builder_t *b = ctx; doc_t *d = b->d;
    char tag[12]; unsigned k = 0;
    for (; k < nl && k < 11; k++) tag[k] = tolower(name[k]);
    tag[k] = 0;
    int kind = b->sp ? b->st[b->sp - 1].kind : FR_NODE;
    if (kind == FR_SKIP || kind == FR_TITLE || kind == FR_STYLE) { push(b, NULL, FR_SKIP); return; }
    if (!strcmp(tag, "html") || !strcmp(tag, "head")) {
        for (int i = 0; !strcmp(tag, "html") && i < na; i++)        /* <html dir="rtl"> */
            if (a[i].name_len == 3 && !memcmp(a[i].name, "dir", 3)) d->rtl = a[i].value_len == 3 && !strncasecmp(a[i].value, "rtl", 3);
        push(b, top_node(b), FR_PASS); return;
    }
    if (!strcmp(tag, "body")) {
        b->in_body = true;
        node_t *r = d->root;                       /* the root stands in for <body>: keep its attributes for selectors */
        r->attr = copy_attrs(d, a, na); r->nattr = na;
        for (int i = 0; i < na; i++) {
            const char *nm = r->attr[i * 2], *v = r->attr[i * 2 + 1];
            if (!strcmp(nm, "bgcolor")) parse_color(v, &d->bgcolor);
            else if (!strcmp(nm, "text")) parse_color(v, &d->text);
            else if (!strcmp(nm, "link")) parse_color(v, &d->link);
            else if (!strcmp(nm, "vlink")) parse_color(v, &d->vlink);
            else if (!strcmp(nm, "dir")) d->rtl = !strcasecmp(v, "rtl");
        }
        push(b, r, FR_PASS);
        return;
    }
    if (!strcmp(tag, "title") && !b->in_body && !d->title[0]) { b->blen = 0; push(b, NULL, FR_TITLE); return; }
    if (!strcmp(tag, "style")) {
        for (int i = 0; i < na; i++) if (a[i].name_len == 5 && !memcmp(a[i].name, "media", 5) && !media_ok(a[i].value, a[i].value_len)) { push(b, NULL, FR_SKIP); return; }
        b->blen = 0; push(b, NULL, FR_STYLE); return;
    }
    if (!strcmp(tag, "base")) {
        for (int i = 0; i < na; i++) if (a[i].name_len == 4 && !memcmp(a[i].name, "href", 4)) {
            extern bool url_resolve(const char *, const char *, char *, int);
            char *h = textdup(&d->arena, a[i].value, a[i].value_len);
            url_resolve(b->url, h, d->base, sizeof d->base);
        }
        push(b, NULL, FR_SKIP); return;
    }
    node_t *n = arena_alloc(&d->arena, sizeof *n);
    strcpy(n->tag, tag);
    n->attr = copy_attrs(d, a, na); n->nattr = na;
    if (!strcmp(tag, "meta")) {
        const char *he = node_attr(n, "http-equiv"), *ct = node_attr(n, "content");
        if (he && ct && !strcasecmp(he, "refresh")) {
            d->refresh_s = atoi(ct);
            const char *u = ct; while (*u && strncasecmp(u, "url=", 4)) u++;
            if (*u) { u += 4; while (*u == ' ' || *u == '\'' || *u == '"') u++; strlcpy(d->refresh, u, sizeof d->refresh); char *q = strpbrk(d->refresh, "'\""); if (q) *q = 0; }
        }
        push(b, NULL, FR_SKIP); return;
    }
    if (!strcmp(tag, "link")) {                    /* stylesheets are fetched by the browser later; their order counts */
        const char *rel = node_attr(n, "rel"), *href = node_attr(n, "href"), *media = node_attr(n, "media");
        if (rel && href && *href && strcasestr_l(rel, "stylesheet") && !strcasestr_l(rel, "alternate") && (!media || media_ok(media, strlen(media))) && d->nlinks < MAX_LINKED_SHEETS) {
            d->links[d->nlinks].href = arena_strdup(&d->arena, href, -1);
            d->links[d->nlinks].sheet = ++d->nsheets;
            d->nlinks++;
        }
        push(b, NULL, FR_SKIP); return;
    }
    append(top_node(b), n);
    push(b, n, FR_NODE);
}
static void sink_end(void *ctx) {
    builder_t *b = ctx;
    if (b->overflow) { b->overflow--; return; }
    if (b->sp <= 1) return;
    frame_t f = b->st[--b->sp];
    if (f.kind == FR_TITLE) {
        char *t = textdup(&b->d->arena, b->buf ? b->buf : "", b->blen);
        for (char *q = t; *q; q++) if (*q == '\n' || *q == '\t' || *q == '\r') *q = ' ';
        while (*t == ' ') t++;
        strlcpy(b->d->title, t, sizeof b->d->title);
        for (int l = strlen(b->d->title); l > 0 && b->d->title[l - 1] == ' '; l--) b->d->title[l - 1] = 0;
    } else if (f.kind == FR_STYLE && b->blen) parse_stylesheet(b->cssdoc, b->buf, b->blen, ++b->cssdoc->nsheets);
}
static void sink_text(void *ctx, const char *s, unsigned n) {
    builder_t *b = ctx;
    int kind = b->st[b->sp - 1].kind;
    if (kind == FR_TITLE || kind == FR_STYLE) { collect_text(b, s, n); return; }
    if (kind == FR_SKIP) return;
    node_t *p = top_node(b);
    node_t *t = arena_alloc(&b->d->arena, sizeof *t);
    t->type = 1;
    t->text = textdup(&b->d->arena, s, n);
    append(p, t);
}

/* ---------------- CSS cascade ---------------- */
/* every complex selector of every rule, bucketed by the key of its right-most
 * compound selector so an element only tries the rules that could match it */
#define VIEW_W 880                                 /* the width @media queries are answered for */
static uint32_t khash(int kind, const char *k, unsigned l) {
    uint32_t h = 2166136261u ^ kind;
    for (unsigned i = 0; i < l; i++) h = (h ^ (uint8_t)tolower(k[i])) * 16777619u;
    return h;
}
static css_bucket_t *bucket_get(doc_t *d, int kind, const char *key, unsigned kl, bool create) {
    uint32_t h = khash(kind, key, kl);
    for (css_bucket_t *b = d->buckets[h % CSS_BUCKETS]; b; b = b->next)
        if (b->hash == h && b->kind == kind && strlen(b->key) == kl && !strncasecmp(b->key, key, kl)) return b;
    if (!create) return NULL;
    css_bucket_t *b = arena_alloc(&d->arena, sizeof *b);
    b->hash = h; b->kind = kind; b->key = arena_strdup(&d->arena, key, kl);
    b->next = d->buckets[h % CSS_BUCKETS]; d->buckets[h % CSS_BUCKETS] = b;
    return b;
}
typedef struct { doc_t *d; css_t css; uint32_t order; } addctx_t;
static void add_rule(void *ctx, void *sel, unsigned spec, int kind, const char *key, unsigned kl) {
    addctx_t *a = ctx; doc_t *d = a->d;
    if (d->nrules == d->caprules) { d->caprules = d->caprules ? d->caprules * 2 : 256; d->rules = krealloc(d->rules, sizeof(css_rule_t) * d->caprules); }
    int idx = d->nrules++;
    d->rules[idx].sel = sel; d->rules[idx].spec = spec; d->rules[idx].order = a->order; d->rules[idx].css = a->css;
    css_bucket_t *b = bucket_get(d, kind, kind == LXG_KEY_ANY ? "*" : key, kind == LXG_KEY_ANY ? 1 : kl, true);
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->idx = krealloc(b->idx, sizeof(int) * b->cap); }
    b->idx[b->n++] = idx;
}
/* @media: media types and the simple width/feature queries sites use */
static int css_length(const char *v) {
    int whole = atoi(v), tenth = 0;
    const char *dot = strchr(v, '.'); if (dot && isdigit(dot[1])) tenth = dot[1] - '0';
    const char *u = v; while (*u && (isdigit(*u) || *u == '.' || *u == '-' || *u == ' ')) u++;
    if (!strncmp(u, "em", 2) || !strncmp(u, "rem", 3)) return (whole * 10 + tenth) * 16 / 10;
    return whole;
}
static bool media_feature(const char *f, int n) {
    char buf[96]; n = MIN(n, 95); memcpy(buf, f, n); buf[n] = 0;
    for (char *p = buf; *p; p++) *p = tolower(*p);
    char *colon = strchr(buf, ':');
    const char *name = buf; while (*name == ' ') name++;
    if (!colon) {
        char *op = strpbrk(buf, "<>=");           /* (width >= 600px) */
        if (op && !strncmp(name, "width", 5)) {
            bool ge = op[0] == '>', eqq = op[1] == '=' || op[0] == '=';
            int v = css_length(op + (op[1] == '=' ? 2 : 1));
            return op[0] == '=' ? VIEW_W == v : ge ? (eqq ? VIEW_W >= v : VIEW_W > v) : (eqq ? VIEW_W <= v : VIEW_W < v);
        }
        return !strncmp(name, "color", 5) || !strncmp(name, "hover", 5) || !strncmp(name, "pointer", 7) || !strncmp(name, "grid", 4) ? true : false;
    }
    *colon = 0;
    char *e = colon - 1; while (e >= buf && *e == ' ') *e-- = 0;
    const char *v = colon + 1; while (*v == ' ') v++;
    if (!strcmp(name, "min-width")) return VIEW_W >= css_length(v);
    if (!strcmp(name, "max-width")) return VIEW_W <= css_length(v);
    if (!strcmp(name, "min-height")) return 600 >= css_length(v);
    if (!strcmp(name, "max-height")) return 600 <= css_length(v);
    if (!strcmp(name, "orientation")) return !strncmp(v, "landscape", 9);
    if (!strcmp(name, "prefers-color-scheme")) return !strncmp(v, "light", 5);
    if (!strcmp(name, "prefers-reduced-motion")) return !strncmp(v, "reduce", 6);
    if (!strcmp(name, "hover") || !strcmp(name, "any-hover")) return !strncmp(v, "hover", 5);
    if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer")) return !strncmp(v, "fine", 4);
    return false;
}
static bool media_ok(const char *q, int n) {
    if (n <= 0) return true;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && q[i] != ',') continue;
        /* one query: [only|not] type [and (feature)]... */
        const char *s = q + start, *e = q + i; start = i + 1;
        while (s < e && isspace(*s)) s++;
        bool neg = false, ok = true;
        if (e - s >= 4 && !strncasecmp(s, "not ", 4)) { neg = true; s += 4; }
        else if (e - s >= 5 && !strncasecmp(s, "only ", 5)) s += 5;
        while (s < e) {
            while (s < e && isspace(*s)) s++;
            if (s >= e) break;
            if (*s == '(') {
                const char *c = s + 1; int depth = 1;
                while (c < e && depth) { if (*c == '(') depth++; else if (*c == ')') depth--; if (depth) c++; }
                if (!media_feature(s + 1, c - s - 1)) ok = false;
                s = c < e ? c + 1 : e;
            } else {
                const char *w = s; while (s < e && !isspace(*s) && *s != '(') s++;
                int wl = s - w;
                if (wl == 3 && !strncasecmp(w, "and", 3)) continue;
                if ((wl == 6 && !strncasecmp(w, "screen", 6)) || (wl == 3 && !strncasecmp(w, "all", 3))) continue;
                ok = false;                           /* print, speech, or something unknown */
            }
        }
        if (ok != neg) return true;
    }
    return false;
}
/* skip a comment or string starting at s[i]; returns the index after it */
static int css_skip(const char *s, int n, int i) {
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++; return MIN(i + 2, n); }
    if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) { if (s[i] == '\\') i++; i++; } return MIN(i + 1, n); }
    return i + 1;
}
/* the end of the block opened just before i (index of its '}'), or n */
static int css_block_end(const char *s, int n, int i) {
    int depth = 1;
    while (i < n) {
        char c = s[i];
        if (c == '{') depth++;
        else if (c == '}' && !--depth) return i;
        i = (c == '/' || c == '"' || c == '\'') ? css_skip(s, n, i) : i + 1;
    }
    return n;
}
/* does any element of the finished tree match one of a rule's selectors? Variables
 * live per element in CSS; this engine keeps one set per page, so a rule that matches
 * nothing (a dark-mode or state variant such as .bar.dark) must not set them */
static const lxg_tree_t tree_ops;
static void *varsel_pool;
typedef struct { void *v[16]; int n; } varsel_t;
static void varsel_add(void *ctx, void *sel, unsigned spec, int kind, const char *key, unsigned kl) {
    (void)spec; (void)kind; (void)key; (void)kl;
    varsel_t *l = ctx; if (l->n < 16) l->v[l->n++] = sel;
}
static bool rule_matches_any(doc_t *d, const char *sel, int sl) {
    if (!varsel_pool) varsel_pool = lxg_pool_new();
    varsel_t l = { .n = 0 };
    if (lxg_selectors(varsel_pool, sel, sl, varsel_add, &l) < 0 || !l.n) return false;
    for (node_t *n = d->root; n; ) {
        if (!n->type) for (int i = 0; i < l.n; i++) if (lxg_match(l.v[i], n, &tree_ops)) return true;
        if (!n->type && n->first) { n = n->first; continue; }
        while (n && !n->next) n = n->parent;
        if (n) n = n->next;
    }
    return false;
}
/* the custom properties in one rule body; :root, html and body set the page's values */
static void collect_vars(doc_t *d, const char *sel, int sl, const char *b, int bl) {
    int prio = 1;
    for (int i = 0; i < sl; i++) if (!strncmp(sel + i, ":root", 5) || ((i == 0 || sel[i - 1] == ',' || sel[i - 1] == ' ') && (!strncmp(sel + i, "html", 4) || !strncmp(sel + i, "body", 4)) && (i + 4 >= sl || !isalnum(sel[i + 4]) )) ) { prio = 2; break; }
    bool any = false;                                  /* skip the matching for rules without variables */
    for (int k = 0; k + 1 < bl; k++) if (b[k] == '-' && b[k + 1] == '-') { any = true; break; }
    if (!any) return;
    if (prio == 1 && d->var_filter && !rule_matches_any(d, sel, sl)) return;
    int i = 0;
    while (i < bl) {
        while (i < bl && (isspace(b[i]) || b[i] == ';')) i++;
        int ks = i; while (i < bl && b[i] != ':' && b[i] != ';') i++;
        if (i >= bl || b[i] != ':') { while (i < bl && b[i] != ';') i++; continue; }
        int ke = i; while (ke > ks && isspace(b[ke - 1])) ke--;
        int vs = ++i, dep = 0;
        while (i < bl && (dep || b[i] != ';')) { if (b[i] == '(' || b[i] == '{' || b[i] == '[') dep++; else if ((b[i] == ')' || b[i] == '}' || b[i] == ']') && dep) dep--; i++; }
        if (ke - ks > 2 && b[ks] == '-' && b[ks + 1] == '-') var_set(d, b + ks, ke - ks, b + vs, i - vs, prio);
    }
}
static void parse_rules_mode(doc_t *d, const char *s, int n, int sheet, int *rule_no, int level, bool collect);
static void parse_rules(doc_t *d, const char *s, int n, int sheet, int *rule_no, int level) { parse_rules_mode(d, s, n, sheet, rule_no, level, false); }
static void parse_rules_mode(doc_t *d, const char *s, int n, int sheet, int *rule_no, int level, bool collect) {
    int i = 0;
    while (i < n) {
        while (i < n && (isspace(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { i = css_skip(s, n, i); continue; }
        if (s[i] == '}') { i++; continue; }
        int pre = i;
        while (i < n && s[i] != '{' && !(s[pre] == '@' && s[i] == ';')) i = (s[i] == '/' || s[i] == '"' || s[i] == '\'') ? css_skip(s, n, i) : i + 1;
        if (i >= n) break;
        if (s[i] == ';') { i++; continue; }              /* @import, @charset, @namespace, @layer a, b; */
        int pre_end = i, body = ++i, body_end = css_block_end(s, n, i);
        i = body_end + 1;
        if (s[pre] == '@') {
            const char *kw = s + pre + 1; int kl = 0; while (pre + 1 + kl < pre_end && (isalnum(kw[kl]) || kw[kl] == '-')) kl++;
            bool nest = false;
            if (kl == 5 && !strncasecmp(kw, "media", 5)) nest = media_ok(kw + 5, pre_end - (pre + 6));
            else if ((kl == 8 && !strncasecmp(kw, "supports", 8)) || (kl == 5 && !strncasecmp(kw, "layer", 5)) || (kl == 8 && !strncasecmp(kw, "document", 8))) nest = true;
            if (nest && level < 8) parse_rules_mode(d, s + body, body_end - body, sheet, rule_no, level + 1, collect);
            continue;                                    /* @font-face, @keyframes, @page... */
        }
        int pl = pre_end - pre; while (pl > 0 && isspace(s[pre + pl - 1])) pl--;
        if (collect) { collect_vars(d, s + pre, pl, s + body, body_end - body); continue; }
        css_t c = { 0 };
        var_doc = d;
        css_decls(s + body, body_end - body, &c);
        if (!c.set) continue;
        addctx_t a = { d, c, ((uint32_t)sheet << 20) | (uint32_t)MIN(*rule_no, 0xFFFFF) };
        (*rule_no)++;
        if (!d->selpool) d->selpool = lxg_pool_new();
        lxg_selectors(d->selpool, s + pre, pl, add_rule, &a);
    }
}
static void store_sheet(doc_t *d, const char *s, int n, int sheet) {
    if (d->nsheet_text == d->capsheet_text) { d->capsheet_text = d->capsheet_text ? d->capsheet_text * 2 : 8; d->sheets = krealloc(d->sheets, sizeof *d->sheets * d->capsheet_text); }
    d->sheets[d->nsheet_text].text = arena_strdup(&d->arena, s, n);
    d->sheets[d->nsheet_text].len = n; d->sheets[d->nsheet_text].sheet = sheet;
    d->nsheet_text++;
    int rn = 0;
    parse_rules_mode(d, s, n, sheet, &rn, 0, true);      /* its variables first, so the rules can use them */
}
static void parse_stylesheet(doc_t *d, const char *s, int n, int sheet) {
    store_sheet(d, s, n, sheet);
    int rule_no = 0;
    parse_rules(d, s, n, sheet, &rule_no, 0);
}
/* a linked sheet arrived: its variables may be used by sheets parsed before it, so every
 * sheet's rules are parsed again, in document order */
/* parse every stored sheet's rules again, in sheet order */
static void reparse_sheets(doc_t *d) {
    for (int i = 0; i < CSS_BUCKETS; i++) { for (css_bucket_t *b = d->buckets[i]; b; b = b->next) kfree(b->idx); d->buckets[i] = NULL; }
    d->nrules = 0;
    if (d->selpool) { lxg_pool_free(d->selpool); d->selpool = NULL; }
    for (int i = 1; i < d->nsheet_text; i++) {                /* insertion sort: a page has few sheets */
        typeof(d->sheets[0]) t = d->sheets[i]; int j = i - 1;
        while (j >= 0 && d->sheets[j].sheet > t.sheet) { d->sheets[j + 1] = d->sheets[j]; j--; }
        d->sheets[j + 1] = t;
    }
    for (int i = 0; i < d->nsheet_text; i++) { int rn = 0; parse_rules(d, d->sheets[i].text, d->sheets[i].len, d->sheets[i].sheet, &rn, 0); }
}
/* the tree is complete: collect the variables again from the rules that match
 * something, and parse the rules with them */
static void refilter_vars(doc_t *d) {
    d->var_filter = true;
    if (!d->nvars) return;
    d->nvars = 0;
    for (int i = 0; i < d->nsheet_text; i++) { int rn = 0; parse_rules_mode(d, d->sheets[i].text, d->sheets[i].len, d->sheets[i].sheet, &rn, 0, true); }
    reparse_sheets(d);
    if (varsel_pool) { lxg_pool_free(varsel_pool); varsel_pool = NULL; }
}
void html_add_stylesheet(doc_t *d, const char *css, uint32_t len, int sheet) {
    store_sheet(d, css, (int)len, sheet);
    if (varsel_pool) { lxg_pool_free(varsel_pool); varsel_pool = NULL; }
    reparse_sheets(d);
}

/* the matcher's view of the tree: the root stands in for <body> */
static void *tr_parent(void *n) { node_t *p = ((node_t *)n)->parent; return p; }
static void *tr_prev(void *n) {
    node_t *x = n, *p = x->parent; if (!p) return NULL;
    node_t *last = NULL;
    for (node_t *c = p->first; c && c != x; c = c->next) if (!c->type) last = c;
    return last;
}
static void *tr_next(void *n) { for (node_t *c = ((node_t *)n)->next; c; c = c->next) if (!c->type) return c; return NULL; }
static const char *tr_tag(void *n) { node_t *x = n; return x->parent ? x->tag : "body"; }
static const char *tr_attr(void *n, const char *name) { return node_attr(n, name); }
static int tr_empty(void *n) { for (node_t *c = ((node_t *)n)->first; c; c = c->next) if (!c->type || (c->text && c->text[0])) return 0; return 1; }
static int tr_isroot(void *n) { return ((node_t *)n)->parent == NULL; }
static const lxg_tree_t tree_ops = { tr_parent, tr_prev, tr_next, tr_tag, tr_attr, tr_empty, tr_isroot };

typedef struct { uint32_t spec, order; int idx; } hit_t;
static void try_bucket(doc_t *d, css_bucket_t *b, node_t *n, hit_t **hits, int *nh, int *cap) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) {
        css_rule_t *r = &d->rules[b->idx[i]];
        if (!lxg_match(r->sel, n, &tree_ops)) continue;
        if (*nh == *cap) { *cap = *cap ? *cap * 2 : 32; *hits = krealloc(*hits, sizeof(hit_t) * *cap); }
        (*hits)[(*nh)++] = (hit_t){ r->spec, r->order, b->idx[i] };
    }
}
static css_t sheet_style(doc_t *d, node_t *n) {
    css_t c = { 0 };
    if (!d->nrules) return c;
    static hit_t *hits; static int cap;
    int nh = 0;
    const char *id = node_attr(n, "id");
    if (id && *id) try_bucket(d, bucket_get(d, LXG_KEY_ID, id, strlen(id), false), n, &hits, &nh, &cap);
    const char *cl = node_attr(n, "class");
    while (cl && *cl) {
        while (*cl && isspace(*cl)) cl++;
        const char *e = cl; while (*e && !isspace(*e)) e++;
        if (e > cl) {
            bool dup = false;                         /* class="a a": try a bucket only once */
            for (const char *p = node_attr(n, "class"); p < cl; p++) if (!strncmp(p, cl, e - cl) && (p == node_attr(n, "class") || isspace(p[-1])) && isspace(p[e - cl])) dup = true;
            if (!dup) try_bucket(d, bucket_get(d, LXG_KEY_CLASS, cl, e - cl, false), n, &hits, &nh, &cap);
        }
        cl = e;
    }
    const char *tag = tr_tag(n);
    try_bucket(d, bucket_get(d, LXG_KEY_TAG, tag, strlen(tag), false), n, &hits, &nh, &cap);
    try_bucket(d, bucket_get(d, LXG_KEY_ANY, "*", 1, false), n, &hits, &nh, &cap);
    /* cascade: lower specificity first, then source order */
    for (int i = 1; i < nh; i++) {
        hit_t h = hits[i]; int j = i - 1;
        while (j >= 0 && (hits[j].spec > h.spec || (hits[j].spec == h.spec && hits[j].order > h.order))) { hits[j + 1] = hits[j]; j--; }
        hits[j + 1] = h;
    }
    for (int i = 0; i < nh; i++) css_merge(&c, &d->rules[hits[i].idx].css);
    return c;
}
static void style_node(doc_t *d, node_t *n) {
    var_doc = d;
    css_t c = sheet_style(d, n);
    const char *st = node_attr(n, "style");
    if (st) { css_t in = { 0 }; css_decls(st, strlen(st), &in); css_merge(&c, &in); }
    if (node_attr(n, "hidden")) { c.set |= ST_DISPLAY; c.display = 0; }
    n->css = c;
}
static void apply_css(doc_t *d, node_t *n) {
    /* iterative pre-order walk over n and its following siblings' subtrees */
    node_t *stop = n ? n->parent : NULL;
    while (n) {
        if (!n->type) style_node(d, n);
        if (!n->type && n->first) { n = n->first; continue; }
        while (n && !n->next) { n = n->parent; if (n == stop) return; }
        if (n) n = n->next;
    }
}
static void style_root(doc_t *d) {
    css_t c = sheet_style(d, d->root);
    const char *st = node_attr(d->root, "style");
    if (st) { css_t in = { 0 }; css_decls(st, strlen(st), &in); css_merge(&c, &in); }
    if (c.set & ST_BG) d->bgcolor = c.bg;
    if (c.set & ST_COLOR) d->text = c.color;
    d->root->css = c;                     /* body's box (margins, max-width) for page_layout */
}

doc_t *html_parse(const char *src, uint32_t len, const char *charset, const char *url) {
    doc_t *d = kzalloc(sizeof *d);
    strlcpy(d->base, url ? url : "", sizeof d->base);
    /* character set: header, then <meta>, default UTF-8 */
    bool utf8 = true;
    char cs[32] = ""; if (charset) strlcpy(cs, charset, sizeof cs);
    if (!cs[0]) {
        const char *m = NULL; uint32_t lim = MIN(len, 4096u);
        for (uint32_t i = 0; i + 8 < lim; i++) if (!strncasecmp(src + i, "charset=", 8)) { m = src + i + 8; break; }
        if (m) { if (*m == '"' || *m == '\'') m++; int k = 0; while (k < 31 && (isalnum(m[k]) || m[k] == '-' || m[k] == '_')) { cs[k] = m[k]; k++; } cs[k] = 0; }
    }
    if (cs[0] && (!strncasecmp(cs, "iso-8859", 8) || !strncasecmp(cs, "windows-125", 11) || !strcasecmp(cs, "latin1") || !strcasecmp(cs, "us-ascii"))) utf8 = false;
    char *conv = NULL;
    if (!utf8) { conv = kmalloc(len * 3 + 1); len = utf8_from_1252((const uint8_t *)src, len, conv); src = conv; }
    d->root = arena_alloc(&d->arena, sizeof(node_t)); strcpy(d->root->tag, "#doc");
    builder_t *b = kzalloc(sizeof *b);
    b->d = d; b->cssdoc = d; b->url = url ? url : "";
    b->st[0].n = d->root; b->st[0].kind = FR_NODE; b->sp = 1;
    lxg_sink_t sink = { b, sink_start, sink_end, sink_text };
    if (lxg_parse_html(src, len, &sink) != 0) klog("xplorerlite: the HTML parser failed on %s", url ? url : "a page");
    kfree(b->buf); kfree(b); kfree(conv);
    refilter_vars(d);
    style_root(d);
    apply_css(d, d->root->first);
    return d;
}
/* innerHTML: nodes go into a temporary document (copied by the caller), while
 * <style> rules are added to the page's own document */
doc_t *html_parse_fragment(doc_t *page, const char *context_tag, const char *utf8, const char *url) {
    doc_t *d = kzalloc(sizeof *d);
    strlcpy(d->base, url ? url : "", sizeof d->base);
    d->root = arena_alloc(&d->arena, sizeof(node_t)); strcpy(d->root->tag, "#doc");
    builder_t *b = kzalloc(sizeof *b);
    b->d = d; b->cssdoc = page ? page : d; b->url = url ? url : ""; b->in_body = true;
    b->st[0].n = d->root; b->st[0].kind = FR_NODE; b->sp = 1;
    lxg_sink_t sink = { b, sink_start, sink_end, sink_text };
    lxg_parse_fragment(!strcmp(context_tag, "#doc") ? "body" : context_tag, utf8, strlen(utf8), &sink);
    kfree(b->buf); kfree(b);
    return d;
}
void html_restyle(doc_t *d, node_t *n) {
    if (!n || n->type) return;
    if (!n->parent) { style_root(d); n = n->first; if (!n) return; apply_css(d, n); return; }
    node_t *nx = n->next; n->next = NULL;
    apply_css(d, n);
    n->next = nx;
}
void html_restyle_all(doc_t *d) { style_root(d); apply_css(d, d->root->first); }

/* querySelector() and matches(): selectors parsed once and kept */
#define SEL_CACHE 64
static struct { char *text; void **sels; int n; } selcache[SEL_CACHE];
static void *selcache_pool; static int selcache_next;
typedef struct { void **v; int n, cap; } sellist_t;
static void collect_sel(void *ctx, void *sel, unsigned spec, int kind, const char *key, unsigned kl) {
    sellist_t *l = ctx;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4; l->v = krealloc(l->v, sizeof(void *) * l->cap); }
    l->v[l->n++] = sel;
}
static int selcache_get(const char *sel, void ***out) {
    for (int i = 0; i < SEL_CACHE; i++) if (selcache[i].text && !strcmp(selcache[i].text, sel)) { *out = selcache[i].sels; return selcache[i].n; }
    if (!selcache_pool || selcache_next >= SEL_CACHE) {         /* full: start again with a fresh pool */
        for (int i = 0; i < SEL_CACHE; i++) { kfree(selcache[i].text); kfree(selcache[i].sels); selcache[i].text = NULL; selcache[i].sels = NULL; selcache[i].n = 0; }
        lxg_pool_free(selcache_pool); selcache_pool = lxg_pool_new(); selcache_next = 0;
    }
    sellist_t l = { 0 };
    int r = lxg_selectors(selcache_pool, sel, strlen(sel), collect_sel, &l);
    int slot = selcache_next++;
    selcache[slot].text = kmalloc(strlen(sel) + 1); strcpy(selcache[slot].text, sel);
    selcache[slot].sels = l.v; selcache[slot].n = r < 0 ? -1 : l.n;
    *out = l.v;
    return selcache[slot].n;
}
int html_matches(node_t *n, const char *sel_utf8) {
    if (n->type) return 0;
    void **v; int k = selcache_get(sel_utf8, &v);
    if (k < 0) return -1;
    for (int i = 0; i < k; i++) if (lxg_match(v[i], n, &tree_ops)) return 1;
    return 0;
}
int html_selector_valid(const char *sel_utf8) { void **v; return selcache_get(sel_utf8, &v) >= 0; }
const char *html_parser_version(void) { return lxg_version(); }

void css_parse_decls(const char *s, int n, css_t *out) { css_decls(s, n, out); }
bool html_is_void(const char *t) { return is_void(t); }
void doc_free(doc_t *d) {
    if (!d) return;
    for (int i = 0; i < CSS_BUCKETS; i++) for (css_bucket_t *b = d->buckets[i]; b; b = b->next) kfree(b->idx);
    kfree(d->rules);
    kfree(d->sheets); kfree(d->vnames); kfree(d->vvals); kfree(d->vprio);
    lxg_pool_free(d->selpool);
    arena_free(&d->arena); kfree(d);
}
