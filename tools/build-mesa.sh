#!/usr/bin/env bash
# Cross-build Mesa (iris for OpenGL/GLES through EGL, anv for Vulkan; no
# window system yet: EGL's surfaceless platform and off-screen Vulkan; plus
# softpipe, Mesa's CPU renderer, so QEMU can run the GL stack too)
# against the InstantOS mlibc sysroot with libc++ (tools/build-libcxx.sh) and
# libdrm (tools/build-libdrm.sh).
#
# Mesa takes its Linux paths (__linux__: the i915 uAPI, /dev/dri, sysfs),
# which is the ABI the kernel follows. Its internal Intel shaders need
# intel_clc, built for the host first (host packages: libclc-20-dev,
# libllvmspirvlib-20-dev, spirv-tools, libclang-cpp20-dev, libclang-20-dev,
# python3-mako, glslang-tools, bison, flex). Off: LLVM (llvmpipe), the shader
# cache, xmlconfig, zlib/zstd, every window system.
#
# The libraries are installed for /lib/mlibc (where they go in InstantOS, so
# Mesa's compiled-in search paths for drivers and ICDs match) into the
# staging tree build/mesa-root; headers also go into the sysroot for the
# test programs. Trees are kept in WSL (~/mesa-port, MESA_WORK).
# Source fixes: tools/mesa-port/patch-mesa.py.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLIBC_ROOT="${MLIBC_ROOT:-$ROOT/build/mlibc-root}"
STAGE="${MESA_STAGE:-$ROOT/build/mesa-root}"
MESA_VERSION="${MESA_VERSION:-24.3.4}"
WORK="${MESA_WORK:-$HOME/mesa-port}"
SRC="$WORK/mesa-$MESA_VERSION"
HOST_OBJ="$WORK/host-clc"
OBJ="$WORK/cross"
export PATH="/usr/lib/llvm-20/bin:$PATH"

for need in "$MLIBC_ROOT/lib/libc.so" "$MLIBC_ROOT/lib/libc++.a" "$MLIBC_ROOT/lib/libdrm.a"; do
  if [ ! -f "$need" ]; then
    printf 'missing %s; run tools/build-mlibc.sh, build-libcxx.sh and build-libdrm.sh first\n' "$need" >&2
    exit 2
  fi
done
mkdir -p "$WORK"
if [ ! -d "$SRC" ]; then
  curl -sfL "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz" -o "$WORK/mesa.tar.xz"
  tar -C "$WORK" -xJf "$WORK/mesa.tar.xz"
  rm -f "$WORK/mesa.tar.xz"
fi
python3 "$ROOT/tools/mesa-port/patch-mesa.py" "$SRC"

# 1. intel_clc for the host.
if [ ! -x "$HOST_OBJ/src/intel/compiler/intel_clc" ]; then
  meson setup --wipe "$HOST_OBJ" "$SRC" -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=enabled -Dintel-clc=enabled -Dinstall-intel-clc=true -Dxmlconfig=disabled -Dzstd=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false
  ninja -C "$HOST_OBJ" src/intel/compiler/intel_clc
fi
mkdir -p "$WORK/host-bin"
ln -sf "$HOST_OBJ/src/intel/compiler/intel_clc" "$WORK/host-bin/intel_clc"
export PATH="$WORK/host-bin:$PATH"

# 2. Mesa for InstantOS. __linux__ and __unix__ as a Linux compiler would
# define them (Mesa's Linux paths; EGL's generic Unix native types),
# __instantos__ for the few fixes in patch-mesa.py (and for vulkan.h, which then
# turns on Vulkan's InstantOS surfaces), and Linux's ioctl macros
# everywhere (Linux's sys/ioctl.h carries them; mlibc's does not).
mkdir -p "$MLIBC_ROOT/include/linux" "$MLIBC_ROOT/include/asm"
cp "$ROOT/tools/libdrm-port/include/linux/"*.h "$MLIBC_ROOT/include/linux/"
cp "$ROOT/tools/libdrm-port/include/asm/"*.h "$MLIBC_ROOT/include/asm/"
DEFINES="'-D__linux__', '-D__unix__', '-D__unix', '-D__instantos__', '-D_GNU_SOURCE', '-include', 'asm/ioctl.h'"
C_FLAGS="'--target=x86_64-unknown-elf', '-nostdlibinc', '-isystem', '$MLIBC_ROOT/include', $DEFINES, '-fPIC', '-fno-stack-protector'"
CXX_FLAGS="'--target=x86_64-unknown-elf', '-nostdlibinc', '-nostdinc++', '-isystem', '$MLIBC_ROOT/include/c++/v1', '-isystem', '$MLIBC_ROOT/include', $DEFINES, '-fPIC', '-fno-stack-protector'"
LINK_FLAGS="'--target=x86_64-unknown-elf', '-nostdlib', '-fuse-ld=lld', '-L$MLIBC_ROOT/lib', '-Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so', '-Wl,-rpath,/lib/mlibc', '-lc++', '-lc++abi', '-lunwind', '-lc', '-lm'"
CROSS="$WORK/instantos-cross.ini"
cat > "$CROSS" <<EOF
[binaries]
c = 'clang'
cpp = 'clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$MLIBC_ROOT/lib/pkgconfig'
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
c_args = [$C_FLAGS]
cpp_args = [$CXX_FLAGS]
c_link_args = [$LINK_FLAGS]
cpp_link_args = [$LINK_FLAGS]
EOF

