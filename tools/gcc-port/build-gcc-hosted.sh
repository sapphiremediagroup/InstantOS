#!/usr/bin/env bash
# P6.3: Canadian-cross build of GCC hosted ON InstantOS.
#   build = x86_64-linux (WSL), host = target = x86_64-unknown-instantos.
# The Phase 5 cross (x86_64-unknown-instantos-gcc/g++) compiles the GCC sources
# into InstantOS/mlibc ELF binaries (cc1, cc1plus, gcc, g++) that RUN in-OS. The
# in-tree gmp/mpfr/mpc are built for host=instantos as part of this build.
#
# We build ONLY the compiler (all-gcc): the target libgcc/libstdc++ cannot be
# built here (that needs to RUN the freshly built host=instantos cc1, which is
# not a Linux binary). Those come from the Phase 5 cross install (identical,
# target=instantos, same GCC 13.3.0) at install-assembly time (P6.4).
set -euo pipefail

GCC_SRC="${GCC_SRC:-/home/sky/gcc-port/gcc-13.3.0}"
BUILD="${GCC_HOSTED_BUILD:-/home/sky/gcc-port/build-hosted}"
PREFIX="${GCC_HOSTED_PREFIX:-/home/sky/gcc-port/gcc-hosted-install}"
CROSS_BIN="/home/sky/gcc-port/gcc-install/bin"
BINUTILS_CROSS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build/binutils-cross/bin"

# The cross compiler (host compiler) + cross binutils (host & target tools).
export PATH="$CROSS_BIN:$BINUTILS_CROSS:$PATH"

[ -x "$CROSS_BIN/x86_64-unknown-instantos-gcc" ] || { echo "Phase 5 cross gcc missing" >&2; exit 2; }
[ -x "$BINUTILS_CROSS/x86_64-unknown-instantos-ld" ] || { echo "binutils-cross missing" >&2; exit 2; }
[ -d "$GCC_SRC/gmp" ] || { echo "in-tree gmp missing (run download_prerequisites)" >&2; exit 2; }

# --- Source fixups for the InstantOS hosted build (idempotent) -------------
# (1) The in-tree gmp/mpfr/mpc/isl ship ancient config.sub/config.guess that do
#     not know the 'instantos' OS; overlay GCC's instantos-aware ones.
GCC_SRC="$GCC_SRC" bash "$(dirname "${BASH_SOURCE[0]}")/patch-gcc-prereqs-config.sh"
# (2) gcc/cp/module.cc's C++-modules mmap I/O uses madvise()/MADV_* which mlibc
#     does not expose (linux_option disabled). Force the plain read/write path.
sed -i 's|#if 0 // 1 for testing no mmap|#if 1 // instantos: no madvise/MADV_* (linux_option disabled)|' \
    "$GCC_SRC/gcc/cp/module.cc" 2>/dev/null || true

if [ "${RECONFIGURE:-0}" = "1" ]; then rm -rf "$BUILD"; fi
mkdir -p "$BUILD"

if [ ! -f "$BUILD/Makefile" ]; then
  ( cd "$BUILD" && "$GCC_SRC/configure" \
      --build=x86_64-pc-linux-gnu \
      --host=x86_64-unknown-instantos \
      --target=x86_64-unknown-instantos \
      --prefix=/usr \
      --with-sysroot=/ \
      --with-native-system-header-dir=/usr/include \
      --disable-bootstrap \
      --disable-multilib \
      --disable-nls \
      --enable-languages=c,c++ \
      --disable-libsanitizer \
      --disable-libssp \
      --disable-libgomp \
      --disable-libquadmath \
      --enable-default-pie \
      --enable-default-hash-style=sysv \
      MAKEINFO=true )
fi

if [ "${STOP_AFTER_CONFIGURE:-0}" = "1" ]; then
  echo "configured: $BUILD"; exit 0
fi

# Build + install only the compiler (host=instantos binaries).
# JOBS: parallel compiles; each cc1plus of gcc's big files takes several hundred MB.
make -C "$BUILD" -j"${JOBS:-$(nproc)}" all-gcc MAKEINFO=true
make -C "$BUILD" install-gcc MAKEINFO=true DESTDIR="$PREFIX"

echo "hosted gcc compiler installed under: $PREFIX"
find "$PREFIX" -name 'cc1' -o -name 'cc1plus' -o -name 'g++' 2>/dev/null | head
