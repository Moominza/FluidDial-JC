// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Homing and jog readiness, reported as SEPARATE facts (Instruction 009). FluidNC is the authority:
//   configured        homing/cycle > 0 for the axis (from the controller's settings)
//   switch present    a limit pin is assigned for the axis
//   switch active     the pin is reported triggered in the current status frame (Pn:)
//   homed             a homing cycle for the axis completed (acknowledged) in THIS connection
//   position trusted  homed this connection (the pendant never assumes more than that)
//   accepts jog       fresh status and a state in which FluidNC takes a jog (Idle / Jog)
// Jogging needs none of the homing facts: FluidNC accepts jogs while unhomed unless it is configured
// with start/must_home, which is shown separately and is not treated as a communication error.
// An unconfigured or inactive homing input is a configuration fact, never a pendant error.
// Pure: no I/O.

#pragma once

#include <stdint.h>
#include <string.h>

#include "JcMachineCaps.hpp"

namespace jc8048w550 {

// Facts the owner has to supply; the pendant cannot know them. Z homing direction is recorded as not
// verified, so a Z or "home all" cycle (which includes Z) is not initiated without that confirmation.
struct JcHomingOwnerFacts {
    bool z_homing_verified = false;
    bool a_homing_present = false;   // Settings > Machine ("A has a home switch")
};

struct JcAxisHoming {
    JcTri configured = JcTri::Unknown;
    JcTri switch_present = JcTri::Unknown;
    JcTri switch_active = JcTri::Unknown;
    JcTri homed = JcTri::Unknown;
    bool  may_home = false;           // the pendant will prepare "home this axis"
    const char* why_not = "";         // reason when !may_home
};

struct JcHomingView {
    JcAxisHoming axis[4];
    JcTri must_home = JcTri::Unknown;
    JcTri position_trusted = JcTri::No;
    bool  accepts_jog = false;
    bool  may_home_all = false;
    const char* home_all_why_not = "";
    char  note[96] = "";              // one line for the operator
};

// Tracks homing cycles completed in this connection. Cleared on link loss, alarm and reset: nothing a
// previous connection established is carried over, and nothing is replayed.
class JcHomedTracker {
public:
    // axis 0..3, or 0xFF for "home all configured axes" (axes given by configured_mask).
    void on_home_acked(uint8_t axis, uint8_t configured_mask) {
        _mask = static_cast<uint8_t>(_mask | (axis == 0xFF ? configured_mask : static_cast<uint8_t>(1u << (axis & 3u))));
        _known = true;
    }
    void on_invalidated() { _mask = 0; _known = false; }   // link loss, alarm, reset
    JcTri homed(uint8_t axis) const {
        if (axis > 3) return JcTri::Unknown;
        return ((_mask >> axis) & 1u) != 0 ? JcTri::Yes : JcTri::Unknown;   // "no" is never claimed: unknown
    }
    uint8_t mask() const { return _mask; }

private:
    uint8_t _mask = 0;
    bool _known = false;
};

inline bool jc_pins_contain(const char* pins, char axis_letter) {
    return pins != nullptr && strchr(pins, axis_letter) != nullptr;
}

inline JcHomingView jc_homing_view(const JcMachineCaps& caps, bool caps_complete, const char* pins, bool status_fresh,
                                   const char* machine_state, const JcHomedTracker& homed, const JcHomingOwnerFacts& owner) {
    JcHomingView v;
    static const char kLetters[4] = { 'X', 'Y', 'Z', 'A' };
    v.must_home = caps_complete ? caps.must_home_tri() : JcTri::Unknown;
    for (uint8_t i = 0; i < 4; ++i) {
        JcAxisHoming& a = v.axis[i];
        const JcAxisCaps& c = caps.axes[i];
        if (caps_complete && c.homing_known()) a.configured = c.get(JcQueryField::HomingCycle) > 0.0f ? JcTri::Yes : JcTri::No;
        const int pins_n = caps_complete ? c.limit_pins_assigned() : -1;
        if (pins_n >= 0) a.switch_present = pins_n > 0 ? JcTri::Yes : JcTri::No;
        if (status_fresh && pins != nullptr) a.switch_active = jc_pins_contain(pins, kLetters[i]) ? JcTri::Yes : JcTri::No;
        a.homed = homed.homed(i);
        // May the pendant prepare a homing command for this axis?
        if (i == 3) {
            a.may_home = owner.a_homing_present && a.configured == JcTri::Yes;
            a.why_not = a.configured == JcTri::No ? "not configured on the controller"
                      : !owner.a_homing_present ? "no home switch declared for A" : "settings not read";
        } else if (i == 2 && !owner.z_homing_verified) {
            a.may_home = false;
            a.why_not = "Z homing direction not verified by the owner";
        } else {
            a.may_home = a.configured != JcTri::No;   // unknown: FluidNC will answer; No: nothing to home
            a.why_not = a.configured == JcTri::No ? "not configured on the controller" : "";
        }
        if (a.may_home && a.configured == JcTri::Unknown) a.why_not = "";   // allowed; FluidNC decides
    }
    // Home all runs every configured axis, including Z: held back until Z is verified and A is not
    // configured without a declared switch.
    v.may_home_all = owner.z_homing_verified && !(v.axis[3].configured == JcTri::Yes && !owner.a_homing_present);
    v.home_all_why_not = !owner.z_homing_verified ? "includes Z: Z homing direction not verified"
                       : (v.axis[3].configured == JcTri::Yes && !owner.a_homing_present) ? "includes A: no home switch declared" : "";
    v.position_trusted = (homed.mask() & 3u) == 3u ? JcTri::Yes : JcTri::No;   // X and Y homed this connection
    const bool state_ok = machine_state != nullptr && (strcmp(machine_state, "Idle") == 0 || strcmp(machine_state, "Jog") == 0);
    v.accepts_jog = status_fresh && state_ok;
    if (!status_fresh) {
        snprintf(v.note, sizeof(v.note), "No fresh status: jog and home are unavailable");
    } else if (v.must_home == JcTri::Yes && v.position_trusted != JcTri::Yes) {
        snprintf(v.note, sizeof(v.note), "Controller requires homing before motion (must_home)");
    } else if (v.accepts_jog) {
        snprintf(v.note, sizeof(v.note), "Jog is available without homing%s",
                 v.position_trusted == JcTri::Yes ? "" : "; machine position is NOT trusted");
    } else {
        snprintf(v.note, sizeof(v.note), "Machine state %s does not take a jog", machine_state ? machine_state : "?");
    }
    return v;
}

inline const char* jc_tri_word(JcTri t) { return t == JcTri::Yes ? "YES" : (t == JcTri::No ? "NO" : "?"); }

}  // namespace jc8048w550
