// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Controller capabilities page: what FluidNC itself reports for each axis, read
// with the closed set of read-only queries in JcQuery.hpp. Values that were not
// read, were rejected as implausible, or that the controller refused are shown
// as such; placeholder limits are never presented as controller values.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kRead { 16, 366, 250, 52 };
constexpr ui::Rect kLimits { 276, 366, 250, 52 };
constexpr ui::Rect kDry { 536, 366, 248, 52 };
constexpr int16_t kColX[6] = { 24, 88, 214, 346, 486, 628 };
constexpr int16_t kRowY0 = 98;
constexpr int16_t kRowH = 34;

void format_cell(const JcAxisCaps& a, JcQueryField f, char* out, size_t size, uint16_t& color) {
    const uint8_t i = static_cast<uint8_t>(f);
    color = WHITE;
    switch (a.state[i]) {
        case JcFieldState::Valid:
            if (f == JcQueryField::HomingCycle) {
                if (a.value[i] > 0.0f) {
                    snprintf(out, size, "cycle %g", static_cast<double>(a.value[i]));
                } else {
                    snprintf(out, size, "none");
                    color = ui::kMuted;
                }
            } else {
                snprintf(out, size, "%g", static_cast<double>(a.value[i]));
            }
            break;
        case JcFieldState::BadValue:
            snprintf(out, size, "bad value");
            color = ui::kWarn;
            break;
        case JcFieldState::CtrlError:
            snprintf(out, size, "error:%d", a.error_code[i]);
            color = ui::kWarn;
            break;
        case JcFieldState::NoValue:
            snprintf(out, size, "no reply");
            color = ui::kWarn;
            break;
        default:
            snprintf(out, size, "--");
            color = ui::kMuted;
            break;
    }
}

}  // namespace

void JcCapsScreen::on_enter() {
    _drawn_key = 0xFFFFFFFFu;
}

void JcCapsScreen::draw_values(bool full) {
    const JcMachineCaps& caps = sv.loader.caps();
    if (full) {
        sv.display.fillRect(0, 70, 800, 292, ui::kBg);
        for (auto& row : _cells) {
            for (auto& s : row) s.invalidate();
        }
        for (auto& s : _lines) s.invalidate();
    }

    static const char* const heads[6] = { "AXIS", "STEPS/MM", "TRAVEL", "MAX RATE/MIN", "ACCEL/S2", "HOMING" };
    if (full) {
        for (uint8_t c = 0; c < 6; ++c) {
            ui::text(sv.display, kColX[c], 74, heads[c], FontId::Body16, ui::kMuted, ui::kBg);
        }
    }
    static constexpr JcQueryField order[5] = { JcQueryField::StepsPerMm, JcQueryField::MaxTravelMm,
                                               JcQueryField::MaxRate, JcQueryField::Accel,
                                               JcQueryField::HomingCycle };
    for (uint8_t axis = 0; axis < 4; ++axis) {
        const int16_t y = kRowY0 + axis * kRowH;
        if (full) {
            char label[8];
            snprintf(label, sizeof(label), "%s", jc_axis_name(axis));
            ui::text(sv.display, kColX[0], y, label, FontId::Button20, WHITE, ui::kBg);
        }
        for (uint8_t c = 0; c < 5; ++c) {
            char cell[24];
            uint16_t color = WHITE;
            format_cell(caps.axes[axis], order[c], cell, sizeof(cell), color);
            const int16_t x0 = kColX[c + 1] - 2;
            const int16_t x1 = (c < 4) ? kColX[c + 2] - 4 : 790;
            _cells[axis][c].draw(sv.display, { x0, static_cast<int16_t>(y), static_cast<int16_t>(x1 - x0), 28 },
                                 cell, FontId::Body16, color, ui::kBg);
        }
    }

    char line[96];
    snprintf(line, sizeof(line), "Firmware: %s", caps.firmware[0] ? caps.firmware : "--");
    _lines[0].draw(sv.display, { 22, 240, 760, 24 }, line, FontId::Body16,
                   caps.firmware[0] ? WHITE : ui::kMuted, ui::kBg);
    if (caps.wcs[0]) {
        snprintf(line, sizeof(line), "Active WCS: %s   Tool: %s", caps.wcs,
                 caps.tool >= 0 ? "set" : "--");
        if (caps.tool >= 0) {
            snprintf(line, sizeof(line), "Active WCS: %s   Tool: T%d", caps.wcs, caps.tool);
        }
    } else {
        snprintf(line, sizeof(line), "Active WCS: --   Tool: --");
    }
    _lines[1].draw(sv.display, { 22, 264, 760, 24 }, line, FontId::Body16,
                   caps.wcs[0] ? WHITE : ui::kMuted, ui::kBg);
    snprintf(line, sizeof(line), "Probe pin: %s   Toolsetter pin: %s",
             caps.probe_pin[0] ? caps.probe_pin : "--", caps.toolsetter_pin[0] ? caps.toolsetter_pin : "--");
    _lines[2].draw(sv.display, { 22, 288, 760, 24 }, line, FontId::Body16, ui::kMuted, ui::kBg);

    uint16_t status_color = ui::kMuted;
    switch (sv.loader.state()) {
        case JcLoadState::Idle:
            snprintf(line, sizeof(line), "Not read yet. Limits below are PLACEHOLDERS.");
            break;
        case JcLoadState::Running:
            snprintf(line, sizeof(line), "Reading %u of %u ...", static_cast<unsigned>(sv.loader.step()),
                     static_cast<unsigned>(JcCapsLoader::kSteps));
            status_color = ui::kInfo;
            break;
        case JcLoadState::Complete:
            snprintf(line, sizeof(line), "Read complete: %u answered, %u refused, %lu ms.",
                     static_cast<unsigned>(caps.queries_ok), static_cast<unsigned>(caps.queries_error),
                     static_cast<unsigned long>(caps.read_ms));
            status_color = ui::kGood;
            break;
        case JcLoadState::Failed:
            snprintf(line, sizeof(line), "Read FAILED: %s", jc_load_failure_name(sv.loader.failure()));
            status_color = ui::kWarn;
            break;
    }
    _lines[3].draw(sv.display, { 22, 316, 760, 24 }, line, FontId::Body16, status_color, ui::kBg);

    const uint8_t mask = jc_profile_limits_mask(sv.profile);
    if (mask == 0x0F) {
        snprintf(line, sizeof(line), "Motion limits: confirmed by the controller for X Y Z A.");
    } else {
        char names[8] = "";
        for (uint8_t i = 0; i < 4; ++i) {
            if (((mask >> i) & 1u) == 0) {
                snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s", jc_axis_name(i));
            }
        }
        snprintf(line, sizeof(line), "Motion limits still PLACEHOLDER for: %s", names);
    }
    _lines[4].draw(sv.display, { 22, 340, 760, 24 }, line, FontId::Body16,
                   mask == 0x0F ? ui::kGood : ui::kWarn, ui::kBg);
}

