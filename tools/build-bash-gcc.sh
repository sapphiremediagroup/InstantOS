#!/usr/bin/env bash
# P5.5 Stage 3: build GNU bash with the GCC cross-compiler against the
# GCC-built mlibc (build/mlibc-root-gcc), into build/bash-gcc. Mirrors
# build-bash.sh's freestanding/PIE/mlibc approach but with GCC flags:
#   - no --target (the instantos-gcc IS the target compiler)
#   - GNU ld from binutils-cross (must be on PATH) instead of -fuse-ld=lld
#   - -nostdinc + explicit mlibc and GCC-freestanding include dirs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$ROOT/build/binutils-cross/bin:$PATH"

GCC="${GCC:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/bin/x86_64-unknown-instantos-gcc}"
MLIBC_ROOT="${MLIBC_ROOT:-$ROOT/build/mlibc-root-gcc}"
GCC_INC="$("$GCC" -print-file-name=include)"

export CC="$GCC"
export MLIBC_ROOT
export OUTPUT="${OUTPUT:-$ROOT/build/bash-gcc}"
export LLVM_STRIP="${LLVM_STRIP:-$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-strip}"
export LLVM_NM="${LLVM_NM:-$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-nm}"
export WORK_DIR="${WORK_DIR:-$ROOT/build/bash-port-gcc}"

# mlibc headers first (win over anything), then GCC's freestanding headers
# (stdarg.h/stddef.h intrinsics). -nostdinc keeps the baked instantos sysroot
# (clang-built mlibc) out of the picture.
export TARGET_CFLAGS="-ffreestanding -fPIE -fno-stack-protector -nostdinc -isystem $MLIBC_ROOT/include -isystem $GCC_INC -D_GNU_SOURCE -Wno-implicit-function-declaration -fcommon"
export TARGET_LDFLAGS="-pie -nostdlib -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--allow-multiple-definition $MLIBC_ROOT/lib/crt1.o"

exec bash "$ROOT/tools/build-bash.sh"
