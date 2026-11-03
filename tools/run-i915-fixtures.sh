#!/usr/bin/env bash
# Build and run the host-side Intel Gen9 (i915) fixture tests.
#
# These tests exercise the driver's pure logic
# (include/graphics/intel_gen9_logic.hpp): device ids, BAR and plane decoding,
# the GGC/BDSM decode, GGTT entries and the GTT range allocator, with register
# values from the HD 530 PC. They run entirely on the host - no QEMU (which
# has no Intel GPU), OVMF, or kernel build required.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build/i915-fixtures}"
CXX="${CXX:-clang++}"
SRC="$ROOT/tools/i915-fixtures/i915_fixture_tests.cpp"
BIN="$BUILD_DIR/i915_fixture_tests"

if ! command -v "$CXX" >/dev/null 2>&1; then
  printf 'error: host C++ compiler "%s" not found (set CXX=...)\n' "$CXX" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"

printf 'building i915 fixture tests with %s\n' "$CXX"
"$CXX" -std=c++23 -O2 -Wall -Wextra -I"$ROOT/include" "$SRC" -o "$BIN"

printf 'running i915 fixture tests\n'
"$BIN"
