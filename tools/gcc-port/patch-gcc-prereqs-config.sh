#!/usr/bin/env bash
# P6.1/P6.3: the in-tree GCC prerequisites (gmp/mpfr/mpc/isl) ship ancient
# config.sub/config.guess that don't know the 'instantos' OS, so their configure
# fails ("config.sub ...-instantos failed") under the Canadian cross. Overlay
# GCC's own (already instantos-patched) config.sub + config.guess onto each.
set -euo pipefail
GCC_SRC="${GCC_SRC:-${GCC_PORT_DIR:-$HOME/gcc-port}/gcc-13.3.0}"

grep -q 'instantos' "$GCC_SRC/config.sub" || { echo "gcc config.sub lacks instantos; run apply-gcc-instantos-port.sh first" >&2; exit 2; }

# config.sub/config.guess may live at the package top or under build-aux/.
for p in gmp mpfr mpc isl; do
  d="$GCC_SRC/$p"
  [ -d "$d" ] || { echo "skip $p (not present)"; continue; }
  for sub in "" "/build-aux"; do
    for f in config.sub config.guess; do
      t="$d$sub/$f"
      if [ -f "$t" ]; then
        [ -f "$t.instantos-orig" ] || cp "$t" "$t.instantos-orig"
        cp "$GCC_SRC/$f" "$t"
        chmod +x "$t"
        echo "patched $p$sub/$f"
      fi
    done
  done
done
echo "prereq config.sub/config.guess overlaid with instantos-aware versions"
