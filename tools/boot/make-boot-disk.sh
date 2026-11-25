#!/usr/bin/env bash
# A GPT disk shaped like the PC, for checking iboot's config and the installer
# in QEMU:
#   1  EFI system partition (FAT32): iboot as the firmware's fallback loader
#      (/EFI/BOOT/BOOTX64.EFI), the kernel and initrd at /EFI/BOOT/INSTANTOS.EFI
#      and /EFI/BOOT/INITRD (where iboot looks for them), and iboot's settings
#      at /EFI/InstantOS/iboot.cfg when OUT_DIR has one.
#   2  "Windows": basic data, empty
#   3  INSTANTOS: basic data, FAT32 labelled INSTANTOS, which is what the PC has
#      today and what instant-install turns into the ext4 root.
# (The PC starts iboot from Limine, \EFI\InstantOS\iboot.efi; that is set up on
# the PC itself by tools/install-limine-windows.ps1, not here.)
#
#   tools/boot/make-boot-disk.sh            -> build/boot-disk/disk.img
#   SKIP_BUILD=1  reuse build/iso/EFI/BOOT
#   OUT_DIR/iboot.cfg, when there, goes to /EFI/InstantOS/iboot.cfg
#   INITRD_FILE   another initrd instead of the system's (test images)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/boot-disk}"
DISK="${DISK:-$OUT_DIR/disk.img}"
SIZE_MB="${SIZE_MB:-2048}"
ESP_MB="${ESP_MB:-512}"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target iso --parallel "$(nproc)"
fi
BOOT="$BUILD_DIR/iso/EFI/BOOT"
for f in BOOTX64.EFI INSTANTOS.EFI INITRD; do
  [ -f "$BOOT/$f" ] || { echo "make-boot-disk: missing $BOOT/$f (build the iso target)" >&2; exit 2; }
done

mkdir -p "$OUT_DIR"
rm -f "$DISK"
truncate -s "${SIZE_MB}M" "$DISK"
# Fixed partition GUIDs, so configs and checks can name them.
sfdisk -q "$DISK" <<EOF
label: gpt
first-lba: 2048
start=2048, size=${ESP_MB}MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=11111111-1111-4111-8111-111111111111, name="EFI system partition"
size=64MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=22222222-2222-4222-8222-222222222222, name="Windows"
type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=33333333-3333-4333-8333-333333333333, name="INSTANTOS"
EOF

# Partition N's byte offset and size, from sfdisk's dump.
part() {   # <n> -> "offset size" in bytes
  sfdisk -d "$DISK" | awk -v n="$1" '
    / : start=/ { i++; if (i == n) { gsub(/[ ,=]+/, " "); for (k = 1; k <= NF; k++) {
              if ($k == "start") s = $(k + 1); if ($k == "size") z = $(k + 1) }
            print s * 512, z * 512 } }'
}
read -r ESP_OFF ESP_SIZE < <(part 1)
read -r INST_OFF INST_SIZE < <(part 3)

ESP="$DISK@@$ESP_OFF"
mformat -i "$ESP" -F -T $((ESP_SIZE / 512)) -v ESP ::
mmd -i "$ESP" ::/EFI ::/EFI/BOOT ::/EFI/InstantOS
mcopy -i "$ESP" "$BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$BOOT/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$ESP" "${INITRD_FILE:-$BOOT/INITRD}" ::/EFI/BOOT/INITRD
if [ -f "$OUT_DIR/iboot.cfg" ]; then
  mcopy -i "$ESP" "$OUT_DIR/iboot.cfg" ::/EFI/InstantOS/iboot.cfg
fi

# The PC's INSTANTOS partition today: FAT32, labelled INSTANTOS.
mformat -i "$DISK@@$INST_OFF" -F -T $((INST_SIZE / 512)) -v INSTANTOS ::

echo "boot disk: $DISK"
sfdisk -l "$DISK" | sed -n '/^Device/,$p'
mdir -i "$ESP" -/ ::/EFI | grep -v "^$" | head -20
