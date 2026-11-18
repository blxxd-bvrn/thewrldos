/* The Linux Package Installer: the window a .deb or .rpm opens in. It reads the
 * package (in a task of its own: unpacking takes a moment), shows what it is and
 * whether what it needs is installed, then installs it with a progress bar and
 * offers to start the program. The work is in pkg.c. */
#include "kernel.h"
#include "wm.h"
#include "theme.h"
#include "apps.h"
#include "fs.h"
#include "pkg.h"

enum { ST_LOADING, ST_READY, ST_BAD, ST_INSTALLING, ST_DONE, ST_FAILED };
typedef struct {
    int refs;                          /* the window and a worker task */
    char path[300];
    pkg_t p;
    pkg_check_t chk;
    volatile int state, pct;
    char msg[300];
    char log[2048];
    char installed_ver[96];
    bool was_installed;
    int menu_first;                    /* a program it added to the Start menu, or -1 */
    window_t *w;
} inst_t;

static void release(inst_t *s) { if (--s->refs == 0) { pkg_free(&s->p); kfree(s); } }
static void log_line(void *ctx, const char *line) {
    inst_t *s = ctx;
    if (strlen(s->log) + strlen(line) + 2 >= sizeof s->log) return;
    strlcat(s->log, line, sizeof s->log); strlcat(s->log, "\n", sizeof s->log);
}

static void load_task(void *arg) {
    inst_t *s = arg;
    if (!pkg_load(s->path, &s->p)) { strlcpy(s->msg, s->p.err, sizeof s->msg); s->state = ST_BAD; }
    else {
        pkg_check(&s->p, &s->chk);
        s->was_installed = pkg_is_installed(s->p.name, s->installed_ver, sizeof s->installed_ver);
        if (ARCH_BITS != 64) { snprintf(s->msg, sizeof s->msg, "Linux programs run on the 64-bit edition of " OS_NAME ". This is the 32-bit edition."); s->state = ST_BAD; }
        else if (!pkg_arch_ok(&s->p)) { snprintf(s->msg, sizeof s->msg, "This package is for %s processors. " OS_NAME " runs x86-64 (amd64) Linux programs: look for the amd64 or x86_64 version of the package.", s->p.arch); s->state = ST_BAD; }
        else s->state = ST_READY;
    }
    if (s->w && wm_alive(s->w)) s->w->dirty = true;
    release(s);
}
static void install_task(void *arg) {
    inst_t *s = arg;
    int before = pkg_menu_count();
    bool ok = pkg_install(&s->p, log_line, s, &s->pct, s->msg, sizeof s->msg);
    s->menu_first = -1;
    if (ok) for (int i = 0; i < pkg_menu_count(); i++) { const pkg_menu_t *m = pkg_menu_get(i); if (m && !strcmp(m->pkg, s->p.name)) { s->menu_first = i; break; } }
    (void)before;
    s->state = ok ? ST_DONE : ST_FAILED;
    if (ok) notify_balloon("Linux package installed", s->msg);
    if (s->w && wm_alive(s->w)) s->w->dirty = true;
    release(s);
}

/* one "label: value" row; returns the next y */
static int row(canvas_t *c, int x, int y, int w, const char *label, const char *value) {
    if (!value || !value[0]) return y;
    draw_text(c, FONT_UI, x, y, label, T.gray_text);
    draw_text_ellipsis(c, FONT_UI, x + 110, y, w - 110, value, T.text);
    return y + 18;
}
/* a coloured note with an icon; returns the next y */
static int note(canvas_t *c, int x, int y, int w, int icon, const char *head, const char *body, uint32_t bg) {
    int bh = body && body[0] ? text_wrapped_height(FONT_UI, w - 44, body, 1) : 0;
    int h = 26 + bh + 8;
    fill_rect(c, x, y, w, h, bg); draw_rect(c, x, y, w, h, mix(bg, T.face_dark, 128));
    draw_icon(c, icon, 16, x + 10, y + 7);
    draw_text(c, FONT_UI_BOLD, x + 34, y + 7, head, T.text);
    if (bh) draw_text_wrapped(c, FONT_UI, x + 34, y + 25, w - 44, body, T.text, 1);
    return y + h + 8;
}

