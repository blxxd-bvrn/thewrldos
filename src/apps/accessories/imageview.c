/* Image Viewer */
#include "kernel.h"
#include "gfx.h"
#include "theme.h"
#include "wm.h"
#include "apps.h"
#include "disk.h"


/* ======================= Image Viewer ======================= */
typedef struct { canvas_t *img; char name[96]; } iv_t;
static void iv_paint(window_t *w) {
    iv_t *v = w->data; canvas_t *c = w->surf;
    gfx_clear(c, T.face);
    if (!v->img) { draw_text(c, FONT_UI, 20, 20, "Unable to display this picture.", T.text); return; }
    int iw = v->img->w, ih = v->img->h, aw = c->w - 20, ah = c->h - 50;
    int sw = iw, sh = ih;
    if (sw > aw) { sh = sh * aw / sw; sw = aw; } if (sh > ah) { sw = sw * ah / sh; sh = ah; }
    int ox = (c->w - sw) / 2, oy = (c->h - 40 - sh) / 2;
    blend_rect(c, ox + 4, oy + 4, sw, sh, BLACK, 40);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        uint32_t p = v->img->px[(y * ih / sh) * v->img->stride + x * iw / sw]; int a = p >> 24;
        uint32_t chk = ((x / 8 + y / 8) & 1) ? HEX(0xE0E0E0) : WHITE;      /* checkerboard behind transparency */
        put_pixel(c, ox + x, oy + y, a == 255 ? p : mix(chk, p | 0xFF000000, a));
    }
    if (ui_button(w, R(c->w / 2 - 110, c->h - 34, 100, 26), "Edit in Paint")) { char p[300]; snprintf(p, sizeof p, "%s", v->name); paint_open(p); }
    if (ui_button(w, R(c->w / 2 + 10, c->h - 34, 100, 26), "Close")) wm_destroy(w);
}
static void iv_destroy(window_t *w) { iv_t *v = w->data; canvas_free(v->img); kfree(v); }
void imageview_open(const char *path) {
    if (path && *path && vfs_resolve(path, NULL)) { char full_[256]; vfs_path(vfs_resolve(path, NULL), full_, sizeof full_); recent_add(full_); }   /* for The WRLD Search */
    vnode_t *n = vfs_resolve(path, NULL);
    iv_t *v = kzalloc(sizeof *v);
    if (n) { char err[96]; v->img = image_decode(n->data, n->size, err, sizeof err); vfs_path(n, v->name, sizeof v->name); }
    char t[160]; snprintf(t, sizeof t, "%s - Image Viewer", n ? n->name : "Image");
    window_t *w = wm_create(t, -1, -1, 560, 460, WF_RESIZABLE);
    w->data = v; w->paint = iv_paint; w->destroy = iv_destroy; w->icon = ICON_IMAGE_FILE;
}
