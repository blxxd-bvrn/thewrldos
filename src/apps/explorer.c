/* File Explorer: XP-style task pane, Icons/Details views, history, clipboard, Recycle Bin */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"

typedef struct { char name[96]; int icon; vnode_t *node; char path[256]; const char *type; uint32_t size; int group; } eitem_t;
typedef struct {
    char path[256];
    char hist[16][256]; int hpos, hlen;
    eitem_t *items; int n, cap;
    int sel, scroll;
    int view;                /* 0 icons, 1 details, 2 tiles */
    uint32_t gen;
    char addr[256];
    bool addr_dirty;
    window_t *win;
    int press_i, press_x, press_y;          /* the item the left button went down on, for a drag */
} expl_t;

#define TB_H 38
#define AB_H 26
#define SB_H 20
#define PANE_W 196
static vnode_t *clip_node; static bool clip_cut;

/* ---------------- recycle bin ---------------- */
typedef struct { vnode_t *node; char orig[256]; } recycled_t;
static recycled_t recycled[64]; static int nrecycled;
static vnode_t *recycler(void) { vnode_t *r = vfs_resolve("C:\\RECYCLER", NULL); if (!r) r = vfs_mkdir_p("C:\\RECYCLER"); return r; }
bool recycle_move(vnode_t *n) {
    if (!n || n->readonly || !n->parent || n->parent->readonly) return false;
    if (vfs_drive_letter(n) != 'C' || nrecycled >= 64) return vfs_delete(n);
    char p[256]; vfs_path(n->parent, p, sizeof p);
    vnode_t *r = recycler();
    vnode_t *ex = vfs_lookup_child(r, n->name);
    if (ex) { for (int i = 0; i < nrecycled; i++) if (recycled[i].node == ex) { recycled[i] = recycled[--nrecycled]; break; } vfs_delete(ex); }
    if (!vfs_move(n, r)) return false;
    recycled[nrecycled].node = n; strlcpy(recycled[nrecycled].orig, p, 256); nrecycled++;
    return true;
}
static void recycle_restore(vnode_t *n) {
    for (int i = 0; i < nrecycled; i++) if (recycled[i].node == n) {
        vnode_t *d = vfs_resolve(recycled[i].orig, NULL); if (!d) d = vfs_mkdir_p(recycled[i].orig);
        vfs_move(n, d); recycled[i] = recycled[--nrecycled]; return;
    }
}
void recycle_empty(void) { vnode_t *r = recycler(); while (r->child) vfs_delete(r->child); nrecycled = 0; desktop_refresh(); }
int recycle_count(void) { return vfs_count_children(recycler()); }

