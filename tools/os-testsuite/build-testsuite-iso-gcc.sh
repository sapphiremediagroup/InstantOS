#!/usr/bin/env bash
# P5.5 Stage 4a: build the os-testsuite ISO with the runner compiled by the GCC
# cross-compiler (instead of clang) into build/os-testsuite-gcc. The runner is a
# self-contained freestanding libinstant program (own _start, raw syscalls), so
# GCC just needs -ffreestanding -nostdinc + its own freestanding headers, and
# GNU ld from binutils-cross. Kernel/managers/loader stay clang (SKIP_CMAKE=1).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="$ROOT/build/binutils-cross/bin:$PATH"

GCC="${GCC:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/bin/x86_64-unknown-instantos-gcc}"
GCC_INC="$("$GCC" -print-file-name=include)"

export SKIP_CMAKE="${SKIP_CMAKE:-1}"
export CC="$GCC"
export LD="$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-ld"
export CC_TARGET_FLAG=""                       # GCC is already the target compiler
export RUNNER_EXTRA_CFLAGS="-isystem $GCC_INC" # freestanding stdint.h etc.
export OUT_DIR="${OUT_DIR:-$ROOT/build/os-testsuite-gcc}"

# GCC-built runtime substrate (from tools/build-userland-gcc.sh). Managers +
# ld-instantos.so live in out/; libinstant.so is one level up.
export SUBSTRATE_DIR="${SUBSTRATE_DIR:-$ROOT/build/iUserApps-gcc/out}"
export LIBINSTANT_SO="${LIBINSTANT_SO:-$ROOT/build/iUserApps-gcc/libinstant.so}"

exec bash "$ROOT/tools/os-testsuite/build-testsuite-iso.sh"
