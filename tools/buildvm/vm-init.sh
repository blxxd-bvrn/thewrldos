#!/bin/bash
# Runs as the build VM's first process (the kernel starts it instead of an init system).
# Disk 2 (vdb) holds the sources as a tar archive, disk 3 (vdc) receives a tar archive with
# the ISOs, the build log and "status" (0 when the build worked). Then the VM powers off.
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev 2>/dev/null
mkdir -p /dev/pts /dev/shm; mount -t devpts devpts /dev/pts; mount -t tmpfs tmpfs /dev/shm; mount -t tmpfs tmpfs /tmp; mount -t tmpfs tmpfs /run
set -o pipefail
export HOME=/root PATH=/root/.cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 TERM=xterm
hostname wrld-build
# QEMU's user network (only needed when the sources ask for a package the image lacks)
ip link set lo up 2>/dev/null
if ip link set eth0 up 2>/dev/null; then ip addr add 10.0.2.15/24 dev eth0; ip route add default via 10.0.2.2; rm -f /etc/resolv.conf; echo "nameserver 10.0.2.3" > /etc/resolv.conf; fi
exec >/dev/console 2>&1
echo
echo "THE WRLD OS build VM ($(cat /etc/wrld-build-vm-date 2>/dev/null)), $(nproc) processors, $(free -m | awk '/Mem/ {print $2}') MB"
rm -rf /root/src; mkdir -p /root/src /tmp/out
status=1
if tar -xf /dev/vdb -C /root/src; then
    cd /root/src
    bash tools/build-iso.sh /root/src 2>&1 | tee /tmp/out/build.log
    status=$?
    [ -f /root/src/wrldos.iso ] && [ -f /root/src/wrldos64.iso ] || status=1
    [ $status -eq 0 ] && cp /root/src/wrldos.iso /root/src/wrldos64.iso /tmp/out/
else
    echo "Could not read the sources from the second disk." | tee /tmp/out/build.log
fi
echo $status > /tmp/out/status
tar -cf /dev/vdc -C /tmp/out . && sync
cd /; sync; mount -o remount,ro / 2>/dev/null
echo "Build VM finished (status $status), powering off."
echo o > /proc/sysrq-trigger
sleep 30
