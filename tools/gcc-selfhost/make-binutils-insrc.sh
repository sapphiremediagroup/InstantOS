#!/usr/bin/env bash
# Phase 7 P7.1: pack the slim, instantos-patched binutils-2.42 source that the
# in-OS gcc rebuilds (build/binutils-port/binutils-insrc.tar.gz, unpacked by
# launcher-binutils.c into /work). Takes the tree tools/binutils-port/
# build-binutils-cross.sh extracted and patched, and leaves out what the
# in-OS configure disables (gdb, sim, gold, gprofng, libctf's users) and the
# testsuites, so the initrd stays small.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
PORT_DIR="$BUILD_DIR/binutils-port"
VER="binutils-2.42"
SRC="$PORT_DIR/$VER"
OUT="$PORT_DIR/binutils-insrc.tar.gz"

[ -f "$SRC/configure" ] || { echo "missing $SRC (run tools/binutils-port/build-binutils-cross.sh)" >&2; exit 2; }
bash "$ROOT/tools/binutils-port/apply-instantos-port.sh" "$SRC"

tar -C "$PORT_DIR" -czf "$OUT.tmp" \
  --exclude="$VER/gdb" --exclude="$VER/gdbserver" --exclude="$VER/gdbsupport" \
  --exclude="$VER/gnulib" --exclude="$VER/sim" --exclude="$VER/gold" \
  --exclude="$VER/gprofng" --exclude="$VER/readline" --exclude="$VER/libdecnumber" \
  --exclude="$VER/libbacktrace" --exclude="*/testsuite" \
  "$VER"
mv "$OUT.tmp" "$OUT"
ls -la "$OUT"
