// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Tools (Instruction 010). Manual tool changes only: no automatic tool changer or M6 macro is assumed, and M6 / T
// words are never sent. The page shows what the CONTROLLER reports (current T word, tool length offset) and starts
// the guided routines: measure the reference tool, change the tool by hand and measure the new one, apply the
// difference as a tool length offset (G43.1, read back) and clear it (G49). Tool length compensation is not the
// work zero: no work offset is ever written here.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kInfoPanel { 16, 72, 388, 214 };
constexpr ui::Rect kNotes { 16, 294, 388, 178 };
ui::Rect button_rect(uint8_t i) { return { 416, static_cast<int16_t>(72 + i * 66), 368, 58 }; }
constexpr uint8_t kButtons = 6;

}  // namespace

void JcToolsScreen::on_enter() {
    _drawn_key = 0xFFFFFFFFu;
}

bool JcToolsScreen::state_known() const {
    return sv.tool_reader.info().tlo_known;
}

uint32_t JcToolsScreen::key() const {
    const JcToolInfo& i = sv.tool_reader.info();
    uint32_t k = static_cast<uint32_t>(sv.tool_reader.state());
    k = k * 7u + (i.tlo_known ? 1u : 0u) + (i.tlo_modal ? 2u : 0u) + static_cast<uint32_t>(i.tool + 2) * 8u;
    k = k * 31u + static_cast<uint32_t>(i.tlo * 1000.0f);
    k = k * 3u + (sv.tool_store.has_ref ? 1u : 0u) + (sv.tool_store.tlo_verified ? 2u : 0u);
    k = k * 31u + static_cast<uint32_t>(sv.tool_store.ref_mz * 1000.0f);
    return k;
}

void JcToolsScreen::draw_info() {
    const JcToolInfo& i = sv.tool_reader.info();
    char l[110];
    ui::panel(sv.display, kInfoPanel);
    ui::text(sv.display, kInfoPanel.x + 10, kInfoPanel.y + 6, "WHAT THE CONTROLLER REPORTS", FontId::Button20, WHITE, ui::kPanel);
    for (ui::Slot& s : _lines) s.invalidate();
    auto line = [&](uint8_t row, const char* text, uint16_t color) {
        _lines[row].draw(sv.display, { static_cast<int16_t>(kInfoPanel.x + 10), static_cast<int16_t>(kInfoPanel.y + 36 + row * 28), 368, 26 }, text,
                         FontId::Body16, color, ui::kPanel);
    };
    switch (sv.tool_reader.state()) {
        case JcToolReadState::Idle:
            line(0, "Tool state not read yet. Press READ TOOL STATE.", ui::kMuted);
            break;
        case JcToolReadState::Reading:
            line(0, "Reading $G and $# (read-only) ...", ui::kInfo);
            break;
        case JcToolReadState::Failed:
            snprintf(l, sizeof(l), "READ FAILED: %s", sv.tool_reader.reason());
            line(0, l, RED);
            break;
        case JcToolReadState::Done:
            if (i.tool >= 0) snprintf(l, sizeof(l), "Current tool (T word): T%d", i.tool); else snprintf(l, sizeof(l), "Current tool: not reported");
            line(0, l, WHITE);
            if (i.tlo_known) {
                snprintf(l, sizeof(l), "Tool length offset: %+.3f mm   %s", static_cast<double>(i.tlo), i.tlo_modal ? "(G43.1 active)" : "(no G43.1)");
                line(1, l, i.tlo_modal || fabsf(i.tlo) > 0.0005f ? ui::kWarn : WHITE);
            }
            break;
    }
    if (sv.tool_store.has_ref) {
        snprintf(l, sizeof(l), "Reference tool touched at machine Z %.3f%s", static_cast<double>(sv.tool_store.ref_mz),
                 sv.tool_store.simulated ? "  (REHEARSAL)" : "");
        line(2, l, sv.tool_store.simulated ? ui::kWarn : ui::kGood);
    } else if (sv.tool_store.invalid_reason[0]) {
        snprintf(l, sizeof(l), "Reference discarded: %s", sv.tool_store.invalid_reason);
        line(2, l, ui::kWarn);
    } else {
        line(2, "No reference tool measured yet.", ui::kMuted);
    }
    line(3, sv.tool_store.tlo_verified ? "G43.1 / G49 confirmed by readback this session." : "G43.1 / G49 not yet confirmed by a readback.",
         sv.tool_store.tlo_verified ? ui::kGood : ui::kMuted);
    line(4, "Automatic tool change / M6: NOT VERIFIED.", ui::kWarn);
    line(5, "M6 and T words are never sent: manual change only.", ui::kMuted);
}

