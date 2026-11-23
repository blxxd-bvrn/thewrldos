#!/usr/bin/env bash
# Makes the portable build VM that "Build to ISO.bat" uses on Windows when there is no WSL or
# Docker: an Ubuntu 24.04 disk with every build tool, Rust and the 64-bit edition's WebKit
# packages already installed (so the build needs no network), and the kernel that boots it.
#
#   sudo tools/buildvm/make-image.sh [OUTDIR]     -> OUTDIR/wrld-build-vm.qcow2, OUTDIR/vmlinuz
#
# Runs on Debian/Ubuntu as root (GitHub Actions does it, see .github/workflows/build-vm-kit.yml).
# EXTRA_CA_FILE=file.crt adds a certificate authority while the image is made (for a proxy that
# inspects HTTPS); it is removed again before the disk is written.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; SRC="$(cd "$HERE/../.." && pwd)"
OUT="$(mkdir -p "${1:-$SRC/build-vm}" && cd "${1:-$SRC/build-vm}" && pwd)"
ROOT="$OUT/rootfs"
MIRROR="${MIRROR:-http://archive.ubuntu.com/ubuntu}"
say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
for t in debootstrap mkfs.ext4 qemu-img; do command -v $t >/dev/null || { echo "missing $t (apt install debootstrap e2fsprogs qemu-utils)"; exit 1; }; done

# the package lists and the Rust setup are the ones tools/build-iso.sh uses
eval "$(sed -n '/^PKGS=/p; /^RUST_TARGETS=/p; /^RUST_SETUP=/p; /^LXPKGS=/,/"$/p' "$SRC/tools/build-iso.sh")"

umount_all() { for m in dev/pts dev proc sys; do mountpoint -q "$ROOT/$m" && umount -l "$ROOT/$m" || true; done; }
trap umount_all EXIT
rm -rf "$ROOT"; mkdir -p "$ROOT"
say "Ubuntu 24.04 base system"
debootstrap --variant=minbase --components=main,universe noble "$ROOT" "$MIRROR"
printf 'deb %s noble main universe\ndeb %s noble-updates main universe\n' "$MIRROR" "$MIRROR" > "$ROOT/etc/apt/sources.list"
mount --bind /dev "$ROOT/dev"; mount --bind /dev/pts "$ROOT/dev/pts"; mount -t proc proc "$ROOT/proc"; mount -t sysfs sys "$ROOT/sys"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
if [ -n "${EXTRA_CA_FILE:-}" ]; then
    mkdir -p "$ROOT/usr/local/share/ca-certificates"; cp "$EXTRA_CA_FILE" "$ROOT/usr/local/share/ca-certificates/zz-build-extra.crt"
fi
in_root() { chroot "$ROOT" env HOME=/root DEBIAN_FRONTEND=noninteractive SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt CURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt REQUESTS_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt PATH=/root/.cargo/bin:/usr/sbin:/usr/bin:/sbin:/bin "$@"; }

say "Build tools and the WebKit packages"
in_root apt-get update -qq
in_root apt-get install -y -qq $PKGS iproute2 >/dev/null
in_root apt-get install -y -qq $(echo $LXPKGS) >/dev/null
[ -n "${EXTRA_CA_FILE:-}" ] && in_root update-ca-certificates >/dev/null
say "Rust ($RUST_TARGETS)"
in_root bash -c "$RUST_SETUP"

say "Kernel"
KPKG=$(in_root apt-cache depends linux-image-virtual | sed -n 's/.*Depends: \(linux-image-[0-9].*-generic\).*/\1/p' | head -1)
[ -n "$KPKG" ] || { echo "no kernel package found"; exit 1; }
in_root bash -c "cd /tmp && apt-get download -qq $KPKG && dpkg-deb -x /tmp/$KPKG*.deb /tmp/k"
cp "$ROOT"/tmp/k/boot/vmlinuz-* "$OUT/vmlinuz"
rm -rf "$ROOT"/tmp/k "$ROOT"/tmp/*.deb

say "The build script the VM runs instead of an init system"
install -m 755 "$HERE/vm-init.sh" "$ROOT/usr/local/sbin/wrld-vm-build"
echo wrld-build > "$ROOT/etc/hostname"

say "Tidying"
if [ -n "${EXTRA_CA_FILE:-}" ]; then rm -f "$ROOT/usr/local/share/ca-certificates/zz-build-extra.crt"; in_root update-ca-certificates --fresh >/dev/null; fi
in_root apt-get clean
rm -rf "$ROOT"/var/lib/apt/lists/* "$ROOT"/usr/share/doc/* "$ROOT"/usr/share/man/* "$ROOT"/usr/share/info/* "$ROOT"/root/.cargo/registry "$ROOT"/root/.rustup/toolchains/*/share/doc
umount_all
date -u +%Y-%m-%dT%H:%MZ > "$ROOT/etc/wrld-build-vm-date"

say "Disk image"
rm -f "$OUT/root.img"; truncate -s 24G "$OUT/root.img"
mkfs.ext4 -q -F -L wrldbuild -E root_owner=0:0 -d "$ROOT" "$OUT/root.img"
qemu-img convert -c -O qcow2 "$OUT/root.img" "$OUT/wrld-build-vm.qcow2"
rm -f "$OUT/root.img"
[ "${KEEP_ROOTFS:-0}" = 1 ] || rm -rf "$ROOT"
say "Done: $OUT/wrld-build-vm.qcow2 ($(du -h "$OUT/wrld-build-vm.qcow2" | cut -f1)), $OUT/vmlinuz"
