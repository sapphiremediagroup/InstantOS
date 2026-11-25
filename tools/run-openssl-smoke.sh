#!/usr/bin/env bash
# OpenSSL in InstantOS: boots the system image (build/iboot.iso) with a small
# ext4 disk whose /autorun.sh runs tools/openssl-smoke (known answers,
# randomness, TLS in memory, threads), its no-entropy twin and the `openssl`
# command, and reads the results off the serial log.
#
#   tools/run-openssl-smoke.sh       (run in WSL; QEMU_WIN=1 uses Windows' QEMU
#                                     with WHPX, else TCG)
#
# Prerequisites: tools/build-openssl.sh, then cmake --build build --target iso.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="$BUILD_DIR/mlibc-root"
OUT="${OUT_DIR:-$BUILD_DIR/openssl-smoke}"
CC="${CC:-clang}"
grep -q "^lib/mlibc/libcrypto.so.3:" "$BUILD_DIR/initrd-entries.txt" || {
  echo "OpenSSL is not in the image (tools/build-openssl.sh, then cmake)" >&2; exit 2; }
rm -rf "$OUT"
mkdir -p "$OUT/root"

CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -O2 -nostdlibinc -isystem $MLIBC_ROOT/include -Wall"
LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--export-dynamic $MLIBC_ROOT/lib/crt1.o"
for v in "openssl-smoke:" "openssl-noentropy:-DNO_ENTROPY"; do
  name="${v%%:*}"
  $CC $CFLAGS -D_GNU_SOURCE ${v#*:} -c "$ROOT/tools/openssl-smoke/openssl_smoke.c" -o "$OUT/$name.o"
  $CC $LDFLAGS -o "$OUT/root/$name" "$OUT/$name.o" -lssl -lcrypto -lc
done
cat > "$OUT/root/autorun.sh" <<'EOF'
/openssl-smoke
echo "[openssl] smoke exit $?"
/openssl-noentropy
echo "[openssl] noentropy exit $?"
openssl version -a | sed 's/^/[openssl] cli: /'
cd /tmp
openssl req -x509 -newkey rsa:2048 -nodes -keyout k.pem -out c.pem -days 2 -subj /CN=cli.test 2>/dev/null \
  && openssl verify -CAfile c.pem c.pem && echo "[openssl] ok: cli makes and verifies a certificate"
printf abc | openssl dgst -sha256 | grep -q ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad \
  && echo "[openssl] ok: cli digest"
# Each run of speed ends by SIGALRM in the middle of its loop (alarm()). By the
# wall clock (-elapsed): there is no per-process CPU time yet (times() is 0).
openssl speed -elapsed -seconds 1 -bytes 1024 sha256 > speed.txt 2>&1 && grep "^sha256" speed.txt | grep -qv inf   && echo "[openssl] ok: openssl speed (SIGALRM ends each run)"
sed 's/^/[openssl] speed: /' speed.txt | tail -3
echo "[openssl] DONE"
EOF
IMG="$OUT/disk.img"
MKE2FS_CONFIG="$ROOT/tools/e2fsprogs/mke2fs.conf" mke2fs -q -F -b 4096 -I 256 -O has_journal,extent -d "$OUT/root" "$IMG" 64M 2>/dev/null

LOG="$OUT/serial.log"
if [ "${QEMU_WIN:-0}" = 1 ]; then
  QEMU_DIR="${QEMU_DIR:-/mnt/c/Program Files/qemu}"
  win() { printf '%s\\%s' "$(wslpath -w "$(dirname "$1")")" "$(basename "$1")"; }
  cp "$QEMU_DIR/share/edk2-i386-vars.fd" "$OUT/vars.fd"
  chmod u+w "$OUT/vars.fd"
  "$QEMU_DIR/qemu-system-x86_64.exe" \
    -drive "if=pflash,format=raw,readonly=on,file=$(win "$QEMU_DIR/share/edk2-x86_64-code.fd")" \
    -drive "if=pflash,format=raw,file=$(win "$OUT/vars.fd")" \
    -drive "file=$(win "$BUILD_DIR/iboot.iso"),media=cdrom" \
    -device ich9-ahci,id=ahci -drive "id=d0,file=$(win "$IMG"),if=none,format=raw" -device ide-hd,drive=d0,bus=ahci.0 \
    -accel whpx,kernel-irqchip=off -vga std -m 2G -smp "${QEMU_SMP:-4}" -nic none -display "$([ "${QEMU_SHOW:-0}" = 1 ] && echo gtk || echo none)" -monitor none \
    -pidfile "$(win "$OUT/qemu.pid")" -serial "file:$(win "$LOG")" -no-reboot &
else
  cp "${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}" "$OUT/vars.fd"
  "${QEMU:-qemu-system-x86_64}" \
    -drive "if=pflash,format=raw,readonly=on,file=${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}" \
    -drive "if=pflash,format=raw,file=$OUT/vars.fd" -drive "file=$BUILD_DIR/iboot.iso,media=cdrom" \
    -device ich9-ahci,id=ahci -drive "id=d0,file=$IMG,if=none,format=raw" -device ide-hd,drive=d0,bus=ahci.0 \
    -accel tcg -cpu max,+tsc -m 2G -smp "${QEMU_SMP:-2}" -nic none -display none -monitor none \
    -serial "file:$LOG" -no-reboot &
fi
qemu=$!
stop() {
  [ -f "$OUT/qemu.pid" ] && taskkill.exe /PID "$(tr -dc 0-9 < "$OUT/qemu.pid")" /F >/dev/null 2>&1 || true
  kill "$qemu" 2>/dev/null || true
}
trap stop EXIT
for _ in $(seq 1 "${TIMEOUT_S:-900}"); do
  sleep 1
  kill -0 "$qemu" 2>/dev/null || break
  grep -aq "\[openssl\] DONE\|User process crash\|Unknown Instruction" "$LOG" 2>/dev/null && break
done
grep -a "\[openssl\]\|User process crash\|Unknown Instruction\|missing symbol\|unresolved" "$LOG" || true
fail=0
want() { grep -aq -- "$1" "$LOG" || { echo "openssl-smoke: FAIL: no '$1'"; fail=1; }; }
want "SCORE main pass=[0-9]* fail=0"
want "SCORE noentropy pass=5 fail=0"
want "ok: cli makes and verifies a certificate"
want "ok: cli digest"
want "ok: openssl speed"
want "\[openssl\] DONE"
if grep -aq "\[openssl\] FAIL\|User process crash\|Unknown Instruction" "$LOG"; then fail=1; fi
if [ "$fail" = 0 ]; then echo "openssl-smoke: PASS"; else echo "openssl-smoke: FAIL"; fi
exit "$fail"
