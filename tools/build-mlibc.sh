#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLIBC_SOURCE_DIR="${MLIBC_SOURCE_DIR:-$ROOT/outside/iUserApps/outside/mlibc}"
MLIBC_BUILD_DIR="${MLIBC_BUILD_DIR:-$ROOT/build/mlibc-build}"
MLIBC_INSTALL_DIR="${MLIBC_INSTALL_DIR:-$ROOT/build/mlibc-root}"
MESON="${MESON:-meson}"
SYSDEPS_SOURCE="$ROOT/outside/iUserApps/outside/mlibc-sysdeps/instantos"
SYSDEPS_DEST="$MLIBC_SOURCE_DIR/sysdeps/instantos"
CROSS_FILE="${MLIBC_CROSS_FILE:-$ROOT/tools/mlibc/instantos-x86_64.ini}"

if [ ! -f "$MLIBC_SOURCE_DIR/meson.build" ]; then
  printf 'mlibc source not found: %s\n' "$MLIBC_SOURCE_DIR" >&2
  printf 'run tools/fetch-mlibc.sh first or set MLIBC_SOURCE_DIR\n' >&2
  exit 2
fi

rm -rf "$SYSDEPS_DEST"
mkdir -p "$SYSDEPS_DEST"
cp -R "$SYSDEPS_SOURCE/." "$SYSDEPS_DEST/"
mkdir -p "$SYSDEPS_DEST/include/abi-bits"
for source in "$MLIBC_SOURCE_DIR"/abis/linux/*.h; do
  header="$(basename "$source")"
  ln -sf "../../../../abis/linux/$header" "$SYSDEPS_DEST/include/abi-bits/$header"
done

if ! grep -q "host_machine.system() == 'instantos'" "$MLIBC_SOURCE_DIR/meson.build"; then
  tmp="$(mktemp)"
  awk '
    /# ANCHOR: demo-sysdeps/ {
      print "elif host_machine.system() == '\''instantos'\''"
      print "\tsubdir('\''sysdeps/instantos'\'')"
    }
    { print }
  ' "$MLIBC_SOURCE_DIR/meson.build" > "$tmp"
  mv "$tmp" "$MLIBC_SOURCE_DIR/meson.build"
fi

# InstantOS fixes to mlibc itself. instantos-fixes.patch: stdio reclassifies a
# stream whose fd became a pipe, qsort_r without C++23 "deducing this" (GCC 13
# builds mlibc too), readdir errors return NULL. instantos-threads.patch: locks
# and condition variables make no syscall without contention and wake one waiter,
# semaphores, deferred cancellation, threads give back their stack and TCB.
# instantos-mm.patch: memfd_create without the Linux option.
for FIXES in "$ROOT/tools/mlibc/instantos-fixes.patch" "$ROOT/tools/mlibc/instantos-threads.patch" \
             "$ROOT/tools/mlibc/instantos-mm.patch"; do
  if git -C "$MLIBC_SOURCE_DIR" apply --check "$FIXES" 2>/dev/null; then
    git -C "$MLIBC_SOURCE_DIR" apply "$FIXES"
  elif ! git -C "$MLIBC_SOURCE_DIR" apply --check --reverse "$FIXES" 2>/dev/null; then
    printf 'mlibc fixes neither apply nor are applied: %s
' "$FIXES" >&2
    exit 2
  fi
done

if [ "${MLIBC_CLEAN:-0}" = "1" ]; then
  rm -rf "$MLIBC_BUILD_DIR"
fi

# Libraries are searched in /lib/mlibc (the initrd's), then /usr/lib: a toolchain
# on the root disk keeps its runtime there (gcc's libstdc++.so.6, libgcc_s.so.1),
# as on Linux, so C++ programs built in-OS find it.
if [ -d "$MLIBC_BUILD_DIR" ] && [ -f "$MLIBC_BUILD_DIR/meson-private/coredata.dat" ]; then
  "$MESON" setup --reconfigure "$MLIBC_BUILD_DIR" "$MLIBC_SOURCE_DIR" \
    --cross-file "$CROSS_FILE" \
    --prefix "$MLIBC_INSTALL_DIR" \
    -Dbuild_tests=false \
    -Dposix_option=enabled \
    -Dlinux_option=disabled \
    -Dglibc_option=enabled \
    -Dbsd_option=enabled \
    -Dlibgcc_dependency=false \
    "-Ddefault_library_paths=['/lib/mlibc','/usr/lib']" \
    -Dld_library_name=ld-instantos
else
  "$MESON" setup "$MLIBC_BUILD_DIR" "$MLIBC_SOURCE_DIR" \
    --cross-file "$CROSS_FILE" \
    --prefix "$MLIBC_INSTALL_DIR" \
    -Dbuild_tests=false \
    -Dposix_option=enabled \
    -Dlinux_option=disabled \
    -Dglibc_option=enabled \
    -Dbsd_option=enabled \
    -Dlibgcc_dependency=false \
    "-Ddefault_library_paths=['/lib/mlibc','/usr/lib']" \
    -Dld_library_name=ld-instantos
fi

# frigg (a subproject meson fetched during setup above) uses C++23 "deducing
# this" in span.hpp, which GCC 13 cannot compile (tools/mlibc/frigg-gcc13.patch).
FRIGG_DIR="$MLIBC_SOURCE_DIR/subprojects/frigg"
FRIGG_FIX="$ROOT/tools/mlibc/frigg-gcc13.patch"
if [ -d "$FRIGG_DIR/.git" ] && git -C "$FRIGG_DIR" apply --check "$FRIGG_FIX" 2>/dev/null; then
  git -C "$FRIGG_DIR" apply "$FRIGG_FIX"
fi

"$MESON" compile -C "$MLIBC_BUILD_DIR"
"$MESON" install -C "$MLIBC_BUILD_DIR" --destdir /
# Install supplementary headers that live in the instantos sysdeps include dir
# but are not part of mlibc's own install_headers set (e.g. mntent.h, whose
# getmntent() family is provided by the sysdeps builtins).
if [ -f "$SYSDEPS_SOURCE/include/mntent.h" ]; then
  cp "$SYSDEPS_SOURCE/include/mntent.h" "$MLIBC_INSTALL_DIR/include/mntent.h"
fi
# Strip the installed runtime libraries: the debug build of libc.so is ~9 MiB
# but strips to ~1 MiB, which matters for fitting the initrd in the EFI image.
for lib in libc.so ld-instantos.so libdl.so libm.so libpthread.so librt.so; do
  if [ -f "$MLIBC_INSTALL_DIR/lib/$lib" ]; then
    "${LLVM_STRIP:-llvm-strip}" --strip-all "$MLIBC_INSTALL_DIR/lib/$lib" 2>/dev/null || true
  fi
done
printf 'mlibc installed to: %s\n' "$MLIBC_INSTALL_DIR"
