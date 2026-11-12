# Contributing to SuperV

Thanks for helping! Bug reports, fixes, new kaomoji and symbols, translations of the docs and packaging help are all welcome.

## Reporting a bug

Open an [issue](https://github.com/saikatnjm/superv/issues/new/choose) and include the output of:

```bash
superv --doctor
```

plus your distro, desktop (GNOME, KDE, Cinnamon…), Wayland or X11, and the app you were pasting or typing into.
For more detail, run SuperV in a terminal with logging on and repeat what went wrong (logs show clipboard *format names*, never what you copied):

```bash
superv --quit; SUPERV_DEBUG=1 superv --daemon
```

## Building

```bash
sudo apt install build-essential pkg-config libgtk-3-dev   # Fedora: gcc make gtk3-devel libX11-devel
make
./superv --quit; ./superv --daemon &   # run your build
./superv                               # the clipboard panel
./superv --emoji                       # the emoji panel
```

## How the code is laid out

| File | What it is |
|---|---|
| `superv.c` | The program: clipboard history, settings, startup, updates, the shared virtual keyboard |
| `emoji.c` | The emoji panel, started only while it is turned on |
| `extras.c` | Plain-text paste, colour picker, copy text from the screen, toasts, screen capture: nothing runs until you press one |
| `shot.c` | The screenshot tool: alive only while you take a screenshot (and for pins) |
| `superv.h` | What they share: the virtual keyboard, shortcuts, theme and colour, panel placing |
| `emoji-data.h` | Built-in emoji, kaomoji and symbols (generated, see below) |
| `superv-setup` | Registers Super+V and every feature's shortcut with the desktop (`--feature NAME`), and `--doctor` |

## Guidelines

- **Keep it light.** SuperV is one small C program with GTK 3 and no runtime dependencies beyond it. Please don't add libraries, runtimes or network access.
- **Only the clipboard is on by default.** Every other feature has a switch in the settings, starts off, and while it is off must not load anything or hold a shortcut.
- **No lag.** Nothing slow on the path from a shortcut to its window: no file reads, no full-screen repaints on mouse moves, no work that can wait until the window is up.
- **Behave like Windows** unless there is a good Linux reason not to.
- Keep the code style of the file you're editing (4-space indent, C11 + GNU extensions, GLib types).
- Build with no warnings: `make CFLAGS="-Os -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror"` (CI adds the size flags from the Makefile).
- Test on Wayland and on X11 if you change anything about pasting or the clipboard.
- One topic per pull request, with a short description of what changed and how you tested it.

## Adding kaomoji or symbols

Edit [`data/kaomoji.txt`](data/kaomoji.txt) or [`data/symbols.txt`](data/symbols.txt) (the format is described at the top of each file), then regenerate the built-in data:

```bash
make data    # downloads the latest Unicode emoji list + CLDR keywords, rebuilds emoji-data.h
make
```

## Releasing

See [packaging/PUBLISHING.md](packaging/PUBLISHING.md). Add every version to [CHANGELOG.md](CHANGELOG.md). The
[versions page](docs/versions.html) lists only the stable version of each major line: point its download links at a new
release of the current line, and add a row when a new major version comes out.

## License

By contributing you agree that your contribution is licensed under the [MIT License](LICENSE).
Emoji names and keywords in `emoji-data.h` come from Unicode and CLDR under the [Unicode License v3](LICENSES/Unicode-3.0.txt).
