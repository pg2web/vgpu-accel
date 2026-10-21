#!/bin/bash
# Fetch the parts of Microsoft's EWDK that vgpu.sys needs into ./ewdk (about 1 GB).
#
# The EWDK is only distributed as an 18.6 GB ISO (free download, not redistributable). It is
# downloaded into a cache directory on a disk with room for it, checked against a pinned SHA-256,
# and kept there so later runs (or a rebuilt Docker image) do not download it again.
#
#   ./fetch-ewdk.sh [cache-dir] [--delete-iso]
#     cache-dir     where the ISO goes (default: $EWDK_CACHE, else ~/.cache/vgpu-accel)
#     --delete-iso  remove the ISO once the subset is extracted
#
# Needs: curl (or aria2c, faster), 7z (p7zip), sha256sum, python3.
set -euo pipefail

ROOT=$(realpath "$(dirname "$0")")
DEST=${EWDK_DEST:-$ROOT/ewdk}

EWDK_URL=https://software-static.download.prss.microsoft.com/dbazure/888969d5-f34g-4e03-ac9d-1f9786c66749/EWDK_ge_release_svc_prod1_26100_250904-1728.iso
EWDK_SHA256=9f48251dd24ad31aac206d8256e95bda5f90a9783982c45a8aafeb9054562379
EWDK_BYTES=20002537472
VFS_URL=https://raw.githubusercontent.com/tmp64/clang-cl-linux/84e723d170e7611c55fff30c44d24c5a04bd4cb7/generate_vfs.py
VFS_SHA256=f42591e0a4b0fc801c89c3d6f1a33d4df7173b2bf6842a8970fd94afc8992f89
SUBSET=(
    "Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/include/*"
    "Program Files/Windows Kits/10/Include/10.0.26100.0/*"
    "Program Files/Windows Kits/10/Include/wdf/kmdf/1.27/*"
    "Program Files/Windows Kits/10/Lib/10.0.26100.0/km/x64/*"
    "Program Files/Windows Kits/10/Lib/wdf/kmdf/x64/1.27/*"
)

CACHE=""
DELETE_ISO=0
for a in "$@"; do
    case $a in
        --delete-iso) DELETE_ISO=1 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) CACHE=$a ;;
    esac
done
CACHE=${CACHE:-${EWDK_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/vgpu-accel}}
ISO=$CACHE/EWDK_26100.6584.iso

die() { echo "fetch-ewdk: $*" >&2; exit 1; }
free_bytes() { echo $(( $(df -Pk "$1" | awk 'NR == 2 { print $4 }') * 1024 )); }

SEVENZIP=$(command -v 7z || command -v 7za || true)
[ -n "$SEVENZIP" ] || die "7z not found (install p7zip / p7zip-full)"
command -v sha256sum >/dev/null || die "sha256sum not found"
command -v python3 >/dev/null || die "python3 not found"
command -v aria2c >/dev/null || command -v curl >/dev/null || die "neither aria2c nor curl found"

if [ -f "$DEST/.ewdk-sha256" ] && [ "$(cat "$DEST/.ewdk-sha256")" = "$EWDK_SHA256" ] && [ -f "$DEST/vfs-overlay.json" ]; then
    echo "EWDK subset already in $DEST"
    exit 0
fi

mkdir -p "$CACHE"
iso_ok=0
if [ -f "$ISO" ] && [ "$(stat -c %s "$ISO")" = "$EWDK_BYTES" ]; then
    echo "=== Checking cached ISO $ISO"
    if echo "$EWDK_SHA256  $ISO" | sha256sum --check --status -; then iso_ok=1; fi
fi

if [ $iso_ok -eq 0 ]; then
    have=$( [ -f "$ISO" ] && stat -c %s "$ISO" || echo 0 )
    need=$(( EWDK_BYTES - have + (1 << 30) ))
    if [ "$(free_bytes "$CACHE")" -lt "$need" ]; then
        echo "fetch-ewdk: not enough space in $CACHE for the $(( EWDK_BYTES >> 30 )) GB EWDK ISO." >&2
        echo "Free space on the mounted file systems:" >&2
        df -h -x tmpfs -x devtmpfs -x overlay 2>/dev/null | sed 's/^/  /' >&2
        die "pass a directory on a disk with at least $(( need >> 30 )) GB free: ./fetch-ewdk.sh /path/to/dir"
    fi
    echo "=== Downloading the EWDK ISO ($(( EWDK_BYTES >> 30 )) GB) to $CACHE (resumes if interrupted)"
    if command -v aria2c >/dev/null; then
        aria2c --continue=true --split=8 --max-connection-per-server=8 --min-split-size=10M \
               --file-allocation=none --summary-interval=30 --console-log-level=warn \
               --dir="$CACHE" --out="$(basename "$ISO")" "$EWDK_URL"
    else
        curl -fL -C - --retry 5 -o "$ISO" "$EWDK_URL"
    fi
    echo "=== Verifying SHA-256"
    echo "$EWDK_SHA256  $ISO" | sha256sum --check --status - ||
        die "checksum mismatch for $ISO: delete it and run again"
fi

parent=$(dirname "$DEST")
[ "$(free_bytes "$parent")" -gt $(( 2 << 30 )) ] || die "need 2 GB free in $parent for the extracted subset"
TMP="$DEST.tmp"
rm -rf "$TMP"
echo "=== Extracting the subset to $DEST"
"$SEVENZIP" x -y -r "$ISO" -o"$TMP" "${SUBSET[@]}" >/dev/null
[ -f "$TMP/Program Files/Windows Kits/10/Include/wdf/kmdf/1.27/wdf.h" ] || die "extraction incomplete"

# clang 14 rejects __ptr32 (POINTER_32) on a typedef'd pointer; spell out the struct pointer
WDM="$TMP/Program Files/Windows Kits/10/Include/10.0.26100.0/km/wdm.h"
sed -i 's/^\(\s*\)PEXTENDED_CREATE_DUAL_OPLOCK_KEYS POINTER_32 DualOplockKeys;/\1struct _EXTENDED_CREATE_DUAL_OPLOCK_KEYS* POINTER_32 DualOplockKeys;/' "$WDM"
grep -q 'struct _EXTENDED_CREATE_DUAL_OPLOCK_KEYS\* POINTER_32' "$WDM" || die "could not patch wdm.h"

# clang VFS overlay: maps the case-insensitive Windows header names for clang-cl
curl -fsSL "$VFS_URL" -o "$TMP/generate_vfs.py"
echo "$VFS_SHA256  $TMP/generate_vfs.py" | sha256sum --check --status - || die "generate_vfs.py checksum mismatch"
rm -rf "$DEST"
mv "$TMP" "$DEST"
python3 "$DEST/generate_vfs.py" \
    --msvc "$DEST/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC" \
    --sdk "$DEST/Program Files/Windows Kits/10" \
    --out "$DEST/vfs-overlay.json"
echo "$EWDK_SHA256" > "$DEST/.ewdk-sha256"

if [ $DELETE_ISO -eq 1 ]; then
    rm -f "$ISO"
    echo "Deleted $ISO"
else
    echo "The ISO stays in $CACHE for later runs (delete it, or pass --delete-iso, to free $(( EWDK_BYTES >> 30 )) GB)."
fi
echo "EWDK subset ready in $DEST ($(du -sh "$DEST" | cut -f1))"
