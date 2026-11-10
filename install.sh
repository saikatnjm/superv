#!/usr/bin/env bash
# SuperV installer — Win+V clipboard history for Linux.
# Ubuntu, Mint, Debian, Pop!_OS, Zorin, elementary, Fedora, Arch, Manjaro, openSUSE…
#
#   ./install.sh                         Super+V (like Windows), asks for your password once
#   ./install.sh --hotkey "<Ctrl><Alt>v" use your own shortcut (change it later from the ⚙ menu)
#   ./install.sh --no-sudo               skip the password step (picking an item then only copies it)
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$HOME/.local/lib/superv"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
AUTOSTART_DIR="$HOME/.config/autostart"
HOTKEY="<Super>v"
NO_SUDO=0
while (($#)); do
  case "$1" in
    --hotkey) HOTKEY="$2"; shift 2 ;;
    --no-sudo) NO_SUDO=1; shift ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
done

V=$'\e[38;5;141m'; B=$'\e[1;38;5;141m'; N=$'\e[0m'; OK="${V}✓${N}"
step() { printf '\n%s%s%s\n' "$B" "$1" "$N"; }

# Install packages with whatever package manager this distro uses.
#   need build   -> C compiler, pkg-config, GTK 3 + X11 development files
#   need python  -> Python GObject bindings (used by the one-time setup helper)
need() {
  local pkgs=""
  if command -v apt-get >/dev/null; then
    [[ $1 == build ]] && pkgs="build-essential pkg-config libgtk-3-dev" || pkgs="python3-gi gir1.2-glib-2.0"
    echo "  installing: $pkgs"; sudo apt-get install -y $pkgs >/dev/null
  elif command -v dnf >/dev/null; then
    [[ $1 == build ]] && pkgs="gcc make pkgconf-pkg-config gtk3-devel libX11-devel" || pkgs="python3-gobject"
    echo "  installing: $pkgs"; sudo dnf install -y $pkgs >/dev/null
  elif command -v pacman >/dev/null; then
    [[ $1 == build ]] && pkgs="base-devel pkgconf gtk3 libx11" || pkgs="python-gobject"
    echo "  installing: $pkgs"; sudo pacman -S --needed --noconfirm $pkgs >/dev/null
  elif command -v zypper >/dev/null; then
    [[ $1 == build ]] && pkgs="gcc make pkg-config gtk3-devel libX11-devel" || pkgs="python3-gobject"
    echo "  installing: $pkgs"; sudo zypper --non-interactive install $pkgs >/dev/null
  else
    echo "  Couldn't find apt, dnf, pacman or zypper. Please install:"
    [[ $1 == build ]] && echo "    a C compiler, make, pkg-config and the GTK 3 + X11 development packages"                       || echo "    the Python GObject bindings (often called python3-gobject or python3-gi)"
    exit 1
  fi
}

step "1/6  Getting the SuperV binary"
BIN=""
if [[ "$(uname -m)" == "x86_64" && -x "$SRC_DIR/bin/superv-x86_64" ]] \
   && "$SRC_DIR/bin/superv-x86_64" --version >/dev/null 2>&1; then
  BIN="$SRC_DIR/bin/superv-x86_64"
  echo "  $OK prebuilt binary works on this system ($(du -h "$BIN" | cut -f1))"
else
  echo "  building from source (prebuilt binary doesn't fit this system)…"
  if ! command -v cc >/dev/null || ! command -v make >/dev/null || ! command -v pkg-config >/dev/null \
     || ! pkg-config --exists gtk+-3.0 x11 2>/dev/null; then
    need build
  fi
  make -C "$SRC_DIR" -B >/dev/null
  BIN="$SRC_DIR/superv"
  echo "  $OK built ($(du -h "$BIN" | cut -f1))"
fi
python3 -c 'from gi.repository import Gio' 2>/dev/null || need python

step "2/6  Installing to $LIB_DIR"
"$BIN_DIR/superv" --quit >/dev/null 2>&1 || true
mkdir -p "$LIB_DIR" "$BIN_DIR" "$APPS_DIR" "$AUTOSTART_DIR"
install -m 755 "$BIN" "$LIB_DIR/superv"
install -m 755 "$SRC_DIR/superv-setup" "$LIB_DIR/superv-setup"
install -m 755 "$SRC_DIR/uninstall.sh" "$LIB_DIR/uninstall.sh"
install -Dm 644 "$SRC_DIR/data/io.github.superv.SuperV.svg" \
  "$HOME/.local/share/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg"
ln -sf "$LIB_DIR/superv" "$BIN_DIR/superv"
ln -sf "$LIB_DIR/superv-setup" "$BIN_DIR/superv-setup"
echo "  $OK installed"

step "3/6  Start at login + app menu entry"
cat > "$AUTOSTART_DIR/superv.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=SuperV
Comment=Clipboard for Linux (Super+V)
Exec="$LIB_DIR/superv" --daemon
Icon=io.github.superv.SuperV
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
cat > "$APPS_DIR/io.github.superv.SuperV.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=SuperV
GenericName=Clipboard history
Comment=Clipboard for Linux: press Super+V for your clipboard history
Exec="$LIB_DIR/superv" --show
Icon=io.github.superv.SuperV
Categories=Utility;
Keywords=clipboard;history;paste;copy;win+v;
StartupNotify=false
EOF
echo "  $OK done"

step "4/6  Keyboard shortcut: $HOTKEY  (+ Super+Shift+S screenshot)"
python3 "$LIB_DIR/superv-setup" --setup-hotkeys "'$LIB_DIR/superv' --toggle" --hotkey "$HOTKEY"

step "5/6  Auto-paste permission"
RELOGIN=0
if [[ -w /dev/uinput ]]; then
  echo "  $OK already enabled"
elif ((NO_SUDO)); then
  echo "  skipped (--no-sudo): picking an item copies it; press Ctrl+V yourself"
else
  echo "  After you pick an item SuperV presses Shift+Insert for you, just like Windows pastes."
  echo "  On Wayland that needs access to /dev/uinput — your password is needed once."
  echo 'KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", TAG+="uaccess"' \
    | sudo tee /etc/udev/rules.d/70-superv-uinput.rules >/dev/null
  echo uinput | sudo tee /etc/modules-load.d/superv-uinput.conf >/dev/null
  sudo modprobe uinput
  sudo udevadm control --reload-rules
  sudo udevadm trigger --action=change --sysname-match=uinput || true
  sudo udevadm settle || true
  sleep 1
  if [[ -w /dev/uinput ]]; then echo "  $OK enabled"
  else echo "  $OK installed — active after you log out and back in"; RELOGIN=1; fi
fi

step "6/6  Starting SuperV"
setsid "$LIB_DIR/superv" --daemon >/dev/null 2>&1 < /dev/null &
sleep 1
"$LIB_DIR/superv-setup" --doctor | sed 's/^/  /'

printf '\n%sSuperV is ready.%s Copy a few things, then press your shortcut.\n' "$B" "$N"
((RELOGIN)) && echo "Log out and back in once so auto-paste starts working."
echo "Uninstall any time: ~/.local/lib/superv/uninstall.sh"
