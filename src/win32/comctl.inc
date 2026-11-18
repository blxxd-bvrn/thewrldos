/* comctl32's window classes (included by user32.c): toolbar, status bar, rebar, progress
 * bar, trackbar, up-down, tab control and list view (report and list modes), plus quiet
 * windows for tooltips and the rarer classes so that creating them succeeds. Each keeps its
 * state in w->cc; notifications go to the parent as WM_NOTIFY or WM_COMMAND/WM_xSCROLL. */

#ifndef LOWORD
#define LOWORD(v) ((WORD)((uintptr_t)(v) & 0xFFFF))
#define HIWORD(v) ((WORD)(((uintptr_t)(v) >> 16) & 0xFFFF))
#endif
typedef struct { uintptr_t hwnd, id; UINT code; } NMHDR32;
/* WM_NOTIFY to the parent. From the program's own task it is sent at once; from the window
 * manager's (mouse and keys) a copy is queued and the program's message loop delivers it
 * (WM_CC_NOTIFY to the control), since the parent's window procedure must run in the program. */
#define WM_CC_NOTIFY 0x7F01
static LRESULT notify_parent_sz(w32win_t *w, int code, NMHDR32 *nm, size_t size) {
    if (!w->parent) return 0;
    nm->hwnd = (uintptr_t)w; nm->id = w->id; nm->code = (UINT)code;
    if (w32_cur() == w->proc) return send(w->parent, 0x4E, w->id, (LPARAM)nm);
    void *copy = kmalloc(size); memcpy(copy, nm, size);
    w32_post(w->proc, (uintptr_t)w, WM_CC_NOTIFY, size, (LPARAM)copy);
    return 0;
}
static void cc_text_in(w32win_t *w, const void *s, bool wide, char *out, int cap) {
    out[0] = 0;
    if (!s || (uintptr_t)s < 0x10000 || (intptr_t)s == -1) return;
    if (wide) { const WCHAR *ws = s; int i = 0; for (; ws[i] && i < cap - 1; i++) out[i] = ws[i] < 256 ? (char)ws[i] : '?'; out[i] = 0; }
    else strlcpy(out, s, cap);
}
static void cc_text_out(const char *s, void *dst, int cap, bool wide) {
    if (!dst || cap <= 0) return;
    if (wide) { WCHAR *w = dst; int i = 0; for (; s[i] && i < cap - 1; i++) w[i] = (uint8_t)s[i]; w[i] = 0; } else strlcpy(dst, s, cap);
}

