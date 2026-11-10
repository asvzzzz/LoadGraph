#include "color_utils.h"
#include <cstdio>

uint32_t HexToRgb(const char* hex)
{
    if (!hex) return 0xFFFFFFFFu;
    unsigned int r = 0, g = 0, b = 0;
    if (std::sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3)
        return 0xFFFFFFFFu;
    return (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8)  |
           static_cast<uint32_t>(b);
}
