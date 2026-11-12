# RPM package for Fedora and openSUSE. Published on Fedora COPR (saikatnjm/superv),
# which downloads Source0 from the GitHub release tag.
# Local build: spectool -g -R packaging/rpm/superv.spec && rpmbuild -bb packaging/rpm/superv.spec
# The GitHub release workflow builds it too, from a local tarball of the same name.
Name:           superv
Version:        2.1.0
Release:        1%{?dist}
Summary:        Clipboard, emoji and more Windows shortcuts for Linux
License:        MIT AND Unicode-3.0
URL:            https://github.com/saikatnjm/superv
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(x11)
BuildRequires:  systemd-rpm-macros
# GTK and X11 libraries are detected automatically from the binary (names differ per distro).
Requires:       python3
Requires:       python3-gobject
%if 0%{?suse_version}
Requires:       typelib(Gio)
%endif
# libXtst is loaded at runtime when present (click-to-paste on X11 without /dev/uinput).
Recommends:     libXtst
# colour emoji in the emoji panel (Fedora: google-noto-emoji-color-fonts, openSUSE: noto-coloremoji-fonts)
Recommends:     (google-noto-color-emoji-fonts or google-noto-emoji-color-fonts or noto-coloremoji-fonts)
# SuperDot (the Win+. emoji panel) is now part of SuperV
Obsoletes:      superdot < 2
# copying text from the screen (Super+Shift+T)
Suggests:       tesseract
Provides:       superdot = %{version}-%{release}

%description
SuperV brings Windows shortcuts to Linux. Super+V shows the last 25 things
you copied, text and images: pick one to paste it into the app you are using,
pin favourites, clear the rest. The others are off until you turn them on in
its settings: Super+. emoji panel, Super+Shift+S screenshots with drawing
tools, Ctrl+Super+Alt+V paste as plain text, Super+Shift+C colour picker,
Super+Shift+T copy text from the screen (with tesseract). Works on GNOME, KDE
Plasma, Cinnamon, MATE and Xfce, on Wayland and X11. No network access, ever.
Replaces SuperDot, which is now part of SuperV.

%prep
%autosetup

%build
%make_build CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}" PREFIX=%{_prefix}

%install
%make_install PREFIX=%{_prefix} UDEVDIR=%{_udevrulesdir} MODLOADDIR=%{_modulesloaddir} SYSCONFDIR=%{_sysconfdir}
# the licence is shipped with %%license below
rm -rf %{buildroot}%{_datadir}/licenses/superv

%post
modprobe uinput >/dev/null 2>&1 || :
udevadm control --reload-rules >/dev/null 2>&1 || :
udevadm trigger --action=change --sysname-match=uinput >/dev/null 2>&1 || :

%preun
# On removal (not upgrade): give logged-in users their desktop shortcuts back and stop SuperV.
if [ "$1" -eq 0 ]; then
    for dir in /run/user/*; do
        uid="${dir##*/}"; bus="$dir/bus"
        [ -S "$bus" ] || continue
        user="$(id -nu "$uid" 2>/dev/null)" || continue
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            python3 %{_prefix}/lib/superv/superv-setup --all --remove-hotkeys >/dev/null 2>&1 || :
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            gdbus call --session --dest io.github.superv.SuperV --object-path /io/github/superv/SuperV \
            --method org.gtk.Actions.Activate quit "[]" "{}" >/dev/null 2>&1 || :
    done
fi

%postun
# On removal: the typing permission SuperV's "Enable" button added.
if [ "$1" -eq 0 ]; then
    rm -f /etc/udev/rules.d/70-superv-uinput.rules /etc/modules-load.d/superv-uinput.conf
fi

%files
%license LICENSE LICENSES/Unicode-3.0.txt
%doc README.md
%{_bindir}/superv
%{_prefix}/lib/superv/
%{_datadir}/applications/io.github.superv.SuperV.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg
%{_datadir}/icons/hicolor/64x64/apps/io.github.superv.SuperV.png
%{_datadir}/icons/hicolor/128x128/apps/io.github.superv.SuperV.png
%{_metainfodir}/io.github.superv.SuperV.metainfo.xml
%config(noreplace) %{_sysconfdir}/xdg/autostart/superv.desktop
%{_udevrulesdir}/70-superv-uinput.rules
%{_modulesloaddir}/superv-uinput.conf

%changelog
* Thu Nov 12 2026 SuperV <saikatnjm@users.noreply.github.com> - 2.1.0-1
- Plain-text paste, colour picker and copy text from the screen, each with an on/off switch

* Thu Nov 12 2026 SuperV <saikatnjm@users.noreply.github.com> - 2.0.0-1
- The emoji panel (Super+.), formerly SuperDot, is now part of SuperV

* Tue Nov 10 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.2.3-1
- New tagline: Clipboard for Linux; packaging for COPR, the AUR and a Launchpad PPA

* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.2.2-1
- App-store screenshots: 1280x720 landscape images with declared sizes

* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.2.1-1
- App-store listing: name, icon, screenshots, MIT licence, age rating, release notes

* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.2.0-1
- Packages for Fedora, openSUSE and Arch; restarts itself after updates; review fixes

* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.1.2-1
- Image copies, clearer settings, safer shortcuts
