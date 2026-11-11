#pragma once

#include <cstdint>

/* Generated from GNU Unifont 18.0.01 (GB2312 + ASCII subset).
 * Unifont is licensed under SIL OFL 1.1; see licenses/unifont-OFL-1.1.txt. */
struct CjkGlyph {
    uint32_t cp;
    uint8_t width;   // 8 for half-width ASCII, 16 for CJK
    uint8_t bitmap[32];  // 16 rows, 2 bytes per row, MSB first
};

extern const CjkGlyph kCjkGlyphs[];
extern const int kCjkGlyphCount;