/* ================================================================ toolbar */
typedef struct { int bmp, id; uint8_t state, style; uintptr_t data; char text[64]; } tbbtn_t;
typedef struct { tbbtn_t *b; int n, cap; HANDLE il; gdiobj_t *bitmap; int bmpw, bmph, btnw, btnh; char strings[32][64]; int nstr; int hot, pressed; DWORD exstyle; } tb_t;
static tb_t *TB(w32win_t *w) { if (!w->cc) { tb_t *t = kzalloc(sizeof *t); t->bmpw = 16; t->bmph = 16; t->hot = t->pressed = -1; w->cc = t; } return w->cc; }
static int tb_btn_w(w32win_t *w, tb_t *t, tbbtn_t *b) {
    if (b->style & 1) return 8;                                    /* TBSTYLE_SEP */
    int f = ctl_font(w), tw = b->text[0] && (w->style & 0x1000) ? text_width(f, b->text) + 8 : 0;   /* TBSTYLE_LIST: text beside */
    int base = t->btnw ? t->btnw : t->bmpw + 8;
    if (b->text[0] && !(w->style & 0x1000)) base = MAX(base, text_width(f, b->text) + 10);
    return base + tw;
}
static int tb_height(w32win_t *w, tb_t *t) {
    int h = t->btnh ? t->btnh : t->bmph + 7;
    for (int i = 0; i < t->n; i++) if (t->b[i].text[0] && !(w->style & 0x1000)) { h = MAX(h, t->bmph + font_height(ctl_font(w)) + 8); break; }
    return h;
}
static void tb_layout(w32win_t *w) {
    tb_t *t = TB(w);
    if (w->parent && !(w->style & 0x3)) {                          /* CCS_TOP unless CCS_NORESIZE/CCS_NOPARENTALIGN... keep at the top */
        w32win_t *p = w->parent;
        if (!(w->style & 4) && !(w->style & 8)) { w->x = 0; w->y = (w->style & 3) == 3 ? p->h - tb_height(w, t) - 4 : 0; w->w = p->w; }
        if (!(w->style & 4)) w->h = tb_height(w, t) + 4;
    }
}
static int tb_index_at(w32win_t *w, int x, int y) {
    tb_t *t = TB(w); int cx = 2;
    for (int i = 0; i < t->n; i++) { if (t->b[i].state & 8) continue; int bw = tb_btn_w(w, t, &t->b[i]); if (x >= cx && x < cx + bw && y >= 2 && y < 2 + tb_height(w, t)) return i; cx += bw; }
    return -1;
}
static void tb_draw_image(w32win_t *w, tb_t *t, tbbtn_t *b, canvas_t *s, int x, int y, bool disabled) {
    if (b->bmp < 0) return;
    imglist_t *l = IL(t->il);
    if (l && b->bmp < l->n) {
        for (int yy = 0; yy < l->cy; yy++) for (int xx = 0; xx < l->cx; xx++) {
            uint32_t p = l->strip->px[(size_t)yy * l->strip->stride + b->bmp * l->cx + xx];
            if (p >> 24) put_pixel(s, x + xx, y + yy, disabled ? mix(p, T.face, 160) : p);
        }
        return;
    }
    if (t->bitmap && t->bitmap->bmp) {                             /* TB_ADDBITMAP: a strip, light gray (192,192,192) is transparent */
        canvas_t *c = t->bitmap->bmp;
        for (int yy = 0; yy < t->bmph && yy < c->h; yy++) for (int xx = 0; xx < t->bmpw; xx++) {
            int sx = b->bmp * t->bmpw + xx; if (sx >= c->w) break;
            uint32_t p = c->px[(size_t)yy * c->stride + sx];
            if ((p & 0xFFFFFF) == 0xC0C0C0 || (p & 0xFFFFFF) == 0xFF00FF) continue;
            put_pixel(s, x + xx, y + yy, disabled ? mix(p, T.face, 160) : p);
        }
        return;
    }
    /* the standard images (HINST_COMMCTRL): a small framed square keeps the layout */
    draw_rect(s, x + 2, y + 2, t->bmpw - 4, t->bmph - 4, disabled ? T.gray_text : T.face_dark);
}
static void draw_toolbar(w32win_t *w, canvas_t *s, rect_t r, int f) {
    tb_t *t = TB(w);
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    if (!(w->style & 0x40)) hline(s, r.x, r.y, r.w, T.face_light);              /* not CCS_NODIVIDER */
    int cx = r.x + 2, bh = tb_height(w, t);
    for (int i = 0; i < t->n; i++) {
        tbbtn_t *b = &t->b[i]; if (b->state & 8) continue;            /* TBSTATE_HIDDEN */
        int bw = tb_btn_w(w, t, b);
        if (b->style & 1) { vline(s, cx + 3, r.y + 4, bh - 4, T.face_dark); vline(s, cx + 4, r.y + 4, bh - 4, T.face_light); cx += bw; continue; }
        bool en = (b->state & 4) != 0, chk = (b->state & 1) != 0, prs = (b->state & 2) || (t->pressed == i && t->hot == i);
        if (en && (t->hot == i || chk || prs)) {
            if (prs || chk) { fill_rect(s, cx, r.y + 2, bw, bh, chk && !prs ? mix(T.face, WHITE, 128) : T.face); bevel_deep(s, cx, r.y + 2, bw, bh, false); }
            else bevel_deep(s, cx, r.y + 2, bw, bh, true);
        }
        int off = prs || chk ? 1 : 0;
        bool list = w->style & 0x1000;
        int ix = list ? cx + 4 : cx + (bw - t->bmpw) / 2, iy = r.y + 2 + (list || !b->text[0] ? (bh - t->bmph) / 2 : 3);
        tb_draw_image(w, t, b, s, ix + off, iy + off, !en);
        if (b->text[0]) {
            int tx = list ? ix + t->bmpw + 4 : cx + (bw - text_width(f, b->text)) / 2, ty = list ? r.y + 2 + (bh - font_height(f)) / 2 : iy + t->bmph + 2;
            draw_text(s, f, tx + off, ty + off, b->text, en ? T.text : T.gray_text);
        }
        cx += bw;
    }
}
typedef struct { int bmp, id; uint8_t state, style; uint8_t pad[sizeof(void *) == 8 ? 6 : 2]; uintptr_t data; intptr_t str; } TBBUTTON32;
static void tb_add(w32win_t *w, int at, const TBBUTTON32 *src, int n, bool wide) {
    tb_t *t = TB(w);
    if (t->n + n > t->cap) { t->cap = t->n + n + 16; t->b = krealloc(t->b, t->cap * sizeof(tbbtn_t)); }
    if (at < 0 || at > t->n) at = t->n;
    memmove(&t->b[at + n], &t->b[at], (t->n - at) * sizeof(tbbtn_t));
    for (int i = 0; i < n; i++) {
        tbbtn_t *b = &t->b[at + i]; memset(b, 0, sizeof *b);
        b->bmp = src[i].bmp; b->id = src[i].id; b->state = src[i].state; b->style = src[i].style; b->data = src[i].data;
        if (src[i].str >= 0 && src[i].str < 32 && src[i].str < t->nstr && !(src[i].style & 1)) strlcpy(b->text, t->strings[src[i].str], sizeof b->text);
        else if ((uintptr_t)src[i].str >= 0x10000) cc_text_in(w, (const void *)src[i].str, wide, b->text, sizeof b->text);
    }
    t->n += n;
    tb_layout(w);
}
static int tb_find(tb_t *t, int id) { for (int i = 0; i < t->n; i++) if (t->b[i].id == id && !(t->b[i].style & 1)) return i; return -1; }
static bool toolbar_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    tb_t *t = TB(w);
    int i;
    switch (m) {
    case 0x41E: *res = 0; return true;                                             /* TB_BUTTONSTRUCTSIZE */
    case 0x414: case 0x444: tb_add(w, -1, (const TBBUTTON32 *)lp, (int)wp, m == 0x444); draw_control(w); *res = 1; return true;   /* TB_ADDBUTTONS */
    case 0x415: case 0x443: tb_add(w, (int)wp, (const TBBUTTON32 *)lp, 1, m == 0x443); draw_control(w); *res = 1; return true;    /* TB_INSERTBUTTON */
    case 0x416: if ((int)wp >= 0 && (int)wp < t->n) { memmove(&t->b[wp], &t->b[wp + 1], (t->n - wp - 1) * sizeof(tbbtn_t)); t->n--; draw_control(w); *res = 1; } else *res = 0; return true;   /* TB_DELETEBUTTON */
    case 0x418: *res = t->n; return true;                                          /* TB_BUTTONCOUNT */
    case 0x419: *res = tb_find(t, (int)wp); return true;                           /* TB_COMMANDTOINDEX */
    case 0x413: {                                                                  /* TB_ADDBITMAP */
        struct { HINSTANCE inst; uintptr_t id; } *ab = (void *)lp;
        int first = 0; for (int k = 0; k < t->n; k++) first = MAX(first, t->b[k].bmp + 1);
        if (ab && ab->inst && ab->inst != (HINSTANCE)-1) { HGDIOBJ b = ab->inst ? load_bitmap(ab->inst, ab->id, false) : (HGDIOBJ)ab->id; t->bitmap = OBJ(b); }
        else if (ab && !ab->inst) t->bitmap = OBJ((HGDIOBJ)ab->id);
        *res = 0; (void)first; return true; }
    case 0x430: { HANDLE o = t->il; t->il = lp; if (IL(lp)) { t->bmpw = IL(lp)->cx; t->bmph = IL(lp)->cy; } tb_layout(w); draw_control(w); *res = o; return true; }   /* TB_SETIMAGELIST */
    case 0x431: *res = t->il; return true;                                          /* TB_GETIMAGELIST */
    case 0x434: case 0x436: case 0x468: *res = 0; return true;                      /* hot/disabled image lists */
    case 0x41C: case 0x44D: {                                                       /* TB_ADDSTRING: NUL-separated, double NUL */
        int first = t->nstr;
        if (wp && lp < 0x10000) { *res = first; return true; }
        if (m == 0x44D) { const WCHAR *s = (const WCHAR *)lp; while (*s && t->nstr < 32) { int k = 0; while (s[k] && k < 63) { t->strings[t->nstr][k] = s[k] < 256 ? (char)s[k] : '?'; k++; } t->strings[t->nstr][k] = 0; t->nstr++; while (*s) s++; s++; } }
        else { const char *s = (const char *)lp; while (*s && t->nstr < 32) { strlcpy(t->strings[t->nstr++], s, 64); s += strlen(s) + 1; } }
        *res = first; return true; }
    case 0x401: case 0x402: case 0x403: case 0x404: case 0x411: {                  /* enable, check, press, hide, set state */
        i = tb_find(t, (int)wp); if (i < 0) { *res = 0; return true; }
        uint8_t bit = m == 0x401 ? 4 : m == 0x402 ? 1 : m == 0x403 ? 2 : 8;
        if (m == 0x411) t->b[i].state = (uint8_t)LOWORD(lp);
        else if (LOWORD(lp)) t->b[i].state |= bit; else t->b[i].state &= ~bit;
        draw_control(w); *res = 1; return true; }
    case 0x409: case 0x40A: case 0x40B: case 0x40C: case 0x412: {                  /* is enabled / checked / pressed / hidden, get state */
        i = tb_find(t, (int)wp); if (i < 0) { *res = -1; return true; }
        *res = m == 0x412 ? t->b[i].state : (t->b[i].state & (m == 0x409 ? 4 : m == 0x40A ? 1 : m == 0x40B ? 2 : 8)) != 0; return true; }
    case 0x417: {                                                                  /* TB_GETBUTTON */
        if ((int)wp < 0 || (int)wp >= t->n) { *res = 0; return true; }
        TBBUTTON32 *o = (TBBUTTON32 *)lp; memset(o, 0, sizeof *o);
        tbbtn_t *b = &t->b[wp]; o->bmp = b->bmp; o->id = b->id; o->state = b->state; o->style = b->style; o->data = b->data;
        *res = 1; return true; }
    case 0x41D: case 0x433: {                                                      /* TB_GETITEMRECT (index), TB_GETRECT (id) */
        int idx = m == 0x41D ? (int)wp : tb_find(t, (int)wp);
        RECT32 *r = (RECT32 *)lp; if (idx < 0 || idx >= t->n || !r) { *res = 0; return true; }
        int cx = 2; for (int k = 0; k < idx; k++) if (!(t->b[k].state & 8)) cx += tb_btn_w(w, t, &t->b[k]);
        r->l = cx; r->t = 2; r->r = cx + tb_btn_w(w, t, &t->b[idx]); r->b = 2 + tb_height(w, t); *res = 1; return true; }
    case 0x41F: t->btnw = LOWORD(lp); t->btnh = HIWORD(lp); tb_layout(w); *res = 1; return true;   /* TB_SETBUTTONSIZE */
    case 0x420: t->bmpw = LOWORD(lp); t->bmph = HIWORD(lp); tb_layout(w); *res = 1; return true;   /* TB_SETBITMAPSIZE */
    case 0x43A: *res = (tb_btn_w(w, t, &(tbbtn_t){ 0 }) & 0xFFFF) | (tb_height(w, t) << 16); return true;   /* TB_GETBUTTONSIZE */
    case 0x421: tb_layout(w); draw_control(w); *res = 0; return true;              /* TB_AUTOSIZE */
    case 0x454: { DWORD o = t->exstyle; t->exstyle = (DWORD)lp; *res = o; return true; }   /* TB_SETEXTENDEDSTYLE */
    case 0x455: *res = t->exstyle; return true;
    case 0x43C: case 0x423: case 0x439: case 0x445: case 0x44A: case 0x45F: *res = 1; return true;   /* max text rows, indent, set tooltips, insert mark, ... */
    case 0x427: *res = 0; return true;                                              /* TB_GETTOOLTIPS */
    case 0x428: *res = 0; return true;                                              /* TB_SETPARENT */
    case 0x41B: case 0x447: { int idx = tb_find(t, (int)wp); *res = idx >= 0 && t->b[idx].text[0] ? (LRESULT)strlen(t->b[idx].text) : -1; if (idx >= 0 && lp) cc_text_out(t->b[idx].text, (void *)lp, 64, m == 0x447); return true; }   /* TB_GETBUTTONTEXT */
    case 0x43F: case 0x441: {                                                      /* TB_GETBUTTONINFO */
        int idx = tb_find(t, (int)wp); uint8_t *bi = (uint8_t *)lp; if (idx < 0 || !bi) { *res = -1; return true; }
        DWORD mask = *(DWORD *)(bi + 4);
        if (mask & 8) *(int *)(bi + 8) = t->b[idx].id; if (mask & 1) *(int *)(bi + 12) = t->b[idx].bmp; if (mask & 4) bi[16] = t->b[idx].state; if (mask & 8 && 0) {}
        *res = idx; return true; }
    case 0x440: case 0x442: {                                                      /* TB_SETBUTTONINFO */
        int idx = tb_find(t, (int)wp); const uint8_t *bi = (const uint8_t *)lp; if (idx < 0 || !bi) { *res = 0; return true; }
        DWORD mask = *(const DWORD *)(bi + 4);
        if (mask & 1) t->b[idx].bmp = *(const int *)(bi + 12);
        if (mask & 4) t->b[idx].state = bi[16];
        if (mask & 2) { const void *s = *(void *const *)(bi + 24 + sizeof(void *)); cc_text_in(w, s, m == 0x440 ? false : true, t->b[idx].text, 64); }
        draw_control(w); *res = 1; return true; }
    case WM_SIZE: tb_layout(w); draw_control(w); *res = 0; return true;
    }
    return false;
}
static bool toolbar_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    tb_t *t = TB(w);
    int i = tb_index_at(w, lx, ly);
    if (type == ME_LEAVE) i = -1;
    if (i != t->hot) { t->hot = i; draw_control(w); }
    if (type == ME_DOWN && (btn & MB_LEFT) && i >= 0 && (t->b[i].state & 4) && !(t->b[i].style & 1)) { t->pressed = i; draw_control(w); }
    if (type == ME_UP && t->pressed >= 0) {
        int p = t->pressed; t->pressed = -1; draw_control(w);
        if (p == i && i >= 0) {
            tbbtn_t *b = &t->b[i];
            if (b->style & 2) { if (b->style & 4) { for (int k = 0; k < t->n; k++) if (t->b[k].style & 4) t->b[k].state &= ~1; b->state |= 1; } else b->state ^= 1; draw_control(w); }   /* TBSTYLE_CHECK, GROUP */
            if (w->parent) w32_post(w->proc, (uintptr_t)w->parent, WM_COMMAND, b->id & 0xFFFF, (uintptr_t)w);
        }
    }
    return true;
}

