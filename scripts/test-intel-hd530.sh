#!/usr/bin/env bash
# Intel HD Graphics 530 / Gen9 test harness.
#
# Runs two layers of verification:
#   1. Host unit tests  - compile the REAL kernel driver source against host
#      stubs and exercise PCI matching, BAR decode, plane interpretation, and
#      the detect/initialize/MMIO-map path. No QEMU required. (Always runs.)
#   2. QEMU boot smoke   - boot InstantOS headless and assert the [i915] probe
#      runs cleanly without crashing the kernel. Under plain QEMU (no Intel
#      iGPU) the expected result is "[i915] Intel Gen9 GPU: not present" with a
#      successful fall-through to virtio-gpu. On real Gen9 hardware the markers
#      report the detected HD 530.
#
# Usage: ./scripts/test-intel-hd530.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-40s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/intel-hd530-smoke}"
LOG="$OUT_DIR/serial.log"

echo "==> [1/2] Intel Gen9 host unit tests"
"$ROOT/testing/intel-gen9/run-tests.sh"

echo
echo "==> [2/2] QEMU boot smoke (probe path must run cleanly, no crash)"
mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target iso --parallel 4 >/dev/null

if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo "SKIP: $QEMU not found; host unit tests already passed." >&2
  exit 0
fi
if [ ! -f "$OVMF_CODE" ]; then
  echo "SKIP: OVMF firmware not found at $OVMF_CODE; host unit tests already passed." >&2
  exit 0
fi

VARS="$OUT_DIR/OVMF_VARS.fd"
cp "$OVMF_VARS_TEMPLATE" "$VARS"
rm -f "$LOG"

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
  -no-shutdown -no-reboot
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
  echo "FAIL: QEMU exited with status $status" >&2
  exit 1
fi

if [ ! -f "$LOG" ]; then
  echo "FAIL: no serial log captured" >&2
  exit 1
fi

# A kernel crash anywhere in the boot is a hard failure.
if grep -Eq "KERNEL PANIC|Unknown Instruction|User process crash|page fault" "$LOG"; then
  echo "FAIL: kernel crash detected during boot:" >&2
  grep -E "KERNEL PANIC|Unknown Instruction|User process crash|page fault" "$LOG" | head -5 >&2
  exit 1
fi

# The Intel Gen9 probe marker must be present (proves the path executed).
if ! grep -q "\[i915\] Intel Gen9 GPU" "$LOG"; then
  echo "FAIL: [i915] probe marker not found in serial log" >&2
  exit 1
fi

echo "---- [i915] serial markers ----"
grep "\[i915\]" "$LOG" || true
echo "-------------------------------"

if grep -q "\[i915\] Intel Gen9 GPU detected" "$LOG"; then
  echo "PASS: Intel Gen9 GPU detected and brought up."
else
  echo "PASS: probe ran cleanly; no Intel iGPU under emulation (fell back to virtio-gpu)."
fi
