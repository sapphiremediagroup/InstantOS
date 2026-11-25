#!/usr/bin/env bash
# Cross-build GNU make against the InstantOS mlibc sysroot.
#
# Produces a dynamically-linked PIE `make` that uses /lib/mlibc/ld-instantos.so
# and links the mlibc runtime. make pulls in gnulib, which probes libc behavior
# that cannot be discovered by running test binaries on a non-Linux freestanding
# target; config.cache primes those answers (same approach as build-coreutils.sh).
#
# Prerequisite: tools/build-mlibc.sh first.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
MK_VERSION="${MK_VERSION:-make-4.4.1}"
MK_URL="${MK_URL:-https://ftp.gnu.org/gnu/make/${MK_VERSION}.tar.gz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/make-port}"
SRC_DIR="$WORK_DIR/$MK_VERSION"
OBJ_DIR="$WORK_DIR/build"
OUT="${OUT:-$BUILD_DIR/make}"
CC="${CC:-clang}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -Wno-implicit-function-declaration -Wno-int-conversion -fcommon"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--allow-multiple-definition $MLIBC_ROOT/lib/crt1.o"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$MK_URL"
  curl -sL "$MK_URL" -o "$WORK_DIR/$MK_VERSION.tar.gz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$MK_VERSION.tar.gz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# Cross-compile cache: gnulib/autoconf answers for the InstantOS+mlibc runtime.
cat > config.cache <<'EOF'
ac_cv_func_malloc_0_nonnull=yes
ac_cv_func_realloc_0_nonnull=yes
gl_cv_func_malloc_0_nonnull=1
ac_cv_func_working_mktime=yes
gl_cv_func_working_mkstemp=yes
ac_cv_func_mmap_fixed_mapped=yes
gl_cv_func_mmap_anon=yes
ac_cv_func_fork_works=yes
ac_cv_func_vfork_works=yes
ac_cv_func_wait3=no
ac_cv_func_strerror_r_char_p=yes
gl_cv_func_getcwd_path_max=yes
gl_cv_func_getcwd_abort_bug=no
gl_cv_func_fnmatch_posix=yes
gl_cv_func_stat_dir_slash=yes
gl_cv_func_stat_file_slash=yes
gl_cv_func_lstat_dereferences_slashed_symlink=yes
gl_cv_func_unlink_honors_slashes=yes
ac_cv_func_setvbuf_reversed=no
make_cv_sys_gnu_glob=no
EOF

CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="-lc" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --without-guile \
  --disable-nls \
  --disable-dependency-tracking

make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS"

# make 4.x builds ./make (or src/make depending on version layout).
if   [ -f "$OBJ_DIR/make" ];     then MK_BIN="$OBJ_DIR/make"
elif [ -f "$OBJ_DIR/src/make" ]; then MK_BIN="$OBJ_DIR/src/make"
else printf 'make binary not found after build\n' >&2; exit 1; fi

cp "$MK_BIN" "$OUT"
"${LLVM_STRIP:-llvm-strip}" --strip-all "$OUT" 2>/dev/null || true
printf 'make built: %s\n' "$OUT"
ls -l "$OUT"
