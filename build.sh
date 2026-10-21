#!/bin/bash
# Cross-build vgpu.sys (KMDF 1.27, x64), the tests and 64/32-bit Mesa ICD packaging on Linux,
# sign them with the test certificate and package everything into target/.
set -euo pipefail

ROOT=$(realpath "$(dirname "$0")")
DRV="$ROOT/driver"
VIO="$ROOT/third_party/virtio-win"
OUT="$ROOT/target/build"
DIST="$ROOT/target/dist"
ZIP="$ROOT/target/vgpu_accel.zip"
SDK=/winsys/sdk/Include/10.0.26100.0
# EWDK subset: ./ewdk (made by fetch-ewdk.sh), else /opt/ewdk (the Docker image), or EWDK=...
if [ -z "${EWDK:-}" ]; then
    if [ -d "$ROOT/ewdk/Program Files" ]; then EWDK=$ROOT/ewdk; else EWDK=/opt/ewdk; fi
fi
[ -f "$EWDK/generate_vfs.py" ] || { echo "no EWDK at $EWDK: run ./fetch-ewdk.sh first" >&2; exit 1; }
EWDK=$(realpath "$EWDK")
KITS="$EWDK/Program Files/Windows Kits/10"
MSVC="$EWDK/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"

# Test-signing certificate: created on first use in certs/ (git-ignored) and reused, so every build
# is signed with the same key and VMs that trust it keep working. CERT_DIR=... uses another one.
CERT_DIR=${CERT_DIR:-$ROOT/certs}
if [ ! -f "$CERT_DIR/key.pem" ]; then
    echo "=== Creating test-signing certificate in $CERT_DIR"
    mkdir -p "$CERT_DIR"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 -subj "/CN=vgpu-accel test signing" \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,digitalSignature,keyCertSign" \
        -addext "extendedKeyUsage=codeSigning" \
        -keyout "$CERT_DIR/key.pem" -out "$CERT_DIR/cert.pem" 2>/dev/null
    chmod 600 "$CERT_DIR/key.pem"
fi
KEY=$CERT_DIR/key.pem
CERT=$CERT_DIR/cert.pem

rm -rf "$OUT" "$DIST"
mkdir -p "$OUT/inc" "$DIST"

