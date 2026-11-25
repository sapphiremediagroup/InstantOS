#!/usr/bin/env bash
# P5.4 oracle ISO: /bin/hellocpp is hello.cpp (iostream + exceptions + threads)
# compiled by the GCC cross g++. /bin/login runs it and checks output. BUILD
# ONLY; boot under QEMU and grep serial for GCC_CXX_* (GCC_CXX_ALL_OK).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/gcc-hello-cpp-iso}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
GP_DIR="$ROOT/tools/gcc-port"
BX="$BUILD_DIR/binutils-cross/bin"
GXX_CROSS="${GXX_CROSS:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/bin/x86_64-unknown-instantos-g++}"
CXXLIB="${CXXLIB:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install/x86_64-unknown-instantos/lib}"

[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc missing" >&2; exit 2; }
[ -x "$GXX_CROSS" ] || { echo "cross g++ missing; build gcc with LANGS=c,c++" >&2; exit 2; }
[ -f "$CXXLIB/libstdc++.so.6" ] || { echo "libstdc++.so.6 missing" >&2; exit 2; }

mkdir -p "$OUT_DIR"
if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj \
        input-manager storage-manager process-manager network-manager font-manager session-manager
fi

# hello.cpp compiled by the cross g++; rpath /lib/mlibc so the loader finds
# libstdc++.so.6 / libgcc_s.so.1 / libc.so there. CPP_SRC selects the oracle
# (default: full hello.cpp with threads; hello-cpp-noth.cpp = iostream+exc only).
CPP_SRC="${CPP_SRC:-hello.cpp}"
env PATH="$BX:/usr/bin:/bin" "$GXX_CROSS" -O2 -Wl,-rpath,/lib/mlibc \
  "$GP_DIR/$CPP_SRC" -o "$OUT_DIR/hellocpp"

# driver -> /bin/login (non-mlibc freestanding)
LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -c "$GP_DIR/gcc-cxx-smoke.c" -o "$OUT_DIR/smoke.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so \
  -o "$LAUNCHER" "$OUT_DIR/smoke.o"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/gcc-hello-cpp.iso"
rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"

ENTRIES=(
  bin/input-manager:"$BUILD_DIR/input-manager"
  bin/storage-manager:"$BUILD_DIR/storage-manager"
  bin/process-manager:"$BUILD_DIR/process-manager"
  bin/font-manager:"$BUILD_DIR/font-manager"
  bin/session-manager:"$BUILD_DIR/session-manager"
  bin/login:"$LAUNCHER"
  bin/hellocpp:"$OUT_DIR/hellocpp"
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so"
  lib/libinstant.so:"$BUILD_DIR/libinstant.so"
  lib/mlibc/ld-instantos.so:"$MLIBC_ROOT/lib/ld-instantos.so"
  lib/mlibc/libc.so:"$MLIBC_ROOT/lib/libc.so"
  lib/mlibc/libdl.so:"$MLIBC_ROOT/lib/libdl.so"
  lib/mlibc/libstdc++.so.6:"$CXXLIB/libstdc++.so.6"
  lib/mlibc/libgcc_s.so.1:"$CXXLIB/libgcc_s.so.1"
)

"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" "${ENTRIES[@]}"

cp "$BUILD_DIR/BOOTX64.EFI" "$EFI_DIR/BOOTX64.EFI"
cp "$BUILD_DIR/INSTANTOS.EFI" "$EFI_DIR/INSTANTOS.EFI"
cp "$INITRD" "$EFI_DIR/INITRD"

dd if=/dev/zero of="$EFI_IMG" bs=1M count=64 status=none
mformat -i "$EFI_IMG" ::
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_IMG" "$BUILD_DIR/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$EFI_IMG" "$INITRD" ::/EFI/BOOT/INITRD
xorriso -as mkisofs -R -J -joliet-long -iso-level 3 \
  -eltorito-alt-boot -e EFI/BOOT/efiboot.img -no-emul-boot \
  -o "$ISO" "$ISO_ROOT" >/dev/null 2>&1

DISK_IMG="$OUT_DIR/ahci.img"
rm -f "$DISK_IMG"; truncate -s 64M "$DISK_IMG"
printf 'label: dos\nstart=2048, type=0c\n' | sfdisk "$DISK_IMG" >/dev/null 2>&1
mformat -F -i "$DISK_IMG@@1M" ::

echo "GCC_HELLO_CPP_ISO=$ISO"
echo "GCC_HELLO_CPP_DISK=$DISK_IMG"
echo "initrd entries: ${#ENTRIES[@]}"
