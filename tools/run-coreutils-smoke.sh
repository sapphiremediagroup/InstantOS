#!/usr/bin/env bash
# Headless coreutils validation: boots InstantOS with a PTY launcher that runs
# real cross-built GNU coreutils binaries (in /bin) through bash, and asserts on
# their output over serial. Exercises the Tier 1 + Access sysdeps end-to-end.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/cu-smoke}"
LOG="${LOG:-$OUT_DIR/cu-smoke.serial.log}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-120s}"
QEMU_ACCEL="${QEMU_ACCEL:-tcg}"
OVMF_CODE="${OVMF_CODE:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}"
OVMF_VARS_TEMPLATE="${OVMF_VARS_TEMPLATE:-/usr/share/edk2/x64/OVMF_VARS.4m.fd}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

MLIBC_ROOT="$BUILD_DIR/mlibc-root"
BASH_BIN="$BUILD_DIR/bash"
CU_DIR="$BUILD_DIR/coreutils"

[ -f "$BASH_BIN" ] || { echo "bash missing; run tools/build-bash.sh" >&2; exit 2; }
[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc missing; run tools/build-mlibc.sh" >&2; exit 2; }
[ -d "$CU_DIR" ] || { echo "coreutils missing; run tools/build-coreutils.sh" >&2; exit 2; }

mkdir -p "$OUT_DIR"
cmake --build "$BUILD_DIR" --target INSTANTOS.EFI BOOTX64.EFI mkInitrd_proj

LAUNCHER="$OUT_DIR/launcher"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector \
  -nostdinc -c "$ROOT/tools/coreutils-smoke/launcher.c" -o "$OUT_DIR/launcher.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so \
  -o "$LAUNCHER" "$OUT_DIR/launcher.o"

INITRD="$OUT_DIR/initrd.img"
ISO_ROOT="$OUT_DIR/iso"
EFI_DIR="$ISO_ROOT/EFI/BOOT"
EFI_IMG="$EFI_DIR/efiboot.img"
ISO="$OUT_DIR/cu-smoke.iso"
OVMF_VARS="$OUT_DIR/OVMF_VARS.fd"

rm -rf "$ISO_ROOT"; mkdir -p "$EFI_DIR"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS"

# Base initrd entries + bash + mlibc runtime.
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
# Add every cross-built coreutils binary under /bin.
for b in "$CU_DIR"/*; do
  ENTRIES+=("bin/$(basename "$b"):$b")
done

"$BUILD_DIR/mkInitrd_build/mkInitrd" "$INITRD" "${ENTRIES[@]}"

cp "$BUILD_DIR/BOOTX64.EFI" "$EFI_DIR/BOOTX64.EFI"
cp "$BUILD_DIR/INSTANTOS.EFI" "$EFI_DIR/INSTANTOS.EFI"
cp "$INITRD" "$EFI_DIR/INITRD"

dd if=/dev/zero of="$EFI_IMG" bs=1M count=48 status=none
mformat -i "$EFI_IMG" ::
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$BUILD_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$EFI_IMG" "$BUILD_DIR/INSTANTOS.EFI" ::/EFI/BOOT/INSTANTOS.EFI
mcopy -i "$EFI_IMG" "$INITRD" ::/EFI/BOOT/INITRD

xorriso -as mkisofs -R -J -joliet-long -iso-level 3 \
  -eltorito-alt-boot -e EFI/BOOT/efiboot.img -no-emul-boot \
  -o "$ISO" "$ISO_ROOT" >/dev/null

rm -f "$LOG"

# A writable root filesystem requires a real persistent disk: the kernel only
# mounts "/" (FAT32) when an AHCI disk with a FAT partition is present. Without
# it, chdir("/")/file creation fail with ENOENT. Create a fresh 64 MiB FAT32
# disk per run so coreutils has a writable / to operate in.
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
[ "$status" -eq 0 ] || [ "$status" -eq 124 ] || { echo "cu smoke qemu failed status=$status" >&2; exit "$status"; }

if grep -q 'Unknown Instruction' "$LOG" || grep -q 'User process crash' "$LOG"; then
  echo "cu smoke: user process crash" >&2
  grep -nE 'Unknown Instruction|User process crash|exception:|rip:' "$LOG" >&2 || true
  exit 1
fi

fail=0
check(){ if ! grep -q "$1" "$LOG"; then echo "cu smoke: missing $1" >&2; fail=1; fi; }
check 'CUTEST] ptmx-open ok'
check 'hello'           # cat
check 'CU_MV_OK'        # mv (FAT32 rename) preserves data
check 'CU_CHMOD_OK'     # chmod (no-op success on FAT)
check 'CU_TOUCH_OK'     # touch (no-op success on FAT)
check 'CU_ACCESS_OK'    # test -r => Access syscall
check 'CU_DF_OK'        # df (statvfs)
check 'CU_DFALL_OK'     # df with no args (mount-table enumeration)
check 'CU_STATF_OK'     # stat -f (statvfs)
check 'CU_CHOWN_OK'     # chown (Chown syscall)
check 'CU_ID_OK'        # id
check 'CU_IDFULL_OK'    # id (GetResuid/GetGroups/GetSid)
check 'uid=0(root)'     # /etc/passwd name resolution
check 'CU_WHOAMI_OK'    # whoami
check 'CU_FIFO_OK'      # mkfifo on tmpfs (RamFS)
check 'PIPED'           # FIFO IPC: data passed reader<-writer
check 'CU_FIFOIO_OK'    # FIFO blocking read/write
check 'CU_MKNOD_OK'     # mknod char device
check 'CU_SHUF_OK'      # shuf (getentropy)
check 'CU_MKTEMP_OK'    # mktemp (getentropy)
check 'CU_SORT_OK'      # sort (GetRlimit)
check 'CU_UPTIME_OK'    # uptime (getloadavg)
check 'CU_WHO_OK'       # who (utmp)
check 'CU_USERS_OK'     # users (utmp)
check 'CU_DONE'         # rm + ls completed
check 'CUTEST] done'

# These sysdeps are now implemented, so the "missing sysdep" abort log must not
# appear for them.
for tag in GetRlimit GetResuid GetGroups GetSid Times ClockGetres Fadvise GetLoadavg; do
  if grep -q "Tag = $tag," "$LOG"; then
    echo "cu smoke: unexpected missing-sysdep for $tag" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "--- captured coreutils session ---" >&2
  grep -nE 'CUTEST|hello|CU_|loop|a\.txt|c\.txt' "$LOG" >&2 || true
  exit 1
fi

# df should no longer warn that it cannot read the mount table.
if grep -q 'cannot read table of mounted file systems' "$LOG"; then
  echo "cu smoke: df still cannot read mount table (/etc/mtab not seeded?)" >&2
  exit 1
fi

echo "coreutils smoke passed: $LOG"
grep -nE 'CUTEST|CU_ACCESS_OK|CU_DONE|hello' "$LOG" || true
