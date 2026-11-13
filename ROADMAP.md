# Roadmap: from Linux programs to a browser engine

The 64-bit edition runs unmodified x86-64 Linux programs in ring 3. This file lists what is
still missing between that and running WPE WebKit as The WRLD Browser, in the order the pieces
have to be built.

## Where the port stands

Already working, and not on the list below:

- Ring-3 x86-64 execution, with the timer preempting user code.
- Position-independent ELF loading (executable plus the `ld-linux` it names).
- glibc dynamic linking (`libc.so.6`), plus static-PIE programs.
- `mmap`/`munmap`/`mprotect`/`brk`/`mremap`, with per-process page tables.
- POSIX signals: real handlers, alt stacks, `SIGCHLD`, `SIGPIPE`, faults, `SA_RESTART`.
- `clone()` as glibc's `pthread_create` uses it, futexes, mutexes, condition variables,
  `pthread_join`.
- SSE/SSE2 and x87 for user code.
- A few hundred file, memory, time, identity and threading system calls.

## What is missing

### 1. Process model

- `fork`, `vfork`, `execve` and `wait4` are in place, and `getppid` reports the real parent.
- Missing: `waitid`, process groups and sessions (`setpgid` is a stub), `posix_spawn`'s
  `POSIX_SPAWN_SETSIGDEF` details, and a `/proc` to look at processes through.
- Needed by shell pipelines with job control and WebKit's process launcher.

### 2. Signals

- Dispositions are per process, pending sets per process and per thread, and a handler runs on a
  real x86-64 `rt_sigframe` (a `ucontext` with 23 registers and a 128-byte `siginfo`), so glibc's
  `__restore_rt` and `rt_sigreturn` work unchanged. `sigaltstack`, `SA_RESTART`, `SA_NODEFER`,
  `SA_RESETHAND`, `SA_SIGINFO`, `sigsuspend`, `sigpending`, `sigtimedwait`, `alarm`/`setitimer`,
  `SIGCHLD`, `SIGPIPE` and ring-3 fault dispatch (`SIGSEGV`, `SIGFPE`, `SIGILL`, `SIGBUS`,
  `SIGTRAP`) are wired up; `kill`, `tkill` and `tgkill` deliver to a process or one thread.
- Still missing: job control (`SIGSTOP`/`SIGCONT`, `waitpid(WUNTRACED)`), `signalfd`, and
  `SIGSEGV` inside a handler when the alternate stack overflows.
- Blocking: glibc start-up paths, crash reporters, and any JIT that uses a guard page.

### 3. IPC and the event loop

- `pipe`/`pipe2`, `socketpair`, `AF_UNIX` and `SCM_RIGHTS` descriptor passing are implemented in
  `src/linux/ipc.c`; abstract socket names are not.
- `epoll`, `eventfd` and `timerfd` exist and are level-triggered; `EPOLLET`, `EPOLLONESHOT`,
  `signalfd`, `inotify`, `memfd_create` and shared memory do not.
- `poll()` and `epoll_wait()` now ask the object behind each descriptor (`lx_ipc_ready`); they
  sleep in 1 ms steps between checks, because the scheduler has no wait queues.
- Blocking: WebKit's architecture is `AF_UNIX` plus `epoll` plus descriptor passing between its
  UI, Web and Network processes.

### 4. Sockets

- `AF_INET` TCP and UDP are wired to the stack in `src/net`: `socket`, `bind`, `connect`,
  `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`,
  `getsockname`, `getpeername`, `getsockopt` and `setsockopt` (`SO_REUSEADDR`, `SO_ERROR`,
  `SO_TYPE`, buffer sizes, `TCP_NODELAY`), including non-blocking sockets and readiness through
  `poll`/`epoll`.
- `getaddrinfo` runs in the distribution's glibc: `/etc/hosts`, `/etc/host.conf`,
  `/etc/nsswitch.conf`, `/etc/resolv.conf` (written from the DHCP lease) and `/etc/services` are
  created in the RAM file system on first lookup, and DNS then travels over the UDP stack.
- Still missing: `AF_INET6`, a loopback interface (so `127.0.0.1` does not answer), IP
  fragmentation (a datagram is capped at 1472 bytes), TCP options beyond MSS,
  `SO_REUSEPORT` (accepted, then treated as `SO_REUSEADDR`), and socket timeouts
  (`SO_RCVTIMEO`/`SO_SNDTIMEO` are accepted but not enforced).

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

