#!/bin/bash
# Fetch Mesa with the vgpu-accel changes (pg2web/mesa: official Mesa at the base tag plus our commits)
# and configure the 64-bit and 32-bit Windows cross builds. Safe to re-run: existing steps are skipped.
#   mesa/prepare.sh [mesa-source-dir]     (default: $MESA_SRC, else mesa/src in this repo)
# MESA_URL, MESA_TAG and MESA_COMMIT override the pinned source (for example to try your own fork).
set -euo pipefail

HERE=$(realpath "$(dirname "$0")")
ROOT=$(dirname "$HERE")
SRC=${1:-${MESA_SRC:-$HERE/src}}
URL=${MESA_URL:-https://github.com/pg2web/mesa.git}
TAG=${MESA_TAG:-vgpu-accel-26.2.5-1}
COMMIT=${MESA_COMMIT:-3e0154f1e6f86cadafe1e53ec6fcc13e49e2e0be}

if [ ! -d "$SRC/.git" ]; then
    echo "=== Fetching Mesa $TAG from $URL"
    git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 "$URL" tag "$TAG"
    git -C "$SRC" -c advice.detachedHead=false checkout -q "$TAG"
    [ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo "unexpected commit for $TAG" >&2; exit 1; }
fi

OPTS=(-Dplatforms=windows -Dgallium-drivers=virgl,softpipe -Dvulkan-drivers= -Dopengl=true
      -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dllvm=disabled -Dxmlconfig=disabled
      -Dbuild-tests=false -Dprefix=/ -Dbuildtype=release -Dzlib:default_library=static)
for arch in x64 x86; do
    if [ $arch = x64 ]; then dir=build-win64; cross=mingw-x86_64.ini; else dir=build-win32; cross=mingw-i686.ini; fi
    # meson stores absolute paths: a build dir configured elsewhere (host vs container) is recreated
    where="$(realpath "$SRC") $ROOT"
    if [ -d "$SRC/$dir" ]; then
        [ "$(cat "$SRC/$dir/.vgpu-accel-paths" 2>/dev/null)" = "$where" ] && continue
        rm -rf "${SRC:?}/$dir"
    fi
    echo "=== Configuring Mesa $dir"
    (cd "$SRC" && meson setup "$dir" --cross-file="$ROOT/cross/$cross" "${OPTS[@]}" -Dgallium-wgl-dll-name=vio_ogl_$arch >/dev/null)
    echo "$where" > "$SRC/$dir/.vgpu-accel-paths"
done
echo "$SRC"
