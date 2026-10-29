// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#pragma once

#include <Arduino_GFX_Library.h>

namespace jc8048w550 {

enum class FontId : uint8_t {
    Body16,
    Button20,
    Heading24,
    Dro18,
    Dro20,
    Dro22,
    Dro24,
    Dro26,
    Dro28,
    Dro30,
    Dro32,
};

int16_t text_width(FontId font, const char* text);
int16_t font_line_height(FontId font);
int16_t font_baseline(FontId font);
void draw_text(Arduino_GFX& display, int16_t x, int16_t baseline,
               const char* text, FontId font, uint16_t foreground,
               uint16_t background);

} // namespace jc8048w550
