/* Visual styles: Luna (Blue / Olive Green / Silver), Classic, plus user hue rotation */
#include "kernel.h"
#include "theme.h"
#include "wm.h"

theme_t T;
int theme_id = THEME_LUNA_BLUE, theme_hue = 0;
const char *theme_names[THEME_COUNT] = { "Luna (Blue)", "Olive Green", "Silver", "Windows Classic" };

static void base_luna(theme_t *t) {
    t->classic = false;
    t->title_h = 30; t->border = 4; t->cap_btn = 21; t->menubar_h = 20; t->taskbar_h = 30;
    t->face = HEX(0xECE9D8); t->face_dark = HEX(0xACA899); t->face_light = HEX(0xFFFFFF);
    t->window_bg = WHITE; t->text = BLACK; t->gray_text = HEX(0xACA899);
    t->select = HEX(0x316AC5); t->select_text = WHITE; t->hot = HEX(0x316AC5); t->hot_text = WHITE;
    t->btn_border = HEX(0x003C74); t->btn_top = HEX(0xFFFFFF); t->btn_bot = HEX(0xE3E1D6);
    t->btn_hot_a = HEX(0xFFF0CF); t->btn_hot_b = HEX(0xF8B330); t->btn_focus = HEX(0x9DB7E8);
    t->pane_top = HEX(0x7BA2E7); t->pane_bot = HEX(0x6375D6);
    t->pane_head_top = HEX(0xFFFFFF); t->pane_head_bot = HEX(0xC6D3F7); t->pane_head_text = HEX(0x215DC6);
    t->pane_body = HEX(0xD6DFF7); t->pane_link = HEX(0x215DC6);
    t->link = HEX(0x0066CC); t->link_head = HEX(0x1E7B1E);
    t->desktop_bg = HEX(0x3A6EA5);
    t->menu_bg = WHITE; t->menu_border = HEX(0xACA899);
    t->start_hi = HEX(0x79C779); t->start_top = HEX(0x3C9C3C); t->start_bot = HEX(0x2D7A2D);
    t->start_text = WHITE; t->start_shadow = HEX(0x1A4A1A);
    t->close_top = HEX(0xE8916E); t->close_bot = HEX(0xC4401C);
    t->sm_left = WHITE; t->sm_accent = HEX(0xF09A3C);
    t->tray_text = WHITE; t->tb_text = WHITE; t->tbtn_text = WHITE; t->sm_foot_text = WHITE; t->sm_head_text = WHITE;
    t->title_text = WHITE; t->title_shadow = HEX(0x0A1E6A); t->cap_glyph = WHITE;
}

