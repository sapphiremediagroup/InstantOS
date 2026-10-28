#!/usr/bin/env bash
# Headless NetSurf suite: the desktop and browser end to end. Boots the real
# system image (build/initrd-entries.txt, which cmake writes) with /bin/login
# swapped for tools/netsurf-suite/driver.c, which gives NetSurf a homepage on
# tools/net-suite/host_server.py (https://10.0.2.2) and starts the desktop.
# tools/netsurf-suite/drive.py then works the VM over QMP like a user: clicks
# the taskbar's Web slot, checks that the page, its stylesheet and image are
# fetched over HTTPS and drawn (screendumps), clicks a link, types a URL.
#
#   QEMU_SMP=N      CPU count (default 2)
#   SKIP_CMAKE=1    reuse the built kernel and userland
#
# Needs NetSurf (tools/build-netsurf-libs.sh, tools/build-netsurf.sh) and the
# CA bundle (tools/fetch-cacert.sh), then a cmake rerun. Exits 0 only when
# every check passes; screendumps and logs land in build/netsurf-suite.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/netsurf-suite}"
SUITE_DIR="$ROOT/tools/net-suite"
NS_DIR="$ROOT/tools/netsurf-suite"
HTTP_PORT="${HTTP_PORT:-18080}"
HTTPS_PORT="${HTTPS_PORT:-18443}"
QEMU_SMP="${QEMU_SMP:-2}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-900}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

if [ "${SKIP_CMAKE:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target make_initrd BOOTX64.EFI --parallel 4 >/dev/null
fi
ENTRY_LIST="$BUILD_DIR/initrd-entries.txt"
if [ ! -f "$ENTRY_LIST" ] || ! grep -q "^bin/nsfb:" "$ENTRY_LIST" || ! grep -q "^bin/netsurf:" "$ENTRY_LIST"; then
  echo "nssuite: NetSurf is not in the system image; build it and rerun cmake" >&2; exit 2
fi
rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"

# The test CA joins the system bundle, as an admin would add a local CA.
"$SUITE_DIR/make-test-certs.sh" "$BUILD_DIR/net-suite/tls"
TLS_DIR="$BUILD_DIR/net-suite/tls"
cat "$BUILD_DIR/cacert/cacert.pem" "$TLS_DIR/ca.pem" > "$OUT_DIR/ca-certificates.crt"

"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdinc -O1 \
  -DHTTPS_PORT="$HTTPS_PORT" -c "$NS_DIR/driver.c" -o "$OUT_DIR/driver.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$OUT_DIR/driver" "$OUT_DIR/driver.o"

ENTRIES=()
while IFS= read -r entry; do
  case "$entry" in
    ""|bin/login:*|ssl/certs/ca-certificates.crt:*) ;;
    *) ENTRIES+=("$entry") ;;
  esac
done < "$ENTRY_LIST"
ENTRIES+=("bin/login:$OUT_DIR/driver" "ssl/certs/ca-certificates.crt:$OUT_DIR/ca-certificates.crt")

ISO_ROOT="$OUT_DIR/iso"; EFI_DIR="$ISO_ROOT/EFI/BOOT"; EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/netsurf-suite.iso"; INITRD="$OUT_DIR/initrd.img"
mkdir -p "$EFI_DIR"
"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" "${ENTRIES[@]}" >/dev/null
dd if=/dev/zero of="$EFI_IMG" bs=1M count=96 status=none
mformat -i "$EFI_IMG" ::
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_IMG" "$BUILD_DIR/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$EFI_IMG" "$INITRD" ::/EFI/BOOT/INITRD
xorriso -as mkisofs -R -J -joliet-long -iso-level 3 \
  -eltorito-alt-boot -e EFI/BOOT/efiboot.img -no-emul-boot \
  -o "$ISO" "$ISO_ROOT" >/dev/null 2>&1
rm -rf "$ISO_ROOT"

if ss -ltn 2>/dev/null | grep -qE ":($HTTP_PORT|$HTTPS_PORT)\b"; then
  echo "nssuite: a test port is already in use on this host" >&2; exit 2
fi
REQUEST_LOG="$OUT_DIR/requests.log"; : > "$REQUEST_LOG"
python3 "$SUITE_DIR/host_server.py" --tcp-echo 18107 --udp-echo 18108 --http "$HTTP_PORT" \
  --https "$HTTPS_PORT" --cert "$TLS_DIR/server.pem" --key "$TLS_DIR/server.key" \
  --pages "$NS_DIR/pages" --request-log "$REQUEST_LOG" > "$OUT_DIR/host_server.log" 2>&1 &
host_pid=$!
qemu_pid=
trap 'kill $host_pid $qemu_pid 2>/dev/null || true; rm -rf "${SOCK_DIR:-}"' EXIT
for _ in $(seq 1 50); do grep -q ready "$OUT_DIR/host_server.log" 2>/dev/null && break; sleep 0.1; done

# Unix sockets cannot live on a Windows (drvfs) mount such as /mnt/d.
SOCK_DIR="$(mktemp -d)"
SERIAL="$OUT_DIR/serial.log"; QMP="$SOCK_DIR/qmp.sock"
cp "$OVMF_VARS_TEMPLATE" "$OUT_DIR/OVMF_VARS.fd"
timeout "$QEMU_TIMEOUT" qemu-system-x86_64 \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$OUT_DIR/OVMF_VARS.fd" \
  -drive "file=$ISO,media=cdrom" \
  -device qemu-xhci,id=usb -device usb-kbd,bus=usb.0 -device usb-mouse,bus=usb.0 \
  -device virtio-net-pci,netdev=net0,disable-legacy=on,disable-modern=off -netdev user,id=net0 \
  -accel tcg -cpu max,+tsc -m 2G -smp "$QEMU_SMP" \
  -display none -monitor none -qmp "unix:$QMP,server,nowait" \
  -serial "file:$SERIAL" -no-reboot ${QEMU_EXTRA_ARGS:-} &
qemu_pid=$!

status=0
python3 "$NS_DIR/drive.py" --qmp "$QMP" --serial "$SERIAL" --request-log "$REQUEST_LOG" \
  --out "$OUT_DIR" --https-port "$HTTPS_PORT" | tee "$OUT_DIR/drive.log" || status=$?
if grep -aqE "Unknown Instruction|User process crash|KERNEL PANIC" "$SERIAL"; then
  echo "nssuite: the serial log shows a crash:" >&2
  grep -aE "Unknown Instruction|User process crash|KERNEL PANIC" "$SERIAL" | head -5 >&2
  status=1
fi
echo "nssuite: smp=$QEMU_SMP status=$status log=$SERIAL"
exit "$status"
