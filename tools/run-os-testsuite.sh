#!/usr/bin/env bash
# Headless in-OS feature suite: builds the os-testsuite ISO + ext4 disk
# (tools/os-testsuite/build-testsuite-iso.sh), boots it under QEMU, and asserts
# on the "[ostest] SCORE ..." line from tools/os-testsuite/runner.c.
#
#   QEMU_SMP=1|2|4   CPU count to boot with (default 1)
#   SKIP_BUILD=1     reuse the existing ISO/disk instead of rebuilding
#   SKIP_CMAKE=1     passed through: only repack runner.c, skip the kernel build
#
#   SUITE_TAG=netsuite  marker prefix of another runner (tools/run-net-suite.sh)
#
# Exits 0 only when the suite reaches [$SUITE_TAG] END with fail=0.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/os-testsuite}"
QEMU_SMP="${QEMU_SMP:-1}"
SUITE_TAG="${SUITE_TAG:-ostest}"
LOG_BASE=os-testsuite; [ "$SUITE_TAG" = ostest ] || LOG_BASE="$SUITE_TAG"
LOG="${LOG:-$OUT_DIR/$LOG_BASE.smp$QEMU_SMP.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-900}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
# The NIC, e.g. QEMU_NIC=e1000 or QEMU_NIC=virtio-net-pci,vectors=0 (no MSI-X).
QEMU_NIC="${QEMU_NIC:-virtio-net-pci,disable-legacy=on,disable-modern=off}"
# The backend behind the NIC's netdev "net0" (split on spaces); the network
# suite puts a hub there to add a fake host beside slirp.
QEMU_NETDEV="${QEMU_NETDEV:--netdev user,id=net0}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"

ISO="$OUT_DIR/os-testsuite.iso"
DISK="$OUT_DIR/ext4.img"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  BUILD_DIR="$BUILD_DIR" OUT_DIR="$OUT_DIR" "$ROOT/tools/os-testsuite/build-testsuite-iso.sh"
fi
[ -f "$ISO" ]  || { echo "ostest: missing $ISO" >&2; exit 2; }
[ -f "$DISK" ] || { echo "ostest: missing $DISK" >&2; exit 2; }

# The suite writes to the ext4 disk; boot a scratch copy so reruns start clean.
RUN_DISK="$OUT_DIR/ext4.run.img"
cp "$DISK" "$RUN_DISK"
OVMF_VARS="$OUT_DIR/OVMF_VARS.fd"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

rm -f "$LOG"
"$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$OVMF_VARS" \
  -drive "file=$ISO,media=cdrom" \
  -device ich9-ahci,id=ahci \
  -drive "id=ahci_disk,file=$RUN_DISK,if=none,format=raw" \
  -device ide-hd,drive=ahci_disk,bus=ahci.0 \
  -device "$QEMU_NIC,netdev=net0" \
  $QEMU_NETDEV \
  -accel "$QEMU_ACCEL" -cpu max,+tsc -m 1G -smp "$QEMU_SMP" \
  -display none -monitor none \
  -serial "file:$LOG" -no-shutdown -no-reboot \
  ${QEMU_EXTRA_ARGS:-} &
qemu_pid=$!
trap 'kill "$qemu_pid" 2>/dev/null || true' EXIT

# The runner sleeps forever after END, so stop QEMU as soon as it appears.
start=$SECONDS
result=timeout
while kill -0 "$qemu_pid" 2>/dev/null; do
  if grep -q "\[$SUITE_TAG\] END" "$LOG" 2>/dev/null; then result=done; break; fi
  if [ $((SECONDS - start)) -ge "$QEMU_TIMEOUT" ]; then break; fi
  sleep 2
done
[ "$result" = timeout ] && ! kill -0 "$qemu_pid" 2>/dev/null && result=qemu-exited
kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true
elapsed=$((SECONDS - start))

echo "$SUITE_TAG: smp=$QEMU_SMP accel=$QEMU_ACCEL result=$result elapsed=${elapsed}s log=$LOG"
grep -nE '  FAIL: |  GAP: |Unknown Instruction|User process crash|KERNEL PANIC|panic' "$LOG" || true
score="$(grep -a "\[$SUITE_TAG\] SCORE" "$LOG" | tail -1 || true)"
if [ -z "$score" ]; then
  echo "$SUITE_TAG: no SCORE line (last serial lines below)" >&2
  tail -20 "$LOG" >&2
  exit 1
fi
echo "$score"
case "$score" in
  *" fail=0 "*) exit 0 ;;
  *) exit 1 ;;
esac