- The test programs in `samples/linux` are built against **musl** as static PIE when `musl-gcc`
  is installed, so the image carries no C library at all; with only gcc, the build machine's
  glibc is copied instead (dynamic linking still works: the kernel loads `ld-linux` and glibc).
- No C++ runtime: libstdc++ needs unwinding (`_Unwind_*`), `__cxa_*` and thread-safe statics.
- `getaddrinfo` resolves through the C library and the kernel's UDP stack (section 4);
  `getpwnam` has only `/etc/passwd`; no `iconv`, no locale past Latin-1.

### 8. USB

- The host stack (`src/usb`) speaks **xHCI** (USB 3.x), **EHCI** (USB 2.0), **UHCI** and
  **OHCI** (USB 1.1): control, interrupt, bulk and (on xHCI) isochronous transfers, enumeration
  through hubs, hotplug (the buses are rescanned twice a second, at root ports and hub ports),
  HID keyboards and mice (boot protocol, or the device's own report format, with `GET_REPORT`
  for a device whose interrupt endpoint never delivers), USB audio class 1 and 2 playback, and
  bulk-only mass storage.
- Missing: split transactions (a full/low-speed device behind a high-speed hub); isochronous
  transfers on EHCI/OHCI/UHCI; USB audio input, video and MIDI; multiple configurations or
  alternate settings past the one a device needs.
- Needed by a browser only for input and storage, but a PC needs at least one of xHCI, EHCI,
  UHCI or OHCI for both.

### 9. Graphics and text

- The kernel-side GUI has its own TrueType engine (`src/gui/ttf.c`, faces in `C:\WRLD\Fonts`)
  that draws UTF-8, shapes text with the font's own GSUB and GPOS tables (ligatures, contextual
  forms, Arabic joining, mark positioning) and lays out mixed left-to-right and right-to-left
  text with the UAX #9 algorithm (`src/gui/text.c`). Noto Naskh Arabic is bundled as the Arabic
  fallback, so the scripts in the font files reach the screen. It still does not hint.
- Missing from the text stack: Indic shaping (syllable reordering, GSUB beyond the generic
  engine), CJK and emoji coverage (the fonts have none), and the FreeType/HarfBuzz/ICU APIs
  themselves for Linux programs, which have no font stack at all.
- The GUI also has its own **software rasteriser** (`src/gui/raster.c`): path fills and strokes,
  anti-aliasing, round joins, linear and radial gradients and clipping, used by The WRLD Browser for
  `border-radius` and gradient backgrounds. Skia and Cairo are still not used.
- No GPU or 3D, so a real Skia would have to use its software backend. Missing from the raster
  stack: filters and blurs, box shadows, canvas, arbitrary CSS transforms and a full CSS layout
  engine.
- The display path starts with the GRUB/UEFI linear framebuffer and a write-combining mapping.
  QEMU/Bochs, VirtualBox and VMware adapters also support runtime mode changes via native drivers.
  There is no NVIDIA/AMD/Intel GPU driver, hardware acceleration, vsync or display-engine audio.
- PNG/JPEG/GIF/BMP, the native SVG subset and MP3/WAV audio are present. WebP, video decoding
  and a complete SVG implementation are still missing.
- Display audio (HDMI/DP) is prepared on the HDA codec side (digital converter,
  channel slots, audio infoframe), but the GPU's display engine must embed the
  stream and pass ELD; the firmware framebuffer path does not enable that, so it
  needs a GPU display driver.

#### Native desktop appearance

- Dark mode is a palette overlay on Luna Blue, Olive Green, Silver, Classic, Aero and Plex,
  preserving their accents and renderers. Explicit advanced colour overrides are applied
  after dark mode and hue/tone. Images, web document colours and Paint canvases stay unchanged.
- Control Panel's **Visual Effects** page selects XP-style border scaling or Aero-style
  fading for window transitions, Slide/Fade for the Start menu, and separate sidebar-slide
  and tile-fade switches. An overall animation switch and per-effect None choices are
  available; choices persist in `wrld.ini`, and safe mode turns animations off.
- Sidebar tiles have framed bodies and theme-aware title strips; Aero bodies remain glass.
  Time-based transitions run in the existing shell loop without sleeping or waiting for
  frames. Window input still takes precedence over the desktop-layer sidebar.
