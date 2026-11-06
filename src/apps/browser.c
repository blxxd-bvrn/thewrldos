/* The WRLD Xplorer - web browser.
 * Pages load on a worker task (DNS, TCP, TLS, HTTP), are parsed by html.c and
 * laid out here into a display list (text runs, boxes, images, form
 * controls). Images load after the text and the page is laid out again.
 * There is no JavaScript engine. */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "fs.h"
#include "net.h"
#include "browser.h"

#define HOME_URL "wrld://home"
#define TOOLBAR_H 40
#define ADDR_H 28
#define STATUS_H 22
#define SB_W 16

/* ---------------- page model ---------------- */
enum { IT_TEXT, IT_RECT, IT_IMG, IT_BULLET, IT_FRAME, IT_CTRL, IT_HLINE };
typedef struct { uint8_t kind; uint8_t under; int16_t font; int x, y, w, h, asc; uint32_t color; const char *text; int len; int link, idx; } item_t;
typedef struct { char *url; canvas_t *img, *scaled; int state; /* 0 pending, 1 ok, 2 failed */ char *alt; } pimg_t;
enum { C_TEXT, C_PASSWORD, C_SUBMIT, C_BUTTON, C_CHECK, C_RADIO, C_HIDDEN, C_SELECT, C_TEXTAREA, C_RESET };
typedef struct { int type, form; node_t *node; char name[64]; char value[512]; char label[64]; bool checked; int sel, nopts; char **opts, **optvals; rect_t r; } ctrl_t;
typedef struct { char action[1024]; bool post; } form_t;
typedef struct page {
    doc_t *doc;
    char url[1024], title[160];
    bool secure; char tls[80];
    char *source; uint32_t srclen;
    pimg_t *imgs; int nimgs;
    ctrl_t *ctrls; int nctrls;
    form_t *forms; int nforms;
    item_t *items; int nitems, capitems;
    char **links; int nlinks, caplinks;
    struct { const char *name; int y; } *anchors; int nanchors, capanchors;
    int height, lay_width;
    uint32_t bg, fg, linkc;
    bool worker_running, orphan;
} page_t;

typedef struct {
    window_t *w;
    page_t *page;
    char addr[1024];
    char status[256];
    char hist[48][1024]; int nhist, hpos;
    int scroll;
    volatile int gen;
    bool loading, closed;
    int refs;
    int hover_link, focus_ctrl;
    bool drag_sb; int drag_off;
    uint32_t prog_got, prog_total;
} browser_t;

/* ---------------- display list ---------------- */
static item_t *add_item(page_t *p, int kind) {
    if (p->nitems == p->capitems) { p->capitems = p->capitems ? p->capitems * 2 : 1024; p->items = krealloc(p->items, sizeof(item_t) * p->capitems); }
    item_t *it = &p->items[p->nitems++];
    memset(it, 0, sizeof *it); it->kind = kind; it->link = -1;
    return it;
}
static int add_link(page_t *p, const char *href) {
    if (!href) return -1;
    char abs[1024];
    url_resolve(p->doc->base[0] ? p->doc->base : p->url, href, abs, sizeof abs);
    for (int i = MAX(0, p->nlinks - 8); i < p->nlinks; i++) if (!strcmp(p->links[i], abs)) return i;
    if (p->nlinks == p->caplinks) { p->caplinks = p->caplinks ? p->caplinks * 2 : 256; p->links = krealloc(p->links, sizeof(char *) * p->caplinks); }
    p->links[p->nlinks] = arena_strdup(&p->doc->arena, abs, -1);
    return p->nlinks++;
}

/* ---------------- layout ---------------- */
typedef struct { int font; uint32_t color; bool under, pre, bold, italic, mono, nowrap; int size; int link; int align; } sty_t;
typedef struct { page_t *pg; int x0, w, y, cx, line_start, line_h, line_asc, align, trail, maxx; bool pending_space; } lay_t;
static int pick_font(sty_t *s) {
    if (s->mono) return FONT_MONO;
    switch (s->size) { case 1: return FONT_H1; case 2: return FONT_H2; case 3: return FONT_H3; case -1: return FONT_WEB_SMALL; }
    return s->bold ? (s->italic ? FONT_WEB_BI : FONT_WEB_B) : (s->italic ? FONT_WEB_I : FONT_WEB);
}
static void newline(lay_t *L, bool force, int font) {
    page_t *p = L->pg;
    /* a list marker waiting for its first line (e.g. <li><p>...) stays pending */
    if (!force && L->line_start == p->nitems - 1 && p->items[p->nitems - 1].kind == IT_BULLET) { L->cx = L->x0; L->pending_space = false; return; }
    if (L->line_start == p->nitems && !L->line_h) {
        if (force) { L->y += font_height(font); L->trail = 0; }
        L->cx = L->x0; L->pending_space = false;
        return;
    }
    int lw = L->cx - L->x0, shift = L->align == 1 ? (L->w - lw) / 2 : L->align == 2 ? L->w - lw : 0;
    if (shift < 0) shift = 0;
    for (int i = L->line_start; i < p->nitems; i++) {
        item_t *it = &p->items[i];
        it->x += shift;
        it->y = L->y + L->line_asc - it->asc;
    }
    if (L->cx + shift > L->maxx) L->maxx = L->cx + shift;
    L->y += L->line_h; L->trail = 0;
    L->line_start = p->nitems; L->line_h = 0; L->line_asc = 0;
    L->cx = L->x0; L->pending_space = false;
}
static void vspace(lay_t *L, int m, int font) {
    newline(L, false, font);
    if (L->trail < m) { L->y += m - L->trail; L->trail = m; }
}
static void place_box(lay_t *L, item_t *it, int w, int h, int asc) {
    int sp = L->pending_space && L->cx > L->x0 ? 4 : 0;
    if (L->cx + sp + w > L->x0 + L->w && L->cx > L->x0) { newline(L, false, FONT_WEB); sp = 0; }
    it->x = L->cx + sp; it->w = w; it->h = h; it->asc = asc;
    L->cx += sp + w; L->pending_space = false;
    if (asc > L->line_asc) L->line_asc = asc;
    if (h - asc + L->line_asc > L->line_h) L->line_h = h - asc + L->line_asc;
    if (L->line_h < h) L->line_h = h;
}
static void place_text(lay_t *L, sty_t *s, const char *t, int n) {
    int f = pick_font(s), fh = font_height(f), fa = font_ascent(f);
    int spw = text_width(f, " ");
    int w = text_width_n(f, t, n);
    int sp = L->pending_space && L->cx > L->x0 ? spw : 0;
    if (!s->nowrap && L->cx + sp + w > L->x0 + L->w && L->cx > L->x0) { newline(L, false, f); sp = 0; }
    page_t *p = L->pg;
    /* extend the previous run when it has the same style (fewer items) */
    item_t *last = p->nitems > L->line_start ? &p->items[p->nitems - 1] : NULL;
    if (last && last->kind == IT_TEXT && last->font == f && last->color == s->color && last->link == s->link && last->under == s->under &&
        last->text + last->len + (sp ? 1 : 0) == t && (!sp || t[-1] == ' ') && last->x + last->w + sp == L->cx + sp) {
        last->len += n + (sp ? 1 : 0); last->w += sp + w;
    } else {
        item_t *it = add_item(p, IT_TEXT);
        it->font = f; it->color = s->color; it->text = t; it->len = n; it->link = s->link; it->under = s->under;
        it->x = L->cx + sp; it->w = w; it->h = fh; it->asc = fa;
    }
    L->cx += sp + w; L->pending_space = false;
    if (fa > L->line_asc) L->line_asc = fa;
    if (fh - fa + L->line_asc > L->line_h) L->line_h = fh - fa + L->line_asc;
    if (L->line_h < fh) L->line_h = fh;
}
static void layout_text(lay_t *L, sty_t *s, const char *t) {
    if (s->pre) {
        while (*t) {
            const char *e = t; while (*e && *e != '\n') e++;
            if (e > t) { int sv = L->pending_space; L->pending_space = false; place_text(L, s, t, e - t); (void)sv; }
            if (*e == '\n') { newline(L, true, pick_font(s)); e++; }
            t = e;
        }
        return;
    }
    while (*t) {
        if (*t == ' ' || *t == '\n' || *t == '\t' || *t == '\r') { L->pending_space = true; t++; continue; }
        const char *e = t; while (*e && *e != ' ' && *e != '\n' && *e != '\t' && *e != '\r') e++;
        place_text(L, s, t, e - t);
        t = e;
    }
}
static void layout_children(lay_t *L, node_t *n, sty_t *s);
static void layout_node(lay_t *L, node_t *n, sty_t *parent);
static void add_anchor(page_t *p, const char *name, int y) {
    if (p->nanchors == p->capanchors) { p->capanchors = p->capanchors ? p->capanchors * 2 : 64; p->anchors = krealloc(p->anchors, sizeof *p->anchors * p->capanchors); }
    p->anchors[p->nanchors].name = name; p->anchors[p->nanchors].y = y; p->nanchors++;
}
static void apply_css_style(node_t *n, sty_t *s) {
    if (n->css.set & ST_COLOR) s->color = n->css.color;
    if (n->css.set & ST_BOLD) s->bold = n->css.bold;
    if (n->css.set & ST_ITALIC) s->italic = n->css.italic;
    if (n->css.set & ST_UNDER) s->under = n->css.under || s->link >= 0;
    if (n->css.set & ST_SIZE) s->size = n->css.size;
}

static bool is_block(node_t *n) {
    static const char *b[] = { "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "blockquote", "pre", "table",
        "form", "center", "header", "footer", "nav", "section", "article", "main", "aside", "figure", "figcaption", "address", "fieldset", "legend",
        "hr", "details", "summary", "noscript", "caption", "menu", "dir", "listing", "xmp", "hgroup", "search" };
    if (n->css.set & ST_DISPLAY) return n->css.display == 1;
    for (unsigned i = 0; i < ARRAY_LEN(b); i++) if (!strcmp(n->tag, b[i])) return true;
    return false;
}
static int text_content(node_t *n, char *buf, int cap, int len) {
    for (node_t *c = n->first; c && len < cap - 1; c = c->next) {
        if (c->type == 1) { int l = MIN((int)strlen(c->text), cap - 1 - len); memcpy(buf + len, c->text, l); len += l; buf[len] = 0; }
        else len = text_content(c, buf, cap, len);
    }
    return len;
}

