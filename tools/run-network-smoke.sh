#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
LOG="${LOG:-$BUILD_DIR/network-smoke.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"

# cmake --build "$BUILD_DIR" --target iso

"$QEMU" \
  -m 512M \
  -cdrom "$BUILD_DIR/iboot.iso" \
  -serial "file:$LOG" \
  -display none \
  -no-reboot \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0 \
  ${QEMU_EXTRA_ARGS:-}

printf 'network smoke serial log: %s\n' "$LOG"
