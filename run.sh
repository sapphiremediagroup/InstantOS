#!/usr/bin/env bash
set -euo pipefail

DISK_IMG="build/ahci.img"
OVMF_VARS="build/OVMF_VARS.4m.fd"
USB_MODE="${USB_MODE:-xhci}"
QEMU_ACCEL="${QEMU_ACCEL:-kvm}"
QEMU_DISPLAY="${QEMU_DISPLAY:-gtk}"

USB_ARGS=()
case "$USB_MODE" in
  xhci)
    USB_ARGS=(-device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0)
    ;;
  ohci)
    USB_ARGS=(-device pci-ohci,id=usb -device usb-kbd,bus=usb.0)
    ;;
  none)
    USB_ARGS=()
    ;;
  *)
    echo "USB_MODE must be one of: xhci, ohci, none" >&2
    exit 2
    ;;
esac

mkdir -p build

if [ ! -f "$DISK_IMG" ]; then
    truncate -s 64M "$DISK_IMG"
    mformat -F -i "$DISK_IMG" ::
fi

if [ ! -f "$OVMF_VARS" ]; then
    cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "$OVMF_VARS"
fi

qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive file=build/iboot.iso,media=cdrom \
  -display "$QEMU_DISPLAY" \
  -device ich9-ahci,id=ahci \
  -accel "$QEMU_ACCEL" \
  -cpu max,+tsc \
  -drive id=ahci_disk,file="$DISK_IMG",if=none,format=raw \
  -device ide-hd,drive=ahci_disk,bus=ahci.0 \
  "${USB_ARGS[@]}" \
  -chardev stdio,id=serial0,signal=off,logfile=serial.log,logappend=off \
  -serial chardev:serial0 \
  -monitor none \
  -m 1G \
  -smp 2 \
  -no-shutdown -no-reboot \
  -d int,cpu_reset,guest_errors \
  -D qemu.log "$@"