/* ================================================================ status bar */
typedef struct { int nparts; int right[32]; char text[32][128]; bool simple; char simple_text[128]; } sb_t;
static sb_t *SB(w32win_t *w) { if (!w->cc) { sb_t *s = kzalloc(sizeof *s); s->nparts = 1; s->right[0] = -1; w->cc = s; } return w->cc; }
static void sb_layout(w32win_t *w) {
    if (!w->parent || (w->style & 4)) return;                   /* CCS_NORESIZE */
    int h = font_height(ctl_font(w)) + 8;
    w->x = 0; w->w = w->parent->w; w->h = h; w->y = (w->style & 3) == 1 ? 0 : w->parent->h - h;
}
static void draw_status(w32win_t *w, canvas_t *s, rect_t r, int f) {
    sb_t *b = SB(w);
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    hline(s, r.x, r.y, r.w, T.face_dark);
    int left = 0, n = b->simple ? 1 : b->nparts;
    for (int i = 0; i < n; i++) {
        int right = b->simple || b->right[i] < 0 ? r.w - (w->style & 0x100 ? 18 : 2) : MIN(b->right[i], r.w);
        if (right <= left) continue;
        const char *t = b->simple ? b->simple_text : b->text[i];
        rect_t pr = R(r.x + left + 1, r.y + 2, right - left - 2, r.h - 4);
        if (i < n - 1 || !b->simple) { hline(s, pr.x, pr.y, pr.w, T.face_dark); vline(s, pr.x, pr.y, pr.h, T.face_dark); hline(s, pr.x, pr.y + pr.h - 1, pr.w, T.face_light); vline(s, pr.x + pr.w - 1, pr.y, pr.h, T.face_light); }
        rect_t o = canvas_push_clip(s, R(pr.x + 2, pr.y, pr.w - 4, pr.h));
        int tw = text_width(f, t), tx = pr.x + 4;
        const char *tab = strchr(t, '\t');
        if (tab) { tx = tab[1] == '\t' ? pr.x + pr.w - 4 - text_width(f, tab + 2) : pr.x + (pr.w - text_width(f, tab + 1)) / 2; t = tab[1] == '\t' ? tab + 2 : tab + 1; }
        (void)tw;
        draw_text(s, f, tx, pr.y + (pr.h - font_height(f)) / 2, t, T.text);
        s->clip = o;
        left = right;
    }
    if (w->style & 0x100) for (int k = 0; k < 3; k++) for (int j = 0; j <= k; j++) { put_pixel(s, r.x + r.w - 4 - k * 4 + j * 4, r.y + r.h - 4 - j * 4, T.face_dark); }   /* SBARS_SIZEGRIP */
}
static bool status_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    sb_t *b = SB(w);
    switch (m) {
    case 0x404: { int n = MIN((int)wp, 32); const int *e = (const int *)lp; b->nparts = MAX(1, n); for (int i = 0; i < n; i++) b->right[i] = e[i]; draw_control(w); *res = 1; return true; }   /* SB_SETPARTS */
    case 0x406: { int n = (int)wp; int *e = (int *)lp; for (int i = 0; e && i < MIN(n, b->nparts); i++) e[i] = b->right[i]; *res = b->nparts; return true; }   /* SB_GETPARTS */
    case 0x401: case 0x40B: {                                                       /* SB_SETTEXT */
        int part = wp & 0xFF; char *dst = part == 255 ? b->simple_text : part < 32 ? b->text[part] : NULL;
        if (dst) cc_text_in(w, (const void *)lp, m == 0x40B, dst, 128);
        if (part == 0 && b->simple) cc_text_in(w, (const void *)lp, m == 0x40B, b->simple_text, 128);
        draw_control(w); *res = 1; return true; }
    case 0x402: case 0x40D: { int part = wp & 0xFF; const char *t = part < 32 ? b->text[part] : ""; cc_text_out(t, (void *)lp, 128, m == 0x40D); *res = strlen(t); return true; }   /* SB_GETTEXT */
    case 0x403: case 0x40C: { int part = wp & 0xFF; *res = part < 32 ? strlen(b->text[part]) : 0; return true; }   /* SB_GETTEXTLENGTH */
    case 0x409: b->simple = wp != 0; draw_control(w); *res = 0; return true;      /* SB_SIMPLE */
    case 0x40E: *res = b->simple; return true;                                     /* SB_ISSIMPLE */
    case 0x40A: { int part = (int)wp; RECT32 *r = (RECT32 *)lp; if (!r || part >= b->nparts) { *res = 0; return true; } r->l = part ? b->right[part - 1] : 0; r->r = b->right[part] < 0 ? w->w : b->right[part]; r->t = 2; r->b = w->h - 2; *res = 1; return true; }   /* SB_GETRECT */
    case 0x408: { int *v = (int *)lp; if (v) { v[0] = 0; v[1] = 2; v[2] = 2; } *res = 1; return true; }   /* SB_GETBORDERS */
    case 0x407: case 0x40F: case 0x410: case 0x411: case 0x2001: *res = 1; return true;   /* min height, icon, tip text, background colour */
    case WM_SIZE: sb_layout(w); draw_control(w); *res = 0; return true;
    case WM_SETTEXT: cc_text_in(w, (const void *)lp, w->unicode, b->text[0], 128); draw_control(w); *res = 1; return true;
    }
    return false;
}

/* ================================================================ rebar: bands of child windows side by side */
typedef struct { uintptr_t child; int minw, minh, cx, id; char text[32]; } band_t;
typedef struct { band_t band[16]; int n; } rb_t;
static rb_t *RB(w32win_t *w) { if (!w->cc) w->cc = kzalloc(sizeof(rb_t)); return w->cc; }
static void rb_layout(w32win_t *w) {
    rb_t *r = RB(w);
    int h = 4;
    for (int i = 0; i < r->n; i++) h = MAX(h, r->band[i].minh + 4);
    if (w->parent && !(w->style & 4)) { w->x = 0; w->y = 0; w->w = w->parent->w; w->h = h; }
    int x = 2, f = ctl_font(w);
    for (int i = 0; i < r->n; i++) {
        band_t *b = &r->band[i]; w32win_t *c = W(b->child);
        if (b->text[0]) x += text_width(f, b->text) + 8;
        int bw = i == r->n - 1 ? w->w - x - 2 : MAX(b->cx, b->minw);
        if (c) { c->x = x; c->y = 2; c->w = MAX(bw, 1); c->h = MAX(b->minh, 1); c->visible = true; }
        x += MAX(bw, 0) + 6;
    }
}
static void draw_rebar(w32win_t *w, canvas_t *s, rect_t r, int f) {
    rb_t *b = RB(w);
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    hline(s, r.x, r.y + r.h - 2, r.w, T.face_dark); hline(s, r.x, r.y + r.h - 1, r.w, T.face_light);
    int x = r.x + 2;
    for (int i = 0; i < b->n; i++) if (b->band[i].text[0]) { draw_text(s, f, x, r.y + (r.h - font_height(f)) / 2, b->band[i].text, T.text); x += text_width(f, b->band[i].text) + 8; }
}
static bool rebar_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    rb_t *r = RB(w);
    switch (m) {
    case 0x401: case 0x40A: case 0x406: case 0x40B: {                              /* RB_INSERTBAND, RB_SETBANDINFO */
        const uint8_t *bi = (const uint8_t *)lp; if (!bi) { *res = 0; return true; }
        bool insert = m == 0x401 || m == 0x40A, wide = m == 0x40A || m == 0x40B;
        int idx = insert ? ((int)wp < 0 || (int)wp > r->n ? r->n : (int)wp) : (int)wp;
        if (insert) { if (r->n >= 16) { *res = 0; return true; } memmove(&r->band[idx + 1], &r->band[idx], (r->n - idx) * sizeof(band_t)); memset(&r->band[idx], 0, sizeof(band_t)); r->n++; }
        else if (idx < 0 || idx >= r->n) { *res = 0; return true; }
        band_t *b = &r->band[idx];
        DWORD mask = *(const DWORD *)(bi + 4); size_t P8 = sizeof(void *);
        /* REBARBANDINFO: cbSize, fMask, fStyle, clrFore, clrBack, lpText, cch, iImage, hwndChild, cxMinChild, cyMinChild, cx, hbmBack, wID */
        size_t o_text = 20, o_cch = 20 + P8, o_child = o_cch + 8, o_minw = o_child + P8, o_cx = o_minw + 8, o_bmp = o_cx + 4, o_id = o_bmp + P8;
        o_text = (o_text + P8 - 1) & ~(P8 - 1);
        if (mask & 4) cc_text_in(w, *(void *const *)(bi + o_text), wide, b->text, 32);
        if (mask & 0x10) { b->child = *(const uintptr_t *)(bi + ((o_child + P8 - 1) & ~(P8 - 1))); w32win_t *c = W(b->child); if (c && c->parent != w) { /* reparent */ } }
        if (mask & 0x20) { b->minw = *(const int *)(bi + o_minw + (P8 == 8 ? 4 : 0)); b->minh = *(const int *)(bi + o_minw + 4 + (P8 == 8 ? 4 : 0)); }
        if (mask & 0x40) b->cx = *(const int *)(bi + o_cx + (P8 == 8 ? 4 : 0));
        if (mask & 0x100) b->id = *(const int *)(bi + o_id + (P8 == 8 ? 4 : 0));
        rb_layout(w); draw_control(w); *res = 1; return true; }
    case 0x402: if ((int)wp < r->n) { memmove(&r->band[wp], &r->band[wp + 1], (r->n - wp - 1) * sizeof(band_t)); r->n--; rb_layout(w); } *res = 1; return true;   /* RB_DELETEBAND */
    case 0x40C: *res = r->n; return true;                                           /* RB_GETBANDCOUNT */
    case 0x40D: *res = 1; return true;                                              /* RB_GETROWCOUNT */
    case 0x41B: *res = w->h; return true;                                           /* RB_GETBARHEIGHT */
    case 0x40E: *res = w->h; return true;                                           /* RB_GETROWHEIGHT */
    case 0x404: case 0x423: case 0x41C: case 0x41E: case 0x426: *res = 1; return true;   /* bar info, show band, maximize, minimize, ... */
    case WM_SIZE: rb_layout(w); draw_control(w); *res = 0; return true;
    }
    return false;
}

