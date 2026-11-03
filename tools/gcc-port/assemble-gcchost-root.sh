#!/usr/bin/env bash
# P6.4: assemble the in-OS root tree for the hosted GCC. This becomes the ext4
# root ("/") of the GCCHOST test image. Layout (gcc --prefix=/usr, sysroot=/):
#   /usr/bin            gcc g++ cpp + binutils-hosted (as ld ar ranlib nm strip ...)
#   /usr/libexec/gcc/.. cc1 cc1plus collect2 lto1 lto-wrapper
#   /usr/lib/gcc/..     crt{begin,end}*.o libgcc*.a + gcc builtin include/
#   /usr/lib            crt1/Scrt1/crti/crtn.o libc.so libstdc++.{so,so.6,a} libsupc++.a libgcc_s.* + libm/dl/pthread/rt symlinks
#   /usr/include        mlibc headers
#   /hello.c /hello.cpp /gcchost inputs
# Runtime .so's for the loader are shipped separately in the initrd's /lib/mlibc.
set -euo pipefail

HOSTED=/home/sky/gcc-port/gcc-hosted-install
CROSS=/home/sky/gcc-port/gcc-install
SYSROOT=/home/sky/gcc-port/instantos-sysroot
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINUTILS_HOSTED="$REPO/build/binutils-hosted-root"
MLIBC_GCC="$REPO/build/mlibc-root-gcc"
TRIPLE=x86_64-unknown-instantos
GVER=13.3.0
ROOT="${GCCHOST_ROOT:-/home/sky/gcc-port/gcchost-root}"

rm -rf "$ROOT"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/lib" "$ROOT/usr/include" \
         "$ROOT/usr/lib/gcc/$TRIPLE/$GVER" "$ROOT/usr/libexec/gcc/$TRIPLE/$GVER" "$ROOT/tmp"

echo "== gcc compiler tree (driver, libexec, gcc lib incl. include/) =="
cp -a "$HOSTED/usr/bin/." "$ROOT/usr/bin/"
cp -a "$HOSTED/usr/libexec/." "$ROOT/usr/libexec/"
cp -a "$HOSTED/usr/lib/gcc/." "$ROOT/usr/lib/gcc/"

# Drop GCC fixincludes' bogus pthread.h. It was "fixed" from a glibc-style header
# during the gcc build and #includes glibc-only bits/ headers (bits/endian.h,
# bits/pthreadtypes.h, bits/setjmp.h, ...) that mlibc does not provide. Because
# include-fixed precedes the sysroot in the search order, it shadows mlibc's own
# self-consistent /usr/include/pthread.h and breaks <thread> (C++). Removing it
# lets the preprocessor fall through to mlibc's pthread.h.
rm -f "$ROOT/usr/lib/gcc/$TRIPLE/$GVER/include-fixed/pthread.h"

echo "== Phase-5 target libgcc (crt*.o, libgcc*.a) =="
cp -a "$CROSS/lib/gcc/$TRIPLE/$GVER/"crt*.o        "$ROOT/usr/lib/gcc/$TRIPLE/$GVER/" 2>/dev/null || true
cp -a "$CROSS/lib/gcc/$TRIPLE/$GVER/"libgcc*.a     "$ROOT/usr/lib/gcc/$TRIPLE/$GVER/" 2>/dev/null || true
cp -a "$CROSS/lib/gcc/$TRIPLE/$GVER/"libgcov.a     "$ROOT/usr/lib/gcc/$TRIPLE/$GVER/" 2>/dev/null || true

echo "== Phase-5 target C++/libgcc_s shared libs =="
cp -a "$CROSS/$TRIPLE/lib/"libstdc++*  "$ROOT/usr/lib/" 2>/dev/null || true
cp -a "$CROSS/$TRIPLE/lib/"libsupc++*  "$ROOT/usr/lib/" 2>/dev/null || true
cp -a "$CROSS/$TRIPLE/lib/"libgcc_s*   "$ROOT/usr/lib/" 2>/dev/null || true

echo "== crt objects + libc + headers (from the mlibc gcc sysroot) =="
cp -a "$SYSROOT/usr/lib/"crt1.o "$SYSROOT/usr/lib/"Scrt1.o "$SYSROOT/usr/lib/"crti.o "$SYSROOT/usr/lib/"crtn.o "$ROOT/usr/lib/" 2>/dev/null || true
cp -a "$SYSROOT/usr/lib/"libc.so "$ROOT/usr/lib/"
# The dynamic linker (mlibc's ld-instantos.so) must be present at LINK time too:
# libc.so has a DT_NEEDED on it and references its __dlapi_*/__rtld_* symbols, so
# ld needs to find it while linking a dynamic executable (otherwise: "ld-instantos.so
# ... not found" + undefined references to __dlapi_error/__rtld_allocateTcb/...).
cp -a "$SYSROOT/usr/lib/"ld-instantos.so "$ROOT/usr/lib/" 2>/dev/null || true
cp -a "$SYSROOT/usr/include/." "$ROOT/usr/include/"
# libm/libdl/libpthread/librt link-time aliases -> libc.so (mlibc is monolithic).
for a in libm libdl libpthread librt; do ln -sf libc.so "$ROOT/usr/lib/$a.so"; done

echo "== libstdc++ C++ headers into /usr/include/c++ =="
# The HOSTED cc1plus searches $prefix/include/c++/$GVER == /usr/include/c++/13.3.0
# (derived at runtime from .../lib/gcc/$TRIPLE/$GVER/../../../../include/c++/$GVER),
# NOT the $TRIPLE-prefixed path the cross compiler uses. The cross's c++ header tree
# already carries the target subdir (x86_64-unknown-instantos/bits/c++config.h) and
# backward/, so copying it verbatim gives the hosted g++ the full <iostream> search set.
mkdir -p "$ROOT/usr/include"
cp -a "$CROSS/$TRIPLE/include/c++" "$ROOT/usr/include/" 2>/dev/null || true

echo "== binutils hosted (as/ld/ar/ranlib/nm/strip/...) into /usr/bin =="
cp -a "$BINUTILS_HOSTED/usr/bin/." "$ROOT/usr/bin/"
# gcc also probes for target-prefixed tools; provide $TRIPLE-ld etc. as needed.
for t in as ld ar ranlib nm strip objcopy objdump; do
  [ -e "$ROOT/usr/bin/$TRIPLE-$t" ] || ln -sf "$t" "$ROOT/usr/bin/$TRIPLE-$t" 2>/dev/null || true
done

echo "== test inputs at / =="
cat > "$ROOT/hello.c" <<'EOF'
#include <stdio.h>
int main(void){ printf("HELLO_FROM_GCCHOST_CC\n"); return 0; }
EOF
cp "$(dirname "${BASH_SOURCE[0]}")/hello.cpp" "$ROOT/hello.cpp"

echo "== strip hosted binaries =="
# Unstripped cc1/cc1plus are ~300 MB each (debug_info); the kernel's exec cannot
# load a binary that large ("failed to read user binary"). Strip everything.
STRIP_TOOL="${STRIP_TOOL:-llvm-strip}"
find "$ROOT/usr/libexec" "$ROOT/usr/bin" -type f 2>/dev/null | while read -r f; do
  case "$(file -b "$f" 2>/dev/null)" in
    *ELF*executable*|*ELF*shared*) "$STRIP_TOOL" --strip-all "$f" 2>/dev/null || true ;;
  esac
done

du -sh "$ROOT"
echo "gcchost root assembled at: $ROOT"
