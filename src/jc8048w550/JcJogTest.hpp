// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// First-live-jog TEST support (Instructions 006 / 006A). Pure logic shared by the
// experimental live-jog build and the host tests; nothing here transmits by
// itself. It provides:
//
//   * the restricted test limits (X/Y 1 mm @ 100 mm/min only; Z on hold, A disabled)
//     and jc_profile_apply_test_limits();
//   * jc_jog_gate(): per-axis PROTECTED / UNPROTECTED MICRO-JOG / BLOCKED, from a
//     current capability read, with no assumption that the machine position is valid;
//   * jc_jog_line_allowed(): a strict, independent check of the one jog line
//     the transport may ever carry in the live-jog build (defence in depth: the
//     transport re-validates what the builder generated);
//   * JcJogSession: ONE jog at a time with distinct states for acknowledgement,
//     controller error, alarm, timeout, motion completion, cancellation and
//     lost / stale connection, jog cancellation (0x85) that never depends on
//     arming, rejection of new jogs while anything is unresolved, and no replay.
//
// The limits are proposed conservative TEST values, not values established as
// safe for every set-up. A configured travel distance is never treated as a
// safe movement envelope: the operator must verify clear travel before every jog.

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "JcCommandBuilder.hpp"
#include "JcCommandExec.hpp"
#include "JcMachineCaps.hpp"
#include "JcResponse.hpp"

