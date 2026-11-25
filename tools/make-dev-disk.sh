#!/usr/bin/env bash
# A root disk that gives the desktop system (build/iboot.iso) a C/C++ compiler:
# gcc 13.3.0 with binutils, mlibc's headers and libraries and libstdc++ under
# /usr, plus make, coreutils and the GNU text tools, on the ext4 volume the
# kernel mounts at /. The desktop terminal's bash
# finds it on its default PATH (/usr/bin) and the programs it builds load
# through the system's /lib/mlibc/ld-instantos.so, which also searches /usr/lib
# (libstdc++.so.6, libgcc_s.so.1).
#
#   tools/make-dev-disk.sh [DISK]          (default build/dev-disk.img; run in WSL)
#
# The toolchain comes from TOOLCHAIN_IMG (default build/p73/p73-run.img: the P7.3
# disk, whose /usr holds the stage3 gcc the OS built itself, installed in-OS)
# or, when TOOLCHAIN_ROOT is set, from that directory's usr/ (e.g. the cross-built
# ~/gcc-port/gcchost-root). Examples go to /examples. AUTORUN=FILE also puts FILE
# at /autorun.sh, which login runs once the desktop is up (run-dev-smoke-win.sh).
# DISK_SIZE defaults to 4G.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK="${1:-$ROOT/build/dev-disk.img}"
SIZE="${DISK_SIZE:-4G}"
TOOLCHAIN_IMG="${TOOLCHAIN_IMG:-$ROOT/build/p73/p73-run.img}"
# Stage the files and make the image in the Linux file system, then copy the
# image out in one go: on /mnt/d (drvfs) files lose their modes, and mke2fs
# writing a 4 GB image there took over 12 minutes.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/devdisk.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"
IMG="$WORK/disk.img"
mkdir -p "$STAGE"

if [ -n "${TOOLCHAIN_ROOT:-}" ]; then
  [ -x "$TOOLCHAIN_ROOT/usr/bin/gcc" ] || { echo "no usr/bin/gcc under $TOOLCHAIN_ROOT" >&2; exit 2; }
  cp -a "$TOOLCHAIN_ROOT/usr" "$STAGE/usr"
else
  [ -f "$TOOLCHAIN_IMG" ] || { echo "missing $TOOLCHAIN_IMG (or set TOOLCHAIN_ROOT)" >&2; exit 2; }
  debugfs -R "rdump /usr $STAGE" "$TOOLCHAIN_IMG?offset=1048576" >/dev/null 2>&1
  [ -x "$STAGE/usr/bin/gcc" ] || { echo "no /usr/bin/gcc on $TOOLCHAIN_IMG" >&2; exit 2; }
fi
# The build trees' leftovers are not part of a toolchain.
rm -rf "$STAGE/usr/share/info" "$STAGE/usr/share/man"

# The GNU userland a build needs (the system image ships only bash): make, the
# text tools and coreutils, the mlibc builds the self-host images use
# (tools/build-gnu-tool.sh, tools/build-coreutils.sh). Missing ones are skipped.
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
for t in make sed grep awk tar gzip diff cmp m4 find xargs; do
  [ -f "$BUILD_DIR/$t" ] && install -m 0755 "$BUILD_DIR/$t" "$STAGE/usr/bin/$t" || echo "note: no build/$t" >&2
done
[ -f "$STAGE/usr/bin/awk" ] && ln -sf awk "$STAGE/usr/bin/gawk"
for f in "$BUILD_DIR"/coreutils/*; do
  [ -f "$f" ] && install -m 0755 "$f" "$STAGE/usr/bin/$(basename "$f")"
done
install -m 0755 "$ROOT/tools/gcc-selfhost/egrep" "$STAGE/usr/bin/egrep"
install -m 0755 "$ROOT/tools/gcc-selfhost/fgrep" "$STAGE/usr/bin/fgrep"

mkdir -p "$STAGE/examples" "$STAGE/home" "$STAGE/tmp"
cat > "$STAGE/examples/hello.c" <<'EOF'
#include <stdio.h>
#include <math.h>

int main(void) {
    printf("hello from gcc on InstantOS, sqrt(2) = %.6f\n", sqrt(2.0));
    return 0;
}
EOF
cat > "$STAGE/examples/hello.cpp" <<'EOF'
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> words = {"hello", "from", "g++", "on", "InstantOS"};
    std::string line;
    for (const auto& w : words) line += (line.empty() ? "" : " ") + w;
    try {
        throw std::runtime_error(line);
    } catch (const std::exception& e) {
        std::cout << e.what() << std::endl;
    }
    return 0;
}
EOF
cat > "$STAGE/examples/README" <<'EOF'
gcc 13.3.0 (C and C++), built on InstantOS by itself (the P7.2 bootstrap).
Try, in the terminal:
    gcc /examples/hello.c -o /tmp/hello -lm && /tmp/hello
    g++ /examples/hello.cpp -o /tmp/hellopp && /tmp/hellopp
EOF
if [ -n "${AUTORUN:-}" ]; then
  cp "$AUTORUN" "$STAGE/autorun.sh"
fi

truncate -s "$SIZE" "$IMG"
printf 'label: dos\nstart=2048, type=83\n' | sfdisk "$IMG" >/dev/null 2>&1
BLOCKS=$(( ($(stat -c %s "$IMG") - 1048576) / 4096 ))
mke2fs -q -F -b 4096 -O "has_journal,extent,filetype,sparse_super,^metadata_csum,^64bit,^uninit_bg,^dir_index" \
  -E offset=1048576,lazy_itable_init=0 -d "$STAGE" "$IMG" "$BLOCKS"
mkdir -p "$(dirname "$DISK")"
rm -f "$DISK"
cp "$IMG" "$DISK"
echo "DEV_DISK=$DISK ($(du -sh "$STAGE/usr" | cut -f1) toolchain)"
