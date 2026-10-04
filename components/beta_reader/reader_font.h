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

// Noto Serif Regular, 24 px, rasterized as a 1-bit ASCII subset.
// Noto fonts are licensed under the SIL Open Font License 1.1.
constexpr int kReaderFontBaselinePx = 20;
constexpr int kReaderLineHeightPx = 30;
constexpr int kReaderTextWidthPx = 424;
constexpr int kReaderLinesPerPage = 18;

const ReaderGlyph& ReaderFontGlyph(char ch);
const uint8_t* ReaderFontBitmap(const ReaderGlyph& glyph);
int ReaderFontAdvance(char ch);
int ReaderFontMeasure(std::string_view text);

}  // namespace beta_reader
