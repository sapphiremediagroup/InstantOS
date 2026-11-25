#!/usr/bin/env bash
# instant-install end to end, in QEMU, on make-boot-disk.sh's PC-shaped disk
# (firmware -> iboot -> kernel; partition 3 the FAT32 INSTANTOS one):
#   1. live boot (iboot.cfg root=none): /bin/login
#      is tools/install-test/driver.c, which runs instant-install --yes disk0p3
#   2. on the host: partition 3 has the InstantOS type and a clean ext4
#      (e2fsck -fn), with /.installed, /bin, /lib and /test/logs on it
#   3. installed boot (iboot.cfg rewritten to root= partition 3's GUID):
#      the kernel takes /bin and /lib from the partition, the driver (now the
#      partition's copy) checks the tree, builds and runs C and C++ with the
#      toolchain the installer unpacked (tools/make-dev-tarball.sh; SKIP_DEV=1
#      leaves it out), and starts the compositor
# Exit 0 only if all three pass. OUT_DIR (default build/install-test) holds
# the disk and both serial logs. SKIP_BUILD=1 reuses the system build.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/install-test}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"
ROOT_GUID=33333333-3333-4333-8333-333333333333

if [ "${SKIP_BUILD:-0}" != "1" ]; then
  cmake --build "$BUILD_DIR" --target iso --parallel "$(nproc)" >/dev/null
fi
ENTRY_LIST="$BUILD_DIR/initrd-entries.txt"
grep -q "^bin/instant-install:" "$ENTRY_LIST" && grep -q "^bin/mke2fs:" "$ENTRY_LIST" || {
  echo "install-test: instant-install or mke2fs missing from the image (tools/build-e2fsprogs.sh, then cmake)" >&2
  exit 2
}
rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"

"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdinc -O1 \
  -c "$ROOT/tools/install-test/driver.c" -o "$OUT_DIR/driver.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$OUT_DIR/driver" "$OUT_DIR/driver.o"
ENTRIES=()
while IFS= read -r entry; do
  case "$entry" in ""|bin/login:*) ;; *) ENTRIES+=("$entry") ;; esac
done < "$ENTRY_LIST"
ENTRIES+=("bin/login:$OUT_DIR/driver")
# link-probe: the errno values of tar's calls on symlinks (its messages hide them).
MLIBC_LIB="$BUILD_DIR/mlibc-root/lib"
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -O2 -nostdlibinc \
  -isystem "$BUILD_DIR/mlibc-root/include" -D_GNU_SOURCE -c "$ROOT/tools/install-test/link-probe.c" -o "$OUT_DIR/link-probe.o"
"$CC" --target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L"$MLIBC_LIB" \
  -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc "$MLIBC_LIB/crt1.o" \
  -o "$OUT_DIR/link-probe" "$OUT_DIR/link-probe.o" -lc
ENTRIES+=("bin/link-probe:$OUT_DIR/link-probe")
# The toolchain, as the PC's INITRD carries it (tools/make-pc-initrd.sh):
# instant-install unpacks it, the installed boot builds with it.
DEV_TAR="${DEV_TAR:-$BUILD_DIR/instantos-dev.tar.gz}"
WANT_DEV=0
if [ "${SKIP_DEV:-0}" != "1" ] && [ -f "$DEV_TAR" ]; then
  ENTRIES+=("lib/install/instantos-dev.tar.gz:$DEV_TAR")
  WANT_DEV=1
else
  echo "note: no $DEV_TAR (tools/make-dev-tarball.sh): the toolchain is not checked"
fi
"$BUILD_DIR/mkInitrd_build/mkInitrd" "$OUT_DIR/initrd.img" "${ENTRIES[@]}" >/dev/null
# The same image whose live boot runs instant-install --update instead.
"$CC" --target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdinc -O1 -DINSTALL_UPDATE \
  -c "$ROOT/tools/install-test/driver.c" -o "$OUT_DIR/driver-update.o"
"$LD" --gc-sections --build-id=none --hash-style=sysv -z max-page-size=0x1000 \
  -pie -e _start --dynamic-linker /lib/ld-instantos.so -o "$OUT_DIR/driver-update" "$OUT_DIR/driver-update.o"
