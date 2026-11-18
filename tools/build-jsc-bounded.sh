#!/bin/bash
# Rebuild JavaScriptCore for a 128 MB guest.
# BUSE_SYSTEM_MALLOC=1 selects the supported non-libpas allocator.
# GIGACAGE_ENABLED=0 matches the regenerated CLoop (no 64/128 GiB cage).
# Does not run JavaScript.
set -euo pipefail
WEBKIT_SRC=${WEBKIT_SRC:-/tmp/WebKit}
OUT=${JSC_BUILD:-/tmp/jsc-build}
OBJ=$OUT/obj-sm
LOG=$OUT/logs-sm
mkdir -p "$OBJ" "$LOG"
if ! grep -q '!defined(GIGACAGE_ENABLED)' "$WEBKIT_SRC/Source/bmalloc/bmalloc/BPlatform.h"; then
    echo "BPlatform.h is missing the GIGACAGE_ENABLED override" >&2
    exit 1
fi
INCS=$(cat "$OUT/incs.line")
: > "$OUT/sm-ok.list"
: > "$OUT/sm-fail.list"
export INCS OBJ LOG WEBKIT_SRC
cat > "$OUT/compile-sm.sh" << 'EOF'
#!/bin/bash
set -u
f="$1"
base=$(basename "$f")
# Keep generated files distinguishable.
case "$f" in
  /tmp/jsc-build/*) rel="${f#/tmp/jsc-build/}"; safe=$(printf '%s' "$rel" | tr '/' '_') ;;
  *) rel="${f#/tmp/WebKit/Source/}"; safe=$(printf '%s' "$rel" | tr '/' '_') ;;
esac
obj="$OBJ/${safe}.o"
log="$LOG/${safe}.log"
extra=""
case "$base" in
  OSAllocatorPOSIX.cpp|FileSystem.cpp|MappedFileDataPOSIX.cpp) extra="-DHAVE_MMAP=1" ;;
  BlockDirectory.cpp) extra="-include sys/mman.h" ;;
  DateMath.cpp|GregorianDateTime.cpp) extra="-DHAVE_LOCALTIME_R=1" ;;
  RandomDevice.cpp) extra="-DQUOTAOS_ENTROPY=1" ;;
esac
if aarch64-linux-gnu-g++ -std=c++23 -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector \
    -ffunction-sections -fdata-sections -Wno-expansion-to-defined -w \
    -DENABLE_C_LOOP=1 -DENABLE_JIT=0 -DENABLE_YARR_JIT=0 -DENABLE_WEBASSEMBLY=0 \
    -DGIGACAGE_ENABLED=0 -DUSE_SYSTEM_MALLOC=1 \
    $extra $INCS -idirafter /usr/include \
    -c "$f" -o "$obj" > "$log" 2>&1; then
  echo "$f" >> "$LOG/../sm-ok.list"
else
  err=$(rg -m1 "error:" "$log" || echo unknown)
  echo "$base | $err" >> "$LOG/../sm-fail.list"
fi
EOF
chmod +x "$OUT/compile-sm.sh"
{
  find "$WEBKIT_SRC/Source/JavaScriptCore/llint" \
       "$WEBKIT_SRC/Source/JavaScriptCore/interpreter" \
       "$WEBKIT_SRC/Source/JavaScriptCore/runtime" \
       "$WEBKIT_SRC/Source/JavaScriptCore/heap" \
       "$WEBKIT_SRC/Source/JavaScriptCore/parser" \
       "$WEBKIT_SRC/Source/JavaScriptCore/bytecode" \
       "$WEBKIT_SRC/Source/JavaScriptCore/bytecompiler" \
       "$WEBKIT_SRC/Source/JavaScriptCore/yarr" \
       "$WEBKIT_SRC/Source/JavaScriptCore/debugger" \
       "$WEBKIT_SRC/Source/JavaScriptCore/builtins" \
       "$WEBKIT_SRC/Source/JavaScriptCore/profiler" \
       "$WEBKIT_SRC/Source/JavaScriptCore/tools" \
       -name '*.cpp' ! -name '*Extractor.cpp'
  find "$WEBKIT_SRC/Source/JavaScriptCore/API" -name '*.cpp' \
       ! -path '*/tests/*' ! -path '*/glib/*' \
       ! -name 'JSStringRefCF.cpp' ! -name 'JSStringRefBSTR.cpp' \
       ! -name 'JSRemoteInspector.cpp' ! -name 'JSRemoteInspectorServer.cpp' \
       ! -name 'MARReportCrashPrivate.cpp' ! -name 'PASReportCrashPrivate.cpp'
  echo "$WEBKIT_SRC/Source/JavaScriptCore/jit/ExecutableAllocator.cpp"
  echo "$WEBKIT_SRC/Source/JavaScriptCore/jit/JITCode.cpp"
  find "$WEBKIT_SRC/Source/WTF" -name '*.cpp' \
       ! -path '*/win/*' ! -path '*/cf/*' ! -path '*/cocoa/*' ! -path '*/glib/*' \
       ! -path '*/playstation/*' ! -path '*/spi/*' ! -path '*/haiku/*' ! -path '*/fuchsia/*'
  find "$WEBKIT_SRC/Source/bmalloc/bmalloc" -name '*.cpp'
  echo "$OUT/gen/JSCBuiltins.cpp"
  echo "$OUT/gen/inspector/InspectorProtocolObjects.cpp"
  echo "$OUT/gen/yarr/YarrCanonicalizeUnicode.cpp"
  find "$WEBKIT_SRC/Source/WTF/wtf/dtoa" -name '*.cc'
} | sort -u > "$OUT/sm-sources.txt"
echo "sources $(wc -l < "$OUT/sm-sources.txt")"
xargs -P 4 -a "$OUT/sm-sources.txt" -n 1 "$OUT/compile-sm.sh"
echo "ok $(wc -l < "$OUT/sm-ok.list") fail $(wc -l < "$OUT/sm-fail.list")"
if [ -s "$OUT/sm-fail.list" ]; then
  sort "$OUT/sm-fail.list" | head -30
fi
