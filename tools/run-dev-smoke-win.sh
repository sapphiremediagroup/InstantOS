#!/usr/bin/env bash
# The desktop system with the dev disk (tools/make-dev-disk.sh), checked: boots
# build/iboot.iso under WHPX (Git Bash, a QEMU window shows the desktop) with a
# copy of the dev disk that carries tools/dev-smoke/autorun.sh as /autorun.sh,
# which compiles and runs C, C++ and a make project with the in-OS gcc once
# the desktop is up. Passes on DEVSMOKE_C_OK, _CXX_OK and _MAKE_OK.
#
#   tools/run-dev-smoke-win.sh          (QEMU_DISPLAY=none for headless)
#
# The disk is made in WSL (debugfs, mke2fs); see make-dev-disk.sh for where the
# toolchain comes from.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/dev-smoke"
mkdir -p "$OUT"
[ -f "$ROOT/build/iboot.iso" ] || { echo "missing build/iboot.iso (cmake --build build --target iso)" >&2; exit 2; }

# SKIP_DISK=1 reuses build/dev-smoke/dev-smoke.img (the run boots a copy of it).
if [ "${SKIP_DISK:-0}" != 1 ] || [ ! -f "$OUT/dev-smoke.img" ]; then
  wsl -e bash -lc "cd /mnt/d/projects/InstantOS && AUTORUN=tools/dev-smoke/autorun.sh DISK_SIZE=${DISK_SIZE:-2G} \
    bash tools/make-dev-disk.sh build/dev-smoke/dev-smoke.img" || exit 1
fi
cp "$ROOT/build/iboot.iso" "$OUT/iboot.iso"

cd "$OUT"
QEMU_SMP="${QEMU_SMP:-2}" QEMU_MEM="${QEMU_MEM:-2G}" STOP_MARKER=DEVSMOKE_DONE \
QEMU_EXTRA="-device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0 -netdev user,id=net0" \
  bash "$ROOT/tools/gcc-selfhost/run-image-win.sh" iboot.iso dev-smoke.img serial.log DEVSMOKE_DONE \
    "${SMOKE_TIMEOUT:-900}" > runner.out 2>&1
grep -a "DEVSMOKE_\|\[login\] autorun" serial.log
grep -aq DEVSMOKE_C_OK serial.log && grep -aq DEVSMOKE_CXX_OK serial.log && grep -aq DEVSMOKE_MAKE_OK serial.log
