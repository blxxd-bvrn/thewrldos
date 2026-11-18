#!/usr/bin/env python3
"""Give BusyBox's commands names in the 64-bit image's Linux root.

THE WRLD OS's file system has no symbolic links, so instead of /usr/bin/ls -> busybox
each command gets a two-line stub script, "#!/usr/bin/busybox". The Linux layer runs a
script's interpreter with the script's path ("busybox /usr/bin/ls ..."), and BusyBox
runs the applet that path names. /usr/bin/sh is one of them, so Linux programs get a
/bin/sh (/bin is /usr/bin there) whose commands are found.

The applet list is read from the binary's own name table (it is not run).

usage: gen_busybox_links.py BUSYBOX LINUXROOT
"""
import os
import sys

# not for a program running on THE WRLD OS: booting, devices, kernel modules, logins, and
# the package tools the OS has its own versions of
SKIP = set("""init linuxrc halt reboot poweroff getty login sulogin su passwd chpasswd mkpasswd
switch_root pivot_root run-init mdev insmod rmmod modprobe depmod lsmod modinfo
klogd syslogd logread crond crontab hwclock swapon swapoff mkswap fdisk mkdosfs mke2fs
losetup mount umount nbd-client blkdiscard fsfreeze fstrim freeramdisk chvt openvt
deallocvt dumpkmap loadkmap loadfont setkeycodes vconfig brctl tunctl nameif
udhcpc udhcpc6 udhcpd dumpleases ifup ifdown ifconfig route ip arp arping tc
dpkg dpkg-deb rpm rpm2cpio start-stop-daemon svc svok watchdog devmem i2cdetect i2cdump
i2cget i2cset i2ctransfer ubirename uevent resume nuke linux32 linux64""".split())


def applets(path):
    d = open(path, "rb").read()
    i = d.find(b"[\x00[[\x00")             # the sorted name table starts with "[" and "[["
    if i < 0:
        return []
    names, p = [], i
    while True:
        e = d.index(b"\x00", p)
        n = d[p:e].decode("latin1")
        if not n or not all(c.isalnum() or c in "[-_." for c in n):
            break
        names.append(n)
        p = e + 1
    return names


def main():
    busybox, root = sys.argv[1], sys.argv[2]
    bindir = os.path.join(root, "usr/bin")
    os.makedirs(bindir, exist_ok=True)
    made = 0
    for n in applets(busybox):
        if n in SKIP:
            continue
        dst = os.path.join(bindir, n)
        if os.path.exists(dst):
            continue                            # a real program of that name wins
        with open(dst, "w") as f:
            f.write("#!/usr/bin/busybox\n")
        made += 1
    print("  linux userland: /bin/sh and %d other commands from BusyBox" % (made - 1))


if __name__ == "__main__":
    main()
