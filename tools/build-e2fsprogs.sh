#!/usr/bin/env bash
# e2fsprogs' mke2fs and e2fsck for InstantOS (mlibc), for instant-install and
# for checking an installed root. Built as the other ports are (a cross
# configure against the mlibc sysroot with a config.cache for the probes that
# cannot run here), with e2fsprogs' own libuuid and libblkid (the sysroot has
# neither; configure's link tests pass by accident there), static, dynamically
# linked to mlibc.
#
#   tools/build-e2fsprogs.sh        -> build/e2fsprogs/{mke2fs,e2fsck}
# The tree lives in WSL (WORK_DIR, default ~/e2fsprogs-port): drvfs is slow.
# Prereq: tools/build-mlibc.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
CC="${CC:-clang}"
VER="${E2FS_VERSION:-1.47.1}"
URL="${URL:-https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v$VER/e2fsprogs-$VER.tar.gz}"
WORK_DIR="${WORK_DIR:-$HOME/e2fsprogs-port}"
SRC_DIR="$WORK_DIR/e2fsprogs-$VER"
OBJ_DIR="$WORK_DIR/build"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/e2fsprogs}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  echo "mlibc sysroot missing ($MLIBC_ROOT); run tools/build-mlibc.sh first" >&2
  exit 2
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -O2 -Wno-implicit-function-declaration -Wno-int-conversion -fcommon"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc $MLIBC_ROOT/lib/crt1.o -lc"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  echo "fetching $URL"
  curl -sfL "$URL" -o "$WORK_DIR/e2fsprogs-$VER.tar.gz"
  tar -C "$WORK_DIR" -xzf "$WORK_DIR/e2fsprogs-$VER.tar.gz"
fi

rm -rf "$OBJ_DIR"; mkdir -p "$OBJ_DIR"; cd "$OBJ_DIR"
cat > config.cache <<'EOF'
ac_cv_func_malloc_0_nonnull=yes
ac_cv_func_realloc_0_nonnull=yes
ac_cv_func_mmap_fixed_mapped=yes
ac_cv_func_setvbuf_reversed=no
ac_cv_func_fork_works=yes
ac_cv_func_vfork_works=yes
ac_cv_have_decl_PR_SET_DUMPABLE=no
ac_cv_func_prctl=no
ac_cv_func_fallocate=no
ac_cv_func_fallocate64=no
ac_cv_func_sync_file_range=no
ac_cv_func_fadvise64=no
ac_cv_func_posix_fadvise=no
ac_cv_func_posix_fadvise64=no
ac_cv_func_mallinfo=no
ac_cv_func_mallinfo2=no
ac_cv_header_linux_fs_h=no
ac_cv_header_linux_fd_h=no
ac_cv_header_linux_falloc_h=no
ac_cv_header_linux_fsmap_h=no
ac_cv_header_linux_major_h=no
ac_cv_header_linux_loop_h=no
ac_cv_header_sys_mount_h=no
ac_cv_header_mntent_h=no
ac_cv_func_getmntinfo=no
ac_cv_func_setmntent=no
ac_cv_lib_uuid_uuid_generate=no
ac_cv_lib_blkid_blkid_get_cache=no
ac_cv_lib_blkid_blkid_probe_get_topology=no
ac_cv_lib_blkid_blkid_topology_get_dax=no
ac_cv_lib_blkid_blkid_probe_enable_partitions=no
EOF

CC="$CC" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" LIBS="-lc" \
BUILD_CC=gcc BUILD_CFLAGS="-O2" BUILD_LDFLAGS="" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf --build=x86_64-linux-gnu --cache-file=config.cache \
  --enable-libuuid --enable-libblkid --disable-nls --disable-fuse2fs --disable-e2initrd-helper --disable-defrag \
  --disable-uuidd --disable-fsck --disable-tdb --disable-imager --disable-resizer \
  --disable-debugfs --disable-elf-shlibs --disable-bsd-shlibs --disable-mmp \
  --disable-tls --disable-backtrace --disable-largefile --without-libarchive \
  --without-pthread --disable-threads --with-udev-rules-dir=no --with-crond-dir=no \
  --with-systemd-unit-dir=no

# Only the libraries, mke2fs and e2fsck.
make -j"$(nproc)" libs
make -j"$(nproc)" -C misc mke2fs
make -j"$(nproc)" -C e2fsck e2fsck

mkdir -p "$OUT_DIR"
cp misc/mke2fs e2fsck/e2fsck "$OUT_DIR/"
"${LLVM_STRIP:-llvm-strip}" --strip-all "$OUT_DIR/mke2fs" "$OUT_DIR/e2fsck" 2>/dev/null || true
cp "$SRC_DIR/misc/mke2fs.conf.in" "$OUT_DIR/mke2fs.conf" 2>/dev/null || true
echo "e2fsprogs built: $OUT_DIR/mke2fs $OUT_DIR/e2fsck"
