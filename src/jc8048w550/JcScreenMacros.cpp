// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

ui::Rect row_rect(uint8_t i) { return { 16, static_cast<int16_t>(76 + i * 52), 520, 46 }; }
constexpr ui::Rect kRun { 552, 76, 232, 66 };
constexpr ui::Rect kEdit { 552, 152, 232, 66 };
constexpr ui::Rect kClear { 552, 228, 232, 66 };

void edit_callback(void* user, bool ok, const char* text) {
    static_cast<JcMacrosScreen*>(user)->edit_done(ok, text);
}

}  // namespace

void JcMacrosScreen::draw_list() {
    for (uint8_t i = 0; i < kJcMacroSlots; ++i) {
        const JcMacroSlot& m = sv.settings.macros[i];
        const ui::Rect r = row_rect(i);
        const bool sel = i == _selected;
        const uint16_t fill = sel ? ui::kInfo : ui::kPanel;
        sv.display.fillRect(r.x, r.y, r.w, r.h, fill);
        sv.display.drawRect(r.x, r.y, r.w, r.h, sel ? CYAN : ui::kBorder);
        char line[64];
        if (m.path[0]) {
            snprintf(line, sizeof(line), "%u. %s", i + 1u, m.name[0] ? m.name : "(unnamed)");
        } else {
            snprintf(line, sizeof(line), "%u. (empty slot)", i + 1u);
        }
        ui::text(sv.display, r.x + 10, r.y + 4, line, FontId::Button20, m.path[0] ? WHITE : ui::kMuted, fill);
        ui::text(sv.display, r.x + 10, r.y + 26, m.description[0] ? m.description : m.path, FontId::Body16,
                 sel ? WHITE : ui::kMuted, fill);
    }
}

void JcMacrosScreen::on_draw() {
    draw_list();
    ui::button(sv.display, kRun, "RUN (PREVIEW)");
    ui::button(sv.display, kEdit, "EDIT SLOT");
    ui::button(sv.display, kClear, "CLEAR SLOT", ui::Btn::Danger);
    ui::wrap(sv.display, { 552, 306, 232, 160 },
             "Macros run controller files with $Localfs/Run=<path>. They are defined here by you; the "
             "controller's own macro list is not shown because reading it needs a query command.",
             FontId::Body16, ui::kMuted, ui::kBg);
    ui::wrap(sv.display, { 16, 392, 520, 74 },
             "Macros can move the machine, change tools or run the spindle. Each run needs confirmation.",
             FontId::Body16, ui::kWarn, ui::kBg);
}

void JcMacrosScreen::next_edit_stage() {
    JcInputRequest r {};
    static const char* const prompts[3] = { "Macro name", "Controller file path, e.g. /macro0.nc", "Description" };
    r.prompt = prompts[_edit_stage];
    const char* initial = _edit_stage == 0 ? _editing.name : (_edit_stage == 1 ? _editing.path : _editing.description);
    strncpy(r.initial, _edit_stage == 1 && !initial[0] ? "/" : initial, sizeof(r.initial) - 1);
    r.numeric = false;
    r.max_len = _edit_stage == 0 ? 15 : (_edit_stage == 1 ? 47 : 39);
    r.done = edit_callback;
    r.user = this;
    sv.host.request_input(r);
}

void JcMacrosScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    if (_edit_stage == 0) {
        strncpy(_editing.name, text, sizeof(_editing.name) - 1);
    } else if (_edit_stage == 1) {
        strncpy(_editing.path, text, sizeof(_editing.path) - 1);
    } else {
        strncpy(_editing.description, text, sizeof(_editing.description) - 1);
    }
    if (_edit_stage < 2) {
        ++_edit_stage;
        next_edit_stage();
        return;
    }
    // Final stage: validate the whole slot before storing it.
    JcUserSettings candidate = sv.settings;
    candidate.macros[_selected] = _editing;
    jc_settings_sanitize(candidate);
    if (candidate.macros[_selected].path[0] == '\0') {
        show_message("MACRO NOT SAVED", "The file path is invalid (must start with / and use plain",
                     "characters only; '..' and special characters are rejected).");
        return;
    }
    sv.settings.macros[_selected] = candidate.macros[_selected];
    sv.host.save_settings();
    draw();
}

void JcMacrosScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < kJcMacroSlots; ++i) {
        if (ui::hit(row_rect(i), x, y)) {
            const uint8_t previous = _selected;
            _selected = i;
            (void)previous;
            draw_list();
            return;
        }
    }
    const JcMacroSlot& m = sv.settings.macros[_selected];
    if (ui::hit(kRun, x, y)) {
        if (m.path[0] == '\0') {
            show_message("EMPTY SLOT", "Use EDIT SLOT to define a macro first.");
            return;
        }
        char title[40];
        snprintf(title, sizeof(title), "RUN MACRO: %s", m.name[0] ? m.name : "(unnamed)");
        const char* notes[] = { m.description[0] ? m.description : "No description.",
                                "The macro may move the machine or change its state." };
        stage(jc_run_macro_file(m.path), title, notes, 2);
    } else if (ui::hit(kEdit, x, y)) {
        _editing = m;
        _edit_stage = 0;
        next_edit_stage();
    } else if (ui::hit(kClear, x, y)) {
        memset(&sv.settings.macros[_selected], 0, sizeof(JcMacroSlot));
        sv.host.save_settings();
        draw_list();
    }
}

}  // namespace jc8048w550
