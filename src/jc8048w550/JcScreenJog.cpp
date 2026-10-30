// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Jog page. In the ordinary firmware every jog is a PREVIEW: nothing is sent.
// The code under JC_LIVE_JOG_TEST belongs to the experimental live-jog test build
// (PlatformIO env jc8048w550_livejog_test) and is not compiled into anything else.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

ui::Rect axis_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 100), 76, 92, 50 }; }
ui::Rect step_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 92), 138, 86, 46 }; }
ui::Rect feed_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 130), 194, 122, 46 }; }
constexpr ui::Rect kIncremental { 484, 194, 150, 46 };
constexpr ui::Rect kContinuous { 640, 194, 144, 46 };  // the ARM button in the live-jog test build
constexpr ui::Rect kJogNeg { 16, 254, 250, 84 };
constexpr ui::Rect kJogStop { 275, 254, 250, 84 };
constexpr ui::Rect kJogPos { 534, 254, 250, 84 };
constexpr ui::Rect kDro { 424, 76, 360, 50 };
constexpr ui::Rect kPanelRect { 16, 350, 768, 122 };
constexpr ui::Rect kAck { 28, 432, 744, 36 };

#if JC_LIVE_JOG_TEST
const char* tri_text(JcTri t) { return t == JcTri::Yes ? "ON" : (t == JcTri::No ? "OFF" : "UNKNOWN"); }
#endif

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
        ui::Btn style = _continuous ? ui::Btn::Disabled : (i == _step ? ui::Btn::Selected : ui::Btn::Normal);
#if JC_LIVE_JOG_TEST
        // Presets above the test limit stay selectable (the jog is then refused with a
        // clear message) but are drawn dimmed so the restriction is visible.
        if (style == ui::Btn::Normal && steps[i] > sv.profile.axes[_axis].max_step) {
            style = ui::Btn::Disabled;
        }
#endif
        ui::button(sv.display, step_rect(i), label, style);
    }
}

void JcJogScreen::draw_feeds() {
    ui::text(sv.display, 16, 206, "FEED", FontId::Body16, ui::kMuted, ui::kBg);
    const float* feeds = _axis == 3 ? sv.settings.rotary_feeds : sv.settings.linear_feeds;
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        char label[24];
        snprintf(label, sizeof(label), "%g /min", static_cast<double>(feeds[i]));
        ui::Btn style = i == _feed ? ui::Btn::Selected : ui::Btn::Normal;
#if JC_LIVE_JOG_TEST
        if (style == ui::Btn::Normal && feeds[i] > sv.profile.axes[_axis].max_feed) {
            style = ui::Btn::Disabled;
        }
#endif
        ui::button(sv.display, feed_rect(i), label, style);
    }
}

void JcJogScreen::draw_mode() {
    ui::button(sv.display, kIncremental, "INCREMENTAL", _continuous ? ui::Btn::Normal : ui::Btn::Selected);
#if JC_LIVE_JOG_TEST
    const uint32_t now = millis();
    if (sv.dispatcher.armed_at(now)) {
        char label[24];
        snprintf(label, sizeof(label), "ARMED %us", static_cast<unsigned>(sv.dispatcher.arm_remaining_ms(now) / 1000u));
        ui::button(sv.display, kContinuous, label, ui::Btn::Good);
    } else {
        ui::button(sv.display, kContinuous, "ARM TEST", ui::Btn::Danger);
    }
#else
    ui::button(sv.display, kContinuous, "CONTINUOUS",
               _axis == 3 ? ui::Btn::Disabled : (_continuous ? ui::Btn::Selected : ui::Btn::Normal));
#endif
}

void JcJogScreen::draw_bottom() {
#if JC_LIVE_JOG_TEST
    if (sv.jogs.needs_acknowledgement()) {
        ui::button(sv.display, kAck, "ACKNOWLEDGE RESULT", ui::Btn::Danger);
        return;
    }
    ui::wrap(sv.display, { 28, 436, 744, 34 },
             "LIVE JOG TEST. Software cancel is NOT an emergency stop. Keep a hand on the machine's physical E-stop.",
             FontId::Body16, RED, ui::kPanel);
#else
    ui::wrap(sv.display, { 28, 436, 744, 34 },
             "Software cancel is NOT an emergency stop. Use the machine's physical E-stop.",
             FontId::Body16, ui::kWarn, ui::kPanel);
#endif
}

