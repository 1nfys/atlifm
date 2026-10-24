#!/bin/sh
# сборка пакетов rpm через rpmbuild в отдельном каталоге
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
proj=$(CDPATH= cd -- "$here/../.." && pwd)

SRC=${1:-$proj}
OUT=${2:-$proj/dist}
VERSION=${VERSION:-1.0.2}
RPMBUILD=${RPMBUILD:-rpmbuild}

command -v "$RPMBUILD" >/dev/null 2>&1 || { echo "error: rpmbuild is required" >&2; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/atlifm-rpm.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

rpmroot=$work/rpmbuild
mkdir -p "$rpmroot/BUILD" "$rpmroot/BUILDROOT" "$rpmroot/RPMS" \
         "$rpmroot/SOURCES" "$rpmroot/SPECS" "$rpmroot/SRPMS"

# исходники
check=$work/check
mkdir -p "$check"
if [ -f "$SRC" ]; then
    tar -xzf "$SRC" -C "$check" --wildcards '*/CMakeLists.txt' 2>/dev/null || true
else
    cp "$SRC/CMakeLists.txt" "$check/" 2>/dev/null || true
fi
if ! grep -rqs 'ENV{DESTDIR}' "$check"; then
    echo "error: $SRC lacks the packaging fixes (ENV{DESTDIR})" >&2
    echo "       use the atlifm source tree (this repository)" >&2
    exit 1
fi

case "$SRC" in
    *.tar.gz|*.tgz)
        cp "$SRC" "$rpmroot/SOURCES/atlifm-$VERSION-fixed.tar.gz"
        ;;
    *)
        sh "$here/make-src-tarball.sh" "$SRC" "$rpmroot/SOURCES" "$VERSION" >/dev/null
        ;;
esac

cp "$proj/packaging/common/atlifm.metainfo.xml" "$rpmroot/SOURCES/"
cp "$proj/packaging/common/atlifm.1" "$rpmroot/SOURCES/"
cp "$proj/packaging/rpm/atlifm.spec" "$rpmroot/SPECS/"

# параметры каталога сборки
set -- --define "_topdir $rpmroot"

rpm_cmd=$(dirname -- "$(command -v "$RPMBUILD")")/rpm
if [ -x "$rpm_cmd" ]; then
    # если утилиты отладочных символов нет то собираем пакет без них
    dbg=$("$rpm_cmd" --eval '%{__debug_install_post}' 2>/dev/null \
        | grep -oE '/[^ ]*find-debuginfo[^ "]*' | head -n1 || true)
    if [ -n "${dbg:-}" ] && [ ! -e "$dbg" ]; then
        set -- "$@" --define 'debug_package %{nil}'
        echo "note: find-debuginfo not present; building without debuginfo"
    fi
fi

# сборка
log=$work/rpmbuild.log
if ! "$RPMBUILD" "$@" -ba "$rpmroot/SPECS/atlifm.spec" >"$log" 2>&1; then
    if grep -q "Failed build dependencies" "$log"; then
        echo "note: rpm has no knowledge of the build dependencies installed"
        echo "      on this system; retrying with --nodeps" >&2
        "$RPMBUILD" "$@" --nodeps -ba "$rpmroot/SPECS/atlifm.spec"
    else
        echo "error: rpmbuild failed" >&2
        tail -n 40 "$log" >&2
        exit 1
    fi
else
    grep -E '^(Wrote|Provides|Requires):' "$log" || true
fi

# копирование результатов
mkdir -p "$OUT"
find "$rpmroot/RPMS" "$rpmroot/SRPMS" -name '*.rpm' -exec cp -v {} "$OUT/" \;
sha256sum "$OUT"/atlifm-*.rpm
