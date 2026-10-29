// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Shared touchscreen drawing primitives for the JC application pages:
// consistent colours, buttons, header, word-wrapped text, change-detecting text
// slots (for incremental redraw) and a modal confirmation / result dialog.

#pragma once

#include <Arduino_GFX_Library.h>
#include <stdint.h>
#include <string.h>

#include "JcFontRenderer.hpp"

namespace jc8048w550 {
namespace ui {

struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

constexpr uint16_t kWidth = 800;
constexpr uint16_t kHeight = 480;

constexpr uint16_t kBg = 0x0841;
constexpr uint16_t kPanel = 0x18E3;
constexpr uint16_t kPanelLight = 0x2965;
constexpr uint16_t kAccentDark = 0x5000;
constexpr uint16_t kMuted = 0x9CF3;
constexpr uint16_t kBorder = 0x5ACB;
constexpr uint16_t kGood = 0x07E0;
constexpr uint16_t kGoodDark = 0x0320;
constexpr uint16_t kInfo = 0x051F;
constexpr uint16_t kWarn = 0xFD20;

constexpr Rect kBackButton { 690, 10, 94, 44 };
constexpr int16_t kContentTop = 72;

inline bool hit(const Rect& r, uint16_t x, uint16_t y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

enum class Btn : uint8_t { Normal, Selected, Danger, Disabled, Good };

void text(Arduino_GFX& d, int16_t x, int16_t y_top, const char* s, FontId font,
          uint16_t fg, uint16_t bg);
void text_center(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg);
void button(Arduino_GFX& d, Rect r, const char* label, Btn style = Btn::Normal);
void panel(Arduino_GFX& d, Rect r, uint16_t fill = kPanel);
// Title bar with BACK button and a mode chip (LIVE / DEMO ...).
void header(Arduino_GFX& d, const char* title, const char* mode_label, bool mode_ok);
// Word-wrapped text inside `r`; returns the y below the last line drawn.
int16_t wrap(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg);

// Text that is only redrawn when its content or colour changes.
class Slot {
public:
    void invalidate() { _valid = false; }
    void draw(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg,
              bool centered = false);

private:
    char _last[72] {};
    uint16_t _fg = 0;
    bool _valid = false;
};

class Dialog {
public:
    enum class Mode : uint8_t { Closed, Confirm, Result };
    enum class Action : uint8_t { None, Confirm, Cancel, Dismiss };
    static constexpr uint8_t kMaxLines = 8;

    void open_confirm(const char* title, const char* confirm_label = "CONFIRM");
    void open_result(const char* title, bool good);
    void add_line(const char* line, uint16_t color = 0);
    void close() { _mode = Mode::Closed; _line_count = 0; }
    bool is_open() const { return _mode != Mode::Closed; }
    Mode mode() const { return _mode; }
    void draw(Arduino_GFX& d) const;
    Action touch(uint16_t x, uint16_t y) const;

private:
    Mode _mode = Mode::Closed;
    char _title[40] {};
    char _confirm_label[16] {};
    char _lines[kMaxLines][78] {};
    uint16_t _colors[kMaxLines] {};
    uint8_t _line_count = 0;
    bool _good = false;
};

}  // namespace ui
}  // namespace jc8048w550