void JcToolsScreen::draw_buttons() {
    const bool known = state_known();
    const bool ref = sv.tool_store.has_ref;
    ui::button(sv.display, button_rect(0), sv.tool_reader.reading() ? "READING ..." : "READ TOOL STATE  ($G  $#)",
               sv.tool_reader.reading() ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, button_rect(1), "MEASURE REFERENCE TOOL", known ? ui::Btn::Normal : ui::Btn::Disabled);
    ui::button(sv.display, button_rect(2), "CHANGE TOOL BY HAND + MEASURE", known && ref ? ui::Btn::Good : ui::Btn::Disabled);
    ui::button(sv.display, button_rect(3), "MEASURE TOOL LENGTH ONLY", known && ref ? ui::Btn::Normal : ui::Btn::Disabled);
    ui::button(sv.display, button_rect(4), "CLEAR TOOL OFFSET  (G49)", known ? ui::Btn::Danger : ui::Btn::Disabled);
    ui::button(sv.display, button_rect(5), "PROBE SETTINGS");
}

void JcToolsScreen::on_draw() {
    draw_info();
    draw_buttons();
    ui::panel(sv.display, kNotes);
    ui::wrap(sv.display, { kNotes.x + 10, kNotes.y + 8, kNotes.w - 20, kNotes.h - 14 },
             "Tool LENGTH compensation is not the work zero. The offset is the extra length of the new tool compared with "
             "the reference tool, measured on the same surface, so the plate thickness cancels. Nothing here writes a work "
             "offset (G54..G59) or G92. The offset is applied only when you confirm and is read back from FluidNC.",
             FontId::Body16, ui::kMuted, ui::kPanel);
    _drawn_key = key();
}

void JcToolsScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(button_rect(0), x, y)) {
        const char* why = sv.host.tool_read();
        if (why != nullptr) show_message("CANNOT READ TOOL STATE", why, "Read-only queries; nothing was changed.");
        else draw();
        return;
    }
    if (ui::hit(button_rect(5), x, y)) { sv.host.go(JcPage::Probing); return; }
    const bool known = state_known();
    uint8_t which = 0xFF;
    for (uint8_t i = 1; i <= 4; ++i) if (ui::hit(button_rect(i), x, y)) which = i;
    if (which == 0xFF) return;
    if (!known) {
        show_message("READ THE TOOL STATE FIRST", "The controller must report [TLO:] before tool routines are offered.",
                     "That also proves it has the tool offset machinery.", "Press READ TOOL STATE.");
        return;
    }
#if JC_AUTHORITY
    if (which == 4) {
        open_custom_confirm("CLEAR THE TOOL LENGTH OFFSET?", "SEND G49");
        dialog.add_line("Sends G49 and reads [TLO:] back; it must read 0.");
        dialog.add_line("The machine must be Idle. No work offset is touched.", ui::kMuted);
        dialog.add_line("Keep the E-stop in reach.", RED);
        draw();
        return;
    }
    if ((which == 2 || which == 3) && !sv.tool_store.has_ref) {
        show_message("MEASURE THE REFERENCE TOOL FIRST", "A tool length offset is a difference to a reference tool.",
                     sv.tool_store.invalid_reason[0] ? sv.tool_store.invalid_reason : "No reference contact is stored.");
        return;
    }
    sv.host.workflow_open(which == 1 ? JcWfKind::ToolReference : (which == 2 ? JcWfKind::ToolChange : JcWfKind::ToolMeasure));
#else
    show_message("NOT AVAILABLE IN THIS BUILD", "The tool routines need the unified command interface.");
#endif
}

void JcToolsScreen::on_custom_confirm() {
#if JC_AUTHORITY
    if (!sv.host.workflow_clear_tlo()) show_message("CANNOT CLEAR THE OFFSET", "The machine must be Idle with fresh status.");
#endif
}

void JcToolsScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 300) return;
    _last_tick = now;
    if (key() != _drawn_key) {
        draw_info();
        draw_buttons();
        _drawn_key = key();
    }
}

}  // namespace jc8048w550
