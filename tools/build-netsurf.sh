#!/usr/bin/env bash
# Cross-build the NetSurf framebuffer browser against the InstantOS mlibc sysroot.
#
# Uses the NetSurf buildsystem (TARGET=framebuffer) with a minimal feature set:
# no curl/openssl (built-in fetchers only for now), no JS (duktape), internal
# bitmap font, our 'instant' libnsfb surface backend. Links against the NetSurf
# core libs (build-netsurf-libs.sh) and the image stack (zlib/png/jpeg).
#
# Prerequisites: build-mlibc.sh, build-zlib.sh, build-libpng.sh, build-libjpeg.sh,
#                build-netsurf-libs.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
NS_ROOT="${NS_ROOT:-$BUILD_DIR/netsurf-root}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/netsurf-port}"
NS_VERSION="${NS_VERSION:-netsurf-3.11}"
NS_URL="${NS_URL:-https://download.netsurf-browser.org/netsurf/releases/source/${NS_VERSION}-src.tar.gz}"
SRC_DIR="$WORK_DIR/$NS_VERSION"
CC="${CC:-clang}"
HOSTTOOLS="${HOSTTOOLS:-$BUILD_DIR/hosttools/bin}"
[ -d "$HOSTTOOLS" ] && export PATH="$HOSTTOOLS:$PATH"
[ -d "$BUILD_DIR/hosttools/share/bison" ] && export BISON_PKGDATADIR="$BUILD_DIR/hosttools/share/bison"
[ -x "$HOSTTOOLS/m4" ] && export M4="$HOSTTOOLS/m4"

if [ ! -f "$NS_ROOT/lib/libcss.a" ] || [ ! -f "$NS_ROOT/lib/libnsfb.a" ]; then
  echo "NetSurf core libs missing; run tools/build-netsurf-libs.sh first" >&2
  exit 2
fi

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  echo "fetching $NS_VERSION"
  curl -fsSL "$NS_URL" -o "$WORK_DIR/$NS_VERSION-src.tar.gz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$NS_VERSION-src.tar.gz"
fi

# Apply the tracked InstantOS NetSurf patches (idempotent):
#   - gui.c:   call nsfb_instant_register() so the 'instant' surface exists
#   - fetch.c: guard the curl fetcher include behind WITH_CURL (curl not ported)
PORT_DIR="$ROOT/tools/netsurf-port"
apply_patch_once() {
  local dir="$1" patchfile="$2"
  if patch -p1 -d "$dir" --dry-run --reverse --force < "$patchfile" >/dev/null 2>&1; then
    echo "  (already applied: $(basename "$patchfile"))"
    return 0
  fi
  patch -p1 -d "$dir" --forward < "$patchfile"
}
apply_patch_once "$SRC_DIR" "$PORT_DIR/patches/netsurf-gui.patch"
apply_patch_once "$SRC_DIR" "$PORT_DIR/patches/netsurf-fetch.patch"
apply_patch_once "$SRC_DIR" "$PORT_DIR/patches/netsurf-config-mmap.patch"
apply_patch_once "$SRC_DIR" "$PORT_DIR/patches/netsurf-cabundle.patch"

# pkg-config must find our cross .pc files and NOTHING from the host. Point it
# exclusively at the NetSurf+mlibc sysroots.
export PKG_CONFIG_PATH="$NS_ROOT/lib/pkgconfig:$MLIBC_ROOT/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$NS_ROOT/lib/pkgconfig:$MLIBC_ROOT/lib/pkgconfig"

# Freestanding-against-mlibc flags, plus NetSurf + mlibc include/lib dirs.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -I$NS_ROOT/include -D_GNU_SOURCE -D__instantos__ -DWITHOUT_ICONV_FILTER -Wno-error -Wno-implicit-function-declaration"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -L$NS_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc $MLIBC_ROOT/lib/crt1.o -Wl,--no-as-needed -lc -lm -liconvshim"

cd "$SRC_DIR"

CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" LIBS="-lpng16 -ljpeg -lz -liconvshim -lm -lc" \
make \
  TARGET=framebuffer \
  HOST=x86_64-unknown-elf BUILD=x86_64-unknown-elf \
  CC="$CC" BUILD_CC="cc" \
  NSSHARED="$NS_ROOT/share/netsurf-buildsystem" \
  PREFIX="$NS_ROOT" \
  Q= \
  NETSURF_USE_CURL=YES \
  NETSURF_USE_OPENSSL=NO \
  NETSURF_USE_DUKTAPE=NO \
  NETSURF_USE_UTF8PROC=NO \
  NETSURF_USE_JPEGXL=NO \
  NETSURF_USE_WEBP=NO \
  NETSURF_USE_VIDEO=NO \
  NETSURF_USE_HARU_PDF=NO \
  NETSURF_USE_NSSVG=NO \
  NETSURF_USE_ROSPRITE=NO \
  NETSURF_USE_LIBICONV_PLUG=NO \
  NETSURF_USE_PNG=YES \
  NETSURF_USE_JPEG=YES \
  NETSURF_USE_BMP=YES \
  NETSURF_USE_GIF=YES \
  NETSURF_USE_NSPSL=YES \
  NETSURF_USE_NSLOG=YES \
  NETSURF_FB_FONTLIB=internal \
  NETSURF_FRAMEBUFFER_RESOURCES=/netsurf/res/ \
  "$@"

echo
echo "NetSurf framebuffer build attempt complete."
ls -l "$SRC_DIR"/nsfb 2>/dev/null || ls -l "$SRC_DIR"/build/*/nsfb 2>/dev/null || true
