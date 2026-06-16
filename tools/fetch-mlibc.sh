#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLIBC_SOURCE_DIR="${MLIBC_SOURCE_DIR:-$ROOT/outside/iUserApps/outside/mlibc}"
MLIBC_REPO="${MLIBC_REPO:-https://github.com/managarm/mlibc.git}"
MLIBC_REV="${MLIBC_REV:-c367b780a47dc1d6c70d19a8379f4a74e5a7ed96}"

if [ -e "$MLIBC_SOURCE_DIR" ] && [ ! -d "$MLIBC_SOURCE_DIR/.git" ]; then
  printf 'refusing to overwrite non-git path: %s\n' "$MLIBC_SOURCE_DIR" >&2
  exit 2
fi

if [ ! -d "$MLIBC_SOURCE_DIR/.git" ]; then
  mkdir -p "$(dirname "$MLIBC_SOURCE_DIR")"
  git clone "$MLIBC_REPO" "$MLIBC_SOURCE_DIR"
fi

git -C "$MLIBC_SOURCE_DIR" fetch --depth 1 origin "$MLIBC_REV"
git -C "$MLIBC_SOURCE_DIR" checkout --detach "$MLIBC_REV"
printf 'mlibc checkout ready: %s @ %s\n' "$MLIBC_SOURCE_DIR" "$MLIBC_REV"
