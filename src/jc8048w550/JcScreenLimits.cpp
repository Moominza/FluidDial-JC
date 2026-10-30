// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Soft-limit, hard-limit, limit-pin and homing settings exactly as the controller
// reports them (read-only queries, JcQuery.hpp). "UNKNOWN" is shown for anything
// that was not read, was refused or was unreadable; it is never shown as OFF. A
// configured travel distance is not a proof of a safe movement envelope.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kRead { 16, 366, 360, 52 };
constexpr ui::Rect kBackToCaps { 400, 366, 384, 52 };
constexpr int16_t kColX[6] = { 24, 90, 180, 270, 570, 690 };
constexpr int16_t kRowY0 = 98;
constexpr int16_t kRowH = 34;

void format_bool(const JcAxisCaps& a, JcQueryField f, char* out, size_t size, uint16_t& color) {
    const uint8_t i = static_cast<uint8_t>(f);
    color = WHITE;
    switch (a.state[i]) {
        case JcFieldState::Valid:
            snprintf(out, size, "%s", a.get(f) != 0.0f ? "ON" : "OFF");
            break;
        case JcFieldState::CtrlError:
            snprintf(out, size, "error:%d", a.error_code[i]);
            color = ui::kWarn;
            break;
        case JcFieldState::Unqueried:
            snprintf(out, size, "--");
            color = ui::kMuted;
            break;
        default:
            snprintf(out, size, "UNKNOWN");
            color = ui::kWarn;
            break;
    }
}

// "-" for an unassigned pin, otherwise the pin text without the "gpio." prefix.
void pin_token(const JcAxisCaps& a, uint8_t slot, char* out, size_t size) {
    const JcQueryField f = static_cast<JcQueryField>(static_cast<uint8_t>(JcQueryField::LimitNegPin) + slot);
    const uint8_t i = static_cast<uint8_t>(f);
    if (a.state[i] == JcFieldState::Valid) {
        if (a.get(f) == 0.0f) {
            snprintf(out, size, "-");
        } else {
            const char* p = a.pin[slot];
            if (strncmp(p, "gpio.", 5) == 0) p += 5;
            snprintf(out, size, "%s", p);
        }
    } else {
        snprintf(out, size, "?");
    }
}

void format_pins(const JcAxisCaps& a, char* out, size_t size, uint16_t& color) {
    bool any = false;
    for (uint8_t s = 0; s < 3; ++s) {
        any = any || a.state[static_cast<uint8_t>(JcQueryField::LimitNegPin) + s] != JcFieldState::Unqueried;
    }
    if (!any) {
        snprintf(out, size, "--");
        color = ui::kMuted;
        return;
    }
    char t[3][kJcPinTextSize];
    for (uint8_t s = 0; s < 3; ++s) pin_token(a, s, t[s], sizeof(t[s]));
    snprintf(out, size, "%s / %s / %s", t[0], t[1], t[2]);
    color = WHITE;
}

void format_homing(const JcAxisCaps& a, char* out, size_t size, uint16_t& color) {
    const uint8_t i = static_cast<uint8_t>(JcQueryField::HomingCycle);
    color = WHITE;
    switch (a.state[i]) {
        case JcFieldState::Valid:
            if (a.get(JcQueryField::HomingCycle) > 0.0f) {
                snprintf(out, size, "cycle %g", static_cast<double>(a.get(JcQueryField::HomingCycle)));
            } else {
                snprintf(out, size, "none");
                color = ui::kMuted;
            }
            break;
        case JcFieldState::CtrlError:
            snprintf(out, size, "error:%d", a.error_code[i]);
            color = ui::kWarn;
            break;
        case JcFieldState::Unqueried:
            snprintf(out, size, "--");
            color = ui::kMuted;
            break;
        default:
            snprintf(out, size, "UNKNOWN");
            color = ui::kWarn;
            break;
    }
}

void format_mpos(const JcAxisCaps& a, char* out, size_t size, uint16_t& color) {
    const uint8_t i = static_cast<uint8_t>(JcQueryField::HomingMpos);
    color = WHITE;
    switch (a.state[i]) {
        case JcFieldState::Valid: snprintf(out, size, "%g", static_cast<double>(a.get(JcQueryField::HomingMpos))); break;
        case JcFieldState::CtrlError:
            snprintf(out, size, "error:%d", a.error_code[i]);
            color = ui::kWarn;
            break;
        case JcFieldState::Unqueried:
            snprintf(out, size, "--");
            color = ui::kMuted;
            break;
        default:
            snprintf(out, size, "UNKNOWN");
            color = ui::kWarn;
            break;
    }
}

const char* tri_name(JcTri t) { return t == JcTri::Yes ? "YES" : (t == JcTri::No ? "NO" : "UNKNOWN"); }

}  // namespace

void JcLimitsScreen::on_enter() {
    _drawn_key = 0xFFFFFFFFu;
}

