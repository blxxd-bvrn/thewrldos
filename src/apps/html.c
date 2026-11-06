/* The WRLD Xplorer: HTML tokenizer and forgiving tree builder, character set
 * conversion (UTF-8 / Windows-1252 -> Latin-1 with ASCII fallbacks) and a
 * small CSS subset (simple selectors; colour, background, display, alignment,
 * bold/italic/underline, size). */
#include "browser.h"
#include "fs.h"

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
static const char *translit(uint32_t cp) {
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
    case 0x2013: case 0x2212: case 0x2010: case 0x2011: return "-";
    case 0x2014: case 0x2015: return "--";
    case 0x2026: return "...";
    case 0x2022: case 0x25CF: case 0x2219: case 0x25AA: return "\xB7";
    case 0x2122: return "(TM)";
    case 0x20AC: return "EUR";
    case 0x2190: return "<-"; case 0x2192: return "->"; case 0x2191: return "^"; case 0x2193: return "v";
    case 0x2039: return "<"; case 0x203A: return ">";
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: case 0x205F: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0x2060: return "";
    case 0x2264: return "<="; case 0x2265: return ">="; case 0x2260: return "!=";
    case 0x2713: case 0x2714: return "v"; case 0x2715: case 0x2716: case 0x2717: return "x";
    case 0x2605: case 0x2606: return "*";
    case 0x25B2: case 0x25B4: return "^"; case 0x25BC: case 0x25BE: return "v";
    case 0x00A0: return "\xA0";
    }
    if (cp >= 0x1F000) return "";            /* emoji and pictographs */
    return "?";
}
static const uint16_t cp1252[32] = { 0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
static int put_cp(char *out, uint32_t cp) {
    if (cp == '\r') return 0;
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) { out[0] = cp; return 1; }
    const char *t = translit(cp);
    int l = strlen(t); memcpy(out, t, l); return l;
}
int latin1_from_utf8(const uint8_t *s, uint32_t n, char *out) {
    int o = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t c = s[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80 && (s[i + 2] & 0xC0) == 0x80) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
        else { cp = c >= 0x80 && c < 0xA0 ? (cp1252[c - 0x80] ? cp1252[c - 0x80] : '?') : c; len = 1; }   /* stray byte: treat as Windows-1252 */
        o += put_cp(out + o, cp);
        i += len;
    }
    out[o] = 0;
    return o;
}
static int latin1_from_1252(const uint8_t *s, uint32_t n, char *out) {
    int o = 0;
    for (uint32_t i = 0; i < n; i++) { uint8_t c = s[i]; o += put_cp(out + o, c >= 0x80 && c < 0xA0 ? (cp1252[c - 0x80] ? cp1252[c - 0x80] : '?') : c); }
    out[o] = 0;
    return o;
}

