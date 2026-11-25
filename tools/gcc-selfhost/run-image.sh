#!/usr/bin/env bash
# Boot a self-host image headless and wait for its last marker.
#   run-image.sh ISO DISK LOG DONE_MARKER [TIMEOUT_S]
# e.g. run-image.sh build/gcchost/gcchost.iso build/gcchost/ext4.img \
#        build/gcchost/serial.log GCCHOST_DONE 7200
# The disk is copied first (DISK-run.img), so the image stays pristine for the
# next run. Uses KVM when /dev/kvm is there, else multi-threaded TCG.
# QEMU_MEM (default 3G) and QEMU_SMP (default: all CPUs) size the VM.
set -u
ISO=$1; DISK=$2; LOG=$3; DONE=$4; TMO=${5:-14400}
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_SRC="${OVMF_VARS_SRC:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
RUNDISK="${DISK%.img}-run.img"
VARS="$(dirname "$LOG")/OVMF_VARS.run.fd"
cp "$DISK" "$RUNDISK"
cp "$OVMF_VARS_SRC" "$VARS"
rm -f "$LOG"
# QEMU_ACCEL=tcg forces TCG even where KVM is there.
if [ -w /dev/kvm ] && [ "${QEMU_ACCEL:-}" != tcg ]; then ACCEL=(-accel kvm -cpu host); else ACCEL=(-accel tcg,thread=multi -cpu max); fi
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -drive file="$ISO",media=cdrom \
  -display none -vga std "${ACCEL[@]}" \
  -device ich9-ahci,id=ahci \
  -drive id=d0,file="$RUNDISK",if=none,format=raw -device ide-hd,drive=d0,bus=ahci.0 \
  -chardev file,id=serial0,path="$LOG" -serial chardev:serial0 -monitor none \
  -m "${QEMU_MEM:-3G}" -smp "${QEMU_SMP:-$(nproc)}" -no-reboot &
QPID=$!
start=$(date +%s)
status=timeout
while kill -0 $QPID 2>/dev/null; do
  if grep -aq "$DONE" "$LOG" 2>/dev/null; then status=done; sleep 3; break; fi
  if grep -aq "KERNEL PANIC" "$LOG" 2>/dev/null; then status=panic; sleep 3; break; fi
  [ $(( $(date +%s) - start )) -ge "$TMO" ] && break
  sleep 10
done
kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
echo "status=$status elapsed=$(( $(date +%s) - start ))s log=$LOG"
grep -a -o "GCC[A-Z]*_[A-Z0-9_]*=\?-\?[0-9x]*" "$LOG" | uniq
grep -a -c "User process crash" "$LOG" | sed 's/^/user process crashes: /'
