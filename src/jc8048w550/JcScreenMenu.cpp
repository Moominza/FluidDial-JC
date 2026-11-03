// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

struct Tile {
    const char* title;
    const char* subtitle;
    JcPage page;       // Count = dashboard
};

constexpr Tile kTiles[9] = {
    { "DASHBOARD", "Live X Y Z A", JcPage::Count },
#if JC_FULL_CONTROL_TEST
    { "JOGGING", "X Y Z mm, A degrees", JcPage::Jog },
    { "HOMING", "Home the axes", JcPage::Homing },
    { "PROBING", "Centre, edge, rotary", JcPage::Probing },
    { "WORK OFFSETS", "G54-G59, zero/set", JcPage::Offsets },
    { "FILES / JOBS", "Monitor only; no start", JcPage::Files },
    { "MACROS", "Work positioning", JcPage::Macros },
    { "TOOLS", "Reference, TLO", JcPage::Tools },
#else
    { "JOGGING", JC_LIVE_JOG_TEST ? "TEST: X/Y 1 mm only" : (JC_JOG_PREVIEW ? "PREVIEW: simulated" : "Preview only"), JcPage::Jog },
    { "HOMING", "Preview only", JcPage::Homing },
    { "PROBING", "Preview only", JcPage::Probing },
    { "WORK OFFSETS", "G54-G59, zero/set", JcPage::Offsets },
    { "FILES / JOBS", "Job status; no listing", JcPage::Files },
    { "MACROS", "User defined", JcPage::Macros },
    { "TOOLS", "Manual tool change", JcPage::Tools },
#endif
    { "SETTINGS", "Setup + diagnostics", JcPage::Settings },
};

ui::Rect tile_rect(uint8_t i) {
    return { static_cast<int16_t>(16 + (i % 3) * 258), static_cast<int16_t>(80 + (i / 3) * 128), 252, 118 };
}

}  // namespace

void JcMenuScreen::on_draw() {
    for (uint8_t i = 0; i < 9; ++i) {
        const ui::Rect r = tile_rect(i);
        ui::panel(sv.display, r, ui::kPanelLight);
        ui::text(sv.display, r.x + 16, r.y + 24, kTiles[i].title, FontId::Heading24, WHITE, ui::kPanelLight);
        ui::text(sv.display, r.x + 16, r.y + 70, kTiles[i].subtitle, FontId::Body16, ui::kMuted, ui::kPanelLight);
    }
#if JC_JOG_PREVIEW
    ui::text(sv.display, 18, 458, "PREVIEW ONLY - MOTION DISABLED: nothing is sent to the machine. Jogging is simulated.",
             FontId::Body16, RED, ui::kBg);
#elif JC_FULL_CONTROL_TEST
    _status.invalidate();
    draw_status();
#elif JC_JOG_UI
    ui::text(sv.display, 18, 458, "LIVE JOG TEST BUILD: only a restricted jog can be sent. Other pages are previews only.",
             FontId::Body16, RED, ui::kBg);
#else
    ui::text(sv.display, 18, 458, "Machine control disabled in this build: motion pages show command previews only.",
             FontId::Body16, ui::kWarn, ui::kBg);
#endif
}

#if JC_FULL_CONTROL_TEST
// Compact connection / controller status. Nothing about the firmware build: that is under SETTINGS > DIAGNOSTICS.
void JcMenuScreen::draw_status() {
    char line[80];
    uint16_t color = ui::kMuted;
    if (!sv.live_mode) {
        snprintf(line, sizeof(line), "DEMO  |  no controller connected");
    } else if (!sv.live_fresh()) {
        snprintf(line, sizeof(line), "DISCONNECTED  |  waiting for the controller");
        color = ui::kWarn;
    } else {
        snprintf(line, sizeof(line), "CONNECTED  |  %s", sv.model.state());
        color = sv.model.alarm() ? RED : (sv.model.in_motion() ? ui::kWarn : ui::kGood);
    }
    _status.draw(sv.display, { 18, 456, 764, 22 }, line, FontId::Body16, color, ui::kBg);
}

void JcMenuScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 500) {
        _last_tick = now;
        draw_status();
    }
}
#endif

void JcMenuScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < 9; ++i) {
        if (ui::hit(tile_rect(i), x, y)) {
            const JcPage p = kTiles[i].page;
            const bool motion_page = p == JcPage::Jog || p == JcPage::Homing || p == JcPage::Probing || p == JcPage::Offsets
                                     || p == JcPage::Macros || p == JcPage::Tools;
            if (motion_page && sv.host.ota_locked()) {
                show_message("FIRMWARE UPDATE ACTIVE", "Machine pages are locked while an update", "is armed or running.",
                             "Cancel it in SETTINGS > FIRMWARE UPDATE.");
            } else if (kTiles[i].page == JcPage::Count) {
                sv.host.leave(JcLeaveTarget::Dashboard);
            } else {
                sv.host.go(kTiles[i].page);
            }
            return;
        }
    }
}

}  // namespace jc8048w550
