#!/usr/bin/env bash
# Headless TinyCC validation: boots InstantOS with a PTY launcher that drives
# /bin/tcc through bash and runs a diagnostic ladder
#   tcc -v -> tcc -E -> tcc -c -> tcc <src> -o exe -> ./exe
# asserting on the result of each rung over serial. This validates the in-OS
# compile loop end-to-end and pinpoints the first failing stage.
#
# Prereqs (same as the coreutils smoke, plus the tcc target):
#   tools/build-mlibc.sh      -> build/mlibc-root/lib/{libc.so,ld-instantos.so,libdl.so}
#   tools/build-bash.sh       -> build/bash
#   cmake --build build --target tcc   (with -DINSTANTOS_ENABLE_TCC=ON)
#       -> build/tcc, build/tcc-sysroot/lib/{crt0.o,crti.o,crtn.o,...}/tcc/libtcc1.a
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/tcc-smoke}"
LOG="${LOG:-$OUT_DIR/tcc-smoke.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-180s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

MLIBC_ROOT="$BUILD_DIR/mlibc-root"
BASH_BIN="$BUILD_DIR/bash"
TCC_BIN="$BUILD_DIR/tcc"
TCC_SYSROOT="$BUILD_DIR/tcc-sysroot"
ILIBCXX_INC="$ROOT/outside/iUserApps/outside/ilibcxx/include"
TCC_PRIV_INC="$ROOT/outside/iUserApps/outside/tinycc/include"

