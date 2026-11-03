#!/usr/bin/env bash
# P5: build the GCC cross-compiler (runs on WSL, targets x86_64-unknown-instantos
# / mlibc). Assembles an mlibc GCC sysroot, applies the OS port, configures and
# builds gcc + libgcc against the Step-A binutils cross tools.
#
# Native build (fast, wedge-safe); GCC source under /home/sky/gcc-port.
#   STOP_AFTER_CONFIGURE=1  validate the port without the long build
#   LANGS=c,c++             build C++ too (default: c)
#   RECONFIGURE=1           force reconfigure
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$REPO/build"
NATIVE="/home/sky/gcc-port"
SRC="$NATIVE/gcc-13.3.0"
OBJ="$NATIVE/build-cross"
PREFIX="$NATIVE/gcc-install"
SYSROOT="$NATIVE/instantos-sysroot"
MLIBC_ROOT="$BUILD_DIR/mlibc-root"
BX="$BUILD_DIR/binutils-cross/bin"
JOBS="${JOBS:-6}"
LANGS="${LANGS:-c}"

# GCC 13's libcody (C++ module mapper) uses u8"" string literals that a modern
# host g++ (>=14; this box has GCC 16) treats as char8_t (C++20), breaking its
# S2C() overloads. Build the host tools with char8_t disabled. Harmless on
# older hosts and for the in-tree gmp/mpfr/mpc host builds.
export CXX="${CXX:-g++ -fno-char8_t}"
export CXX_FOR_BUILD="${CXX_FOR_BUILD:-$CXX}"

[ -d "$SRC" ] || { echo "GCC source missing at $SRC" >&2; exit 2; }
[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc missing" >&2; exit 2; }
[ -x "$BX/x86_64-unknown-instantos-as" ] || { echo "binutils-cross missing" >&2; exit 2; }

# --- assemble the mlibc GCC sysroot: headers + crt + libc.so ---
rm -rf "$SYSROOT"; mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib"
cp -a "$MLIBC_ROOT/include/." "$SYSROOT/usr/include/"
cp "$MLIBC_ROOT/lib/crt1.o" "$SYSROOT/usr/lib/crt1.o"
cp "$MLIBC_ROOT/lib/crt1.o" "$SYSROOT/usr/lib/Scrt1.o"   # mlibc crt1 is PIE-safe
"$BX/x86_64-unknown-instantos-as" "$REPO/tools/gcc-port/crti.s" -o "$SYSROOT/usr/lib/crti.o"
"$BX/x86_64-unknown-instantos-as" "$REPO/tools/gcc-port/crtn.s" -o "$SYSROOT/usr/lib/crtn.o"
cp "$MLIBC_ROOT/lib/libc.so" "$SYSROOT/usr/lib/libc.so"
[ -f "$MLIBC_ROOT/lib/libdl.so" ] && cp "$MLIBC_ROOT/lib/libdl.so" "$SYSROOT/usr/lib/libdl.so"
# libc.so has a DT_NEEDED on ld-instantos.so (the mlibc rtld provides its
# __dlapi_* symbols); ld must find it to link against libc.so.
cp "$MLIBC_ROOT/lib/ld-instantos.so" "$SYSROOT/usr/lib/ld-instantos.so"
# mlibc folds libm/libpthread/librt into libc; alias them to libc.so so
# -lm/-lpthread/-lrt resolve at link time. The symlink target's SONAME is
# libc.so, so binaries get a DT_NEEDED on libc.so only (no missing runtime dep).
for alias in libm libpthread librt; do
  ln -sf libc.so "$SYSROOT/usr/lib/$alias.so"
done
echo "sysroot assembled at $SYSROOT"

# --- apply the instantos GCC port ---
bash "$REPO/tools/gcc-port/apply-gcc-instantos-port.sh" "$SRC"

# Host-compat (modern host g++ >=15, e.g. GCC 16): its libstdc++ <string> pulls
# in <locale> whose std::toupper(c,loc) collides with gcc/system.h's safe-ctype.h
# ctype poisoning. Pre-include <string>/<locale> before the poisoning. Idempotent.
if ! grep -q 'instantos host-compat' "$SRC/gcc/system.h"; then
  perl -0pi -e 's{(# include <type_traits>\n)(#endif)}{$1/* instantos host-compat: pre-include locale headers before safe-ctype.h */\n# include <string>\n# include <locale>\n$2}' "$SRC/gcc/system.h"
  grep -q 'instantos host-compat' "$SRC/gcc/system.h" && echo "patched gcc/system.h (host-compat)" || echo "WARN: system.h host-compat anchor not found" >&2
fi

# --- configure ---
if [ "${RECONFIGURE:-0}" = "1" ] || [ ! -f "$OBJ/Makefile" ]; then
  rm -rf "$OBJ"; mkdir -p "$OBJ"
  ( cd "$OBJ" && PATH="$BX:$PATH" "$SRC/configure" \
      --target=x86_64-unknown-instantos \
      --prefix="$PREFIX" \
      --with-sysroot="$SYSROOT" \
      --disable-multilib --disable-nls --disable-werror \
      --disable-libssp --disable-libsanitizer --disable-libgomp \
      --disable-libquadmath --disable-libvtv \
      --enable-default-pie --enable-initfini-array \
      --enable-languages="$LANGS" )
  echo "configured (langs=$LANGS)"
fi

if [ "${STOP_AFTER_CONFIGURE:-0}" = "1" ]; then
  echo "STOP_AFTER_CONFIGURE"; exit 0
fi

# --- build + install ---
PATH="$BX:$PATH" make -C "$OBJ" -j"$JOBS" all-gcc
PATH="$BX:$PATH" make -C "$OBJ" -j"$JOBS" all-target-libgcc
PATH="$BX:$PATH" make -C "$OBJ" install-gcc install-target-libgcc
# C++: the target runtime (libstdc++.so.6, libsupc++.a) against mlibc. Needs
# the cross gcc/libgcc installed above.
case ",$LANGS," in *,c++,*)
  PATH="$PREFIX/bin:$BX:$PATH" make -C "$OBJ" -j"$JOBS" all-target-libstdc++-v3
  PATH="$PREFIX/bin:$BX:$PATH" make -C "$OBJ" install-target-libstdc++-v3 ;;
esac

echo "gcc-cross installed to $PREFIX"
ls "$PREFIX/bin"
