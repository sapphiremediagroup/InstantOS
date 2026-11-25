#!/usr/bin/env bash
# run-image.sh for Windows (Git Bash): boots ISO + a fresh copy of DISK under
# QEMU for Windows with WHPX, serial to LOG, until MARKER shows up (or a panic,
# or TIMEOUT seconds). WHPX needs kernel-irqchip=off; virtio-gpu init hangs
# under WHPX, so the display is -vga std.
#   tools/gcc-selfhost/run-image-win.sh ISO DISK LOG MARKER [TIMEOUT]
# QEMU_MEM (default 3G), QEMU_SMP (default all CPUs), QEMU_DIR, QEMU_EXTRA (more
# QEMU arguments, e.g. a NIC). The VM opens a window (the kernel console on the
# firmware's framebuffer; QEMU_DISPLAY=gtk, or sdl, or none for headless runs);
# closing it ends the run like a crash would. SKIP_COPY=1 boots
# the existing *-run.img as it is (resuming a build left on it). Paths must be
# relative or Windows-style: QEMU for Windows cannot open /d/... serial paths.
# The run also ends at STOP_MARKER (default GCCSELF_DONE: the launcher has
# finished, pass or fail), so a failed build does not idle until TIMEOUT.
set -uo pipefail
ISO="$1"; DISK="$2"; LOG="$3"; DONE="$4"; TMO="${5:-3600}"
QEMU_DIR="${QEMU_DIR:-/c/Program Files/qemu}"
QEMU_BIN="$QEMU_DIR/qemu-system-x86_64.exe"
OVMF_CODE="$QEMU_DIR/share/edk2-x86_64-code.fd"
OVMF_VARS_SRC="$QEMU_DIR/share/edk2-i386-vars.fd"
[ -x "$QEMU_BIN" ] || { echo "no $QEMU_BIN (set QEMU_DIR)" >&2; exit 2; }
RUNDISK="${DISK%.img}-run.img"
VARS="$(dirname "$LOG")/OVMF_VARS_run.fd"
rm -f "$LOG"   # first: copying the disk takes a minute
[ "${SKIP_COPY:-0}" = 1 ] && [ -f "$RUNDISK" ] || cp "$DISK" "$RUNDISK"
cp "$OVMF_VARS_SRC" "$VARS"
"$QEMU_BIN" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$ISO",media=cdrom \
  -name "InstantOS $(basename "$LOG" .log)" \
  -display "${QEMU_DISPLAY:-gtk}" -vga std -accel whpx,kernel-irqchip=off \
  -device ich9-ahci,id=ahci \
  -drive id=d0,file="$RUNDISK",if=none,format=raw -device ide-hd,drive=d0,bus=ahci.0 \
  -serial file:"$LOG" -monitor none \
  -m "${QEMU_MEM:-3G}" -smp "${QEMU_SMP:-$NUMBER_OF_PROCESSORS}" -no-reboot ${QEMU_EXTRA:-} &
QPID=$!
sleep 1; WPID=$(cat /proc/$QPID/winpid 2>/dev/null)
start=$(date +%s)
status=timeout
while kill -0 $QPID 2>/dev/null; do
  if grep -aq "$DONE" "$LOG" 2>/dev/null; then status=done; sleep 3; break; fi
  if [ -n "${STOP_MARKER-GCCSELF_DONE}" ] && grep -aq "${STOP_MARKER-GCCSELF_DONE}" "$LOG" 2>/dev/null; then status=finished; sleep 3; break; fi
  if grep -aq "KERNEL PANIC" "$LOG" 2>/dev/null; then status=panic; sleep 3; break; fi
  [ $(( $(date +%s) - start )) -ge "$TMO" ] && break
  sleep 10
done
# QEMU gone before a marker or the timeout: it exited (its window was closed,
# or it failed to start).
if [ "$status" = timeout ] && ! kill -0 $QPID 2>/dev/null && [ $(( $(date +%s) - start )) -lt "$TMO" ]; then
  status=exited
fi
kill $QPID 2>/dev/null
# A native Windows QEMU may outlive the MSYS kill; it holds the run disk open.
[ -n "$WPID" ] && taskkill //PID "$WPID" //F >/dev/null 2>&1
wait $QPID 2>/dev/null
echo "status=$status elapsed=$(( $(date +%s) - start ))s log=$LOG"
grep -a -o "GCC[A-Z]*_[A-Z0-9_]*=\?-\?[0-9x]*" "$LOG" | uniq
{ grep -a -c "User process crash" "$LOG" || true; } | sed "s/^/user process crashes: /"
[ "$status" = done ]
