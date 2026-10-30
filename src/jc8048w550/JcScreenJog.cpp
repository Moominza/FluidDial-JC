// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Jog page. In the ordinary firmware every jog is a PREVIEW: nothing is sent.
// The code under JC_LIVE_JOG_TEST belongs to the experimental live-jog test build
// (PlatformIO env jc8048w550_livejog_test) and is not compiled into anything else.

#include "JcJogLayout.hpp"
#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

using jl::axis_rect;
using jl::step_rect;
using jl::feed_rect;
using jl::kIncremental;
using jl::kJogNeg;
using jl::kJogStop;
using jl::kJogPos;
using jl::kDro;
using jl::kPanelRect;
using jl::kAck;
constexpr ui::Rect kContinuous = jl::kArm;  // ARM / READ SETTINGS in the jog-UI builds

#if JC_JOG_PREVIEW
#define JC_ARM_LABEL "ARM PREVIEW"
#define JC_STAGE_TITLE "CONFIRM SIMULATED JOG"
#define JC_ARM_TITLE "PREVIEW ONLY: ARM MICRO-JOG?"
#else
#define JC_ARM_LABEL "ARM TEST"
#define JC_STAGE_TITLE "CONFIRM LIVE JOG"
#define JC_ARM_TITLE "ARM UNPROTECTED MICRO-JOG?"
#endif

#if JC_JOG_UI
const char* tri_text(JcTri t) { return t == JcTri::Yes ? "ON" : (t == JcTri::No ? "OFF" : "UNKNOWN"); }

// Z is on hold and A is disabled in the first-motion policy; only X and Y can jog.
bool live_axis_allowed(uint8_t axis) { return axis < 2; }
const char* live_jog_label(uint8_t axis, bool negative) {
    if (axis == 2) return "Z  ON HOLD";
    if (axis == 3) return "A  DISABLED";
    return negative ? "JOG  -" : "JOG  +";
}
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
    _auto_tried = false;
    _drawn_step = 0xFF;
    service_caps(millis());
    _dro.invalidate();
    _motion.invalidate();
    _preview.invalidate();
    _result.invalidate();
    _caps.invalidate();
}

void JcJogScreen::draw_axes() {
    for (uint8_t i = 0; i < 4; ++i) {
        ui::button(sv.display, axis_rect(i), jc_axis_name(i), i == _axis ? ui::Btn::Selected : ui::Btn::Normal);
    }
}

void JcJogScreen::draw_steps() {
    ui::text(sv.display, 16, 138, "STEP", FontId::Body16, ui::kMuted, ui::kBg);
    const float* steps = _axis == 3 ? sv.settings.rotary_steps : sv.settings.linear_steps;
    for (uint8_t i = 0; i < kJcStepPresets; ++i) {
        char label[24];
        jc_format_step(label, sizeof(label), steps[i], _axis);
        ui::Btn style = _continuous ? ui::Btn::Disabled : (i == _step ? ui::Btn::Selected : ui::Btn::Normal);
#if JC_JOG_UI
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
    ui::text(sv.display, 16, 190, "FEED", FontId::Body16, ui::kMuted, ui::kBg);
    const float* feeds = _axis == 3 ? sv.settings.rotary_feeds : sv.settings.linear_feeds;
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        char label[24];
        snprintf(label, sizeof(label), "%g /min", static_cast<double>(feeds[i]));
        ui::Btn style = i == _feed ? ui::Btn::Selected : ui::Btn::Normal;
#if JC_JOG_UI
        if (style == ui::Btn::Normal && feeds[i] > sv.profile.axes[_axis].max_feed) {
            style = ui::Btn::Disabled;
        }
#endif
        ui::button(sv.display, feed_rect(i), label, style);
    }
}

