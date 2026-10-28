#!/usr/bin/env bash
# Build a headless InstantOS ISO whose /bin/login is the feature test runner
# (tools/os-testsuite/runner.c). Boot it under QEMU and grep the serial log for
# the "[ostest] SCORE ..." line. Also builds an ext4 root disk (so the ext4
# driver + kernel ext4 selftest run) — attach it as an AHCI disk when booting.
#
# This script only BUILDS (this host runs QEMU on Windows separately).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/os-testsuite}"
SRC_DIR="$ROOT/tools/os-testsuite"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
# Compiler target selector: clang needs --target=..., the GCC cross does not
# (set CC_TARGET_FLAG="" for GCC). Overridable but defaults to the clang value.
CC_TARGET_FLAG="${CC_TARGET_FLAG---target=x86_64-unknown-elf}"

mkdir -p "$OUT_DIR"

# Kernel + loader + managers + initrd packer. Skippable (SKIP_CMAKE=1) when only
# runner.c changed and the kernel/managers are already built (repack is fast;
# a full cmake dependency rescan on the /mnt/c 9p mount is slow).
if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj ld-instantos \
        input-manager storage-manager process-manager network-manager font-manager session-manager
fi

compile_freestanding() { # <src> <out>
  local src="$1" out="$2"
  # RUNNER_EXTRA_CFLAGS lets callers tune the build, e.g. -DSOAK_MB=1024 for the
  # dedicated large-RAM soak boot.
  "$CC" $CC_TARGET_FLAG -ffreestanding -fPIE -fno-stack-protector \
    -nostdinc -O1 ${RUNNER_EXTRA_CFLAGS:-} -c "$src" -o "$out.o"
  "$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
    -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$out" "$out.o"
}
# RUNNER_SRC swaps in another /bin/login test program (tools/run-net-suite.sh).
compile_freestanding "${RUNNER_SRC:-$SRC_DIR/runner.c}" "$OUT_DIR/runner"
compile_freestanding "$SRC_DIR/helper.c" "$OUT_DIR/ostest-helper"
# PROBE=1 swaps in a tiny thread_create diagnostic as /bin/login.
if [ "${PROBE:-0}" = "1" ]; then compile_freestanding "$SRC_DIR/threadprobe.c" "$OUT_DIR/runner"; echo "PROBE: using threadprobe as /bin/login"; fi
# KILLRACE=1 swaps in the kill-storm scheduler-race reproducer as /bin/login.
if [ "${KILLRACE:-0}" = "1" ]; then compile_freestanding "$SRC_DIR/killrace.c" "$OUT_DIR/runner"; echo "KILLRACE: using killrace as /bin/login"; fi
# FSCRASH=1 swaps in the sustained ext4 churn driver (P1.4 power-cut test).
if [ "${FSCRASH:-0}" = "1" ]; then compile_freestanding "$SRC_DIR/fscrash.c" "$OUT_DIR/runner"; echo "FSCRASH: using fscrash as /bin/login"; fi

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/os-testsuite.iso"
rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"

# Runtime substrate (managers + ld-instantos.so + libinstant.so). Defaults to the
# clang cmake build; the GCC userland build (P5.5 Stage 4b) overrides SUBSTRATE_DIR
# (managers + ld-instantos.so) and LIBINSTANT_SO.
SUBSTRATE_DIR="${SUBSTRATE_DIR:-$BUILD_DIR}"
LIBINSTANT_SO="${LIBINSTANT_SO:-$BUILD_DIR/libinstant.so}"
TTF="$ROOT/assets/fonts/JetBrainsMono-Regular.ttf"
ENTRIES=(
  bin/login:"$OUT_DIR/runner"
  bin/ostest-helper:"$OUT_DIR/ostest-helper"
  bin/input-manager:"$SUBSTRATE_DIR/input-manager"
  bin/storage-manager:"$SUBSTRATE_DIR/storage-manager"
  bin/process-manager:"$SUBSTRATE_DIR/process-manager"
  bin/network-manager:"$SUBSTRATE_DIR/network-manager"
  bin/font-manager:"$SUBSTRATE_DIR/font-manager"
  bin/session-manager:"$SUBSTRATE_DIR/session-manager"
  lib/ld-instantos.so:"$SUBSTRATE_DIR/ld-instantos.so"
  lib/libinstant.so:"$LIBINSTANT_SO"
)
[ -f "$TTF" ] && ENTRIES+=("bin/JetBrainsMono-Regular.ttf:$TTF")
# EXTRA_INITRD: more space-separated initrd/path:host/path entries.
for entry in ${EXTRA_INITRD:-}; do ENTRIES+=("$entry"); done

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

# ext4 root disk (simple journal, writable), seeded with /readme.txt — mounts at
# / and triggers the in-kernel ext4 selftest, and lets the runner test / writes.
DISK="$OUT_DIR/ext4.img"
if [ "${SKIP_DISK:-0}" != "1" ] || [ ! -f "$DISK" ]; then
  rm -f "$DISK"; truncate -s 64M "$DISK"
  printf "label: dos\nstart=2048, type=83\n" | sfdisk "$DISK" >/dev/null 2>&1
  SEED="$(mktemp -d)"; printf 'InstantOS ext4 in-OS test seed\n' > "$SEED/readme.txt"
  mke2fs -q -F -b 4096 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index" \
    -E offset=1048576,lazy_itable_init=0 -d "$SEED" "$DISK" 15000
fi

echo "OSTEST_ISO=$ISO"
echo "OSTEST_DISK=$DISK"
echo "initrd entries: ${#ENTRIES[@]}"
