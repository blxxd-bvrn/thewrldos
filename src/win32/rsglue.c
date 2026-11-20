/* Small accessors for the Win32 code in Rust (rust/src/kernel/w32.rs and the modules that
 * use it), so that Rust does not mirror w32_proc_t, whose layout differs per architecture. */
#include "w32.h"
#include "theme.h"
#include "gfx.h"

void w32_set_last_error(uint32_t e) { w32_proc_t *p = w32_cur(); if (p) p->last_error = e; }

/* the process environment (k32more's functions, now rust/src/kernel/w32k32.rs) */
const char *w32_env_lookup(const char *n) {
    w32_proc_t *p = w32_cur(); if (!p || !n) return NULL;
    size_t l = strlen(n);
    for (int i = 0; p->envp && p->envp[i]; i++) if (!strncasecmp(p->envp[i], n, l) && p->envp[i][l] == '=') return p->envp[i] + l + 1;
    return NULL;
}
void w32_env_store(const char *name, const char *val) {
    w32_proc_t *p = w32_cur(); if (!p) return;
    int n = 0; while (p->envp && p->envp[n]) n++;
    size_t nl = strlen(name);
    for (int i = 0; i < n; i++) if (!strncasecmp(p->envp[i], name, nl) && p->envp[i][nl] == '=') {
        if (!val) { for (int k = i; k < n; k++) p->envp[k] = p->envp[k + 1]; }
        else { char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); p->envp[i] = e; }
        p->wenvp = NULL; return;
    }
    if (!val) return;
    char **ne = kzalloc((n + 2) * sizeof(char *)); if (n) memcpy(ne, p->envp, n * sizeof(char *));
    char *e = kmalloc(nl + strlen(val) + 2); sprintf(e, "%s=%s", name, val); ne[n] = e;
    kfree(p->envp); p->envp = ne; p->wenvp = NULL;
}
const char *w32_env_at(int i) { w32_proc_t *p = w32_cur(); return p && p->envp ? p->envp[i] : NULL; }   /* NULL after the last */

/* the calling process */
const char *w32_proc_cwd(void) { w32_proc_t *p = w32_cur(); return p ? p->cwd : "C:\\"; }
const char *w32_proc_exe(void) { w32_proc_t *p = w32_cur(); return p ? p->path : ""; }
int w32_proc_pid(void) { w32_proc_t *p = w32_cur(); return p ? p->pid : 0; }
const char *w32_pid_exe(int pid) { w32_proc_t *p = pe_find(pid); return p ? p->path : NULL; }
bool w32_mod_info(int i, const char **name, const char **path, uintptr_t *base, uint32_t *size) {
    w32_proc_t *p = w32_cur(); if (!p || i < 0 || i >= p->nmods) return false;
    w32_mod_t *m = p->mods[i]; *name = m->name; *path = m->path; *base = m->base; *size = m->size; return true;
}
int w32_console_key(void) { w32_proc_t *p = w32_cur(); return p && p->in ? p->in->getkey(p->in) : -1; }
uintptr_t w32_file_node(uintptr_t h) { return (uintptr_t)w32_handle_node(w32_cur(), h); }
void w32_fatal_app_exit(const char *m) {
    w32_proc_t *p = w32_cur();
    msgbox(path_basename(p ? p->path : "?"), m ? m : "Fatal application exit", MBI_ERROR, MBX_OK, NULL, NULL);
    w32_exit_process(255);
}

/* shell (rust/src/kernel/w32shell.rs) */
const char *w32_mod_path_by_name(const char *name) { w32_mod_t *m = w32_mod_by_name(w32_cur(), name); return m ? m->path : NULL; }
void w32_taskbar_rect(int32_t *r) { r[0] = 0; r[1] = SCREEN_H - T.taskbar_h; r[2] = SCREEN_W; r[3] = SCREEN_H; }
