#!/usr/bin/env bash
# Cross-build the classic IJG libjpeg against the InstantOS mlibc sysroot.
#
# Produces a static libjpeg.a plus headers installed into the mlibc sysroot for
# NetSurf's JPEG image decoding.  The IJG libjpeg uses a plain autoconf
# configure with no SIMD/assembly, so it cross-compiles cleanly to a
# freestanding target (simpler than libjpeg-turbo, which needs CMake + NASM).
#
# Prerequisites: tools/build-mlibc.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
JPEG_VERSION="${JPEG_VERSION:-jpegsrc.v9f}"
JPEG_DIR="${JPEG_DIR:-jpeg-9f}"
JPEG_URL="${JPEG_URL:-https://www.ijg.org/files/${JPEG_VERSION}.tar.gz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/libjpeg-port}"
SRC_DIR="$WORK_DIR/$JPEG_DIR"
OBJ_DIR="$WORK_DIR/build"
CC="${CC:-clang}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$JPEG_URL"
  curl -sL "$JPEG_URL" -o "$WORK_DIR/$JPEG_VERSION.tar.gz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$JPEG_VERSION.tar.gz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
CPPFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="-lc" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --prefix="$MLIBC_ROOT" \
  --enable-static \
  --disable-shared

# Build only the static library (skip cjpeg/djpeg/etc. which link executables).
make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" libjpeg.la

# Install headers + static archive.
cp .libs/libjpeg.a "$MLIBC_ROOT/lib/libjpeg.a"
"${LLVM_RANLIB:-llvm-ranlib}" "$MLIBC_ROOT/lib/libjpeg.a" 2>/dev/null || true
# Public headers: jpeglib.h needs jconfig.h (generated), jmorecfg.h, jerror.h.
cp jconfig.h "$MLIBC_ROOT/include/" 2>/dev/null || true
cp "$SRC_DIR"/jpeglib.h "$SRC_DIR"/jmorecfg.h "$SRC_DIR"/jerror.h "$MLIBC_ROOT/include/"

# Emit a pkg-config file so NetSurf's pkg-config lookup finds libjpeg.
JPEG_PC_VERSION="${JPEG_VERSION#jpegsrc.v}"
JPEG_PC_VERSION="${JPEG_PC_VERSION%%[a-z]*}"
[ -n "$JPEG_PC_VERSION" ] || JPEG_PC_VERSION="9"
mkdir -p "$MLIBC_ROOT/lib/pkgconfig"
cat > "$MLIBC_ROOT/lib/pkgconfig/libjpeg.pc" <<EOF
prefix=$MLIBC_ROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libjpeg
Description: A SIMD-accelerated JPEG codec (static)
Version: $JPEG_PC_VERSION
Libs: -L\${libdir} -ljpeg
Cflags: -I\${includedir}
EOF

printf 'libjpeg built: %s\n' "$MLIBC_ROOT/lib/libjpeg.a"
ls -l "$MLIBC_ROOT/lib/libjpeg.a" "$MLIBC_ROOT/include/jpeglib.h"
