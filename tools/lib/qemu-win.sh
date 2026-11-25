# Windows' QEMU with WHPX from WSL (QEMU_WIN=1): much faster than TCG, since
# WSL has no KVM. Sourced by the run-*.sh scripts.
#
#   winpath PATH            the Windows form of a WSL path (the file need not exist yet)
#   qemu_win_start PIDDIR ARGS...
#                           starts it in the background with the firmware, WHPX and
#                           -pidfile; $QEMU_WIN_PID is the WSL-side pid
#   qemu_win_stop PIDDIR    ends it (taskkill: killing the WSL side leaves it running)
#   qemu_win_wait LOG SECONDS REGEX
#                           waits until REGEX is in LOG, QEMU exits or the time is up;
#                           0 if it was seen
#   win_python              Windows' Python, for host-side helpers that talk to it
#                           over 127.0.0.1 (WSL in NAT mode cannot reach Windows'
#                           localhost, nor share UDP with it)
#   QEMU_SHOW=1             a window on the Windows desktop (-display gtk) instead of
#                           none, to watch a run; do not type or click in it while a
#                           suite drives the VM over QMP
QEMU_WIN_DIR="${QEMU_DIR:-/mnt/c/Program Files/qemu}"
if [ "${QEMU_SHOW:-0}" = 1 ]; then QEMU_DISPLAY_WIN=gtk; else QEMU_DISPLAY_WIN=none; fi

winpath() {
  printf '%s\\%s' "$(wslpath -w "$(dirname "$1")")" "$(basename "$1")"
}

qemu_win_start() {
  local piddir="$1"; shift
  mkdir -p "$piddir"
  cp "$QEMU_WIN_DIR/share/edk2-i386-vars.fd" "$piddir/vars.fd"
  chmod u+w "$piddir/vars.fd"
  rm -f "$piddir/qemu.pid"
  "$QEMU_WIN_DIR/qemu-system-x86_64.exe" \
    -drive "if=pflash,format=raw,readonly=on,file=$(winpath "$QEMU_WIN_DIR/share/edk2-x86_64-code.fd")" \
    -drive "if=pflash,format=raw,file=$(winpath "$piddir/vars.fd")" \
    -accel whpx,kernel-irqchip=off -pidfile "$(winpath "$piddir/qemu.pid")" "$@" &
  QEMU_WIN_PID=$!
}

qemu_win_stop() {
  local piddir="$1"
  [ -f "$piddir/qemu.pid" ] && taskkill.exe /PID "$(tr -dc 0-9 < "$piddir/qemu.pid")" /F >/dev/null 2>&1 || true
  [ -n "${QEMU_WIN_PID:-}" ] && kill "$QEMU_WIN_PID" 2>/dev/null || true
}

qemu_win_wait() {
  local log="$1" seconds="$2" regex="$3" start=$SECONDS
  while [ $((SECONDS - start)) -lt "$seconds" ]; do
    grep -aqE "$regex" "$log" 2>/dev/null && return 0
    kill -0 "$QEMU_WIN_PID" 2>/dev/null || break
    sleep 1
  done
  grep -aqE "$regex" "$log" 2>/dev/null
}

win_python() {
  python.exe "$@"
}
