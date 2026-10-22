#!/bin/sh
# сборка пакета deb через dpkg deb напрямую для песочниц
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
proj=$(CDPATH= cd -- "$here/../.." && pwd)

SRC=${1:-$proj}
OUT=${2:-$proj/dist}

VERSION=${VERSION:-1.0.1}
REVISION=${REVISION:-1}
ARCH=${ARCH:-$(dpkg --print-architecture)}

command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb is required" >&2; exit 1; }
command -v cmake    >/dev/null 2>&1 || { echo "error: cmake is required" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/atlifm-deb.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# распаковка
case "$SRC" in
    *.tar.gz|*.tgz|*.tar.xz|*.tar.bz2|*.tar.zst)
        tarball=$SRC
        ;;
    *)
        tarball=$work/atlifm-$VERSION-fixed.tar.gz
        sh "$here/make-src-tarball.sh" "$SRC" "$work" "$VERSION" >/dev/null
        ;;
esac
mkdir -p "$work/src"
tar -xf "$tarball" -C "$work/src"
srcdir=$(find "$work/src" -mindepth 1 -maxdepth 1 -type d | head -n1)
[ -n "$srcdir" ] || srcdir=$work/src

# проверка исходников
if ! grep -q 'ENV{DESTDIR}' "$srcdir/CMakeLists.txt" 2>/dev/null; then
    echo "error: $SRC lacks the packaging fixes (ENV{DESTDIR})" >&2
    echo "       use the atlifm source tree (this repository)" >&2
    exit 1
fi

# сборка
if command -v ninja >/dev/null 2>&1; then
    cmake -S "$srcdir" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
else
    cmake -S "$srcdir" -B "$work/build" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$work/build" --parallel "$(nproc 2>/dev/null || echo 2)"

DESTDIR="$work/root" cmake --install "$work/build" --prefix /usr

# вырезаем отладочные символы
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$work/root/usr/bin/atlifm" || true
fi

# дерево пакета
pkg="$work/pkg"
mkdir -p "$pkg/DEBIAN"
cp -a "$work/root/." "$pkg/"

docdir="$pkg/usr/share/doc/atlifm"
mkdir -p "$docdir" "$pkg/usr/share/man/man1" "$pkg/usr/share/metainfo"
gzip -9nc "$proj/packaging/debian/changelog" > "$docdir/changelog.Debian.gz"
cp "$proj/packaging/debian/copyright" "$docdir/copyright"
cp "$proj/packaging/common/atlifm.1" "$pkg/usr/share/man/man1/atlifm.1"
cp "$proj/packaging/common/atlifm.metainfo.xml" "$pkg/usr/share/metainfo/atlifm.metainfo.xml"

# зависимости
DEPENDS="libc6 (>= 2.38), libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libqt6dbus6 | libqt6dbus6t64, libqt6svg6, libqt6svgwidgets6, libgcc-s1, libstdc++6"
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    mkdir -p "$work/shl/debian"
    cat > "$work/shl/debian/control" <<EOF
Source: atlifm
Section: utils
Priority: optional
Maintainer: 1nfys <lifawafla@gmail.com>

Package: atlifm
Architecture: any
Depends: \${shlibs:Depends}, \${misc:Depends}
Description: placeholder
EOF
    computed=$(cd "$work/shl" && dpkg-shlibdeps -O "$pkg/usr/bin/atlifm" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
    [ -n "$computed" ] && DEPENDS=$computed
fi

# метаданные и скрипты пакета
cat > "$pkg/DEBIAN/control" <<EOF
Package: atlifm
Version: $VERSION-$REVISION
Architecture: $ARCH
Maintainer: 1nfys <lifawafla@gmail.com>
Depends: $DEPENDS
Section: utils
Priority: optional
Homepage: https://github.com/1nfys/atlifm
Description: terminal-style file manager with a GUI mouse-first workflow
 atlifm is a mouse-first file manager that keeps a terminal aesthetic:
 a multi-pane (Miller columns) layout, monospace font and no animations,
 while adding drag and drop, tabs, previews and XDG desktop portal
 integration.
 .
 Features include interactive archive browsing (zip, tar, 7z, rar),
 image, SVG, code, directory-tree and hex previews, full undo and redo
 for file operations, bookmarks, tabs with independent navigation
 history, and a plugin system (configuration, shell script and native
 C++ plugins).
EOF

cat > "$pkg/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d /usr/share/icons/hicolor ]; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
fi
exit 0
EOF

cat > "$pkg/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d /usr/share/icons/hicolor ]; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF
chmod 755 "$pkg/DEBIAN/postinst" "$pkg/DEBIAN/postrm"

# контрольные суммы
( cd "$pkg" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
    | sort -z | xargs -0 md5sum > DEBIAN/md5sums )

# упаковка в deb
mkdir -p "$OUT"
deb="$OUT/atlifm_${VERSION}-${REVISION}_${ARCH}.deb"
rm -f "$deb"
dpkg-deb --build --root-owner-group "$pkg" "$deb"
echo "built: $deb"
dpkg-deb --info "$deb" | sed -n '1,12p'
sha256sum "$deb"
