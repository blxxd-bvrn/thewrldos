#ifndef WM_H
#define WM_H
#include "kernel.h"
#include "gfx.h"

#define WF_RESIZABLE 0x001
#define WF_NO_MAX    0x002
#define WF_NO_MIN    0x004
#define WF_DIALOG    0x008   /* no taskbar entry */
#define WF_POPUP     0x010   /* no frame */
#define WF_TOPMOST   0x020
#define WF_HIDDEN    0x040
#define WF_MODAL     0x080
#define WF_NOCLOSE   0x100
#define WF_SKINNED   0x200   /* the app draws its own frame; pixels with alpha 0 are see-through */
#define WF_NOMEM     0x4000  /* (set by the window manager) its surface could not be allocated: it closes */

enum { WS_NORMAL, WS_MINIMIZED, WS_MAXIMIZED };
enum { ME_MOVE, ME_DOWN, ME_UP, ME_DBLCLICK, ME_WHEEL, ME_LEAVE };
#define MB_LEFT 1
#define MB_RIGHT 2
#define MB_MIDDLE 4

typedef struct { const char *label; int id; int flags; } menu_item_t;  /* label "-" = separator */
#define MI_CHECKED 1
#define MI_DISABLED 2
#define MI_RADIO 4
typedef struct { const char *title; menu_item_t items[20]; } menu_t;

typedef struct {
    int mx, my;          /* client coords */
    bool down, pressed, released, rpressed;
    int hot, active, focus, last_focus;
    int next_id;
    int wheel;
    key_event_t key;
    bool has_key;
    int cursor;
    bool in_window;
    bool dblclick;
} ui_t;

typedef struct window window_t;
struct window {
    int id;
    char title[96];
    int icon;
    int x, y, w, h;
    int min_w, min_h;
    int flags, state;
    rect_t restore;
    canvas_t *surf;
    int pid;
    bool dirty;
    menu_t *menus;
    int nmenus;
    void (*paint)(window_t *);
    void (*mouse)(window_t *, int type, int x, int y, int btn);
    void (*key)(window_t *, key_event_t *);
    bool (*close)(window_t *);
    void (*menu_cmd)(window_t *, int id);
    void (*resized)(window_t *);
    void (*timer)(window_t *);
    void (*destroy)(window_t *);
    void (*activate)(window_t *, bool active);
    uint32_t timer_ms, timer_next;
    void *data;
    ui_t ui;
    window_t *owner;
    int cursor;
    void *w32;
    int minimized_state;             /* restore maximized windows without losing their state */
    bool opening;                    /* start the effect after the first client paint */
};

/* window manager */
void wm_init(void);
void wm_run(void);                  /* main GUI loop (never returns) */
void shellmanager_start(void);      /* start the desktop shell as its own task */
void shellmanager_restart(void);    /* soft restart request, handled by wm_run */
void wm_recover_shell(void);        /* close windows and reset the shell state */
void shell_reset(void);             /* reset Start menu/sidebar transient state */
window_t *wm_create(const char *title, int x, int y, int cw, int ch, int flags);  /* client size */
void wm_destroy(window_t *w);
void wm_close(window_t *w);         /* asks close callback */
void wm_show(window_t *w);
void wm_focus(window_t *w);
void wm_minimize(window_t *w);
void wm_maximize(window_t *w, bool on);
/* for skinned windows, from a paint while the left button is held: let the
 * window manager move or resize (edges: 1 left, 2 right, 4 top, 8 bottom) */
void wm_begin_move(window_t *w);
void wm_begin_resize(window_t *w, int edges);
void wm_set_skinned(window_t *w, bool on);   /* switch between the app's frame and the system frame */
void wm_set_title(window_t *w, const char *t);
void wm_invalidate(window_t *w);
void wm_redraw_all(void);
void wm_resize_client(window_t *w, int cw, int ch);
void wm_center(window_t *w);
rect_t wm_client_rect(window_t *w);  /* screen coords */
window_t *wm_focused(void);
window_t *wm_find(int id);
bool wm_alive(window_t *w);
int wm_window_count(void);
window_t *wm_window_at(int idx);    /* z order, 0 = bottom */
bool wm_window_at_point(int x, int y);   /* a visible window covers this screen point */
void wm_theme_changed(void);
void wm_effects_changed(void);        /* settle effects after settings/theme/resolution changes */
void wm_set_timer(window_t *w, uint32_t ms, void (*fn)(window_t *));
rect_t wm_work_area(void);
void wm_destroy_for_pid(int pid);
extern window_t *wm_active;
extern int wm_mouse_x, wm_mouse_y;
extern uint32_t wm_dblclick_ms;
extern bool wm_need_redraw;
extern bool gfx_ready_for_watchdog;
typedef void (*wm_deferred_fn)(void *);
void wm_defer(wm_deferred_fn fn, void *arg);   /* run on GUI thread */

/* popup menus */
typedef void (*popup_cb)(void *ctx, int id);
void popup_menu(int x, int y, const menu_item_t *items, int n, popup_cb cb, void *ctx);
void popup_close(void);
bool popup_open(void);

