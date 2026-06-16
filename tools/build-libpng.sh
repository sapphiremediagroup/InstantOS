#!/usr/bin/env bash
# Cross-build libpng against the InstantOS mlibc sysroot.
#
# Produces a static libpng16.a plus headers installed into the mlibc sysroot for
# later use by NetSurf's image decoders.  libpng uses an autoconf configure and
# depends on zlib, so tools/build-zlib.sh must have run first.
#
# Prerequisites: tools/build-mlibc.sh, tools/build-zlib.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
PNG_VERSION="${PNG_VERSION:-libpng-1.6.43}"
PNG_URL="${PNG_URL:-https://download.sourceforge.net/libpng/${PNG_VERSION}.tar.xz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/libpng-port}"
SRC_DIR="$WORK_DIR/$PNG_VERSION"
OBJ_DIR="$WORK_DIR/build"
CC="${CC:-clang}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
if [ ! -f "$MLIBC_ROOT/lib/libz.a" ]; then
  printf 'zlib missing (%s/lib/libz.a); run tools/build-zlib.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

# Freestanding-against-mlibc flags; point at the sysroot for zlib.h and libz.a.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -I$MLIBC_ROOT/include -D_GNU_SOURCE"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$PNG_URL"
  curl -sL "$PNG_URL" -o "$WORK_DIR/$PNG_VERSION.tar.xz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$PNG_VERSION.tar.xz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# libpng's configure runs a couple of link tests (AC_CHECK_FUNCS for pow, zlib
# probe). Provide cache answers so it does not try to run target binaries, and
# disable the ARM/Intel SIMD optimizations + the tools (pngfix/pngtest link
# executables against a freestanding libc).
cat > config.cache <<'EOF'
ac_cv_func_pow=yes
ac_cv_lib_z_zlibVersion=yes
EOF

CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
CPPFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="-lz -lc" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --prefix="$MLIBC_ROOT" \
  --enable-static \
  --disable-shared \
  --disable-tools \
  --without-binconfigs \
  --enable-hardware-optimizations=no

# Build only the static library target (avoids building pngtest/pngfix, which
# would link executables against the freestanding runtime).
make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" libpng16.la

# Install just the headers and the static archive.
make install-pkgincludeHEADERS install-nodist_pkgincludeHEADERS >/dev/null 2>&1 || true
# The .la wraps .libs/libpng16.a; copy the real archive and create the libpng.a alias.
cp .libs/libpng16.a "$MLIBC_ROOT/lib/libpng16.a"
cp "$MLIBC_ROOT/lib/libpng16.a" "$MLIBC_ROOT/lib/libpng.a"
"${LLVM_RANLIB:-llvm-ranlib}" "$MLIBC_ROOT/lib/libpng16.a" 2>/dev/null || true
# Ensure headers are present even if the install target name differs by version.
cp "$SRC_DIR"/png.h "$SRC_DIR"/pngconf.h pnglibconf.h "$MLIBC_ROOT/include/" 2>/dev/null || true

# Emit a pkg-config file so NetSurf's `pkg_config_find_and_add_enabled` can find
# libpng (it does not search for bare .a/.h, only via pkg-config).
PNG_PC_VERSION="${PNG_VERSION#libpng-}"
mkdir -p "$MLIBC_ROOT/lib/pkgconfig"
cat > "$MLIBC_ROOT/lib/pkgconfig/libpng16.pc" <<EOF
prefix=$MLIBC_ROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libpng
Description: Loads and saves PNG files
Version: $PNG_PC_VERSION
Requires: zlib
Libs: -L\${libdir} -lpng16
Cflags: -I\${includedir}
EOF
cp "$MLIBC_ROOT/lib/pkgconfig/libpng16.pc" "$MLIBC_ROOT/lib/pkgconfig/libpng.pc"

printf 'libpng built: %s\n' "$MLIBC_ROOT/lib/libpng16.a"
ls -l "$MLIBC_ROOT/lib/libpng16.a" "$MLIBC_ROOT/include/png.h"
