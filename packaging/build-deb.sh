#!/usr/bin/env bash
# Builds dist/superv_<version>_amd64.deb: the double-click installer for Ubuntu, Mint, Debian, Pop!_OS…
# Usage: packaging/build-deb.sh   (uses bin/superv-x86_64; run `make` + copy it there first)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$ROOT/superv.c")"
BIN="$ROOT/bin/superv-x86_64"
OUT="$ROOT/dist"
PKG="$(mktemp -d)"
trap 'rm -r "$PKG"' EXIT

install -Dm755 "$BIN"                                       "$PKG/usr/bin/superv"
install -Dm755 "$ROOT/superv-setup"                         "$PKG/usr/lib/superv/superv-setup"
install -Dm644 "$ROOT/data/io.github.superv.SuperV.desktop" "$PKG/usr/share/applications/io.github.superv.SuperV.desktop"
install -Dm644 "$ROOT/data/io.github.superv.SuperV.svg"     "$PKG/usr/share/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg"
install -Dm644 "$ROOT/data/superv-autostart.desktop"        "$PKG/etc/xdg/autostart/superv.desktop"
# click-to-paste on Wayland: let the logged-in user use /dev/uinput
install -Dm644 "$ROOT/data/70-superv-uinput.rules"          "$PKG/usr/lib/udev/rules.d/70-superv-uinput.rules"
install -Dm644 "$ROOT/data/superv-uinput.conf"              "$PKG/usr/lib/modules-load.d/superv-uinput.conf"
install -Dm644 "$ROOT/LICENSE"                              "$PKG/usr/share/doc/superv/copyright"
install -d "$PKG/DEBIAN"

cat > "$PKG/DEBIAN/control" <<EOF
Package: superv
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.34), libgtk-3-0 (>= 3.22), python3, python3-gi, gir1.2-glib-2.0
Installed-Size: $(du -sk "$PKG/usr" | cut -f1)
Maintainer: SuperV <saikatnjm@users.noreply.github.com>
Homepage: https://github.com/saikatnjm/superv
Description: Win+V clipboard history for Linux
 SuperV brings the Windows clipboard history panel (Win+V) to Linux.
 Press Super+V to see the last 25 things you copied (text and images), pick
 one to paste it into the app you are using, pin favourites, and clear the
 rest. Works on GNOME, KDE Plasma, Cinnamon, MATE and Xfce (Wayland and X11).
 .
 A single 88 KB C program that uses about 7 MB of memory.
EOF
echo /etc/xdg/autostart/superv.desktop > "$PKG/DEBIAN/conffiles"

cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    modprobe uinput 2>/dev/null || true
    if command -v udevadm >/dev/null; then
        udevadm control --reload-rules || true
        udevadm trigger --action=change --sysname-match=uinput || true
    fi
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q /usr/share/icons/hicolor || true
    command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
fi
exit 0
EOF
cat > "$PKG/DEBIAN/prerm" <<'EOF'
#!/bin/sh
# On removal, stop SuperV and give the desktop its shortcuts back for every logged-in user.
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    for dir in /run/user/*; do
        uid="${dir##*/}"; bus="$dir/bus"
        [ -S "$bus" ] || continue
        user="$(id -nu "$uid" 2>/dev/null)" || continue
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            python3 /usr/lib/superv/superv-setup --remove-hotkeys >/dev/null 2>&1 || true
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            gdbus call --session --dest io.github.superv.SuperV --object-path /io/github/superv/SuperV \
            --method org.gtk.Actions.Activate quit "[]" "{}" >/dev/null 2>&1 || true
    done
fi
exit 0
EOF
chmod 755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/prerm"

chmod 755 "$PKG"
find "$PKG" -type d -exec chmod 755 {} +
find "$PKG" -type f ! -path "*/DEBIAN/*" ! -path "*/usr/bin/*" ! -name superv-setup -exec chmod 644 {} +
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT/superv_${VERSION}_amd64.deb" >/dev/null
cp "$OUT/superv_${VERSION}_amd64.deb" "$OUT/superv_amd64.deb"   # stable name for "latest release" links
ls -l "$OUT"
