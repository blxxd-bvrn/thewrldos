# THE WRLD OS

**Version: Pre-Alpha 1** (build 2600). It is shown in About THE WRLD OS (Run: `winver`), System
Properties, `ver` and the Command Prompt banner; `uname -r` reports it as `0.1.2600`.

An original x86 operating system written from scratch in C and assembly (about 20,000
lines, plus the Mbed TLS library for HTTPS, the QuickJS JavaScript engine, the Lexbor HTML parser and the minimp3, stb_vorbis, dr_flac and PL_MPEG decoders), in a **32-bit**
and a **64-bit (x86-64) edition**. It has an XP-style "Luna" desktop and a
category-style Control Panel, and it runs **Win32 `.exe` programs**, **`.sh` shell scripts** and opens
**Apple `.dmg` disk images**. It connects to the network and has its own web browser,
**The WRLD Browser** (formerly XplorerLite).

- Boots from a GRUB CD image (BIOS or UEFI) into a spinning-globe boot screen, a log-on
  screen and the desktop.
- **Two editions from the same sources, on one CD:** the 64-bit edition (x86-64 long mode) runs
  64-bit (x64) Win32 programs and has The WRLD Browser on WebKit; the 32-bit edition (any
  Pentium-class CPU or later) runs 32-bit Win32 programs and uses the browser's built-in engine.
  `wrldos.iso` carries both: its boot menu starts the 64-bit edition on a processor that can
  run it and the 32-bit one otherwise, and offers both. `wrldos64.iso` is the 64-bit edition
  alone.
- Runs **live from the CD** or **installs to a hard disk** with a graphical Setup (a detailed
  disk list, confirmations before formatting and a progress page), after which every file and
  setting is saved to the disk.
- Visual styles **Luna (Blue), Olive Green, Silver, Royale, WRLD Classic, Aero** (translucent
  blurred glass), **Vista Aero**, **Plex** (Longhorn-style) and **Metro** (flat, Windows 8-style). Each one is a `.wrldstyle` text file you can edit
  or copy, and each can be recoloured with **hue, saturation and lightness** sliders.
- **Networking:** drivers for Intel (PRO/1000, e1000e, I217-I219, I210/I211/I350), Realtek
  (RTL8111/8168/8125, RTL8139), AMD PCnet, VMware VMXNET3 and virtio-net cards, a TCP/IP stack
  with DHCP and DNS, and HTTPS with certificate checking.
- **Real power-off** through ACPI, on VirtualBox, VMware, Hyper-V and real PCs as well as QEMU.
- **Sound:** drivers for Intel HD Audio, AC'97 (Intel, nVidia, AMD, SiS, VIA), C-Media CMI8738,
  Ensoniq AudioPCI (ES1370, ES1371/ES1373), VirtIO sound, Sound Blaster 16, Windows Sound System
  (Crystal CS4231A) and USB audio (class 1 and 2). A startup sound plays when
  you log on and a shutdown sound plays before the computer turns off. The Sound page lists the
  playback endpoints and every detected audio-class device with its driver state; **Scan again**
  refreshes the list after plugging in a device.
- **Graphics:** drivers for the Bochs/QEMU, VirtualBox and VMware display adapters, so the
  resolution can be changed at runtime (1024x768 up to 1920x1200 or more).
- **Memory:** physical RAM is detected and reported up to 512 GB. The kernel heap stays in the
  low 4 GB window, where device DMA buffers are safe.
- **Security:** optional account password at first-boot setup and the logon screen, an inbound
  firewall in the network stack (blocks unsolicited TCP SYNs and unbound UDP), and a Security
  Center for the firewall, password and shell status.
- **Text:** scalable TrueType fonts (Liberation Sans, Serif and Mono, read from `C:\WRLD\Fonts`) with
  OpenType shaping: Arabic (with its joining forms and mark positioning, in Noto Naskh Arabic),
  Hebrew, ligatures, and the Unicode bidirectional algorithm, so right-to-left text mixes with
  Latin and numbers correctly — at any size.

**Logo animation:** [docs/logo/THE-WRLD-OS-logo.mp4](docs/logo/THE-WRLD-OS-logo.mp4) (1080p, 60 fps, 13.5 s, no sound): a
holographic globe in space becomes the globe on the log-on screen's blue, and the letters fly in
to form the logo. [docs/logo/logo-animation.html](docs/logo/logo-animation.html) is the page it was rendered
from; open it in a browser to watch it live.

![Boot screen](docs/screenshots/boot.png)

| | |
|---|---|
| ![Sign-in screen (the default logon skin)](docs/screenshots/logon-signin.png) | ![XP Classic logon skin](docs/screenshots/welcome.png) |
| ![Logon Skin Themer](docs/screenshots/logon-themer.png) | ![Logon Skin Maker](docs/screenshots/logon-maker.png) |
| ![Start menu](docs/screenshots/start-menu.png) | |
| ![Control Panel](docs/screenshots/control-panel.png) | ![Olive Green with a custom hue](docs/screenshots/olive-hue.png) |
| ![Silver](docs/screenshots/silver.png) | ![WRLD Classic](docs/screenshots/classic.png) |
| ![Win32 GUI program](docs/screenshots/win32-exe.png) | ![Win32 console program](docs/screenshots/console-exe.png) |
| ![Shell script](docs/screenshots/shell-script.png) | ![Mounted .dmg disk image](docs/screenshots/dmg-mounted.png) |
| ![Aero](docs/screenshots/aero.png) | ![Plex](docs/screenshots/plex.png) |
| ![Dark mode](docs/screenshots/dark.png) | ![Explorer in dark mode](docs/screenshots/dark-explorer.png) |
| ![Advanced appearance](docs/screenshots/advanced.png) | ![Sound settings](docs/screenshots/sound.png) |
| ![Visual Effects settings](docs/screenshots/visual-effects.png) | ![Longhorn-style sidebar tiles](docs/screenshots/sidebar-tiles.png) |

### The WRLD Browser

| | |
|---|---|
| ![Browser showing the Welcome page](docs/screenshots/browser.png) | ![Images and forms](docs/screenshots/browser-forms.png) |
| ![A page over HTTPS](docs/screenshots/browser-https.png) | ![Network and Sharing Center](docs/screenshots/network.png) |

### Installing to a hard disk

| | |
|---|---|
| ![Setup welcome](docs/screenshots/setup-welcome.png) | ![Choosing a partition](docs/screenshots/setup-partitions.png) |
| ![Choosing the file system](docs/screenshots/setup-format.png) | ![Copying files](docs/screenshots/setup-copying.png) |
| ![CD boot menu after installing](docs/screenshots/cd-menu-installed.png) | ![First-boot wizard](docs/screenshots/setup-oobe.png) |
| ![Finalizing installation](docs/screenshots/setup-finalizing.png) | ![Installed desktop](docs/screenshots/installed-desktop.png) |
| ![Choosing Aero in the wizard](docs/screenshots/oobe-aero.png) | ![Installed with Aero](docs/screenshots/installed-aero.png) |

## Quick start

### Windows: one double-click

Double-click **`Build to ISO.bat`** in the source folder. It:

1. builds through **WSL** (Windows Subsystem for Linux), or through **Docker Desktop** if WSL
   is not set up;
2. installs the Linux build tools on the first run (Ubuntu may ask for your Linux password);
3. writes **`wrldos.iso`** (the CD with both editions) and **`wrldos64.iso`** (the 64-bit edition alone) next to
   the .bat file.

If neither WSL nor Docker is available, it offers three ways out, none of which needs anything
installed beforehand:

- **Download ready-made ISOs.** GitHub Actions builds both ISOs on every push to the main branch
  (`.github/workflows/build-iso.yml`) and publishes them as the `latest-iso` release; the .bat
  fetches them with Windows' own `curl.exe` and checks their SHA-256. A few minutes, but your
  own changes to the sources are not in them.
- **Build in a portable Linux VM.** The first time, it downloads the build kit (about 1 GB, the
  `build-vm-kit` release made by `.github/workflows/build-vm-kit.yml`) into
  `%LOCALAPPDATA%\WRLD OS build VM`: an Ubuntu 24.04 disk with every build tool, Rust and the
  WebKit packages already installed, the kernel that boots it, and QEMU for Windows. Your sources
  go into the VM as a tar archive on a second virtual disk, the build runs, and the ISOs come back
  on a third disk, unpacked with Windows' own `tar.exe`. No administrator rights and no network
  inside the build. With *Windows Hypervisor Platform* turned on (Windows Features) QEMU runs at
  close to full speed; without it the computer is emulated: a full build took 25 minutes with four
  processors in a test, longer on slower PCs.
  The VM's disk keeps the object files, so later builds are quicker. `build-vm.log` next to the
  .bat holds the build's messages. (`tools/buildvm/`: `make-image.sh` makes the disk,
  `build-iso.ps1` runs it on Windows, `run-vm.sh` on Linux.)
- **Install WSL with Ubuntu** (an administrator prompt and a restart; then open *Ubuntu* once to
  create a Linux user, and run the .bat again). After that, builds are quick.

`Build to ISO.bat download` and `Build to ISO.bat vm` choose the first two directly, also on a
PC that has WSL. A copy of the sources that is not a git checkout (a downloaded zip) reads the
GitHub repository to download from in `tools/buildvm/repo.txt`.

> MSYS2, MinGW and Cygwin cannot build the kernel. Their `gcc` makes Windows PE files, not the
> ELF kernel that GRUB loads, and they have no BIOS GRUB tools. `make` stops with an
> explanation if you try. The .bat file uses WSL for exactly this reason.

### Linux (or a WSL terminal)

```bash
sudo apt install build-essential gcc-multilib nasm grub-pc-bin grub-efi-amd64-bin grub-common xorriso mtools qemu-system-x86
```

Part of the kernel is written in Rust (see [Rust in the kernel](#rust-in-the-kernel-rust)), so the
build also needs Rust from [rustup](https://rustup.rs) with a freestanding target for each
edition (distribution packages of Rust do not include them). `Build to ISO.bat` installs this
by itself.

```bash
curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
rustup target add x86_64-unknown-none i586-unknown-linux-gnu
```

The 64-bit edition's Linux userland (The WRLD Browser's WebKit engine, GTK 4, video and sound,
`/bin/sh`) comes from these packages on the build machine (Ubuntu 24.04 or newer). The WebKit
image is the default build: without these packages `make` stops and says what to install, and
`make classic` (the same as `make LX_WEBKITGTK=0`) builds the smaller image with the browser's classic engine instead.
`Build to ISO.bat` installs them by itself, and on an older distribution builds the 64-bit
edition in an Ubuntu 24.04 of its own (a Docker container, or a chroot it makes with
debootstrap); `WRLD_CLASSIC=1 tools/build-iso.sh .` builds the classic image.

```bash
sudo apt install pkg-config python3 musl-tools busybox ca-certificates libwebkitgtk-6.0-dev libgtk-4-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libglib2.0-dev libcairo2-dev libfontconfig-dev libfreetype-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad xkb-data gsettings-desktop-schemas
```

```bash
make            # the main image: the 64-bit edition with WebKit, build64/wrld.elf and wrldos64.iso
```

```bash
make ARCH=i686      # the 32-bit edition alone: build/wrld.elf and wrldos32.iso
```

```bash
make both           # both editions, and wrldos.iso: the CD with both (make dual assembles it)
```

```bash
make run        # boots the CD in QEMU: live session, nothing is saved
```

`make run` (and `run-install`, `run-disk`) boots the 64-bit edition in `qemu-system-x86_64`,
with the hard disk image `disk64.img`; `make ARCH=i686 run` does the same with the 32-bit
edition and `disk.img`.

Tested with Ubuntu 22.04 (gcc 11, binutils 2.38, GRUB 2.06) and 24.04 (gcc 13, binutils
2.42). `make check-tools` lists any missing tool. Fedora's `grub2-*` tool names are detected
automatically. On WSL, clone the repository inside the Linux file system (for example
`~/XPLikeTest`, not `/mnt/c/...`). On Windows 11 the QEMU window opens on the normal desktop.

## Running and installing

### In QEMU

| Command | What it does |
|---|---|
| `make run` | Live session from the CD. Nothing is saved. Networking uses QEMU's built-in NAT, and sound goes to PulseAudio/PipeWire when it's running (also under WSLg). |
| `make run-install` | CD plus a blank hard disk image (`disk64.img`; `disk.img` with `ARCH=i686`). Choose **Install THE WRLD OS**. |
| `make run-disk` | Boots the installed disk image on its own, with no CD. |
| `make disk` | Creates the blank `disk.img` (size: `make disk DISK_MB=1024`). |

You can also choose the emulated hardware:

- `make run SOUND=ac97` (or `ich9`, `es1370`, `sb16`) picks another emulated sound card
  instead of HD Audio.
- `make run VGA=vmware` uses the VMware SVGA II display adapter.
- `make run AUDIO=alsa` (or `sdl`, or `none`) picks QEMU's host sound output.

### In VirtualBox, VMware or on a real PC

Boot `wrldos.iso` (in a VM set up for a 64-bit guest, for example VirtualBox's
"Other/Unknown (64-bit)", it starts the 64-bit edition; in a 32-bit guest the 32-bit one) with **legacy BIOS** or **UEFI** boot and at least
**48 MB RAM** for the 32-bit edition (a Pentium-class PC; in 48 MB the desktop, in a plain colour,
the Command Prompt and Notepad run, 64 MB adds Explorer and the system sounds, and 96 MB shows the
photo wallpapers and leaves room for several programs; a program that does not fit is refused with
a "Not enough memory" message) and **384 MB** for the 64-bit one, whose live CD carries the Linux programs and
WebKit; 256 MB is comfortable for the 32-bit browser. The ISO
is a hybrid image: a PC-BIOS core and an EFI System Partition with both `EFI/BOOT/BOOTX64.EFI` and
`EFI/BOOT/BOOTIA32.EFI`, so 64-bit UEFI and 32-bit UEFI firmware (Bay Trail tablets and 2-in-1s,
early Intel Macs) both boot it; Setup installs both loaders. See [real hardware](docs/HARDWARE.md)
for what works on which generation of PC. To install,
attach a hard disk on any of the usual controllers: **IDE**, **SATA (AHCI)**, **NVMe**, or **SCSI**
(VMware/VirtualBox **LSI Logic** and **LSI Logic SAS**, **VMware Paravirtual**, **virtio-scsi**), a
**virtio-blk** disk in QEMU/KVM/Proxmox, or a **USB** disk behind a UHCI, OHCI, EHCI or xHCI
controller.
USB keyboards and mice work the same way, including through a hub. BusLogic adapters are not supported.
For sound, any of the cards VirtualBox offers works (**Intel HD Audio**, **ICH AC97** or
**SoundBlaster 16**), and VMware's **HD Audio** or **Ensoniq AudioPCI** (ES1371). **HD Audio is the recommended card.** VMware gives "Other" and Windows XP guests the Ensoniq
AudioPCI, which plays nothing on some hosts: `tools/vm/THE WRLD OS.vmx` is a ready VMware machine
set to HD Audio (or set the guest OS to Windows 7 or later, or add `sound.virtualDev = "hdaudio"`
to your `.vmx`); in VirtualBox choose Intel HD Audio. The OS cannot choose the card: the virtual
machine's settings do. With VMware's Ensoniq card, or any card that does not play, the OS says
how to switch at the end of Setup, at log-on and in Control Panel > Sound. Graphics work with **VBoxVGA**, **VMSVGA** or
VMware's SVGA, and those can change resolution. Every network adapter VMware offers works (**e1000**, **e1000e**,
**VMXNET3**, and the **AMD PCnet** "vlance" it picks for Windows XP guests), as do VirtualBox's (**Intel PRO/1000**,
**PCnet**, **virtio-net**) and QEMU's e1000, e1000e, igb, rtl8139, pcnet, vmxnet3 and virtio-net.

While an installed system starts, the boot screen shows what it is doing, including
"Loading files from the hard disk… N%". If it stops on one step, that step names the device
involved. It also warns when the machine has less than 64 MB of memory.

If the desktop ever stops responding while the clock still runs, a red banner at the top of the
screen names the task that is busy (the same line goes to the serial log). Please include it
when you report the problem.

### The CD boot menu

| Entry | |
|---|---|
| **Boot THE WRLD OS from the hard disk** | Shown, and selected by default, once the system is installed. |
| **THE WRLD OS Live, 64-bit** | Runs from the CD in RAM, with the WebKit browser (default on a 64-bit processor when nothing is installed). |
| **THE WRLD OS Live, 32-bit** | The 32-bit edition, for 32-bit Windows programs. On a 32-bit processor it is the only edition shown. |
| **Install THE WRLD OS, 64-bit / 32-bit** | Starts Setup; it installs the edition it runs in. |
| More options | Text-mode Setup, verbose boot (the kernel log on the boot screen), Basic Display recovery and safe mode (WRLD Classic style), for each edition. |

GRUB tells the two kinds of processor apart with `cpuid -l` (`grub/grub-dual.cfg`).
`wrldos64.iso` and `wrldos32.iso` have the single-edition menu (`grub/grub.cfg`).

The installed system has its own boot menu with the same normal, verbose and safe-mode entries.

If the firmware never delivers the 8259's timer interrupt, the kernel notices within the first
few milliseconds and switches to the local APIC timer by itself, so the clock, the boot
animation and every timeout keep working. Adding `lapic` to a kernel command line forces that
timer, which is useful when testing a machine that boots with a silent timer.

### What Setup does

You can also start Setup from the **Install THE WRLD OS** icon on the live desktop.

1. **Graphical Setup** (the same wizard look as the first-boot pages, mouse or keyboard):
   - welcome screen and licence (accept the checkbox or press F8);
   - **detailed disk list**: each disk shows its number, model, bus, capacity and partition table
     (MBR, GPT or none), and each partition shows its drive letter, partition number, file
     system, size and free space. Unpartitioned space is listed with its available size;
   - create (**New partition**) and delete partitions. Both actions show a confirmation naming
     the disk, partition, drive letter and size; a GPT disk is called out before Setup replaces
     its table with MBR;
   - **format** as NTFS or FAT32, quick or full, or keep the existing file system. When Setup
     was started by UEFI firmware the system partition must be FAT32: it becomes the EFI System
     Partition (type `0xEF`) and receives `\EFI\BOOT\BOOTX64.EFI`;
   - a **summary page** repeats the exact disk, partition, letter, size and file system, warns
     that formatting deletes everything on the partition, and requires an "I understand"
     checkbox before **Install** is enabled;
   - **installs**, on a screen laid out like the Windows XP installer's: the stages on the left
     (*Preparing the disk*, *Copying files*, *Installing devices*, *Finalizing installation*)
     with the time left and a progress bar, and on the right pages about what THE WRLD OS does,
     changing every 8 seconds. Setup formats the partition and copies the system. It then
     **installs devices**: it matches each device in the computer to its built-in driver and
     writes the hardware profile to the new system. Last, it writes the boot loader: GRUB's boot
     code in the MBR and its core image after the MBR (which also keeps the disk BIOS-bootable).
     The work takes about a minute, so Setup then shows the pages it has not shown yet
     (Enter moves on). The pages come from `C:\WRLD\Setup\Slides.txt`, which has a title line,
     its text, and a blank line between pages, so they can be changed without a rebuild;
   - restarts. If the sound card is not HD Audio and may play nothing, the last page says how to
     switch.

   The blue text-mode Setup is kept as a fallback: choose **Install THE WRLD OS (text mode)** in
   the boot menu (or add `setup=text` to the kernel command line).
2. **First boot from the disk**: the devices are already installed, so a graphical wizard
   asks straight away for (a disk installed by an older Setup, with no hardware profile, gets
   the *Installing devices* step first):
   - time zone;
   - your name and organisation;
   - computer name;
   - visual style and account picture.

   It then finishes with *Finalizing installation*.
3. From then on, changes to `C:` are written back to the disk: new folders, deletes and
   renames immediately, file contents within about 2 seconds and at shutdown/restart.
   Settings (theme, hue, wallpaper, account picture, user and computer name, time zone, mouse
   and clock options) are kept in `C:\WRLD\wrld.ini`. Control Panel > System and About show
   which disk and partition the system runs from, and how much space is free.

## Using THE WRLD OS

- **Samples** in `C:\Samples`:
  - `hello.exe`, a Win32 console program;
  - `winhello.exe`, a Win32 GUI program;
  - `wsget.exe`, a Windows sockets program that fetches a web page (`wsget example.com`);
  - `glcube.exe`, `d3dcube.exe` and `d3d10cube.exe`, spinning cubes drawn through OpenGL,
    Direct3D 9 and Direct3D 10 (3D acceleration must be on);
  - `dxdemo.exe`, a textured cube through Direct3D 8 that turns with the arrow keys or a
    mouse drag through DirectInput 8 and plays a tone through DirectSound on Space;
  - `demo.sh`, a shell script;
  - `Sample.dmg`, a disk image with a `.app` bundle and an installer script.
