#!/usr/bin/env bash
# Watch the Venus GPU triangle in action.
#
# Boots InstantOS with a Venus-capable virtio-gpu-gl device and a *windowed*
# display so you can see the GPU-rendered RGB triangle that the kernel draws at
# boot (see runVenusProbe -> Venus::renderTriangleToScreen). The triangle is
# rendered by a real Vulkan graphics pipeline on the host driver, copied back to
# guest memory, and blitted to the scanout; it is held on screen for a few
# seconds before the boot continues.
#
# Requires: a GUI session (X11/Wayland), QEMU with virtio-gpu-gl + Venus, a host
# Vulkan ICD. By default uses the real host GPU; set VK_ICD_FILENAMES to force a
# specific driver (e.g. lavapipe for software rendering).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_ACCEL="${QEMU_ACCEL:-kvm}"
# A GL-capable windowed display so the 2D scanout (with the blitted triangle)
# is visible. gtk works on most desktops; try "sdl,gl=on" if gtk is unavailable.
QEMU_DISPLAY="${QEMU_DISPLAY:-gtk,gl=on}"
HOSTMEM="${HOSTMEM:-256M}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
VARS="$BUILD_DIR/venus-triangle.OVMF_VARS.fd"

cmake --build "$BUILD_DIR" --target iso
cp "$OVMF_VARS_TEMPLATE" "$VARS"

if ! "$QEMU" -device virtio-gpu-gl-pci,help 2>&1 | grep -qi 'venus'; then
  echo "this QEMU lacks virtio-gpu-gl venus support" >&2
  exit 2
fi

echo "Watch the window: a colored triangle is drawn by the GPU at boot and held"
echo "for a few seconds (look for '[VENUS] triangle on screen: ok' on the console)."

exec "$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$VARS" \
  -drive "file=$BUILD_DIR/iboot.iso,media=cdrom" \
  -display "$QEMU_DISPLAY" \
  -device "virtio-gpu-gl-pci,blob=on,venus=on,hostmem=$HOSTMEM" \
  -accel "$QEMU_ACCEL" \
  -cpu max,+tsc \
  -serial stdio \
  -monitor none \
  -m 1G \
  -smp 2 \
  -no-shutdown -no-reboot \
  "$@"
