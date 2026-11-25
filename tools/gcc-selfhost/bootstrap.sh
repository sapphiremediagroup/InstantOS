#!/usr/bin/env bash
# Build everything the GCC self-host runs need, from a fresh clone, in order:
#   mlibc -> bash -> system build (kernel, managers, libinstant, tcc sysroot)
#   -> GNU tools (make sed grep tar gzip awk cmp diff m4 coreutils)
#   -> binutils cross -> GCC cross (C, C++, libstdc++) -> mlibc built by GCC
#   -> binutils and GCC hosted on InstantOS -> gcchost root + image (Phase 6)
#   -> binutils source tarball + P7.1 image.
# Each step is skipped when its output exists (FORCE=1 redoes them all).
# Logs go to $GCC_PORT_DIR/logs/<step>.log. JOBS bounds the GCC builds
# (each cc1plus of GCC's big files takes several hundred MB).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# LLVM 20 first: from clang 21 or so the bare-metal driver (x86_64-unknown-elf,
# which every port uses) links -Bstatic and drops -shared. Ubuntu installs it
# at /usr/lib/llvm-20, Arch (clang20/lld20/llvm20) at /usr/lib/llvm20.
for d in "${LLVM_BIN:-}" /usr/lib/llvm-20/bin /usr/lib/llvm20/bin; do
  if [ -n "$d" ] && [ -x "$d/clang" ]; then export PATH="$d:$PATH"; break; fi
done
echo "clang: $(command -v clang) ($(clang --version | head -1))"
export GCC_PORT_DIR="${GCC_PORT_DIR:-$HOME/gcc-port}"
export JOBS="${JOBS:-$(nproc)}"
B="$ROOT/build"
LOGS="$GCC_PORT_DIR/logs"
GNU_WORK="${GNU_WORK:-$HOME/gnu-ports}"
mkdir -p "$LOGS" "$GNU_WORK"
cd "$ROOT"

step() {
  local name=$1 done_file=$2; shift 2
  if [ "${FORCE:-0}" != "1" ] && [ -e "$done_file" ]; then
    echo "skip  $name"; return 0
  fi
  echo "build $name ($(date +%H:%M))"
  if ! "$@" > "$LOGS/$name.log" 2>&1; then
    echo "FAIL  $name (log: $LOGS/$name.log)"; tail -30 "$LOGS/$name.log"; exit 1
  fi
}

gcc_source() ( # subshell: it changes directory
  mkdir -p "$GCC_PORT_DIR" && cd "$GCC_PORT_DIR"
  [ -f gcc-13.3.0.tar.xz ] || curl -fsSL -o gcc-13.3.0.tar.xz https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/gcc-13.3.0.tar.xz
  [ -d gcc-13.3.0 ] || tar xf gcc-13.3.0.tar.xz
  cd gcc-13.3.0 && { [ -d gmp ] || ./contrib/download_prerequisites; }
)

gnu_tools() {
  WORK_DIR="$GNU_WORK/make-port" bash tools/build-make.sh
  for t in sed grep tar gzip awk cmp diff m4; do
    WORK_DIR="$GNU_WORK/$t-port" bash tools/build-gnu-tool.sh "$t"
  done
  WORK_DIR="$GNU_WORK/coreutils-port" bash tools/build-coreutils.sh
}

system_build() {
  cmake -S "$ROOT" -B "$B" -DINSTANTOS_ENABLE_TCC=ON
  cmake --build "$B" --target iso --parallel "$JOBS"
}

step mlibc-fetch     outside/iUserApps/outside/mlibc/meson.build  bash tools/fetch-mlibc.sh
step mlibc           "$B/mlibc-root/lib/libc.so"                 bash tools/build-mlibc.sh
step bash            "$B/bash"                                   bash tools/build-bash.sh
step system          "$B/iboot.iso"                              system_build
step gnu-tools       "$B/coreutils/expr"                         gnu_tools
step binutils-cross  "$B/binutils-cross/bin/x86_64-unknown-instantos-ld" bash tools/binutils-port/build-binutils-cross.sh
step gcc-source      "$GCC_PORT_DIR/gcc-13.3.0/gmp"              gcc_source
step gcc-cross       "$GCC_PORT_DIR/gcc-install/x86_64-unknown-instantos/lib/libstdc++.so.6" \
                     env LANGS=c,c++ bash tools/gcc-port/build-gcc-cross.sh
step mlibc-gcc       "$B/mlibc-root-gcc/lib/libc.so"             bash tools/build-mlibc-gcc.sh
step binutils-hosted "$B/binutils-hosted-root/bin/as"            bash tools/binutils-port/build-binutils-hosted.sh
step gcc-hosted      "$GCC_PORT_DIR/gcc-hosted-install/usr/bin/gcc" bash tools/gcc-port/build-gcc-hosted.sh
step gcchost-root    "$GCC_PORT_DIR/gcchost-root/usr/bin/gcc"    bash tools/gcc-port/assemble-gcchost-root.sh
step gcchost-iso     "$B/gcchost/ext4.img"                       env SKIP_CMAKE=1 bash tools/gcc-port/build-gcchost-iso.sh
step binutils-insrc  "$B/binutils-port/binutils-insrc.tar.gz"    bash tools/gcc-selfhost/make-binutils-insrc.sh
step p71-iso         "$B/gcc-selfhost-binutils/gcc-selfhost-binutils.iso" \
                     env SKIP_CMAKE=1 bash tools/gcc-selfhost/build-binutils-iso.sh
echo "done: $B/gcchost/gcchost.iso (Phase 6), $B/gcc-selfhost-binutils/gcc-selfhost-binutils.iso (P7.1), disk $B/gcchost/ext4.img"