/* tables: two measuring passes (min/max content width) per cell, then placement */
typedef struct { node_t *n; int span, minw, maxw; } cell_t;
static void measure(lay_t *L, node_t *cell, sty_t *s, int width, int *out_w) {
    page_t *p = L->pg;
    int save_items = p->nitems, save_links = p->nlinks;
    lay_t M = { p, 0, width, 0, 0, p->nitems, 0, 0, 0, 0, 0, false };
    layout_children(&M, cell, s);
    newline(&M, false, FONT_WEB);
    *out_w = M.maxx;
    p->nitems = save_items; p->nlinks = save_links;
}
static int attr_int(node_t *n, const char *a, int def) { const char *v = node_attr(n, a); return v && isdigit(*v) ? atoi(v) : def; }
static bool attr_pct(node_t *n, const char *a, int *pct) { const char *v = node_attr(n, a); if (v && strchr(v, '%')) { *pct = atoi(v); return true; } return false; }
static void layout_table(lay_t *L, node_t *t, sty_t *s) {
    page_t *p = L->pg;
    /* gather rows and cells */
    node_t *rows[400]; int nrows = 0;
    for (node_t *c = t->first; c; c = c->next) {
        if (c->type) continue;
        if (!strcmp(c->tag, "tr") && nrows < 400) rows[nrows++] = c;
        else if (!strcmp(c->tag, "tbody") || !strcmp(c->tag, "thead") || !strcmp(c->tag, "tfoot")) {
            for (node_t *r = c->first; r; r = r->next) if (!r->type && !strcmp(r->tag, "tr") && nrows < 400) rows[nrows++] = r;
        } else if (!strcmp(c->tag, "caption")) { sty_t cs = *s; int al = L->align; L->align = 1; layout_node(L, c, &cs); newline(L, false, FONT_WEB); L->align = al; }
    }
    if (!nrows) { layout_children(L, t, s); return; }
    int border = attr_int(t, "border", 0) ? 1 : 0;
    int pad = attr_int(t, "cellpadding", 2), spacing = attr_int(t, "cellspacing", 2);
    int ncols = 0;
    cell_t *cells = kzalloc(sizeof(cell_t) * nrows * 64); int *rowcells = kzalloc(sizeof(int) * nrows);
    for (int r = 0; r < nrows; r++) {
        int cols = 0;
        for (node_t *c = rows[r]->first; c; c = c->next) {
            if (c->type || (strcmp(c->tag, "td") && strcmp(c->tag, "th")) || rowcells[r] >= 64) continue;
            if ((c->css.set & ST_DISPLAY) && !c->css.display) continue;
            cell_t *ce = &cells[r * 64 + rowcells[r]++];
            ce->n = c; ce->span = CLAMP(attr_int(c, "colspan", 1), 1, 32);
            cols += ce->span;
        }
        if (cols > ncols) ncols = cols;
    }
    if (!ncols) { kfree(cells); kfree(rowcells); return; }
    ncols = MIN(ncols, 64);
    int *colmin = kzalloc(sizeof(int) * ncols), *colmax = kzalloc(sizeof(int) * ncols), *colw = kzalloc(sizeof(int) * ncols);
    for (int r = 0; r < nrows; r++) {
        int col = 0;
        for (int k = 0; k < rowcells[r] && col < ncols; k++) {
            cell_t *ce = &cells[r * 64 + k];
            sty_t cs = *s; if (!strcmp(ce->n->tag, "th")) cs.bold = true;
            if (node_attr(ce->n, "nowrap")) cs.nowrap = true;
            apply_css_style(rows[r], &cs); apply_css_style(ce->n, &cs);
            measure(L, ce->n, &cs, 100000, &ce->maxw);
            measure(L, ce->n, &cs, 1, &ce->minw);
            int fixed = attr_int(ce->n, "width", 0);
            if (fixed && !strchr(node_attr(ce->n, "width"), '%')) { ce->minw = MAX(ce->minw, fixed - 2 * pad); ce->maxw = MAX(ce->minw, fixed - 2 * pad); }
            ce->minw += 2 * pad; ce->maxw += 2 * pad;
            int span = MIN(ce->span, ncols - col);
            if (span == 1) { colmin[col] = MAX(colmin[col], ce->minw); colmax[col] = MAX(colmax[col], ce->maxw); }
            col += span;
        }
    }
    for (int r = 0; r < nrows; r++) {                        /* spanning cells: spread what is missing */
        int col = 0;
        for (int k = 0; k < rowcells[r] && col < ncols; k++) {
            cell_t *ce = &cells[r * 64 + k]; int span = MIN(ce->span, ncols - col);
            if (span > 1) {
                int smin = 0, smax = 0; for (int c = col; c < col + span; c++) { smin += colmin[c]; smax += colmax[c]; }
                if (ce->minw > smin) for (int c = col; c < col + span; c++) colmin[c] += (ce->minw - smin + span - 1) / span;
                if (ce->maxw > smax) for (int c = col; c < col + span; c++) colmax[c] += (ce->maxw - smax + span - 1) / span;
            }
            col += span;
        }
    }
    int avail = L->w, tw = 0, pct;
    if (attr_pct(t, "width", &pct)) tw = avail * CLAMP(pct, 1, 100) / 100;
    else if (attr_int(t, "width", 0)) tw = MIN(attr_int(t, "width", 0), avail);
    int extra = spacing * (ncols + 1) + 2 * border;
    int smin = 0, smax = 0; for (int c = 0; c < ncols; c++) { colmax[c] = MAX(colmax[c], colmin[c]); smin += colmin[c]; smax += colmax[c]; }
    int target = tw ? tw - extra : MIN(smax, avail - extra);
    if (target < smin) target = smin;
    if (smax <= target) {
        int spare = target - smax;
        for (int c = 0; c < ncols; c++) colw[c] = colmax[c] + (tw && smax ? (int)((int64_t)spare * colmax[c] / smax) : 0);
        if (tw && !smax) for (int c = 0; c < ncols; c++) colw[c] = target / ncols;
    } else {
        int range = smax - smin;
        for (int c = 0; c < ncols; c++) colw[c] = colmin[c] + (range ? (int)((int64_t)(target - smin) * (colmax[c] - colmin[c]) / range) : 0);
    }
    int total = extra; for (int c = 0; c < ncols; c++) total += colw[c];
    newline(L, false, FONT_WEB);
    const char *al = node_attr(t, "align");
    int tx = L->x0 + ((al && !strcasecmp(al, "center")) || (L->align == 1 && !al) ? MAX(0, (L->w - total) / 2) : al && !strcasecmp(al, "right") ? MAX(0, L->w - total) : 0);
    int y = L->y;
    uint32_t tbg = 0; const char *tbs = node_attr(t, "bgcolor");
    bool has_tbg = (t->css.set & ST_BG) ? (tbg = t->css.bg, true) : (tbs && parse_color(tbs, &tbg));
    int tbg_item = -1;
    if (has_tbg) { item_t *it = add_item(p, IT_RECT); it->x = tx; it->y = y; it->w = total; it->color = tbg; tbg_item = p->nitems - 1; }
    y += border + spacing;
    for (int r = 0; r < nrows; r++) {
        uint32_t rbg = 0; const char *rbs = node_attr(rows[r], "bgcolor");
        bool has_rbg = (rows[r]->css.set & ST_BG) ? (rbg = rows[r]->css.bg, true) : (rbs && parse_color(rbs, &rbg));
        int row_item = -1;
        if (has_rbg) { item_t *it = add_item(p, IT_RECT); it->x = tx + border; it->y = y; it->w = total - 2 * border; it->color = rbg; row_item = p->nitems - 1; }
        int col = 0, x = tx + border + spacing, rowh = attr_int(rows[r], "height", 0);
        int cstart[64], cend[64], ch[64], cbg[64]; char cval[64];
        const char *rva = node_attr(rows[r], "valign");
        for (int k = 0; k < rowcells[r] && col < ncols; k++) {
            cell_t *ce = &cells[r * 64 + k]; int span = MIN(ce->span, ncols - col);
            int cw = 0; for (int c = col; c < col + span; c++) cw += colw[c];
            cw += spacing * (span - 1);
            uint32_t cb = 0; const char *cbs = node_attr(ce->n, "bgcolor");
            bool has_cb = (ce->n->css.set & ST_BG) ? (cb = ce->n->css.bg, true) : (cbs && parse_color(cbs, &cb));
            cbg[k] = -1;
            if (has_cb) { item_t *it = add_item(p, IT_RECT); it->x = x; it->y = y; it->w = cw; it->color = cb; cbg[k] = p->nitems - 1; }
            cstart[k] = p->nitems;
            sty_t cs = *s; bool th = !strcmp(ce->n->tag, "th"); if (th) cs.bold = true;
            if (node_attr(ce->n, "nowrap")) cs.nowrap = true;
            apply_css_style(rows[r], &cs); apply_css_style(ce->n, &cs);
            const char *ca = node_attr(ce->n, "align"); if (!ca) ca = node_attr(rows[r], "align");
            int align = ca ? (!strcasecmp(ca, "center") ? 1 : !strcasecmp(ca, "right") ? 2 : 0) : th ? 1 : 0;
            if (ce->n->css.set & ST_ALIGN) align = ce->n->css.align;
            lay_t C = { p, x + pad, MAX(1, cw - 2 * pad), y + pad, x + pad, p->nitems, 0, 0, align, 1000, 0, false };
            layout_children(&C, ce->n, &cs);
            newline(&C, false, FONT_WEB);
            cend[k] = p->nitems;
            ch[k] = C.y - y + pad;
            int fixed_h = attr_int(ce->n, "height", 0);
            if (fixed_h > ch[k]) ch[k] = fixed_h;
            if (ch[k] > rowh) rowh = ch[k];
            const char *va = node_attr(ce->n, "valign"); if (!va) va = rva;
            cval[k] = va ? (!strcasecmp(va, "top") ? 't' : !strcasecmp(va, "bottom") ? 'b' : 'm') : 'm';
            if (border) { item_t *it = add_item(p, IT_FRAME); it->x = x; it->y = y; it->w = cw; it->color = HEX(0x808080); it->idx = k; }
            x += cw + spacing; col += span;
        }
        for (int k = 0; k < rowcells[r] && k < 64; k++) {
            if (cbg[k] >= 0) p->items[cbg[k]].h = rowh;
            int dy = cval[k] == 'm' ? (rowh - ch[k]) / 2 : cval[k] == 'b' ? rowh - ch[k] : 0;
            if (dy > 0) for (int i = cstart[k]; i < cend[k]; i++) p->items[i].y += dy;
        }
        for (int i = cstart[0]; i < p->nitems; i++) if (p->items[i].kind == IT_FRAME && p->items[i].y == y) p->items[i].h = rowh;
        if (row_item >= 0) p->items[row_item].h = rowh;
        y += rowh + spacing;
    }
    y += border;
    if (tbg_item >= 0) p->items[tbg_item].h = y - p->items[tbg_item].y;
    if (border) { item_t *it = add_item(p, IT_FRAME); it->x = tx; it->y = L->y; it->w = total; it->h = y - L->y; it->color = HEX(0x606060); }
    if (tx + total > L->maxx) L->maxx = tx + total;
    L->y = y; L->trail = 0; L->cx = L->x0; L->line_start = p->nitems; L->line_h = 0; L->line_asc = 0;
    kfree(cells); kfree(rowcells); kfree(colmin); kfree(colmax); kfree(colw);
}

