/* Solitaire: Klondike patience with cards drawn here (no card artwork is copied: the faces are
 * rank, suit symbols built from circles and polygons, and a crown panel for the court cards;
 * the back is a lattice around a small globe).
 *
 * Mouse: drag cards between piles, click the stock to deal, double-click a card to send it to
 * its foundation. Keyboard: Left/Right pick a pile (stock, waste, the four foundations, the seven
 * columns), Up/Down how deep in a column, Space or Enter picks up and puts down, F sends the
 * card to a foundation, Ctrl+Z undoes, F2 deals a new game, Esc lets go of a held card.
 * Scoring as is usual for Klondike: +5 to a column, +10 to a foundation, +5 for turning a
 * card over, -15 back from a foundation, -100 for going through the deck again (draw one). */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"
#include "raster.h"

#define CW 72
#define CH 100
#define GAP 14
#define TOP 12
#define TAB_Y (TOP + CH + 22)
#define FAN_DOWN 6
#define FAN_UP 22
#define WIN_W (GAP + 7 * (CW + GAP))
#define WIN_H 560

enum { P_STOCK, P_WASTE, P_F0, P_F1, P_F2, P_F3, P_T0, P_T1, P_T2, P_T3, P_T4, P_T5, P_T6, P_N };
typedef struct {
    int8_t stock[52], waste[52], fnd[4][13], tab[7][20];
    int8_t nstock, nwaste, nf[4], nt[7], up[7];          /* up: index of the first face-up card in a column */
    int score, moves;
} deal_t;
typedef struct {
    deal_t d;
    deal_t undo[32]; int nundo;
    int draw3;
    uint32_t start_ms, end_ms; bool won;
    /* a held run: from pile hp (a column: from index hi), drawn at the pointer */
    bool holding; int hp, hi, hx, hy, offx, offy; bool by_key;
    int cur, curi;                                       /* keyboard cursor: pile, depth */
    int press_x, press_y; bool dragged;
    int best;
    /* the celebration: cards flying up and bursting */
    struct { float x, y, vx, vy; int card; } fly[52]; int nfly;
} sol_t;
static window_t *sol_win;

static int rank_of(int c) { return c % 13; }
static int suit_of(int c) { return c / 13; }              /* 0 spades, 1 hearts, 2 diamonds, 3 clubs */
static bool red(int c) { int s = suit_of(c); return s == 1 || s == 2; }

