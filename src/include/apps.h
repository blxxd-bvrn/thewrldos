#ifndef APPS_H
#define APPS_H
#include "wm.h"
#include "fs.h"

typedef struct { const char *id, *title, *subtitle; int icon; } app_info_t;
extern const app_info_t app_table[];
extern const int app_count;
const app_info_t *app_find(const char *id);

void app_launch(const char *name, const char *arg);
bool shell_open(const char *path, const char *cwd);
bool shell_execute(const char *cmdline, const char *cwd);   /* Run dialog / start */
int file_icon(vnode_t *n);
const char *file_type_name(vnode_t *n);

void explorer_open(const char *path);
void notepad_open(const char *path);
void terminal_open(const char *script, const char *cwd);
void calc_open(void);
void paint_open(const char *path);
void taskmgr_open(void);
void control_open(const char *page);
void style_create_from(int base_style);
canvas_t *bmp_load(const uint8_t *d, uint32_t n);
canvas_t *image_decode(const uint8_t *d, uint32_t n, char *err, int errcap);   /* PNG, GIF, JPEG, BMP */
void browser_open(const char *url);
void cp_theme_thumb(canvas_t *s, int x, int y, int id, int hue, bool sel);
void about_open(void);
void snake_open(void);
void minesweeper_open(void);
void pong_open(void);
void security_open(void);
void mediaplayer_open(const char *path);   /* NULL: My Music */
void imageview_open(const char *path);
void properties_open(vnode_t *n);
void bundle_open(vnode_t *app);

/* console abstraction shared by the shell interpreter and Win32 console apps */
typedef struct console console_t;
struct console {
    void (*write)(console_t *c, const char *s, int n);
    int (*readline)(console_t *c, char *buf, int cap);   /* blocking (yields) */
    int (*getkey)(console_t *c);                          /* blocking, returns char */
    void (*clear)(console_t *c);
    void (*set_title)(console_t *c, const char *t);
    void (*set_color)(console_t *c, int fg, int bg);
    bool (*alive)(console_t *c);
    void *impl;
};
void con_printf(console_t *c, const char *fmt, ...);
void con_puts(console_t *c, const char *s);
console_t *terminal_new_console(const char *title, window_t **out);

/* sh interpreter */
int sh_run_file(console_t *con, const char *path, int argc, char **argv, const char *cwd);
int sh_run_string(console_t *con, const char *script, const char *cwd);
void sh_interactive(console_t *con, const char *cwd);
void sh_spawn_script(const char *path, const char *cwd);

/* Win32 PE loader */
int pe_spawn(const char *path, const char *args, console_t *con, char *err, int errcap);
bool pe_probe(vnode_t *n, char *desc, int cap);
void w32_init(void);
const char *w32_api_list(int dll, int idx);  /* for documentation */

/* Mach-O identification */
bool macho_probe(const uint8_t *d, uint32_t n, char *desc, int cap);
#endif
