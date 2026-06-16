#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TCC_SOURCE_DIR="${TCC_SOURCE_DIR:-$ROOT/outside/iUserApps/outside/tinycc}"
TCC_REPO="${TCC_REPO:-https://repo.or.cz/tinycc.git}"
TCC_REV="${TCC_REV:-release_0_9_27}"

if [ -e "$TCC_SOURCE_DIR" ] && [ ! -d "$TCC_SOURCE_DIR/.git" ]; then
  printf 'refusing to overwrite non-git path: %s\n' "$TCC_SOURCE_DIR" >&2
  exit 2
fi

if [ ! -d "$TCC_SOURCE_DIR/.git" ]; then
  mkdir -p "$(dirname "$TCC_SOURCE_DIR")"
  git clone "$TCC_REPO" "$TCC_SOURCE_DIR"
fi

git -C "$TCC_SOURCE_DIR" fetch --depth 1 origin "$TCC_REV"
git -C "$TCC_SOURCE_DIR" checkout --detach FETCH_HEAD
printf 'tcc checkout ready: %s @ %s\n' "$TCC_SOURCE_DIR" "$TCC_REV"
