/* SVG for The WRLD Browser: the drawing elements icons and logos use, rendered
 * through the kernel's path rasteriser (raster.c).
 *
 * Supported: <svg> with viewBox and preserveAspectRatio's default (xMidYMid
 * meet), <g>, <path> (every command, elliptical arcs included), <rect> (with
 * rx/ry), <circle>, <ellipse>, <line>, <polyline>, <polygon> and <use>; fill,
 * stroke, stroke-width, fill-rule, opacity, fill-opacity and stroke-opacity from
 * attributes, style="" or inherited from a parent; currentColor; transform
 * (matrix, translate, scale, rotate, skewX, skewY). A gradient fill is drawn in
 * its first stop's colour. Text, filters, masks and clip paths are not drawn. */
#include "kernel.h"
#include "gfx.h"
#include "raster.h"
#include "browser.h"
#include "svg.h"

/* ---------------- small float maths (the kernel has no libm) ---------------- */
#define PI_F 3.14159265f
static float fabs_(float x) { return x < 0 ? -x : x; }
static float sqrt_(float x) {
    if (x <= 0) return 0;
    float r = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 20; i++) r = 0.5f * (r + x / r);
    return r;
}
static float sin_(float x) {
    while (x > PI_F) x -= 2 * PI_F;
    while (x < -PI_F) x += 2 * PI_F;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static float cos_(float x) { return sin_(x + PI_F / 2); }
static float atan2_(float y, float x) {
    if (x == 0 && y == 0) return 0;
    float ax = fabs_(x), ay = fabs_(y), a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = PI_F / 2 - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}

/* ---------------- attributes and style ---------------- */
static const char *sattr(node_t *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcasecmp(n->attr[i * 2], name)) return n->attr[i * 2 + 1];
    return NULL;
}
/* a property from style="a: b; ..." or the attribute of the same name */
static bool sprop(node_t *n, const char *name, char *out, int cap) {
    const char *st = sattr(n, "style");
    if (st) {
        int nl = strlen(name);
        for (const char *p = st; *p; p++) {
            while (*p == ' ' || *p == ';') p++;
            if (!strncasecmp(p, name, nl)) {
                const char *q = p + nl; while (*q == ' ') q++;
                if (*q == ':') {
                    q++; while (*q == ' ') q++;
                    int l = 0; while (q[l] && q[l] != ';' && l < cap - 1) { out[l] = q[l]; l++; }
                    while (l && out[l - 1] == ' ') l--;
                    out[l] = 0; return true;
                }
            }
            while (*p && *p != ';') p++;
            if (!*p) break;
        }
    }
    const char *a = sattr(n, name);
    if (a) { strlcpy(out, a, cap); return true; }
    return false;
}
static float num(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    float sign = 1, v = 0, frac = 0.1f;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * frac; frac *= 0.1f; } }
    if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        p++; int es = 1, e = 0; if (*p == '-') { es = -1; p++; } else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
        while (e-- > 0) v = es > 0 ? v * 10 : v / 10;
    }
    *pp = p;
    return sign * v;
}
static bool has_num(const char *p) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
}
static float len_attr(node_t *n, const char *name, float def) { const char *v = sattr(n, name); if (!v) return def; const char *p = v; return num(&p); }

/* ---------------- transforms ---------------- */
typedef struct { float a, b, c, d, e, f; } mat_t;   /* x' = a x + c y + e, y' = b x + d y + f */
static mat_t mul(mat_t m, mat_t n) {                  /* m then n applied first: m * n */
    mat_t r = { m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
                m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f };
    return r;
}
static mat_t parse_transform(mat_t m, const char *t) {
    while (t && *t) {
        while (*t == ' ' || *t == ',') t++;
        const char *name = t; while (*t && *t != '(') t++;
        if (!*t) break;
        int nl = t - name; while (nl && name[nl - 1] == ' ') nl--;
        t++;
        float v[6] = { 0 }; int k = 0;
        while (*t && *t != ')' && k < 6) { if (!has_num(t)) { t++; continue; } v[k++] = num(&t); }
        if (*t == ')') t++;
        mat_t n = { 1, 0, 0, 1, 0, 0 };
        if (nl == 6 && !strncmp(name, "matrix", 6) && k == 6) { n.a = v[0]; n.b = v[1]; n.c = v[2]; n.d = v[3]; n.e = v[4]; n.f = v[5]; }
        else if (nl == 9 && !strncmp(name, "translate", 9)) { n.e = v[0]; n.f = k > 1 ? v[1] : 0; }
        else if (nl == 5 && !strncmp(name, "scale", 5)) { n.a = v[0]; n.d = k > 1 ? v[1] : v[0]; }
        else if (nl == 6 && !strncmp(name, "rotate", 6)) {
            float r = v[0] * PI_F / 180, cs = cos_(r), sn = sin_(r);
            mat_t rot = { cs, sn, -sn, cs, 0, 0 };
            if (k >= 3) { mat_t t1 = { 1, 0, 0, 1, v[1], v[2] }, t2 = { 1, 0, 0, 1, -v[1], -v[2] }; n = mul(mul(t1, rot), t2); }
            else n = rot;
        }
        else if (nl == 5 && !strncmp(name, "skewX", 5)) { float r = v[0] * PI_F / 180; n.c = sin_(r) / cos_(r); }
        else if (nl == 5 && !strncmp(name, "skewY", 5)) { float r = v[0] * PI_F / 180; n.b = sin_(r) / cos_(r); }
        m = mul(m, n);
    }
    return m;
}

