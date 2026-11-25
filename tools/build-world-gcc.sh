#!/usr/bin/env bash
# P5.5: build the entire InstantOS userland with the GCC cross-toolchain (the
# kernel stays clang -- it is an EFI/PE image GCC cannot target; that is Phase 7).
# Prerequisites already built on WSL: build/binutils-cross, the GCC cross at
# ${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install, and the clang cmake build (kernel + tcc-sysroot).
#
# Produces:
#   build/mlibc-root-gcc     GCC-built mlibc (libc.so, ld-instantos.so, ...)
#   build/iUserApps-gcc/     GCC-built libinstant substrate:
#                              libinstant.so, out/ld-instantos.so, out/*-manager
#   build/bash-gcc           GCC-built GNU bash
#   build/tcc-gcc            GCC-built TinyCC (+ GCC libtcc1 in the tcc-sysroot)
#
# Then the *-gcc ISO builders assemble the GCC world for the ladders:
#   tools/os-testsuite/build-testsuite-iso-gcc.sh   -> suite 314/314
#   tools/tcc-selfhost/build-bashfork-iso-gcc.sh    -> BASHFORK_RESULT_OK
#   tools/tcc-selfhost/build-selfhost-iso-gcc.sh    -> SELFHOST_FIXPOINT_OK
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/.."
ROOT="$(cd "$ROOT" && pwd)"

echo "== [1/4] mlibc (GCC) =="
bash "$ROOT/tools/build-mlibc-gcc.sh"
echo "== [2/4] libinstant userland substrate (GCC) =="
bash "$ROOT/tools/build-userland-gcc.sh"
echo "== [3/4] GNU bash (GCC) =="
bash "$ROOT/tools/build-bash-gcc.sh"
echo "== [4/4] TinyCC (GCC) =="
bash "$ROOT/tools/build-tcc-gcc.sh"

echo "GCC world built. ISO builders: tools/{os-testsuite,tcc-selfhost}/build-*-iso-gcc.sh"