/* ================================================================ progress bar, trackbar, up-down */
typedef struct { int lo, hi, pos, step, page, line; uint32_t bar; uintptr_t buddy; bool drag; } rng_t;
static rng_t *RNG(w32win_t *w) { if (!w->cc) { rng_t *r = kzalloc(sizeof *r); r->hi = 100; r->step = 10; r->page = 10; r->line = 1; w->cc = r; } return w->cc; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void cc_draw_progress(w32win_t *w, canvas_t *s, rect_t r) {
    rng_t *g = RNG(w);
    draw_sunken_box(s, r, T.window_bg);
    int span = g->hi - g->lo; if (span <= 0) return;
    bool vert = w->style & 4;
    int len = vert ? r.h - 4 : r.w - 4, fill = (int)((int64_t)(clampi(g->pos, g->lo, g->hi) - g->lo) * len / span);
    uint32_t c = g->bar ? g->bar : RGB(6, 176, 37);
    if (w->style & 1) { if (vert) fill_rect(s, r.x + 2, r.y + r.h - 2 - fill, r.w - 4, fill, c); else fill_rect(s, r.x + 2, r.y + 2, fill, r.h - 4, c); return; }   /* PBS_SMOOTH */
    int chunk = MAX(6, (vert ? r.w : r.h) * 2 / 3);
    for (int x = 0; x + chunk - 2 <= fill + chunk - 2 && x < fill; x += chunk) {
        int seg = MIN(chunk - 2, len - x);
        if (vert) fill_rect(s, r.x + 2, r.y + r.h - 2 - x - seg, r.w - 4, seg, c); else fill_rect(s, r.x + 2 + x, r.y + 2, seg, r.h - 4, c);
    }
}
static bool progress_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    rng_t *g = RNG(w); int old = g->pos;
    switch (m) {
    case 0x401: *res = (g->lo & 0xFFFF) | (g->hi << 16); g->lo = LOWORD(lp); g->hi = HIWORD(lp); break;   /* PBM_SETRANGE */
    case 0x406: *res = (g->lo & 0xFFFF) | (g->hi << 16); g->lo = (int)wp; g->hi = (int)lp; break;           /* PBM_SETRANGE32 */
    case 0x402: g->pos = (int)wp; *res = old; break;                                                         /* PBM_SETPOS */
    case 0x403: g->pos += (int)wp; *res = old; break;                                                        /* PBM_DELTAPOS */
    case 0x404: *res = g->step; g->step = (int)wp; return true;                                              /* PBM_SETSTEP */
    case 0x405: g->pos += g->step; if (g->pos > g->hi) g->pos = g->lo + (g->pos - g->hi); *res = old; break;   /* PBM_STEPIT */
    case 0x407: { int *r = (int *)lp; if (r) { r[0] = g->lo; r[1] = g->hi; } *res = wp ? g->lo : g->hi; return true; }   /* PBM_GETRANGE */
    case 0x408: *res = g->pos; return true;                                                                  /* PBM_GETPOS */
    case 0x409: case 0x2001: *res = 0; g->bar = cref((COLORREF)lp); break;                                   /* PBM_SETBARCOLOR */
    case 0x40A: case 0x410: case 0x40F: *res = 0; return true;                                               /* marquee, state */
    default: return false;
    }
    g->pos = clampi(g->pos, MIN(g->lo, g->hi), MAX(g->lo, g->hi));
    draw_control(w);
    return true;
}
static void draw_trackbar(w32win_t *w, canvas_t *s, rect_t r) {
    rng_t *g = RNG(w);
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    bool vert = w->style & 2;
    int span = MAX(1, g->hi - g->lo);
    if (!vert) {
        int ty = r.y + r.h / 2 - 2; bevel_deep(s, r.x + 8, ty, r.w - 16, 4, false);
        int tx = r.x + 8 + (int)((int64_t)(clampi(g->pos, g->lo, g->hi) - g->lo) * (r.w - 16) / span);
        fill_rect(s, tx - 5, r.y + r.h / 2 - 10, 11, 20, T.face); bevel_deep(s, tx - 5, r.y + r.h / 2 - 10, 11, 20, true);
        if (!(w->style & 0x10)) for (int k = 0; k <= MIN(span, 20); k++) vline(s, r.x + 8 + k * (r.w - 16) / MIN(span, 20), r.y + r.h - 5, 3, T.text);   /* not TBS_NOTICKS */
    } else {
        int tx = r.x + r.w / 2 - 2; bevel_deep(s, tx, r.y + 8, 4, r.h - 16, false);
        int ty = r.y + 8 + (int)((int64_t)(clampi(g->pos, g->lo, g->hi) - g->lo) * (r.h - 16) / span);
        fill_rect(s, r.x + r.w / 2 - 10, ty - 5, 20, 11, T.face); bevel_deep(s, r.x + r.w / 2 - 10, ty - 5, 20, 11, true);
    }
}
static void track_notify(w32win_t *w, int code) {
    rng_t *g = RNG(w);
    if (w->parent) w32_post(w->proc, (uintptr_t)w->parent, (w->style & 2) ? 0x115 : 0x114, code | (g->pos << 16), (uintptr_t)w);   /* WM_VSCROLL / WM_HSCROLL */
}
static bool trackbar_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    rng_t *g = RNG(w);
    switch (m) {
    case 0x400: *res = g->pos; return true;                                          /* TBM_GETPOS */
    case 0x401: *res = g->lo; return true; case 0x402: *res = g->hi; return true;    /* TBM_GETRANGEMIN/MAX */
    case 0x405: g->pos = clampi((int)lp, g->lo, g->hi); break;                       /* TBM_SETPOS */
    case 0x406: g->lo = LOWORD(lp); g->hi = HIWORD(lp); break;                       /* TBM_SETRANGE */
    case 0x407: g->lo = (int)lp; break; case 0x408: g->hi = (int)lp; break;           /* TBM_SETRANGEMIN/MAX */
    case 0x415: { int o = g->page; g->page = (int)lp; *res = o; return true; }        /* TBM_SETPAGESIZE */
    case 0x417: { int o = g->line; g->line = (int)lp; *res = o; return true; }        /* TBM_SETLINESIZE */
    case 0x416: *res = g->page; return true; case 0x418: *res = g->line; return true;
    case 0x414: case 0x404: case 0x409: case 0x40A: case 0x40B: case 0x40C: case 0x41F: case 0x420: *res = 0; return true;   /* ticks, selection, tooltips, buddy */
    default: return false;
    }
    g->pos = clampi(g->pos, g->lo, g->hi); draw_control(w); *res = 0;
    return true;
}
static bool trackbar_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    rng_t *g = RNG(w);
    bool vert = w->style & 2;
    if (type == ME_DOWN && (btn & MB_LEFT)) g->drag = true;
    if ((type == ME_DOWN || type == ME_MOVE) && g->drag) {
        int len = (vert ? w->h : w->w) - 16, v = (vert ? ly : lx) - 8;
        int pos = g->lo + (int)((int64_t)clampi(v, 0, len) * (g->hi - g->lo) / MAX(len, 1));
        if (pos != g->pos) { g->pos = pos; draw_control(w); track_notify(w, 5); }   /* TB_THUMBTRACK */
    }
    if (type == ME_UP && g->drag) { g->drag = false; track_notify(w, 4); track_notify(w, 8); }   /* TB_THUMBPOSITION, TB_ENDTRACK */
    if (type == ME_WHEEL) { g->pos = clampi(g->pos + (btn > 0 ? -g->line : g->line), g->lo, g->hi); draw_control(w); track_notify(w, btn > 0 ? 0 : 1); }
    return true;
}
static void draw_updown(w32win_t *w, canvas_t *s, rect_t r) {
    bool horz = w->style & 0x40;
    if (horz) { draw_scrollbar(s, r, 1, 1, 0, false); return; }
    int h = r.h / 2;
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    bevel_deep(s, r.x, r.y, r.w, h, true); bevel_deep(s, r.x, r.y + h, r.w, r.h - h, true);
    int cx = r.x + r.w / 2;
    for (int i = 0; i < 3; i++) { hline(s, cx - i, r.y + h / 2 - 1 + i, 2 * i + 1, T.text); hline(s, cx - i, r.y + h + (r.h - h) / 2 + 1 - i, 2 * i + 1, T.text); }
}
static void updown_buddy(w32win_t *w) {
    rng_t *g = RNG(w); w32win_t *b = W(g->buddy);
    if (b && (w->style & 2)) { char t[24]; snprintf(t, sizeof t, "%d", g->pos); strlcpy(b->text, t, sizeof b->text); draw_control(b); }   /* UDS_SETBUDDYINT */
}
static bool updown_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    rng_t *g = RNG(w);
    switch (m) {
    case 0x465: g->lo = (int16_t)HIWORD(lp); g->hi = (int16_t)LOWORD(lp); *res = 0; return true;   /* UDM_SETRANGE (max, min) */
    case 0x46F: g->lo = (int)wp; g->hi = (int)lp; *res = 0; return true;                         /* UDM_SETRANGE32 */
    case 0x466: *res = (g->hi & 0xFFFF) | (g->lo << 16); return true;                             /* UDM_GETRANGE */
    case 0x470: if (wp) *(int *)wp = g->lo; if (lp) *(int *)lp = g->hi; *res = 0; return true;     /* UDM_GETRANGE32 */
    case 0x467: case 0x471: { int o = g->pos; g->pos = clampi((int)(m == 0x467 ? (int16_t)LOWORD(lp) : lp), MIN(g->lo, g->hi), MAX(g->lo, g->hi)); updown_buddy(w); *res = o; return true; }   /* UDM_SETPOS(32) */
    case 0x468: case 0x472: {                                                                     /* UDM_GETPOS(32): from the buddy's text */
        w32win_t *b = W(g->buddy); if (b && (w->style & 2) && b->text[0]) g->pos = atoi(b->text);
        *res = m == 0x468 ? (g->pos & 0xFFFF) : g->pos; if (m == 0x472 && wp) *(BOOL *)wp = 0; return true; }
    case 0x469: { uintptr_t o = g->buddy; g->buddy = wp; w32win_t *b = W(wp);
        if (b && (w->style & 0x14) && w->parent == b->parent) { w->h = b->h; w->y = b->y; if (w->style & 4) { w->x = b->x + b->w; } else { w->x = b->x - w->w; } }   /* UDS_ALIGNRIGHT/LEFT */
        updown_buddy(w); *res = o; return true; }
    case 0x46A: *res = g->buddy; return true;                                                     /* UDM_GETBUDDY */
    case 0x46B: case 0x46C: case 0x46D: case 0x46E: *res = 10; return true;                        /* acceleration, base */
    }
    return false;
}
static bool updown_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    if (type != ME_DOWN || !(btn & MB_LEFT)) return true;
    rng_t *g = RNG(w);
    bool up = (w->style & 0x40) ? lx >= w->w / 2 : ly < w->h / 2;
    int step = g->hi >= g->lo ? 1 : -1;
    int np = g->pos + (up ? step : -step);
    if (np < MIN(g->lo, g->hi) || np > MAX(g->lo, g->hi)) { if (w->style & 1) np = up ? g->lo : g->hi; else return true; }   /* UDS_WRAP */
    struct { NMHDR32 h; int pos, delta; } nm = { { 0 }, g->pos, up ? step : -step };
    if (notify_parent_sz(w, -722, &nm.h, sizeof nm)) return true;                                               /* UDN_DELTAPOS: non-zero cancels */
    g->pos = np; updown_buddy(w);
    if (w->parent) w32_post(w->proc, (uintptr_t)w->parent, (w->style & 0x40) ? 0x114 : 0x115, 4 | (g->pos << 16), (uintptr_t)w);
    return true;
}

