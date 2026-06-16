#!/usr/bin/env bash
# Cross-build the NetSurf core C libraries against the InstantOS mlibc sysroot.
#
# NetSurf splits into ~10 first-party libraries that use NetSurf's own shared
# "buildsystem" (Makefile fragments), not autoconf. They must be built in
# dependency order. We trick the buildsystem into a "native" build (BUILD==HOST)
# so it uses our CC verbatim, and bake the freestanding mlibc target flags into
# CFLAGS/LDFLAGS. Everything installs into a NetSurf prefix that also references
# the mlibc sysroot for libc/zlib/png/jpeg.
#
# Prerequisites: tools/build-mlibc.sh, build-zlib.sh, build-libpng.sh, build-libjpeg.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
NS_ROOT="${NS_ROOT:-$BUILD_DIR/netsurf-root}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/netsurf-port}"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
BASEURL="https://download.netsurf-browser.org/libs/releases"

# Host build tools (gperf for libhubbub/libcss codegen) live here when not
# installed system-wide. Extracted by the porting setup, prepended to PATH.
HOSTTOOLS="${HOSTTOOLS:-$BUILD_DIR/hosttools/bin}"
if [ -d "$HOSTTOOLS" ]; then
  export PATH="$HOSTTOOLS:$PATH"
  # bison/flex (libnslog grammar) need their data/skeleton dirs and m4.
  [ -d "$BUILD_DIR/hosttools/share/bison" ] && export BISON_PKGDATADIR="$BUILD_DIR/hosttools/share/bison"
  [ -x "$HOSTTOOLS/m4" ] && export M4="$HOSTTOOLS/m4"
fi

# Pinned latest (2023-12) release versions.
declare -A VER=(
  [buildsystem]=1.10
  [libwapcaplet]=0.4.3
  [libparserutils]=0.2.5
  [libhubbub]=0.3.8
  [libdom]=0.4.2
  [libcss]=0.9.2
  [libnsgif]=1.0.0
  [libnsbmp]=0.1.7
  [libnsutils]=0.1.1
  [libnspsl]=0.1.7
  [libnslog]=0.1.3
  [libnsfb]=0.2.2
  [nsgenbind]=0.9
)