namespace jc8048w550 {

// ---- limits ---------------------------------------------------------------

struct JcJogTestLimits {
    float max_step[4];  // mm; 0 = jogging disabled for the axis
    float max_feed[4];  // mm/min
};

// First live motion is X and Y only, 1 mm at 100 mm/min at most.
//   Z: ON HOLD. Its homing direction (positive) and limit-pin side (negative only)
//      need owner / physical verification, and a Z fault has higher consequence.
//   A: disabled pending rotary calibration.
inline JcJogTestLimits jc_test_jog_limits() {
    JcJogTestLimits l;
    l.max_step[0] = 1.0f;  l.max_feed[0] = 100.0f;  // X
    l.max_step[1] = 1.0f;  l.max_feed[1] = 100.0f;  // Y
    l.max_step[2] = 0.0f;  l.max_feed[2] = 0.0f;    // Z: HOLD
    l.max_step[3] = 0.0f;  l.max_feed[3] = 0.0f;    // A: disabled
    return l;
}

// Clamps the profile to the test limits (never raises a limit) and marks it as a
// test profile. Applied AFTER jc_profile_apply_caps(), so the controller's travel
// can only ever lower, never widen, the allowed step.
inline void jc_profile_apply_test_limits(JcMachineProfile& p) {
    const JcJogTestLimits l = jc_test_jog_limits();
    for (uint8_t i = 0; i < 4; ++i) {
        JcAxisProfile& a = p.axes[i];
        if (l.max_step[i] < a.max_step || l.max_step[i] <= 0.0f) a.max_step = l.max_step[i];
        if (l.max_feed[i] < a.max_feed || l.max_feed[i] <= 0.0f) a.max_feed = l.max_feed[i];
    }
    p.test_mode = true;
}

// ---- jog gate: what the controller does and does not protect ---------------------------
//
// Real controller (read 2026-10-30): soft limits OFF, hard limits OFF, start/must_home
// false. A configured travel, a configured limit input and a configured homing cycle
// are NOT protection and NOT proof of a valid position:
//
//   * max_travel_mm is a configuration value, not a measured remaining path;
//   * a limit input that is configured is not hard-limit protection unless
//     hard_limits is ON, and even then it only helps if the position is valid;
//   * a homing cycle that is configured does not mean the machine has homed;
//   * start/must_home is NOT used as a jog gate (jogging is not governed by it).
//
// Modes, per axis:
//   Protected         travel protection AND a valid position are both established.
//                     Not reachable in this firmware: nothing it can read says that the
//                     machine is homed, so position trust is never Established.
//   UnprotectedMicro  the capability values are known and current but no trusted travel
//                     protection exists. Only the tightly bounded test jog is possible,
//                     after a deliberate operator acknowledgement.
//   Blocked           anything unknown, refused, stale, not read, or an excluded axis.

enum class JcJogGateMode : uint8_t { Blocked, UnprotectedMicro, Protected };

enum class JcJogGateReason : uint8_t {
    None,
    AxisExcluded,   // Z on hold, A disabled: not part of the first-motion policy
    NotRead,        // no completed capability read in this LIVE session
    Stale,          // the read is older than kJcCapsMaxAgeMs
    SoftUnknown,    // soft-limit state not known (not read, refused or unreadable)
    HardUnknown,    // hard-limit state not known
    PinUnknown,     // a limit-pin setting for the axis is not known
};

struct JcJogGate {
    JcJogGateMode   mode;
    JcJogGateReason reason;
};

// The capability read must be recent. A reconnect also discards it (JcApp), so a new
// read is needed in every LIVE session.
constexpr uint32_t kJcCapsMaxAgeMs = 5u * 60u * 1000u;

enum class JcPositionTrust : uint8_t { Untrusted, Established };

// Nothing the pendant can read establishes that the machine position is valid:
// must_home=false does not imply it, a homing cycle being configured does not imply it,
// and numeric MPos values after boot are not a reference. So this is always Untrusted.
// It is a function (not a constant) so the policy has one place to change and one place
// to test, and so the tests can show that no combination of values changes it.
inline JcPositionTrust jc_position_trust(const JcMachineCaps&) { return JcPositionTrust::Untrusted; }

// Whether the UI may assert anything about remaining path from MPos / max_travel_mm /
// home MPOS. Only with an established position. Today: never.
enum class JcPathClaim : uint8_t { None, Assessed };

inline JcPathClaim jc_path_claim(const JcMachineCaps&, uint8_t, JcPositionTrust trust) {
    return trust == JcPositionTrust::Established ? JcPathClaim::Assessed : JcPathClaim::None;
}

inline JcJogGate jc_jog_gate(const JcMachineCaps& caps, bool read_complete, uint32_t now_ms, uint8_t axis,
                             JcPositionTrust trust) {
    JcJogGate g;
    g.mode = JcJogGateMode::Blocked;
    g.reason = JcJogGateReason::None;
    const JcJogTestLimits lim = jc_test_jog_limits();
    if (axis >= 4 || lim.max_step[axis] <= 0.0f) {
        g.reason = JcJogGateReason::AxisExcluded;
        return g;
    }
    if (!read_complete || !caps.complete) {
        g.reason = JcJogGateReason::NotRead;
        return g;
    }
    if (static_cast<uint32_t>(now_ms - caps.completed_ms) > kJcCapsMaxAgeMs) {
        g.reason = JcJogGateReason::Stale;
        return g;
    }
    const JcAxisCaps& a = caps.axes[axis];
    const JcTri soft = a.soft_limits();
    if (soft == JcTri::Unknown) {
        g.reason = JcJogGateReason::SoftUnknown;
        return g;
    }
    if (a.hard_limits() == JcTri::Unknown) {
        g.reason = JcJogGateReason::HardUnknown;
        return g;
    }
    const JcQueryField pins[3] = { JcQueryField::LimitNegPin, JcQueryField::LimitPosPin, JcQueryField::LimitAllPin };
    for (const JcQueryField f : pins) {
        if (!a.valid(f)) {
            g.reason = JcJogGateReason::PinUnknown;
            return g;
        }
    }
    // Everything needed is known and current. Protected needs travel protection AND a valid
    // position; soft limits being ON without a valid position protect nothing trustworthy.
    if (trust == JcPositionTrust::Established && soft == JcTri::Yes) {
        g.mode = JcJogGateMode::Protected;
    } else {
        g.mode = JcJogGateMode::UnprotectedMicro;
    }
    return g;
}

// The two masks the authorization context carries: axes whose limit settings are known from a
// CURRENT read (mode is not Blocked), and axes with trusted travel protection (Protected).
// Used by the firmware's dispatch context and by the host tests, so they cannot disagree.
inline void jc_jog_gate_masks(const JcMachineCaps& caps, bool read_complete, uint32_t now_ms, uint8_t& envelope_mask,
                              uint8_t& protected_mask) {
    envelope_mask = 0;
    protected_mask = 0;
    const JcPositionTrust trust = jc_position_trust(caps);
    for (uint8_t axis = 0; axis < 4; ++axis) {
        const JcJogGate g = jc_jog_gate(caps, read_complete, now_ms, axis, trust);
        if (g.mode != JcJogGateMode::Blocked) envelope_mask |= static_cast<uint8_t>(1u << axis);
        if (g.mode == JcJogGateMode::Protected) protected_mask |= static_cast<uint8_t>(1u << axis);
    }
}

inline const char* jc_jog_gate_mode_name(JcJogGateMode m) {
    switch (m) {
        case JcJogGateMode::Blocked: return "BLOCKED";
        case JcJogGateMode::UnprotectedMicro: return "UNPROTECTED MICRO-JOG";
        case JcJogGateMode::Protected: return "PROTECTED";
    }
    return "?";
}

inline const char* jc_jog_gate_reason_text(JcJogGateReason r) {
    switch (r) {
        case JcJogGateReason::None: return "ok";
        case JcJogGateReason::AxisExcluded: return "axis not part of the first-motion test (Z on hold, A disabled)";
        case JcJogGateReason::NotRead: return "no capability read in this LIVE session";
        case JcJogGateReason::Stale: return "the capability read is too old; read again";
        case JcJogGateReason::SoftUnknown: return "soft-limit state is UNKNOWN";
        case JcJogGateReason::HardUnknown: return "hard-limit state is UNKNOWN";
        case JcJogGateReason::PinUnknown: return "a limit-pin setting is UNKNOWN";
    }
    return "?";
}

// The two facts that must never be merged on screen: is a limit INPUT configured, and
// is HARD LIMIT PROTECTION enabled. A configured input with protection OFF does not stop
// an ordinary jog.
inline void jc_limit_wording(const JcAxisCaps& a, char* input, size_t input_size, char* protection,
                             size_t protection_size) {
    static const char* const kSide[3] = { "neg", "pos", "all" };
    const JcQueryField pins[3] = { JcQueryField::LimitNegPin, JcQueryField::LimitPosPin, JcQueryField::LimitAllPin };
    int unknown = 0, assigned = 0;
    char list[64] = "";
    for (uint8_t s = 0; s < 3; ++s) {
        if (!a.valid(pins[s])) {
            ++unknown;
        } else if (a.get(pins[s]) != 0.0f) {
            ++assigned;
            const size_t used = strlen(list);
            snprintf(list + used, sizeof(list) - used, "%s%s %s", used ? ", " : "", kSide[s], a.pin[s]);
        }
    }
    if (unknown > 0) {
        snprintf(input, input_size, "LIMIT INPUT UNKNOWN");
    } else if (assigned > 0) {
        snprintf(input, input_size, "LIMIT INPUT CONFIGURED (%s)", list);
    } else {
        snprintf(input, input_size, "NO LIMIT INPUT ASSIGNED");
    }
    const JcTri hard = a.hard_limits();
    snprintf(protection, protection_size, "HARD LIMIT PROTECTION %s",
             hard == JcTri::Yes ? "ON" : (hard == JcTri::No ? "OFF" : "UNKNOWN"));
}

// Axes grouped by (limit input, hard-limit protection), one line per group, e.g.
//   "X Y Z: LIMIT INPUT CONFIGURED, HARD LIMIT PROTECTION OFF"
//   "A: NO LIMIT INPUT ASSIGNED, HARD LIMIT PROTECTION OFF"
// At most `max_lines` lines are written; if there are more groups the last line says so.
// Returns the number of lines. Used by the Limits page so the two facts are never merged.
inline uint8_t jc_limits_summary(const JcMachineCaps& caps, char lines[][96], uint8_t max_lines) {
    char keys[4][2][72] = {};
    char names[4][8] = {};
    uint8_t groups = 0;
    for (uint8_t axis = 0; axis < 4; ++axis) {
        char input[72], protection[48];
        jc_limit_wording(caps.axes[axis], input, sizeof(input), protection, sizeof(protection));
        // Drop the pin detail: the grouping is about the fact, not the pin name.
        char* paren = strchr(input, '(');
        if (paren != nullptr) {
            *(paren > input && paren[-1] == ' ' ? paren - 1 : paren) = '\0';
        }
        uint8_t g = 0;
        while (g < groups && !(strcmp(keys[g][0], input) == 0 && strcmp(keys[g][1], protection) == 0)) ++g;
        if (g == groups) {
            snprintf(keys[g][0], sizeof(keys[g][0]), "%s", input);
            snprintf(keys[g][1], sizeof(keys[g][1]), "%s", protection);
            ++groups;
        }
        const size_t used = strlen(names[g]);
        snprintf(names[g] + used, sizeof(names[g]) - used, "%s%c", used ? " " : "", "XYZA"[axis]);
    }
    uint8_t written = 0;
    for (uint8_t g = 0; g < groups && written < max_lines; ++g) {
        if (written + 1 == max_lines && g + 1 < groups) {
            snprintf(lines[written++], 96, "(other axes differ: see the table)");
            break;
        }
        snprintf(lines[written++], 96, "%s: %s, %s", names[g], keys[g][0], keys[g][1]);
    }
    return written;
}

// ---- strict jog line ---------------------------------------------------------

struct JcJogRequest {
    uint8_t axis;
    float   distance;  // signed
    float   feed;
};

// Parses exactly what jc_jog_incremental() formats:  $J=G91G21F<feed><X|Y|Z|A><-?d.ddd>
// Anything else (extra words, a second axis, spaces, exponents) is rejected.
inline bool jc_parse_jog_line(const char* text, JcJogRequest& out) {
    static const char kPrefix[] = "$J=G91G21F";
    if (text == nullptr || strncmp(text, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
    const char* p = text + sizeof(kPrefix) - 1;
    uint32_t feed = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 5) return false;
        feed = feed * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (digits == 0) return false;
    uint8_t axis = 0xFF;
    switch (*p) {
        case 'X': axis = 0; break;
        case 'Y': axis = 1; break;
        case 'Z': axis = 2; break;
        case 'A': axis = 3; break;
        default: return false;
    }
    ++p;
    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    }
    uint32_t whole = 0;
    digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 4) return false;
        whole = whole * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (digits == 0 || *p != '.') return false;
    ++p;
    uint32_t frac = 0;
    for (int i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') return false;
        frac = frac * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (*p != '\0') return false;
    float distance = static_cast<float>(whole) + static_cast<float>(frac) / 1000.0f;
    out.axis = axis;
    out.distance = negative ? -distance : distance;
    out.feed = static_cast<float>(feed);
    return true;
}

inline bool jc_jog_line_allowed(const char* text, const JcJogTestLimits& l) {
    JcJogRequest r;
    if (!jc_parse_jog_line(text, r)) return false;
    if (r.axis > 3 || l.max_step[r.axis] <= 0.0f) return false;
    const float mag = fabsf(r.distance);
    if (mag <= 0.0f || mag > l.max_step[r.axis] + 0.0005f) return false;
    return r.feed >= 1.0f && r.feed <= l.max_feed[r.axis] + 0.5f;
}

// ---- session -------------------------------------------------------------------

constexpr uint32_t kJcJogDebounceMs = 400;       // minimum time between jog starts (touch bounce)
constexpr uint32_t kJcJogSettleMs = 300;         // minimum time after a jog resolves before the next
constexpr uint32_t kJcJogCancelConfirmMs = 3000; // cancel must be seen to reach Idle within this
constexpr uint32_t kJcJogMotionMarginMs = 5000;  // allowed on top of distance / feed
constexpr uint32_t kJcJogArmTtlMs = 120000;      // arming lapses by itself
constexpr float    kJcJogPositionToleranceMm = 0.05f;
// A "Jog" state that appears while the session believes nothing is in flight, this
// soon after a jog line was sent, is treated as our own late-starting jog (e.g. a
// cancel that overtook the line in the controller's input path); later than this it
// is foreign motion (another client) and is only counted, never cancelled by us.
constexpr uint32_t kJcJogLateStartWindowMs = 5000;

enum class JcJogPhase : uint8_t {
    Idle,         // nothing outstanding
    AwaitingAck,  // jog line sent; waiting for ok / error:N / ALARM:N
    Accepted,     // "ok" received; motion not yet observed
    Moving,       // status showed Jog / Run after the acknowledgement
    Cancelling,   // 0x85 sent; waiting for the controller to report Idle
    Recovering,   // link lost / stale: waiting for fresh Idle status after the quiet period
};

enum class JcJogResult : uint8_t {
    None,
    Completed,         // acknowledged, motion finished, controller Idle, position as expected
    Rejected,          // error:N (nothing moved), or an error after acceptance (cancelled)
    Alarm,             // ALARM:N or the machine entered Alarm
    AckTimeout,        // no reply: state unknown, cancel sent
    MotionTimeout,     // acknowledged but never returned to Idle in time, cancel sent
    StaleStatus,       // status went stale while unresolved, cancel attempted
    LinkLost,          // connection lost while unresolved
    Cancelled,         // operator cancel resolved to Idle
    CancelUnconfirmed, // cancel sent but Idle not seen in time: use the physical E-stop
    SendFailed,        // the transport could not hand the line over
    PositionMismatch,  // finished Idle but the reported position is not where it should be
    UnexpectedMotion,  // the machine started jogging after we believed the operation was over
};

enum class JcJogReject : uint8_t {
    None,
    NoTransport,          // no transmit path (ordinary firmware)
    NotIdle,              // a previous operation is unresolved
    ResultUnacknowledged, // a failure result has not been acknowledged by the operator
    Debounce,             // too soon after the previous jog (touch bounce)
    NoFreshStatus,
    MachineNotIdle,
    ResyncWait,           // quiet period after a timeout / cancel
};

inline const char* jc_jog_phase_name(JcJogPhase p) {
    switch (p) {
        case JcJogPhase::Idle: return "idle";
        case JcJogPhase::AwaitingAck: return "awaiting ok";
        case JcJogPhase::Accepted: return "accepted (ok), waiting for motion";
        case JcJogPhase::Moving: return "moving";
        case JcJogPhase::Cancelling: return "cancelling";
        case JcJogPhase::Recovering: return "recovering connection";
    }
    return "?";
}

inline const char* jc_jog_result_name(JcJogResult r) {
    switch (r) {
        case JcJogResult::None: return "no jog yet";
        case JcJogResult::Completed: return "COMPLETED";
        case JcJogResult::Rejected: return "REJECTED by controller";
        case JcJogResult::Alarm: return "ALARM";
        case JcJogResult::AckTimeout: return "NO REPLY (timeout), cancel sent";
        case JcJogResult::MotionTimeout: return "MOTION TIMEOUT, cancel sent";
        case JcJogResult::StaleStatus: return "STATUS STALE, cancel attempted";
        case JcJogResult::LinkLost: return "CONNECTION LOST";
        case JcJogResult::Cancelled: return "CANCELLED";
        case JcJogResult::CancelUnconfirmed: return "CANCEL NOT CONFIRMED - USE E-STOP";
        case JcJogResult::SendFailed: return "SEND FAILED";
        case JcJogResult::PositionMismatch: return "POSITION MISMATCH";
        case JcJogResult::UnexpectedMotion: return "UNEXPECTED MOTION, cancel sent";
    }
    return "?";
}

inline const char* jc_jog_reject_name(JcJogReject r) {
    switch (r) {
        case JcJogReject::None: return "ok";
        case JcJogReject::NoTransport: return "no transmit path in this build";
        case JcJogReject::NotIdle: return "a previous operation is still unresolved";
        case JcJogReject::ResultUnacknowledged: return "acknowledge the previous result first";
        case JcJogReject::Debounce: return "too soon after the previous jog";
        case JcJogReject::NoFreshStatus: return "no fresh machine status";
        case JcJogReject::MachineNotIdle: return "machine is not Idle";
        case JcJogReject::ResyncWait: return "waiting for the controller to settle";
    }
    return "?";
}

enum class JcJogPosCheck : uint8_t { NotChecked, Match, Mismatch };

// One status frame, as the application sees it. `sequence` must change with every
// new frame: the session never acts twice on the same frame, and it only trusts
// frames that arrived AFTER the event it is waiting for.
struct JcJogStatus {
    bool        fresh = false;
    const char* state = "";
    uint32_t    sequence = 0;
    bool        pos_valid = false;
    float       pos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};

class JcJogSession {
public:
    using TxFn = bool (*)(void* user, const char* bytes, size_t length);

