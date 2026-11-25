#!/usr/bin/env bash
# Boots tools/boot/make-boot-disk.sh's disk under QEMU (OVMF, the disk on AHCI,
# as the PC's), headless, and waits for a marker in the serial log.
#
#   tools/boot/run-boot-disk.sh [MARKER] [TIMEOUT_S]
#   DISK=...        the disk (default build/boot-disk/disk.img; booted in place
#                   with KEEP=1, else from a scratch copy)
#   QEMU_DISPLAY    none (default) or gtk to watch it
#   QEMU_WIN=1      run Windows' QEMU (QEMU_DIR, default C:\Program Files\qemu)
#                   with WHPX from WSL, near native speed instead of TCG; the
#                   files must be on a Windows drive (/mnt/c, /mnt/d)
# Exit 0 when MARKER (a grep pattern) shows up, 1 on timeout, a crash marker or
# a kernel exception.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/boot-disk}"
DISK="${DISK:-$OUT_DIR/disk.img}"
MARKER="${1:-\[graphics.compositor\]}"
TIMEOUT_S="${2:-600}"
LOG="${LOG:-$OUT_DIR/serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"

[ -f "$DISK" ] || { echo "run-boot-disk: no $DISK (tools/boot/make-boot-disk.sh)" >&2; exit 2; }
RUN_DISK="$DISK"
if [ "${KEEP:-0}" != "1" ]; then
  RUN_DISK="$OUT_DIR/run.img"
  cp "$DISK" "$RUN_DISK"
fi
QEMU_DIR="${QEMU_DIR:-/mnt/c/Program Files/qemu}"
if [ "${QEMU_WIN:-0}" = 1 ]; then
  rm -f "$OUT_DIR/OVMF_VARS.fd"
  cp "$QEMU_DIR/share/edk2-i386-vars.fd" "$OUT_DIR/OVMF_VARS.fd"
  chmod u+w "$OUT_DIR/OVMF_VARS.fd"   # Program Files' copy is read-only
else
  cp "$OVMF_VARS_TEMPLATE" "$OUT_DIR/OVMF_VARS.fd"
fi
rm -f "$LOG" "$OUT_DIR/qemu.pid"

if [ "${QEMU_WIN:-0}" = 1 ]; then
  # WHPX: no kernel irqchip, std VGA (virtio-gpu hangs there), its own CPU
  # model (-cpu host makes OVMF fault). The Windows process is stopped by the
  # pid it writes: killing the WSL side leaves it running.
  # wslpath -w wants an existing path: convert the directory (the pid file and
  # the log do not exist yet).
  win() { printf '%s\\%s' "$(wslpath -w "$(dirname "$1")")" "$(basename "$1")"; }
  "$QEMU_DIR/qemu-system-x86_64.exe" \
    -drive "if=pflash,format=raw,readonly=on,file=$(win "$QEMU_DIR/share/edk2-x86_64-code.fd")" \
    -drive "if=pflash,format=raw,file=$(win "$OUT_DIR/OVMF_VARS.fd")" \
    -device ich9-ahci,id=ahci \
    -drive "id=disk0,file=$(win "$RUN_DISK"),if=none,format=raw" \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
    -nic user,model=virtio-net-pci \
    -accel whpx,kernel-irqchip=off -vga std -m 2G -smp "${QEMU_SMP:-2}" \
    -display "${QEMU_DISPLAY:-none}" -monitor none -pidfile "$(win "$OUT_DIR/qemu.pid")" \
    -serial "file:$(win "$LOG")" -no-reboot ${QEMU_EXTRA:-} &
else
  "$QEMU" \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$OUT_DIR/OVMF_VARS.fd" \
    -device ich9-ahci,id=ahci \
    -drive "id=disk0,file=$RUN_DISK,if=none,format=raw" \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
    -nic user,model=virtio-net-pci \
    -accel "$QEMU_ACCEL" -cpu max,+tsc -m 2G -smp "${QEMU_SMP:-2}" \
    -display "${QEMU_DISPLAY:-none}" -monitor none \
    -serial "file:$LOG" -no-reboot ${QEMU_EXTRA:-} &
fi
qemu=$!
stop_qemu() {
  if [ "${QEMU_WIN:-0}" = 1 ] && [ -f "$OUT_DIR/qemu.pid" ]; then
    taskkill.exe /PID "$(tr -dc 0-9 < "$OUT_DIR/qemu.pid")" /F >/dev/null 2>&1 || true
  fi
  kill "$qemu" 2>/dev/null || true
}
trap stop_qemu EXIT

result=timeout
for _ in $(seq 1 "$TIMEOUT_S"); do
  sleep 1
  kill -0 "$qemu" 2>/dev/null || { result=exited; break; }
  if grep -aq "Unknown Instruction\|User process crash\|\[exception\] kernel" "$LOG" 2>/dev/null; then
    result=crash; break
  fi
  if grep -aq -- "$MARKER" "$LOG" 2>/dev/null; then result=done; break; fi
done
echo "run-boot-disk: result=$result log=$LOG"
[ "$result" = done ]