/* ================================================================ tab control */
typedef struct { char text[64]; LPARAM data; int image; } tabitem_t;
typedef struct { tabitem_t it[64]; int n, sel; HANDLE il; } tab_t;
static tab_t *TABC(w32win_t *w) { if (!w->cc) { tab_t *t = kzalloc(sizeof *t); t->sel = -1; w->cc = t; } return w->cc; }
static int tab_h(w32win_t *w) { return font_height(ctl_font(w)) + 8; }
static int tab_x(w32win_t *w, int i) { tab_t *t = TABC(w); int x = 2, f = ctl_font(w); for (int k = 0; k < i && k < t->n; k++) x += text_width(f, t->it[k].text) + 16; return x; }
static void draw_tabs(w32win_t *w, canvas_t *s, rect_t r, int f) {
    tab_t *t = TABC(w); int th = tab_h(w);
    fill_rect(s, r.x, r.y, r.w, r.h, T.face);
    bevel_deep(s, r.x, r.y + th, r.w, r.h - th, true);
    for (int i = 0; i < t->n; i++) {
        int x = r.x + tab_x(w, i), tw = text_width(f, t->it[i].text) + 16;
        bool sel = i == t->sel;
        int y = r.y + (sel ? 0 : 2), h = th - (sel ? 0 : 2) + (sel ? 1 : 0);
        fill_rect(s, x, y, tw, h, sel ? T.face : mix(T.face, T.face_dark, 40));
        hline(s, x + 1, y, tw - 2, T.face_light); vline(s, x, y + 1, h - 1, T.face_light); vline(s, x + tw - 1, y + 1, h - 1, T.face_dark);
        draw_text(s, f, x + 8, y + (th - font_height(f)) / 2, t->it[i].text, T.text);
    }
}
/* TCITEM: mask, dwState, dwStateMask, pszText, cchTextMax, iImage, lParam */
static void tab_set(w32win_t *w, tabitem_t *it, const uint8_t *ti, bool wide) {
    DWORD mask = *(const DWORD *)ti; size_t P8 = sizeof(void *);
    size_t o_text = (12 + P8 - 1) & ~(P8 - 1), o_img = o_text + P8 + 4, o_lp = (o_img + 4 + P8 - 1) & ~(P8 - 1);
    if (mask & 1) cc_text_in(w, *(void *const *)(ti + o_text), wide, it->text, sizeof it->text);
    if (mask & 2) it->image = *(const int *)(ti + o_img);
    if (mask & 8) it->data = *(const LPARAM *)(ti + o_lp);
}
static bool tab_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    tab_t *t = TABC(w);
    switch (m) {
    case 0x1307: case 0x133E: {                                                     /* TCM_INSERTITEM */
        if (t->n >= 64) { *res = -1; return true; }
        int i = clampi((int)wp, 0, t->n); memmove(&t->it[i + 1], &t->it[i], (t->n - i) * sizeof(tabitem_t)); memset(&t->it[i], 0, sizeof(tabitem_t)); t->n++;
        tab_set(w, &t->it[i], (const uint8_t *)lp, m == 0x133E);
        if (t->sel < 0) t->sel = 0; else if (i <= t->sel && t->n > 1) t->sel++;
        draw_control(w); *res = i; return true; }
    case 0x1306: case 0x133D: if ((int)wp < t->n) { tab_set(w, &t->it[wp], (const uint8_t *)lp, m == 0x133D); draw_control(w); *res = 1; } else *res = 0; return true;   /* TCM_SETITEM */
    case 0x1305: case 0x133C: {                                                     /* TCM_GETITEM */
        if ((int)wp >= t->n) { *res = 0; return true; }
        uint8_t *ti = (uint8_t *)lp; DWORD mask = *(DWORD *)ti; size_t P8 = sizeof(void *);
        size_t o_text = (12 + P8 - 1) & ~(P8 - 1), o_img = o_text + P8 + 4, o_lp = (o_img + 4 + P8 - 1) & ~(P8 - 1);
        if (mask & 1) cc_text_out(t->it[wp].text, *(void **)(ti + o_text), *(int *)(ti + o_text + P8), m == 0x133C);
        if (mask & 2) *(int *)(ti + o_img) = t->it[wp].image;
        if (mask & 8) *(LPARAM *)(ti + o_lp) = t->it[wp].data;
        *res = 1; return true; }
    case 0x1304: *res = t->n; return true;                                          /* TCM_GETITEMCOUNT */
    case 0x1308: if ((int)wp < t->n) { memmove(&t->it[wp], &t->it[wp + 1], (t->n - wp - 1) * sizeof(tabitem_t)); t->n--; if (t->sel >= t->n) t->sel = t->n - 1; draw_control(w); } *res = 1; return true;
    case 0x1309: t->n = 0; t->sel = -1; draw_control(w); *res = 1; return true;     /* TCM_DELETEALLITEMS */
    case 0x130B: *res = t->sel; return true;                                        /* TCM_GETCURSEL */
    case 0x130C: { int o = t->sel; if ((int)wp < t->n) t->sel = (int)wp; draw_control(w); *res = o; return true; }   /* TCM_SETCURSEL */
    case 0x132F: *res = t->sel; return true;                                        /* TCM_GETCURFOCUS */
    case 0x1330: if ((int)wp < t->n) t->sel = (int)wp; draw_control(w); *res = 0; return true;
    case 0x1328: { RECT32 *r = (RECT32 *)lp; int th = tab_h(w); if (wp) { r->t -= th + 2; r->l -= 2; r->r += 2; r->b += 2; } else { r->t += th + 2; r->l += 2; r->r -= 2; r->b -= 2; } *res = 0; return true; }   /* TCM_ADJUSTRECT */
    case 0x130A: { RECT32 *r = (RECT32 *)lp; if ((int)wp >= t->n || !r) { *res = 0; return true; } r->l = tab_x(w, (int)wp); r->r = r->l + text_width(ctl_font(w), t->it[wp].text) + 16; r->t = 0; r->b = tab_h(w); *res = 1; return true; }
    case 0x1303: { HANDLE o = t->il; t->il = lp; *res = o; return true; }            /* TCM_SETIMAGELIST */
    case 0x1302: *res = t->il; return true;
    case 0x130D: *res = 0; return true;                                             /* TCM_HITTEST: hit by position below */
    case 0x1329: case 0x132A: case 0x132B: case 0x132C: case 0x1334: case 0x1335: *res = 0; return true;
    }
    return false;
}
static bool tab_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    if (type != ME_DOWN || !(btn & MB_LEFT) || ly >= tab_h(w)) return true;
    tab_t *t = TABC(w);
    for (int i = 0; i < t->n; i++) {
        int x = tab_x(w, i), tw = text_width(ctl_font(w), t->it[i].text) + 16;
        if (lx >= x && lx < x + tw && i != t->sel) {
            NMHDR32 nm;
            if (notify_parent_sz(w, -552, &nm, sizeof nm)) return true;                          /* TCN_SELCHANGING: TRUE keeps the old tab */
            t->sel = i; draw_control(w);
            notify_parent_sz(w, -551, &nm, sizeof nm);                                           /* TCN_SELCHANGE */
            return true;
        }
    }
    return true;
}