void JcJogScreen::draw_mode() {
    ui::button(sv.display, kIncremental, "INCREMENTAL", _continuous ? ui::Btn::Normal : ui::Btn::Selected);
#if JC_JOG_UI
    const uint32_t now = millis();
    if (!live_axis_allowed(_axis)) {
        ui::button(sv.display, kContinuous, "NOT AVAILABLE", ui::Btn::Disabled);
    } else if (sv.dispatcher.armed_at(now) && ((sv.dispatcher.unprotected_ack_mask() >> _axis) & 1u) != 0) {
        char label[24];
        snprintf(label, sizeof(label), "ARMED %us", static_cast<unsigned>(sv.dispatcher.arm_remaining_ms(now) / 1000u));
        ui::button(sv.display, kContinuous, label, ui::Btn::Good);
    } else {
        const JcCapsFlow flow = caps_flow(now);
        char label[24];
        switch (flow.phase) {
            case JcCapsPhase::Current:
                ui::button(sv.display, kContinuous, JC_ARM_LABEL, ui::Btn::Danger);
                break;
            case JcCapsPhase::Reading:
                snprintf(label, sizeof(label), "READING %u/%u", static_cast<unsigned>(sv.loader.step()),
                         static_cast<unsigned>(JcCapsLoader::kSteps));
                ui::button(sv.display, kContinuous, label, ui::Btn::Disabled);
                break;
            case JcCapsPhase::NotLive:
                ui::button(sv.display, kContinuous, jc_caps_action_label(flow), ui::Btn::Disabled);
                break;
            default:   // NotRead / Expired / Failed: the prominent action
                ui::button(sv.display, kContinuous, jc_caps_action_label(flow), flow.can_read ? ui::Btn::Selected : ui::Btn::Disabled);
                break;
        }
    }
#else
    ui::button(sv.display, kContinuous, "CONTINUOUS",
               _axis == 3 ? ui::Btn::Disabled : (_continuous ? ui::Btn::Selected : ui::Btn::Normal));
#endif
}

void JcJogScreen::draw_bottom() {
#if JC_JOG_UI
    if (sv.jogs.needs_acknowledgement()) {
        ui::button(sv.display, kAck, "ACKNOWLEDGE RESULT", ui::Btn::Danger);
        return;
    }
    ui::wrap(sv.display, jl::kWarnRect, kJcLiveJogWarning, FontId::Body16, JC_JOG_PREVIEW ? ui::kInfo : RED, ui::kPanel);
#else
    ui::wrap(sv.display, jl::kWarnRect,
             "Software cancel is NOT an emergency stop. Use the machine's physical E-stop.",
             FontId::Body16, ui::kWarn, ui::kPanel);
#endif
}

