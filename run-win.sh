#!/usr/bin/env bash
# Windows-native QEMU runner for InstantOS (Git Bash / MSYS).
# Uses WHPX (Hyper-V) acceleration, which coexists with WSL2.
#
# Differences from run.sh (Linux/WSL):
#   - accel: whpx instead of kvm (fallback to tcg with QEMU_ACCEL=tcg)
#   - OVMF firmware taken from the QEMU for Windows install
#   - no virtio-gpu GL/venus modes (host GL contexts unsupported here)
set -euo pipefail

QEMU_DIR="${QEMU_DIR:-/c/Program Files/qemu}"
QEMU_BIN="$QEMU_DIR/qemu-system-x86_64.exe"

# DISK_IMG=build/dev-disk.img boots with the gcc toolchain disk (tools/make-dev-disk.sh).
DISK_IMG="${DISK_IMG:-build/ahci.img}"
OVMF_CODE="$QEMU_DIR/share/edk2-x86_64-code.fd"
OVMF_VARS_SRC="$QEMU_DIR/share/edk2-i386-vars.fd"
OVMF_VARS="build/OVMF_VARS_win.fd"
USB_MODE="${USB_MODE:-xhci}"
QEMU_ACCEL="${QEMU_ACCEL:-whpx,kernel-irqchip=off}"
QEMU_DISPLAY="${QEMU_DISPLAY:-sdl}"

if [ ! -x "$QEMU_BIN" ]; then
    echo "qemu-system-x86_64.exe not found in $QEMU_DIR (set QEMU_DIR)" >&2
    exit 1
fi

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
    echo "$DISK_IMG missing - build the OS first (build/ahci.img is created by run.sh" >&2
    echo "in WSL, or copy an existing formatted image)." >&2
    exit 1
fi

if [ ! -f "$OVMF_VARS" ]; then
    cp "$OVMF_VARS_SRC" "$OVMF_VARS"
fi

"$QEMU_BIN" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive file=build/iboot.iso,media=cdrom \
  -display "$QEMU_DISPLAY" \
  -device virtio-gpu-pci \
  -accel "$QEMU_ACCEL" \
  -cpu max,vmx=off,svm=off,lmce=off,sgx=off,sgx1=off,sgxlc=off \
  -drive id=ahci_disk,file="$DISK_IMG",if=none,format=raw \
  -device ich9-ahci,id=ahci \
  -device ide-hd,drive=ahci_disk,bus=ahci.0 \
  "${USB_ARGS[@]}" \
  -chardev file,id=serial0,path=serial.log \
  -serial chardev:serial0 \
  -monitor none \
  -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off \
  -netdev user,id=net0 \
  -m 1G \
  -smp "${QEMU_SMP:-1}" \
  -no-shutdown -no-reboot \
  "$@"
