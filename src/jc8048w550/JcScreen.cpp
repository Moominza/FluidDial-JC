// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreen.hpp"

namespace jc8048w550 {

const char* jc_cmd_error_name(JcCmdError e) {
    switch (e) {
        case JcCmdError::None: return "no error";
        case JcCmdError::BadAxis: return "axis not available";
        case JcCmdError::BadValue: return "value out of range";
        case JcCmdError::BadFeed: return "feed rate out of range";
        case JcCmdError::Unsupported: return "not supported for this axis / configuration";
        case JcCmdError::BadPath: return "invalid file path";
        case JcCmdError::BadIndex: return "invalid index";
        case JcCmdError::JogDisabled: return "jogging is disabled for this axis";
        case JcCmdError::TestLimit: return "larger than the live-jog test limit";
    }
    return "invalid";
}

void JcScreen::enter() {
    dialog.close();
    _has_pending = false;
    _custom_pending = false;
    on_enter();
    draw();
}

void JcScreen::open_custom_confirm(const char* title, const char* confirm_label) {
    dialog.open_confirm(title, confirm_label);
    _custom_pending = true;
    _has_pending = false;
}

void JcScreen::draw() {
    sv.display.fillScreen(ui::kBg);
    const bool fresh = sv.live_fresh();
    const char* mode = sv.live_mode ? (fresh ? "LIVE" : "LIVE STALE") : "DEMO MODE";
    ui::header(sv.display, jc_page_title(page()), mode, fresh);
#if JC_JOG_UI
    ui::draw_live_jog_banner(sv.display, jc_page_title(page()));  // on every page of the experimental build
#endif
    on_draw();
    if (dialog.is_open()) {
        dialog.draw(sv.display);
    }
}

void JcScreen::touch(uint16_t x, uint16_t y) {
    if (dialog.is_open()) {
        switch (dialog.touch(x, y)) {
            case ui::Dialog::Action::Confirm:
                if (_custom_pending) {
                    _custom_pending = false;
                    dialog.close();
                    on_custom_confirm();
                    if (!dialog.is_open()) {
                        draw();
                    }
                } else {
                    run_pending();
                }
                break;
            case ui::Dialog::Action::Cancel:
            case ui::Dialog::Action::Dismiss:
                dialog.close();
                _has_pending = false;
                _custom_pending = false;
                draw();
                break;
            case ui::Dialog::Action::None:
                break;
        }
        return;
    }
    if (ui::hit(ui::kBackButton, x, y)) {
        sv.host.back();
        return;
    }
    on_touch(x, y);
}

void JcScreen::tick(uint32_t now) {
    if (!dialog.is_open()) {
        on_tick(now);
    }
}

void JcScreen::open_confirm_dialog(const char* title, const char* const* notes, uint8_t note_count) {
    dialog.open_confirm(title, "CONFIRM");
    for (uint8_t i = 0; i < note_count; ++i) {
        dialog.add_line(notes[i]);
    }
    if (_pending_is_sequence) {
        char line[80];
        for (uint8_t i = 0; i < _sequence.count && i < 4; ++i) {
            snprintf(line, sizeof(line), "%u. %s", static_cast<unsigned>(i + 1), _sequence.steps[i].text);
            dialog.add_line(line, CYAN);
        }
    } else {
        char line[80];
        if (_command.realtime) {
            snprintf(line, sizeof(line), "Real-time byte 0x%02X", static_cast<unsigned>(static_cast<uint8_t>(_command.text[0])));
        } else {
            snprintf(line, sizeof(line), "%s", _command.text);
        }
        dialog.add_line(line, CYAN);
    }
#if JC_JOG_PREVIEW
    if (!_pending_is_sequence && _command.cls == JcCommandClass::Jog) {
        dialog.add_line("PREVIEW ONLY - MOTION DISABLED: confirming runs", RED);
        dialog.add_line("a SIMULATED jog. Nothing is sent to the machine.", ui::kWarn);
    } else {
        dialog.add_line("PREVIEW ONLY - MOTION DISABLED: the command is", RED);
        dialog.add_line("rehearsed on a SIMULATED controller, never sent.", ui::kWarn);
    }
#elif JC_FULL_CONTROL_TEST
    dialog.add_line("FULL CONTROL: confirming SENDS this command to", ui::kWarn);
    dialog.add_line("the controller. E-stop in reach; travel clear.", ui::kWarn);
#elif JC_JOG_UI
    if (!_pending_is_sequence && _command.cls == JcCommandClass::Jog) {
        dialog.add_line("LIVE JOG TEST BUILD: confirming SENDS this jog", ui::kWarn);
        dialog.add_line("to the controller. Check the travel is clear.", ui::kWarn);
    } else {
        dialog.add_line("Blocked in the live-jog test build:", ui::kWarn);
        dialog.add_line("only a restricted jog is ever sent.", ui::kWarn);
    }
#else
    dialog.add_line("Machine control is DISABLED in this build:", ui::kWarn);
    dialog.add_line("confirming records a preview only; nothing is sent.", ui::kWarn);
#endif
    draw();
}

void JcScreen::stage(const JcCommand& command, const char* title, const char* const* notes,
                     uint8_t note_count) {
    if (!command.ok) {
        show_message("CANNOT PREPARE COMMAND", jc_cmd_error_name(command.error));
        sv.dispatcher.submit(command, sv.dispatch_context(false));  // recorded as rejected
        return;
    }
    _command = command;
    _pending_is_sequence = false;
    _has_pending = true;
    open_confirm_dialog(title, notes, note_count);
}

void JcScreen::stage_sequence(const JcCommandSequence& sequence, const char* title,
                              const char* const* notes, uint8_t note_count) {
    if (!sequence.ok) {
        show_message("CANNOT PREPARE SEQUENCE", jc_cmd_error_name(sequence.error));
        return;
    }
    _sequence = sequence;
    _pending_is_sequence = true;
    _has_pending = true;
    open_confirm_dialog(title, notes, note_count);
}

void JcScreen::run_pending() {
    if (!_has_pending) {
        dialog.close();
        draw();
        return;
    }
    const JcDispatchContext ctx = sv.dispatch_context(true);
    const uint32_t sent_before = sv.dispatcher.sent();
#if JC_JOG_PREVIEW
    const uint32_t preview_before = sv.dispatcher.previewed();
#endif
    const JcAuthDecision decision = _pending_is_sequence
        ? sv.dispatcher.submit_sequence(_sequence, ctx)
        : sv.dispatcher.submit(_command, ctx);
    _has_pending = false;
    const bool allowed = decision == JcAuthDecision::Allowed;
#if JC_JOG_PREVIEW
    // Accepted by the SIMULATED controller is counted as previewed; sent() cannot move in this build.
    const bool rehearsed = sv.dispatcher.previewed() != preview_before;
    (void)sent_before;
    dialog.open_result(rehearsed ? "SIMULATED - NOTHING SENT" : (allowed ? "AUTHORIZED - NOT SIMULATED" : "BLOCKED - NOTHING SENT"), rehearsed);
    dialog.add_line(jc_auth_name(decision), allowed ? ui::kGood : ui::kWarn);
    dialog.add_line("PREVIEW ONLY - MOTION DISABLED.", RED);
    dialog.add_line("Nothing was transmitted to the controller.");
    if (rehearsed) {
        dialog.add_line("Any reply shown later comes from the simulation.", ui::kMuted);
    }
    Serial.printf("dispatch: %s%s\n", jc_auth_name(decision), rehearsed ? " (SIMULATED, NOT SENT)" : " (NOT SENT)");
#elif JC_JOG_UI
    const bool sent = sv.dispatcher.sent() != sent_before;
    dialog.open_result(sent ? "JOG SENT" : (allowed ? "AUTHORIZED - NOT SENT" : "BLOCKED - NOT SENT"), sent);
    dialog.add_line(jc_auth_name(decision), allowed ? ui::kGood : ui::kWarn);
    if (sent) {
        dialog.add_line("Watch the machine. CANCEL JOG sends the cancel byte.", ui::kWarn);
        dialog.add_line("The software cancel is NOT an emergency stop.", ui::kWarn);
    } else {
        dialog.add_line("Nothing was transmitted to the controller.");
    }
    Serial.printf("dispatch: %s%s\n", jc_auth_name(decision), sent ? " (SENT)" : "");
#else
    (void)sent_before;
    dialog.open_result(allowed ? "AUTHORIZED - NO TRANSMIT PATH" : "BLOCKED - NOT SENT", allowed);
    dialog.add_line(jc_auth_name(decision), allowed ? ui::kGood : ui::kWarn);
    dialog.add_line("Nothing was transmitted to the controller.");
    dialog.add_line("Machine control stays disabled until the owner approves a", ui::kMuted);
    dialog.add_line("separate safety review (see docs/development).", ui::kMuted);
    Serial.printf("dispatch: %s\n", jc_auth_name(decision));
#endif
    draw();
}

JcAuthDecision JcScreen::submit_now(const JcCommand& command) {
    return sv.dispatcher.submit(command, sv.dispatch_context(false));
}

void JcScreen::show_message(const char* title, const char* line1, const char* line2,
                            const char* line3) {
    dialog.open_result(title, false);
    if (line1) dialog.add_line(line1);
    if (line2) dialog.add_line(line2);
    if (line3) dialog.add_line(line3);
    draw();
}

}  // namespace jc8048w550
