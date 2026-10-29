// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Modal numeric / text entry page used by probing, offsets, files, macros,
// jog presets and connection settings.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kValueBox { 16, 100, 768, 56 };

struct Key {
    ui::Rect rect;
    const char* label;
};

constexpr const char* kTextRows[4] = { "1234567890", "qwertyuiop", "asdfghjkl/", "zxcvbnm._-" };

ui::Rect text_key(uint8_t row, uint8_t col) {
    return { static_cast<int16_t>(16 + col * 77), static_cast<int16_t>(170 + row * 56), 71, 48 };
}

ui::Rect num_key(uint8_t row, uint8_t col) {
    return { static_cast<int16_t>(180 + col * 112), static_cast<int16_t>(172 + row * 70), 104, 62 };
}
constexpr const char* kNumKeys[4][4] = {
    { "7", "8", "9", "<" },
    { "4", "5", "6", "CLR" },
    { "1", "2", "3", "-" },
    { "0", ".", "CANCEL", "OK" },
};

constexpr ui::Rect kShift { 16, 398, 150, 50 };
constexpr ui::Rect kSpace { 176, 398, 250, 50 };
constexpr ui::Rect kBack { 436, 398, 110, 50 };
constexpr ui::Rect kCancel { 556, 398, 110, 50 };
constexpr ui::Rect kOk { 676, 398, 108, 50 };

char shifted(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 32);
    return c;
}

}  // namespace

void JcInputScreen::begin(const JcInputRequest& request) {
    _request = request;
    strncpy(_value, request.initial, sizeof(_value) - 1);
    _value[sizeof(_value) - 1] = '\0';
    _length = static_cast<uint8_t>(strlen(_value));
    _shift = false;
    _finished = false;
}

void JcInputScreen::draw_value() {
    ui::panel(sv.display, kValueBox, ui::kPanelLight);
    char shown[72];
    snprintf(shown, sizeof(shown), "%s_", _value);
    ui::text(sv.display, kValueBox.x + 12, kValueBox.y + 12, shown, FontId::Heading24, WHITE, ui::kPanelLight);
}

void JcInputScreen::on_draw() {
    ui::text(sv.display, 16, 74, _request.prompt ? _request.prompt : "Enter value", FontId::Body16, ui::kMuted, ui::kBg);
    draw_value();
    if (_request.numeric) {
        for (uint8_t r = 0; r < 4; ++r) {
            for (uint8_t c = 0; c < 4; ++c) {
                const char* label = kNumKeys[r][c];
                const bool special = strlen(label) > 1 || label[0] == '<';
                ui::button(sv.display, num_key(r, c), label,
                           strcmp(label, "OK") == 0 ? ui::Btn::Good : (strcmp(label, "CANCEL") == 0 ? ui::Btn::Danger
                                                                                                    : (special ? ui::Btn::Selected : ui::Btn::Normal)));
            }
        }
        return;
    }
    for (uint8_t r = 0; r < 4; ++r) {
        for (uint8_t c = 0; c < 10; ++c) {
            char label[2] = { _shift ? shifted(kTextRows[r][c]) : kTextRows[r][c], '\0' };
            ui::button(sv.display, text_key(r, c), label);
        }
    }
    ui::button(sv.display, kShift, "SHIFT", _shift ? ui::Btn::Selected : ui::Btn::Normal);
    ui::button(sv.display, kSpace, "SPACE");
    ui::button(sv.display, kBack, "<");
    ui::button(sv.display, kCancel, "CANCEL", ui::Btn::Danger);
    ui::button(sv.display, kOk, "OK", ui::Btn::Good);
}

void JcInputScreen::finish(bool ok) {
    if (_finished) {
        return;
    }
    _finished = true;
    // Copy out before navigating: the callback may immediately request another input.
    void (*done)(void*, bool, const char*) = _request.done;
    void* user = _request.user;
    char text[64];
    strncpy(text, _value, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    sv.host.back();
    if (done != nullptr) {
        done(user, ok, text);
    }
}

void JcInputScreen::press(const char* key) {
    if (strcmp(key, "OK") == 0) {
        finish(true);
        return;
    }
    if (strcmp(key, "CANCEL") == 0) {
        finish(false);
        return;
    }
    if (strcmp(key, "<") == 0) {
        if (_length > 0) _value[--_length] = '\0';
    } else if (strcmp(key, "CLR") == 0) {
        _length = 0;
        _value[0] = '\0';
    } else if (strcmp(key, "SPACE") == 0 || strlen(key) == 1) {
        const char c = strcmp(key, "SPACE") == 0 ? ' ' : key[0];
        if (_length < _request.max_len && _length < sizeof(_value) - 1) {
            if (_request.numeric && c == '-' && _length != 0) return;
            if (_request.numeric && c == '.' && strchr(_value, '.') != nullptr) return;
            _value[_length++] = c;
            _value[_length] = '\0';
        }
    }
    draw_value();
}

void JcInputScreen::on_touch(uint16_t x, uint16_t y) {
    if (_request.numeric) {
        for (uint8_t r = 0; r < 4; ++r) {
            for (uint8_t c = 0; c < 4; ++c) {
                if (ui::hit(num_key(r, c), x, y)) {
                    press(kNumKeys[r][c]);
                    return;
                }
            }
        }
        return;
    }
    for (uint8_t r = 0; r < 4; ++r) {
        for (uint8_t c = 0; c < 10; ++c) {
            if (ui::hit(text_key(r, c), x, y)) {
                const char key[2] = { _shift ? shifted(kTextRows[r][c]) : kTextRows[r][c], '\0' };
                press(key);
                return;
            }
        }
    }
    if (ui::hit(kShift, x, y)) {
        _shift = !_shift;
        on_draw();
    } else if (ui::hit(kSpace, x, y)) {
        press("SPACE");
    } else if (ui::hit(kBack, x, y)) {
        press("<");
    } else if (ui::hit(kCancel, x, y)) {
        press("CANCEL");
    } else if (ui::hit(kOk, x, y)) {
        press("OK");
    }
}

}  // namespace jc8048w550
