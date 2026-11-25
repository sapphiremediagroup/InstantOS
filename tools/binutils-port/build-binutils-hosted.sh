#!/usr/bin/env bash
# P4 Step B: build binutils HOSTED on InstantOS (Canadian cross:
# build=x86_64-linux-gnu, host=target=x86_64-unknown-instantos). The host C
# compiler is clang targeting instantos+mlibc, wrapped as an x86_64-unknown-
# instantos-gcc driver; the host binutils (ar/ranlib/as/ld/...) come from the
# Step-A cross build. Produces as/ld/ar/ranlib/nm/strip ELF binaries that RUN in
# InstantOS, staged under build/binutils-hosted-root.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
PORT_DIR="$BUILD_DIR/binutils-port"
VER="${VER:-binutils-2.42}"
SRC="$PORT_DIR/$VER"
OBJ="$PORT_DIR/build-hosted"
STAGE="${STAGE:-$BUILD_DIR/binutils-hosted-root}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
BX="$BUILD_DIR/binutils-cross/bin"
JOBS="${JOBS:-4}"

[ -d "$SRC" ] || { echo "patched binutils source missing; run build-binutils-cross.sh" >&2; exit 2; }
[ -x "$BX/x86_64-unknown-instantos-ar" ] || { echo "cross binutils missing; run build-binutils-cross.sh" >&2; exit 2; }
[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc missing" >&2; exit 2; }

# --- host C compiler: a gcc-named clang driver targeting instantos + mlibc.
# Compile mode: mlibc headers, freestanding-ish, PIE. Link mode: mlibc crt1 +
# -lc + mlibc loader + rpath + eager binding (matches the lld-built userland).
WRAP="$BX/x86_64-unknown-instantos-gcc"
cat > "$WRAP" <<EOF
#!/bin/sh
# gcc-compatible clang driver for the x86_64-unknown-instantos (mlibc) host.
MLIBC_ROOT="$MLIBC_ROOT"
link=1; shared=0
for a in "\$@"; do
  case "\$a" in
    -c|-E|-S|-M|-MM) link=0 ;;
    -shared) shared=1 ;;
  esac
done
COMMON="--target=x86_64-unknown-elf -fPIC -fno-stack-protector -fno-addrsig -nostdlibinc -isystem \$MLIBC_ROOT/include -D_GNU_SOURCE -Wno-implicit-function-declaration -Wno-int-conversion -fcommon"
if [ "\$link" = 0 ]; then
  exec clang \$COMMON "\$@"
elif [ "\$shared" = 1 ]; then
  # shared object (e.g. ld test plugins): no crt/-pie, no interpreter.
  exec clang \$COMMON -shared -nostdlib -fuse-ld=lld -L"\$MLIBC_ROOT/lib" \\
    -Wl,-rpath,/lib/mlibc -Wl,-z,now -Wl,--allow-multiple-definition "\$@" -lc
else
  # executable: mlibc crt1 + loader + rpath + eager binding.
  exec clang \$COMMON -pie -nostdlib -fuse-ld=lld -L"\$MLIBC_ROOT/lib" \\
    -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc \\
    -Wl,-z,now -Wl,--allow-multiple-definition "\$@" "\$MLIBC_ROOT/lib/crt1.o" -lc
fi
EOF
chmod +x "$WRAP"
# gcc/cc aliases some configure paths look for
ln -sf x86_64-unknown-instantos-gcc "$BX/x86_64-unknown-instantos-cc" 2>/dev/null || true

# --- configure (cross mode: build != host, so no host binaries are run) ---
if [ "${RECONFIGURE:-0}" = "1" ] || [ ! -f "$OBJ/Makefile" ]; then
  rm -rf "$OBJ"; mkdir -p "$OBJ"
  # No lex library: a build host with flex installed would otherwise link ar
  # and ranlib against its libfl.so.2, which InstantOS lacks (the lexers use
  # noyywrap, so nothing from libfl is needed). autoconf 2.69 takes the cached
  # answer, not LEXLIB; make is also given LEXLIB= below.
  ( cd "$OBJ" && PATH="$BX:$PATH" \
    CC_FOR_BUILD="clang" ac_cv_lib_lex="none needed" \
    "$SRC/configure" \
      --build=x86_64-linux-gnu \
      --host=x86_64-unknown-instantos \
      --target=x86_64-unknown-instantos \
      --prefix=/usr \
      --with-sysroot=/ \
      --disable-nls --disable-werror \
      --disable-gold --disable-gprofng --disable-libctf --disable-plugins \
      --without-zstd --without-debuginfod --without-msgpack \
      --enable-default-hash-style=sysv )
fi

# --- build + stage-install ---
PATH="$BX:$PATH" make -C "$OBJ" -j"$JOBS" MAKEINFO=true LEXLIB=
rm -rf "$STAGE"
PATH="$BX:$PATH" make -C "$OBJ" MAKEINFO=true LEXLIB= DESTDIR="$STAGE" install

echo "binutils-hosted staged to $STAGE"
ls "$STAGE/usr/bin" 2>/dev/null || true
