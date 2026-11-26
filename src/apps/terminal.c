/* Command Prompt: a VT-ish console window hosting the sh interpreter or Win32 console programs */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"

#define TCOLS 132
#define TROWS 600
#define CELL_W 8
#define CELL_H 15
typedef struct { char ch; uint8_t fg, bg; } cell_t;
typedef struct {
    console_t con;
    window_t *win;
    cell_t *cells;           /* TROWS x TCOLS ring */
    int top;                 /* index of first row in ring */
    int nrows;               /* rows used */
    int cx, cy;              /* cursor (cy relative to first row) */
    int fg, bg;
    int scroll;              /* lines scrolled back */
    /* input */
    bool reading, line_done, want_key;
    char line[512]; int llen, lpos;
    char typeahead[256]; int tlen;
    int key_val; bool key_ready;
    char *hist[50]; int nhist, hpos;
    bool interrupted;
    bool closed;
    int pid;
    char cwd[256];
    char esc[16]; int esc_len; bool in_esc;
    bool wrap_pending;
} term_t;

static const uint32_t vga_pal[16] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xC0C0C0,
    0x808080, 0x0000FF, 0x00FF00, 0x00FFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF };
static const int ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static int term_cols(term_t *t) { return MAX(20, MIN(TCOLS, (t->win->surf->w - 20) / CELL_W)); }
static int term_vis_rows(term_t *t) { return MAX(3, (t->win->surf->h - 6) / CELL_H); }
static cell_t *row_ptr(term_t *t, int r) { return t->cells + ((t->top + r) % TROWS) * TCOLS; }
static void clear_row(term_t *t, int r) { cell_t *p = row_ptr(t, r); for (int i = 0; i < TCOLS; i++) { p[i].ch = ' '; p[i].fg = t->fg; p[i].bg = t->bg; } }
static void newline(term_t *t) {
    t->cx = 0; t->cy++;
    if (t->cy >= t->nrows) {
        if (t->nrows < TROWS) { t->nrows++; clear_row(t, t->nrows - 1); }
        else { t->top = (t->top + 1) % TROWS; t->cy = TROWS - 1; clear_row(t, t->cy); }
    }
}
static void put_ch(term_t *t, char c) {
    if (t->in_esc) {
        if (t->esc_len < 15) t->esc[t->esc_len++] = c;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            t->esc[t->esc_len] = 0; t->in_esc = false;
            if (c == 'm') {
                char *p = t->esc + 1;
                while (*p) {
                    int v = strtol(p, &p, 10);
                    if (v == 0) { t->fg = 7; t->bg = 0; }
                    else if (v == 1) t->fg |= 8;
                    else if (v >= 30 && v <= 37) t->fg = (t->fg & 8) | ansi_to_vga[v - 30];
                    else if (v >= 90 && v <= 97) t->fg = 8 | ansi_to_vga[v - 90];
                    else if (v >= 40 && v <= 47) t->bg = ansi_to_vga[v - 40];
                    if (*p == ';') p++; else if (*p != 'm') break; else break;
                }
            } else if (c == 'J') { t->con.clear(&t->con); }
        }
        return;
    }
    if (c == 27) { t->in_esc = true; t->esc_len = 0; return; }
    if (t->cy >= t->nrows) { t->nrows = t->cy + 1; clear_row(t, t->cy); }
    if (c == '\n') { newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\b') { if (t->cx > 0) t->cx--; return; }
    if (c == '\t') { int n = 8 - (t->cx % 8); while (n--) put_ch(t, ' '); return; }
    if (c == 7) return;
    if ((uint8_t)c < 32) return;
    if (t->cx >= term_cols(t)) newline(t);
    cell_t *p = row_ptr(t, t->cy) + t->cx;
    p->ch = c; p->fg = t->fg; p->bg = t->bg;
    t->cx++;
}
static void t_write(console_t *c, const char *s, int n) {
    term_t *t = c->impl;
    if (t->closed) return;
    for (int i = 0; i < n; i++) put_ch(t, s[i]);
    t->scroll = 0;
    t->win->dirty = true;
}
static void t_clear(console_t *c) {
    term_t *t = c->impl;
    t->top = 0; t->nrows = 1; t->cx = t->cy = 0; clear_row(t, 0); t->win->dirty = true;
}
static int t_readline(console_t *c, char *buf, int cap) {
    term_t *t = c->impl;
    if (t->closed) return -1;
    t->llen = t->lpos = 0; t->line[0] = 0; t->line_done = false; t->interrupted = false;
    /* typeahead */
    for (int i = 0; i < t->tlen; i++) {
        char ch = t->typeahead[i];
        if (ch == '\n') { t->line_done = true; memmove(t->typeahead, t->typeahead + i + 1, t->tlen - i - 1); t->tlen -= i + 1; goto done; }
        if (t->llen < 510) { t->line[t->llen++] = ch; t->lpos = t->llen; }
    }
    t->tlen = 0;
    t->reading = true; t->win->dirty = true;
    while (!t->line_done && !t->closed) task_sleep(15);
    t->reading = false;
    if (t->closed) return -1;
done:
    t->line[t->llen] = 0;
    t_write(c, t->line, t->llen); t_write(c, "\n", 1);
    if (t->llen && (!t->nhist || strcmp(t->hist[t->nhist - 1], t->line))) {
        if (t->nhist == 50) { kfree(t->hist[0]); memmove(t->hist, t->hist + 1, 49 * sizeof(char *)); t->nhist--; }
        t->hist[t->nhist++] = strdup(t->line);
    }
    t->hpos = t->nhist;
    if (t->interrupted) { buf[0] = 0; return 0; }
    strlcpy(buf, t->line, cap);
    return strlen(buf);
}
static int t_getkey(console_t *c) {
    term_t *t = c->impl;
    if (t->tlen) { int k = t->typeahead[0]; memmove(t->typeahead, t->typeahead + 1, --t->tlen); return k; }
    t->key_ready = false; t->want_key = true;
    while (!t->key_ready && !t->closed) task_sleep(15);
    t->want_key = false;
    return t->closed ? -1 : t->key_val;
}
static void t_set_title(console_t *c, const char *s) { term_t *t = c->impl; if (!t->closed) wm_set_title(t->win, s); }
static void t_set_color(console_t *c, int fg, int bg) { term_t *t = c->impl; t->fg = fg & 15; t->bg = bg & 15; }
static bool t_alive(console_t *c) { term_t *t = c->impl; return !t->closed && !t->interrupted; }

