#!/usr/bin/env bash
# Cross-build OpenSSL 3.5 (LTS, supported until April 2030) against the
# InstantOS mlibc sysroot: libcrypto.so.3 and libssl.so.3 (shared; mlibc's
# loader looks in /lib/mlibc) and the `openssl` command.
#
# Installed into the sysroot: include/openssl, lib/lib{ssl,crypto}.so.3, the
# linker scripts lib{ssl,crypto}.so naming them, pkgconfig. Stripped copies for
# the initrd go to build/openssl (CMake bundles them; rerun cmake after).
#
#   - Entropy only from mlibc's getentropy() (the kernel's ChaCha20 CSPRNG):
#     --with-rand-seed=getrandom, which calls getentropy and fails when it does.
#     No device, no fallback.
#   - x86-64 assembly, pthreads. No symbol versions: mlibc's loader ignores them.
#   - Left out: engines, async (ucontext switching), QUIC, loadable modules and
#     the legacy provider, kTLS, SCTP, and the secure heap (its guard pages rely
#     on PROT_NONE, which the kernel treats as read/write).
#   - OPENSSLDIR is /etc/ssl (the kernel mounts the initrd's ssl/ there).
#
# The tree is kept in WSL (~/openssl-port, OPENSSL_WORK). Host needs perl.
# Prerequisites: tools/build-mlibc.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.9}"
OPENSSL_SHA256="${OPENSSL_SHA256:-603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a}"
NAME="openssl-$OPENSSL_VERSION"
URL="https://github.com/openssl/openssl/releases/download/$NAME/$NAME.tar.gz"
WORK="${OPENSSL_WORK:-$HOME/openssl-port}"
SRC="$WORK/$NAME"
OBJ="$WORK/build"
STAGE="$WORK/stage"
OUT="${OUT_DIR:-$BUILD_DIR/openssl}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
command -v perl >/dev/null || { echo "perl is needed to build OpenSSL" >&2; exit 2; }

mkdir -p "$WORK"
[ -f "$WORK/$NAME.tar.gz" ] || curl -fsSL "$URL" -o "$WORK/$NAME.tar.gz"
echo "$OPENSSL_SHA256  $WORK/$NAME.tar.gz" | sha256sum -c --quiet - || {
  echo "$WORK/$NAME.tar.gz does not have the pinned checksum: not building it" >&2; exit 1; }
[ -d "$SRC" ] || tar -C "$WORK" -xzf "$WORK/$NAME.tar.gz"

rm -rf "$OBJ" "$STAGE"
mkdir -p "$OBJ"
# The target. Flags are OpenSSL's own lists: cflags for everything, lflags for
# every link, bin_lflags for programs.
cat > "$WORK/instantos.conf" <<EOF
my %targets = (
    "instantos-x86_64" => {
        inherit_from     => [ "BASE_unix" ],
        CC               => "clang",
        AR               => "llvm-ar",
        RANLIB           => "llvm-ranlib",
        CFLAGS           => "-O2 -Wall",
        cflags           => "--target=x86_64-unknown-elf -ffreestanding -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -D__instantos__",
        lib_cppflags     => "-DL_ENDIAN",
        lflags           => "--target=x86_64-unknown-elf -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,-rpath,/lib/mlibc",
        bin_lflags       => "-pie -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so $MLIBC_ROOT/lib/crt1.o",
        bin_cflags       => "-fPIE",
        ex_libs          => "-lc",
        bn_ops           => "SIXTY_FOUR_BIT_LONG",
        asm_arch         => "x86_64",
        perlasm_scheme   => "elf",
        thread_scheme    => "pthreads",
        shared_target    => "gnu-shared",
        shared_cflag     => "-fPIC",
        shared_ldflag    => "-Wl,-z,defs -Wl,-e,0",
        shared_extension => ".so.\\\$(SHLIB_VERSION_NUMBER)",
    },
);
EOF

