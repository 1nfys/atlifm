#!/bin/sh
# создаёт atlifm-1.0.0-fixed.tar.gz из дерева исходников.
#
# использование: make-src-tarball.sh [SOURCE-ROOT] [OUTPUT-DIR] [VERSION]
#   SOURCE-ROOT  дерево исходников (по умолчанию корень репозитория)
#   OUTPUT-DIR   куда положить архив (по умолчанию <repo>/dist)
#   VERSION      версия пакета (по умолчанию 1.0.0)
#
# внутри архива верхний каталог atlifm-<version>/, и в него попадают
# только исходники проекта (packaging, .github и результаты сборки исключены).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
proj=$(CDPATH= cd -- "$here/../.." && pwd)

root=${1:-$proj}
out=${2:-$proj/dist}
version=${3:-${VERSION:-1.0.0}}

name=atlifm-$version-fixed.tar.gz

[ -f "$root/CMakeLists.txt" ] || {
    echo "error: $root does not look like the atlifm source tree" >&2
    exit 1
}
if ! grep -q 'ENV{DESTDIR}' "$root/CMakeLists.txt"; then
    echo "error: $root lacks the packaging fixes (ENV{DESTDIR})" >&2
    exit 1
fi

mkdir -p "$out"
tarball=$out/$name

stage=$(mktemp -d "${TMPDIR:-/tmp}/atlifm-src.XXXXXX")
trap 'rm -rf "$stage"' EXIT INT TERM

mkdir -p "$stage/atlifm-$version"
tar -cf - -C "$root" \
    --exclude=./.git \
    --exclude=./.github \
    --exclude=./packaging \
    --exclude=./build \
    --exclude=./dist \
    --exclude=./out \
    --exclude='./atlifm-*.tar.gz' \
    . | tar -xf - -C "$stage/atlifm-$version"

rm -f "$tarball"
tar -czf "$tarball" -C "$stage" "atlifm-$version"
echo "$tarball"
