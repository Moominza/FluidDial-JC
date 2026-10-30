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
    { "JOGGING", "Preview only", JcPage::Jog },
    { "HOMING", "Preview only", JcPage::Homing },
    { "PROBING", "Preview only", JcPage::Probing },
    { "WORK OFFSETS", "G54-G59, zero/set", JcPage::Offsets },
    { "FILES / JOBS", "Job status; no listing", JcPage::Files },
    { "MACROS", "User defined", JcPage::Macros },
    { "TOOLS", "Manual tool change", JcPage::Tools },
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
#if JC_LIVE_JOG_TEST
    ui::text(sv.display, 18, 458, "LIVE JOG TEST BUILD: only a restricted jog can be sent. Other pages are previews only.",
             FontId::Body16, RED, ui::kBg);
#else
    ui::text(sv.display, 18, 458, "Machine control disabled in this build: motion pages show command previews only.",
             FontId::Body16, ui::kWarn, ui::kBg);
#endif
}

void JcMenuScreen::on_touch(uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < 9; ++i) {
        if (ui::hit(tile_rect(i), x, y)) {
            if (kTiles[i].page == JcPage::Count) {
                sv.host.leave(JcLeaveTarget::Dashboard);
            } else {
                sv.host.go(kTiles[i].page);
            }
            return;
        }
    }
}

}  // namespace jc8048w550
