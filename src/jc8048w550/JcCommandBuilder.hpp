// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Pure command formatting and validation. Nothing here transmits anything;
// results are handed to JcCommandGate (JcCommandAuth.hpp). Formats follow the
// pinned upstream FluidDial sources (MultiJogScene, ProbingScene, HomingScene,
// FilePreviewScene, MacroMenu).

#pragma once

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcCommandAuth.hpp"

namespace jc8048w550 {

struct JcAxisProfile {
    char  letter;
    bool  rotary;         // true: degrees, false: millimetres
    float max_step;       // largest single incremental move
    float max_feed;       // units/min (mm/min or deg/min)
    bool  homing_allowed;
    bool  probe_allowed;
    // Filled from the controller by jc_profile_apply_caps(); until then max_step
    // and max_feed are placeholders and limits_confirmed stays false.
    // No default member initializers: the firmware builds as C++11, where they
    // would make the brace initializers in jc_default_profile() non-aggregate.
    float travel_mm;
    bool  limits_confirmed;
};

// Conservative placeholders, used until the controller's own values have been
// read and validated (JcMachineCaps.hpp); an axis whose limits are unconfirmed
// is blocked by the authorization gate whenever machine control is enabled.
struct JcMachineProfile {
    JcAxisProfile axes[4];
    float         max_probe_travel;
    float         max_probe_feed;
    // Live-jog TEST mode (JcJogTest.hpp): max_step / max_feed were clamped to the
    // test limits (0 = jogging disabled for that axis) and continuous jogging is
    // refused. Set by jc_profile_apply_test_limits(); false in the ordinary firmware.
    bool          test_mode;
};

inline JcMachineProfile jc_default_profile() {
    JcMachineProfile p {};
    p.axes[0] = { 'X', false, 100.0f, 3000.0f, true, true, 0.0f, false };
    p.axes[1] = { 'Y', false, 100.0f, 3000.0f, true, true, 0.0f, false };
    p.axes[2] = { 'Z', false, 100.0f, 1500.0f, true, true, 0.0f, false };
    // The rotary axis is not assumed to have a homing switch and is never probed.
    p.axes[3] = { 'A', true, 360.0f, 3600.0f, false, false, 0.0f, false };
    p.max_probe_travel = 50.0f;
    p.max_probe_feed = 200.0f;
    return p;
}

enum class JcCmdError : uint8_t {
    None,
    BadAxis,
    BadValue,        // non-finite, zero, or out of range
    BadFeed,
    Unsupported,     // e.g. probing the rotary axis, homing an axis not allowed
    BadPath,
    BadIndex,
    JogDisabled,     // jogging is disabled for this axis (e.g. A pending rotary calibration)
    TestLimit,       // larger than the live-jog test limit for this axis (step or feed)
};

constexpr size_t kJcMaxCommandLength = 120;
constexpr size_t kJcMaxSequenceSteps = 5;

struct JcCommand {
    bool           ok = false;
    JcCmdError     error = JcCmdError::None;
    JcCommandClass cls = JcCommandClass::Other;
    bool           realtime = false;  // single real-time byte in text[0]
    uint8_t        axis = 0xFF;       // axis moved by a jog / probe move; 0xFF = none
    char           text[kJcMaxCommandLength] = "";
};

struct JcCommandSequence {
    bool       ok = false;
    JcCmdError error = JcCmdError::None;
    uint8_t    count = 0;
    // The executor must stop the sequence on any error, alarm, or probe miss.
    JcCommand  steps[kJcMaxSequenceSteps];
};

namespace detail {

inline JcCommand fail(JcCommandClass cls, JcCmdError error) {
    JcCommand c;
    c.cls = cls;
    c.error = error;
    return c;
}

inline JcCommand make(JcCommandClass cls, const char* fmt, ...) {
    JcCommand c;
    c.cls = cls;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(c.text, sizeof(c.text), fmt, ap);
    va_end(ap);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(c.text)) {
        return fail(cls, JcCmdError::BadValue);
    }
    c.ok = true;
    return c;
}

inline JcCommand realtime(JcCommandClass cls, char byte) {
    JcCommand c;
    c.cls = cls;
    c.ok = true;
    c.realtime = true;
    c.text[0] = byte;
    c.text[1] = '\0';
    return c;
}

inline bool axis_ok(const JcMachineProfile& p, uint8_t axis) {
    return axis < 4 && p.axes[axis].letter != '\0';
}

// Avoid printing "-0.000".
inline float tidy(float v) { return fabsf(v) < 0.0005f ? 0.0f : v; }

// Allow only plain path characters; rejects anything that could smuggle a
// second command, a parameter assignment, or a traversal.
inline bool path_ok(const char* path) {
    if (path == nullptr || path[0] != '/' || strlen(path) > 80) {
        return false;
    }
    for (const char* p = path; *p; ++p) {
        const char c = *p;
        const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                          || c == '/' || c == '.' || c == '_' || c == '-' || c == ' ';
        if (!good) {
            return false;
        }
    }
    return strstr(path, "..") == nullptr;
}

}  // namespace detail

