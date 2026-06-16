#!/usr/bin/env bash
# Cross-build GNU bash against the InstantOS mlibc sysroot.
#
# Produces a dynamically-linked PIE bash that uses /lib/mlibc/ld-instantos.so
# and links against the mlibc runtime (libc.so, libdl.so). All of bash's
# dynamic symbols resolve against the mlibc port.
#
# Prerequisites: run tools/build-mlibc.sh first (with glibc_option enabled, as
# this script's companion change to build-mlibc.sh ensures).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
BASH_VERSION_TAG="${BASH_VERSION_TAG:-bash-5.2.21}"
BASH_TARBALL_URL="${BASH_TARBALL_URL:-https://ftp.gnu.org/gnu/bash/${BASH_VERSION_TAG}.tar.gz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/bash-port}"
SRC_DIR="$WORK_DIR/$BASH_VERSION_TAG"
OBJ_DIR="$WORK_DIR/build"
OUTPUT="${OUTPUT:-$BUILD_DIR/bash}"
CC="${CC:-clang}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
if [ ! -f "$MLIBC_ROOT/include/sys/ioctl.h" ]; then
  printf 'mlibc is missing glibc headers (sys/ioctl.h); rebuild mlibc with glibc_option enabled\n' >&2
  exit 2
fi

# Target (mlibc, freestanding, PIE) compile/link flags.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -Wno-implicit-function-declaration -fcommon"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--allow-multiple-definition $MLIBC_ROOT/lib/crt1.o"
# Host build-tool flags: bash builds mkbuiltins/mksignames on the host; gcc>=14
# defaults to C23 which rejects bash's K&R prototypes.
BUILD_CFLAGS="-g -DCROSS_COMPILING -std=gnu89 -Wno-implicit-function-declaration -Wno-implicit-int"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$BASH_TARBALL_URL"
  curl -sL "$BASH_TARBALL_URL" -o "$WORK_DIR/$BASH_VERSION_TAG.tar.gz"
  tar -C "$WORK_DIR" -xzf "$WORK_DIR/$BASH_VERSION_TAG.tar.gz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# Autoconf answers that cannot be probed when cross-compiling to a freestanding
# target that cannot run test binaries on the host.
cat > config.cache <<'EOF'
ac_cv_func_setvbuf_reversed=no
bash_cv_termcap_lib=gnutermcap
ac_cv_c_long_double=yes
bash_cv_have_mbstate_t=yes
bash_cv_func_sigsetjmp=present
bash_cv_unusable_rtsigs=no
ac_cv_func_mmap_fixed_mapped=yes
bash_cv_getcwd_malloc=yes
bash_cv_job_control_missing=present
bash_cv_sys_named_pipes=present
bash_cv_func_strcoll_broken=no
bash_cv_dup2_broken=no
bash_cv_pgrp_pipe=no
bash_cv_signal_vintage=posix
bash_cv_must_reinstall_sighandlers=no
bash_cv_func_ctype_nonascii=yes
bash_cv_wcwidth_broken=no
ac_cv_func_working_mktime=yes
gt_cv_int_divbyzero_sigfpe=no
ac_cv_c_undeclared_builtin_options=none needed
EOF

CC="$CC" \
CFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="-lc" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --without-bash-malloc \
  --disable-nls

make -j"$(nproc)" \
  CFLAGS="$TARGET_CFLAGS" \
  CFLAGS_FOR_BUILD="$BUILD_CFLAGS" \
  LDFLAGS="$TARGET_LDFLAGS"

cp "$OBJ_DIR/bash" "$OUTPUT"
"${LLVM_STRIP:-llvm-strip}" --strip-all "$OUTPUT" 2>/dev/null || true
printf 'bash built: %s\n' "$OUTPUT"
"${LLVM_NM:-llvm-nm}" -D -u "$OUTPUT" >/dev/null 2>&1 || true
printf 'interpreter: /lib/mlibc/ld-instantos.so (links libc.so, libdl.so)\n'
