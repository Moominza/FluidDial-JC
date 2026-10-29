// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Settings hub plus the Display, Jog presets and Machine pages.

#include "JcScreens.hpp"

#include <stdlib.h>

namespace jc8048w550 {

// ---------------------------------------------------------------- hub

namespace {

struct HubTile {
    const char* title;
    const char* subtitle;
};
constexpr HubTile kHub[7] = {
    { "CONNECTIONS", "Transport selection" },
    { "DISPLAY", "Brightness, precision" },
    { "JOG PRESETS", "Steps and feeds" },
    { "MACHINE", "Axes, capabilities" },
    { "DIAGNOSTICS", "Firmware, link, memory" },
    { "WI-FI SETUP", "SSID, password, host" },
    { "TOUCH CALIBRATION", "Five-point guided" },
};
ui::Rect hub_rect(uint8_t i) {
    const uint8_t col = i < 4 ? i : i - 4;
    const uint8_t row = i < 4 ? 0 : 1;
    return { static_cast<int16_t>(16 + col * 194), static_cast<int16_t>(84 + row * 170), 186, 160 };
}

}  // namespace

void JcSettingsScreen::on_draw() {
    for (uint8_t i = 0; i < 7; ++i) {
        const ui::Rect r = hub_rect(i);
        ui::panel(sv.display, r, ui::kPanelLight);
        ui::wrap(sv.display, { static_cast<int16_t>(r.x + 12), static_cast<int16_t>(r.y + 16), 162, 70 },
                 kHub[i].title, FontId::Button20, WHITE, ui::kPanelLight);
        ui::wrap(sv.display, { static_cast<int16_t>(r.x + 12), static_cast<int16_t>(r.y + 98), 162, 56 },
                 kHub[i].subtitle, FontId::Body16, ui::kMuted, ui::kPanelLight);
    }
    ui::text(sv.display, 400, 322, "Firmware: experimental, machine control disabled", FontId::Body16, ui::kWarn, ui::kBg);
}

void JcSettingsScreen::on_touch(uint16_t x, uint16_t y) {
    static constexpr JcPage pages[5] = { JcPage::SetConnections, JcPage::SetDisplay, JcPage::SetJogging,
                                          JcPage::SetMachine, JcPage::Diagnostics };
    for (uint8_t i = 0; i < 7; ++i) {
        if (!ui::hit(hub_rect(i), x, y)) {
            continue;
        }
        if (i < 5) {
            sv.host.go(pages[i]);
        } else if (i == 5) {
            sv.host.leave(JcLeaveTarget::WifiSettings);
        } else {
            sv.host.leave(JcLeaveTarget::Calibration);
        }
        return;
    }
}

// ---------------------------------------------------------------- display

namespace {
constexpr ui::Rect kBrightMinus { 300, 90, 70, 52 };
constexpr ui::Rect kBrightPlus { 520, 90, 70, 52 };
constexpr ui::Rect kBrightValue { 376, 90, 138, 52 };
constexpr ui::Rect kDecMinus { 300, 160, 70, 52 };
constexpr ui::Rect kDecPlus { 520, 160, 70, 52 };
constexpr ui::Rect kDecValue { 376, 160, 138, 52 };
constexpr ui::Rect kRefMachine { 300, 230, 142, 52 };
constexpr ui::Rect kRefWork { 448, 230, 142, 52 };
constexpr ui::Rect kCalButton { 300, 300, 290, 52 };
}  // namespace

void JcDisplaySettingsScreen::draw_values() {
    char v[24];
    snprintf(v, sizeof(v), "%u %%", sv.settings.brightness_percent);
    ui::button(sv.display, kBrightValue, v, ui::Btn::Disabled);
    snprintf(v, sizeof(v), "%u", sv.settings.coordinate_decimals);
    ui::button(sv.display, kDecValue, v, ui::Btn::Disabled);
    ui::button(sv.display, kRefMachine, "MACHINE", sv.settings.default_reference == 0 ? ui::Btn::Selected : ui::Btn::Normal);
    ui::button(sv.display, kRefWork, "WORK", sv.settings.default_reference == 1 ? ui::Btn::Selected : ui::Btn::Normal);
}

void JcDisplaySettingsScreen::on_draw() {
    ui::text(sv.display, 16, 104, "Brightness", FontId::Button20, ui::kMuted, ui::kBg);
    ui::text(sv.display, 16, 174, "Coordinate decimals", FontId::Button20, ui::kMuted, ui::kBg);
    ui::text(sv.display, 16, 244, "Default reference", FontId::Button20, ui::kMuted, ui::kBg);
    ui::button(sv.display, kBrightMinus, "-");
    ui::button(sv.display, kBrightPlus, "+");
    ui::button(sv.display, kDecMinus, "-");
    ui::button(sv.display, kDecPlus, "+");
    ui::button(sv.display, kCalButton, "TOUCH CALIBRATION");
    ui::wrap(sv.display, { 16, 372, 768, 90 },
             "Units: millimetres for X/Y/Z, degrees for the rotary A axis (inch mode is not supported). "
             "Fonts are the fixed anti-aliased Roboto set. Changes are saved automatically.",
             FontId::Body16, ui::kMuted, ui::kBg);
    draw_values();
}

void JcDisplaySettingsScreen::on_touch(uint16_t x, uint16_t y) {
    JcUserSettings& s = sv.settings;
    if (ui::hit(kBrightMinus, x, y) && s.brightness_percent > 10) {
        s.brightness_percent = s.brightness_percent > 20 ? s.brightness_percent - 10 : 10;
        sv.host.apply_brightness();
    } else if (ui::hit(kBrightPlus, x, y) && s.brightness_percent < 100) {
        s.brightness_percent = s.brightness_percent < 90 ? s.brightness_percent + 10 : 100;
        sv.host.apply_brightness();
    } else if (ui::hit(kDecMinus, x, y) && s.coordinate_decimals > 1) {
        --s.coordinate_decimals;
    } else if (ui::hit(kDecPlus, x, y) && s.coordinate_decimals < 4) {
        ++s.coordinate_decimals;
    } else if (ui::hit(kRefMachine, x, y)) {
        s.default_reference = 0;
    } else if (ui::hit(kRefWork, x, y)) {
        s.default_reference = 1;
    } else if (ui::hit(kCalButton, x, y)) {
        sv.host.leave(JcLeaveTarget::Calibration);
        return;
    } else {
        return;
    }
    sv.host.save_settings();
    draw_values();
}

// ---------------------------------------------------------------- jog presets

namespace {

ui::Rect preset_rect(uint8_t row, uint8_t i) {
    return { static_cast<int16_t>(200 + i * 116), static_cast<int16_t>(80 + row * 70), 108, 52 };
}
constexpr ui::Rect kResetDefaults { 16, 380, 300, 56 };

void jog_edit_callback(void* user, bool ok, const char* text) {
    static_cast<JcJogSettingsScreen*>(user)->edit_done(ok, text);
}

}  // namespace

float* JcJogSettingsScreen::value_ptr(uint8_t row, uint8_t index) {
    JcUserSettings& s = sv.settings;
    switch (row) {
        case 0: return &s.linear_steps[index];
        case 1: return &s.rotary_steps[index];
        case 2: return &s.linear_feeds[index];
        default: return &s.rotary_feeds[index];
    }
}

void JcJogSettingsScreen::draw_values() {
    for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t count = row < 2 ? kJcStepPresets : kJcFeedPresets;
        for (uint8_t i = 0; i < count; ++i) {
            char v[20];
            snprintf(v, sizeof(v), "%g", static_cast<double>(*value_ptr(row, i)));
            ui::button(sv.display, preset_rect(row, i), v);
        }
    }
}

