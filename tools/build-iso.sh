#!/usr/bin/env bash
# Builds wrldos.iso from the source folder given as $1 (default: current folder).
# Used by "Build to ISO.bat" on Windows (runs inside WSL or a Docker container),
# but works on any Debian/Ubuntu-like Linux too.
#
# The sources are copied to a Linux-side build folder first, so Windows line
# endings (CRLF) and slow /mnt/c file access do not get in the way.
set -euo pipefail

SRC="$(cd "${1:-.}" && pwd)"
BUILD_DIR="${WRLD_BUILD_DIR:-$HOME/.cache/wrldos-build}"
PKGS="build-essential gcc-multilib nasm grub-pc-bin grub-efi-amd64-bin grub-efi-ia32-bin grub-common xorriso mtools"
# The 64-bit edition's Linux userland comes from these: WebKitGTK (The WRLD Browser's engine),
# GTK 4, Wayland, GStreamer (video and sound), musl, BusyBox (/bin/sh) and the CA list.
# Without them the build still works, but The WRLD Browser falls back to its classic engine.
LXPKGS="pkg-config python3 musl-tools busybox ca-certificates libwebkitgtk-6.0-dev libgtk-4-dev
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libglib2.0-dev libcairo2-dev
        libfontconfig-dev libfreetype-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ -f "$SRC/Makefile" ] && [ -d "$SRC/src/kernel" ] || fail "$SRC does not look like the THE WRLD OS source folder."

# ---- 1. build tools -------------------------------------------------------
have_tools() {
    for t in make gcc nasm grub-mkrescue grub-mkimage xorriso mformat; do command -v "$t" >/dev/null 2>&1 || return 1; done
    [ -f /usr/lib/grub/i386-pc/boot.img ] || return 1
    [ -f /usr/lib/grub/x86_64-efi/kernel.img ] || return 1
    echo 'int x;' | gcc -m32 -x c -c - -o /dev/null 2>/dev/null || return 1   # 32-bit support (gcc-multilib)
}
if ! have_tools; then
    if command -v apt-get >/dev/null 2>&1; then
        say "Installing the build tools: $PKGS"
        SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo" && say "(you may be asked for your Linux password)"
        export DEBIAN_FRONTEND=noninteractive
        $SUDO apt-get update -qq
        $SUDO apt-get install -y -qq $PKGS
        have_tools || fail "the build tools are still missing after installing them."
    else
        fail "missing build tools and no apt-get. Install the equivalents of: $PKGS"
    fi
fi

# the Linux userland's packages (64-bit edition), installed when any is missing
lx_missing() { for p in $LXPKGS; do dpkg -s "$p" >/dev/null 2>&1 || echo "$p"; done; }
if command -v dpkg >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1 && [ -n "$(lx_missing)" ]; then
    say "Installing the 64-bit edition's Linux userland: $(lx_missing | tr '\n' ' ')"
    SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update -qq
    # one at a time, so a package an older distribution lacks does not stop the others
    for p in $(lx_missing); do $SUDO apt-get install -y -qq "$p" >/dev/null 2>&1 || say "  (could not install $p)"; done
    if ! pkg-config --exists webkitgtk-6.0 2>/dev/null; then
        say "WebKitGTK 6.0 is not available here (it needs Ubuntu 24.04 or newer, or Debian 13):"
        say "the 64-bit edition's WRLD Browser will use its classic engine."
    fi
fi

# ---- 2. copy the sources into the Linux build folder ----------------------
say "Copying sources to $BUILD_DIR"
mkdir -p "$BUILD_DIR"
# keep build/ (object files) for fast rebuilds, replace everything else
find "$BUILD_DIR" -mindepth 1 -maxdepth 1 ! -name build ! -name build64 -exec rm -rf {} +
( cd "$SRC" && tar --exclude=./build --exclude=./build64 --exclude=./.git --exclude='*.iso' --exclude=./disk.img --exclude=./disk64.img -cf - . ) | ( cd "$BUILD_DIR" && tar -xf - )

# Windows checkouts may have CRLF line endings: convert text files back to LF
find "$BUILD_DIR" \( -path "$BUILD_DIR/build" -o -path "$BUILD_DIR/build64" \) -prune -o -type f \( -name Makefile -o -name '*.c' -o -name '*.h' \
    -o -name '*.asm' -o -name '*.ld' -o -name '*.cfg' -o -name '*.sh' -o -name '*.py' -o -name '*.txt' \
    -o -name '*.plist' -o -name '*.lnk' -o -name '.profile' -o -path '*/MacOS/*' \) -print0 | xargs -0 -r sed -i 's/\r$//'

# ---- 3. build -------------------------------------------------------------
say "Building THE WRLD OS (32-bit edition)"
make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" ARCH=i686
say "Building THE WRLD OS (64-bit edition)"
make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" ARCH=x86_64

cp "$BUILD_DIR/wrldos.iso" "$SRC/wrldos.iso"
cp "$BUILD_DIR/wrldos64.iso" "$SRC/wrldos64.iso"
say "Done: $SRC/wrldos.iso ($(du -h "$SRC/wrldos.iso" | cut -f1), 32-bit)"
say "      $SRC/wrldos64.iso ($(du -h "$SRC/wrldos64.iso" | cut -f1), 64-bit)"
