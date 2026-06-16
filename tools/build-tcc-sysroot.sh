#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
TCC_SYSROOT="${TCC_SYSROOT:-$BUILD_DIR/tcc-sysroot}"
ILIBCXX_DIR="${ILIBCXX_DIR:-$ROOT/outside/iUserApps/outside/ilibcxx}"

CRT0_OBJECT="${CRT0_OBJECT:-$BUILD_DIR/outside/iUserApps/outside/ilibcxx/CMakeFiles/instant_c_crt0.dir/src/c_runtime.cpp.o}"
LIBINSTANT="${LIBINSTANT:-$BUILD_DIR/libinstant.so}"
LOADER="${LOADER:-$BUILD_DIR/ld-instantos.so}"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"

if [ ! -d "$ILIBCXX_DIR/include" ]; then
  printf 'ilibcxx headers not found: %s\n' "$ILIBCXX_DIR/include" >&2
  exit 2
fi

if [ ! -f "$CRT0_OBJECT" ] || [ ! -f "$LIBINSTANT" ] || [ ! -f "$LOADER" ]; then
  printf 'missing build artifacts for TCC sysroot\n' >&2
  printf 'expected: %s\n' "$CRT0_OBJECT" >&2
  printf 'expected: %s\n' "$LIBINSTANT" >&2
  printf 'expected: %s\n' "$LOADER" >&2
  printf 'run: cmake --build %s --target instant_c_crt0 ilibcxx ld-instantos\n' "$BUILD_DIR" >&2
  exit 2
fi

rm -rf "$TCC_SYSROOT/include" "$TCC_SYSROOT/lib" "$TCC_SYSROOT/usr"
mkdir -p "$TCC_SYSROOT/include" "$TCC_SYSROOT/lib" "$TCC_SYSROOT/usr"

cp -R "$ILIBCXX_DIR/include/." "$TCC_SYSROOT/include/"

cp "$CRT0_OBJECT" "$TCC_SYSROOT/lib/crt0.o"
ln -sf crt0.o "$TCC_SYSROOT/lib/crt1.o"
ln -sf crt0.o "$TCC_SYSROOT/lib/Scrt1.o"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIC -c "$ROOT/tools/tcc/empty-crt.c" -o "$TCC_SYSROOT/lib/crti.o"
cp "$TCC_SYSROOT/lib/crti.o" "$TCC_SYSROOT/lib/crtn.o"

cp "$LIBINSTANT" "$TCC_SYSROOT/lib/libinstant.so"
ln -sf libinstant.so "$TCC_SYSROOT/lib/libc.so"
cp "$LOADER" "$TCC_SYSROOT/lib/ld-instantos.so"

ln -sf ../include "$TCC_SYSROOT/usr/include"
ln -sf ../lib "$TCC_SYSROOT/usr/lib"

cat > "$TCC_SYSROOT/README.instantos" <<EOF
InstantOS TCC sysroot

Layout:
  include/          ilibcxx C/POSIX headers
  lib/crt0.o        InstantOS process entry object
  lib/crt1.o        alias for crt0.o for hosted compiler conventions
  lib/Scrt1.o       alias for crt0.o for PIE compiler conventions
  lib/crti.o        empty init-section compatibility object
  lib/crtn.o        empty fini-section compatibility object
  lib/libinstant.so InstantOS userland runtime/libc shim
  lib/libc.so       alias for libinstant.so so -lc resolves
  lib/ld-instantos.so dynamic loader used by produced executables
  lib/tcc/libtcc1.a TinyCC runtime support archive after tools/build-tcc.sh

Executables should be linked as PIE with interpreter /lib/ld-instantos.so.
EOF

printf 'TCC sysroot ready: %s\n' "$TCC_SYSROOT"