/* ================================================================ list view */
typedef struct { char text[96]; int width, fmt; } lvcol_t;
typedef struct { char **sub; int nsub; LPARAM data; int image; UINT state; } lvitem_t;
typedef struct { lvcol_t col[32]; int ncol; lvitem_t *it; int n, cap, top, focus, anchor; HANDLE il, il_small; DWORD ex; uint32_t bk, fg, tbk; bool owner; int sort_col; } lv_t;
static lv_t *LV(w32win_t *w) { if (!w->cc) { lv_t *v = kzalloc(sizeof *v); v->focus = -1; v->anchor = -1; v->bk = 0xFFFFFFFF; v->fg = 0xFFFFFFFF; w->cc = v; } return w->cc; }
static bool lv_report(w32win_t *w) { return (w->style & 3) == 1; }                  /* LVS_REPORT */
static int lv_row_h(w32win_t *w) { lv_t *v = LV(w); imglist_t *l = IL(v->il_small); return MAX(font_height(ctl_font(w)) + 4, l ? l->cy + 2 : 0); }
static int lv_head_h(w32win_t *w) { return lv_report(w) && !(w->style & 0x4000) ? font_height(ctl_font(w)) + 8 : 0; }   /* LVS_NOCOLUMNHEADER */
static int lv_visible(w32win_t *w) { return MAX(1, (w->h - 4 - lv_head_h(w)) / lv_row_h(w)); }
static const char *lv_text(lv_t *v, int i, int sub) { lvitem_t *it = &v->it[i]; return sub < it->nsub && it->sub[sub] ? it->sub[sub] : ""; }
static void lv_set_text(lvitem_t *it, int sub, const char *s) {
    if (sub >= it->nsub) { int n = sub + 1; it->sub = krealloc(it->sub, n * sizeof(char *)); for (int k = it->nsub; k < n; k++) it->sub[k] = NULL; it->nsub = n; }
    kfree(it->sub[sub]); it->sub[sub] = strdup(s);
}
static void draw_listview(w32win_t *w, canvas_t *s, rect_t r, int f) {
    lv_t *v = LV(w);
    uint32_t bg = v->bk != 0xFFFFFFFF ? v->bk : T.window_bg, fg = v->fg != 0xFFFFFFFF ? v->fg : T.text;
    draw_sunken_box(s, r, bg);
    rect_t in = R(r.x + 2, r.y + 2, r.w - 4, r.h - 4);
    rect_t o = canvas_push_clip(s, in);
    int y = in.y, hh = lv_head_h(w), rh = lv_row_h(w);
    imglist_t *sil = IL(v->il_small);
    if (hh) {
        int x = in.x;
        for (int c = 0; c < v->ncol; c++) {
            fill_rect(s, x, y, v->col[c].width, hh, T.face); bevel_deep(s, x, y, v->col[c].width, hh, true);
            draw_text(s, f, x + 6, y + (hh - font_height(f)) / 2, v->col[c].text, T.text);
            x += v->col[c].width;
        }
        if (x < in.x + in.w) { fill_rect(s, x, y, in.x + in.w - x, hh, T.face); bevel_deep(s, x, y, in.x + in.w - x, hh, true); }
        y += hh;
    }
    bool full = (v->ex & 0x20) != 0;                                               /* LVS_EX_FULLROWSELECT */
    for (int i = v->top; i < v->n && y < in.y + in.h; i++, y += rh) {
        lvitem_t *it = &v->it[i]; bool sel = it->state & 2;
        bool active = focus_win == w || (w->style & 8);                            /* LVS_SHOWSELALWAYS */
        int x = in.x;
        int ncols = lv_report(w) ? MAX(v->ncol, 1) : 1;
        for (int c = 0; c < ncols; c++) {
            int cw = lv_report(w) && v->ncol ? v->col[c].width : in.w;
            int tx = x + 4;
            if (c == 0 && sil && it->image >= 0 && it->image < sil->n) {
                for (int yy = 0; yy < sil->cy; yy++) for (int xx = 0; xx < sil->cx; xx++) { uint32_t p = sil->strip->px[(size_t)yy * sil->strip->stride + it->image * sil->cx + xx]; if (p >> 24) put_pixel(s, tx + xx, y + (rh - sil->cy) / 2 + yy, p); }
                tx += sil->cx + 3;
            }
            if (v->ex & 4 && c == 0) { draw_check(s, tx, y + (rh - 13) / 2, (it->state >> 12) == 2, false); tx += 16; }   /* LVS_EX_CHECKBOXES */
            const char *t = lv_text(v, i, c);
            int tw = text_width(f, t);
            if (sel && (c == 0 || full)) fill_rect(s, c == 0 ? tx - 2 : x, y, full ? cw - (c == 0 ? tx - 2 - x : 0) : tw + 4, rh, active ? T.select : T.face);
            rect_t co = canvas_push_clip(s, R(x, y, cw - 2, rh));
            int fmt = lv_report(w) && c < v->ncol ? v->col[c].fmt & 3 : 0;
            int px = fmt == 1 ? x + cw - 6 - tw : fmt == 2 ? x + (cw - tw) / 2 : tx;
            draw_text(s, f, px, y + (rh - font_height(f)) / 2, t, sel && (c == 0 || full) && active ? T.select_text : fg);
            s->clip = co;
            if (i == v->focus && focus_win == w && c == 0) dotted_rect(s, tx - 2, y, (full ? cw - (tx - 2 - x) : tw + 4), rh, T.text);
            x += cw;
        }
        if (v->ex & 1) hline(s, in.x, y + rh - 1, in.w, mix(bg, T.face_dark, 80));  /* LVS_EX_GRIDLINES */
    }
    s->clip = o;
    int vis = lv_visible(w);
    if (v->n > vis) draw_scrollbar(s, R(r.x + r.w - 18, r.y + 2 + hh, 16, r.h - 4 - hh), v->n, vis, v->top, true);
}
static void lv_changed(w32win_t *w, int i, UINT olds, UINT news) {
    lv_t *v = LV(w);
    struct { NMHDR32 h; int item, sub; UINT nstate, ostate, changed; LONG x, y; LPARAM lp; } nm;
    memset(&nm, 0, sizeof nm); nm.item = i; nm.nstate = news; nm.ostate = olds; nm.changed = 8; nm.lp = i >= 0 && i < v->n ? v->it[i].data : 0;
    notify_parent_sz(w, -101, &nm.h, sizeof nm);                                                 /* LVN_ITEMCHANGED */
}
static void lv_select(w32win_t *w, int i, bool only) {
    lv_t *v = LV(w);
    if (only || (w->style & 4)) for (int k = 0; k < v->n; k++) if (k != i && (v->it[k].state & 2)) { UINT o = v->it[k].state; v->it[k].state &= ~3u; lv_changed(w, k, o, v->it[k].state); }   /* LVS_SINGLESEL */
    if (i >= 0 && i < v->n) { UINT o = v->it[i].state; v->it[i].state |= 3; v->focus = i; if (o != v->it[i].state) lv_changed(w, i, o, v->it[i].state); }
    int vis = lv_visible(w);
    if (v->focus >= 0) { if (v->focus < v->top) v->top = v->focus; if (v->focus >= v->top + vis) v->top = v->focus - vis + 1; }
    draw_control(w);
}
/* LVITEM: mask, iItem, iSubItem, state, stateMask, pszText, cchTextMax, iImage, lParam */
typedef struct { UINT mask; int item, sub; UINT state, smask; void *text; int cch, image; LPARAM lp; } LVITEM32;
typedef struct { UINT mask; int fmt, cx; void *text; int cch, sub, image, order; } LVCOL32;
static void lv_apply(w32win_t *w, lvitem_t *it, const LVITEM32 *li, bool wide) {
    if (li->mask & 1) { char t[512]; if ((intptr_t)li->text == -1) t[0] = 0; else cc_text_in(w, li->text, wide, t, sizeof t); lv_set_text(it, li->sub, t); }
    if (li->sub) return;
    if (li->mask & 2) it->image = li->image;
    if (li->mask & 4) it->data = li->lp;
    if (li->mask & 8) it->state = (it->state & ~li->smask) | (li->state & li->smask);
}
static bool listview_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    lv_t *v = LV(w);
    switch (m) {
    case 0x101B: case 0x1061: {                                                     /* LVM_INSERTCOLUMN */
        const LVCOL32 *c = (const LVCOL32 *)lp; if (v->ncol >= 32 || !c) { *res = -1; return true; }
        int i = clampi((int)wp, 0, v->ncol); memmove(&v->col[i + 1], &v->col[i], (v->ncol - i) * sizeof(lvcol_t)); memset(&v->col[i], 0, sizeof(lvcol_t)); v->ncol++;
        if (c->mask & 4) cc_text_in(w, c->text, m == 0x1061, v->col[i].text, sizeof v->col[i].text);
        v->col[i].width = (c->mask & 2) ? c->cx : 100; v->col[i].fmt = (c->mask & 1) ? c->fmt : 0;
        draw_control(w); *res = i; return true; }
    case 0x101A: case 0x1060: {                                                     /* LVM_SETCOLUMN */
        const LVCOL32 *c = (const LVCOL32 *)lp; if ((int)wp >= v->ncol || !c) { *res = 0; return true; }
        if (c->mask & 4) cc_text_in(w, c->text, m == 0x1060, v->col[wp].text, sizeof v->col[wp].text);
        if (c->mask & 2) v->col[wp].width = c->cx; if (c->mask & 1) v->col[wp].fmt = c->fmt;
        draw_control(w); *res = 1; return true; }
    case 0x1019: case 0x105F: { LVCOL32 *c = (LVCOL32 *)lp; if ((int)wp >= v->ncol || !c) { *res = 0; return true; } if (c->mask & 2) c->cx = v->col[wp].width; if (c->mask & 1) c->fmt = v->col[wp].fmt; if (c->mask & 4) cc_text_out(v->col[wp].text, c->text, c->cch, m == 0x105F); *res = 1; return true; }   /* LVM_GETCOLUMN */
    case 0x101C: if ((int)wp < v->ncol) { memmove(&v->col[wp], &v->col[wp + 1], (v->ncol - wp - 1) * sizeof(lvcol_t)); v->ncol--; draw_control(w); } *res = 1; return true;   /* LVM_DELETECOLUMN */
    case 0x101D: *res = (int)wp < v->ncol ? v->col[wp].width : 0; return true;       /* LVM_GETCOLUMNWIDTH */
    case 0x101E: if ((int)wp < v->ncol) { int cw = (int16_t)LOWORD(lp); v->col[wp].width = cw < 0 ? (cw == -2 ? text_width(ctl_font(w), v->col[wp].text) + 16 : MAX(60, w->w - 24)) : cw; draw_control(w); } *res = 1; return true;   /* LVM_SETCOLUMNWIDTH */
    case 0x1007: case 0x104D: {                                                     /* LVM_INSERTITEM */
        const LVITEM32 *li = (const LVITEM32 *)lp; if (!li) { *res = -1; return true; }
        if (v->n >= v->cap) { v->cap = v->cap * 2 + 32; v->it = krealloc(v->it, v->cap * sizeof(lvitem_t)); }
        int i = clampi(li->item, 0, v->n);
        memmove(&v->it[i + 1], &v->it[i], (v->n - i) * sizeof(lvitem_t)); memset(&v->it[i], 0, sizeof(lvitem_t)); v->n++;
        v->it[i].image = -1;
        LVITEM32 x = *li; x.sub = 0; lv_apply(w, &v->it[i], &x, m == 0x104D);
        if (v->focus >= i && v->focus >= 0) v->focus++;
        draw_control(w); *res = i; return true; }
    case 0x1006: case 0x104C: case 0x102E: case 0x1074: {                           /* LVM_SETITEM, LVM_SETITEMTEXT */
        const LVITEM32 *li = (const LVITEM32 *)lp; int i = (m == 0x102E || m == 0x1074) ? (int)wp : li ? li->item : -1;
        if (!li || i < 0 || i >= v->n) { *res = 0; return true; }
        LVITEM32 x = *li; if (m == 0x102E || m == 0x1074) x.mask = 1;
        lv_apply(w, &v->it[i], &x, m == 0x104C || m == 0x1074);
        draw_control(w); *res = 1; return true; }
    case 0x1005: case 0x104B: case 0x102D: case 0x1073: {                           /* LVM_GETITEM, LVM_GETITEMTEXT */
        LVITEM32 *li = (LVITEM32 *)lp; int i = (m == 0x102D || m == 0x1073) ? (int)wp : li ? li->item : -1;
        if (!li || i < 0 || i >= v->n) { *res = 0; return true; }
        UINT mask = (m == 0x102D || m == 0x1073) ? 1 : li->mask;
        const char *t = lv_text(v, i, li->sub);
        if (mask & 1) cc_text_out(t, li->text, li->cch, m == 0x104B || m == 0x1073);
        if (mask & 2) li->image = v->it[i].image; if (mask & 4) li->lp = v->it[i].data; if (mask & 8) li->state = v->it[i].state & li->smask;
        *res = (m == 0x102D || m == 0x1073) ? (LRESULT)strlen(t) : 1; return true; }
    case 0x1004: *res = v->n; return true;                                          /* LVM_GETITEMCOUNT */
    case 0x1008: if ((int)wp < v->n && (int)wp >= 0) { lvitem_t *it = &v->it[wp]; for (int k = 0; k < it->nsub; k++) kfree(it->sub[k]); kfree(it->sub); memmove(&v->it[wp], &v->it[wp + 1], (v->n - wp - 1) * sizeof(lvitem_t)); v->n--; if (v->focus >= v->n) v->focus = v->n - 1; draw_control(w); *res = 1; } else *res = 0; return true;
    case 0x1009: for (int i = 0; i < v->n; i++) { for (int k = 0; k < v->it[i].nsub; k++) kfree(v->it[i].sub[k]); kfree(v->it[i].sub); } v->n = 0; v->top = 0; v->focus = -1; draw_control(w); *res = 1; return true;
    case 0x100C: {                                                                  /* LVM_GETNEXTITEM */
        UINT fl = (UINT)lp; int start = (int)wp;
        for (int i = start + 1; i < v->n; i++) { if ((fl & 2) && !(v->it[i].state & 2)) continue; if ((fl & 1) && !(v->it[i].state & 1)) continue; *res = i; return true; }
        *res = -1; return true; }
    case 0x102B: {                                                                  /* LVM_SETITEMSTATE: wp -1 = all */
        const LVITEM32 *li = (const LVITEM32 *)lp; if (!li) { *res = 0; return true; }
        int a = (int)wp < 0 ? 0 : (int)wp, b = (int)wp < 0 ? v->n : MIN((int)wp + 1, v->n);
        for (int i = a; i < b; i++) { UINT o = v->it[i].state; v->it[i].state = (o & ~li->smask) | (li->state & li->smask); if (li->state & li->smask & 1) v->focus = i; if (o != v->it[i].state) lv_changed(w, i, o, v->it[i].state); }
        draw_control(w); *res = 1; return true; }
    case 0x102C: *res = (int)wp >= 0 && (int)wp < v->n ? v->it[wp].state & (UINT)lp : 0; return true;   /* LVM_GETITEMSTATE */
    case 0x1032: { int c = 0; for (int i = 0; i < v->n; i++) if (v->it[i].state & 2) c++; *res = c; return true; }   /* LVM_GETSELECTEDCOUNT */
    case 0x1013: { int i = (int)wp, vis = lv_visible(w); if (i >= 0 && i < v->n) { if (i < v->top) v->top = i; if (i >= v->top + vis) v->top = i - vis + 1; draw_control(w); } *res = 1; return true; }   /* LVM_ENSUREVISIBLE */
    case 0x1027: *res = v->top; return true;                                        /* LVM_GETTOPINDEX */
    case 0x1028: *res = lv_visible(w); return true;                                 /* LVM_GETCOUNTPERPAGE */
    case 0x1036: { DWORD o = v->ex; v->ex = wp ? (v->ex & ~(DWORD)wp) | ((DWORD)lp & (DWORD)wp) : (DWORD)lp; draw_control(w); *res = o; return true; }   /* LVM_SETEXTENDEDLISTVIEWSTYLE */
    case 0x1037: *res = v->ex; return true;
    case 0x1003: { HANDLE o; if (wp == 1) { o = v->il_small; v->il_small = lp; } else { o = v->il; v->il = lp; if (!v->il_small && wp == 0) {} } draw_control(w); *res = o; return true; }   /* LVM_SETIMAGELIST */
    case 0x1002: *res = wp == 1 ? v->il_small : v->il; return true;
    case 0x1001: v->bk = lp == 0xFFFFFFFF ? 0xFFFFFFFF : cref((COLORREF)lp); draw_control(w); *res = 1; return true;   /* LVM_SETBKCOLOR */
    case 0x1024: v->fg = cref((COLORREF)lp); draw_control(w); *res = 1; return true; /* LVM_SETTEXTCOLOR */
    case 0x1026: v->tbk = cref((COLORREF)lp); *res = 1; return true;                 /* LVM_SETTEXTBKCOLOR */
    case 0x1000: *res = v->bk == 0xFFFFFFFF ? colref(T.window_bg) : colref(v->bk); return true;
    case 0x102F: if (w->style & 0x1000) { v->owner = true; int n = (int)wp; if (n > v->cap) { v->cap = n; v->it = krealloc(v->it, v->cap * sizeof(lvitem_t)); } for (int i = v->n; i < n; i++) { memset(&v->it[i], 0, sizeof(lvitem_t)); v->it[i].image = -1; } v->n = n; draw_control(w); } *res = 1; return true;   /* LVM_SETITEMCOUNT */
    case 0x100E: case 0x1038: {                                                     /* LVM_GETITEMRECT, LVM_GETSUBITEMRECT */
        RECT32 *r = (RECT32 *)lp; int i = (int)wp; if (!r || i < 0 || i >= v->n) { *res = 0; return true; }
        int y = 2 + lv_head_h(w) + (i - v->top) * lv_row_h(w);
        r->l = 2; r->r = w->w - 2; r->t = y; r->b = y + lv_row_h(w);
        if (m == 0x1038 && lv_report(w)) { int sub = r->t; (void)sub; int x = 2; int si = ((RECT32 *)lp)->t; (void)si; r->l = x; }
        *res = 1; return true; }
    case 0x1012: case 0x1039: {                                                     /* LVM_HITTEST, LVM_SUBITEMHITTEST */
        struct { LONG x, y; UINT flags; int item, sub; } *h = (void *)lp;
        int row = (h->y - 2 - lv_head_h(w)) / lv_row_h(w) + v->top;
        h->item = h->y > 2 + lv_head_h(w) && row >= 0 && row < v->n ? row : -1; h->flags = h->item >= 0 ? 0xE : 1; h->sub = 0;
        if (h->item >= 0 && lv_report(w)) { int x = 2; for (int c = 0; c < v->ncol; c++) { if (h->x >= x && h->x < x + v->col[c].width) { h->sub = c; break; } x += v->col[c].width; } }
        *res = h->item; return true; }
    case 0x1030: case 0x1051: {                                                     /* LVM_SORTITEMS(EX): a stable insertion sort with the program's comparer */
        int (WINAPI *cmp)(LPARAM, LPARAM, LPARAM) = (void *)lp;
        for (int i = 1; i < v->n; i++) for (int j = i; j > 0; j--) {
            int c = m == 0x1030 ? cmp(v->it[j - 1].data, v->it[j].data, wp) : cmp(j - 1, j, wp);
            if (c <= 0) break;
            lvitem_t t = v->it[j]; v->it[j] = v->it[j - 1]; v->it[j - 1] = t;
        }
        draw_control(w); *res = 1; return true; }
    case 0x100D: case 0x1053: {                                                     /* LVM_FINDITEM: by text or lParam */
        struct { UINT flags; void *str; LPARAM lp; } *fi = (void *)lp; char want[256]; cc_text_in(w, fi->str, m == 0x1053, want, sizeof want);
        for (int i = (int)wp + 1; i < v->n; i++) { if ((fi->flags & 1) && v->it[i].data == fi->lp) { *res = i; return true; } if ((fi->flags & 2) && !strcasecmp(lv_text(v, i, 0), want)) { *res = i; return true; } if ((fi->flags & 8) && !strncasecmp(lv_text(v, i, 0), want, strlen(want))) { *res = i; return true; } }
        *res = -1; return true; }
    case 0x101F: *res = 0; return true;                                             /* LVM_GETHEADER: none of its own */
    case 0x1014: { int dy = (int)lp / lv_row_h(w); v->top = clampi(v->top + dy, 0, MAX(0, v->n - lv_visible(w))); draw_control(w); *res = 1; return true; }   /* LVM_SCROLL */
    case 0x1015: case 0x1029: case 0x1011: case 0x103C: case 0x1050: *res = 1; return true;   /* redraw, arrange, image positions, ... */
    case 0x1017: case 0x1018: *res = 0; return true;                                 /* LVM_EDITLABEL: no in-place editing */
    case 0x1035: case 0x108F: *res = 0; return true;                                 /* LVM_SETVIEW: report kept */
    }
    return false;
}
static bool listview_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    lv_t *v = LV(w);
    int hh = lv_head_h(w), rh = lv_row_h(w);
    if (type == ME_WHEEL) { v->top = clampi(v->top + (btn > 0 ? -3 : 3), 0, MAX(0, v->n - lv_visible(w))); draw_control(w); return true; }
    if (type != ME_DOWN && type != ME_DBLCLICK) return true;
    if (focus_win != w) { w32win_t *o = focus_win; focus_win = w; if (o && o->ctl) draw_control(o); }
    if (lx >= w->w - 20 && v->n > lv_visible(w)) {                                  /* the scroll bar */
        v->top = clampi(ly < w->h / 2 ? v->top - lv_visible(w) : v->top + lv_visible(w), 0, MAX(0, v->n - lv_visible(w))); draw_control(w); return true;
    }
    if (ly < 2 + hh) {                                                              /* a header click: LVN_COLUMNCLICK */
        int x = 2; for (int c = 0; c < v->ncol; c++) { if (lx >= x && lx < x + v->col[c].width) { struct { NMHDR32 h; int item, sub; UINT a, b, c; LONG px, py; LPARAM lp; } nm; memset(&nm, 0, sizeof nm); nm.item = -1; nm.sub = c; notify_parent_sz(w, -108, &nm.h, sizeof nm); break; } x += v->col[c].width; }
        return true;
    }
    int row = (ly - 2 - hh) / rh + v->top;
    struct { NMHDR32 h; int item, sub; LONG x, y; UINT newst, oldst, changed; LPARAM lp; UINT keys; } nmia;
    memset(&nmia, 0, sizeof nmia); nmia.x = lx; nmia.y = ly;
    if (row >= 0 && row < v->n) {
        if ((v->ex & 4) && lx < 22) { UINT o = v->it[row].state; v->it[row].state ^= 0x3000; if (!(v->it[row].state & 0x3000)) v->it[row].state |= 0x1000; lv_changed(w, row, o, v->it[row].state); draw_control(w); }
        bool ctrl = keystate[0x11] & 0x80;
        if (ctrl && !(w->style & 4)) { UINT o = v->it[row].state; v->it[row].state ^= 2; v->focus = row; lv_changed(w, row, o, v->it[row].state); draw_control(w); }
        else lv_select(w, row, true);
        nmia.item = row;
    } else { lv_select(w, -1, true); nmia.item = -1; }
    notify_parent_sz(w, (btn & MB_RIGHT) ? (type == ME_DBLCLICK ? -6 : -5) : (type == ME_DBLCLICK ? -3 : -2), &nmia.h, sizeof nmia);   /* NM_CLICK / NM_DBLCLK / NM_RCLICK / NM_RDBLCLK */
    return true;
}
static bool listview_key(w32win_t *w, key_event_t *e) {
    lv_t *v = LV(w);
    if (!e->pressed) return true;
    int k = e->key, f = v->focus < 0 ? 0 : v->focus, vis = lv_visible(w);
    if (k == KEY_UP) f--; else if (k == KEY_DOWN) f++; else if (k == KEY_PGUP) f -= vis; else if (k == KEY_PGDN) f += vis; else if (k == KEY_HOME) f = 0; else if (k == KEY_END) f = v->n - 1;
    else if (k == '\n' || k == '\r') { NMHDR32 nm; notify_parent_sz(w, -4, &nm, sizeof nm); return true; }   /* NM_RETURN */
    else { struct { NMHDR32 h; WORD vk; UINT flags; } kd; memset(&kd, 0, sizeof kd); kd.vk = k == KEY_DELETE ? 0x2E : (k >= 'a' && k <= 'z') ? k - 32 : k; notify_parent_sz(w, -155, &kd.h, sizeof kd); return true; }   /* LVN_KEYDOWN */
    if (v->n) lv_select(w, clampi(f, 0, v->n - 1), true);
    return true;
}

