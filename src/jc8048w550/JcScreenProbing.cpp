// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

#include <stdlib.h>

namespace jc8048w550 {

namespace {

ui::Rect mode_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 100), 76, 92, 46 }; }
ui::Rect row_label(uint8_t i) { return { 16, static_cast<int16_t>(146 + i * 46), 300, 38 }; }
ui::Rect row_value(uint8_t i) { return { 330, static_cast<int16_t>(142 + i * 46), 150, 40 }; }
constexpr ui::Rect kRunButton { 16, 398, 464, 66 };
constexpr ui::Rect kStatusPanel { 500, 76, 284, 150 };
constexpr ui::Rect kNotePanel { 500, 238, 284, 226 };
constexpr const char* kModeLabels[5] = { "Z TOOL", "EDGE X-", "EDGE X+", "EDGE Y-", "EDGE Y+" };

void edit_callback(void* user, bool ok, const char* text) {
    static_cast<JcProbingScreen*>(user)->edit_done(ok, text);
}

}  // namespace

float* JcProbingScreen::field_ptr(uint8_t field) {
    switch (field) {
        case 0: return &sv.settings.probe_feed;
        case 1: return &sv.settings.probe_travel;
        case 2: return &sv.settings.probe_retract;
        default: return _mode == ModeZ ? &sv.settings.plate_thickness : &sv.settings.tool_diameter;
    }
}

void JcProbingScreen::draw_modes() {
    for (uint8_t i = 0; i < ModeCount; ++i) {
        ui::button(sv.display, mode_rect(i), kModeLabels[i], i == _mode ? ui::Btn::Selected : ui::Btn::Normal);
    }
}

void JcProbingScreen::draw_params() {
    const char* labels[4] = { "Probe feed (mm/min)", "Max travel (mm)", "Retract (mm)",
                              _mode == ModeZ ? "Plate thickness (mm)" : "Tool diameter (mm)" };
    for (uint8_t i = 0; i < 4; ++i) {
        sv.display.fillRect(row_label(i).x, row_label(i).y, row_label(i).w, row_label(i).h, ui::kBg);
        ui::text(sv.display, row_label(i).x, row_label(i).y + 8, labels[i], FontId::Body16, ui::kMuted, ui::kBg);
        char v[24];
        snprintf(v, sizeof(v), "%g", static_cast<double>(*field_ptr(i)));
        ui::button(sv.display, row_value(i), v, ui::Btn::Normal);
    }
}

void JcProbingScreen::draw_probe_status() {
    const char* text;
    uint16_t color;
    if (!sv.model.has_status()) {
        text = "PROBE: NO DATA";
        color = ui::kMuted;
    } else if (sv.model.probe_triggered()) {
        text = "PROBE: TRIGGERED";
        color = ui::kWarn;
    } else {
        text = "PROBE: open";
        color = ui::kGood;
    }
    _pin.draw(sv.display, { 510, 84, 264, 34 }, text, FontId::Button20, color, ui::kPanel);
}

void JcProbingScreen::on_draw() {
    draw_modes();
    draw_params();
    ui::button(sv.display, kRunButton, "PREVIEW PROBE SEQUENCE", ui::Btn::Normal);
    ui::panel(sv.display, kStatusPanel);
    ui::text(sv.display, 510, 128, "from FluidNC 'Pn:' pin field", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::wrap(sv.display, { 510, 156, 266, 66 },
             "Probing needs live status. Touch the probe once and check it reads TRIGGERED.",
             FontId::Body16, ui::kMuted, ui::kPanel);
    ui::panel(sv.display, kNotePanel);
    ui::text(sv.display, 510, 246, "PREFLIGHT / RECOVERY", FontId::Button20, WHITE, ui::kPanel);
    ui::wrap(sv.display, { 510, 276, 266, 186 },
             "Plate/probe connected and open. Tool clear of clamps. If no contact within the max travel, "
             "FluidNC raises an ALARM: unlock ($X), reposition and retry. Rotary A is never probed "
             "(no linear probing routine is applied to it).",
             FontId::Body16, ui::kMuted, ui::kPanel);
    _pin.invalidate();
    draw_probe_status();
}

void JcProbingScreen::begin_edit(uint8_t field) {
    _edit_field = field;
    JcInputRequest r {};
    static const char* const prompts[4] = { "Probe feed (mm/min)", "Max probe travel (mm)", "Retract distance (mm)",
                                            "Plate thickness / tool diameter (mm)" };
    r.prompt = prompts[field];
    snprintf(r.initial, sizeof(r.initial), "%g", static_cast<double>(*field_ptr(field)));
    r.numeric = true;
    r.max_len = 10;
    r.done = edit_callback;
    r.user = this;
    sv.host.request_input(r);
}

void JcProbingScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    char* end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || *end != '\0') {
        return;
    }
    *field_ptr(_edit_field) = value;
    jc_settings_sanitize(sv.settings);  // clamps to the allowed probe limits
    sv.host.save_settings();
    draw();
}

void JcProbingScreen::run_sequence() {
    const JcUserSettings& s = sv.settings;
    JcCommandSequence seq;
    const char* title;
    if (_mode == ModeZ) {
        seq = jc_probe_z_sequence(sv.profile, s.probe_travel, s.probe_feed, s.probe_retract, s.plate_thickness);
        title = "Z TOOL PROBE";
    } else {
        const uint8_t axis = (_mode == ModeXNeg || _mode == ModeXPos) ? 0 : 1;
        const int direction = (_mode == ModeXPos || _mode == ModeYPos) ? 1 : -1;
        seq = jc_probe_edge_sequence(sv.profile, axis, direction, s.probe_travel, s.probe_feed, s.probe_retract,
                                     s.tool_diameter);
        title = "EDGE PROBE";
    }
    const char* notes[3];
    uint8_t n = 0;
    notes[n++] = "Steps run only if the probe makes contact; abort on any error.";
    if (sv.model.probe_triggered()) {
        notes[n++] = "WARNING: probe input is already TRIGGERED - check wiring.";
    }
    if (!sv.model.fresh()) {
        notes[n++] = "WARNING: no fresh live status; contact cannot be verified.";
    }
    stage_sequence(seq, title, notes, n);
}

void JcProbingScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < ModeCount; ++i) {
        if (ui::hit(mode_rect(i), x, y)) {
            _mode = i;
            draw_modes();
            draw_params();
            return;
        }
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (ui::hit(row_value(i), x, y)) {
            begin_edit(i);
            return;
        }
    }
    if (ui::hit(kRunButton, x, y)) {
        run_sequence();
    }
}

void JcProbingScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 300) {
        _last_tick = now;
        draw_probe_status();
    }
}

}  // namespace jc8048w550