static void layout_image(lay_t *L, node_t *n, sty_t *s) {
    page_t *p = L->pg;
    if (!n->aux) return;
    pimg_t *im = &p->imgs[n->aux - 1];
    int aw = attr_int(n, "width", 0), ah = attr_int(n, "height", 0), w, h;
    const char *ws = node_attr(n, "width"); if (ws && strchr(ws, '%')) aw = L->w * atoi(ws) / 100;
    if (im->state == 1) {
        int iw = im->img->w, ih = im->img->h;
        if (aw && ah) { w = aw; h = ah; } else if (aw) { w = aw; h = ih * aw / MAX(1, iw); } else if (ah) { h = ah; w = iw * ah / MAX(1, ih); } else { w = iw; h = ih; }
    } else if (aw && ah) { w = aw; h = ah; }
    else if (im->state == 2 && im->alt && im->alt[0]) { w = MIN(text_width(FONT_WEB_SMALL, im->alt) + 24, L->w); h = 20; }
    else if (im->state == 2) return;
    else { w = aw ? aw : 20; h = ah ? ah : 20; }
    if (w > L->w) { h = h * L->w / MAX(1, w); w = L->w; }
    if (w <= 0 || h <= 0) return;
    item_t *it = add_item(p, IT_IMG);
    it->idx = n->aux - 1; it->link = s->link;
    place_box(L, it, w, h, h);
}
static void layout_control(lay_t *L, node_t *n, sty_t *s) {
    page_t *p = L->pg;
    if (!n->aux) return;
    ctrl_t *c = &p->ctrls[n->aux - 1];
    int w = 0, h = 22;
    switch (c->type) {
    case C_HIDDEN: return;
    case C_TEXT: case C_PASSWORD: w = MIN(attr_int(n, "size", 20) * 7 + 10, L->w); break;
    case C_SUBMIT: case C_BUTTON: case C_RESET: w = text_width(FONT_UI, c->label) + 20; break;
    case C_CHECK: case C_RADIO: w = 16; h = 16; break;
    case C_SELECT: { int mw = 40; for (int i = 0; i < c->nopts; i++) mw = MAX(mw, text_width(FONT_UI, c->opts[i])); w = MIN(mw + 30, L->w); break; }
    case C_TEXTAREA: w = MIN(attr_int(n, "cols", 40) * 7 + 10, L->w); h = attr_int(n, "rows", 3) * 15 + 8; break;
    }
    item_t *it = add_item(p, IT_CTRL);
    it->idx = n->aux - 1;
    place_box(L, it, w, h, h - 5);
    L->pending_space = true;
}
static void layout_children(lay_t *L, node_t *n, sty_t *s) { for (node_t *c = n->first; c; c = c->next) layout_node(L, c, s); }
static int list_counter;
static void layout_node(lay_t *L, node_t *n, sty_t *parent) {
    if (n->type == 1) { layout_text(L, parent, n->text); return; }
    if ((n->css.set & ST_DISPLAY) && n->css.display == 0) return;
    const char *t = n->tag;
    static const char *skip[] = { "head", "template", "datalist", "map", "svg", "math", "object", "embed", "applet", "audio", "video", "canvas", "param", "option", "optgroup", "frameset" };
    for (unsigned i = 0; i < ARRAY_LEN(skip); i++) if (!strcmp(t, skip[i])) return;
    page_t *p = L->pg;
    sty_t s = *parent;
    { const char *id = node_attr(n, "id"); if (!id && !strcmp(t, "a")) id = node_attr(n, "name"); if (id && *id) add_anchor(p, id, L->y); }
    /* inline styles from tags */
    if (!strcmp(t, "b") || !strcmp(t, "strong")) s.bold = true;
    else if (!strcmp(t, "i") || !strcmp(t, "em") || !strcmp(t, "cite") || !strcmp(t, "var") || !strcmp(t, "dfn") || !strcmp(t, "address")) s.italic = true;
    else if (!strcmp(t, "u") || !strcmp(t, "ins")) s.under = true;
    else if (!strcmp(t, "code") || !strcmp(t, "tt") || !strcmp(t, "kbd") || !strcmp(t, "samp")) s.mono = true;
    else if (!strcmp(t, "small") || !strcmp(t, "sub") || !strcmp(t, "sup")) s.size = -1;
    else if (!strcmp(t, "big")) s.size = 3;
    else if (!strcmp(t, "font")) {
        uint32_t c; if (node_attr(n, "color") && parse_color(node_attr(n, "color"), &c)) s.color = c;
        const char *sz = node_attr(n, "size");
        if (sz) { int v = atoi(sz); if (sz[0] == '+') v = 3 + v; else if (sz[0] == '-') v = 3 - atoi(sz + 1); s.size = v <= 2 ? -1 : v == 3 ? 0 : v == 4 ? 3 : v == 5 ? 2 : 1; }
    } else if (!strcmp(t, "a")) {
        const char *href = node_attr(n, "href");
        if (href && *href && strncasecmp(href, "javascript:", 11)) { s.link = add_link(p, href); s.color = p->linkc; s.under = true; }
    } else if (!strcmp(t, "pre") || !strcmp(t, "xmp") || !strcmp(t, "listing")) { s.pre = true; s.mono = true; }
    else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) { int lv = t[1] - '0'; s.size = lv <= 3 ? lv : 0; s.bold = true; if (lv >= 5) s.size = -1; }
    else if (!strcmp(t, "th")) s.bold = true;
    else if (!strcmp(t, "nobr")) s.nowrap = true;
    else if (!strcmp(t, "summary") || !strcmp(t, "dt") || !strcmp(t, "legend")) s.bold = !strcmp(t, "summary") || !strcmp(t, "legend") ? true : s.bold;
    apply_css_style(n, &s);
    if (!strcmp(t, "br")) { newline(L, true, pick_font(&s)); return; }
    if (!strcmp(t, "img")) { layout_image(L, n, &s); return; }
    if (!strcmp(t, "input") || !strcmp(t, "select") || !strcmp(t, "textarea") || !strcmp(t, "button")) { layout_control(L, n, &s); return; }
    if (!strcmp(t, "iframe")) {
        const char *src = node_attr(n, "src");
        if (src) { sty_t ls = s; ls.link = add_link(p, src); ls.color = p->linkc; ls.under = true; layout_text(L, &ls, "[embedded frame]"); }
        return;
    }
    if (!strcmp(t, "hr")) {
        vspace(L, 8, FONT_WEB);
        item_t *it = add_item(p, IT_HLINE); it->x = L->x0; it->y = L->y; it->w = L->w; it->h = 2;
        L->y += 2; L->trail = 0; vspace(L, 8, FONT_WEB);
        return;
    }
    if (!strcmp(t, "table")) {
        int al = L->align;
        vspace(L, 0, FONT_WEB);
        layout_table(L, n, &s);
        L->align = al;
        return;
    }
    if (!is_block(n)) { layout_children(L, n, &s); return; }

    /* blocks */
    int top = 0, bottom = 0, indent = 0, rindent = 0;
    if (!strcmp(t, "p") || !strcmp(t, "dl") || !strcmp(t, "figure") || !strcmp(t, "details")) top = bottom = 12;
    else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) { top = t[1] == '1' ? 18 : t[1] == '2' ? 16 : 13; bottom = top - 4; }
    else if (!strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "menu") || !strcmp(t, "dir")) { indent = 32; bool nested = false; for (node_t *a = n->parent; a; a = a->parent) if (!strcmp(a->tag, "li")) nested = true; if (!nested) top = bottom = 10; }
    else if (!strcmp(t, "blockquote")) { indent = rindent = 32; top = bottom = 12; }
    else if (!strcmp(t, "dd")) indent = 32;
    else if (!strcmp(t, "pre") || !strcmp(t, "xmp") || !strcmp(t, "listing")) top = bottom = 10;
    else if (!strcmp(t, "form")) bottom = 12;
    else if (!strcmp(t, "fieldset")) { top = bottom = 8; indent = rindent = 10; }
    vspace(L, top, pick_font(&s));
    int ox0 = L->x0, ow = L->w, oal = L->align;
    L->x0 += indent; L->w = MAX(20, L->w - indent - rindent); L->cx = L->x0;
    if (!strcmp(t, "center")) L->align = 1;
    const char *al = node_attr(n, "align");
    if (al) L->align = !strcasecmp(al, "center") || !strcasecmp(al, "middle") ? 1 : !strcasecmp(al, "right") ? 2 : 0;
    if (n->css.set & ST_ALIGN) L->align = n->css.align;
    int bg_item = -1, y0 = L->y;
    if ((n->css.set & ST_BG) && strcmp(t, "body")) { item_t *it = add_item(p, IT_RECT); it->x = L->x0 - indent; it->y = L->y; it->w = L->w + indent + rindent; it->color = n->css.bg; bg_item = p->nitems - 1; L->line_start = p->nitems; }
    int save_counter = list_counter;
    if (!strcmp(t, "ol")) list_counter = attr_int(n, "start", 1);
    if (!strcmp(t, "li")) {
        int f = pick_font(&s);
        bool ordered = n->parent && !strcmp(n->parent->tag, "ol");
        item_t *it = add_item(p, IT_BULLET);
        it->font = f; it->color = s.color; it->h = font_height(f); it->asc = font_ascent(f);
        if (ordered) { it->idx = list_counter++; it->x = L->x0 - 6; } else { it->idx = -1; it->x = L->x0 - 14; }
        it->w = 0;
        if (font_height(f) > L->line_h) L->line_h = font_height(f);
        if (font_ascent(f) > L->line_asc) L->line_asc = font_ascent(f);
    }
    layout_children(L, n, &s);
    newline(L, false, pick_font(&s));
    if (!strcmp(t, "li") && L->line_start == p->nitems && p->nitems && p->items[p->nitems - 1].kind == IT_BULLET) { /* empty item */ }
    if (bg_item >= 0) p->items[bg_item].h = L->y - y0;
    list_counter = !strcmp(t, "ol") ? save_counter : list_counter;
    L->x0 = ox0; L->w = ow; L->align = oal; L->cx = L->x0;
    vspace(L, bottom, pick_font(&s));
}
static void page_layout(page_t *p, int width) {
    p->nitems = 0; p->nlinks = 0; p->nanchors = 0;
    p->lay_width = width;
    sty_t s = { FONT_WEB, p->fg, false, false, false, false, false, false, 0, -1, 0 };
    lay_t L = { p, 10, MAX(100, width - 20), 10, 10, 0, 0, 0, 0, 0, 0, false };
    list_counter = 1;
    layout_children(&L, p->doc->root, &s);
    newline(&L, false, FONT_WEB);
    p->height = L.y + 16;
}

