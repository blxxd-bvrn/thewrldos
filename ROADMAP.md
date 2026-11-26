# Roadmap: from Linux programs to a browser engine

The 64-bit edition runs unmodified x86-64 Linux programs in ring 3. This file lists what is
still missing between that and running WebKit as THE WRLD OS's browser, in the order the pieces
have to be built.

**Where it stands (November 2026):** **WebKit runs.** WebKitGTK 6 (2.52) renders pages inside a
desktop window, with its web and network processes, JavaScript, canvas, images, and pages
real websites over HTTPS (`wrldweb`, below). Verified inside the OS by these test programs:

- the C++ runtime (glibc's `libstdc++`): exceptions, RTTI, threads, `thread_local`, iostreams,
  `std::filesystem` (`samples/linux-cxx/cxxtest.cpp`, 14 checks);
- the GNOME platform layer WPE is built on: the GLib main loop, `GThread`, `g_spawn` child
  processes, GIO files, Fontconfig, FreeType and Cairo drawing text into a PNG
  (`samples/linux-gnome/platform.c`, 6 checks);
- **WebKit's JavaScript engine**, JavaScriptCore 2.52 from WebKitGTK, JIT included: modern
  JavaScript through its C API and a hot loop that speeds up 4x as the JIT tiers kick in
  (`samples/linux-webkit/jsctest.c`, 12 checks; built with `make ARCH=x86_64 LX_WEBKIT=1`
  because it adds about 70 MB with ICU);
- **shared memory between processes**, the way WebKit's processes exchange tiles and IPC
  buffers: `memfd_create` (with seals), `shm_open`/`shm_unlink` under `/dev/shm`,
  `MAP_SHARED` of either and `MAP_SHARED | MAP_ANONYMOUS`, kept shared across `fork` and when
  the descriptor is passed with `SCM_RIGHTS` (`samples/linux/shm.c`, 16 checks);
- **desktop windows for Linux programs**: `/dev/wrld/window` creates a window whose pixels the
  program maps and draws (32-bit ARGB, what Cairo, pixman and Skia's CPU backend write), presents
  with an `ioctl`, and reads mouse, keyboard, resize and close events from with `read`, `poll`
  or `epoll` (`src/linux/wrldwin.c`, interface in `samples/linux-gnome/wrldwin.h`; the
  `wrldpaint` demo draws with Cairo and takes clicks and typing);
- **a Wayland compositor**, `wrldcomp` (`samples/linux-wayland`), on Ubuntu's libwayland-server:
  `wl_compositor`, subsurfaces, `wl_shm`, `xdg_wm_base` (toplevels and popups), `wl_seat` with an
  XKB keymap, `wl_output`. Each toplevel becomes a `/dev/wrld/window` window, frameless when the
  client draws its own decorations; `wlhello` is a bare test client;
- **GTK 4** with its Cairo renderer on `wrldcomp`: header bar, popovers, entries, buttons,
  sliders (`samples/linux-gtk/gtkhello.c`, `make ARCH=x86_64 LX_GTK=1`, about 35 MB);
- **WebKitGTK 6**: `wrldweb` (`samples/linux-webkitgtk`, `make ARCH=x86_64 LX_WEBKITGTK=1`, about
  210 MB, so give the VM 3 GB) is a small browser with back, forward, reload and an address bar.
  WebKit renders on the CPU; a stand-in `libEGL.so.1` gives it the EGL display it insists on
  without shipping Mesa and LLVM. Run it with
  `C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/wrldweb [address]`.

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
- `clone()` creates processes as well as threads: `fork` is `clone(SIGCHLD)`, and
  `posix_spawn` (what `g_spawn` and WebKit's process launcher use) is `clone(CLONE_VM |
  CLONE_VFORK)` on a stack of its own. Processes cannot share an address space here, so that
  child gets a copy; a failed `exec` then shows as exit status 127 instead of an error from
  `posix_spawn` itself. `close_range` closes or marks inherited descriptors.
- Missing: `waitid`, process groups and sessions (`setpgid` is a stub), `clone3` (glibc falls
  back to `clone`), and a `/proc` to look at other processes through.
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

- Shared objects (`src/linux/shm.c`) are reference-counted page sets: a `MAP_SHARED` mapping of
  a memfd, a `/dev/shm` object or shared anonymous memory uses the object's own pages (marked so
  no address space frees them), and `fork` keeps them shared. `MAP_SHARED` of an ordinary file
  maps one set of pages per file (`rust/src/kernel/lx/locks.rs`) that every process mapping it
  shares; they are written back to the file on `msync`, `munmap` and exit, and `read`/`write`
  of the file stay in step with them.
- A fixed 8 MiB stack with no growth and no guard page.
- No swap and no reclaim, and about 3.75 GB of RAM at most.

### 6. Kernel and scheduler

- The kernel is cooperative, so one long system call (a disk poll, a TLS handshake, populating a
  large mapping) stalls every thread. Only ring 3 is preemptive.
- One CPU: `sched_setaffinity` is a no-op and the affinity mask reports a single CPU.
- `fcntl` record locks (`F_GETLK`, `F_SETLK`, `F_SETLKW` and the OFD ones) and `flock` are real:
  conflicting locks are refused or waited for, and they go on close and exit as on Linux.
  `fsync` writes a file on C: through to the system disk. Deadlocks between two waiting
  processes are not detected (`EDEADLK`).

### 7. C and C++ runtime

- The test programs in `samples/linux` are built against **musl** as static PIE when `musl-gcc`
  is installed, so the image carries no C library at all; with only gcc, the build machine's
  glibc is copied instead (dynamic linking still works: the kernel loads `ld-linux` and glibc).
- C++ works with the distribution's runtime: programs in `samples/linux-cxx` are built with g++
  against glibc and `libstdc++`, and the build copies in every shared library `ldd` lists (the
  same is done for `samples/linux-gnome`, which links GLib, GIO, Cairo, Fontconfig and FreeType).
  musl-built programs still have no C++ runtime of their own.
- `link`/`linkat` make a copy (the VFS has no shared inodes): enough for lock files and
  write-then-link saves, not for two names that must stay one file.
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
- The fixed-function engine remains native CPU rendering: `src/gui/r3d.c` serves OpenGL 1.1 and D3D9;
  D3D10's shared interpreter now accepts SM4, a staged **SM5.0 DXBC** subset and a staged
  **SM6.0 / DXIL 1.0** vertex/pixel subset (`dxbc.c`, `dxil.c`, `shader_priv.h`). Prepared
  per-draw register/operand bindings remove shader-invocation allocations; masked execution,
  native float32 rounding and live-component scanline planes/spans reduce CPU work. Packed
  cross-model signatures, flat/noperspective interpolation, late shader depth and unsigned
  render targets have host framebuffer tests. A fixed 120-frame x64 QEMU cube improved from
  7.97–8.12 to 12.14–13.18 FPS across two paired runs; genuine SM5/SM6 cubes and a 16-check in-OS upload /
  rejection probe also pass. There is no LLVM runtime or new external graphics dependency.
- **First native GPU backend:** eligible D3D10 vertex/pixel draws now lower that shared IR
  directly to legacy SM3 programs and run through VMware SVGA3D (`gpu3d.c`, `shader_gpu.c`).
  Contexts/surfaces, GMR DMA, indexed draws, shader constants and fences are real device
  commands, not a renamed CPU renderer. Stage-legal pixel comparisons and SM3 constant/input
  read-port restrictions are tested. Unsupported hardware operations, control flow,
  linkage, formats, states and large temporary banks retain per-draw native CPU fallback.
  OpenGL, fixed-function D3D9, Flip 3D and desktop animations are not GPU-offloaded.
  - Verified on Workstation 15.0.0 build-10134415 with 3D enabled, virtual hardware **7**,
    `svga.vgpu10 = "FALSE"`, and its DX11 renderer on an RTX 3050. The modern guest-backed
    interface (tested hardware 16) is **not implemented** and falls back. This is not a
    physical NVIDIA driver or a D3D11/12 feature-level claim.
  - Upload/readback and rendered-triangle/depth probes gate enablement. CPU buffers remain
    authoritative, with fenced per-draw readback and safe CPU retry on failure; a 16 MiB
    staging GMR is retained after loss to prevent late-DMA use-after-free. Legacy depth uses
    D24S8; unsupported D32F stays on the CPU.
  - Native SM4/SM5 cubes complete on the GPU; the larger SM6 cube stays on the CPU.
    `gpuverify.exe` passes 36 fixed-pose CPU/GPU readback/counter checks, with zero pixels
    exceeding its RGB-error threshold. Host reference/legality and mock fallback/transport
    tests also cover the boundary. `d3d10cube --cpu --bench` supplies a forced-CPU comparison;
    small-cube measurements show workload-specific speedups, but synchronous upload/readback
    overhead still limits performance and is not a universal speedup.
  - **Still needed:** guest-backed SVGA3D objects, more native shader/state lowering,
    asynchronous/resident GPU resources that avoid per-draw transfers, broader real-device
    validation, and native physical GPU drivers. Do not equate passing shader upload or
    completed fences alone with correct pixels or faster frames.
- **Still needed, not implied by shader acceptance:** D3D11/12 devices/resources/feature
  levels and additional shader stages; geometry, tessellation, compute, waves, atomics/UAVs,
  derivatives, double precision, broader textures/mips, signed render targets, newer profiles,
  stencil, MSAA and stream output. DXIL currently rejects helper functions, backward branches,
  nonzero resource spaces and unsupported intrinsics/properties at upload. D3D10 profile
  queries remain SM4, and there is no run-time HLSL compiler. Extend the tested native subsets
  before claiming more compatibility; JIT/native code generation is not part of this stage.
  Vulkan still needs a SPIR-V frontend and API/driver work and is not planned. Linux programs
  could get OpenGL through Mesa on `/dev/wrld/window`, but that is not implemented. Missing
  from the native 2D raster stack: filters/blurs, box shadows, canvas, arbitrary CSS transforms
  and a full CSS layout engine.
- The display path starts with the GRUB/UEFI linear framebuffer and a write-combining mapping.
  **WRLD Basic Display** now preserves firmware masks/stride, supports direct RGB 15/16/24/32-bit
  layouts and x64 framebuffer apertures above 4 GiB, and correctly pairs PAT PA1 with its page
  selector (including a no-PAT uncached fallback). The shared presenter uses exact RAM damage
  history, contiguous changed-region writes, x64 SSE2 streaming/native packing and small
  cursor-only uploads. x86 remains no-SIMD. History allocation is optional; firmware scanout
  remains usable without it. `video=basic` is an explicit recovery path and PCI ownership is
  conservatively associated without BAR sizing writes or vendor GPU initialization.
  A separate bounded UC MMIO window and opt-in 64-bit BAR decoding also support high
  xHCI register apertures on UEFI systems. The old BAR accessor keeps its low-address
  contract for other drivers; further ports remain explicit work, not an all-driver claim.
  This is a **CPU-rendered basic display driver**, not physical GPU 3D acceleration or
  compatibility with arbitrary Windows `.sys` / Linux `.ko` drivers. BIOS/UEFI QEMU and
  synthetic high-framebuffer mapping are test coverage, not proof for all physical machines;
  live-USB hardware validation remains necessary. Native GPU engines, runtime GOP-independent
  mode setting, monitor hotplug/multi-monitor, power management and broad vendor coverage remain.
  QEMU/Bochs, VirtualBox and VMware adapters also support runtime mode changes via native drivers.
  There is no physical NVIDIA/AMD/Intel GPU driver, vsync or display-engine audio; the
  limited VMware D3D10 offload above is separate from the firmware framebuffer path.
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

## The next steps to WebKit

1. **WebKit is The WRLD Browser** in the 64-bit edition (done): the desktop icon, the Start
   menu and links from other programs open WebKit (`src/apps/webkit.c`) with tabs, a bookmarks
   bar, find, zoom and downloads; one browser runs and later addresses open as tabs in it. The
   built-in engine stays for the 32-bit edition, machines under about 2 GB, and as a choice
   (Control Panel, Network: "Use WebKit"; or `start classicbrowser`). Next: history, saved
   bookmarks, and loading the WebKit libraries from disk to need less memory.
2. **Input under load.** Typing very fast into a WebKit page can drop a key after Shift; typing at
   normal speed, the address bar and all GTK widgets are fine. Not reproducible on the build
   machine or with `lxtrace on`, so it is a timing issue in the Linux layer still to be found.
3. **More real sites.** Real HTTPS sites load (pypi.org directly, docs.python.org through an HTTP
   proxy with `--proxy`); the build environment only reaches a few hosts, so heavier sites
   (video, big JavaScript apps) are still untested. `resolve` checks name lookups the way GLib
   makes them.
4. **Memory.** The root file system lives in RAM, so the WebKit build needs a 3 GB VM; loading
   the WebKit libraries from disk instead would bring that down.

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
9. [x] **libjpeg/libpng and SQLite** - media and storage. `mediatest` (samples/linux-media) runs
   the build machine's libjpeg-turbo, libpng and SQLite: JPEG and PNG written and read back,
   SQLite on C: with transactions, an index, record locks between two processes and WAL mode
   across processes (15 of 15 checks pass).
10. [x] **`/proc`, `/sys`, honest `prctl`** - `/proc/self/{maps,stat,status,cmdline,fd}`,
   `/proc/{meminfo,cpuinfo,uptime,version}`, a minimal `/sys` CPU/kernel tree, per-process
   rlimits (NOFILE and FSIZE enforced) and a `prctl` that returns `EINVAL` for what it does not
   implement, including `PR_SET_SECCOMP`.

11. [x] **C++ runtime** - glibc's `libstdc++` (`samples/linux-cxx`).
12. [x] **GNOME platform layer** - GLib, GIO, `g_spawn`, Fontconfig, FreeType, Cairo
   (`samples/linux-gnome`).
13. [x] **JavaScriptCore** - WebKit's JavaScript engine with its JIT (`samples/linux-webkit`).
14. [x] **Shared memory between processes** - `memfd_create`, `/dev/shm`, real `MAP_SHARED`
   (`src/linux/shm.c`, `samples/linux/shm.c`).
15. [x] **Desktop windows for Linux programs** - `/dev/wrld/window` (`src/linux/wrldwin.c`).
16. [x] **A Wayland compositor on `/dev/wrld/window`**, GTK 4 and WebKitGTK (`wrldcomp`, `gtkhello`,
    `wrldweb`).
17. [x] **WebKit as a browser** on THE WRLD OS: The WRLD Browser in the 64-bit edition.

Milestones 1-8 and 10 are implemented in `src/linux/proc.c`, `ipc.c`, `syscall.c`, `signal.c`,
`fs.c`, `src/gui/ttf.c`/`text.c`/`raster.c` and `src/usb`.

The shell and The WRLD Browser get their text from the same engine as the desktop: TrueType,
OpenType shaping and bidi, so Arabic and Hebrew reach the screen without a library dependency.
A full browser engine would still benefit from a CJK font and Indic shaping, which is what the
fonts and the generic GSUB engine do not cover.
