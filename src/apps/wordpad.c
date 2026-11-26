/* WordPad: a rich-text word processor in the style of Windows 7's, with a ribbon.
 *
 *   File menu (the blue button): New, Open, Save, Save as, About, Exit.
 *   Home:  Clipboard (Paste, Cut, Copy), Font (family, size, grow and shrink, bold,
 *          italic, underline, strikethrough, subscript, superscript, highlight and text
 *          colour), Paragraph (indents, bullets, line spacing, alignment), Insert (date
 *          and time), Editing (Find, Replace, Select all).
 *   View:  Zoom (in, out, 100%), Show or hide (ruler, status bar), Settings (word wrap).
 *   A ruler, the page, and a status bar with the zoom.
 *
 * A document is a list of paragraphs; every byte of a paragraph's UTF-8 text has a style
 * (font family, size, bold, italic, underline, strikethrough, sub/superscript, colour,
 * highlight). Files: Rich Text (.rtf, the default), Office Open XML (.docx) and plain text
 * are read and written; OpenDocument Text (.odt) is read. Undo keeps RTF snapshots. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"
#include "raster.h"

typedef void (*fdlg_cb)(void *ctx, const char *path);
void file_dialog(const char *title, const char *initial, bool save, const char *ext, fdlg_cb cb, void *ctx);
int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *dst, uint32_t cap, uint32_t *outlen);
void rtc_now(rtc_time_t *t);

#define TABS_H 25                      /* the File button and the tabs */
#define RIB_H 94                       /* the ribbon's body */
#define RULER_H 24
#define STATUS_H 24
#define PAGE_IN_W 8.5
#define DPI 96
#define MARGIN_PX (DPI * 5 / 4)        /* 1.25 inches left and right, as WordPad */
#define TOP_PX DPI                     /* 1 inch at the top */
#define UNDO_MAX 40

/* ---------------------------------------------------------------- styles */
typedef struct { uint8_t fam, b, i, u, s, script; uint16_t hp; uint32_t fg, bg; } cst_t;   /* hp: half-points; script 1 sub 2 super; bg 0: none */
static cst_t stab[1024];
static int nst;
static int st_id(const cst_t *c) {
    for (int i = 0; i < nst; i++) if (!memcmp(&stab[i], c, sizeof *c)) return i;
    if (nst >= (int)ARRAY_LEN(stab)) return 0;
    stab[nst] = *c;
    return nst++;
}
static cst_t default_style(void) { cst_t c; memset(&c, 0, sizeof c); c.fam = FAM_SERIF; c.hp = 22; c.fg = 0xFF000000; return c; }   /* 11 pt, as WordPad */
static const char *fam_name[3] = { "Liberation Sans", "Liberation Serif", "Liberation Mono" };

/* ---------------------------------------------------------------- the document */
typedef struct {
    char *t; uint16_t *a; int n, cap;  /* text (UTF-8) and a style per byte */
    uint16_t endst;                    /* the style of an empty paragraph */
    uint8_t align, bullet, lsp;        /* align 0 left 1 centre 2 right 3 justify; lsp: line spacing x100 / 5 (20 = 1.0) */
    int16_t lind, first, rind;         /* indents in pixels at 100% */
    /* layout */
    bool dirty; int16_t *cw; int nl, lcap; int *ls, *lh, *la, *lw; int y, h;
} para_t;
typedef struct { int p, o; } pos_t;
typedef struct {
    para_t *p; int np, pcap;
    pos_t cur, anc;                    /* caret and anchor (a selection when they differ) */
    int typing;                        /* the style new text gets */
    int zoom;                          /* percent */
    int scroll, doc_h, want_x;
    char path[256]; int fmt;           /* 0 rtf 1 docx 2 txt 3 odt (read only) */
    bool modified, ruler, status, dragging, closing, shift;
    int wrap;                          /* 0 no wrap, 1 wrap to window, 2 wrap to ruler */
    int tab;                           /* 0 Home, 1 View */
    char *undo[UNDO_MAX]; int nundo; char *redo[UNDO_MAX]; int nredo; int undo_kind;
    char find[128], repl[128];
    window_t *w;
    int page_x, page_w, text_x, text_w, view_y, view_h;   /* layout of the current paint */
} wp_t;

static void para_init(para_t *p, int st) { memset(p, 0, sizeof *p); p->cap = 16; p->t = kmalloc(p->cap); p->a = kmalloc(p->cap * 2); p->endst = st; p->lsp = 20; p->dirty = true; }
static void para_free(para_t *p) { kfree(p->t); kfree(p->a); kfree(p->cw); kfree(p->ls); kfree(p->lh); kfree(p->la); kfree(p->lw); }
static void para_room(para_t *p, int n) {
    if (p->n + n <= p->cap) return;
    while (p->cap < p->n + n) p->cap *= 2;
    p->t = krealloc(p->t, p->cap); p->a = krealloc(p->a, p->cap * 2);
}
static void doc_clear(wp_t *d) {
    for (int i = 0; i < d->np; i++) para_free(&d->p[i]);
    d->np = 0;
}
static para_t *doc_add(wp_t *d, int at, int st) {
    if (d->np >= d->pcap) { d->pcap = d->pcap ? d->pcap * 2 : 16; d->p = krealloc(d->p, d->pcap * sizeof(para_t)); }
    memmove(&d->p[at + 1], &d->p[at], (d->np - at) * sizeof(para_t));
    d->np++;
    para_init(&d->p[at], st);
    return &d->p[at];
}
static void doc_new(wp_t *d) {
    doc_clear(d);
    cst_t c = default_style();
    d->typing = st_id(&c);
    doc_add(d, 0, d->typing);
    d->cur.p = d->cur.o = d->anc.p = d->anc.o = 0;
    d->scroll = 0;
}
static void all_dirty(wp_t *d) { for (int i = 0; i < d->np; i++) d->p[i].dirty = true; }
static int style_at(wp_t *d, pos_t q) {         /* the style of the text just before a position */
    para_t *p = &d->p[q.p];
    if (q.o > 0) return p->a[q.o - 1];
    if (p->n) return p->a[0];
    return p->endst;
}
static int u8len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
static int prev_char(para_t *p, int o) { if (o <= 0) return 0; o--; while (o > 0 && ((unsigned char)p->t[o] & 0xC0) == 0x80) o--; return o; }
static int next_char(para_t *p, int o) { if (o >= p->n) return p->n; o += u8len((unsigned char)p->t[o]); return o > p->n ? p->n : o; }
static int pcmp(pos_t a, pos_t b) { return a.p != b.p ? a.p - b.p : a.o - b.o; }
static bool has_sel(wp_t *d) { return pcmp(d->cur, d->anc) != 0; }
static void sel_range(wp_t *d, pos_t *a, pos_t *b) { if (pcmp(d->cur, d->anc) < 0) { *a = d->cur; *b = d->anc; } else { *a = d->anc; *b = d->cur; } }

/* ---------------------------------------------------------------- fonts and layout */
static int font_of(wp_t *d, int st) {
    cst_t *c = &stab[st];
    int px = (c->hp * DPI * d->zoom + 72 * 100) / (2 * 72 * 100);
    if (c->script) px = px * 2 / 3;
    return font_sized(c->fam, px < 6 ? 6 : px, c->b, c->i);
}
static int zpx(wp_t *d, int v) { return v * d->zoom / 100; }
static void layout_para(wp_t *d, para_t *p, int width) {
    kfree(p->cw); p->cw = kmalloc((p->n + 1) * sizeof(int16_t));
    int tabw = zpx(d, DPI / 2);
    int xstart = zpx(d, p->lind + (p->bullet ? DPI / 4 : 0));
    for (int i = 0; i < p->n; ) {
        int cl = u8len((unsigned char)p->t[i]);
        if (i + cl > p->n) cl = p->n - i;
        int w = p->t[i] == '\t' ? 0 : text_width_n(font_of(d, p->a[i]), p->t + i, cl);
        p->cw[i] = w;
        for (int k = 1; k < cl; k++) p->cw[i + k] = 0;
        i += cl;
    }
    p->nl = 0;
    int avail = width - zpx(d, p->rind) - xstart;
    if (avail < 20) avail = 20;
    int start = 0;
    do {
        if (p->nl + 1 >= p->lcap) {
            p->lcap = p->lcap ? p->lcap * 2 : 8;
            p->ls = krealloc(p->ls, p->lcap * sizeof(int)); p->lh = krealloc(p->lh, p->lcap * sizeof(int));
            p->la = krealloc(p->la, p->lcap * sizeof(int)); p->lw = krealloc(p->lw, p->lcap * sizeof(int));
        }
        int first = p->nl == 0 ? zpx(d, p->first) : 0;
        int x = first, i = start, brk = -1, asc = 0, desc = 0;
        while (i < p->n) {
            int w = p->cw[i];
            if (p->t[i] == '\t') { int abs_x = xstart + x; w = (abs_x / tabw + 1) * tabw - abs_x; p->cw[i] = w; }
            if (d->wrap && x + w > avail && i > start) break;
            if (p->t[i] == ' ' || p->t[i] == '\t') brk = i + 1;
            int f = font_of(d, p->a[i]);
            int ha = font_ascent(f), hd = font_height(f) - font_ascent(f);
            if (stab[p->a[i]].script == 2) ha += font_height(f) / 2;
            if (stab[p->a[i]].script == 1) hd += font_height(f) / 3;
            if (ha > asc) asc = ha;
            if (hd > desc) desc = hd;
            x += w; i = next_char(p, i);
        }
        int end = i;
        if (i < p->n && brk > start) {                   /* break after the last space */
            end = brk;
            asc = desc = 0; x = first;
            for (int k = start; k < end; k++) {
                if (!p->cw[k] && ((unsigned char)p->t[k] & 0xC0) == 0x80) continue;
                int f = font_of(d, p->a[k]);
                int ha = font_ascent(f), hd = font_height(f) - font_ascent(f);
                if (ha > asc) asc = ha;
                if (hd > desc) desc = hd;
                x += p->cw[k];
            }
        }
        if (!asc) { int f = font_of(d, p->n ? p->a[end > 0 ? end - 1 : 0] : p->endst); asc = font_ascent(f); desc = font_height(f) - asc; }
        p->ls[p->nl] = start; p->la[p->nl] = asc; p->lh[p->nl] = (asc + desc) * p->lsp / 20; p->lw[p->nl] = x;
        p->nl++;
        start = end;
    } while (start < p->n);
    p->ls[p->nl] = p->n;
    p->h = 0;
    for (int l = 0; l < p->nl; l++) p->h += p->lh[l];
    p->h += zpx(d, 10) * p->lsp / 20;                   /* a little space after each paragraph */
    p->dirty = false;
}
static void layout(wp_t *d) {
    int y = 0;
    for (int i = 0; i < d->np; i++) {
        para_t *p = &d->p[i];
        if (p->dirty) layout_para(d, p, d->text_w);
        p->y = y; y += p->h;
    }
    d->doc_h = y;
}
/* where a line starts horizontally, from the alignment */
static int line_x(wp_t *d, para_t *p, int l) {
    int xstart = zpx(d, p->lind + (p->bullet ? DPI / 4 : 0)) + (l == 0 ? zpx(d, p->first) : 0);
    int avail = d->text_w - zpx(d, p->rind) - zpx(d, p->lind + (p->bullet ? DPI / 4 : 0));
    int w = p->lw[l] - (l == 0 ? zpx(d, p->first) : 0);
    if (p->align == 1) return xstart + (avail - w) / 2;
    if (p->align == 2) return xstart + avail - w;
    return xstart;
}
static int line_of(para_t *p, int o) { for (int l = 0; l < p->nl; l++) if (o < p->ls[l + 1] || l == p->nl - 1) return l; return 0; }
static void caret_xy(wp_t *d, pos_t q, int *x, int *y, int *h) {
    para_t *p = &d->p[q.p];
    int l = line_of(p, q.o);
    if (q.o == p->ls[l + 1] && l + 1 < p->nl && q.o > p->ls[l]) { /* end of a wrapped line: the next line's start */ }
    int cx = line_x(d, p, l);
    for (int k = p->ls[l]; k < q.o && k < p->n; k++) cx += p->cw[k];
    int cy = p->y; for (int k = 0; k < l; k++) cy += p->lh[k];
    *x = cx; *y = cy; *h = p->lh[l];
}
static pos_t pos_at(wp_t *d, int x, int y) {
    pos_t q = { d->np - 1, d->p[d->np - 1].n };
    for (int i = 0; i < d->np; i++) {
        para_t *p = &d->p[i];
        if (y >= p->y + p->h && i < d->np - 1) continue;
        int ly = p->y, l = 0;
        for (; l < p->nl - 1; l++) { if (y < ly + p->lh[l]) break; ly += p->lh[l]; }
        int cx = line_x(d, p, l), o = p->ls[l], end = p->ls[l + 1];
        if (end > p->ls[l] && l < p->nl - 1 && (p->t[end - 1] == ' ')) end--;   /* past a wrapped line: before its space */
        while (o < end) {
            int nx = next_char(p, o), w = 0;
            for (int k = o; k < nx; k++) w += p->cw[k];
            if (x < cx + w / 2) break;
            cx += w; o = nx;
        }
        q.p = i; q.o = o;
        return q;
    }
    return q;
}

