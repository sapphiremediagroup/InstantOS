#!/usr/bin/env bash
set -euo pipefail

cmake_args=()
if [ "${ENABLE_TCC:-auto}" = "1" ] || { [ "${ENABLE_TCC:-auto}" = "auto" ] && [ -f outside/iUserApps/outside/tinycc/configure ]; }; then
  cmake_args+=(-DINSTANTOS_ENABLE_TCC=ON)
fi

# Optional GNU bash + mlibc runtime. These are produced by tools/build-mlibc.sh
# and tools/build-bash.sh (outside CMake) and bundled into the initrd when
# present. The bash cross-build is slow, so it is opt-in and cached: it is only
# (re)built when missing, unless BASH_FORCE=1 is set.
#
#   ENABLE_BASH=auto (default) -> build if the bash port sources are available
#   ENABLE_BASH=1              -> always build
#   ENABLE_BASH=0              -> never build
ENABLE_BASH="${ENABLE_BASH:-auto}"
if [ "$ENABLE_BASH" = "auto" ]; then
  if [ -f outside/iUserApps/outside/mlibc/meson.build ]; then
    ENABLE_BASH=1
  else
    ENABLE_BASH=0
  fi
fi

build_bash_artifacts() {
  # Keep the cross-built artifacts outside build/ so `rm -rf build` does not
  # discard the (expensive) bash build between runs.
  local cache_dir="${BASH_CACHE_DIR:-$PWD/.bash-cache}"
  local mlibc_root="$cache_dir/mlibc-root"
  local bash_bin="$cache_dir/bash"

  mkdir -p "$cache_dir"

  if [ "${BASH_FORCE:-0}" = "1" ] || [ ! -f "$mlibc_root/lib/libc.so" ]; then
    echo "[build.sh] building mlibc runtime..."
    MLIBC_INSTALL_DIR="$mlibc_root" MLIBC_BUILD_DIR="$cache_dir/mlibc-build" \
      tools/build-mlibc.sh
  else
    echo "[build.sh] mlibc runtime cached at $mlibc_root (set BASH_FORCE=1 to rebuild)"
  fi

  if [ "${BASH_FORCE:-0}" = "1" ] || [ ! -f "$bash_bin" ]; then
    echo "[build.sh] cross-building GNU bash..."
    MLIBC_ROOT="$mlibc_root" OUTPUT="$bash_bin" WORK_DIR="$cache_dir/bash-port" \
      tools/build-bash.sh
  else
    echo "[build.sh] bash cached at $bash_bin (set BASH_FORCE=1 to rebuild)"
  fi

  # Stage the artifacts where CMakeLists.txt expects them inside build/.
  mkdir -p build/mlibc-root/lib
  cp "$bash_bin" build/bash
  for lib in ld-instantos.so libc.so libdl.so; do
    cp "$mlibc_root/lib/$lib" "build/mlibc-root/lib/$lib"
  done
}

rm -rf build
mkdir -p build

if [ "$ENABLE_BASH" = "1" ]; then
  build_bash_artifacts
fi

cmake -S . -B build "${cmake_args[@]}"
cmake --build build --target iso --parallel 4
./run.sh
