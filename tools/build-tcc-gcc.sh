#!/usr/bin/env bash
# P5.5 Stage 2: build TinyCC with the GCC cross-compiler (instead of clang),
# still against the libinstant tcc-sysroot, into build/tcc-gcc. binutils-cross
# must be on PATH so GCC's collect2 finds x86_64-unknown-instantos-ld.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$ROOT/build/binutils-cross/bin:$PATH"

GCC="${GCC:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/bin/x86_64-unknown-instantos-gcc}"
GCC_INC="$("$GCC" -print-file-name=include)"
TCC_SYSROOT="${TCC_SYSROOT:-$ROOT/build/tcc-sysroot}"

export TCC_NATIVE_BUILD=1
export TCC_SKIP_SYSROOT=1   # reuse the existing clang-built libinstant sysroot
export TCC_SYSROOT
export CC="$GCC"
export AR="$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-ar"
export STRIP="$ROOT/build/binutils-cross/bin/x86_64-unknown-instantos-strip"
export TCC_BUILD_DIR="${TCC_BUILD_DIR:-$ROOT/build/tcc-build-gcc}"
export TCC_OUTPUT="${TCC_OUTPUT:-$ROOT/build/tcc-gcc}"

# GCC flavor of the cc-wrapper flags: no --target (instantos-gcc IS the target),
# GNU ld (not lld), plus GCC's own freestanding headers for stdarg.h/stddef.h.
export TCC_WRAP_CFLAGS="-fPIC -ffreestanding -fno-stack-protector -nostdinc -isystem $TCC_SYSROOT/include -isystem $GCC_INC"
export TCC_WRAP_LDFLAGS="-nostdlib -Wl,--gc-sections -Wl,--build-id=none -Wl,--hash-style=sysv -Wl,-z,max-page-size=0x1000 -pie -Wl,-e,_start -Wl,--dynamic-linker,/lib/ld-instantos.so"
# libinstant provides ldexp etc. and tcc uses no dl symbols; drop these so the
# GCC-built tcc stays a pure libinstant binary (NEEDED: libinstant.so only).
export TCC_WRAP_DROP_LIBS="-lm -ldl"

exec bash "$ROOT/tools/build-tcc.sh"