/* ---------------- building the page model ---------------- */
static void add_options(page_t *p, ctrl_t *c, node_t *n) {
    for (node_t *o = n->first; o; o = o->next) {
        if (o->type) continue;
        if (!strcmp(o->tag, "optgroup")) { add_options(p, c, o); continue; }
        if (strcmp(o->tag, "option") || c->nopts >= 200) continue;
        c->opts = krealloc(c->opts, sizeof(char *) * (c->nopts + 1)); c->optvals = krealloc(c->optvals, sizeof(char *) * (c->nopts + 1));
        char lbl[128]; lbl[0] = 0; text_content(o, lbl, sizeof lbl, 0);
        char *lt = lbl; while (*lt == ' ' || *lt == '\n') lt++;
        c->opts[c->nopts] = arena_strdup(&p->doc->arena, lt, -1);
        const char *ov = node_attr(o, "value"); c->optvals[c->nopts] = ov ? (char *)ov : c->opts[c->nopts];
        if (node_attr(o, "selected")) c->sel = c->nopts;
        c->nopts++;
    }
}
static void collect(page_t *p, node_t *n, int form) {
    for (; n; n = n->next) {
        if (n->type) continue;
        int f = form;
        if (!strcmp(n->tag, "form")) {
            p->forms = krealloc(p->forms, sizeof(form_t) * (p->nforms + 1));
            form_t *fm = &p->forms[p->nforms];
            const char *a = node_attr(n, "action"), *m = node_attr(n, "method");
            url_resolve(p->doc->base[0] ? p->doc->base : p->url, a && *a ? a : p->url, fm->action, sizeof fm->action);
            fm->post = m && !strcasecmp(m, "post");
            f = p->nforms++;
        } else if (!strcmp(n->tag, "img")) {
            const char *src = node_attr(n, "src");
            if (!src || !*src) src = node_attr(n, "data-src");
            if (src && *src && p->nimgs < 120) {
                p->imgs = krealloc(p->imgs, sizeof(pimg_t) * (p->nimgs + 1));
                pimg_t *im = &p->imgs[p->nimgs]; memset(im, 0, sizeof *im);
                char abs[1024]; url_resolve(p->doc->base[0] ? p->doc->base : p->url, src, abs, sizeof abs);
                im->url = arena_strdup(&p->doc->arena, abs, -1);
                const char *alt = node_attr(n, "alt"); im->alt = alt ? (char *)alt : NULL;
                n->aux = ++p->nimgs;
            }
        } else if (!strcmp(n->tag, "input") || !strcmp(n->tag, "select") || !strcmp(n->tag, "textarea") || !strcmp(n->tag, "button")) {
            p->ctrls = krealloc(p->ctrls, sizeof(ctrl_t) * (p->nctrls + 1));
            ctrl_t *c = &p->ctrls[p->nctrls]; memset(c, 0, sizeof *c);
            c->node = n; c->form = form;
            const char *type = node_attr(n, "type"), *nm = node_attr(n, "name"), *v = node_attr(n, "value");
            if (nm) strlcpy(c->name, nm, sizeof c->name);
            if (v) strlcpy(c->value, v, sizeof c->value);
            if (!strcmp(n->tag, "select")) {
                c->type = C_SELECT;
                add_options(p, c, n);
            } else if (!strcmp(n->tag, "textarea")) { c->type = C_TEXTAREA; if (n->first && n->first->type == 1) strlcpy(c->value, n->first->text, sizeof c->value); }
            else if (!strcmp(n->tag, "button")) {
                c->type = type && !strcasecmp(type, "reset") ? C_RESET : type && !strcasecmp(type, "button") ? C_BUTTON : C_SUBMIT;
                char lbl[64]; lbl[0] = 0; text_content(n, lbl, sizeof lbl, 0);
                char *lt = lbl; while (*lt == ' ' || *lt == '\n') lt++;
                strlcpy(c->label, *lt ? lt : "Submit", sizeof c->label);
            } else {
                if (!type || !strcasecmp(type, "text") || !strcasecmp(type, "search") || !strcasecmp(type, "email") || !strcasecmp(type, "url") || !strcasecmp(type, "tel") || !strcasecmp(type, "number")) c->type = C_TEXT;
                else if (!strcasecmp(type, "password")) c->type = C_PASSWORD;
                else if (!strcasecmp(type, "submit") || !strcasecmp(type, "image")) { c->type = C_SUBMIT; strlcpy(c->label, v ? v : !strcasecmp(type, "image") && node_attr(n, "alt") ? node_attr(n, "alt") : "Submit", sizeof c->label); }
                else if (!strcasecmp(type, "button")) { c->type = C_BUTTON; strlcpy(c->label, v ? v : "", sizeof c->label); }
                else if (!strcasecmp(type, "reset")) { c->type = C_RESET; strlcpy(c->label, v ? v : "Reset", sizeof c->label); }
                else if (!strcasecmp(type, "checkbox")) { c->type = C_CHECK; if (!v) strcpy(c->value, "on"); c->checked = node_attr(n, "checked") != NULL; }
                else if (!strcasecmp(type, "radio")) { c->type = C_RADIO; if (!v) strcpy(c->value, "on"); c->checked = node_attr(n, "checked") != NULL; }
                else if (!strcasecmp(type, "hidden")) c->type = C_HIDDEN;
                else c->type = C_HIDDEN;
            }
            n->aux = ++p->nctrls;
        }
        collect(p, n->first, f);
    }
}
static page_t *page_from_html(const char *url, const char *html, uint32_t len, const char *charset) {
    page_t *p = kzalloc(sizeof *p);
    strlcpy(p->url, url, sizeof p->url);
    p->doc = html_parse(html, len, charset, url);
    strlcpy(p->title, p->doc->title, sizeof p->title);
    p->bg = p->doc->bgcolor ? p->doc->bgcolor : WHITE;
    p->fg = p->doc->text ? p->doc->text : BLACK;
    p->linkc = p->doc->link ? p->doc->link : HEX(0x0000EE);
    collect(p, p->doc->root->first, -1);
    p->source = kmalloc(len + 1); memcpy(p->source, html, len); p->source[len] = 0; p->srclen = len;
    return p;
}
static void page_free(page_t *p) {
    if (!p) return;
    for (int i = 0; i < p->nimgs; i++) { if (p->imgs[i].img) canvas_free(p->imgs[i].img); if (p->imgs[i].scaled) canvas_free(p->imgs[i].scaled); }
    for (int i = 0; i < p->nctrls; i++) { kfree(p->ctrls[i].opts); kfree(p->ctrls[i].optvals); }
    kfree(p->imgs); kfree(p->ctrls); kfree(p->forms); kfree(p->items); kfree(p->links); kfree(p->anchors); kfree(p->source);
    doc_free(p->doc);
    kfree(p);
}

/* ---------------- built-in pages ---------------- */
static const char *home_html =
    "<html><head><title>Home</title></head><body bgcolor=\"#F2F6FC\">"
    "<center><img src=\"wrld://logo\" width=\"96\" height=\"96\"><h1>The WRLD Xplorer</h1>"
    "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\"><input name=\"q\" size=\"46\"> <input type=\"submit\" value=\"Search the web\"></form>"
    "<p><small>Search results by DuckDuckGo. Type an address in the Address bar to visit a website.</small></p></center>"
    "<table width=\"100%\" cellpadding=\"10\"><tr><td valign=\"top\" width=\"50%\">"
    "<h3>Websites that work well</h3><ul>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a> - the free encyclopedia</li>"
    "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
    "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a> - text-only news</li>"
    "<li><a href=\"https://text.npr.org/\">NPR text edition</a></li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - news for vintage computers</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search that simplifies pages</li>"
    "<li><a href=\"https://www.gutenberg.org/\">Project Gutenberg</a> - free books</li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a></li></ul>"
    "</td><td valign=\"top\">"
    "<h3>On this computer</h3><ul>"
    "<li><a href=\"file:///C:/Samples/Welcome.html\">Welcome page</a> - what the browser can show</li>"
    "<li><a href=\"file:///C:/\">Browse drive C:</a></li></ul>"
    "<h3>About</h3><p>The WRLD Xplorer understands HTML, simple CSS, tables, forms, links and PNG, GIF, JPEG and BMP images, "
    "over HTTP and HTTPS (TLS 1.2, certificates checked against the Mozilla root store).</p>"
    "<p>It has <b>no JavaScript</b>, so interactive web apps will not work. Text-heavy sites work best.</p>"
    "</td></tr></table></body></html>";

