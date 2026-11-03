#!/usr/bin/env bash
# P5.5 Stage 1: build mlibc with the GCC cross-compiler (instead of clang),
# into a separate prefix so the working clang-built mlibc-root is untouched.
# binutils-cross must be on PATH so GCC's collect2 finds x86_64-unknown-instantos-ld.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$ROOT/build/binutils-cross/bin:$PATH"

# Meson cross files take absolute paths only: point the checked-in one's tool
# paths at this checkout (it was written for a repo at /home/sky/projects).
mkdir -p "$ROOT/build"
sed "s#/home/sky/projects/InstantOS#$ROOT#g" "$ROOT/tools/mlibc/instantos-x86_64-gcc.ini" \
  > "$ROOT/build/instantos-x86_64-gcc.ini"
export MLIBC_CROSS_FILE="$ROOT/build/instantos-x86_64-gcc.ini"
export MLIBC_BUILD_DIR="${MLIBC_BUILD_DIR:-$ROOT/build/mlibc-build-gcc}"
export MLIBC_INSTALL_DIR="${MLIBC_INSTALL_DIR:-$ROOT/build/mlibc-root-gcc}"
# GNU strip from binutils-cross (llvm-strip also works, but keep it all-GNU).
export LLVM_STRIP="${LLVM_STRIP:-$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-strip}"

exec bash "$ROOT/tools/build-mlibc.sh"
