# Changelog

All notable changes to SuperV. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
Every release can be downloaded from [GitHub Releases](https://github.com/saikatnjm/superv/releases); the [versions page](https://saikatnjm.github.io/superv/versions.html) shows the stable version of each line.

## [2.1.0] - 2026-11-12

Four more Windows shortcuts, a clipboard that shows what you already pasted, and a lighter, faster program. **Only the clipboard history is on at first**: every other feature has its own switch in the settings (Super+V, then the gear, **Features**), and one that is off loads nothing and leaves its keys to your desktop.

### Added
- **See what you already pasted.** Paste several items one after another and each one gets a ✓ and fades; the latest is marked **Last pasted**, and the panel opens with the next one selected, so Enter pastes it. The list keeps its order while you paste from it (pasting no longer moves an item to the top; copying it again still does). The ticks clear when you start copying a new batch.
- **Features are off until you turn them on.** A new install sets up Super+V only. The first time the panel opens, a tip says what else there is; clicking ☺ asks before turning on the emoji panel; pressing a feature's shortcut while it is off shows a card saying where its switch is. Coming from 2.0 or SuperDot? What you had on stays on.
- **Paste as plain text** on <kbd>Ctrl</kbd>+<kbd>Super</kbd>+<kbd>Alt</kbd>+<kbd>V</kbd> (Ctrl+Win+Alt+V, PowerToys Advanced Paste): what you copied is pasted without fonts, colours, links or images.
- **Colour picker** on <kbd>Super</kbd>+<kbd>Shift</kbd>+<kbd>C</kbd> (Win+Shift+C, PowerToys Color Picker): click anywhere on screen; the colour is copied as HEX and a small card offers RGB and HSL. On X11 the screen freezes with a magnifier; on Wayland the desktop's own picker is used.
- **Copy text from the screen** on <kbd>Super</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> (Win+Shift+T, PowerToys Text Extractor): select an area and its text is copied. Uses Tesseract when it is installed (`tesseract-ocr`), in English plus your own language when its data is installed. Nothing leaves your computer.
- **Screenshots** on <kbd>Super</kbd>+<kbd>Shift</kbd>+<kbd>S</kbd> (Win+Shift+S), Flameshot-style: the screen freezes, you select an area (resize it with its handles, move it, see its size), draw on it with pen, line, arrow, rectangle, circle, marker, text, pixelate and numbers in eight colours and any thickness (mouse wheel), undo and redo, then copy it, save it to Pictures/Screenshots, pin it on top of everything (drag, zoom, right-click to copy or save) or copy its text. Keyboard shortcuts like Flameshot's; `--delay N`. No dialog on GNOME: SuperV presses GNOME's own instant screenshot (Shift+Print), takes the picture, deletes GNOME's file and puts your clipboard back; other Wayland desktops are asked through the portal. Everything is freed when you're done.
- **One colour for every SuperV window**: a Colour row in the settings (violet, SuperV's, by default; azure, graphite, emerald, amber, rose) colours the clipboard panel, emoji panel, colour card, toasts, screenshot tool and overlays alike.
- A **Features** section in the settings with the switches for the emoji panel and the four new shortcuts.
- `superv --plain-paste`, `--pick-color`, `--extract-text`, `--screenshot`.
- Pressing Super+Shift+T without Tesseract opens a card with an **Install Tesseract** button (your password once, through the desktop's dialog), then starts the capture.
- Results ("Text copied", "No text found"…) show as a small toast at the bottom of the screen, which also appears with Do Not Disturb on.
- The colour card opens next to where you clicked, on that monitor, and can be dragged.

### Changed
- **Faster.** The clipboard panel opens in about 0.03 s, and 0.05 s the first time after login. With ten pictures in the history the first opening took 0.56 s; it now takes about 0.1 s: pictures get a small copy for the list, are saved in the background, and the list is rebuilt only when the history changed. The panel is prepared in the background after login. The screenshot tool, colour picker and text selection redraw only what the pointer changed instead of the whole screen on every mouse move.
- **Lighter.** About 5.5 MB of private memory as installed (clipboard only). The program is 25 KB smaller than the first 2.1 builds (363 KB) although it does more: code the clipboard and emoji panels each had (placing, resizing, shortcuts, the typing permission, theme) exists once now. History is written at most once every 0.4 s instead of on every change.
- **Copy text from the screen** uses the screenshot tool's frozen screen on every desktop: press Super+Shift+T, drag over the text, let go. On GNOME (Wayland) it used to open GNOME's own screenshot dialog, which took several clicks, could capture the whole screen and left files in Pictures.
- One setup helper: `superv-setup --feature NAME` registers any feature's shortcut (`--all --remove-hotkeys` gives every one back). `superv-emoji-setup` and `superv-feature-setup` are gone.
- The emoji panel takes its theme and colour from SuperV's settings; its own Theme and Colour rows are gone. The one-time typing permission (Enable) is shared by both panels.
- `install.sh` sets up Super+V only (`--no-emoji` is still accepted and does nothing), refuses to run as root, and uses the system's Python even when another one is first in your PATH.
- Desktop shortcut changes run one at a time, so two of them can no longer overwrite each other in GNOME's shortcut list.
- Tesseract is a suggested package (`Suggests`/`optdepends`), not a requirement.
- Switching between the clipboard and emoji panels (☺, 📋, Super+V, Super+.) opens the other one in the same spot.
- The emoji panel uses SuperV's violet unless you picked another colour (SuperDot's old default, azure, becomes violet).
- The emoji panel is solid by default, like the clipboard panel. Its **Glass effect** switch (in the emoji panel's ⚙) turns the see-through look on.

### Fixed
- Text copied from LibreOffice and other office apps was sometimes stored as a picture of the text.
- After a copy that isn't recorded (a password manager's), SuperV could put the previous item back on the clipboard.
- A pinned screenshot's right-click menu (Copy, Save, Close) did nothing.
- On GNOME, the instant screenshot only takes a picture GNOME itself made after the key press (never a PNG you saved in Pictures at that moment), finds GNOME's Screenshots folder in your language, no longer creates folders, and removes a second picture if both key presses arrived.
- The screenshot tool: the selection can't be dragged off the screen; text marks accept your input method, dead keys and non-Latin layouts; Ctrl+C, Ctrl+S and the tool keys work on non-Latin layouts; a pin keeps its size when a later screenshot is taken on a monitor with another scale; redo no longer brings back marks from an earlier selection.
- The colour picker and text selection keep trying for 1.5 s to take the keyboard and mouse instead of giving up at once, and never show SuperV's own toast or colour card in the picture.
- A click on a toast could make the next toast disappear early. A Tesseract that fails is reported instead of "No text found". A desktop portal that never answers no longer blocks the next capture.
- Shortcut setup: MATE's free slot was not recognised; Xfce reported success when the shortcut wasn't saved and lost a custom command it replaced; removing shortcuts on KDE from a package's removal script did nothing; restoring a desktop shortcut list put back only what SuperV changed, in its original order.
- An update while a shortcut was being registered could restart SuperV in the middle of it; unsaved history is written before restarting.
- Removing the package also removes the typing permission the Enable button added.

## [2.0.0] - 2026-11-12

SuperV becomes "Clipboard and emoji for Linux": the Win+. emoji panel, until now the separate app SuperDot, is part of SuperV.

### Added
- **Emoji panel on <kbd>Super</kbd> + <kbd>.</kbd>** (Win+. on Windows): Emoji, Kaomoji and Symbols tabs, search by name and keywords, recently used first, skin tones, glass look, six colour palettes. Picking one types it where you were writing and puts your clipboard back.
- **Emoji panel switch** in the settings (Super+V, then the gear). Off: the emoji part isn't loaded at all (SuperV uses about 5 MB of private memory instead of 11–18 MB) and Super+. goes back to your desktop.
- ☺ button in the clipboard panel and 📋 button in the emoji panel to switch between them, like the tabs on top of Windows 11's Win+V panel; Super+V and Super+. also switch.
- `superv --emoji`, with `--tab kaomoji|symbols`, `--search TEXT` and `--insert TEXT`.
- `install.sh --no-emoji` installs without the emoji panel.
- CHANGELOG, CONTRIBUTING, CODE_OF_CONDUCT, SECURITY, issue and pull-request templates, and a CI check (build with `-Werror`, shellcheck, AppStream and desktop-file validation).
- A [versions page](https://saikatnjm.github.io/superv/versions.html) with the stable version of each line (2.0, the last 1.x, SuperDot), what each one has, and download links.

### Changed
- One program and one background process for both panels: one install, one virtual keyboard, about 40 KB smaller than SuperV 1.2.3 and SuperDot 1.0.0 together.
- `superv --doctor` checks both panels; `superv --remove-hotkeys` gives back both shortcuts.
- Packages replace SuperDot (`Provides`/`Conflicts`/`Replaces` on Debian/Ubuntu, `Obsoletes` on Fedora/openSUSE, `replaces` on Arch). SuperDot's settings and recently used emoji are brought over on first start.

## [1.2.3] - 2026-11-10

### Changed
- New tagline "Clipboard for Linux" in the store listing, package descriptions, app menu, README and website.

### Added
- Packaging for the AUR (checksum, `.SRCINFO`, `bump.sh`), Fedora COPR and an Ubuntu/Mint PPA (`ppa:saikatnjm/superv`, Ubuntu 22.04 to 26.10, Mint 21 and 22), with a publishing guide.

## [1.2.2] - 2026-11-09

### Fixed
- Ubuntu Software showed no screenshots: the listing now has 1280×720 landscape screenshots with declared sizes.

## [1.2.1] - 2026-11-09

### Added
- Proper app-store listing (AppStream metadata): name, icon, screenshots, MIT licence, age rating and release notes in Ubuntu Software, GNOME Software and KDE Discover; 64 and 128 px icons.

## [1.2.0] - 2026-11-09

### Added
- Packages for Fedora and openSUSE (.rpm) and Arch (PKGBUILD), built by a GitHub workflow on every release; `make install`/`uninstall`.
- `install.sh` fetches build tools with apt, dnf, pacman or zypper.
- KDE Plasma 5/6 shortcut setup (experimental; frees Klipper's Meta+V); exact shortcut lines for Sway, Hyprland, i3 and COSMIC.
- SuperV restarts into the new version after an update, keeping what's on the clipboard.

### Fixed
- Clicking outside closes the panel even with a menu open; long non-ASCII text no longer slows the panel; Reset only resets appearance, placement and shortcut; `superv --quit` never starts SuperV; and other fixes from a code review.

## [1.1.2] - 2026-11-09

### Changed
- Image copies from browsers are saved as the image, not its link; any readable image format is accepted.
- Compact settings menu with Clear history and Reset settings, a keyboard-shortcuts list, version and GitHub link.
- Shortcuts that apps need (Ctrl+V, Alt+V…) are refused with an explanation; a green tick confirms a saved shortcut.

### Added
- `SUPERV_DEBUG=1` logs clipboard format names (never content).

## [1.1.1] - 2026-11-09

### Fixed
- Super+V was not set up again after uninstalling and reinstalling.

## [1.1.0] - 2026-11-09

First public release.

### Added
- Super+V panel with the last 25 texts and images; click or Enter pastes into the app you were using.
- Pin, delete, clear all; unpinned history clears on reboot, like Windows.
- Custom shortcut, System/Light/Dark theme, adjustable text size; movable, resizable panel.
- Click-to-paste through uinput (Wayland) or XTest (X11).
- No network access, owner-only storage, password-manager copies skipped.

## SuperDot [1.0.0] - 2026-11-11

The emoji panel as a separate app, merged into SuperV 2.0.0. Its history is in the [SuperDot repository](https://github.com/saikatnjm/superdot).

[2.1.0]: https://github.com/saikatnjm/superv/releases/tag/v2.1.0
[2.0.0]: https://github.com/saikatnjm/superv/releases/tag/v2.0.0
[1.2.3]: https://github.com/saikatnjm/superv/releases/tag/v1.2.3
[1.2.2]: https://github.com/saikatnjm/superv/releases/tag/v1.2.2
[1.2.1]: https://github.com/saikatnjm/superv/releases/tag/v1.2.1
[1.2.0]: https://github.com/saikatnjm/superv/releases/tag/v1.2.0
[1.1.2]: https://github.com/saikatnjm/superv/releases/tag/v1.1.2
[1.1.1]: https://github.com/saikatnjm/superv/releases/tag/v1.1.1
[1.1.0]: https://github.com/saikatnjm/superv/releases/tag/v1.1.0
[1.0.0]: https://github.com/saikatnjm/superdot/releases/tag/v1.0.0
