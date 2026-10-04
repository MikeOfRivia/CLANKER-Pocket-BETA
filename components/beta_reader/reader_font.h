#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace beta_reader {

struct ReaderGlyph {
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t advance;
    uint16_t bitmap_offset;
};

// FreeSerif 12 pt rasterized as a 1-bit ASCII subset. The bitmap is vendored
// from Adafruit-GFX's generated FreeSerif12pt7b font data.
constexpr int kReaderFontBaselinePx = 22;
constexpr int kReaderLineHeightPx = 29;
constexpr int kReaderTextWidthPx = 424;
constexpr int kReaderLinesPerPage = 22;

ReaderGlyph ReaderFontGlyph(char ch);
const uint8_t* ReaderFontBitmap(const ReaderGlyph& glyph);
int ReaderFontAdvance(char ch);
int ReaderFontMeasure(std::string_view text);

}  // namespace beta_reader