/* ---------------- window ---------------- */
static void term_paint(window_t *w) {
    term_t *t = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, HEX(vga_pal[0]));
    int cols = term_cols(t), vis = term_vis_rows(t);
    int sbw = 17;
    int first = MAX(0, t->nrows - vis - t->scroll);
    /* input line overlay position */
    for (int r = 0; r < vis && first + r < t->nrows; r++) {
        cell_t *row = row_ptr(t, first + r);
        int y = 3 + r * CELL_H;
        for (int i = 0; i < cols; i++) {
            if (row[i].bg) fill_rect(c, 4 + i * CELL_W, y, CELL_W, CELL_H, HEX(vga_pal[row[i].bg]));
            if (row[i].ch != ' ') { char ch = row[i].ch; draw_text_n(c, FONT_MONO, 4 + i * CELL_W, y, &ch, 1, HEX(vga_pal[row[i].fg])); }
        }
    }
    /* pending input and cursor */
    int cy = t->cy - first, cx = t->cx;
    if (t->reading) {
        int x = cx, y = cy;
        for (int i = 0; i <= t->llen; i++) {
            if (x >= cols) { x = 0; y++; }
            if (y >= 0 && y < vis) {
                if (i == t->lpos && (timer_ticks() / 500) % 2 == 0) fill_rect(c, 4 + x * CELL_W, 3 + y * CELL_H + CELL_H - 3, CELL_W, 2, HEX(0xC0C0C0));
                if (i < t->llen) draw_text_n(c, FONT_MONO, 4 + x * CELL_W, 3 + y * CELL_H, &t->line[i], 1, HEX(vga_pal[7]));
            }
            x++;
        }
    } else if (cy >= 0 && cy < vis && t->scroll == 0 && (timer_ticks() / 500) % 2 == 0 && !t->closed)
        fill_rect(c, 4 + cx * CELL_W, 3 + cy * CELL_H + CELL_H - 3, CELL_W, 2, HEX(0xC0C0C0));
    draw_scrollbar(c, R(c->w - sbw, 0, sbw, c->h), t->nrows, vis, MAX(0, t->nrows - vis - t->scroll), true);
}
static void term_blink(window_t *w) { w->dirty = true; }
static void push_typeahead(term_t *t, char ch) { if (t->tlen < 255) t->typeahead[t->tlen++] = ch; }
static void complete_word(term_t *t) {
    int s = t->llen; while (s > 0 && t->line[s - 1] != ' ') s--;
    char word[256]; strlcpy(word, t->line + s, sizeof word);
    char dir[256] = "", *base = word;
    char *sl = strrchr(word, '\\'); char *sl2 = strrchr(word, '/'); if (sl2 > sl) sl = sl2;
    if (sl) { int l = sl - word + 1; memcpy(dir, word, l); dir[l] = 0; base = sl + 1; }
    vnode_t *d = vfs_resolve(dir[0] ? dir : ".", t->cwd);
    if (!d || !d->dir) return;
    vnode_t *match = NULL; int nm = 0;
    for (vnode_t *c = d->child; c; c = c->next) if (!strncasecmp(c->name, base, strlen(base))) { match = c; nm++; }
    if (nm != 1) return;
    char add[160]; strlcpy(add, match->name + strlen(base), sizeof add);
    bool sp = strchr(match->name, ' ') != NULL;
    if (sp && !strchr(t->line + s, '"')) {
        char nw[300]; snprintf(nw, sizeof nw, "\"%s%s%s\"", dir, match->name, match->dir ? "\\" : "");
        t->llen = s; t->line[s] = 0; strlcat(t->line, nw, sizeof t->line); t->llen = strlen(t->line);
    } else { strlcat(t->line, add, sizeof t->line); if (match->dir) strlcat(t->line, "\\", sizeof t->line); t->llen = strlen(t->line); }
    t->lpos = t->llen;
}
static void term_key(window_t *w, key_event_t *e) {
    term_t *t = w->data;
    if (!e->pressed) return;
    if ((e->mods & MOD_CTRL) && (e->key == 'c' || e->key == 'C')) {
        t->interrupted = true;
        if (t->reading) { t->line_done = true; t_write(&t->con, "^C", 2); }
        else { t_write(&t->con, "^C\n", 3); }
        return;
    }
    if ((e->mods & MOD_CTRL) && (e->key == 'l' || e->key == 'L')) { t_clear(&t->con); return; }
    if (e->key == KEY_PGUP) { t->scroll = MIN(t->scroll + term_vis_rows(t) / 2, MAX(0, t->nrows - term_vis_rows(t))); w->dirty = true; return; }
    if (e->key == KEY_PGDN) { t->scroll = MAX(0, t->scroll - term_vis_rows(t) / 2); w->dirty = true; return; }
    if (t->want_key && e->ch) { t->key_val = e->ch; t->key_ready = true; return; }
    if (!t->reading) { if (e->ch) push_typeahead(t, e->ch == '\r' ? '\n' : e->ch); return; }
    t->scroll = 0;
    switch (e->key) {
    case KEY_ENTER: t->line_done = true; break;
    case KEY_BACKSPACE: if (t->lpos > 0) { memmove(t->line + t->lpos - 1, t->line + t->lpos, t->llen - t->lpos); t->llen--; t->lpos--; } break;
    case KEY_DELETE: if (t->lpos < t->llen) { memmove(t->line + t->lpos, t->line + t->lpos + 1, t->llen - t->lpos - 1); t->llen--; } break;
    case KEY_LEFT: if (t->lpos > 0) t->lpos--; break;
    case KEY_RIGHT: if (t->lpos < t->llen) t->lpos++; break;
    case KEY_HOME: t->lpos = 0; break;
    case KEY_END: t->lpos = t->llen; break;
    case KEY_UP: if (t->hpos > 0) { t->hpos--; strlcpy(t->line, t->hist[t->hpos], sizeof t->line); t->llen = t->lpos = strlen(t->line); } break;
    case KEY_DOWN: if (t->hpos < t->nhist - 1) { t->hpos++; strlcpy(t->line, t->hist[t->hpos], sizeof t->line); t->llen = t->lpos = strlen(t->line); } else { t->hpos = t->nhist; t->llen = t->lpos = 0; } break;
    case KEY_TAB: t->line[t->llen] = 0; complete_word(t); break;
    case KEY_ESC: t->llen = t->lpos = 0; break;
    default:
        if (e->ch >= 32 && e->ch < 127 && t->llen < 510) { memmove(t->line + t->lpos + 1, t->line + t->lpos, t->llen - t->lpos); t->line[t->lpos++] = e->ch; t->llen++; }
    }
    t->line[t->llen] = 0;
    w->dirty = true;
}
static void term_mouse(window_t *w, int type, int x, int y, int b) {
    term_t *t = w->data;
    if (type == ME_WHEEL) { t->scroll = CLAMP(t->scroll - b * 3, 0, MAX(0, t->nrows - term_vis_rows(t))); w->dirty = true; }
    if (type == ME_DOWN && x >= w->surf->w - 17) {
        int vis = term_vis_rows(t), maxs = MAX(0, t->nrows - vis);
        if (y < 17) t->scroll = MIN(maxs, t->scroll + 1); else if (y > w->surf->h - 17) t->scroll = MAX(0, t->scroll - 1);
        else t->scroll = maxs - (y - 17) * maxs / MAX(1, w->surf->h - 34);
        t->scroll = CLAMP(t->scroll, 0, maxs); w->dirty = true;
    }
}
static bool term_close(window_t *w) {
    term_t *t = w->data;
    t->closed = true;
    if (t->pid && task_find(t->pid) && t->pid != task_current()->pid) task_kill(t->pid);
    return true;
}
static void term_destroy(window_t *w) {
    term_t *t = w->data;
    t->closed = true;
    /* the console may still be referenced by a dying task; keep struct, free buffers lazily */
    kfree(t->cells); t->cells = NULL;
    for (int i = 0; i < t->nhist; i++) kfree(t->hist[i]);
    t->nhist = 0;
}