static void inst_paint(window_t *w) {
    inst_t *s = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, T.window_bg);
    /* header */
    int hh = 74;
    fill_rect(c, 0, 0, c->w, hh, dark_mode ? HEX(0x20242A) : HEX(0xFFFFFF));
    hline(c, 0, hh, c->w, T.face_dark);
    draw_icon(c, ICON_PACKAGE, 48, 16, 13);
    const pkg_t *p = &s->p;
    char title[160];
    if (s->state == ST_LOADING) snprintf(title, sizeof title, "Reading %s", path_basename(s->path));
    else if (s->state == ST_BAD) snprintf(title, sizeof title, "This package cannot be installed");
    else if (s->state == ST_DONE) snprintf(title, sizeof title, "%s is installed", p->name);
    else if (s->was_installed && !strcmp(s->installed_ver, p->version)) snprintf(title, sizeof title, "Reinstall %s?", p->name);
    else if (s->was_installed) snprintf(title, sizeof title, "Update %s?", p->name);
    else snprintf(title, sizeof title, "Install %s?", p->name);
    draw_text_ellipsis(c, FONT_LARGE_BOLD, 78, 16, c->w - 94, title, T.text);
    const char *kind = p->fmt == 'r' ? "RPM package (Fedora, openSUSE, Red Hat)" : p->fmt == 'd' ? "Debian package (Debian, Ubuntu, Mint)" : "Linux software package";
    char sub[200]; snprintf(sub, sizeof sub, "%s  -  %s", kind, path_basename(s->path));
    draw_text_ellipsis(c, FONT_UI, 78, 44, c->w - 94, s->state == ST_LOADING || s->state == ST_BAD ? path_basename(s->path) : (p->summary[0] ? p->summary : sub), T.gray_text);

    int x = 20, y = hh + 14, cw = c->w - 40;
    int by = c->h - 42;                                     /* the button row */
    if (s->state == ST_LOADING) {
        draw_text(c, FONT_UI, x, y, "Unpacking the package to see what is in it...", T.text);
        draw_progress(c, R(x, y + 26, cw, 18), (timer_ticks() / 40) % 100);
        if (ui_button(w, R(c->w - 100, by, 80, 26), "Cancel")) wm_close(w);
        return;
    }
    if (s->state == ST_BAD) {
        note(c, x, y, cw, ICON_ERROR, "The package could not be opened", s->msg, dark_mode ? HEX(0x3A2424) : HEX(0xFDECEA));
        if (ui_button_ex(w, R(c->w - 100, by, 80, 26), "Close", true, true)) wm_close(w);
        return;
    }
    /* details */
    char size[32] = ""; if (p->inst_kb) format_size(size, sizeof size, p->inst_kb * 1024u);
    char files[48]; snprintf(files, sizeof files, "%d files", p->nfiles);
    y = row(c, x, y, cw, "Version", p->version);
    if (s->was_installed && strcmp(s->installed_ver, p->version)) { char b[140]; snprintf(b, sizeof b, "%s is installed now", s->installed_ver); y = row(c, x, y, cw, "", b); }
    y = row(c, x, y, cw, "Architecture", p->arch);
    y = row(c, x, y, cw, "Size", size[0] ? size : files);
    y = row(c, x, y, cw, p->fmt == 'r' ? "Packager" : "Maintainer", p->maint);
    y = row(c, x, y, cw, "Website", p->homepage);
    y = row(c, x, y, cw, "Commands", s->chk.programs);
    y += 6;
    if (s->state == ST_READY || s->state == ST_INSTALLING) {
        /* the description, a few lines */
        if (p->desc && p->desc[0]) {
            char d[700]; strlcpy(d, p->desc, sizeof d);
            for (char *q = d; *q; q++) if (*q == '\n') *q = (q[1] == '\n' || !q[1]) ? '\n' : ' ';
            int dh = MIN(text_wrapped_height(FONT_UI, cw, d, 1), 4 * 16);
            rect_t old = canvas_push_clip(c, R(x, y, cw, dh));
            draw_text_wrapped(c, FONT_UI, x, y, cw, d, T.text, 1);
            c->clip = old;
            y += dh + 10;
        }
        /* what it needs */
        const pkg_check_t *k = &s->chk;
        if (k->ok) y = note(c, x, y, cw, ICON_INFO, "Everything this package needs is installed.", NULL, dark_mode ? HEX(0x203A26) : HEX(0xE8F6EA));
        else {
            char b[1200] = "";
            if (k->missing_pkgs[0]) { strlcat(b, "Packages: ", sizeof b); strlcat(b, k->missing_pkgs, sizeof b); strlcat(b, "\n", sizeof b); }
            if (k->missing_libs[0]) { strlcat(b, "Libraries: ", sizeof b); strlcat(b, k->missing_libs, sizeof b); strlcat(b, "\n", sizeof b); }
            if (k->missing_vers[0]) { strlcat(b, "Newer library versions: ", sizeof b); strlcat(b, k->missing_vers, sizeof b); strlcat(b, "\n", sizeof b); }
            if (k->missing_interp[0]) { strlcat(b, "Interpreters: ", sizeof b); strlcat(b, k->missing_interp, sizeof b); strlcat(b, "\n", sizeof b); }
            if (k->foreign_elf) strlcat(b, "Some programs are for another processor and cannot run.\n", sizeof b);
            strlcat(b, "Install those packages first (their .deb or .rpm files), or install this one anyway: parts of it may not work.", sizeof b);
            y = note(c, x, y, cw, ICON_WARNING, "Some things this package needs are not installed", b, dark_mode ? HEX(0x3A3420) : HEX(0xFFF6DA));
        }
        if (p->scripts[0] && y < by - 40) {
            char b[200]; snprintf(b, sizeof b, "Its setup scripts (%s) will not be run.", p->scripts);
            draw_text_ellipsis(c, FONT_SMALL, x, y, cw, b, T.gray_text); y += 16;
        }
    }
    if (s->state == ST_INSTALLING) {
        draw_text(c, FONT_UI, x, by - 34, "Installing...", T.text);
        draw_progress(c, R(x + 90, by - 34, cw - 90, 16), s->pct);
        ui_button_ex(w, R(c->w - 100, by, 80, 26), "Cancel", false, false);
        return;
    }
    if (s->state == ST_DONE || s->state == ST_FAILED) {
        y = note(c, x, y, cw, s->state == ST_DONE ? ICON_INFO : ICON_ERROR, s->msg, NULL, s->state == ST_DONE ? (dark_mode ? HEX(0x203A26) : HEX(0xE8F6EA)) : (dark_mode ? HEX(0x3A2424) : HEX(0xFDECEA)));
        /* what happened, in the installer's words */
        rect_t lr = R(x, y, cw, MAX(40, by - 12 - y));
        fill_rect(c, lr.x, lr.y, lr.w, lr.h, T.face); draw_rect(c, lr.x, lr.y, lr.w, lr.h, T.face_dark);
        rect_t old = canvas_push_clip(c, R(lr.x + 1, lr.y + 1, lr.w - 2, lr.h - 2));
        draw_text_wrapped(c, FONT_SMALL, lr.x + 6, lr.y + 4, lr.w - 12, s->log, T.text, 0);
        if (s->state == ST_DONE && s->chk.programs[0] && s->menu_first < 0) {
            int ly = lr.y + 4 + text_wrapped_height(FONT_SMALL, lr.w - 12, s->log, 0) + 4;
            char b[600]; snprintf(b, sizeof b, "To run it, type its name in the Command Prompt: %s", s->chk.programs);
            draw_text_wrapped(c, FONT_SMALL, lr.x + 6, ly, lr.w - 12, b, T.link_head, 0);
        }
        c->clip = old;
        int bx = c->w - 100;
        if (ui_button_ex(w, R(bx, by, 80, 26), "Close", true, true)) { wm_close(w); return; }
        if (s->state == ST_DONE && s->menu_first >= 0) {
            bx -= 90;
            if (ui_button(w, R(bx, by, 80, 26), "Open")) {
                char err[200];
                if (!pkg_menu_launch(s->menu_first, err, sizeof err)) msgbox("Linux Package Installer", err, MBI_ERROR, MBX_OK, NULL, NULL);
                else { wm_close(w); return; }
            }
        } else if (s->state == ST_DONE && s->chk.programs[0]) {
            bx -= 150;
            if (ui_button(w, R(bx, by, 140, 26), "Command Prompt")) { terminal_open(NULL, USER_HOME); wm_close(w); return; }
        }
        return;
    }
    /* ST_READY */
    int bx = c->w - 100;
    if (ui_button(w, R(bx, by, 80, 26), "Cancel")) { wm_close(w); return; }
    bx -= 90;
    const char *verb = s->was_installed && !strcmp(s->installed_ver, p->version) ? "Reinstall" : s->was_installed ? "Update" : "Install";
    if (ui_button_ex(w, R(bx, by, 80, 26), verb, true, true)) {
        s->state = ST_INSTALLING; s->pct = 0; s->log[0] = 0;
        s->refs++;
        task_create("Package install", install_task, s, 64 * 1024);
    }
    if (s->was_installed) {
        bx -= 90;
        if (ui_button(w, R(bx, by, 80, 26), "Remove")) { if (pkg_remove_confirm(p->name)) wm_close(w); return; }
    }
}
static void inst_tick(window_t *w) { inst_t *s = w->data; if (s->state == ST_LOADING || s->state == ST_INSTALLING) w->dirty = true; }
static bool inst_close(window_t *w) { inst_t *s = w->data; return s->state != ST_INSTALLING; }
static void inst_destroy(window_t *w) { inst_t *s = w->data; s->w = NULL; release(s); }

