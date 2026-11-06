Lumina Sample Disk Image
========================

This volume was packaged as an Apple UDIF disk image (Sample.dmg) with
zlib-compressed chunks. Lumina OS decodes the koly trailer, the XML
block map and the compressed chunks, then mounts the FAT volume inside
as a new drive letter (read-only).

Contents:
  Lumina Demo.app   - a macOS-style application bundle whose executable is
                      a shell script (Contents/MacOS/demo). Double-click it.
  install.sh        - copies the app to C:\Program Files, like dragging an
                      app into the Applications folder on a Mac.
  native-tool       - a Mach-O x86_64 header stub. Lumina identifies native
                      macOS binaries but cannot execute them.
