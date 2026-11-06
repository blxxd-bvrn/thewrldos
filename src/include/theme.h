#ifndef THEME_H
#define THEME_H
#include "gfx.h"

/* Visual styles are .wrldstyle files in C:\Windows\Resources\Themes. The built-in
 * ones always come first, in this order; files added by the user follow. */
enum { THEME_LUNA_BLUE, THEME_OLIVE, THEME_SILVER, THEME_CLASSIC, THEME_AERO, THEME_PLEX, THEME_BUILTIN };
#define THEME_COUNT style_count()
#define STYLE_DIR "C:\\Windows\\Resources\\Themes"
enum { RENDER_LUNA, RENDER_CLASSIC, RENDER_AERO, RENDER_PLEX };
void styles_load(void);                 /* (re)scan STYLE_DIR */
int style_count(void);
const char *style_name(int i);          /* display name, e.g. "Luna (Blue)" */
const char *style_file(int i);          /* file name without extension, e.g. "Luna Blue" */
const char *style_desc(int i);
const char *style_wallpaper(int i);     /* suggested wallpaper name or "" */
int style_find(const char *name);       /* by file name, display name or path; -1 if none */

typedef struct {
    int id;
    bool classic;               /* renderer == RENDER_CLASSIC */
    int renderer;
    uint32_t glass, iglass;     /* aero: glass tint; plex: accent glow */
    int glass_alpha, iglass_alpha, glass_blur;
    uint32_t sm_right_text;
    int title_h, border, cap_btn, menubar_h, taskbar_h;
    uint32_t title_top, title_mid, title_bot, title_hi;
    uint32_t ititle_top, ititle_mid, ititle_bot, ititle_hi;
    uint32_t frame_out, frame_mid, frame_in, iframe_out, iframe_mid, iframe_in;
    uint32_t title_text, title_shadow, ititle_text;
    uint32_t cap_top, cap_bot, cap_border, cap_glyph;
    uint32_t close_top, close_bot;
    uint32_t tb_hi, tb_top, tb_mid, tb_bot, tb_text;
    uint32_t tray_top, tray_bot, tray_border, tray_hi, tray_text;
    uint32_t start_hi, start_top, start_bot, start_text, start_shadow;
    uint32_t tbtn_top, tbtn_bot, tbtn_border, tbtn_act_top, tbtn_act_bot, tbtn_text, tbtn_hot;
    uint32_t sm_head_top, sm_head_bot, sm_head_text, sm_border, sm_left, sm_right, sm_right_line, sm_foot_top, sm_foot_bot, sm_accent, sm_foot_text;
    uint32_t face, face_dark, face_light, window_bg, text, gray_text;
    uint32_t select, select_text, hot, hot_text;
    uint32_t btn_border, btn_top, btn_bot, btn_hot_a, btn_hot_b, btn_focus;
    uint32_t pane_top, pane_bot, pane_head_top, pane_head_bot, pane_head_text, pane_body, pane_link;
    uint32_t link, link_head;
    uint32_t desktop_bg;
    uint32_t menu_bg, menu_border;
} theme_t;

extern theme_t T;
extern int theme_id, theme_hue;
void theme_apply(int id, int hue);
void theme_compute(int id, int hue, theme_t *out);

/* decorated drawing helpers */
void draw_window_frame(canvas_t *c, rect_t r, const char *title, int icon, bool active, int flags, int hot_btn, int pressed_btn, int state);
void draw_button(canvas_t *c, rect_t r, const char *label, bool hot, bool pressed, bool focus, bool enabled);
void draw_sunken_box(canvas_t *c, rect_t r, uint32_t bg);
void draw_check(canvas_t *c, int x, int y, bool checked, bool hot);
void draw_radio(canvas_t *c, int x, int y, bool checked, bool hot);
void draw_scrollbar(canvas_t *c, rect_t r, int total, int visible, int pos, bool vertical);
void draw_progress(canvas_t *c, rect_t r, int pct);
void draw_group(canvas_t *c, rect_t r, const char *label);
void draw_tab(canvas_t *c, rect_t r, const char *label, bool active, bool hot);
void draw_menu_panel(canvas_t *c, rect_t r);
void draw_arrow(canvas_t *c, int x, int y, int dir, uint32_t col); /* 0 up 1 down 2 left 3 right */
rect_t caption_button_rect(rect_t win, int flags, int kind);      /* kind 0 min, 1 max, 3 close; w=0 if absent */
void draw_glass(canvas_t *c, int x, int y, int w, int h, uint32_t tint, int alpha, int blur);
#endif