static void make_blue(theme_t *t) {
    base_luna(t);
    t->title_hi = HEX(0x8AC0FF); t->title_top = HEX(0x2E82F8); t->title_mid = HEX(0x0A5BE6); t->title_bot = HEX(0x0049D0);
    t->ititle_hi = HEX(0xC4D6F6); t->ititle_top = HEX(0x9DB9EB); t->ititle_mid = HEX(0x7A9AE0); t->ititle_bot = HEX(0x7090DA);
    t->frame_out = HEX(0x0831D9); t->frame_mid = HEX(0x0C55E6); t->frame_in = HEX(0x2E74F0);
    t->iframe_out = HEX(0x6582D0); t->iframe_mid = HEX(0x7B97E0); t->iframe_in = HEX(0x91ABEA);
    t->ititle_text = HEX(0xD8E4F8);
    t->cap_top = HEX(0x5C9BFF); t->cap_bot = HEX(0x1D5AD8); t->cap_border = WHITE;
    t->tb_hi = HEX(0x4C93F7); t->tb_top = HEX(0x2A66E0); t->tb_mid = HEX(0x245DDA); t->tb_bot = HEX(0x1C47B8);
    t->tray_top = HEX(0x139BEF); t->tray_bot = HEX(0x0D7EDE); t->tray_border = HEX(0x0F4FAE); t->tray_hi = HEX(0x24B6FA);
    t->tbtn_top = HEX(0x4C8DF5); t->tbtn_bot = HEX(0x2A66DF); t->tbtn_border = HEX(0x1747A8);
    t->tbtn_act_top = HEX(0x1A4CB8); t->tbtn_act_bot = HEX(0x1E55C8); t->tbtn_hot = HEX(0x5CA0FF);
    t->sm_head_top = HEX(0x1A6BDA); t->sm_head_bot = HEX(0x0F4EC2); t->sm_border = HEX(0x1550C0);
    t->sm_right = HEX(0xD3E5FA); t->sm_right_line = HEX(0x95BDEE);
    t->sm_foot_top = HEX(0x3C80F0); t->sm_foot_bot = HEX(0x1C47B8);
}
static void make_olive(theme_t *t) {
    base_luna(t);
    t->select = HEX(0x93A070); t->hot = HEX(0x93A070);
    t->btn_border = HEX(0x37622B); t->btn_focus = HEX(0xB5C88A);
    t->title_hi = HEX(0xDCE6C0); t->title_top = HEX(0xB6C78C); t->title_mid = HEX(0x97AB68); t->title_bot = HEX(0x7E9450);
    t->ititle_hi = HEX(0xF0F2E4); t->ititle_top = HEX(0xDDE3C8); t->ititle_mid = HEX(0xCBD4AE); t->ititle_bot = HEX(0xBEC8A0);
    t->frame_out = HEX(0x758B48); t->frame_mid = HEX(0x8BA169); t->frame_in = HEX(0xA4B77F);
    t->iframe_out = HEX(0xB6C19A); t->iframe_mid = HEX(0xC6CFAE); t->iframe_in = HEX(0xD4DBC0);
    t->title_shadow = HEX(0x4A5A2A); t->ititle_text = HEX(0xF4F6EC);
    t->cap_top = HEX(0xC8D6A2); t->cap_bot = HEX(0x8BA169); t->cap_border = WHITE;
    t->tb_hi = HEX(0xC8D6A6); t->tb_top = HEX(0xA9BC80); t->tb_mid = HEX(0x99AE6E); t->tb_bot = HEX(0x7C924E);
    t->tray_top = HEX(0xDCE6C4); t->tray_bot = HEX(0xC4D2A0); t->tray_border = HEX(0x8EA266); t->tray_hi = HEX(0xF2F6E6); t->tray_text = BLACK;
    t->tbtn_top = HEX(0xBACB91); t->tbtn_bot = HEX(0x9CB070); t->tbtn_border = HEX(0x6F8549);
    t->tbtn_act_top = HEX(0x7D9350); t->tbtn_act_bot = HEX(0x8BA160); t->tbtn_hot = HEX(0xCEDCA8);
    t->sm_head_top = HEX(0xA6BA7A); t->sm_head_bot = HEX(0x80975A); t->sm_border = HEX(0x758B48);
    t->sm_right = HEX(0xE9EDD8); t->sm_right_line = HEX(0xBBC79A);
    t->sm_foot_top = HEX(0xA9BC80); t->sm_foot_bot = HEX(0x7C924E);
    t->pane_top = HEX(0xCCD6A8); t->pane_bot = HEX(0xA9B97F); t->pane_head_text = HEX(0x56662D);
    t->pane_head_bot = HEX(0xE0E7C8); t->pane_body = HEX(0xF2F5E6); t->pane_link = HEX(0x56662D);
    t->desktop_bg = HEX(0x6E7F48);
}
static void make_silver(theme_t *t) {
    base_luna(t);
    t->face = HEX(0xE0DFE3); t->face_dark = HEX(0x9D9DA1);
    t->select = HEX(0x7C88AE); t->hot = HEX(0x7C88AE);
    t->btn_border = HEX(0x5E5D74); t->btn_focus = HEX(0xBCBCD0);
    t->title_hi = HEX(0xFFFFFF); t->title_top = HEX(0xF2F2F6); t->title_mid = HEX(0xD6D6E2); t->title_bot = HEX(0xB6B6C8);
    t->ititle_hi = HEX(0xFFFFFF); t->ititle_top = HEX(0xF6F6F8); t->ititle_mid = HEX(0xE6E6EE); t->ititle_bot = HEX(0xDADAE4);
    t->frame_out = HEX(0x8E8DA6); t->frame_mid = HEX(0xC0C0D2); t->frame_in = HEX(0xDADAE6);
    t->iframe_out = HEX(0xB8B8C8); t->iframe_mid = HEX(0xD8D8E2); t->iframe_in = HEX(0xE8E8EE);
    t->title_text = HEX(0x101030); t->title_shadow = 0; t->ititle_text = HEX(0x8A8AA0);
    t->cap_top = HEX(0xFFFFFF); t->cap_bot = HEX(0xC6C6D6); t->cap_border = HEX(0x8E8DA6); t->cap_glyph = HEX(0x30304A);
    t->tb_hi = HEX(0xFFFFFF); t->tb_top = HEX(0xEAEAF2); t->tb_mid = HEX(0xD8D8E4); t->tb_bot = HEX(0xB4B4C6); t->tb_text = BLACK;
    t->tray_top = HEX(0xF6F6FA); t->tray_bot = HEX(0xDCDCE6); t->tray_border = HEX(0x9C9CB4); t->tray_hi = WHITE; t->tray_text = BLACK;
    t->tbtn_top = HEX(0xFBFBFD); t->tbtn_bot = HEX(0xDCDCE6); t->tbtn_border = HEX(0x9C9CB4); t->tbtn_text = BLACK;
    t->tbtn_act_top = HEX(0xC2C2D2); t->tbtn_act_bot = HEX(0xD6D6E2); t->tbtn_hot = WHITE;
    t->sm_head_top = HEX(0xC8C8D8); t->sm_head_bot = HEX(0x9C9CB6); t->sm_border = HEX(0x8E8DA6);
    t->sm_right = HEX(0xEDEDF3); t->sm_right_line = HEX(0xC4C4D4);
    t->sm_foot_top = HEX(0xE4E4EC); t->sm_foot_bot = HEX(0xB8B8C8); t->sm_foot_text = BLACK;
    t->pane_top = HEX(0xC8C8D8); t->pane_bot = HEX(0xA8A8C0); t->pane_head_text = HEX(0x3A3A5A);
    t->pane_head_bot = HEX(0xE2E2EC); t->pane_body = HEX(0xF4F4F8); t->pane_link = HEX(0x3A3A5A);
    t->desktop_bg = HEX(0x7A7A90);
}
static void make_classic(theme_t *t) {
    memset(t, 0, sizeof *t);
    t->classic = true;
    t->title_h = 22; t->border = 4; t->cap_btn = 16; t->menubar_h = 19; t->taskbar_h = 28;
    t->face = HEX(0xD4D0C8); t->face_dark = HEX(0x808080); t->face_light = WHITE;
    t->window_bg = WHITE; t->text = BLACK; t->gray_text = HEX(0x808080);
    t->title_top = HEX(0x0A246A); t->title_bot = HEX(0xA6CAF0);
    t->ititle_top = HEX(0x808080); t->ititle_bot = HEX(0xC0C0C0);
    t->title_text = WHITE; t->ititle_text = HEX(0xD4D0C8);
    t->select = HEX(0x0A246A); t->select_text = WHITE; t->hot = HEX(0x0A246A); t->hot_text = WHITE;
    t->tb_text = BLACK; t->tray_text = BLACK; t->tbtn_text = BLACK; t->start_text = BLACK;
    t->sm_head_top = HEX(0x000080); t->sm_head_bot = HEX(0x1084D0); t->sm_head_text = WHITE;
    t->sm_left = HEX(0xD4D0C8); t->sm_right = HEX(0xD4D0C8);
    t->pane_top = HEX(0xD4D0C8); t->pane_bot = HEX(0xD4D0C8); t->pane_head_top = HEX(0x0A246A); t->pane_head_bot = HEX(0x0A246A);
    t->pane_head_text = WHITE; t->pane_body = HEX(0xFFFFFF); t->pane_link = HEX(0x0000CC);
    t->link = HEX(0x0000CC); t->link_head = HEX(0x006000);
    t->desktop_bg = HEX(0x3A6EA5);
    t->menu_bg = HEX(0xD4D0C8); t->menu_border = HEX(0x808080);
    t->btn_focus = BLACK;
}