/* ---------------- items ---------------- */
static void add_item(expl_t *e, const char *name, int icon, vnode_t *n, const char *path, const char *type, int group) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 32; e->items = krealloc(e->items, e->cap * sizeof(eitem_t)); }
    eitem_t *it = &e->items[e->n++];
    memset(it, 0, sizeof *it);
    strlcpy(it->name, name, sizeof it->name); it->icon = icon; it->node = n; strlcpy(it->path, path, sizeof it->path);
    it->type = type; it->size = n && !n->dir ? n->size : 0; it->group = group;
}
static bool is_special(expl_t *e) { return e->path[0] == ':'; }
static void title_for(expl_t *e, char *b, int cap) {
    if (!strcmp(e->path, "::computer")) strlcpy(b, "My Computer", cap);
    else if (!strcmp(e->path, "::recycle")) strlcpy(b, "Recycle Bin", cap);
    else {
        vnode_t *n = vfs_resolve(e->path, NULL);
        if (!n) strlcpy(b, e->path, cap);
        else if (!n->parent) snprintf(b, cap, "%s (%c:)", vfs_drive_label(n->drive), n->drive);
        else if (!strcasecmp(e->path, USER_DOCS)) strlcpy(b, "My Documents", cap);
        else strlcpy(b, n->name, cap);
    }
}
static void rebuild(expl_t *e) {
    e->n = 0;
    if (!strcmp(e->path, "::computer")) {
        /* Hard Disk Drives, then Devices with Removable Storage (discs, sticks, disk images) */
        for (int pass = 0; pass < 2; pass++)
            for (char l = 'C'; l <= 'Z'; l++) {
                int k = vfs_drive_kind(l);
                if (!k || (pass == 0) != (k == 1)) continue;
                char nm[64], p[8] = { l, ':', '\\', 0 };
                snprintf(nm, sizeof nm, "%s (%c:)", vfs_drive_label(l), l);
                static const char *types[] = { "", "Local Disk", "Disk Image (read-only)", "CD Drive", "Removable Disk" };
                static const int icons[] = { ICON_DRIVE, ICON_DRIVE, ICON_DRIVE_DMG, ICON_DRIVE_CD, ICON_DRIVE_USB };
                add_item(e, nm, icons[k > 4 ? 1 : k], vfs_root(l), p, types[k > 4 ? 1 : k], pass);
            }
        add_item(e, "Control Panel", ICON_CONTROL_PANEL, NULL, "::control", "System Folder", 2);
        add_item(e, "My Documents", ICON_MY_DOCUMENTS, vfs_resolve(USER_DOCS, NULL), USER_DOCS, "File Folder", 2);
    } else if (!strcmp(e->path, "::recycle")) {
        vnode_t *r = recycler();
        for (vnode_t *c = r->child; c; c = c->next) { char p[256]; vfs_path(c, p, sizeof p); add_item(e, c->name, file_icon(c), c, p, file_type_name(c), 0); }
    } else {
        vnode_t *d = vfs_resolve(e->path, NULL);
        if (d && d->dir) {
            vfs_sort_children(d);
            for (vnode_t *c = d->child; c; c = c->next) {
                if (!d->parent && !strcmp(c->name, "RECYCLER")) continue;
                char p[256]; vfs_path(c, p, sizeof p);
                add_item(e, c->name, file_icon(c), c, p, file_type_name(c), 0);
            }
        }
    }
    e->gen = vfs_generation;
    if (e->sel >= e->n) e->sel = -1;
    char t[128]; title_for(e, t, sizeof t);
    wm_set_title(e->win, t);
    e->win->icon = !strcmp(e->path, "::computer") ? ICON_COMPUTER : !strcmp(e->path, "::recycle") ? ICON_RECYCLE_BIN : !strcasecmp(e->path, USER_DOCS) ? ICON_MY_DOCUMENTS : ICON_FOLDER_OPEN;
}
static void navigate(expl_t *e, const char *path, bool push) {
    if (!strcmp(path, "::control")) { control_open(NULL); return; }
    char p[256];
    if (path[0] != ':') {
        vnode_t *n = vfs_resolve(path, NULL);
        if (!n) { char m[300]; snprintf(m, sizeof m, OS_NAME " cannot find '%s'. Check the spelling and try again.", path); msgbox("Address Bar", m, MBI_ERROR, MBX_OK, NULL, NULL); return; }
        if (!n->dir) { shell_open(path, NULL); return; }
        if (str_ends_with_ci(n->name, ".app")) { bundle_open(n); return; }
        vfs_path(n, p, sizeof p);
    } else strlcpy(p, path, sizeof p);
    if (push) {
        if (e->hlen && strcmp(e->hist[e->hpos], p)) { e->hpos++; }
        if (e->hpos >= 16) { memmove(e->hist, e->hist + 1, 15 * 256); e->hpos = 15; }
        strlcpy(e->hist[e->hpos], p, 256); e->hlen = e->hpos + 1;
    }
    strlcpy(e->path, p, sizeof e->path);
    strlcpy(e->addr, !strcmp(p, "::computer") ? "My Computer" : !strcmp(p, "::recycle") ? "Recycle Bin" : p, sizeof e->addr);
    e->sel = -1; e->scroll = 0;
    rebuild(e);
    e->win->dirty = true;
}
static void go_up(expl_t *e) {
    if (is_special(e)) { if (!strcmp(e->path, "::recycle")) navigate(e, "::computer", true); return; }
    vnode_t *n = vfs_resolve(e->path, NULL);
    if (!n || !n->parent) navigate(e, "::computer", true);
    else { char p[256]; vfs_path(n->parent, p, sizeof p); navigate(e, p, true); }
}
static void open_item(expl_t *e, int i) {
    if (i < 0 || i >= e->n) return;
    eitem_t *it = &e->items[i];
    if (!strcmp(e->path, "::recycle")) { properties_open(it->node); return; }
    if (it->path[0] == ':' || (it->node && it->node->dir && !str_ends_with_ci(it->node->name, ".app"))) navigate(e, it->path, true);
    else shell_open(it->path, NULL);
}

/* ---------------- layout ---------------- */
static rect_t content_rect(expl_t *e) {
    canvas_t *c = e->win->surf;
    int px = c->w >= 520 ? PANE_W : 0;
    return R(px, TB_H + AB_H, c->w - px, c->h - TB_H - AB_H - SB_H);
}
static int cols_for(expl_t *e) { rect_t r = content_rect(e); return MAX(1, (r.w - 20) / (e->view == 2 ? 230 : 84)); }
static rect_t item_rect(expl_t *e, int i) {
    rect_t r = content_rect(e);
    if (e->view == 1) return R(r.x + 2, r.y + 22 + i * 20 - e->scroll, r.w - 22, 20);
    int cols = cols_for(e);
    if (e->view == 2) return R(r.x + 10 + (i % cols) * 230, r.y + 10 + (i / cols) * 58 - e->scroll, 222, 52);
    return R(r.x + 8 + (i % cols) * 84, r.y + 8 + (i / cols) * 76 - e->scroll, 80, 72);
}
static int content_height(expl_t *e) {
    if (e->view == 1) return 26 + e->n * 20;
    int cols = cols_for(e), rows = (e->n + cols - 1) / cols;
    return 16 + rows * (e->view == 2 ? 58 : 76);
}
static int item_at(expl_t *e, int x, int y) { for (int i = 0; i < e->n; i++) if (pt_in(item_rect(e, i), x, y)) return i; return -1; }