void JcJogScreen::on_draw() {
    draw_axes();
    draw_steps();
    draw_feeds();
    draw_mode();
#if JC_LIVE_JOG_TEST
    ui::button(sv.display, kJogNeg, _axis == 3 ? "A  DISABLED" : "JOG  -", _axis == 3 ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kJogPos, _axis == 3 ? "A  DISABLED" : "JOG  +", _axis == 3 ? ui::Btn::Disabled : ui::Btn::Normal);
#else
    ui::button(sv.display, kJogNeg, _axis == 3 ? "A -  (deg)" : "JOG  -", ui::Btn::Normal);
    ui::button(sv.display, kJogPos, _axis == 3 ? "A +  (deg)" : "JOG  +", ui::Btn::Normal);
#endif
    ui::button(sv.display, kJogStop, "CANCEL JOG", ui::Btn::Danger);
    ui::panel(sv.display, kPanelRect);
    ui::text(sv.display, 28, 358, "PREVIEW", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, 384, "RESULT", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, 410, "MOTION", FontId::Body16, ui::kMuted, ui::kPanel);
    draw_bottom();
    _dro.invalidate();
    _motion.invalidate();
    _preview.invalidate();
    _result.invalidate();
#if JC_LIVE_JOG_TEST
    _live_key = (sv.jogs.needs_acknowledgement() ? 1u : 0u) | (sv.dispatcher.armed_at(millis()) ? 2u : 0u);
#endif
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
#if JC_LIVE_JOG_TEST
    snprintf(text, sizeof(text), "%s [%s]  jog: %s", motion, sv.model.state(), jc_jog_phase_name(sv.jogs.phase()));
#else
    snprintf(text, sizeof(text), "%s  [%s]", motion, sv.model.state());
#endif
    _motion.draw(sv.display, { 110, 406, 650, 26 }, text, FontId::Body16,
                 sv.model.in_motion() ? ui::kWarn : (sv.model.alarm() ? RED : ui::kGood), ui::kPanel);

#if JC_LIVE_JOG_TEST
    // Acknowledgement, motion and completion are separate states and are reported separately.
    char result[96];
    uint16_t color = ui::kMuted;
    const JcJogResult r = sv.jogs.result();
    if (sv.jogs.phase() == JcJogPhase::AwaitingAck) {
        snprintf(result, sizeof(result), "sent, waiting for the controller's ok");
        color = ui::kInfo;
    } else if (sv.jogs.phase() == JcJogPhase::Accepted || sv.jogs.phase() == JcJogPhase::Moving) {
        snprintf(result, sizeof(result), "ACCEPTED by the controller; motion not finished yet");
        color = ui::kWarn;
    } else if (sv.jogs.phase() == JcJogPhase::Cancelling) {
        snprintf(result, sizeof(result), "cancel sent; waiting for the controller to report Idle");
        color = ui::kWarn;
    } else if (r == JcJogResult::Completed) {
        snprintf(result, sizeof(result), "COMPLETED: moved %+.3f (asked %+.3f)", static_cast<double>(sv.jogs.pos_delta()),
                 static_cast<double>(sv.jogs.request().distance));
        color = ui::kGood;
    } else if (r == JcJogResult::Rejected || r == JcJogResult::Alarm) {
        snprintf(result, sizeof(result), "%s (code %d)", jc_jog_result_name(r), sv.jogs.code());
        color = RED;
    } else if (r == JcJogResult::PositionMismatch) {
        snprintf(result, sizeof(result), "POSITION MISMATCH: moved %+.3f, asked %+.3f", static_cast<double>(sv.jogs.pos_delta()),
                 static_cast<double>(sv.jogs.request().distance));
        color = RED;
    } else if (r == JcJogResult::None) {
        snprintf(result, sizeof(result), "no jog yet");
    } else {
        snprintf(result, sizeof(result), "%s", jc_jog_result_name(r));
        color = r == JcJogResult::Cancelled ? ui::kMuted : RED;
    }
    _result.draw(sv.display, { 110, 380, 650, 26 }, result, FontId::Body16, color, ui::kPanel);
#endif
}

#if JC_LIVE_JOG_TEST

const char* JcJogScreen::arm_blocker() const {
    if (!sv.live_mode) return "DEMO mode: connect LIVE first";
    if (!sv.model.fresh()) return "no fresh controller status";
    if (strcmp(sv.model.state(), "Idle") != 0) return "the machine must be Idle";
    if (sv.loader.state() != JcLoadState::Complete) return "read the controller configuration first (Settings > Machine)";
    if (sv.loader.caps().envelope_axis_mask() == 0) return "soft / hard limit settings were not read";
    if (sv.jogs.unresolved()) return "a previous operation is unresolved";
    return nullptr;
}

