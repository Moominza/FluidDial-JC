// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Homing page (Instruction 009). Homing facts are shown SEPARATELY (configured, switch fitted, switch active,
// homed in this connection) and none of them is a prerequisite for jogging: FluidNC accepts jogs while
// unhomed. An unconfigured or inactive homing input is a configuration fact, not a communication error.
// Z (direction not yet verified) and the A axis (no homing configuration) are not homed silently; "home
// all" includes Z, so it waits for the same verification.

#include "JcHomingState.hpp"
#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {
constexpr ui::Rect kHomeAll { 16, 150, 768, 70 };
ui::Rect axis_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 193), 232, 183, 64 }; }
constexpr ui::Rect kStatusRect { 16, 76, 768, 62 };
constexpr ui::Rect kPreflight { 16, 308, 768, 164 };
int16_t col_x(uint8_t i) { return static_cast<int16_t>(318 + i * 112); }
int16_t row_y(uint8_t r) { return static_cast<int16_t>(336 + r * 22); }

JcHomedTracker g_no_tracker;   // builds without a command system never learn that an axis was homed
}  // namespace

static JcHomingView make_view(JcAppServices& sv, bool z_verified) {
    JcHomingOwnerFacts owner;
    owner.z_homing_verified = z_verified;
    owner.a_homing_present = sv.settings.a_axis_has_homing;
    const JcLiveMachineStatus& st = sv.model.status();
#if JC_AUTHORITY
    const JcHomedTracker& homed = sv.cmd.homed();
#else
    const JcHomedTracker& homed = g_no_tracker;
#endif
    const bool complete = sv.loader.state() == JcLoadState::Complete && sv.loader.caps().complete;
    return jc_homing_view(sv.loader.caps(), complete, st.pins, sv.model.fresh(), sv.model.has_status() ? sv.model.state() : "", homed, owner);
}

void JcHomingScreen::draw_status() {
    char line[96];
    const char* state = sv.model.has_status() ? sv.model.state() : "no data";
    snprintf(line, sizeof(line), "MACHINE %s%s", state, sv.model.homing() ? "  -  HOMING IN PROGRESS" : "");
    _state.draw(sv.display, { 28, 82, 744, 26 }, line, FontId::Button20,
                sv.model.homing() ? ui::kWarn : (sv.model.alarm() ? RED : WHITE), ui::kPanel);

    const JcHomingView v = make_view(sv, _z_verified);
    for (uint8_t a = 0; a < 4; ++a) {
        const JcAxisHoming& h = v.axis[a];
        const JcTri vals[4] = { h.configured, h.switch_present, h.switch_active, h.homed };
        for (uint8_t r = 0; r < 4; ++r) {
            const JcTri t = vals[r];
            const char* w = jc_tri_word(t);
            // "?" = not known (not read yet, or not reported): never shown as NO.
            _cell[a * 4 + r].draw(sv.display, { col_x(a), row_y(r), 96, 20 }, w, FontId::Body16,
                                  t == JcTri::Yes ? ui::kGood : (t == JcTri::No ? WHITE : ui::kMuted), ui::kPanel);
        }
    }
    snprintf(line, sizeof(line), "%s.  %s", v.note, v.must_home == JcTri::Yes ? "Homing is required by the controller." : "Jogging does not need homing.");
    _note.draw(sv.display, { 28, 430, 744, 24 }, line, FontId::Body16, v.accepts_jog ? ui::kGood : ui::kWarn, ui::kPanel);
}

void JcHomingScreen::on_draw() {
    const JcHomingView v = make_view(sv, _z_verified);
    ui::panel(sv.display, kStatusRect);
    ui::text(sv.display, 28, 110, "Homing moves axes toward their limit switches. It is not needed to jog.", FontId::Body16, ui::kMuted, ui::kPanel);
    ui::button(sv.display, kHomeAll, v.may_home_all ? "HOME ALL AXES  ($H)" : "HOME ALL: includes Z - verify Z first",
               v.may_home_all ? ui::Btn::Normal : ui::Btn::Disabled);
    for (uint8_t i = 0; i < 4; ++i) {
        char label[32];
        if (i == 3 && !v.axis[3].may_home) {
            snprintf(label, sizeof(label), "A (no homing)");
        } else if (i == 2 && !v.axis[2].may_home) {
            snprintf(label, sizeof(label), "Z (verify first)");
        } else {
            snprintf(label, sizeof(label), "HOME %s", jc_axis_name(i));
        }
        ui::button(sv.display, axis_rect(i), label, v.axis[i].may_home ? ui::Btn::Normal : ui::Btn::Disabled);
    }
    ui::panel(sv.display, kPreflight);
    ui::text(sv.display, 28, 312, "HOMING STATE (from FluidNC)", FontId::Button20, WHITE, ui::kPanel);
    for (uint8_t a = 0; a < 4; ++a) ui::text(sv.display, col_x(a), 314, jc_axis_name(a), FontId::Button20, CYAN, ui::kPanel);
    static const char* const rows[4] = { "Homing configured", "Home switch fitted", "Switch active now", "Homed this connection" };
    for (uint8_t r = 0; r < 4; ++r) ui::text(sv.display, 28, row_y(r), rows[r], FontId::Body16, ui::kMuted, ui::kPanel);
    for (ui::Slot& s : _cell) s.invalidate();
    _state.invalidate();
    _note.invalidate();
    draw_status();
}

void JcHomingScreen::on_custom_confirm() {
    _z_verified = true;   // this session only; a restart asks again
    Serial.println("homing: the owner confirmed the Z homing direction for this session");
}

void JcHomingScreen::on_touch(uint16_t x, uint16_t y) {
    const JcHomingView v = make_view(sv, _z_verified);
    if (ui::hit(kHomeAll, x, y)) {
        if (!v.may_home_all) {
            show_message("HOME ALL NOT STARTED", v.home_all_why_not, "Home X and Y separately, or verify Z and A first.");
            return;
        }
        const char* notes[] = {
            "Homes ALL configured axes using FluidNC's homing cycle (X, Y and Z here).",
            "FluidNC executes it; the reply arrives when the cycle ends.",
        };
        stage(jc_home(sv.profile, 0xFF), "HOME ALL AXES", notes, 2);
        return;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (!ui::hit(axis_rect(i), x, y)) {
            continue;
        }
        if (i == 2 && !v.axis[2].may_home) {
            open_custom_confirm("Z HOMING DIRECTION", "I HAVE VERIFIED");
            dialog.add_line("The recorded controller configuration homes Z in the positive");
            dialog.add_line("direction. That has NOT been verified on the machine.", ui::kWarn);
            dialog.add_line("Confirm only after you checked the Z home switch side, travel");
            dialog.add_line("and that Z can move up without hitting anything.");
            dialog.add_line("This lasts for this session only.", ui::kMuted);
            dialog.add_line("Keep the E-stop in reach.", RED);
            draw();
            return;
        }
        if (!v.axis[i].may_home) {
            show_message("HOMING NOT AVAILABLE", v.axis[i].why_not, "Nothing was sent.");
            return;
        }
        char title[32];
        snprintf(title, sizeof(title), "HOME %s AXIS", jc_axis_name(i));
        const char* notes[] = { "FluidNC executes the homing cycle for this axis." };
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