/* NULL (and no window) when memory runs out */
console_t *terminal_new_console(const char *title, window_t **out) {
    if (out) *out = NULL;
    term_t *t = kzalloc(sizeof(term_t));
    if (t) t->cells = kmalloc(sizeof(cell_t) * TROWS * TCOLS);
    if (!t || !t->cells) {
        if (t) kfree(t);
        msgbox("Not enough memory", "There is not enough memory to open a Command Prompt window. Close some programs or windows and try again.", MBI_ERROR, MBX_OK, NULL, NULL);
        return NULL;
    }
    window_t *w = wm_create(title, -1, -1, 80 * CELL_W + 25, 25 * CELL_H + 6, WF_RESIZABLE);
    if (!w) { kfree(t->cells); kfree(t); return NULL; }
    t->win = w; t->fg = 7; t->bg = 0;
    t->nrows = 1; clear_row(t, 0);
    t->con.write = t_write; t->con.readline = t_readline; t->con.getkey = t_getkey; t->con.clear = t_clear;
    t->con.set_title = t_set_title; t->con.set_color = t_set_color; t->con.alive = t_alive; t->con.impl = t;
    strcpy(t->cwd, USER_HOME);
    w->data = t; w->paint = term_paint; w->key = term_key; w->mouse = term_mouse; w->close = term_close; w->destroy = term_destroy;
    w->icon = ICON_TERMINAL; w->min_w = 300; w->min_h = 150;
    w->cursor = CURSOR_IBEAM;
    wm_set_timer(w, 500, term_blink);
    if (out) *out = w;
    return &t->con;
}
const char *terminal_cwd_ptr(console_t *c, char **cwd) { static char dummy[256]; if (!c || c->write != t_write) { *cwd = dummy; return dummy; } term_t *t = c->impl; *cwd = t->cwd; return t->cwd; }
bool console_interrupted(console_t *c) { if (!c || c->write != t_write) return false; term_t *t = c->impl; return t->interrupted || t->closed; }
void console_clear_interrupt(console_t *c) { if (c && c->write == t_write) ((term_t *)c->impl)->interrupted = false; }
void console_set_pid(console_t *c, int pid) { if (c && c->write == t_write) { term_t *t = c->impl; t->pid = pid; t->win->pid = pid; } }