void JcLimitsScreen::draw_values(bool full) {
    const JcMachineCaps& caps = sv.loader.caps();
    if (full) {
        sv.display.fillRect(0, 70, 800, 292, ui::kBg);
        for (auto& row : _cells) {
            for (auto& s : row) s.invalidate();
        }
        for (auto& s : _lines) s.invalidate();
        static const char* const heads[6] = { "AXIS", "SOFT", "HARD", "LIMIT INPUT  - / + / ALL", "HOMING", "HOME MPOS" };
        for (uint8_t c = 0; c < 6; ++c) {
            ui::text(sv.display, kColX[c], 74, heads[c], FontId::Body16, ui::kMuted, ui::kBg);
        }
    }
    for (uint8_t axis = 0; axis < 4; ++axis) {
        const int16_t y = kRowY0 + axis * kRowH;
        if (full) {
            ui::text(sv.display, kColX[0], y, jc_axis_name(axis), FontId::Button20, WHITE, ui::kBg);
        }
        for (uint8_t c = 0; c < 5; ++c) {
            char cell[40];
            uint16_t color = WHITE;
            const JcAxisCaps& a = caps.axes[axis];
            switch (c) {
                case 0: format_bool(a, JcQueryField::SoftLimits, cell, sizeof(cell), color); break;
                case 1: format_bool(a, JcQueryField::HardLimits, cell, sizeof(cell), color); break;
                case 2: format_pins(a, cell, sizeof(cell), color); break;
                case 3: format_homing(a, cell, sizeof(cell), color); break;
                default: format_mpos(a, cell, sizeof(cell), color); break;
            }
            const int16_t x0 = kColX[c + 1] - 2;
            const int16_t x1 = c < 4 ? kColX[c + 2] - 4 : 792;
            _cells[axis][c].draw(sv.display, { x0, y, static_cast<int16_t>(x1 - x0), 28 }, cell, FontId::Body16, color,
                                 ui::kBg);
        }
    }

    char line[100];
    snprintf(line, sizeof(line), "Start must_home: %s   (the controller refuses motion until homed when YES)",
             tri_name(caps.must_home_tri()));
    _lines[0].draw(sv.display, { 22, 238, 760, 22 }, line, FontId::Body16,
                   caps.must_home_state == JcFieldState::Unqueried ? ui::kMuted : WHITE, ui::kBg);
    // The two facts are never merged: is a limit INPUT configured, and is HARD LIMIT PROTECTION on.
    char groups[2][96];
    const uint8_t n = jc_limits_summary(caps, groups, 2);
    for (uint8_t g = 0; g < 2; ++g) {
        _lines[1 + g].draw(sv.display, { 22, static_cast<int16_t>(260 + g * 22), 760, 22 }, g < n ? groups[g] : "",
                           FontId::Body16, WHITE, ui::kBg);
    }
    _lines[3].draw(sv.display, { 22, 304, 760, 22 },
                   "A limit INPUT is not protection unless HARD is ON. Soft limits need a valid position.",
                   FontId::Body16, ui::kWarn, ui::kBg);

    uint16_t status_color = ui::kMuted;
    switch (sv.loader.state()) {
        case JcLoadState::Idle:
            snprintf(line, sizeof(line), "Not read yet. Tap READ CONTROLLER.");
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
    _lines[4].draw(sv.display, { 22, 330, 760, 22 }, line, FontId::Body16, status_color, ui::kBg);
}

void JcLimitsScreen::on_draw() {
    draw_values(true);
    const bool running = sv.loader.running();
    ui::button(sv.display, kRead, running ? "CANCEL READ" : "READ CONTROLLER", running ? ui::Btn::Danger : ui::Btn::Normal);
    ui::button(sv.display, kBackToCaps, "CAPABILITIES");
    ui::wrap(sv.display, { 16, 428, 768, 48 },
             "Read-only queries: nothing is changed on the controller. UNKNOWN means not read, refused or unreadable; "
             "it never means OFF. Values are discarded if the connection is lost.",
             FontId::Body16, ui::kMuted, ui::kBg);
    _drawn_key = (static_cast<uint32_t>(sv.loader.state()) << 24) | (static_cast<uint32_t>(sv.loader.step()) << 8);
}

void JcLimitsScreen::on_touch(uint16_t x, uint16_t y) {
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
    if (ui::hit(kBackToCaps, x, y)) {
        sv.host.back();
    }
}

void JcLimitsScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 250) {
        return;
    }
    _last_tick = now;
    const uint32_t key = (static_cast<uint32_t>(sv.loader.state()) << 24) | (static_cast<uint32_t>(sv.loader.step()) << 8);
    if (key == _drawn_key) {
        return;
    }
    if ((key >> 24) != (_drawn_key >> 24)) {
        draw();  // loader state changed: the READ / CANCEL button changes too
        return;
    }
    draw_values(false);
    _drawn_key = key;
}

}  // namespace jc8048w550
