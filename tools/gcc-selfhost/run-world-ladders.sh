#!/usr/bin/env bash
# Phase 7 P7.3: the bash-fork and TinyCC self-host ladders on the world the
# stage3 gcc built in-OS (launcher-world.c). Windows (Git Bash) side, like
# run-world-suite.sh:
#
#   1. copy the in-OS results off the run disk (WSL debugfs rdump): the userland
#      (iUserApps-build), mlibc-root, out/{bash,tcc}, tcc-sysroot
#   2. on the build server, build each ladder image with them in place of the
#      cross builds (build-bashfork-iso.sh, build-selfhost-iso.sh; the ladders'
#      own driver programs, /bin/login, are compiled there by the GCC cross)
#   3. boot each here under WHPX: BASHFORK_RESULT_OK, SELFHOST_FIXPOINT_OK
#
#   tools/gcc-selfhost/run-world-ladders.sh RUNDISK [OUTDIR]
# BUILD_HOST (default hippo) holds the InstantOS tree at ~/InstantOS.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDISK="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="${2:-$(dirname "$RUNDISK")/world-ladders}"
HOST="${BUILD_HOST:-hippo}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
win2wsl() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr A-Z a-z)${p:2}"; }

echo "== [1/3] copy the in-OS world off $RUNDISK"
# rdump keeps the tcc sysroot's symlinks (crt1.o, libc.so); tar -h sends their
# targets, since the Windows side of the copy cannot be trusted with them.
wsl -e bash -c "set -e; I='$(win2wsl "$RUNDISK")?offset=1048576'; O=\$HOME/p73-ladders; rm -rf \$O; mkdir -p \$O/world/iUserApps-build
  for d in mlibc-root out tcc-sysroot; do debugfs -R \"rdump /world/\$d \$O/world\" \"\$I\" >/dev/null 2>&1; done
  debugfs -R \"rdump /world/iUserApps-build/out \$O/world/iUserApps-build\" \"\$I\" >/dev/null 2>&1
  debugfs -R \"dump /world/iUserApps-build/libinstant.so \$O/world/iUserApps-build/libinstant.so\" \"\$I\" >/dev/null 2>&1
  cd \$O && for f in world/out/bash world/out/tcc world/mlibc-root/lib/libc.so world/tcc-sysroot/lib/tcc/libtcc1.a world/iUserApps-build/libinstant.so; do [ -s \$f ] || { echo missing \$f; exit 2; }; done
  tar -chf '$(win2wsl "$OUT")/world.tar' world" || exit 2

echo "== [2/3] build the ladder images on $HOST"
ssh "$HOST" 'rm -rf ~/InstantOS/build/p73-ladders && mkdir -p ~/InstantOS/build/p73-ladders && tar -C ~/InstantOS/build/p73-ladders -xf -' < "$OUT/world.tar"
ssh "$HOST" 'set -e; cd ~/InstantOS && W=$PWD/build/p73-ladders/world && chmod +x $W/out/* $W/iUserApps-build/out/* &&
  export PATH=$PWD/build/binutils-cross/bin:$PATH &&
  GCC=$HOME/gcc-port/gcc-install/bin/x86_64-unknown-instantos-gcc &&
  export SKIP_CMAKE=1 CC=$GCC LD=$PWD/build/binutils-cross/bin/x86_64-unknown-instantos-ld CC_TARGET_FLAG= \
         LAUNCHER_EXTRA_CFLAGS="-isystem $($GCC -print-file-name=include)" \
         MLIBC_ROOT=$W/mlibc-root BASH_BIN=$W/out/bash TCC_BIN=$W/out/tcc TCC_SYSROOT=$W/tcc-sysroot \
         SUBSTRATE_DIR=$W/iUserApps-build/out LIBINSTANT_SO=$W/iUserApps-build/libinstant.so &&
  OUT_DIR=build/p73-ladders/bashfork bash tools/tcc-selfhost/build-bashfork-iso.sh | tail -2 &&
  OUT_DIR=build/p73-ladders/tcc-selfhost bash tools/tcc-selfhost/build-selfhost-iso.sh | tail -2' || exit 1
for f in bashfork/bashfork.iso bashfork/ahci.img tcc-selfhost/tcc-selfhost.iso tcc-selfhost/ahci.img; do
  mkdir -p "$OUT/$(dirname "$f")"
  ssh "$HOST" "gzip -1 -c ~/InstantOS/build/p73-ladders/$f" | gzip -dc > "$OUT/$f"
done

echo "== [3/3] boot them under WHPX"
rc=0
run() { # <dir> <iso> <ok marker> <done marker>
  (cd "$OUT/$1" && QEMU_SMP="${QEMU_SMP:-2}" QEMU_MEM="${QEMU_MEM:-2G}" STOP_MARKER="$4" \
    bash "$ROOT/tools/gcc-selfhost/run-image-win.sh" "$2" ahci.img "$1.log" "$3" "${LADDER_TIMEOUT:-1800}" > runner.out 2>&1)
  grep -a "${1^^}\|SELFHOST_\|BASHFORK_" "$OUT/$1/$1.log" | grep -av '^\[' | tail -8
  grep -aq "$3" "$OUT/$1/$1.log" && echo "$1: $3" || { echo "$1: no $3 (see $OUT/$1/$1.log)"; rc=1; }
}
run bashfork bashfork.iso BASHFORK_RESULT_OK BASHFORK_DONE
run tcc-selfhost tcc-selfhost.iso SELFHOST_FIXPOINT_OK SELFHOST_DONE
exit $rc
