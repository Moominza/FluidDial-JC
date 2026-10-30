// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// First-live-jog TEST support (Instruction 006). Pure logic shared by the
// experimental live-jog build and the host tests; nothing here transmits by
// itself. It provides:
//
//   * the restricted test limits (X/Y 1 mm @ 100 mm/min, Z 0.5 mm @ 50 mm/min,
//     A disabled) and jc_profile_apply_test_limits();
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
#include "JcResponse.hpp"

namespace jc8048w550 {

// ---- limits ---------------------------------------------------------------

struct JcJogTestLimits {
    float max_step[4];  // mm; 0 = jogging disabled for the axis
    float max_feed[4];  // mm/min
};

inline JcJogTestLimits jc_test_jog_limits() {
    JcJogTestLimits l;
    l.max_step[0] = 1.0f;  l.max_feed[0] = 100.0f;  // X
    l.max_step[1] = 1.0f;  l.max_feed[1] = 100.0f;  // Y
    l.max_step[2] = 0.5f;  l.max_feed[2] = 50.0f;   // Z
    l.max_step[3] = 0.0f;  l.max_feed[3] = 0.0f;    // A: disabled pending rotary calibration
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
