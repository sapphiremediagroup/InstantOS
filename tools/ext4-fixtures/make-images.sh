#!/usr/bin/env bash
# Build golden ext2/ext4 images for the host-side Ext4FS read-path tests.
#
# Uses `mke2fs -d` to populate a directory tree into a fresh image WITHOUT
# mounting it (no root/loop device needed), so it runs unprivileged (incl. in
# WSL). Images are written to build/ext4-fixtures/ under the repo root and are
# consumed by tools/ext4-fixtures/ext4_read_tests.cpp.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/ext4-fixtures"
mkdir -p "$OUT"

SRC="$(mktemp -d)"
trap 'rm -rf "$SRC"' EXIT
mkdir -p "$SRC/dir"

printf 'Hello, ext4 from InstantOS!\n' > "$SRC/hello.txt"
printf 'nested file contents\n' > "$SRC/dir/nested.txt"

# Deterministic 400000-byte pattern: byte[i] = i % 251. Large enough that with
# 1 KiB blocks (ext2) it spans direct + single + double indirect blocks, and
# with 4 KiB blocks (ext4) it forms a multi-block extent.
if command -v python3 >/dev/null 2>&1; then
  python3 -c 'import sys; sys.stdout.buffer.write(bytes(i%251 for i in range(400000)))' > "$SRC/big.bin"
else
  perl -e 'for($i=0;$i<400000;$i++){print chr($i%251)}' > "$SRC/big.bin"
fi

ln -s hello.txt "$SRC/link"

# A directory with many entries forces HTree indexing (dir_index) on the
# default-feature image, and is a large multi-block linear directory (resolved
# via indirect blocks / extents) on the others.
mkdir -p "$SRC/many"
for i in $(seq 0 599); do printf '%s\n' "$i" > "$SRC/many/file_$i"; done

# A slow (out-of-line) symlink: the target is >= 60 bytes so it is stored in a
# data block instead of inline in i_block. Keep this string in sync with the
# EXPECTED value in ext4_read_tests.cpp.
ln -s "/an/intentionally/long/symlink/target/path/that/exceeds/sixty/characters" "$SRC/slowlink"

# A 1024-byte file (exactly one 1 KiB block) whose single data block the
# journal-recovery test overwrites via a crafted JBD2 transaction.
head -c 1024 /dev/zero | tr '\0' 'O' > "$SRC/journaltest.txt"

# Everything not needed by the phase-1 read path is turned off so the images
# exercise exactly one variable each (extent vs block-map) in an otherwise
# simple, fully-supported configuration.
COMMON_OFF="^has_journal,^metadata_csum,^dir_index,^64bit,^flex_bg"

# ext4: extent-tree block resolution.
mke2fs -q -F -b 4096 -O "extent,filetype,sparse_super,$COMMON_OFF" \
  -E lazy_itable_init=0 -d "$SRC" "$OUT/ext4.img" 8192

# ext2: classic block map (12 direct + single/double/triple indirect).
mke2fs -q -F -b 1024 -O "^extent,filetype,sparse_super,$COMMON_OFF" \
  -E lazy_itable_init=0 -d "$SRC" "$OUT/ext2.img" 32768

# ext4 with the full default real-world feature set (64bit, flex_bg,
# metadata_csum, dir_index/HTree, has_journal, extent, ...). Exercises 64-byte
# group descriptors, flex_bg inode-table placement, HTree directories, and slow
# symlinks in a single image.
mke2fs -q -F -b 4096 -t ext4 -E lazy_itable_init=0 -d "$SRC" "$OUT/ext4def.img" 32768

# Writable test targets. metadata_csum / uninit_bg are OFF and inodes are 128
# bytes, so the driver's writes stay e2fsck-clean without recomputing on-disk
# checksums. ext2w exercises the block map natively; ext4w confirms that
# block-mapped files created by the driver also pass fsck on an extent volume.
WRITE_OFF="^has_journal,^metadata_csum,^uninit_bg,^dir_index,^64bit,^flex_bg,^resize_inode"
mke2fs -q -F -b 1024 -I 128 -O "^extent,filetype,sparse_super,$WRITE_OFF" \
  -E lazy_itable_init=0 -d "$SRC" "$OUT/ext2w.img" 32768
mke2fs -q -F -b 4096 -I 128 -O "extent,filetype,sparse_super,$WRITE_OFF" \
  -E lazy_itable_init=0 -d "$SRC" "$OUT/ext4w.img" 8192

# metadata_csum write target: full DEFAULT ext4 feature set (metadata_csum,
# 64bit, flex_bg, 256-byte inodes). The driver maintains crc32c on every
# metadata write; e2fsck must still pass.
mke2fs -q -F -b 4096 -t ext4 -E lazy_itable_init=0 -d "$SRC" "$OUT/ext4c.img" 16384

# Journal-recovery target: has_journal, but metadata_csum/64bit OFF so the JBD2
# journal uses the simplest (unchecksummed, 32-bit) transaction format.
mke2fs -q -F -b 1024 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index" \
  -E lazy_itable_init=0 -d "$SRC" "$OUT/ext4j.img" 16384
# Independent copy for the write-side journaling test.
cp "$OUT/ext4j.img" "$OUT/ext4j2.img"

# Random-op fuzzer targets (P1.4): hammered with a randomized create/write/
# truncate/chmod/rename/unlink sequence, then e2fsck must be clean.
#  - ext4fuzz.img : simple extent volume, 128-byte inodes, no csum.
#  - ext4fuzzc.img: full DEFAULT ext4 (metadata_csum/64bit/flex_bg) csum path.
mke2fs -q -F -b 4096 -I 128 -O "extent,filetype,sparse_super,$WRITE_OFF" \
  -E lazy_itable_init=0 "$OUT/ext4fuzz.img" 16384
mke2fs -q -F -b 4096 -t ext4 -E lazy_itable_init=0 "$OUT/ext4fuzzc.img" 16384

# Journaled fuzz target in the InstantOS disk format (4 KiB blocks, a simple
# unchecksummed JBD2 journal, as tools/os-testsuite and gcchost make them):
# exercises the driver's batched transactions.
mke2fs -q -F -b 4096 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index"   -E lazy_itable_init=0 "$OUT/ext4fuzzj.img" 16384

# HTree write target: a copy of the full-featured default image (which has the
# 600-entry HTree `many/` directory). The driver converts HTree dirs to linear
# on write; e2fsck must accept the result.
cp "$OUT/ext4def.img" "$OUT/ext4hw.img"

echo "built:"
ls -l "$OUT"/*.img