void JcJogScreen::on_draw() {
    draw_axes();
    draw_steps();
    draw_feeds();
    draw_mode();
#if JC_JOG_UI
    ui::button(sv.display, kJogNeg, live_jog_label(_axis, true), live_axis_allowed(_axis) ? ui::Btn::Normal : ui::Btn::Disabled);
    ui::button(sv.display, kJogPos, live_jog_label(_axis, false), live_axis_allowed(_axis) ? ui::Btn::Normal : ui::Btn::Disabled);
#else
    ui::button(sv.display, kJogNeg, _axis == 3 ? "A -  (deg)" : "JOG  -", ui::Btn::Normal);
    ui::button(sv.display, kJogPos, _axis == 3 ? "A +  (deg)" : "JOG  +", ui::Btn::Normal);
#endif
    ui::button(sv.display, kJogStop, "CANCEL JOG", ui::Btn::Danger);
    ui::panel(sv.display, kPanelRect);
    ui::text(sv.display, 28, jl::kLabelPreviewY, "PREVIEW", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, jl::kLabelResultY, "RESULT", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::text(sv.display, 28, jl::kLabelMotionY, "MOTION", FontId::Body16, ui::kMuted, ui::kPanel);
    draw_bottom();
    _dro.invalidate();
    _motion.invalidate();
    _preview.invalidate();
    _result.invalidate();
    _caps.invalidate();
#if JC_JOG_UI
    _live_key = (sv.jogs.needs_acknowledgement() ? 1u : 0u) | (sv.dispatcher.armed_at(millis()) ? 2u : 0u)
                | (static_cast<uint32_t>(caps_flow(millis()).phase) << 2);
#endif
    draw_live();
}

#if JC_JOG_UI
JcCapsFlow JcJogScreen::caps_flow(uint32_t now) const {
    return jc_caps_flow(sv.live_mode, sv.model.fresh(), sv.model.state(), sv.jogs.unresolved(), sv.loader.state(),
                        sv.loader.caps().complete, sv.loader.caps().completed_ms, now, _auto_tried);
}

// READ MACHINE SETTINGS: the existing read-only capability read (allowlisted `$` queries only).
void JcJogScreen::read_settings() {
    const char* blocker = sv.host.start_caps_read();
    if (blocker != nullptr) {
        Serial.printf(JC_JOG_TAG ": settings read refused: %s\n", blocker);
        show_message("CANNOT READ MACHINE SETTINGS", blocker, "The queries are read-only; nothing is changed.");
        return;
    }
    Serial.println(JC_JOG_TAG ": machine settings read started from the jog page (read-only)");
    draw();
}

// Called from enter and the tick: starts ONE automatic read per visit when it is plainly needed and safe.
void JcJogScreen::service_caps(uint32_t now) {
    if (!sv.model.fresh()) {
        _auto_tried = false;   // a lost connection discards the read; allow one automatic read after recovery
    }
    const JcCapsFlow flow = caps_flow(now);
    if (flow.auto_read) {
        _auto_tried = true;
        if (const char* blocker = sv.host.start_caps_read()) {
            Serial.printf(JC_JOG_TAG ": automatic settings read not started: %s\n", blocker);
        } else {
            Serial.println(JC_JOG_TAG ": automatic machine settings read started (read-only)");
        }
    }
}
#else
JcCapsFlow JcJogScreen::caps_flow(uint32_t) const { return JcCapsFlow(); }
void JcJogScreen::read_settings() {}
void JcJogScreen::service_caps(uint32_t) {}
#endif

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
    char text[96];
#if JC_JOG_UI
    {
        const JcJogGate g = jc_jog_gate(sv.loader.caps(), sv.loader.state() == JcLoadState::Complete, millis(), _axis,
                                        jc_position_trust(sv.loader.caps()));
        jc_jog_motion_line(text, sizeof(text), motion, sv.model.state(), sv.jogs.phase(), jc_jog_gate_mode_name(g.mode));
    }
#else
    snprintf(text, sizeof(text), "%s  [%s]", motion, sv.model.state());
#endif
    _motion.draw(sv.display, jl::kMotionSlot, text, FontId::Body16,
                 sv.model.in_motion() ? ui::kWarn : (sv.model.alarm() ? RED : ui::kGood), ui::kPanel);

#if JC_JOG_UI
    // Acknowledgement, motion and completion are separate states and are reported separately.
    char result[96];
    const JcJogTone tone = jc_jog_result_line(sv.jogs, sv.live_fresh(), result, sizeof(result), JC_JOG_PREVIEW != 0);
    const uint16_t color = tone == JcJogTone::Alarm ? RED : tone == JcJogTone::Warn ? ui::kWarn
                         : tone == JcJogTone::Info ? ui::kInfo : tone == JcJogTone::Good ? ui::kGood : ui::kMuted;
    _result.draw(sv.display, jl::kResultSlot, result, FontId::Body16, color, ui::kPanel);

    // Capability workflow: always visible, so the operator never has to guess why ARM is unavailable.
    const uint32_t now = millis();
    const JcCapsFlow flow = caps_flow(now);
    char caps_text[120], caps_line[140];
    jc_caps_status_text(flow, sv.loader.step(), JcCapsLoader::kSteps, sv.loader.failure(),
                        static_cast<uint32_t>(now - sv.loader.caps().completed_ms), caps_text, sizeof(caps_text));
#if JC_JOG_PREVIEW
    snprintf(caps_line, sizeof(caps_line), "REAL controller: %s", caps_text);
#else
    snprintf(caps_line, sizeof(caps_line), "%s", caps_text);
#endif
    _caps.draw(sv.display, jl::kCapsSlot, caps_line, FontId::Body16,
               flow.phase == JcCapsPhase::Current ? ui::kGood : (flow.phase == JcCapsPhase::Reading ? ui::kInfo : ui::kWarn),
               ui::kPanel);
#endif
}

#if JC_JOG_UI

