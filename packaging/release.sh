#!/usr/bin/env bash
# Cuts the x86_64 linux release tarball geas-lang-bin installs. It builds the
# compiler and the runtime, runs the quick gates over them, lays the toolchain
# out with the same make install the source packages run, and packs that
# tree beside its sha256.
#
# Usage: packaging/release.sh [VERSION] [OUTDIR]
#
# VERSION defaults to the one compiler/src/main.cpp carries, and a VERSION
# given here must match it, so a tarball never ships under a name its own
# binary disagrees with. OUTDIR defaults to target/release.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

# The version the compiler source declares
src_ver=$(grep -oE 'versionString = "geas [0-9]+\.[0-9]+\.[0-9]+"' compiler/src/main.cpp \
    | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
if [ -z "$src_ver" ]; then
    echo "release: cannot read the version from compiler/src/main.cpp" >&2
    exit 1
fi

version=${1:-$src_ver}
version=${version#v}
if [ "$version" != "$src_ver" ]; then
    echo "release: asked for $version but compiler/src/main.cpp says $src_ver" >&2
    exit 1
fi

outdir=${2:-target/release}
mkdir -p "$outdir"
outdir=$(cd "$outdir" && pwd)
jobs=$(nproc 2>/dev/null || echo 2)

name="geas-lang-$version-x86_64-linux"
tarball="$name.tar.gz"

# Build the toolchain and hold it to the quick tier before anything is packed
make -j"$jobs" compiler runtime
built=$(target/bin/geas version)
if [ "$built" != "geas $version" ]; then
    echo "release: the built compiler says '$built', expected 'geas $version'" >&2
    exit 1
fi
make -j"$jobs" quick

# Lay the toolchain out under a staging tree the way a system install does
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
make DESTDIR="$stage/$name" PREFIX=/usr install
strip --strip-unneeded "$stage/$name/usr/bin/geas" "$stage/$name/usr/lib/libgeasrt.so.0"
install -Dm644 LICENSE "$stage/$name/LICENSE"
install -Dm644 README.md "$stage/$name/README.md"
install -Dm644 spec.md "$stage/$name/spec.md"

# The runtime must answer to the SONAME every module names, through its symlink
test "$(readlink "$stage/$name/usr/lib/libgeasrt.so")" = "libgeasrt.so.0"
readelf -d "$stage/$name/usr/lib/libgeasrt.so.0" | grep -qE '\(SONAME\).*\[libgeasrt\.so\.0\]'

# Pack with fixed owners, order, and times, so the same tree packs the same bytes
mtime=$(git log -1 --format=%ct 2>/dev/null || date +%s)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$mtime" \
    -C "$stage" -cf - "$name" | gzip -n > "$outdir/$tarball"
(cd "$outdir" && sha256sum "$tarball" > "$tarball.sha256")

echo "release: $outdir/$tarball"
cat "$outdir/$tarball.sha256"
