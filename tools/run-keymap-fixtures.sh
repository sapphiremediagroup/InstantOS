#!/usr/bin/env bash
# Build and run the host-side keymap fixture tests.
#
# These tests exercise the kernel's pure keyboard tables
# (include/interrupts/keymap.hpp): PS/2 scan codes, repeat, the keypad, US
# text, character -> key and the console's bytes per key. They run entirely on
# the host - no QEMU, OVMF, or kernel build required.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/keymap-fixtures}"
CXX="${CXX:-clang++}"
SRC="$ROOT/tools/keymap-fixtures/keymap_fixture_tests.cpp"
BIN="$BUILD_DIR/keymap_fixture_tests"

if ! command -v "$CXX" >/dev/null 2>&1; then
  printf 'error: host C++ compiler "%s" not found (set CXX=...)\n' "$CXX" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"

printf 'building keymap fixture tests with %s\n' "$CXX"
"$CXX" -std=c++23 -O2 -Wall -Wextra -I"$ROOT/include" "$SRC" -o "$BIN"

printf 'running keymap fixture tests\n'
"$BIN"
