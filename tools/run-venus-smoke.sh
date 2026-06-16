#!/usr/bin/env bash
# Venus (Vulkan over virtio-gpu) smoke test.
#
# Boots InstantOS under QEMU with a Venus-capable virtio-gpu device and greps
# the serial log for the kernel's [VENUS] boot self-test markers.
#
# Requirements on the host:
#   * QEMU built with virtio-gpu-gl + Venus support (virtio-gpu-gl-pci with a
#     "venus" property),
#   * libvirglrenderer with VK_MESA_venus_protocol,
#   * a working host Vulkan ICD (e.g. radv, or llvmpipe via lavapipe).
#
# If the host lacks Venus support the device falls back and the kernel prints
# "[VENUS] capset: unavailable"; this script reports that as SKIP, not failure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/venus-smoke}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-40s}"
# Venus exercises the host GL/Vulkan stack; KVM keeps boot fast. Override with
# QEMU_ACCEL=tcg on hosts without /dev/kvm.
QEMU_ACCEL="${QEMU_ACCEL:-kvm}"
# egl-headless gives the host a GL context without needing a window/X server.
QEMU_DISPLAY="${QEMU_DISPLAY:-egl-headless,gl=on}"
HOSTMEM="${HOSTMEM:-256M}"
# Force a software Vulkan driver by default so the test is deterministic on CI
# machines without a discrete GPU. Override VK_ICD_FILENAMES to use the GPU.
LAVAPIPE_ICD="${LAVAPIPE_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target iso

# Verify the QEMU build actually supports the Venus property; otherwise the run
# would error out on an unknown device option.
if ! "$QEMU" -device virtio-gpu-gl-pci,help 2>&1 | grep -qi 'venus'; then
  printf 'venus smoke: this QEMU lacks virtio-gpu-gl venus support; SKIP\n' >&2
  exit 0
fi

VARS="$OUT_DIR/OVMF_VARS.fd"
LOG="$OUT_DIR/serial.log"
cp "$OVMF_VARS_TEMPLATE" "$VARS"
rm -f "$LOG"

# Prefer lavapipe (software) for reproducibility if present and the caller did
# not pin an ICD.
ICD_ENV=()
if [ -z "${VK_ICD_FILENAMES:-}" ] && [ -f "$LAVAPIPE_ICD" ]; then
  ICD_ENV=(env "VK_ICD_FILENAMES=$LAVAPIPE_ICD")
fi

printf 'venus smoke: accel=%s display=%s hostmem=%s log=%s\n' \
  "$QEMU_ACCEL" "$QEMU_DISPLAY" "$HOSTMEM" "$LOG"

set +e
timeout "$QEMU_TIMEOUT" "${ICD_ENV[@]}" "$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$VARS" \
  -drive "file=$BUILD_DIR/iboot.iso,media=cdrom" \
  -display "$QEMU_DISPLAY" \
  -device "virtio-gpu-gl-pci,blob=on,venus=on,hostmem=$HOSTMEM" \
  -accel "$QEMU_ACCEL" \
  -cpu max,+tsc \
  -chardev "file,id=serial0,path=$LOG" \
  -serial chardev:serial0 \
  -monitor none \
  -m 1G \
  -smp 2 \
  -no-shutdown \
  -no-reboot \
  -d guest_errors \
  -D "$OUT_DIR/qemu.log" \
  ${QEMU_EXTRA_ARGS:-}
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
  printf 'venus smoke: qemu exited with status=%s\n' "$status" >&2
  exit "$status"
fi

if [ ! -s "$LOG" ]; then
  printf 'venus smoke: no serial output captured\n' >&2
  exit 1
fi

echo '--- [VENUS] serial markers ---'
grep -aE '\[VENUS\]' "$LOG" || true
echo '------------------------------'

if grep -aqE '\[VENUS\] capset: unavailable' "$LOG"; then
  printf 'venus smoke: host renderer did not enter venus mode; SKIP\n' >&2
  exit 0
fi

# Hard failure markers.
if grep -aqE 'User process crash|Unknown Instruction|KERNEL PANIC|page fault' "$LOG"; then
  printf 'venus smoke: FAIL (crash detected)\n' >&2
  exit 1
fi

# Success requires the probe round trip, the Vulkan instance/device bring-up,
# and the end-to-end compute dispatch (GPU writes verified by CPU readback).
if grep -aqE '\[VENUS\] probe: ok' "$LOG" &&
   grep -aqE '\[VENUS\] stages: .*reply=ok' "$LOG" &&
   grep -aqE '\[VENUS\] vk stages: ring=ok instance=ok phys_devs=[1-9].*device=ok compute=ok' "$LOG" &&
   grep -aqE '\[VENUS\] compute: elements=[0-9]+ mismatches=0' "$LOG"; then
  printf 'venus smoke: PASS (Venus probe + Vulkan bring-up + compute dispatch + readback verified)\n'
  exit 0
fi

printf 'venus smoke: FAIL (probe/vulkan/compute did not complete; see %s)\n' "$LOG" >&2
exit 1
