#!/usr/bin/env bash
# Cross-build the curl command-line tool for InstantOS: a dynamically linked
# mlibc PIE with libcurl built in statically, installed to build/curl/curl.
# CMake bundles it as /bin/curl when it exists (rerun cmake after building).
#
# TLS: with mbedTLS in the mlibc sysroot (tools/build-mbedtls.sh) curl speaks
# https://, otherwise it is built --without-ssl and does http:// only. zlib
# (tools/build-zlib.sh) is used for --compressed when present.
#
# Prerequisites: tools/build-mlibc.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
CURL_VERSION="${CURL_VERSION:-curl-8.11.1}"
CURL_URL="${CURL_URL:-https://curl.se/download/${CURL_VERSION}.tar.xz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/curl-port}"
SRC_DIR="$WORK_DIR/$CURL_VERSION"
OBJ_DIR="$WORK_DIR/cli-build"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/curl}"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
RANLIB="${RANLIB:-llvm-ranlib}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

TLS_ARGS=(--without-ssl)
LIBS="-lc"
TLS_NAME="none (http only)"
if [ -f "$MLIBC_ROOT/lib/libmbedtls.a" ]; then
  TLS_ARGS=(--with-mbedtls="$MLIBC_ROOT" --without-ca-path --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt)
  LIBS="-lmbedtls -lmbedx509 -lmbedcrypto $LIBS"
  TLS_NAME="mbedTLS"
fi
ZLIB_ARGS=(--without-zlib)
if [ -f "$MLIBC_ROOT/lib/libz.a" ]; then
  ZLIB_ARGS=(--with-zlib="$MLIBC_ROOT")
  LIBS="-lz $LIBS"
fi

TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIE -fno-stack-protector -O2 -nostdlibinc -isystem $MLIBC_ROOT/include -I$MLIBC_ROOT/include -D_GNU_SOURCE"
[ "$TLS_NAME" = "mbedTLS" ] && TARGET_CFLAGS="$TARGET_CFLAGS -DMBEDTLS_CONFIG_FILE='<instantos_mbedtls_config.h>'"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc"
# crt1.o only for the executable: libtool refuses it for the library.
BIN_LDFLAGS="$TARGET_LDFLAGS $MLIBC_ROOT/lib/crt1.o"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$CURL_URL"
  curl -sL "$CURL_URL" -o "$WORK_DIR/$CURL_VERSION.tar.xz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$CURL_VERSION.tar.xz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# Answers configure would need to run target binaries for.
cat > config.cache <<'EOF'
curl_cv_func_recv=yes
curl_cv_func_send=yes
curl_cv_recv=yes
curl_cv_send=yes
curl_cv_func_select=yes
ac_cv_func_getaddrinfo=yes
ac_cv_func_getaddrinfo_threadsafe=yes
curl_cv_func_getaddrinfo=yes
ac_cv_func_recv=yes
ac_cv_func_send=yes
ac_cv_func_poll=yes
ac_cv_func_fcntl=yes
ac_cv_func_fcntl_o_nonblock=yes
ac_cv_func_getpeername=yes
ac_cv_func_getsockname=yes
ac_cv_func_setsockopt=yes
ac_cv_func_getsockopt=yes
ac_cv_func_clock_gettime_monotonic=yes
ac_cv_func_gmtime_r=yes
ac_cv_func_gethostname=yes
curl_cv_writable_argv=yes
EOF

CC="$CC" AR="$AR" RANLIB="$RANLIB" \
CFLAGS="$TARGET_CFLAGS" CPPFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" LIBS="$LIBS" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --prefix=/usr \
  --enable-static \
  --disable-shared \
  "${TLS_ARGS[@]}" \
  "${ZLIB_ARGS[@]}" \
  --without-libpsl --without-libidn2 --without-brotli --without-zstd \
  --without-nghttp2 --without-ngtcp2 --without-librtmp \
  --disable-ldap --disable-ldaps --disable-ftp --disable-tftp --disable-telnet \
  --disable-dict --disable-gopher --disable-smtp --disable-pop3 --disable-imap \
  --disable-smb --disable-rtsp --disable-mqtt --disable-manual \
  --disable-threaded-resolver --disable-ares --disable-unix-sockets \
  --disable-ipv6 --disable-docs \
  --enable-http --enable-file --enable-proxy --enable-ipv4 > configure.log 2>&1 || {
    tail -40 configure.log >&2; exit 1; }

make -C lib -j"$(nproc)" >make.log 2>&1 || { tail -40 make.log >&2; exit 1; }
make -C src -j"$(nproc)" curl LDFLAGS="$BIN_LDFLAGS" >>make.log 2>&1 || { tail -40 make.log >&2; exit 1; }

mkdir -p "$OUT_DIR"
cp src/curl "$OUT_DIR/curl"
printf 'curl built: %s (TLS: %s)\n' "$OUT_DIR/curl" "$TLS_NAME"