#define SHIFT(f) t->f = hue_shift(t->f, hue)
void theme_compute(int id, int hue, theme_t *t) {
    id = CLAMP(id, 0, THEME_COUNT - 1);
    hue = ((hue % 360) + 360) % 360;
    switch (id) {
    case THEME_OLIVE: make_olive(t); break;
    case THEME_SILVER: make_silver(t); break;
    case THEME_CLASSIC: make_classic(t); break;
    default: make_blue(t); break;
    }
    t->id = id;
    if (hue) {
        SHIFT(title_hi); SHIFT(title_top); SHIFT(title_mid); SHIFT(title_bot);
        SHIFT(ititle_hi); SHIFT(ititle_top); SHIFT(ititle_mid); SHIFT(ititle_bot);
        SHIFT(frame_out); SHIFT(frame_mid); SHIFT(frame_in); SHIFT(iframe_out); SHIFT(iframe_mid); SHIFT(iframe_in);
        SHIFT(title_shadow); SHIFT(ititle_text); SHIFT(cap_top); SHIFT(cap_bot);
        SHIFT(tb_hi); SHIFT(tb_top); SHIFT(tb_mid); SHIFT(tb_bot);
        SHIFT(tray_top); SHIFT(tray_bot); SHIFT(tray_border); SHIFT(tray_hi);
        SHIFT(tbtn_top); SHIFT(tbtn_bot); SHIFT(tbtn_border); SHIFT(tbtn_act_top); SHIFT(tbtn_act_bot); SHIFT(tbtn_hot);
        SHIFT(sm_head_top); SHIFT(sm_head_bot); SHIFT(sm_border); SHIFT(sm_right); SHIFT(sm_right_line); SHIFT(sm_foot_top); SHIFT(sm_foot_bot);
        SHIFT(select); SHIFT(hot); SHIFT(btn_border); SHIFT(btn_focus);
        SHIFT(pane_top); SHIFT(pane_bot); SHIFT(pane_head_bot); SHIFT(pane_head_text); SHIFT(pane_body); SHIFT(pane_link);
        SHIFT(desktop_bg);
    }
}
void theme_apply(int id, int hue) {
    theme_id = CLAMP(id, 0, THEME_COUNT - 1);
    theme_hue = ((hue % 360) + 360) % 360;
    theme_compute(theme_id, theme_hue, &T);
    wm_theme_changed();
}

