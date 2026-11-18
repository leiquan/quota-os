/* Platform adapter. Upstream CSSParserFastPaths parses the hex.
 * The pixels are the kernel framebuffer service. This file does not
 * program the UART or the page tables. The direct framebuffer pointer
 * is a bring-up adapter, not a protected display service. */
#include "config.h"
#include "CSSParserFastPaths.h"

#include <wtf/text/StringView.h>

extern "C" int fb_reserved_range(unsigned long long *start, unsigned long long *bytes);

static int parse_hex(const char *hex, unsigned hex_len, unsigned *rgba)
{
    auto parsed = WebCore::CSSParserFastPaths::parseHexColor(
        WTF::StringView(std::span<const char>(hex, hex_len)));
    if (!parsed)
        return 0;
    auto parts = asColorComponents(parsed->resolved());
    *rgba = (static_cast<unsigned>(parts[0]) << 16)
        | (static_cast<unsigned>(parts[1]) << 8)
        | static_cast<unsigned>(parts[2])
        | (static_cast<unsigned>(parts[3]) << 24);
    return 1;
}

extern "C" int quotaos_webcore_paint_hex(const char *hex, unsigned hex_len,
    unsigned *rgba_out, unsigned *painted, unsigned *total)
{
    unsigned rgba = 0;
    if (!parse_hex(hex, hex_len, &rgba))
        return 0;
    unsigned long long at = 0, bytes = 0;
    if (fb_reserved_range(&at, &bytes) != 0 || bytes < 4)
        return 0;
    auto *px = reinterpret_cast<unsigned *>(at);
    unsigned count = static_cast<unsigned>(bytes / 4);
    for (unsigned i = 0; i < count; i++)
        px[i] = rgba;
    unsigned same = 0;
    for (unsigned i = 0; i < count; i++)
        same += px[i] == rgba;
    if (rgba_out)
        *rgba_out = rgba;
    if (painted)
        *painted = same;
    if (total)
        *total = count;
    return same == count;
}
