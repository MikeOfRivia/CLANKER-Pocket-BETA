#include "reader_font.h"

#include <cstdint>

struct GFXglyph {
    uint16_t bitmapOffset;
    uint8_t width;
    uint8_t height;
    uint8_t xAdvance;
    int8_t xOffset;
    int8_t yOffset;
};

struct GFXfont {
    uint8_t* bitmap;
    GFXglyph* glyph;
    uint16_t first;
    uint16_t last;
    uint8_t yAdvance;
};

#ifndef PROGMEM
#define PROGMEM
#endif

// OpenDyslexic generated bitmap data.
#include "reader_font_data.h"
// Original FreeSerif bitmap data retained as an alternate face.
#include "reader_font_freeserif_data.h"

namespace beta_reader {
namespace {

ReaderFontFace s_reader_font_face = ReaderFontFace::kOpenDyslexic;

const GFXglyph* ActiveGlyphs()
{
    return s_reader_font_face == ReaderFontFace::kFreeSerif
        ? FreeSerif12pt7bGlyphs
        : OpenDyslexic20pxGlyphs;
}

const uint8_t* ActiveBitmaps()
{
    return s_reader_font_face == ReaderFontFace::kFreeSerif
        ? FreeSerif12pt7bBitmaps
        : OpenDyslexic20pxBitmaps;
}

}  // namespace

void SetReaderFontFace(ReaderFontFace face)
{
    s_reader_font_face = face;
}

ReaderFontFace GetReaderFontFace()
{
    return s_reader_font_face;
}

const char* ReaderFontName()
{
    return s_reader_font_face == ReaderFontFace::kFreeSerif
        ? "FREE SERIF"
        : "OPEN DYSLEXIC";
}

int ReaderFontBaselinePx()
{
    return s_reader_font_face == ReaderFontFace::kFreeSerif ? 22 : 23;
}

ReaderGlyph ReaderFontGlyph(char ch)
{
    unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c > 0x7E) c = '?';

    const GFXglyph& glyph = ActiveGlyphs()[c - 0x20];
    return ReaderGlyph{
        glyph.width,
        glyph.height,
        glyph.xOffset,
        glyph.yOffset,
        glyph.xAdvance,
        glyph.bitmapOffset,
    };
}

const uint8_t* ReaderFontBitmap(const ReaderGlyph& glyph)
{
    return ActiveBitmaps() + glyph.bitmap_offset;
}

int ReaderFontAdvance(char ch)
{
    return ReaderFontGlyph(ch).advance;
}

int ReaderFontMeasure(std::string_view text)
{
    int width = 0;
    for (const char ch : text) width += ReaderFontAdvance(ch);
    return width;
}

}  // namespace beta_reader
