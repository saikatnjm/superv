# Security Policy

## Supported versions

Only the latest release gets security fixes. Older versions stay downloadable on
[GitHub Releases](https://github.com/saikatnjm/superv/releases), but please update.

## Reporting a vulnerability

Please **don't open a public issue**. Report it privately through GitHub's
[private vulnerability reporting](https://github.com/saikatnjm/superv/security/advisories/new).
You'll get a reply within a week, and a fix is released as soon as possible.

## What SuperV can access

So you can judge the impact of a bug:

- **Pasting and typing for you:** to paste into Wayland apps SuperV creates a virtual keyboard through `/dev/uinput`. The packages and installer add a udev rule (`70-superv-uinput.rules`) that gives the *logged-in* user access to `/dev/uinput`. Any program running as that user can then also create virtual input devices. Remove the rule (or install with `install.sh --no-sudo`) if you don't want that; SuperV then only copies.
- **Clipboard history:** what you copy (text, and images as PNG files) is kept in `~/.local/share/superv/`, readable only by you. Copies marked as passwords by password managers are never recorded. Unpinned items are deleted when you restart the computer.
- **Emoji panel:** while you pick an emoji, SuperV keeps a copy of your clipboard in memory (never on disk) so it can put it back. It is dropped once you copy something else. The emoji itself is marked so clipboard histories don't record it.
- **Files:** settings in `~/.config/superv/` (`config.ini`, `emoji.ini`) and recently used emoji in `~/.local/share/superv/emoji-recent.txt`, all readable only by you.
- **Network:** none. SuperV never connects to the internet.
