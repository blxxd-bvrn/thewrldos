#!/usr/bin/env bash
# The build VM on a Linux (or macOS) host, as build-iso.ps1 runs it on Windows: builds the
# sources in SRC inside the VM in KITDIR (wrld-build-vm.qcow2, vmlinuz) and puts the ISOs into SRC.
# Used by the kit workflow to check a new kit; QEMU with KVM makes it as fast as a normal build.
#   tools/buildvm/run-vm.sh KITDIR SRC
set -euo pipefail
KIT="$(cd "$1" && pwd)"; SRC="$(cd "$2" && pwd)"
QEMU="${QEMU:-qemu-system-x86_64}"
[ -f "$KIT/wrld-build-vm.qcow2" ] && [ -f "$KIT/vmlinuz" ] || { echo "no kit in $KIT"; exit 1; }
[ -f "$KIT/work.qcow2" ] || qemu-img create -q -f qcow2 -F qcow2 -b "$KIT/wrld-build-vm.qcow2" "$KIT/work.qcow2"
rm -f "$KIT/sources.tar" "$KIT/output.img"
tar -cf "$KIT/sources.tar" --exclude=./build --exclude=./build64 --exclude=./.git --exclude=./rust/target --exclude='*.iso' --exclude=./disk.img --exclude=./disk64.img -C "$SRC" .
qemu-img create -q -f raw "$KIT/output.img" 3G
CPUS=$(( $(nproc) < 8 ? $(nproc) : 8 ))
ACCEL="-accel tcg,thread=multi"; [ -w /dev/kvm ] && ACCEL="-accel kvm $ACCEL"
$QEMU -machine q35 -m "${VM_MEM:-6144}" -smp "$CPUS" $ACCEL -kernel "$KIT/vmlinuz" \
    -append 'root=/dev/vda rw console=ttyS0 init=/usr/local/sbin/wrld-vm-build quiet loglevel=3 panic=-1' \
    -drive "file=$KIT/work.qcow2,if=virtio,format=qcow2" -drive "file=$KIT/sources.tar,if=virtio,format=raw,readonly=on" \
    -drive "file=$KIT/output.img,if=virtio,format=raw" -netdev user,id=n0 -device virtio-net-pci,netdev=n0,romfile= \
    -nographic -vga none -no-reboot </dev/null
rm -rf "$KIT/result"; mkdir -p "$KIT/result"
tar -xf "$KIT/output.img" -C "$KIT/result"
cp "$KIT/result/build.log" "$SRC/build-vm.log" 2>/dev/null || true
[ "$(cat "$KIT/result/status" 2>/dev/null)" = 0 ] || { echo "the build failed in the VM (see build-vm.log)"; exit 1; }
mv "$KIT/result/wrldos.iso" "$KIT/result/wrldos64.iso" "$SRC/"
rm -f "$KIT/sources.tar" "$KIT/output.img"
echo "Done: $SRC/wrldos.iso, $SRC/wrldos64.iso"
