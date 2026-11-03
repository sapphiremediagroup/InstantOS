#!/usr/bin/env bash
# Cross-build a GNU autotools+gnulib tool (gzip/sed/grep/tar) against the
# InstantOS mlibc sysroot, producing a dynamically-linked PIE binary that uses
# /lib/mlibc/ld-instantos.so. Same approach as build-coreutils.sh/build-make.sh:
# a config.cache primes the gnulib/autoconf probes that cannot be run on a
# non-Linux freestanding target.
#
# Usage:  build-gnu-tool.sh <gzip|sed|grep|tar>
# Prereq: tools/build-mlibc.sh first.
set -euo pipefail

PKG="${1:?usage: build-gnu-tool.sh <gzip|sed|grep|tar>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
CC="${CC:-clang}"

case "$PKG" in
  gzip) VER="gzip-1.13"; URLBASE="gzip"; BINPATHS="gzip";        XARGS="" ;;
  sed)  VER="sed-4.9";   URLBASE="sed";  BINPATHS="sed/sed";     XARGS="" ;;
  grep) VER="grep-3.11"; URLBASE="grep"; BINPATHS="src/grep";    XARGS="--disable-perl-regexp" ;;
  tar)  VER="tar-1.35";  URLBASE="tar";  BINPATHS="src/tar";     XARGS="--without-selinux --disable-acl --disable-xattrs --without-posix-acls" ;;
  awk)  VER="gawk-5.3.0"; URLBASE="gawk"; BINPATHS="gawk";       XARGS="--disable-extensions --disable-mpfr" ;;
  cmp)  VER="diffutils-3.10"; URLBASE="diffutils"; BINPATHS="src/cmp"; XARGS=""; URL="${URL:-https://ftp.gnu.org/gnu/diffutils/${VER}.tar.xz}" ;;
  diff) VER="diffutils-3.10"; URLBASE="diffutils"; BINPATHS="src/diff"; XARGS=""; URL="${URL:-https://ftp.gnu.org/gnu/diffutils/${VER}.tar.xz}" ;;
  m4)   VER="m4-1.4.19"; URLBASE="m4";   BINPATHS="src/m4";      XARGS="" ;;
  *) echo "unknown package: $PKG" >&2; exit 2 ;;
esac
URL="${URL:-https://ftp.gnu.org/gnu/${URLBASE}/${VER}.tar.gz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/${PKG}-port}"
SRC_DIR="$WORK_DIR/$VER"
OBJ_DIR="$WORK_DIR/build"
OUT="${OUT:-$BUILD_DIR/$PKG}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -Wno-implicit-function-declaration -Wno-int-conversion -fcommon"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--allow-multiple-definition $MLIBC_ROOT/lib/crt1.o"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$URL"
  curl -sL "$URL" -o "$WORK_DIR/$VER.tar.gz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$VER.tar.gz"
fi

rm -rf "$OBJ_DIR"; mkdir -p "$OBJ_DIR"; cd "$OBJ_DIR"

# gnulib/autoconf answers for the InstantOS+mlibc runtime (superset).
cat > config.cache <<'EOF'
ac_cv_func_malloc_0_nonnull=yes
# iconv.h in the sysroot is NetSurf's shim (its library links into NetSurf only).
am_cv_func_iconv=no
am_cv_lib_iconv=no
ac_cv_func_realloc_0_nonnull=yes
gl_cv_func_malloc_0_nonnull=1
ac_cv_func_working_mktime=yes
gl_cv_func_working_mkstemp=yes
ac_cv_func_mmap_fixed_mapped=yes
gl_cv_func_mmap_anon=yes
ac_cv_func_fork_works=yes
ac_cv_func_vfork_works=yes
ac_cv_func_strerror_r_char_p=no
ac_cv_func_setvbuf_reversed=no
gl_cv_func_getcwd_path_max=yes
gl_cv_func_getcwd_abort_bug=no
gl_cv_func_fnmatch_posix=yes
gl_cv_func_stat_dir_slash=yes
gl_cv_func_stat_file_slash=yes
gl_cv_func_lstat_dereferences_slashed_symlink=yes
gl_cv_func_unlink_honors_slashes=yes
gl_cv_func_link_works=yes
gl_cv_func_symlink_works=yes
gl_cv_func_rename_dest_works=yes
gl_cv_func_rename_link_works=yes
gl_cv_func_utimensat_works=yes
gl_cv_func_futimens_works=yes
gl_cv_func_select_supports0=yes
gl_cv_func_gettimeofday_clobber=no
gl_cv_func_tzset_clobber=no
gl_cv_func_working_strerror=yes
gl_cv_func_re_compile_pattern_working=no
ac_cv_header_utmp_h=no
ac_cv_header_utmpx_h=yes
EOF

CC="$CC" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" LIBS="-lc" \
FORCE_UNSAFE_CONFIGURE=1 \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --disable-nls \
  --disable-dependency-tracking \
  $XARGS

make -j"$(nproc)" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS"

MK_BIN=""
for c in $BINPATHS; do [ -f "$OBJ_DIR/$c" ] && MK_BIN="$OBJ_DIR/$c" && break; done
[ -n "$MK_BIN" ] || { printf '%s binary not found (looked for: %s)\n' "$PKG" "$BINPATHS" >&2; exit 1; }

cp "$MK_BIN" "$OUT"
"${LLVM_STRIP:-llvm-strip}" --strip-all "$OUT" 2>/dev/null || true
printf '%s built: %s\n' "$PKG" "$OUT"
ls -l "$OUT"