    explicit JcJogSession(TxFn tx = nullptr, void* user = nullptr) : _tx(tx), _user(user) {}

    void set_clock(uint32_t now) { _now = now; }

    // Why a new jog cannot start now (None = it may). Checked again, independently,
    // when the authorized line reaches the sink.
    JcJogReject can_begin(uint32_t now) const {
        if (_tx == nullptr) return JcJogReject::NoTransport;
        if (_phase != JcJogPhase::Idle) return JcJogReject::NotIdle;
        if (_needs_ack) return JcJogReject::ResultUnacknowledged;
        if (_had_begin && static_cast<uint32_t>(now - _last_begin_ms) < kJcJogDebounceMs) return JcJogReject::Debounce;
        if (_had_resolved && static_cast<uint32_t>(now - _last_resolved_ms) < kJcJogSettleMs) return JcJogReject::Debounce;
        if (!_fresh) return JcJogReject::NoFreshStatus;
        if (strcmp(_state, "Idle") != 0) return JcJogReject::MachineNotIdle;
        if (!_exec.ready(now)) return JcJogReject::ResyncWait;
        return JcJogReject::None;
    }

    // JcCommandSinkEx: called by the gate for an AUTHORIZED command only.
    static bool sink(void* self, JcCommandClass cls, const char* text) {
        JcJogSession* s = static_cast<JcJogSession*>(self);
        if (cls == JcCommandClass::JogCancel) return s->cancel(s->_now);
        if (cls != JcCommandClass::Jog) {
            ++s->_refused_class;
            return false;
        }
        return s->begin_jog(text);
    }

