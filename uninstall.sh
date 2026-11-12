#!/usr/bin/env bash
# Removes SuperV and puts your desktop's keyboard shortcuts back the way they were.
#   uninstall.sh            keep your history + settings
#   uninstall.sh --purge    delete them too
set -uo pipefail
LIB_DIR="$HOME/.local/lib/superv"

if [[ -x "$LIB_DIR/superv" ]]; then
  "$LIB_DIR/superv" --quit >/dev/null 2>&1
  python3 "$LIB_DIR/superv-setup" --remove-hotkeys
  [[ -f "$LIB_DIR/superv-emoji-setup" ]] && python3 "$LIB_DIR/superv-emoji-setup" --remove-hotkeys
fi
rm -f "$HOME/.config/autostart/superv.desktop" \
      "$HOME/.local/share/applications/io.github.superv.SuperV.desktop" \
      "$HOME/.local/bin/superv" "$HOME/.local/bin/superv-setup" \
      "$HOME/.local/share/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg" \
      "$LIB_DIR/superv" "$LIB_DIR/superv-setup" "$LIB_DIR/superv-emoji-setup" "$LIB_DIR/uninstall.sh"
rmdir "$LIB_DIR" 2>/dev/null

if [[ -f /etc/udev/rules.d/70-superv-uinput.rules || -f /etc/modules-load.d/superv-uinput.conf ]]; then
  echo "Removing the auto-paste permission (needs your password):"
  sudo rm -f /etc/udev/rules.d/70-superv-uinput.rules /etc/modules-load.d/superv-uinput.conf
  sudo udevadm control --reload-rules
fi

if [[ "${1:-}" == "--purge" ]]; then
  rm -f "$HOME/.local/share/superv/history.ini" "$HOME/.local/share/superv/images/"*.png \
        "$HOME/.config/superv/config.ini" "$HOME/.config/superv/hotkey-backup.json" \
        "$HOME/.config/superv/emoji.ini" "$HOME/.config/superv/emoji-hotkey-backup.json" \
        "$HOME/.local/share/superv/emoji-recent.txt"
  rmdir "$HOME/.local/share/superv/images" "$HOME/.local/share/superv" "$HOME/.config/superv" 2>/dev/null
  echo "History and settings deleted."
else
  echo "Kept your history (~/.local/share/superv) and settings (~/.config/superv). Use --purge to delete."
fi
echo "SuperV removed."