cd "$OBJ"
"$SRC/Configure" instantos-x86_64 --config="$WORK/instantos.conf" \
  --prefix=/usr --libdir=lib --openssldir=/etc/ssl \
  --with-rand-seed=getrandom \
  shared threads \
  no-engine no-async no-quic no-module no-legacy no-dso no-ktls no-sctp no-afalgeng \
  no-secure-memory no-ssl3 no-comp no-tests no-docs > configure.log 2>&1 || { tail -30 configure.log >&2; exit 1; }
# Configure adds -Wa,--noexecstack, which reaches the link (clang hands a bare
# ELF target's link to gcc) as an unknown option. lld's stacks are not
# executable without it; checked below.
sed -i 's/-Wa,--noexecstack//g' Makefile
make -j"$(nproc)" > make.log 2>&1 || { grep -E "error|undefined" make.log | head -30 >&2; tail -10 make.log >&2; exit 1; }
make DESTDIR="$STAGE" install_sw > install.log 2>&1 || { tail -20 install.log >&2; exit 1; }

# The sysroot. Plain files, no links (the sysroot is on a Windows drive):
# lib<name>.so is a linker script naming the real library.
rm -rf "$MLIBC_ROOT/include/openssl"
cp -rL "$STAGE/usr/include/openssl" "$MLIBC_ROOT/include/openssl"
mkdir -p "$OUT" "$MLIBC_ROOT/lib/pkgconfig"
for lib in crypto ssl; do
  cp -L "$STAGE/usr/lib/lib$lib.so.3" "$MLIBC_ROOT/lib/lib$lib.so.3"
  rm -f "$MLIBC_ROOT/lib/lib$lib.so"
  printf 'INPUT(lib%s.so.3)\n' "$lib" > "$MLIBC_ROOT/lib/lib$lib.so"
  cp -L "$STAGE/usr/lib/lib$lib.so.3" "$OUT/lib$lib.so.3"
  "${LLVM_STRIP:-llvm-strip}" --strip-unneeded "$OUT/lib$lib.so.3"
done
for pc in libcrypto libssl openssl; do
  sed "s|^prefix=.*|prefix=$MLIBC_ROOT|" "$STAGE/usr/lib/pkgconfig/$pc.pc" > "$MLIBC_ROOT/lib/pkgconfig/$pc.pc"
done
cp "$STAGE/usr/bin/openssl" "$OUT/openssl"
"${LLVM_STRIP:-llvm-strip}" "$OUT/openssl"

# What must hold: only our libraries needed, stacks not executable, no text
# relocations, and getentropy imported (the only entropy source).
for f in libcrypto.so.3 libssl.so.3 openssl; do
  needed="$(llvm-readelf -d "$OUT/$f" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
  for n in $needed; do
    case "$n" in libc.so|libcrypto.so.3|libssl.so.3|ld-instantos.so) ;; *) echo "$f needs $n" >&2; exit 1 ;; esac
  done
  if llvm-readelf -lW "$OUT/$f" | grep GNU_STACK | grep -q "RWE"; then echo "$f: executable stack" >&2; exit 1; fi
  if llvm-readelf -d "$OUT/$f" | grep -q TEXTREL; then echo "$f: text relocations" >&2; exit 1; fi
  printf '%-16s needs %s\n' "$f" "$needed"
done
for f in libcrypto.so.3 libssl.so.3; do
  # lld's default entry symbol (hence -e 0): programs linked without crt1.o,
  # such as configure's tests, would fail on it.
  if llvm-nm -D --undefined-only "$OUT/$f" | grep -q " _start$"; then echo "$f wants _start" >&2; exit 1; fi
done
llvm-nm -D --undefined-only "$OUT/libcrypto.so.3" | grep -q " getentropy$" || {
  echo "libcrypto does not import getentropy" >&2; exit 1; }
ls -la "$OUT"