    // Operator (or the application) cancels. ALWAYS attempted, whatever the phase:
    // stopping must not depend on arming, freshness or a free executor.
    bool cancel(uint32_t now) {
        _now = now;
        if (_tx == nullptr) return false;
        const char byte = static_cast<char>(0x85);
        const bool sent = _tx(_user, &byte, 1);
        if (sent) ++_cancels_sent;
        if (_phase == JcJogPhase::AwaitingAck || _phase == JcJogPhase::Accepted || _phase == JcJogPhase::Moving) {
            if (_phase == JcJogPhase::AwaitingAck) _exec.abandon(now);
            _exec.release();
            enter_cancelling(JcJogResult::Cancelled, false, now);
        } else if (_phase == JcJogPhase::Cancelling) {
            _cancel_ms = now;  // sent again: restart the confirmation window
            _cancel_seq = _last_seq;
        }
        return sent;
    }

    void on_line(const char* line, uint32_t now) {
        _now = now;
        const JcResponse r = jc_classify_line(line);
        switch (_phase) {
            case JcJogPhase::AwaitingAck:
                if (_exec.on_response(r, now)) {
                    const JcExecOutcome o = _exec.outcome();
                    const int code = _exec.code();
                    _exec.release();
                    if (o == JcExecOutcome::Acked) {
                        _phase = JcJogPhase::Accepted;
                        _seq_at_ack = _last_seq;
                        _ack_ms = now;
                        ++_acked;
                    } else if (o == JcExecOutcome::ControllerError) {
                        _code = code;
                        finish(JcJogResult::Rejected, false, now);  // nothing was started
                        ++_rejected;
                    } else if (o == JcExecOutcome::Alarm) {
                        _code = code;
                        finish(JcJogResult::Alarm, true, now);
                    }
                }
                break;
            case JcJogPhase::Accepted:
            case JcJogPhase::Moving:
                _exec.on_response(r, now);  // counts a stray reply as unsolicited
                if (r.kind == JcRespKind::Alarm) {
                    _code = r.code;
                    finish(JcJogResult::Alarm, true, now);  // the controller has already stopped
                } else if (r.kind == JcRespKind::Error) {
                    _code = r.code;
                    send_cancel_byte();  // an error after acceptance: stop and require acknowledgement
                    enter_cancelling(JcJogResult::Rejected, true, now);
                }
                break;
            case JcJogPhase::Cancelling:
                _exec.on_response(r, now);
                if (r.kind == JcRespKind::Alarm) {
                    _code = r.code;
                    finish(JcJogResult::Alarm, true, now);
                }
                break;
            default:
                _exec.on_response(r, now);
                if (r.kind == JcRespKind::Alarm) ++_unexpected_alarms;
                break;
        }
    }

