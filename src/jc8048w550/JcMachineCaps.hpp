// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Machine capabilities as REPORTED by the controller, plus the validation that
// decides whether a reported value may replace a placeholder motion limit.
// Nothing is assumed: a field is either confirmed (the controller answered with
// a plausible number) or it is not, and unconfirmed limits stay placeholders.
// Capabilities are held in RAM only and are discarded whenever the connection
// is lost, because the controller may have been reconfigured or restarted.
//
// Not readable from FluidNC and therefore never claimed: whether the A axis is
// rotary (the operator declares that; the unit is degrees) and the
// controller's soft-limit state.

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "JcCommandBuilder.hpp"
#include "JcQuery.hpp"

namespace jc8048w550 {

enum class JcFieldState : uint8_t {
    Unqueried,
    Valid,        // answered with a plausible value
    BadValue,     // answered, but non-numeric / non-finite / out of range
    CtrlError,    // controller replied error:N (item not present or not readable)
    NoValue,      // "ok" with no value line
};

struct JcAxisCaps {
    JcFieldState state[kJcQueryFieldCount] {};
    float        value[kJcQueryFieldCount] {};  // positive_direction: 1 = true, 0 = false
    int          error_code[kJcQueryFieldCount] {};

    bool valid(JcQueryField f) const { return state[static_cast<uint8_t>(f)] == JcFieldState::Valid; }
    float get(JcQueryField f) const { return value[static_cast<uint8_t>(f)]; }

    // The four values needed to replace the placeholder motion limits.
    bool limits_confirmed() const {
        return valid(JcQueryField::StepsPerMm) && valid(JcQueryField::MaxTravelMm)
            && valid(JcQueryField::MaxRate) && valid(JcQueryField::Accel);
    }
    bool homing_known() const { return valid(JcQueryField::HomingCycle); }
    bool any_valid() const {
        for (uint8_t i = 0; i < kJcQueryFieldCount; ++i) {
            if (state[i] == JcFieldState::Valid) return true;
        }
        return false;
    }
};

struct JcMachineCaps {
    bool        complete = false;       // every query finished (answered or errored)
    char        firmware[40] = "";      // text of [VER:...]
    char        options[40] = "";       // text of [OPT:...]
    char        wcs[6] = "";            // active work coordinate system from $G, e.g. "G54"
    int         tool = -1;              // T word from $G; -1 unknown
    char        probe_pin[32] = "";
    char        toolsetter_pin[32] = "";
    JcAxisCaps  axes[kJcQueryAxes] {};
    uint8_t     queries_ok = 0;
    uint8_t     queries_error = 0;
    uint32_t    read_ms = 0;

    bool any_data() const {
        if (firmware[0] != '\0' || wcs[0] != '\0') return true;
        for (const auto& a : axes) {
            if (a.any_valid()) return true;
        }
        return false;
    }
    uint8_t confirmed_axis_mask() const {
        uint8_t m = 0;
        for (uint8_t i = 0; i < kJcQueryAxes; ++i) {
            if (axes[i].limits_confirmed()) m |= static_cast<uint8_t>(1u << i);
        }
        return m;
    }
};

// ---- value validation --------------------------------------------------

// Plausibility ranges. A value outside them is treated as a bad answer, never
// clamped into range.
inline bool jc_caps_parse_number(const char* text, float& out) {
    if (text == nullptr || text[0] == '\0') return false;
    char* end = nullptr;
    const float v = strtof(text, &end);
    if (end == text || *end != '\0' || !isfinite(v)) return false;
    out = v;
    return true;
}

inline bool jc_caps_field_valid(JcQueryField f, const char* text, float& out) {
    if (f == JcQueryField::HomingPositiveDir) {
        if (text != nullptr && strcmp(text, "true") == 0) { out = 1.0f; return true; }
        if (text != nullptr && strcmp(text, "false") == 0) { out = 0.0f; return true; }
        return false;
    }
    float v = 0.0f;
    if (!jc_caps_parse_number(text, v)) return false;
    switch (f) {
        case JcQueryField::StepsPerMm:
        case JcQueryField::MaxTravelMm:
        case JcQueryField::Accel:
            if (!(v > 0.0f && v <= 100000.0f)) return false;
            break;
        case JcQueryField::MaxRate:
            if (!(v > 0.0f && v <= 200000.0f)) return false;
            break;
        case JcQueryField::HomingCycle:
            if (v < 0.0f || v > 6.0f || v != floorf(v)) return false;
            break;
        default:
            return false;
    }
    out = v;
    return true;
}

// ---- profile -----------------------------------------------------------

// Replaces placeholder limits with controller-confirmed values, axis by axis,
// and only for axes whose four limit values were all confirmed. Homing
// permission comes from the controller's homing/cycle (cycle 0 = not homed);
// the operator's own "A has a home switch" declaration can only restrict it.
inline void jc_profile_apply_caps(JcMachineProfile& profile, const JcMachineCaps& caps) {
    for (uint8_t i = 0; i < kJcQueryAxes; ++i) {
        const JcAxisCaps& c = caps.axes[i];
        JcAxisProfile& a = profile.axes[i];
        if (c.limits_confirmed()) {
            a.travel_mm = c.get(JcQueryField::MaxTravelMm);
            a.max_step = a.travel_mm;
            a.max_feed = c.get(JcQueryField::MaxRate);
            a.limits_confirmed = true;
        }
        if (c.homing_known()) {
            a.homing_allowed = a.homing_allowed && c.get(JcQueryField::HomingCycle) > 0.0f;
        }
    }
}

// Why a capability read cannot start now (nullptr = it may).
inline const char* jc_caps_read_blocker(bool live, bool link_fresh, const char* machine_state, bool busy) {
    if (busy) return "a read is already in progress";
    if (!live) return "DEMO mode: switch to LIVE and connect first";
    if (!link_fresh) return "no fresh controller status";
    if (machine_state == nullptr
        || (strcmp(machine_state, "Idle") != 0 && strcmp(machine_state, "Alarm") != 0)) {
        return "machine must be Idle (or Alarm) to read configuration";
    }
    return nullptr;
}

inline uint8_t jc_profile_limits_mask(const JcMachineProfile& profile) {
    uint8_t m = 0;
    for (uint8_t i = 0; i < kJcQueryAxes; ++i) {
        if (profile.axes[i].limits_confirmed) m |= static_cast<uint8_t>(1u << i);
    }
    return m;
}

}  // namespace jc8048w550