/* message boxes */
#define MBX_OK 0
#define MBX_OKCANCEL 1
#define MBX_YESNO 4
#define MBX_YESNOCANCEL 3
enum { MBI_NONE = -1, MBI_INFO, MBI_WARNING, MBI_ERROR, MBI_QUESTION };
enum { IDOK = 1, IDCANCEL = 2, IDYES = 6, IDNO = 7 };
typedef void (*msgbox_cb)(void *ctx, int result);
window_t *msgbox(const char *title, const char *text, int icon, int buttons, msgbox_cb cb, void *ctx);
void input_box(const char *title, const char *prompt, const char *initial, void (*cb)(void *ctx, const char *text), void *ctx);
void notify_balloon(const char *title, const char *text);

/* immediate-mode widgets (call inside paint) */
void ui_begin(window_t *w);
void ui_end(window_t *w);
bool ui_button(window_t *w, rect_t r, const char *label);
bool ui_button_ex(window_t *w, rect_t r, const char *label, bool enabled, bool is_default);
bool ui_link(window_t *w, int x, int y, int font, const char *label, uint32_t col);
bool ui_link_wrapped(window_t *w, int x, int y, int maxw, int font, const char *label, uint32_t col);
bool ui_checkbox(window_t *w, int x, int y, const char *label, bool *v);
bool ui_radio(window_t *w, int x, int y, const char *label, bool checked);
bool ui_slider(window_t *w, rect_t r, int *v, int lo, int hi);
bool ui_textbox(window_t *w, rect_t r, char *buf, int cap);
bool ui_textbox_ex(window_t *w, rect_t r, char *buf, int cap, bool flat, uint32_t fg, bool *focused_out);
bool ui_password(window_t *w, rect_t r, char *buf, int cap);
bool ui_hover(window_t *w, rect_t r);
bool ui_click_area(window_t *w, rect_t r);
int ui_list(window_t *w, rect_t r, const char **items, int n, int *sel, int *scroll);
bool ui_vscroll(window_t *w, rect_t r, int *pos, int total, int visible);   /* arrows, track paging, thumb drag */
bool ui_tab(window_t *w, rect_t r, const char *label, bool active);
bool ui_icon_button(window_t *w, rect_t r, int icon, const char *label);
bool ui_dropdown(window_t *w, rect_t r, const char **items, int n, int *sel);
int ui_id(window_t *w);

/* desktop shell (taskbar, start menu, desktop) */
void shell_init(void);
void desktop_paint(canvas_t *c);
void taskbar_paint(canvas_t *c);
bool shell_mouse(int type, int x, int y, int btn);  /* taskbar / start menu; true if consumed */
void desktop_mouse(int type, int x, int y, int btn); /* clicks that reached the desktop */
int shell_cursor(int x, int y);
bool shell_key(key_event_t *e);
void shell_paint_overlays(canvas_t *c);
void shell_tick(void);
void shell_animate(uint32_t now);      /* short, nonblocking animation tick */
void shell_effects_changed(void);
rect_t shell_task_button_rect(window_t *w);
void shell_wallpaper_changed(void);
void desktop_refresh(void);
bool startmenu_is_open(void);
void startmenu_toggle(void);
void shell_show_run(void);
void shell_show_shutdown(void);
void shell_logoff(void);
void shell_power_action(void *restart);   /* NULL = shut down, non-NULL = restart */
extern int wallpaper_id;
extern int wallpaper_fit;                    /* 0 crop, 1 scale, 2 stretch */
extern bool sidebar_open;
extern int sidebar_tiles;                    /* bit per sidebar tile */
extern uint32_t desktop_color;
extern char user_name[32];
extern char user_password[32];
void password_set(const char *plain);          /* empty clears it; stores a hash */
bool password_matches(const char *plain);
extern char computer_name[32];
extern int user_picture;
extern int user_picture_count;
void draw_user_picture(canvas_t *c, int id, int size, int x, int y);
/* pictures the user chose (Control Panel): copied into C:\WRLD\Custom */
#define CUSTOM_WALLPAPER "C:\\WRLD\\Custom\\Desktop picture"
#define CUSTOM_AVATAR "C:\\WRLD\\Custom\\Account picture"
#define USER_PICTURE_CUSTOM user_picture_count               /* the id of the custom account picture */
#define WALLPAPER_CUSTOM (wallpaper_count - 2)               /* "Your picture", just before "(None)" */
bool wallpaper_set_custom(const char *path, char *err, int cap);
bool wallpaper_custom_exists(void);
bool user_picture_set_custom(const char *path, char *err, int cap);
bool user_picture_custom_exists(void);
extern bool show_clock, show_seconds, clock_24h, font_smoothing_dummy;
extern bool wm_animations;
enum { WINDOW_ANIM_NONE, WINDOW_ANIM_BORDER, WINDOW_ANIM_AERO, WINDOW_ANIM_COUNT };
enum { MENU_ANIM_NONE, MENU_ANIM_SLIDE, MENU_ANIM_FADE, MENU_ANIM_COUNT };
extern int wm_animation_style, startmenu_animation;
extern bool sidebar_slide, sidebar_tile_fade;
extern const char *wallpaper_names[];
extern int wallpaper_count;
void wallpaper_render(canvas_t *c, int id);

/* boot / welcome */
void boot_screen_begin(void);
void boot_screen_progress(const char *msg, int pct);
void boot_screen_status(const char *msg, int pct);
void boot_screen_end(void);
void welcome_screen(void);   /* blocks until user logs on */
#endif
