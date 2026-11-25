#!/bin/sh
# Compile the backend's GLSL to SPIR-V C arrays (committed; rerun after edits).
set -eu
cd "$(dirname "$0")"
for s in blit.vert blit.frag; do
    n=$(echo "$s" | tr . _)
    glslc --target-env=vulkan1.1 -O -mfmt=c "$s" -o "$n.spv.h"
done
