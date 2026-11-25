#!/usr/bin/env bash
# Phase 7 P7.2: pack the slim, instantos-patched gcc-13.3.0 source that the
# in-OS gcc bootstraps (build/gcc-selfhost-gcc/gcc-insrc.tar.gz; make-gcc-disk.sh
# puts it on the disk at /src). Takes the tree tools/gcc-port prepared for the
# hosted build (port applied, prerequisites downloaded and patched) and leaves
# out what a C/C++ bootstrap never touches: the testsuites, the other front ends
# (except gcc/d: gcc/Makefile.in always reads d/d-target.def for genhooks)
# and their runtimes, isl (configured --without-isl) and the prerequisite
# tarballs. gmp/mpfr/mpc are symlinks in the tree; they are stored as real
# directories under those names, where gcc's configure looks for them.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
PORT_DIR="${GCC_PORT_DIR:-$HOME/gcc-port}"
VER="gcc-13.3.0"
SRC="$PORT_DIR/$VER"
OUT_DIR="$BUILD_DIR/gcc-selfhost-gcc"
OUT="$OUT_DIR/gcc-insrc.tar.gz"

[ -f "$SRC/gcc/config/instantos.h" ] || { echo "missing patched $SRC (run tools/gcc-port/build-gcc-hosted.sh)" >&2; exit 2; }
for d in gmp-6.2.1 mpfr-4.1.0 mpc-1.2.1; do
  [ -d "$SRC/$d" ] || { echo "missing $SRC/$d (download_prerequisites)" >&2; exit 2; }
done
mkdir -p "$OUT_DIR"

X=()
for d in libgomp/testsuite libatomic/testsuite \
         gcc/ada gcc/go gcc/fortran gcc/m2 gcc/rust gcc/jit \
         libada gnattools gotools libgo libphobos libgfortran libgm2 libobjc \
         libsanitizer libvtv libitm libffi libgomp libquadmath libssp \
         isl isl-0.24 gmp mpfr mpc .git; do
  X+=(--exclude="$VER/$d")
done
# The testsuites go too, except what the build reads: gcc's selftest data (the
# stage builds run cc1 -fself-test on it) and libstdc++'s testsuite Makefile
# (its configure generates testsuite/Makefile).
keep_only() {   # keep_only DIR NAME...: exclude every entry of DIR but NAMEs
  local dir=$1 e n; shift
  for e in "$SRC/$dir"/*; do
    n=${e##*/}
    case " $* " in *" $n "*) ;; *) X+=(--exclude="$VER/$dir/$n") ;; esac
  done
}
keep_only gcc/testsuite selftests
keep_only libstdc++-v3/testsuite Makefile.am Makefile.in

tar -C "$PORT_DIR" -czf "$OUT.tmp" "${X[@]}" --exclude="$VER/*.tar.*" \
  --transform="s,^$VER/gmp-6.2.1,$VER/gmp," \
  --transform="s,^$VER/mpfr-4.1.0,$VER/mpfr," \
  --transform="s,^$VER/mpc-1.2.1,$VER/mpc," \
  "$VER"
mv "$OUT.tmp" "$OUT"
ls -la "$OUT"
