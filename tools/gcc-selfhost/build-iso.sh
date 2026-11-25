#!/usr/bin/env bash
# Phase 7 P7.0: in-OS GCC build-environment oracle ISO.
#
# Reuses the gcchost ext4 (build/gcchost/ext4.img -- has /usr/bin/gcc + the mlibc
# gcc world) at RUN time; this script builds only the ISO (kernel + an initrd of
# the managers, GNU make + /bin/sh + coreutils ports, the gcc-selfhost launcher,
# and a tiny multi-file make project). Boot headless with the gcchost ext4
# attached as an AHCI disk and grep serial for GCCSELF_* (GCCSELF_MAKE_OK == green).
#
# The mlibc runtime shipped at /lib/mlibc is the GCC-built one (matches gcc's
# libstdc++/libgcc_s); the clang-built GNU tools use it too (mlibc ABI is
# compiler-independent). The libinstant substrate (managers + launcher) uses
# /lib/ld-instantos.so as before.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/gcc-selfhost}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
CROSS="${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install"
MLIBC_GCC="$BUILD_DIR/mlibc-root-gcc"
GS_DIR="$ROOT/tools/gcc-selfhost"

for t in make bash sed grep tar gzip libinstant.so ld-instantos.so; do
  [ -f "$BUILD_DIR/$t" ] || { echo "missing build/$t" >&2; exit 2; }
done
[ -f "$MLIBC_GCC/lib/libc.so" ] || { echo "gcc mlibc missing; run tools/build-mlibc-gcc.sh" >&2; exit 2; }
[ -f "$BUILD_DIR/gcchost/ext4.img" ] || echo "note: build/gcchost/ext4.img not found; build it with tools/gcc-port/build-gcchost-iso.sh (attach it at run time)" >&2

mkdir -p "$OUT_DIR"
if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj \
        input-manager storage-manager process-manager font-manager session-manager
fi

# gcc-selfhost driver -> /bin/login (freestanding libinstant, like the other oracles).
LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -O1 -c "$GS_DIR/launcher.c" -o "$OUT_DIR/launcher.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$LAUNCHER" "$OUT_DIR/launcher.o"

# Small multi-file make project: main.c + a.c + b.c -> app (exits a()+b()-7 == 0;
# after the incremental edit a()=10 -> exits 7). configure-lite selects gcc and
# sed-substitutes Makefile.in. Recipe lines use TABs.
PROJ="$OUT_DIR/proj"; rm -rf "$PROJ"; mkdir -p "$PROJ"
printf 'extern int a(void); extern int b(void);\nint main(void){ return a()+b()-7; }\n' > "$PROJ/main.c"
printf 'int a(void){ return 3; }\n' > "$PROJ/a.c"
printf 'int b(void){ return 4; }\n' > "$PROJ/b.c"
printf 'CC = @CC@\napp: main.o a.o b.o\n\t$(CC) -o $@ $^\n%%.o: %%.c\n\t$(CC) -c $< -o $@\n' > "$PROJ/Makefile.in"
cat > "$PROJ/configure" <<'CFG'
#!/bin/sh
CC=gcc
echo "configure: checking for C compiler... $CC"
printf 'int main(void){return 0;}\n' > conftest.c
if $CC -c conftest.c -o conftest.o; then
  echo "configure: the C compiler works"
else
  echo "configure: error: C compiler cannot create object files" >&2
  exit 1
fi
if $CC -v 2>&1 | grep -q version; then
  echo "configure: compiler version check ok"
fi
echo "configure: creating Makefile"
sed "s|@CC@|$CC|g" Makefile.in > Makefile
echo "configure: done"
CFG
chmod +x "$PROJ/configure"
tar -czf "$OUT_DIR/proj.tar.gz" -C "$OUT_DIR" proj

INITRD="$OUT_DIR/initrd.img"; ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/gcc-selfhost.iso"
rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"

ENTRIES=(
  bin/input-manager:"$BUILD_DIR/input-manager"
  bin/storage-manager:"$BUILD_DIR/storage-manager"
  bin/process-manager:"$BUILD_DIR/process-manager"
  bin/font-manager:"$BUILD_DIR/font-manager"
  bin/session-manager:"$BUILD_DIR/session-manager"
  bin/login:"$LAUNCHER"
  bin/make:"$BUILD_DIR/make"
  bin/bash:"$BUILD_DIR/bash"
  bin/sh:"$BUILD_DIR/bash"
  bin/sed:"$BUILD_DIR/sed"
  bin/grep:"$BUILD_DIR/grep"
  bin/tar:"$BUILD_DIR/tar"
  bin/gzip:"$BUILD_DIR/gzip"
  bin/proj.tar.gz:"$OUT_DIR/proj.tar.gz"
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so"
  lib/libinstant.so:"$BUILD_DIR/libinstant.so"
  # GCC-built mlibc runtime (serves gcc AND the clang-built GNU tools).
  lib/mlibc/ld-instantos.so:"$MLIBC_GCC/lib/ld-instantos.so"
  lib/mlibc/libc.so:"$MLIBC_GCC/lib/libc.so"
  lib/mlibc/libdl.so:"$MLIBC_GCC/lib/libdl.so"
  lib/mlibc/libstdc++.so.6:"$CROSS/x86_64-unknown-instantos/lib/libstdc++.so.6"
  lib/mlibc/libgcc_s.so.1:"$CROSS/x86_64-unknown-instantos/lib/libgcc_s.so.1"
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

echo "GCCSELF_ISO=$ISO"
echo "GCCSELF_DISK=$BUILD_DIR/gcchost/ext4.img"
echo "initrd entries: ${#ENTRIES[@]}"
