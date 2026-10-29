// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

ui::Rect axis_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 100), 76, 92, 50 }; }
ui::Rect step_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 92), 138, 86, 46 }; }
ui::Rect feed_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 130), 194, 122, 46 }; }
constexpr ui::Rect kIncremental { 484, 194, 150, 46 };
constexpr ui::Rect kContinuous { 640, 194, 144, 46 };
constexpr ui::Rect kJogNeg { 16, 254, 250, 84 };
constexpr ui::Rect kJogStop { 275, 254, 250, 84 };
constexpr ui::Rect kJogPos { 534, 254, 250, 84 };
constexpr ui::Rect kDro { 424, 76, 360, 50 };
constexpr ui::Rect kPanelRect { 16, 350, 768, 122 };

}  // namespace

float JcJogScreen::step_value() const {
    const float* steps = _axis == 3 ? sv.settings.rotary_steps : sv.settings.linear_steps;
    return steps[_step < kJcStepPresets ? _step : 0];
}

float JcJogScreen::feed_value() const {
    const float* feeds = _axis == 3 ? sv.settings.rotary_feeds : sv.settings.linear_feeds;
    return feeds[_feed < kJcFeedPresets ? _feed : 0];
}

void JcJogScreen::on_enter() {
    _dro.invalidate();
    _motion.invalidate();
    _preview.invalidate();
    _result.invalidate();
}

void JcJogScreen::draw_axes() {
    for (uint8_t i = 0; i < 4; ++i) {
        ui::button(sv.display, axis_rect(i), jc_axis_name(i), i == _axis ? ui::Btn::Selected : ui::Btn::Normal);
    }
}

void JcJogScreen::draw_steps() {
    ui::text(sv.display, 16, 150, "STEP", FontId::Body16, ui::kMuted, ui::kBg);
    const float* steps = _axis == 3 ? sv.settings.rotary_steps : sv.settings.linear_steps;
    for (uint8_t i = 0; i < kJcStepPresets; ++i) {
        char label[24];
        jc_format_step(label, sizeof(label), steps[i], _axis);
        ui::button(sv.display, step_rect(i), label,
                   _continuous ? ui::Btn::Disabled : (i == _step ? ui::Btn::Selected : ui::Btn::Normal));
    }
}

void JcJogScreen::draw_feeds() {
    ui::text(sv.display, 16, 206, "FEED", FontId::Body16, ui::kMuted, ui::kBg);
    const float* feeds = _axis == 3 ? sv.settings.rotary_feeds : sv.settings.linear_feeds;
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        char label[24];
        snprintf(label, sizeof(label), "%g /min", static_cast<double>(feeds[i]));
        ui::button(sv.display, feed_rect(i), label, i == _feed ? ui::Btn::Selected : ui::Btn::Normal);
    }
}

void JcJogScreen::draw_mode() {
    ui::button(sv.display, kIncremental, "INCREMENTAL", _continuous ? ui::Btn::Normal : ui::Btn::Selected);
    ui::button(sv.display, kContinuous, "CONTINUOUS",
               _axis == 3 ? ui::Btn::Disabled : (_continuous ? ui::Btn::Selected : ui::Btn::Normal));
}