    void on_status(const JcJogStatus& s, uint32_t now) {
        _now = now;
        _fresh = s.fresh;
        if (!s.fresh) return;
        strncpy(_state, s.state != nullptr ? s.state : "", sizeof(_state) - 1);
        _state[sizeof(_state) - 1] = '\0';
        if (s.sequence == _last_seq && _have_seq) return;  // never act twice on one frame
        _last_seq = s.sequence;
        _have_seq = true;
        _pos_valid = s.pos_valid;
        memcpy(_pos, s.pos, sizeof(_pos));
        const bool alarm = strcmp(_state, "Alarm") == 0;
        const bool idle = strcmp(_state, "Idle") == 0;
        const bool moving = strcmp(_state, "Jog") == 0 || strcmp(_state, "Run") == 0;

        if (alarm && _phase != JcJogPhase::Idle && _phase != JcJogPhase::Recovering) {
            if (_phase == JcJogPhase::AwaitingAck) {
                _exec.abandon(now);
                _exec.release();
            }
            finish(JcJogResult::Alarm, true, now);
            return;
        }
        switch (_phase) {
            case JcJogPhase::Accepted:
            case JcJogPhase::Moving:
                if (_last_seq == _seq_at_ack) return;  // not newer than the acknowledgement
                if (moving) {
                    _saw_motion = true;
                    _phase = JcJogPhase::Moving;
                } else if (idle) {
                    ++_idle_after_ack;
                    check_complete(now);
                }
                break;
            case JcJogPhase::Cancelling:
                if (idle && _last_seq != _cancel_seq) {
                    finish(_cancel_reason, _cancel_latch, now);
                }
                break;
            case JcJogPhase::Recovering:
                if (idle && static_cast<int32_t>(now - _recover_ready_ms) >= 0) {
                    _phase = JcJogPhase::Idle;
                    _last_resolved_ms = now;
                    _had_resolved = true;
                }
                break;
            case JcJogPhase::Idle:
                if (strcmp(_state, "Jog") == 0) {
                    if (_had_begin && static_cast<uint32_t>(now - _last_begin_ms) <= kJcJogLateStartWindowMs) {
                        send_cancel_byte();  // our own jog started late: stop it and require acknowledgement
                        enter_cancelling(JcJogResult::UnexpectedMotion, true, now);
                        ++_unexpected_motion;
                    } else {
                        ++_foreign_motion;   // someone else's jog: never cancelled by us
                    }
                }
                break;
            default:
                break;
        }
    }