/* ---------------- task pane ---------------- */
static void pane_box(window_t *w, canvas_t *c, int x, int *y, int width, const char *title, const char **links, const int *icons, int n, int *clicked) {
    int hh = 25;
    if (T.classic) { fill_rect(c, x, *y, width, hh, T.pane_head_top); draw_text(c, FONT_UI_BOLD, x + 8, *y + 6, title, T.pane_head_text); }
    else {
        fill_round_rect(c, x, *y, width, hh, 4, T.pane_head_bot);
        gradient_h(c, x, *y + 3, width, hh - 3, T.pane_head_top, T.pane_head_bot);
        fill_rect(c, x, *y + 3, 2, hh - 3, T.pane_head_top);
        draw_text(c, FONT_UI_BOLD, x + 10, *y + 6, title, T.pane_head_text);
        fill_circle(c, x + width - 13, *y + 12, 8, T.pane_head_top); draw_circle(c, x + width - 13, *y + 12, 8, mix(T.pane_head_bot, T.text, 40));
        draw_arrow(c, x + width - 13, *y + 9, 0, T.pane_head_text); draw_arrow(c, x + width - 13, *y + 13, 0, T.pane_head_text);
    }
    *y += hh;
    int bh = n * 18 + 12;
    fill_rect(c, x, *y, width, bh, T.pane_body);
    if (!T.classic) draw_rect(c, x, *y, width, bh, T.pane_head_top);
    for (int i = 0; i < n; i++) {
        int ly = *y + 7 + i * 18;
        if (icons && icons[i] >= 0) draw_icon(c, icons[i], 16, x + 10, ly - 1);
        if (ui_link(w, x + (icons ? 31 : 12), ly, FONT_UI, links[i], T.pane_link)) *clicked = i;
    }
    *y += bh + 12;
}
static void ren_cb(void *ctx, const char *t) {
    vnode_t *n = ctx;
    if (t && *t && !vfs_rename(n, t)) msgbox("Rename", "Cannot rename: the name is in use or the file is read-only.", MBI_ERROR, MBX_OK, NULL, NULL);
}
static void del_cb(void *ctx, int r) { if (r != IDYES) return; vnode_t *n = ctx; bool inbin = n->parent && !strcmp(n->parent->name, "RECYCLER"); if (inbin ? !vfs_delete(n) : !recycle_move(n)) msgbox("Error Deleting File", "Cannot delete: the item is read-only.", MBI_ERROR, MBX_OK, NULL, NULL); desktop_refresh(); }
static void ask_delete(vnode_t *n) {
    if (!n) return;
    char m[200];
    bool inbin = n->parent && !strcmp(n->parent->name, "RECYCLER");
    snprintf(m, sizeof m, inbin ? "Are you sure you want to permanently delete '%s'?" : "Are you sure you want to send '%s' to the Recycle Bin?", n->name);
    msgbox(n->dir ? "Confirm Folder Delete" : "Confirm File Delete", m, MBI_QUESTION, MBX_YESNO, del_cb, n);
}
static void new_item(expl_t *e, bool folder) {
    vnode_t *d = vfs_resolve(e->path, NULL);
    if (!d || !d->dir || d->readonly) { msgbox("Error", "You cannot create items in this location.", MBI_ERROR, MBX_OK, NULL, NULL); return; }
    char nm[96];
    for (int k = 1; k < 100; k++) {
        if (folder) { if (k == 1) strcpy(nm, "New Folder"); else snprintf(nm, sizeof nm, "New Folder (%d)", k); }
        else { if (k == 1) strcpy(nm, "New Text Document.txt"); else snprintf(nm, sizeof nm, "New Text Document (%d).txt", k); }
        if (!vfs_lookup_child(d, nm)) { vnode_t *n = vfs_new_node(d, nm, folder); input_box("Rename", "Type a name for the new item:", nm, ren_cb, n); break; }
    }
}
static void paste(expl_t *e) {
    vnode_t *d = vfs_resolve(e->path, NULL);
    if (!clip_node || !d || !d->dir || d->readonly) return;
    if (clip_cut) { if (vfs_move(clip_node, d)) clip_node = NULL; }
    else {
        char nm[96]; strlcpy(nm, clip_node->name, sizeof nm);
        if (vfs_lookup_child(d, nm)) snprintf(nm, sizeof nm, "Copy of %s", clip_node->name);
        vfs_copy(clip_node, d, nm);
    }
}
/* Dropped files go into folder dir: on the same drive they move, from another drive (or with
 * Ctrl held) they are copied, as in Windows. "Copy of" when the name is taken. */
