#!/usr/bin/env bash
# The INITRD for a real PC: the system's (build/initrd-entries.txt) plus the
# C/C++ toolchain tarball (tools/make-dev-tarball.sh) as
# /lib/install/instantos-dev.tar.gz, which instant-install unpacks onto the
# partition it installs. The ISO's INITRD stays without it: its EFI image has
# 128 MiB. install-limine-windows.ps1 copies this one to the ESP.
#
#   tools/make-pc-initrd.sh [OUT]   (default build/pc/INITRD; run in WSL, after
#                                    cmake --build build --target iso)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT="${1:-$BUILD_DIR/pc/INITRD}"
DEV_TAR="${DEV_TAR:-$BUILD_DIR/instantos-dev.tar.gz}"
[ -f "$DEV_TAR" ] || { echo "missing $DEV_TAR (tools/make-dev-tarball.sh)" >&2; exit 2; }
ENTRIES=()
while IFS= read -r entry; do
  [ -n "$entry" ] && ENTRIES+=("$entry")
done < "$BUILD_DIR/initrd-entries.txt"
ENTRIES+=("lib/install/instantos-dev.tar.gz:$DEV_TAR")
mkdir -p "$(dirname "$OUT")"
"$BUILD_DIR/mkInitrd_build/mkInitrd" "$OUT" "${ENTRIES[@]}" >/dev/null
echo "$OUT: $(du -h "$OUT" | cut -f1)"
