#!/usr/bin/env bash
# P5.5 Stage 5: bashfork ISO on the full GCC world -- GCC substrate
# (ld-instantos/libinstant/managers), GCC bash, GCC tcc, GCC mlibc, and a
# GCC-compiled bashfork launcher. Kernel stays clang (SKIP_CMAKE=1).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="$ROOT/build/binutils-cross/bin:$PATH"
GCC="${GCC:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/bin/x86_64-unknown-instantos-gcc}"
GCC_INC="$("$GCC" -print-file-name=include)"

export SKIP_CMAKE="${SKIP_CMAKE:-1}"
export CC="$GCC"
export LD="$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-ld"
export CC_TARGET_FLAG=""
export LAUNCHER_EXTRA_CFLAGS="-isystem $GCC_INC"
export MLIBC_ROOT="$ROOT/build/mlibc-root-gcc"
export BASH_BIN="$ROOT/build/bash-gcc"
export TCC_BIN="$ROOT/build/tcc-gcc"
export SUBSTRATE_DIR="$ROOT/build/iUserApps-gcc/out"
export LIBINSTANT_SO="$ROOT/build/iUserApps-gcc/libinstant.so"
export OUT_DIR="${OUT_DIR:-$ROOT/build/bashfork-gcc}"

exec bash "$ROOT/tools/tcc-selfhost/build-bashfork-iso.sh"
