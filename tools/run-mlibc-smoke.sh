#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/mlibc-smoke}"
LOG="${LOG:-$OUT_DIR/mlibc-smoke.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-2m}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj
"$ROOT/tools/build-mlibc.sh"
"$ROOT/tools/build-mlibc-hello.sh"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"
EFI_DIR="$ISO_ROOT/EFI/BOOT"
EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/mlibc-smoke.iso"
OVMF_VARS="$OUT_DIR/OVMF_VARS.fd"

rm -rf "$ISO_ROOT"
mkdir -p "$EFI_DIR"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" \
  bin/input-manager:"$BUILD_DIR/input-manager" \
  bin/storage-manager:"$BUILD_DIR/storage-manager" \
  bin/process-manager:"$BUILD_DIR/process-manager" \
  bin/font-manager:"$BUILD_DIR/font-manager" \
  bin/session-manager:"$BUILD_DIR/session-manager" \
  bin/login:"$BUILD_DIR/mlibc-hello" \
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so" \
  lib/libinstant.so:"$BUILD_DIR/libinstant.so" \
  lib/libc.so:"$BUILD_DIR/mlibc-root/lib/libc.so" \
  lib/mlibc/ld-instantos.so:"$BUILD_DIR/mlibc-root/lib/ld-instantos.so" \
  lib/mlibc/libc.so:"$BUILD_DIR/mlibc-root/lib/libc.so"

cp "$BUILD_DIR/BOOTX64.EFI" "$EFI_DIR/BOOTX64.EFI"
cp "$BUILD_DIR/INSTANTOS.EFI" "$EFI_DIR/INSTANTOS.EFI"
cp "$INITRD" "$EFI_DIR/INITRD"

dd if=/dev/zero of="$EFI_IMG" bs=1M count=32 status=none
mformat -i "$EFI_IMG" ::
mmd -i "$EFI_IMG" ::/EFI
mmd -i "$EFI_IMG" ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_IMG" "$BUILD_DIR/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$EFI_IMG" "$INITRD" ::/EFI/BOOT/INITRD

xorriso -as mkisofs \
  -R -J -joliet-long \
  -iso-level 3 \
  -eltorito-alt-boot \
  -e EFI/BOOT/efiboot.img \
  -no-emul-boot \
  -o "$ISO" \
  "$ISO_ROOT" >/dev/null

rm -f "$LOG"
set +e
timeout "$QEMU_TIMEOUT" "$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$OVMF_VARS" \
  -drive "file=$ISO,media=cdrom" \
  -accel "$QEMU_ACCEL" \
  -cpu max,+tsc \
  -m 1G \
  -smp 2 \
  -serial "file:$LOG" \
  -no-shutdown \
  -no-reboot \
  ${QEMU_EXTRA_ARGS:-}
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
  printf 'mlibc smoke qemu failed with status=%s\n' "$status" >&2
  exit "$status"
fi

if ! grep -q 'mlibc-hello: serial-done' "$LOG"; then
  printf 'mlibc smoke did not find completion marker in %s\n' "$LOG" >&2
  exit 1
fi

printf 'mlibc smoke passed: %s\n' "$LOG"