/* ---------------- building paths ---------------- */
typedef struct { raster_path_t *p; mat_t m; bool open; } pb_t;
static int32_t fx(float v) { return (int32_t)(v * 64 + (v < 0 ? -0.5f : 0.5f)); }
static void pt(pb_t *b, float x, float y, int32_t *ox, int32_t *oy) { *ox = fx(b->m.a * x + b->m.c * y + b->m.e); *oy = fx(b->m.b * x + b->m.d * y + b->m.f); }
static void P_move(pb_t *b, float x, float y) { int32_t X, Y; pt(b, x, y, &X, &Y); raster_move_to(b->p, X, Y); b->open = true; }
static void P_line(pb_t *b, float x, float y) { int32_t X, Y; pt(b, x, y, &X, &Y); raster_line_to(b->p, X, Y); }
static void P_cubic(pb_t *b, float x1, float y1, float x2, float y2, float x, float y) {
    int32_t a, bb, c, d, e, f; pt(b, x1, y1, &a, &bb); pt(b, x2, y2, &c, &d); pt(b, x, y, &e, &f);
    raster_cubic_to(b->p, a, bb, c, d, e, f);
}
static void P_quad(pb_t *b, float x1, float y1, float x, float y) {
    int32_t a, bb, e, f; pt(b, x1, y1, &a, &bb); pt(b, x, y, &e, &f);
    raster_quad_to(b->p, a, bb, e, f);
}
/* an elliptical arc from (x0,y0), as cubic pieces of at most 90 degrees (SVG implementation notes F.6) */
static void P_arc(pb_t *b, float x0, float y0, float rx, float ry, float rot, bool large, bool sweep, float x, float y) {
    if (rx == 0 || ry == 0) { P_line(b, x, y); return; }
    rx = fabs_(rx); ry = fabs_(ry);
    float phi = rot * PI_F / 180, cp = cos_(phi), sp = sin_(phi);
    float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { float s = sqrt_(lam); rx *= s; ry *= s; }
    float num_ = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float co = den > 0 && num_ > 0 ? sqrt_(num_ / den) : 0;
    if (large == sweep) co = -co;
    float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    float cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
    float t1 = atan2_((y1 - cyp) / ry, (x1 - cxp) / rx), t2 = atan2_((-y1 - cyp) / ry, (-x1 - cxp) / rx);
    float dt = t2 - t1;
    if (sweep && dt < 0) dt += 2 * PI_F; else if (!sweep && dt > 0) dt -= 2 * PI_F;
    int segs = (int)(fabs_(dt) / (PI_F / 2) + 0.999f); if (segs < 1) segs = 1;
    float step = dt / segs, k = 4.0f / 3 * sin_(step / 4) / cos_(step / 4);
    for (int i = 0; i < segs; i++) {
        float a1 = t1 + i * step, a2 = a1 + step;
        float c1 = cos_(a1), s1 = sin_(a1), c2 = cos_(a2), s2 = sin_(a2);
        float e1x = c1 - k * s1, e1y = s1 + k * c1, e2x = c2 + k * s2, e2y = s2 - k * c2;
        #define EX(ux, uy) (cx + rx * (ux) * cp - ry * (uy) * sp)
        #define EY(ux, uy) (cy + rx * (ux) * sp + ry * (uy) * cp)
        P_cubic(b, EX(e1x, e1y), EY(e1x, e1y), EX(e2x, e2y), EY(e2x, e2y), EX(c2, s2), EY(c2, s2));
        #undef EX
        #undef EY
    }
}
static void path_d(pb_t *b, const char *d) {
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; char last = 0;
    char cmd = 0;
    while (*d) {
        while (*d == ' ' || *d == ',' || *d == '\n' || *d == '\t' || *d == '\r') d++;
        if (!*d) break;
        if ((*d >= 'A' && *d <= 'Z') || (*d >= 'a' && *d <= 'z')) cmd = *d++;
        else if (!cmd) break;
        else if (!has_num(d)) { d++; continue; }
        bool rel = cmd >= 'a';
        float ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (cmd | 0x20) {
        case 'm': { float x = num(&d) + ox, y = num(&d) + oy; P_move(b, x, y); cx = sx = x; cy = sy = y; cmd = rel ? 'l' : 'L'; break; }
        case 'l': { float x = num(&d) + ox, y = num(&d) + oy; P_line(b, x, y); cx = x; cy = y; break; }
        case 'h': { float x = num(&d) + ox; P_line(b, x, cy); cx = x; break; }
        case 'v': { float y = num(&d) + oy; P_line(b, cx, y); cy = y; break; }
        case 'c': { float x1 = num(&d) + ox, y1 = num(&d) + oy, x2 = num(&d) + ox, y2 = num(&d) + oy, x = num(&d) + ox, y = num(&d) + oy;
                    P_cubic(b, x1, y1, x2, y2, x, y); lcx = x2; lcy = y2; cx = x; cy = y; break; }
        case 's': { float x1 = (last == 'c' || last == 's') ? 2 * cx - lcx : cx, y1 = (last == 'c' || last == 's') ? 2 * cy - lcy : cy;
                    float x2 = num(&d) + ox, y2 = num(&d) + oy, x = num(&d) + ox, y = num(&d) + oy;
                    P_cubic(b, x1, y1, x2, y2, x, y); lcx = x2; lcy = y2; cx = x; cy = y; break; }
        case 'q': { float x1 = num(&d) + ox, y1 = num(&d) + oy, x = num(&d) + ox, y = num(&d) + oy;
                    P_quad(b, x1, y1, x, y); lcx = x1; lcy = y1; cx = x; cy = y; break; }
        case 't': { float x1 = (last == 'q' || last == 't') ? 2 * cx - lcx : cx, y1 = (last == 'q' || last == 't') ? 2 * cy - lcy : cy;
                    float x = num(&d) + ox, y = num(&d) + oy; P_quad(b, x1, y1, x, y); lcx = x1; lcy = y1; cx = x; cy = y; break; }
        case 'a': { float rx = num(&d), ry = num(&d), rot = num(&d);
                    /* the two flags may be written without separators ("a1 1 0 011 1") */
                    while (*d == ' ' || *d == ',') d++; bool large = *d == '1'; if (*d) d++;
                    while (*d == ' ' || *d == ',') d++; bool sweep = *d == '1'; if (*d) d++;
                    float x = num(&d) + ox, y = num(&d) + oy;
                    P_arc(b, cx, cy, rx, ry, rot, large, sweep, x, y); cx = x; cy = y; break; }
        case 'z': raster_close(b->p); cx = sx; cy = sy; break;
        default: d++; break;
        }
        last = cmd | 0x20;
    }
}

