#!/usr/bin/env bash
# Cross-build LLVM's C++ runtime (libc++, libc++abi, libunwind; the same
# release as the host clang) against the InstantOS mlibc sysroot, for the C++
# parts of Mesa (its GLSL and Intel compilers).
#
# Static and position-independent, symbols hidden (hermetic), so each shared
# object that links them (Mesa's gallium and Vulkan drivers) carries its own
# copy without clashing. No localization (no iostreams/locale: mlibc has no
# locale layer libc++ knows, and the Mesa code we build uses none), threads
# through pthreads, exceptions through libunwind. Installed into the sysroot:
# include/c++/v1, lib/libc++.a, lib/libc++abi.a, lib/libunwind.a.
#
# The tree is kept in WSL (~/libcxx-port, LIBCXX_WORK) since it is large.
# Prerequisites: tools/build-mlibc.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLIBC_ROOT="${MLIBC_ROOT:-$ROOT/build/mlibc-root}"
LLVM_VERSION="${LLVM_VERSION:-20.1.2}"
WORK="${LIBCXX_WORK:-$HOME/libcxx-port}"
SRC="$WORK/llvm-project-$LLVM_VERSION.src"
OBJ="$WORK/build"
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/llvm-project-$LLVM_VERSION.src.tar.xz"

if [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc sysroot missing (%s); run tools/build-mlibc.sh first\n' "$MLIBC_ROOT" >&2
  exit 2
fi
mkdir -p "$WORK"
# (libc++'s from_chars uses LLVM libc's shared/fp_bits.h, hence libc/.)
if [ ! -d "$SRC/libcxx" ] || [ ! -d "$SRC/libc" ]; then
  printf 'fetching %s\n' "$URL"
  curl -sfL "$URL" -o "$WORK/llvm.tar.xz"
  tar -C "$WORK" -xJf "$WORK/llvm.tar.xz" --wildcards \
    "llvm-project-$LLVM_VERSION.src/libcxx/*" "llvm-project-$LLVM_VERSION.src/libcxxabi/*" \
    "llvm-project-$LLVM_VERSION.src/libunwind/*" "llvm-project-$LLVM_VERSION.src/runtimes/*" \
    "llvm-project-$LLVM_VERSION.src/cmake/*" "llvm-project-$LLVM_VERSION.src/llvm/cmake/*" \
    "llvm-project-$LLVM_VERSION.src/llvm/utils/*" "llvm-project-$LLVM_VERSION.src/third-party/*"     "llvm-project-$LLVM_VERSION.src/libc/*"
  rm -f "$WORK/llvm.tar.xz"
fi

# std::atomic's wait/notify (and the semaphores, latches and barriers made of it)
# sleep on kernel futexes (<instantos/futex.h>) instead of polling: 32-bit words.
python3 - "$SRC/libcxx" <<'PY'
import sys
def fix(path, old, new):
    s = open(path).read()
    if new in s:
        return
    assert s.count(old) == 1, path
    open(path, "w").write(s.replace(old, new))
fix(sys.argv[1] + "/include/__atomic/contention_t.h",
    "#if defined(__linux__) || (defined(_AIX) && !defined(__64BIT__))",
    "#if 1 // InstantOS: 32-bit futexes")
p = sys.argv[1] + "/src/atomic.cpp"
fix(p, "// Baseline needs no new headers\n", "#  include <instantos/futex.h> // InstantOS\n")
fix(p, """static void
__libcpp_platform_wait_on_address(__cxx_atomic_contention_t const volatile* __ptr, __cxx_contention_t __val) {
  __libcpp_thread_poll_with_backoff(
      [=]() -> bool { return !__cxx_nonatomic_compare_equal(__cxx_atomic_load(__ptr, memory_order_relaxed), __val); },
      __libcpp_timed_backoff_policy());
}

static void __libcpp_platform_wake_by_address(__cxx_atomic_contention_t const volatile*, bool) {}
""", """static void
__libcpp_platform_wait_on_address(__cxx_atomic_contention_t const volatile* __ptr, __cxx_contention_t __val) {
  instantos_futex_wait(reinterpret_cast<const volatile uint32_t*>(__ptr), static_cast<uint32_t>(__val), nullptr);
}

static void __libcpp_platform_wake_by_address(__cxx_atomic_contention_t const volatile* __ptr, bool __notify_one) {
  instantos_futex_wake(reinterpret_cast<const volatile uint32_t*>(__ptr), __notify_one ? 1 : INT_MAX);
}
""")
PY
# (mlibc has no __cxa_thread_atexit_impl, which the compile-only check cannot see:
# libc++abi runs the destructors of thread_local objects itself.)
FLAGS="-nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -fPIC -fno-stack-protector"
rm -rf "$OBJ"
cmake -G Ninja -S "$SRC/runtimes" -B "$OBJ" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_ASM_COMPILER=clang \
  -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-elf -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-elf \
  -DCMAKE_ASM_COMPILER_TARGET=x86_64-unknown-elf \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_ASM_FLAGS="$FLAGS" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_INSTALL_PREFIX="$MLIBC_ROOT" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;libunwind" \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
  -DLIBCXX_ENABLE_STATIC=ON -DLIBCXXABI_ENABLE_STATIC=ON -DLIBUNWIND_ENABLE_STATIC=ON \
  -DLIBCXX_HERMETIC_STATIC_LIBRARY=ON -DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON -DLIBUNWIND_HIDE_SYMBOLS=ON \
  -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON \
  -DLIBCXX_HAS_PTHREAD_API=ON -DLIBCXXABI_HAS_PTHREAD_API=ON -DLIBUNWIND_ENABLE_THREADS=ON \
  -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
  -DLIBCXX_ENABLE_LOCALIZATION=OFF -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF \
  -DLIBCXX_ENABLE_FILESYSTEM=OFF -DLIBCXX_ENABLE_RANDOM_DEVICE=ON -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
  -DLIBCXX_ENABLE_MONOTONIC_CLOCK=ON \
  -DLIBCXX_USE_COMPILER_RT=OFF -DLIBCXXABI_USE_COMPILER_RT=OFF -DLIBUNWIND_USE_COMPILER_RT=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_DOCS=OFF -DLIBUNWIND_INCLUDE_DOCS=OFF
ninja -C "$OBJ"
ninja -C "$OBJ" install
printf 'libc++ %s installed into %s\n' "$LLVM_VERSION" "$MLIBC_ROOT"

# cxx-smoke (tools/cxx-smoke): the runtime at work in InstantOS; CMake bundles
# build/cxx-smoke/cxx-smoke as /bin/cxx-smoke when it exists (rerun cmake).
SMOKE="$ROOT/build/cxx-smoke"
mkdir -p "$SMOKE"
clang++ --target=x86_64-unknown-elf -std=c++17 -O2 -fPIE -fno-stack-protector -nostdlibinc -nostdinc++ \
  -isystem "$MLIBC_ROOT/include/c++/v1" -isystem "$MLIBC_ROOT/include" -D_GNU_SOURCE \
  -c "$ROOT/tools/cxx-smoke/cxx_smoke.cpp" -o "$SMOKE/cxx_smoke.o"
clang++ --target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L"$MLIBC_ROOT/lib" \
  -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc "$MLIBC_ROOT/lib/crt1.o" \
  "$SMOKE/cxx_smoke.o" -lc++ -lc++abi -lunwind -lc -o "$SMOKE/cxx-smoke"
printf 'cxx-smoke in %s (rerun cmake to bundle it)\n' "$SMOKE"