static char *error_html(const char *url, const char *title, const char *msg) {
    char *b = kmalloc(4096);
    snprintf(b, 4096, "<html><head><title>%s</title></head><body bgcolor=\"#FFFFFF\"><table width=\"100%%\" cellpadding=\"16\"><tr><td width=\"56\" valign=\"top\">"
        "<img src=\"wrld://error\" width=\"48\" height=\"48\"></td><td><h2>%s</h2><p>%s</p><p><small>%s</small></p>"
        "<ul><li>Check the address for typing errors.</li><li>Make sure the computer is connected (run <tt>ipconfig</tt> in the Command Prompt).</li>"
        "<li>Some sites need a newer browser (TLS 1.3 or JavaScript).</li></ul><p><a href=\"%s\">Try again</a> &nbsp; <a href=\"wrld://home\">Home page</a></p></td></tr></table></body></html>",
        title, title, msg, url, url);
    return b;
}
static char *dir_listing_html(const char *path, vnode_t *d) {
    uint32_t cap = 8192, len = 0; char *b = kmalloc(cap);
    len += snprintf(b + len, cap - len, "<html><head><title>Index of %s</title></head><body><h2>Index of %s</h2><hr><table cellpadding=\"3\">", path, path);
    vfs_sort_children(d);
    for (vnode_t *c = d->child; c; c = c->next) {
        char full[600]; vfs_path(c, full, sizeof full);
        for (char *q = full; *q; q++) if (*q == '\\') *q = '/';
        if (len + 1200 > cap) { cap *= 2; b = krealloc(b, cap); }
        char sz[24] = ""; if (!c->dir) snprintf(sz, sizeof sz, "%u bytes", c->size);
        len += snprintf(b + len, cap - len, "<tr><td><a href=\"file:///%s%s\">%s%s</a></td><td align=\"right\">%s</td></tr>", full, c->dir ? "/" : "", c->name, c->dir ? "/" : "", sz);
    }
    snprintf(b + len, cap - len, "</table><hr></body></html>");
    return b;
}

/* ---------------- loading (worker task) ---------------- */
typedef struct { browser_t *b; int gen; char url[1024]; char *post; uint32_t postlen; bool push; int scroll_to; } job_t;
static void set_status(browser_t *b, const char *s) { strlcpy(b->status, s, sizeof b->status); if (b->w && !b->closed) b->w->dirty = true; }
static void progress(void *ctx, const char *st, uint32_t got, uint32_t total) {
    job_t *j = ctx;
    if (j->b->gen != j->gen) return;
    if (got) { char s[96]; if (total) snprintf(s, sizeof s, "Downloading... %u of %u KB", got / 1024, total / 1024); else snprintf(s, sizeof s, "Downloading... %u KB", got / 1024); set_status(j->b, s); }
    else set_status(j->b, st);
    j->b->prog_got = got; j->b->prog_total = total;
}
static void browser_release(browser_t *b) { if (--b->refs <= 0) { page_free(b->page); kfree(b); } }
static void install_page(browser_t *b, page_t *p, job_t *j) {
    page_t *old = b->page;
    b->page = p;
    b->scroll = j->scroll_to;
    b->hover_link = -1; b->focus_ctrl = -1;
    strlcpy(b->addr, p->url, sizeof b->addr);
    if (j->push) {
        if (b->hpos < b->nhist - 1) b->nhist = b->hpos + 1;
        if (b->nhist && !strcmp(b->hist[b->hpos], p->url)) {}
        else {
            if (b->nhist == 48) { memmove(b->hist[0], b->hist[1], sizeof b->hist[0] * 47); b->nhist--; }
            strlcpy(b->hist[b->nhist], p->url, sizeof b->hist[0]); b->hpos = b->nhist++;
        }
    }
    char t[200]; snprintf(t, sizeof t, "%s - The WRLD Xplorer", p->title[0] ? p->title : p->url);
    if (!b->closed) wm_set_title(b->w, t);
    if (old) { if (old->worker_running) old->orphan = true; else page_free(old); }
}
static int view_width(browser_t *b) { rect_t cr = b->closed ? R(0, 0, 800, 600) : R(0, 0, b->w->surf->w, b->w->surf->h); return cr.w - SB_W; }
static canvas_t *builtin_image(const char *url) {
    if (!strcmp(url, "wrld://logo")) {
        canvas_t *c = canvas_new(96, 96); gfx_clear(c, HEX(0xF2F6FC)); draw_globe(c, 0, 0, 96, -36); return c;
    }
    if (!strcmp(url, "wrld://error")) { canvas_t *c = canvas_new(48, 48); gfx_clear(c, WHITE); draw_icon(c, ICON_ERROR, 48, 0, 0); return c; }
    return NULL;
}
static uint8_t *data_url(const char *u, uint32_t *len) {
    const char *comma = strchr(u, ','); if (!comma) return NULL;
    bool b64 = strstr(u, ";base64") && strstr(u, ";base64") < comma;
    const char *s = comma + 1; uint32_t n = strlen(s);
    uint8_t *out = kmalloc(n + 1); uint32_t o = 0;
    if (b64) {
        uint32_t acc = 0; int bits = 0;
        for (; *s; s++) {
            int v = *s >= 'A' && *s <= 'Z' ? *s - 'A' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 26 : *s >= '0' && *s <= '9' ? *s - '0' + 52 : *s == '+' ? 62 : *s == '/' ? 63 : -1;
            if (v < 0) continue;
            acc = (acc << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; out[o++] = acc >> bits; }
        }
    } else for (; *s; s++) out[o++] = *s;
    *len = o;
    return out;
}
static bool load_images(browser_t *b, job_t *j, page_t *p) {
    uint32_t last_layout = timer_ticks();
    for (int i = 0; i < p->nimgs; i++) {
        if (b->gen != j->gen || b->closed) return false;
        pimg_t *im = &p->imgs[i];
        if (im->state) continue;
        char err[96];
        canvas_t *c = builtin_image(im->url);
        if (!c && !strncmp(im->url, "data:", 5)) { uint32_t l; uint8_t *d = data_url(im->url, &l); if (d) { c = image_decode(d, l, err, sizeof err); kfree(d); } }
        else if (!c && !strncasecmp(im->url, "file:", 5)) {
            const char *pth = im->url + 5; while (*pth == '/') pth++;
            uint32_t l; uint8_t *d = vfs_read_file(pth, &l);
            if (d) { c = image_decode(d, l, err, sizeof err); kfree(d); }
        } else if (!c && (!strncasecmp(im->url, "http://", 7) || !strncasecmp(im->url, "https://", 8))) {
            char s[300]; snprintf(s, sizeof s, "Downloading picture %d of %d...", i + 1, p->nimgs); set_status(b, s);
            http_response_t r;
            if (http_get(im->url, &r, NULL, NULL) && r.status == 200 && r.body_len) c = image_decode(r.body, r.body_len, err, sizeof err);
            http_free(&r);
        }
        if (b->gen != j->gen || b->closed) { if (c) canvas_free(c); return false; }
        if (c) { im->img = c; im->state = 1; } else im->state = 2;
        if (timer_ticks() - last_layout > 400 || i == p->nimgs - 1) {
            page_layout(p, p->lay_width ? p->lay_width : view_width(b));
            if (!b->closed) b->w->dirty = true;
            last_layout = timer_ticks();
        }
    }
    return true;
}
static int find_anchor_y(page_t *p, const char *frag) {
    if (!frag || !*frag) return 0;
    char f[256]; int k = 0;     /* undo %20 etc. */
    for (const char *q = frag; *q && k < 255; q++) { if (*q == '%' && isxdigit(q[1]) && isxdigit(q[2])) { char h[3] = { q[1], q[2], 0 }; f[k++] = strtoul(h, NULL, 16); q += 2; } else f[k++] = *q; }
    f[k] = 0;
    for (int i = 0; i < p->nanchors; i++) if (!strcmp(p->anchors[i].name, f)) return MAX(0, p->anchors[i].y - 6);
    return 0;
}
static void nav_worker(void *arg) {
    job_t *j = arg;
    browser_t *b = j->b;
    page_t *p = NULL;
    char *url = j->url;
    if (!strcasecmp(url, HOME_URL) || !strcasecmp(url, "about:home")) p = page_from_html(HOME_URL, home_html, strlen(home_html), "utf-8");
    else if (!strcasecmp(url, "about:blank")) p = page_from_html(url, "<html><body></body></html>", 26, NULL);
    else if (!strncasecmp(url, "file:", 5)) {
        const char *pth = url + 5; while (*pth == '/') pth++;
        char vp[512]; strlcpy(vp, pth, sizeof vp);
        for (char *q = vp; *q; q++) { if (*q == '/') *q = '\\'; if (*q == '%' && q[1] == '2' && q[2] == '0') { *q = ' '; memmove(q + 1, q + 3, strlen(q + 3) + 1); } }
        int vl = strlen(vp); if (vl > 3 && vp[vl - 1] == '\\') vp[vl - 1] = 0;
        vnode_t *n = vfs_resolve(vp, NULL);
        if (!n) { char *e = error_html(url, "File not found", "The file or folder could not be found on this computer."); p = page_from_html(url, e, strlen(e), NULL); kfree(e); }
        else if (n->dir) { char *h = dir_listing_html(vp, n); p = page_from_html(url, h, strlen(h), NULL); kfree(h); }
        else {
            const char *ext = vfs_ext(n->name);
            if (!strcasecmp(ext, "html") || !strcasecmp(ext, "htm")) p = page_from_html(url, (const char *)n->data, n->size, NULL);
            else if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") || !strcasecmp(ext, "gif") || !strcasecmp(ext, "bmp")) {
                char h[1400]; snprintf(h, sizeof h, "<html><head><title>%s</title></head><body><center><img src=\"%s\"></center></body></html>", n->name, url);
                p = page_from_html(url, h, strlen(h), NULL);
            } else {
                uint32_t cap = n->size * 5 + 200; char *h = kmalloc(cap); int o = snprintf(h, cap, "<html><head><title>%s</title></head><body><pre>", n->name);
                for (uint32_t i = 0; i < n->size; i++) { char ch = n->data[i]; if (ch == '<') o += snprintf(h + o, cap - o, "&lt;"); else if (ch == '&') o += snprintf(h + o, cap - o, "&amp;"); else h[o++] = ch; }
                o += snprintf(h + o, cap - o, "</pre></body></html>");
                p = page_from_html(url, h, o, NULL); kfree(h);
            }
        }
    } else {
        http_response_t r;
        bool ok = j->post ? http_request("POST", url, "application/x-www-form-urlencoded", j->post, j->postlen, &r, progress, j)
                          : http_get(url, &r, progress, j);
        if (b->gen != j->gen || b->closed) { http_free(&r); goto out; }
        if (!ok) {
            char *e = error_html(url, "The page cannot be displayed", r.error);
            p = page_from_html(url, e, strlen(e), NULL); kfree(e);
        } else {
            const char *ct = r.content_type;
            bool html = !ct[0] || strstr(ct, "html") || strstr(ct, "xml");
            if (html) p = page_from_html(r.url, (const char *)r.body, r.body_len, r.charset[0] ? r.charset : NULL);
            else if (!strncmp(ct, "text/", 5)) {
                uint32_t cap = r.body_len * 2 + 128; char *h = kmalloc(cap); int o = snprintf(h, cap, "<html><body><pre>");
                for (uint32_t i = 0; i < r.body_len && o < (int)cap - 32; i++) { char c = r.body[i]; if (c == '<') o += snprintf(h + o, cap - o, "&lt;"); else if (c == '&') o += snprintf(h + o, cap - o, "&amp;"); else h[o++] = c; }
                o += snprintf(h + o, cap - o, "</pre></body></html>");
                p = page_from_html(r.url, h, o, r.charset[0] ? r.charset : NULL); kfree(h);
            } else if (!strncmp(ct, "image/", 6)) {
                char h[1400]; snprintf(h, sizeof h, "<html><body bgcolor=\"#E8E8E8\"><center><img src=\"%s\"></center></body></html>", r.url);
                p = page_from_html(r.url, h, strlen(h), NULL);
                char err[96];
                if (p->nimgs) { canvas_t *c = image_decode(r.body, r.body_len, err, sizeof err); p->imgs[0].img = c; p->imgs[0].state = c ? 1 : 2; }
            } else {
                /* anything else is a download */
                char sch[8], host[256], path[1024]; uint16_t port;
                url_parse(r.url, sch, host, sizeof host, &port, path, sizeof path);
                char *q = strchr(path, '?'); if (q) *q = 0;
                const char *base = strrchr(path, '/'); base = base && base[1] ? base + 1 : "download.bin";
                char dst[600]; snprintf(dst, sizeof dst, USER_DOCS "\\Downloads\\%s", base);
                vfs_mkdir_p(USER_DOCS "\\Downloads");
                vfs_write_file(dst, r.body, r.body_len);
                char *h = kmalloc(2048);
                snprintf(h, 2048, "<html><head><title>Download complete</title></head><body><h2>Download complete</h2><p>Saved <b>%s</b> (%u bytes) to <a href=\"file:///C:/Documents and Settings/User/My Documents/Downloads/\">My Documents\\Downloads</a>.</p></body></html>", base, r.body_len);
                p = page_from_html(r.url, h, strlen(h), NULL); kfree(h);
            }
            if (p) { p->secure = r.secure; strlcpy(p->tls, r.tls_info, sizeof p->tls); }
        }
        http_free(&r);
    }
    if (!p || b->gen != j->gen || b->closed) { page_free(p); goto out; }
    p->worker_running = true;
    page_layout(p, view_width(b));
    { const char *h = strchr(j->url, '#'); j->scroll_to = h ? find_anchor_y(p, h + 1) : j->scroll_to; }
    install_page(b, p, j);
    set_status(b, "Opening page...");
    if (!b->closed) b->w->dirty = true;
    bool complete = load_images(b, j, p);
    if (complete && b->gen == j->gen && !b->closed) {
        char s[160];
        if (p->secure) snprintf(s, sizeof s, "Done - secure connection (%s)", p->tls); else strcpy(s, "Done");
        set_status(b, s);
        b->loading = false;
        /* <meta http-equiv="refresh"> to another page */
        if (p->doc->refresh[0] && p->doc->refresh_s <= 5) {
            task_sleep(p->doc->refresh_s * 1000);
            if (b->gen == j->gen && !b->closed) {
                char to[1024]; url_resolve(p->url, p->doc->refresh, to, sizeof to);
                extern void browser_navigate_ext(browser_t *b, const char *url, const char *post, bool push);
                browser_navigate_ext(b, to, NULL, false);
            }
        }
    }
    p->worker_running = false;
    if (p->orphan) page_free(p);