    void tick(uint32_t now) {
        _now = now;
        switch (_phase) {
            case JcJogPhase::AwaitingAck:
                _exec.tick(now);
                if (_exec.state() == JcExecState::Finished && _exec.outcome() == JcExecOutcome::TimedOut) {
                    _exec.release();
                    ++_timeouts;
                    send_cancel_byte();
                    enter_cancelling(JcJogResult::AckTimeout, true, now);
                }
                break;
            case JcJogPhase::Accepted:
            case JcJogPhase::Moving:
                if (static_cast<uint32_t>(now - _ack_ms) >= _motion_timeout_ms) {
                    ++_timeouts;
                    send_cancel_byte();
                    enter_cancelling(JcJogResult::MotionTimeout, true, now);
                }
                break;
            case JcJogPhase::Cancelling:
                if (!_cancel_unconfirmed && static_cast<uint32_t>(now - _cancel_ms) >= kJcJogCancelConfirmMs) {
                    _cancel_unconfirmed = true;
                    _needs_ack = true;
                    _result = JcJogResult::CancelUnconfirmed;
                }
                break;
            default:
                break;
        }
    }

    // The status stream went stale or the connection dropped. can_send: the socket
    // is still open (stale data), so a cancel may still get through.
    void on_link_lost(uint32_t now, bool can_send) {
        _now = now;
        _fresh = false;
        _state[0] = '\0';
        _have_seq = false;
        if (_phase == JcJogPhase::Idle) return;
        if (can_send && _tx != nullptr) {
            send_cancel_byte();
        }
        _exec.on_link_lost(now);
        _exec.release();
        _phase = JcJogPhase::Recovering;
        _result = can_send ? JcJogResult::StaleStatus : JcJogResult::LinkLost;
        _needs_ack = true;
        _recover_ready_ms = now + kJcResyncQuietMs;
        _cancel_unconfirmed = false;
        ++_losses;
    }

