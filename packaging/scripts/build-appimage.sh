#!/bin/sh
# сборка пакета appimage для текущей архитектуры
# инструменты скачиваются в папку кэша и запускаются без fuse
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
proj=$(CDPATH= cd -- "$here/../.." && pwd)

SRC=${1:-$proj}
OUT=${2:-$proj/dist}
VERSION=${VERSION:-1.0.2}
ARCH=$(uname -m)
TOOLS=${APPIMAGE_TOOLS:-${XDG_CACHE_HOME:-$HOME/.cache}/atlifm-appimage}

command -v cmake >/dev/null 2>&1 || { echo "error: cmake is required" >&2; exit 1; }
command -v curl  >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 1; }

export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE=$(command -v qmake6 || command -v qmake)
if [ -z "$QMAKE" ]; then
    echo "error: qmake (Qt 6 dev tools) is required to bundle Qt" >&2
    exit 1
fi

# бандл для вейланда и иксов и запрашиваем только установленные плагины
qt_plugins_dir=$("$QMAKE" -query QT_INSTALL_PLUGINS 2>/dev/null)/platforms
extra_platforms=""
for _p in libqwayland-egl.so libqwayland-generic.so libqoffscreen.so libqminimal.so; do
    if [ -e "$qt_plugins_dir/$_p" ]; then
        extra_platforms="${extra_platforms:+$extra_platforms;}$_p"
    fi
done
if [ -n "$extra_platforms" ]; then
    export EXTRA_PLATFORM_PLUGINS="$extra_platforms"
fi

mkdir -p "$TOOLS"
fetch_tool() {
    name=$1 url=$2
    [ -x "$TOOLS/$name" ] && return 0
    echo "downloading $name ..."
    curl -fL --retry 3 -o "$TOOLS/$name" "$url"
    chmod +x "$TOOLS/$name"
}

fetch_tool linuxdeploy \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
fetch_tool linuxdeploy-plugin-qt \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage"
fetch_tool appimagetool \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"

work=$(mktemp -d "${TMPDIR:-/tmp}/atlifm-appimage.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# распаковка
case "$SRC" in
    *.tar.gz|*.tgz|*.tar.xz)
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

# подготовка каталога appdir
appdir=$work/AppDir
mkdir -p "$appdir"
cp -a "$work/root/." "$appdir/"

install -Dm644 "$proj/packaging/common/atlifm.desktop" "$appdir/atlifm.desktop"
install -Dm644 "$proj/packaging/common/atlifm.metainfo.xml" \
    "$appdir/usr/share/metainfo/atlifm.metainfo.xml"
install -Dm644 "$proj/packaging/common/atlifm.1" \
    "$appdir/usr/share/man/man1/atlifm.1"

# значок по спецификации appimage
icon=$srcdir/atlifm-logo/icons/atlifm-512.png
[ -f "$icon" ] || icon=$srcdir/atlifm-logo/icons/atlifm-256.png
cp "$icon" "$appdir/atlifm.png"

# сборка плагинов qt и упаковка пакета
"$TOOLS/linuxdeploy" --appdir "$appdir" --plugin qt \
    --desktop-file "$appdir/atlifm.desktop" \
    --icon-file "$appdir/atlifm.png"

mkdir -p "$OUT"
appimage="$OUT/atlifm-$VERSION-$ARCH.AppImage"
rm -f "$appimage"
"$TOOLS/appimagetool" "$appdir" "$appimage"

echo "built: $appimage"
sha256sum "$appimage"