- **The WRLD Browser** (Start menu > Internet, or the globe on Quick Launch):
  - Tabs: Ctrl+T opens one, Ctrl+W closes it, Ctrl+Tab and Ctrl+1..9 switch, and a middle-click
    opens a link in a new tab. Ctrl+L selects the address bar.
  - Try `C:\Samples\CSS demo.html` for boxes, margins and a linked style sheet, and
    `C:\Samples\Modern layout.html` for flexbox, grid and CSS variables, and
    `C:\Samples\Icons and floats.html` for SVG icons, floats and positioning.
  - It runs **JavaScript** (QuickJS) and has a built-in **ad blocker**. Try
    `C:\Samples\JavaScript demo.html` (linked from the home page).
  - The home page links to sites that work well, such as Wikipedia, Hacker News, CNN Lite and
    68k.news.
  - It has DuckDuckGo search; you can also type words in the address bar to search (the
    WebKit browser of the 64-bit edition uses The WRLD Web, below).
  - `C:\Samples\Welcome.html` shows everything it can render.
- **Command Prompt** examples:
  - `ipconfig`, `ping example.com`, `nslookup example.com`;
  - `wget https://example.com/file.zip` downloads a file; `curl URL` prints it;
  - `unzip file.zip` or `unrar x file.rar [folder]` extracts an archive (`-l` / `unrar l` lists
    it);
  - `theme list`, `theme aero`, `theme plex 200`, `theme saturation -40`, `theme lightness 20`;
  - `js -e "[1,2,3].map(x => x * x)"` or `js script.js` runs JavaScript;
  - `open control personalization` opens Control Panel at Personalization;
  - `open C:/Samples/Sample.dmg` mounts the disk image;
  - `hdiutil info` shows mounted images;
  - `sync` writes pending changes to the disk now (installed systems);
  - `display` lists screen resolutions, and `display 1280x800` switches to one;
  - `volume 60`, `volume mute`; `play "C:/WRLD/Media/WRLD Startup.flac"`;
  - `hostname`, `ps`, `free`, `shutdown -r`;
  - `help` lists all commands.
- **Keyboard**:
  - Win key opens the Start menu;
  - Alt+Tab switches windows and Alt+F4 closes them;
  - Ctrl+Shift+Esc opens Task Manager.
- **Clipboard**: one system clipboard for text, shared by every program:
  - text boxes (the Run box, address and search boxes, dialogs): Ctrl+A selects the text,
    Ctrl+C / Ctrl+X copy or cut it, Ctrl+V (or Shift+Insert) pastes;
  - Notepad and WordPad use it for their Copy, Cut and Paste;
  - the Command Prompt pastes with Ctrl+V, Shift+Insert or a right-click (a pasted list of
    commands runs one after another), and Ctrl+Shift+C copies the text on the screen;
  - the WebKit browser shares it both ways: copy on a page or in the address bar and paste in
    Notepad, or copy in Notepad and paste into a web form;
  - Explorer has Copy, Cut and Paste for files and folders (Ctrl+C, Ctrl+X, Ctrl+V, or the
    right-click menu).