- These are native software-rendered effects, not GPU acceleration or a new protected shell
  process. `shellmanager.exe` remains a dedicated kernel task with shared kernel-linked GUI
  code. High-memory allocation and full 16/32 TB system volumes remain separate work.

### 10. Services a browser engine expects

- `/proc` serves `/proc/self/{maps,stat,status,cmdline,fd}`, `/proc/self/exe` and
  `/proc/{meminfo,cpuinfo,uptime,version}`, generated when a program opens them. `/proc/<pid>`
  for another process is not implemented; only the caller's own view exists.
- `/sys` has the minimal tree programs probe: `devices/system/cpu/{online,possible,present}` and
  `kernel/{osrelease,ostype,version}`. There is no device tree, cgroup or power interface.
- Limits are per process and inherited; `RLIMIT_NOFILE` and `RLIMIT_FSIZE` are enforced, the
  other resources are stored and reported but not enforced.
- No seccomp-bpf, namespaces or `CLONE_NEWUSER`. `prctl` is honest: thread names, parent-death
  signals, dumpable, `NO_NEW_PRIVS` and the TID address work, while unsupported options
  (including `PR_SET_SECCOMP`) return `EINVAL`, so a sandbox has to be disabled rather than faked.

### 11. Practical ceilings

- 512 MB minimum and about 3.75 GB maximum, 64-bit edition only; WebKit wants 1-2 GB per
  process.
- The installed partition is loaded into RAM at boot, so its size is bounded by RAM.
- No Wi-Fi; USB needs xHCI, EHCI, UHCI or OHCI, and wired IPv4 Ethernet only.
- An unimplemented system call returns `ENOSYS`, is logged once, and the program dies.
- The Linux ABI is covered by the test programs in `samples/linux`: `hello.c`, `threads.c`,
  `ipc.c`, `procs.c`, `socket.c` and `sysinfo.c`.

## Milestones

1. [x] **`fork`, `execve`, `wait4`** - process creation and replacement.
2. [x] **`pipe`, `AF_UNIX`, `SCM_RIGHTS`** - the IPC substrate.
3. [x] **`epoll`, `eventfd`, `timerfd`** - the event loop.
4. [x] **Socket system calls** over the existing TCP stack.
5. [x] **Signal delivery** (`src/linux/signal.c`): handlers, framing, pending sets,
   `sigsuspend`, `SIGCHLD`, `SIGPIPE`, `alarm`, faults-to-signals, `SA_RESTART`.
6. [x] **musl static-PIE** - `musl-gcc -static-pie` builds the sample userland, so the image
   needs no C library; gcc + glibc remains the fallback when musl is not installed.
7. [x] **Text and shaping** - the GUI's own engine gained OpenType GSUB/GPOS, Arabic joining,
   mark positioning, right-to-left layout and bidi (`src/gui/ttf.c`, `text.c`), with Noto Naskh
   Arabic as the Arabic fallback. FreeType, HarfBuzz and ICU themselves are not used: for the
   scripts the image ships, the same work is done without a 30 MB dependency.
8. [x] **Software rasterisation** - the GUI's own path engine (`src/gui/raster.c`): anti-aliased
    fills and strokes, round joins, linear/radial gradients and clipping, used by The WRLD Browser for
   `border-radius` and gradient backgrounds. Skia itself is not used.
9. [ ] **libjpeg/libpng and SQLite** - media and storage.
10. [x] **`/proc`, `/sys`, honest `prctl`** - `/proc/self/{maps,stat,status,cmdline,fd}`,
   `/proc/{meminfo,cpuinfo,uptime,version}`, a minimal `/sys` CPU/kernel tree, per-process
   rlimits (NOFILE and FSIZE enforced) and a `prctl` that returns `EINVAL` for what it does not
   implement, including `PR_SET_SECCOMP`.

Milestones 1-8 and 10 are implemented in `src/linux/proc.c`, `ipc.c`, `syscall.c`, `signal.c`,
`fs.c`, `src/gui/ttf.c`/`text.c`/`raster.c` and `src/usb`.

The shell and The WRLD Browser get their text from the same engine as the desktop: TrueType,
OpenType shaping and bidi, so Arabic and Hebrew reach the screen without a library dependency.
A full browser engine would still benefit from a CJK font and Indic shaping, which is what the
fonts and the generic GSUB engine do not cover.