void pkg_installer_open(const char *wrld_path) {
    inst_t *s = kzalloc(sizeof *s);
    if (!s) return;
    strlcpy(s->path, wrld_path, sizeof s->path);
    s->state = ST_LOADING; s->menu_first = -1;
    s->refs = 2;
    window_t *w = wm_create("Linux Package Installer", -1, -1, 560, 470, WF_NO_MAX);
    s->w = w;
    w->data = s; w->paint = inst_paint; w->destroy = inst_destroy; w->close = inst_close; w->icon = ICON_PACKAGE;
    wm_set_timer(w, 100, inst_tick);
    wm_center(w);
    task_create("Package reader", load_task, s, 64 * 1024);
}

/* removing, from the Control Panel or the installer: ask first */
typedef struct { char name[64]; } rm_t;
static void rm_task(void *arg) {
    rm_t *r = arg; char msg[200];
    if (pkg_remove(r->name, NULL, NULL, msg, sizeof msg)) notify_balloon("Linux package removed", msg);
    else msgbox("Remove package", msg, MBI_ERROR, MBX_OK, NULL, NULL);
    kfree(r);
}
static void rm_answer(void *ctx, int result) {
    rm_t *r = ctx;
    if (result != IDYES) { kfree(r); return; }
    task_create("Package removal", rm_task, r, 64 * 1024);
}
bool pkg_remove_confirm(const char *name) {
    rm_t *r = kzalloc(sizeof *r); strlcpy(r->name, name, sizeof r->name);
    char deps[256], q[600];
    if (pkg_dependents(name, deps, sizeof deps)) snprintf(q, sizeof q, "Remove the Linux package %s?\n\nThese installed packages use it and may stop working: %s", name, deps);
    else snprintf(q, sizeof q, "Remove the Linux package %s and the files it installed?", name);
    msgbox("Remove package", q, MBI_QUESTION, MBX_YESNO, rm_answer, r);
    return true;
}