- **Drag and drop**: drag a file or folder from Explorer or the desktop
  - onto a folder (in Explorer, on the desktop, or a drive in My Computer): on the same drive
    it moves, from another drive it is copied, and holding Ctrl copies;
  - onto the Recycle Bin to delete it, or onto the desktop to put it in the Desktop folder;
  - onto Notepad, WordPad, Media Player or Paint to open it there, or onto a program's icon
    on the desktop to open it with that program;
  - onto the WebKit browser: the drop reaches the page as a list of file links (text/uri-list),
    but WebKit does not yet hand it to the page as files, so drag-and-drop uploads do not work
    yet (use the page's Browse button). Inside the browser, text, links and pictures drag and
    drop as on any desktop.
  - onto empty space on the desktop: desktop icons stay where you drop them, snapped to the
    grid, as on Windows without Auto Arrange (kept in `C:\WRLD\deskpos.txt`); the others fill
    the free grid cells from the top left.
  A right-click during the drag cancels it.
- **Archives (ZIP and RAR)**: double-click a `.zip` or `.rar` file, or right-click it and
  choose *Extract All...*. *Extract Compressed Folder* suggests a folder next to the archive
  named after it, shows a progress bar and the file being extracted (Cancel keeps what is
  done), then opens the folder. It reads ZIP (stored and deflated, ZIP64, self-extracting and
  padded archives) and RAR from version 2.9 to 7, solid archives included, with RAR's PPMd
  text compression and its filters for programs, pictures and sound. Archives that need a
  password or are split into parts (`.part1.rar`, `.r00`) are reported, not extracted.

## What's inside

### 32-bit and 64-bit (`src/arch/i686`, `src/arch/x86_64`)
- One set of C sources builds both editions; only the boot stub, the interrupt and
  context-switch assembly and the linker script are per architecture.
- **32-bit:** compiled for the Pentium (i586, tuned for i686) without MMX or SSE, so it also runs
  on Pentium MMX, AMD K6 and the VIA C3 of many thin clients; PAT, the local APIC, write-combining
  and the TSC are used only when CPUID reports them. Programs may use SSE all the same: on a
  processor that has it, the kernel turns it on at boot (`CR4.OSFXSR`) and interrupts save the
  interrupted code's x87/SSE state with `FXSAVE` (`FNSAVE` on older processors), so SSE-built
  libraries such as SDL2 work.
- **x86-64:** GRUB loads the 64-bit ELF kernel through Multiboot 2 (or Multiboot 1 on an older
  GRUB). A 32-bit stub checks for long mode, identity-maps the first 4 GiB with 2 MiB pages,
  enables PAE, long mode, the FPU and SSE, and calls the same `kmain`. Interrupt stubs save the
  SSE state (`FXSAVE`), so drivers and the JavaScript engine can use SSE; the cooperative
  context switch keeps the callee-saved registers.
- Win32 programs get their own address space: on x64 a private PML4/PDPT, with the image
  mapped at its preferred base (usually `0x140000000`) in 2 MiB pages, so no relocation is
  needed.

### Rust in the kernel (`rust/`)
The kernel is moving from C to Rust one module at a time. `rust/` is a `no_std` static library
(`wrldrs`) that the Makefile builds with cargo and links into the kernel beside the C objects:
- **Targets:** `x86_64-unknown-none` for the 64-bit edition; for the 32-bit edition
  `i586-unknown-linux-gnu`, used only for its prebuilt `core` and `alloc`, so the Rust code runs
  on the same Pentium-class processors (x87, no SSE) as the C kernel. Both work with stable
  Rust; nothing rebuilds the standard library.
- **Bridge (`rust/src/kernel.rs`):** the kernel heap (`kmalloc`) is Rust's global allocator,
  `klog!` writes to the kernel log, and a Rust panic is a kernel panic with its message. The
  boot log says `rust: wrldrs 0.1.0 ready` once `kmain` has checked that allocation works.
- **Converting a module:** the Rust module exports the same C functions as the file it
  replaces, so callers do not change. It must not panic on bad input (in the kernel that would
  be a kernel panic): it returns errors instead, and `make rust-test` (`cargo test` on the build
  machine) fuzzes it. Logic that does not touch the kernel builds and tests on the host.
  Before the C file is deleted, the old and new versions are run side by side on the test
  vectors and on thousands of damaged inputs (the C one under AddressSanitizer).
- **Shared structs:** a C struct that Rust reads or writes is mirrored with `#[repr(C)]`, and
  C exports its real size and field offsets, which `rust_init` checks at boot (a mismatch is a
  kernel panic naming the struct, not silent memory corruption). The kernel's `bool` is an
  `int` (`src/include/types.h`); Rust uses `kernel::CBool` for it.
- **Converted so far:**
  - **DEFLATE/zlib (`rust/src/inflate.rs`, was `src/kernel/inflate.c`):** PNG images, ZIP
    archives, gzip packages, DMG images, HTTP compression and WordPad's documents. Every read
    and write is bounds-checked, and the Huffman tables are on the caller's stack, so two
    threads can now decompress at once (the C version used static tables). Tested against
    62 zlib streams (`tools/gen_inflate_testdata.py`) and a 20,000-case fuzz run. Before the
    switch, 200,000 intact and damaged streams gave the same result in the C and Rust versions.
  - **FAT12/16/32 image reader (`rust/src/fat.rs`, was `src/kernel/fat.c`) and Apple disk
    images (`rust/src/dmg.rs`, was `src/kernel/dmg.c`):** what mounts a `.dmg` or raw `.img`
    as a drive (`hdiutil attach`, double-clicking it). The C versions trusted the image: the
    FAT lookups, the GPT entry table, a partition's length, a file size of 4 GB and chunk
    offsets near 2^64 could all make them read or write outside their buffers. In 6,000
    damaged images run through both, 83 crashed the C code under AddressSanitizer (heap
    overflows in the partition and directory code, wild reads in the FAT chain code); the
    Rust code handled all of them, and every other case gave the same tree, except that names
    with control or non-ASCII bytes get `_` and impossible boot sectors are refused. Chains
    stop at the volume's cluster count, copied file data is limited to the image's size and
    entries to 200,000, so loops and cross-links cannot hang or exhaust memory. Mounted files
    are now actually read-only. Test vectors: FAT12/16/32 volumes made with mkfs.fat and
    mtools (long names, a 200-entry directory, a deleted entry, a fragmented file, dates) and
    DMGs with zlib, raw and zero chunks, MBR and GPT maps, HFS+ and an unsupported codec
    (`tools/gen_diskimage_testdata.py`).
  - **Network packet parsing (`rust/src/net.rs`, from `src/net/net.c` and `tcp.c`):** every
    received ARP packet, IPv4 header, UDP datagram, TCP header (with its MSS option), DHCP
    reply and DNS answer is checked and read in Rust; the C code keeps the protocol state
    (ARP cache, connections, the DHCP and DNS clients) and acts on what the parsers return.
    The Internet checksum and `str_to_ip` moved too. One addition: IPv4 headers with a wrong
    checksum are now dropped (the C code never checked). Over 400,000 mutated packets the old
    and new parsers agreed on every one, apart from those checksum drops. Tested live on both
    editions with e1000 and virtio-net: DHCP, ping, a download with its SHA-256 checked, DNS
    through a CNAME and a non-existent name (`ipconfig /dns A.B.C.D` sets the DNS server by
    hand), and inbound connections to a Winsock server.
  - **The network stack itself (`rust/src/netstack.rs`, was `src/net/net.c` and `tcp.c`):**
    Ethernet, ARP, IPv4, ICMP, UDP, the inbound firewall, the DHCP and DNS clients and TCP
    (active and passive opens, retransmission, close), behind the same `net.h` functions;
    `netif` and the firewall switches are Rust statics with C's layout. The protocols are plain
    state machines over a small `Link` (clock, randomness, frame output), so `cargo test`
    plays whole exchanges against them: ARP, ping, DHCP, DNS, a TCP client conversation, a TCP
    server accepting, retransmission and time-out, resets, the firewall, and random frames.
    Changes in behaviour: a packet for an address whose MAC is unknown waits for the ARP
    answer (and is dropped after a second) instead of `ip_send` sleeping; UDP handlers in C
    are called after the stack is released, so they can reply; stale TCP handles from C are
    refused instead of used; buffers that depend on traffic are reserved so that running out
    of memory refuses a connection instead of panicking. Tested live like the parsers, plus
    the Linux socket layer (`C:/Linux/usr/local/bin/socket` and `resolve`).
  - **The Windows program loader (`rust/src/pe.rs`, from `src/win32/pe.c`):** reading the PE
    headers and section table, mapping sections, base relocations, walking the import table,
    looking up exports (names, ordinals, forwarders), resources (`FindResource`,
    `LoadString`, dialogs, icons) and TLS. `pe.c` keeps what needs the process: finding DLLs,
    the API tables, thunks for missing functions, `DllMain` and TLS callbacks; Rust calls back
    into it for each imported DLL and function. The C code trusted the file: import and
    export tables, resource directories and relocation blocks could all make it read or
    write outside the image. In 6,000 damaged programs and DLLs (the samples, Tiny C and
    MinGW test DLLs, Notepad2, SDL2 and the libraries of a SDL game) run through both, 291
    crashed the C code under AddressSanitizer and none the Rust code; 5,690 gave identical
    results, and each of the other 19 is the C code reading or writing past the image or
    skipping relocations because a pointer wrapped round, where Rust does the right thing.
    One new refusal: an optional header too small for its own fields. Test vectors
    (`tools/pe_testdata/`): an x86 and an x64 DLL and program built with MinGW, with named
    and ordinal exports, a forwarder, string tables in two languages, a named resource and
    TLS, checked against `objdump`, and the same program linked at two base addresses to
    check relocation.
  - **The registry (`rust/src/registry.rs` and `rust/src/kernel/winreg.rs`, was
    `src/win32/registry.c`):** the key tree, handles, `registry.dat` and all 57 advapi32,
    kernel32 and shlwapi registry functions (74 table entries). They are written in Rust with each
    edition's Windows calling convention (stdcall on x86, the Microsoft x64 one on x86-64),
    and `apitab.c` finds them in a table Rust exports. Old and new were compared on the
    build machine, the C code under AddressSanitizer: 3,000 random sequences of registry
    operations and 6,000 damaged `registry.dat` files gave identical results, buffer sizes
    and saved files. A test program (`tools/regtest`: 136 checks of every function,
    including short buffers, wide names, HKEY_CLASSES_ROOT and deletion under open handles)
    printed the same on the old and new kernels, on both editions. What changed: names
    longer than 127 bytes are cut the same way when stored and when looked up (the C code
    stored a cut name and then never found it, and created a new key each time); `registry.dat`
    is now version 2, which escapes control bytes and `%` in key names so that a key name
    with a line break cannot inject keys at the next start (version 1 files still load);
    keys and data have upper limits (200,000 keys, 32 MB of data); saving and deleting deep
    trees no longer use recursion; `RegOpenCurrentUser` returns the sign-extended
    `HKEY_CURRENT_USER` on x64, as Windows does.
  - **The FAT32 driver (`rust/src/fat32.rs` and `rust/src/kernel/fat32k.rs`, was
    `src/kernel/fat32.c`):** the file system of installed systems that chose FAT32 (Setup
    formats it, and `C:` is loaded from it and saved back to it). The driver works over a small
    disk interface, so the build machine runs it against disk images in memory. On valid
    volumes it writes exactly what the C driver wrote, so whole disk images were compared:
    350 random sequences of formatting, writing, reading, renaming, deleting, syncing and
    remounting, and 150 volumes made by `mkfs.fat` and filled by mtools, gave byte-identical
    images, and every result passes `fsck.fat`. That test found three bugs in the C driver,
    fixed in the Rust one: a file or folder could be created with the name `..`, which no
    listing shows and no deletion frees (clusters were lost); a rename whose target path ran
    through a file removed the source before failing, losing the file and its clusters; and a
    volume formatted without a label got a blank label entry that `fsck.fat` rejects (it is now
    `NO NAME`). Of 1,500 damaged volumes, 1,475 behaved identically; in the rest the Rust driver
    refuses volumes whose root lies outside them, or does not count clusters outside the volume
    as freed. A damaged volume can no longer make the driver read or write past the volume (a
    cluster number beyond it ends a chain), allocate a file's claimed size before checking it
    against its chain (a 4 GB size wrapped the C allocation), or recurse without end (deletion
    stops at cycles and 64 levels; C recursed). Tested by installing both editions onto FAT32,
    putting a file on the disk with mtools, then creating, renaming, deleting and copying files
    on the running system and checking the disk with `fsck.fat` and mtools.
  - **The NTFS driver (`rust/src/ntfs.rs` and `rust/src/kernel/ntfsk.rs`, was
    `src/kernel/ntfs.c`):** the other file system Setup offers (format, MFT records with
    fixups, run lists, `$I30` B-tree folders, the cluster and MFT bitmaps). Like the FAT32
    driver it works over a small disk interface and was compared with the C driver on disk
    images: 150 random sequences of formatting, writing, reading, renaming, deleting, syncing
    and remounting, and 60 volumes made by `mkntfs` and filled by `ntfscp`, gave byte-identical
    images, and every result passes `ntfsfix`. Of 800 damaged volumes, 784 behaved
    identically; 13 crashed the C driver (heap buffer overflows) and are handled by the Rust
    one; in the other 3 a damaged `$STANDARD_INFORMATION` length made the C driver read
    timestamps from past the attribute, where the Rust one ignores them. A damaged volume can
    no longer make the driver read past a record or an attribute, follow a run list outside
    its attribute, read or write past the volume, allocate bitmaps or record tables of any size
    it claims (they are capped and checked against the volume), or recurse without end
    (loading and deleting trees no longer recurse). `rust/src/kernel/volk.rs` (was
    `src/kernel/vol.c`) puts the two drivers behind the one interface Setup and persistence use.
  - **The shell (`rust/src/shell.rs` and `rust/src/kernel/shk.rs`, was `src/shell/sh.c`):**
    the tokenizer, the parser, arithmetic, wildcard matching and every expansion are in
    `shell.rs` with host tests; `shk.rs` runs the syntax trees (pipelines, redirection,
    functions, loops, `case`), starts programs (Win32, Linux, scripts, disk images, GUI
    applications) and has the built-in commands that work on text and files (echo, printf,
    test, read, cd, ls, dir, cat, grep, sort, head, tail, wc, tr, cp, mv, rm, find, tree,
    hexdump, unzip, xargs…). The commands that front other parts of the system (ipconfig,
    ping, wget, js, display, play, ps, mount, theme, devices, dpkg, shutdown…) stay in C, in
    `src/shell/shcmds.c`. Three test scripts (about 130 commands) gave the same output on
    the old and new shells, and the terminal looked pixel-identical, except where C bugs are
    fixed: `$((…))` skipped the character after it (`echo $((1+2))x` printed `3`), `printf`
    with `%%` and more arguments, or a format ending in `%5`, looped for ever, a function
    defined inside a loop lost its body the second time, `unzip` read past the end of a
    damaged archive, `${v#pattern}` overran a 512-byte buffer on long values, and deep
    nesting or long strings could overflow the task's stack (parsing, arithmetic and
    wildcards no longer recurse without limit). Output to a pipe or `$(…)` is dropped
    rather than crashing when memory runs out, and `seq` stops at Ctrl+C.
  - **The image decoders (`rust/src/imgdec.rs` and `rust/src/kernel/imgk.rs`, were in
    `src/apps/imgdec.c` and what was `accessories.c`):** PNG, GIF, baseline and progressive JPEG and
    BMP, for the desktop wallpaper, the logon screen, the Image Viewer, Paint and the browser.
    `imgdec.c` keeps the format sniffing, the size limits and SVG. Over 1,239 images (the
    system's own wallpapers, avatars and samples, and ImageMagick conversions of three of them
    into every PNG colour type and bit depth, interlaced and not, GIFs with and without
    interlacing and transparency, and JPEGs at several qualities, subsamplings, restart
    intervals and progressive scripts, from 1x1 to 300x201) the old and new decoders gave
    identical pixels, and the Rust ones are as fast. On 4,904 damaged copies the C decoder
    ran into 390 memory errors (all JPEG: heap overflows from Huffman, quantisation and
    component table numbers above 3, scan bands past coefficient 63, blocks outside the
    coefficient buffer, Huffman code counts that overflowed the lookup table into the rest of
    the decoder's state, and one crash); the Rust decoders read every one safely, and 14,160
    more damaged files caused no panic. Where both ran, 10 results differ, all from damaged
    JPEG headers: a table segment shorter than it claims now reads as zeros instead of the
    next segment's bytes, a zero sampling factor is refused, and the 3 where the C decoder had
    overwritten its own state.
  - **The browser's document parser (`rust/src/html.rs` and `rust/src/css.rs`, was
    `src/apps/html.c`):** everything between Lexbor and the layout engine. `html.rs` builds
    the tree from Lexbor's parse events (title, `<base>`, refresh, body colours, linked and
    `<style>` sheets, depth capped at 512), runs the cascade with Lexbor's selector matcher
    on that tree, and serves `innerHTML` fragments and `querySelector()`; `css.rs`, with host
    tests, parses declarations (colours, gradients, lengths, the box model, positioning,
    flexbox and grid, `calc()`/`min()`/`max()`/`clamp()`, `var()` with fallbacks),
    `@media` queries, sheets into rules and custom properties, and the charset and UTF-8
    clean-up. Nodes stay C `node_t`s in the page arena (`html.c` now holds only the arena)
    and `doc_t` keeps its fields; the sheets, rules and variables are Rust's, and `rust_init`
    checks the three structures' layout. To compare, the old and new code were built on the
    build machine with the real Lexbor into two programs that print the whole tree, every
    element's computed style and 17 selector matches per element, after linked sheets
    arrive and after an `innerHTML` fragment. They printed the same for 278 real pages
    (documentation found on the build machine, paired with 56 real style sheets), 1,500
    generated pages using every property the parser knows, 4,000 damaged ones and 11 stress
    pages (3,000-deep nesting, a 20 KB `style=""`, self-referencing variables, 5,000 rules,
    50 `@media` levels, 50 links), and ten pages looked pixel-identical in the 32-bit
    edition's browser on the old and new kernels. Nothing is shared between documents any
    more (the C code sent every declaration through one static 8 KB buffer and `strtok`,
    and `var()` read whichever page was styled last, even one already freed), and the
    sheets, rules, variables and selector lists use fallible allocation, so running out of
    memory drops style rules instead of crashing (the tree's nodes still come from the
    page arena, as before).
  - **Persistence (`rust/src/kernel/persistk.rs` and `rust/src/persist.rs`, was
    `src/kernel/persist.c`):** finding the system partition at boot (and upgrading systems
    installed by older versions: `C:\Windows` to `C:\WRLD`, the old classic style and browser
    shortcut), writing every change on C: back to the disk (folders, deletes and renames at
    once, file contents from the background `persistd` task and on shutdown), the settings
    store behind `settings_get`/`settings_set`, reading and writing `C:\WRLD\wrld.ini`, and
    the account password. `persist.rs` (settings store, ini format, password digest, the
    upgrade's style rewrite) has host tests; the digest matches the C one. Copying the
    settings to and from the desktop's variables stays in C (`settings_apply` and
    `settings_capture` in `persist.c`). The dirty-file list was shared by the VFS hook and
    the write-back task with no locking, so a file deleted during a flush could stay in it
    and a file changed just as a flush ended could be dropped; it is now changed with
    interrupts off, a flush takes it as one batch, a delete also clears the batch being
    written, and two flushes (the background task and shutdown) take turns. Folder trees
    that arrive on C: are walked without recursion. The same session (folders, files,
    appends, deletes before and after a flush, renames, copying and moving a folder of 15
    fonts, a style, the volume and a password, then a reboot and logon with that password)
    left the same files, contents and `wrld.ini` on FAT32 disks installed with the old and
    new kernels and on an NTFS disk, and both kernels upgraded an old-style install the same
    way. (The VM test also caught a Rust pitfall: in inline assembly a bare `pushf` assembles
    to the 16-bit `pushfw`, which left the stack two bytes off; it is spelled `pushfd` /
    `pushfq`.)
  - **The font engine (`rust/src/ttf.rs`, was `src/gui/ttf.c`):** TrueType tables, Unicode
    cmaps, `kern` pairs, OpenType shaping (GSUB substitutions and ligatures, contextual and
    chaining lookups, Arabic joining forms, GPOS kerning and mark attachment) and the
    anti-aliased rasteriser. Its C interface (`src/include/ttf.h`) needs only the caller's
    allocator, so the kernel and the `check-display` host test link the same code. Built on
    the build machine next to the old C engine, it gave identical metrics, cmaps, advances,
    kerning, shaping and pixels for the 13 bundled fonts and all 107 TrueType fonts found on
    the build machine (about 1.3 million glyph renders at four sizes, upright and slanted, and
    shaping of Latin, Arabic, Hebrew and combining-mark text plus random strings); the 49
    CFF fonts are refused by both. On 3,000 damaged fonts the C engine overflowed its buffer
    on 13 (lookup subtable offsets it read without checking); the Rust one reads 0 past the
    file and matched the C results on all the others. One C bug is fixed: GPOS pair
    positioning (format 1) stepped through each pair set 2 bytes too far per record, so only
    the first pair of a set ever matched; "Te", "Ta" and similar pairs in Liberation Sans now
    kern as the font intends, which moves text after them by a pixel or so. Rendering is as
    fast as before; shaping Arabic is up to about a third slower, and lines are cached.
  - **Reading and checking software packages (`rust/src/pkgfmt.rs` and
    `rust/src/kernel/pkgk.rs`, were in `src/apps/pkg.c` and `src/pkg/unpack.c`):** ar, tar
    (GNU long names, pax) and cpio archives, Debian control files, RPM headers, gzip members,
    the pre-install check (dependencies, and the loader, `NEEDED` libraries, `RUNPATH` and
    symbol versions of every ELF program, and script interpreters), and the `.desktop`
    entries and `update-alternatives` lines the installer reads. `pkg.c` keeps installing,
    removing, the records, the menus and the dpkg/rpm/apt commands; xz and Zstandard stay
    C libraries. The check sees the installed system through a small interface, so it was
    compared on the build machine with the old C against the machine's real Ubuntu
    libraries: the same package fields, file lists, payloads and check results for 782
    Debian packages (Ubuntu 24.04) and 106 CentOS Stream 9 RPMs, and for 2,997 of 3,000
    damaged ones; on the other 3 (damaged RPM tag counts) the C code spun for minutes and
    the Rust one answers in under a tenth of a second. Installing hello, figlet (with its
    update-alternatives lines), cowsay, a package with a menu entry and two RPMs printed the
    same in the 64-bit edition with the old and new kernels. Fixed on the way: a pax record
    shorter than its key gave a negative copy length, cpio names and pax lengths were read
    past the end of the archive, 32-bit size checks in ar and cpio and the 64-bit ELF
    program header check could wrap, an empty item hung `list_add`, and an RPM tag claiming
    billions of entries looped for as long.
  - **What USB devices send (`rust/src/usbdesc.rs`, was `src/usb/hid.c`,
    `src/usb/audioclass.c` and parts of `src/usb/usb.c`):** configuration descriptors (the
    interface and endpoints a storage or hub driver uses, and every HID interface), HID report
    descriptors and the mouse and keyboard reports read with them, the USB audio class 1 and 2
    formats, sample rates and clock sources, and mass storage's command status, capacity and
    model name. The controller drivers, enumeration and the class drivers stay C. Compared on
    the build machine with the old C over two million generated report and configuration
    descriptors (with damaged ones among them): the same formats, decoded reports, interfaces,
    endpoints and audio formats, with no panics; each read is checked against the buffer
    (the old C read a third byte from two-byte descriptors). `make check-hid` and `make check-audio` now run against the Rust
    code. A USB keyboard, mouse and memory stick on xHCI and EHCI were found the same way, and
    the keyboard and mouse worked, in both editions with the old and new kernels.
  - **Direct3D shader bytecode (`rust/src/dxbc.rs` and `rust/src/dxil.rs`, were
    `src/win32/dxbc.c` and `dxil.c`):** the DXBC container and its signatures, Shader Model
    4/5 program tokens, the DXIL (LLVM 3.7 bitcode) reader for the SM6.0 subset, the checks
    every program passes before it runs, and the interpreter that runs vertex and pixel
    shaders for Direct3D 10. The instruction list stays the C structures in
    `src/win32/shader_priv.h` (the SVGA3D lowering in `shader_gpu.c` reads them; `rust_init`
    checks their layout at boot), allocated with kmalloc. The 64-bit kernel's Rust code is
    soft-float, so no float passes by value between Rust and C: the C library's sqrt, sin,
    cos, exp2 and log2 and the texture-sampling callback go through `src/win32/shader_abi.c`.
    Compared with the old C (built with the kernel's flags) over the 102 fixture shaders,
    mutated copies of them and random SM4/SM5 programs from a generator, for each of the
    host, 64-bit kernel and 32-bit kernel builds of the Rust code: about 96,000 programs per
    build, 35,000 of them loaded and run with random inputs (106,000 runs), gave the same
    errors, instruction lists and output bits, NaN bits included. The 64-bit kernel build
    applies SSE's NaN rules to its soft-float results to get there. In the 32-bit build one
    run in 108,000 gave a different NaN: the two compilers keep x87 intermediate results in
    registers at different points; with NaNs counted as equal, no value differed.
    `make check-shaders` runs against the Rust code and passes unchanged, and `d3d10test`
    prints exactly what it did. Speed: the cube pixel-shader benchmark runs about 1.5x faster
    than the C one on a host build with SSE; the 64-bit kernel's soft-float build runs at 0.75
    to 1x the C speed. In QEMU, `d3d10cube --bench` took 0.93 to 1.11 times as long as with
    the old kernel, and `shaderprobe` passes.
  - **The Linux layer (`rust/src/kernel/lx/`, with paths, "#!" lines and ELF headers in
    `rust/src/lxpath.rs`; was `src/linux`):** address spaces and demand paging, shared memory,
    processes, threads, fork/vfork/execve/wait, signals and their x86-64 frames, the file,
    pipe, socket (AF_UNIX and AF_INET), epoll, eventfd and timerfd calls, /proc and /sys,
    /dev/dsp and /dev/wrld/window. Its structures are now the Rust code's own (C holds only a
    task's `lx` pointer); the C structures it reads (`task_t`, `regs_t`, `console_t`,
    `window_t`) are checked at boot. The path, "#!" and ELF code gave the same results as the
    old C for 900,000 generated paths, script lines and damaged headers. With `lxtrace on`,
    the sample programs (hello, sysinfo, procs, threads, signals, ipc, shm, socket, the C++
    runtime test) and a BusyBox session made the same system calls with the same results on
    the old and new kernels, and printed the same; only futex calls and thread order in
    multi-threaded programs, times and random values differ between runs. The Cairo window
    sample, DNS over UDP, /dev/dsp and the WebKit browser (compositor, GTK, threads, shared
    memory and sockets) behave as before. `madvise(MADV_DONTNEED)` now visits only the
    mappings in its range instead of every page: WebKit drops a 64 GiB reservation that way,
    which took the C code 3 seconds in QEMU, close to the desktop watchdog's limit. Two C bugs
    that the port first kept are fixed: readlink of a /proc/self/fd link failed with EINVAL
    (the C code passed one buffer for two results) and now gives the file's path, or
    `pipe:[N]`, `socket:[N]` and so on as Linux does; and newfstatat with AT_EMPTY_PATH
    returned ENAMETOOLONG for any path of two or more characters, where it now looks the path
    up. A test program gives the same answers as on a Linux kernel.
  - **The browser's SVG renderer and document interface (`rust/src/svg.rs`, `rust/src/dom.rs`;
    were `src/apps/svg.c` and `src/apps/dom.c`):** inline `<svg>` and `.svg` pictures, and the
    `dom_*` calls the JavaScript engine uses to read and change the page (nodes, attributes,
    text, `innerHTML`, inline styles, form controls, title, cookies and storage). Fetching for
    scripts and the wall clock as a double stay in C (`src/apps/domnet.c`); the page and form
    control structures (`page_t`, `ctrl_t`) are checked at boot. A host harness ran the old C
    and the Rust code side by side: 5,615 SVG icons from freely licensed icon themes and 3,000
    generated SVGs (odd numbers, every path command, arcs, transforms, paints, `<use>`) gave
    the same rasteriser calls; 1,144 runs of 800 random DOM operations on the parser's test
    pages and a form page gave the same results and the same trees, styles and controls after
    every step. In QEMU the SVG and JavaScript sample pages, clicked, typed into and added to,
    look the same pixel for pixel on both editions. Fixed: a path with numbers after `Z`
    (`M0 0Z 5 5`) and a huge or infinite angle (`rotate(1e10)`) froze the browser in endless
    loops, and an exponent such as `1e-12147483647` took seconds.
  - **Archives (`rust/src/unarc.rs`, `rust/src/ppmd.rs`):** ZIP and RAR extraction for
    Explorer's *Extract All...* (`src/apps/extract.c`) and the `unzip` / `unrar` commands,
    written for this project from the formats' published descriptions (decompression only):
    RAR 2.9-4.x's LZ coder, its PPMd variant H model and its standard filters (x86 E8/E8E9,
    Itanium, delta, RGB, audio), RAR 5/7's LZ coder and filters (delta, x86, ARM), and a ZIP
    reader with ZIP64, prefixes and a local-header fallback. Every member's CRC-32 is checked,
    names are made safe (no drive letters or `..`), and the decoders give other tasks the
    processor every 256 KB because kernel tasks are not preempted. Tested on the host with
    libarchive's test archives (BSD licence; `WRLD_RAR_TESTS=folder cargo test unarc`), on
    both 64-bit and 32-bit targets: every RAR 2.9-5 sample decodes with a matching checksum,
    including a 241 MB solid PPMd/LZ stream, and the deliberately damaged ones are refused
    without a crash.
  - **Audio files and the ad blocker (`rust/src/pcm.rs`, `rust/src/adblock.rs`; were
    `src/media/pcm.c` and `src/apps/adblock.c`):** the media player's WAV (PCM, float, A-law,
    mu-law, IMA and Microsoft ADPCM), AIFF/AIFC and Sun .au reader, and the browser's ad
    blocker with its list file. The C structures they share (`mdec_info_t`, the start of
    `vnode_t`) are checked at boot. 197 files made with FFmpeg in every encoding and channel
    count, and 6,010 damaged copies, gave the same errors, lengths and samples as the C code,
    read in random chunk sizes; 1,000,000 random checks (list files, addresses, classes and
    ids) gave the same answers from both ad blockers. In QEMU, files of each format downloaded
    with `wget` play in the media player as before on both editions (recorded through an
    emulated AC97 card), and a fresh `C:\WRLD\adblock.txt` is the same. Fixed: an AIFF whose
    sound data offset ran past the end of the file made the C code read outside the file
    (its 32-bit sums wrapped); such a file now has no samples. An ADPCM block smaller than its
    own header, which the C code accepted by asking for a 2 GB buffer, is refused as a bad
    header. The sound system's own WAV loader (in `src/kernel/audio.c`, used by `play`,
    system sounds and Win32 `PlaySound`) is gone: every sound file goes through the same
    decoders, so those places also play ADPCM, A-law, mu-law, float and AIFF/.au files, and
    surround files keep their centre channel. Plain PCM WAVs give the same samples as before.
    The old loader froze the desktop on a WAV with a chunk size near 4 GB (its offset wrapped
    round) and read past the end of a file cut off in its format chunk.
  - **The HTTP client (`rust/src/http.rs`, was `src/net/http.c`):** URL parsing and
    resolving, GET/POST over TCP or TLS, redirects, chunked transfer encoding and gzip/deflate
    content encoding, for the browser, scripts' `fetch`, `wget`/`curl` and package downloads.
    `http_response_t` is checked at boot. A host harness ran the old and new clients against
    the same simulated network (DNS, TCP and TLS stand-ins serving 10,001 generated server
    responses in random pieces: damaged headers, bad chunk sizes, broken gzip, resets and
    timeouts, redirect chains): both sent the same bytes and returned the same results. In
    QEMU both editions sent the same requests, byte for byte, as before to a local test
    server (redirects, POST from `fetch`, chunked gzip, deflate) and showed the same pages.
    Fixed: a script calling `fetch` with a method longer than about 2,800 characters
    overflowed the C code's 3,072-byte request buffer on the kernel stack; and relative links
    now resolve as RFC 3986 says. The C code left "." and ".." in paths that start with "/"
    (`/../g`) and in `//host/..` references, turned a trailing "." into "//", collapsed "//"
    inside paths, took any reference with "://" in its first ten characters for an absolute
    one, and knew only a few schemes, so `tel:` and `sms:` links were taken as relative paths.
    On 24,550 generated base/link pairs the new code matches RFC 3986 (with a one-letter
    "scheme" read as a drive letter, and an empty http path written as "/" as browsers do);
    the C code differed on 6,022, and every pair the C code got right is unchanged.
  - **WordPad's document readers (`rust/src/wpdoc.rs`, were in `src/apps/accessories/wordpad.c`):** Rich
    Text (files, the undo snapshots and the rich clipboard), and the zip and XML of `.docx` and
    `.odt`. They build a list of paragraphs (text, a character style per byte, alignment,
    bullet, spacing, indents) that WordPad copies into its own document; the editor, the
    writers and plain text stay in C, and the shared structures are checked at boot. 324 real
    documents (made by LibreOffice, pandoc and python-docx from varied formatting) and 7,000
    generated and damaged ones gave the same paragraphs, text and styles as the C readers. In
    QEMU a `.docx`, an `.odt` and an `.rtf`, then copy, paste and undo, look the same as
    before on both editions. Fixed: an "&" in a document's text that is not an entity (a bare
    "&" as in "AT&T", "&#0;", "&nbsp;") made the C reader go round forever, and a zip whose
    sizes point outside the file made it read past its end (a stored member was copied by
    the size it claimed); such members are now refused.
  - **The in-memory file system (`rust/src/kernel/vfsk.rs`, with paths and the initrd's tar
    reader in `rust/src/vfspath.rs`, was `src/kernel/vfs.c`):** every drive, folder and file
    the desktop, the shell, the Win32 and Linux layers see. Nodes stay C `vnode_t`s (C code
    reads them directly; `rust_init` checks every field's offset at boot), so the C API is
    unchanged. The same file commands (creating, appending, copying and moving folders,
    renaming, deleting, relative, `..` and `~` paths) printed the same on the old and new
    kernels, and installs to NTFS and FAT32 kept every change across reboots. Changes:
    walking, copying and freeing trees no longer recurse; copying a folder into itself (which
    looped until memory ran out) and moving a drive's root are refused; making folders under a
    file or on a missing drive fails instead of creating unreachable nodes; and a damaged
    initrd member can no longer read past the archive.
  - **The smaller Win32 API files (`rust/src/kernel/w32misc.rs`, `w32k32.rs`, `w32shell.rs`,
    was `src/win32/misc_api.c`, `k32more.c` and `shell.c`):** about 660 functions. ole32 and
    oleaut32 basics (GUID strings, SAFEARRAYs, VARIANT conversions) and the other "misc" DLLs
    (security, event log, crypt32, setupapi, winspool, ldap, IME, fibers), more of kernel32
    (environment, version checks, INI files, atoms, Tool Help snapshots, SearchPath, console
    input records, file and volume information), and shlwapi, shell32, version.dll and winmm
    (paths, strings, URLs, colours, special folders, item ID lists, ShellExecute,
    SHFileOperation, CommandLineToArgvW, version resources, PlaySound, waveOut, MCI). The
    parsing they need (INI files, %VAR% expansion, VerifyVersionInfo, paths, command lines,
    version resources and blocks, GUIDs, OLE dates) is in `rust/src/w32util.rs` with host
    tests; `src/win32/rsglue.c` gives Rust what it needs of the C process structure, and the
    four C-variadic `wnsprintf` functions, which stable Rust cannot define, stay in
    `src/win32/shell_va.c`. A test program (`tools/apitest`, 186 checks) printed the same on
    the old and new kernels on both editions; ccdemo, Notepad2 and Chocolate Doom run as
    before. Changes: version resources are read with every offset checked against the file
    (the C code read past short or damaged files), INI files longer than 4,096 lines are no
    longer cut when written, deleting a folder that cannot be emptied stops instead of
    looping, and `GetVersionEx` no longer clears more than its structure when a program
    passes a larger size.

### Kernel (`src/kernel`)
- **CPU and interrupts:**
  - Multiboot 1 **and** Multiboot 2 headers with a 1024x768x32 framebuffer request. A small
    boot-information layer (`bootinfo.c`) normalises memory maps, modules, command lines and
    the framebuffer; on UEFI, Multiboot 2 also supplies the ACPI RSDP and marks the firmware
    as EFI.
  - x86: GDT with a per-thread FS segment for the Win32 TEB; x64: the TEB through the GS base
    MSR. IDT and remapped PIC.
  - Exception handling: a faulting program is closed with an error dialog; kernel faults show
    a stop screen.
- **Memory:**
  - 4 MiB-page paging. Each Win32 process gets **its own page directory** for its image window
    (4–16 MiB), so several programs can load at the same base address.
  - Heap allocator.
- **Scheduling:** cooperative multitasking (tasks, sleep, kill, exit codes, CPU/idle
  accounting).
- **Devices:**
  - PIT timer (1 kHz), CMOS RTC, PS/2 keyboard and wheel mouse.
  - **Display (`video.c`):**
    - **WRLD Basic Display Driver** (`wrldbasic`, `src/gui/basicfb.c`): a native,
      vendor-neutral fallback for the direct-color framebuffer set up by BIOS/VESA or
      UEFI GOP. This is the basic-display-driver approach, **not universal GPU-accelerated
      3D** and not a way to load Windows/Linux graphics drivers. It retains firmware scanout
      without resetting or programming an unsupported Intel/AMD/NVIDIA GPU.
      - Firmware RGB masks and padded pitch are preserved: RGB/BGR 32/24-bit, RGB565/555
        and supported 32-bit bitmask layouts such as 10:10:10. Invalid/overlapping masks,
        indexed/text modes and invalid apertures are rejected before framebuffer writes.
        The x64 edition can map framebuffer apertures above 4 GiB; x86 still requires
        them below 4 GiB. This does not enable high-RAM heap allocation.
      - PAT entry **PA1** is programmed and selected consistently by the large-page PWT
        bit. PAT/MSR capability is checked; an uncached path works when unavailable.
        x64 has aligned SSE2 streaming stores for large native-format writes; x86 stays
        SSE/MMX-free. RGB conversion uses native packing/lookups, not external libraries.
      - Exact 32x16-pixel RAM history skips unchanged regions and merges adjacent changed
        tiles, with no device-memory reads. Cursor composition is done in RAM and restored
        afterwards; movement uploads only old/new cursor regions. History is optional
        (at most 64 MiB with a RAM headroom check); allocation failure retains full-copy
        presentation. A failed back-buffer allocation does not change the display mode.
      - `video=basic` / `basicdisplay` forces firmware-only recovery; the GRUB menu has a
        Basic Display live entry. PCI association uses read-only BAR bases and is conservative
        on multi-GPU machines, rather than claiming or resetting an arbitrary adapter.
        `display` reports the driver, masks, physical/mapped address, cache/copy path and
        transfer counters. See [Basic Display and live-USB testing](docs/BASIC_DISPLAY.md).
      - A bounded uncached high-MMIO window and read-only 64-bit PCI BAR decoder let
        xHCI use UEFI-assigned USB-controller registers above 4 GiB. Other native
        drivers retain their existing limits until ported; this is not universal
        Windows/Linux driver compatibility.
    - Bochs/BGA "DISPI" driver for QEMU's standard VGA, Bochs and VirtualBox VBoxVGA.
    - VMware SVGA II driver (command FIFO) for VMware and VirtualBox VMSVGA.
      A native **legacy VMware SVGA3D** backend offloads eligible D3D10 draws; see
      the 3D section below for the tested VM configuration and CPU fallbacks.
    - Resolution changes at runtime from Control Panel > Display, with an XP-style "keep these
      settings?" countdown, and are saved in `wrld.ini`.
    - On real hardware the bootloader asks for the display's preferred 32-bit mode (a
      1920x1080x32-through-1024x768x32 list, then `auto`) and the kernel marks the framebuffer
      write-combining, so a real GPU gets a native-resolution, faster surface; a 24-bit boot mode
      is converted too. A physical NVIDIA/AMD/Intel **GPU-engine** driver is not included;
      WRLD Basic Display uses the GRUB/UEFI framebuffer instead, and the display-engine
      half of HDMI audio still needs such a driver.
  - **Sound (`audio.c`):**
    - **Intel HD Audio:** CORB/RIRB codec commands, and a search for the output path from the
      DAC to the speaker or headphone pins. Every codec on the link and every audio function
      group is tried, so a display-audio (HDMI/DP) codec that comes first does not hide the analog
      one, and a codec's jack detection decides which pin is used. Controllers are probed with the
      motherboard (bus 0) first, then GPUs, so a laptop with both gets its analog codec rather
      than HDMI. The codec vendor is reported (Realtek, Conexant, SigmaTel/IDT, Analog Devices,
      NVIDIA, AMD, Intel), and amplifier paths, EAPD and headphone amps are set up for both laptop
      and desktop codecs.
    - **Display audio (HDMI/DP):** the digital converter is enabled, its channel slots mapped, and
      the codec's audio infoframe is written through the HDA DIP verbs, so the codec side is ready.
      The GPU's display engine still has to embed the audio into the HDMI/DP signal and pass ELD
      information; that is part of a GPU display driver (the firmware framebuffer does not do it).
    - **AC'97:** bus-master DMA, for Intel ICH and the compatible nVidia nForce, AMD and SiS
      controllers.
    - **Ensoniq AudioPCI:** ES1370 (AK4531 codec) and ES1371/ES1373/CT5880 (AC'97 codec and
      the sample rate converter); DAC2 loops over the ring.
    - **Sound Blaster 16:** DSP 4.x, 16-bit stereo auto-init ISA DMA from a buffer below
      16 MiB; the 8237 count register gives the position.
    - **Windows Sound System (Crystal CS4231A, AD1848):** the codec at port 534h (QEMU's
      `cs4231a`) or the other usual WSS ports, 48 kHz 16-bit stereo from 8-bit ISA DMA channel 3,
      IRQ 9.
    - **VirtIO sound** (QEMU/KVM `virtio-sound-pci`, Proxmox): the first 16-bit stereo output
      stream; each ring buffer is a period on the TX queue, and the device returning it is the
      play position.
    - **VIA VT82C686 and VT8233/VT8235/VT8237/VT8251 AC'97** (VIA chipsets) and **C-Media
      CMI8738/CMI8338**: written from the chips' documented registers; QEMU cannot emulate them,
      so they are untested on real hardware.
    - **USB audio (class 1 and 2):** the audio control and streaming interfaces are read from the
      configuration, the streaming alternate setting is selected, the sample rate is set (from the
      format descriptor's list in class 1, on the clock source entity in class 2), and the
      isochronous OUT endpoint is kept fed with one packet per service interval (1 ms on a
      full-speed bus, 125 us on a high-speed one). When a device offers both classes the class 2
      stream is chosen, and the playback device is named `USB Audio 2.0` or `USB Audio 1.0`
      accordingly. HD Audio is the default output; while it is present the USB device stays in the
      Sound playback list, and picking it switches the mixer over (HD Audio is kept for switching
      back). When no HD Audio controller is present, a USB audio device becomes the output when it
      is plugged in, and the card it took over from comes back when it is unplugged. When no audio
      controller is on the PCI bus at all, the mixer also presents a software HDA controller (one
      codec, an audio function group, a DAC and a speaker pin) whose stream is fed into the USB
      device's packets, so the system keeps an HDA playback path even without HDA hardware.
      The Sound page lists every playback endpoint the controller reports - line output, speakers,
      headphones, S/PDIF or HDMI/DP output - next to the USB device. Picking a pin switches that
      card's output enable; the software controller's virtual jacks play through the USB device.
    - The log-on screen names the sound device it found, or the audio controllers it could not
      use and why (`check-audio` checks the descriptor parser on the build machine).
    - Output is 48 kHz 16-bit stereo (44.1 kHz on the ES1370, Sound Blaster 16, and a USB device
      that cannot do 48), fed by a ring of DMA buffers refilled from the timer interrupt. USB
      audio is paced by its own packets instead of a card position.
    - A 4-voice mixer with volume and mute, and a WAV loader for 8/16/24-bit PCM at any sample
      rate and channel count.
  - PCI enumeration, CPUID, PC speaker.
  - ACPI power management: reads the FADT and the DSDT's `\_S5_` soft-off value, so power-off
    works on any ACPI machine; restart uses the ACPI reset register, then port 0xCF9, then the
    keyboard controller.
  - Serial (COM1) kernel log.
- **Storage:**
  - Disk controllers behind one block-device layer (`blk.c`), all polled and found by a PCI scan
    of every bus (VMware puts NVMe and SAS controllers behind bridges):
    - IDE/ATA (`ata.c`): bus-master DMA with a PIO fallback, LBA28/LBA48.
    - SATA AHCI (`ahci.c`): Intel ICH/PCH, QEMU q35, VirtualBox and VMware SATA; BIOS hand-off.
    - NVMe (`nvme.c`): admin + one I/O queue pair, PRP lists, every namespace, 512-byte or 4 KB blocks.
    - SCSI (`scsi.c`): INQUIRY / REPORT LUNS discovery, READ CAPACITY(10/16), READ/WRITE(10/16),
      SYNCHRONIZE CACHE, over these host adapters:
      - LSI Logic Fusion-MPT (`mpt.c`): 53C1030 and SAS1064/1068, i.e. VMware's and VirtualBox's
        "LSI Logic" and "LSI Logic SAS" (doorbell handshake, request/reply FIFOs).
      - VMware Paravirtual SCSI (`pvscsi.c`).
      - virtio-scsi (`virtio.c`).
    - virtio-blk (`virtio.c`). Both virtio drivers handle the virtio 1.0 and the legacy PCI transport.
    - Disks with 4 KB sectors are read and written with read-modify-write of partial blocks.
  - MBR partition tables.
  - **Read/write FAT32** driver (in Rust: `rust/src/fat32.rs`): format, long file names,
    create/replace, recursive delete, rename/move, in-memory FAT with write-back.
  - **Read/write NTFS** driver (in Rust: `rust/src/ntfs.rs`), written from scratch for NTFS 3.1:
    - Format: boot sector and its backup, `$MFT` and `$MFTMirr`, an empty `$LogFile`,
      `$Volume`, `$AttrDef`, `$Bitmap`, `$Boot`, `$BadClus`, `$Secure`, `$UpCase`, `$Extend`.
    - 1 KB MFT records and 4 KB index blocks with update-sequence fixups; resident and
      non-resident attributes with run lists; files keep `$STANDARD_INFORMATION`,
      `$FILE_NAME` (Win32 names), a security descriptor and `$DATA`.
    - Folders are `$I30` B-tree indexes (`$INDEX_ROOT`, `$INDEX_ALLOCATION`, `$BITMAP`),
      rebuilt as a balanced tree when they change.
    - Create/replace, mkdir, rename/move, recursive delete, read; GRUB boots from it through
      its `ntfs` module.
    - Volumes it creates pass `ntfsfix`, and `ntfs-3g` lists and reads every file.
  - `rust/src/kernel/volk.rs` puts FAT32 and NTFS behind one interface for Setup and persistence.
  - Read-only FAT12/16/32 reader for disk images.
  - DEFLATE/zlib decompressor and UDIF `.dmg` parser.
- **Files:** in-memory VFS with drive letters (in Rust: `rust/src/kernel/vfsk.rs`), filled from a tar initrd (live CD) or from the
  system partition (installed). `rust/src/kernel/persistk.rs` writes changes back to the disk
  and keeps the settings file.

### Linux programs (`rust/src/kernel/lx`, 64-bit edition)
The first step towards running WPE WebKit as a browser on THE WRLD OS: unmodified x86-64 Linux
programs run in ring 3 and call the kernel with Linux's system call numbers.
- **Address space:** programs live above 512 GiB, so the kernel's identity map of the low
  4 GiB stays untouched; pages are allocated on first touch (`uvm.c`, `pmm.c`).
- **Loading:** position-independent executables (the default on current distributions) and
  the dynamic loader they name, with a Linux start-up stack (argv, environment, auxv) (`proc.c`).
- **Files:** `/` is `C:\Linux`, `/mnt/c` is `C:\`, `/lib`, `/bin` and so on follow the merged
  `/usr` layout, and `/dev/null`, `/dev/zero`, `/dev/urandom` and `/dev/tty` are built in
  (`fs.c`).
- **`/proc`:** `/proc/self/{maps,stat,status,cmdline,fd}` (the fd directory lists descriptors,
  `open()` on an fd link duplicates it, and `readlink` returns its path), `/proc/self/exe`, and
  `/proc/{meminfo,cpuinfo,uptime,version}`, all generated per process when opened.
- **`/sys`:** a small tree for the files programs probe: `devices/system/cpu/{online,possible,
  present}` and `kernel/{osrelease,ostype,version}`.
- **Limits and `prctl`:** rlimits are per process and inherited across `fork`/`execve`;
  `RLIMIT_NOFILE` is enforced when descriptors are allocated and `RLIMIT_FSIZE` on writes.
  `prctl` implements `PR_SET_NAME`/`GET_NAME`, `PR_SET_PDEATHSIG` (delivered when the parent
  exits), `PR_SET_DUMPABLE`, `PR_SET_NO_NEW_PRIVS`/`GET_NO_NEW_PRIVS` and `PR_GET_TID_ADDRESS`;
  unsupported options, including `PR_SET_SECCOMP`, return `EINVAL` instead of pretending to
  work.
- **System calls:** files, directories, mmap/brk/mprotect, time, thread-local storage, futexes,
  identity and limits (`syscall.c`); kernel code stays cooperative, and a program running in
  ring 3 is preempted by the timer.
- Type the program's path in the Command Prompt, for example
  `C:/Linux/usr/bin/hello` or `C:/Linux/usr/local/bin/signals`. The 64-bit ISO carries test
  programs from `samples/linux` (`hello`, `threads`, `ipc`, `procs`, `signals`, `socket`,
  `sysinfo`); they are
  built against **musl** as static PIE when `musl-gcc` is installed, so nothing else is needed, or
  against the build machine's glibc (copied in) when only gcc is available.
- Threads: `clone()` with the flags glibc's `pthread_create` uses (new TLS, child TID cleared
  and woken on exit), so mutexes, condition variables and `pthread_join` work.
- **Processes:** `fork`, `vfork` and `execve` (a new image and start-up stack in the same pid),
  and `wait4` with `WNOHANG`, so a shell can start a program, wait for it and collect its exit
  status. `getppid` reports the real parent.
- **Pipes:** `pipe` and `pipe2` over a 64 KiB ring buffer, with `O_NONBLOCK` and EOF when the
  last writer closes.
- **Sockets (`ipc.c`):** `AF_UNIX` stream, datagram and seqpacket sockets, `socketpair`,
  `bind`/`connect`/`listen`/`accept`, `sendmsg`/`recvmsg` with `SCM_RIGHTS` descriptor passing.
- **Network sockets:** `AF_INET` TCP and UDP over the kernel stack in `src/net`, with
  `send`/`recv`/`sendto`/`recvfrom`, `getsockname`/`getpeername`, `getsockopt`/`setsockopt`
  (`SO_REUSEADDR`, `SO_ERROR`, `SO_TYPE`, buffers, `TCP_NODELAY`) and non-blocking mode.
  `getaddrinfo` works through the distribution's glibc: `/etc/hosts`, `/etc/host.conf`,
  `/etc/nsswitch.conf`, `/etc/resolv.conf` (from the DHCP lease) and `/etc/services` are created
  in the RAM file system on first lookup.
- **Event loop:** `epoll_create1`/`epoll_ctl`/`epoll_wait`, `eventfd` and `timerfd`, and `poll()`
  now asks each pipe, socket, eventfd and timerfd whether it is ready instead of answering
  "always ready".
- **Signals (`signal.c`):** POSIX dispositions per process, pending sets per process and per
  thread, and a real x86-64 `rt_sigframe` (a `ucontext` with 23 registers and a 128-byte
  `siginfo`) handed to a handler in ring 3, so glibc's `__restore_rt` and `rt_sigreturn` work
  unchanged. `sigaltstack`, `SA_RESTART`, `SA_NODEFER`, `SA_RESETHAND`, `SA_SIGINFO`,
  `sigsuspend`, `sigpending`, `sigtimedwait`, `alarm`/`setitimer`, `SIGCHLD` from a child that
  ends, `SIGPIPE` from a pipe or socket with no reader, and dispatch of `SIGSEGV`/`SIGFPE`/
  `SIGILL`/`SIGBUS`/`SIGTRAP` to a handler installed for them. `kill`, `tkill` and `tgkill`
  deliver to a whole process or to one thread.
- **C++ and the platform stack:** `C:/Linux/usr/local/bin/cxxtest` runs the C++ runtime
  (exceptions, threads, iostreams, `std::filesystem`) and `platform` runs GLib's main loop,
  `g_spawn` child processes, GIO, Fontconfig, FreeType and Cairo (it draws `C:\cairo-test.png`).
  Both use the distribution's own glibc, `libstdc++` and libraries, copied in by the build.
- **WebKit's JavaScript engine:** built with `make ARCH=x86_64 LX_WEBKIT=1`, the image also
  carries JavaScriptCore from WebKitGTK (about 70 MB with ICU) and `jsctest`, which runs modern
  JavaScript through it, JIT included. Give the VM 1 GB for that image.
- **Desktop windows:** a Linux program can open `/dev/wrld/window`, create a window, map its
  pixels (32-bit ARGB, as Cairo draws them), present frames and read mouse, keyboard, resize and
  close events (`samples/linux-gnome/wrldwin.h`). `C:/Linux/usr/local/bin/wrldpaint` is a Cairo
  demo: click to drop dots, type to write, Esc to quit.
- **Shared memory:** `memfd_create` (with seals), `shm_open`/`shm_unlink` under `/dev/shm`,
  and `MAP_SHARED` of those or of anonymous memory share real pages between processes, across
  `fork` and through descriptors passed with `SCM_RIGHTS` (`shm` tests all of it).
- **The WRLD Browser on WebKit.** The 64-bit edition's browser is WebKitGTK 6 (the engine of
  Safari and GNOME Web), part of the default build (`make LX_WEBKITGTK=0` leaves it out; the
  image is about 290 MB and needs about 2 GB of memory). It is dressed like a 2007 desktop
  browser: round Back and Forward buttons, the address bar with Refresh and Stop and a search
  box, a menu bar (File, Edit, View, Favorites, Tools, Help), tabs sharing their row with the
  Favorites buttons and the command bar (Home, Page, Tools), and a status bar with the zone and
  a zoom menu; icons are drawn by the browser itself. Find (Ctrl+F), zoom, Delete Browsing
  History, downloads into My Documents\Downloads, cookies in the user's profile. The built-in engine is used on the 32-bit
  edition, on smaller machines, or when the Classic engine is chosen in Internet Options.
- **CDs, DVDs, Blu-ray discs and USB flash drives** become drive letters by themselves, as in
  Windows: CD/DVD drives on IDE, SATA (AHCI), SCSI (LSI, PVSCSI, virtio-scsi) and USB, and USB
  sticks and card readers. A disc put in, or a stick plugged in, is mounted within two seconds and
  **AutoPlay** offers to play its music or video, show its pictures or open it in Explorer; a disc
  taken out, or a stick pulled, takes its letter with it. My Computer lists them with their own
  icons, and Eject (right-click) opens the drive or lets a stick go. The file systems are read
  by `rust/src/media.rs`: ISO 9660 with Joliet and Rock Ridge names, UDF 1.02 to 2.60 (DVDs and
  Blu-ray discs, including the metadata partition of UDF 2.50+), FAT12/16/32 with long names and
  exFAT, whole-disk or in MBR/GPT partitions. Only names and sizes are read at first: a file's
  contents are read from the medium the first time it is opened. Removable drives are read-only.
- **Without a mouse.** Everything can be reached from the keyboard, as in Windows: Tab and
  Shift+Tab move between the controls of a window (a dotted rectangle shows which), Space and
  Enter press them, the arrows move sliders, lists and drop-downs, and Esc closes a dialog; Alt
  or F10 opens the menu bar (the arrows move through it); the Windows key opens the Start menu
  (arrows, Enter, Esc, a letter jumps), Win+D shows the desktop and the arrows then pick an icon,
  Win+E opens Explorer, Win+R Run, Win+F Search; Alt+Tab switches windows. **Mouse Keys** lets the
  numeric keypad move the pointer (8 2 4 6 and the diagonals, 5 clicks, + double-clicks, 0 holds
  the button, . lets go); it turns itself on at start when no mouse is found, and Left Alt + Left
  Shift + Num Lock or Control Panel > Ease of Access turns it on and off.
- **The Start menu** is built from folders, as in Windows XP, so a program that adds a shortcut
  appears in it: `Start Menu\Pinned` holds the pinned programs at the top left, and
  `Start Menu\Programs` (in `C:\Documents and Settings\User`) is All Programs, its folders
  (Accessories, Games, System Tools...) opening as cascading menus. Under the pinned programs are
  the ones started most often from the menu. Right-click a program for Pin to Start menu, Unpin
  and Remove from This List. A shortcut (`.lnk`) holds a command: an application name, a path, or
  an application and its argument (`control ncpa.cpl`).
- **Network Connections** (Control Panel > Network and Internet > View network connections, the
  Network and Sharing Center's "Change adapter settings", or `control ncpa.cpl`) is Windows 7's
  window: the adapter as a tile with its state and device name, and the command bar (Disable or
  Enable, Diagnose, Rename, View status, Change settings). Double-click (or Enter) opens the
  **Local Area Connection Status** dialog: IPv4/IPv6 connectivity, media state, duration, speed,
  bytes sent and received, with Details... (address, mask, gateway, DHCP server and lease, DNS),
  Properties, Disable and Diagnose. Properties lists the items the connection uses; Internet
  Protocol Version 4 (TCP/IPv4) Properties sets DHCP or a fixed address and DNS server. Disable,
  the fixed address and the name are saved; `ipconfig` shows them.
- **Internet Options** (Control Panel > Network and Internet, the browser's Tools menu, or
  `control inetcpl.cpl`) is the Windows dialog with its seven tabs, each wired to a real setting:
  General (home page with Use current/default/blank, Delete Browsing History), Security
  (JavaScript, the trusted root certificates), Privacy (cookies: accept all, block third-party,
  block all; the pop-up blocker), Content (the ad blocker and its allowed sites, the certificates
  folder), Connections (network status, an HTTP proxy), Programs (WebKit or the classic engine)
  and Advanced (smooth scrolling, pictures, reset). OK and Apply save them in `wrld.ini`; a
  running WebKit browser picks them up at once. Cookies, pop-ups, the proxy and smooth scrolling
  apply to WebKit; the ad blocker to the classic engine; the rest to both.
- **Heavy pages.** The WebKit browser keeps more of a page in memory rather than redoing work
  (the "web browser" cache model: decoded pictures, the back/forward cache), within a limit set
  from the computer's memory (a third, at most 1.5 GB a process) past which WebKit drops caches
  before it could run the system out. Linux programs now see the computer's real memory size
  (they were told 256 MB). The compositor gives a large window 30 frames a second, a small one
  60, and an inactive or minimized one 4.
- **Browser speed.** Setting a `timerfd` clears its unread expirations, as on Linux: the
  compositor and WebKit re-arm timers without reading them, and a stale count kept their event
  loops spinning (948,000 system calls in 79 seconds while showing a page, now 52,000, and the
  page appears about twice as fast). The clocks have sub-millisecond resolution (the time-stamp
  counter since the last 1 ms tick), and finding room for a new mapping is one pass over the
  memory regions.
  Video and audio go through GStreamer: WebM and MP4 with VP8, VP9, H.264 (baseline profile),
  Opus, AAC, Vorbis, FLAC and MP3, both as plain `<video>`/`<audio>` and through Media Source
  Extensions, which is how YouTube plays. YouTube sends VP9 video with Opus sound. AV1 is left
  out on purpose: the only AV1 decoder available (libaom) is too slow in software, and YouTube
  picks AV1 whenever the browser offers it, which left videos stalled and the computer busy
  (FFmpeg is left out for size). There is no GPU driver for Linux
  programs, so pages are drawn on the CPU, and composited ones (videos, some animations) reach the
  window through shared memory. Sound plays through `/dev/dsp` (the Open Sound System interface),
  which feeds the OS mixer alongside the desktop's own sounds; `dsptone` in /usr/local/bin is a
  short test tone. On processors with AVX, the OS saves the full AVX state (XSAVE) for programs.
- **Wayland, GTK 4 and WebKit.** `wrldcomp` is a Wayland compositor that shows each Linux
  program's window on the desktop. It tells programs the desktop's real size (and the new one
  after a change in Display settings) and when their window is maximized, so a maximized
  browser fills the whole screen at any resolution. `make ARCH=x86_64 LX_GTK=1` adds GTK 4 and `gtkhello`;
  `make ARCH=x86_64 LX_WEBKITGTK=1` adds WebKitGTK and `wrldweb`, a small WebKit browser
  (needs a 3 GB VM): `C:/Linux/usr/local/bin/wrldcomp /usr/local/bin/wrldweb [address]`.
  - **Ad blocker:** WebKit's content blocker never requests about 165 ad and tracking servers
    or the paths YouTube serves its ads and ad reports from, and hides ad slots
    (`tools/gen_adblock_rules.py` makes the rules from the same lists as The WRLD Browser's
    blocker; your own `C:\WRLD\adblock.txt` is added: `example.com` blocks a server, `##.class`
    hides, `@@example.org` allows a site). On YouTube a page script removes the ads from the
    player's data before the player reads them, skips any ad that still starts, and removes
    the "ad blockers are not allowed" dialog, resuming the video. Internet Options' "Block
    advertisements and trackers" turns it on and off.
  - **Downloads** ask as Internet Explorer does: *File Download - Security Warning* (name, type,
    size and server; Run, Save, Cancel), then the *File Download* window with a progress bar,
    the time left, where the file goes and the transfer rate, "Close this dialog box when
    download completes", and Open / Open Folder. Run keeps the file in `C:\WRLD\Temp\Downloads`
    and opens it when it is complete.
  - **The WRLD Web** is the search page (`wrld://web`): the search box, the start page and
    words typed in the address bar go there. It asks DuckDuckGo's and Brave's HTML result pages
    at once (`samples/linux-webkitgtk/wrldweb-search.h`, through the browser's proxy when one
    is set), merges the results one from each in turn without duplicates, and shows them on
    its own page; when neither answers it offers links to search on either site directly.
  - **OpenGL ES on the CPU:** pages are painted on the CPU, but some (YouTube's watch page among
    them) make WebKit switch to its composited mode, whose compositor needs a GL context for every
    frame. Mesa's llvmpipe (`libEGL_mesa`, `libgallium`, LLVM; about 190 MB, from the build machine's
    `libegl-mesa0`/`libgles2`) provides one; before, such a page froze after its first frame while
    its scripts kept running. With Mesa every page is composited, as WebKit does by default (a web
    process sets up GL only when acceleration is on); without it a stand-in `libEGL` is used and
    pages are painted without compositing. `export WRLDWEB_ACCEL=0` (or `1`) before starting the
    browser overrides that; the log says which mode it chose. WebGL works too. Under QEMU's emulated
    processor (TCG) llvmpipe is kept to SSE4.1, because its AVX2 code crashes there.
  - **A page that stops responding** shows a bar above it: *Wait* keeps waiting, *Reload page*
    stops that page's web process and loads it again, so one stuck page no longer leaves the
    browser frozen. The browser writes its messages (with GStreamer's warnings, for video) to
    `C:\WRLD\Temp\wrldweb.log`.
  - **Clipboard and drag and drop** go through `wrldcomp` (wl_data_device,
    `samples/linux-wayland/wrldcomp-data.h`): a copy in a program becomes THE WRLD OS's
    clipboard and the other way round, drags inside a program work, and files dropped on the
    window from the desktop arrive as a drop of file links (WebKit does not yet turn them into
    files for the page). The window device gains requests for
    these (`WRLDWIN_CLIP_SET/GET`, `WRLDWIN_DROP_GET`) and `WRLDWIN_OPEN`, which asks THE WRLD
    OS to open a file as a double-click would.
  `lxthreads` lists every Linux thread with its CPU time and the system call it waits in (saved to
  `C:\WRLD\Temp\lxthreads.txt`; run it twice on a frozen program). Variables given to `export`
  reach the Linux programs the Command Prompt starts.
  `lxtrace on` in the Command Prompt logs every Linux system call to the serial port (with
  the path of calls that take one), and
  `guiprof on` logs how long each window takes to paint, every 2 seconds.
- **wrldcomp** releases a window's old pixel mapping with that mapping's own length when the
  window changes size (it used the new, larger size, which freed neighbouring memory and
  crashed the compositor under programs that resize while starting).
- **Software packages (.deb and .rpm).** Double-clicking a `.deb` (Debian, Ubuntu, Mint) or an
  `.rpm` (Fedora, openSUSE, Red Hat) opens the Linux Package Installer (`src/apps/pkg.c`,
  `pkgui.c`, reading packages in `rust/src/pkgfmt.rs`): it shows what the package is and checks what it needs before installing:
  the packages it depends on (those installed and those the image carries, listed at build time
  in `C:\Linux\var\lib\wrld\base-packages`), the shared libraries its programs load, the
  library versions they were built against (`GLIBC_2.39`...) and their script interpreters.
  Files go under `C:\Linux`, are recorded in `C:\Linux\var\lib\wrld\packages`, and come out
  again under Control Panel, Programs and Features. Programs with a menu entry (a `.desktop`
  file) appear under All Programs and start through `wrldcomp`; commands go on the Command
  Prompt's PATH. In the Command Prompt: `dpkg -i/-r/-l/-L/-I/-c`, `rpm -i/-e/-qa/-ql/-qip`,
  `apt install ./FILE.deb`. Packages may be gzip, xz or Zstandard compressed (XZ Embedded and
  the zstd reference decoder, in `third_party`). Not done: setup scripts (`postinst`, `%post`)
  are not run, apart from their `update-alternatives --install` lines; symbolic links are
  installed as copies; and nothing is downloaded: dependencies are installed from their own
  package files.
- **/bin/sh.** BusyBox (from the build machine's `busybox` package) is `/usr/bin/sh`, with its
  commands (`ls`, `sed`, `grep`, `awk`, `tar`, `vi`...): each is a small `#!/usr/bin/busybox`
  stub, since the file system has no symbolic links, and the Linux layer runs `#!` scripts with
  their interpreter. So package scripts, `system()` and `popen()` work, and
  `C:/Linux/usr/bin/sh` in the Command Prompt is an interactive Linux shell. Linux scripts under
  `C:\Linux` started from the Command Prompt run with their own interpreter.
- File locks and storage: `fcntl` record locks, OFD locks and `flock` are enforced between
  processes, `fsync` writes a file on C: through to the disk, and `MAP_SHARED` of an ordinary
  file is shared between processes and written back to it. So **SQLite** works, WAL mode
  included, as do **libjpeg-turbo** and **libpng**: `C:/Linux/usr/local/bin/mediatest` in the
  Command Prompt checks all three (samples/linux-media).
- Not yet: job control (`SIGSTOP`/`SIGCONT`, `WUNTRACED`), `signalfd`, IPv6 sockets and a
  loopback interface. See `ROADMAP.md` for what is left before WebKit becomes the browser.

### USB (`src/usb`)
- In **both editions** (`-DWRLD_NO_USB` builds a kernel without it).
- **Taking over from the BIOS:** every controller is handed over properly before it is reset, so
  the firmware's USB keyboard emulation (SMIs) stops cleanly: the xHCI and EHCI legacy support
  capabilities (BIOS/OS semaphores, SMI enables cleared, up to a second for the BIOS to let go),
  OHCI's ownership change request, and UHCI's LEGSUP register. On Intel 7- and 8-series chipsets
  (Panther Point, Lynx Point, Wildcat Point) the ports shared between EHCI and xHCI are switched to
  xHCI before anything is probed.
- **Split transactions on EHCI:** full- and low-speed devices behind a high-speed hub, including
  the hub built into Intel chipsets from about 2010 (which have no companion controllers, so every
  keyboard and mouse sits behind it), are reached through the hub's transaction translator. Each
  device address keeps its speed, translator hub and port, and endpoint 0's packet size.
- **Host controllers:** all four kinds a PC uses. **xHCI** (`xhci.c`, USB 3.x, what every
  machine has had for the last decade), **EHCI** (`ehci.c`, USB 2.0 high speed),
  **UHCI** (`uhci.c`, USB 1.1 on PIIX/ICH chipsets) and **OHCI** (`ohci.c`, USB 1.1 on
  non-Intel chipsets and add-in cards). They are all driven by polling: xHCI through command,
  event and per-endpoint TRB rings; EHCI through queue heads and qTDs on the asynchronous and
  periodic schedules; UHCI through a frame list of queue heads and transfer descriptors; OHCI
  through endpoint descriptor queues on its control, bulk and periodic lists. Every controller
  found is enumerated, and several can be active at once.
- **Enumeration:** port reset and enable, `GET_DESCRIPTOR`, `SET_ADDRESS` (on xHCI the address
  lives in the slot context), full configuration parsing (interfaces and endpoints) and
  `SET_CONFIGURATION`, plus one level of **hub**: the hub's ports are powered and reset and the
  devices behind them enumerated too, with the xHCI route string filled in. Devices are
  **hot-pluggable**: the buses are rescanned every half second, a root port whose connection bit
  has cleared in two scans running has its device (and everything behind its hub) dropped, and a
  port that has something on it but no device is reset and enumerated — three times, and then
  left alone until it is unplugged, so a device this kernel has no driver for is not reset every
  few seconds. A hub's ports are asked the same way; a hub that stops answering is left alone for
  five seconds instead of stalling input. A removed xHCI device's slot is disabled and its rings
  and context freed, so both the slot and the USB address can be used again; addresses are the
  lowest free ones, found by scanning the live devices. A removed USB disk leaves the block
  layer (its number goes to the next disk), and its structures stay allocated so a mounted volume
  that still holds it fails its reads instead of reading freed memory. Waits for a controller are
  spent halted rather than polling, so a transfer that is slow or never answered does not burn the
  CPU.
- **HID** (`usb.c`, report formats in `rust/src/usbdesc.rs`): every HID interface of a device becomes its own keyboard or mouse, so a
  keyboard with a media-key interface, or a wireless receiver that carries both a keyboard and a
  mouse, works. The report descriptor is read to find the buttons, X, Y and wheel (any width,
  with or without a report ID), and a mouse is read in that format; a keyboard uses the boot
  protocol, or its descriptor (a key array or an N-key rollover bitmap) when it has no boot
  interface. Absolute pointers (tablets) are not driven. Reports are translated to PS/2 set 1
  scancodes and packet deltas, so the existing keyboard and mouse code (modifiers, Caps Lock,
  speed, button swap) works unchanged; a held key repeats in software, because a USB keyboard
  does not repeat by itself. A boot report has a fixed length — a keyboard's eight bytes (nine
  if it still starts with a report ID), a mouse's three or four — and a report of any other
  length is refused, so another interface's bytes cannot be read as keys or buttons. A
  report-protocol format is read at the offsets its descriptor gives, so a padded report is
  still its device's (only the named bits are read); when the reports carry a report ID, both
  the ID and the length must match the descriptor. Two interfaces of one device may name the
  same endpoint: the first moves it and the rest listen, and every report is routed to the one
  keyboard and the one mouse whose format it fits (a report ID or a boot report's length
  decides; when both fit only loosely the length still does — eight bytes or more is a
  keyboard's report, four or fewer a mouse's — and otherwise the endpoint's owner), so a
  receiver's keyboard and mouse both work on one pipe and neither is fed the other's bytes. A
  keyboard is asked for a report every half second, so a held key keeps saying it is held; the
  repeat stops, and the key is let go, after three seconds in which no report confirmed it. A
  device whose interrupt endpoint has never delivered anything after a second is polled with
  `GET_REPORT` instead, asked for exactly the report length its descriptor gives (asking for
  more can make a real device stall), and an answer of any other length is not that report and
  is refused. A device with more than one HID interface is not asked this way: such a device can
  answer every interface with the same report, and that report is then not the interface's — a
  receiver answered a keyboard and a mouse interface with one vendor report, whose bytes read as
  buttons nobody pressed. Their own endpoints deliver the real reports. An interface that has
  delivered a report of its own is trusted alone, and an answer identical to the last report
  taken is a movement already counted: the buttons are applied, the movement is not.
- **Mass storage:** bulk-only transport with CBW/CSW and the SCSI commands `INQUIRY`,
  `READ CAPACITY(10)`, `READ(10)`, `WRITE(10)` and `SYNCHRONIZE CACHE(10)`. The disk registers in
  the block layer like any other, so Setup and the volume code see it as a normal disk.
- Tested in QEMU with each controller: `qemu-xhci`, `usb-ehci`, `piix3-usb-uhci` and
  `pci-ohci`, with a USB keyboard, mouse and disk. One script (log on with a mouse click, open
  Command Prompt from the taskbar, type a line and edit it with the arrow, Home, End and Delete
  keys) gives the same screen with USB devices on each of the four, and behind a hub on xHCI, as
  it does with PS/2. Mice with a report ID, 16-bit or packed 12-bit axes, N-key rollover
  keyboards and multi-interface devices are checked against the descriptor parser on the build
  machine (`make check-hid`), since QEMU has no such devices. A USB disk reads and writes
  sectors back unchanged. With more than one controller in the same machine, each enumerates
  its own devices. Unplugging and replugging a keyboard, a mouse, or a hub's device is detected
  and re-enumerated on all four controllers (the boot screen comes back with the devices'
  reports). EHCI handles high-speed devices itself and hands full/low-speed ports to the
  companion; it does not implement split transactions, so a full-speed device behind a
  high-speed hub is not seen, and isochronous transfers are not implemented.
- The log-on screen always names every controller it found, the devices it enumerated and the
  last failures, and keeps a live line per device — its address, interface and endpoint, whether
  it uses the boot protocol or its own report format (and the length and report ID of that
  format), vendor and product, interrupt polls, reports they gave, `GET_REPORT` answers, the key
  or buttons and bytes of its last report, and how many reports were refused as another
  interface's format. Below it is the last few input events, each named with the interface that
  produced it (`kbd@10.0 04+` is a key press, `mouse@10.2 b1 3,-2` a move with the left button),
  so a photo of the screen says which device moved the pointer or pressed a button nobody
  touched, and what bytes it sent.

### Networking (`src/net`)
- **Network cards** (all polled; the first one found is used):
  - Intel PRO/1000 8254x, 8257x (VMware's e1000e is an 82574L), ICH8-10/PCH and I217/I218/I219 (`e1000.c`).
  - Intel 82575/82576/I350/I210/I211 (`igb.c`), with advanced descriptors.
  - Realtek RTL8111/8168, RTL8169, RTL810x and RTL8125 (`rtl8169.c`), and RTL8139 (`rtl8139.c`).
  - AMD PCnet-PCI II/FAST III (`pcnet.c`), VMware VMXNET3 (`vmxnet3.c`), and virtio-net
    over the modern or legacy transport shared with the virtio disks (`virtionet.c`).
  - Tested in QEMU on both editions. On real hardware, the Realtek and I217/I218/I219 drivers
    leave the PHY as the firmware set it up, so a board whose firmware powers the port down may
    show "cable unplugged".
- **Protocols:** Ethernet, ARP, IPv4, ICMP, UDP, DHCP client, and a DNS resolver (with
  `C:\WRLD\hosts`). The stack (these and TCP) is in Rust: `rust/src/netstack.rs`, with the
  packet parsers in `rust/src/net.rs`; see [Rust in the kernel](#rust-in-the-kernel-rust).
- **TCP:** client and server side, with MSS, a 64 KiB window, retransmission with back-off,
  graceful close, and non-blocking connects for the socket layer.
- **HTTPS (TLS 1.3 and 1.2):**
  - [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 4.1 with TF-PSA-Crypto
    (`third_party/mbedtls`), built freestanding against the same small C library as QuickJS
    and configured as a TLS client only (`third_party/mbedtls-config`). The glue is `src/tls/tls.c`.
  - TLS 1.3 and 1.2 in one handshake; ECDHE over X25519, P-256, P-384 and P-521;
    AES-GCM and ChaCha20-Poly1305; RSA, RSA-PSS and ECDSA certificates; SNI and ALPN.
  - Certificates are checked against the 117 roots of the Mozilla root store (generated by
    `tools/gen_root_certs.py`), plus any `.crt`/`.pem`/`.der` you put in
    `C:\WRLD\Certificates`. Roots are parsed only when a server's chain names them, so they
    cost little memory.
  - Mbed TLS's random generator is seeded from RDRAND and timing jitter, conditioned with SHA-256.
- **HTTP/1.1 client:** GET and POST, redirects, chunked transfer, gzip/deflate.

### The WRLD Browser (`src/apps/browser.c`; documents: `rust/src/html.rs`, `css.rs`; images: `rust/src/imgdec.rs`)
- **Window:** up to 12 tabs per window, each with its own page, history and loading task, in
  the same 2007 look as the WebKit browser: round blue Back and Forward buttons, the address
  field with Refresh and Stop at its end (the lock shows on secure pages; focusing it selects
  the address; a progress line while loading), a search box (DuckDuckGo), the Favorites star,
  IE-style tabs with a small empty tab for a new one, Home, Page and Tools menus, and a status
  bar with the ad blocker's count and the zone. Numeric addresses and ones with a port open
  over plain HTTP.
- **CSS boxes:** `margin` (including `auto` to centre a box), `padding`, `border` /
  `border-width` / `border-color`, `width` and `max-width` in px, em, pt or %, on blocks and on
  `<body>`; borders without a background are drawn as outlines.
- **Flexbox and grid:** `display: flex` with `flex-direction` (and `-reverse`), `flex-wrap`,
  `justify-content`, `align-items`/`align-self`, `gap`, and `flex`/`flex-grow`/`flex-shrink`/
  `flex-basis`; `display: grid` with `grid-template-columns` (a track count, `repeat(N, ...)` or
  `repeat(auto-fill, minmax(...))`). Items are made blocks, as in the specification. Nested
  containers are measured with a depth guard, so deep component trees stay fast.
- **CSS variables and maths:** `--name: value` custom properties with `var(--name, fallback)`
  (values set on `:root`, `html` or `body` win, as on real sites; linked sheets are parsed again
  so their variables reach earlier sheets), and `calc()`, `min()`, `max()` and `clamp()` for
  lengths. `height`/`min-height`, CSS `width`/`max-width` on images, and `text-decoration: none`
  on links.
- **Inline boxes, floats and positioning:** an inline element with padding, a border, a rounded
  background or `display: inline-block` (badges, tags, link buttons) is one box on the line,
  aligned on its text's baseline. `float: left/right` takes the space beside the following text,
  which wraps around it; `clear` moves below; a box encloses floats that start in it (what
  clearfix does). `position: absolute/fixed` boxes leave the flow and sit at their offsets in the
  nearest positioned ancestor; `relative` shifts a box. Visually hidden helpers (`left: -9999px`,
  1x1 screen-reader text) are not drawn.
- **SVG:** inline `<svg>` and `.svg` pictures (also `data:image/svg+xml`) are drawn through the
  path rasteriser (`rust/src/svg.rs`): paths with every command including arcs, rects, circles,
  ellipses, lines, polylines, polygons, groups, `<use>`/`<symbol>`, transforms, fill and stroke
  (from attributes, `style` or CSS `fill`), `currentColor` and opacity; a gradient fill uses its
  first colour. Text, filters, masks and clip paths are not drawn.
- **Linked style sheets:** `<link rel="stylesheet">` files are fetched (HTTP, HTTPS or `file:`)
  in document order before the first layout, filtered by the ad blocker, and cascaded with the
  page's `<style>` sheets.
- **Parsing:** Lexbor 3.0's HTML5 parser (`third_party/lexbor`, glue in `src/lexbor/lexglue.c`)
  parses the page and `rust/src/html.rs` builds the browser's tree from it, so misnested tags, tables and entities come out as in other browsers;
  `innerHTML` parses fragments in the context of their element.
- **CSS:** full selectors (combinators, attribute selectors, `:not()`, `:is()`, `:nth-child()`
  and the other structural pseudo-classes), parsed by Lexbor and matched on the browser's own tree,
  with the cascade by specificity and source order; simple `@media` queries are answered for an
  880-pixel-wide window. `querySelector()` uses the same matcher.
- **Software rasterisation:** `border-radius`, linear gradients (`linear-gradient(90deg, …)`) and
  radial gradients (`radial-gradient(…)`) are filled through the kernel's own path engine
  (`src/gui/raster.c`), not a rectangle approximation.
- **HTML:** a forgiving parser with entities. Documents are decoded to UTF-8 (Windows-1252
  input is converted), so text is stored and drawn exactly as written.
- **CSS properties:** colour, background, `display:none`, alignment, bold, italic, underline,
  size, plus the box properties above, from rules and `style` attributes.
- **Layout:**
  - Text wrapping, headings, lists, blockquote, `pre` and `hr`.
  - Tables: column widths from min/max content, `colspan`, cell colours, borders, `valign`.
  - Images, form controls and in-page `#anchors`.
- **Forms:** text, password, checkbox, radio, select, textarea and submit, sent by GET or POST.
- **JavaScript** (`src/js`, the document interface in `rust/src/dom.rs`):
  - [QuickJS-ng](https://github.com/quickjs-ng/quickjs) 0.18 (`third_party/quickjs`), a
    complete modern JavaScript engine, built inside the kernel against a small C library and an
    x87 math library (`third_party/quickjs-shim`).
  - DOM: `document`, elements and text nodes, `getElementById`, `querySelector(All)` (CSS
    selectors with combinators, attributes and `:nth-child`), `innerHTML`/`outerHTML`,
    `textContent`, `createElement`, `appendChild`/`insertBefore`/`remove`, attributes,
    `classList`, `dataset`, `style`, form values, `document.write`.
  - Events with bubbling: `addEventListener`, `onclick` properties and attributes,
    `preventDefault`, click/input/change/keydown/submit, `DOMContentLoaded` and `load`.
  - `setTimeout`/`setInterval`/`requestAnimationFrame`, Promises, `localStorage`,
    `sessionStorage`, `document.cookie`, `fetch`, `XMLHttpRequest`, `location`, `history`,
    `navigator`, `alert`, `console`.
  - Inline and external scripts run in order before the first layout; each page then keeps a
    script task for events and timers. The page is laid out again whenever a script changes it.
  - Scripts that run long are interrupted regularly so the desktop stays responsive.
  - **Memory guard:** a page's memory (scripts, document, pictures) is counted. When it passes
    **500 MB**, the page is paused and you're asked whether to continue (asked again every
    further 500 MB) or stop it. Allocations are refused before the OS itself runs short.
  - Tools menu: Enable JavaScript, Script Messages (errors and `console` output).
- **Ad blocker** (`rust/src/adblock.rs`):
  - Pictures, scripts and frames from about 140 ad and tracking servers, and ad-server URL
    paths, are never requested.
  - Elements whose class or id marks an ad slot (`ad-banner`, `adsbygoogle`, `div-gpt-ad-…`)
    are hidden.
  - Your own rules go in `C:\WRLD\adblock.txt`: host names (`||host^` also works), `##.class`
    and `###id` hiding rules, and `@@site` to allow a site.
  - The status bar shows how many items were blocked; Tools > Block Ads turns it off, Allow Ads
    on This Site adds an allow rule.
- **Images:**
  - PNG: all colour types, transparency and interlacing.
  - GIF: first frame, with transparency.
  - Baseline and progressive JPEG (multiple scans, successive approximation), and BMP.
  - The Image Viewer uses the same decoders.
- **Window:**
  - Back/Forward/Stop/Refresh/Home, an address bar with a padlock for HTTPS, and a status bar.
  - Page source, Save Page, Favorites.
  - Error pages, `file://` folder listings, and downloads saved to `My Documents\Downloads`.

### Devices and drivers (`src/kernel/devmgr.c`, `src/include/devmgr.h`)
Every driver is part of the kernel, so there is nothing to download or install by hand; the
device manager's job is to say what the computer has and which driver runs it.
- **Driver list:** each driver registers itself with `DRIVER_PCI()` or `DRIVER_REGISTER()`
  (name, description, device class and, for PCI drivers, a function that says whether it
  would take a device). The linker collects the entries into one table. A driver calls
  `dev_claim()` once it has a device working.
- **Detection:** `devmgr_scan()` lists PCI devices, USB devices (including ones with no class
  driver), disks and the fixed PC devices (PS/2 keyboard and mouse, timer, clock, COM1,
  speaker, ACPI). Each gets a hardware ID such as `PCI\VEN_8086&DEV_100E`,
  `USB\VID_046D&PID_C077` or `ACPI\PNP0303`, and one of five states: working; built in (bridges
  and the like need no driver); driver available but not in use (a second network adapter or
  sound card); the driver could not start it; no driver.
- **Device Manager** (Control Panel > Hardware and Sound) shows the devices as a tree by type,
  with the driver, status, hardware ID and location of the selected one, and a second tab with
  every built-in driver and how many devices it runs. It updates by itself when a device is
  plugged in or removed.
- **Commands:** `devices` (`-p` only problems, `-i` with hardware IDs), `driverquery`, `lsusb`.
- **Found New Hardware:** the hardware is remembered in
  `C:\WRLD\System32\config\hardware.ini` (written by Setup's *Installing devices* step). A
  device that is new since the last start, or plugged in while the system runs, gets a balloon
  saying whether its driver is ready.
- To add a driver, write it, describe it with `DRIVER_PCI()` and call `dev_claim()`; it then
  appears in all of the above without further changes.

### Setup (`src/apps/setup.c`)
- **Graphical install:** the default Setup. Detailed disk/partition list with numbers, letters,
  sizes and file systems, New/Delete with confirmations, a file-system choice, a review page
  with an "I understand" checkbox, and graphical format/copy progress. It works from the boot
  menu and from the live desktop.
- **Text mode:** fallback (`setup=text`), with the same partition editor, NTFS/FAT32 format
  (quick/full), file copy and GRUB install.
- **First boot:** the graphical wizard (devices were installed by Setup). It runs before
  the log-on screen, using the normal widgets.

### Desktop (`src/gui`)
- **3D rendering:** an on/off toggle in
  Control Panel > Visual Effects (saved as `accel3d`) enables both the native CPU engines
  and supported VMware GPU offload. **WRLD 3D** (`src/gui/r3d.c`) is a software engine:
  a fixed-function pipeline like early OpenGL: 4x4 matrices, per-vertex lighting,
  near-plane clipping, a depth buffer,
  perspective-correct texture mapping with bilinear filtering, and alpha blending. Rows are
  filled as exact spans with one perspective division per 16 pixels. Flip 3D, WRLD 3D,
  OpenGL 1.1 and fixed-function Direct3D 9 still use the processor; the separate D3D10
  shader pipeline can use the GPU backend described below. Desktop animations are unchanged.
  - **Flip 3D** (`flip3d.c`): with 3D on, Alt+Tab shows the open windows as a tilted stack of
    cards. Tab, Shift+Tab, the arrow keys or the wheel move through it; releasing Alt, Enter or a
    click picks a window; Esc goes back. Ctrl+Alt+Tab keeps the stack open. With 3D off, Alt+Tab
    switches windows directly.
  - **WRLD 3D** (`src/apps/demo3d.c`, Start > All Programs, or `wrld3d`): a textured, lit
    scene with a frame counter and toggles for lighting, textures and filtering. Under QEMU's
    emulation it draws about 170,000 pixels a frame at about 24 frames/s on the 64-bit edition
    and about 7 on the 32-bit one, which uses x87 floating point.
  - **3D for Windows programs** (`src/win32`): OpenGL and fixed-function D3D9 share WRLD 3D;
    D3D10 has a separate shader pipeline.
    - **OpenGL 1.1** (`opengl32.dll`, `glu32.dll`): WGL contexts (`ChoosePixelFormat`,
      `wglCreateContext`, `SwapBuffers`), `glBegin`/`glEnd` with every primitive type, vertex
      arrays, display lists, the matrix stacks, textures (RGB, RGBA, BGR(A), luminance), one
      directional light, culling, depth and blending; `gluPerspective`, `gluLookAt` and friends.
      For 2D layers such as SDL2's OpenGL renderer there are also the remaining 1.1 pixel calls
      (`glReadPixels`, `glDrawPixels`, `glScissor`, `glCopyTexSubImage2D`), packed pixel formats
      and row-length unpacking, render-to-texture with `GL_EXT_framebuffer_object`, one-unit
      `GL_ARB_multitexture`, and vsync through `WGL_EXT_swap_control` (60 frames/s at most).
    - **OpenGL 2.1 and GLSL** (`src/win32/gl2.inc`, `glsl.inc`, compiler in `rust/src/glsl`):
      shader and program objects (OpenGL 2.0 names and `GL_ARB_shader_objects`), uniforms,
      attributes, buffer objects, vertex array objects and generic vertex attributes. GLSL 1.10
      to 4.x and ES source compiles to the shader IR the Direct3D 10 engine already runs:
      functions are inlined, and the compatibility built-ins are there (`gl_Vertex`,
      `gl_MultiTexCoord0`-`7`, `gl_ModelViewProjectionMatrix`, `gl_NormalMatrix`,
      `gl_LightSource`, `gl_FrontLightProduct`, `gl_FrontLightModelProduct`, `gl_TexCoord`,
      `gl_FrontColor`, `gl_FragColor`, `gl_FragCoord` and the rest). Draws with a program run the
      vertex shader per vertex and the fragment shader per pixel on the processor, with GL's
      depth functions, alpha test, blend factors and equations, colour masks and texture wrap
      modes. Shaders run as machine code (below); a scene that shades over a million pixels a
      frame is still slow on the processor, so 3D games run at a few frames a second at best.
      Not there yet: geometry shaders, shadow comparisons, derivatives, cube maps.
    - **Several processors** (`src/kernel/smp.c`): the other processors of the machine start
      at boot as 3D workers (16 at most; `nosmp` on the kernel command line leaves them
      asleep). The kernel still schedules tasks on the first processor. A GLSL draw is split
      three ways: every processor runs the vertex shader on a share of the vertices, clips and
      sets up a share of the triangles into its own list, then rasterises and shades its own
      bands of 8 rows from all the lists in draw order. A 4-processor virtual machine draws
      about 3.7 times as fast as one.
    - **Four pixels at a time** (`rust/src/jit4.rs`): fragment shaders are also compiled to
      code that shades four neighbouring pixels of a row at once, one per SSE lane, with each
      register kept as component vectors so swizzles cost nothing. Branches, loops,
      `discard` and `return` run under per-pixel masks; texture reads take one call for the
      four pixels (the texture looked up once, the four channels of each bilinear texel
      filtered together with SSE2 integer and float instructions, same bits), and the rare instructions it does not
      compile run pixel by pixel in the interpreter, so results are the same bits as before. The pixel code around it
      (walking the rows, interpolation, tests, blending, texture filtering, clipping and
      triangle setup;
      `src/win32/glsl_px*.inc`) is compiled twice, once for SSE: the 32-bit edition uses
      that copy where SSE2 is. Together with the processors, CoronaKO draws about twice as
      fast again.
    - **Uniform work once per draw** (`rust/src/hoist.rs`): when a GLSL program links,
      what its shaders compute from uniforms alone (a light direction normalised, colours
      added up, the sine of an animation time) is split into a small pre-shader that runs
      once per draw; the per-pixel and per-vertex code reads the results like uniforms. Only
      values every pixel shares are moved (not ones inside branches on per-pixel values, nor
      loop counters), and tests check the split shaders give the same bits as the originals.
    - **Shader machine code** (`rust/src/jit.rs`): Direct3D 10 and GLSL shaders are compiled
      once to x86 SSE code (both editions; the 32-bit one when the processor has SSE2) instead
      of being interpreted, 4 to 11 times faster. Arithmetic, comparisons, logic, conversions,
      rounding, texture sampling and control flow are compiled; the rest (sines, logarithms,
      integer division) calls the interpreter for that one instruction, so results are the same
      bits either way. Host tests compare the two on every shader they run.
    - **Direct3D 9** (`d3d9.dll`, fixed function): `Direct3DCreate9`, devices, `Clear`,
      `Present`, transforms, viewport, material, one directional light, render and sampler
      states, flexible vertex formats (`XYZ` or pre-transformed `XYZRHW`, normal, diffuse, one
      texture set), all four `Draw*Primitive` calls, vertex and index buffers, and 32-bit
      textures with `LockRect`. The COM method tables are generated from mingw-w64's `d3d9.h`
      (`tools/gen_d3d9.py`), so methods that are not implemented return `D3DERR_INVALIDCALL`
      with the stack intact. A few `d3dx9` matrix helpers are exported too. Shaders are not
      supported: `SetVertexShader`/`SetPixelShader` accept only NULL.
    - **Direct3D 8** (`d3d8.dll`): the same device behind D3D8's method tables (generated from
      mingw-w64's `d3d8.h` by `tools/gen_com.py`): D3D8 presentation parameters, caps and
      adapter identifier, `SetVertexShader` with an FVF, `SetIndices` with a base vertex, and
      texture-stage filtering. Textures in both versions take 16-bit (R5G6B5, X1R5G5B5,
      A1R5G5B5, A4R4G4B4), 8-bit (L8, A8, P8 as grey), A8L8, 24/32-bit and DXT1-5 formats,
      converted when they are unlocked.
    - **DirectInput** (`dinput.dll`, `dinput8.dll`; versions 3-8, ANSI and wide): the system
      keyboard and mouse, immediate (`GetDeviceState`) and buffered (`GetDeviceData`) data,
      custom data formats, properties, `EnumDevices`/`EnumObjects`. Input goes only to the
      program whose window has the focus. There are no joysticks: `CreateDevice` answers
      `DIERR_DEVICENOTREG` for them.
    - **DirectSound** (`dsound.dll`): `DirectSoundCreate(8)`, a primary buffer and any number of
      secondary buffers (8/16-bit, mono/stereo, any rate), mixed by a kernel task into one
      sound-card stream with resampling, volume, pan, frequency, looping and position
      notifications. 3D-buffer and listener interfaces are accepted but sound unpositioned;
      capture answers `DSERR_NODRIVER`, as does everything without a sound card.
    - **Direct3D 10 and staged shader extensions** (`d3d10.dll`, `dxgi.dll`,
      `src/win32/d3d10.c`, `rust/src/dxbc.rs`, `dxil.rs`): vertex/pixel shaders use a native CPU
      interpreter or eligible **VMware SVGA3D GPU** offload. SM4.0/4.1 DXBC is joined by
      tested **SM5.0 DXBC** and **SM6.0 / DXIL 1.0**
      subsets. These are WRLD shader-upload extensions, **not D3D11/12 compatibility or a
      higher feature level**; D3D10's vertex/pixel profile queries still return `vs_4_0` /
      `ps_4_0`. Both formats lower to the same native IR. DXIL's LLVM bitstream is parsed
      directly: there is no LLVM runtime, JIT or native-code execution, and no new graphics
      library dependency.
      - SM4 arithmetic, integers, conversions/comparisons, structured if/else/loop, discard,
        relatively indexed constant buffers, immediate constant buffers and indexable
        temporaries remain. SM5 adds reciprocal, half conversions, bit count/reversal/scan,
        bitfields, carry/borrow and conditional swap. The DXIL subset covers scalar f32/i32,
        signature I/O (including packed columns), legacy constant buffers, float Texture2D
        sampling, common math/dot intrinsics, discard and bounded forward branches/two-input
        phi nodes. Modern `SampleLevel`/bias operands must be constant zero; only the base
        mip is rendered. Unsupported instructions, resource kinds/spaces and stages fail
        upload with a diagnostic rather than becoming silent no-ops.
      - A prepared execution context binds operands and allocates registers **once per draw**;
        shader invocations allocate nothing, including large temporary banks. Only destination
        lanes are calculated, float rounding avoids per-component x87 rounding-mode changes,
        and fully written pixel outputs need no clearing. The rasterizer clips the frustum,
        solves top-left-covered scanline spans and steps only live varying components. It
        supports perspective, noperspective and flat interpolation, component-remapped linkage,
        shader-written depth, depth testing, culling, blending and color write masks. Uint
        pixel outputs work with `R8G8B8A8_UINT` / `R32_UINT` targets (no blending); signed
        render targets are not implemented.
      - Swap chains present into windows; buffers, 2D textures, views, input layouts, sampler,
        rasterizer, depth/blend states, `Draw`, `DrawIndexed` and `DrawInstanced` triangle lists
        and strips remain. Missing: geometry/tessellation/compute, waves, UAVs/atomics, doubles,
        DXIL helper functions/backward branches, derivatives, comparison/array/cube/MSAA
        textures, newer shader profiles, stencil, multisampling, line/point primitives, stream
        output, lower mip storage and run-time HLSL compilation (`D3D10CompileShader` /
        `D3DCompile` fail). This is a bounded implementation, not a complete DXIL validator.
      - **Native VMware GPU backend** (`src/kernel/gpu3d.c`, `src/win32/shader_gpu.c`):
        real SVGA3D contexts, surfaces, shader programs, indexed draws, guest-memory DMA and
        completion fences. The shared IR lowers directly to legal legacy SM3 programs,
        including stage-specific comparison lowering and register-read-port limits; no
        Mesa, LLVM, FreeType, Skia or ICU runtime is introduced.
        - Tested with **VMware Workstation 15.0.0 build-10134415**, accelerated 3D enabled,
          **virtual hardware version 7** (`virtualHW.version = "7"`, `mks.enable3d = "TRUE"`,
          `svga.vgpu10 = "FALSE"`). Its DX11 host renderer used an NVIDIA RTX 3050; this is
          **not** a native guest NVIDIA driver. Modern guest-backed-object SVGA3D (including
          the tested hardware-version-16 VM), QEMU's 2D SVGA and unsupported devices keep
          the CPU renderer. A guest-backed-object backend is not implemented.
        - Offload is narrower than shader upload: float vertex/pixel math, bounded if/else,
          perspective-linked varyings, up to 28 original temporaries / 512 emitted instruction
          slots and eight 2D texture/sampler pairs. RGBA8/BGRA8 targets, point/linear sampling,
          common depth/cull/blend states and color masks are supported. Integer/bit operations,
          relative/indexable addressing, loops/discard, larger programs, unsupported linkage,
          formats and states fall back **per draw**, without rejecting CPU-supported shaders.
          The supplied SM4/SM5 cubes offload; the larger SM6 cube correctly falls back.
        - Upload/readback and a rendered triangle/depth self-test must pass before enabling
          offload. CPU color/depth buffers stay authoritative between draws; readback commits
          only after a fence. The legacy target uses D24S8 depth; D32F requests stay on the CPU
          when float depth is unavailable. A bounded 16 MiB staging GMR is retained after a
          timeout so outstanding host DMA cannot access freed memory.
        - `C:\Samples\gpuverify.exe` compares actual GPU and forced-CPU readbacks at four
          fixed poses for both SM4 and SM5, checks completed GPU draw counts, and reports
          **36 checks** through `OutputDebugString`. `--show` displays both images. The tested
          poses have zero pixels exceeding the 24-level RGB error threshold. A missing GPU
          fails this hardware verifier rather than passing a CPU-versus-CPU comparison.
          Synchronous per-draw transfers/readbacks are still costly: **GPU execution is not
          a guaranteed FPS improvement**, especially for small windows/simple shaders.
          In two GPU-stage trials at `a326397` (before Basic Display presentation changes),
          one-vCPU, 512 MiB x64 VMware, the 120-frame 480x360 SM4 cube
          took **1,148–1,158 ms CPU vs 795–901 ms GPU** (paired 1.28–1.44x speedups);
          SM5 took **1,224–1,252 vs 908–1,167 ms** (1.05–1.38x). These include synchronous
          transfers and window presentation, not just shader time, and are not comparable
          to QEMU TCG figures or a general physical-GPU performance claim.
      - `C:\Samples\d3d10cube.exe` defaults to the original SM4 assembly; `--sm5` / `--sm6`
        select genuine FXC/DXC equivalents from `samples/d3d10cube.hlsl`. `--bench` draws four
        warm-up frames followed by 120 deterministic 480x360 frames, logging the elapsed
        milliseconds, the last-draw backend and GPU/CPU draw counts with `OutputDebugString`.
        `--cpu` selects the native CPU renderer even on a supported GPU; combine it with
        `--sm5` / `--sm6` for like-for-like comparisons. In the earlier same-host,
        512 MB, one-vCPU QEMU TCG x64
        comparison (two runs), SM4 improved from **7.97–8.12 to 12.14–13.18 FPS**
        (**1.52–1.62x** paired speedups). SM5 measured **10.72–12.02 FPS** and SM6
        **7.80–8.73 FPS**. These are software-renderer benchmark
        results, not real-hardware/GPU numbers; x86 is compile/host-tested, not re-timed here.
      - `C:\Samples\shaderprobe.exe` checks successful uploads, rejected features and unchanged
        profile queries (16 checks). `make check-shaders` runs native host execution and
        framebuffer, GPU-lowering/reference and FIFO-transport tests using checked-in real
        compiler fixtures; `make bench-shaders` adds
        an interpreter microbenchmark, **not OS frame rate**. Regeneration uses host FXC/DXC
        only; see [`tools/shaders/README.md`](tools/shaders/README.md) for the exact subset,
        limits and commands. The COM tables still come from `tools/gen_d3d10.py`.
    - **Vulkan** is not feasible: it draws only with SPIR-V shader programs. `vulkan-1.dll`
      exists so that programs can ask, and `vkCreateInstance` answers
      `VK_ERROR_INCOMPATIBLE_DRIVER` so that they fall back to Direct3D 9 or OpenGL.
    - With 3D acceleration off, `wglCreateContext`, `Direct3DCreate9` and `D3D10CreateDevice` fail as on a computer
      without 3D support. `glcube.exe` and `d3dcube.exe` in `C:\Samples` run at about 40-50
      frames/s on the 64-bit edition under QEMU and 18-28 on the 32-bit one.
    - Linux programs do not get OpenGL yet (that would be Mesa's software renderer on
      `/dev/wrld/window`).
- **Rendering:**
  - Double-buffered framebuffer renderer (1024×768×32).
  - **TrueType text** (`rust/src/ttf.rs`): a small engine with no library dependencies that reads
    the sfnt tables, Unicode cmaps (formats 4 and 12), composite glyphs and `kern` pairs, and
    fills each glyph with 8 horizontal scanlines per pixel. Faces come from `C:\WRLD\Fonts`
    (Liberation Sans, Serif and Mono, SIL OFL) and can be drawn at any pixel size; rasterisations are
    cached. If the files are missing, or before the file system is up, the original
    anti-aliased bitmaps are used instead.
  - **OpenType layout** in the same engine: GSUB and GPOS (single, multiple, alternate,
    ligature, contextual and chaining substitutions; pair, mark-to-base, mark-to-ligature and
    mark-to-mark positioning), so ligatures, Arabic joining forms and diacritics are placed by
    the font itself. Arabic falls back to the bundled **Noto Naskh Arabic** when Liberation has
    no glyph.
  - **Software rasteriser** (`src/gui/raster.c`): path fills and strokes with eight scanline
    samples per pixel, round joins and caps, nonzero and even-odd rules, path construction
    through move/line/quadratic/cubic commands, linear and radial gradients, and clipping to
    the canvas. The WRLD Browser uses it for rounded boxes, gradient backgrounds and CSS borders.
  - **Bidirectional text** (`src/gui/text.c`): the UAX #9 algorithm (explicit embeddings, weak
    types, brackets, neutrals, implicit levels) plus script itemisation, so a line mixing
    Arabic, Hebrew, Latin and numbers comes out in the right visual order.
  - Text is UTF-8 from the parser to the screen, so the fonts' Latin, Greek, Cyrillic,
    punctuation, arrow and maths coverage all reaches the display.
  - 64 original, procedurally drawn icons (48/32/16 px); the desktop wallpapers are the nine
    photographs in `WRLD/Wallpapers` and the account pictures are the 71 photographs in
    `WRLD/Avatars`, all decoded by the same image decoder as the browser. Each wallpaper can be
    cropped to the screen, scaled to fit inside it or stretched to both edges.
  - **Your own pictures:** **Custom desktop...** in Personalization and **Custom avatar...** in
    User Accounts open a file dialog; any JPEG, PNG, GIF, BMP or SVG becomes the desktop
    background ("Your picture" among the backgrounds) or the account picture (the log-on
    screen, the Start menu and the last tile of the picture grid). A copy is kept in
    `C:\WRLD\Custom`, so the choice survives the original file moving; the wallpaper is now
    saved in `wrld.ini` by name.
- **Branding:** a **globe logo** projected from a hand-drawn world map (`tools/globe.py`). It
  spins on the boot screen and is used for the Start button, About and Setup.
- **Window manager:**
  - Luna title bars with rounded corners and caption buttons.
  - Move/resize, maximize/minimize, double-click title, system menu.
  - Menu bars, popup menus, modal dialogs.
  - Alt+Tab, Alt+F4, Win key, Ctrl+Shift+Esc.
  - **Visual Effects** in Control Panel (also linked from Display and Taskbar): choose
    **Border scale (Windows XP)**, **Aero fade (Vista / 7)** or **None** for opening, closing,
    minimizing, restoring and maximizing windows. Start menu transitions have separate
    **Slide**, **Fade** and **None** choices; sidebar sliding and tile fading can be toggled
    independently. A master switch disables all of these effects without losing the choices.
    Defaults are border scale, Start-menu slide, sidebar slide and tile fade.
  - Effects use timer-based software composition, not sleeps or blocking animation loops.
    Transitions can reverse, and theme/resolution changes and shell recovery settle them.
    Preferences are saved in `C:\WRLD\wrld.ini`; safe mode disables animations.
    `make check-animations` runs host-side checks for easing, reversal, instant effects and
    timer wraparound without starting the OS; `make check-composite` checks scaled alpha,
    clipping, transparent corners and padded scanlines.
    See [desktop appearance checks](docs/appearance-testing.md) for the GUI regression checklist.
- **Shell:**
  - The desktop, taskbar, sidebar, icons and window manager run in a separate
    **`shellmanager.exe`** kernel task started at boot (not a separately loaded or protected
    executable). A small **shellwatch** task restarts it
    if it exits, and the watchdog restarts only the shell loop (not the computer) if it
    stops responding. Administrative Tools and Security Center have a restart button.
  - Taskbar with Start button, Quick Launch, task buttons, volume and network icons, and clock.
  - Two-column Luna Start menu with an All Programs flyout; Classic cascading Start menu with
    a vertical banner.
  - Longhorn-style **sidebar** on the right, toggled by the arrow at the end of the taskbar. Its
    header is the account photo drawn full-bleed with a gradient wipe into the panel, followed by
    removable tiles: Clock & Date, Apps, Search, Shortcuts, Quick Launch, Usage Meter and Memo.
    Search covers apps, files under My Documents/Samples/C: and Control Panel pages. The open
    state and tile set are saved. Tiles have distinct Longhorn-style title strips, framed
    content panels and separators, using the selected style's colours and Classic bevels.
    Drag a tile by its title bar (the grip and the move pointer show where) to give it another
    place: it lifts out with a shadow, the other tiles slide apart to leave a dashed space where
    it will land, and the order is saved.
    Aero keeps translucent glass tile bodies. The panel stays below application windows,
    including during transitions; covering windows receive the mouse input.
  - Tray balloons; desktop icons with context menus.
  - Boot screen, welcome screen, and Turn Off / Log Off dialogs over a desaturated desktop.
- **Logon skins**, all `.wrldlogon` files in `C:\WRLD\Resources\Logon Skins`:
  - **Sign In** (the default): your account on the left of a deep blue glass background,
    with the clock, **Power Off** and **Help and Settings** at the top right and
    **Sign in as Guest** at the bottom left. With a password, the account opens into a panel
    with the password box and a **Sign In** button.
  - **XP Classic:** the previous XP-style screen with the divider down the middle.
  - **Midnight**, **Sunrise** (sign-in layout) and **Olive Classic** (classic layout).
  - **Logon Skin Themer** (All Programs, or Personalization > Welcome screen) shows every skin
    as a thumbnail. You can apply one, preview it full screen, edit it or delete your own.
  - **Logon Skin Maker** (All Programs) makes a skin from scratch or from an existing one:
    - the layout;
    - the title, subtitle and corner text;
    - a background picture;
    - which items are shown;
    - every colour, with sliders, hex values or quick swatches.

    The preview updates as you edit; **Save and use** applies the skin.
  - **Help and Settings > Next skin** on the logon screen switches skins before you sign in.
  - There's only one account, so **Sign in as Guest** only signs in when the account has no
    password. With a password it says there's no Guest account.
- **Visual styles**, all `.wrldstyle` files in `C:\WRLD\Resources\Themes`:
  - **Luna (Blue), Olive Green, Silver:** XP Luna.
  - **Royale:** XP's deeper, glossier blue with a dark navy taskbar (Luna renderer).
  - **WRLD Classic:** also the built-in safe-mode fallback.
  - **Aero:**
    - Window frames are drawn on real **blurred, tinted glass**, with reflection streaks.
    - Title text glows; there's a wide red close button.
    - Glass taskbar, the globe on its own as the Start button (it glows on hover), and a glass
      Start menu.
    - The sidebar is glass too: the wallpaper shows through a blurred blue tint, and the
      account photo fades into those glass pixels instead of ending on a solid strip.
  - **Vista Aero:** the first Aero glass: smokier, darker glass, a black glass taskbar and the
    glowing orb (Aero renderer).
  - **Metro:** the flat Windows 8 look (its own `metro` renderer): window frames in one solid
    accent colour with square corners and a centred title, thin-line caption glyphs that light up
    on hover beside an always-red close button, flat push buttons, a flat taskbar whose Start
    button is the globe as a simple line logo, and a square Start menu.
  - **Plex:** after the Longhorn reference.
    - Glossy sky-blue title bars, bare caption glyphs, a blue taskbar.
    - Blue Start menu with round lock/power buttons.
  - **Dark mode:** a checkbox in Personalization turns the shell, menus, dialog boxes and
    built-in app chrome dark with readable text. Luna Blue, Olive Green, Silver, Classic,
    Aero and Plex keep their own accents, gradients and renderer; Aero retains transparency.
    Explorer task panes, the All Programs flyout, file dialogs, Calculator, Paint tools,
    browser chrome and standard Win32 controls use the appropriate palette roles. Wallpapers,
    photos, web document colours and Paint's image pixels are not recoloured. The Media Player
    keeps its own dark skin. Dark mode is saved with your settings.
  - **Advanced Appearance:** a page in Personalization with a per-role colour editor (window
    face, background, text, hyperlinks, buttons, scrollbars, check/radio accent, menus, taskbar,
    title bars and more) that previews changes live and can reset individual colours or all of
    them.
    Dark mode is applied before hue/tone adjustments; explicit per-role overrides win last
    and are stored in `C:\WRLD\System32\config\appearance.bin`.
  - **Cursor sets:** drop folders of standard Windows `.cur` files into `C:\WRLD\Cursors` and
    choose the set in Advanced Appearance. Missing roles fall back to the built-in cursors.
  - **Hue (0–359°), saturation and lightness** sliders recolour any style; they are saved with
    your settings.
- **`.wrldstyle` format:** plain INI text.
  - `[Style]` holds `Name`, `Renderer` (luna, classic, aero, plex or metro), `Description`, a
    suggested `Wallpaper`, and `Base` (inherit everything from another style).
  - It is followed by metrics (`title_h`, `border`, `taskbar_h`, …) and about 90 named colours.
  - Personalization's **Create a style...** writes a new file based on the current style and
    opens it in Notepad.
  - Double-clicking a `.wrldstyle` file anywhere installs and applies it.

### Applications (`src/apps`)
- **The WRLD Search** (`search.c`; Start menu, the classic Start menu's Search, Enter in the
  sidebar's Search tile, `start search` in the Command Prompt): a sky-blue start page with the
  search box, nine areas of the computer with their most-used places, Recommendations, Your
  Recent Activities (the files you opened last, kept in `C:\WRLD\recent.txt`) and Top Things
  To Do. A search lists matching programs, Control Panel settings (by name and by the words
  people use, such as "wallpaper"), and files and folders on every drive (your documents
  first), plus a web search; every word typed must be in the name. File, Edit, View and Help
  menus, a Back button and a breadcrumb in the top bar.
- **File Explorer:** task pane, Icons/Tiles/Details views, history, copy/cut/paste, rename,
  Recycle Bin, eject.
- **Notepad:** selection, clipboard, word wrap, Open/Save dialog.
- **WordPad** (`wordpad.c`; its document readers are `rust/src/wpdoc.rs`): a word processor after the one in Windows 7, with a ribbon (the
  File menu button, Home and View tabs, undo/redo on the title row), a ruler with indent
  markers, a page view and a status bar with a zoom slider. Text can be any size in Liberation
  Sans, Serif or Mono, bold, italic, underlined, struck through, subscript or superscript,
  coloured and highlighted; paragraphs can be aligned left, centred, right or justified,
  indented, bulleted and spaced at 1.0 to 3.0 lines. It opens and saves Rich Text (`.rtf`),
  Office Open XML (`.docx`, which Word opens) and plain text, and opens OpenDocument text
  (`.odt`). Undo and redo, find and replace, insert date and time, double-click to select a
  word, and a context menu. `C:\Samples\Welcome to WordPad.rtf` is a sample; double-clicking
  an `.rtf`, `.docx` or `.odt` file opens it, and `wordpad` or `write` starts it from the
  Command Prompt.
- **One clipboard for the whole system:** Notepad, WordPad, the Command Prompt's programs and
  Windows programs (`OpenClipboard`, `SetClipboardData`, `GetClipboardData`) share it.
- **Command Prompt.**
- **Control Panel (category style):**
  - Category / Large icons / Small icons views, breadcrumb bar, search, navigation pane.
  - 16 pages: Personalization, Display, Taskbar, Date and Time, System, Device Manager,
    Network, Mouse, Keyboard, Sound, Programs and Features, Default Programs, User Accounts,
    Fonts, Ease of Access.
- **Task Manager:** applications, processes, CPU/memory graphs.
- **The WRLD Browser** (see above).
- **THE WRLD Media Player** (`mediaplayer.c`, `mpviz.c`): after the player that came with
  Windows XP.
  - A skinned window with its own rounded frame (`WF_SKINNED` windows draw their whole frame;
    pixels with alpha 0 are see-through, and the app can ask the window manager to move or resize
    it). The title band has the playlist selector and the caption buttons; the feature taskbar
    down the left has Now Playing, Media Guide, Copy from CD, Media Library, Radio Tuner, Copy to
    CD or Device, Premium Services and Skin Chooser, and can be hidden with its grip.
  - Now Playing shows the video or the visualisation, its toolbar (visualisation menu, previous
    and next, the name of the current look, full screen) and the playlist pane with the album
    picture. Along the bottom: rewind, the seek bar and fast forward, then glassy round play/pause,
    stop, previous, next and mute buttons, the volume slider, shuffle, repeat and a status box.
  - **Visualisations** (`mpviz.c`), 29 looks in 7 collections. Ambience and Charge are feedback
    effects: every frame the previous picture is warped (zoom, rotation, swirl, ripple, tunnel,
    kaleidoscope, rising flames, mirror, waves), softened and faded, shapes driven by the music
    (waveform lines and rings, spectrum rays, orbiting dots, beat bursts, lightning, stars) are
    drawn on top, and the result is coloured through a drifting palette that flashes on the beat.
    Charge's Randomization invents a new combination every 12 seconds or on a big beat. Bars and
    Waves (bars, mirrored bars, scope, fire scope, spectrum circle), Plasma, Particles (starfield,
    fountain, fireworks on the beat), Album Art, and Spectrum 3D (a landscape and towers drawn by
    the WRLD 3D engine). Double-click the picture, click the full-screen button or press Alt+Enter
    for full screen. The audio is analysed with a 512-point FFT into 32 bands, levels and beats.
  - **Formats** (`src/media`, one decoder interface in `src/include/mediadec.h`; formats are
    recognised by their contents): MP3, MP2 and MP1 ([minimp3](https://github.com/lieff/minimp3)),
    Ogg Vorbis ([stb_vorbis](https://github.com/nothings/stb)), FLAC and Ogg FLAC
    ([dr_flac](https://github.com/mackron/dr_libs)), WAV (PCM 8/16/24/32-bit, 32/64-bit float,
    A-law, mu-law, IMA ADPCM, Microsoft ADPCM, WAVE_FORMAT_EXTENSIBLE, up to 8 channels folded to
    stereo), AIFF and AIFC, Sun .au, and **MPEG-1 video** with MP2 sound (.mpg, .mpeg;
    [PL_MPEG](https://github.com/phoboslab/pl_mpeg)), whose pictures are decoded in step with the
    sound. Tags come from ID3v1/v2 and Vorbis comments. Decoding runs on its own task, straight
    into a sound of the full length; playback starts once half a second is ready. Each decoder was
    checked against ffmpeg's own decoding of the same files (signal-to-noise ratio 45 to 129 dB).
    Not supported: AAC/M4A, WMA/WMV, Opus, MP4/H.264, AVI, MIDI and audio CDs.
  - **Skins:** WRLD Blue (the default), Silver, Olive Green, Midnight, Tangerine, Lavender,
    and **Classic**, which uses the system window frame and grey 3D controls. The Skin Chooser
    previews each one; the choice and the visualisation are remembered.
  - Media Library: a tree (All Media, All Music, each artist and album, All Video) and a table
    with search. Media Guide lists what is on the computer; Copy to CD or Device copies checked
    items to My Music\Copied or another drive. Copy from CD, Radio Tuner and Premium Services
    explain what is not available (audio CD reading, internet radio streams, online services).
  - Keys: Ctrl+P play/pause, Ctrl+S stop, Ctrl+F/Ctrl+B next/previous, Ctrl+H shuffle, Ctrl+T
    repeat, Ctrl+O open, Ctrl+1..8 the features, Alt+Enter full screen, F8 mute, F9/F10 volume;
    on Now Playing, Space, Left/Right to seek and Up/Down to change the visualisation.
  - Opening any of these files opens it in the player. My Music has three original sample
    tracks (synthesised by `tools/gen_music.py`) and My Videos a short MPEG-1 clip.
- **Others:** Calculator, Paint (selections with cut/copy/paste, undo/redo, text, airbrush, filled shapes, zoom with a grid, flip/rotate/stretch/canvas size/crop; opens BMP/PNG/JPEG/GIF, saves BMP/PNG), Image Viewer (PNG/JPEG/GIF/BMP), Snake,
  Minesweeper, Pong, WRLD Security Center, Administrative Tools, Properties, Run, About, Setup.
- **Games** (Start > All Programs > Games), all original, mouse or keyboard:
  - **Solitaire**: Klondike with cards drawn by the OS (draw one or three, undo, scoring, a
    clock, best score, double-click to a foundation; arrows + Space to play without a mouse).
  - **Orbit Pinball**: a table of its own - a planet whose three moons are the bumpers,
    slingshots, a bank of drop targets, rollover lanes that raise the bonus multiplier, a
    plunger (hold Space). Z / Left Shift and / / Right Shift are the flippers.
  - **WRLD Sentinel**: a first-person maze shooter. Each level is a maze generated for the game,
    guarded by security drones that fire energy bolts; health and energy cells lie about and a
    portal leads on. WASD or the arrows, Space or a click to fire, M for the map.
  - **Minesweeper** now has Beginner, Intermediate and Expert, a first click that is never a mine,
    opening around a number whose flags are placed, a clock with best times, and keyboard play;
    **Snake** keeps its best score.

### Compatibility
- **`.exe`: Win32 subsystem (`src/win32`)**
  - PE loader: PE32 (x86) in the 32-bit edition, PE32+ (x64) in the 64-bit edition, with
    sections, base relocations (`HIGHLOW`, `DIR64`), import binding, TLS callbacks and the
    TEB/PEB of each architecture. The file format is read in Rust (`rust/src/pe.rs`); see [Rust in the kernel](#rust-in-the-kernel-rust).
  - **Programs' own DLLs:** a DLL that ships with a program (its folder, the current folder,
    `C:\WRLD\System32`) is mapped into the process, relocated, has its imports resolved the same
    way and gets `DllMain`; `LoadLibrary`/`GetProcAddress` (names, ordinals, forwarders),
    `GetModuleHandle`, `GetModuleFileName` and `FindResource` work with it. Tested with Tiny C
    Compiler (`libtcc.dll`: it compiles and runs C programs) and embedded Python 3.8
    (`python38.dll` with the real `vcruntime140.dll`). `C:\Samples\dlldemo.exe` with
    `mathlib.dll` shows both ways of using a DLL.
  - **A real registry** shared by all programs and kept in `C:\WRLD\registry.dat` (in Rust:
    `rust/src/registry.rs` and `rust/src/kernel/winreg.rs`), a per-process
    environment, INI files (`GetPrivateProfileString` and friends) and the standard Open and
    Save As dialogs (`GetOpenFileName`/`GetSaveFileName`, on the system's file dialog).
  - **Common controls:** toolbar, status bar, rebar, progress bar, trackbar, up-down, tab control
    and list view (report and list modes, `WM_NOTIFY`), image lists and window subclassing
    (`C:\Samples\ccdemo.exe` uses them, with the registry and an INI file).
  - **msvcp60.dll:** Visual C++ 6's `std::basic_string<char>` and `_Lockit`, with VC6's own object
    layout (programs inline parts of it), `__thiscall` on x86. With keyboard accelerators
    (`LoadAccelerators`/`TranslateAccelerator`), class menus, baseline-aligned text and
    clipped `ExtTextOut`, Notepad2 (x86 and x64) runs: its Scintilla editor, menus, toolbar and
    shortcuts work.
  - **Doom (Chocolate Doom 3.0, SDL2) runs and plays** on the 32-bit edition, with the Freedoom
    IWAD: video through SDL's OpenGL renderer, keyboard, OPL music and sound effects through
    winmm. What it needed: SSE in the 32-bit kernel; real counting semaphores, owned recursive
    mutexes, critical sections, SRW locks and condition variables (semaphores had been
    auto-reset events, which lose releases and hung SDL's audio); PC scan codes in the
    `WM_KEYDOWN`/`WM_KEYUP` lParam (SDL identifies keys by them); and the OpenGL additions above.
  - For hung programs, Ctrl+Alt+Shift+F12 logs every task's stack (the code addresses on it) to
    the serial port.
  - A much larger C runtime (file descriptors, `stat`/`findfirst`, the Universal CRT start-up
    and `__stdio_common_*` functions, wide printf, `setjmp`/`longjmp`, time conversions),
    DIB sections, regions, viewport origins and bitmaps from resources in GDI, shlwapi, more
    of shell32, version.dll, winmm (`PlaySound`, `waveOut`), and failure-reporting versions of
    the security, crypto, printing, LDAP and device-setup calls programs probe for. The newer
    tables (`apitab.c`: the registry, kernel32, shell and misc tables from Rust, `user32_more.inc`,
    `comctl.inc`, `crt2.inc`) are looked up first.
  - On x64 every exported function uses the Microsoft x64 calling convention (GCC's
    `ms_abi`), variadic ones (`printf`, `wsprintfA`) the matching `va_list`; handles,
    `WPARAM`/`LPARAM` and `*LongPtr` values are pointer-sized while `long` stays 32-bit (LLP64).
  - About 190 kernel32, 190 msvcrt, 160 user32 and 45 gdi32 functions, plus advapi32/shell32
    basics, and opengl32, glu32, d3d9, d3d10, dxgi and vulkan-1 (see 3D for Windows programs).
    `src/win32/compat.c` adds about 230 more for real programs: the Unicode (`W`) file, process,
    environment and locale functions, `ReadFile`/`WriteFile` at an `OVERLAPPED` offset, SRW
    locks, condition variables, init-once, interlocked lists, time conversions, registry and
    security stubs, BSTR/VARIANT (oleaut32) and the wide-string C runtime.
  - Tested with real programs: **7-Zip** (`7zr.exe`, 32-bit) creates, lists and extracts
    archives; **SQLite** (`sqlite3.exe`, 64-bit) creates and queries database files, also
    interactively; **plink** (PuTTY's command-line tool) connects and talks in both editions;
    **PuTTY** works in both editions: its configuration window (tree view, group boxes, radio
    buttons, combo and list boxes, page switching), then a terminal window with a live
    connection, local echo and PuTTY's own key translation.
  - **Dialog boxes and resources** (`src/win32/user32_dlg.inc`, `src/win32/pe.c`):
    `FindResource`/`LoadResource`/`SizeofResource` on the program's resource directory;
    `DialogBox*`/`CreateDialog*` from `DLGTEMPLATE`/`DLGTEMPLATEEX` resources (dialog units,
    the dialog font, the template's own class or the system's `#32770`); `DefDlgProc` with
    `DWLP_MSGRESULT`/`DWLP_DLGPROC`/`DWLP_USER` and class extra bytes (`cbWndExtra`,
    `Get/SetWindowLongPtr` at positive offsets); `IsDialogMessage` (Tab, Enter for the default
    button, Esc); `MapDialogRect`, `CheckRadioButton`, `SendDlgItemMessage`, `LoadString`,
    `LoadMenu`. Controls: list boxes (sorted, multi-select, item data, scrolling), combo boxes
    (drop-down lists, editable), tree views (`SysTreeView32`: insert, expand, select,
    `TVN_SELCHANGED` through `WM_NOTIFY`), auto radio groups, read-only edits, Unicode window
    text and messages (`SendMessageW` converts strings for the built-in controls), caret,
    clipboard, `TrackPopupMenu`, `ToAscii`/`ToUnicode`. Also imm32, ole32 (`CoInitialize`,
    `CoCreateInstance` says "not registered"), comdlg32 (file/colour/font pickers answer
    "cancelled"), comctl32 and winspool.drv (no printers) for programs that expect them.
  - As on Windows, `WM_CHAR` comes from `TranslateMessage` (a key `IsDialogMessage` took makes
    no character), `WM_KEYUP` carries the transition bits, punctuation has `VK_OEM_*` codes, a
    hidden window gets its first `WM_SIZE` when it is first shown, and `MessageBox` keeps the
    program's timers and socket messages running behind the box.
  - **Windows sockets** (`ws2_32.dll`, `wsock32.dll`, `src/win32/ws2_32.c`) on the kernel's
    TCP/IP stack: IPv4 TCP (connect, listen/accept) and UDP, blocking and non-blocking sockets,
    `select`, `WSAEventSelect`/`WSAEnumNetworkEvents` and `WSAAsyncSelect` (a kernel task polls
    the sockets that use them), `getaddrinfo`/`gethostbyname` through the stack's DNS, and the
    usual ordinals. Not done: IPv6, overlapped I/O, raw sockets. `C:\Samples\wsget.exe`
    fetches a web page in each of the three styles (`wsget [-s|-e] HOST [PORT] [PATH]`).
    `WaitForMultipleObjects` now waits for any (or all) of its objects, events can be
    auto-reset, and mutexes and semaphores are signalled.
  - Console programs use the terminal for stdin/stdout.
  - GUI programs get real windows, message queues, timers, BUTTON/STATIC/EDIT/LISTBOX/
    COMBOBOX/tree view controls, dialog boxes, menus, MessageBox and GDI drawing.
  - The samples are ordinary programs compiled with `i686-w64-mingw32-gcc` (32-bit edition,
    `rootfs/Samples`) and `x86_64-w64-mingw32-gcc` (64-bit edition, `rootfs64/Samples`) and run
    unmodified.
  - A program that calls an unimplemented function is stopped with a message naming it.
- **`.sh`: shell (in Rust: `rust/src/shell.rs` and `rust/src/kernel/shk.rs`)**
  - Tokenizer and recursive-descent parser to an AST.
  - Variables, `${…}` operators, `$(…)`, backticks, `$((…))`, globbing.
  - Pipes, `>`, `>>`, `<`, `2>&1`, `&&`/`||`.
  - `if/elif/else`, `while/until`, `for`, `case`, functions.
  - About 70 built-in commands (`ls grep sort wc head tail find tree cp mv rm mkdir read
    printf test seq ps kill mount hdiutil open theme hostname shutdown …`).
- **`.dmg`: Apple disk images**
  - Reads the `koly` trailer, the XML plist `blkx` table and the `mish` chunk maps (raw,
    zero-fill, zlib/UDZO), then the MBR/GPT partition map.
  - The FAT volume inside is mounted as a new drive.
  - `.app` bundles launch through `Info.plist` → `CFBundleExecutable`; script executables run
    in the shell.

## Build targets

| Target | |
|---|---|
| `make` | Kernel and `wrldos64.iso`: the 64-bit edition with the WebKit browser (the main image). |
| `make classic` (or `make CLASSIC=1`) | The 64-bit edition with the browser's classic engine instead of WebKit: smaller, needs no WebKitGTK. |
| `make ARCH=i686` | Kernel and `wrldos32.iso` (the 32-bit edition alone). |
| `make both` | Both editions and `wrldos.iso`, the CD with both (`make dual` assembles it from built editions). |
| `make run` / `run-install` / `run-disk` | See [Running and installing](#running-and-installing). |
| `make check-tools` | Lists missing build tools. |
| `make samples` | Rebuilds the Win32 samples (needs `gcc-mingw-w64-i686` and `gcc-mingw-w64-x86-64`). |
| `make dmg` | Rebuilds `rootfs/Samples/Sample.dmg` (needs `python3`, `dosfstools`). |
| `make assets` | Regenerates fonts, icons and the globe (needs Python with Pillow and NumPy, `fonts-liberation`). |
| `make rust-test` | Runs the tests of the kernel's Rust part on the build machine. |
| `make clean` | Removes build output. |

`tools/qemu_harness.py` drives a headless QEMU over QMP (mouse, keyboard, screenshots). It was
used to test every screen above, including a full install from a blank disk.

## Limits

- **One program architecture per edition.** The 32-bit edition runs x86 programs and the
  64-bit edition runs x64 programs; each recognises the other kind and says which edition to
  use. .NET assemblies and 16-bit DOS programs are refused with an explanation. C++ exceptions
  in x64 programs (which need SEH unwinding) are not supported. Win32 coverage is a subset. Programs that use
  unimplemented APIs (COM objects, …) stop with a message naming
  the missing function. Only the built-in DLLs exist: a program that ships its own `.dll`
  files (curl's `libcurl-x64.dll`) does not load them yet.
- **macOS cannot be emulated.** Native Mach-O programs inside a `.dmg` are identified but not
  run; only script-based bundles work. HFS+/APFS volumes and bzip2/LZFSE/LZMA-compressed images
  are reported as unsupported. FAT volumes with raw or zlib chunks work.
- **Storage:**
  - The live CD runs from RAM.
  - An installed system keeps `C:` on an NTFS or FAT32 partition of an IDE, SATA, NVMe, SCSI or
    virtio disk, or of a USB disk on any of the supported USB controllers. BusLogic and the LSI 53C895A (QEMU's
    `-drive if=scsi` default) are not supported. Hyper-V's synthetic SCSI is not supported either,
    so use a generation 1 VM's IDE disk.
  - Disks with 4 KB sectors (4Kn) can't be installed to: a BIOS can't start from them.
  - At boot the whole partition is loaded into memory and changes are written back, so the
    installed system's size is limited by RAM.
  - **Boot:** the CD is a hybrid image: PC BIOS and 64-bit UEFI both boot it. Installed systems
  use an MBR partition table (no GPT). Setup always writes the BIOS boot loader; when Setup
  itself runs under UEFI the system partition must be FAT32, becomes an EFI System Partition
  and receives `\EFI\BOOT\BOOTX64.EFI`. An NTFS installation can therefore be started only by
  BIOS/CSM, and a 32-bit-EFI-only machine cannot boot the images.
  - NTFS: no compression, encryption, alternate data streams or attribute lists, and the MFT
    has a fixed size (4096 files and folders on partitions under 512 MB, 16384 above), so
    FAT32 is the choice for very many small files. The driver mounts volumes it formatted;
    volumes made by Windows may use features it does not read. Checked with `ntfs-3g`'s tools,
    not with Windows' own chkdsk.
- **Notepad and the Command Prompt** treat text as bytes, one cell per byte, so a
  UTF-8 file's non-ASCII characters take two cells (as they always did). The desktop,
  The WRLD Browser, the dialogs and the Win32 text APIs are UTF-8.
- **Network and browser:**
  - Wired Ethernet only (no Wi-Fi, no USB adapters, no Broadcom), and IPv4 only.
  - HTTPS: no HTTP/2, and no post-quantum (hybrid ML-KEM) key exchange.
  - JavaScript works for ordinary pages, but big web apps (which need layout-accurate CSS,
    canvas, modules or web workers) mostly won't.
  - It doesn't load external stylesheets or web fonts.
  - Unsupported images: WebP and SVG (the alt text is shown instead).
  - Text is limited to what the bundled fonts cover. Liberation has Latin, Greek (including
    polytonic), Cyrillic, Hebrew, punctuation, arrows and common maths; Noto Naskh Arabic
    supplies Arabic. There is no CJK, emoji or Indic script, so those characters show as an
    empty box. Arabic and Hebrew are shaped and laid out right to left; Indic shaping, font
    hinting and web fonts are not implemented.
  - Text-heavy sites work best.
- **Linux programs:**
  - `fork` copies the whole address space (there is no copy-on-write); `MAP_SHARED` mappings of
    shared memory and of ordinary files stay shared. `vfork` and `posix_spawn`
    run the child in the parent's memory until it calls `execve`, as on Linux.
  - The file system is case-insensitive, so a package with two files whose names differ only in
    case installs one of them.
  - Pipes hold at most 64 KiB; network sockets are IPv4 only, with no loopback interface and no
    IP fragmentation (a UDP datagram tops out at 1472 bytes).
- **Hardware and design:**
  - USB runs on **xHCI** (USB 3.x), **EHCI** (USB 2.0), **UHCI** and **OHCI** (USB 1.1)
    controllers. EHCI owns the high-speed ports and hands full/low-speed ones to the companion;
    full-speed devices behind a high-speed hub (split transactions) are not supported.
    Isochronous audio (USB audio class 1 and 2 playback) works on xHCI only; USB video is not driven.
    Devices (disks included) can be plugged in and
    removed while the system runs.
  - No GPU or 3D; the software rasteriser has paths, strokes and gradients but no filters,
    shadows or canvas, and CSS layout covers block and inline boxes, floats, positioning, flexbox, simple grids and tables; no transforms, animations, `position: sticky` behaviour or grid areas.
  - Sound is playback only (no recording).
  - Resolution can only be changed on the display adapters listed above; otherwise it stays at
    GRUB's 1024×768. The minimum is 1024×768.
  - Single CPU. Both editions use memory below 4 GiB only (about 3.75 GB at most); the desktop
    itself needs about 16 MB.
  - Cooperative (not preemptive) scheduling, and programs run in ring 0.
- **Names and artwork:** "Luna", "Olive Green", "Silver", "Classic", "Aero" and "Plex" name the
  visual styles this project recreates. The icons and the globe logo are original; the wallpaper
  and account-picture photographs live in `WRLD/Wallpapers` and `WRLD/Avatars`.

## Project layout

```
Build to ISO.bat   one-click ISO build on Windows (WSL, Docker, ready-made ISOs or the portable build VM)
src/arch           boot stub, interrupt/context-switch assembly, linker script: i686/, x86_64/
src/kernel         boot, cpu, ACPI, drivers, memory, scheduler, ATA, applying the settings (the
                   VFS, FAT32, NTFS and persistence are in rust/)
rust/              the kernel's Rust part (wrldrs): heap/log/panic bridge, DEFLATE decoder,
                   FAT image reader, DMG disk images, network packet parsers and network stack,
                   PE (Windows program) loader, the registry, the FAT32 and NTFS drivers, the in-memory
                   file system (kernel/vfsk.rs, paths and the initrd in vfspath.rs), the smaller Win32 API
                   files (kernel/w32*.rs, their parsing in w32util.rs), the shell (shell.rs, kernel/shk.rs),
                   the PNG/GIF/JPEG/BMP decoders (imgdec.rs, kernel/imgk.rs), the browser's
                   document tree, CSS parsing and cascade (html.rs, css.rs), persistence and
                   the settings file (persist.rs, kernel/persistk.rs), the TrueType engine with
                   OpenType shaping (ttf.rs), reading and checking .deb/.rpm packages (pkgfmt.rs,
                   kernel/pkgk.rs), USB descriptors and reports (usbdesc.rs), Direct3D shader
                   bytecode and the shader interpreter (dxbc.rs, dxil.rs), the Linux layer
                   (kernel/lx/, its paths and ELF headers in lxpath.rs), the browser's SVG
                   renderer and the document interface for scripts (svg.rs, dom.rs), WAV, AIFF
                   and .au files (pcm.rs), the ad blocker (adblock.rs), the HTTP client (http.rs),
                   WordPad's Rich Text, .docx and .odt readers (wpdoc.rs)
src/net            network card drivers, TLS glue (the stack is rust/src/netstack.rs, HTTP rust/src/http.rs)
src/usb            USB host controllers (xhci, ehci, uhci, ohci), enumeration, HID, mass storage
                   (descriptors and reports are read in rust/src/usbdesc.rs)
src/gui            graphics, text layout with bidi and
                   script itemisation (text.c), the software path rasteriser (raster.c),
                   globe + wordmark, themes, window manager, widgets, desktop, screens
src/apps           explorer, terminal, control panel, launcher, setup, system windows (system.c: Task Manager,
                   About, Properties), browser (fetching for scripts in domnet.c, the page arena in html.c),
                   image format dispatch (imgdec.c)
src/apps/accessories  Notepad, WordPad, Paint, Calculator, Image Viewer (one file each)
src/apps/games     Solitaire, Minesweeper, Orbit Pinball, WRLD Sentinel, Snake, Pong (one file each)
src/js             JavaScript glue (engine.c) and the browser environment (prelude.js)
src/tls            HTTPS glue for Mbed TLS (tls.c) and the root certificates (root_certs.c)
src/shell          the shell's system commands (shcmds.c: network, display, sound, devices, packages...;
                   the shell itself is rust/src/shell.rs and rust/src/kernel/shk.rs)
src/win32          PE loader glue (the format: rust/src/pe.rs), kernel32, msvcrt, user32/gdi32, OpenGL, Direct3D 8/9/10
                   (shader bytecode: rust/src/dxbc.rs, dxil.rs), DirectInput, DirectSound
src/include        headers (disk.h: disks/FAT32/NTFS volumes/setup; net.h: network stack)
third_party/mbedtls  Mbed TLS 4.1.0 with TF-PSA-Crypto 1.1 (TLS), Apache-2.0 licence
third_party/mbedtls-config  the Mbed TLS / TF-PSA-Crypto configuration for this OS
third_party/quickjs  QuickJS-ng 0.18 (JavaScript), MIT licence
third_party/quickjs-shim  the small C and math library QuickJS is built against
third_party/minimp3  minimp3 (MP3 decoding for the media player), public domain (CC0)
third_party/stb_vorbis  stb_vorbis (Ogg Vorbis), public domain or MIT
third_party/dr_flac  dr_flac (FLAC), public domain or MIT-0
third_party/pl_mpeg  PL_MPEG (MPEG-1 video, MP2), MIT
src/media          decoder glue: one interface (mdec.c; WAV/AIFF/AU are read in rust/src/pcm.rs), MP3, Vorbis, FLAC, MPEG-1
samples/           Win32 sample sources (hello.c, winhello.c, glcube.c, d3dcube.c, d3d10cube.c, dxdemo.c)
dmgroot/           contents of Sample.dmg
rootfs64/          files that replace rootfs/ ones in the 64-bit edition (x64 samples)
rootfs/            live C: drive (initrd), including WRLD/Resources/Themes/*.wrldstyle;
                   the build adds boot/ (kernel and GRUB images)
grub/grub.cfg      CD boot menu
grub/efi.cfg       early config embedded in the installed EFI loader
tools/             asset/globe/root-certificate generators, DMG builder, build-iso.sh, QEMU harness,
                   buildvm/ (the portable build VM for Windows PCs without WSL or Docker)
.github/workflows  build-iso.yml (ISOs as the "latest-iso" release), build-vm-kit.yml (the build VM kit)
docs/screenshots/  the screenshots in this README
```

## Credits and licences

- Fonts: [Liberation](https://github.com/liberationfonts/liberation-fonts) 2.1.5 (SIL Open Font
  License): the bitmaps generated by `tools/gen_assets.py` and the TrueType files in
  `rootfs/WRLD/Fonts`, with the licence alongside them.
- Arabic: [Noto Naskh Arabic](https://fonts.google.com/noto/specimen/Noto+Naskh+Arabic) (SIL
  Open Font License, `rootfs/WRLD/Fonts/OFL-Noto.txt`), the fallback face that shapes Arabic.
- Boot loader: [GNU GRUB](https://www.gnu.org/software/grub/) (GPL v3). It is used on the CD,
  and Setup writes GRUB's boot images to the installed disk. Its source code is available from
  the GNU project.
- TLS: [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 4.1.0 and TF-PSA-Crypto by the Mbed TLS
  contributors (Apache-2.0 licence, `third_party/mbedtls/LICENSE`), from the official 4.1.0
  release, built unmodified (tests, programs, docs and the ML-DSA driver left out).
- JavaScript: [QuickJS-ng](https://github.com/quickjs-ng/quickjs) 0.18 by Fabrice Bellard,
  Charlie Gordon, Ben Noordhuis, Saúl Ibarra Corretgé and contributors (MIT licence), built
  unmodified; the licence text is at the top of each source file.
- Root certificates: the Mozilla CA certificate store, as shipped in Debian/Ubuntu's
  `ca-certificates` package.
- Archives: the PPMd compression algorithm by Dmitry Shkarin (public domain), which RAR uses
  for text; the decoder in `rust/src/ppmd.rs` was written for this project. RAR is a format of
  RARLAB; THE WRLD OS only extracts it and contains no RARLAB code. The RAR and ZIP archives the
  tests read come from libarchive's test suite (BSD licence) and are downloaded for testing,
  not shipped.
- Everything else (kernel, drivers, network stack, browser, DOM, ad blocker, GUI, applications,
  Win32 subsystem, shell, artwork) was written for this project.
