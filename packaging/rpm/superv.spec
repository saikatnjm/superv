# RPM package for Fedora and openSUSE.
# Build: rpmbuild -bb packaging/rpm/superv.spec --define "_sourcedir $PWD/dist" (expects dist/superv-VERSION.tar.gz)
# The GitHub release workflow does this automatically for every tagged version.
Name:           superv
Version:        1.2.0
Release:        1%{?dist}
Summary:        Win+V clipboard history for Linux
License:        MIT
URL:            https://github.com/saikatnjm/superv
Source0:        %{name}-%{version}.tar.gz

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

%description
SuperV brings the Windows clipboard history panel (Win+V) to Linux. Press
Super+V to see the last 25 things you copied, text and images, pick one to
paste it into the app you are using, pin favourites and clear the rest.
Works on GNOME, KDE Plasma, Cinnamon, MATE and Xfce, on Wayland and X11.
No network access, ever.

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
            python3 %{_prefix}/lib/superv/superv-setup --remove-hotkeys >/dev/null 2>&1 || :
        runuser -u "$user" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$bus" \
            gdbus call --session --dest io.github.superv.SuperV --object-path /io/github/superv/SuperV \
            --method org.gtk.Actions.Activate quit "[]" "{}" >/dev/null 2>&1 || :
    done
fi

%files
%license LICENSE
%doc README.md
%{_bindir}/superv
%{_prefix}/lib/superv/
%{_datadir}/applications/io.github.superv.SuperV.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.superv.SuperV.svg
%config(noreplace) %{_sysconfdir}/xdg/autostart/superv.desktop
%{_udevrulesdir}/70-superv-uinput.rules
%{_modulesloaddir}/superv-uinput.conf

%changelog
* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.2.0-1
- Packages for Fedora, openSUSE and Arch; restarts itself after updates; review fixes

* Mon Nov 09 2026 SuperV <saikatnjm@users.noreply.github.com> - 1.1.2-1
- Image copies, clearer settings, safer shortcuts
