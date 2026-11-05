/* Weak fallbacks; real implementations override these. */
#include "kernel.h"
#include "wm.h"
#include "apps.h"
#define W __attribute__((weak))
W void explorer_open(const char *p) { (void)p; msgbox("Explorer", "Not available", MBI_INFO, MBX_OK, NULL, NULL); }
W void notepad_open(const char *p) { (void)p; }
W void terminal_open(const char *s, const char *c) { (void)s; (void)c; }
W void calc_open(void) {}
W void paint_open(const char *p) { (void)p; }
W void taskmgr_open(void) {}
W void control_open(const char *p) { (void)p; }
W void about_open(void) {}
W void snake_open(void) {}
W void imageview_open(const char *p) { (void)p; }
W void properties_open(vnode_t *n) { (void)n; }
W void bundle_open(vnode_t *n) { (void)n; }
W bool recycle_move(vnode_t *n) { return vfs_delete(n); }
W void recycle_empty(void) {}
W int pe_spawn(const char *p, const char *a, console_t *c, char *e, int ec) { (void)p; (void)a; (void)c; snprintf(e, ec, "PE loader not built"); return -1; }
W void w32_init(void) {}
W void pe_task_cleanup(task_t *t) { (void)t; }
