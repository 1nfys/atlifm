Name:           atlifm
Version:        1.0.1
Release:        1%{?dist}
Summary:        Terminal-style file manager with a GUI mouse-first workflow
License:        MIT
URL:            https://github.com/1nfys/atlifm
Source0:        atlifm-%{version}-fixed.tar.gz
Source1:        atlifm.metainfo.xml
Source2:        atlifm.1

%if 0%{?suse_version}
BuildRequires:  qt6-base-devel >= 6.5
BuildRequires:  qt6-svg-devel
%else
BuildRequires:  qt6-qtbase-devel >= 6.5
BuildRequires:  qt6-qtsvg-devel
%endif
BuildRequires:  cmake >= 3.28
%if 0%{?suse_version}
BuildRequires:  ninja
%else
BuildRequires:  ninja-build
%endif
BuildRequires:  gcc-c++
BuildRequires:  desktop-file-utils
BuildRequires:  hicolor-icon-theme
Requires:       hicolor-icon-theme

%description
atlifm is a mouse-first file manager that keeps a terminal aesthetic:
a multi-pane (Miller columns) layout, monospace font and no animations,
while adding drag and drop, tabs, previews and XDG desktop portal
integration.

Features include interactive archive browsing (zip, tar, 7z, rar),
image, SVG, code, directory-tree and hex previews, full undo and redo
for file operations, bookmarks, tabs with independent navigation
history, and a plugin system (configuration, shell script and native
C++ plugins).

%prep
%setup -q

%build
%if 0%{?cmake:1}
%if 0%{?suse_version}
%cmake -DCMAKE_BUILD_TYPE=Release
%else
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
%endif
%cmake_build
%else
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=%{_libdir} \
    -DCMAKE_INSTALL_INCLUDEDIR=%{_includedir} \
    -DCMAKE_INSTALL_DATADIR=%{_datadir} \
    -DCMAKE_INSTALL_MANDIR=%{_mandir}
cmake --build build --parallel
%endif

%install
%if 0%{?cmake:1}
%cmake_install
%else
DESTDIR=%{buildroot} cmake --install build
%endif
install -Dpm 0644 %{SOURCE1} %{buildroot}%{_datadir}/metainfo/atlifm.metainfo.xml
install -Dpm 0644 %{SOURCE2} %{buildroot}%{_mandir}/man1/atlifm.1

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/atlifm.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/atlifm-portal.desktop

%files
%{_bindir}/atlifm
%dir %{_includedir}/atlifm
%{_includedir}/atlifm/atlifm-plugin-api.h
%{_datadir}/applications/atlifm.desktop
%{_datadir}/applications/atlifm-portal.desktop
%{_datadir}/dbus-1/services/org.freedesktop.impl.portal.desktop.atlifm.service
%{_datadir}/icons/hicolor/*/apps/atlifm.png
%{_datadir}/metainfo/atlifm.metainfo.xml
%dir %{_datadir}/xdg-desktop-portal
%dir %{_datadir}/xdg-desktop-portal/portals
%{_datadir}/xdg-desktop-portal/portals/atlifm.portal
%{_datadir}/xdg-desktop-portal/hyprland-portals.conf
%{_mandir}/man1/atlifm.1*
%license LICENSE
%doc README.md

%changelog
* Wed Oct 21 2026 1nfys <lifawafla@gmail.com> - 1.0.1-1
- New upstream release.

* Mon Oct 19 2026 1nfys <lifawafla@gmail.com> - 1.0.0-1
- Initial package.
