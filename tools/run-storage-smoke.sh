#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/storage-smoke}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-20s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target iso

if ! command -v mformat >/dev/null 2>&1; then
  printf 'mformat is required for the formatted storage smoke image\n' >&2
  exit 2
fi

make_disk() {
  local kind="$1"
  local image="$OUT_DIR/$kind.img"
  rm -f "$image"
  truncate -s 64M "$image"

  case "$kind" in
    empty)
      ;;
    formatted)
      mformat -F -i "$image" ::
      ;;
    corrupted)
      mformat -F -i "$image" ::
      dd if=/dev/zero of="$image" bs=512 count=1 conv=notrunc status=none
      ;;
    *)
      printf 'unknown storage smoke image kind: %s\n' "$kind" >&2
      exit 2
      ;;
  esac

  printf '%s\n' "$image"
}

run_case() {
  local kind="$1"
  local image
  image="$(make_disk "$kind")"
  local vars="$OUT_DIR/$kind.OVMF_VARS.fd"
  local log="$OUT_DIR/$kind.serial.log"
  cp "$OVMF_VARS_TEMPLATE" "$vars"
  rm -f "$log"

  printf 'storage smoke case=%s image=%s log=%s\n' "$kind" "$image" "$log"
  set +e
  timeout "$QEMU_TIMEOUT" "$QEMU" \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,format=raw,file=$vars" \
    -drive "file=$BUILD_DIR/iboot.iso,media=cdrom" \
    -display none \
    -device ich9-ahci,id=ahci \
    -accel "$QEMU_ACCEL" \
    -cpu max,+tsc \
    -drive "id=ahci_disk,file=$image,if=none,format=raw" \
    -device ide-hd,drive=ahci_disk,bus=ahci.0 \
    -chardev "file,id=serial0,path=$log" \
    -serial chardev:serial0 \
    -monitor none \
    -m 1G \
    -smp 2 \
    -no-shutdown \
    -no-reboot \
    ${QEMU_EXTRA_ARGS:-}
  local status=$?
  set -e

  if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    printf 'storage smoke case=%s failed with qemu status=%s\n' "$kind" "$status" >&2
    return "$status"
  fi
}

run_case empty
run_case formatted
run_case corrupted

printf 'storage smoke logs: %s\n' "$OUT_DIR"
