// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// JOG PREVIEW TEST page (Instruction 007). The jog page, arming, acknowledgement, confirmation, result
// and cancel flows run the real rendering, touch, gate and session code against a SIMULATED controller.
// This page chooses how that simulation behaves and shows what was counted. Nothing here can transmit.

#include "JcScreens.hpp"

#if JC_JOG_PREVIEW

namespace jc8048w550 {

namespace {

constexpr ui::Rect kNext { 560, 188, 224, 52 };
constexpr ui::Rect kReset { 400, 372, 384, 52 };
constexpr ui::Rect kNote { 16, 112, 768, 64 };

}  // namespace

void JcJogPreviewScreen::on_enter() {
    _scenario.invalidate();
    for (ui::Slot& s : _lines) s.invalidate();
}

void JcJogPreviewScreen::on_draw() {
    ui::text(sv.display, 16, 78, "PREVIEW ONLY - MOTION DISABLED", FontId::Heading24, RED, ui::kBg);
    ui::wrap(sv.display, kNote,
             "The jog page, arming, confirmations and cancel run the real UI and gate against a SIMULATED "
             "controller. Nothing is sent to the machine. Status and capability reads still use the real controller.",
             FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 16, 204, "SIMULATED CONTROLLER", FontId::Body16, ui::kMuted, ui::kBg);
    ui::button(sv.display, kNext, "NEXT SCENARIO", ui::Btn::Normal);
    ui::panel(sv.display, { 16, 248, 768, 116 });
    ui::button(sv.display, kReset, "RESET SIMULATION", ui::Btn::Normal);
    ui::text(sv.display, 16, 388, "Reset clears the simulated position offset.", FontId::Body16, ui::kMuted, ui::kBg);
    draw_values();
}

void JcJogPreviewScreen::draw_values() {
    const JcPreviewController& c = sv.preview.controller();
    _scenario.draw(sv.display, { 236, 196, 316, 36 }, jc_preview_scenario_name(c.scenario()), FontId::Button20, WHITE, ui::kBg);
    char line[96];
    snprintf(line, sizeof(line), "Gate: previewed %lu   SENT %lu   blocked %lu",
             static_cast<unsigned long>(sv.dispatcher.previewed()), static_cast<unsigned long>(sv.dispatcher.sent()),
             static_cast<unsigned long>(sv.dispatcher.blocked()));
    _lines[0].draw(sv.display, { 28, 254, 744, 24 }, line, FontId::Body16, WHITE, ui::kPanel);
    snprintf(line, sizeof(line), "Simulation: jog lines %lu   cancels %lu   refused %lu",
             static_cast<unsigned long>(c.lines()), static_cast<unsigned long>(c.cancels()),
             static_cast<unsigned long>(c.refused()));
    _lines[1].draw(sv.display, { 28, 280, 744, 24 }, line, FontId::Body16, WHITE, ui::kPanel);
    snprintf(line, sizeof(line), "Last simulated line: %s", c.last_line()[0] != '\0' ? c.last_line() : "(none)");
    _lines[2].draw(sv.display, { 28, 306, 744, 24 }, line, FontId::Body16, CYAN, ui::kPanel);
    snprintf(line, sizeof(line), "Armed: %s   Jog: %s", sv.dispatcher.armed_at(millis()) ? "YES" : "no",
             jc_jog_phase_short(sv.jogs.phase()));
    _lines[3].draw(sv.display, { 28, 332, 744, 24 }, line, FontId::Body16, ui::kMuted, ui::kPanel);
}

void JcJogPreviewScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kNext, x, y)) {
        sv.preview.controller().next_scenario();
        Serial.printf("jogpreview: simulated controller scenario = %s\n",
                      jc_preview_scenario_name(sv.preview.controller().scenario()));
        draw_values();
    } else if (ui::hit(kReset, x, y)) {
        if (sv.jogs.unresolved()) {
            show_message("CANNOT RESET YET", "A simulated jog is still unresolved.",
                         "Wait for it to finish, or press CANCEL JOG on the jog page.");
            return;
        }
        sv.preview.controller().reset();
        sv.preview.controller().reset_displacement();
        Serial.println("jogpreview: simulation reset");
        draw_values();
    }
}

void JcJogPreviewScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 250) {
        _last_tick = now;
        draw_values();
    }
}

}  // namespace jc8048w550

#endif  // JC_JOG_PREVIEW
