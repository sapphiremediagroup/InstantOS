#!/usr/bin/env bash
# Fetch the Mozilla CA certificate bundle (PEM) for InstantOS's TLS stack.
#
# NetSurf (via libcurl + mbedTLS) does full certificate-chain validation, which
# needs a set of trusted root CAs. We use the curl project's periodically
# regenerated extract of Mozilla's CA store (cacert.pem) and bundle it into the
# initrd at /netsurf/etc/cacert.pem (the /netsurf subtree is already mounted by
# the kernel, so no new mount point is required).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
CACERT_URL="${CACERT_URL:-https://curl.se/ca/cacert.pem}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/cacert}"
OUT_FILE="$OUT_DIR/cacert.pem"

mkdir -p "$OUT_DIR"
if [ ! -s "$OUT_FILE" ] || [ "${CACERT_FORCE:-0}" = "1" ]; then
  printf 'fetching %s\n' "$CACERT_URL"
  curl -fsSL "$CACERT_URL" -o "$OUT_FILE"
fi

# Sanity check: must contain at least one PEM certificate block.
if ! grep -q 'BEGIN CERTIFICATE' "$OUT_FILE"; then
  printf 'cacert.pem does not look like a PEM bundle: %s\n' "$OUT_FILE" >&2
  exit 2
fi

CERT_COUNT="$(grep -c 'BEGIN CERTIFICATE' "$OUT_FILE")"
printf 'CA bundle ready: %s (%s certificates)\n' "$OUT_FILE" "$CERT_COUNT"
