#!/bin/bash
# Compile the WebCore CSS hex-color path. Sources are the pinned tree.
# -ffunction-sections lets the guest link drop unused style/render code.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
WEBKIT_SRC=${WEBKIT_SRC:-/tmp/WebKit}
WEB=$WEBKIT_SRC/Source/WebCore
DER=build/webcore-derived
if [ ! -f "$DER/HTTPHeaderNames.h" ] || [ ! -f "$DER/ColorData.cpp" ]; then
  "$ROOT/tools/generate-webcore-derived.sh"
fi
CAIRO_I=/tmp/arm64-debs/cairo/usr/include/cairo
mkdir -p build/webcore build/webcore-hdr/WebCore
python3 - "$DER" "$WEB" build/webcore-hdr/WebCore << 'PY'
import os, sys
gen, web, dest = sys.argv[1:]
os.makedirs(dest, exist_ok=True)
skip = ("/soup/", "/cf/", "/cocoa/", "/ios/", "/mac/", "/win/", "/avfoundation/", "/scripts/test/")
def link(src, name):
    dst = os.path.join(dest, name)
    if os.path.lexists(dst):
        os.remove(dst)
    os.symlink(os.path.abspath(src), dst)
for fn in os.listdir(gen):
    if fn.endswith((".h", ".hpp")):
        link(os.path.join(gen, fn), fn)
for dp, dns, fns in os.walk(web):
    if any(s in dp + "/" for s in skip):
        continue
    for fn in fns:
        if fn.endswith((".h", ".hpp")) and not os.path.lexists(os.path.join(dest, fn)):
            link(os.path.join(dp, fn), fn)
curl = os.path.join(web, "platform/network/curl")
if os.path.isdir(curl):
    for fn in os.listdir(curl):
        if fn.endswith(".h"):
            link(os.path.join(curl, fn), fn)
PY
INCS=$(cat /tmp/jsc-build/incs.line)
mapfile -t DIRS < <(find "$WEB" -type d)
INCDIRS=()
for d in "${DIRS[@]}"; do INCDIRS+=("-I" "$d"); done
: > build/webcore-color.rsp
compile_one() {
  local src=$1 base obj
  base=$(basename "$src" .cpp)
  obj="build/webcore/${base}.o"
  aarch64-linux-gnu-g++ -std=c++23 -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -ffunction-sections -fdata-sections -w \
    -DENABLE_C_LOOP=1 -DENABLE_JIT=0 -DUSE_SYSTEM_MALLOC=1 -DGIGACAGE_ENABLED=0 -DUSE_CAIRO=1 \
    -I "$DER" -I build/webcore-hdr -I "$CAIRO_I" "${INCDIRS[@]}" $INCS -idirafter /usr/include \
    -c "$src" -o "$obj"
  echo "$ROOT/$obj" >> build/webcore-color.rsp
}
compile_one port/quotaos/webcore_color.cpp
compile_one "$WEB/css/parser/CSSParserFastPaths.cpp"
compile_one "$WEB/platform/graphics/Color.cpp"
compile_one "$DER/ColorData.cpp"
echo "webcore color objects $(wc -l < build/webcore-color.rsp)"
