#!/usr/bin/env bash
# The graphics compositor built on mlibc and libc++ instead of ilibcxx (step
# 8a), so it can link Mesa (EGL/GLES) and draw on the GPU. The same sources as
# the native one (outside/iUserApps/graphics-compositor/src) plus the parts of
# ilibcxx it uses: its kernel interface (syscall.hpp, all inline but
# _syscall_impl), the ui drawing code (canvas, font, image, theme), the SVG
# renderer and stb. ilibcxx's stand-ins for standard headers (<cstring.hpp>,
# <cmath.hpp>, ...) are the standard ones here (graphics-compositor/mlibc/shim),
# and only ilibcxx's own headers are copied into the include path, so its libc
# headers never shadow mlibc's. INSTANTOS_MLIBC marks what differs (threads).
#
# Output: build/compositor-mlibc/graphics-compositor, which CMake puts in the
# image as /bin/graphics-compositor when it exists (the native one otherwise).
# Needs build/mlibc-root with libc++ (tools/build-mlibc.sh, build-libcxx.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLIBC_ROOT="${MLIBC_ROOT:-$ROOT/build/mlibc-root}"
OUT="${COMPOSITOR_MLIBC_OUT:-$ROOT/build/compositor-mlibc}"
APP="$ROOT/outside/iUserApps/graphics-compositor"
ILIBCXX="$ROOT/outside/iUserApps/outside/ilibcxx"
export PATH="/usr/lib/llvm-20/bin:$PATH"

for need in "$MLIBC_ROOT/lib/libc.so" "$MLIBC_ROOT/lib/libc++.a"; do
  if [ ! -f "$need" ]; then
    printf 'missing %s; run tools/build-mlibc.sh and build-libcxx.sh first\n' "$need" >&2
    exit 2
  fi
done

INC="$OUT/include"
OBJ="$OUT/obj"
rm -rf "$INC" "$OBJ"
mkdir -p "$INC/ui" "$OBJ"
cp "$APP/mlibc/shim/"*.hpp "$INC/"
for header in resizer.hpp anti_aliasing.hpp service_protocol.hpp svg_renderer.hpp time.hpp \
              stb_image.h stb_truetype.h; do
  cp "$ILIBCXX/include/$header" "$INC/"
done
# mlibc's <sys/stat.h> makes st_atime/st_mtime/st_ctime macros (st_atim.tv_sec
# ...), which would rewrite the fields of the kernel's Stat in syscall.hpp.
cp "$ILIBCXX/include/syscall.hpp" "$INC/syscall_ilibcxx.hpp"
{
  echo '#pragma once'
  echo '// syscall.hpp (ilibcxx) with mlibc'"'"'s st_[amc]time macros held off.'
  for m in st_atime st_mtime st_ctime; do echo "#pragma push_macro(\"$m\")"; echo "#undef $m"; done
  echo '#include "syscall_ilibcxx.hpp"'
  for m in st_atime st_mtime st_ctime; do echo "#pragma pop_macro(\"$m\")"; done
} > "$INC/syscall.hpp"
cp "$ILIBCXX/include/ui/"*.hpp "$INC/ui/"

CXXFLAGS=(--target=x86_64-unknown-elf -std=c++23 -O2 -finline-functions -fPIE -fno-stack-protector
          -fno-rtti -fno-exceptions -nostdlibinc -nostdinc++
          -isystem "$INC" -isystem "$MLIBC_ROOT/include/c++/v1" -isystem "$MLIBC_ROOT/include"
          -D_GNU_SOURCE -DINSTANTOS_MLIBC)
# The GL backend (gl_backend.cpp, step 8c) when Mesa's headers are in the
# sysroot (tools/build-mesa.sh); it opens libEGL/libGLESv2 with dlopen.
if [ -f "$MLIBC_ROOT/include/EGL/egl.h" ] && [ -f "$MLIBC_ROOT/include/GLES3/gl3.h" ]; then
  CXXFLAGS+=(-DINSTANTOS_COMPOSITOR_GL)
fi
LDFLAGS=(--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L"$MLIBC_ROOT/lib"
         -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc -Wl,--gc-sections
         "$MLIBC_ROOT/lib/crt1.o")

SOURCES=("$APP"/src/*.cpp
         "$ILIBCXX/src/ui/canvas.cpp" "$ILIBCXX/src/ui/font.cpp" "$ILIBCXX/src/ui/image.cpp"
         "$ILIBCXX/src/ui/theme.cpp" "$ILIBCXX/src/svg_renderer.cpp" "$ILIBCXX/src/syscall.cpp")
objects=()
for source in "${SOURCES[@]}"; do
  object="$OBJ/$(basename "$(dirname "$source")")_$(basename "${source%.cpp}").o"
  clang++ "${CXXFLAGS[@]}" -ffunction-sections -fdata-sections -c "$source" -o "$object"
  objects+=("$object")
done
# mlibc's libc.so holds pthreads and libm too (their .so files are empty).
clang++ "${LDFLAGS[@]}" "${objects[@]}" -lc++ -lc++abi -lunwind -ldl -lc -o "$OUT/graphics-compositor"
llvm-strip --strip-unneeded "$OUT/graphics-compositor"
ls -la "$OUT/graphics-compositor"