if [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  echo "mlibc sysroot missing ($MLIBC_ROOT); run tools/build-mlibc.sh first" >&2
  exit 2
fi

mkdir -p "$WORK_DIR" "$NS_ROOT"

# Tracked InstantOS port sources (instant.c backend, iconv shim, patches).
PORT_DIR="$ROOT/tools/netsurf-port"

# --- iconv shim ---------------------------------------------------------------
# mlibc has no iconv(3). NetSurf still references the iconv symbols at link
# time, so we provide a tiny static libiconvshim.a (UTF-8 <-> ASCII/Latin-1/
# CP1252) plus its header, installed into the mlibc sysroot.
build_iconv_shim() {
  echo "=== building iconv shim ==="
  install -d "$MLIBC_ROOT/include" "$MLIBC_ROOT/lib"
  install -m644 "$PORT_DIR/iconv-shim/iconv.h" "$MLIBC_ROOT/include/iconv.h"
  "$CC" --target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector \
    -nostdlibinc -isystem "$MLIBC_ROOT/include" -D_GNU_SOURCE -Wno-error \
    -c "$PORT_DIR/iconv-shim/iconv.c" -o "$WORK_DIR/iconvshim.o"
  "$AR" rcs "$MLIBC_ROOT/lib/libiconvshim.a" "$WORK_DIR/iconvshim.o"
  # NetSurf's Makefile appends a bare `-liconv` when LIBICONV_PLUG=NO, so also
  # install the same archive under the libiconv name the linker expects.
  "$AR" rcs "$MLIBC_ROOT/lib/libiconv.a" "$WORK_DIR/iconvshim.o"
  echo "=== installed libiconvshim.a + libiconv.a + iconv.h -> $MLIBC_ROOT ==="
}
build_iconv_shim

# --- fetch + extract a release tarball (returns extracted dir name) -----------
fetch() {
  local name="$1" ver="${VER[$1]}" suffix="$2"  # suffix: "" or "-src"
  local tarball="${name}-${ver}${suffix}"
  local dest="$WORK_DIR/${name}-${ver}"
  if [ ! -d "$dest" ]; then
    echo "fetching $tarball" >&2
    local archive="$WORK_DIR/${tarball}.tar.gz"
    # Retry the download until we have a valid gzip archive (the mirror
    # occasionally returns a short/empty body).
    local tries=0
    while [ "$tries" -lt 4 ]; do
      curl -fsSL "$BASEURL/${tarball}.tar.gz" -o "$archive" 2>/dev/null || true
      if gzip -t "$archive" 2>/dev/null; then break; fi
      tries=$((tries+1))
      sleep 1
    done
    if ! gzip -t "$archive" 2>/dev/null; then
      echo "failed to download a valid $tarball" >&2
      exit 3
    fi
    tar -C "$WORK_DIR" -xf "$archive"
    if [ ! -d "$dest" ]; then
      echo "extraction did not produce $dest" >&2
      exit 3
    fi
  fi
  echo "$dest"
}

# Freestanding-against-mlibc flags, plus the NetSurf prefix include/lib so libs
# find each other's headers. -DWITHOUT_ICONV_FILTER makes libparserutils use its
# built-in charset codecs instead of iconv (mlibc has no iconv).
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -I$NS_ROOT/include -D_GNU_SOURCE -DWITHOUT_ICONV_FILTER -Wno-error"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -L$NS_ROOT/lib"

# Common make args that force the buildsystem into a native (BUILD==HOST) build
# using our clang verbatim, producing a static lib.  CFLAGS/LDFLAGS are passed
# via the ENVIRONMENT (not make args) so the component Makefile's own
# `CFLAGS := -I$(CURDIR)/include ... $(CFLAGS)` append keeps its -I paths; a
# make-command-line CFLAGS= would override (discard) those.
ns_make() {
  local dir="$1"; shift
  CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" \
  make -C "$dir" -j"$(nproc)" \
    CC="$CC" BUILD_CC="cc" AR="$AR" \
    HOST=x86_64-unknown-elf BUILD=x86_64-unknown-elf \
    COMPONENT_TYPE=lib-static \
    PREFIX="$NS_ROOT" \
    NSSHARED="$NS_ROOT/share/netsurf-buildsystem" \
    Q= "$@"
}

# --- buildsystem (host tool collection; install into NS_ROOT) -----------------
BS_DIR="$(fetch buildsystem "")"
make -C "$BS_DIR" install PREFIX="$NS_ROOT" >/dev/null
echo "buildsystem installed -> $NS_ROOT/share/netsurf-buildsystem"

# --- helper: build+install one library ----------------------------------------
build_lib() {
  local name="$1"; shift
  local extra=("$@")
  local dir; dir="$(fetch "$name" -src)"
  echo "=== building $name ==="
  ns_make "$dir" "${extra[@]}"
  ns_make "$dir" install "${extra[@]}"
  echo "=== installed $name ==="
}

# --- helper: apply a patch once (idempotent) ----------------------------------
# Uses `patch` with --forward so re-running the script on an already-patched
# tree is a no-op rather than an error.
apply_patch_once() {
  local dir="$1" patchfile="$2"
  if patch -p1 -d "$dir" --dry-run --reverse --force < "$patchfile" >/dev/null 2>&1; then
    echo "  (already applied: $(basename "$patchfile"))"
    return 0
  fi
  patch -p1 -d "$dir" --forward < "$patchfile"
}

# Dependency order. Leaves first, then parsers, then DOM/CSS, then image/fb.
build_lib libwapcaplet
build_lib libparserutils
build_lib libhubbub
build_lib libcss
build_lib libnsutils
build_lib libnslog
build_lib libnsgif
build_lib libnsbmp
build_lib libnspsl

# --- nsgenbind: a build-time HOST tool that generates libdom's JS/DOM bindings.
# It must be compiled for the build machine (native cc), NOT cross-compiled.
NSGENBIND_DIR="$(fetch nsgenbind -src)"
echo "=== building nsgenbind (native host tool) ==="
make -C "$NSGENBIND_DIR" -j"$(nproc)" \
  CC="cc" BUILD_CC="cc" \
  HOST="$(cc -dumpmachine)" BUILD="$(cc -dumpmachine)" \
  PREFIX="$BUILD_DIR/hosttools" \
  NSSHARED="$NS_ROOT/share/netsurf-buildsystem" \
  Q=
make -C "$NSGENBIND_DIR" install \
  PREFIX="$BUILD_DIR/hosttools" \
  NSSHARED="$NS_ROOT/share/netsurf-buildsystem" >/dev/null
echo "=== installed nsgenbind -> $BUILD_DIR/hosttools/bin/nsgenbind ==="

# --- libdom: needs nsgenbind (from hosttools, now on PATH) --------------------
# Disable the XML bindings (expat/libxml2 not ported); HTML parsing via the
# hubbub binding is what the browser needs.
build_lib libdom WITH_EXPAT_BINDING=no WITH_LIBXML_BINDING=no

# --- libnsfb: framebuffer abstraction. We ship a custom 'instant' surface
# backend (src/surface/instant.c) that draws to the InstantOS compositor.
# Force-disable the SDL/X/VNC/Wayland backends (host pkg-config may otherwise
# detect SDL/xcb and try to build them against headers we don't cross-have).
#
# The 'instant' backend source + the two libnsfb patches (register the surface
# enum/prototype, add instant.c to the surface Makefile) live in the tracked
# tools/netsurf-port/ tree and are injected into the extracted libnsfb here.
LIBNSFB_DIR="$(fetch libnsfb -src)"
echo "=== injecting instant surface backend into libnsfb ==="
install -m644 "$PORT_DIR/libnsfb/instant.c" "$LIBNSFB_DIR/src/surface/instant.c"
apply_patch_once "$LIBNSFB_DIR" "$PORT_DIR/patches/libnsfb-header.patch"
apply_patch_once "$LIBNSFB_DIR" "$PORT_DIR/patches/libnsfb-surface-makefile.patch"
echo "=== building libnsfb ==="
ns_make "$LIBNSFB_DIR" NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no NSFB_VNC_AVAILABLE=no NSFB_WLD_AVAILABLE=no
ns_make "$LIBNSFB_DIR" install NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no NSFB_VNC_AVAILABLE=no NSFB_WLD_AVAILABLE=no
echo "=== installed libnsfb ==="

echo
echo "NetSurf core libs installed into: $NS_ROOT"
ls -1 "$NS_ROOT/lib"/*.a 2>/dev/null || true
