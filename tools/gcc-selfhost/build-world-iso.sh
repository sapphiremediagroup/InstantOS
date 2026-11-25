#!/usr/bin/env bash
# Phase 7 P7.3: in-OS "the stage3 gcc rebuilds the userland" image. The P7.2
# image with launcher-world.c; boot it with the P7.2 disk (finished bootstrap
# tree) plus /src/world-src.tar.gz (make-world-src.sh) and grep serial for
# GCCSELF_WORLD_OK.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
LAUNCHER_SRC=launcher-world.c ISO_NAME=gcc-selfhost-world.iso SRC_TARBALL= \
OUT_DIR="${OUT_DIR:-$BUILD_DIR/gcc-selfhost-world}" \
  exec bash "$ROOT/tools/gcc-selfhost/build-binutils-iso.sh"
