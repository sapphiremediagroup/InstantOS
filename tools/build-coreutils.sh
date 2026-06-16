#!/usr/bin/env bash
# Cross-build GNU coreutils against the InstantOS mlibc sysroot.
#
# Produces dynamically-linked PIE coreutils binaries that use
# /lib/mlibc/ld-instantos.so and link against the mlibc runtime. coreutils pulls
# in gnulib, which probes a large surface of libc behavior; cross-compiling to a
# non-Linux freestanding target requires priming config.cache with answers that
# cannot be discovered by running test binaries.
#
# Prerequisites: tools/build-mlibc.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
CU_VERSION="${CU_VERSION:-coreutils-9.5}"
CU_URL="${CU_URL:-https://ftp.gnu.org/gnu/coreutils/${CU_VERSION}.tar.xz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/coreutils-port}"
SRC_DIR="$WORK_DIR/$CU_VERSION"
OBJ_DIR="$WORK_DIR/build"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/coreutils}"
CC="${CC:-clang}"

# A small, dependency-light set to validate the sysdeps we have. Expanded as
# more sysdeps land (df/stat -f need statfs, chown needs ownership, etc.).
PROGRAMS="${PROGRAMS:-echo cat ls pwd mkdir rmdir rm mv ln cp touch chmod true false head wc env stat df chown chgrp id mkfifo mknod shuf mktemp sort whoami sleep uptime who users}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -DO_BINARY=0 -DO_TEXT=0 -Wno-implicit-function-declaration -Wno-int-conversion -fcommon"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--allow-multiple-definition $MLIBC_ROOT/lib/crt1.o"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$CU_URL"
  curl -sL "$CU_URL" -o "$WORK_DIR/$CU_VERSION.tar.xz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$CU_VERSION.tar.xz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# Cross-compile cache: answers gnulib/autoconf cannot probe by running binaries
# on a non-Linux freestanding target. These describe the InstantOS+mlibc runtime.
cat > config.cache <<'EOF'
ac_cv_func_malloc_0_nonnull=yes
ac_cv_func_realloc_0_nonnull=yes
gl_cv_func_malloc_0_nonnull=1
ac_cv_func_working_mktime=yes
gl_cv_func_working_mkstemp=yes
ac_cv_func_mmap_fixed_mapped=yes
gl_cv_func_mmap_anon=yes
ac_cv_func_chown_works=yes
ac_cv_func_setvbuf_reversed=no
gl_cv_func_fnmatch_posix=yes
ac_cv_func_fork_works=yes
ac_cv_func_strerror_r_char_p=no
gl_cv_func_getcwd_path_max=yes
gl_cv_func_getcwd_abort_bug=no
ac_cv_func_getgroups_works=yes
gl_cv_func_link_works=yes
gl_cv_func_symlink_works=yes
gl_cv_func_rename_dest_works=yes
gl_cv_func_rename_link_works=yes
gl_cv_func_stat_dir_slash=yes
gl_cv_func_stat_file_slash=yes
gl_cv_func_lstat_dereferences_slashed_symlink=yes
gl_cv_func_unlink_honors_slashes=yes
gl_cv_func_utimensat_works=yes
gl_cv_func_futimens_works=yes
gl_cv_func_select_supports0=yes
ac_cv_header_utmp_h=no
ac_cv_header_utmpx_h=yes
ac_cv_member_struct_utmpx_ut_type=yes
ac_cv_member_struct_utmpx_ut_pid=yes
ac_cv_member_struct_utmpx_ut_id=yes
ac_cv_member_struct_utmpx_ut_user=yes
ac_cv_member_struct_utmpx_ut_host=yes
ac_cv_member_struct_utmpx_ut_tv=yes
ac_cv_member_struct_utmpx_ut_session=yes
ac_cv_func_getutxent=yes
EOF

CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="-lc" \
FORCE_UNSAFE_CONFIGURE=1 \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --disable-nls \
  --disable-acl \
  --disable-xattr \
  --disable-libcap \
  --enable-no-install-program=stdbuf \
  --enable-install-program="$(echo "$PROGRAMS" | tr ' ' ',')"

# Generate gnulib's BUILT_SOURCES first (configured headers such as unictype.h,
# the *.in.h -> *.h replacements, configmake.h). With `make -j` and a named
# target, automake does not run these GEN rules before the parallel compiles, so
# build them explicitly up front. Expand the BUILT_SOURCES make variable via an
# auxiliary makefile so $(...) references are resolved by make itself.
printf 'print-bs:\n\t@echo $(BUILT_SOURCES)\n' > .print-bs.mk
BUILT_SOURCES="$(make -f Makefile -f .print-bs.mk print-bs 2>/dev/null)"
if [ -n "$BUILT_SOURCES" ]; then
  make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" $BUILT_SOURCES || true
fi

make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" lib/libcoreutils.a || true
# Build only the requested programs (coreutils compiles every src/* otherwise,
# and some programs depend on sysdeps we have not implemented yet, e.g. statfs).
CU_TARGETS=""
for p in $PROGRAMS; do CU_TARGETS="$CU_TARGETS src/$p"; done
make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" $CU_TARGETS || {
  printf '\ncoreutils make failed (expected while sysdeps are incomplete)\n' >&2
  exit 1
}

mkdir -p "$OUT_DIR"
for p in $PROGRAMS; do
  if [ -f "$OBJ_DIR/src/$p" ]; then
    cp "$OBJ_DIR/src/$p" "$OUT_DIR/$p"
    "${LLVM_STRIP:-llvm-strip}" --strip-all "$OUT_DIR/$p" 2>/dev/null || true
  fi
done
printf 'coreutils built into: %s\n' "$OUT_DIR"
ls -1 "$OUT_DIR"
