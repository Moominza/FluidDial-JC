// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Geometry of the dashboard header and body (800x480). One definition shared by the page code, the touch
// handling and the host layout check (tests/host/ui_render.cpp), so a drawn control and its hit area are
// the same rectangle and no region can overlap another.
//
//   y   0..63   header band: title | mode chip | link chip | build banner | MENU | LINK   (all y 12..54)
//   y  76..176  four coordinate cards (X Y Z A)
//   y 185..232  machine / link / reference strip
//   y 243..467  lower panel: status cells (y 278..388), diagnostics (y 396..446), footer (y 452..473)

#pragma once

#include "JcUiKit.hpp"

namespace jc8048w550 {
namespace dl {

constexpr int16_t kTitleX = 18;
constexpr int16_t kTitleMaxRight = 206;                  // page titles must end here
constexpr ui::Rect kModeChip { 212, 13, 106, 38 };        // LIVE / LIVE STALE / DEMO MODE (tap toggles)
constexpr ui::Rect kLinkChip { 324, 13, 176, 38 };       // the six honest link states
constexpr int16_t kBannerLeft = 506;                     // build banner area, left edge
constexpr int16_t kBannerRight = 618;                    // right edge on every page (MENU starts at 624)
constexpr ui::Rect kMenuButton { 624, 12, 76, 42 };
constexpr ui::Rect kLinkButton { 706, 12, 78, 42 };
constexpr ui::Rect kHomeButton { 704, 12, 80, 42 };      // other legacy pages

constexpr ui::Rect axis_card(uint8_t i) { return { static_cast<int16_t>(16 + i * 195), 76, 183, 101 }; }
constexpr ui::Rect kStatusStrip { 16, 185, 768, 48 };
constexpr ui::Rect kReferenceButton { 600, 190, 176, 38 };
constexpr ui::Rect kLowerPanel { 16, 243, 768, 225 };
constexpr ui::Rect info_label(uint8_t i) { return { static_cast<int16_t>(30 + (i % 3) * 252), static_cast<int16_t>(278 + (i / 3) * 62), 240, 20 }; }
constexpr ui::Rect info_value(uint8_t i) { return { static_cast<int16_t>(30 + (i % 3) * 252), static_cast<int16_t>(298 + (i / 3) * 62), 240, 28 }; }
constexpr ui::Rect kDiagLine1 { 28, 396, 744, 24 };
constexpr ui::Rect kDiagLine2 { 28, 422, 744, 24 };
constexpr ui::Rect kFooter { 22, 452, 756, 22 };

}  // namespace dl
}  // namespace jc8048w550
