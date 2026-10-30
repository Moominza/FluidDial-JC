// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Read-only controller queries. This is the ONLY text (besides the '?' status
// byte) that either transport may send in the ordinary build, and it is a closed
// set: firmware / modal-state queries and reads of individual configuration items
// ("$/axes/x/max_travel_mm" with no '=' is a read, never a write). Anything
// else - jog, home, unlock, any G-code, any assignment - fails
// jc_query_allowed() and is never transmitted.
//
// Instruction 006 adds the soft-limit, hard-limit, limit-pin, homing-position and
// must-home items, so the operator can see what the controller itself enforces
// before any motion test. They are reads like the rest.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace jc8048w550 {

constexpr uint8_t kJcQueryAxes = 4;  // X, Y, Z, A

enum class JcQueryField : uint8_t {
    StepsPerMm,
    MaxTravelMm,
    MaxRate,
    Accel,
    HomingCycle,
    HomingPositiveDir,
    SoftLimits,       // axes/<a>/soft_limits                  true / false
    HardLimits,       // axes/<a>/motor0/hard_limits           true / false
    LimitNegPin,      // axes/<a>/motor0/limit_neg_pin         NO_PIN or gpio.N...
    LimitPosPin,      // axes/<a>/motor0/limit_pos_pin
    LimitAllPin,      // axes/<a>/motor0/limit_all_pin
    HomingMpos,       // axes/<a>/homing/mpos_mm               machine position after homing
    Count,
};
constexpr uint8_t kJcQueryFieldCount = static_cast<uint8_t>(JcQueryField::Count);

// How the controller's answer for a field is interpreted.
enum class JcFieldKind : uint8_t { Number, Cycle, Bool, Pin, Position };

inline JcFieldKind jc_query_field_kind(JcQueryField f) {
    switch (f) {
        case JcQueryField::HomingCycle: return JcFieldKind::Cycle;
        case JcQueryField::HomingPositiveDir:
        case JcQueryField::SoftLimits:
        case JcQueryField::HardLimits: return JcFieldKind::Bool;
        case JcQueryField::LimitNegPin:
        case JcQueryField::LimitPosPin:
        case JcQueryField::LimitAllPin: return JcFieldKind::Pin;
        case JcQueryField::HomingMpos: return JcFieldKind::Position;
        default: return JcFieldKind::Number;
    }
}

inline const char* jc_query_field_key(JcQueryField f) {
    switch (f) {
        case JcQueryField::StepsPerMm: return "steps_per_mm";
        case JcQueryField::MaxTravelMm: return "max_travel_mm";
        case JcQueryField::MaxRate: return "max_rate_mm_per_min";
        case JcQueryField::Accel: return "acceleration_mm_per_sec2";
        case JcQueryField::HomingCycle: return "homing/cycle";
        case JcQueryField::HomingPositiveDir: return "homing/positive_direction";
        case JcQueryField::SoftLimits: return "soft_limits";
        case JcQueryField::HardLimits: return "motor0/hard_limits";
        case JcQueryField::LimitNegPin: return "motor0/limit_neg_pin";
        case JcQueryField::LimitPosPin: return "motor0/limit_pos_pin";
        case JcQueryField::LimitAllPin: return "motor0/limit_all_pin";
        case JcQueryField::HomingMpos: return "homing/mpos_mm";
        case JcQueryField::Count: break;
    }
    return "";
}

inline char jc_query_axis_letter(uint8_t axis) {
    return "xyza"[axis & 3];
}

// "$/axes/<x|y|z|a>/<key>". Returns false if the buffer is too small.
inline bool jc_query_axis_line(char* out, size_t size, uint8_t axis, JcQueryField field) {
    if (axis >= kJcQueryAxes || field >= JcQueryField::Count) {
        return false;
    }
    const int n = snprintf(out, size, "$/axes/%c/%s", jc_query_axis_letter(axis), jc_query_field_key(field));
    return n > 0 && static_cast<size_t>(n) < size;
}

// Fixed (non-axis) queries.
constexpr const char* kJcQueryBuildInfo = "$I";
constexpr const char* kJcQueryModalState = "$G";
constexpr const char* kJcQueryProbePin = "$/probe/pin";
constexpr const char* kJcQueryToolsetterPin = "$/probe/toolsetter_pin";
constexpr const char* kJcQueryMustHome = "$/start/must_home";

// True only for the exact lines the application may send as read-only queries.
inline bool jc_query_allowed(const char* line) {
    if (line == nullptr || line[0] != '$' || strlen(line) > 64) {
        return false;
    }
    if (strcmp(line, kJcQueryBuildInfo) == 0 || strcmp(line, kJcQueryModalState) == 0
        || strcmp(line, kJcQueryProbePin) == 0 || strcmp(line, kJcQueryToolsetterPin) == 0
        || strcmp(line, kJcQueryMustHome) == 0) {
        return true;
    }
    char candidate[72];
    for (uint8_t axis = 0; axis < kJcQueryAxes; ++axis) {
        for (uint8_t f = 0; f < kJcQueryFieldCount; ++f) {
            if (jc_query_axis_line(candidate, sizeof(candidate), axis, static_cast<JcQueryField>(f))
                && strcmp(line, candidate) == 0) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace jc8048w550