/* ---------------- drawing helpers ---------------- */
void draw_arrow(canvas_t *c, int x, int y, int dir, uint32_t col) {
    for (int i = 0; i < 4; i++) {
        switch (dir) {
        case 0: hline(c, x - i, y + i, 2 * i + 1, col); break;
        case 1: hline(c, x - (3 - i), y + i, 2 * (3 - i) + 1, col); break;
        case 2: vline(c, x + i, y - i, 2 * i + 1, col); break;
        case 3: vline(c, x + i, y - (3 - i), 2 * (3 - i) + 1, col); break;
        }
    }
}

static void cap_button(canvas_t *c, int x, int y, int s, int kind, bool active, bool hot, bool pressed) {
    if (T.classic) {
        fill_rect(c, x, y, s, s - 2, T.face);
        bevel_deep(c, x, y, s, s - 2, !pressed);
        int ox = x + (pressed ? 1 : 0), oy = y + (pressed ? 1 : 0);
        switch (kind) {
        case 0: fill_rect(c, ox + 4, oy + 9, 6, 2, BLACK); break;
        case 1: draw_rect(c, ox + 3, oy + 2, 9, 9, BLACK); hline(c, ox + 3, oy + 3, 9, BLACK); break;
        case 2: draw_rect(c, ox + 5, oy + 2, 6, 5, BLACK); hline(c, ox + 5, oy + 3, 6, BLACK); fill_rect(c, ox + 3, oy + 6, 6, 5, T.face); draw_rect(c, ox + 3, oy + 6, 6, 5, BLACK); hline(c, ox + 3, oy + 7, 6, BLACK); break;
        case 3: for (int i = 0; i < 7; i++) { fill_rect(c, ox + 4 + i, oy + 3 + i, 2, 1, BLACK); fill_rect(c, ox + 10 - i, oy + 3 + i, 2, 1, BLACK); } break;
        }
        return;
    }
    uint32_t top = kind == 3 ? T.close_top : T.cap_top, bot = kind == 3 ? T.close_bot : T.cap_bot;
    if (!active) { top = mix(top, WHITE, 90); bot = mix(bot, WHITE, 90); }
    if (hot) { top = lighten(top, 50); bot = lighten(bot, 40); }
    if (pressed) { uint32_t t2 = top; top = darken(bot, 30); bot = t2; }
    fill_round_rect(c, x, y, s, s, 3, T.cap_border);
    gradient_round_rect(c, x + 1, y + 1, s - 2, s - 2, 2, lighten(top, 40), bot);
    blend_rect(c, x + 2, y + s - 6, s - 4, 3, WHITE, 25);
    uint32_t g = kind == 3 ? WHITE : T.cap_glyph;
    switch (kind) {
    case 0: fill_rect(c, x + 5, y + s - 8, 7, 3, g); break;
    case 1: draw_rect(c, x + 5, y + 5, s - 10, s - 10, g); fill_rect(c, x + 5, y + 5, s - 10, 3, g); break;
    case 2: draw_rect(c, x + 8, y + 4, 8, 7, g); hline(c, x + 8, y + 5, 8, g);
            fill_rect(c, x + 4, y + 9, 8, 7, top); draw_rect(c, x + 4, y + 9, 8, 7, g); fill_rect(c, x + 4, y + 9, 8, 2, g); break;
    case 3: for (int i = 0; i < 9; i++) { fill_rect(c, x + 5 + i, y + 5 + i, 2, 2, g); fill_rect(c, x + 14 - i, y + 5 + i, 2, 2, g); } break;
    }
}

