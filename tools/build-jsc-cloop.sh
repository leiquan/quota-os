#!/bin/bash
# Build the real CLoop interpreter from the pinned WebKit tree.
# This does not link JavaScriptCore into Quota OS and it does not run JS.
#
# Pin: tag WebKit-7625.2.8.18.3
#      commit 4d967f4ca7bdd75319e732247e714c2bc634555e
# Flags: ENABLE_JIT=0 ENABLE_C_LOOP=1 ENABLE_YARR_JIT=0 ENABLE_WEBASSEMBLY=0
#        asserts on (no NDEBUG), so the generated LLInt matches OFFLINE_ASM_ASSERT_ENABLED.
#
# The tree needs the patch port/quotaos/patches/jsc-cloop-7625.2.8.18.3.patch
# (16-bit CLoop register accessors, storeh lowering, return helpers 32-37).
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WEBKIT_SRC=${WEBKIT_SRC:-/tmp/WebKit}
OUT=${JSC_BUILD:-/tmp/jsc-build}
CXX=${CXX:-aarch64-linux-gnu-g++}

test -d "$WEBKIT_SRC/Source/JavaScriptCore/llint"
pin=$(git -C "$WEBKIT_SRC" rev-parse HEAD)
test "$pin" = "4d967f4ca7bdd75319e732247e714c2bc634555e"

if ! grep -q 'llint_cloop_did_return_from_js_37' "$WEBKIT_SRC/Source/JavaScriptCore/bytecode/BytecodeList.rb"; then
    git -C "$WEBKIT_SRC" apply "$ROOT/port/quotaos/patches/jsc-cloop-7625.2.8.18.3.patch"
fi

mkdir -p "$OUT/gen" "$OUT/include/JavaScriptCore"
ln -sfn "$OUT/gen/LLIntDesiredOffsets.h" "$OUT/include/JavaScriptCore/LLIntDesiredOffsets.h" || true
ln -sfn "$OUT/gen/LLIntDesiredSettings.h" "$OUT/include/JavaScriptCore/LLIntDesiredSettings.h" || true
ln -sfn "$OUT/gen/JSCBuiltins.h" "$OUT/include/JavaScriptCore/JSCBuiltins.h" || true

python3 "$WEBKIT_SRC/Source/JavaScriptCore/Scripts/generate-js-builtins.py" \
    --framework JavaScriptCore --output-directory "$OUT/gen" --combined \
    "$WEBKIT_SRC"/Source/JavaScriptCore/builtins/*.js \
    "$WEBKIT_SRC/Source/JavaScriptCore/inspector/InjectedScriptSource.js"

ruby "$WEBKIT_SRC/Source/JavaScriptCore/generator/main.rb" \
    --bytecodes_h "$OUT/gen/Bytecodes.h" \
    --init_bytecodes_asm "$OUT/gen/InitBytecodes.asm" \
    --bytecode_structs_h "$OUT/gen/BytecodeStructs.h" \
    --bytecode_indices_h "$OUT/gen/BytecodeIndices.h" \
    --wasm_json "$WEBKIT_SRC/Source/JavaScriptCore/wasm/wasm.json" \
    --bytecode_dumper "$OUT/gen/BytecodeDumperGenerated.cpp" \
    "$WEBKIT_SRC/Source/JavaScriptCore/bytecode/BytecodeList.rb"

cd "$WEBKIT_SRC/Source/JavaScriptCore"
ruby offlineasm/generate_settings_extractor.rb -I"$OUT/gen" \
    llint/LowLevelInterpreter.asm "$OUT/gen/LLIntDesiredSettings.h" C_LOOP

INCS=$(find "$WEBKIT_SRC/Source/JavaScriptCore" "$WEBKIT_SRC/Source/WTF" "$WEBKIT_SRC/Source/bmalloc" -type d -printf '-I %p ')
FLAGS=(-std=c++23 -O1 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector
    -Wno-expansion-to-defined -w
    -DENABLE_C_LOOP=1 -DENABLE_JIT=0 -DENABLE_YARR_JIT=0 -DENABLE_WEBASSEMBLY=0
    -I "$OUT/include" -I "$OUT/gen" $INCS -idirafter /usr/include)

$CXX "${FLAGS[@]}" llint/LLIntSettingsExtractor.cpp -o "$OUT/LLIntSettingsExtractor"
ruby offlineasm/generate_offset_extractor.rb -I"$OUT/gen" \
    llint/LowLevelInterpreter.asm "$OUT/LLIntSettingsExtractor" \
    "$OUT/gen/LLIntDesiredOffsets.h" C_LOOP normal
$CXX "${FLAGS[@]}" llint/LLIntOffsetsExtractor.cpp -o "$OUT/LLIntOffsetsExtractor"
CMAKE_CXX_COMPILER_ID=GNU ruby offlineasm/asm.rb -I"$OUT/gen" \
    llint/LowLevelInterpreter.asm "$OUT/LLIntOffsetsExtractor" \
    "$OUT/gen/LLIntAssembly.h" normal
$CXX "${FLAGS[@]}" -c llint/LowLevelInterpreter.cpp -o "$OUT/LowLevelInterpreter.o"

aarch64-linux-gnu-nm -C "$OUT/LowLevelInterpreter.o" | grep 'CLoop::execute'
echo "built $OUT/LowLevelInterpreter.o"