// Why the selected axis cannot be armed now (nullptr = it may). Uses the same gate the
// authorization context uses, so the page and the gate cannot disagree.
const char* JcJogScreen::arm_blocker() const {
    if (!live_axis_allowed(_axis)) {
        return _axis == 2 ? "Z is ON HOLD: homing direction / limit side need owner verification"
                          : "A is disabled pending rotary calibration";
    }
    if (!sv.live_mode) return "DEMO mode: connect LIVE first";
    if (!sv.model.fresh()) return "no fresh controller status";
    if (strcmp(sv.model.state(), "Idle") != 0) return "the machine must be Idle";
    if (sv.jogs.unresolved()) return "a previous operation is unresolved";
    const JcJogGate g = jc_jog_gate(sv.loader.caps(), sv.loader.state() == JcLoadState::Complete, millis(), _axis,
                                    jc_position_trust(sv.loader.caps()));
    if (g.mode == JcJogGateMode::Blocked) return jc_jog_gate_reason_text(g.reason);
    return nullptr;
}

void JcJogScreen::request_arm() {
    const uint32_t now = millis();
    if (sv.dispatcher.armed_at(now) && ((sv.dispatcher.unprotected_ack_mask() >> _axis) & 1u) != 0) {
        sv.dispatcher.disarm();  // tapping an armed button disarms everything
        Serial.println(JC_JOG_TAG ": disarmed by the operator");
        draw();
        return;
    }
    if (const char* blocker = arm_blocker()) {
        Serial.printf(JC_JOG_TAG ": arm refused (%s): %s\n", jc_axis_name(_axis), blocker);
        show_message("CANNOT ARM THE JOG TEST", blocker, "Nothing was sent.");
        return;
    }
    _arm_axis = _axis;
    const JcAxisCaps& ax = sv.loader.caps().axes[_axis];
    const bool both_off = ax.soft_limits() == JcTri::No && ax.hard_limits() == JcTri::No;
    char l2[80], l4[80];
    snprintf(l2, sizeof(l2), "%s: soft limits %s, hard limits %s.", jc_axis_name(_axis), tri_text(ax.soft_limits()),
             tri_text(ax.hard_limits()));
    snprintf(l4, sizeof(l4), "%s", ax.hard_limits() == JcTri::No ? "Limit switches will not stop an ordinary jog."
                                                               : "Limit switches may not stop a jog: position is not trusted.");
    open_custom_confirm(JC_ARM_TITLE, "I ACKNOWLEDGE");
    dialog.add_line(both_off ? "NO CONTROLLER TRAVEL PROTECTION" : "NO TRUSTED CONTROLLER TRAVEL PROTECTION", RED);
    dialog.add_line(l2, ui::kWarn);
    dialog.add_line("Machine position may not match the physical position.", ui::kWarn);
    dialog.add_line(l4, ui::kWarn);
    dialog.add_line("Check: machine still, Idle, spindle cannot start, no job.");
    dialog.add_line("Check clear space in the commanded direction.");
    dialog.add_line("Keep E-stop / power removal in reach. Not 'safe': 1 mm only.", RED);
    draw();
}

void JcJogScreen::on_custom_confirm() {
    // The acknowledgement covers this axis in this arming only. Axes acknowledged earlier in
    // the same, still-valid arming are kept; a lapsed or disarmed arming starts from nothing.
    const uint32_t now = millis();
    uint8_t mask = sv.dispatcher.armed_at(now) ? sv.dispatcher.unprotected_ack_mask() : 0;
    mask = static_cast<uint8_t>(mask | (1u << _arm_axis));
    sv.dispatcher.arm_for(now, kJcJogArmTtlMs, mask);
    Serial.printf(JC_JOG_TAG ": ARMED for 120 s; unprotected-micro-jog acknowledged for %s (mask 0x%02X)\n",
                  jc_axis_name(_arm_axis), static_cast<unsigned>(mask));
}