void con_puts(console_t *c, const char *s) { if (c) c->write(c, s, strlen(s)); }
void con_printf(console_t *c, const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (c) c->write(c, buf, MIN(n, (int)sizeof buf - 1));
}

typedef struct { console_t *con; char path[256]; char cwd[256]; char cmd[600]; } term_job_t;
static void term_task(void *arg) {
    term_job_t *j = arg;
    console_t *con = j->con;
    if (j->cmd[0]) {                                   /* a command line (terminal_run) */
        sh_run_string(con, j->cmd, j->cwd);
        con_puts(con, "\n\x1b[93m[Process finished] Press any key to close this window...\x1b[0m");
        con->getkey(con);
    } else if (j->path[0]) {
        const char *e = vfs_ext(j->path);
        if (!strcasecmp(e, "exe")) {
            char cmd[300]; snprintf(cmd, sizeof cmd, "\"%s\"", j->path);
            sh_run_string(con, cmd, j->cwd);
        } else {
            con_printf(con, "\x1b[90m$ sh %s\x1b[0m\n", j->path);
            sh_run_file(con, j->path, 0, NULL, j->cwd);
        }
        con_puts(con, "\n\x1b[93m[Process finished] Press any key to close this window...\x1b[0m");
        con->getkey(con);
    } else {
        con_printf(con, "%s [Version %s (Build %s)]\n(C) THE WRLD OS project. Type 'help' for a list of commands.\n\n", OS_NAME, OS_VERSION, OS_BUILD);
        sh_interactive(con, j->cwd[0] ? j->cwd : USER_HOME);
    }
    kfree(j);
}
void terminal_open(const char *path, const char *cwd) {
    window_t *w;
    char title[128];
    if (path) snprintf(title, sizeof title, "%s", path_basename(path)); else strcpy(title, "Command Prompt");
    console_t *con = terminal_new_console(title, &w);
    if (!con) return;
    term_job_t *j = kzalloc(sizeof *j);
    if (!j) { con->write(con, "Not enough memory.\n", 19); return; }
    j->con = con;
    if (path) strlcpy(j->path, path, sizeof j->path);
    strlcpy(j->cwd, cwd ? cwd : USER_HOME, sizeof j->cwd);
    task_t *t = task_create(path ? title : "cmd.exe", term_task, j, 128 * 1024);
    if (!t) { kfree(j); con->write(con, "Not enough memory to start the command interpreter.\n", 52); return; }
    t->kind = 1;
    t->console = con;
    console_set_pid(con, t->pid);
}

/* a Command Prompt window that runs one command line, then waits for a key */
void terminal_run(const char *cmdline, const char *cwd, const char *title) {
    window_t *w;
    console_t *con = terminal_new_console(title && *title ? title : "Command Prompt", &w);
    if (!con) return;
    term_job_t *j = kzalloc(sizeof *j);
    if (!j) { con->write(con, "Not enough memory.\n", 19); return; }
    j->con = con;
    strlcpy(j->cmd, cmdline, sizeof j->cmd);
    strlcpy(j->cwd, cwd ? cwd : USER_HOME, sizeof j->cwd);
    task_t *t = task_create(title && *title ? title : "cmd.exe", term_task, j, 128 * 1024);
    if (!t) { kfree(j); con->write(con, "Not enough memory to start the command interpreter.\n", 52); return; }
    t->kind = 1;
    t->console = con;
    console_set_pid(con, t->pid);
}