SETUP_MODE=""
[ -f "$OBJ/build.ninja" ] && SETUP_MODE="--reconfigure"
meson setup $SETUP_MODE "$OBJ" "$SRC" --cross-file "$CROSS" \
  --prefix /usr --libdir /lib/mlibc --buildtype release \
  -Dplatforms=instantos -Degl-native-platform=instantos -Dgallium-drivers=iris,softpipe -Dvulkan-drivers=intel \
  -Degl=enabled -Dgbm=disabled -Dglx=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
  -Dshared-glapi=enabled -Dglvnd=disabled -Dllvm=disabled -Dintel-clc=system -Dintel-rt=disabled \
  -Dshader-cache=disabled -Dxmlconfig=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dbuild-tests=false \
  -Dvideo-codecs= -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
  -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false -Dosmesa=false \
  -Dandroid-libbacktrace=disabled -Dvulkan-layers= -Dtools= -Dmicrosoft-clc=disabled -Dallow-kcmp=disabled
ninja -C "$OBJ"
rm -rf "$STAGE"
DESTDIR="$STAGE" meson install -C "$OBJ" --no-rebuild
cp -r "$STAGE/usr/include/." "$MLIBC_ROOT/include/"
printf 'Mesa %s staged in %s\n' "$MESA_VERSION" "$STAGE"
cp -r "$SRC/include/vulkan" "$SRC/include/vk_video" "$MLIBC_ROOT/include/"

# 3. What InstantOS loads: the libraries stripped, under their SONAMEs (the
# initrd has no symlinks), and the tests (tools/mesa-tests).
# CMake bundles build/mesa-root/initrd into /lib/mlibc and /bin when it exists.
RUNTIME="$STAGE/initrd"
mkdir -p "$RUNTIME"
for lib in libEGL.so.1 libGLESv2.so.2 libglapi.so.0 "libgallium-$MESA_VERSION.so" libvulkan_intel.so; do
  llvm-strip --strip-unneeded -o "$RUNTIME/$lib" "$STAGE/lib/mlibc/$lib"
done
TEST_CFLAGS="--target=x86_64-unknown-elf -O2 -fPIE -fno-stack-protector -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -D__linux__ -D__unix__"
TEST_LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -L$STAGE/lib/mlibc -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc $MLIBC_ROOT/lib/crt1.o"
clang $TEST_CFLAGS -c "$ROOT/tools/mesa-tests/gl_offscreen.c" -o "$RUNTIME/gl_offscreen.o"
clang $TEST_LDFLAGS "$RUNTIME/gl_offscreen.o" -lEGL -lGLESv2 -lc -o "$RUNTIME/gl-offscreen"
clang $TEST_CFLAGS -c "$ROOT/tools/mesa-tests/gl_window.c" -o "$RUNTIME/gl_window.o"
clang $TEST_LDFLAGS "$RUNTIME/gl_window.o" -lEGL -lGLESv2 -lm -lc -o "$RUNTIME/gl-window"
clang $TEST_CFLAGS -c "$ROOT/tools/mesa-tests/vk_offscreen.c" -o "$RUNTIME/vk_offscreen.o"
clang $TEST_LDFLAGS "$RUNTIME/vk_offscreen.o" -ldl -lc -o "$RUNTIME/vk-offscreen"
# vk-window's shaders, compiled to SPIR-V arrays (glslang-tools).
SHADERS="$WORK/vk-window-shaders"
mkdir -p "$SHADERS"
glslangValidator -V --vn kCubeVert -o "$SHADERS/vk_window_cube.vert.h" "$ROOT/tools/mesa-tests/vk_window_cube.vert" >/dev/null
glslangValidator -V --vn kCubeFrag -o "$SHADERS/vk_window_cube.frag.h" "$ROOT/tools/mesa-tests/vk_window_cube.frag" >/dev/null
clang $TEST_CFLAGS -I"$SHADERS" -c "$ROOT/tools/mesa-tests/vk_window.c" -o "$RUNTIME/vk_window.o"
clang $TEST_LDFLAGS "$RUNTIME/vk_window.o" -ldl -lm -lc -o "$RUNTIME/vk-window"
rm -f "$RUNTIME"/*.o
du -sh "$RUNTIME"/*
