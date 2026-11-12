# SuperV: clipboard history (Win+V), emoji panel (Win+.) and more Windows shortcuts for Linux
#   make                          build ./superv
#   sudo make install             install system-wide (default PREFIX=/usr/local)
#   make install PREFIX=/usr DESTDIR=pkg/   staged install, used by the .deb/.rpm/Arch packages
#   sudo make uninstall
#   make data                     re-download Unicode emoji + CLDR keywords and rebuild emoji-data.h
# Build needs a C compiler, pkg-config and the GTK 3 + X11 development files:
#   Debian/Ubuntu/Mint: sudo apt install build-essential pkg-config libgtk-3-dev
#   Fedora:             sudo dnf install gcc make pkgconf-pkg-config gtk3-devel libX11-devel
#   Arch/Manjaro:       sudo pacman -S --needed base-devel gtk3
#   openSUSE:           sudo zypper install gcc make pkg-config gtk3-devel libX11-devel

CC         ?= gcc
# small on disk: no unwind tables (plain C, nothing unwinds), direct calls instead of PLT stubs, unused code dropped
CFLAGS     ?= -Os -fno-asynchronous-unwind-tables -fno-plt -ffunction-sections -fdata-sections \
              -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LDFLAGS    ?= -Wl,--gc-sections
PKGS        = gtk+-3.0 x11
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib/superv
DATADIR    ?= $(PREFIX)/share
SYSCONFDIR ?= /etc
UDEVDIR    ?= $(PREFIX)/lib/udev/rules.d
MODLOADDIR ?= $(PREFIX)/lib/modules-load.d
UNICODE     = https://unicode.org/Public/emoji/latest/emoji-test.txt
CLDR        = https://raw.githubusercontent.com/unicode-org/cldr/main/common

superv: superv.c emoji.c extras.c shot.c superv.h emoji-data.h
	$(CC) -std=gnu11 $(CFLAGS) -DSETUP_DIR='"$(LIBDIR)"' -o $@ superv.c emoji.c extras.c shot.c \
		$$(pkg-config --cflags --libs $(PKGS)) -ldl -lm -Wl,--as-needed $(LDFLAGS)

data: tools/gen-data.py data/kaomoji.txt data/symbols.txt
	mkdir -p tools/cache
	curl -sSfL -o tools/cache/emoji-test.txt $(UNICODE)
	curl -sSfL -o tools/cache/en.xml $(CLDR)/annotations/en.xml
	curl -sSfL -o tools/cache/en-derived.xml $(CLDR)/annotationsDerived/en.xml
	python3 -I tools/gen-data.py tools/cache/emoji-test.txt tools/cache/en.xml tools/cache/en-derived.xml > emoji-data.h

install: superv
	install -Dm755 superv                              $(DESTDIR)$(BINDIR)/superv
	install -Dm755 superv-setup                        $(DESTDIR)$(LIBDIR)/superv-setup
	install -Dm644 data/io.github.superv.SuperV.desktop $(DESTDIR)$(DATADIR)/applications/io.github.superv.SuperV.desktop
	install -Dm644 data/io.github.superv.SuperV.svg    $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg
	install -Dm644 data/io.github.superv.SuperV-64.png $(DESTDIR)$(DATADIR)/icons/hicolor/64x64/apps/io.github.superv.SuperV.png
	install -Dm644 data/io.github.superv.SuperV-128.png $(DESTDIR)$(DATADIR)/icons/hicolor/128x128/apps/io.github.superv.SuperV.png
	install -Dm644 data/io.github.superv.SuperV.metainfo.xml $(DESTDIR)$(DATADIR)/metainfo/io.github.superv.SuperV.metainfo.xml
	install -Dm644 data/superv-autostart.desktop       $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/superv.desktop
	install -Dm644 data/70-superv-uinput.rules         $(DESTDIR)$(UDEVDIR)/70-superv-uinput.rules
	install -Dm644 data/superv-uinput.conf             $(DESTDIR)$(MODLOADDIR)/superv-uinput.conf
	install -Dm644 LICENSE                             $(DESTDIR)$(DATADIR)/licenses/superv/LICENSE
	install -Dm644 LICENSES/Unicode-3.0.txt            $(DESTDIR)$(DATADIR)/licenses/superv/Unicode-3.0.txt

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/superv $(DESTDIR)$(LIBDIR)/superv-setup \
	      $(DESTDIR)$(DATADIR)/applications/io.github.superv.SuperV.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/64x64/apps/io.github.superv.SuperV.png \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/128x128/apps/io.github.superv.SuperV.png \
	      $(DESTDIR)$(DATADIR)/metainfo/io.github.superv.SuperV.metainfo.xml \
	      $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/superv.desktop \
	      $(DESTDIR)$(UDEVDIR)/70-superv-uinput.rules \
	      $(DESTDIR)$(MODLOADDIR)/superv-uinput.conf \
	      $(DESTDIR)$(DATADIR)/licenses/superv/LICENSE $(DESTDIR)$(DATADIR)/licenses/superv/Unicode-3.0.txt
	-rmdir $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DATADIR)/licenses/superv 2>/dev/null

clean:
	rm -f superv

.PHONY: install uninstall clean data
