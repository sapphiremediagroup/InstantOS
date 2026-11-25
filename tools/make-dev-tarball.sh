#!/usr/bin/env bash
# The C/C++ toolchain of the dev disk (tools/make-dev-disk.sh: gcc 13.3.0,
# binutils, mlibc's headers and libraries, libstdc++, make, the GNU tools under
# /usr, plus /examples) as one tarball for an installed InstantOS, and OpenSSL's
# headers with what -lssl -lcrypto need (tools/build-openssl.sh; the libraries
# themselves are the system's, in /lib/mlibc).
#
#   tools/make-dev-tarball.sh [OUT]   (default build/instantos-dev.tar.gz; run in WSL)
#
# tools/make-pc-initrd.sh puts it in the PC's INITRD and instant-install unpacks
# it. By hand, in InstantOS: tar xzf instantos-dev.tar.gz -C /
#
# The image read is DEV_IMG (default
# build/dev-disk.img: an MBR disk whose ext4 volume starts at 1 MiB, or a
# bare ext4 volume; make it first if missing).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/instantos-dev.tar.gz}"
DEV_IMG="${DEV_IMG:-$ROOT/build/dev-disk.img}"
[ -f "$DEV_IMG" ] || { echo "missing $DEV_IMG (tools/make-dev-disk.sh)" >&2; exit 2; }
# Stage in the Linux file system: on /mnt/d (drvfs) files lose their modes.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/devtar.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"
VOLUME="$DEV_IMG"
debugfs -R stats "$VOLUME" 2>/dev/null | grep -q "^Block count" || VOLUME="$DEV_IMG?offset=1048576"
debugfs -R "rdump /usr /examples $WORK/root" "$VOLUME" >/dev/null 2>&1
[ -x "$WORK/root/usr/bin/gcc" ] || { echo "no /usr/bin/gcc on $DEV_IMG" >&2; exit 2; }
# The GNU tools the system's /bin already has (the initrd's, see CMakeLists.txt)
# are left out: the dev disk's copies are older builds, and /usr/bin comes
# first on bash's PATH.
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
for t in $(ls "$BUILD_DIR/coreutils" 2>/dev/null) tar gzip make sed grep awk gawk diff cmp m4 egrep fgrep; do
  if [ -e "$BUILD_DIR/coreutils/$t" ] || [ -e "$BUILD_DIR/$t" ] || [ "$t" = gawk ] || [ "$t" = egrep ] || [ "$t" = fgrep ]; then
    rm -f "$WORK/root/usr/bin/$t"
  fi
done
# OpenSSL: linker scripts naming the system's libraries, so a program links
# what it will run with. (libssl needs libcrypto: both are named.)
if [ -d "$BUILD_DIR/mlibc-root/include/openssl" ] && [ -f "$BUILD_DIR/openssl/libssl.so.3" ]; then
  cp -r "$BUILD_DIR/mlibc-root/include/openssl" "$WORK/root/usr/include/openssl"
  echo 'INPUT(/lib/mlibc/libssl.so.3 AS_NEEDED(/lib/mlibc/libcrypto.so.3))' > "$WORK/root/usr/lib/libssl.so"
  echo 'INPUT(/lib/mlibc/libcrypto.so.3)' > "$WORK/root/usr/lib/libcrypto.so"
  cp "$ROOT/tools/openssl-smoke/example.c" "$WORK/root/examples/openssl.c"
else
  echo "note: OpenSSL is not built (tools/build-openssl.sh): no -lssl in the toolchain" >&2
fi
chmod -R u=rwX,go=rX "$WORK/root/usr/include" "$WORK/root/examples"
tar -C "$WORK/root" --owner=0 --group=0 --numeric-owner -czf "$WORK/out.tar.gz" usr examples
cp "$WORK/out.tar.gz" "$OUT"
echo "$OUT: $(du -h "$OUT" | cut -f1) ($(du -sh "$WORK/root" | cut -f1) unpacked)"
