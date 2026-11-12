#!/usr/bin/env bash
# Builds dist/superv_<version>_amd64.deb: the double-click installer for Ubuntu, Mint, Debian, Pop!_OS…
# Usage: packaging/build-deb.sh   (uses bin/superv-x86_64; run `make` + copy it there first)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -n 's/^#define SV_VERSION *"\(.*\)"/\1/p' "$ROOT/superv.h")"
BIN="$ROOT/bin/superv-x86_64"
OUT="$ROOT/dist"
PKG="$(mktemp -d)"
trap 'rm -r "$PKG"' EXIT

install -Dm755 "$BIN"                                       "$PKG/usr/bin/superv"
install -Dm755 "$ROOT/superv-setup"                         "$PKG/usr/lib/superv/superv-setup"
install -Dm644 "$ROOT/data/io.github.superv.SuperV.desktop" "$PKG/usr/share/applications/io.github.superv.SuperV.desktop"
install -Dm644 "$ROOT/data/io.github.superv.SuperV.svg"     "$PKG/usr/share/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg"
install -Dm644 "$ROOT/data/io.github.superv.SuperV-64.png"  "$PKG/usr/share/icons/hicolor/64x64/apps/io.github.superv.SuperV.png"
install -Dm644 "$ROOT/data/io.github.superv.SuperV-128.png" "$PKG/usr/share/icons/hicolor/128x128/apps/io.github.superv.SuperV.png"
# app-store listing (Ubuntu Software, GNOME Software, KDE Discover, Mint Software Manager)
install -Dm644 "$ROOT/data/io.github.superv.SuperV.metainfo.xml" "$PKG/usr/share/metainfo/io.github.superv.SuperV.metainfo.xml"
install -Dm644 "$ROOT/data/superv-autostart.desktop"        "$PKG/etc/xdg/autostart/superv.desktop"
# click-to-paste on Wayland: let the logged-in user use /dev/uinput
install -Dm644 "$ROOT/data/70-superv-uinput.rules"          "$PKG/usr/lib/udev/rules.d/70-superv-uinput.rules"
install -Dm644 "$ROOT/data/superv-uinput.conf"              "$PKG/usr/lib/modules-load.d/superv-uinput.conf"
# MIT for SuperV, plus the Unicode licence of the emoji names and keywords
{ cat "$ROOT/LICENSE"; printf '\nEmoji names and keywords (emoji-data.h) come from Unicode and CLDR:\n\n'
  cat "$ROOT/LICENSES/Unicode-3.0.txt"; } | install -Dm644 /dev/stdin "$PKG/usr/share/doc/superv/copyright"
install -d "$PKG/DEBIAN"

cat > "$PKG/DEBIAN/control" <<EOF
Package: superv
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.35), libgtk-3-0 (>= 3.22), python3, python3-gi, gir1.2-glib-2.0
Recommends: fonts-noto-color-emoji, libxtst6
Suggests: tesseract-ocr
Provides: superdot
Conflicts: superdot
Replaces: superdot
Installed-Size: $(du -sk "$PKG/usr" | cut -f1)
Maintainer: SuperV <saikatnjm@users.noreply.github.com>
Homepage: https://github.com/saikatnjm/superv
Description: Clipboard, emoji and more Windows shortcuts for Linux: Win+V, Win+. and more
 SuperV brings Windows shortcuts to Linux. Super+V shows the last 25 things
 you copied (text and images): pick one to paste it into the app you are
 using, pin favourites, clear the rest. The others are off until you turn
 them on in its settings: Super+. emoji panel, Super+Shift+S screenshots
 with drawing tools, Ctrl+Super+Alt+V paste as plain text, Super+Shift+C
 colour picker, Super+Shift+T copy text from the screen (with
 tesseract-ocr). Works on GNOME, KDE Plasma, Cinnamon, MATE and Xfce
 (Wayland and X11). Never connects to the internet.
 .
 One small C program. Replaces SuperDot, which is now part of SuperV.
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
    command -v appstreamcli >/dev/null && appstreamcli refresh-cache >/dev/null 2>&1 || true
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
            /usr/bin/python3 /usr/lib/superv/superv-setup --all --remove-hotkeys >/dev/null 2>&1 || true
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            gdbus call --session --dest io.github.superv.SuperV --object-path /io/github/superv/SuperV \
            --method org.gtk.Actions.Activate quit "[]" "{}" >/dev/null 2>&1 || true
    done
fi
exit 0
EOF
cat > "$PKG/DEBIAN/postrm" <<'EOF'
#!/bin/sh
# The typing permission SuperV's "Enable" button added (when the package's own rule wasn't active yet).
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    rm -f /etc/udev/rules.d/70-superv-uinput.rules /etc/modules-load.d/superv-uinput.conf
fi
exit 0
EOF
chmod 755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/prerm" "$PKG/DEBIAN/postrm"

chmod 755 "$PKG"
find "$PKG" -type d -exec chmod 755 {} +
find "$PKG" -type f ! -path "*/DEBIAN/*" ! -path "*/usr/bin/*" ! -name superv-setup -exec chmod 644 {} +
mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT/superv_${VERSION}_amd64.deb" >/dev/null
cp "$OUT/superv_${VERSION}_amd64.deb" "$OUT/superv_amd64.deb"   # stable name for "latest release" links
ls -l "$OUT"