/* ---------------- painting ---------------- */
typedef struct {
    uint32_t fill, stroke, current;
    bool fill_none, stroke_none, evenodd;
    float stroke_w, opacity, fill_op, stroke_op;
} paint_t;
static node_t *find_id(node_t *n, const char *id) {
    if (!n->type) { const char *v = sattr(n, "id"); if (v && !strcmp(v, id)) return n; }
    for (node_t *c = n->first; c; c = c->next) { node_t *r = find_id(c, id); if (r) return r; }
    return NULL;
}
static node_t *svg_root_of(node_t *n) { node_t *r = n; while (r->parent && strcasecmp(r->tag, "svg")) r = r->parent; return r; }
static bool paint_value(node_t *n, const char *v, uint32_t current, uint32_t *out, bool *none) {
    while (*v == ' ') v++;
    if (!strncasecmp(v, "none", 4) || !strncasecmp(v, "transparent", 11)) { *none = true; return true; }
    if (!strncasecmp(v, "currentcolor", 12)) { *out = current; *none = false; return true; }
    if (!strncmp(v, "url(", 4)) {                               /* gradient: its first stop */
        const char *h = strchr(v, '#'); char id[64] = "";
        if (h) { int l = 0; h++; while (h[l] && h[l] != ')' && h[l] != '"' && h[l] != '\'' && l < 63) { id[l] = h[l]; l++; } id[l] = 0; }
        node_t *g = id[0] ? find_id(svg_root_of(n), id) : NULL;
        for (int hop = 0; g && hop < 4; hop++) {
            for (node_t *c = g->first; c; c = c->next) if (!c->type && !strcasecmp(c->tag, "stop")) {
                char sc[48];
                if (sprop(c, "stop-color", sc, sizeof sc) && parse_color(sc, out)) { *none = false; return true; }
            }
            const char *href = sattr(g, "href"); if (!href) href = sattr(g, "xlink:href");
            g = href && href[0] == '#' ? find_id(svg_root_of(n), href + 1) : NULL;
        }
        const char *fb = strchr(v, ')');                         /* url(#x) fallback-colour */
        if (fb && parse_color(fb + 1, out)) { *none = false; return true; }
        *out = HEX(0x808080); *none = false; return true;
    }
    if (parse_color(v, out)) { *none = false; return true; }
    return false;
}
static void inherit(node_t *n, paint_t *pa) {
    char v[64];
    if (n->css.set & ST_COLOR) pa->current = n->css.color;
    if (sprop(n, "color", v, sizeof v)) { uint32_t c; if (parse_color(v, &c)) pa->current = c; }
    if (n->css.set & ST_FILL) { if (n->css.fill_cur) { pa->fill = pa->current; pa->fill_none = false; } else { pa->fill = n->css.fill; pa->fill_none = !(n->css.fill >> 24); } }
    if (sprop(n, "fill", v, sizeof v)) paint_value(n, v, pa->current, &pa->fill, &pa->fill_none);
    if (sprop(n, "stroke", v, sizeof v)) paint_value(n, v, pa->current, &pa->stroke, &pa->stroke_none);
    if (sprop(n, "stroke-width", v, sizeof v)) { const char *p = v; pa->stroke_w = num(&p); }
    if (sprop(n, "fill-rule", v, sizeof v)) pa->evenodd = !strncasecmp(v, "evenodd", 7);
    if (sprop(n, "opacity", v, sizeof v)) { const char *p = v; pa->opacity *= num(&p); }
    if (sprop(n, "fill-opacity", v, sizeof v)) { const char *p = v; pa->fill_op = num(&p); }
    if (sprop(n, "stroke-opacity", v, sizeof v)) { const char *p = v; pa->stroke_op = num(&p); }
}
static uint32_t with_alpha(uint32_t c, float a) {
    if (a >= 0.999f) return c;
    if (a < 0) a = 0;
    uint32_t al = (uint32_t)((c >> 24) * a);
    return (al << 24) | (c & 0xFFFFFF);
}
static void draw_shape(canvas_t *cv, node_t *n, mat_t m, const paint_t *pa, raster_path_t *path) {
    pb_t b = { path, m, false };
    raster_reset(path);
    const char *t = n->tag;
    if (!strcasecmp(t, "path")) { const char *d = sattr(n, "d"); if (!d) return; path_d(&b, d); }
    else if (!strcasecmp(t, "rect")) {
        float x = len_attr(n, "x", 0), y = len_attr(n, "y", 0), w = len_attr(n, "width", 0), h = len_attr(n, "height", 0);
        float rx = len_attr(n, "rx", -1), ry = len_attr(n, "ry", -1);
        if (rx < 0) rx = ry; if (ry < 0) ry = rx; if (rx < 0) rx = ry = 0;
        rx = rx > w / 2 ? w / 2 : rx; ry = ry > h / 2 ? h / 2 : ry;
        if (w <= 0 || h <= 0) return;
        if (rx > 0 && ry > 0) {
            float k = 0.5523f;
            P_move(&b, x + rx, y); P_line(&b, x + w - rx, y);
            P_cubic(&b, x + w - rx + rx * k, y, x + w, y + ry - ry * k, x + w, y + ry); P_line(&b, x + w, y + h - ry);
            P_cubic(&b, x + w, y + h - ry + ry * k, x + w - rx + rx * k, y + h, x + w - rx, y + h); P_line(&b, x + rx, y + h);
            P_cubic(&b, x + rx - rx * k, y + h, x, y + h - ry + ry * k, x, y + h - ry); P_line(&b, x, y + ry);
            P_cubic(&b, x, y + ry - ry * k, x + rx - rx * k, y, x + rx, y);
        } else { P_move(&b, x, y); P_line(&b, x + w, y); P_line(&b, x + w, y + h); P_line(&b, x, y + h); }
        raster_close(path);
    } else if (!strcasecmp(t, "circle") || !strcasecmp(t, "ellipse")) {
        float cx = len_attr(n, "cx", 0), cy = len_attr(n, "cy", 0), rx, ry;
        if (t[0] == 'c' || t[0] == 'C') rx = ry = len_attr(n, "r", 0); else { rx = len_attr(n, "rx", 0); ry = len_attr(n, "ry", 0); }
        if (rx <= 0 || ry <= 0) return;
        float k = 0.5523f;
        P_move(&b, cx + rx, cy);
        P_cubic(&b, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
        P_cubic(&b, cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
        P_cubic(&b, cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
        P_cubic(&b, cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
        raster_close(path);
    } else if (!strcasecmp(t, "line")) {
        P_move(&b, len_attr(n, "x1", 0), len_attr(n, "y1", 0)); P_line(&b, len_attr(n, "x2", 0), len_attr(n, "y2", 0));
    } else if (!strcasecmp(t, "polyline") || !strcasecmp(t, "polygon")) {
        const char *p = sattr(n, "points"); if (!p) return;
        bool first = true;
        while (has_num(p)) { float x = num(&p); if (!has_num(p)) break; float y = num(&p); if (first) P_move(&b, x, y); else P_line(&b, x, y); first = false; }
        if (t[4] == 'g' || t[4] == 'G') raster_close(path);
    } else return;
    bool is_line = !strcasecmp(t, "line") || !strcasecmp(t, "polyline");
    if (!pa->fill_none && !is_line) raster_fill(cv, path, with_alpha(pa->fill, pa->opacity * pa->fill_op), pa->evenodd);
    if (!pa->stroke_none && pa->stroke_w > 0) {
        float sc = sqrt_(fabs_(m.a * m.d - m.b * m.c));
        int w = (int)(pa->stroke_w * sc + 0.5f); if (w < 1) w = 1;
        raster_stroke(cv, path, w, with_alpha(pa->stroke, pa->opacity * pa->stroke_op));
    }
}
static void draw_node(canvas_t *cv, node_t *n, mat_t m, paint_t pa, raster_path_t *path, int depth) {
    if (n->type || depth > 24) return;
    const char *t = n->tag;
    static const char *skip[] = { "defs", "lineargradient", "radialgradient", "clippath", "mask", "pattern", "symbol", "marker", "filter", "title", "desc", "metadata", "style", "script", "text", "foreignobject" };
    for (unsigned i = 0; i < ARRAY_LEN(skip); i++) if (!strcasecmp(t, skip[i])) return;
    if ((n->css.set & ST_DISPLAY) && n->css.display == 0) return;
    const char *disp = sattr(n, "display"); if (disp && !strcasecmp(disp, "none")) return;
    inherit(n, &pa);
    const char *tr = sattr(n, "transform"); if (tr) m = parse_transform(m, tr);
    if (!strcasecmp(t, "g") || !strcasecmp(t, "a") || (!strcasecmp(t, "svg") && depth > 0)) {
        if (!strcasecmp(t, "svg")) { float x = len_attr(n, "x", 0), y = len_attr(n, "y", 0); mat_t tt = { 1, 0, 0, 1, x, y }; m = mul(m, tt); }
        for (node_t *c = n->first; c; c = c->next) draw_node(cv, c, m, pa, path, depth + 1);
        return;
    }
    if (!strcasecmp(t, "use")) {
        const char *href = sattr(n, "href"); if (!href) href = sattr(n, "xlink:href");
        node_t *ref = href && href[0] == '#' ? find_id(svg_root_of(n), href + 1) : NULL;
        if (!ref || ref == n) return;
        mat_t tt = { 1, 0, 0, 1, len_attr(n, "x", 0), len_attr(n, "y", 0) }; m = mul(m, tt);
        if (!strcasecmp(ref->tag, "symbol")) {                  /* a symbol: its children, scaled from its viewBox */
            const char *vb = sattr(ref, "viewBox");
            float w = len_attr(n, "width", 0), h = len_attr(n, "height", 0);
            if (vb && w > 0 && h > 0) { const char *p = vb; float vx = num(&p), vy = num(&p), vw = num(&p), vh = num(&p);
                if (vw > 0 && vh > 0) { float s = w / vw < h / vh ? w / vw : h / vh; mat_t sm = { s, 0, 0, s, -vx * s, -vy * s }; m = mul(m, sm); } }
            inherit(ref, &pa);
            for (node_t *c = ref->first; c; c = c->next) draw_node(cv, c, m, pa, path, depth + 1);
        } else draw_node(cv, ref, m, pa, path, depth + 1);
        return;
    }
    draw_shape(cv, n, m, &pa, path);
}

/* ---------------- public ---------------- */
void svg_intrinsic(node_t *svg, int *w, int *h) {
    float aw = len_attr(svg, "width", -1), ah = len_attr(svg, "height", -1);
    const char *wa = sattr(svg, "width"), *ha = sattr(svg, "height");
    if (wa && strchr(wa, '%')) aw = -1;
    if (ha && strchr(ha, '%')) ah = -1;
    float vw = 0, vh = 0;
    const char *vb = sattr(svg, "viewBox");
    if (vb) { const char *p = vb; num(&p); num(&p); vw = num(&p); vh = num(&p); }
    if (aw <= 0 && ah <= 0) { aw = vw > 0 ? vw : 300; ah = vh > 0 ? vh : 150; }
    else if (aw <= 0) aw = vw > 0 && vh > 0 ? ah * vw / vh : ah;
    else if (ah <= 0) ah = vw > 0 && vh > 0 ? aw * vh / vw : aw;
    *w = (int)(aw + 0.5f); *h = (int)(ah + 0.5f);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}
void svg_draw(canvas_t *cv, node_t *svg, int x, int y, int w, int h, uint32_t current) {
    static raster_path_t *path;
    if (!path) path = raster_path_new();
    if (!path || w <= 0 || h <= 0) return;
    mat_t m = { 1, 0, 0, 1, (float)x, (float)y };
    const char *vb = sattr(svg, "viewBox");
    if (vb) {
        const char *p = vb; float vx = num(&p), vy = num(&p), vw = num(&p), vh = num(&p);
        if (vw > 0 && vh > 0) {
            const char *par = sattr(svg, "preserveAspectRatio");
            float sx = w / vw, sy = h / vh;
            if (par && !strncmp(par, "none", 4)) { mat_t s = { sx, 0, 0, sy, -vx * sx, -vy * sy }; m = mul(m, s); }
            else {
                float s = sx < sy ? sx : sy;
                mat_t sm = { s, 0, 0, s, -vx * s + (w - vw * s) / 2, -vy * s + (h - vh * s) / 2 };
                m = mul(m, sm);
            }
        }
    }
    paint_t pa = { BLACK, BLACK, current, false, true, false, 1, 1, 1, 1 };
    rect_t old = canvas_push_clip(cv, R(x, y, w, h));
    inherit(svg, &pa);
    for (node_t *c = svg->first; c; c = c->next) draw_node(cv, c, m, pa, path, 1);
    cv->clip = old;
}
/* an .svg file as a picture: parsed as HTML (the parser builds SVG elements) and drawn once */
canvas_t *svg_decode(const uint8_t *data, uint32_t len) {
    doc_t *d = html_parse((const char *)data, len, "utf-8", "");
    if (!d) return NULL;
    node_t *n = d->root, *svg = NULL;
    while (n && !svg) {                                         /* the first <svg> in document order */
        if (!n->type && !strcasecmp(n->tag, "svg")) { svg = n; break; }
        if (n->first) { n = n->first; continue; }
        while (n && !n->next) n = n->parent;
        if (n) n = n->next;
    }
    canvas_t *c = NULL;
    if (svg) {
        int w, h; svg_intrinsic(svg, &w, &h);
        if (w > 2048) { h = h * 2048 / w; w = 2048; }
        if (h > 2048) { w = w * 2048 / h; h = 2048; }
        c = canvas_new(MAX(1, w), MAX(1, h));
        if (c) { gfx_clear(c, 0); svg_draw(c, svg, 0, 0, w, h, BLACK); }
    }
    doc_free(d);
    return c;
}
bool svg_sniff(const uint8_t *d, uint32_t n) {
    uint32_t lim = MIN(n, 1024u);
    for (uint32_t i = 0; i + 4 <= lim; i++) if (d[i] == '<' && (d[i + 1] == 's' || d[i + 1] == 'S') && (d[i + 2] == 'v' || d[i + 2] == 'V') && (d[i + 3] == 'g' || d[i + 3] == 'G')) return true;
    return false;
}
