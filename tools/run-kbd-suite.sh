#!/usr/bin/env bash
# Keyboard suite: boots QEMU with each keyboard (KBD_MODES, default
# "ps2 usb ohci": the i8042 alone, usb-kbd on xHCI, usb-kbd on OHCI) and
# presses keys over QMP like a user (tools/kbd-suite/drive.py):
#   - a probe image whose /bin/login (tools/kbd-suite/console_probe.c) prints
#     each console read: the bytes of every kind of key, whole repeats;
#   - the system image (build/iboot.iso): bash line editing in the terminal,
#     auto-repeat, the keypad, Caps/Num Lock, checked through the curl
#     requests the edited lines make to tools/net-suite/host_server.py;
#   - the lock lights, when QEMU (QEMU=..., default ~/qemu-rtl's from
#     tools/qemu-rtl8168/build-qemu.sh if it is built) has the hid_kbd_set_leds
#     trace event.
# The host-side tables are tested by tools/run-keymap-fixtures.sh.
#
#   tools/run-kbd-suite.sh                  # all keyboards, both images
#   KBD_MODES=ps2 KBD_PARTS=console tools/run-kbd-suite.sh
#   SKIP_CMAKE=1 ...                         # reuse the built system image
#
# Exits 0 only on "[kbdsuite] SCORE ... fail=0".
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/kbd-suite}"
KBD_MODES="${KBD_MODES:-ps2 usb ohci}"
KBD_PARTS="${KBD_PARTS:-console terminal}"
HTTP_PORT="${HTTP_PORT:-18380}"
QEMU_SMP="${QEMU_SMP:-2}"
export PATH="/usr/lib/llvm-20/bin:$PATH"

# QEMU_WIN=1: Windows' QEMU with WHPX; the Python helpers then run on Windows'
# Python too (WSL in NAT mode cannot reach Windows' 127.0.0.1, where QMP is).
PY=(python3)
WIN_ARGS=()
p() { printf '%s' "$1"; }
if [ "${QEMU_WIN:-0}" = 1 ]; then
  . "$ROOT/tools/lib/qemu-win.sh"
  PY=(python.exe)
  p() { winpath "$1"; }
  QEMU="$(winpath "$QEMU_WIN_DIR/qemu-system-x86_64.exe")"
  WIN_ARGS=(--win $([ "${QEMU_SHOW:-0}" = 1 ] && echo --show) --ovmf-code "$(winpath "$QEMU_WIN_DIR/share/edk2-x86_64-code.fd")"
            --ovmf-vars "$(winpath "$QEMU_WIN_DIR/share/edk2-i386-vars.fd")")
  TRACE=()
  echo "kbdsuite: Windows' QEMU has no hid_kbd_set_leds trace; the lock lights are not checked"
else
if [ -z "${QEMU:-}" ]; then
  QEMU=qemu-system-x86_64
  [ -x "$HOME/qemu-rtl/bin/qemu-system-x86_64" ] && QEMU="$HOME/qemu-rtl/bin/qemu-system-x86_64"
fi
TRACE=()
if "$QEMU" -trace help 2>/dev/null | grep -q '^hid_kbd_set_leds$'; then
  TRACE=(--trace)
else
  echo "kbdsuite: $QEMU has no hid_kbd_set_leds trace; the lock lights are not checked"
fi
fi

if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target iso --parallel 4 >/dev/null
fi
ENTRY_LIST="$BUILD_DIR/initrd-entries.txt"
case " $KBD_PARTS " in *" terminal "*)
  if ! grep -q "^bin/bash:" "$ENTRY_LIST" || ! grep -q "^bin/curl:" "$ENTRY_LIST"; then
    echo "kbdsuite: the system image needs bash and curl (tools/build-bash-mlibc.sh, tools/build-curl.sh, rerun cmake)" >&2
    exit 2
  fi ;;
esac
mkdir -p "$OUT_DIR"

# The probe image: the feature suite's image with console_probe as /bin/login.
SKIP_CMAKE=1 OUT_DIR="$OUT_DIR/probe" RUNNER_SRC="$ROOT/tools/kbd-suite/console_probe.c" \
  RUNNER_EXTRA_CFLAGS="-I$ROOT/tools/os-testsuite" \
  "$ROOT/tools/os-testsuite/build-testsuite-iso.sh" >/dev/null

if ss -ltn 2>/dev/null | grep -qE ":$HTTP_PORT\b"; then
  echo "kbdsuite: port $HTTP_PORT is already in use on this host" >&2; exit 2
fi
REQUEST_LOG="$OUT_DIR/requests.log"; : > "$REQUEST_LOG"
rm -f "$OUT_DIR/host.pid"
"${PY[@]}" "$(p "$ROOT/tools/net-suite/host_server.py")" --tcp-echo 18307 --udp-echo 18308 --http "$HTTP_PORT" \
  --request-log "$(p "$REQUEST_LOG")" --pidfile "$(p "$OUT_DIR/host.pid")" > "$OUT_DIR/host_server.log" 2>&1 &
host_pid=$!
stop_host() {
  kill $host_pid 2>/dev/null || true
  if [ "${QEMU_WIN:-0}" = 1 ] && [ -f "$OUT_DIR/host.pid" ]; then
    taskkill.exe /PID "$(tr -dc 0-9 < "$OUT_DIR/host.pid")" /F >/dev/null 2>&1 || true
  fi
}
trap stop_host EXIT
for _ in $(seq 1 50); do grep -q ready "$OUT_DIR/host_server.log" 2>/dev/null && break; sleep 0.1; done

status=0
"${PY[@]}" "$(p "$ROOT/tools/kbd-suite/drive.py")" --modes "$KBD_MODES" --parts "$KBD_PARTS" \
  --probe-iso "$(p "$OUT_DIR/probe/os-testsuite.iso")" --system-iso "$(p "$BUILD_DIR/iboot.iso")" \
  --request-log "$(p "$REQUEST_LOG")" --http-port "$HTTP_PORT" --out "$(p "$OUT_DIR")" \
  --qemu "$QEMU" --smp "$QEMU_SMP" "${TRACE[@]}" "${WIN_ARGS[@]}" | tee "$OUT_DIR/drive.log" || status=$?
echo "kbdsuite: modes=\"$KBD_MODES\" status=$status out=$OUT_DIR"
exit "$status"
