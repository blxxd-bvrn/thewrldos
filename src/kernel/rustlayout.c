/* The layout of vnode_t (src/include/fs.h) for the Rust copy of it in rust/src/kernel.rs,
 * which checks it at boot. The VFS itself is rust/src/kernel/vfsk.rs. */
#include "kernel.h"
#include "fs.h"

const uint32_t rust_vnode_layout[15] = {
    sizeof(vnode_t), offsetof(vnode_t, name), offsetof(vnode_t, dir), offsetof(vnode_t, readonly), offsetof(vnode_t, data),
    offsetof(vnode_t, size), offsetof(vnode_t, cap), offsetof(vnode_t, parent), offsetof(vnode_t, child), offsetof(vnode_t, next),
    offsetof(vnode_t, mtime), offsetof(vnode_t, drive), offsetof(vnode_t, dirty), offsetof(vnode_t, lazy), offsetof(vnode_t, lazy_loc),
};

#ifdef __x86_64__
/* The Linux layer (rust/src/kernel/lx) mirrors these. */
#include "apps.h"
#include "wm.h"
const uint32_t rust_lx_layout[16] = {
    sizeof(regs_t), offsetof(regs_t, eip), sizeof(task_t), offsetof(task_t, cr3), offsetof(task_t, cpu_ms),
    offsetof(task_t, console), offsetof(task_t, kind), offsetof(task_t, lx), offsetof(task_t, next),
    sizeof(console_t), sizeof(rtc_time_t),
    offsetof(window_t, surf), offsetof(window_t, paint), offsetof(window_t, data), sizeof(canvas_t), sizeof(key_event_t),
};
#endif
