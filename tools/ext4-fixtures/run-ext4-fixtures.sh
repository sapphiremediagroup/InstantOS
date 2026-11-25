#!/usr/bin/env bash
# Build the golden ext2/ext4 images, compile the host Ext4FS read-path tests
# against the real kernel driver (src/fs/ext4/ext4.cpp), and run them.
#
# Requires e2fsprogs (mke2fs) to build the images and a host C++ compiler. It
# runs entirely on the host - no QEMU or kernel build - so it is cheap enough
# for CI on every change to the ext4 driver.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/ext4-fixtures}"
CXX="${CXX:-clang++}"

if ! command -v mke2fs >/dev/null 2>&1; then
  printf 'error: mke2fs (e2fsprogs) not found\n' >&2
  exit 2
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
  printf 'error: host C++ compiler "%s" not found (set CXX=...)\n' "$CXX" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"

printf 'building golden ext2/ext4 images\n'
bash "$ROOT/tools/ext4-fixtures/make-images.sh"

printf 'building ext4 read tests with %s\n' "$CXX"
"$CXX" -std=c++23 -O1 -Wall -Wextra -I "$ROOT/include" \
  "$ROOT/tools/ext4-fixtures/ext4_read_tests.cpp" \
  "$ROOT/src/fs/ext4/ext4.cpp" \
  -o "$BUILD_DIR/ext4_read_tests"

printf 'running ext4 read/write tests\n'
"$BUILD_DIR/ext4_read_tests" "$BUILD_DIR"

# The read/write tests mutate the writable images; confirm the on-disk result
# is consistent according to the reference implementation.
for img in ext2w.img ext4w.img ext4c.img ext4j.img ext4j2.img ext4fuzz.img ext4fuzzc.img ext4fuzzj.img ext4hw.img; do
  printf 'e2fsck -fn %s\n' "$img"
  e2fsck -fn "$BUILD_DIR/$img"
done

# Journal cross-checks: Linux must be able to replay the journals the driver
# wrote (crafted recovery journal + write-side transactions) and end consistent.
for img in ext4j_dirty.img ext4j2_dirty.img ext4fuzzj_crash.img; do
  if [ -f "$BUILD_DIR/$img" ]; then
    printf 'e2fsck -fy %s (journal replay cross-check)\n' "$img"
    e2fsck -fy "$BUILD_DIR/$img"
  fi
done