/* ---------------------------------------------------------------- RTF: writing (also the undo snapshots) */
typedef struct { char *b; int n, cap; } sb_t;
static void sb_put(sb_t *s, const char *t, int n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 256; s->b = krealloc(s->b, s->cap); }
    memcpy(s->b + s->n, t, n); s->n += n; s->b[s->n] = 0;
}
static void sb_add(sb_t *s, const char *t) { sb_put(s, t, strlen(t)); }
static void sb_addf(sb_t *s, const char *fmt, ...) { char t[600]; va_list ap; va_start(ap, fmt); vsnprintf(t, sizeof t, fmt, ap); va_end(ap); sb_add(s, t); }
static uint32_t utf8_cp(const char *s, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if (u[0] < 0xE0) { *len = 2; return ((u[0] & 31) << 6) | (u[1] & 63); }
    if (u[0] < 0xF0) { *len = 3; return ((u[0] & 15) << 12) | ((u[1] & 63) << 6) | (u[2] & 63); }
    *len = 4; return ((u[0] & 7) << 18) | ((u[1] & 63) << 12) | ((u[2] & 63) << 6) | (u[3] & 63);
}
/* colours: the table is built from what the document uses */
static int col_index(uint32_t *tab, int *n, uint32_t c) {
    c &= 0xFFFFFF;
    for (int i = 0; i < *n; i++) if (tab[i] == c) return i + 1;
    if (*n < 64) tab[(*n)++] = c;
    return *n;
}
static char *rtf_write(wp_t *d, pos_t a, pos_t b) {
    uint32_t cols[64]; int ncol = 0;
    sb_t body = { 0 };
    int prev = -1;
    for (int pi = a.p; pi <= b.p; pi++) {
        para_t *p = &d->p[pi];
        sb_addf(&body, "\\pard%s%s\\li%d\\fi%d\\ri%d\\sl%d\\slmult1 ", p->align == 1 ? "\\qc" : p->align == 2 ? "\\qr" : p->align == 3 ? "\\qj" : "\\ql",
                p->bullet ? "{\\pntext\\'95\\tab}{\\*\\pn\\pnlvlblt\\pnf0\\pnindent360{\\pntxtb\\'95}}" : "",
                (p->lind + (p->bullet ? DPI / 4 : 0)) * 15, p->bullet ? -(DPI / 4) * 15 : p->first * 15, p->rind * 15, p->lsp * 12);
        int o0 = pi == a.p ? a.o : 0, o1 = pi == b.p ? b.o : p->n;
        if (o0 >= o1) { cst_t *c = &stab[p->endst]; sb_addf(&body, "\\plain\\f%d\\fs%d ", c->fam, c->hp); prev = -1; }
        for (int i = o0; i < o1; ) {
            int st = p->a[i];
            if (st != prev) {
                cst_t *c = &stab[st];
                sb_addf(&body, "\\plain\\f%d\\fs%d", c->fam, c->hp);
                if (c->b) sb_add(&body, "\\b");
                if (c->i) sb_add(&body, "\\i");
                if (c->u) sb_add(&body, "\\ul");
                if (c->s) sb_add(&body, "\\strike");
                if (c->script == 1) sb_add(&body, "\\sub");
                if (c->script == 2) sb_add(&body, "\\super");
                if ((c->fg & 0xFFFFFF) != 0) sb_addf(&body, "\\cf%d", col_index(cols, &ncol, c->fg));
                if (c->bg) sb_addf(&body, "\\highlight%d", col_index(cols, &ncol, c->bg));
                sb_add(&body, " ");
                prev = st;
            }
            int len; uint32_t cp = utf8_cp(p->t + i, &len);
            if (cp == '\\' || cp == '{' || cp == '}') { char e[3] = { '\\', (char)cp, 0 }; sb_add(&body, e); }
            else if (cp == '\t') sb_add(&body, "\\tab ");
            else if (cp < 0x80) sb_put(&body, p->t + i, 1);
            else sb_addf(&body, "\\u%d?", cp > 32767 ? (int)cp - 65536 : (int)cp);
            i += len;
        }
        if (pi < b.p) sb_add(&body, "\\par\n");
    }
    sb_t out = { 0 };
    sb_add(&out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss Liberation Sans;}{\\f1\\froman Liberation Serif;}{\\f2\\fmodern Liberation Mono;}}\n");
    sb_add(&out, "{\\colortbl ;");
    for (int i = 0; i < ncol; i++) sb_addf(&out, "\\red%d\\green%d\\blue%d;", (cols[i] >> 16) & 255, (cols[i] >> 8) & 255, cols[i] & 255);
    sb_add(&out, "}\n{\\*\\generator THE WRLD OS WordPad;}\\viewkind4\n");
    if (body.b) sb_add(&out, body.b);
    sb_add(&out, "\\par\n}\n");
    kfree(body.b);
    return out.b;
}

/* ---------------------------------------------------------------- reading (rust/src/wpdoc.rs) */
/* The readers for Rich Text, .docx and .odt are in Rust: they build a list of paragraphs (text,
 * a character style per byte, the paragraph's settings) that load_doc copies in. */
typedef struct { const uint8_t *text; const cst_t *st; int n; cst_t endst; uint8_t align, bullet, lsp; int16_t lind, first, rind; } wpr_para_t;
void *wpr_read(int kind, const uint8_t *data, uint32_t n, int *fmt);   /* kind 0 Rich Text, 1 zip; NULL: nothing read */
int wpr_count(void *h);
void wpr_para(void *h, int i, wpr_para_t *out);
void wpr_free(void *h);
/* the layout of cst_t and wpr_para_t for rust/src/wpdoc.rs, which rust_init checks at boot */
const uint32_t rust_wp_layout[6] = { sizeof(cst_t), offsetof(cst_t, hp), offsetof(cst_t, bg), sizeof(wpr_para_t), offsetof(wpr_para_t, endst), offsetof(wpr_para_t, lind) };
static void ins_text(wp_t *d, para_t *p, const char *t, int n, int st) {
    para_room(p, n);
    memcpy(p->t + p->n, t, n);
    for (int i = 0; i < n; i++) p->a[p->n + i] = st;
    p->n += n; p->dirty = true;
}
/* replaces the document's paragraphs with what a reader built */
static void load_doc(wp_t *d, void *h) {
    doc_clear(d);
    int np = wpr_count(h);
    for (int i = 0; i < np; i++) {
        wpr_para_t w; wpr_para(h, i, &w);
        para_t *p = doc_add(d, i, st_id(&w.endst));
        p->align = w.align; p->bullet = w.bullet; p->lsp = w.lsp; p->lind = w.lind; p->first = w.first; p->rind = w.rind;
        for (int k = 0; k < w.n; ) {
            int j = k; while (j < w.n && !memcmp(&w.st[j], &w.st[k], sizeof(cst_t))) j++;
            ins_text(d, p, (const char *)w.text + k, j - k, st_id(&w.st[k]));
            k = j;
        }
    }
}
/* Rich Text (a file, an undo snapshot or the rich clipboard) into an empty document */
static void rtf_load(wp_t *d, const char *s, uint32_t n) { void *h = wpr_read(0, (const uint8_t *)s, n, NULL); load_doc(d, h); wpr_free(h); }