// $J=G91 G21 F<feed> <axis><signed distance>. Rotary distance is in degrees.
inline JcCommand jc_jog_incremental(const JcMachineProfile& p, uint8_t axis,
                                    float signed_distance, float feed) {
    using namespace detail;
    if (!axis_ok(p, axis)) return fail(JcCommandClass::Jog, JcCmdError::BadAxis);
    const JcAxisProfile& a = p.axes[axis];
    if (a.max_step <= 0.0f) return fail(JcCommandClass::Jog, JcCmdError::JogDisabled);
    if (!isfinite(signed_distance) || signed_distance == 0.0f)
        return fail(JcCommandClass::Jog, JcCmdError::BadValue);
    if (fabsf(signed_distance) > a.max_step)
        return fail(JcCommandClass::Jog, p.test_mode ? JcCmdError::TestLimit : JcCmdError::BadValue);
    if (!isfinite(feed) || feed < 1.0f) return fail(JcCommandClass::Jog, JcCmdError::BadFeed);
    if (feed > a.max_feed)
        return fail(JcCommandClass::Jog, p.test_mode ? JcCmdError::TestLimit : JcCmdError::BadFeed);
    JcCommand c = make(JcCommandClass::Jog, "$J=G91G21F%.0f%c%.3f", feed, a.letter, tidy(signed_distance));
    c.axis = axis;
    return c;
}

inline JcCommand jc_jog_cancel() { return detail::realtime(JcCommandClass::JogCancel, static_cast<char>(0x85)); }
inline JcCommand jc_feed_hold() { return detail::realtime(JcCommandClass::Hold, '!'); }
inline JcCommand jc_cycle_resume() { return detail::realtime(JcCommandClass::Job, '~'); }
inline JcCommand jc_soft_reset() { return detail::realtime(JcCommandClass::Reset, static_cast<char>(0x18)); }
inline JcCommand jc_unlock() { return detail::make(JcCommandClass::Unlock, "$X"); }

// Home all axes (axis == 0xFF) or a single axis ("$HX").
inline JcCommand jc_home(const JcMachineProfile& p, uint8_t axis) {
    using namespace detail;
    if (axis == 0xFF) return make(JcCommandClass::Home, "$H");
    if (!axis_ok(p, axis)) return fail(JcCommandClass::Home, JcCmdError::BadAxis);
    if (!p.axes[axis].homing_allowed) return fail(JcCommandClass::Home, JcCmdError::Unsupported);
    return make(JcCommandClass::Home, "$H%c", p.axes[axis].letter);
}

// Work coordinate system G54..G59, index 0..5.
inline JcCommand jc_select_wcs(uint8_t index) {
    if (index > 5) return detail::fail(JcCommandClass::WorkOffset, JcCmdError::BadIndex);
    return detail::make(JcCommandClass::WorkOffset, "G%u", 54u + index);
}

// Sets the current position of one axis to `value` in the active WCS (P0).
inline JcCommand jc_set_axis_value(const JcMachineProfile& p, uint8_t axis, float value) {
    using namespace detail;
    if (!axis_ok(p, axis)) return fail(JcCommandClass::WorkOffset, JcCmdError::BadAxis);
    if (!isfinite(value) || fabsf(value) > 100000.0f) return fail(JcCommandClass::WorkOffset, JcCmdError::BadValue);
    return make(JcCommandClass::WorkOffset, "G10 L20 P0 %c%.3f", p.axes[axis].letter, tidy(value));
}

inline JcCommand jc_zero_axis(const JcMachineProfile& p, uint8_t axis) {
    return jc_set_axis_value(p, axis, 0.0f);
}

// Single probe move toward the workpiece. The rotary axis is never probed.
inline JcCommand jc_probe_move(const JcMachineProfile& p, uint8_t axis, float signed_travel, float feed) {
    using namespace detail;
    if (!axis_ok(p, axis)) return fail(JcCommandClass::Probe, JcCmdError::BadAxis);
    if (!p.axes[axis].probe_allowed || p.axes[axis].rotary) return fail(JcCommandClass::Probe, JcCmdError::Unsupported);
    if (!isfinite(signed_travel) || signed_travel == 0.0f || fabsf(signed_travel) > p.max_probe_travel)
        return fail(JcCommandClass::Probe, JcCmdError::BadValue);
    if (!isfinite(feed) || feed < 1.0f || feed > p.max_probe_feed)
        return fail(JcCommandClass::Probe, JcCmdError::BadFeed);
    JcCommand c = make(JcCommandClass::Probe, "G21 G91 G38.2 F%.0f %c%.3f", feed, p.axes[axis].letter,
                       tidy(signed_travel));
    c.axis = axis;
    return c;
}

