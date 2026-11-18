#!/bin/bash
# Recreate WebCore derived headers from the pinned tree.
# Upstream scripts only. Output is build/webcore-derived, not a hand-written parser.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WEBKIT_SRC=${WEBKIT_SRC:-/tmp/WebKit}
WEB=$WEBKIT_SRC/Source/WebCore
OUT=$ROOT/build/webcore-derived
GPERF=${GPERF:-/usr/bin/gperf}
mkdir -p "$OUT"
cd "$OUT"

perl "$WEB/dom/make_names.pl" --outputDir "$OUT" \
  --attrs "$WEB/html/HTMLAttributeNames.in" \
  --elements "$WEB/html/HTMLTagNames.in" --factory --wrapperFactory
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" \
  --attrs "$WEB/svg/svgattrs.in" --elements "$WEB/svg/svgtags.in" --factory --wrapperFactory
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" \
  --attrs "$WEB/mathml/mathattrs.in" --elements "$WEB/mathml/mathtags.in" --factory --wrapperFactory
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" --enum Namespace \
  --elements "$WEB/html/HTMLTagNames.in" --elements "$WEB/svg/svgtags.in" --elements "$WEB/mathml/mathtags.in" \
  --attrs "$WEB/html/HTMLAttributeNames.in" --attrs "$WEB/mathml/mathattrs.in" \
  --attrs "$WEB/svg/svgattrs.in" --attrs "$WEB/svg/xlinkattrs.in" \
  --attrs "$WEB/xml/xmlattrs.in" --attrs "$WEB/xml/xmlnsattrs.in"
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" --enum NodeName \
  --elements "$WEB/html/HTMLTagNames.in" --elements "$WEB/svg/svgtags.in" --elements "$WEB/mathml/mathtags.in" \
  --attrs "$WEB/html/HTMLAttributeNames.in" --attrs "$WEB/mathml/mathattrs.in" \
  --attrs "$WEB/svg/svgattrs.in" --attrs "$WEB/svg/xlinkattrs.in" \
  --attrs "$WEB/xml/xmlattrs.in" --attrs "$WEB/xml/xmlnsattrs.in"
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" --enum TagName \
  --elements "$WEB/html/HTMLTagNames.in" --elements "$WEB/svg/svgtags.in" --elements "$WEB/mathml/mathtags.in" \
  --attrs "$WEB/html/HTMLAttributeNames.in" --attrs "$WEB/mathml/mathattrs.in" \
  --attrs "$WEB/svg/svgattrs.in" --attrs "$WEB/svg/xlinkattrs.in" \
  --attrs "$WEB/xml/xmlattrs.in" --attrs "$WEB/xml/xmlnsattrs.in"
perl "$WEB/dom/make_names.pl" --outputDir "$OUT" --fonts "$WEB/css/WebKitFontFamilyNames.in"
perl "$WEB/dom/make_event_factory.pl" --input "$WEB/dom/EventInterfaces.in" --outputDir "$OUT"
perl "$WEB/dom/make_event_factory.pl" --input "$WEB/dom/EventTargetFactory.in" --outputDir "$OUT"
python3 "$WEB/platform/network/create-http-header-name-table" "$WEB/platform/network/HTTPHeaderNames.in" "$GPERF"
python3 "$WEB/css/scripts/process-css-values.py" \
  --values "$WEB/css/CSSValueKeywords.in" --values "$WEB/css/SVGCSSValueKeywords.in" --gperf-executable "$GPERF"
python3 "$WEB/css/scripts/process-css-properties.py" \
  --properties "$WEB/css/CSSProperties.json" --gperf-executable "$GPERF"
python3 "$WEB/css/scripts/process-css-pseudo-selectors.py" \
  --selectors "$WEB/css/CSSPseudoSelectors.json" --defines "ENABLE_JIT=0" --gperf-executable "$GPERF"
python3 "$WEB/Scripts/generate-log-declarations.py" \
  "$WEB/platform/LogMessages.in" "$OUT/WebCoreLogDefinitions.h" "$OUT/WebCoreVirtualLogFunctions.h"
python3 "$WEB/Scripts/generate-process-sync-data.py" FrameTree "$WEB/page/FrameTreeSyncData.in" "$OUT"
perl "$WEB/make-hash-tools.pl" "$OUT" "$WEB/platform/ColorData.gperf" "$GPERF"

# This pin's Settings.yaml does not declare every settings-flag named by
# CSSProperties.json. GenerateSettings.rb adds those bools, default false.
python3 - "$WEB/page/Settings.yaml" "$WEB/css/CSSProperties.json" "$OUT/Settings-quotaos.yaml" << 'PY'
import re, sys
src, props, dest = sys.argv[1:]
text = open(src).read()
flags = sorted(set(re.findall(r'"settings-flag":\s*"([^"]+)"', open(props).read())))
chunks = {}
current = None
buf = []
def flush():
    if current:
        chunks[current] = "".join(buf)
for line in text.splitlines(True):
    m = re.match(r"^([A-Za-z0-9_]+):\s*$", line)
    if m:
        flush()
        current = m.group(1)
        buf = [line if line.endswith("\n") else line + "\n"]
    else:
        buf.append(line)
flush()
for flag in flags:
    if flag.startswith("Deprecated"):
        continue
    key = flag[0].upper() + flag[1:]
    if key in chunks:
        continue
    chunks[key] = (
        f"{key}:\n  webcoreName: {flag}\n  type: bool\n"
        "  defaultValue:\n    WebCore:\n      default: false\n"
    )
open(dest, "w").write("".join(chunks[k].rstrip() + "\n" for k in sorted(chunks)))
PY
ruby "$WEB/Scripts/GenerateSettings.rb" --outputDir "$OUT" \
  --template "$WEB/Scripts/SettingsTemplates/Settings.h.erb" \
  --template "$WEB/Scripts/SettingsTemplates/Settings.cpp.erb" \
  "$OUT/Settings-quotaos.yaml"

# Prefix header for this port. It does not parse HTML.
cat > "$OUT/config.h" << 'EOF'
#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <wtf/Assertions.h>
#include <wtf/GetPtr.h>
#include <wtf/HashTraits.h>
#include <wtf/Platform.h>
#include <wtf/TZoneMalloc.h>
#define WEBCORE_EXPORT
#define PAL_EXPORT
#define WTF_EXPORT_PRIVATE
EOF
echo "derived headers in $OUT"
ls "$OUT/HTTPHeaderNames.h" "$OUT/Namespace.h" "$OUT/Settings.h" "$OUT/ColorData.cpp" >/dev/null