/* ---------------- drawing a card ---------------- */
static raster_path_t *rp;
static void poly(canvas_t *c, const int *xy, int n, uint32_t col) {
    if (!rp) rp = raster_path_new();
    if (!rp) return;
    raster_reset(rp);
    raster_move_to(rp, RASTER_FIX(xy[0]), RASTER_FIX(xy[1]));
    for (int i = 1; i < n; i++) raster_line_to(rp, RASTER_FIX(xy[i * 2]), RASTER_FIX(xy[i * 2 + 1]));
    raster_close(rp);
    raster_fill(c, rp, col, false);
}
/* a suit symbol centred on (x, y), about s pixels tall */
static void suit_glyph(canvas_t *c, int suit, int x, int y, int s, uint32_t col) {
    int r = s * 27 / 100 + 1;
    switch (suit) {
    case 1: {                                            /* heart: two lobes, a point */
        fill_circle(c, x - r + 1, y - r / 2, r, col); fill_circle(c, x + r - 1, y - r / 2, r, col);
        int t[] = { x - 2 * r + 1, y - r / 4, x + 2 * r - 1, y - r / 4, x, y + s / 2 };
        poly(c, t, 3, col);
        break;
    }
    case 2: { int d[] = { x, y - s / 2, x + s * 2 / 5, y, x, y + s / 2, x - s * 2 / 5, y }; poly(c, d, 4, col); break; }
    case 0: {                                            /* spade: a heart upside down, a stem */
        fill_circle(c, x - r + 1, y + r / 3, r, col); fill_circle(c, x + r - 1, y + r / 3, r, col);
        int t[] = { x - 2 * r + 1, y + r / 6, x + 2 * r - 1, y + r / 6, x, y - s / 2 };
        poly(c, t, 3, col);
        int st[] = { x, y + r / 3, x + r / 2 + 1, y + s / 2, x - r / 2 - 1, y + s / 2 };
        poly(c, st, 3, col);
        break;
    }
    default: {                                           /* club: three leaves, a stem */
        int q = r * 9 / 10 + 1;
        fill_circle(c, x, y - q, q, col); fill_circle(c, x - q - 1, y + q / 3, q, col); fill_circle(c, x + q + 1, y + q / 3, q, col);
        fill_circle(c, x, y, q / 2, col);
        int st[] = { x, y, x + q * 2 / 3, y + s / 2, x - q * 2 / 3, y + s / 2 };
        poly(c, st, 3, col);
        break;
    }
    }
}
static const char *ranks[13] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
static void card_back(canvas_t *c, int x, int y) {
    fill_round_rect(c, x, y, CW, CH, 6, HEX(0x24306A));
    gradient_round_rect(c, x + 4, y + 4, CW - 8, CH - 8, 4, HEX(0x3A6FD8), HEX(0x173A8C));
    for (int yy = y + 8; yy < y + CH - 8; yy += 8)
        for (int xx = x + 8 + ((yy - y) / 8 % 2) * 4; xx < x + CW - 8; xx += 8) fill_circle(c, xx, yy, 1, HEX(0x8FB4F2));
    int cx = x + CW / 2, cy = y + CH / 2;
    fill_circle(c, cx, cy, 15, HEX(0xF4F6F9)); fill_circle(c, cx, cy, 13, HEX(0x2D7FCB));
    fill_circle(c, cx - 4, cy - 3, 5, HEX(0x6CC46C)); fill_circle(c, cx + 5, cy + 4, 4, HEX(0x6CC46C));
    draw_round_rect(c, x, y, CW, CH, 6, HEX(0x101830));
}
static void card_face(canvas_t *c, int card, int x, int y, bool lit) {
    uint32_t ink = red(card) ? HEX(0xC8102E) : HEX(0x161616);
    fill_round_rect(c, x, y, CW, CH, 6, lit ? HEX(0xFFF6C8) : WHITE);
    draw_round_rect(c, x, y, CW, CH, 6, HEX(0x707070));
    int r = rank_of(card), s = suit_of(card);
    draw_text(c, FONT_UI_BOLD, x + 5, y + 4, ranks[r], ink);
    suit_glyph(c, s, x + 10, y + 26, 11, ink);
    int bw = text_width(FONT_UI_BOLD, ranks[r]);
    draw_text(c, FONT_UI_BOLD, x + CW - 5 - bw, y + CH - 19, ranks[r], ink);
    if (r >= 10) {                                       /* a court card: a panel with a crown */
        rect_t p = R(x + 18, y + 18, CW - 30, CH - 36);
        gradient_round_rect(c, p.x, p.y, p.w, p.h, 4, red(card) ? HEX(0xFFE3E6) : HEX(0xE3EAF7), red(card) ? HEX(0xF4B8C0) : HEX(0xB9C8E6));
        draw_round_rect(c, p.x, p.y, p.w, p.h, 4, ink);
        int cx = p.x + p.w / 2, cy = p.y + 26;
        int crown[] = { cx - 14, cy + 8, cx - 14, cy - 6, cx - 7, cy + 1, cx, cy - 10, cx + 7, cy + 1, cx + 14, cy - 6, cx + 14, cy + 8 };
        poly(c, crown, 7, HEX(0xE0A020));
        fill_circle(c, cx, cy - 10, 2, HEX(0xC8102E));
        draw_text(c, FONT_LARGE_BOLD, cx - text_width(FONT_LARGE_BOLD, ranks[r]) / 2, cy + 14, ranks[r], ink);
        suit_glyph(c, s, cx, p.y + p.h - 12, 12, ink);
    } else suit_glyph(c, s, x + CW / 2, y + CH / 2 + 4, r == 0 ? 34 : 26, ink);
}
static void slot(canvas_t *c, int x, int y, const char *label) {
    draw_round_rect(c, x, y, CW, CH, 6, HEX(0x5FA05F));
    if (label) draw_text(c, FONT_LARGE_BOLD, x + (CW - text_width(FONT_LARGE_BOLD, label)) / 2, y + CH / 2 - 10, label, HEX(0x5FA05F));
}