// Standard Z tool-setter / touch-plate sequence. Steps 2.. run only if the
// probe reported contact; the executor must abort on any failure.
inline JcCommandSequence jc_probe_z_sequence(const JcMachineProfile& p, float travel, float feed,
                                             float retract, float plate_thickness) {
    JcCommandSequence s;
    if (!isfinite(retract) || retract < 0.0f || retract > p.max_probe_travel
        || !isfinite(plate_thickness) || fabsf(plate_thickness) > 100.0f) {
        s.error = JcCmdError::BadValue;
        return s;
    }
    JcCommand probe = jc_probe_move(p, 2, -fabsf(travel), feed);
    if (!probe.ok) {
        s.error = probe.error;
        return s;
    }
    s.steps[0] = probe;
    s.steps[1] = detail::make(JcCommandClass::WorkOffset, "G10 L20 P0 Z%.3f", detail::tidy(plate_thickness));
    s.steps[2] = detail::make(JcCommandClass::Probe, "G91 G0 Z%.3f", detail::tidy(retract));
    s.steps[2].axis = 2;
    s.steps[3] = detail::make(JcCommandClass::Probe, "G90");
    s.count = 4;
    s.ok = s.steps[1].ok && s.steps[2].ok && s.steps[3].ok;
    return s;
}

// Continuous jog: a long incremental move that the operator cancels on release
// with the real-time jog-cancel byte (0x85), as upstream MultiJogScene does.
// Not generated for the rotary axis: it is not safely supported.
inline JcCommand jc_jog_continuous(const JcMachineProfile& p, uint8_t axis, int direction, float feed) {
    using namespace detail;
    if (!axis_ok(p, axis)) return fail(JcCommandClass::Jog, JcCmdError::BadAxis);
    const JcAxisProfile& a = p.axes[axis];
    if (a.rotary) return fail(JcCommandClass::Jog, JcCmdError::Unsupported);
    if (p.test_mode) return fail(JcCommandClass::Jog, JcCmdError::Unsupported);  // continuous jog stays disabled
    if (direction != 1 && direction != -1) return fail(JcCommandClass::Jog, JcCmdError::BadValue);
    if (!isfinite(feed) || feed < 1.0f || feed > a.max_feed) return fail(JcCommandClass::Jog, JcCmdError::BadFeed);
    // Travel the controller reported when known; otherwise the placeholder rule.
    const float distance = a.travel_mm > 0.0f ? a.travel_mm : 10.0f * a.max_step;
    JcCommand c = make(JcCommandClass::Jog, "$J=G91G21F%.0f%c%.0f", feed, a.letter, direction * distance);
    c.axis = axis;
    return c;
}

// Edge probe on X or Y: probe toward the edge, set the axis to the tool radius
// offset, retract, restore absolute mode. direction is the probing direction.
// If the probe moves +X it touches a left edge, so the tool centre is one
// radius short of it: X := -radius (and +radius for a -X probe).
inline JcCommandSequence jc_probe_edge_sequence(const JcMachineProfile& p, uint8_t axis, int direction,
                                                float travel, float feed, float retract, float tool_diameter) {
    JcCommandSequence s;
    if (axis > 1 || (direction != 1 && direction != -1)) {
        s.error = axis > 1 ? JcCmdError::Unsupported : JcCmdError::BadValue;
        return s;
    }
    if (!isfinite(retract) || retract < 0.0f || retract > p.max_probe_travel
        || !isfinite(tool_diameter) || tool_diameter < 0.0f || tool_diameter > 100.0f
        || !isfinite(travel) || travel <= 0.0f) {
        s.error = JcCmdError::BadValue;
        return s;
    }
    JcCommand probe = jc_probe_move(p, axis, direction * travel, feed);
    if (!probe.ok) {
        s.error = probe.error;
        return s;
    }
    const char letter = p.axes[axis].letter;
    s.steps[0] = probe;
    s.steps[1] = detail::make(JcCommandClass::WorkOffset, "G10 L20 P0 %c%.3f", letter,
                              detail::tidy(-direction * tool_diameter / 2.0f));
    s.steps[2] = detail::make(JcCommandClass::Probe, "G91 G0 %c%.3f", letter, detail::tidy(-direction * retract));
    s.steps[2].axis = axis;
    s.steps[3] = detail::make(JcCommandClass::Probe, "G90");
    s.count = 4;
    s.ok = s.steps[1].ok && s.steps[2].ok && s.steps[3].ok;
    return s;
}

inline JcCommand jc_run_sd_file(const char* path) {
    if (!detail::path_ok(path)) return detail::fail(JcCommandClass::Job, JcCmdError::BadPath);
    return detail::make(JcCommandClass::Job, "$SD/Run=%s", path);
}

inline JcCommand jc_run_macro_file(const char* path) {
    if (!detail::path_ok(path)) return detail::fail(JcCommandClass::Macro, JcCmdError::BadPath);
    return detail::make(JcCommandClass::Macro, "$Localfs/Run=%s", path);
}

inline JcCommand jc_spindle_stop() { return detail::make(JcCommandClass::Spindle, "M5"); }

}  // namespace jc8048w550
