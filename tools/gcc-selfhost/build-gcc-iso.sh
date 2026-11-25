#!/usr/bin/env bash
# Phase 7 P7.2: in-OS "gcc bootstraps gcc" oracle ISO. The P7.1 image with
# launcher-gcc.c and without a source tarball in the initrd: the gcc source is
# on the disk (tools/gcc-selfhost/make-gcc-disk.sh). Boot it with that disk and
# grep serial for SELFHOST_GCC_STAGE2_EQ_STAGE3.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
[ -f "$BUILD_DIR/find" ] || { echo "missing build/find (tools/build-gnu-tool.sh find): gcc's compare needs it" >&2; exit 2; }
LAUNCHER_SRC=launcher-gcc.c ISO_NAME=gcc-selfhost-gcc.iso SRC_TARBALL= \
OUT_DIR="${OUT_DIR:-$BUILD_DIR/gcc-selfhost-gcc}" \
  exec bash "$ROOT/tools/gcc-selfhost/build-binutils-iso.sh"