    // The operator has seen a failure result. Only possible once nothing is in flight.
    bool acknowledge() {
        if (_phase != JcJogPhase::Idle) return false;
        _needs_ack = false;
        _cancel_unconfirmed = false;
        return true;
    }

    // True while a new motion must be refused: an operation is in flight, or a
    // failure has not been acknowledged.
    bool unresolved() const { return _phase != JcJogPhase::Idle || _needs_ack; }

    JcJogPhase    phase() const { return _phase; }
    JcJogResult   result() const { return _result; }
    int           code() const { return _code; }
    bool          needs_acknowledgement() const { return _needs_ack; }
    bool          cancel_unconfirmed() const { return _cancel_unconfirmed; }
    JcJogPosCheck pos_check() const { return _pos_check; }
    float         pos_delta() const { return _pos_delta; }
    const JcJogRequest& request() const { return _req; }
    bool          saw_motion() const { return _saw_motion; }

    uint32_t jogs_sent() const { return _jogs_sent; }
    uint32_t cancels_sent() const { return _cancels_sent; }
    uint32_t acked() const { return _acked; }
    uint32_t rejected() const { return _rejected; }
    uint32_t completed() const { return _completed; }
    uint32_t timeouts() const { return _timeouts; }
    uint32_t losses() const { return _losses; }
    uint32_t refused_policy() const { return _refused_policy; }
    uint32_t refused_busy() const { return _refused_busy; }
    uint32_t refused_class() const { return _refused_class; }
    uint32_t unexpected_alarms() const { return _unexpected_alarms; }
    uint32_t unexpected_motion() const { return _unexpected_motion; }
    uint32_t foreign_motion() const { return _foreign_motion; }
    uint32_t unsolicited() const { return _exec.unsolicited(); }

private:
    bool begin_jog(const char* text) {
        JcJogRequest req;
        if (!jc_parse_jog_line(text, req) || !jc_jog_line_allowed(text, jc_test_jog_limits())) {
            ++_refused_policy;  // not a jog the test configuration may ever send
            return false;
        }
        if (can_begin(_now) != JcJogReject::None) {
            ++_refused_busy;
            return false;
        }
        if (!_exec.begin(text, jc_command_timeout_ms(JcCommandClass::Jog), _now, false)) {
            ++_refused_busy;
            return false;
        }
        char line[kJcMaxCommandLength + 2];
        const size_t n = strlen(text);
        if (n + 1 >= sizeof(line)) {
            _exec.abandon(_now);
            _exec.release();
            ++_refused_policy;
            return false;
        }
        memcpy(line, text, n);
        line[n] = '\n';
        if (!_tx(_user, line, n + 1)) {
            // A partial line may have left the device: treat the stream as unusable.
            _exec.on_link_lost(_now);
            _exec.release();
            _phase = JcJogPhase::Recovering;
            _result = JcJogResult::SendFailed;
            _needs_ack = true;
            _recover_ready_ms = _now + kJcResyncQuietMs;
            return false;
        }
        _req = req;
        _result = JcJogResult::None;
        _code = 0;
        _phase = JcJogPhase::AwaitingAck;
        _start_valid = _pos_valid;
        memcpy(_start_pos, _pos, sizeof(_pos));
        _pos_check = JcJogPosCheck::NotChecked;
        _pos_delta = 0.0f;
        _saw_motion = false;
        _idle_after_ack = 0;
        _seq_at_ack = _last_seq;
        _last_begin_ms = _now;
        _had_begin = true;
        _cancel_unconfirmed = false;
        const float seconds = req.feed >= 1.0f ? fabsf(req.distance) / req.feed * 60.0f : 0.0f;
        _motion_timeout_ms = static_cast<uint32_t>(seconds * 1000.0f) + kJcJogMotionMarginMs;
        ++_jogs_sent;
        return true;
    }

