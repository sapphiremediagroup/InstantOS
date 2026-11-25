#!/usr/bin/env bash
# P6.4/P6.5/P6.6: build the GCCHOST oracle image.
#  - ext4 root disk seeded (mke2fs -d) with the hosted-gcc world (/usr, /hello.*).
#  - initrd: managers + gcchost launcher (/bin/login) + libinstant loader +
#    the mlibc/gcc runtime .so's at /lib/mlibc (loader search path).
# Boot headless with a LARGE -Mem (cc1 ~200 MB RSS) and grep serial for GCCHOST_*.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/gcchost}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
GP="$ROOT/tools/gcc-port"
GCCHOST_ROOT="${GCCHOST_ROOT:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcchost-root}"
CROSS="${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-install"
MLIBC_GCC="$BUILD_DIR/mlibc-root-gcc"
# P6.7 tcc self-host world (libinstant): clang-built tcc-sysroot + tcc 0.9.27
# sources. The in-OS gcc rebuilds tcc from these, then that tcc self-hosts.
TCC_SYSROOT="$BUILD_DIR/tcc-sysroot"
TCC_SRC="$ROOT/outside/iUserApps/outside/tinycc"
ILIBCXX_INC="$ROOT/outside/iUserApps/outside/ilibcxx/include"
SELFHOST_DIR="$ROOT/tools/tcc-selfhost"

[ -d "$GCCHOST_ROOT/usr/bin" ] || { echo "gcchost root missing; run assemble-gcchost-root.sh" >&2; exit 2; }
mkdir -p "$OUT_DIR"

if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj \
        input-manager storage-manager process-manager network-manager font-manager session-manager
fi

# gcchost launcher -> /bin/login (freestanding libinstant program).
LAUNCHER="$OUT_DIR/gcchost"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -O1 -c "$GP/gcchost.c" -o "$OUT_DIR/gcchost.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$LAUNCHER" "$OUT_DIR/gcchost.o"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/gcchost.iso"
rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"

ENTRIES=(
  bin/input-manager:"$BUILD_DIR/input-manager"
  bin/storage-manager:"$BUILD_DIR/storage-manager"
  bin/process-manager:"$BUILD_DIR/process-manager"
  bin/font-manager:"$BUILD_DIR/font-manager"
  bin/session-manager:"$BUILD_DIR/session-manager"
  bin/login:"$LAUNCHER"
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so"
  lib/libinstant.so:"$BUILD_DIR/libinstant.so"
  # mlibc + gcc runtime for the hosted compiler binaries (loader default path).
  lib/mlibc/ld-instantos.so:"$MLIBC_GCC/lib/ld-instantos.so"
  lib/mlibc/libc.so:"$MLIBC_GCC/lib/libc.so"
  lib/mlibc/libdl.so:"$MLIBC_GCC/lib/libdl.so"
  lib/mlibc/libstdc++.so.6:"$CROSS/x86_64-unknown-instantos/lib/libstdc++.so.6"
  lib/mlibc/libgcc_s.so.1:"$CROSS/x86_64-unknown-instantos/lib/libgcc_s.so.1"
)

# P6.7 tcc self-host world (libinstant CRT/sysroot + tcc sources) so the in-OS
# gcc can rebuild tcc from source and that tcc can self-host. /lib and /include
# are initrd mounts; the gcc/mlibc world on the ext4 (/usr) is untouched. The
# base ENTRIES above already ship lib/ld-instantos.so + lib/libinstant.so.
ENTRIES+=(
  lib/libc.so:"$BUILD_DIR/libinstant.so"
  lib/crt0.o:"$TCC_SYSROOT/lib/crt0.o"
  lib/crt1.o:"$TCC_SYSROOT/lib/crt1.o"
  lib/Scrt1.o:"$TCC_SYSROOT/lib/Scrt1.o"
  lib/crti.o:"$TCC_SYSROOT/lib/crti.o"
  lib/crtn.o:"$TCC_SYSROOT/lib/crtn.o"
  lib/tcc/libtcc1.a:"$TCC_SYSROOT/lib/tcc/libtcc1.a"
)
# libc/POSIX headers (ilibcxx) -> /include ; tcc builtin headers -> /lib/tcc/include
while IFS= read -r -d '' h; do
  rel="${h#"$ILIBCXX_INC"/}"
  ENTRIES+=("include/$rel:$h")
done < <(find "$ILIBCXX_INC" -type f -name '*.h' -print0)
for h in "$TCC_SRC/include"/*.h; do
  [ -f "$h" ] || continue
  ENTRIES+=("lib/tcc/include/$(basename "$h"):$h")
done
# tcc 0.9.27 sources to (re)compile -> /lib/tcc/src, plus the InstantOS config.h.
for f in tcc.c libtcc.c tccpp.c tccgen.c tccelf.c tccasm.c tccrun.c tcctools.c \
         x86_64-gen.c x86_64-link.c i386-asm.c \
         coff.h elf.h i386-asm.h i386-tok.h il-opcodes.h libtcc.h stab.def \
         stab.h tcc.h tcclib.h tcctok.h x86_64-asm.h; do
  [ -f "$TCC_SRC/$f" ] || { echo "missing tcc source: $f" >&2; exit 2; }
  ENTRIES+=("lib/tcc/src/$f:$TCC_SRC/$f")
done
ENTRIES+=("lib/tcc/src/config.h:$SELFHOST_DIR/config.instantos.h")

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

# ext4 root disk seeded with the hosted-gcc world (~900 MB -> 2 GiB disk).
# SKIP_DISK=1 reuses an existing seeded disk (the seed is slow/IO-heavy).
DISK="$OUT_DIR/ext4.img"
if [ "${SKIP_DISK:-0}" != "1" ] || [ ! -f "$DISK" ]; then
  rm -f "$DISK"; truncate -s 2G "$DISK"
  printf 'label: dos\nstart=2048, type=83\n' | sfdisk "$DISK" >/dev/null 2>&1
  mke2fs -q -F -b 4096 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index" \
    -E offset=1048576,lazy_itable_init=0 -d "$GCCHOST_ROOT" "$DISK" 500000
fi

echo "GCCHOST_ISO=$ISO"
echo "GCCHOST_DISK=$DISK"
echo "initrd entries: ${#ENTRIES[@]}"
