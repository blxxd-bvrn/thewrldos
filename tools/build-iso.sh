#!/usr/bin/env bash
# Builds wrldos.iso (both editions) and wrldos64.iso from the source folder given as $1 (default: current folder).
# Used by "Build to ISO.bat" on Windows (runs inside WSL or a Docker container),
# but works on any Debian/Ubuntu-like Linux too.
#
# The sources are copied to a Linux-side build folder first, so Windows line
# endings (CRLF) and slow /mnt/c file access do not get in the way.
set -euo pipefail

SRC="$(cd "${1:-.}" && pwd)"
BUILD_DIR="${WRLD_BUILD_DIR:-$HOME/.cache/wrldos-build}"
PKGS="build-essential gcc-multilib nasm grub-pc-bin grub-efi-amd64-bin grub-efi-ia32-bin grub-common xorriso mtools curl ca-certificates"
# The kernel's Rust part (rust/) needs Rust with a freestanding target for each edition. Distribution
# packages of Rust lack those targets, so it comes from rustup (into ~/.cargo, no root needed).
RUST_TARGETS="x86_64-unknown-none i586-unknown-linux-gnu"
RUST_SETUP='[ -x "$HOME/.cargo/bin/cargo" ] || command -v rustup >/dev/null 2>&1 || curl -sSf https://sh.rustup.rs | sh -s -- -y -q --profile minimal; [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"; rustup target add '"$RUST_TARGETS"' >/dev/null'

# The 64-bit edition's Linux userland comes from these: WebKitGTK (The WRLD Browser's engine),
# GTK 4, Wayland, GStreamer (video and sound), musl, BusyBox (/bin/sh) and the CA list.
# Without them the build still works, but The WRLD Browser falls back to its classic engine.
LXPKGS="pkg-config python3 musl-tools busybox ca-certificates libwebkitgtk-6.0-dev libgtk-4-dev
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libglib2.0-dev libcairo2-dev
        libfontconfig-dev libfreetype-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad
        xkb-data gsettings-desktop-schemas"

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

# Rust and its two targets (RUST_SETUP above): installed the first time, then only checked
have_rust() {
    [ -f "$HOME/.cargo/env" ] && . "$HOME/.cargo/env"
    command -v cargo >/dev/null 2>&1 && command -v rustup >/dev/null 2>&1 || return 1
    for t in $RUST_TARGETS; do rustup target list --installed 2>/dev/null | grep -qx "$t" || return 1; done
}
if ! have_rust; then
    say "Installing Rust (rustup, into ~/.cargo) with the targets $RUST_TARGETS"
    bash -c "$RUST_SETUP" || fail "could not install Rust. Install it from https://rustup.rs, then: rustup target add $RUST_TARGETS"
    have_rust || fail "Rust is still missing its targets: rustup target add $RUST_TARGETS"
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
fi

# The WRLD Browser's engine (WebKitGTK 6.0) needs Ubuntu 24.04 or newer. On an older distribution
# the 64-bit edition is built in an Ubuntu 24.04 of its own: a Docker container when Docker runs,
# otherwise a debootstrap chroot kept in ~/.cache for the next build. WRLD_NOBLE=0 skips that
# (the image is then built here). The WebKit image is the default; WRLD_CLASSIC=1 builds the
# 64-bit edition with the browser's classic engine instead (smaller, no WebKitGTK needed).
NOBLE=""
MAKE64="ARCH=x86_64"
if [ "${WRLD_CLASSIC:-0}" = "1" ]; then
    MAKE64="ARCH=x86_64 LX_WEBKITGTK=0"
    say "WRLD_CLASSIC=1: the 64-bit edition is built with The WRLD Browser's classic engine."
elif [ "${WRLD_NOBLE:-auto}" = "1" ] || { [ "${WRLD_NOBLE:-auto}" = "auto" ] && ! pkg-config --exists webkitgtk-6.0 2>/dev/null; }; then
    if [ -z "${WRLD_NOBLE_CHROOT:-}" ] && command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then NOBLE=docker
    elif command -v apt-get >/dev/null 2>&1; then NOBLE=chroot
    else
        say "WebKitGTK 6.0 is not available here, and there is no Docker or apt-get to build in Ubuntu 24.04."
        say "Install WebKitGTK 6.0 (Ubuntu 24.04 or newer), or run with WRLD_CLASSIC=1 for the classic-engine image."
        exit 1
    fi
fi
[ -n "$NOBLE" ] && say "WebKitGTK 6.0 is not available on this system: the 64-bit edition is built in Ubuntu 24.04 ($NOBLE)."

