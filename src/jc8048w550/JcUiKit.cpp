// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcUiKit.hpp"

namespace jc8048w550 {
namespace ui {

void text(Arduino_GFX& d, int16_t x, int16_t y_top, const char* s, FontId font,
          uint16_t fg, uint16_t bg) {
    draw_text(d, x, y_top + font_baseline(font), s, font, fg, bg);
}

void text_center(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg) {
    const int16_t w = text_width(font, s);
    const int16_t x = r.x + (r.w - w) / 2;
    const int16_t baseline = r.y + (r.h - font_line_height(font)) / 2 + font_baseline(font);
    draw_text(d, x < r.x ? r.x : x, baseline, s, font, fg, bg);
}

void panel(Arduino_GFX& d, Rect r, uint16_t fill) {
    d.fillRect(r.x, r.y, r.w, r.h, fill);
    d.drawRect(r.x, r.y, r.w, r.h, kBorder);
}

void button(Arduino_GFX& d, Rect r, const char* label, Btn style) {
    uint16_t fill = kPanelLight, border = kBorder, fg = LIGHTGREY;
    switch (style) {
        case Btn::Selected: fill = kInfo; border = CYAN; fg = WHITE; break;
        case Btn::Danger: fill = kAccentDark; border = RED; fg = WHITE; break;
        case Btn::Good: fill = kGoodDark; border = kGood; fg = WHITE; break;
        case Btn::Disabled: fill = kPanel; border = kBorder; fg = kMuted; break;
        case Btn::Normal: break;
    }
    d.fillRect(r.x, r.y, r.w, r.h, fill);
    d.drawRect(r.x, r.y, r.w, r.h, border);
    FontId font = FontId::Button20;
    if (text_width(font, label) > r.w - 12) {
        font = FontId::Body16;
    }
    text_center(d, r, label, font, fg, fill);
}

void header(Arduino_GFX& d, const char* title, const char* mode_label, bool mode_ok) {
    d.fillRect(0, 0, kWidth, 64, kPanel);
    text(d, 18, 12, title, FontId::Heading24, WHITE, kPanel);
    const Rect chip { 520, 13, 150, 38 };
    const uint16_t fill = mode_ok ? kGoodDark : kAccentDark;
    d.fillRect(chip.x, chip.y, chip.w, chip.h, fill);
    d.drawRect(chip.x, chip.y, chip.w, chip.h, mode_ok ? kGood : RED);
    text_center(d, chip, mode_label, FontId::Body16, WHITE, fill);
    button(d, kBackButton, "BACK");
}

bool live_jog_banner(const char* title, Rect& box, const char*& text) {
#if JC_JOG_PREVIEW
    static const char* const kWordings[3] = { "PREVIEW ONLY - MOTION DISABLED", "PREVIEW - NO MOTION", "PREVIEW ONLY" };
#else
    static const char* const kWordings[3] = { "LIVE JOG TEST BUILD", "JOG TEST BUILD", "JOG TEST" };
#endif
    const int16_t left = static_cast<int16_t>(18 + text_width(FontId::Heading24, title) + 14);
    const int16_t right = static_cast<int16_t>(kHeaderChipLeft - 8);
    for (const char* w : kWordings) {
        const int16_t width = static_cast<int16_t>(text_width(FontId::Body16, w) + 16);
        if (left + width <= right) {
            box = { left, 13, width, 38 };
            text = w;
            return true;
        }
    }
    return false;
}

void draw_live_jog_banner(Arduino_GFX& d, const char* title) {
    Rect box {};
    const char* text = nullptr;
    if (!live_jog_banner(title, box, text)) {
        return;
    }
    d.fillRect(box.x, box.y, box.w, box.h, RED);
    d.drawRect(box.x, box.y, box.w, box.h, WHITE);
    text_center(d, box, text, FontId::Body16, WHITE, RED);
}

int16_t wrap(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg) {
    char line[96];
    int16_t y = r.y;
    const int16_t step = font_line_height(font) + 3;
    const char* p = s;
    while (*p != '\0' && y + step <= r.y + r.h + step) {
        line[0] = '\0';
        size_t len = 0;
        while (*p != '\0') {
            const char* word_end = p;
            while (*word_end != '\0' && *word_end != ' ') ++word_end;
            const size_t wl = static_cast<size_t>(word_end - p);
            char trial[96];
            if (len + wl + 1 >= sizeof(trial)) break;
            memcpy(trial, line, len);
            size_t tl = len;
            if (tl > 0) trial[tl++] = ' ';
            memcpy(trial + tl, p, wl);
            tl += wl;
            trial[tl] = '\0';
            if (len > 0 && text_width(font, trial) > r.w) break;
            memcpy(line, trial, tl + 1);
            len = tl;
            p = word_end;
            while (*p == ' ') ++p;
        }
        if (len == 0) break;
        text(d, r.x, y, line, font, fg, bg);
        y += step;
    }
    return y;
}

void Slot::draw(Arduino_GFX& d, Rect r, const char* s, FontId font, uint16_t fg, uint16_t bg,
                bool centered) {
    if (_valid && fg == _fg && strncmp(_last, s, sizeof(_last)) == 0) {
        return;
    }
    d.fillRect(r.x, r.y, r.w, r.h, bg);
    if (centered) {
        text_center(d, r, s, font, fg, bg);
    } else {
        const int16_t y = r.y + (r.h - font_line_height(font)) / 2;
        text(d, r.x + 2, y, s, font, fg, bg);
    }
    strncpy(_last, s, sizeof(_last) - 1);
    _last[sizeof(_last) - 1] = '\0';
    _fg = fg;
    _valid = true;
}

void Dialog::open_confirm(const char* title, const char* confirm_label) {
    _mode = Mode::Confirm;
    _line_count = 0;
    strncpy(_title, title, sizeof(_title) - 1);
    _title[sizeof(_title) - 1] = '\0';
    strncpy(_confirm_label, confirm_label, sizeof(_confirm_label) - 1);
    _confirm_label[sizeof(_confirm_label) - 1] = '\0';
    _good = false;
}

void Dialog::open_result(const char* title, bool good) {
    _mode = Mode::Result;
    _line_count = 0;
    strncpy(_title, title, sizeof(_title) - 1);
    _title[sizeof(_title) - 1] = '\0';
    _good = good;
}

void Dialog::add_line(const char* line, uint16_t color) {
    if (_line_count >= kMaxLines) {
        return;
    }
    strncpy(_lines[_line_count], line, sizeof(_lines[0]) - 1);
    _lines[_line_count][sizeof(_lines[0]) - 1] = '\0';
    _colors[_line_count] = color;
    ++_line_count;
}

void Dialog::draw(Arduino_GFX& d) const {
    if (_mode == Mode::Closed) {
        return;
    }
    d.fillRect(kDialogBox.x, kDialogBox.y, kDialogBox.w, kDialogBox.h, kPanel);
    d.drawRect(kDialogBox.x, kDialogBox.y, kDialogBox.w, kDialogBox.h, _good ? kGood : (_mode == Mode::Result ? RED : CYAN));
    d.drawRect(kDialogBox.x + 1, kDialogBox.y + 1, kDialogBox.w - 2, kDialogBox.h - 2, kBorder);
    text(d, kDialogBox.x + 20, kDialogBox.y + 14, _title, FontId::Heading24, WHITE, kPanel);
    int16_t y = kDialogBox.y + kDialogFirstLineY;
    for (uint8_t i = 0; i < _line_count; ++i) {
        text(d, kDialogBox.x + kDialogTextLeft, y, _lines[i], FontId::Body16, _colors[i] ? _colors[i] : LIGHTGREY, kPanel);
        y += kDialogLineStep;
    }
    if (_mode == Mode::Confirm) {
        button(d, kDialogCancel, "CANCEL");
        button(d, kDialogConfirm, _confirm_label, Btn::Good);
    } else {
        button(d, kDialogOk, "OK");
    }
}

Dialog::Action Dialog::touch(uint16_t x, uint16_t y) const {
    if (_mode == Mode::Confirm) {
        if (hit(kDialogConfirm, x, y)) return Action::Confirm;
        if (hit(kDialogCancel, x, y)) return Action::Cancel;
    } else if (_mode == Mode::Result) {
        if (hit(kDialogOk, x, y)) return Action::Dismiss;
    }
    return Action::None;  // taps outside the buttons never dismiss or confirm
}

}  // namespace ui
}  // namespace jc8048w550
