// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

#include <stdlib.h>

namespace jc8048w550 {

namespace {

ui::Rect wcs_rect(uint8_t i) { return { static_cast<int16_t>(176 + i * 101), 76, 94, 42 }; }
int16_t row_y(uint8_t i) { return static_cast<int16_t>(172 + i * 56); }
ui::Rect machine_rect(uint8_t i) { return { 120, row_y(i) + 6, 190, 36 }; }
ui::Rect work_rect(uint8_t i) { return { 320, row_y(i) + 6, 190, 36 }; }
ui::Rect zero_rect(uint8_t i) { return { 524, row_y(i), 120, 48 }; }
ui::Rect set_rect(uint8_t i) { return { 654, row_y(i), 130, 48 }; }

void edit_callback(void* user, bool ok, const char* text) {
    static_cast<JcOffsetsScreen*>(user)->edit_done(ok, text);
}

}  // namespace

void JcOffsetsScreen::on_draw() {
    ui::text(sv.display, 16, 86, "TARGET WCS", FontId::Button20, ui::kMuted, ui::kBg);
    for (uint8_t i = 0; i < 6; ++i) {
        char label[8];
        snprintf(label, sizeof(label), "G%u", 54u + i);
        ui::button(sv.display, wcs_rect(i), label);
    }
    ui::text(sv.display, 16, 124, "Active WCS: unknown (reading it needs a $G query; this build sends only '?').",
             FontId::Body16, ui::kWarn, ui::kBg);
    ui::text(sv.display, 24, 150, "AXIS", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 120, 150, "MACHINE", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 320, 150, "WORK", FontId::Body16, ui::kMuted, ui::kBg);
    for (uint8_t i = 0; i < 4; ++i) {
        ui::panel(sv.display, { 16, row_y(i), 768, 48 });
        char name[16];
        snprintf(name, sizeof(name), "%s %s", jc_axis_name(i), jc_axis_unit(i));
        ui::text(sv.display, 26, row_y(i) + 12, name, FontId::Button20, WHITE, ui::kPanel);
        ui::button(sv.display, zero_rect(i), "ZERO");
        ui::button(sv.display, set_rect(i), "SET...");
        _machine[i].invalidate();
        _work[i].invalidate();
    }
    _wco.invalidate();
    ui::text(sv.display, 16, 404, "Unknown offsets are shown as '--' and never assumed to be 0.",
             FontId::Body16, ui::kMuted, ui::kBg);
    draw_values();
}

void JcOffsetsScreen::draw_values() {
    const uint16_t color = sv.model.fresh() ? WHITE : ui::kMuted;
    for (uint8_t i = 0; i < 4; ++i) {
        char v[32];
        jc_format_position(sv, i, JcRef::Machine, v, sizeof(v));
        _machine[i].draw(sv.display, machine_rect(i), v, FontId::Dro20, color, ui::kPanel);
        jc_format_position(sv, i, JcRef::Work, v, sizeof(v));
        _work[i].draw(sv.display, work_rect(i), v, FontId::Dro20, color, ui::kPanel);
    }
    char line[96];
    if (sv.model.wco_known()) {
        float w[4];
        for (uint8_t i = 0; i < 4; ++i) {
            sv.model.wco(i, w[i]);
        }
        snprintf(line, sizeof(line), "WCO  X %.3f  Y %.3f  Z %.3f  A %.3f", static_cast<double>(w[0]),
                 static_cast<double>(w[1]), static_cast<double>(w[2]), static_cast<double>(w[3]));
    } else {
        snprintf(line, sizeof(line), "WCO: not yet reported by the controller");
    }
    _wco.draw(sv.display, { 16, 430, 768, 30 }, line, FontId::Body16, sv.model.wco_known() ? WHITE : ui::kMuted, ui::kBg);
}

void JcOffsetsScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    char* end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || *end != '\0') {
        show_message("INVALID VALUE", "Enter a number such as 12.5 or -3.");
        return;
    }
    char title[40];
    snprintf(title, sizeof(title), "SET %s POSITION", jc_axis_name(_edit_axis));
    const char* notes[] = { "Sets the current position of this axis to the value in the active WCS (G10 L20 P0)." };
    stage(jc_set_axis_value(sv.profile, _edit_axis, value), title, notes, 1);
}

void JcOffsetsScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < 6; ++i) {
        if (ui::hit(wcs_rect(i), x, y)) {
            char title[32];
            snprintf(title, sizeof(title), "SELECT G%u", 54u + i);
            const char* notes[] = { "Makes this work coordinate system active for later moves." };
            stage(jc_select_wcs(i), title, notes, 1);
            return;
        }
    }
    for (uint8_t i = 0; i < 4; ++i) {
        if (ui::hit(zero_rect(i), x, y)) {
            char title[32];
            snprintf(title, sizeof(title), "ZERO %s AXIS", jc_axis_name(i));
            const char* notes[] = { "Sets the work position of this axis to 0 at the current location." };
            stage(jc_zero_axis(sv.profile, i), title, notes, 1);
            return;
        }
        if (ui::hit(set_rect(i), x, y)) {
            _edit_axis = i;
            JcInputRequest r {};
            r.prompt = "New work position";
            r.initial[0] = '\0';
            r.numeric = true;
            r.max_len = 10;
            r.done = edit_callback;
            r.user = this;
            sv.host.request_input(r);
            return;
        }
    }
}

void JcOffsetsScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 300) {
        _last_tick = now;
        draw_values();
    }
}

}  // namespace jc8048w550
