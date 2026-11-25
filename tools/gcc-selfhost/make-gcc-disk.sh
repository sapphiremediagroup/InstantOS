#!/usr/bin/env bash
# Phase 7 P7.2: the disk the in-OS gcc bootstrap runs on. The gcchost world
# (gcc, binutils, mlibc and headers under /usr; tools/gcc-port/
# assemble-gcchost-root.sh) plus the slim gcc source tarball at /src
# (make-gcc-insrc.sh), on an ext4 volume large enough for three stages of
# build trees (DISK_SIZE, default 8G). Same partition layout and ext4 features
# as the gcchost disk (tools/gcc-port/build-gcchost-iso.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
GCCHOST_ROOT="${GCCHOST_ROOT:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcchost-root}"
OUT_DIR="$BUILD_DIR/gcc-selfhost-gcc"
TARBALL="$OUT_DIR/gcc-insrc.tar.gz"
DISK="${DISK:-$OUT_DIR/ext4.img}"
SIZE="${DISK_SIZE:-8G}"

[ -d "$GCCHOST_ROOT/usr/bin" ] || { echo "gcchost root missing; run tools/gcc-port/assemble-gcchost-root.sh" >&2; exit 2; }
[ -f "$TARBALL" ] || { echo "missing $TARBALL (run tools/gcc-selfhost/make-gcc-insrc.sh)" >&2; exit 2; }

STAGE="$OUT_DIR/disk-root"
rm -rf "$STAGE"
cp -a "$GCCHOST_ROOT" "$STAGE"
mkdir -p "$STAGE/src"
cp "$TARBALL" "$STAGE/src/gcc-insrc.tar.gz"

rm -f "$DISK"
truncate -s "$SIZE" "$DISK"
printf 'label: dos\nstart=2048, type=83\n' | sfdisk "$DISK" >/dev/null 2>&1
BLOCKS=$(( ($(stat -c %s "$DISK") - 1048576) / 4096 ))
mke2fs -q -F -b 4096 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index" \
  -E offset=1048576,lazy_itable_init=0 -d "$STAGE" "$DISK" "$BLOCKS"
rm -rf "$STAGE"
ls -la "$DISK"
echo "GCCSELF_GCC_DISK=$DISK"
