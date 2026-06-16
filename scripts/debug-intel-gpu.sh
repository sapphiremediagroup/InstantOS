#!/usr/bin/env bash
# Intel GPU debugging helper. Boots InstantOS headless, captures the serial log,
# and extracts everything relevant to diagnosing the Intel HD 530 / Gen9 path:
# PCI probe, BAR mapping, display mode recovery, and any crash markers.
#
# Use this when chasing a black screen, a failed PCI probe, a bad BAR mapping,
# or a GPU hang. It prints a focused digest and points at the full log.
#
# Usage: ./scripts/debug-intel-gpu.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-40s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/intel-gpu-debug}"
LOG="$OUT_DIR/serial.log"
QLOG="$OUT_DIR/qemu.log"

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target iso --parallel 4 >/dev/null

VARS="$OUT_DIR/OVMF_VARS.fd"
cp "$OVMF_VARS_TEMPLATE" "$VARS"
rm -f "$LOG" "$QLOG"

echo "==> Booting InstantOS (headless, ${QEMU_TIMEOUT} budget)..."
set +e
timeout "$QEMU_TIMEOUT" "$QEMU" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$BUILD_DIR/iboot.iso",media=cdrom \
  -display none \
  -device virtio-gpu-pci \
  -accel "$QEMU_ACCEL" \
  -cpu max,+tsc \
  -chardev file,id=serial0,path="$LOG" \
  -serial chardev:serial0 \
  -monitor none \
  -m 1G -smp 2 \
  -no-shutdown -no-reboot \
  -d int,cpu_reset,guest_errors -D "$QLOG" "$@"
set -e

echo
echo "================ Intel / GPU bring-up digest ================"
if [ -f "$LOG" ]; then
  echo "--- [i915] Intel Gen9 markers ---"
  grep -n "\[i915\]" "$LOG" || echo "(none - probe marker missing!)"
  echo
  echo "--- framebuffer / GPU backend selection ---"
  grep -nE "\[BOOT\] (framebuffer|VirtIO GPU|Intel Gen9|active GPU)" "$LOG" || echo "(none)"
  echo
  echo "--- crash / fault markers ---"
  if grep -nE "KERNEL PANIC|Unknown Instruction|User process crash|page fault" "$LOG"; then
    echo ">> Crash markers present (see above)."
  else
    echo "(none - clean boot)"
  fi
else
  echo "No serial log captured at $LOG"
fi
echo "============================================================="
echo "Full serial log: $LOG"
echo "QEMU debug log:  $QLOG"
