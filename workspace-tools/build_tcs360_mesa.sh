#!/usr/bin/env bash
# Reproduce the pinned port's Mesa build without removing any source/build tree.
# Usage from devkitPro MSYS: bash build_tcs360_mesa.sh /c/path/to/lswtcs-360 [jobs]
set -euo pipefail
ROOT=$(cd "${1:?port checkout required}" && pwd)
B="$ROOT/switch/mesa/build"
JOBS=${2:-2}
MESA_SHA=c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d
PKG_COMMIT=f103fe88e37180ecd0b7a9173b52b7580a54f71a
mkdir -p "$B/dl"
cd "$B/dl"
if [ ! -f mesa-20.1.0-rc3.tar.xz ]; then
    curl -sSLf -o mesa-20.1.0-rc3.tar.xz https://archive.mesa3d.org/older-versions/20.x/mesa-20.1.0-rc3.tar.xz
fi
echo "$MESA_SHA *mesa-20.1.0-rc3.tar.xz" | sha256sum -c -
if [ ! -d pkgs ]; then
    git clone -q --filter=blob:none --sparse https://github.com/devkitPro/pacman-packages.git pkgs
    git -C pkgs sparse-checkout set switch/mesa
fi
git -C pkgs checkout -q "$PKG_COMMIT"
P="$B/dl/pkgs/switch/mesa"
S="$B/src/mesa-20.1.0-rc3"
if [ ! -f "$B/lswtcs-edits-complete" ]; then
    if [ -e "$B/src" ]; then
        echo 'Source tree exists without completion marker; inspect it before resuming.' >&2
        exit 1
    fi
    mkdir "$B/src"
    tar -xf "$B/dl/mesa-20.1.0-rc3.tar.xz" -C "$B/src"
    cd "$S"
    patch -p1 -s -i "$P/switch-mesa-20.1.0-5.patch"
    patch -p1 -s -i "$P/gl_XML.py.patch"
    patch -p1 -s -i "$P/glX_XML.py.patch"
    for edit in cache nir fmz egl_share glthread cache_race pushbuf cbflush; do
        python "$ROOT/switch/mesa/mesa_${edit}_edits.py" "$S"
    done
    touch "$B/lswtcs-edits-complete"
fi
cd "$S"
if [ ! -f build/build.ninja ]; then
    MSYSTEM=MSYS /opt/devkitpro/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true -Dshader-cache=true
fi
ninja -C build -j"$JOBS"
DESTDIR="$B/install" ninja -C build install