void JcJogScreen::request_arm() {
    if (sv.dispatcher.armed_at(millis())) {
        sv.dispatcher.disarm();
        Serial.println("jogtest: disarmed by the operator");
        draw();
        return;
    }
    if (const char* blocker = arm_blocker()) {
        show_message("CANNOT ARM THE JOG TEST", blocker, "Nothing was sent.");
        return;
    }
    open_custom_confirm("ARM THE LIVE JOG TEST?", "ARM");
    dialog.add_line("Only a restricted incremental jog can be sent:", ui::kWarn);
    dialog.add_line("X/Y 1 mm @ 100/min, Z 0.5 mm @ 50/min, A disabled.", ui::kWarn);
    dialog.add_line("Each jog needs its own confirmation.");
    dialog.add_line("Arming lapses after 120 s, on leaving this page,");
    dialog.add_line("and on any loss of the connection.");
    dialog.add_line("Software cancel is NOT an emergency stop.", RED);
    draw();
}

void JcJogScreen::on_custom_confirm() {
    sv.dispatcher.arm_for(millis(), kJcJogArmTtlMs);
    Serial.println("jogtest: ARMED for 120 s");
}

void JcJogScreen::jog_live(int direction) {
    const uint32_t now = millis();
    const JcCommand command = jc_jog_incremental(sv.profile, _axis, direction * step_value(), feed_value());
    if (!command.ok) {
        _preview.draw(sv.display, { 110, 354, 650, 26 }, "(no command)", FontId::Body16, ui::kMuted, ui::kPanel);
        char reason[64];
        snprintf(reason, sizeof(reason), "Refused: %s", jc_cmd_error_name(command.error));
        sv.dispatcher.submit(command, sv.dispatch_context(false));  // recorded as rejected
        Serial.printf("jogtest: refused axis=%s step=%g feed=%g: %s\n", jc_axis_name(_axis),
                      static_cast<double>(direction * step_value()), static_cast<double>(feed_value()),
                      jc_cmd_error_name(command.error));
        show_message("JOG REFUSED", reason, "Test limits: X/Y 1 mm @ 100/min, Z 0.5 mm @ 50/min.",
                     "A is disabled (rotary calibration pending). No continuous jog.");
        return;
    }
    _preview.draw(sv.display, { 110, 354, 650, 26 }, command.text, FontId::Body16, CYAN, ui::kPanel);
    if (!sv.dispatcher.armed_at(now)) {
        show_message("NOT ARMED", "Tap ARM TEST first.", "Arming lapses after 120 s, on leaving this page,",
                     "and on any loss of the connection.");
        return;
    }
    // Everything except the operator's confirmation is checked BEFORE the dialog opens.
    const JcAuthDecision pre = sv.dispatcher.evaluate(command, sv.dispatch_context(true));
    if (pre != JcAuthDecision::Allowed) {
        show_message("JOG BLOCKED", jc_auth_name(pre), "Nothing was sent.");
        return;
    }
    const JcJogReject why = sv.jogs.can_begin(now);
    if (why != JcJogReject::None) {
        show_message("JOG REFUSED", jc_jog_reject_name(why), "Nothing was sent.");
        return;
    }
    // What the controller itself enforces. A configured travel is NOT proof the path is clear.
    const JcMachineCaps& caps = sv.loader.caps();
    const JcAxisCaps& ax = caps.axes[_axis];
    char l1[80], l2[80], l3[80];
    const int pins = ax.limit_pins_assigned();
    char pin_text[8];
    if (pins < 0) snprintf(pin_text, sizeof(pin_text), "?");
    else snprintf(pin_text, sizeof(pin_text), "%d", pins);
    snprintf(l1, sizeof(l1), "%s: soft limits %s, hard limits %s, limit pins %s", jc_axis_name(_axis),
             tri_text(ax.soft_limits()), tri_text(ax.hard_limits()), pin_text);
    if (ax.homing_known()) {
        snprintf(l2, sizeof(l2), "Homing cycle %g, must_home %s", static_cast<double>(ax.get(JcQueryField::HomingCycle)),
                 tri_text(caps.must_home_tri()));
    } else {
        snprintf(l2, sizeof(l2), "Homing UNKNOWN, must_home %s", tri_text(caps.must_home_tri()));
    }
    snprintf(l3, sizeof(l3), "Travel %g mm is NOT proof the path is clear.", static_cast<double>(ax.get(JcQueryField::MaxTravelMm)));
    const char* notes[3] = { l1, l2, l3 };
    Serial.printf("jogtest: staged [%s] soft=%s hard=%s pins=%s\n", command.text, tri_text(ax.soft_limits()),
                  tri_text(ax.hard_limits()), pin_text);
    stage(command, "CONFIRM LIVE JOG", notes, 3);
}