void JcJogScreen::on_draw() {
    draw_axes();
    draw_steps();
    draw_feeds();
    draw_mode();
    ui::button(sv.display, kJogNeg, _axis == 3 ? "A -  (deg)" : "JOG  -", ui::Btn::Normal);
    ui::button(sv.display, kJogStop, "CANCEL JOG", ui::Btn::Danger);
    ui::button(sv.display, kJogPos, _axis == 3 ? "A +  (deg)" : "JOG  +", ui::Btn::Normal);
    ui::panel(sv.display, kPanelRect);
    ui::text(sv.display, 28, 358, "PREVIEW", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, 384, "RESULT", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, 410, "MOTION", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::wrap(sv.display, { 28, 436, 744, 34 },
             "Software cancel is NOT an emergency stop. Use the machine's physical E-stop.",
             FontId::Body16, ui::kWarn, ui::kPanel);
    _dro.invalidate();
    _motion.invalidate();
    _preview.invalidate();
    _result.invalidate();
    draw_live();
}

void JcJogScreen::draw_live() {
    char value[32];
    char line[64];
    const JcRef ref = sv.settings.default_reference ? JcRef::Work : JcRef::Machine;
    bool ok = false;
    jc_format_position(sv, _axis, ref, value, sizeof(value), &ok);
    if (ok) {
        snprintf(line, sizeof(line), "%s %s  %s %s", ref == JcRef::Work ? "WORK" : "MACH", jc_axis_name(_axis),
                 value, jc_axis_unit(_axis));
    } else {
        snprintf(line, sizeof(line), "%s: no position available", jc_axis_name(_axis));
    }
    _dro.draw(sv.display, kDro, line, FontId::Button20, ok && sv.live_fresh() ? WHITE : ui::kMuted, ui::kPanel);

    const char* motion = !sv.model.has_status() ? "UNKNOWN (no live data)"
                       : sv.model.in_motion() ? "ACTIVE" : (sv.model.alarm() ? "ALARM" : "IDLE");
    char text[64];
    snprintf(text, sizeof(text), "%s  [%s]", motion, sv.model.state());
    _motion.draw(sv.display, { 110, 406, 650, 26 }, text, FontId::Body16,
                 sv.model.in_motion() ? ui::kWarn : (sv.model.alarm() ? RED : ui::kGood), ui::kPanel);
}

void JcJogScreen::do_jog(int direction) {
    JcCommand command;
    if (_continuous) {
        command = jc_jog_continuous(sv.profile, _axis, direction, feed_value());
    } else {
        command = jc_jog_incremental(sv.profile, _axis, direction * step_value(), feed_value());
    }
    char line[96];
    if (!command.ok) {
        snprintf(line, sizeof(line), "Rejected: %s", jc_cmd_error_name(command.error));
        _preview.draw(sv.display, { 110, 354, 650, 26 }, "(no command)", FontId::Body16, ui::kMuted, ui::kPanel);
        _result.draw(sv.display, { 110, 380, 650, 26 }, line, FontId::Body16, RED, ui::kPanel);
        submit_now(command);
        return;
    }
    _preview.draw(sv.display, { 110, 354, 650, 26 }, command.text, FontId::Body16, CYAN, ui::kPanel);
    const JcAuthDecision decision = submit_now(command);
    snprintf(line, sizeof(line), "%s - NOT SENT", jc_auth_name(decision));
    _result.draw(sv.display, { 110, 380, 650, 26 }, line, FontId::Body16, ui::kWarn, ui::kPanel);
    Serial.printf("jog: preview [%s] %s\n", command.text, jc_auth_name(decision));
}

void JcJogScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < 4; ++i) {
        if (ui::hit(axis_rect(i), x, y)) {
            _axis = i;
            if (_axis == 3) {
                _continuous = false;  // continuous jog is never generated for the rotary axis
            }
            draw_axes();
            draw_steps();
            draw_feeds();
            draw_mode();
            ui::button(sv.display, kJogNeg, _axis == 3 ? "A -  (deg)" : "JOG  -", ui::Btn::Normal);
            ui::button(sv.display, kJogPos, _axis == 3 ? "A +  (deg)" : "JOG  +", ui::Btn::Normal);
            _dro.invalidate();
            draw_live();
            return;
        }
    }
    for (uint8_t i = 0; i < kJcStepPresets; ++i) {
        if (ui::hit(step_rect(i), x, y) && !_continuous) {
            _step = i;
            draw_steps();
            return;
        }
    }
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        if (ui::hit(feed_rect(i), x, y)) {
            _feed = i;
            draw_feeds();
            return;
        }
    }
    if (ui::hit(kIncremental, x, y)) {
        _continuous = false;
        draw_mode();
        draw_steps();
        return;
    }
    if (ui::hit(kContinuous, x, y)) {
        if (_axis == 3) {
            show_message("CONTINUOUS JOG UNAVAILABLE", "Continuous jogging is not supported for the rotary A axis.",
                         "Use incremental jogging (degrees).");
            return;
        }
        _continuous = true;
        draw_mode();
        draw_steps();
        return;
    }
    if (ui::hit(kJogNeg, x, y)) {
        do_jog(-1);
    } else if (ui::hit(kJogPos, x, y)) {
        do_jog(1);
    } else if (ui::hit(kJogStop, x, y)) {
        const JcCommand cancel = jc_jog_cancel();
        _preview.draw(sv.display, { 110, 354, 650, 26 }, "<jog cancel 0x85>", FontId::Body16, CYAN, ui::kPanel);
        const JcAuthDecision decision = submit_now(cancel);
        char line[96];
        snprintf(line, sizeof(line), "%s - NOT SENT", jc_auth_name(decision));
        _result.draw(sv.display, { 110, 380, 650, 26 }, line, FontId::Body16, ui::kWarn, ui::kPanel);
    }
}

void JcJogScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 250) {
        _last_tick = now;
        draw_live();
    }
}

}  // namespace jc8048w550
