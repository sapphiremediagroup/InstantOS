#!/usr/bin/env bash
# Cross-build mbedTLS against the InstantOS mlibc sysroot.
#
# Produces static libmbedtls.a / libmbedx509.a / libmbedcrypto.a plus headers,
# installed into the mlibc sysroot for use by the libcurl port (and thus
# NetSurf's https:// fetcher).
#
# InstantOS is a freestanding-ish target: there is no /dev/urandom, no usable
# filesystem entropy, and no pthreads in the libc we link NetSurf against.  We
# therefore build mbedTLS with a custom config (instantos_mbedtls_config.h) that
#   - disables MBEDTLS_FS_IO / MBEDTLS_NET_C / MBEDTLS_TIMING_C / threading
#     (the kernel sockets + our own timing are driven from libcurl, not mbedTLS)
#   - enables MBEDTLS_NO_PLATFORM_ENTROPY + MBEDTLS_ENTROPY_HARDWARE_ALT so the
#     only entropy source is mbedtls_hardware_poll(), which we implement on top
#     of the libc getentropy() (backed by the kernel ChaCha20 CSPRNG).
#   - keeps TLS 1.2 + 1.3, X.509 chain validation, and a CA-bundle-capable
#     verify path so we can do full certificate validation.
#
# Prerequisites: tools/build-mlibc.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
MBEDTLS_VERSION="${MBEDTLS_VERSION:-mbedtls-3.6.2}"
MBEDTLS_URL="${MBEDTLS_URL:-https://github.com/Mbed-TLS/mbedtls/releases/download/${MBEDTLS_VERSION}/${MBEDTLS_VERSION}.tar.bz2}"
WORK_DIR="${WORK_DIR:-$BUILD_DIR/mbedtls-port}"
SRC_DIR="$WORK_DIR/$MBEDTLS_VERSION"
CC="${CC:-clang}"
AR="${AR:-llvm-ar}"
RANLIB="${RANLIB:-llvm-ranlib}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi

# Freestanding-against-mlibc flags, mirroring the zlib/libpng/libjpeg ports.
# MBEDTLS_CONFIG_FILE points the whole tree at our custom config header.
TARGET_CFLAGS="--target=x86_64-unknown-elf -ffreestanding -fPIC -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -I$MLIBC_ROOT/include -D_GNU_SOURCE -DMBEDTLS_CONFIG_FILE='<instantos_mbedtls_config.h>'"

mkdir -p "$WORK_DIR"
if [ ! -d "$SRC_DIR" ]; then
  printf 'fetching %s\n' "$MBEDTLS_URL"
  curl -sL "$MBEDTLS_URL" -o "$WORK_DIR/$MBEDTLS_VERSION.tar.bz2"
  tar -C "$WORK_DIR" -xf "$WORK_DIR/$MBEDTLS_VERSION.tar.bz2"
fi

# --- Custom mbedTLS configuration ----------------------------------------
# mbedTLS 3.x derives a large web of "MBEDTLS_*_CAN_*" capability macros inside
# its config_adjust_*.h headers, so hand-rolling a minimal config from scratch
# trips check_config.h. Instead start from the upstream default config (which is
# complete and self-consistent) and patch only what InstantOS needs to differ:
#   - no filesystem (MBEDTLS_FS_IO and the PSA storage that depends on it)
#   - no in-library BSD sockets (MBEDTLS_NET_C) - libcurl owns the socket
#   - no MBEDTLS_TIMING_C (depends on a POSIX timing layer we don't ship)
#   - no threading (single-threaded use from NetSurf/libcurl)
#   - entropy comes only from our hardware poll (getentropy -> kernel CSPRNG)
DEFAULT_CONFIG="$SRC_DIR/include/mbedtls/mbedtls_config.h"
CONFIG="$SRC_DIR/include/instantos_mbedtls_config.h"
cp "$DEFAULT_CONFIG" "$CONFIG"

# Disable features that need a POSIX net / POSIX timing / threads.
# NOTE: MBEDTLS_FS_IO is kept ENABLED — InstantOS has a working filesystem
# (initrd + file fetcher with fopen/fread), and libcurl's mbedTLS backend loads
# the CA bundle via mbedtls_x509_crt_parse_file(), which is guarded by
# MBEDTLS_FS_IO. Disabling it makes curl return CURLE_NOT_BUILT_IN for https.
for sym in MBEDTLS_NET_C MBEDTLS_TIMING_C \
           MBEDTLS_PSA_CRYPTO_STORAGE_C MBEDTLS_PSA_ITS_FILE_C \
           MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD \
           MBEDTLS_PSA_CRYPTO_SE_C; do
  sed -i "s|^#define ${sym}\b|//#define ${sym}|" "$CONFIG"
done

# Enable our hardware entropy source; disable the /dev/urandom collectors.
sed -i 's|^//#define MBEDTLS_NO_PLATFORM_ENTROPY|#define MBEDTLS_NO_PLATFORM_ENTROPY|' "$CONFIG"
sed -i 's|^//#define MBEDTLS_ENTROPY_HARDWARE_ALT|#define MBEDTLS_ENTROPY_HARDWARE_ALT|' "$CONFIG"

# mbedTLS's POSIX mbedtls_ms_time() path keys off _POSIX_VERSION, which our
# bare-metal --target=x86_64-unknown-elf compile does not advertise. Provide our
# own millisecond clock via MBEDTLS_PLATFORM_MS_TIME_ALT (implemented in the
# entropy shim on top of clock_gettime(CLOCK_MONOTONIC)).
sed -i 's|^//#define MBEDTLS_PLATFORM_MS_TIME_ALT|#define MBEDTLS_PLATFORM_MS_TIME_ALT|' "$CONFIG"
grep -q '^#define MBEDTLS_PLATFORM_MS_TIME_ALT' "$CONFIG" || \
  printf '\n#define MBEDTLS_PLATFORM_MS_TIME_ALT\n' >> "$CONFIG"