void JcCapsScreen::on_draw() {
    draw_values(true);
    const bool running = sv.loader.running();
    ui::button(sv.display, kRead, running ? "CANCEL READ" : "READ CONTROLLER",
               running ? ui::Btn::Danger : ui::Btn::Normal);
    ui::button(sv.display, kLimits, "LIMITS / HOMING");
    ui::button(sv.display, kDry, "DRY RUN");
    ui::wrap(sv.display, { 16, 428, 768, 48 },
             "Read-only: nothing is changed on the controller. Values are discarded if the connection "
             "is lost. A's rotary/degrees cannot be read; it is set in Settings. Limits: LIMITS / HOMING.",
             FontId::Body16, ui::kMuted, ui::kBg);
    _drawn_key = (static_cast<uint32_t>(sv.loader.state()) << 24) | (static_cast<uint32_t>(sv.loader.step()) << 8)
                 | jc_profile_limits_mask(sv.profile);
}

void JcCapsScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kRead, x, y)) {
        if (sv.loader.running()) {
            sv.host.cancel_caps_read();
            draw();
            return;
        }
        const char* blocker = sv.host.start_caps_read();
        if (blocker != nullptr) {
            show_message("CANNOT READ CONFIGURATION", blocker, "The queries are read-only; nothing is changed.");
            return;
        }
        draw();
        return;
    }
    if (ui::hit(kLimits, x, y)) {
        sv.host.go(JcPage::LimitsHoming);
        return;
    }
    if (ui::hit(kDry, x, y)) {
        sv.host.go(JcPage::DryRun);
    }
}

void JcCapsScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 250) {
        return;
    }
    _last_tick = now;
    const uint32_t key = (static_cast<uint32_t>(sv.loader.state()) << 24)
                         | (static_cast<uint32_t>(sv.loader.step()) << 8) | jc_profile_limits_mask(sv.profile);
    if (key == _drawn_key) {
        return;
    }
    if ((key >> 24) != (_drawn_key >> 24)) {
        draw();  // loader state changed: the READ / CANCEL button changes too
        return;
    }
    draw_values(false);  // progress or limits changed: repaint only the items that differ
    _drawn_key = key;
}

}  // namespace jc8048w550