out:
    kfree(j->post);
    if (b->gen == j->gen) b->loading = false;
    browser_release(b);
    kfree(j);
    task_exit(0);
}
void browser_navigate_ext(browser_t *b, const char *url, const char *post, bool push) {
    job_t *j = kzalloc(sizeof *j);
    j->b = b; j->gen = ++b->gen; j->push = push;
    strlcpy(j->url, url, sizeof j->url);
    if (post) { j->post = strdup(post); j->postlen = strlen(post); }
    b->refs++;
    b->loading = true;
    strlcpy(b->addr, url, sizeof b->addr);
    set_status(b, "Opening page...");
    task_create("xplorer", nav_worker, j, 65536);
}
static void normalize_address(const char *in, char *out, int cap) {
    while (*in == ' ') in++;
    if (!*in) { strlcpy(out, HOME_URL, cap); return; }
    if (strstr(in, "://") || !strncasecmp(in, "about:", 6) || !strncasecmp(in, "wrld:", 5) || !strncasecmp(in, "file:", 5)) { strlcpy(out, in, cap); return; }
    if (isalpha(in[0]) && in[1] == ':') { snprintf(out, cap, "file:///%s", in); for (char *q = out; *q; q++) if (*q == '\\') *q = '/'; return; }
    bool looks_like_host = strchr(in, '.') && !strchr(in, ' ');
    if (looks_like_host || !strncasecmp(in, "localhost", 9)) { snprintf(out, cap, "https://%s", in); return; }
    /* anything else: search */
    char q[700]; int o = 0;
    for (const char *p = in; *p && o < 690; p++) { if (isalnum(*p)) q[o++] = *p; else if (*p == ' ') q[o++] = '+'; else o += snprintf(q + o, sizeof q - o, "%%%02X", (uint8_t)*p); }
    q[o] = 0;
    snprintf(out, cap, "https://html.duckduckgo.com/html/?q=%s", q);
}

/* ---------------- forms ---------------- */
static void urlenc(char *out, int *o, int cap, const char *s) {
    for (; *s && *o < cap - 4; s++) {
        uint8_t c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[(*o)++] = c;
        else if (c == ' ') out[(*o)++] = '+';
        else *o += snprintf(out + *o, cap - *o, "%%%02X", c);
    }
    out[*o] = 0;
}
static void submit_form(browser_t *b, int form, int submitter) {
    page_t *p = b->page;
    if (form < 0 || form >= p->nforms) return;
    char q[4096]; int o = 0; q[0] = 0;
    for (int i = 0; i < p->nctrls; i++) {
        ctrl_t *c = &p->ctrls[i];
        if (c->form != form || !c->name[0]) continue;
        const char *v = NULL;
        switch (c->type) {
        case C_TEXT: case C_PASSWORD: case C_HIDDEN: case C_TEXTAREA: v = c->value; break;
        case C_CHECK: case C_RADIO: if (c->checked) v = c->value; break;
        case C_SELECT: if (c->nopts) v = c->optvals[c->sel]; break;
        case C_SUBMIT: if (i == submitter) v = c->value[0] ? c->value : c->label; break;
        }
        if (!v) continue;
        if (o) q[o++] = '&';
        urlenc(q, &o, sizeof q, c->name); q[o++] = '='; urlenc(q, &o, sizeof q, v);
    }
    form_t *f = &p->forms[form];
    if (f->post) browser_navigate_ext(b, f->action, q, true);
    else {
        char url[1400]; strlcpy(url, f->action, sizeof url);
        char *qm = strchr(url, '?'); if (qm) *qm = 0;
        char *hash = strchr(url, '#'); if (hash) *hash = 0;
        strlcat(url, "?", sizeof url); strlcat(url, q, sizeof url);
        browser_navigate_ext(b, url, NULL, true);
    }
}

