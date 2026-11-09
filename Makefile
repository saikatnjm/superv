# SuperV
#   make                          build ./superv
#   sudo make install             install system-wide (default PREFIX=/usr/local)
#   make install PREFIX=/usr DESTDIR=pkg/   staged install, used by the .deb/.rpm/Arch packages
#   sudo make uninstall
# Build needs a C compiler, pkg-config and the GTK 3 + X11 development files:
#   Debian/Ubuntu/Mint: sudo apt install build-essential pkg-config libgtk-3-dev
#   Fedora:             sudo dnf install gcc make pkgconf-pkg-config gtk3-devel libX11-devel
#   Arch/Manjaro:       sudo pacman -S --needed base-devel gtk3
#   openSUSE:           sudo zypper install gcc make pkg-config gtk3-devel libX11-devel

CC         ?= gcc
CFLAGS     ?= -Os -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
PKGS        = gtk+-3.0 x11
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib/superv
DATADIR    ?= $(PREFIX)/share
SYSCONFDIR ?= /etc
UDEVDIR    ?= $(PREFIX)/lib/udev/rules.d
MODLOADDIR ?= $(PREFIX)/lib/modules-load.d

superv: superv.c
	$(CC) -std=gnu11 $(CFLAGS) -DSETUP_DIR='"$(LIBDIR)"' -o $@ $< \
		$$(pkg-config --cflags --libs $(PKGS)) -ldl -Wl,--as-needed $(LDFLAGS)

install: superv
	install -Dm755 superv                              $(DESTDIR)$(BINDIR)/superv
	install -Dm755 superv-setup                        $(DESTDIR)$(LIBDIR)/superv-setup
	install -Dm644 data/io.github.superv.SuperV.desktop $(DESTDIR)$(DATADIR)/applications/io.github.superv.SuperV.desktop
	install -Dm644 data/io.github.superv.SuperV.svg    $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg
	install -Dm644 data/superv-autostart.desktop       $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/superv.desktop
	install -Dm644 data/70-superv-uinput.rules         $(DESTDIR)$(UDEVDIR)/70-superv-uinput.rules
	install -Dm644 data/superv-uinput.conf             $(DESTDIR)$(MODLOADDIR)/superv-uinput.conf
	install -Dm644 LICENSE                             $(DESTDIR)$(DATADIR)/licenses/superv/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/superv $(DESTDIR)$(LIBDIR)/superv-setup \
	      $(DESTDIR)$(DATADIR)/applications/io.github.superv.SuperV.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg \
	      $(DESTDIR)$(SYSCONFDIR)/xdg/autostart/superv.desktop \
	      $(DESTDIR)$(UDEVDIR)/70-superv-uinput.rules \
	      $(DESTDIR)$(MODLOADDIR)/superv-uinput.conf \
	      $(DESTDIR)$(DATADIR)/licenses/superv/LICENSE
	-rmdir $(DESTDIR)$(LIBDIR) $(DESTDIR)$(DATADIR)/licenses/superv 2>/dev/null

clean:
	rm -f superv

.PHONY: install uninstall clean