void JcJogScreen::jog_live(int direction) {
    const uint32_t now = millis();
    const JcCommand command = jc_jog_incremental(sv.profile, _axis, direction * step_value(), feed_value());
    if (!command.ok) {
        _preview.draw(sv.display, jl::kPreviewSlot, "(no command)", FontId::Body16, ui::kMuted, ui::kPanel);
        char reason[64];
        snprintf(reason, sizeof(reason), "Refused: %s", jc_cmd_error_name(command.error));
        sv.dispatcher.submit(command, sv.dispatch_context(false));  // recorded as rejected
        Serial.printf(JC_JOG_TAG ": refused axis=%s step=%g feed=%g: %s\n", jc_axis_name(_axis),
                      static_cast<double>(direction * step_value()), static_cast<double>(feed_value()),
                      jc_cmd_error_name(command.error));
        if (_axis == 2) {
            show_message("Z JOG ON HOLD", "Z is not part of the first-motion test.",
                         "Its homing direction (positive) and limit pin (negative side only)",
                         "need owner / physical verification first.");
        } else {
            show_message("JOG REFUSED", reason, "Test limits: X/Y 1 mm @ 100/min only.",
                         "Z is on hold, A is disabled. No continuous jog.");
        }
        return;
    }
    _preview.draw(sv.display, jl::kPreviewSlot, command.text, FontId::Body16, CYAN, ui::kPanel);
    if (!sv.dispatcher.armed_at(now)) {
        show_message("NOT ARMED", "Tap " JC_ARM_LABEL " first.", "Arming lapses after 120 s, on leaving this page,",
                     "and on any loss of the connection.");
        return;
    }
    // Everything except the operator's confirmation is checked BEFORE the dialog opens.
    const JcAuthDecision pre = sv.dispatcher.evaluate(command, sv.dispatch_context(true));
    if (pre != JcAuthDecision::Allowed) {
        const bool rearm = pre == JcAuthDecision::BlockedUnprotectedNotAcknowledged
                           || pre == JcAuthDecision::BlockedNotArmed;
        show_message("JOG BLOCKED", jc_auth_name(pre),
                     rearm ? "Tap " JC_ARM_LABEL " for this axis and acknowledge." : "Nothing was sent.",
                     "Nothing was sent.");
        return;
    }
    const JcJogReject why = sv.jogs.can_begin(now);
    if (why != JcJogReject::None) {
        show_message("JOG REFUSED", jc_jog_reject_name(why), "Nothing was sent.");
        return;
    }
    // What the controller does and does not protect. A configured limit INPUT is shown apart
    // from HARD LIMIT PROTECTION, and nothing here claims a safe path or remaining travel.
    const JcMachineCaps& caps = sv.loader.caps();
    const JcAxisCaps& ax = caps.axes[_axis];
    char input[72], protection[48], l1[80], l2[100];
    jc_limit_wording(ax, input, sizeof(input), protection, sizeof(protection));
    snprintf(l1, sizeof(l1), "NO TRUSTED TRAVEL PROTECTION: %s soft %s, hard %s", jc_axis_name(_axis),
             tri_text(ax.soft_limits()), tri_text(ax.hard_limits()));
    snprintf(l2, sizeof(l2), "%s; %s", input, protection);
    const char* notes[3] = { l1, l2, "Check clear space in THIS direction. Position is not trusted." };
    Serial.printf(JC_JOG_TAG ": staged [%s] soft=%s hard=%s | %s | %s\n", command.text, tri_text(ax.soft_limits()),
                  tri_text(ax.hard_limits()), input, protection);
    stage(command, JC_STAGE_TITLE, notes, 3);
}

