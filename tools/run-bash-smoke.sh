#!/usr/bin/env bash
# Headless end-to-end test for the GNU bash + mlibc port.
#
# Boots InstantOS in QEMU with a tiny launcher standing in for /bin/login. The
# launcher spawns /bin/bash -c '<script>' and reports markers over serial, so
# the whole bash + mlibc runtime is exercised without driving the GUI.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/bash-smoke}"
LOG="${LOG:-$OUT_DIR/bash-smoke.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-90s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

MLIBC_ROOT="$BUILD_DIR/mlibc-root"
BASH_BIN="$BUILD_DIR/bash"

if [ ! -f "$BASH_BIN" ]; then
  printf 'bash binary missing (%s); run tools/build-bash.sh first\n' "$BASH_BIN" >&2
  exit 2
fi
if [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc runtime missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj

# Build the freestanding launcher (no libc, raw syscalls), as a PIE ELF using
# the default InstantOS dynamic linker.
LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -c "$ROOT/tools/bash-smoke/launcher.c" -o "$OUT_DIR/launcher.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so \
  -o "$LAUNCHER" "$OUT_DIR/launcher.o"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"
EFI_DIR="$ISO_ROOT/EFI/BOOT"
EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/bash-smoke.iso"
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
  bin/login:"$LAUNCHER" \
  bin/bash:"$BASH_BIN" \
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so" \
  lib/libinstant.so:"$BUILD_DIR/libinstant.so" \
  lib/mlibc/ld-instantos.so:"$MLIBC_ROOT/lib/ld-instantos.so" \
  lib/mlibc/libc.so:"$MLIBC_ROOT/lib/libc.so" \
  lib/mlibc/libdl.so:"$MLIBC_ROOT/lib/libdl.so"

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
  printf 'bash smoke qemu failed with status=%s\n' "$status" >&2
  exit "$status"
fi

if grep -q 'Unknown Instruction' "$LOG" || grep -q 'User process crash' "$LOG"; then
  printf 'bash smoke: detected a user process crash in %s\n' "$LOG" >&2
  grep -nE 'Unknown Instruction|User process crash|exception:|rip:' "$LOG" >&2 || true
  exit 1
fi

# Success criteria (verified end-to-end):
#  1. bash's mlibc runtime starts (reaches its entry point).
#  2. bash runs the -c script and exits cleanly (status 0), reaped by waitpid.
#  3. bash's stdout actually reaches the console/serial.
#  4. no user-process crash occurred (checked above).
if ! grep -q 'mlibc-entry: before-main' "$LOG"; then
  printf 'bash smoke: bash mlibc runtime did not reach main in %s\n' "$LOG" >&2
  exit 1
fi
if ! grep -qE 'bash-smoke: waited rc=0x[0-9a-f]+ status=0x0+$' "$LOG"; then
  printf 'bash smoke: bash did not exit cleanly (status 0) in %s\n' "$LOG" >&2
  grep -nE 'bash-smoke:' "$LOG" >&2 || true
  exit 1
fi
if ! grep -q 'BASH_SMOKE_OK' "$LOG"; then
  printf 'bash smoke: bash stdout did not reach the console in %s\n' "$LOG" >&2
  grep -nE 'fwrite|bash-smoke:' "$LOG" >&2 || true
  exit 1
fi
if ! grep -q 'arith=42' "$LOG"; then
  printf 'bash smoke: arithmetic output missing/incorrect in %s\n' "$LOG" >&2
  exit 1
fi

printf 'bash smoke passed: %s\n' "$LOG"
grep -nE 'bash-smoke:|BASH_SMOKE_OK|arith=|shell=|mlibc-entry' "$LOG" || true
