#!/usr/bin/env bash
# End-to-end http(s) test for the NetSurf browser on InstantOS in QEMU.
#
# Boots the full ISO machinery but replaces the boot "login" process with a
# network-enabled launcher that spawns network-manager (packet pump) +
# graphics-compositor + nsfb pointed at $URL. QEMU user-mode networking
# (virtio-net + slirp) matches the kernel's hardcoded 10.0.2.x addressing, and
# the boot-written /etc/resolv.conf points DNS at 10.0.2.3.
#
# Usage: URL=http://example.com/ tools/run-netsurf-net.sh
set -euo pipefail
ROOT="/home/sky/projects/InstantOS"
BUILD_DIR="$ROOT/build"
MLIBC_ROOT="$BUILD_DIR/mlibc-root"
NS_SRC="$BUILD_DIR/netsurf-port/netsurf-3.11"
RES="$NS_SRC/frontends/framebuffer/res"
CACERT="${CACERT:-$BUILD_DIR/cacert/cacert.pem}"
OUT_DIR="$BUILD_DIR/netsurf-net-run"
LOG="$OUT_DIR/netsurf.serial.log"
QEMU="qemu-system-x86_64"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-90s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
URL="${URL:-http://example.com/}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
OVMF_CODE="/usr/share/edk2/x64/OVMF_CODE.4m.fd"
OVMF_VARS_TEMPLATE="/usr/share/edk2/x64/OVMF_VARS.4m.fd"

mkdir -p "$OUT_DIR"
INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/netsurf.iso"; OVMF_VARS="$OUT_DIR/OVMF_VARS.fd"

# Build the network launcher (freestanding PIE, URL baked in).
LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -DNETSURF_START_URL="\"$URL\"" \
  -nostdinc -c "$ROOT/tools/netsurf-smoke/net_launcher.c" -o "$OUT_DIR/launcher.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so \
  -o "$LAUNCHER" "$OUT_DIR/launcher.o"

rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

ARGS=(
  bin/input-manager:"$BUILD_DIR/input-manager"
  bin/storage-manager:"$BUILD_DIR/storage-manager"
  bin/process-manager:"$BUILD_DIR/process-manager"
  bin/network-manager:"$BUILD_DIR/network-manager"
  bin/font-manager:"$BUILD_DIR/font-manager"
  bin/session-manager:"$BUILD_DIR/session-manager"
  bin/login:"$LAUNCHER"
  bin/graphics-compositor:"$BUILD_DIR/graphics-compositor"
  bin/nsfb:"$NS_SRC/nsfb"
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so"
  lib/libinstant.so:"$BUILD_DIR/libinstant.so"
  lib/mlibc/ld-instantos.so:"$MLIBC_ROOT/lib/ld-instantos.so"
  lib/mlibc/libc.so:"$MLIBC_ROOT/lib/libc.so"
  lib/mlibc/libm.so:"$MLIBC_ROOT/lib/libm.so"
  lib/mlibc/libdl.so:"$MLIBC_ROOT/lib/libdl.so"
  lib/mlibc/libpthread.so:"$MLIBC_ROOT/lib/libpthread.so"
  netsurf/etc/cacert.pem:"$CACERT"
)
while IFS= read -r f; do
  rel="${f#$RES/}"
  ARGS+=( "netsurf/res/$rel:$f" )
done < <(find -L "$RES" -type f)

"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" "${ARGS[@]}"

cp "$BUILD_DIR/BOOTX64.EFI" "$EFI_DIR/BOOTX64.EFI"
cp "$BUILD_DIR/INSTANTOS.EFI" "$EFI_DIR/INSTANTOS.EFI"
cp "$INITRD" "$EFI_DIR/INITRD"

dd if=/dev/zero of="$EFI_IMG" bs=1M count=64 status=none
mformat -i "$EFI_IMG" ::
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_IMG" "$BUILD_DIR/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$EFI_IMG" "$INITRD" ::/EFI/BOOT/INITRD

xorriso -as mkisofs -R -J -joliet-long -iso-level 3 \
  -eltorito-alt-boot -e EFI/BOOT/efiboot.img -no-emul-boot \
  -o "$ISO" "$ISO_ROOT" >/dev/null 2>&1

rm -f "$LOG"
set +e
QMP="$OUT_DIR/qmp.sock"; rm -f "$QMP"
timeout "$QEMU_TIMEOUT" "$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$OVMF_VARS" \
  -drive "file=$ISO,media=cdrom" \
  -accel "$QEMU_ACCEL" -cpu max,+tsc -m 1G -smp 2 \
  -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off -netdev user,id=net0 \
  -serial "file:$LOG" -display none \
  -qmp "unix:$QMP,server,nowait" \
  -no-shutdown -no-reboot &
QPID=$!
sleep 60
python3 /tmp/opencode/qmp-shot.py "$QMP" "$OUT_DIR/screenshot.ppm" 2>/dev/null || true
sleep 2
kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
set -e
echo "=== serial tail ==="
tail -60 "$LOG" 2>/dev/null
echo "=== fetch/tls/dns markers ==="
grep -aiE "curl|fetch|http|https|ssl|tls|mbedtls|cert|dns|resolve|connect|10\.0\.2|NSERROR|fetcherror|UnacceptableType|launcher" "$LOG" 2>/dev/null | tail -40
