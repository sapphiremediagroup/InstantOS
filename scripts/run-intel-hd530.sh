#!/usr/bin/env bash
# Boot InstantOS in QEMU and surface the Intel HD Graphics 530 / Gen9 probe.
#
# IMPORTANT: QEMU does not emulate an Intel HD 530 (there is no virtual Intel
# iGPU). On this host the [i915] probe correctly reports "not present" and the
# GPU manager falls back to virtio-gpu, exactly as designed. This script is for
# (a) confirming the probe path runs cleanly under emulation, and (b) booting on
# REAL Intel Skylake/Gen9 hardware (or QEMU with PCI passthrough of a real iGPU),
# where the [i915] markers will report the detected HD 530 and inherited mode.
#
# Build first with: cmake --build build --target iso
# Then run:        ./scripts/run-intel-hd530.sh
#
# It is a thin wrapper around run.sh: virtio-gpu remains present as the fallback
# so the desktop still comes up under plain QEMU. To passthrough a real Intel
# iGPU, append vfio-pci args, e.g.:
#   ./scripts/run-intel-hd530.sh -device vfio-pci,host=00:02.0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ ! -f build/iboot.iso ]; then
  echo "build/iboot.iso not found; building the ISO first..." >&2
  cmake --build build --target iso --parallel 4
fi

# Default to a headless GL-less run; callers can override QEMU_DISPLAY/GPU_MODE.
export GPU_MODE="${GPU_MODE:-plain}"
exec ./run.sh "$@"
