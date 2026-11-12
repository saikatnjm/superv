# Changelog

All notable changes to SuperV. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).
Every release can be downloaded from [GitHub Releases](https://github.com/saikatnjm/superv/releases); the [versions page](https://saikatnjm.github.io/superv/versions.html) shows the stable version of each line.

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

[2.0.0]: https://github.com/saikatnjm/superv/releases/tag/v2.0.0
[1.2.3]: https://github.com/saikatnjm/superv/releases/tag/v1.2.3
[1.2.2]: https://github.com/saikatnjm/superv/releases/tag/v1.2.2
[1.2.1]: https://github.com/saikatnjm/superv/releases/tag/v1.2.1
[1.2.0]: https://github.com/saikatnjm/superv/releases/tag/v1.2.0
[1.1.2]: https://github.com/saikatnjm/superv/releases/tag/v1.1.2
[1.1.1]: https://github.com/saikatnjm/superv/releases/tag/v1.1.1
[1.1.0]: https://github.com/saikatnjm/superv/releases/tag/v1.1.0
[1.0.0]: https://github.com/saikatnjm/superdot/releases/tag/v1.0.0