void JcJogSettingsScreen::on_draw() {
    const char* labels[4] = { "Linear step (mm)", "Rotary step (deg)", "Linear feed (mm/min)", "Rotary feed (deg/min)" };
    for (uint8_t row = 0; row < 4; ++row) {
        ui::wrap(sv.display, { 16, static_cast<int16_t>(84 + row * 70), 176, 48 }, labels[row], FontId::Body16,
                 ui::kMuted, ui::kBg);
    }
    ui::button(sv.display, kResetDefaults, "RESET TO DEFAULTS", ui::Btn::Danger);
    ui::wrap(sv.display, { 340, 380, 444, 80 },
             "Tap a value to edit it. Values are clamped to safe ranges; feeds above an axis limit are refused "
             "when a jog command is built.",
             FontId::Body16, ui::kMuted, ui::kBg);
    draw_values();
}

void JcJogSettingsScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    char* end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || *end != '\0') {
        return;
    }
    *value_ptr(_edit_row, _edit_index) = value;
    jc_settings_sanitize(sv.settings);
    sv.host.save_settings();
    draw();
}

void JcJogSettingsScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t count = row < 2 ? kJcStepPresets : kJcFeedPresets;
        for (uint8_t i = 0; i < count; ++i) {
            if (ui::hit(preset_rect(row, i), x, y)) {
                _edit_row = row;
                _edit_index = i;
                JcInputRequest r {};
                r.prompt = row < 2 ? "Jog step" : "Jog feed rate per minute";
                snprintf(r.initial, sizeof(r.initial), "%g", static_cast<double>(*value_ptr(row, i)));
                r.numeric = true;
                r.max_len = 8;
                r.done = jog_edit_callback;
                r.user = this;
                sv.host.request_input(r);
                return;
            }
        }
    }
    if (ui::hit(kResetDefaults, x, y)) {
        const JcUserSettings d = jc_settings_defaults();
        memcpy(sv.settings.linear_steps, d.linear_steps, sizeof(d.linear_steps));
        memcpy(sv.settings.rotary_steps, d.rotary_steps, sizeof(d.rotary_steps));
        memcpy(sv.settings.linear_feeds, d.linear_feeds, sizeof(d.linear_feeds));
        memcpy(sv.settings.rotary_feeds, d.rotary_feeds, sizeof(d.rotary_feeds));
        sv.host.save_settings();
        draw_values();
    }
}

