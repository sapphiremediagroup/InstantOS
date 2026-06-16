#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
MLIBC_ROOT="${MLIBC_ROOT:-$BUILD_DIR/mlibc-root}"
SOURCE="${SOURCE:-$ROOT/outside/iUserApps/mlibc-hello/src/main.c}"
START_SOURCE="${START_SOURCE:-$ROOT/outside/iUserApps/mlibc-hello/src/start.S}"
OUTPUT="${OUTPUT:-$BUILD_DIR/mlibc-hello}"
OBJECT="$BUILD_DIR/mlibc-hello.o"
START_OBJECT="$BUILD_DIR/mlibc-hello-start.o"
INTERPRETER="${INTERPRETER:-/lib/mlibc/ld-instantos.so}"
RPATH="${RPATH:-/lib/mlibc}"
CC="${CC:-clang}"
LD="${LD:-ld.lld}"

if [ ! -f "$MLIBC_ROOT/lib/crt1.o" ] || [ ! -f "$MLIBC_ROOT/lib/libc.so" ]; then
  printf 'mlibc root is missing crt1.o or libc.so: %s\n' "$MLIBC_ROOT" >&2
  printf 'run tools/build-mlibc.sh first\n' >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"
"$CC" \
  --target=x86_64-unknown-elf \
  -ffreestanding \
  -fPIE \
  -fno-stack-protector \
  -nostdinc \
  -isystem "$MLIBC_ROOT/include" \
  -c "$SOURCE" \
  -o "$OBJECT"

"$CC" \
  --target=x86_64-unknown-elf \
  -ffreestanding \
  -fPIE \
  -c "$START_SOURCE" \
  -o "$START_OBJECT"

"$LD" \
  --gc-sections \
  --build-id=none \
  --hash-style=sysv \
  -z max-page-size=0x1000 \
  -pie \
  -e _start \
  --dynamic-linker "$INTERPRETER" \
  -rpath "$RPATH" \
  -o "$OUTPUT" \
  "$START_OBJECT" \
  "$OBJECT" \
  -L "$MLIBC_ROOT/lib" \
  -lc

printf 'mlibc hello built: %s\n' "$OUTPUT"
