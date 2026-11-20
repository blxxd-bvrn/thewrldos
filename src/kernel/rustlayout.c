/* The layout of vnode_t (src/include/fs.h) for the Rust copy of it in rust/src/kernel.rs,
 * which checks it at boot. The VFS itself is rust/src/kernel/vfsk.rs. */
#include "kernel.h"
#include "fs.h"

const uint32_t rust_vnode_layout[13] = {
    sizeof(vnode_t), offsetof(vnode_t, name), offsetof(vnode_t, dir), offsetof(vnode_t, readonly), offsetof(vnode_t, data),
    offsetof(vnode_t, size), offsetof(vnode_t, cap), offsetof(vnode_t, parent), offsetof(vnode_t, child), offsetof(vnode_t, next),
    offsetof(vnode_t, mtime), offsetof(vnode_t, drive), offsetof(vnode_t, dirty),
};
