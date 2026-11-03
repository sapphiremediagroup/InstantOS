#!/usr/bin/env bash
# Cross-build libdrm (core only: xf86drm, the sync object and PRIME helpers,
# drmGetDevice2) against the InstantOS mlibc sysroot, then drm-probe
# (tools/drm-probe/drm_probe.c), which drives /dev/dri/renderD128 through it.
#
# libdrm is installed static (libdrm.a, xf86drm.h, libdrm/*.h, libdrm.pc)
# for the later Mesa port; every vendor library is off (Mesa's iris and anv
# need only the core). drm-probe lands in build/drm-probe/drm-probe, which
# CMake bundles as /bin/drm-probe when it exists (rerun cmake).
#
# Prerequisites: tools/build-mlibc.sh first (its ioctl wrapper translates the
# fds in DRM structs between mlibc's table and the kernel's handle slots).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
LIBDRM_VERSION="${LIBDRM_VERSION:-libdrm-2.4.123}"
LIBDRM_URL="${LIBDRM_URL:-https://dri.freedesktop.org/libdrm/${LIBDRM_VERSION}.tar.xz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/libdrm-port}"
SRC_DIR="$WORK_DIR/$LIBDRM_VERSION"
OBJ_DIR="$WORK_DIR/build"
PROBE_DIR="$BUILD_DIR/drm-probe"
MESON="${MESON:-meson}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$LIBDRM_URL"
  curl -sfL "$LIBDRM_URL" -o "$WORK_DIR/$LIBDRM_VERSION.tar.xz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$LIBDRM_VERSION.tar.xz"
fi

# The Linux uAPI headers DRM's headers need (linux/types.h, asm/ioctl.h: Linux's
# ioctl encoding, which the kernel uses) go into the sysroot first; __linux__
# makes libdrm (and later Mesa) take their Linux paths: the same ioctls,
# /dev/dri, sysfs, major 226.
mkdir -p "$MLIBC_ROOT/include/linux" "$MLIBC_ROOT/include/asm"
cp "$ROOT/tools/libdrm-port/include/linux/types.h" "$MLIBC_ROOT/include/linux/types.h"
cp "$ROOT/tools/libdrm-port/include/asm/ioctl.h" "$MLIBC_ROOT/include/asm/ioctl.h"

TARGET_CFLAGS="['--target=x86_64-unknown-elf', '-ffreestanding', '-fPIC', '-fno-stack-protector', '-O2', '-nostdlibinc', '-isystem', '$MLIBC_ROOT/include', '-D_GNU_SOURCE', '-D__linux__']"
TARGET_LDFLAGS="['--target=x86_64-unknown-elf', '-pie', '-nostdlib', '-fuse-ld=lld', '-L$MLIBC_ROOT/lib', '-Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so', '-Wl,-rpath,/lib/mlibc', '$MLIBC_ROOT/lib/crt1.o', '-lc']"
CROSS="$WORK_DIR/instantos-cross.ini"
cat > "$CROSS" <<EOF
[binaries]
c = 'clang'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
c_args = $TARGET_CFLAGS
c_link_args = $TARGET_LDFLAGS
EOF

rm -rf "$OBJ_DIR"
# host_machine.system is 'linux' so libdrm takes its Linux paths (sysfs,
# /dev/dri, major 226): what the kernel now provides.
"$MESON" setup "$OBJ_DIR" "$SRC_DIR" \
  --cross-file "$CROSS" \
  --prefix "$MLIBC_ROOT" \
  --libdir lib \
  -Ddefault_library=static \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled \
  -Dtegra=disabled -Dvc4=disabled -Detnaviv=disabled \
  -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
  -Dtests=false -Dinstall-test-programs=false -Dudev=false
"$MESON" compile -C "$OBJ_DIR"
"$MESON" install -C "$OBJ_DIR"

# drm-probe, linked against it.
mkdir -p "$PROBE_DIR"
clang --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -O2 -nostdlibinc \
  -isystem "$MLIBC_ROOT/include" -I"$MLIBC_ROOT/include/libdrm" -D_GNU_SOURCE -D__linux__ \
  -c "$ROOT/tools/drm-probe/drm_probe.c" -o "$PROBE_DIR/drm_probe.o"
clang --target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L"$MLIBC_ROOT/lib" \
  -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc "$MLIBC_ROOT/lib/crt1.o" \
  "$PROBE_DIR/drm_probe.o" -ldrm -lc -o "$PROBE_DIR/drm-probe"
printf 'libdrm %s in %s; drm-probe in %s (rerun cmake to bundle it)\n' "$LIBDRM_VERSION" "$MLIBC_ROOT" "$PROBE_DIR"
