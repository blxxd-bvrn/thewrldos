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
void extract_open(const char *archive);       /* Extract Compressed Folder (src/apps/extract.c) */
bool archive_is_known(const char *path);      /* a .zip or .rar it can extract */
bool shell_drop_files(const char *dir, const char *paths, bool copy);   /* move / copy dropped files into a folder */
void notepad_open(const char *path);
void terminal_open(const char *script, const char *cwd);
void terminal_run(const char *cmdline, const char *cwd, const char *title);   /* a Command Prompt running one command */
void calc_open(void);
void paint_open(const char *path);
void taskmgr_open(void);
void control_open(const char *page);
void style_create_from(int base_style);
canvas_t *bmp_load(const uint8_t *d, uint32_t n);
canvas_t *image_decode(const uint8_t *d, uint32_t n, char *err, int errcap);
uint8_t *bmp_save(canvas_t *c, uint32_t *size);                /* src/apps/system.c: a 24-bit BMP of the canvas */   /* PNG, GIF, JPEG, BMP */
void browser_open(const char *url);             /* WebKit when it can run, else the built-in engine (webkit.c) */
void browser_classic_open(const char *url);     /* the built-in engine */
void cp_theme_thumb(canvas_t *s, int x, int y, int id, int hue, bool sel);
canvas_t *cp_theme_preview(int id, int hue, int w, int h);   /* cached; owned by Control Panel */
void about_open(void);
void snake_open(void);
void minesweeper_open(void);
void pong_open(void);
void solitaire_open(void);
void pinball_open(void);
void sentinel_open(void);
void security_open(void);
void inetopts_open(int tab);
void netconn_open(void);                         /* Network Connections (ncpa.cpl) */
void netstatus_open(void);                       /* Local Area Connection Status */                     /* Internet Options; tab 0 General .. 6 Advanced */
void wrldsearch_open(const char *query);   /* The WRLD Search, optionally already searching */
void recent_add(const char *path);
void clip_set_text(const char *s);         /* the system clipboard (plain text, UTF-8) */
const char *clip_text(void);                /* NULL when empty */
uint32_t clip_gen(void);                    /* changes whenever the clipboard does */
void shell_open_later(const char *path);    /* shell_open on the desktop's thread */
void wordpad_open(const char *path);          /* a file the user opened, for Recent Activities */
void mediaplayer_open(const char *path);
void demo3d_open(void);                    /* WRLD 3D: the 3D engine's test */   /* NULL: My Music */
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
/* the Open/Save dialog (notepad.c); the window is returned so a caller waiting for the
 * answer can tell Cancel (the window is gone and cb was not called) */
window_t *file_dialog_win(const char *title, const char *initial, bool save, const char *ext, void (*cb)(void *ctx, const char *path), void *ctx);
#endif