[ -f "$BASH_BIN" ]              || { echo "bash missing; run tools/build-bash.sh" >&2; exit 2; }
[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc missing; run tools/build-mlibc.sh" >&2; exit 2; }
[ -f "$TCC_BIN" ]              || { echo "tcc missing; cmake --build build --target tcc" >&2; exit 2; }
[ -f "$TCC_SYSROOT/lib/tcc/libtcc1.a" ] || { echo "libtcc1.a missing; cmake --build build --target tcc" >&2; exit 2; }

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj

LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -c "$ROOT/tools/tcc-smoke/launcher.c" -o "$OUT_DIR/launcher.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so \
  -o "$LAUNCHER" "$OUT_DIR/launcher.o"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"
EFI_DIR="$ISO_ROOT/EFI/BOOT"
EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/tcc-smoke.iso"
OVMF_VARS="$OUT_DIR/OVMF_VARS.fd"

rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

# Base initrd: managers + tcc launcher as /bin/login + bash + mlibc runtime.
ENTRIES=(
  bin/input-manager:"$BUILD_DIR/input-manager"
  bin/storage-manager:"$BUILD_DIR/storage-manager"
  bin/process-manager:"$BUILD_DIR/process-manager"
  bin/font-manager:"$BUILD_DIR/font-manager"
  bin/session-manager:"$BUILD_DIR/session-manager"
  bin/login:"$LAUNCHER"
  bin/bash:"$BASH_BIN"
  lib/ld-instantos.so:"$BUILD_DIR/ld-instantos.so"
  lib/libinstant.so:"$BUILD_DIR/libinstant.so"
  lib/mlibc/ld-instantos.so:"$MLIBC_ROOT/lib/ld-instantos.so"
  lib/mlibc/libc.so:"$MLIBC_ROOT/lib/libc.so"
  lib/mlibc/libdl.so:"$MLIBC_ROOT/lib/libdl.so"
)

# tcc itself + the C sysroot (CRT objects, libc.so, libtcc1.a), mirroring the
# CMakeLists INSTANTOS_ENABLE_TCC initrd entries.
ENTRIES+=(
  bin/tcc:"$TCC_BIN"
  bin/tcc-hello.c:"$ROOT/tools/tcc/hello.c"
  lib/crt0.o:"$TCC_SYSROOT/lib/crt0.o"
  lib/crt1.o:"$TCC_SYSROOT/lib/crt1.o"
  lib/Scrt1.o:"$TCC_SYSROOT/lib/Scrt1.o"
  lib/crti.o:"$TCC_SYSROOT/lib/crti.o"
  lib/crtn.o:"$TCC_SYSROOT/lib/crtn.o"
  lib/libc.so:"$TCC_SYSROOT/lib/libc.so"
  lib/tcc/libtcc1.a:"$TCC_SYSROOT/lib/tcc/libtcc1.a"
)

# libc/POSIX headers -> /include (system include path).
while IFS= read -r -d '' h; do
  rel="${h#"$ILIBCXX_INC"/}"
  ENTRIES+=("include/$rel:$h")
done < <(find "$ILIBCXX_INC" -type f -name '*.h' -print0)

# tcc's own builtin headers (stdarg.h/stddef.h/...) -> /lib/tcc/include.
for h in "$TCC_PRIV_INC"/*.h; do
  [ -f "$h" ] || continue
  ENTRIES+=("lib/tcc/include/$(basename "$h"):$h")
done

"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" "${ENTRIES[@]}"

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
  -o "$ISO" "$ISO_ROOT" >/dev/null

rm -f "$LOG"

# Writable root FS for any on-disk output (tcc writes to /tmp tmpfs, but mount /
# anyway to match the normal boot path so chdir("/") etc. behave).
DISK_IMG="$OUT_DIR/ahci.img"
rm -f "$DISK_IMG"
truncate -s 64M "$DISK_IMG"
sfdisk "$DISK_IMG" >/dev/null 2>&1 <<'EOF'
label: dos
start=2048, type=0c
EOF
mformat -F -i "$DISK_IMG@@1M" ::

set +e
timeout "$QEMU_TIMEOUT" "$QEMU" \
  -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
  -drive "if=pflash,format=raw,file=$OVMF_VARS" \
  -drive "file=$ISO,media=cdrom" \
  -device ich9-ahci,id=ahci \
  -drive "id=ahci_disk,file=$DISK_IMG,if=none,format=raw" \
  -device ide-hd,drive=ahci_disk,bus=ahci.0 \
  -accel "$QEMU_ACCEL" -cpu max,+tsc -m 1G -smp 2 \
  -serial "file:$LOG" -no-shutdown -no-reboot \
  ${QEMU_EXTRA_ARGS:-}
status=$?
set -e
[ "$status" -eq 0 ] || [ "$status" -eq 124 ] || { echo "tcc smoke qemu failed status=$status" >&2; exit "$status"; }

echo "=== serial log: $LOG ==="
grep -nE 'TCCTEST|TCC_|Unknown Instruction|User process crash|exception:|rip:|missing sysdep|Tag = |ld-instantos]' "$LOG" || true
echo "=== end ==="

# Verdict: assert the full compile ladder reached a runnable binary. Each rung
# must report success so a regression points at the exact failing stage.
fail=0
ladder_check(){ if ! grep -q "$1" "$LOG"; then echo "tcc smoke: missing $1" >&2; fail=1; fi; }
ladder_check 'TCC_VERSION_RC=0'   # tcc binary loads and runs (ld-instantos deps resolve)
ladder_check 'TCC_PP_RC=0'        # preprocess: include-path lookup + header reads
ladder_check 'TCC_COMPILE_RC=0'   # compile-only: codegen + .o written
ladder_check 'TCC_OBJ_EXISTS'
ladder_check 'TCC_LINK_RC=0'      # link: crt*.o/libtcc1.a/libc.so + ELF emit
ladder_check 'TCC_EXE_EXISTS'
ladder_check 'TCC_ELF_MAGIC_OK'   # produced file is a valid ELF
ladder_check 'hello from tcc on InstantOS'  # the tcc-built binary actually ran
ladder_check 'TCC_RUN_RC=0'

if grep -qE 'Unknown Instruction|User process crash' "$LOG"; then
  echo "tcc smoke: user process crash" >&2
  grep -nE 'Unknown Instruction|User process crash|exception:|rip:' "$LOG" >&2 || true
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "tcc smoke FAILED" >&2
  exit 1
fi
echo "tcc smoke passed: in-OS 'tcc hello.c -o hello && ./hello' works ($LOG)"