void JcJogScreen::do_cancel() {
    const JcCommand cancel = jc_jog_cancel();
    _preview.draw(sv.display, { 110, 354, 650, 26 }, "<jog cancel 0x85>", FontId::Body16, CYAN, ui::kPanel);
    const uint32_t before = sv.dispatcher.sent();
    const JcAuthDecision decision = submit_now(cancel);   // never needs arming or confirmation
    const bool sent = sv.dispatcher.sent() != before;
    Serial.printf("jogtest: cancel requested: %s (%s)\n", jc_auth_name(decision), sent ? "SENT" : "NOT SENT");
    if (!sent) {
        show_message("CANCEL NOT SENT", jc_auth_name(decision),
                     "IF THE MACHINE IS MOVING, USE THE PHYSICAL E-STOP NOW.");
    }
}

#else   // ordinary firmware: these exist only so the class has one definition

const char* JcJogScreen::arm_blocker() const { return "machine control is disabled in this build"; }
void JcJogScreen::request_arm() {}
void JcJogScreen::on_custom_confirm() {}
void JcJogScreen::jog_live(int) {}
void JcJogScreen::do_cancel() {}

#endif

void JcJogScreen::do_jog(int direction) {
#if JC_LIVE_JOG_TEST
    jog_live(direction);
    return;
#else
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
#endif
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
#if JC_LIVE_JOG_TEST
            ui::button(sv.display, kJogNeg, _axis == 3 ? "A  DISABLED" : "JOG  -", _axis == 3 ? ui::Btn::Disabled : ui::Btn::Normal);
            ui::button(sv.display, kJogPos, _axis == 3 ? "A  DISABLED" : "JOG  +", _axis == 3 ? ui::Btn::Disabled : ui::Btn::Normal);
#else
            ui::button(sv.display, kJogNeg, _axis == 3 ? "A -  (deg)" : "JOG  -", ui::Btn::Normal);
            ui::button(sv.display, kJogPos, _axis == 3 ? "A +  (deg)" : "JOG  +", ui::Btn::Normal);
#endif
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
#if JC_LIVE_JOG_TEST
        request_arm();
        return;
#else
        if (_axis == 3) {
            show_message("CONTINUOUS JOG UNAVAILABLE", "Continuous jogging is not supported for the rotary A axis.",
                         "Use incremental jogging (degrees).");
            return;
        }
        _continuous = true;
        draw_mode();
        draw_steps();
        return;
#endif
    }
#if JC_LIVE_JOG_TEST
    if (sv.jogs.needs_acknowledgement() && ui::hit(kAck, x, y)) {
        if (!sv.jogs.acknowledge()) {
            show_message("CANNOT ACKNOWLEDGE YET", "The operation is still in progress.",
                         "Wait for the controller to report Idle, or press CANCEL JOG.");
        } else {
            Serial.println("jogtest: result acknowledged by the operator");
            draw();
        }
        return;
    }
#endif
    if (ui::hit(kJogNeg, x, y)) {
        do_jog(-1);
    } else if (ui::hit(kJogPos, x, y)) {
        do_jog(1);
    } else if (ui::hit(kJogStop, x, y)) {
#if JC_LIVE_JOG_TEST
        do_cancel();
#else
        const JcCommand cancel = jc_jog_cancel();
        _preview.draw(sv.display, { 110, 354, 650, 26 }, "<jog cancel 0x85>", FontId::Body16, CYAN, ui::kPanel);
        const JcAuthDecision decision = submit_now(cancel);
        char line[96];
        snprintf(line, sizeof(line), "%s - NOT SENT", jc_auth_name(decision));
        _result.draw(sv.display, { 110, 380, 650, 26 }, line, FontId::Body16, ui::kWarn, ui::kPanel);
#endif
    }
}

void JcJogScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 250) {
        _last_tick = now;
#if JC_LIVE_JOG_TEST
        const uint32_t key = (sv.jogs.needs_acknowledgement() ? 1u : 0u) | (sv.dispatcher.armed_at(now) ? 2u : 0u);
        if (key != _live_key) {
            draw();  // the ACK button or the arming state changed
            return;
        }
        if (sv.dispatcher.armed_at(now)) {
            draw_mode();  // seconds remaining
        }
#endif
        draw_live();
    }
}

}  // namespace jc8048w550