bool shell_drop_files(const char *dir, const char *paths, bool copy) {
    vnode_t *d = vfs_resolve(dir, NULL);
    if (!d || !d->dir || d->readonly) { msgbox("Error", "Files cannot be put in this location.", MBI_ERROR, MBX_OK, NULL, NULL); return false; }
    bool any = false;
    for (const char *p = paths; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[512]; snprintf(line, sizeof line, "%.*s", (int)MIN(n, sizeof line - 1), p);
        p += n + (e ? 1 : 0);
        vnode_t *f = line[0] ? vfs_resolve(line, NULL) : NULL;
        if (!f || f == d || f->parent == d) continue;          /* already there */
        bool inside = false;
        for (vnode_t *q = d; q; q = q->parent) if (q == f) inside = true;
        if (inside) { msgbox("Error", "A folder cannot be moved into itself.", MBI_ERROR, MBX_OK, NULL, NULL); continue; }
        bool same = vfs_drive_letter(f) == vfs_drive_letter(d) && !(f->parent && f->parent->readonly);
        if (same && !copy) { if (vfs_move(f, d)) any = true; continue; }
        char nm[96]; strlcpy(nm, f->name, sizeof nm);
        if (vfs_lookup_child(d, nm)) snprintf(nm, sizeof nm, "Copy of %s", f->name);
        if (vfs_copy(f, d, nm)) any = true;
    }
    return any;
}
static bool expl_drop(window_t *w, int x, int y, const char *paths, bool copy) {
    expl_t *e = w->data;
    int i = item_at(e, x, y);
    const char *dir = e->path;
    char p[256];
    if (i >= 0 && e->items[i].node && e->items[i].node->dir) { vfs_path(e->items[i].node, p, sizeof p); dir = p; }
    else if (i >= 0 && e->items[i].path[1] == ':' && !e->items[i].node) dir = e->items[i].path;   /* a drive in My Computer */
    else if (is_special(e)) return false;
    bool ok = shell_drop_files(dir, paths, copy);
    rebuild(e); w->dirty = true;
    return ok;
}
static void ctx_cb(void *ctx, int id) {
    window_t *w = ctx; if (!wm_alive(w)) return;
    expl_t *e = w->data;
    eitem_t *it = e->sel >= 0 && e->sel < e->n ? &e->items[e->sel] : NULL;
    switch (id) {
    case 1: open_item(e, e->sel); break;
    case 2: if (it && it->node) ask_delete(it->node); break;
    case 3: if (it && it->node) input_box("Rename", "Type a new name:", it->node->name, ren_cb, it->node); break;
    case 4: if (it && it->node) properties_open(it->node); else if (it && it->path[1] == ':') properties_open(vfs_root(it->path[0])); break;
    case 5: if (it && it->node) { clip_node = it->node; clip_cut = false; } break;
    case 6: if (it && it->node) { clip_node = it->node; clip_cut = true; } break;
    case 7: paste(e); break;
    case 8: new_item(e, true); break;
    case 9: new_item(e, false); break;
    case 10: e->view = 0; break; case 11: e->view = 1; break; case 12: e->view = 2; break;
    case 13: rebuild(e); break;
    case 14: if (it && it->node) recycle_restore(it->node); break;
    case 15: recycle_empty(); break;
    case 16: if (it && it->node && it->node->dir && !it->node->parent) {
            char l = it->node->drive; int k = vfs_drive_kind(l);
            if (k == 2) { vfs_unmount(l); notify_balloon("Disk image ejected", "The disk image was ejected safely."); }
            else if (k == 3 || k == 4) { extern bool media_eject(char letter); if (media_eject(l)) notify_balloon(k == 3 ? "Eject" : "Safe To Remove Hardware", k == 3 ? "The disc was ejected." : "The device can now be safely removed from the computer."); }
            rebuild(e);
        } break;
    case 17: if (it && it->node) { char dir[256]; vfs_path(it->node->dir ? it->node : it->node->parent, dir, sizeof dir); terminal_open(NULL, dir); } else terminal_open(NULL, e->path); break;
    case 20: about_open(); break;
    case 21: wm_close(w); break;
    case 22: if (it && it->node && !it->node->dir) notepad_open(it->path); break;
    }
    w->dirty = true;
}
static void show_ctx(window_t *w, int sx, int sy) {
    expl_t *e = w->data;
    if (!strcmp(e->path, "::recycle")) {
        static const menu_item_t m[] = { { "Restore", 14, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Properties", 4, 0 }, { "-", 0, 0 }, { "Empty Recycle Bin", 15, 0 } };
        popup_menu(sx, sy, m, 6, ctx_cb, w); return;
    }
    if (e->sel >= 0) {
        eitem_t *it = &e->items[e->sel];
        if (it->node && !it->node->parent) {
            static const menu_item_t m[] = { { "Open", 1, 0 }, { "Open Command Prompt Here", 17, 0 }, { "-", 0, 0 }, { "Eject", 16, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } };
            popup_menu(sx, sy, m, 6, ctx_cb, w); return;
        }
        static menu_item_t m[] = { { "Open", 1, 0 }, { "Edit with Notepad", 22, 0 }, { "-", 0, 0 }, { "Cut", 6, 0 }, { "Copy", 5, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Rename", 3, 0 }, { "-", 0, 0 }, { "Properties", 4, 0 } };
        bool ro = it->node && (it->node->readonly || (it->node->parent && it->node->parent->readonly));
        m[1].flags = it->node && !it->node->dir ? 0 : MI_DISABLED;
        m[3].flags = m[6].flags = m[7].flags = ro || !it->node ? MI_DISABLED : 0;
        popup_menu(sx, sy, m, 10, ctx_cb, w);
    } else {
        static menu_item_t m[] = { { "View: Icons", 10, 0 }, { "View: Tiles", 12, 0 }, { "View: Details", 11, 0 }, { "-", 0, 0 }, { "Refresh", 13, 0 }, { "-", 0, 0 }, { "Paste", 7, 0 }, { "-", 0, 0 }, { "New Folder", 8, 0 }, { "New Text Document", 9, 0 }, { "-", 0, 0 }, { "Open Command Prompt Here", 17, 0 } };
        m[0].flags = e->view == 0 ? MI_CHECKED | MI_RADIO : 0; m[1].flags = e->view == 2 ? MI_CHECKED | MI_RADIO : 0; m[2].flags = e->view == 1 ? MI_CHECKED | MI_RADIO : 0;
        m[6].flags = clip_node ? 0 : MI_DISABLED;
        popup_menu(sx, sy, m, is_special(e) ? 5 : 12, ctx_cb, w);
    }
}

/* ---------------- painting ---------------- */
static void expl_paint(window_t *w) {
    expl_t *e = w->data;
    canvas_t *c = w->surf;
    if (e->gen != vfs_generation) rebuild(e);
    gfx_clear(c, T.window_bg);
    /* toolbar */
    if (T.classic) { fill_rect(c, 0, 0, c->w, TB_H + AB_H, T.face); hline(c, 0, TB_H - 1, c->w, T.gray_text); }
    else { gradient_v(c, 0, 0, c->w, TB_H, T.face_light, T.face); fill_rect(c, 0, TB_H, c->w, AB_H, T.face); hline(c, 0, TB_H - 1, c->w, T.face_dark); }
    if (ui_icon_button(w, R(4, 3, 66, 32), ICON_BACK, "Back") && e->hpos > 0) { e->hpos--; navigate(e, e->hist[e->hpos], false); }
    if (ui_icon_button(w, R(72, 3, 32, 32), ICON_FORWARD, NULL) && e->hpos < e->hlen - 1) { e->hpos++; navigate(e, e->hist[e->hpos], false); }
    if (ui_icon_button(w, R(106, 3, 32, 32), ICON_UP, NULL)) go_up(e);
    vline(c, 142, 6, 26, T.face_dark);
    if (ui_icon_button(w, R(146, 3, 76, 32), ICON_SEARCH, "Search")) terminal_open(NULL, is_special(e) ? "C:\\" : e->path);
    if (ui_icon_button(w, R(224, 3, 78, 32), ICON_FOLDER, "Folders")) navigate(e, "::computer", true);
    vline(c, 306, 6, 26, T.face_dark);
    static const char *views[] = { "Icons", "Details", "Tiles" };
    int vi = e->view;
    if (ui_dropdown(w, R(312, 9, 84, 21), views, 3, &vi)) {}
    if (vi != e->view) e->view = vi;
    /* address bar */
    draw_text(c, FONT_UI, 8, TB_H + 6, "Address", T.text);
    bool go = ui_textbox(w, R(58, TB_H + 2, c->w - 112, 21), e->addr, sizeof e->addr);
    if (ui_icon_button(w, R(c->w - 50, TB_H + 2, 44, 21), ICON_GO, "Go") || go) { char a[256]; strcpy(a, e->addr); if (!strcasecmp(a, "My Computer")) navigate(e, "::computer", true); else if (!strcasecmp(a, "Recycle Bin")) navigate(e, "::recycle", true); else navigate(e, a, true); return; }
    hline(c, 0, TB_H + AB_H - 1, c->w, T.face_dark);
    rect_t cr = content_rect(e);
    /* task pane */
    if (cr.x > 0) {
        int px = 0, py = TB_H + AB_H, ph = cr.h;
        rect_t old = canvas_push_clip(c, R(px, py, PANE_W, ph));
        gradient_v(c, px, py, PANE_W, ph, T.pane_top, T.pane_bot);
        int y = py + 12, bx = 12, bw = PANE_W - 24, clicked = -1;
        eitem_t *it = e->sel >= 0 && e->sel < e->n ? &e->items[e->sel] : NULL;
        if (!strcmp(e->path, "::recycle")) {
            const char *l[] = { "Empty the Recycle Bin", "Restore this item" }; const int ic[] = { ICON_RECYCLE_BIN, ICON_BACK };
            pane_box(w, c, bx, &y, bw, "Recycle Bin Tasks", l, ic, it ? 2 : 1, &clicked);
            if (clicked == 0) recycle_empty(); else if (clicked == 1 && it) recycle_restore(it->node);
        } else if (!strcmp(e->path, "::computer")) {
            const char *l[] = { "View system information", "Change a setting", "Open Command Prompt" }; const int ic[] = { ICON_CP_SYSTEM, ICON_CONTROL_PANEL, ICON_TERMINAL };
            pane_box(w, c, bx, &y, bw, "System Tasks", l, ic, 3, &clicked);
            if (clicked == 0) control_open("system"); else if (clicked == 1) control_open(NULL); else if (clicked == 2) terminal_open(NULL, "C:\\");
        } else {
            const char *l[] = { "Make a new folder", "Create a text file", "Delete this item", "Rename this item", "Copy this item", "Command Prompt here" };
            const int ic[] = { ICON_FOLDER, ICON_TEXT_FILE, ICON_ERROR, ICON_NOTEPAD, ICON_FILE, ICON_TERMINAL };
            const char *l2[6]; int ic2[6], map[6], k = 0;
            for (int i = 0; i < 6; i++) { if ((i >= 2 && i <= 4) && !it) continue; l2[k] = l[i]; ic2[k] = ic[i]; map[k] = i; k++; }
            pane_box(w, c, bx, &y, bw, "File and Folder Tasks", l2, ic2, k, &clicked);
            if (clicked >= 0) {
                switch (map[clicked]) {
                case 0: new_item(e, true); break; case 1: new_item(e, false); break;
                case 2: if (it) ask_delete(it->node); break;
                case 3: if (it && it->node) input_box("Rename", "Type a new name:", it->node->name, ren_cb, it->node); break;
                case 4: if (it && it->node) { clip_node = it->node; clip_cut = false; notify_balloon("Copied", "Right-click in a folder and choose Paste."); } break;
                case 5: terminal_open(NULL, e->path); break;
                }
            }
        }
        clicked = -1;
        const char *op[] = { "My Computer", "My Documents", "Desktop", "Control Panel", "Recycle Bin" };
        const int oi[] = { ICON_COMPUTER, ICON_MY_DOCUMENTS, ICON_DESKTOP, ICON_CONTROL_PANEL, ICON_RECYCLE_BIN };
        pane_box(w, c, bx, &y, bw, "Other Places", op, oi, 5, &clicked);
        if (clicked == 0) navigate(e, "::computer", true); else if (clicked == 1) navigate(e, USER_DOCS, true);
        else if (clicked == 2) navigate(e, USER_DESKTOP, true); else if (clicked == 3) control_open(NULL); else if (clicked == 4) navigate(e, "::recycle", true);
        /* details box */
        if (y + 80 < py + ph) {
            int hh = 25;
            if (T.classic) fill_rect(c, bx, y, bw, hh, T.pane_head_top);
            else { fill_round_rect(c, bx, y, bw, hh, 4, T.pane_head_bot); gradient_h(c, bx, y + 3, bw, hh - 3, T.pane_head_top, T.pane_head_bot); }
            draw_text(c, FONT_UI_BOLD, bx + 10, y + 6, "Details", T.pane_head_text);
            y += hh;
            fill_rect(c, bx, y, bw, 76, T.pane_body);
            char t[128], s2[64];
            if (it) {
                draw_text_ellipsis(c, FONT_UI_BOLD, bx + 10, y + 8, bw - 20, it->name, T.text);
                draw_text(c, FONT_UI, bx + 10, y + 26, it->type, T.text);
                if (it->node && !it->node->dir) { format_size(s2, sizeof s2, it->size); snprintf(t, sizeof t, "Size: %s", s2); draw_text(c, FONT_UI, bx + 10, y + 42, t, T.text); }
                else if (it->node) { snprintf(t, sizeof t, "%d items", vfs_count_children(it->node)); draw_text(c, FONT_UI, bx + 10, y + 42, t, T.text); }
                if (it->node) { snprintf(t, sizeof t, "Modified: %02d/%02d/%04d", it->node->mtime.month, it->node->mtime.day, it->node->mtime.year); draw_text(c, FONT_UI, bx + 10, y + 58, t, T.text); }
            } else { title_for(e, t, sizeof t); draw_text_ellipsis(c, FONT_UI_BOLD, bx + 10, y + 8, bw - 20, t, T.text); snprintf(t, sizeof t, "%d objects", e->n); draw_text(c, FONT_UI, bx + 10, y + 26, t, T.text); }
        }
        c->clip = old;
    }
    /* content */
    int ch = content_height(e);
    int maxs = MAX(0, ch - cr.h);
    e->scroll = CLAMP(e->scroll, 0, maxs);
    rect_t old = canvas_push_clip(c, cr);
    if (e->view == 1) {
        int cx = cr.x;
        const char *hdr[] = { "Name", "Size", "Type", "Date Modified" }; int cw[] = { MAX(160, cr.w - 420), 80, 150, 170 };
        for (int k = 0; k < 4; k++) {
            if (T.classic) { fill_rect(c, cx, cr.y, cw[k], 20, T.face); bevel(c, cx, cr.y, cw[k], 20, true); }
            else { gradient_v(c, cx, cr.y, cw[k], 20, T.face_light, T.face); vline(c, cx + cw[k] - 1, cr.y + 3, 14, T.face_dark); hline(c, cx, cr.y + 19, cw[k], T.face_dark); }
            draw_text(c, FONT_UI, cx + 6, cr.y + 3, hdr[k], T.text); cx += cw[k];
        }
        rect_t o2 = canvas_push_clip(c, R(cr.x, cr.y + 20, cr.w, cr.h - 20));
        for (int i = 0; i < e->n; i++) {
            rect_t r = item_rect(e, i);
            if (r.y + r.h < cr.y || r.y > cr.y + cr.h) continue;
            eitem_t *it = &e->items[i];
            bool sel = i == e->sel;
            if (sel) fill_rect(c, r.x, r.y, cw[0] - 4, r.h, T.select);
            draw_icon(c, it->icon, 16, r.x + 4, r.y + 2);
            draw_text_ellipsis(c, FONT_UI, r.x + 24, r.y + 3, cw[0] - 30, it->name, sel ? T.select_text : T.text);
            char s2[32] = "";
            if (it->node && !it->node->dir) { if (it->size < 1024) snprintf(s2, sizeof s2, "%u B", it->size); else snprintf(s2, sizeof s2, "%u KB", (it->size + 1023) / 1024); }
            draw_text(c, FONT_UI, cr.x + cw[0] + cw[1] - 8 - text_width(FONT_UI, s2), r.y + 3, s2, T.text);
            draw_text_ellipsis(c, FONT_UI, cr.x + cw[0] + cw[1] + 6, r.y + 3, cw[2] - 10, it->type, T.text);
            if (it->node) { char t[40]; snprintf(t, sizeof t, "%02d/%02d/%04d %02d:%02d", it->node->mtime.month, it->node->mtime.day, it->node->mtime.year, it->node->mtime.hour, it->node->mtime.minute); draw_text(c, FONT_UI, cr.x + cw[0] + cw[1] + cw[2] + 6, r.y + 3, t, T.text); }
        }
        c->clip = o2;
    } else {
        int last_group = -1;
        for (int i = 0; i < e->n; i++) {
            rect_t r = item_rect(e, i);
            eitem_t *it = &e->items[i];
            if (!strcmp(e->path, "::computer") && it->group != last_group) { last_group = it->group; }
            if (r.y + r.h < cr.y || r.y > cr.y + cr.h) continue;
            bool sel = i == e->sel;
            if (e->view == 2) {
                if (sel) { fill_rect(c, r.x, r.y, r.w, r.h, mix(T.window_bg, T.select, dark_mode ? 100 : 50)); draw_rect(c, r.x, r.y, r.w, r.h, T.select); }
                if (sel) draw_icon_tinted(c, it->icon, 48, r.x + 2, r.y + 2, T.select, 80); else draw_icon(c, it->icon, 48, r.x + 2, r.y + 2);
                draw_text_ellipsis(c, FONT_UI, r.x + 56, r.y + 8, r.w - 60, it->name, T.text);
                draw_text_ellipsis(c, FONT_UI, r.x + 56, r.y + 23, r.w - 60, it->type, T.gray_text);
                if (it->node && !it->node->dir) { char s2[32]; format_size(s2, sizeof s2, it->size); draw_text(c, FONT_UI, r.x + 56, r.y + 37, s2, T.gray_text); }
                continue;
            }
            int ix = r.x + (r.w - 32) / 2;
            if (sel) draw_icon_tinted(c, it->icon, 32, ix, r.y + 2, T.select, 110); else draw_icon(c, it->icon, 32, ix, r.y + 2);
            if (clip_cut && clip_node == it->node) blend_rect(c, ix, r.y + 2, 32, 32, T.face_light, 120);
            int lines_y = r.y + 38;
            char l1[64], l2[64] = ""; const char *s = it->name;
            int n = text_fit_chars(FONT_UI, s, r.w - 4);
            if (n < (int)strlen(s)) { int b = n; for (int k = n; k > 0; k--) if (s[k] == ' ') { b = k; break; } strlcpy(l1, s, MIN(b + 1, 64)); const char *rest = s + b; while (*rest == ' ') rest++; strlcpy(l2, rest, 64); }
            else strlcpy(l1, s, 64);
            for (int L = 0; L < 2; L++) {
                const char *t = L ? l2 : l1; if (!*t) break;
                int tw = MIN(text_width(FONT_UI, t), r.w - 4), tx = r.x + (r.w - tw) / 2, ty = lines_y + L * 13;
                if (sel) fill_rect(c, tx - 2, ty - 1, tw + 4, 14, T.select);
                draw_text_ellipsis(c, FONT_UI, tx, ty, r.w - 4, t, sel ? T.select_text : T.text);
            }
        }
    }
    if (!e->n) draw_text(c, FONT_UI, cr.x + 20, cr.y + (e->view == 1 ? 30 : 16), !strcmp(e->path, "::recycle") ? "The Recycle Bin is empty." : "This folder is empty.", T.gray_text);
    c->clip = old;
    if (maxs > 0) draw_scrollbar(c, R(cr.x + cr.w - 17, cr.y, 17, cr.h), ch, cr.h, e->scroll, true);
    /* status bar */
    int sy = c->h - SB_H;
    fill_rect(c, 0, sy, c->w, SB_H, T.face);
    if (T.classic) bevel(c, 2, sy + 2, c->w - 4, SB_H - 3, false); else hline(c, 0, sy, c->w, T.face_dark);
    char st[128];
    if (e->sel >= 0 && e->sel < e->n && e->items[e->sel].node && !e->items[e->sel].node->dir) { char s2[32]; format_size(s2, sizeof s2, e->items[e->sel].size); snprintf(st, sizeof st, "Type: %s   Size: %s", e->items[e->sel].type, s2); }
    else snprintf(st, sizeof st, "%d object%s", e->n, e->n == 1 ? "" : "s");
    draw_text(c, FONT_UI, 8, sy + 4, st, T.text);
    if (!is_special(e)) { vnode_t *n = vfs_resolve(e->path, NULL); if (n) { char d[48]; int k = vfs_drive_kind(vfs_drive_letter(n)); snprintf(d, sizeof d, "%c: %s", vfs_drive_letter(n), k == 2 ? "Disk image (read-only)" : k == 3 ? "CD Drive (read-only)" : k == 4 ? "Removable Disk (read-only)" : "Local Disk"); draw_text(c, FONT_UI, c->w - text_width(FONT_UI, d) - 12, sy + 4, d, T.text); } }
}
static void expl_mouse(window_t *w, int type, int x, int y, int btn) {
    expl_t *e = w->data;
    rect_t cr = content_rect(e);
    if (type == ME_WHEEL && pt_in(cr, x, y)) { e->scroll += btn * 40; w->dirty = true; return; }
    if (!pt_in(cr, x, y)) return;
    int maxs = MAX(0, content_height(e) - cr.h);
    if (type == ME_DOWN && maxs > 0 && x >= cr.x + cr.w - 17) {
        if (y < cr.y + 17) e->scroll -= 40; else if (y > cr.y + cr.h - 17) e->scroll += 40;
        else e->scroll = (y - cr.y - 17) * maxs / MAX(1, cr.h - 34);
        w->dirty = true; return;
    }
    if (type == ME_UP) e->press_i = -1;
    if (type == ME_MOVE && e->press_i >= 0 && e->press_i < e->n && (ABSDIFF(x, e->press_x) > 5 || ABSDIFF(y, e->press_y) > 5)) {
        eitem_t *it = &e->items[e->press_i];
        e->press_i = -1;
        if (it->node) { char p[256]; vfs_path(it->node, p, sizeof p); wm_drag_files(p, it->icon); }
        return;
    }
    if (type == ME_DOWN) {
        int i = item_at(e, x, y);
        e->sel = i;
        e->press_i = (btn & MB_LEFT) ? i : -1; e->press_x = x; e->press_y = y;
        if (btn & MB_RIGHT) { rect_t c = wm_client_rect(w); show_ctx(w, c.x + x, c.y + y); }
        w->dirty = true;
    } else if (type == ME_DBLCLICK) { int i = item_at(e, x, y); if (i >= 0) open_item(e, i); }
}
static void expl_key(window_t *w, key_event_t *k) {
    expl_t *e = w->data;
    if (!k->pressed || w->ui.focus) return;
    int cols = e->view == 1 ? 1 : cols_for(e);
    switch (k->key) {
    case KEY_RIGHT: if (e->sel < e->n - 1) e->sel++; break;
    case KEY_LEFT: if (e->sel > 0) e->sel--; break;
    case KEY_DOWN: e->sel = e->sel < 0 ? 0 : MIN(e->n - 1, e->sel + cols); break;
    case KEY_UP: e->sel = MAX(0, e->sel - cols); break;
    case KEY_ENTER: open_item(e, e->sel); break;
    case KEY_BACKSPACE: go_up(e); break;
    case KEY_DELETE: if (e->sel >= 0 && e->items[e->sel].node) ask_delete(e->items[e->sel].node); break;
    case KEY_F2: ctx_cb(w, 3); break;
    case KEY_F5: rebuild(e); break;
    case KEY_MENU:                                           /* the context-menu key (Shift+F10 below) */
    menu_key: {
        rect_t cr = wm_client_rect(w);
        rect_t r = e->sel >= 0 ? item_rect(e, e->sel) : R(cr.w / 2, cr.h / 2, 0, 0);
        show_ctx(w, cr.x + r.x + r.w / 2, cr.y + r.y + r.h / 2);
        break;
    }
    case KEY_F10: if (k->mods & MOD_SHIFT) goto menu_key; break;
    default:
        if (k->mods & MOD_CTRL) { if (k->key == 'c') ctx_cb(w, 5); else if (k->key == 'x') ctx_cb(w, 6); else if (k->key == 'v') ctx_cb(w, 7); }
    }
    w->dirty = true;
}
static void expl_menu(window_t *w, int id) { if (id > -1000) ctx_cb(w, id); }
static void expl_destroy(window_t *w) { expl_t *e = w->data; kfree(e->items); kfree(e); }
static const menu_t expl_menus[] = {
    { "File", { { "New Folder", 8, 0 }, { "New Text Document", 9, 0 }, { "-", 0, 0 }, { "Delete", 2, 0 }, { "Rename", 3, 0 }, { "Properties", 4, 0 }, { "-", 0, 0 }, { "Close", 21, 0 }, { NULL } } },
    { "Edit", { { "Cut\tCtrl+X", 6, 0 }, { "Copy\tCtrl+C", 5, 0 }, { "Paste\tCtrl+V", 7, 0 }, { NULL } } },
    { "View", { { "Icons", 10, 0 }, { "Tiles", 12, 0 }, { "Details", 11, 0 }, { "-", 0, 0 }, { "Refresh\tF5", 13, 0 }, { NULL } } },
    { "Tools", { { "Open Command Prompt Here", 17, 0 }, { NULL } } },
    { "Help", { { "About " OS_NAME, 20, 0 }, { NULL } } },
};
void explorer_open(const char *path) {
    window_t *w = wm_create("Explorer", -1, -1, 720, 480, WF_RESIZABLE);
    expl_t *e = kzalloc(sizeof *e);
    e->win = w; e->sel = -1; e->view = 0; e->press_i = -1;
    w->drop = expl_drop;
    w->data = e; w->paint = expl_paint; w->mouse = expl_mouse; w->key = expl_key; w->destroy = expl_destroy; w->menu_cmd = expl_menu;
    w->min_w = 360; w->min_h = 260;
    w->menus = kmalloc(sizeof expl_menus); memcpy(w->menus, expl_menus, sizeof expl_menus); w->nmenus = ARRAY_LEN(expl_menus);
    rect_t cr = wm_client_rect(w); wm_resize_client(w, cr.w, cr.h);
    navigate(e, path && *path ? path : USER_DOCS, true);
}

/* ---------------- .app bundles ---------------- */
void bundle_open(vnode_t *app) {
    char base[256]; vfs_path(app, base, sizeof base);
    char p[320]; snprintf(p, sizeof p, "%s\\Contents\\Info.plist", base);
    vnode_t *pl = vfs_resolve(p, NULL);
    char exe[96] = "";
    if (pl && pl->size) {
        char *s = strndup((char *)pl->data, pl->size);
        char *k = strstr(s, "<key>CFBundleExecutable</key>");
        if (k) { char *a = strstr(k, "<string>"), *b = a ? strstr(a, "</string>") : NULL; if (a && b) { a += 8; strlcpy(exe, a, MIN((int)(b - a) + 1, 96)); } }
        kfree(s);
    }
    if (!exe[0]) { strlcpy(exe, app->name, sizeof exe); char *d = strrchr(exe, '.'); if (d) *d = 0; }
    snprintf(p, sizeof p, "%s\\Contents\\MacOS\\%s", base, exe);
    vnode_t *x = vfs_resolve(p, NULL);
    if (!x || x->dir) { char m[400]; snprintf(m, sizeof m, "The application bundle '%s' has no executable (Contents\\MacOS\\%s).", app->name, exe); msgbox(app->name, m, MBI_ERROR, MBX_OK, NULL, NULL); return; }
    char desc[128];
    if (macho_probe(x->data, x->size, desc, sizeof desc)) {
        char m[400]; snprintf(m, sizeof m, "'%s' contains a native macOS program (%s).\n\nNative Mach-O binaries cannot run on " OS_NAME "; script-based bundles can.", app->name, desc);
        msgbox(app->name, m, MBI_ERROR, MBX_OK, NULL, NULL); return;
    }
    if (x->size >= 2 && x->data[0] == 'M' && x->data[1] == 'Z') { shell_open(p, NULL); return; }
    char dir[320]; snprintf(dir, sizeof dir, "%s\\Contents\\MacOS", base);
    terminal_open(p, dir);
}
