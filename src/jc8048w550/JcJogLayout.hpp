// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Geometry of the jog page (800x480, header 0..63). One definition shared by the page code and the
// host renderer, so what is drawn and what a touch hits are the same rectangle. Regions never overlap;
// tests/host/ui_render.cpp checks that.

#pragma once

#include "JcUiKit.hpp"

namespace jc8048w550 {
namespace jl {

constexpr ui::Rect axis_rect(uint8_t i) { return { static_cast<int16_t>(16 + i * 100), 72, 92, 46 }; }
constexpr ui::Rect step_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 92), 126, 86, 44 }; }
constexpr ui::Rect feed_rect(uint8_t i) { return { static_cast<int16_t>(84 + i * 130), 178, 122, 44 }; }
constexpr ui::Rect kIncremental { 484, 178, 150, 44 };
constexpr ui::Rect kArm { 640, 178, 144, 44 };          // ARM / READ SETTINGS (continuous in the ordinary legacy build)
constexpr ui::Rect kJogNeg { 16, 230, 250, 76 };
constexpr ui::Rect kJogStop { 275, 230, 250, 76 };
constexpr ui::Rect kJogPos { 534, 230, 250, 76 };
constexpr ui::Rect kDro { 424, 72, 360, 46 };
constexpr ui::Rect kPanelRect { 16, 314, 768, 158 };
constexpr ui::Rect kPreviewSlot { 110, 322, 650, 24 };
constexpr ui::Rect kResultSlot { 110, 348, 650, 24 };
constexpr ui::Rect kMotionSlot { 110, 374, 650, 24 };
constexpr ui::Rect kCapsSlot { 28, 400, 744, 24 };
constexpr ui::Rect kWarnRect { 28, 428, 744, 40 };      // up to two lines
constexpr ui::Rect kAck { 28, 430, 744, 38 };
constexpr int16_t kLabelPreviewY = 326, kLabelResultY = 352, kLabelMotionY = 378;

}  // namespace jl
}  // namespace jc8048w550