/* ---------------------------------------------------------------- zip (writing .docx) */
static uint32_t crc_tab[256];
static uint32_t crc32_buf(const uint8_t *p, uint32_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
typedef struct { sb_t b; int n; sb_t cd; } zw_t;
static void zip_add(zw_t *z, const char *name, const char *data, uint32_t len) {
    uint32_t crc = crc32_buf((const uint8_t *)data, len), off = z->b.n, nl = strlen(name);
    uint8_t h[30] = { 0x50, 0x4b, 3, 4, 20, 0, 0, 0, 0, 0, 0, 0, 0x21, 0 };
    #define P32(a, v) do { (a)[0] = (v) & 255; (a)[1] = ((v) >> 8) & 255; (a)[2] = ((v) >> 16) & 255; (a)[3] = ((v) >> 24) & 255; } while (0)
    #define P16(a, v) do { (a)[0] = (v) & 255; (a)[1] = ((v) >> 8) & 255; } while (0)
    P32(h + 14, crc); P32(h + 18, len); P32(h + 22, len); P16(h + 26, nl); P16(h + 28, 0);
    sb_put(&z->b, (const char *)h, 30); sb_put(&z->b, name, nl); sb_put(&z->b, data, len);
    uint8_t c[46] = { 0x50, 0x4b, 1, 2, 20, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0x21, 0 };
    P32(c + 16, crc); P32(c + 20, len); P32(c + 24, len); P16(c + 28, nl); P16(c + 30, 0); P16(c + 32, 0); P16(c + 34, 0); P16(c + 36, 0); P32(c + 38, 0); P32(c + 42, off);
    sb_put(&z->cd, (const char *)c, 46); sb_put(&z->cd, name, nl);
    z->n++;
}
static void zip_end(zw_t *z) {
    uint32_t off = z->b.n;
    sb_put(&z->b, z->cd.b, z->cd.n);
    uint8_t e[22] = { 0x50, 0x4b, 5, 6 };
    P16(e + 8, z->n); P16(e + 10, z->n); P32(e + 12, z->cd.n); P32(e + 16, off); P16(e + 20, 0);
    sb_put(&z->b, (const char *)e, 22);
    kfree(z->cd.b);
    #undef P32
    #undef P16
}

/* ---------------------------------------------------------------- XML (writing .docx) */
static void xml_esc(sb_t *s, const char *t, int n) {
    for (int i = 0; i < n; i++) {
        char c = t[i];
        if (c == '&') sb_add(s, "&amp;"); else if (c == '<') sb_add(s, "&lt;"); else if (c == '>') sb_add(s, "&gt;"); else if (c == '"') sb_add(s, "&quot;");
        else sb_put(s, &c, 1);
    }
}
static char *docx_write(wp_t *d, uint32_t *len) {
    sb_t x = { 0 };
    sb_add(&x, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>");
    for (int pi = 0; pi < d->np; pi++) {
        para_t *p = &d->p[pi];
        sb_addf(&x, "<w:p><w:pPr><w:jc w:val=\"%s\"/><w:ind w:left=\"%d\" w:right=\"%d\" w:%s=\"%d\"/><w:spacing w:after=\"150\" w:line=\"%d\" w:lineRule=\"auto\"/></w:pPr>",
                p->align == 1 ? "center" : p->align == 2 ? "right" : p->align == 3 ? "both" : "left", (p->lind + (p->bullet ? DPI / 4 : 0)) * 15, p->rind * 15,
                p->first < 0 ? "hanging" : "firstLine", (p->first < 0 ? -p->first : p->first) * 15, p->lsp * 12);
        if (p->bullet) sb_add(&x, "<w:r><w:t xml:space=\"preserve\">\xe2\x80\xa2 </w:t></w:r>");
        for (int i = 0; i < p->n; ) {
            int st = p->a[i], j = i;
            while (j < p->n && p->a[j] == st && p->t[j] != '\t') j++;
            cst_t *c = &stab[st];
            sb_addf(&x, "<w:r><w:rPr><w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\"/>%s%s%s%s<w:sz w:val=\"%d\"/>", fam_name[c->fam], fam_name[c->fam],
                    c->b ? "<w:b/>" : "", c->i ? "<w:i/>" : "", c->u ? "<w:u w:val=\"single\"/>" : "", c->s ? "<w:strike/>" : "", c->hp);
            if ((c->fg & 0xFFFFFF) != 0) sb_addf(&x, "<w:color w:val=\"%06X\"/>", c->fg & 0xFFFFFF);
            if (c->bg) sb_addf(&x, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", c->bg & 0xFFFFFF);
            if (c->script) sb_addf(&x, "<w:vertAlign w:val=\"%s\"/>", c->script == 1 ? "subscript" : "superscript");
            sb_add(&x, "</w:rPr>");
            if (j == i) { sb_add(&x, "<w:tab/>"); j = i + 1; }
            else { sb_add(&x, "<w:t xml:space=\"preserve\">"); xml_esc(&x, p->t + i, j - i); sb_add(&x, "</w:t>"); }
            sb_add(&x, "</w:r>");
            i = j;
        }
        sb_add(&x, "</w:p>");
    }
    sb_add(&x, "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1800\" w:bottom=\"1440\" w:left=\"1800\"/></w:sectPr></w:body></w:document>");
    zw_t z; memset(&z, 0, sizeof z);
    const char *ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
    const char *rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>";
    zip_add(&z, "[Content_Types].xml", ct, strlen(ct));
    zip_add(&z, "_rels/.rels", rels, strlen(rels));
    zip_add(&z, "word/document.xml", x.b, x.n);
    zip_end(&z);
    kfree(x.b);
    *len = z.b.n;
    return z.b.b;
}

/* ---------------------------------------------------------------- files */
static const char *ext_of(const char *p) { const char *e = strrchr(p, '.'); return e ? e + 1 : ""; }
static void wp_title(wp_t *d) {
    char t[300]; snprintf(t, sizeof t, "%s%s - WordPad", d->modified ? "*" : "", d->path[0] ? path_basename(d->path) : "Document");
    strlcpy(d->w->title, t, sizeof d->w->title);
    wm_need_redraw = true;
}
static bool wp_load(wp_t *d, const char *path) {
    uint32_t n; uint8_t *data = vfs_read_file(path, &n);
    if (!data) return false;
    doc_clear(d);
    cst_t c = default_style(); d->typing = st_id(&c);
    doc_add(d, 0, d->typing);
    const char *e = ext_of(path);
    bool ok = true;
    if (n >= 5 && !memcmp(data, "{\\rtf", 5)) { rtf_load(d, (const char *)data, n); d->fmt = 0; }
    else if (n >= 4 && !memcmp(data, "PK\3\4", 4)) {
        int fmt = 0; void *h = wpr_read(1, data, n, &fmt);          /* .docx (1) or .odt (3) */
        if (h) { load_doc(d, h); wpr_free(h); d->fmt = fmt; }
        else ok = false;
    } else {                                              /* plain text: a paragraph per line */
        cst_t pc = default_style(); pc.fam = FAM_MONO; pc.hp = 20;
        int st = st_id(&c);
        (void)pc;
        para_t *p = &d->p[0];
        for (uint32_t i = 0; i < n; ) {
            uint32_t j = i; while (j < n && data[j] != '\n') j++;
            uint32_t k = j; if (k > i && data[k - 1] == '\r') k--;
            ins_text(d, p, (const char *)data + i, k - i, st);
            if (j < n) p = doc_add(d, d->np, st);
            i = j + 1;
        }
        d->fmt = strcasecmp(e, "txt") ? 0 : 2;
    }
    kfree(data);
    if (!ok) { doc_new(d); return false; }
    /* a document ends with an empty paragraph after a final paragraph mark in RTF: drop it */
    if (d->np > 1 && d->p[d->np - 1].n == 0 && d->fmt == 0) { para_free(&d->p[d->np - 1]); d->np--; }
    strlcpy(d->path, path, sizeof d->path);
    d->cur.p = d->cur.o = 0; d->anc = d->cur; d->scroll = 0;
    d->modified = false;
    for (int i = 0; i < d->nundo; i++) kfree(d->undo[i]);
    d->nundo = 0;
    all_dirty(d);
    recent_add(path);
    wp_title(d);
    return true;
}
static bool wp_save_to(wp_t *d, const char *path) {
    const char *e = ext_of(path);
    char *data; uint32_t len;
    if (!strcasecmp(e, "docx")) { data = docx_write(d, &len); d->fmt = 1; }
    else if (!strcasecmp(e, "txt")) {
        sb_t s = { 0 };
        for (int i = 0; i < d->np; i++) { sb_put(&s, d->p[i].t, d->p[i].n); sb_add(&s, "\r\n"); }
        data = s.b ? s.b : strdup(""); len = s.n; d->fmt = 2;
    } else {
        pos_t a = { 0, 0 }, b = { d->np - 1, d->p[d->np - 1].n };
        data = rtf_write(d, a, b); len = strlen(data); d->fmt = 0;
    }
    vnode_t *v = vfs_write_file(path, data, len);
    kfree(data);
    if (!v) { msgbox("WordPad", "The document could not be saved there.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    strlcpy(d->path, path, sizeof d->path);
    d->modified = false;
    recent_add(path);
    wp_title(d);
    return true;
}
static void save_as_cb(void *ctx, const char *path) {
    window_t *w = ctx; if (!wm_alive(w)) return;
    wp_t *d = w->data;
    char p[300]; strlcpy(p, path, sizeof p);
    const char *e = ext_of(p);
    if (strcasecmp(e, "rtf") && strcasecmp(e, "docx") && strcasecmp(e, "txt")) strlcat(p, ".rtf", sizeof p);
    if (wp_save_to(d, p) && d->closing) wm_destroy(w);
}
static void wp_save_as(wp_t *d) {
    char init[300];
    if (d->path[0] && d->fmt != 3) strlcpy(init, d->path, sizeof init);
    else snprintf(init, sizeof init, "%s\\Document.rtf", USER_DOCS);
    if (d->fmt == 3 && d->path[0]) { snprintf(init, sizeof init, "%s", d->path); char *x = strrchr(init, '.'); if (x) strcpy(x, ".rtf"); }
    file_dialog("Save As (.rtf, .docx or .txt)", init, true, NULL, save_as_cb, d->w);
}
static void wp_save(wp_t *d) { if (d->path[0] && d->fmt != 3) { if (wp_save_to(d, d->path) && d->closing) wm_destroy(d->w); } else wp_save_as(d); }
static void open_cb(void *ctx, const char *path) {
    window_t *w = ctx; if (!wm_alive(w)) return;
    wp_t *d = w->data;
    if (!wp_load(d, path)) msgbox("WordPad", "WordPad cannot open this file: it is not a Rich Text, Office Open XML, OpenDocument or text document.", MBI_WARNING, MBX_OK, NULL, NULL);
    w->dirty = true;
}

/* ---------------------------------------------------------------- editing */
static void push_undo(wp_t *d, int kind) {
    if (kind && kind == d->undo_kind) return;           /* typing continues one step */
    d->undo_kind = kind;
    pos_t a = { 0, 0 }, b = { d->np - 1, d->p[d->np - 1].n };
    char *snap = rtf_write(d, a, b);
    if (d->nundo == UNDO_MAX) { kfree(d->undo[0]); memmove(d->undo, d->undo + 1, (UNDO_MAX - 1) * sizeof(char *)); d->nundo--; }
    d->undo[d->nundo++] = snap;
    for (int i = 0; i < d->nredo; i++) kfree(d->redo[i]);
    d->nredo = 0;
}
static void restore(wp_t *d, const char *rtf) {
    pos_t keep = d->cur;
    doc_clear(d);
    cst_t c = default_style(); doc_add(d, 0, st_id(&c));
    rtf_load(d, rtf, strlen(rtf));
    if (d->np > 1 && d->p[d->np - 1].n == 0) { para_free(&d->p[d->np - 1]); d->np--; }
    if (keep.p >= d->np) keep.p = d->np - 1;
    if (keep.o > d->p[keep.p].n) keep.o = d->p[keep.p].n;
    d->cur = d->anc = keep;
    all_dirty(d);
}
static void wp_undo(wp_t *d) {
    if (!d->nundo) return;
    pos_t a = { 0, 0 }, b = { d->np - 1, d->p[d->np - 1].n };
    if (d->nredo < UNDO_MAX) d->redo[d->nredo++] = rtf_write(d, a, b);
    char *s = d->undo[--d->nundo];
    restore(d, s); kfree(s);
    d->undo_kind = 0; d->modified = true;
}
static void wp_redo(wp_t *d) {
    if (!d->nredo) return;
    pos_t a = { 0, 0 }, b = { d->np - 1, d->p[d->np - 1].n };
    if (d->nundo < UNDO_MAX) d->undo[d->nundo++] = rtf_write(d, a, b);
    char *s = d->redo[--d->nredo];
    restore(d, s); kfree(s);
    d->undo_kind = 0; d->modified = true;
}
static void changed(wp_t *d) { if (!d->modified) { d->modified = true; wp_title(d); } d->w->dirty = true; }
static void delete_range(wp_t *d, pos_t a, pos_t b) {
    if (pcmp(a, b) >= 0) return;
    para_t *pa = &d->p[a.p];
    if (a.p == b.p) {
        memmove(pa->t + a.o, pa->t + b.o, pa->n - b.o);
        memmove(pa->a + a.o, pa->a + b.o, (pa->n - b.o) * 2);
        pa->n -= b.o - a.o;
    } else {
        para_t *pb = &d->p[b.p];
        int tail = pb->n - b.o;
        pa->n = a.o;
        para_room(pa, tail);
        memcpy(pa->t + pa->n, pb->t + b.o, tail);
        memcpy(pa->a + pa->n, pb->a + b.o, tail * 2);
        pa->n += tail;
        for (int i = a.p + 1; i <= b.p; i++) para_free(&d->p[i]);
        memmove(&d->p[a.p + 1], &d->p[b.p + 1], (d->np - b.p - 1) * sizeof(para_t));
        d->np -= b.p - a.p;
    }
    pa->dirty = true;
    d->cur = d->anc = a;
}
static void delete_sel(wp_t *d) { if (has_sel(d)) { pos_t a, b; sel_range(d, &a, &b); delete_range(d, a, b); } }
static void insert_text(wp_t *d, const char *t, int n, int st) {
    delete_sel(d);
    for (int i = 0; i < n; ) {
        int j = i; while (j < n && t[j] != '\n' && t[j] != '\r') j++;
        if (j > i) {
            para_t *p = &d->p[d->cur.p];
            para_room(p, j - i);
            memmove(p->t + d->cur.o + (j - i), p->t + d->cur.o, p->n - d->cur.o);
            memmove(p->a + d->cur.o + (j - i), p->a + d->cur.o, (p->n - d->cur.o) * 2);
            memcpy(p->t + d->cur.o, t + i, j - i);
            for (int k = 0; k < j - i; k++) p->a[d->cur.o + k] = st;
            p->n += j - i; d->cur.o += j - i; p->dirty = true;
        }
        if (j < n) {                                      /* a new paragraph, with this one's format */
            if (t[j] == '\r' && j + 1 < n && t[j + 1] == '\n') j++;
            para_t *p = &d->p[d->cur.p];
            int tail = p->n - d->cur.o;
            para_t *q = doc_add(d, d->cur.p + 1, st);
            p = &d->p[d->cur.p];
            q->align = p->align; q->bullet = p->bullet; q->lsp = p->lsp; q->lind = p->lind; q->first = p->first; q->rind = p->rind;
            para_room(q, tail);
            memcpy(q->t, p->t + d->cur.o, tail); memcpy(q->a, p->a + d->cur.o, tail * 2); q->n = tail;
            p->n = d->cur.o; p->endst = st; p->dirty = true;
            d->cur.p++; d->cur.o = 0;
        }
        i = j + 1;
    }
    d->anc = d->cur;
}
/* apply a change to every character in the selection, or to the typing style */
typedef void (*stfn_t)(cst_t *c, int v);
static void apply_style(wp_t *d, stfn_t fn, int v) {
    if (!has_sel(d)) { cst_t c = stab[d->typing]; fn(&c, v); d->typing = st_id(&c); d->w->dirty = true; return; }
    push_undo(d, 0);
    pos_t a, b; sel_range(d, &a, &b);
    for (int pi = a.p; pi <= b.p; pi++) {
        para_t *p = &d->p[pi];
        int o0 = pi == a.p ? a.o : 0, o1 = pi == b.p ? b.o : p->n;
        for (int i = o0; i < o1; i++) { cst_t c = stab[p->a[i]]; fn(&c, v); p->a[i] = st_id(&c); }
        if (o0 == o1) { cst_t c = stab[p->endst]; fn(&c, v); p->endst = st_id(&c); }
        p->dirty = true;
    }
    cst_t c = stab[d->typing]; fn(&c, v); d->typing = st_id(&c);
    changed(d);
}
static bool sel_all_have(wp_t *d, int which) {        /* for a toggle: is the attribute on across the selection? */
    if (!has_sel(d)) { cst_t *c = &stab[d->typing]; return which == 0 ? c->b : which == 1 ? c->i : which == 2 ? c->u : c->s; }
    pos_t a, b; sel_range(d, &a, &b);
    for (int pi = a.p; pi <= b.p; pi++) {
        para_t *p = &d->p[pi];
        int o0 = pi == a.p ? a.o : 0, o1 = pi == b.p ? b.o : p->n;
        for (int i = o0; i < o1; i++) { cst_t *c = &stab[p->a[i]]; int on = which == 0 ? c->b : which == 1 ? c->i : which == 2 ? c->u : c->s; if (!on) return false; }
    }
    return true;
}
static void f_bold(cst_t *c, int v) { c->b = v; }
static void f_italic(cst_t *c, int v) { c->i = v; }
static void f_under(cst_t *c, int v) { c->u = v; }
static void f_strike(cst_t *c, int v) { c->s = v; }
static void f_script(cst_t *c, int v) { c->script = c->script == v ? 0 : v; }
static void f_size(cst_t *c, int v) { c->hp = v; }
static void f_grow(cst_t *c, int v) { int s = c->hp + v * 2; c->hp = s < 2 ? 2 : s > 288 ? 288 : s; }
static void f_family(cst_t *c, int v) { c->fam = v; }
static void f_color(cst_t *c, int v) { c->fg = 0xFF000000u | (uint32_t)v; }
static void f_highlight(cst_t *c, int v) { c->bg = v < 0 ? 0 : 0xFF000000u | (uint32_t)v; }
static void toggle(wp_t *d, stfn_t fn, int which) { apply_style(d, fn, !sel_all_have(d, which)); }
/* paragraph formats of every paragraph the selection touches */
static void apply_para(wp_t *d, int what, int v) {
    push_undo(d, 0);
    pos_t a, b; sel_range(d, &a, &b);
    for (int pi = a.p; pi <= b.p; pi++) {
        para_t *p = &d->p[pi];
        if (what == 0) p->align = v;
        else if (what == 1) p->bullet = v < 0 ? !d->p[a.p].bullet : v;
        else if (what == 2) { int l = p->lind + v; p->lind = l < 0 ? 0 : l > DPI * 5 ? DPI * 5 : l; }
        else if (what == 3) p->lsp = v;
        p->dirty = true;
    }
    changed(d);
}
static char *selection_text(wp_t *d) {
    pos_t a, b; sel_range(d, &a, &b);
    sb_t s = { 0 };
    for (int pi = a.p; pi <= b.p; pi++) {
        para_t *p = &d->p[pi];
        int o0 = pi == a.p ? a.o : 0, o1 = pi == b.p ? b.o : p->n;
        sb_put(&s, p->t + o0, o1 - o0);
        if (pi < b.p) sb_add(&s, "\r\n");
    }
    return s.b ? s.b : strdup("");
}
/* the rich clipboard: the RTF of the last copy, used when the system clipboard still holds its text */
static char *rich_clip, *rich_clip_plain;
static void wp_copy(wp_t *d, bool cut) {
    if (!has_sel(d)) return;
    pos_t a, b; sel_range(d, &a, &b);
    kfree(rich_clip); kfree(rich_clip_plain);
    rich_clip = rtf_write(d, a, b);
    rich_clip_plain = selection_text(d);
    clip_set_text(rich_clip_plain);
    if (cut) { push_undo(d, 0); delete_sel(d); changed(d); }
}
static void wp_paste(wp_t *d) {
    const char *t = clip_text();
    if (!t) return;
    push_undo(d, 0);
    if (rich_clip && rich_clip_plain && !strcmp(t, rich_clip_plain)) {
        /* parse the copy into a scratch document, then splice its paragraphs in */
        wp_t tmp; memset(&tmp, 0, sizeof tmp); tmp.zoom = 100;
        cst_t c = default_style(); doc_add(&tmp, 0, st_id(&c));
        rtf_load(&tmp, rich_clip, strlen(rich_clip));
        if (tmp.np > 1 && tmp.p[tmp.np - 1].n == 0) { para_free(&tmp.p[tmp.np - 1]); tmp.np--; }
        delete_sel(d);
        for (int pi = 0; pi < tmp.np; pi++) {
            para_t *src = &tmp.p[pi];
            /* a pasted paragraph keeps its own format when it starts a paragraph here */
            para_t *dst = &d->p[d->cur.p];
            if (d->cur.o == 0 && (pi > 0 || dst->n == 0)) {
                dst->align = src->align; dst->bullet = src->bullet; dst->lsp = src->lsp; dst->lind = src->lind; dst->first = src->first; dst->rind = src->rind; dst->dirty = true;
            }
            for (int i = 0; i < src->n; ) { int j = i; while (j < src->n && src->a[j] == src->a[i]) j++; insert_text(d, src->t + i, j - i, src->a[i]); i = j; }
            if (pi < tmp.np - 1) insert_text(d, "\n", 1, d->typing);
        }
        doc_clear(&tmp); kfree(tmp.p);
    } else insert_text(d, t, strlen(t), d->typing);
    changed(d);
}

/* ---------------------------------------------------------------- the ribbon */
static const uint32_t C_TABROW = 0xFFDFE9F5, C_RIB_A = 0xFFFDFDFE, C_RIB_B = 0xFFE3EAF3, C_RIB_LINE = 0xFFB6BCC6, C_GROUP_TXT = 0xFF4A5566,
                      C_HOT_A = 0xFFFFF8DB, C_HOT_B = 0xFFFFE493, C_HOT_LINE = 0xFFE3C26A, C_ON_A = 0xFFFDE3A7, C_ON_B = 0xFFF8C76A;
static bool rbutton(window_t *w, rect_t r, bool on, bool enabled) {
    canvas_t *c = w->surf;
    bool hot = enabled && ui_hover(w, r);
    if (on || hot) {
        gradient_round_rect(c, r.x, r.y, r.w, r.h, 2, on ? C_ON_A : C_HOT_A, on ? C_ON_B : C_HOT_B);
        draw_round_rect(c, r.x, r.y, r.w, r.h, 2, on ? 0xFFC29B4A : C_HOT_LINE);
    }
    return enabled && ui_click_area(w, r);
}
/* small glyphs for the ribbon, drawn (16x16 around x, y) */
enum { G_PASTE, G_CUT, G_COPY, G_BOLD, G_ITALIC, G_UNDER, G_STRIKE, G_SUB, G_SUPER, G_GROW, G_SHRINK, G_COLOR, G_HILITE, G_OUTDENT, G_INDENT,
       G_BULLETS, G_SPACING, G_LEFT, G_CENTER, G_RIGHT, G_JUSTIFY, G_DATE, G_FIND, G_REPLACE, G_SELALL, G_ZOOMIN, G_ZOOMOUT, G_ZOOM100, G_UNDO, G_REDO };
static void lines(canvas_t *c, int x, int y, int n, const int *w, int align, uint32_t col) {
    for (int i = 0; i < n; i++) { int lx = align == 1 ? x + (14 - w[i]) / 2 : align == 2 ? x + 14 - w[i] : x; fill_rect(c, lx, y + 2 + i * 3, w[i], 1, col); }
}
static void glyph(canvas_t *c, int g, int x, int y, uint32_t extra) {
    const uint32_t K = 0xFF2B3A4E, B = 0xFF3C6FB8;
    switch (g) {
    case G_PASTE: fill_rect(c, x + 2, y + 3, 11, 13, 0xFFC8A060); draw_rect(c, x + 2, y + 3, 11, 13, 0xFF7A5A20); fill_rect(c, x + 5, y + 1, 5, 4, 0xFF9AA4B0);
                  fill_rect(c, x + 7, y + 7, 9, 9, WHITE); draw_rect(c, x + 7, y + 7, 9, 9, 0xFF6A7A90); fill_rect(c, x + 9, y + 10, 5, 1, B); fill_rect(c, x + 9, y + 12, 5, 1, B); break;
    case G_CUT: draw_circle(c, x + 4, y + 12, 3, 0xFFB03030); draw_circle(c, x + 11, y + 12, 3, 0xFFB03030); draw_thick_line(c, x + 5, y + 9, x + 11, y + 1, 1, 0xFF505A66); draw_thick_line(c, x + 10, y + 9, x + 4, y + 1, 1, 0xFF505A66); break;
    case G_COPY: fill_rect(c, x + 1, y + 1, 9, 11, WHITE); draw_rect(c, x + 1, y + 1, 9, 11, 0xFF6A7A90); fill_rect(c, x + 6, y + 5, 9, 11, WHITE); draw_rect(c, x + 6, y + 5, 9, 11, 0xFF6A7A90);
                 for (int i = 0; i < 3; i++) fill_rect(c, x + 8, y + 8 + i * 2, 5, 1, B); break;
    case G_BOLD: draw_text(c, font_sized(FAM_SANS, 15, true, false), x + 3, y, "B", K); break;
    case G_ITALIC: draw_text(c, font_sized(FAM_SERIF, 16, false, true), x + 4, y, "I", K); break;
    case G_UNDER: draw_text(c, font_sized(FAM_SANS, 14, false, false), x + 3, y - 1, "U", K); fill_rect(c, x + 2, y + 14, 11, 1, K); break;
    case G_STRIKE: draw_text(c, font_sized(FAM_SANS, 11, false, false), x, y + 2, "abc", K); fill_rect(c, x, y + 9, 16, 1, 0xFFC03030); break;
    case G_SUB: case G_SUPER: draw_text(c, font_sized(FAM_SANS, 13, false, false), x + 1, y, "x", K); draw_text(c, font_sized(FAM_SANS, 9, false, false), x + 9, g == G_SUB ? y + 7 : y - 2, "2", B); break;
    case G_GROW: case G_SHRINK: draw_text(c, font_sized(FAM_SANS, g == G_GROW ? 15 : 11, false, false), x + 1, g == G_GROW ? y : y + 4, "A", K);
        { int ax = x + 12, ay = y + 3; if (g == G_GROW) { fill_rect(c, ax, ay + 2, 5, 1, B); fill_rect(c, ax + 1, ay + 1, 3, 1, B); fill_rect(c, ax + 2, ay, 1, 1, B); }
          else { fill_rect(c, ax, ay, 5, 1, B); fill_rect(c, ax + 1, ay + 1, 3, 1, B); fill_rect(c, ax + 2, ay + 2, 1, 1, B); } } break;
    case G_COLOR: draw_text(c, font_sized(FAM_SANS, 13, false, false), x + 3, y - 1, "A", K); fill_rect(c, x + 1, y + 12, 14, 4, extra); break;
    case G_HILITE: draw_thick_line(c, x + 4, y + 10, x + 11, y + 2, 3, 0xFF6A7A90); draw_thick_line(c, x + 3, y + 11, x + 5, y + 9, 2, 0xFF303030); fill_rect(c, x + 1, y + 12, 14, 4, extra); break;
    case G_OUTDENT: case G_INDENT: { int w4[4] = { 14, 8, 8, 14 }; for (int i = 0; i < 4; i++) fill_rect(c, x + (i == 1 || i == 2 ? 6 : 0) + 2, y + 2 + i * 3, w4[i] - 2, 1, K);
        if (g == G_INDENT) { fill_rect(c, x + 1, y + 5, 3, 3, B); } else { fill_rect(c, x + 1, y + 6, 4, 1, B); fill_rect(c, x + 1, y + 5, 1, 3, B); } break; }
    case G_BULLETS: for (int i = 0; i < 3; i++) { fill_circle(c, x + 3, y + 3 + i * 5, 1, B); fill_rect(c, x + 7, y + 3 + i * 5, 8, 1, K); } break;
    case G_SPACING: for (int i = 0; i < 4; i++) fill_rect(c, x + 7, y + 2 + i * 4, 8, 1, K); fill_rect(c, x + 3, y + 2, 1, 12, B); fill_rect(c, x + 2, y + 3, 3, 1, B); fill_rect(c, x + 2, y + 12, 3, 1, B); break;
    case G_LEFT: { int w5[5] = { 14, 9, 14, 9, 14 }; lines(c, x + 1, y, 5, w5, 0, K); break; }
    case G_CENTER: { int w5[5] = { 14, 9, 14, 9, 14 }; lines(c, x + 1, y, 5, w5, 1, K); break; }
    case G_RIGHT: { int w5[5] = { 14, 9, 14, 9, 14 }; lines(c, x + 1, y, 5, w5, 2, K); break; }
    case G_JUSTIFY: { int w5[5] = { 14, 14, 14, 14, 14 }; lines(c, x + 1, y, 5, w5, 0, K); break; }
    case G_DATE: draw_icon(c, ICON_CP_DATETIME, 16, x, y); break;
    case G_FIND: draw_circle(c, x + 6, y + 6, 4, K); draw_thick_line(c, x + 9, y + 9, x + 14, y + 14, 2, K); break;
    case G_REPLACE: draw_text(c, font_sized(FAM_SANS, 9, false, false), x, y, "ab", K); draw_text(c, font_sized(FAM_SANS, 9, false, false), x + 6, y + 7, "ac", B); break;
    case G_SELALL: draw_rect(c, x + 1, y + 1, 14, 14, B); for (int i = 0; i < 3; i++) fill_rect(c, x + 4, y + 4 + i * 3, 8, 1, K); break;
    case G_ZOOMIN: case G_ZOOMOUT: case G_ZOOM100: draw_circle(c, x + 6, y + 6, 5, K); draw_thick_line(c, x + 10, y + 10, x + 15, y + 15, 2, K);
        if (g != G_ZOOM100) fill_rect(c, x + 4, y + 6, 5, 1, K);
        if (g == G_ZOOMIN) fill_rect(c, x + 6, y + 4, 1, 5, K);
        if (g == G_ZOOM100) draw_text(c, font_sized(FAM_SANS, 7, true, false), x + 2, y + 2, "1:1", K); break;
    case G_UNDO: case G_REDO: { int d = g == G_UNDO ? 1 : -1, cx = x + 8;
        draw_thick_line(c, cx - 5 * d, y + 6, cx + 4 * d, y + 6, 2, B); draw_thick_line(c, cx + 4 * d, y + 6, cx + 4 * d, y + 12, 2, B);
        draw_thick_line(c, cx - 5 * d, y + 6, cx - 2 * d, y + 3, 2, B); draw_thick_line(c, cx - 5 * d, y + 6, cx - 2 * d, y + 9, 2, B); break; }
    }
}
static bool sbtn(window_t *w, int x, int y, int g, const char *label, bool on, uint32_t extra) {   /* small: 16 px glyph, optional label */
    int wd = 22 + (label ? text_width(FONT_UI, label) + 4 : 0);
    bool click = rbutton(w, R(x, y, wd, 22), on, true);
    glyph(w->surf, g, x + 3, y + 3, extra);
    if (label) draw_text(w->surf, FONT_UI, x + 22, y + 4, label, 0xFF1E2833);
    return click;
}
static int group_end(canvas_t *c, int x0, int x1, int top, const char *label) {
    int lw = text_width(FONT_UI, label);
    if (x1 - x0 < lw + 12) x1 = x0 + lw + 12;
    draw_text(c, FONT_UI, x0 + (x1 - x0 - lw) / 2, top + RIB_H - 18, label, C_GROUP_TXT);
    vline(c, x1 + 3, top + 4, RIB_H - 8, 0xFFC5CDD8); vline(c, x1 + 4, top + 4, RIB_H - 8, WHITE);
    return x1 + 9;
}
static void combo(window_t *w, rect_t r, const char *text) {
    canvas_t *c = w->surf;
    bool hot = ui_hover(w, r);
    fill_rect(c, r.x, r.y, r.w, r.h, WHITE);
    draw_rect(c, r.x, r.y, r.w, r.h, hot ? 0xFF7DA2CE : 0xFFABBAD0);
    draw_text_ellipsis(c, FONT_UI, r.x + 4, r.y + 4, r.w - 22, text, 0xFF1E2833);
    fill_rect(c, r.x + r.w - 16, r.y + 1, 15, r.h - 2, hot ? 0xFFEAF2FC : 0xFFF2F5F9);
    for (int i = 0; i < 4; i++) fill_rect(c, r.x + r.w - 12 + i, r.y + r.h / 2 - 2 + i, 7 - 2 * i, 1, 0xFF1E2833);
}
/* drop-downs */
enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_ABOUT, M_EXIT, M_FAM0 = 20, M_SIZE0 = 40, M_COL0 = 100, M_HIL0 = 130, M_SP0 = 160, M_DATE0 = 180 };
static window_t *menu_win;
static const uint32_t text_colors[] = { 0x000000, 0x7F7F7F, 0xC00000, 0xFF0000, 0xFFC000, 0xFFFF00, 0x92D050, 0x00B050, 0x00B0F0, 0x0070C0, 0x002060, 0x7030A0 };
static const char *text_color_names[] = { "Black", "Gray", "Dark red", "Red", "Orange", "Yellow", "Light green", "Green", "Light blue", "Blue", "Dark blue", "Purple" };
static const uint32_t hl_colors[] = { 0xFFFF00, 0x00FF00, 0x00FFFF, 0xFF00FF, 0x0000FF, 0xFF0000, 0x000080, 0x008080, 0x008000, 0x800080, 0xC0C0C0 };
static const char *hl_names[] = { "Yellow", "Bright green", "Turquoise", "Pink", "Blue", "Red", "Dark blue", "Teal", "Green", "Violet", "Gray 25%" };
static const int sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
static void date_insert(wp_t *d, int which);
static void wp_cmd(window_t *w, int id);
static void menu_cb(void *ctx, int id) { window_t *w = ctx; if (w == menu_win && wm_alive(w) && id > 0) wp_cmd(w, id); }
static void drop(window_t *w, rect_t r, const menu_item_t *items, int n) {
    rect_t cr = wm_client_rect(w);
    menu_win = w;
    popup_menu(cr.x + r.x, cr.y + r.y + r.h, items, n, menu_cb, w);
}
static void find_next(wp_t *d);
static void find_cb(void *ctx, const char *text) {
    window_t *w = ctx; if (!wm_alive(w) || !text) return;
    wp_t *d = w->data; strlcpy(d->find, text, sizeof d->find);
    find_next(d);
}
static void replace_with_cb(void *ctx, const char *text) {
    window_t *w = ctx; if (!wm_alive(w) || !text) return;
    wp_t *d = w->data; strlcpy(d->repl, text, sizeof d->repl);
    if (!d->find[0]) return;
    push_undo(d, 0);
    int count = 0;
    d->cur.p = d->cur.o = 0; d->anc = d->cur;
    for (int pi = 0; pi < d->np; pi++) {
        para_t *p = &d->p[pi];
        int fl = strlen(d->find);
        for (int i = 0; i + fl <= p->n; ) {
            if (!strncasecmp(p->t + i, d->find, fl)) {
                int st = p->a[i];
                pos_t a = { pi, i }, b = { pi, i + fl };
                delete_range(d, a, b);
                d->cur = a; insert_text(d, d->repl, strlen(d->repl), st);
                p = &d->p[pi];
                i += strlen(d->repl); count++;
            } else i++;
        }
    }
    char m[120]; snprintf(m, sizeof m, "WordPad replaced %d occurrence%s.", count, count == 1 ? "" : "s");
    msgbox("Replace", m, MBI_INFO, MBX_OK, NULL, NULL);
    changed(d);
}
static void replace_find_cb(void *ctx, const char *text) {
    window_t *w = ctx; if (!wm_alive(w) || !text || !*text) return;
    wp_t *d = w->data; strlcpy(d->find, text, sizeof d->find);
    input_box("Replace", "Replace with:", d->repl, replace_with_cb, w);
}
static void find_next(wp_t *d) {
    if (!d->find[0]) return;
    int fl = strlen(d->find);
    pos_t s = d->cur;
    for (int pass = 0; pass < 2; pass++) {
        for (int pi = pass ? 0 : s.p; pi < d->np; pi++) {
            para_t *p = &d->p[pi];
            for (int i = (pass == 0 && pi == s.p) ? s.o : 0; i + fl <= p->n; i++)
                if (!strncasecmp(p->t + i, d->find, fl)) { d->anc.p = pi; d->anc.o = i; d->cur.p = pi; d->cur.o = i + fl; d->w->dirty = true; return; }
        }
    }
    msgbox("Find", "WordPad has finished searching the document.", MBI_INFO, MBX_OK, NULL, NULL);
}
static void ensure_caret_visible(wp_t *d);

static void ribbon(window_t *w, wp_t *d) {
    canvas_t *c = w->surf;
    /* the tab row: the File button and the tabs */
    fill_rect(c, 0, 0, c->w, TABS_H, C_TABROW);
    rect_t fb = R(4, 2, 56, TABS_H - 2);
    bool fhot = ui_hover(w, fb);
    gradient_round_rect(c, fb.x, fb.y, fb.w, fb.h + 3, 3, fhot ? 0xFF4F86D0 : 0xFF3B73C2, fhot ? 0xFF2B5FA8 : 0xFF1E4F94);
    draw_icon(c, ICON_FILE, 16, fb.x + 8, fb.y + 4);
    for (int i = 0; i < 4; i++) fill_rect(c, fb.x + 32 + i, fb.y + 10 + i, 7 - 2 * i, 1, WHITE);
    if (ui_click_area(w, fb)) {
        static const menu_item_t fm[] = { { "New\tCtrl+N", M_NEW, 0 }, { "Open...\tCtrl+O", M_OPEN, 0 }, { "Save\tCtrl+S", M_SAVE, 0 },
                                          { "Save as...", M_SAVEAS, 0 }, { "-", 0, 0 }, { "About WordPad", M_ABOUT, 0 }, { "-", 0, 0 }, { "Exit", M_EXIT, 0 } };
        drop(w, R(fb.x, fb.y, fb.w, fb.h), fm, ARRAY_LEN(fm));
    }
    static const char *tabs[2] = { "Home", "View" };
    int tx = fb.x + fb.w + 6;
    for (int i = 0; i < 2; i++) {
        int tw = text_width(FONT_UI, tabs[i]) + 22;
        rect_t r = R(tx, 3, tw, TABS_H - 3);
        if (i == d->tab) { gradient_v(c, r.x, r.y, r.w, r.h + 1, WHITE, C_RIB_A); draw_rect(c, r.x, r.y, r.w, r.h + 1, C_RIB_LINE); fill_rect(c, r.x + 1, r.y + r.h, r.w - 2, 1, C_RIB_A); }
        else if (ui_hover(w, r)) { fill_rect(c, r.x, r.y, r.w, r.h, 0xFFEAF1FA); draw_rect(c, r.x, r.y, r.w, r.h + 1, 0xFFC6D4E6); }
        draw_text(c, FONT_UI, r.x + 11, r.y + 5, tabs[i], 0xFF1E2833);
        if (ui_click_area(w, r)) { d->tab = i; w->dirty = true; }
        tx += tw + 2;
    }
    /* undo and redo, at the right of the tab row (the quick access toolbar's place) */
    if (sbtn(w, c->w - 52, 1, G_UNDO, NULL, false, 0)) wp_cmd(w, 'z');
    if (sbtn(w, c->w - 28, 1, G_REDO, NULL, false, 0)) wp_cmd(w, 'y');
    /* the body */
    int top = TABS_H;
    gradient_v(c, 0, top, c->w, RIB_H, C_RIB_A, C_RIB_B);
    hline(c, 0, top, c->w, C_RIB_LINE);
    hline(c, 0, top + RIB_H - 1, c->w, C_RIB_LINE);
    int x = 6;
    if (d->tab == 0) {
        /* Clipboard */
        rect_t pr = R(x, top + 4, 44, 66);
        if (rbutton(w, pr, false, clip_text() != NULL)) wp_cmd(w, 'v');
        draw_icon(c, ICON_NOTEPAD, 32, pr.x + 6, pr.y + 4);
        glyph(c, G_PASTE, pr.x + 14, pr.y + 12, 0);
        draw_text_centered(c, FONT_UI, R(pr.x, pr.y + 44, pr.w, 16), "Paste", clip_text() ? 0xFF1E2833 : 0xFF98A2B0);
        if (sbtn(w, x + 48, top + 6, G_CUT, "Cut", false, 0)) wp_cmd(w, 'x');
        if (sbtn(w, x + 48, top + 30, G_COPY, "Copy", false, 0)) wp_cmd(w, 'c');
        x = group_end(c, x, x + 48 + 66, top, "Clipboard");
        /* Font */
        int st = has_sel(d) ? d->p[MIN(d->cur.p, d->anc.p)].a[0] : d->typing;
        if (has_sel(d)) { pos_t a, b; sel_range(d, &a, &b); para_t *p = &d->p[a.p]; st = a.o < p->n ? p->a[a.o] : p->endst; }
        cst_t *cs = &stab[st];
        rect_t fr = R(x, top + 6, 150, 22);
        combo(w, fr, fam_name[cs->fam]);
        if (ui_click_area(w, fr)) {
            static menu_item_t fmn[3];
            for (int i = 0; i < 3; i++) { fmn[i].label = fam_name[i]; fmn[i].id = M_FAM0 + i; fmn[i].flags = i == cs->fam ? MI_CHECKED : 0; }
            drop(w, fr, fmn, 3);
        }
        rect_t sr = R(x + 154, top + 6, 48, 22);
        char sz[16]; snprintf(sz, sizeof sz, cs->hp % 2 ? "%d.5" : "%d", cs->hp / 2);
        combo(w, sr, sz);
        if (ui_click_area(w, sr)) {
            static menu_item_t smn[ARRAY_LEN(sizes)]; static char lab[ARRAY_LEN(sizes)][8];
            for (unsigned i = 0; i < ARRAY_LEN(sizes); i++) { snprintf(lab[i], 8, "%d", sizes[i]); smn[i].label = lab[i]; smn[i].id = M_SIZE0 + i; smn[i].flags = sizes[i] * 2 == cs->hp ? MI_CHECKED : 0; }
            drop(w, sr, smn, ARRAY_LEN(sizes));
        }
        if (sbtn(w, x + 206, top + 6, G_GROW, NULL, false, 0)) apply_style(d, f_grow, 2);
        if (sbtn(w, x + 230, top + 6, G_SHRINK, NULL, false, 0)) apply_style(d, f_grow, -2);
        int fy = top + 32;
        if (sbtn(w, x, fy, G_BOLD, NULL, cs->b, 0)) toggle(d, f_bold, 0);
        if (sbtn(w, x + 24, fy, G_ITALIC, NULL, cs->i, 0)) toggle(d, f_italic, 1);
        if (sbtn(w, x + 48, fy, G_UNDER, NULL, cs->u, 0)) toggle(d, f_under, 2);
        if (sbtn(w, x + 72, fy, G_STRIKE, NULL, cs->s, 0)) toggle(d, f_strike, 3);
        if (sbtn(w, x + 100, fy, G_SUB, NULL, cs->script == 1, 0)) apply_style(d, f_script, 1);
        if (sbtn(w, x + 124, fy, G_SUPER, NULL, cs->script == 2, 0)) apply_style(d, f_script, 2);
        static uint32_t last_hl = 0xFFFF00, last_col = 0xFF0000;
        rect_t hr = R(x + 152, fy, 34, 22);
        if (rbutton(w, R(hr.x, hr.y, 22, 22), false, true)) apply_style(d, f_highlight, (int)last_hl);
        glyph(c, G_HILITE, hr.x + 3, hr.y + 3, 0xFF000000 | last_hl);
        if (rbutton(w, R(hr.x + 22, hr.y, 12, 22), false, true)) {
            static menu_item_t hm[ARRAY_LEN(hl_colors) + 1];
            hm[0].label = "No color"; hm[0].id = M_HIL0; hm[0].flags = 0;
            for (unsigned i = 0; i < ARRAY_LEN(hl_colors); i++) { hm[i + 1].label = hl_names[i]; hm[i + 1].id = M_HIL0 + 1 + i; hm[i + 1].flags = 0; }
            drop(w, hr, hm, ARRAY_LEN(hl_colors) + 1);
        }
        for (int i = 0; i < 3; i++) fill_rect(c, hr.x + 25 + i, hr.y + 10 + i, 5 - 2 * i, 1, 0xFF1E2833);
        rect_t cr = R(x + 190, fy, 34, 22);
        if (rbutton(w, R(cr.x, cr.y, 22, 22), false, true)) apply_style(d, f_color, (int)last_col);
        glyph(c, G_COLOR, cr.x + 3, cr.y + 3, 0xFF000000 | last_col);
        if (rbutton(w, R(cr.x + 22, cr.y, 12, 22), false, true)) {
            static menu_item_t cm[ARRAY_LEN(text_colors)];
            for (unsigned i = 0; i < ARRAY_LEN(text_colors); i++) { cm[i].label = text_color_names[i]; cm[i].id = M_COL0 + i; cm[i].flags = 0; }
            drop(w, cr, cm, ARRAY_LEN(text_colors));
        }
        for (int i = 0; i < 3; i++) fill_rect(c, cr.x + 25 + i, cr.y + 10 + i, 5 - 2 * i, 1, 0xFF1E2833);
        (void)last_hl; (void)last_col;
        x = group_end(c, x, x + 254, top, "Font");
        /* Paragraph */
        para_t *pp = &d->p[d->cur.p];
        if (sbtn(w, x, top + 6, G_OUTDENT, NULL, false, 0)) apply_para(d, 2, -DPI / 2);
        if (sbtn(w, x + 24, top + 6, G_INDENT, NULL, false, 0)) apply_para(d, 2, DPI / 2);
        if (sbtn(w, x + 48, top + 6, G_BULLETS, NULL, pp->bullet, 0)) apply_para(d, 1, -1);
        rect_t lsr = R(x + 72, top + 6, 34, 22);
        if (rbutton(w, lsr, false, true)) {
            static const menu_item_t lm[] = { { "1.0", M_SP0 + 20, 0 }, { "1.15", M_SP0 + 23, 0 }, { "1.5", M_SP0 + 30, 0 }, { "2.0", M_SP0 + 40, 0 } };
            drop(w, lsr, lm, ARRAY_LEN(lm));
        }
        glyph(c, G_SPACING, lsr.x + 3, lsr.y + 3, 0);
        for (int i = 0; i < 3; i++) fill_rect(c, lsr.x + 25 + i, lsr.y + 10 + i, 5 - 2 * i, 1, 0xFF1E2833);
        for (int i = 0; i < 4; i++) if (sbtn(w, x + i * 24, top + 32, G_LEFT + i, NULL, pp->align == i, 0)) apply_para(d, 0, i);
        x = group_end(c, x, x + 108, top, "Paragraph");
        /* Insert */
        rect_t dr = R(x, top + 4, 54, 66);
        if (rbutton(w, dr, false, true)) {
            static menu_item_t dm[5]; static char dl[5][48];
            rtc_time_t t; rtc_now(&t);
            static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
            snprintf(dl[0], 48, "%d/%d/%d", t.month, t.day, t.year);
            snprintf(dl[1], 48, "%s %d, %d", mon[(t.month + 11) % 12], t.day, t.year);
            snprintf(dl[2], 48, "%d-%02d-%02d", t.year, t.month, t.day);
            snprintf(dl[3], 48, "%d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour >= 12 ? "PM" : "AM");
            snprintf(dl[4], 48, "%d:%02d", t.hour, t.minute);
            for (int i = 0; i < 5; i++) { dm[i].label = dl[i]; dm[i].id = M_DATE0 + i; dm[i].flags = 0; }
            drop(w, dr, dm, 5);
        }
        draw_icon(c, ICON_CP_DATETIME, 32, dr.x + 11, dr.y + 4);
        draw_text_centered(c, FONT_UI, R(dr.x, dr.y + 38, dr.w, 14), "Date and", 0xFF1E2833);
        draw_text_centered(c, FONT_UI, R(dr.x, dr.y + 51, dr.w, 14), "time", 0xFF1E2833);
        x = group_end(c, x, x + 56, top, "Insert");
        /* Editing */
        if (sbtn(w, x, top + 4, G_FIND, "Find", false, 0)) wp_cmd(w, 'f');
        if (sbtn(w, x, top + 26, G_REPLACE, "Replace", false, 0)) wp_cmd(w, 'h');
        if (sbtn(w, x, top + 48, G_SELALL, "Select all", false, 0)) wp_cmd(w, 'a');
        x = group_end(c, x, x + 92, top, "Editing");
    } else {
        /* Zoom */
        rect_t zi = R(x, top + 4, 48, 66), zo = R(x + 50, top + 4, 48, 66), z1 = R(x + 100, top + 4, 48, 66);
        if (rbutton(w, zi, false, d->zoom < 500)) { d->zoom = MIN(500, d->zoom + (d->zoom < 100 ? 10 : 25)); all_dirty(d); }
        if (rbutton(w, zo, false, d->zoom > 10)) { d->zoom = MAX(10, d->zoom - (d->zoom <= 100 ? 10 : 25)); all_dirty(d); }
        if (rbutton(w, z1, false, true)) { d->zoom = 100; all_dirty(d); }
        static const char *zl[3] = { "Zoom in", "Zoom out", "100%" };
        rect_t zr[3] = { zi, zo, z1 };
        for (int i = 0; i < 3; i++) {
            canvas_t tmp; (void)tmp;
            glyph(c, G_ZOOMIN + i, zr[i].x + 16, zr[i].y + 12, 0);
            draw_text_centered(c, FONT_UI, R(zr[i].x, zr[i].y + 44, zr[i].w, 16), zl[i], 0xFF1E2833);
        }
        x = group_end(c, x, x + 150, top, "Zoom");
        /* Show or hide */
        bool r = d->ruler, s = d->status;
        if (ui_checkbox(w, x, top + 8, "Ruler", &r)) { d->ruler = r; w->dirty = true; }
        if (ui_checkbox(w, x, top + 32, "Status bar", &s)) { d->status = s; w->dirty = true; }
        x = group_end(c, x, x + 100, top, "Show or hide");
        /* Settings: word wrap */
        rect_t wr = R(x, top + 6, 120, 22);
        static const char *wn[3] = { "No wrap", "Wrap to window", "Wrap to ruler" };
        draw_text(c, FONT_UI, x, top + 34, "Word wrap", 0xFF1E2833);
        combo(w, wr, wn[d->wrap]);
        if (ui_click_area(w, wr)) {
            static menu_item_t wm[3];
            for (int i = 0; i < 3; i++) { wm[i].label = wn[i]; wm[i].id = 200 + i; wm[i].flags = d->wrap == i ? MI_CHECKED : 0; }
            drop(w, wr, wm, 3);
        }
        x = group_end(c, x, x + 124, top, "Settings");
    }
}

/* ---------------------------------------------------------------- painting */
static void ruler(canvas_t *c, wp_t *d, int y) {
    fill_rect(c, 0, y, c->w, RULER_H, 0xFFDCE4EF);
    hline(c, 0, y + RULER_H - 1, c->w, 0xFFB6BCC6);
    int x0 = d->page_x, inch = zpx(d, DPI);
    fill_rect(c, d->page_x, y + 4, d->page_w, RULER_H - 9, 0xFFC4CEDB);
    fill_rect(c, d->text_x, y + 4, d->text_w, RULER_H - 9, WHITE);
    draw_rect(c, d->page_x, y + 4, d->page_w, RULER_H - 9, 0xFF9AA8BC);
    for (int k = 1; k * inch / 8 < d->page_w; k++) {
        int tx = d->text_x + k * inch / 8 - zpx(d, MARGIN_PX) * 0;
        if (tx >= d->page_x + d->page_w) break;
        int rel = k;
        if (rel % 8 == 0) { char n[8]; snprintf(n, sizeof n, "%d", rel / 8); draw_text(c, FONT_SMALL, tx - 2, y + 6, n, 0xFF3A4656); }
        else vline(c, tx, y + (rel % 4 == 0 ? 9 : 11), rel % 4 == 0 ? 6 : 3, 0xFF5A6676);
    }
    (void)x0;
    /* the indent markers of the caret's paragraph */
    para_t *p = &d->p[d->cur.p];
    int lx = d->text_x + zpx(d, p->lind), fx = lx + zpx(d, p->first), rx = d->text_x + d->text_w - zpx(d, p->rind);
    for (int i = 0; i < 4; i++) fill_rect(c, fx - 3 + i, y + 2 + i, 7 - 2 * i, 1, 0xFF4A5A70);
    for (int i = 0; i < 4; i++) fill_rect(c, lx - i, y + RULER_H - 6 - i, 1 + 2 * i, 1, 0xFF4A5A70);
    for (int i = 0; i < 4; i++) fill_rect(c, rx - i, y + RULER_H - 6 - i, 1 + 2 * i, 1, 0xFF4A5A70);
}
static void wp_paint(window_t *w) {
    wp_t *d = w->data;
    canvas_t *c = w->surf;
    int top = TABS_H + RIB_H + (d->ruler ? RULER_H : 0);
    int bottom = c->h - (d->status ? STATUS_H : 0);
    d->view_y = top; d->view_h = bottom - top;
    int page_w = zpx(d, (int)(PAGE_IN_W * DPI));
    if (d->wrap == 1) page_w = c->w - 40;
    d->page_w = page_w;
    d->page_x = MAX(10, (c->w - 16 - page_w) / 2);
    d->text_x = d->page_x + (d->wrap == 1 ? 12 : zpx(d, MARGIN_PX));
    int tw = d->wrap == 1 ? page_w - 24 : page_w - 2 * zpx(d, MARGIN_PX);
    if (tw != d->text_w) { d->text_w = tw; all_dirty(d); }
    layout(d);
    /* the background and the page */
    fill_rect(c, 0, top, c->w, bottom - top, 0xFFD6DDE8);
    int ptop = top + 10 - d->scroll, ph = MAX(d->view_h - 20, zpx(d, TOP_PX) * 2 + d->doc_h);
    rect_t oldclip = canvas_push_clip(c, R(0, top, c->w - 16, bottom - top));
    fill_rect(c, d->page_x + 3, ptop + 3, page_w, ph, 0xFFB0B9C8);
    fill_rect(c, d->page_x, ptop, page_w, ph, WHITE);
    draw_rect(c, d->page_x, ptop, page_w, ph, 0xFFA8B3C4);
    int ty = ptop + (d->wrap == 1 ? 12 : zpx(d, TOP_PX));
    pos_t sa, sb; sel_range(d, &sa, &sb);
    bool sel = has_sel(d);
    bool focused = w == wm_active;
    for (int pi = 0; pi < d->np; pi++) {
        para_t *p = &d->p[pi];
        if (ty + p->y > bottom || ty + p->y + p->h < top) continue;
        int ly = ty + p->y;
        for (int l = 0; l < p->nl; l++) {
            int x = d->text_x + line_x(d, p, l), base = ly + p->la[l];
            if (l == 0 && p->bullet) {
                int f = font_of(d, p->n ? p->a[0] : p->endst);
                draw_text(c, f, d->text_x + zpx(d, p->lind), base - font_ascent(f), "\xe2\x80\xa2", stab[p->n ? p->a[0] : p->endst].fg);
            }
            int end = p->ls[l + 1];
            int extra = 0, spaces = 0;
            if (p->align == 3 && l < p->nl - 1) {          /* justify: spread the spaces */
                for (int k = p->ls[l]; k < end; k++) if (p->t[k] == ' ') spaces++;
                int avail = d->text_w - zpx(d, p->rind) - zpx(d, p->lind + (p->bullet ? DPI / 4 : 0)) - (l == 0 ? zpx(d, p->first) : 0);
                extra = avail - (p->lw[l] - (l == 0 ? zpx(d, p->first) : 0));
                if (extra < 0) extra = 0;
            }
            int sp_seen = 0;
            for (int k = p->ls[l]; k < end; ) {
                int nk = next_char(p, k), cw = 0;
                for (int q = k; q < nk; q++) cw += p->cw[q];
                if (spaces && p->t[k] == ' ') { int add = extra * (sp_seen + 1) / spaces - extra * sp_seen / spaces; cw += add; sp_seen++; }
                cst_t *cs = &stab[p->a[k]];
                int f = font_of(d, p->a[k]);
                int gy = base - font_ascent(f);
                if (cs->script == 2) gy -= font_height(f) / 2;
                if (cs->script == 1) gy += font_height(f) / 3;
                pos_t here = { pi, k };
                bool in_sel = sel && pcmp(here, sa) >= 0 && pcmp(here, sb) < 0;
                if (in_sel) fill_rect(c, x, ly, cw, p->lh[l], focused ? 0xFF3399FF : 0xFFC8D2DE);
                else if (cs->bg) fill_rect(c, x, ly, cw, p->lh[l], cs->bg);
                if (p->t[k] != '\t' && p->t[k] != ' ') draw_text_n(c, f, x, gy, p->t + k, nk - k, in_sel && focused ? WHITE : (cs->fg ? cs->fg : 0xFF000000));
                if (cs->u) fill_rect(c, x, base + 2, cw, MAX(1, zpx(d, 1)), cs->fg);
                if (cs->s) fill_rect(c, x, base - font_ascent(f) / 3, cw, MAX(1, zpx(d, 1)), cs->fg);
                x += cw; k = nk;
            }
            if (sel && pi >= sa.p && pi < sb.p && l == p->nl - 1) fill_rect(c, x, ly, zpx(d, 5), p->lh[l], focused ? 0xFF3399FF : 0xFFC8D2DE);
            ly += p->lh[l];
        }
    }
    /* the caret */
    if (focused && (timer_ticks() / 530) % 2 == 0) {
        int cx, cy, chh; caret_xy(d, d->cur, &cx, &cy, &chh);
        vline(c, d->text_x + cx, ty + cy, chh, BLACK);
    }
    c->clip = oldclip;
    /* the scroll bar */
    int total = d->doc_h + zpx(d, TOP_PX) * 2 + 20;
    d->scroll = CLAMP(d->scroll, 0, MAX(0, total - d->view_h));
    draw_scrollbar(c, R(c->w - 16, top, 16, bottom - top), MAX(total, d->view_h), d->view_h, d->scroll, true);
    ribbon(w, d);
    if (d->ruler) ruler(c, d, TABS_H + RIB_H);
    if (d->status) {
        int sy = c->h - STATUS_H;
        gradient_v(c, 0, sy, c->w, STATUS_H, 0xFFE9EEF5, 0xFFD5DEEA);
        hline(c, 0, sy, c->w, 0xFFB6BCC6);
        int words = 0, chars = 0;
        for (int i = 0; i < d->np; i++) { bool inw = false; para_t *p = &d->p[i]; for (int k = 0; k < p->n; k++) { if (((unsigned char)p->t[k] & 0xC0) != 0x80) chars++; bool sp = p->t[k] == ' ' || p->t[k] == '\t'; if (!sp && !inw) words++; inw = !sp; } }
        char st[80]; snprintf(st, sizeof st, "%d word%s, %d character%s", words, words == 1 ? "" : "s", chars, chars == 1 ? "" : "s");
        draw_text(c, FONT_UI, 8, sy + 5, st, 0xFF2B3A4E);
        char z[16]; snprintf(z, sizeof z, "%d%%", d->zoom);
        int zx = c->w - 200;
        draw_text(c, FONT_UI, zx, sy + 5, z, 0xFF2B3A4E);
        rect_t mr = R(zx + 42, sy + 4, 16, 16), pr2 = R(zx + 176, sy + 4, 16, 16);
        if (rbutton(w, mr, false, d->zoom > 10)) { d->zoom = MAX(10, d->zoom - 10); all_dirty(d); }
        if (rbutton(w, pr2, false, d->zoom < 500)) { d->zoom = MIN(500, d->zoom + 10); all_dirty(d); }
        fill_rect(c, mr.x + 4, mr.y + 7, 8, 2, 0xFF2B3A4E);
        fill_rect(c, pr2.x + 4, pr2.y + 7, 8, 2, 0xFF2B3A4E); fill_rect(c, pr2.x + 7, pr2.y + 4, 2, 8, 0xFF2B3A4E);
        int sx0 = zx + 62, sw = 110;
        fill_rect(c, sx0, sy + 11, sw, 2, 0xFF8E9BB0);
        vline(c, sx0 + sw / 2, sy + 7, 10, 0xFF8E9BB0);
        int kx = d->zoom <= 100 ? sx0 + (d->zoom - 10) * (sw / 2) / 90 : sx0 + sw / 2 + (d->zoom - 100) * (sw / 2) / 400;
        fill_round_rect(c, kx - 3, sy + 5, 7, 14, 2, 0xFF5D7799);
        rect_t sr = R(sx0, sy + 3, sw, 18);
        if (ui_hover(w, sr) && w->ui.down) {
            int mx = CLAMP(w->ui.mx - sx0, 0, sw);
            int z = mx <= sw / 2 ? 10 + mx * 90 / (sw / 2) : 100 + (mx - sw / 2) * 400 / (sw / 2);
            if (z != d->zoom) { d->zoom = z; all_dirty(d); }
        }
    }
}

/* ---------------------------------------------------------------- input */
static void ensure_caret_visible(wp_t *d) {
    layout(d);
    int cx, cy, ch; caret_xy(d, d->cur, &cx, &cy, &ch);
    int y = cy + 10 + zpx(d, TOP_PX);
    if (y < d->scroll) d->scroll = y - 10;
    if (y + ch > d->scroll + d->view_h) d->scroll = y + ch - d->view_h + 10;
    if (d->scroll < 0) d->scroll = 0;
}
static void date_insert(wp_t *d, int which) {
    rtc_time_t t; rtc_now(&t);
    static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    char s[48];
    if (which == 0) snprintf(s, sizeof s, "%d/%d/%d", t.month, t.day, t.year);
    else if (which == 1) snprintf(s, sizeof s, "%s %d, %d", mon[(t.month + 11) % 12], t.day, t.year);
    else if (which == 2) snprintf(s, sizeof s, "%d-%02d-%02d", t.year, t.month, t.day);
    else if (which == 3) snprintf(s, sizeof s, "%d:%02d %s", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour >= 12 ? "PM" : "AM");
    else snprintf(s, sizeof s, "%d:%02d", t.hour, t.minute);
    push_undo(d, 0);
    insert_text(d, s, strlen(s), d->typing);
    changed(d);
}
static void ask_save_cb(void *ctx, int r);
static void wp_cmd(window_t *w, int id) {
    wp_t *d = w->data;
    switch (id) {
    case M_NEW: if (d->modified) { d->closing = false; msgbox("WordPad", "Do you want to save changes to this document before starting a new one?", MBI_WARNING, MBX_YESNOCANCEL, ask_save_cb, w); break; }
                doc_new(d); d->path[0] = 0; d->modified = false; wp_title(d); break;
    case M_OPEN: file_dialog("Open (.rtf, .docx, .odt, .txt)", d->path[0] ? d->path : USER_DOCS, false, NULL, open_cb, w); break;
    case M_SAVE: wp_save(d); break;
    case M_SAVEAS: wp_save_as(d); break;
    case M_ABOUT: msgbox("About WordPad", "WordPad\nTHE WRLD OS\n\nA word processor for Rich Text (.rtf), Office Open XML (.docx) and plain text documents; it also opens OpenDocument Text (.odt).", MBI_INFO, MBX_OK, NULL, NULL); break;
    case M_EXIT: wm_close(w); return;
    case 'z': wp_undo(d); break;
    case 'y': wp_redo(d); break;
    case 'x': wp_copy(d, true); break;
    case 'c': wp_copy(d, false); break;
    case 'v': wp_paste(d); break;
    case 'a': d->anc.p = 0; d->anc.o = 0; d->cur.p = d->np - 1; d->cur.o = d->p[d->np - 1].n; break;
    case 'f': input_box("Find", "Find what:", d->find, find_cb, w); break;
    case 'h': input_box("Replace", "Find what:", d->find, replace_find_cb, w); break;
    default:
        if (id >= M_FAM0 && id < M_FAM0 + 3) apply_style(d, f_family, id - M_FAM0);
        else if (id >= M_SIZE0 && id < M_SIZE0 + (int)ARRAY_LEN(sizes)) apply_style(d, f_size, sizes[id - M_SIZE0] * 2);
        else if (id >= M_COL0 && id < M_COL0 + (int)ARRAY_LEN(text_colors)) apply_style(d, f_color, (int)text_colors[id - M_COL0]);
        else if (id == M_HIL0) apply_style(d, f_highlight, -1);
        else if (id > M_HIL0 && id <= M_HIL0 + (int)ARRAY_LEN(hl_colors)) apply_style(d, f_highlight, (int)hl_colors[id - M_HIL0 - 1]);
        else if (id >= M_SP0 && id < M_SP0 + 20) apply_para(d, 3, id - M_SP0);
        else if (id >= M_DATE0 && id < M_DATE0 + 5) date_insert(d, id - M_DATE0);
        else if (id >= 200 && id < 203) { d->wrap = id - 200; all_dirty(d); }
    }
    w->dirty = true;
}
static void ask_save_cb(void *ctx, int r) {
    window_t *w = ctx; if (!wm_alive(w)) return;
    wp_t *d = w->data;
    if (r == IDYES) { wp_save(d); if (!d->closing && !d->modified) { doc_new(d); d->path[0] = 0; wp_title(d); } }
    else if (r == IDNO) { if (d->closing) { wm_destroy(w); return; } doc_new(d); d->path[0] = 0; d->modified = false; wp_title(d); }
    else d->closing = false;
    w->dirty = true;
}
static bool wp_close(window_t *w) {
    wp_t *d = w->data;
    if (!d->modified) return true;
    d->closing = true;
    char m[300]; snprintf(m, sizeof m, "Do you want to save changes to %s?", d->path[0] ? path_basename(d->path) : "Document");
    msgbox("WordPad", m, MBI_WARNING, MBX_YESNOCANCEL, ask_save_cb, w);
    return false;
}
static void move_caret(wp_t *d, pos_t q, bool extend) { d->cur = q; if (!extend) d->anc = q; d->want_x = -1; }
static void vertical(wp_t *d, int dir, bool extend) {
    layout(d);
    int cx, cy, ch; caret_xy(d, d->cur, &cx, &cy, &ch);
    if (d->want_x < 0) d->want_x = cx;
    int ny = dir < 0 ? cy - 2 : cy + ch + 2;
    pos_t q = ny < 0 ? (pos_t){ 0, 0 } : pos_at(d, d->want_x, ny);
    int keep = d->want_x;
    move_caret(d, q, extend);
    d->want_x = keep;
}
static void wp_key(window_t *w, key_event_t *e) {
    wp_t *d = w->data;
    d->shift = (e->mods & MOD_SHIFT) != 0;
    if (e->key == KEY_LSHIFT || e->key == KEY_RSHIFT) d->shift = e->pressed;
    if (!e->pressed) return;
    if (w->ui.focus) return;                               /* a text box of the ribbon has it */
    bool ctrl = e->mods & MOD_CTRL, shift = e->mods & MOD_SHIFT;
    para_t *p = &d->p[d->cur.p];
    if (ctrl) {
        int k = tolower(e->key < 0x100 ? e->key : 0);
        if (!k && e->ch) k = tolower(e->ch);
        switch (k) {
        case 'n': wp_cmd(w, M_NEW); return;
        case 'o': wp_cmd(w, M_OPEN); return;
        case 's': wp_cmd(w, M_SAVE); return;
        case 'z': case 'y': case 'x': case 'c': case 'v': case 'a': case 'f': case 'h': wp_cmd(w, k); ensure_caret_visible(d); return;
        case 'b': toggle(d, f_bold, 0); return;
        case 'i': toggle(d, f_italic, 1); return;
        case 'u': toggle(d, f_under, 2); return;
        case 'l': apply_para(d, 0, 0); return;
        case 'e': apply_para(d, 0, 1); return;
        case 'r': apply_para(d, 0, 2); return;
        case 'j': apply_para(d, 0, 3); return;
        case '=': apply_style(d, f_script, shift ? 2 : 1); return;
        }
        if (e->key == KEY_HOME) { move_caret(d, (pos_t){ 0, 0 }, shift); ensure_caret_visible(d); w->dirty = true; return; }
        if (e->key == KEY_END) { move_caret(d, (pos_t){ d->np - 1, d->p[d->np - 1].n }, shift); ensure_caret_visible(d); w->dirty = true; return; }
    }
    if (e->key == KEY_F3) { find_next(d); return; }
    switch (e->key) {
    case KEY_LEFT: { pos_t q = d->cur;
        if (has_sel(d) && !shift) { pos_t a, b; sel_range(d, &a, &b); q = a; }
        else if (q.o > 0) q.o = prev_char(p, q.o); else if (q.p > 0) { q.p--; q.o = d->p[q.p].n; }
        move_caret(d, q, shift); break; }
    case KEY_RIGHT: { pos_t q = d->cur;
        if (has_sel(d) && !shift) { pos_t a, b; sel_range(d, &a, &b); q = b; }
        else if (q.o < p->n) q.o = next_char(p, q.o); else if (q.p < d->np - 1) { q.p++; q.o = 0; }
        move_caret(d, q, shift); break; }
    case KEY_UP: vertical(d, -1, shift); break;
    case KEY_DOWN: vertical(d, 1, shift); break;
    case KEY_PGUP: for (int i = 0; i < 20; i++) vertical(d, -1, shift); break;
    case KEY_PGDN: for (int i = 0; i < 20; i++) vertical(d, 1, shift); break;
    case KEY_HOME: { int l = line_of(p, d->cur.o); move_caret(d, (pos_t){ d->cur.p, p->ls[l] }, shift); break; }
    case KEY_END: { int l = line_of(p, d->cur.o); int o = p->ls[l + 1]; if (l < p->nl - 1 && o > 0 && p->t[o - 1] == ' ') o--; move_caret(d, (pos_t){ d->cur.p, o }, shift); break; }
    case KEY_BACKSPACE:
        push_undo(d, 2);
        if (has_sel(d)) delete_sel(d);
        else if (d->cur.o > 0) { pos_t a = { d->cur.p, prev_char(p, d->cur.o) }; delete_range(d, a, d->cur); }
        else if (d->cur.p > 0) { pos_t a = { d->cur.p - 1, d->p[d->cur.p - 1].n }; delete_range(d, a, d->cur); }
        changed(d); break;
    case KEY_DELETE:
        push_undo(d, 3);
        if (has_sel(d)) delete_sel(d);
        else if (d->cur.o < p->n) { pos_t b = { d->cur.p, next_char(p, d->cur.o) }; delete_range(d, d->cur, b); }
        else if (d->cur.p < d->np - 1) { pos_t b = { d->cur.p + 1, 0 }; delete_range(d, d->cur, b); }
        changed(d); break;
    case KEY_ENTER: push_undo(d, 0); insert_text(d, "\n", 1, has_sel(d) ? d->typing : style_at(d, d->cur)); changed(d); break;
    case KEY_TAB: push_undo(d, 1); insert_text(d, "\t", 1, d->typing); changed(d); break;
    default:
        if (e->ch >= 32 && (uint8_t)e->ch != 127) {
            push_undo(d, 1);
            char ch = e->ch;
            insert_text(d, &ch, 1, d->typing);
            changed(d);
        } else return;
    }
    /* the typing style follows the caret when it moves without typing */
    if (e->key == KEY_LEFT || e->key == KEY_RIGHT || e->key == KEY_UP || e->key == KEY_DOWN || e->key == KEY_HOME || e->key == KEY_END || e->key == KEY_PGUP || e->key == KEY_PGDN || e->key == KEY_BACKSPACE)
        d->typing = style_at(d, d->cur);
    ensure_caret_visible(d);
    w->dirty = true;
}
static void wp_mouse(window_t *w, int type, int x, int y, int btn) {
    wp_t *d = w->data;
    if (type == ME_WHEEL) { d->scroll += btn * 48; w->dirty = true; return; }
    int top = d->view_y, bottom = d->view_y + d->view_h;
    if (x >= w->surf->w - 16 && y >= top && y < bottom && (type == ME_DOWN || type == ME_DBLCLICK)) {   /* the scroll bar: page up or down */
        d->scroll += (y < (top + bottom) / 2 ? -1 : 1) * (d->view_h - 40); w->dirty = true; return;
    }
    bool in_doc = y >= top && y < bottom && x < w->surf->w - 16;
    int ty = top + 10 - d->scroll + (d->wrap == 1 ? 12 : zpx(d, TOP_PX));
    if (type == ME_DOWN && (btn & MB_LEFT) && in_doc) {
        w->ui.focus = 0;
        layout(d);
        pos_t q = pos_at(d, x - d->text_x, y - ty);
        move_caret(d, q, d->shift);
        d->typing = style_at(d, d->cur);
        d->dragging = true; w->dirty = true;
    } else if (type == ME_MOVE && d->dragging) {
        layout(d);
        d->cur = pos_at(d, x - d->text_x, y - ty);
        if (y < top) d->scroll -= 20; else if (y >= bottom) d->scroll += 20;
        w->dirty = true;
    } else if (type == ME_UP) d->dragging = false;
    else if (type == ME_DBLCLICK && in_doc) {                /* a word */
        layout(d);
        pos_t q = pos_at(d, x - d->text_x, y - ty);
        para_t *p = &d->p[q.p];
        int a = q.o, b = q.o;
        while (a > 0 && (isalnum((unsigned char)p->t[a - 1]) || ((unsigned char)p->t[a - 1] & 0x80))) a--;
        while (b < p->n && (isalnum((unsigned char)p->t[b]) || ((unsigned char)p->t[b] & 0x80))) b++;
        while (b < p->n && p->t[b] == ' ') b++;
        d->anc.p = d->cur.p = q.p; d->anc.o = a; d->cur.o = b;
        d->dragging = false; w->dirty = true;
    } else if (type == ME_DOWN && (btn & MB_RIGHT) && in_doc) {
        static const menu_item_t cm[] = { { "Cut", 'x', 0 }, { "Copy", 'c', 0 }, { "Paste", 'v', 0 }, { "-", 0, 0 }, { "Select all", 'a', 0 } };
        rect_t cr = wm_client_rect(w);
        menu_win = w;
        popup_menu(cr.x + x, cr.y + y, cm, ARRAY_LEN(cm), menu_cb, w);
    }
}
static void wp_blink(window_t *w) { w->dirty = true; }
static void wp_destroy(window_t *w) {
    wp_t *d = w->data;
    if (!d) return;
    doc_clear(d); kfree(d->p);
    for (int i = 0; i < d->nundo; i++) kfree(d->undo[i]);
    for (int i = 0; i < d->nredo; i++) kfree(d->redo[i]);
    kfree(d); w->data = NULL;
}
static bool wp_drop(window_t *w, int x, int y, const char *paths, bool copy) {
    (void)w; (void)x; (void)y; (void)copy;
    char p[512]; const char *e = strchr(paths, '\n'); size_t n = e ? (size_t)(e - paths) : strlen(paths);
    snprintf(p, sizeof p, "%.*s", (int)MIN(n, sizeof p - 1), paths);
    if (!p[0]) return false;
    wordpad_open(p);
    return true;
}
void wordpad_open(const char *path) {
    window_t *w = wm_create("Document - WordPad", -1, -1, MIN(940, SCREEN_W - 40), MIN(680, SCREEN_H - 70), WF_RESIZABLE);
    if (!w) return;
    wp_t *d = kzalloc(sizeof *d);
    d->w = w; d->zoom = 100; d->ruler = true; d->status = true; d->wrap = 2; d->want_x = -1;
    doc_new(d);
    w->data = d; w->paint = wp_paint; w->key = wp_key; w->mouse = wp_mouse; w->close = wp_close; w->destroy = wp_destroy;
    w->drop = wp_drop;
    w->icon = ICON_WORDPAD; w->cursor = CURSOR_IBEAM;
    wm_set_timer(w, 530, wp_blink);
    if (path && *path && !wp_load(d, path)) msgbox("WordPad", "WordPad cannot open this file.", MBI_WARNING, MBX_OK, NULL, NULL);
    wp_title(d);
}