"$BUILD_DIR/mkInitrd_build/mkInitrd" "$OUT_DIR/initrd-update.img" \
  "${ENTRIES[@]/#bin\/login:*/bin/login:$OUT_DIR/driver-update}" >/dev/null

printf '# InstantOS boot settings: the live system\nroot=none\n' > "$OUT_DIR/iboot.cfg"
SKIP_BUILD=1 OUT_DIR="$OUT_DIR" INITRD_FILE="$OUT_DIR/initrd.img" \
  "$ROOT/tools/boot/make-boot-disk.sh" > "$OUT_DIR/make.out"
DISK="$OUT_DIR/disk.img"
ESP_OFF=$((2048 * 512))
P3_OFF=$(sfdisk -d "$DISK" | awk '/ : start=/ {i++; if (i == 3) {gsub(/[ ,=]+/, " "); for (k = 1; k <= NF; k++) if ($k == "start") print $(k + 1) * 512}}')

fail=0
echo "== 1. live boot, install"
KEEP=1 LOG="$OUT_DIR/boot1.log" OUT_DIR="$OUT_DIR" \
  "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALL_FAILED' 1800 || true   # OK reboots: QEMU exits
grep -a "\[storage\] root=none\|\[installtest\]\|instant-install\|^\[[1-6]/6\]\|^      \|Done\.\|root=" "$OUT_DIR/boot1.log" | head -30
grep -aq "INSTALL_OK" "$OUT_DIR/boot1.log" || { echo "install-test: FAIL the install"; fail=1; }
sleep 3   # QEMU gone

echo "== 2. the disk, from the host"
TYPE=$(sfdisk --part-type "$DISK" 3)
echo "partition 3 type: $TYPE"
[ "${TYPE,,}" = "9566fa50-7665-4f44-962e-113fd99cdc87" ] || { echo "install-test: FAIL not the InstantOS type"; fail=1; }
dd if="$DISK" of="$OUT_DIR/p3.img" bs=1M skip=$((P3_OFF / 1048576)) status=none
if e2fsck -fn "$OUT_DIR/p3.img" > "$OUT_DIR/e2fsck.out" 2>&1; then
  echo "e2fsck: clean"
else
  echo "install-test: FAIL e2fsck"; tail -20 "$OUT_DIR/e2fsck.out"; fail=1
fi
debugfs -R "ls -l /" "$OUT_DIR/p3.img" 2>/dev/null | head -20
for f in /.installed /bin/login /lib/ld-instantos.so /test/logs; do
  debugfs -R "stat $f" "$OUT_DIR/p3.img" 2>/dev/null | grep -q "Inode:" || { echo "install-test: FAIL $f missing"; fail=1; }
done
if [ "$WANT_DEV" = 1 ]; then
  debugfs -R "stat /usr/bin/gcc" "$OUT_DIR/p3.img" 2>/dev/null | grep -q "Inode:" || { echo "install-test: FAIL /usr/bin/gcc missing"; fail=1; }
  debugfs -R "stat /lib/install" "$OUT_DIR/p3.img" 2>/dev/null | grep -q "Inode:" && { echo "install-test: FAIL the tarball was copied too"; fail=1; }
fi
rm -f "$OUT_DIR/p3.img"

# What happened on the PC: Windows was made to show the installed partition,
# which retyped it to basic data. It must still be ours, by its ext4 label, for
# the boot below and a reinstall, while p2 (basic data, ext4 under another
# label) must not become ours.
# For the update further down: the user's own files, and a system file gone bad.
P3="$DISK?offset=$P3_OFF"
head -c 300000 /dev/urandom > "$OUT_DIR/keep.bin"
echo "mine" > "$OUT_DIR/mine.txt"
echo "not a program any more" > "$OUT_DIR/broken"
debugfs -w -R "write $OUT_DIR/keep.bin /home/keep.bin" "$P3" >/dev/null 2>&1
debugfs -w -R "write $OUT_DIR/mine.txt /bin/my-own-tool" "$P3" >/dev/null 2>&1
debugfs -w -R "rm /bin/expr" "$P3" >/dev/null 2>&1
debugfs -w -R "write $OUT_DIR/broken /bin/expr" "$P3" >/dev/null 2>&1
# A system installed before /.toolchain existed: its toolchain is unpacked over.
debugfs -w -R "rm /.toolchain" "$P3" >/dev/null 2>&1
BASIC_DATA=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
sfdisk -q --part-type "$DISK" 3 "$BASIC_DATA"
P2_OFF=$(sfdisk -d "$DISK" | awk '/ : start=/ {i++; if (i == 2) {gsub(/[ ,=]+/, " "); for (k = 1; k <= NF; k++) if ($k == "start") print $(k + 1) * 512}}')
mke2fs -q -F -t ext4 -L Other -E offset="$P2_OFF" "$DISK" 32M
dd if="$DISK" bs=1M skip=$((P2_OFF / 1048576)) count=64 status=none | sha256sum > "$OUT_DIR/p2.sha"