void draw_window_frame(canvas_t *c, rect_t r, const char *title, int icon, bool active, int flags, int hot_btn, int pressed_btn, int state) {
    int x = r.x, y = r.y, w = r.w, h = r.h;
    int th = T.title_h, b = T.border;
    bool maxed = state == WS_MAXIMIZED;
    if (T.classic) {
        fill_rect(c, x, y, w, h, T.face);
        bevel_deep(c, x, y, w, h, true);
        int tx = x + 4, ty = y + 4, tw = w - 8, tht = th - 4;
        gradient_h(c, tx, ty, tw, tht - 1, active ? T.title_top : T.ititle_top, active ? T.title_bot : T.ititle_bot);
        int textx = tx + 3;
        if (icon >= 0) { draw_icon(c, icon, 16, tx + 2, ty + (tht - 17) / 2); textx += 19; }
        rect_t old = canvas_push_clip(c, R(tx, ty, tw - 60, tht));
        draw_text(c, FONT_UI_BOLD, textx, ty + (tht - 1 - font_height(FONT_UI_BOLD)) / 2, title, active ? T.title_text : T.ititle_text);
        c->clip = old;
    } else {
        uint32_t fo = active ? T.frame_out : T.iframe_out, fm = active ? T.frame_mid : T.iframe_mid, fi = active ? T.frame_in : T.iframe_in;
        /* body frame */
        fill_rect(c, x, y + th, b, h - th, fm); fill_rect(c, x + w - b, y + th, b, h - th, fm);
        fill_rect(c, x, y + h - b, w, b, fm);
        vline(c, x, y + 8, h - 8, fo); vline(c, x + w - 1, y + 8, h - 8, fo); hline(c, x, y + h - 1, w, fo);
        vline(c, x + b - 1, y + th, h - th - b + 1, fi); vline(c, x + w - b, y + th, h - th - b + 1, fi); hline(c, x + b - 1, y + h - b, w - 2 * b + 2, fi);
        /* title bar with rounded top corners */
        int rad = maxed ? 0 : 7;
        for (int j = 0; j < th; j++) {
            uint32_t col;
            if (j == 0) col = fo;
            else if (j == 1) col = active ? T.title_hi : T.ititle_hi;
            else if (j < 8) col = mix(active ? T.title_top : T.ititle_top, active ? T.title_mid : T.ititle_mid, (j - 2) * 255 / 6);
            else col = mix(active ? T.title_mid : T.ititle_mid, active ? T.title_bot : T.ititle_bot, (j - 8) * 255 / (th - 8));
            int in = 0;
            if (j < rad) { int dy = rad - j, dx = 0; while ((dx + 1) * (dx + 1) + dy * dy <= rad * rad) dx++; in = rad - dx; }
            hline(c, x + in, y + j, w - 2 * in, col);
            if (j > 0 && j < rad + 1) { put_pixel(c, x + in, y + j, fo); put_pixel(c, x + w - 1 - in, y + j, fo); }
            else if (j > 0) { put_pixel(c, x, y + j, fo); put_pixel(c, x + w - 1, y + j, fo); }
        }
        int textx = x + 8;
        if (icon >= 0) { draw_icon(c, icon, 16, x + 6, y + (th - 16) / 2 + 1); textx = x + 27; }
        rect_t old = canvas_push_clip(c, R(x, y, w - 75, th));
        int ty = y + (th - font_height(FONT_TITLE)) / 2 + 1;
        if (T.title_shadow && active) draw_text(c, FONT_TITLE, textx + 1, ty + 1, title, T.title_shadow);
        draw_text(c, FONT_TITLE, textx, ty, title, active ? T.title_text : T.ititle_text);
        c->clip = old;
    }
    /* caption buttons: close, max, min (right to left) */
    int s = T.cap_btn;
    int bx = x + w - (T.classic ? 6 : 5) - s, by = y + (T.classic ? 6 : (th - s) / 2 + 1);
    if (!(flags & WF_NOCLOSE)) cap_button(c, bx, by, s, 3, active, hot_btn == 3, pressed_btn == 3);
    if (!(flags & WF_NO_MAX) || !(flags & WF_NO_MIN)) {
        bx -= s + (T.classic ? 2 : 2);
        if (!(flags & WF_NO_MAX)) cap_button(c, bx, by, s, maxed ? 2 : 1, active, hot_btn == 1, pressed_btn == 1);
        else if (T.classic) { cap_button(c, bx, by, s, 1, active, 0, 0); }
        bx -= s + (T.classic ? 0 : 2);
        if (!(flags & WF_NO_MIN)) cap_button(c, bx, by, s, 0, active, hot_btn == 0, pressed_btn == 0);
    }
}

