# Roadmap: from Linux programs to a browser engine

The 64-bit edition runs unmodified x86-64 Linux programs in ring 3. This file lists what is
still missing between that and running WPE WebKit as The WRLD Xplorer, in the order the pieces
have to be built.

## Where the port stands

Already working, and not on the list below:

- Ring-3 x86-64 execution, with the timer preempting user code.
- Position-independent ELF loading (executable plus the `ld-linux` it names).
- glibc dynamic linking (`libc.so.6`), plus static-PIE programs.
- `mmap`/`munmap`/`mprotect`/`brk`/`mremap`, demand paging, per-process page tables.
- `clone()` as glibc's `pthread_create` uses it, futexes, mutexes, condition variables,
  `pthread_join`.
- SSE/SSE2 and x87 for user code.
- A few hundred file, memory, time, identity and threading system calls.

## What is missing

### 1. Process model

- No `fork`/`vfork`/`execve`: `lx_spawn()` loads one program from the shell, and there is no way
  to replace a running image.
- No `wait4`/`waitid` and no parent/child bookkeeping; `getppid` returns a constant.
- Needed by shell pipelines, `system()`, `posix_spawn`, and WebKit's process launcher.

### 2. Signals

- `rt_sigaction` is remembered but never delivered; `rt_sigreturn` is a no-op and
  `sigsuspend` returns `EINTR` unconditionally.
- Missing: signal frames and alternate stacks, `SA_RESTART`, `SIGCHLD`, `SIGPIPE`, handler
  dispatch for `SIGSEGV`, and `pthread_kill`.
- Blocking: glibc start-up paths, crash reporters, and any JIT that uses a guard page.

### 3. IPC and the event loop

- No `pipe`/`pipe2`, no `socketpair`, no `AF_UNIX`, no `SCM_RIGHTS` descriptor passing.
- No `epoll`, `eventfd`, `timerfd`, `inotify`, `signalfd`, `memfd_create`, or shared memory.
- `poll()` answers "always ready", so it cannot drive an event loop.
- Blocking: WebKit's architecture is `AF_UNIX` plus `epoll` plus descriptor passing between its
  UI, Web and Network processes.

### 4. Sockets

- No socket system calls at all. The TCP/UDP/DNS stack in `src/net` is reachable only from the
  kernel-side browser.
- Missing: `socket`/`bind`/`connect`/`listen`/`accept`/`send`/`recv`/`getsockopt`/`setsockopt`,
  non-blocking mode, `getaddrinfo`, `SO_REUSEADDR`.

### 5. Memory

- `uvm` copies file data on first touch, so `MAP_SHARED` behaves like a private snapshot: no
  write-back and no sharing between processes.
- A fixed 8 MiB stack with no growth and no guard page.
- No swap and no reclaim, and about 3.75 GB of RAM at most.

### 6. Kernel and scheduler

- The kernel is cooperative, so one long system call (a disk poll, a TLS handshake, populating a
  large mapping) stalls every thread. Only ring 3 is preemptive.
- One CPU: `sched_setaffinity` is a no-op and the affinity mask reports a single CPU.
- `fcntl` locks are always granted and `fsync` does nothing, which is fine for the in-memory VFS
  but unsafe for SQLite.

### 7. C and C++ runtime

- glibc is copied from the build machine rather than built for this ABI.
- No musl build yet; static-PIE would remove the `ld-linux` dependency.
- No C++ runtime: libstdc++ needs unwinding (`_Unwind_*`), `__cxa_*` and thread-safe statics.
- No `getaddrinfo`/`getpwnam` beyond `/etc/passwd`, no `iconv`, no locale past Latin-1.

### 8. Graphics and text

- No FreeType, HarfBuzz, ICU, Skia or Cairo. The kernel-side GUI has its own small TrueType
  engine (`src/gui/ttf.c`, faces in `C:\WRLD\Fonts`) and draws UTF-8, but it does not hint,
  shape (Arabic and Indic joining) or do bidirectional layout; Linux programs have no font
  stack at all.
- Fonts need large file mappings; shaping needs HarfBuzz plus the ICU data (about 30 MB).
- No GPU or 3D, so Skia has to use its software backend.
- Missing codecs: WebP, SVG, progressive JPEG; audio/video beyond WAV.

### 9. Services a browser engine expects

- `/proc` has only `/proc/self/exe`; it needs `/proc/self/{maps,stat,status,fd}`, `/proc/meminfo`
  and `/proc/cpuinfo`.
- No `/sys`, no enforced `rlimit`s.
- No seccomp-bpf, namespaces or `CLONE_NEWUSER`; `prctl` returns success, so `NO_NEW_PRIVS` is
  dishonest and the sandbox has to be disabled rather than faked.

### 10. Practical ceilings

- 512 MB minimum and about 3.75 GB maximum, 64-bit edition only; WebKit wants 1-2 GB per
  process.
- The installed partition is loaded into RAM at boot, so its size is bounded by RAM.
- No USB and no Wi-Fi; wired IPv4 Ethernet only.
- An unimplemented system call returns `ENOSYS`, is logged once, and the program dies.
- The Linux ABI is covered by two test programs, `samples/linux/hello.c` and `threads.c`.

## Milestones

1. [x] **`fork`, `execve`, `wait4`** - process creation and replacement.
2. [x] **`pipe`, `AF_UNIX`, `SCM_RIGHTS`** - the IPC substrate.
3. [x] **`epoll`, `eventfd`, `timerfd`** - the event loop.
4. [ ] **Socket system calls** over the existing TCP stack.
5. [ ] **Signal delivery** - before anything JITs.
6. [ ] **musl static-PIE** - stop shipping the build machine's glibc.
7. [ ] **FreeType, HarfBuzz, ICU** - text and shaping.
8. [ ] **Skia (software backend)** - rasterisation.
9. [ ] **libjpeg/libpng and SQLite** - media and storage.
10. [ ] **`/proc`, `/sys`, honest `prctl`** - introspection and sandboxing.

Milestones 1-3 are implemented in `src/linux/proc.c` and `src/linux/ipc.c`.

The shell and XplorerLite no longer wait on milestone 7 for their own text: the GUI reads
TrueType directly and handles UTF-8 end to end. A browser engine's text still needs the
FreeType/HarfBuzz/ICU path, because shaping and bidi are what a web page assumes.
