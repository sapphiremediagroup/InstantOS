#!/usr/bin/env bash
# Threads in InstantOS: boots a test ISO whose /bin/login runs the programs of
# tools/thread-suite (every pthread wait on mlibc, memory shared between processes, signals, many open files, the C++ library's threads on
# libc++, libinstant's pthreads) and passes only with "[threads] SCORE ... fail=0".
#
#   QEMU_SMP=1|2|4   CPUs (default 2)     QEMU_WIN=1  Windows' QEMU with WHPX
#   THREAD_CFLAGS=   more flags for the C program
#   SKIP_CMAKE=1     do not rebuild the kernel and managers
#
# Prerequisites: tools/build-mlibc.sh; tools/build-libcxx.sh for the C++ half.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export OUT_DIR="${OUT_DIR:-$BUILD_DIR/thread-suite}"
MLIBC_ROOT="$BUILD_DIR/mlibc-root"
[ -f "$MLIBC_ROOT/lib/libc.so" ] || { echo "mlibc is not built (tools/build-mlibc.sh)" >&2; exit 2; }
mkdir -p "$OUT_DIR"
CFLAGS="--target=x86_64-unknown-elf -fPIE -fno-stack-protector -O2 -nostdlibinc -isystem $MLIBC_ROOT/include -D_GNU_SOURCE -Wall"
LDFLAGS="--target=x86_64-unknown-elf -pie -nostdlib -fuse-ld=lld -L$MLIBC_ROOT/lib -Wl,--dynamic-linker,/lib/mlibc/ld-instantos.so -Wl,-rpath,/lib/mlibc $MLIBC_ROOT/lib/crt1.o"
clang $CFLAGS ${THREAD_CFLAGS:-} -c "$ROOT/tools/thread-suite/thread_suite.c" -o "$OUT_DIR/thread_suite.o"
clang $LDFLAGS -o "$OUT_DIR/thread-suite" "$OUT_DIR/thread_suite.o" -lc
clang $CFLAGS -c "$ROOT/tools/thread-suite/shm_suite.c" -o "$OUT_DIR/shm_suite.o"
clang $LDFLAGS -o "$OUT_DIR/shm-suite" "$OUT_DIR/shm_suite.o" -lc
clang $CFLAGS -c "$ROOT/tools/thread-suite/signal_suite.c" -o "$OUT_DIR/signal_suite.o"
clang $LDFLAGS -o "$OUT_DIR/signal-suite" "$OUT_DIR/signal_suite.o" -lc
clang $CFLAGS -c "$ROOT/tools/thread-suite/fd_suite.c" -o "$OUT_DIR/fd_suite.o"
clang $LDFLAGS -o "$OUT_DIR/fd-suite" "$OUT_DIR/fd_suite.o" -lc
EXTRA="bin/thread-suite:$OUT_DIR/thread-suite bin/shm-suite:$OUT_DIR/shm-suite bin/signal-suite:$OUT_DIR/signal-suite bin/fd-suite:$OUT_DIR/fd-suite"
PROGRAMS='"/bin/thread-suite","/bin/shm-suite","/bin/signal-suite","/bin/fd-suite"'
if [ -f "$MLIBC_ROOT/lib/libc++.a" ] && [ "${NO_CXX:-0}" != 1 ]; then
  clang++ --target=x86_64-unknown-elf -std=c++20 -O2 -fPIE -fno-stack-protector -nostdlibinc -nostdinc++ \
    -isystem "$MLIBC_ROOT/include/c++/v1" -isystem "$MLIBC_ROOT/include" -D_GNU_SOURCE \
    -c "$ROOT/tools/thread-suite/cxx_threads.cpp" -o "$OUT_DIR/cxx_threads.o"
  clang++ $LDFLAGS -o "$OUT_DIR/cxx-threads" "$OUT_DIR/cxx_threads.o" -lc++ -lc++abi -lunwind -lc
  EXTRA="$EXTRA bin/cxx-threads:$OUT_DIR/cxx-threads"
  PROGRAMS="$PROGRAMS,\"/bin/cxx-threads\""
fi
if [ "${NO_NATIVE:-0}" != 1 ]; then   # libinstant's pthreads
  cmake -S "$ROOT" -B "$BUILD_DIR" > "$OUT_DIR/cmake.log" 2>&1 && cmake --build "$BUILD_DIR" --target native-threads >> "$OUT_DIR/cmake.log" 2>&1 || {
    grep -E "error" "$OUT_DIR/cmake.log" | head; exit 2; }
  EXTRA="$EXTRA bin/native-threads:$BUILD_DIR/native-threads"
  PROGRAMS="$PROGRAMS,\"/bin/native-threads\""
fi
for extra in ${THREAD_PROGRAMS:-}; do   # more programs: path/in/initrd:/host/path
  EXTRA="$EXTRA $extra"
  PROGRAMS="$PROGRAMS,\"/${extra%%:*}\""
done
for lib in ld-instantos.so libc.so libm.so libdl.so libpthread.so librt.so; do
  [ -f "$MLIBC_ROOT/lib/$lib" ] && EXTRA="$EXTRA lib/mlibc/$lib:$MLIBC_ROOT/lib/$lib"
done
printf '#define PROGRAMS %s\n#include "%s"\n' "$PROGRAMS" "$ROOT/tools/thread-suite/runner.c" > "$OUT_DIR/runner_main.c"
SUITE_TAG=threads RUNNER_SRC="$OUT_DIR/runner_main.c" EXTRA_INITRD="$EXTRA" \
QEMU_SMP="${QEMU_SMP:-2}" QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}" exec "$ROOT/tools/run-os-testsuite.sh"
