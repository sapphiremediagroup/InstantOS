#!/usr/bin/env bash
# Build the membench image on the build server (tools/membench/membench.c as
# /bin/login of the feature-suite image) and run it, printing its table:
#
#   tools/membench/run-membench-win.sh [whpx|kvm|tcg]   (default whpx)
#
# whpx boots it here (Git Bash, QEMU for Windows), on this PC's CPU (WHPX
# passes its features through); kvm and tcg boot it on the build server
# (run-image.sh; kvm is nested there). BUILD_HOST defaults to hippo.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:-whpx}"
HOST="${BUILD_HOST:-hippo}"
OUT="$ROOT/build/membench"
mkdir -p "$OUT"

ssh "$HOST" 'cd ~/InstantOS && export PATH=/usr/lib/llvm20/bin:$PATH &&
  SKIP_CMAKE=1 SKIP_DISK=1 RUNNER_SRC=tools/membench/membench.c OUT_DIR=build/membench \
  bash tools/os-testsuite/build-testsuite-iso.sh | tail -1' || exit 1

case "$MODE" in
  whpx)
    ssh "$HOST" 'gzip -1 -c ~/InstantOS/build/membench/os-testsuite.iso' | gzip -dc > "$OUT/membench.iso"
    ssh "$HOST" 'gzip -1 -c ~/InstantOS/build/membench/ext4.img' | gzip -dc > "$OUT/ext4.img"
    # No -cpu: WHPX's default model passes the host's SSE/AVX/AVX2 through, and
    # -cpu host makes OVMF fault (#GP in PlatformPei) under WHPX.
    (cd "$OUT" && QEMU_SMP=1 QEMU_MEM=1G STOP_MARKER='\[membench\] END' \
      bash "$ROOT/tools/gcc-selfhost/run-image-win.sh" membench.iso ext4.img membench-whpx.log \
        '\[membench\] END' "${MEMBENCH_TIMEOUT:-900}" > runner.out 2>&1)
    grep -a '^\[membench\]' "$OUT/membench-whpx.log"
    ;;
  kvm|tcg)
    accel=""; [ "$MODE" = tcg ] && accel=tcg
    ssh "$HOST" "cd ~/InstantOS/build/membench && QEMU_ACCEL=$accel QEMU_SMP=1 QEMU_MEM=1G \
      bash ../../tools/gcc-selfhost/run-image.sh os-testsuite.iso ext4.img membench-$MODE.log \
        '\[membench\] END' ${MEMBENCH_TIMEOUT:-3600} > runner-$MODE.out 2>&1; grep -a '^\[membench\]' membench-$MODE.log"
    ;;
  *) echo "mode: whpx, kvm or tcg" >&2; exit 2 ;;
esac
