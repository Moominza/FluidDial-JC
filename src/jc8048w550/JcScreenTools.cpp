// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr uint8_t kSteps = 6;
const char* const kStepTitles[kSteps] = {
    "1  Raise Z to a safe height",
    "2  Stop the spindle",
    "3  Change the tool by hand",
    "4  Measure tool length (probe)",
    "5  Set the tool offset",
    "6  Verify and resume",
};
const char* const kStepText[kSteps] = {
    "Jog Z to a safe height on the Jogging page, clear of the workpiece and clamps, then return here.",
    "Stop the spindle before touching the tool. The preview below shows the stop command (M5).",
    "Remove and install the tool by hand. No automatic tool changer is assumed. Keep hands clear.",
    "Place the tool over the probe / plate and preview the Z probe sequence. Probe settings come from the Probing page.",
    "After contact the sequence sets Z to the plate thickness (G10 L20 P0 Z...). Preview that offset change here.",
    "Check the Z reading on the dashboard. Reading the active tool and tool length offset needs a $G query, which this build does not send.",
};

ui::Rect step_rect(uint8_t i) { return { 16, static_cast<int16_t>(76 + i * 54), 330, 48 }; }
constexpr ui::Rect kDetail { 362, 76, 422, 168 };
constexpr ui::Rect kAction { 362, 256, 422, 56 };
constexpr ui::Rect kConfigure { 362, 322, 206, 50 };
constexpr ui::Rect kDone { 578, 322, 206, 50 };
constexpr ui::Rect kPrev { 362, 382, 206, 50 };
constexpr ui::Rect kNext { 578, 382, 206, 50 };

const char* action_label(uint8_t step) {
    switch (step) {
        case 1: return "PREVIEW SPINDLE STOP (M5)";
        case 3: return "PREVIEW TOOL MEASUREMENT";
        case 4: return "PREVIEW TOOL OFFSET";
        default: return nullptr;
    }
}

}  // namespace

void JcToolsScreen::on_enter() {
    _step = 0;
    memset(_done, 0, sizeof(_done));
}

void JcToolsScreen::draw_steps() {
    for (uint8_t i = 0; i < kSteps; ++i) {
        ui::button(sv.display, step_rect(i), kStepTitles[i],
                   i == _step ? ui::Btn::Selected : (_done[i] ? ui::Btn::Good : ui::Btn::Normal));
    }
}

void JcToolsScreen::draw_detail() {
    ui::panel(sv.display, kDetail);
    ui::text(sv.display, kDetail.x + 12, kDetail.y + 8, kStepTitles[_step], FontId::Button20, WHITE, ui::kPanel);
    ui::wrap(sv.display, { kDetail.x + 12, kDetail.y + 42, kDetail.w - 24, kDetail.h - 50 }, kStepText[_step],
             FontId::Body16, ui::kMuted, ui::kPanel);
    const char* label = action_label(_step);
    if (label) {
        ui::button(sv.display, kAction, label);
    } else {
        sv.display.fillRect(kAction.x, kAction.y, kAction.w, kAction.h, ui::kBg);
    }
    ui::button(sv.display, kConfigure, "PROBE SETTINGS");
    ui::button(sv.display, kDone, _done[_step] ? "DONE (tap to undo)" : "MARK STEP DONE",
               _done[_step] ? ui::Btn::Good : ui::Btn::Normal);
    ui::button(sv.display, kPrev, "< PREVIOUS", _step == 0 ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kNext, "NEXT >", _step + 1 >= kSteps ? ui::Btn::Disabled : ui::Btn::Normal);
}

void JcToolsScreen::on_draw() {
    draw_steps();
    draw_detail();
    ui::wrap(sv.display, { 16, 404, 330, 68 },
             "Current tool and tool length offset: unavailable ($G not sent). Manual tool changes only.",
             FontId::Body16, ui::kWarn, ui::kBg);
}

void JcToolsScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < kSteps; ++i) {
        if (ui::hit(step_rect(i), x, y)) {
            _step = i;
            draw_steps();
            draw_detail();
            return;
        }
    }
    if (ui::hit(kAction, x, y) && action_label(_step)) {
        const JcUserSettings& s = sv.settings;
        if (_step == 1) {
            const char* notes[] = { "Stops the spindle. Always wait for it to stop before changing the tool." };
            stage(jc_spindle_stop(), "STOP SPINDLE", notes, 1);
        } else if (_step == 3) {
            const char* notes[] = { "Measures tool length with the Z probe; contact is verified by the probe pin." };
            stage_sequence(jc_probe_z_sequence(sv.profile, s.probe_travel, s.probe_feed, s.probe_retract, s.plate_thickness),
                           "MEASURE TOOL LENGTH", notes, 1);
        } else {
            const char* notes[] = { "Sets Z to the plate thickness at the current position (G10 L20 P0)." };
            stage(jc_set_axis_value(sv.profile, 2, s.plate_thickness), "SET TOOL OFFSET", notes, 1);
        }
    } else if (ui::hit(kConfigure, x, y)) {
        sv.host.go(JcPage::Probing);
    } else if (ui::hit(kDone, x, y)) {
        _done[_step] = !_done[_step];
        draw_steps();
        draw_detail();
    } else if (ui::hit(kPrev, x, y) && _step > 0) {
        --_step;
        draw_steps();
        draw_detail();
    } else if (ui::hit(kNext, x, y) && _step + 1 < kSteps) {
        ++_step;
        draw_steps();
        draw_detail();
    }
}

}  // namespace jc8048w550
