// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcFontRenderer.hpp"

#include <pgmspace.h>

#include "JcFonts.generated.hpp"

namespace jc8048w550 {
namespace {

constexpr uint8_t kGlyphBufferWidth = 40;
constexpr uint8_t kGlyphBufferHeight = 40;
uint16_t glyph_pixels[kGlyphBufferWidth * kGlyphBufferHeight];

const fonts::Font& get_font(FontId font) {
    switch (font) {
        case FontId::Body16: return fonts::kRoboto16;
        case FontId::Button20: return fonts::kRoboto20;
        case FontId::Heading24: return fonts::kRoboto24;
        case FontId::Dro18: return fonts::kRobotoMono18;
        case FontId::Dro20: return fonts::kRobotoMono20;
        case FontId::Dro22: return fonts::kRobotoMono22;
        case FontId::Dro24: return fonts::kRobotoMono24;
        case FontId::Dro26: return fonts::kRobotoMono26;
        case FontId::Dro28: return fonts::kRobotoMono28;
        case FontId::Dro30: return fonts::kRobotoMono30;
        case FontId::Dro32: return fonts::kRobotoMono32;
    }
    return fonts::kRoboto16;
}

uint8_t next_codepoint(const char*& text) {
    const uint8_t first = static_cast<uint8_t>(*text++);
    if (first == 0) {
        return 0;
    }
    if (first == 0xC2 && static_cast<uint8_t>(*text) == 0xB0) {
        ++text;
        return 176;
    }
    return first >= 32 && first <= 126 ? first : static_cast<uint8_t>('?');
}

uint16_t blend565(uint16_t foreground, uint16_t background, uint8_t alpha) {
    const uint8_t inverse = 15 - alpha;
    const uint16_t red = (((foreground >> 11) & 0x1F) * alpha
                        + ((background >> 11) & 0x1F) * inverse + 7) / 15;
    const uint16_t green = (((foreground >> 5) & 0x3F) * alpha
                          + ((background >> 5) & 0x3F) * inverse + 7) / 15;
    const uint16_t blue = ((foreground & 0x1F) * alpha
                         + (background & 0x1F) * inverse + 7) / 15;
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

} // namespace

int16_t text_width(FontId font_id, const char* text) {
    if (text == nullptr) {
        return 0;
    }
    const fonts::Font& font = get_font(font_id);
    int16_t width = 0;
    while (*text != '\0') {
        const uint8_t codepoint = next_codepoint(text);
        const uint8_t index = codepoint == 176 ? 95 : codepoint - 32;
        fonts::Glyph glyph;
        memcpy_P(&glyph, &font.glyphs[index], sizeof(glyph));
        width += glyph.advance;
    }
    return width;
}

int16_t font_line_height(FontId font) {
    return get_font(font).line_height;
}

int16_t font_baseline(FontId font) {
    return get_font(font).baseline;
}

void draw_text(Arduino_GFX& display, int16_t x, int16_t baseline,
               const char* text, FontId font_id, uint16_t foreground,
               uint16_t background) {
    if (text == nullptr) {
        return;
    }
    const fonts::Font& font = get_font(font_id);
    while (*text != '\0') {
        const uint8_t codepoint = next_codepoint(text);
        const uint8_t index = codepoint == 176 ? 95 : codepoint - 32;
        fonts::Glyph glyph;
        memcpy_P(&glyph, &font.glyphs[index], sizeof(glyph));
        if (glyph.width == 0 || glyph.height == 0) {
            x += glyph.advance;
            continue;
        }
        if (glyph.width > kGlyphBufferWidth || glyph.height > kGlyphBufferHeight) {
            x += glyph.advance;
            continue;
        }

        for (uint16_t pixel = 0; pixel < glyph.width * glyph.height; ++pixel) {
            const uint8_t packed = pgm_read_byte(font.bitmap + glyph.offset + pixel / 2);
            const uint8_t alpha = (pixel & 1) == 0 ? packed >> 4 : packed & 0x0F;
            glyph_pixels[pixel] = blend565(foreground, background, alpha);
        }
        display.draw16bitRGBBitmap(x + glyph.x_offset, baseline + glyph.y_offset,
                                   glyph_pixels, glyph.width, glyph.height);
        x += glyph.advance;
    }
}

} // namespace jc8048w550