# Windows filesystems are case-insensitive; give the headers the names the sources use.
WDFINC="$KITS/Include/wdf/kmdf/1.27"
for f in "$WDFINC"/*.h; do ln -sf "$f" "$OUT/inc/"; done
grep -ho '#include *[<"][A-Za-z0-9_]*\.h' "$WDFINC"/*.h | sed 's/.*[<"]//' | sort -u | while read -r n; do
    [ -e "$OUT/inc/$n" ] && continue
    m=$(ls "$WDFINC" | grep -ix "$n" || true)
    if [ -n "$m" ]; then ln -sf "$WDFINC/$m" "$OUT/inc/$n"; fi
done
ln -sf "$VIO/VirtIO.h" "$OUT/inc/virtio.h"
# clang VFS overlay with case-insensitive header names; it holds absolute paths, so it is made per build
python3 -I "$EWDK/generate_vfs.py" --msvc "$KITS/../../Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC" \
    --sdk "$KITS" --out "$OUT/vfs-overlay.json"

CFLAGS=(--target=x86_64-pc-windows-msvc -fms-compatibility -fms-extensions
    /c /O2 /GS- /Gy /Zl /W3 -Wno-everything
    -Xclang -ivfsoverlay -Xclang "$OUT/vfs-overlay.json"
    -I/winsys/msvc/14.44.35207/include -I"$SDK/ucrt"
    -I"$SDK/km/crt" -I"$SDK/km" -I"$SDK/shared" -I"$OUT/inc"
    -I"$DRV" -I"$VIO" -I"$VIO/WDF"
    -D_AMD64_ -DAMD64 -D_WIN64 -D_KERNEL_MODE -DNDEBUG
    -DNTDDI_VERSION=0x0A000008 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
    -DKMDF_VERSION_MAJOR=1 -DKMDF_VERSION_MINOR=27 -DPOOL_NX_OPTIN=1)

SOURCES=(
    "$DRV/device.c" "$DRV/queue.c" "$DRV/resource.c" "$DRV/ioctl.c" "$DRV/portio.c"
    "$VIO/VirtIOPCICommon.c" "$VIO/VirtIOPCILegacy.c" "$VIO/VirtIOPCIModern.c"
    "$VIO/VirtIORing.c" "$VIO/VirtIORing-Packed.c"
    "$VIO/WDF/VirtIOWdf.c" "$VIO/WDF/PCI.c" "$VIO/WDF/MemPortIO.c" "$VIO/WDF/Callbacks.c" "$VIO/WDF/Dma.c"
)

echo "=== Compiling vgpu.sys"
OBJS=()
for src in "${SOURCES[@]}"; do
    obj="$OUT/$(basename "${src%.*}").obj"
    clang-cl "${CFLAGS[@]}" "$src" /Fo"$obj"
    OBJS+=("$obj")
done

echo "=== Linking vgpu.sys"
lld-link /NOLOGO /OUT:"$OUT/vgpu.sys" /DRIVER /SUBSYSTEM:NATIVE,10.00 /OSVERSION:10.00 \
    /ENTRY:FxDriverEntry /NODEFAULTLIB /DYNAMICBASE /NXCOMPAT /OPT:REF /OPT:ICF \
    /TSAWARE:NO /MERGE:.retplne=.rdata /SECTION:.data,RW /MAP:"$OUT/vgpu.map" \
    /LIBPATH:"$KITS/Lib/10.0.26100.0/km/x64" /LIBPATH:"$KITS/Lib/wdf/kmdf/x64/1.27" \
    wdfdriverentry.lib wdfldr.lib ntoskrnl.lib hal.lib wdmguid.lib libcntpr.lib bufferoverflowfastfailk.lib \
    "${OBJS[@]}"

# Mesa: a sibling ../mesa checkout if present, else fetched into mesa/src (pg2web/mesa, pinned in mesa/prepare.sh)
if [ -z "${MESA_BUILD:-}" ]; then
    if [ -z "${MESA_SRC:-}" ]; then
        if [ -d "$ROOT/../mesa/.git" ]; then MESA_SRC=$(realpath "$ROOT/../mesa"); else MESA_SRC=$ROOT/mesa/src; fi
    fi
    "$ROOT/mesa/prepare.sh" "$MESA_SRC" >/dev/null
    echo "=== Building Mesa ($MESA_SRC)"
    ninja -C "$MESA_SRC/build-win64" >/dev/null
    ninja -C "$MESA_SRC/build-win32" >/dev/null
    MESA_BUILD=$MESA_SRC/build-win64
    MESA_BUILD32=$MESA_SRC/build-win32
fi
MESA_BUILD32=${MESA_BUILD32:-$(dirname "$MESA_BUILD")/build-win32}
sign() { osslsigncode sign -key "$KEY" -certs "$CERT" -h sha256 -in "$1" -out "$2" >/dev/null; }

# 64-bit and 32-bit (WOW64) user mode: tests plus the app-local Mesa opengl32.dll / vio_ogl_*.dll
mkdir -p "$DIST/system-icd/test"
for arch in x64 x86; do
    if [ $arch = x64 ]; then cc=x86_64-w64-mingw32; mesa=$MESA_BUILD; sfx=; else cc=i686-w64-mingw32; mesa=$MESA_BUILD32; sfx=32; fi
    echo "=== Building $arch tests"
    $cc-gcc -O2 -Wall -static -o "$OUT/vgputest$sfx.exe" "$ROOT/tests/vgputest.c" -I"$DRV" -lsetupapi
    $cc-gcc -O2 -Wall -static -o "$OUT/gltest$sfx.exe" "$ROOT/tests/gltest.c" -lopengl32 -lgdi32
    $cc-gcc -O2 -Wall -static -o "$OUT/glspin$sfx.exe" "$ROOT/tests/glspin.c" -lopengl32 -lgdi32
    mkdir -p "$DIST/gltest$sfx"
    for dll in "$mesa/src/gallium/targets/libgl-gdi/opengl32.dll" "$mesa/src/gallium/targets/wgl/vio_ogl_$arch.dll"; do
        $cc-strip --strip-debug -o "$DIST/gltest$sfx/$(basename "$dll")" "$dll"
    done
    sign "$OUT/vgputest$sfx.exe" "$DIST/vgputest$sfx.exe"
    sign "$OUT/gltest$sfx.exe" "$DIST/gltest$sfx/gltest$sfx.exe"
    sign "$OUT/glspin$sfx.exe" "$DIST/gltest$sfx/glspin$sfx.exe"
    # system-wide ICD: the same Mesa DLL registered as MSOGL; tests run against Windows' own opengl32.dll
    cp "$DIST/gltest$sfx/vio_ogl_$arch.dll" "$DIST/system-icd/"
    cp "$DIST/gltest$sfx/gltest$sfx.exe" "$DIST/gltest$sfx/glspin$sfx.exe" "$DIST/system-icd/test/"
done

echo "=== Signing and packaging"
sign "$OUT/vgpu.sys" "$DIST/vgpu.sys"
cp "$ROOT/installer/install_icd.bat" "$ROOT/installer/uninstall_icd.bat" "$DIST/system-icd/"
# every build gets a new, increasing version so Windows always replaces an installed build
BUILDVER="2.1.$(TZ=UTC date '+%y%j').$(TZ=UTC date '+%H%M')"
sed -e "s#%%BUILDDATE%%#$(TZ=UTC date '+%m/%d/%Y')#" -e "s#%%BUILDVERSION%%#$BUILDVER#" "$DRV/vgpu.inf" > "$DIST/vgpu.inf"
echo "Driver version $BUILDVER"
makecat --os-attr "2:10.0" --hwid "" --output "$OUT/vgpu.cat-unsigned" "$DIST/vgpu.inf" "$DIST/vgpu.sys" >/dev/null
sign "$OUT/vgpu.cat-unsigned" "$DIST/vgpu.cat"
openssl x509 -in "$CERT" -outform der -out "$DIST/vgpu_test_signing.cer"
cp "$ROOT/installer/install.bat" "$ROOT/installer/uninstall.bat" "$ROOT/installer/setup.bat" "$ROOT/installer/remove.bat" \
   "$ROOT/installer/remove_viogpu.bat" "$DIST/"
# licenses travel with the binaries: ours, the vendored virtio library and Mesa
mkdir -p "$DIST/licenses"
cp "$ROOT/LICENSE" "$DIST/licenses/LICENSE-vgpu-accel.txt"
cp "$VIO/LICENSE" "$DIST/licenses/LICENSE-virtio-win.txt"
if [ -n "${MESA_SRC:-}" ] && [ -f "$MESA_SRC/docs/license.rst" ]; then
    cp "$MESA_SRC/docs/license.rst" "$DIST/licenses/LICENSE-mesa.rst"
else
    echo "warning: Mesa license text not found (set MESA_SRC); the package lacks it" >&2
fi
cp "$ROOT/installer/NOTICE.txt" "$DIST/NOTICE.txt"
rm -f "$ZIP"
(cd "$DIST" && python3 -m zipfile -c "$ZIP" ./*)
echo "Package: $ZIP"
ls -l "$DIST"