build64_noble_docker() {
    docker run --rm -v "$BUILD_DIR:/build" ubuntu:24.04 bash -c "
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq && apt-get install -y -qq $PKGS $(echo $LXPKGS) >/dev/null &&
        { $RUST_SETUP; } &&
        make -C /build -j$(nproc 2>/dev/null || echo 2) $MAKE64"
}
build64_noble_chroot() {
    local CH="${WRLD_NOBLE_DIR:-$HOME/.cache/wrldos-noble}"
    SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    export DEBIAN_FRONTEND=noninteractive
    if [ ! -x "$CH/usr/bin/apt-get" ]; then
        say "Setting up Ubuntu 24.04 in $CH (once; about 2 GB)"
        command -v debootstrap >/dev/null 2>&1 || $SUDO apt-get install -y -qq debootstrap
        $SUDO debootstrap --variant=minbase --components=main,universe noble "$CH" http://archive.ubuntu.com/ubuntu
        printf 'deb http://archive.ubuntu.com/ubuntu noble main universe\ndeb http://archive.ubuntu.com/ubuntu noble-updates main universe\n' | $SUDO tee "$CH/etc/apt/sources.list" >/dev/null
    fi
    $SUDO mkdir -p "$CH/build"
    $SUDO mount --bind "$BUILD_DIR" "$CH/build"; $SUDO mount --bind /proc "$CH/proc"; $SUDO mount --bind /dev "$CH/dev"; $SUDO mount --bind /dev/pts "$CH/dev/pts"
    local rc=0
    # the build packages: installed the first time, and whenever the list grows
    local missing; missing=$($SUDO chroot "$CH" sh -c "for p in $PKGS $(echo $LXPKGS); do dpkg -s \$p >/dev/null 2>&1 || echo \$p; done")
    if [ -n "$missing" ]; then
        say "Installing in Ubuntu 24.04: $(echo $missing)"
        $SUDO chroot "$CH" apt-get update -qq && $SUDO chroot "$CH" apt-get install -y -qq $missing >/dev/null || rc=$?
    fi
    [ $rc -eq 0 ] && { $SUDO chroot "$CH" env HOME=/root bash -c "$RUST_SETUP" || rc=$?; }
    [ $rc -eq 0 ] && { $SUDO chroot "$CH" env HOME=/root bash -c ". /root/.cargo/env && make -C /build -j$(nproc 2>/dev/null || echo 2) $MAKE64" || rc=$?; }
    $SUDO umount "$CH/build" "$CH/dev/pts" "$CH/dev" "$CH/proc"
    [ "$SUDO" ] && $SUDO chown -R "$(id -u):$(id -g)" "$BUILD_DIR/build64" "$BUILD_DIR/wrldos64.iso" 2>/dev/null
    return $rc
}

# ---- 2. copy the sources into the Linux build folder ----------------------
say "Copying sources to $BUILD_DIR"
mkdir -p "$BUILD_DIR"
# keep build/ (object files) for fast rebuilds, replace everything else
find "$BUILD_DIR" -mindepth 1 -maxdepth 1 ! -name build ! -name build64 -exec rm -rf {} +
( cd "$SRC" && tar --exclude=./build --exclude=./build64 --exclude=./rust/target --exclude=./.git --exclude='*.iso' --exclude=./disk.img --exclude=./disk64.img -cf - . ) | ( cd "$BUILD_DIR" && tar -xf - )

# Windows checkouts may have CRLF line endings: convert text files back to LF
find "$BUILD_DIR" \( -path "$BUILD_DIR/build" -o -path "$BUILD_DIR/build64" \) -prune -o -type f \( -name Makefile -o -name '*.c' -o -name '*.h' \
    -o -name '*.asm' -o -name '*.rs' -o -name '*.toml' -o -name '*.ld' -o -name '*.cfg' -o -name '*.sh' -o -name '*.py' -o -name '*.txt' \
    -o -name '*.plist' -o -name '*.lnk' -o -name '.profile' -o -path '*/MacOS/*' \) -print0 | xargs -0 -r sed -i 's/\r$//'

# ---- 3. build -------------------------------------------------------------
# the main image first: the 64-bit edition with The WRLD Browser on WebKit
say "Building THE WRLD OS (64-bit edition, the main image)"
case "$NOBLE" in
    docker) build64_noble_docker ;;
    chroot) build64_noble_chroot ;;
    *) make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" $MAKE64 ;;
esac
say "Building THE WRLD OS (32-bit edition) and the CD with both editions"
make -C "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" ARCH=i686 dual

cp "$BUILD_DIR/wrldos.iso" "$SRC/wrldos.iso"
cp "$BUILD_DIR/wrldos64.iso" "$SRC/wrldos64.iso"
say "Done: $SRC/wrldos.iso ($(du -h "$SRC/wrldos.iso" | cut -f1), both editions: 64-bit with WebKit where the processor can run it, else 32-bit)"
say "      $SRC/wrldos64.iso ($(du -h "$SRC/wrldos64.iso" | cut -f1), the 64-bit edition alone)"