/* ---------------- entities ---------------- */
static const struct { const char *n; uint16_t cp; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE },
    { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB },
    { "middot", 0xB7 }, { "bull", 0x2022 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "sbquo", 0x201A }, { "bdquo", 0x201E }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "euro", 0x20AC },
    { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 }, { "para", 0xB6 }, { "frac12", 0xBD }, { "frac14", 0xBC }, { "frac34", 0xBE },
    { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "ecirc", 0xEA }, { "euml", 0xEB }, { "aacute", 0xE1 }, { "agrave", 0xE0 }, { "acirc", 0xE2 }, { "auml", 0xE4 },
    { "aring", 0xE5 }, { "atilde", 0xE3 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 }, { "iacute", 0xED }, { "igrave", 0xEC }, { "icirc", 0xEE }, { "iuml", 0xEF },
    { "oacute", 0xF3 }, { "ograve", 0xF2 }, { "ocirc", 0xF4 }, { "ouml", 0xF6 }, { "otilde", 0xF5 }, { "oslash", 0xF8 }, { "uacute", 0xFA }, { "ugrave", 0xF9 },
    { "ucirc", 0xFB }, { "uuml", 0xFC }, { "ntilde", 0xF1 }, { "szlig", 0xDF }, { "yacute", 0xFD }, { "yuml", 0xFF }, { "Eacute", 0xC9 }, { "Agrave", 0xC0 },
    { "Aacute", 0xC1 }, { "Auml", 0xC4 }, { "Aring", 0xC5 }, { "AElig", 0xC6 }, { "Ccedil", 0xC7 }, { "Ouml", 0xD6 }, { "Oslash", 0xD8 }, { "Uuml", 0xDC },
    { "Ntilde", 0xD1 }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "shy", 0 }, { "zwj", 0x200D }, { "zwnj", 0x200C }, { "thinsp", 0x2009 }, { "ensp", 0x2002 },
    { "emsp", 0x2003 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "micro", 0xB5 }, { "ordm", 0xBA }, { "ordf", 0xAA },
    { "sup2", 0xB2 }, { "sup3", 0xB3 }, { "sup1", 0xB9 }, { "acute", 0xB4 }, { "cedil", 0xB8 }, { "macr", 0xAF }, { "uml", 0xA8 }, { "not", 0xAC }, { "brvbar", 0xA6 },
    { "curren", 0xA4 }, { "prime", 0x2032 }, { "Prime", 0x2033 }, { "minus", 0x2212 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A },
};
/* decodes entities in [s, s+n) into out (Latin-1); returns length */
static int decode_entities(const char *s, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        if (s[i] != '&') { out[o++] = s[i++]; continue; }
        int j = i + 1; uint32_t cp = 0; bool ok = false;
        if (j < n && s[j] == '#') {
            j++;
            bool hex = j < n && (s[j] == 'x' || s[j] == 'X'); if (hex) j++;
            int st = j;
            while (j < n && (hex ? isxdigit(s[j]) : isdigit(s[j])) && j - st < 8) { int c = tolower(s[j]); cp = cp * (hex ? 16 : 10) + (c <= '9' ? c - '0' : c - 'a' + 10); j++; }
            ok = j > st;
            if (ok && cp >= 0x80 && cp < 0xA0) cp = cp1252[cp - 0x80] ? cp1252[cp - 0x80] : '?';
        } else {
            int st = j;
            while (j < n && isalnum(s[j]) && j - st < 10) j++;
            for (unsigned k = 0; k < ARRAY_LEN(ents); k++) if ((int)strlen(ents[k].n) == j - st && !memcmp(ents[k].n, s + st, j - st)) { cp = ents[k].cp; ok = true; break; }
        }
        if (!ok) { out[o++] = s[i++]; continue; }
        if (j < n && s[j] == ';') j++;
        if (cp) o += put_cp(out + o, cp);
        i = j;
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
static void css_decls(const char *s, int n, css_t *c) {
    char buf[1024]; n = MIN(n, 1023); memcpy(buf, s, n); buf[n] = 0;
    for (char *d = strtok(buf, ";"); d; d = strtok(NULL, ";")) {
        char *colon = strchr(d, ':'); if (!colon) continue;
        *colon = 0; char *k = d, *v = colon + 1;
        while (*k == ' ' || *k == '\n' || *k == '\t' || *k == '\r') k++;
        for (char *e = k + strlen(k) - 1; e >= k && (*e == ' ' || *e == '\n' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *p = k; *p; p++) *p = tolower(*p);
        bool imp = strstr(v, "!important") != NULL;
        (void)imp;
        if (!strcmp(k, "color")) { if (parse_color(v, &c->color)) c->set |= ST_COLOR; }
        else if (!strcmp(k, "background-color") || !strcmp(k, "background")) { if (parse_color(v, &c->bg)) c->set |= ST_BG; }
        else if (!strcmp(k, "display")) { c->set |= ST_DISPLAY; c->display = !strncmp(v, "none", 4) ? 0 : !strncmp(v, "inline", 6) ? 2 : 1; }
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
static void parse_stylesheet(doc_t *d, const char *s, int n) {
    for (int i = 0; i < n;) {
        while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') { const char *e = strstr(s + i + 2, "*/"); if (!e || e >= s + n) break; i = e - s + 2; continue; }
        if (i >= n) break;
        int sel = i;
        while (i < n && s[i] != '{') i++;
        if (i >= n) break;
        int sel_end = i, depth = 1, body = ++i;
        if (s[sel] == '@') {            /* @media etc.: skip the whole block */
            while (i < n && depth) { if (s[i] == '{') depth++; else if (s[i] == '}') depth--; i++; }
            continue;
        }
        while (i < n && s[i] != '}') i++;
        int body_end = i++;
        css_t c = { 0 };
        css_decls(s + body, body_end - body, &c);
        if (!c.set) continue;
        /* only simple selectors (tag, .class, #id, tag.class) - descendant/pseudo selectors are skipped */
        char sl[512]; int l = MIN(sel_end - sel, 511); memcpy(sl, s + sel, l); sl[l] = 0;
        for (char *one = strtok(sl, ","); one; one = strtok(NULL, ",")) {
            while (*one == ' ' || *one == '\n' || *one == '\t' || *one == '\r') one++;
            for (char *e = one + strlen(one) - 1; e >= one && (*e == ' ' || *e == '\n' || *e == '\t' || *e == '\r'); e--) *e = 0;
            if (!*one || strpbrk(one, " >+~:[*()") || strlen(one) >= 48) continue;
            if (d->nrules % 64 == 0) {
                css_rule_t *nr = arena_alloc(&d->arena, sizeof(css_rule_t) * (d->nrules + 64));
                if (d->nrules) memcpy(nr, d->rules, sizeof(css_rule_t) * d->nrules);
                d->rules = nr;
            }
            strlcpy(d->rules[d->nrules].sel, one, 48);
            d->rules[d->nrules].css = c;
            d->nrules++;
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
    dst->set |= src->set;
}
static bool has_class(node_t *n, const char *cls, int len) {
    const char *c = node_attr(n, "class");
    while (c && *c) {
        while (*c == ' ') c++;
        const char *e = c; while (*e && *e != ' ') e++;
        if (e - c == len && !strncasecmp(c, cls, len)) return true;
        c = e;
    }
    return false;
}
static bool sel_match(node_t *n, const char *sel) {
    const char *dot = strchr(sel, '.'), *hash = strchr(sel, '#');
    const char *tagend = dot ? dot : hash ? hash : sel + strlen(sel);
    if (tagend > sel && (strncasecmp(n->tag, sel, tagend - sel) || n->tag[tagend - sel])) return false;
    if (dot) { const char *e = dot + 1; while (*e && *e != '.' && *e != '#') e++; if (!has_class(n, dot + 1, e - dot - 1)) return false; }
    if (hash) { const char *id = node_attr(n, "id"); if (!id || strcasecmp(id, hash + 1)) return false; }
    return tagend > sel || dot || hash;
}
static void apply_css(doc_t *d, node_t *n) {
    for (; n; n = n->next) {
        if (n->type) continue;
        css_t c = { 0 };
        for (int pass = 0; pass < 3; pass++)        /* tag rules, then class rules, then id rules */
            for (int i = 0; i < d->nrules; i++) {
                const char *s = d->rules[i].sel;
                int kind = strchr(s, '#') ? 2 : strchr(s, '.') ? 1 : 0;
                if (kind == pass && sel_match(n, s)) css_merge(&c, &d->rules[i].css);
            }
        const char *st = node_attr(n, "style");
        if (st) { css_t in = { 0 }; css_decls(st, strlen(st), &in); css_merge(&c, &in); }
        if (node_attr(n, "hidden")) { c.set |= ST_DISPLAY; c.display = 0; }
        n->css = c;
        apply_css(d, n->first);
    }
}

/* ---------------- tree building ---------------- */
static const char *strcasestr_simple(const char *h, const char *n) {
    int l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return h;
    return NULL;
}
const char *node_attr(node_t *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attr[i * 2], name)) return n->attr[i * 2 + 1];
    return NULL;
}
static bool is_void(const char *t) {
    static const char *v[] = { "br", "img", "hr", "input", "meta", "link", "area", "base", "col", "embed", "source", "wbr", "param", "track", "keygen" };
    for (unsigned i = 0; i < ARRAY_LEN(v); i++) if (!strcmp(t, v[i])) return true;
    return false;
}
static bool closes_p(const char *t) {
    static const char *v[] = { "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl", "table", "pre", "blockquote", "hr", "form",
        "section", "article", "header", "footer", "nav", "aside", "main", "figure", "address", "fieldset", "center", "li", "dd", "dt", "details" };
    for (unsigned i = 0; i < ARRAY_LEN(v); i++) if (!strcmp(t, v[i])) return true;
    return false;
}
typedef struct { doc_t *d; node_t *stack[256]; int sp; } builder_t;
static node_t *cur(builder_t *b) { return b->stack[b->sp - 1]; }
static void append(builder_t *b, node_t *n) { node_t *p = cur(b); n->parent = p; if (p->last) p->last->next = n; else p->first = n; p->last = n; }
static bool in_scope(builder_t *b, const char *tag, const char *stop1, const char *stop2) {
    for (int i = b->sp - 1; i > 0; i--) {
        if (!strcmp(b->stack[i]->tag, tag)) return true;
        if ((stop1 && !strcmp(b->stack[i]->tag, stop1)) || (stop2 && !strcmp(b->stack[i]->tag, stop2)) || !strcmp(b->stack[i]->tag, "table")) return false;
    }
    return false;
}
static void pop_to(builder_t *b, const char *tag) { while (b->sp > 1) { bool m = !strcmp(cur(b)->tag, tag); b->sp--; if (m) break; } }
static void add_text(builder_t *b, const char *s, int n) {
    if (n <= 0) return;
    node_t *last = cur(b)->last;
    char *t = arena_alloc(&b->d->arena, n * 4 + 1);
    int l = decode_entities(s, n, t);
    if (last && last->type == 1) {             /* merge adjacent text */
        int ol = strlen(last->text);
        char *m = arena_alloc(&b->d->arena, ol + l + 1); memcpy(m, last->text, ol); memcpy(m + ol, t, l + 1); last->text = m;
        return;
    }
    node_t *nd = arena_alloc(&b->d->arena, sizeof *nd);
    nd->type = 1; nd->text = t;
    append(b, nd);
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
    char *text = kmalloc(len * 3 + 1);
    uint32_t tl = utf8 ? latin1_from_utf8((const uint8_t *)src, len, text) : latin1_from_1252((const uint8_t *)src, len, text);
    builder_t *b = kzalloc(sizeof *b);
    b->d = d;
    d->root = arena_alloc(&d->arena, sizeof(node_t)); strcpy(d->root->tag, "#doc");
    b->stack[0] = d->root; b->sp = 1;
    const char *s = text, *e = text + tl;
    while (s < e) {
        if (*s != '<') {
            const char *t = s; while (t < e && *t != '<') t++;
            add_text(b, s, t - s); s = t; continue;
        }
        if (s + 4 <= e && !strncmp(s, "<!--", 4)) { const char *c = strstr(s + 4, "-->"); s = c ? c + 3 : e; continue; }
        if (s + 1 < e && (s[1] == '!' || s[1] == '?')) { const char *c = memchr(s, '>', e - s); s = c ? c + 1 : e; continue; }
        bool end = s + 1 < e && s[1] == '/';
        const char *p = s + (end ? 2 : 1);
        if (p >= e || !isalpha(*p)) { add_text(b, s, 1); s++; continue; }
        char tag[12]; int k = 0;
        while (p < e && (isalnum(*p) || *p == '-' || *p == ':') ) { if (k < 11) tag[k++] = tolower(*p); p++; }
        tag[k] = 0;
        /* attributes */
        char *names[32], *vals[32]; int na = 0;
        while (p < e && *p != '>') {
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r' || *p == '/')) p++;
            if (p >= e || *p == '>') break;
            const char *an = p; while (p < e && *p != '=' && *p != '>' && *p != ' ' && *p != '\n' && *p != '\t' && *p != '/') p++;
            int anl = p - an;
            while (p < e && (*p == ' ' || *p == '\n')) p++;
            const char *av = ""; int avl = 0;
            if (p < e && *p == '=') {
                p++; while (p < e && (*p == ' ' || *p == '\n')) p++;
                if (p < e && (*p == '"' || *p == '\'')) { char q = *p++; av = p; while (p < e && *p != q) p++; avl = p - av; if (p < e) p++; }
                else { av = p; while (p < e && *p != '>' && *p != ' ' && *p != '\n' && *p != '\t') p++; avl = p - av; }
            }
            if (!end && na < 32 && anl > 0 && anl < 40) {
                char *nm = arena_alloc(&d->arena, anl + 1); for (int i = 0; i < anl; i++) nm[i] = tolower(an[i]);
                char *vv = arena_alloc(&d->arena, avl * 4 + 1); decode_entities(av, avl, vv);
                names[na] = nm; vals[na] = vv; na++;
            }
        }
        s = p < e ? p + 1 : e;
        if (end) {
            if (!strcmp(tag, "p") && !in_scope(b, "p", NULL, NULL)) continue;
            for (int i = b->sp - 1; i > 0; i--) if (!strcmp(b->stack[i]->tag, tag)) { b->sp = i; break; }
            continue;
        }
        /* raw text elements */
        if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "title") || !strcmp(tag, "textarea")) {
            char close[16]; snprintf(close, sizeof close, "</%s", tag);
            const char *c = s; while (c < e && strncasecmp(c, close, strlen(close))) c++;
            if (!strcmp(tag, "style")) parse_stylesheet(d, s, c - s);
            else if (!strcmp(tag, "title")) { char *t = arena_alloc(&d->arena, (c - s) * 4 + 1); decode_entities(s, c - s, t); for (char *q = t; *q; q++) if (*q == '\n' || *q == '\t') *q = ' '; char *tt = t; while (*tt == ' ') tt++; strlcpy(d->title, tt, sizeof d->title); }
            else if (!strcmp(tag, "textarea")) {
                node_t *n = arena_alloc(&d->arena, sizeof *n); strcpy(n->tag, tag);
                n->attr = arena_alloc(&d->arena, sizeof(char *) * (na * 2 + 2)); n->nattr = na;
                for (int i = 0; i < na; i++) { n->attr[i * 2] = names[i]; n->attr[i * 2 + 1] = vals[i]; }
                append(b, n);
                node_t *t = arena_alloc(&d->arena, sizeof *t); t->type = 1; t->text = arena_alloc(&d->arena, (c - s) * 4 + 1); decode_entities(s, c - s, t->text);
                t->parent = n; n->first = n->last = t;
            }
            const char *gt = c < e ? memchr(c, '>', e - c) : NULL;
            s = gt ? gt + 1 : e;
            continue;
        }
        if (!strcmp(tag, "html") || !strcmp(tag, "head")) continue;
        if (!strcmp(tag, "body")) {
            for (int i = 0; i < na; i++) {
                if (!strcmp(names[i], "bgcolor")) parse_color(vals[i], &d->bgcolor);
                else if (!strcmp(names[i], "text")) parse_color(vals[i], &d->text);
                else if (!strcmp(names[i], "link")) parse_color(vals[i], &d->link);
                else if (!strcmp(names[i], "vlink")) parse_color(vals[i], &d->vlink);
                else if (!strcmp(names[i], "style")) { css_t c = { 0 }; css_decls(vals[i], strlen(vals[i]), &c); if (c.set & ST_BG) d->bgcolor = c.bg; if (c.set & ST_COLOR) d->text = c.color; }
            }
            continue;
        }
        if (!strcmp(tag, "base")) { for (int i = 0; i < na; i++) if (!strcmp(names[i], "href")) { extern bool url_resolve(const char *, const char *, char *, int); url_resolve(url, vals[i], d->base, sizeof d->base); } continue; }
        if (!strcmp(tag, "meta")) {
            const char *he = NULL, *ct = NULL;
            for (int i = 0; i < na; i++) { if (!strcmp(names[i], "http-equiv")) he = vals[i]; if (!strcmp(names[i], "content")) ct = vals[i]; }
            if (he && ct && !strcasecmp(he, "refresh")) {
                d->refresh_s = atoi(ct);
                const char *u = strcasestr_simple(ct, "url=");
                if (u) { u += 4; while (*u == ' ' || *u == '\'' || *u == '"') u++; strlcpy(d->refresh, u, sizeof d->refresh); char *q = strpbrk(d->refresh, "'\""); if (q) *q = 0; }
            }
            continue;
        }
        if (!strcmp(tag, "link")) continue;
        /* implicit end tags */
        if (closes_p(tag) && in_scope(b, "p", NULL, NULL)) pop_to(b, "p");
        if (!strcmp(tag, "li") && in_scope(b, "li", "ul", "ol")) pop_to(b, "li");
        if ((!strcmp(tag, "dt") || !strcmp(tag, "dd")) && (in_scope(b, "dd", "dl", NULL) || in_scope(b, "dt", "dl", NULL))) { while (b->sp > 1 && strcmp(cur(b)->tag, "dt") && strcmp(cur(b)->tag, "dd")) b->sp--; if (b->sp > 1) b->sp--; }
        if (!strcmp(tag, "option") && !strcmp(cur(b)->tag, "option")) b->sp--;
        if (!strcmp(tag, "tr") || !strcmp(tag, "tbody") || !strcmp(tag, "thead") || !strcmp(tag, "tfoot")) {
            /* close the open row (and, for a new section, the open section) inside the current table */
            bool section = strcmp(tag, "tr") != 0;
            for (int i = b->sp - 1; i > 0; i--) {
                const char *t = b->stack[i]->tag;
                if (!strcmp(t, "table")) break;
                if (!strcmp(t, "tr") || (section && (!strcmp(t, "tbody") || !strcmp(t, "thead") || !strcmp(t, "tfoot")))) b->sp = i;
            }
        }
        if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
            for (int i = b->sp - 1; i > 0; i--) {
                const char *t = b->stack[i]->tag;
                if (!strcmp(t, "td") || !strcmp(t, "th")) { b->sp = i; break; }
                if (!strcmp(t, "tr") || !strcmp(t, "table")) break;
            }
        }
        node_t *n = arena_alloc(&d->arena, sizeof *n);
        strcpy(n->tag, tag);
        n->attr = arena_alloc(&d->arena, sizeof(char *) * (na * 2 + 2)); n->nattr = na;
        for (int i = 0; i < na; i++) { n->attr[i * 2] = names[i]; n->attr[i * 2 + 1] = vals[i]; }
        append(b, n);
        if (!is_void(tag) && b->sp < 255) b->stack[b->sp++] = n;
    }
    kfree(b); kfree(text);
    apply_css(d, d->root->first);
    return d;
}
void doc_free(doc_t *d) { if (!d) return; arena_free(&d->arena); kfree(d); }