/* ================================================================ dispatch (called from user32.c) */
static int cc_class(const char *n) {
    static const struct { const char *name; int ctl; } t[] = {
        { "ToolbarWindow32", CTL_TOOLBAR }, { "msctls_statusbar32", CTL_STATUS }, { "ReBarWindow32", CTL_REBAR }, { "msctls_progress32", CTL_PROGRESS },
        { "msctls_trackbar32", CTL_TRACKBAR }, { "msctls_updown32", CTL_UPDOWN }, { "SysTabControl32", CTL_TAB }, { "SysListView32", CTL_LISTVIEW },
        { "SysHeader32", CTL_HEADER }, { "tooltips_class32", CTL_TOOLTIP }, { "SysLink", CTL_STATIC }, { "SysDateTimePick32", CTL_QUIET }, { "SysMonthCal32", CTL_QUIET },
        { "SysIPAddress32", CTL_EDIT }, { "msctls_hotkey32", CTL_EDIT }, { "SysAnimate32", CTL_QUIET }, { "SysPager", CTL_QUIET }, { "NativeFontCtl", CTL_QUIET },
        { "ComboLBox", CTL_LISTBOX }, { "RICHEDIT50W", CTL_EDIT }, { "RichEdit", CTL_EDIT }, { "RICHEDIT", CTL_EDIT }, { "RichEdit20", CTL_EDIT }, { "MDIClient", CTL_QUIET },
        { "DirectUIHWND", CTL_QUIET }, { "Scrollbar", 7 },
    };
    for (unsigned i = 0; i < ARRAY_LEN(t); i++) if (!strcasecmp(n, t[i].name)) return t[i].ctl;
    if (!strncasecmp(n, "RichEdit", 8)) return CTL_EDIT;
    if (!strncasecmp(n, "msctls_", 7)) return CTL_QUIET;
    return -1;
}
static const char *cc_class_name(int ctl) {
    switch (ctl) { case CTL_TOOLBAR: return "ToolbarWindow32"; case CTL_STATUS: return "msctls_statusbar32"; case CTL_REBAR: return "ReBarWindow32"; case CTL_PROGRESS: return "msctls_progress32";
    case CTL_TRACKBAR: return "msctls_trackbar32"; case CTL_UPDOWN: return "msctls_updown32"; case CTL_TAB: return "SysTabControl32"; case CTL_LISTVIEW: return "SysListView32";
    case CTL_HEADER: return "SysHeader32"; case CTL_TOOLTIP: return "tooltips_class32"; }
    return "Static";
}
static void cc_created(w32win_t *w) {
    if (w->ctl == CTL_TOOLBAR) tb_layout(w);
    else if (w->ctl == CTL_STATUS) sb_layout(w);
    else if (w->ctl == CTL_REBAR) rb_layout(w);
}
static void cc_draw(w32win_t *c, canvas_t *s, rect_t r, int f) {
    switch (c->ctl) {
    case CTL_TOOLBAR: draw_toolbar(c, s, r, f); break;
    case CTL_STATUS: draw_status(c, s, r, f); break;
    case CTL_REBAR: draw_rebar(c, s, r, f); break;
    case CTL_PROGRESS: cc_draw_progress(c, s, r); break;
    case CTL_TRACKBAR: draw_trackbar(c, s, r); break;
    case CTL_UPDOWN: draw_updown(c, s, r); break;
    case CTL_TAB: draw_tabs(c, s, r, f); break;
    case CTL_LISTVIEW: draw_listview(c, s, r, f); break;
    case CTL_HEADER: fill_rect(s, r.x, r.y, r.w, r.h, T.face); bevel_deep(s, r.x, r.y, r.w, r.h, true); break;
    default: break;
    }
}
static bool cc_msg(w32win_t *w, UINT m, WPARAM wp, LPARAM lp, LRESULT *res) {
    if (m == WM_CC_NOTIFY) {                                       /* a queued notification: now in the program's task */
        NMHDR32 *nm = (NMHDR32 *)lp;
        *res = w->parent && W((uintptr_t)w->parent) ? send(w->parent, 0x4E, nm->id, lp) : 0;
        kfree(nm); return true;
    }
    switch (w->ctl) {
    case CTL_TOOLBAR: return toolbar_msg(w, m, wp, lp, res);
    case CTL_STATUS: return status_msg(w, m, wp, lp, res);
    case CTL_REBAR: return rebar_msg(w, m, wp, lp, res);
    case CTL_PROGRESS: return progress_msg(w, m, wp, lp, res);
    case CTL_TRACKBAR: return trackbar_msg(w, m, wp, lp, res);
    case CTL_UPDOWN: return updown_msg(w, m, wp, lp, res);
    case CTL_TAB: return tab_msg(w, m, wp, lp, res);
    case CTL_LISTVIEW: return listview_msg(w, m, wp, lp, res);
    case CTL_TOOLTIP: case CTL_QUIET: case CTL_HEADER:
        if (m >= 0x400 && m < 0x2100) { *res = 0; return true; }
        return false;
    }
    return false;
}
static bool cc_mouse(w32win_t *w, int type, int lx, int ly, int btn) {
    if (!w->enabled) return true;
    switch (w->ctl) {
    case CTL_TOOLBAR: return toolbar_mouse(w, type, lx, ly, btn);
    case CTL_TRACKBAR: return trackbar_mouse(w, type, lx, ly, btn);
    case CTL_UPDOWN: return updown_mouse(w, type, lx, ly, btn);
    case CTL_TAB: return tab_mouse(w, type, lx, ly, btn);
    case CTL_LISTVIEW: return listview_mouse(w, type, lx, ly, btn);
    }
    return true;
}
static bool cc_key(w32win_t *w, key_event_t *e) {
    if (w->ctl == CTL_LISTVIEW) return listview_key(w, e);
    return false;
}