/* ---------------- painting ---------------- */
static rect_t content_rect(window_t *w) { return R(0, TOOLBAR_H + ADDR_H, w->surf->w - SB_W, w->surf->h - TOOLBAR_H - ADDR_H - STATUS_H); }
static void draw_img(canvas_t *c, pimg_t *im, int x, int y, int w, int h) {
    if (im->state != 1) {
        draw_rect(c, x, y, w, h, HEX(0xA0A0A0));
        if (w > 20 && h > 18) { draw_icon(c, ICON_IMAGE_FILE, 16, x + 2, y + 2); if (im->alt && w > 40) { rect_t old = canvas_push_clip(c, R(x, y, w, h)); draw_text(c, FONT_WEB_SMALL, x + 21, y + 3, im->alt, HEX(0x404040)); c->clip = old; } }
        return;
    }
    canvas_t *src = im->img;
    if (src->w != w || src->h != h) {
        if (!im->scaled || im->scaled->w != w || im->scaled->h != h) {
            if (im->scaled) canvas_free(im->scaled);
            im->scaled = canvas_new(w, h);
            for (int j = 0; j < h; j++) {
                int sy = j * src->h / h;
                for (int i = 0; i < w; i++) im->scaled->px[j * w + i] = src->px[sy * src->stride + i * src->w / w];
            }
        }
        src = im->scaled;
    }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < c->clip.y || py >= c->clip.y + c->clip.h) continue;
        for (int i = 0; i < w; i++) {
            uint32_t pxl = src->px[j * src->stride + i]; int a = pxl >> 24;
            if (a == 255) put_pixel(c, x + i, py, pxl); else if (a) blend_pixel(c, x + i, py, pxl | 0xFF000000, a);
        }
    }
}
static void draw_ctrl(canvas_t *c, browser_t *b, int idx, int x, int y, int w, int h) {
    ctrl_t *ct = &b->page->ctrls[idx];
    bool focus = b->focus_ctrl == idx;
    switch (ct->type) {
    case C_TEXT: case C_PASSWORD: case C_TEXTAREA: {
        draw_sunken_box(c, R(x, y, w, h), WHITE);
        rect_t old = canvas_push_clip(c, R(x + 2, y + 2, w - 4, h - 4));
        char shown[512]; strlcpy(shown, ct->value, sizeof shown);
        if (ct->type == C_PASSWORD) for (char *q = shown; *q; q++) *q = '*';
        int tw = text_width(FONT_UI, shown), off = tw > w - 12 ? tw - (w - 12) : 0;
        if (ct->type == C_TEXTAREA) draw_text_wrapped(c, FONT_UI, x + 4, y + 4, w - 8, shown, BLACK, 2);
        else draw_text(c, FONT_UI, x + 4 - off, y + (h - font_height(FONT_UI)) / 2, shown, BLACK);
        if (focus && (timer_ticks() / 500) % 2 == 0 && ct->type != C_TEXTAREA) vline(c, x + 4 - off + tw, y + 4, h - 8, BLACK);
        c->clip = old;
        break;
    }
    case C_SUBMIT: case C_BUTTON: case C_RESET: draw_button(c, R(x, y, w, h), ct->label, false, false, focus, true); break;
    case C_CHECK: draw_check(c, x + 1, y + 1, ct->checked, false); break;
    case C_RADIO: draw_radio(c, x + 1, y + 1, ct->checked, false); break;
    case C_SELECT:
        draw_sunken_box(c, R(x, y, w, h), WHITE);
        if (ct->nopts) draw_text_ellipsis(c, FONT_UI, x + 4, y + (h - font_height(FONT_UI)) / 2, w - 24, ct->opts[ct->sel], BLACK);
        fill_rect(c, x + w - 18, y + 2, 16, h - 4, T.face); draw_arrow(c, x + w - 10, y + h / 2 - 1, 1, BLACK);
        break;
    }
}
static void paint_content(browser_t *b, canvas_t *c, rect_t v) {
    page_t *p = b->page;
    rect_t old = canvas_push_clip(c, v);
    fill_rect(c, v.x, v.y, v.w, v.h, p ? p->bg : WHITE);
    if (!p) { c->clip = old; return; }
    int oy = v.y - b->scroll, ox = v.x;
    for (int i = 0; i < p->nitems; i++) {
        item_t *it = &p->items[i];
        int y = it->y + oy, x = it->x + ox;
        if (y > v.y + v.h || y + MAX(it->h, 1) < v.y) continue;
        switch (it->kind) {
        case IT_TEXT: {
            bool hot = it->link >= 0 && it->link == b->hover_link;
            draw_text_n(c, it->font, x, y, it->text, it->len, hot ? HEX(0xE04000) : it->color);
            if (it->under || hot) hline(c, x, y + it->asc + 2, it->w, hot ? HEX(0xE04000) : it->color);
            break;
        }
        case IT_RECT: fill_rect(c, x, y, it->w, it->h, it->color); break;
        case IT_FRAME: draw_rect(c, x, y, it->w, it->h, it->color); break;
        case IT_HLINE: hline(c, x, y, it->w, HEX(0xA0A0A0)); hline(c, x, y + 1, it->w, HEX(0xF0F0F0)); break;
        case IT_IMG: draw_img(c, &p->imgs[it->idx], x, y, it->w, it->h); if (it->link >= 0 && it->link == b->hover_link) draw_rect(c, x, y, it->w, it->h, HEX(0xE04000)); break;
        case IT_BULLET:
            if (it->idx < 0) fill_circle(c, x + 3, y + it->asc - font_height(it->font) / 3 - 1, 2, it->color);
            else { char n[16]; snprintf(n, sizeof n, "%d.", it->idx); draw_text(c, it->font, x - text_width(it->font, n), y, n, it->color); }
            break;
        case IT_CTRL: draw_ctrl(c, b, it->idx, x, y, it->w, it->h); p->ctrls[it->idx].r = R(it->x, it->y, it->w, it->h); break;
        }
    }
    c->clip = old;
}
static void clamp_scroll(browser_t *b, rect_t v) {
    int maxs = b->page ? MAX(0, b->page->height - v.h) : 0;
    b->scroll = CLAMP(b->scroll, 0, maxs);
}
static void go_back(browser_t *b) { if (b->hpos > 0) { b->hpos--; browser_navigate_ext(b, b->hist[b->hpos], NULL, false); } }
static void go_fwd(browser_t *b) { if (b->hpos < b->nhist - 1) { b->hpos++; browser_navigate_ext(b, b->hist[b->hpos], NULL, false); } }
static void stop_loading(browser_t *b) { b->gen++; b->loading = false; set_status(b, "Stopped"); }
static void br_paint(window_t *w) {
    browser_t *b = w->data;
    canvas_t *c = w->surf;
    /* toolbar */
    if (T.classic) fill_rect(c, 0, 0, c->w, TOOLBAR_H + ADDR_H, T.face);
    else gradient_v(c, 0, 0, c->w, TOOLBAR_H + ADDR_H, lighten(T.face, 120), T.face);
    hline(c, 0, TOOLBAR_H - 1, c->w, darken(T.face, 30)); hline(c, 0, TOOLBAR_H, c->w, lighten(T.face, 160));
    int x = 6;
    bool canb = b->hpos > 0, canf = b->hpos < b->nhist - 1;
    if (ui_icon_button(w, R(x, 4, 72, 32), ICON_BACK, "Back") && canb) go_back(b);
    if (!canb) blend_rect(c, x + 2, 6, 68, 28, T.face, 140);
    x += 76;
    if (ui_icon_button(w, R(x, 4, 32, 32), ICON_FORWARD, NULL) && canf) go_fwd(b);
    if (!canf) blend_rect(c, x + 2, 6, 28, 28, T.face, 140);
    x += 38;
    if (ui_icon_button(w, R(x, 4, 32, 32), ICON_STOP, NULL)) stop_loading(b);
    x += 34;
    if (ui_icon_button(w, R(x, 4, 32, 32), ICON_REFRESH, NULL)) browser_navigate_ext(b, b->page ? b->page->url : b->addr, NULL, false);
    x += 34;
    if (ui_icon_button(w, R(x, 4, 32, 32), ICON_HOME, NULL)) browser_navigate_ext(b, HOME_URL, NULL, true);
    x += 40;
    vline(c, x, 6, 28, darken(T.face, 40)); vline(c, x + 1, 6, 28, WHITE);
    /* spinning globe while loading */
    draw_icon(c, ICON_BROWSER, 32, c->w - 40, 4);
    if (b->loading) { int f = (timer_ticks() / 120) % 8; fill_circle(c, c->w - 24 + (f < 4 ? f * 3 - 6 : 6 - (f - 4) * 3), 30, 2, HEX(0xF08A24)); }
    /* address bar */
    int ay = TOOLBAR_H + 3;
    draw_text(c, FONT_UI, 8, ay + 5, "Address", HEX(0x404040));
    rect_t ab = R(62, ay, c->w - 62 - 74, 22);
    bool enter = ui_textbox(w, ab, b->addr, sizeof b->addr);
    if (b->page && b->page->secure) draw_icon(c, ICON_LOCK, 16, ab.x + ab.w - 20, ab.y + 3);
    if (ui_icon_button(w, R(c->w - 68, ay - 1, 60, 24), ICON_GO, "Go") || enter) {
        char u[1024]; normalize_address(b->addr, u, sizeof u);
        browser_navigate_ext(b, u, NULL, true);
        w->ui.focus = 0;
    }
    /* page */
    rect_t v = content_rect(w);
    if (b->page && b->page->lay_width != v.w) page_layout(b->page, v.w);
    clamp_scroll(b, v);
    paint_content(b, c, v);
    draw_scrollbar(c, R(v.x + v.w, v.y, SB_W, v.h), b->page ? MAX(b->page->height, v.h) : v.h, v.h, b->scroll, true);
    /* status bar */
    int sy = c->h - STATUS_H;
    fill_rect(c, 0, sy, c->w, STATUS_H, T.face);
    hline(c, 0, sy, c->w, darken(T.face, 40));
    const char *st = b->hover_link >= 0 && b->page && b->hover_link < b->page->nlinks ? b->page->links[b->hover_link] : b->status;
    draw_icon(c, b->loading ? ICON_BROWSER : ICON_INFO, 16, 4, sy + 3);
    draw_text_ellipsis(c, FONT_UI, 24, sy + 4, c->w - 200, st, BLACK);
    vline(c, c->w - 170, sy + 3, STATUS_H - 6, darken(T.face, 40));
    if (b->page && b->page->secure) draw_icon(c, ICON_LOCK, 16, c->w - 162, sy + 3);
    draw_icon(c, ICON_CP_NETWORK, 16, c->w - 130, sy + 3);
    draw_text(c, FONT_UI, c->w - 110, sy + 4, netif.configured ? "Internet" : "Offline", BLACK);
}
static int item_at(page_t *p, int x, int y, int kind_ctrl) {
    if (!p) return -1;
    for (int i = p->nitems - 1; i >= 0; i--) {
        item_t *it = &p->items[i];
        if (kind_ctrl ? it->kind != IT_CTRL : it->link < 0) continue;
        if (x >= it->x && x < it->x + it->w && y >= it->y - 1 && y < it->y + it->h + 1) return kind_ctrl ? it->idx : it->link;
    }
    return -1;
}
static void select_cb(void *ctx, int id) {
    browser_t *b = ctx;
    if (b->page && b->focus_ctrl >= 0 && b->focus_ctrl < b->page->nctrls) { b->page->ctrls[b->focus_ctrl].sel = id; b->w->dirty = true; }
}
static void br_mouse(window_t *w, int type, int mx, int my, int btn) {
    browser_t *b = w->data;
    rect_t v = content_rect(w);
    if (b->drag_sb) {
        if (type == ME_UP) b->drag_sb = false;
        else if (type == ME_MOVE && b->page) { int total = MAX(b->page->height, v.h); b->scroll = (int)((int64_t)(my - v.y - b->drag_off) * total / v.h); clamp_scroll(b, v); w->dirty = true; }
        return;
    }
    if (type == ME_WHEEL) { b->scroll += btn * 48; clamp_scroll(b, v); w->dirty = true; return; }
    if (mx >= v.x + v.w && mx < v.x + v.w + SB_W && my >= v.y && my < v.y + v.h && type == ME_DOWN && b->page) {
        int total = MAX(b->page->height, v.h), th = MAX(20, v.h * v.h / total), ty = v.y + (int)((int64_t)b->scroll * v.h / total);
        if (my >= ty && my < ty + th) { b->drag_sb = true; b->drag_off = my - ty; }
        else b->scroll += my < ty ? -v.h + 30 : v.h - 30;
        clamp_scroll(b, v); w->dirty = true; return;
    }
    if (!pt_in(v, mx, my)) { if (b->hover_link >= 0) { b->hover_link = -1; w->dirty = true; } w->cursor = CURSOR_ARROW; return; }
    int px = mx - v.x, py = my - v.y + b->scroll;
    int link = item_at(b->page, px, py, 0), ctl = item_at(b->page, px, py, 1);
    if (type == ME_MOVE) {
        if (link != b->hover_link) { b->hover_link = link; w->dirty = true; }
        w->cursor = link >= 0 ? CURSOR_HAND : ctl >= 0 && b->page && (b->page->ctrls[ctl].type == C_TEXT || b->page->ctrls[ctl].type == C_PASSWORD || b->page->ctrls[ctl].type == C_TEXTAREA) ? CURSOR_IBEAM : CURSOR_ARROW;
        return;
    }
    if (type == ME_DOWN && btn == MB_LEFT) {
        w->ui.focus = 0;                          /* leave the address bar */
        b->focus_ctrl = ctl;
        if (ctl >= 0) {
            ctrl_t *c = &b->page->ctrls[ctl];
            if (c->type == C_CHECK) c->checked = !c->checked;
            else if (c->type == C_RADIO) { for (int i = 0; i < b->page->nctrls; i++) { ctrl_t *o = &b->page->ctrls[i]; if (o->type == C_RADIO && o->form == c->form && !strcmp(o->name, c->name)) o->checked = false; } c->checked = true; }
            else if (c->type == C_SUBMIT) submit_form(b, c->form, ctl);
            else if (c->type == C_RESET) { for (int i = 0; i < b->page->nctrls; i++) if (b->page->ctrls[i].form == c->form && (b->page->ctrls[i].type == C_TEXT || b->page->ctrls[i].type == C_TEXTAREA)) b->page->ctrls[i].value[0] = 0; }
            else if (c->type == C_SELECT && c->nopts) {
                menu_item_t mi[20]; int k = MIN(c->nopts, 20);
                for (int i = 0; i < k; i++) { mi[i].label = c->opts[i]; mi[i].id = i; mi[i].flags = i == c->sel ? MI_CHECKED | MI_RADIO : 0; }
                rect_t cr = wm_client_rect(w);
                popup_menu(cr.x + v.x + c->r.x, cr.y + v.y + c->r.y - b->scroll + c->r.h, mi, k, select_cb, b);
            }
            w->dirty = true;
            return;
        }
        if (link >= 0) {
            const char *href = b->page->links[link];
            if (!strncasecmp(href, "mailto:", 7)) { set_status(b, "E-mail links are not supported."); return; }
            const char *hash = strchr(href, '#');
            if (hash) {
                int base_len = hash - href, cur_len = strchr(b->page->url, '#') ? (int)(strchr(b->page->url, '#') - b->page->url) : (int)strlen(b->page->url);
                if (base_len == cur_len && !strncmp(href, b->page->url, base_len)) { b->scroll = find_anchor_y(b->page, hash + 1); clamp_scroll(b, v); w->dirty = true; return; }
            }
            browser_navigate_ext(b, href, NULL, true);
        }
        w->dirty = true;
    }
}
static void br_key(window_t *w, key_event_t *e) {
    browser_t *b = w->data;
    if (!e->pressed) return;
    rect_t v = content_rect(w);
    if ((e->mods & MOD_ALT) && e->key == KEY_LEFT) { go_back(b); return; }
    if ((e->mods & MOD_ALT) && e->key == KEY_RIGHT) { go_fwd(b); return; }
    if (e->key == KEY_F5) { browser_navigate_ext(b, b->page ? b->page->url : b->addr, NULL, false); return; }
    if (w->ui.focus) return;                      /* typing into the address bar */
    if (b->page && b->focus_ctrl >= 0 && b->focus_ctrl < b->page->nctrls) {
        ctrl_t *c = &b->page->ctrls[b->focus_ctrl];
        if (c->type == C_TEXT || c->type == C_PASSWORD || c->type == C_TEXTAREA) {
            int l = strlen(c->value);
            if (e->key == KEY_BACKSPACE) { if (l) c->value[l - 1] = 0; }
            else if (e->key == KEY_ENTER) { if (c->type == C_TEXTAREA) { if (l < (int)sizeof c->value - 1) { c->value[l] = '\n'; c->value[l + 1] = 0; } } else submit_form(b, c->form, -1); }
            else if (e->ch >= 32 && (uint8_t)e->ch < 255 && l < (int)sizeof c->value - 1 && !(e->mods & MOD_CTRL)) { c->value[l] = e->ch; c->value[l + 1] = 0; }
            w->dirty = true;
            return;
        }
        if ((c->type == C_SUBMIT) && (e->key == KEY_ENTER || e->key == ' ')) { submit_form(b, c->form, b->focus_ctrl); return; }
    }
    switch (e->key) {
    case KEY_UP: b->scroll -= 40; break;
    case KEY_DOWN: b->scroll += 40; break;
    case KEY_PGUP: b->scroll -= v.h - 40; break;
    case KEY_PGDN: case ' ': b->scroll += v.h - 40; break;
    case KEY_HOME: b->scroll = 0; break;
    case KEY_END: b->scroll = 1 << 28; break;
    case KEY_BACKSPACE: go_back(b); return;
    default: return;
    }
    clamp_scroll(b, v); w->dirty = true;
}
static void br_tick(window_t *w) { browser_t *b = w->data; if (b->loading || b->focus_ctrl >= 0) w->dirty = true; }
static void br_destroy(window_t *w) { browser_t *b = w->data; b->closed = true; b->gen++; b->w = NULL; browser_release(b); }
enum { BM_NEW = 1, BM_OPEN, BM_SAVE, BM_CLOSE, BM_SOURCE, BM_REFRESH, BM_STOP, BM_BACK, BM_FWD, BM_HOME, BM_ABOUT, BM_FAV0 = 100 };
static const menu_t br_menus[] = {
    { "File", { { "New Window", BM_NEW, 0 }, { "Open Location...", BM_OPEN, 0 }, { "Save Page As...", BM_SAVE, 0 }, { "-", 0, 0 }, { "Close", BM_CLOSE, 0 }, { NULL } } },
    { "View", { { "Stop", BM_STOP, 0 }, { "Refresh", BM_REFRESH, 0 }, { "-", 0, 0 }, { "Page Source", BM_SOURCE, 0 }, { NULL } } },
    { "Go", { { "Back", BM_BACK, 0 }, { "Forward", BM_FWD, 0 }, { "Home Page", BM_HOME, 0 }, { NULL } } },
    { "Favorites", { { "Wikipedia", BM_FAV0, 0 }, { "Hacker News", BM_FAV0 + 1, 0 }, { "CNN Lite", BM_FAV0 + 2, 0 }, { "NPR text edition", BM_FAV0 + 3, 0 },
                     { "68k.news", BM_FAV0 + 4, 0 }, { "FrogFind", BM_FAV0 + 5, 0 }, { NULL } } },
    { "Help", { { "About The WRLD Xplorer", BM_ABOUT, 0 }, { NULL } } },
};
static const char *favs[] = { "https://en.wikipedia.org/wiki/Main_Page", "https://news.ycombinator.com/", "https://lite.cnn.com/", "https://text.npr.org/", "http://68k.news/", "http://frogfind.com/" };
static void br_menu(window_t *w, int id) {
    browser_t *b = w->data;
    switch (id) {
    case BM_NEW: browser_open(NULL); break;
    case BM_OPEN: w->ui.focus = 0; b->addr[0] = 0; w->dirty = true; break;
    case BM_SAVE: case BM_SOURCE:
        if (b->page && b->page->source) {
            char path[300];
            if (id == BM_SAVE) { snprintf(path, sizeof path, USER_DOCS "\\%s.html", b->page->title[0] ? "Saved page" : "page"); }
            else strcpy(path, "C:\\tmp\\source.html.txt");
            vfs_write_file(path, b->page->source, b->page->srclen);
            if (id == BM_SOURCE) notepad_open(path); else { char m[400]; snprintf(m, sizeof m, "The page was saved as %s", path); msgbox("Save Page", m, MBI_INFO, MBX_OK, NULL, NULL); }
        }
        break;
    case BM_CLOSE: wm_close(w); break;
    case BM_STOP: stop_loading(b); break;
    case BM_REFRESH: browser_navigate_ext(b, b->page ? b->page->url : b->addr, NULL, false); break;
    case BM_BACK: go_back(b); break;
    case BM_FWD: go_fwd(b); break;
    case BM_HOME: browser_navigate_ext(b, HOME_URL, NULL, true); break;
    case BM_ABOUT: {
        char m[600];
        snprintf(m, sizeof m, "The WRLD Xplorer 1.0\n\nA web browser for " OS_NAME ": HTML, simple CSS, tables, forms and PNG/GIF/JPEG/BMP images over HTTP and HTTPS.\n\n"
                 "Secure connections use BearSSL (TLS 1.2) with %d trusted root certificates.\nThere is no JavaScript engine.", tls_trust_anchor_count());
        msgbox("About The WRLD Xplorer", m, MBI_INFO, MBX_OK, NULL, NULL);
        break;
    }
    default: if (id >= BM_FAV0 && id < BM_FAV0 + (int)ARRAY_LEN(favs)) browser_navigate_ext(b, favs[id - BM_FAV0], NULL, true);
    }
}
void browser_open(const char *url) {
    browser_t *b = kzalloc(sizeof *b);
    b->refs = 1; b->hover_link = -1; b->focus_ctrl = -1; b->hpos = -1;
    window_t *w = wm_create("The WRLD Xplorer", -1, -1, 900, 640, WF_RESIZABLE);
    b->w = w;
    w->data = b; w->paint = br_paint; w->mouse = br_mouse; w->key = br_key; w->destroy = br_destroy; w->menu_cmd = br_menu;
    w->menus = (menu_t *)br_menus; w->nmenus = ARRAY_LEN(br_menus);
    w->icon = ICON_BROWSER; w->min_w = 420; w->min_h = 300;
    wm_set_timer(w, 250, br_tick);
    char u[1024]; normalize_address(url && *url ? url : HOME_URL, u, sizeof u);
    browser_navigate_ext(b, u, NULL, true);
}
