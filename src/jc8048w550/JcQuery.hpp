// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Read-only controller queries. This is the ONLY text (besides the '?' status
// byte) that either transport may send in this build, and it is a closed set:
// firmware / modal-state queries and reads of individual configuration items
// ("$/axes/x/max_travel_mm" with no '=' is a read, never a write). Anything
// else - jog, home, unlock, any G-code, any assignment - fails
// jc_query_allowed() and is never transmitted.

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
    Count,
};
constexpr uint8_t kJcQueryFieldCount = static_cast<uint8_t>(JcQueryField::Count);

inline const char* jc_query_field_key(JcQueryField f) {
    switch (f) {
        case JcQueryField::StepsPerMm: return "steps_per_mm";
        case JcQueryField::MaxTravelMm: return "max_travel_mm";
        case JcQueryField::MaxRate: return "max_rate_mm_per_min";
        case JcQueryField::Accel: return "acceleration_mm_per_sec2";
        case JcQueryField::HomingCycle: return "homing/cycle";
        case JcQueryField::HomingPositiveDir: return "homing/positive_direction";
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

// True only for the exact lines the application may send as read-only queries.
inline bool jc_query_allowed(const char* line) {
    if (line == nullptr || line[0] != '$' || strlen(line) > 64) {
        return false;
    }
    if (strcmp(line, kJcQueryBuildInfo) == 0 || strcmp(line, kJcQueryModalState) == 0
        || strcmp(line, kJcQueryProbePin) == 0 || strcmp(line, kJcQueryToolsetterPin) == 0) {
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
