#!/usr/bin/env bash
# Regenerates the embedded SPIR-V shaders used by the Venus GPU demos in
# src/graphics/venus.cpp:
#   - square_plus_one.comp -> kComputeSpirv[]  (compute: data[i] = i*i + 1)
#   - triangle.vert        -> kTriVertSpirv[]  (graphics: RGB triangle)
#   - triangle.frag        -> kTriFragSpirv[]
#
# Each is compiled to SPIR-V and emitted as a C array of little-endian uint32
# words. Requires glslangValidator (or glslc). Run from the repo root:
#   tools/venus-shader/build.sh
# Then paste the printed arrays over the corresponding arrays in venus.cpp.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

emit() {
  local src="$1" stage="$2" name="$3"
  local spv="$DIR/$(basename "${src%.*}").spv"
  if command -v glslangValidator >/dev/null 2>&1; then
    glslangValidator -V "$src" -o "$spv" >/dev/null
  elif command -v glslc >/dev/null 2>&1; then
    glslc -fshader-stage="$stage" "$src" -o "$spv"
  else
    echo "need glslangValidator or glslc" >&2
    exit 2
  fi
  python3 - "$spv" "$name" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
print('// %s: %d words (%d bytes)' % (sys.argv[2], len(words), len(data)))
for i in range(0, len(words), 6):
    print('    ' + ', '.join('0x%08x' % w for w in words[i:i+6]) + ',')
print()
PY
}

emit "$DIR/square_plus_one.comp" comp kComputeSpirv
emit "$DIR/triangle.vert"        vert kTriVertSpirv
emit "$DIR/triangle.frag"        frag kTriFragSpirv
