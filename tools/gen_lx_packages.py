#!/usr/bin/env python3
"""List the Debian packages whose files the 64-bit image's Linux root carries.

The Linux root (C:\\Linux) is assembled from the build machine's libraries, file by
file. When the build machine is Debian or Ubuntu, this finds the package each of
those files came from and writes var/lib/wrld/base-packages: "name version" lines,
plus a line for each virtual package they provide. The OS's package installer
(src/apps/pkg.c) counts these as installed when it checks a .deb's dependencies.
Without dpkg on the build machine the list is empty, and dependencies are only
checked library by library.

usage: gen_lx_packages.py LINUXROOT   (the folder that becomes C:\\Linux)
"""
import os
import subprocess
import sys


def host_candidates(rel):
    """paths the build machine may know this file by (merged /usr or not)"""
    p = "/" + rel
    out = [p]
    for a, b in (("/usr/lib/", "/lib/"), ("/usr/lib64/", "/lib64/"), ("/usr/bin/", "/bin/"), ("/usr/sbin/", "/sbin/")):
        if p.startswith(a):
            out.append(b + p[len(a):])
    real = os.path.realpath(p)
    if real not in out:
        out.append(real)
    return out


def main():
    root = sys.argv[1]
    out_path = os.path.join(root, "var/lib/wrld/base-packages")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    lines = []
    try:
        subprocess.run(["dpkg-query", "--version"], capture_output=True, check=True)
        have_dpkg = True
    except (OSError, subprocess.CalledProcessError):
        have_dpkg = False
    if have_dpkg:
        paths = []
        for d, _, files in os.walk(root):
            rel_dir = os.path.relpath(d, root)
            if rel_dir.startswith(("var", "tmp", "home", "etc/fonts")):
                continue
            for f in files:
                rel = os.path.normpath(os.path.join(rel_dir, f))
                if rel.startswith("usr/local/"):
                    continue                     # the OS's own programs
                for c in host_candidates(rel):
                    if os.path.exists(c):
                        paths.append(c)
        pkgs = set()
        for i in range(0, len(paths), 200):
            r = subprocess.run(["dpkg", "-S"] + paths[i:i + 200], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                if ": " not in line:
                    continue
                names = line.split(": ", 1)[0]
                for n in names.split(", "):
                    pkgs.add(n.split(":")[0])
        if pkgs:
            r = subprocess.run(["dpkg-query", "-W", "-f=${Package} ${Version}\t${Provides}\n"] + sorted(pkgs),
                               capture_output=True, text=True)
            for line in r.stdout.splitlines():
                head, _, provides = line.partition("\t")
                lines.append(head)
                name, _, ver = head.partition(" ")
                for pv in provides.split(","):
                    pv = pv.strip().split(" ")[0]
                    if pv:
                        lines.append("%s %s" % (pv, ver))
    with open(out_path, "w") as f:
        f.write("".join(sorted(set(l + "\n" for l in lines))))
    print("  linux userland: %d base packages listed" % len(set(lines)))


if __name__ == "__main__":
    main()