echo "== 3. installed boot (partition retyped to basic data)"
printf '# InstantOS boot settings: the installed system\nroot=%s\n' "$ROOT_GUID" > "$OUT_DIR/iboot.cfg"
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/iboot.cfg" ::/EFI/InstantOS/iboot.cfg
KEEP=1 LOG="$OUT_DIR/boot2.log" OUT_DIR="$OUT_DIR" \
  "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALLED_BOOT_OK' 3600 || true
grep -a "\[boot\]\|\[storage\] root=\|\[installtest\]\|^hello from" "$OUT_DIR/boot2.log" | head -30
if [ "$WANT_DEV" = 1 ]; then
  grep -aq "installtest\] ok: DEVTOOLS" "$OUT_DIR/boot2.log" || { echo "install-test: FAIL the toolchain"; fail=1; }
  # The hash of "InstantOS", from the in-OS program linked with -lssl -lcrypto.
  want="$(printf InstantOS | sha256sum | cut -d' ' -f1)"
  if tar tzf "$DEV_TAR" examples/openssl.c >/dev/null 2>&1; then
    grep -a "sha256(InstantOS) = $want" "$OUT_DIR/boot2.log" | head -1 | grep -q . && echo "ok: a program built with -lssl -lcrypto ran" \
      || { echo "install-test: FAIL -lssl"; fail=1; }
  else
    echo "install-test: FAIL the toolchain has no OpenSSL (tools/make-dev-tarball.sh)"; fail=1
  fi
fi
grep -aq "INSTALLED_BOOT_OK" "$OUT_DIR/boot2.log" && grep -aq "\[boot\] installed system" "$OUT_DIR/boot2.log" \
  && ! grep -aq "installtest\] FAIL" "$OUT_DIR/boot2.log" || { echo "install-test: FAIL the installed boot"; fail=1; }

sleep 3
echo "== 4. update it, keeping the user's files"
echo "root=none" > "$OUT_DIR/iboot.cfg"
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/iboot.cfg" ::/EFI/InstantOS/iboot.cfg
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/initrd-update.img" ::/EFI/BOOT/INITRD
KEEP=1 LOG="$OUT_DIR/boot-update.log" OUT_DIR="$OUT_DIR" \
  "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALL_FAILED' 1800 || true
grep -a "\[installtest\]\|^\[[1-3]/3\]\|^      \|Updated\." "$OUT_DIR/boot-update.log" | sort -u | head -20
grep -aq "INSTALL_OK" "$OUT_DIR/boot-update.log" || { echo "install-test: FAIL the update"; fail=1; }
if [ "$WANT_DEV" = 1 ]; then
  grep -aq "unpacking the C/C++ toolchain" "$OUT_DIR/boot-update.log" || { echo "install-test: FAIL the toolchain was not unpacked"; fail=1; }
fi
sleep 3
KEEP=1 LOG="$OUT_DIR/boot-update2.log" OUT_DIR="$OUT_DIR" \
  "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALL_FAILED' 1800 || true
grep -aq "INSTALL_OK" "$OUT_DIR/boot-update2.log" || { echo "install-test: FAIL the second update"; fail=1; }
if [ "$WANT_DEV" = 1 ]; then
  grep -aq "toolchain is this one already" "$OUT_DIR/boot-update2.log" && echo "ok: a second update skips the toolchain" \
    || { echo "install-test: FAIL the toolchain was unpacked again"; fail=1; }