// ---------------------------------------------------------------- machine

namespace {
constexpr ui::Rect kAHoming { 16, 300, 360, 56 };
constexpr ui::Rect kCapsButton { 400, 300, 384, 56 };
}  // namespace

void JcMachineScreen::draw_toggle() {
    char label[48];
    snprintf(label, sizeof(label), "A AXIS HAS HOME SWITCH: %s", sv.settings.a_axis_has_homing ? "YES" : "NO");
    ui::button(sv.display, kAHoming, label, sv.settings.a_axis_has_homing ? ui::Btn::Selected : ui::Btn::Normal);
}

void JcMachineScreen::on_draw() {
    ui::text(sv.display, 24, 80, "AXIS", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 100, 80, "UNIT", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 190, 80, "MAX STEP", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 330, 80, "MAX FEED", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 480, 80, "HOME", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 580, 80, "PROBE", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 672, 80, "LIMITS", FontId::Body16, ui::kMuted, ui::kBg);
    for (uint8_t i = 0; i < 4; ++i) {
        const JcAxisProfile& a = sv.profile.axes[i];
        const int16_t y = 104 + i * 40;
        char step[24], feed[24];
        snprintf(step, sizeof(step), "%g", static_cast<double>(a.max_step));
        snprintf(feed, sizeof(feed), "%g /min", static_cast<double>(a.max_feed));
        ui::text(sv.display, 24, y, jc_axis_name(i), FontId::Button20, WHITE, ui::kBg);
        ui::text(sv.display, 100, y, a.rotary ? "deg (rotary)" : "mm", FontId::Body16, WHITE, ui::kBg);
        ui::text(sv.display, 190, y, step, FontId::Body16, WHITE, ui::kBg);
        ui::text(sv.display, 330, y, feed, FontId::Body16, WHITE, ui::kBg);
        ui::text(sv.display, 480, y, a.homing_allowed ? "yes" : "no", FontId::Body16, a.homing_allowed ? ui::kGood : ui::kMuted, ui::kBg);
        ui::text(sv.display, 580, y, a.probe_allowed ? "yes" : "no", FontId::Body16, a.probe_allowed ? ui::kGood : ui::kMuted, ui::kBg);
        ui::text(sv.display, 672, y, a.limits_confirmed ? "controller" : "default", FontId::Body16,
                 a.limits_confirmed ? ui::kGood : ui::kWarn, ui::kBg);
    }
    const bool all = jc_profile_limits_mask(sv.profile) == 0x0F;
    ui::wrap(sv.display, { 16, 268, 768, 28 },
             all ? "All limits were read from the controller." : "Axes marked default use conservative placeholder limits.",
             FontId::Body16, all ? ui::kGood : ui::kWarn, ui::kBg);
    draw_toggle();
    ui::button(sv.display, kCapsButton, "CONTROLLER CAPABILITIES");
    ui::wrap(sv.display, { 16, 372, 768, 96 },
             "Capabilities are read from FluidNC with read-only queries when you ask (CONTROLLER CAPABILITIES). "
             "Placeholders are replaced only by values the controller confirmed, and are dropped again if the "
             "connection is lost. Whether A is rotary cannot be read: it is declared here and measured in degrees.",
             FontId::Body16, ui::kMuted, ui::kBg);
}

void JcMachineScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kAHoming, x, y)) {
        sv.settings.a_axis_has_homing = !sv.settings.a_axis_has_homing;
        sv.host.save_settings();  // also rebuilds the machine profile
        draw();
    } else if (ui::hit(kCapsButton, x, y)) {
        sv.host.go(JcPage::Capabilities);
    }
}

}  // namespace jc8048w550
