#!/bin/sh
# Copies LibreOffice (Writer, Calc, Impress, with its GTK 4 front end) from the build machine
# into the 64-bit edition's Linux root, $1 (build64/linuxroot/Linux). Called by the Makefile
# (LX_LIBREOFFICE). Needs: libreoffice-writer libreoffice-calc libreoffice-impress libreoffice-gtk4
set -e
LX="$1"
LO=/usr/lib/libreoffice
[ -n "$LX" ] && [ -f "$LO/program/libvclplug_gtk4lo.so" ] || { echo "stage_libreoffice: no LibreOffice with libreoffice-gtk4 on this machine" >&2; exit 1; }
LIB="$LX/usr/lib/x86_64-linux-gnu"
mkdir -p "$LX/usr/lib/libreoffice" "$LX/etc/libreoffice" "$LIB" "$LX/usr/local/bin" "$LX/var/lib/wrld/packages"

# the program and its configuration; THE WRLD OS has no symbolic links, so they are copied
# as what they point to (two sample files point nowhere and are left out)
cp -rL "$LO/." "$LX/usr/lib/libreoffice/" 2>/dev/null || true
cp -rL /etc/libreoffice/. "$LX/etc/libreoffice/" 2>/dev/null || true
# the data its language guessing reads
[ -d /usr/share/libexttextcat ] && mkdir -p "$LX/usr/share/libexttextcat" && cp -rL /usr/share/libexttextcat/. "$LX/usr/share/libexttextcat/"
# the dark icon sets are not used here (the profile asks for Colibre)
rm -f "$LX"/usr/lib/libreoffice/share/config/images_colibre_dark*.zip

# the system libraries it loads that the image does not carry yet
for f in "$LO"/program/soffice.bin "$LO"/program/*.so; do ldd "$f" 2>/dev/null | awk '/=> \// { print $3 }'; done | sort -u | while read -r p; do
    case "$p" in "$LO"/*) continue ;; esac
    b=$(basename "$p")
    [ -e "$LIB/$b" ] || cp -L "$p" "$LIB/$b"
done

# the command: the GTK 4 front end, and a restart when LibreOffice asks for one (code 81,
# after it sets up a new user profile)
cat > "$LX/usr/local/bin/libreoffice" <<'EOF'
#!/bin/sh
# LibreOffice in THE WRLD OS: its GTK 4 front end, on the wrldcomp Wayland compositor
export SAL_USE_VCLPLUGIN=gtk4
export HOME="${HOME:-/home/user}"
while :; do
    /usr/lib/libreoffice/program/soffice.bin "$@"
    r=$?
    [ "$r" = 81 ] || exit "$r"
done
EOF

# the user's starting profile: the light Colibre icons (the automatic choice comes out with
# the dark set's light strokes on a light toolbar)
U="$LX/home/user/.config/libreoffice/4/user"
mkdir -p "$U" "$LX/home/user/.config/gtk-4.0"
cat > "$U/registrymodifications.xcu" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<item oor:path="/org.openoffice.Office.Common/Misc"><prop oor:name="SymbolStyle" oor:op="fuse"><value>colibre</value></prop></item>
</oor:items>
EOF
# GTK 4 programs draw their own title bar; THE WRLD OS draws the frame and the title, so
# main windows fold theirs away (dialogs keep it: it can hold their buttons)
cat > "$LX/home/user/.config/gtk-4.0/gtk.css" <<'EOF'
/* THE WRLD OS draws the window frame and title: GTK's own title bar (client-side
 * decorations) is folded away on main windows so it does not appear twice. Dialogs
 * keep theirs: it can hold their buttons. */
window.csd:not(.dialog) > .titlebar, window.csd:not(.dialog) > headerbar.titlebar {
    min-height: 0; padding: 0; margin: 0; border: none; box-shadow: none; background: none;
}
window.csd:not(.dialog) > .titlebar *, window.csd:not(.dialog) > headerbar.titlebar * {
    min-height: 0; min-width: 0; padding: 0; margin: 0; border: none; font-size: 1px; -gtk-icon-size: 1px;
}
window.csd { box-shadow: none; margin: 0; border-radius: 0; }
EOF

# Start menu entries (All Programs), through the package database
VER=$(dpkg-query -W -f '${Version}' libreoffice-core 2>/dev/null || echo "")
cat > "$LX/var/lib/wrld/packages/libreoffice.list" <<EOF
Package: libreoffice
Version: ${VER:-installed}
Architecture: amd64
Summary: LibreOffice: Writer, Calc and Impress (included with THE WRLD OS)
Format: deb
Desktop: LibreOffice Writer	libreoffice --writer	0
Desktop: LibreOffice Calc	libreoffice --calc	0
Desktop: LibreOffice Impress	libreoffice --impress	0
EOF
echo "  LibreOffice: $(du -sh "$LX/usr/lib/libreoffice" | cut -f1) in /usr/lib/libreoffice"
