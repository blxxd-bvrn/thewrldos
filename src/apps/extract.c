/*
 * Extract Compressed Folder: Explorer's "Extract All..." for .zip and .rar archives.
 *
 * The decoding is Rust (rust/src/unarc.rs for the formats, archive_extract in
 * rust/src/kernel/shk.rs for the files); it runs on its own task, which yields often, and
 * this window follows it through a shared status block. One extraction runs at a time.
 */
#include "kernel.h"
#include "wm.h"
#include "theme.h"
#include "apps.h"
#include "fs.h"

/* mirrors ArcStatus in rust/src/kernel/shk.rs */
typedef struct {
    int percent, files, failed;
    int done, cancel;                      /* the kernel's bool is an int */
    char current[160];
    char error[256];
} arc_status_t;
extern int archive_extract(const char *arc, const char *dest, arc_status_t *st);
extern int archive_detect(const char *path);

enum { EX_ASK, EX_RUNNING, EX_DONE };
typedef struct {
    window_t *w;
    char arc[256], name[96], dest[256];
    bool show;                             /* open the folder afterwards */
    int state, result;
    arc_status_t st;
} ex_t;
static ex_t *running;                      /* the extraction in progress, if any */

static void ex_task(void *arg) {
    ex_t *e = arg;
    e->result = archive_extract(e->arc, e->dest, &e->st);
    e->st.done = 1;
    if (!e->w) { if (running == e) running = NULL; kfree(e); }   /* its window was closed */
}

static void ex_finish(window_t *w) {
    ex_t *e = w->data;
    if (running == e) running = NULL;
    e->state = EX_DONE;
    char m[600];
    if (e->result < 0) {
        snprintf(m, sizeof m, "%s could not be extracted: it is %s.", e->name, e->st.error[0] ? e->st.error : "damaged");
        msgbox("Extract Compressed Folder", m, MBI_ERROR, MBX_OK, NULL, NULL);
    } else if (e->st.cancel) {
        notify_balloon("Extraction cancelled", "The files extracted so far were kept.");
    } else if (e->st.failed > 0) {
        snprintf(m, sizeof m, "%d file%s extracted, %d could not be:\n\n%s%s", e->st.files, e->st.files == 1 ? " was" : "s were", e->st.failed,
                 e->st.error, e->st.failed > 1 ? "\n(and others)" : "");
        msgbox("Extract Compressed Folder", m, MBI_WARNING, MBX_OK, NULL, NULL);
    }
    if (e->result >= 0 && e->show && !e->st.cancel) explorer_open(e->dest);
    wm_destroy(w);
}

static void ex_paint(window_t *w) {
    ex_t *e = w->data;
    canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    /* the wizard's white header band */
    fill_rect(c, 0, 0, c->w, 58, dark_mode ? HEX(0x2A2A2A) : HEX(0xFFFFFF));
    hline(c, 0, 58, c->w, T.face_dark);
    draw_text(c, FONT_UI_BOLD, 16, 12, e->state == EX_ASK ? "Select a Destination and Extract Files" : "Extracting Files", T.text);
    draw_text_ellipsis(c, FONT_UI, 26, 32, c->w - 90, e->name, T.gray_text);
    draw_icon(c, ICON_ZIP_FOLDER, 32, c->w - 48, 13);
    int x = 16, cw = c->w - 32, by = c->h - 38;
    if (e->state == EX_ASK) {
        draw_text(c, FONT_UI, x, 74, "Files will be extracted to this folder:", T.text);
        if (!w->ui.focus) w->ui.focus = 1;
        bool enter = ui_textbox(w, R(x, 94, cw, 22), e->dest, sizeof e->dest);
        ui_checkbox(w, x, 130, "Show extracted files when complete", &e->show);
        bool go = ui_button_ex(w, R(c->w - 176, by, 75, 23), "Extract", true, true);
        if (ui_button(w, R(c->w - 91, by, 75, 23), "Cancel")) { wm_destroy(w); return; }
        if ((go || enter) && e->dest[0]) {
            if (running) { msgbox("Extract Compressed Folder", "Another archive is being extracted. Wait for it to finish, then try again.", MBI_INFO, MBX_OK, NULL, NULL); return; }
            if (!(e->dest[0] && e->dest[1] == ':')) { msgbox("Extract Compressed Folder", "Type a full folder path, such as C:\\Documents and Settings\\User\\My Documents\\Files.", MBI_WARNING, MBX_OK, NULL, NULL); return; }
            e->state = EX_RUNNING;
            running = e;
            task_create("Extract", ex_task, e, 256 * 1024);
        }
        return;
    }
    if (e->state == EX_RUNNING) {
        if (e->st.done) { ex_finish(w); return; }
        draw_text(c, FONT_UI, x, 74, "Extracting:", T.text);
        draw_text_ellipsis(c, FONT_UI, x + 70, 74, cw - 70, e->st.current[0] ? e->st.current : "...", T.text);
        draw_progress(c, R(x, 98, cw, 18), CLAMP(e->st.percent, 0, 100));
        char b[80]; snprintf(b, sizeof b, "%d file%s extracted", e->st.files, e->st.files == 1 ? "" : "s");
        draw_text(c, FONT_SMALL, x, 124, b, T.gray_text);
        if (ui_button_ex(w, R(c->w - 91, by, 75, 23), "Cancel", !e->st.cancel, false)) e->st.cancel = 1;
    }
}
static void ex_tick(window_t *w) { ex_t *e = w->data; if (e->state == EX_RUNNING) w->dirty = true; }
static bool ex_close(window_t *w) { ex_t *e = w->data; if (e->state == EX_RUNNING) { e->st.cancel = 1; return false; } return true; }
static void ex_destroy(window_t *w) {
    ex_t *e = w->data;
    if (e->state == EX_RUNNING && !e->st.done) { e->w = NULL; e->st.cancel = 1; return; }   /* the task frees it */
    kfree(e);
}

/* true for the archives this dialog can open (by name, then by contents) */
bool archive_is_known(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".rar") || !strcasecmp(dot, ".cbz") || !strcasecmp(dot, ".cbr"))) return true;
    return dot && !strcasecmp(dot, ".exe") ? false : archive_detect(path);
}

void extract_open(const char *archive) {
    vnode_t *n = vfs_resolve(archive, NULL);
    if (!n || n->dir) return;
    ex_t *e = kzalloc(sizeof *e);
    if (!e) return;
    vfs_path(n, e->arc, sizeof e->arc);
    strlcpy(e->name, n->name, sizeof e->name);
    /* default: a folder next to the archive, named after it */
    char dir[256], base[96];
    path_dirname(e->arc, dir, sizeof dir);
    strlcpy(base, n->name, sizeof base);
    char *dot = strrchr(base, '.'); if (dot && dot != base) *dot = 0;
    path_join(e->dest, sizeof e->dest, dir, base);
    e->show = true;
    window_t *w = wm_create("Extract Compressed Folder", -1, -1, 440, 210, WF_NO_MAX | WF_NO_MIN);
    e->w = w;
    w->data = e; w->paint = ex_paint; w->close = ex_close; w->destroy = ex_destroy; w->icon = ICON_ZIP_FOLDER;
    wm_set_timer(w, 100, ex_tick);
    wm_center(w);
    ui_textbox_select_all(w);
}
