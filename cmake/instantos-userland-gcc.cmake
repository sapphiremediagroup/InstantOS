# P5.5 Stage 4b: CMake toolchain to build the libinstant userland (ld-instantos,
# libinstant/ilibcxx, the managers) with the GCC cross-compiler instead of clang.
# Used to configure outside/iUserApps as a standalone project. binutils-cross
# must be on PATH so GCC's collect2 and the ld link-command resolve the linker.
# Linux (not Generic) so CMake keeps shared-library support (libinstant.so /
# ld-instantos.so are real ELF .so's); it still counts as a cross build, so no
# test binaries are executed.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{GCC_PORT_DIR})
  set(_GCC_PORT_DIR "$ENV{GCC_PORT_DIR}")
else()
  set(_GCC_PORT_DIR "$ENV{HOME}/gcc-port")
endif()
set(_GCC_PREFIX "${_GCC_PORT_DIR}/gcc-install/bin/x86_64-unknown-instantos-")
set(CMAKE_C_COMPILER   "${_GCC_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_GCC_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${_GCC_PREFIX}gcc")

# The cross compiler is freestanding (-nostdlib); it cannot link a hosted test
# executable, so restrict CMake's compiler checks to compile-only.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Consumed by outside/iUserApps/CMakeLists.txt.
set(INSTANTOS_USERLAND_GCC ON CACHE BOOL "" FORCE)
set(INSTANTOS_GCC_LD "x86_64-unknown-instantos-ld" CACHE STRING "" FORCE)
