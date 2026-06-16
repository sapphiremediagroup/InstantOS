#!/usr/bin/env bash
# Cross-build libcurl against the InstantOS mlibc sysroot, with mbedTLS for TLS
# and zlib for transfer compression.
#
# Produces a static libcurl.a plus headers installed into the mlibc sysroot, so
# NetSurf's curl fetcher can do http:// and https:// transfers.
#
# InstantOS specifics handled here:
#   - mbedTLS (not OpenSSL) is the TLS backend (--with-mbedtls).
#   - The synchronous resolver is used (no threads); it goes through mlibc's
#     getaddrinfo(), which resolves via the kernel UDP socket DNS path.
#   - Most optional protocols/features are disabled: NetSurf only needs
#     HTTP/HTTPS (+ the file: scheme it handles itself), so we strip FTP, LDAP,
#     SMTP, etc. to keep the static archive small and the link surface minimal.
#   - configure runs target link tests; we feed it a config.cache plus
#     cross-compile host/build triples so it never executes a target binary.
#
# Prerequisites: tools/build-mlibc.sh, tools/build-zlib.sh, tools/build-mbedtls.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
CURL_VERSION="${CURL_VERSION:-curl-8.11.1}"
CURL_URL="${CURL_URL:-https://curl.se/download/${CURL_VERSION}.tar.xz}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/curl-port}"
SRC_DIR="$WORK_DIR/$CURL_VERSION"
OBJ_DIR="$WORK_DIR/build"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
RANLIB="${RANLIB:-llvm-ranlib}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
if [ ! -f "$MLIBC_ROOT/lib/libmbedtls.a" ]; then
  printf 'mbedTLS missing (%s/lib/libmbedtls.a); run tools/build-mbedtls.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
if [ ! -f "$MLIBC_ROOT/lib/libz.a" ]; then
  printf 'zlib missing (%s/lib/libz.a); run tools/build-zlib.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

# Freestanding-against-mlibc flags. mbedTLS headers need its config file macro.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -I$MLIBC_ROOT/include -D_GNU_SOURCE -DMBEDTLS_CONFIG_FILE='<instantos_mbedtls_config.h>'"
TARGET_LDFLAGS="--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$CURL_URL"
  curl -sL "$CURL_URL" -o "$WORK_DIR/$CURL_VERSION.tar.xz"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$CURL_VERSION.tar.xz"
fi

rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"

# Pre-seed configure checks that would otherwise need to run target binaries.
# These all describe the InstantOS/mlibc target, which advertises a POSIX-ish
# environment (getaddrinfo, poll, recv/send, nonblocking sockets via fcntl).
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
EOF

# mbedTLS link arguments (link order: tls -> x509 -> crypto).
MBEDTLS_LIBS="-lmbedtls -lmbedx509 -lmbedcrypto"

CC="$CC" \
AR="$AR" \
RANLIB="$RANLIB" \
CFLAGS="$TARGET_CFLAGS" \
CPPFLAGS="$TARGET_CFLAGS" \
LDFLAGS="$TARGET_LDFLAGS" \
LIBS="$MBEDTLS_LIBS -lz -lc" \
"$SRC_DIR/configure" \
  --host=x86_64-unknown-elf \
  --build=x86_64-linux-gnu \
  --cache-file=config.cache \
  --prefix="$MLIBC_ROOT" \
  --enable-static \
  --disable-shared \
  --with-mbedtls="$MLIBC_ROOT" \
  --with-zlib="$MLIBC_ROOT" \
  --without-libpsl \
  --without-libidn2 \
  --without-brotli \
  --without-zstd \
  --without-nghttp2 \
  --without-ngtcp2 \
  --without-librtmp \
  --without-ca-bundle \
  --without-ca-path \
  --disable-ldap \
  --disable-ldaps \
  --disable-ftp \
  --disable-file \
  --disable-tftp \
  --disable-telnet \
  --disable-dict \
  --disable-gopher \
  --disable-smtp \
  --disable-pop3 \
  --disable-imap \
  --disable-smb \
  --disable-rtsp \
  --disable-mqtt \
  --disable-manual \
  --disable-libcurl-option \
  --disable-threaded-resolver \
  --disable-ares \
  --disable-unix-sockets \
  --enable-http \
  --enable-proxy \
  --enable-ipv4 \
  --disable-ipv6

# Build only the library (skip src/ which links the curl executable).
make -C lib -j"$(nproc)" CFLAGS="$TARGET_CFLAGS"

# Install headers and the static archive.
make -C lib install >/dev/null 2>&1 || true
mkdir -p "$MLIBC_ROOT/lib" "$MLIBC_ROOT/include/curl"
cp lib/.libs/libcurl.a "$MLIBC_ROOT/lib/libcurl.a"
"$RANLIB" "$MLIBC_ROOT/lib/libcurl.a" 2>/dev/null || true
cp "$SRC_DIR"/include/curl/*.h "$MLIBC_ROOT/include/curl/"

# Emit a pkg-config file (NetSurf finds libcurl via pkg-config). Declare the
# private deps so a static link pulls in mbedTLS + zlib in the right order.
CURL_PC_VERSION="${CURL_VERSION#curl-}"
mkdir -p "$MLIBC_ROOT/lib/pkgconfig"
cat > "$MLIBC_ROOT/lib/pkgconfig/libcurl.pc" <<EOF
prefix=$MLIBC_ROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libcurl
Description: Library to transfer files with HTTP/HTTPS
Version: $CURL_PC_VERSION
Requires.private: mbedtls
Libs: -L\${libdir} -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
Libs.private: -lmbedtls -lmbedx509 -lmbedcrypto -lz
Cflags: -I\${includedir} -DCURL_STATICLIB
EOF

printf 'libcurl built: %s\n' "$MLIBC_ROOT/lib/libcurl.a"
ls -l "$MLIBC_ROOT/lib/libcurl.a" "$MLIBC_ROOT/include/curl/curl.h"