/* ---------------- piles ---------------- */
static rect_t pile_rect(int p) {
    if (p == P_STOCK) return R(GAP, TOP, CW, CH);
    if (p == P_WASTE) return R(GAP + CW + GAP, TOP, CW + 30, CH);
    if (p <= P_F3) return R(GAP + (3 + p - P_F0) * (CW + GAP), TOP, CW, CH);
    return R(GAP + (p - P_T0) * (CW + GAP), TAB_Y, CW, WIN_H - TAB_Y);
}
static int card_y(deal_t *d, int t, int i) {           /* y of card i in column t */
    int y = TAB_Y;
    for (int k = 0; k < i; k++) y += k < d->up[t] ? FAN_DOWN : FAN_UP;
    return y;
}
static int top_of(deal_t *d, int p) {
    if (p == P_WASTE) return d->nwaste ? d->waste[d->nwaste - 1] : -1;
    if (p >= P_F0 && p <= P_F3) { int f = p - P_F0; return d->nf[f] ? d->fnd[f][d->nf[f] - 1] : -1; }
    if (p >= P_T0) { int t = p - P_T0; return d->nt[t] ? d->tab[t][d->nt[t] - 1] : -1; }
    return -1;
}
static void deal_new(sol_t *s) {
    int8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    for (int i = 51; i > 0; i--) { int j = rand_next() % (i + 1); int8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    deal_t *d = &s->d;
    memset(d, 0, sizeof *d);
    int k = 0;
    for (int t = 0; t < 7; t++) { for (int i = 0; i <= t; i++) d->tab[t][i] = deck[k++]; d->nt[t] = t + 1; d->up[t] = t; }
    for (; k < 52; k++) d->stock[d->nstock++] = deck[k];
    s->nundo = 0; s->won = false; s->holding = false; s->nfly = 0;
    s->start_ms = timer_ticks(); s->end_ms = 0; s->cur = P_STOCK; s->curi = 0;
}
static void push_undo(sol_t *s) {
    if (s->nundo == 32) { memmove(s->undo, s->undo + 1, sizeof s->undo[0] * 31); s->nundo--; }
    s->undo[s->nundo++] = s->d;
}
static void flip_tops(sol_t *s) {                       /* a column whose face-up cards are gone turns its top over */
    deal_t *d = &s->d;
    for (int t = 0; t < 7; t++) if (d->nt[t] && d->up[t] >= d->nt[t]) { d->up[t] = d->nt[t] - 1; d->score += 5; }
}
static void check_win(sol_t *s) {
    deal_t *d = &s->d;
    if (d->nf[0] + d->nf[1] + d->nf[2] + d->nf[3] != 52 || s->won) return;
    s->won = true; s->end_ms = timer_ticks();
    int secs = (s->end_ms - s->start_ms) / 1000;
    if (secs < 30) secs = 30;
    d->score += 700000 / secs;                           /* a quick game earns a bonus */
    if (d->score > s->best) { s->best = d->score; char b[16]; snprintf(b, sizeof b, "%d", s->best); settings_set("solitaire_best", b); settings_save(); }
    s->nfly = 0;
    for (int f = 0; f < 4; f++) for (int i = 0; i < 13; i++) {
        rect_t r = pile_rect(P_F0 + f);
        int a = rand_next() % 360;
        float sp = 120 + rand_next() % 260;
        s->fly[s->nfly].x = r.x; s->fly[s->nfly].y = r.y;
        s->fly[s->nfly].vx = sp * ((a % 180) - 90) / 90.0f; s->fly[s->nfly].vy = -(200 + rand_next() % 300);
        s->fly[s->nfly].card = d->fnd[f][i]; s->nfly++;
    }
}
static void deal_stock(sol_t *s) {
    deal_t *d = &s->d;
    push_undo(s);
    if (!d->nstock) {                                    /* through the deck again */
        if (!d->nwaste) { s->nundo--; return; }
        for (int i = d->nwaste - 1; i >= 0; i--) d->stock[d->nstock++] = d->waste[i];
        d->nwaste = 0;
        if (!s->draw3) d->score = MAX(0, d->score - 100);
    } else {
        int n = s->draw3 ? MIN(3, d->nstock) : 1;
        for (int i = 0; i < n; i++) d->waste[d->nwaste++] = d->stock[--d->nstock];
    }
    d->moves++;
}
/* can the run starting with card c go onto pile p? */
static bool fits(deal_t *d, int c, int run, int p) {
    if (p >= P_F0 && p <= P_F3) {
        if (run != 1) return false;
        int f = p - P_F0, top = top_of(d, p);
        return top < 0 ? rank_of(c) == 0 && (d->nf[f] == 0) : suit_of(top) == suit_of(c) && rank_of(c) == rank_of(top) + 1;
    }
    if (p >= P_T0) {
        int top = top_of(d, p);
        return top < 0 ? rank_of(c) == 12 : red(top) != red(c) && rank_of(c) + 1 == rank_of(top);
    }
    return false;
}
/* the held cards: how many, and the first */
static int held_run(sol_t *s, int *first) {
    deal_t *d = &s->d;
    if (s->hp == P_WASTE) { *first = d->waste[d->nwaste - 1]; return 1; }
    if (s->hp >= P_F0 && s->hp <= P_F3) { *first = top_of(d, s->hp); return 1; }
    int t = s->hp - P_T0; *first = d->tab[t][s->hi]; return d->nt[t] - s->hi;
}
/* move the held run to p: true when it went */
static bool drop_on(sol_t *s, int p) {
    deal_t *d = &s->d;
    int first, n = held_run(s, &first);
    if (p == s->hp || !fits(d, first, n, p)) return false;
    push_undo(s);
    int8_t run[20];
    if (s->hp == P_WASTE) { run[0] = d->waste[--d->nwaste]; }
    else if (s->hp >= P_F0 && s->hp <= P_F3) { int f = s->hp - P_F0; run[0] = d->fnd[f][--d->nf[f]]; d->score = MAX(0, d->score - 15); }
    else { int t = s->hp - P_T0; for (int i = 0; i < n; i++) run[i] = d->tab[t][s->hi + i]; d->nt[t] = s->hi; }
    if (p >= P_F0 && p <= P_F3) { int f = p - P_F0; d->fnd[f][d->nf[f]++] = run[0]; d->score += 10; }
    else { int t = p - P_T0; for (int i = 0; i < n; i++) d->tab[t][d->nt[t]++] = run[i]; if (s->hp == P_WASTE) d->score += 5; }
    d->moves++;
    flip_tops(s);
    check_win(s);
    return true;
}
/* send the top card of pile p to a foundation */
static bool to_foundation(sol_t *s, int p) {
    deal_t *d = &s->d;
    int c = top_of(d, p);
    if (c < 0 || (p >= P_F0 && p <= P_F3)) return false;
    s->hp = p; s->hi = p >= P_T0 ? d->nt[p - P_T0] - 1 : 0;
    for (int f = 0; f < 4; f++) if (drop_on(s, P_F0 + f)) return true;
    return false;
}

/* ---------------- painting ---------------- */
static void sol_paint(window_t *w) {
    sol_t *s = w->data; canvas_t *c = w->surf; deal_t *d = &s->d;
    gradient_v(c, 0, 0, c->w, c->h, HEX(0x1E7A3A), HEX(0x0F5226));
    /* stock and waste */
    rect_t r = pile_rect(P_STOCK);
    if (d->nstock) { card_back(c, r.x, r.y); if (d->nstock > 1) card_back(c, r.x - 2, r.y - 2); }
    else slot(c, r.x, r.y, d->nwaste ? "O" : NULL);
    r = pile_rect(P_WASTE);
    int shown = s->draw3 ? MIN(3, d->nwaste) : MIN(1, d->nwaste);
    for (int i = 0; i < shown; i++) {
        bool held = s->holding && s->hp == P_WASTE && i == shown - 1;
        if (!held) card_face(c, d->waste[d->nwaste - shown + i], r.x + i * 14, r.y, false);
    }
    if (!d->nwaste) slot(c, r.x, r.y, NULL);
    for (int f = 0; f < 4; f++) {
        r = pile_rect(P_F0 + f);
        int n = d->nf[f] - (s->holding && s->hp == P_F0 + f ? 1 : 0);
        if (n > 0) card_face(c, d->fnd[f][n - 1], r.x, r.y, false);
        else { slot(c, r.x, r.y, NULL); suit_glyph(c, f, r.x + CW / 2, r.y + CH / 2, 26, HEX(0x3E8A4E)); }
    }
    for (int t = 0; t < 7; t++) {
        r = pile_rect(P_T0 + t);
        if (!d->nt[t]) slot(c, r.x, r.y, "K");
        int n = d->nt[t];
        if (s->holding && s->hp == P_T0 + t) n = s->hi;
        for (int i = 0; i < n; i++) {
            int y = card_y(d, t, i);
            if (i < d->up[t]) card_back(c, r.x, y); else card_face(c, d->tab[t][i], r.x, y, false);
        }
    }
    /* the keyboard cursor */
    if (s->by_key || (s->holding && s->by_key)) {
        rect_t cr = pile_rect(s->cur);
        if (s->cur >= P_T0) { int t = s->cur - P_T0; int i = d->nt[t] ? CLAMP(s->curi, d->up[t], d->nt[t] - 1) : 0; cr.y = d->nt[t] ? card_y(d, t, i) : TAB_Y; cr.h = CH; }
        for (int k = 0; k < 3; k++) draw_round_rect(c, cr.x - 3 - k, cr.y - 3 - k, cr.w + 6 + 2 * k, cr.h + 6 + 2 * k, 8, HEX(0xFFD54A));
    }
    /* the held cards, at the pointer (or above the cursor) */
    if (s->holding) {
        int first, n = held_run(s, &first);
        int hx = s->hx - s->offx, hy = s->hy - s->offy;
        if (s->by_key) { rect_t cr = pile_rect(s->cur); hx = cr.x + 10; hy = (s->cur >= P_T0 ? card_y(d, s->cur - P_T0, d->nt[s->cur - P_T0]) : cr.y) - 30; }
        for (int i = 0; i < n; i++) {
            int card = s->hp >= P_T0 ? d->tab[s->hp - P_T0][s->hi + i] : first;
            blend_rect(c, hx + 4, hy + i * FAN_UP + 5, CW, CH, BLACK, 60);
            card_face(c, card, hx, hy + i * FAN_UP, false);
        }
    }
    /* the score bar */
    char b[160]; uint32_t secs = ((s->end_ms ? s->end_ms : timer_ticks()) - s->start_ms) / 1000;
    snprintf(b, sizeof b, "Score: %d    Moves: %d    Time: %u:%02u    Best: %d", d->score, d->moves, secs / 60, secs % 60, s->best);
    blend_rect(c, 0, c->h - 24, c->w, 24, BLACK, 90);
    draw_text(c, FONT_UI_BOLD, 10, c->h - 19, b, WHITE);
    draw_text(c, FONT_UI, c->w - 300, c->h - 19, "F2 new game   Ctrl+Z undo   arrows + Space", HEX(0xC8E6C8));
    if (s->won) {
        for (int i = 0; i < s->nfly; i++) card_face(c, s->fly[i].card, (int)s->fly[i].x, (int)s->fly[i].y, true);
        const char *m = "You won! F2 deals a new game.";
        int tw = text_width(FONT_LARGE_BOLD, m);
        fill_round_rect_blend(c, (c->w - tw) / 2 - 16, c->h / 2 - 24, tw + 32, 48, 8, BLACK, 160);
        draw_text(c, FONT_LARGE_BOLD, (c->w - tw) / 2, c->h / 2 - 11, m, HEX(0xFFE36A));
    }
}
static void sol_tick(window_t *w) {
    sol_t *s = w->data;
    if (s->won && s->nfly) {
        for (int i = 0; i < s->nfly; i++) {
            s->fly[i].vy += 900 * 0.033f;
            s->fly[i].x += s->fly[i].vx * 0.033f; s->fly[i].y += s->fly[i].vy * 0.033f;
            if (s->fly[i].y > WIN_H - CH) { s->fly[i].y = WIN_H - CH; s->fly[i].vy *= -0.7f; }
            if (s->fly[i].x < 0 || s->fly[i].x > WIN_W - CW) s->fly[i].vx = -s->fly[i].vx;
        }
        w->dirty = true;
        return;
    }
    static uint32_t last_sec;                            /* otherwise only the clock moves */
    uint32_t sec = timer_ticks() / 1000;
    if (sec != last_sec) { last_sec = sec; w->dirty = true; }
}

/* ---------------- input ---------------- */
static int pile_at(sol_t *s, int x, int y, int *idx) {
    deal_t *d = &s->d;
    *idx = 0;
    for (int p = 0; p < P_N; p++) {
        rect_t r = pile_rect(p);
        if (p >= P_T0) {
            int t = p - P_T0;
            int bottom = d->nt[t] ? card_y(d, t, d->nt[t] - 1) + CH : TAB_Y + CH;
            if (x < r.x || x >= r.x + CW || y < TAB_Y || y >= bottom) continue;
            int i = d->nt[t] - 1;
            while (i > 0 && y < card_y(d, t, i)) i--;
            *idx = MAX(0, i);
            return p;
        }
        if (pt_in(r, x, y)) return p;
    }
    return -1;
}
static void pick(sol_t *s, int p, int i) {
    deal_t *d = &s->d;
    if (p == P_WASTE && d->nwaste) { s->holding = true; s->hp = p; s->hi = 0; }
    else if (p >= P_F0 && p <= P_F3 && d->nf[p - P_F0]) { s->holding = true; s->hp = p; s->hi = 0; }
    else if (p >= P_T0) { int t = p - P_T0; if (d->nt[t] && i >= d->up[t]) { s->holding = true; s->hp = p; s->hi = i; } }
}
static void sol_mouse(window_t *w, int type, int x, int y, int btn) {
    sol_t *s = w->data; deal_t *d = &s->d;
    if (s->won) return;
    if (type == ME_DBLCLICK) { int i, p = pile_at(s, x, y, &i); s->holding = false; if (p >= 0) to_foundation(s, p); w->dirty = true; return; }
    if (type == ME_DOWN && (btn & MB_LEFT)) {
        s->by_key = false;
        int i, p = pile_at(s, x, y, &i);
        if (p == P_STOCK) { deal_stock(s); w->dirty = true; return; }
        if (p < 0) return;
        pick(s, p, i);
        if (s->holding) {
            rect_t r = pile_rect(p);
            int cy = p >= P_T0 ? card_y(d, p - P_T0, s->hi) : r.y, cx = p == P_WASTE ? r.x + (s->draw3 ? (MIN(3, d->nwaste) - 1) * 14 : 0) : r.x;
            s->offx = x - cx; s->offy = y - cy; s->hx = x; s->hy = y; s->press_x = x; s->press_y = y; s->dragged = false;
        }
        w->dirty = true;
    } else if (type == ME_MOVE && s->holding && !s->by_key) {
        s->hx = x; s->hy = y;
        if (ABSDIFF(x, s->press_x) + ABSDIFF(y, s->press_y) > 3) s->dragged = true;
        w->dirty = true;
    } else if (type == ME_UP && s->holding && !s->by_key) {
        /* where the middle of the held card is */
        int i, p = pile_at(s, x - s->offx + CW / 2, y - s->offy + 20, &i);
        if (p < 0) p = pile_at(s, x, y, &i);
        if (p >= 0) drop_on(s, p);
        s->holding = false;
        w->dirty = true;
    }
}
static void sol_new(sol_t *s) { deal_new(s); }
static void sol_key(window_t *w, key_event_t *e) {
    sol_t *s = w->data; deal_t *d = &s->d;
    if (!e->pressed) return;
    if (e->key == KEY_F2) { sol_new(s); w->dirty = true; return; }
    if ((e->mods & MOD_CTRL) && (e->key == 'z' || e->key == 'Z' || e->ch == 26)) { if (s->nundo) { d[0] = s->undo[--s->nundo]; s->holding = false; } w->dirty = true; return; }
    if (s->won) return;
    s->by_key = true;
    switch (e->key) {
    case KEY_LEFT: s->cur = (s->cur + P_N - 1) % P_N; break;
    case KEY_RIGHT: s->cur = (s->cur + 1) % P_N; break;
    case KEY_DOWN:
        if (s->cur < P_T0) { s->cur = P_T0 + (s->cur <= P_WASTE ? s->cur : s->cur - P_F0 + 3); s->curi = 99; }
        else s->curi = MIN(s->curi + 1, d->nt[s->cur - P_T0] - 1);
        break;
    case KEY_UP:
        if (s->cur >= P_T0) {
            int t = s->cur - P_T0;
            int i = CLAMP(s->curi, d->up[t], MAX(0, d->nt[t] - 1));
            if (i > d->up[t] && d->nt[t]) s->curi = i - 1;
            else { int col = t; s->cur = col <= 1 ? col : col == 2 ? P_WASTE : P_F0 + col - 3; }
        }
        break;
    case KEY_ESC: s->holding = false; break;
    case 'f': case 'F': to_foundation(s, s->cur); break;
    case ' ': case KEY_ENTER:
        if (s->holding) { if (drop_on(s, s->cur) || s->cur == s->hp) s->holding = false; }
        else if (s->cur == P_STOCK) deal_stock(s);
        else {
            int i = 0;
            if (s->cur >= P_T0) { int t = s->cur - P_T0; i = d->nt[t] ? CLAMP(s->curi, d->up[t], d->nt[t] - 1) : 0; }
            pick(s, s->cur, i);
        }
        break;
    default: return;
    }
    if (s->cur >= P_T0) { int t = s->cur - P_T0; if (s->curi > 90 || s->curi >= d->nt[t]) s->curi = MAX(0, d->nt[t] - 1); }
    w->dirty = true;
}
enum { SM_NEW = 1, SM_UNDO, SM_DRAW1, SM_DRAW3, SM_EXIT };
static menu_t sol_menus[] = {
    { "Game", { { "New Game    F2", SM_NEW, 0 }, { "Undo    Ctrl+Z", SM_UNDO, 0 }, { "-", 0, 0 },
                { "Draw One", SM_DRAW1, MI_RADIO | MI_CHECKED }, { "Draw Three", SM_DRAW3, MI_RADIO }, { "-", 0, 0 }, { "Exit", SM_EXIT, 0 }, { NULL } } },
};
static void sol_menu(window_t *w, int id) {
    sol_t *s = w->data;
    if (id == SM_NEW) sol_new(s);
    else if (id == SM_UNDO) { if (s->nundo) { s->d = s->undo[--s->nundo]; s->holding = false; } }
    else if (id == SM_DRAW1 || id == SM_DRAW3) {
        s->draw3 = id == SM_DRAW3;
        sol_menus[0].items[3].flags = MI_RADIO | (s->draw3 ? 0 : MI_CHECKED);
        sol_menus[0].items[4].flags = MI_RADIO | (s->draw3 ? MI_CHECKED : 0);
        settings_set("solitaire_draw", s->draw3 ? "3" : "1"); settings_save();
        sol_new(s);
    } else if (id == SM_EXIT) { wm_close(w); return; }
    w->dirty = true;
}
static void sol_destroy(window_t *w) { kfree(w->data); w->data = NULL; sol_win = NULL; }
void solitaire_open(void) {
    if (sol_win && wm_alive(sol_win)) { wm_focus(sol_win); return; }
    sol_t *s = kzalloc(sizeof *s);
    if (!s) return;
    const char *v = settings_get("solitaire_best"); s->best = v ? atoi(v) : 0;
    v = settings_get("solitaire_draw"); s->draw3 = v && atoi(v) == 3;
    sol_menus[0].items[3].flags = MI_RADIO | (s->draw3 ? 0 : MI_CHECKED);
    sol_menus[0].items[4].flags = MI_RADIO | (s->draw3 ? MI_CHECKED : 0);
    sol_new(s);
    window_t *w = wm_create("Solitaire", -1, -1, WIN_W, WIN_H, WF_NO_MAX);
    w->data = s; w->paint = sol_paint; w->mouse = sol_mouse; w->key = sol_key; w->destroy = sol_destroy; w->menu_cmd = sol_menu;
    w->menus = sol_menus; w->nmenus = ARRAY_LEN(sol_menus); w->icon = ICON_CARDS;
    wm_set_timer(w, 33, sol_tick);
    sol_win = w;
}
