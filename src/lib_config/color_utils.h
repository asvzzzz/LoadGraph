#ifndef COLOR_UTILS_H
#define COLOR_UTILS_H

#include <cstdint>

// Cross-platform hex color parser.
// Returns a packed 0xRRGGBB value, or 0xFFFFFFFF on parse failure.
uint32_t HexToRgb(const char* hex);

#endif // COLOR_UTILS_H
