// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Diagnostic dry-run page. Picking an action shows the FluidNC command that
// would be generated, the decision the real authorization gate would make right
// now, the reply that would be expected and how timeout, error and loss of
// connection are handled. It calls JcDispatcher::evaluate() only: nothing is
// recorded, counted or sent.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr const char* kActionLabels[8] = {
    "JOG X +10 mm", "JOG A +45 deg", "HOME ALL ($H)", "ZERO X (G10 L20)",
    "PROBE Z (1st step)", "UNLOCK ($X)", "JOG CANCEL (0x85)", "UNLOCK AGAIN ($X)",
};
constexpr const char* kActionNames[8] = {
    "Jog X by +10 mm at F500", "Jog A by +45 deg at F1000", "Home all axes", "Set X work zero",
    "Z probe: first move", "Unlock after alarm", "Cancel a jog", "Unlock after alarm (repeat)",
};

ui::Rect action_rect(uint8_t i) {
    return { 16, static_cast<int16_t>(76 + i * 48), 262, 42 };
}

constexpr int16_t kLabelX = 296;
constexpr int16_t kValueX = 420;
constexpr int16_t kRowY0 = 76;
constexpr int16_t kRowH = 48;

}  // namespace

void JcDryRunScreen::on_enter() {
    build_report(_selected);
}

void JcDryRunScreen::build_report(uint8_t action) {
    const JcMachineProfile& p = sv.profile;
    JcCommand command;
    switch (action) {
        case 0: command = jc_jog_incremental(p, 0, 10.0f, 500.0f); break;
        case 1: command = jc_jog_incremental(p, 3, 45.0f, 1000.0f); break;
        case 2: command = jc_home(p, 0xFF); break;
        case 3: command = jc_zero_axis(p, 0); break;
        case 4: {
            const JcCommandSequence seq = jc_probe_z_sequence(p, 20.0f, 50.0f, 5.0f, 0.0f);
            command = seq.ok ? seq.steps[0] : JcCommand();
            if (!seq.ok) {
                command.error = seq.error;
            }
            break;
        }
        case 5: command = jc_unlock(); break;
        case 6: command = jc_jog_cancel(); break;
        default: command = jc_unlock(); break;
    }
    _report = jc_dry_run(sv.dispatcher, kActionNames[action], command, sv.dispatch_context(true),
                         command.ok ? "" : jc_cmd_error_name(command.error));
}

void JcDryRunScreen::draw_actions() {
    for (uint8_t i = 0; i < kActions; ++i) {
        ui::button(sv.display, action_rect(i), kActionLabels[i],
                   i == _selected ? ui::Btn::Selected : ui::Btn::Normal);
    }
}

void JcDryRunScreen::draw_report() {
    sv.display.fillRect(290, 70, 510, 410, ui::kBg);
    struct Row {
        const char* label;
        char        text[110];
        uint16_t    color;
    };
    Row rows[7] = {};
    static const char* const labels[7] = { "ACTION", "COMMAND", "AUTHORIZATION", "EXPECTED REPLY",
                                           "ON TIMEOUT", "ON ERROR", "ON LINK LOSS" };
    for (uint8_t i = 0; i < 7; ++i) {
        rows[i].label = labels[i];
        rows[i].color = WHITE;
    }
    rows[1].color = ui::kInfo;
    rows[2].color = ui::kWarn;
    snprintf(rows[0].text, sizeof(rows[0].text), "%s", _report.action);
    if (_report.built) {
        snprintf(rows[1].text, sizeof(rows[1].text), "%s", _report.command);
        snprintf(rows[2].text, sizeof(rows[2].text), "%s%s", jc_auth_name(_report.decision),
                 _report.would_transmit ? " (still not sent: dry run)" : "");
        rows[2].color = _report.would_transmit ? ui::kGood : ui::kWarn;
        snprintf(rows[3].text, sizeof(rows[3].text), "%s", _report.expected);
        snprintf(rows[4].text, sizeof(rows[4].text), "%s", _report.on_timeout);
        snprintf(rows[5].text, sizeof(rows[5].text), "%s", _report.on_error);
        snprintf(rows[6].text, sizeof(rows[6].text), "%s", _report.on_link_loss);
    } else {
        snprintf(rows[1].text, sizeof(rows[1].text), "not generated: %s", _report.rejected_reason);
        rows[1].color = ui::kWarn;
        snprintf(rows[2].text, sizeof(rows[2].text), "%s", jc_auth_name(_report.decision));
        snprintf(rows[3].text, sizeof(rows[3].text), "%s", _report.expected);
    }
    for (uint8_t i = 0; i < 7; ++i) {
        const int16_t y = kRowY0 + i * kRowH;
        ui::text(sv.display, kLabelX, y, rows[i].label, FontId::Body16, ui::kMuted, ui::kBg);
        if (rows[i].text[0] != '\0') {
            ui::wrap(sv.display, { kValueX, y, 364, kRowH - 4 }, rows[i].text, FontId::Body16, rows[i].color,
                     ui::kBg);
        }
    }
    ui::text(sv.display, kLabelX, 424, "Dry run: evaluated only. Nothing is recorded or sent.", FontId::Body16,
             ui::kMuted, ui::kBg);
}

void JcDryRunScreen::on_draw() {
    draw_actions();
    draw_report();
}

void JcDryRunScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < kActions; ++i) {
        if (ui::hit(action_rect(i), x, y)) {
            _selected = i;
            build_report(i);
            draw_actions();
            draw_report();
            return;
        }
    }
}

}  // namespace jc8048w550
