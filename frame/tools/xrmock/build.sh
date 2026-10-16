#!/bin/bash
# Builds the mock OpenXR runtime: frame/tools/xrmock/build.sh [out dir]
# Writes libxrmock.so and runtime.json (for XR_RUNTIME_JSON) into the out dir
# (default ~/.cache/pf-xrmock).  Standalone: clang and the OpenXR headers
# (OPENXR_INCLUDE, default /usr/include), nothing of the game's build.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$HOME/.cache/pf-xrmock}
INC=${OPENXR_INCLUDE:-/usr/include}   # a dir holding openxr/openxr.h (the SDK headers, or /usr/include/openxr)
[ -f "$INC/openxr/openxr.h" ] || { echo "no OpenXR headers: set OPENXR_INCLUDE" >&2; exit 1; }
mkdir -p "$OUT"
clang -O1 -g -Wall -Wextra -shared -fPIC -o "$OUT/libxrmock.so" "$HERE/xrmock.c" "$HERE/xrmock_gles.c" "$HERE/xrmock_vk.c" -I "$INC" -I "$HERE" -lGLESv2 -lEGL -lm -ldl
sed "s#@LIB@#$OUT/libxrmock.so#" "$HERE/runtime.json.in" > "$OUT/runtime.json"
echo "$OUT/runtime.json"
