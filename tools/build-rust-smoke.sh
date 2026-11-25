#!/usr/bin/env bash
# Builds tools/rust-port/smoke (Rust's std on InstantOS) into build/rust-smoke:
# Rust 1.98 for x86_64-unknown-linux-musl, not linked statically, its libc being
# mlibc (tools/rust-port/mlibc-cc). Run it with the thread suite:
#   THREAD_PROGRAMS="bin/rust-smoke:$PWD/build/rust-smoke" QEMU_WIN=1 tools/run-thread-suite.sh
#
# Prerequisites: rustup with 1.98.0 and the x86_64-unknown-linux-musl target,
# tools/build-mlibc.sh, tools/build-libcxx.sh (libunwind).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
export MLIBC_ROOT="$BUILD_DIR/mlibc-root"
export PATH="$HOME/.cargo/bin:/usr/lib/llvm-20/bin:$PATH"
[ -f "$MLIBC_ROOT/lib/libunwind.a" ] || { echo "no libunwind in $MLIBC_ROOT (tools/build-libcxx.sh)" >&2; exit 2; }
chmod +x "$ROOT/tools/rust-port/mlibc-cc"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER="$ROOT/tools/rust-port/mlibc-cc"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C target-feature=-crt-static -C link-self-contained=no"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$HOME/rust-port/smoke-target}"   # off /mnt/d
OUT="$CARGO_TARGET_DIR/x86_64-unknown-linux-musl/release"
# cargo does not see changes to the linker script, compat.c or mlibc itself:
# without its fingerprint the program is compiled and linked again.
rm -rf "$OUT"/rust-smoke "$OUT"/deps/rust_smoke-* "$OUT"/.fingerprint/rust-smoke-*
cargo +1.98.0 build --release --target x86_64-unknown-linux-musl \
  --manifest-path "$ROOT/tools/rust-port/smoke/Cargo.toml"
# It must have gone through compat.c (tools/rust-port/wrapped-symbols).
SYMS="$(llvm-nm "$OUT/rust-smoke")"
if ! grep -q ' __wrap_sigaction$' <<< "$SYMS"; then
  echo "rust-smoke was not linked through tools/rust-port/compat.c" >&2
  exit 1
fi
cp "$OUT/rust-smoke" "$BUILD_DIR/rust-smoke"
echo "built $BUILD_DIR/rust-smoke"