# We link consumers with -nostdlib, so the compiler-rt/libgcc 128-bit integer
# division builtin (__udivti3, pulled in by bignum's 64x64->128 division path)
# is unavailable. Tell mbedTLS to use its portable long-division fallback
# instead of the __int128 divide.
sed -i 's|^//#define MBEDTLS_NO_UDBL_DIVISION|#define MBEDTLS_NO_UDBL_DIVISION|' "$CONFIG"
grep -q '^#define MBEDTLS_NO_UDBL_DIVISION' "$CONFIG" || \
  printf '\n#define MBEDTLS_NO_UDBL_DIVISION\n' >> "$CONFIG"

# --- Hardware entropy shim ---------------------------------------------- */
# MBEDTLS_ENTROPY_HARDWARE_ALT requires us to provide mbedtls_hardware_poll().
# Back it with getentropy() (which mlibc routes to the kernel ChaCha20 CSPRNG).
cat > "$SRC_DIR/library/instantos_entropy.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Declared by mlibc <unistd.h>; avoid pulling the full header into the
 * freestanding build to keep the dependency surface tiny. */
extern int getentropy(void *buffer, size_t length);

/* mbedTLS entropy hardware poll. getentropy() yields at most 256 bytes per
 * call, so loop until we satisfy the request. */
int mbedtls_hardware_poll(void *data, unsigned char *output,
                          size_t len, size_t *olen) {
    (void) data;
    size_t got = 0;
    while (got < len) {
        size_t chunk = len - got;
        if (chunk > 256) chunk = 256;
        if (getentropy(output + got, chunk) != 0) {
            /* Can't continue without entropy. */
            if (olen) *olen = got;
            return got ? 0 : -1;
        }
        got += chunk;
    }
    if (olen) *olen = got;
    return 0;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT: provide a monotonic millisecond clock.
 * mbedtls_ms_time_t is int64_t. mlibc's clock_gettime(CLOCK_MONOTONIC) is
 * backed by the kernel uptime counter. */
typedef int64_t mbedtls_ms_time_t;
mbedtls_ms_time_t mbedtls_ms_time(void) {
    struct timespec tv;
    if (clock_gettime(CLOCK_MONOTONIC, &tv) != 0) {
        return (mbedtls_ms_time_t) time(NULL) * 1000;
    }
    return (mbedtls_ms_time_t) tv.tv_sec * 1000 +
           (mbedtls_ms_time_t) tv.tv_nsec / 1000000;
}
EOF

cd "$SRC_DIR"

# Build only the static libraries (mbedTLS Makefile target `lib`); skip the
# programs/ and tests/ which would link executables against the freestanding
# runtime. WINDOWS/SHARED off → only .a archives are produced in library/.
make -C library clean >/dev/null 2>&1 || true
make -C library \
  CC="$CC" \
  AR="$AR" \
  CFLAGS="$TARGET_CFLAGS" \
  SHARED= \
  -j"$(nproc)" \
  libmbedcrypto.a libmbedx509.a libmbedtls.a

# The entropy shim isn't part of mbedTLS's own source list; compile it and add
# it to libmbedcrypto.a (where the entropy module lives).
"$CC" $TARGET_CFLAGS -I"$SRC_DIR/include" -c library/instantos_entropy.c -o library/instantos_entropy.o
"$AR" rs library/libmbedcrypto.a library/instantos_entropy.o

# --- Install ------------------------------------------------------------ */
mkdir -p "$MLIBC_ROOT/lib" "$MLIBC_ROOT/include"
cp library/libmbedcrypto.a library/libmbedx509.a library/libmbedtls.a "$MLIBC_ROOT/lib/"
"$RANLIB" "$MLIBC_ROOT/lib/libmbedcrypto.a" 2>/dev/null || true
"$RANLIB" "$MLIBC_ROOT/lib/libmbedx509.a" 2>/dev/null || true
"$RANLIB" "$MLIBC_ROOT/lib/libmbedtls.a" 2>/dev/null || true
cp -R include/mbedtls "$MLIBC_ROOT/include/"
cp -R include/psa "$MLIBC_ROOT/include/" 2>/dev/null || true
# Ship the custom config alongside the headers so consumers compile against the
# same feature set (curl includes <mbedtls/*> which transitively pulls it in).
cp include/instantos_mbedtls_config.h "$MLIBC_ROOT/include/"

# --- pkg-config --------------------------------------------------------- */
MBEDTLS_PC_VERSION="${MBEDTLS_VERSION#mbedtls-}"
mkdir -p "$MLIBC_ROOT/lib/pkgconfig"
# curl looks for mbedtls via -lmbedtls -lmbedx509 -lmbedcrypto; provide a .pc
# for tooling that prefers pkg-config. Note link order matters: tls -> x509 ->
# crypto.
cat > "$MLIBC_ROOT/lib/pkgconfig/mbedtls.pc" <<EOF
prefix=$MLIBC_ROOT
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: mbedTLS
Description: Mbed TLS / X.509 / crypto library
Version: $MBEDTLS_PC_VERSION
Libs: -L\${libdir} -lmbedtls -lmbedx509 -lmbedcrypto
Cflags: -I\${includedir} -DMBEDTLS_CONFIG_FILE='<instantos_mbedtls_config.h>'
EOF

printf 'mbedTLS built: %s\n' "$MLIBC_ROOT/lib/libmbedtls.a"
ls -l "$MLIBC_ROOT/lib/libmbedtls.a" "$MLIBC_ROOT/lib/libmbedx509.a" "$MLIBC_ROOT/lib/libmbedcrypto.a"
