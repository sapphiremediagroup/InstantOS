#!/usr/bin/env bash
# Phase 7 P7.3: run the feature suite on the userland the stage3 gcc built in-OS
# (launcher-world.c). Windows (Git Bash) side, like run-image-win.sh:
#
#   1. copy the in-OS results off the run disk (WSL debugfs rdump):
#      /world/iUserApps-build/{libinstant.so,out/*} and /world/suite/*
#   2. on the build server, pack the suite image with them in place of the
#      clang/cross builds: managers, ld-instantos.so, libinstant.so, runner,
#      ostest-helper (build-testsuite-iso.sh; the kernel stays clang's)
#   3. boot it here under WHPX with a NIC, as run-os-testsuite.sh does
#
#   tools/gcc-selfhost/run-world-suite.sh RUNDISK [OUTDIR]
# BUILD_HOST (default hippo) holds the InstantOS tree at ~/InstantOS. Exits 0
# only on "[ostest] SCORE ... fail=0"; prints the SCORE line either way.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNDISK="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="${2:-$(dirname "$RUNDISK")/world-suite}"
HOST="${BUILD_HOST:-hippo}"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
win2wsl() { local p; p="$(cygpath -m "$1")"; echo "/mnt/$(echo "${p:0:1}" | tr A-Z a-z)${p:2}"; }

echo "== [1/3] copy the in-OS world off $RUNDISK"
rm -rf "$OUT/world"; mkdir -p "$OUT/world"
wsl -e bash -c "I='$(win2wsl "$RUNDISK")?offset=1048576'; O='$(win2wsl "$OUT/world")';
  for d in /world/iUserApps-build/out /world/suite; do debugfs -R \"rdump \$d \$O\" \"\$I\" >/dev/null 2>&1; done
  debugfs -R \"dump /world/iUserApps-build/libinstant.so \$O/libinstant.so\" \"\$I\" >/dev/null 2>&1; ls -l \$O \$O/out"
for f in libinstant.so out/ld-instantos.so out/input-manager out/storage-manager out/process-manager \
         out/network-manager out/font-manager out/session-manager suite/runner suite/ostest-helper; do
  [ -s "$OUT/world/$f" ] || { echo "missing in-OS output $f" >&2; exit 2; }
done

echo "== [2/3] pack the suite image on $HOST"
tar -C "$OUT" -cf - world | ssh "$HOST" 'rm -rf ~/InstantOS/build/p73-world && mkdir -p ~/InstantOS/build/p73-world && tar -C ~/InstantOS/build/p73-world -xf -'
ssh "$HOST" 'cd ~/InstantOS && W=build/p73-world/world && chmod +x $W/out/* $W/suite/* &&
  SKIP_CMAKE=1 SUBSTRATE_DIR=$W/out LIBINSTANT_SO=$W/libinstant.so RUNNER_BIN=$W/suite/runner HELPER_BIN=$W/suite/ostest-helper \
  OUT_DIR=build/p73-world/suite bash tools/os-testsuite/build-testsuite-iso.sh | tail -3' || exit 1
ssh "$HOST" 'gzip -1 -c ~/InstantOS/build/p73-world/suite/os-testsuite.iso' | gzip -dc > "$OUT/os-testsuite.iso"
ssh "$HOST" 'gzip -1 -c ~/InstantOS/build/p73-world/suite/ext4.img' | gzip -dc > "$OUT/ext4.img"

echo "== [3/3] boot it under WHPX"
cd "$OUT"
# The markers are grep patterns (run-image-win.sh): escape the brackets.
QEMU_SMP="${QEMU_SMP:-1}" QEMU_MEM="${QEMU_MEM:-1G}" STOP_MARKER='\[ostest\] END' \
QEMU_EXTRA="-device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0 -netdev user,id=net0" \
  bash "$ROOT/tools/gcc-selfhost/run-image-win.sh" os-testsuite.iso ext4.img suite.log '\[ostest\] END' "${SUITE_TIMEOUT:-1800}" > runner.out 2>&1
grep -a '\[ostest\] SCORE' suite.log || { echo "no SCORE line (see $OUT/suite.log)"; exit 1; }
grep -a '  FAIL' suite.log
grep -aq '\[ostest\] SCORE.* fail=0' suite.log