fi
sleep 3
TYPE=$(sfdisk --part-type "$DISK" 3)
[ "${TYPE^^}" = "$BASIC_DATA" ] || { echo "install-test: FAIL the update changed the partition type"; fail=1; }
dd if="$DISK" of="$OUT_DIR/p3.img" bs=1M skip=$((P3_OFF / 1048576)) status=none
e2fsck -fn "$OUT_DIR/p3.img" > "$OUT_DIR/e2fsck-update.out" 2>&1 && echo "e2fsck: clean" \
  || { echo "install-test: FAIL e2fsck after the update"; tail -20 "$OUT_DIR/e2fsck-update.out"; fail=1; }
same() {   # <path on the partition> <host file> <what>
  debugfs -R "dump $1 $OUT_DIR/dumped" "$OUT_DIR/p3.img" >/dev/null 2>&1
  if cmp -s "$OUT_DIR/dumped" "$2"; then echo "ok: $3"; else echo "install-test: FAIL $3"; fail=1; fi
  rm -f "$OUT_DIR/dumped"
}
same /home/keep.bin "$OUT_DIR/keep.bin" "the user's file is untouched"
same /bin/my-own-tool "$OUT_DIR/mine.txt" "the user's own file in /bin is kept"
same /bin/expr "$BUILD_DIR/coreutils/expr" "the bad system file is the image's again"
same /bin/login "$OUT_DIR/driver-update" "a changed system file is the new one"
debugfs -R "cat /.installed" "$OUT_DIR/p3.img" 2>/dev/null
debugfs -R "ls -p /bin" "$OUT_DIR/p3.img" 2>/dev/null | grep -q "instant-new" && { echo "install-test: FAIL temporary files left"; fail=1; }
rm -f "$OUT_DIR/p3.img"

echo "== 5. the updated system boots"
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/initrd.img" ::/EFI/BOOT/INITRD
printf 'root=%s\n' "$ROOT_GUID" > "$OUT_DIR/iboot.cfg"
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/iboot.cfg" ::/EFI/InstantOS/iboot.cfg
KEEP=1 LOG="$OUT_DIR/boot-updated.log" OUT_DIR="$OUT_DIR" \
  "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALLED_BOOT_OK' 3600 || true
grep -a "\[ext4\]\|\[boot\] installed\|installtest\] \(ok\|FAIL\|INSTALLED\)" "$OUT_DIR/boot-updated.log" | head -14
grep -aq "INSTALLED_BOOT_OK" "$OUT_DIR/boot-updated.log" && ! grep -aq "installtest\] FAIL" "$OUT_DIR/boot-updated.log" \
  || { echo "install-test: FAIL the updated system's boot"; fail=1; }

sleep 3
echo "== 6. reinstall over it"
echo "root=none" > "$OUT_DIR/iboot.cfg"
mcopy -o -i "$DISK@@$ESP_OFF" "$OUT_DIR/iboot.cfg" ::/EFI/InstantOS/iboot.cfg
sfdisk -q --part-type "$DISK" 3 "$BASIC_DATA"
KEEP=1 LOG="$OUT_DIR/boot3.log" OUT_DIR="$OUT_DIR"   "$ROOT/tools/boot/run-boot-disk.sh" 'installtest\] INSTALL_FAILED' 1800 || true
grep -a "\[storage\] partitions:\|\[installtest\]\|belongs to another" "$OUT_DIR/boot3.log" | head
grep -aq "INSTALL_OK" "$OUT_DIR/boot3.log" || { echo "install-test: FAIL the reinstall"; fail=1; }
grep -a "\[storage\] partitions:" "$OUT_DIR/boot3.log" | grep -q "disk0p2(instantos"   && { echo "install-test: FAIL another system's ext4 partition was taken for ours"; fail=1; }
sleep 3
dd if="$DISK" bs=1M skip=$((P2_OFF / 1048576)) count=64 status=none | sha256sum | cmp -s - "$OUT_DIR/p2.sha"   || { echo "install-test: FAIL partition 2 changed"; fail=1; }

if [ "$fail" = 0 ]; then echo "install-test: PASS"; else echo "install-test: FAIL"; fi
exit "$fail"