void JcJogScreen::do_cancel() {
    const JcCommand cancel = jc_jog_cancel();
    _preview.draw(sv.display, jl::kPreviewSlot, "<jog cancel 0x85>", FontId::Body16, CYAN, ui::kPanel);
#if JC_JOG_PREVIEW
    const uint32_t before = sv.dispatcher.previewed();
    const JcAuthDecision decision = submit_now(cancel);   // never needs arming or confirmation
    const bool sent = sv.dispatcher.previewed() != before;   // "accepted by the SIMULATION"; nothing is ever sent
    Serial.printf(JC_JOG_TAG ": cancel requested: %s (%s)\n", jc_auth_name(decision),
                  sent ? "SIMULATED, NOT SENT" : "NOT ACCEPTED");
#else
    const uint32_t before = sv.dispatcher.sent();
    const JcAuthDecision decision = submit_now(cancel);   // never needs arming or confirmation
    const bool sent = sv.dispatcher.sent() != before;
    Serial.printf(JC_JOG_TAG ": cancel requested: %s (%s)\n", jc_auth_name(decision), sent ? "SENT" : "NOT SENT");
#endif
    if (!sent) {
        // "allowed" alone would read as success. Say why nothing left the device.
        show_message("CANCEL NOT SENT",
                     decision == JcAuthDecision::Allowed ? "Authorized, but the connection could not carry it." : jc_auth_name(decision),
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
#if JC_JOG_UI
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
        _preview.draw(sv.display, jl::kPreviewSlot, "(no command)", FontId::Body16, ui::kMuted, ui::kPanel);
        _result.draw(sv.display, jl::kResultSlot, line, FontId::Body16, RED, ui::kPanel);
        submit_now(command);
        return;
    }
    _preview.draw(sv.display, jl::kPreviewSlot, command.text, FontId::Body16, CYAN, ui::kPanel);
    const JcAuthDecision decision = submit_now(command);
    snprintf(line, sizeof(line), "%s - NOT SENT", jc_auth_name(decision));
    _result.draw(sv.display, jl::kResultSlot, line, FontId::Body16, ui::kWarn, ui::kPanel);
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
#if JC_JOG_UI
            ui::button(sv.display, kJogNeg, live_jog_label(_axis, true), live_axis_allowed(_axis) ? ui::Btn::Normal : ui::Btn::Disabled);
            ui::button(sv.display, kJogPos, live_jog_label(_axis, false), live_axis_allowed(_axis) ? ui::Btn::Normal : ui::Btn::Disabled);
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
#if JC_JOG_UI
        {
            const uint32_t now = millis();
            const bool armed_here = sv.dispatcher.armed_at(now) && ((sv.dispatcher.unprotected_ack_mask() >> _axis) & 1u) != 0;
            const JcCapsFlow flow = caps_flow(now);
            if (!armed_here && live_axis_allowed(_axis) && flow.phase != JcCapsPhase::Current) {
                if (flow.phase == JcCapsPhase::Reading) {
                    show_message("READING MACHINE SETTINGS", "Wait for the read to finish.",
                                 "ARM stays unavailable until valid, current results are held.");
                } else if (flow.can_read) {
                    read_settings();
                } else {
                    show_message("CANNOT READ MACHINE SETTINGS", flow.blocker, "The queries are read-only; nothing is changed.");
                }
                return;
            }
        }
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
#if JC_JOG_UI
    if (sv.jogs.needs_acknowledgement() && ui::hit(kAck, x, y)) {
        if (!sv.jogs.acknowledge()) {
            show_message("CANNOT ACKNOWLEDGE YET", "The operation is still in progress.",
                         "Wait for the controller to report Idle, or press CANCEL JOG.");
        } else {
            Serial.println(JC_JOG_TAG ": result acknowledged by the operator");
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
#if JC_JOG_UI
        do_cancel();
#else
        const JcCommand cancel = jc_jog_cancel();
        _preview.draw(sv.display, jl::kPreviewSlot, "<jog cancel 0x85>", FontId::Body16, CYAN, ui::kPanel);
        const JcAuthDecision decision = submit_now(cancel);
        char line[96];
        snprintf(line, sizeof(line), "%s - NOT SENT", jc_auth_name(decision));
        _result.draw(sv.display, jl::kResultSlot, line, FontId::Body16, ui::kWarn, ui::kPanel);
#endif
    }
}

void JcJogScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 250) {
        _last_tick = now;
#if JC_JOG_UI
        service_caps(now);
        const uint32_t key = (sv.jogs.needs_acknowledgement() ? 1u : 0u) | (sv.dispatcher.armed_at(now) ? 2u : 0u)
                             | (static_cast<uint32_t>(caps_flow(now).phase) << 2);
        if (key != _live_key) {
            draw();  // the ACK button or the arming state changed
            return;
        }
        if (sv.dispatcher.armed_at(now)) {
            draw_mode();  // seconds remaining
        } else if (sv.loader.running() && _drawn_step != sv.loader.step()) {
            _drawn_step = sv.loader.step();
            draw_mode();  // read progress
        }
#endif
        draw_live();
    }
}

}  // namespace jc8048w550
