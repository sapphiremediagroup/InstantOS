#!/usr/bin/env bash
# Cross-build zlib against the InstantOS mlibc sysroot.
#
# Produces a static libz.a plus headers (zlib.h, zconf.h) installed into the
# mlibc sysroot, so later ports (libpng, NetSurf) can link against it.  zlib's
# configure is a hand-written script (not autoconf); it honors CC/CFLAGS and a
# --static flag and cross-compiles cleanly to a freestanding target.
#
# Prerequisites: tools/build-mlibc.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
ZLIB_VERSION="${ZLIB_VERSION:-zlib-1.3.1}"
ZLIB_URL="${ZLIB_URL:-https://github.com/madler/zlib/releases/download/v1.3.1/${ZLIB_VERSION}.tar.gz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/zlib-port}"
SRC_DIR="$WORK_DIR/$ZLIB_VERSION"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
RANLIB="${RANLIB:-llvm-ranlib}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

# Same freestanding-against-mlibc toolchain flags the bash/coreutils ports use.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$ZLIB_URL"
  curl -sL "$ZLIB_URL" -o "$WORK_DIR/$ZLIB_VERSION.tar.gz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$ZLIB_VERSION.tar.gz"
fi

cd "$SRC_DIR"
make distclean >/dev/null 2>&1 || true

# zlib's configure builds a config; --static avoids needing a shared-link step.
CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
AR="$AR" \
RANLIB="$RANLIB" \
./configure --static --prefix="$MLIBC_ROOT"

# Build only the static library (skip the shared lib and the example/test
# binaries, which would try to link executables against a freestanding libc).
make -j"$(nproc)" CC="$CC" CFLAGS="$TARGET_CFLAGS" libz.a

# Install headers + the static archive into the sysroot.
mkdir -p "$MLIBC_ROOT/include" "$MLIBC_ROOT/lib"
cp zlib.h zconf.h "$MLIBC_ROOT/include/"
cp libz.a "$MLIBC_ROOT/lib/"
"$RANLIB" "$MLIBC_ROOT/lib/libz.a" 2>/dev/null || true

# Emit a pkg-config file (libpng.pc Requires: zlib, and NetSurf resolves deps
# transitively via pkg-config).
ZLIB_PC_VERSION="$(sed -n 's/^#define ZLIB_VERSION "\(.*\)"/\1/p' zlib.h 2>/dev/null)"
[ -n "$ZLIB_PC_VERSION" ] || ZLIB_PC_VERSION="1.3"
mkdir -p "$MLIBC_ROOT/lib/pkgconfig"
cat > "$MLIBC_ROOT/lib/pkgconfig/zlib.pc" <<EOF
prefix=$MLIBC_ROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: zlib compression library
Version: $ZLIB_PC_VERSION
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF

printf 'zlib built: %s\n' "$MLIBC_ROOT/lib/libz.a"
ls -l "$MLIBC_ROOT/lib/libz.a" "$MLIBC_ROOT/include/zlib.h"