    bool send_cancel_byte() {
        if (_tx == nullptr) return false;
        const char byte = static_cast<char>(0x85);
        const bool sent = _tx(_user, &byte, 1);
        if (sent) ++_cancels_sent;
        return sent;
    }

    void enter_cancelling(JcJogResult reason, bool latch, uint32_t now) {
        _phase = JcJogPhase::Cancelling;
        _cancel_reason = reason;
        _cancel_latch = latch;
        _cancel_ms = now;
        _cancel_seq = _last_seq;
        _cancel_unconfirmed = false;
    }

    void finish(JcJogResult result, bool latch, uint32_t now) {
        _phase = JcJogPhase::Idle;
        _result = result;
        if (latch) _needs_ack = true;
        _last_resolved_ms = now;
        _had_resolved = true;
        _cancel_unconfirmed = false;
    }

    // Idle was observed after the acknowledgement: decide whether the move is done
    // and whether the reported position is where the jog should have left it.
    void check_complete(uint32_t now) {
        const bool checkable = _start_valid && _pos_valid && _req.axis < 4;
        float delta = 0.0f;
        bool match = false;
        if (checkable) {
            delta = _pos[_req.axis] - _start_pos[_req.axis];
            match = fabsf(delta - _req.distance) <= kJcJogPositionToleranceMm;
        }
        // A position that "matches" is evidence of completion only when the commanded
        // step is clearly larger than the tolerance; for a tiny step, zero movement
        // would match too, so motion (or a second Idle frame) must be seen instead.
        const bool match_is_evidence = match && fabsf(_req.distance) > 2.0f * kJcJogPositionToleranceMm;
        if (!(_saw_motion || match_is_evidence || _idle_after_ack >= 2)) {
            return;  // Idle too early to trust: wait for another frame
        }
        _pos_check = !checkable ? JcJogPosCheck::NotChecked : (match ? JcJogPosCheck::Match : JcJogPosCheck::Mismatch);
        _pos_delta = delta;
        if (_pos_check == JcJogPosCheck::Mismatch) {
            finish(JcJogResult::PositionMismatch, true, now);
        } else {
            ++_completed;
            finish(JcJogResult::Completed, false, now);
        }
    }

    TxFn          _tx;
    void*         _user;
    JcCommandExec _exec;
    uint32_t      _now = 0;

    JcJogPhase    _phase = JcJogPhase::Idle;
    JcJogResult   _result = JcJogResult::None;
    JcJogResult   _cancel_reason = JcJogResult::Cancelled;
    bool          _cancel_latch = false;
    bool          _cancel_unconfirmed = false;
    bool          _needs_ack = false;
    int           _code = 0;

    JcJogRequest  _req = { 0, 0.0f, 0.0f };
    bool          _start_valid = false;
    float         _start_pos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    JcJogPosCheck _pos_check = JcJogPosCheck::NotChecked;
    float         _pos_delta = 0.0f;
    bool          _saw_motion = false;
    uint8_t       _idle_after_ack = 0;

    bool          _fresh = false;
    char          _state[20] = "";
    bool          _pos_valid = false;
    float         _pos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t      _last_seq = 0;
    bool          _have_seq = false;
    uint32_t      _seq_at_ack = 0;
    uint32_t      _cancel_seq = 0;

    uint32_t      _ack_ms = 0;
    uint32_t      _motion_timeout_ms = 0;
    uint32_t      _cancel_ms = 0;
    uint32_t      _last_begin_ms = 0;
    uint32_t      _last_resolved_ms = 0;
    uint32_t      _recover_ready_ms = 0;
    bool          _had_begin = false;
    bool          _had_resolved = false;

    uint32_t      _jogs_sent = 0, _cancels_sent = 0, _acked = 0, _rejected = 0, _completed = 0;
    uint32_t      _timeouts = 0, _losses = 0;
    uint32_t      _refused_policy = 0, _refused_busy = 0, _refused_class = 0, _unexpected_alarms = 0;
    uint32_t      _unexpected_motion = 0, _foreign_motion = 0;
};

}  // namespace jc8048w550
