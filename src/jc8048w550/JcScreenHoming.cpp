// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {
constexpr ui::Rect kHomeAll { 16, 150, 768, 70 };
ui::Rect axis_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 193), 232, 183, 64 }; }
constexpr ui::Rect kStatusRect { 16, 76, 768, 62 };
constexpr ui::Rect kPreflight { 16, 308, 768, 164 };
}  // namespace

void JcHomingScreen::draw_status() {
    char line[80];
    const char* state = sv.model.has_status() ? sv.model.state() : "no data";
    snprintf(line, sizeof(line), "MACHINE %s%s", state, sv.model.homing() ? "  -  HOMING IN PROGRESS" : "");
    _state.draw(sv.display, { 28, 82, 744, 26 }, line, FontId::Button20,
                sv.model.homing() ? ui::kWarn : (sv.model.alarm() ? RED : WHITE), ui::kPanel);

    const bool fresh = sv.model.fresh();
    _pre1.draw(sv.display, { 28, 340, 744, 26 }, fresh ? "[OK] Live status is fresh" : "[!!] No fresh live status",
               FontId::Body16, fresh ? ui::kGood : ui::kWarn, ui::kPanel);
    const bool idle = sv.model.has_status() && strcmp(sv.model.state(), "Idle") == 0;
    _pre2.draw(sv.display, { 28, 366, 744, 26 },
               idle ? "[OK] Machine is Idle" : (sv.model.alarm() ? "[!!] Machine in ALARM - clear the cause first"
                                                                 : "[!!] Machine is not Idle / unknown"),
               FontId::Body16, idle ? ui::kGood : ui::kWarn, ui::kPanel);
    _pre3.draw(sv.display, { 28, 392, 744, 26 }, "[--] Control: DISABLED in this build (preview only)",
               FontId::Body16, ui::kWarn, ui::kPanel);
}

void JcHomingScreen::on_draw() {
    ui::panel(sv.display, kStatusRect);
    ui::text(sv.display, 28, 110, "Homing moves axes toward their limit switches.", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::button(sv.display, kHomeAll, "HOME ALL AXES  ($H)", ui::Btn::Normal);
    for (uint8_t i = 0; i < 4; ++i) {
        char label[24];
        if (i == 3 && !sv.settings.a_axis_has_homing) {
            snprintf(label, sizeof(label), "A (no switch set)");
        } else {
            snprintf(label, sizeof(label), "HOME %s", jc_axis_name(i));
        }
        ui::button(sv.display, axis_rect(i), label,
                   (i == 3 && !sv.settings.a_axis_has_homing) ? ui::Btn::Disabled : ui::Btn::Normal);
    }
    ui::panel(sv.display, kPreflight);
    ui::text(sv.display, 28, 312, "PREFLIGHT", FontId::Button20, WHITE, ui::kPanel);
    ui::wrap(sv.display, { 28, 420, 744, 48 },
             "Check clear travel, working limit switches and that you can reach the physical E-stop. "
             "Rotary A is not assumed to have a home switch (Settings > Machine).",
             FontId::Body16, ui::kMuted, ui::kPanel);
    _state.invalidate();
    _pre1.invalidate();
    _pre2.invalidate();
    _pre3.invalidate();
    draw_status();
}

void JcHomingScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kHomeAll, x, y)) {
        const char* notes[] = {
            "Homes ALL configured axes using FluidNC's homing cycle.",
            "Whether the rotary A axis is homed depends on the controller.",
        };
        stage(jc_home(sv.profile, 0xFF), "HOME ALL AXES", notes, 2);
        return;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (!ui::hit(axis_rect(i), x, y)) {
            continue;
        }
        if (i == 3 && !sv.settings.a_axis_has_homing) {
            show_message("A-AXIS HOMING UNAVAILABLE", "The rotary axis is not assumed to have a home switch.",
                         "Enable it under Settings > Machine only if one is fitted.");
            return;
        }
        char title[32];
        snprintf(title, sizeof(title), "HOME %s AXIS", jc_axis_name(i));
        const char* notes[] = { "Single-axis homing must be allowed by the controller configuration." };
        stage(jc_home(sv.profile, i), title, notes, 1);
        return;
    }
}

void JcHomingScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 300) {
        _last_tick = now;
        draw_status();
    }
}

}  // namespace jc8048w550