void draw_button(canvas_t *c, rect_t r, const char *label, bool hot, bool pressed, bool focus, bool enabled) {
    if (T.classic) {
        fill_rect(c, r.x, r.y, r.w, r.h, T.face);
        if (focus) { draw_rect(c, r.x, r.y, r.w, r.h, BLACK); bevel_deep(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, !pressed); }
        else bevel_deep(c, r.x, r.y, r.w, r.h, !pressed);
        int o = pressed ? 1 : 0;
        if (!enabled) draw_text_centered(c, FONT_UI, R(r.x + 1, r.y + 1, r.w, r.h), label, WHITE);
        draw_text_centered(c, FONT_UI, R(r.x + o, r.y + o, r.w, r.h), label, enabled ? BLACK : HEX(0x808080));
        if (focus) dotted_rect(c, r.x + 4, r.y + 4, r.w - 8, r.h - 8, BLACK);
        return;
    }
    fill_round_rect(c, r.x, r.y, r.w, r.h, 3, enabled ? T.btn_border : HEX(0xC9C7BA));
    uint32_t top = T.btn_top, bot = T.btn_bot;
    if (pressed) { top = HEX(0xE3E2DA); bot = HEX(0xF0EFEA); }
    gradient_round_rect(c, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 2, top, bot);
    if (!pressed && enabled) hline(c, r.x + 2, r.y + r.h - 3, r.w - 4, HEX(0xD6D0C5));
    if (enabled && (hot || focus)) {
        uint32_t a = hot ? T.btn_hot_a : lighten(T.btn_focus, 80), b2 = hot ? T.btn_hot_b : T.btn_focus;
        hline(c, r.x + 2, r.y + 1, r.w - 4, a); hline(c, r.x + 2, r.y + r.h - 2, r.w - 4, b2);
        hline(c, r.x + 1, r.y + 2, r.w - 2, a);
        hline(c, r.x + 1, r.y + r.h - 3, r.w - 2, b2);
        vline(c, r.x + 1, r.y + 2, r.h - 4, mix(a, b2, 128)); vline(c, r.x + r.w - 2, r.y + 2, r.h - 4, mix(a, b2, 128));
        vline(c, r.x + 2, r.y + 2, r.h - 4, mix(a, b2, 128)); vline(c, r.x + r.w - 3, r.y + 2, r.h - 4, mix(a, b2, 128));
    }
    draw_text_centered(c, FONT_UI, R(r.x + (pressed ? 1 : 0), r.y + (pressed ? 1 : 0), r.w, r.h), label, enabled ? BLACK : HEX(0xACA899));
}
void draw_sunken_box(canvas_t *c, rect_t r, uint32_t bg) {
    fill_rect(c, r.x, r.y, r.w, r.h, bg);
    if (T.classic) bevel_deep(c, r.x, r.y, r.w, r.h, false);
    else draw_rect(c, r.x, r.y, r.w, r.h, HEX(0x7F9DB9));
}
void draw_check(canvas_t *c, int x, int y, bool checked, bool hot) {
    if (T.classic) { fill_rect(c, x, y, 13, 13, WHITE); bevel_deep(c, x, y, 13, 13, false); }
    else {
        draw_rect(c, x, y, 13, 13, HEX(0x1C5180));
        gradient_v(c, x + 1, y + 1, 11, 11, HEX(0xDCDCD7), WHITE);
        if (hot) { draw_rect(c, x + 1, y + 1, 11, 11, HEX(0xF8B330)); draw_rect(c, x + 2, y + 2, 9, 9, HEX(0xFFE0A0)); }
    }
    if (checked) {
        uint32_t k = T.classic ? BLACK : HEX(0x21A121);
        for (int i = 0; i < 3; i++) { vline(c, x + 3 + i, y + 5 + i, 3, k); }
        for (int i = 0; i < 4; i++) { vline(c, x + 6 + i, y + 6 - i, 3, k); }
    }
}
void draw_radio(canvas_t *c, int x, int y, bool checked, bool hot) {
    if (T.classic) {
        fill_circle(c, x + 6, y + 6, 6, HEX(0x808080)); fill_circle(c, x + 6, y + 6, 5, WHITE);
    } else {
        fill_circle(c, x + 6, y + 6, 6, HEX(0x1C5180)); fill_circle(c, x + 6, y + 6, 5, HEX(0xE4E3DB)); fill_circle(c, x + 6, y + 6, 4, WHITE);
        if (hot) draw_circle(c, x + 6, y + 6, 4, HEX(0xF8B330));
    }
    if (checked) fill_circle(c, x + 6, y + 6, 2, T.classic ? BLACK : HEX(0x21A121));
}
void draw_scrollbar(canvas_t *c, rect_t r, int total, int visible, int pos, bool vertical) {
    int len = vertical ? r.h : r.w, thick = vertical ? r.w : r.h;
    uint32_t track = T.classic ? HEX(0xE6E3DD) : HEX(0xF4F3EE);
    fill_rect(c, r.x, r.y, r.w, r.h, track);
    int avail = len - 2 * thick;
    int tl = total > visible && total > 0 ? MAX(12, avail * visible / total) : avail;
    int tp = total > visible ? (avail - tl) * pos / (total - visible) : 0;
    tp = CLAMP(tp, 0, avail - tl);
    rect_t a1 = vertical ? R(r.x, r.y, thick, thick) : R(r.x, r.y, thick, thick);
    rect_t a2 = vertical ? R(r.x, r.y + r.h - thick, thick, thick) : R(r.x + r.w - thick, r.y, thick, thick);
    rect_t th = vertical ? R(r.x, r.y + thick + tp, thick, tl) : R(r.x + thick + tp, r.y, tl, thick);
    rect_t parts[3] = { a1, a2, th };
    for (int i = 0; i < 3; i++) {
        rect_t p = parts[i];
        if (T.classic) { fill_rect(c, p.x, p.y, p.w, p.h, T.face); bevel_deep(c, p.x, p.y, p.w, p.h, true); }
        else { fill_round_rect(c, p.x, p.y, p.w, p.h, 2, HEX(0xA9BCE8)); gradient_round_rect(c, p.x + 1, p.y + 1, p.w - 2, p.h - 2, 2, HEX(0xD8E4FC), HEX(0xB9CDF5)); }
    }
    uint32_t ac = T.classic ? BLACK : HEX(0x4D6185);
    if (vertical) { draw_arrow(c, a1.x + thick / 2, a1.y + thick / 2 - 2, 0, ac); draw_arrow(c, a2.x + thick / 2, a2.y + thick / 2 - 1, 1, ac); }
    else { draw_arrow(c, a1.x + thick / 2 - 2, a1.y + thick / 2, 2, ac); draw_arrow(c, a2.x + thick / 2 - 1, a2.y + thick / 2, 3, ac); }
    if (!T.classic && (vertical ? tl : tl) > 14) {
        for (int i = -2; i <= 2; i += 2) {
            if (vertical) hline(c, th.x + 4, th.y + tl / 2 + i, thick - 8, HEX(0x8AA4D6));
            else vline(c, th.x + tl / 2 + i, th.y + 4, thick - 8, HEX(0x8AA4D6));
        }
    }
}
void draw_progress(canvas_t *c, rect_t r, int pct) {
    draw_sunken_box(c, r, WHITE);
    int fw = (r.w - 4) * CLAMP(pct, 0, 100) / 100;
    if (T.classic) { for (int x = 0; x + 8 <= fw; x += 10) fill_rect(c, r.x + 2 + x, r.y + 2, 8, r.h - 4, T.select); return; }
    for (int x = 0; x + 6 <= fw; x += 8) gradient_v3(c, r.x + 2 + x, r.y + 2, 6, r.h - 4, HEX(0xB2EDB2), HEX(0x2FBD2F), HEX(0x8BE38B), (r.h - 4) / 2);
}
void draw_group(canvas_t *c, rect_t r, const char *label) {
    int fh = font_height(FONT_UI);
    if (T.classic) { draw_rect(c, r.x, r.y + fh / 2, r.w, r.h - fh / 2, HEX(0x808080)); draw_rect(c, r.x + 1, r.y + fh / 2 + 1, r.w, r.h - fh / 2, WHITE); }
    else draw_round_rect(c, r.x, r.y + fh / 2, r.w, r.h - fh / 2, 3, HEX(0xD0D0BF));
    int tw = text_width(FONT_UI, label);
    fill_rect(c, r.x + 7, r.y, tw + 6, fh, T.face);
    draw_text(c, FONT_UI, r.x + 10, r.y, label, T.classic ? BLACK : HEX(0x0046D5));
}
void draw_tab(canvas_t *c, rect_t r, const char *label, bool active, bool hot) {
    if (!active) { r.y += 2; r.h -= 2; }
    if (T.classic) {
        fill_rect(c, r.x, r.y, r.w, r.h, T.face);
        hline(c, r.x + 2, r.y, r.w - 4, WHITE); vline(c, r.x, r.y + 2, r.h - 2, WHITE); put_pixel(c, r.x + 1, r.y + 1, WHITE);
        vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, HEX(0x404040)); vline(c, r.x + r.w - 2, r.y + 1, r.h - 1, HEX(0x808080));
    } else {
        fill_rect(c, r.x, r.y, r.w, r.h, HEX(0x919B9C));
        gradient_v(c, r.x + 1, r.y + 1, r.w - 2, r.h - 1, WHITE, active ? HEX(0xFCFCFE) : HEX(0xECEBE6));
        if (active || hot) { hline(c, r.x + 1, r.y, r.w - 2, HEX(0xE68B2C)); hline(c, r.x, r.y + 1, r.w, HEX(0xFFC73C)); hline(c, r.x, r.y + 2, r.w, HEX(0xFFC73C)); }
    }
    draw_text_centered(c, FONT_UI, r, label, BLACK);
}
void draw_menu_panel(canvas_t *c, rect_t r) {
    if (T.classic) { fill_rect(c, r.x, r.y, r.w, r.h, T.face); bevel_deep(c, r.x, r.y, r.w, r.h, true); return; }
    blend_rect(c, r.x + 3, r.y + 3, r.w, r.h, BLACK, 50);
    fill_rect(c, r.x, r.y, r.w, r.h, WHITE);
    draw_rect(c, r.x, r.y, r.w, r.h, HEX(0xACA899));
}
