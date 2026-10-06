#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace beta_reader {

enum class ReaderFontFace : uint8_t {
    kOpenDyslexic = 0,
    kFreeSerif = 1,
};

struct ReaderGlyph {
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t advance;
    uint16_t bitmap_offset;
};

constexpr int kReaderLineHeightPx = 29;
constexpr int kReaderTextWidthPx = 424;
constexpr int kReaderLinesPerPage = 23;

void SetReaderFontFace(ReaderFontFace face);
ReaderFontFace GetReaderFontFace();
const char* ReaderFontName();
int ReaderFontBaselinePx();

ReaderGlyph ReaderFontGlyph(char ch);
const uint8_t* ReaderFontBitmap(const ReaderGlyph& glyph);
int ReaderFontAdvance(char ch);
int ReaderFontMeasure(std::string_view text);

}  // namespace beta_reader
