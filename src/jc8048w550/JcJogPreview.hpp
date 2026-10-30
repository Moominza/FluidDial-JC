// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// SIMULATED controller for the NON-MOTION jog test mode (Instruction 007).
//
// This is the "final transport layer" of the preview: JcJogSession transmits to it instead of to a
// socket. It has NO connection to the Wi-Fi link, the UART or any hardware: it is a few hundred
// bytes of RAM that pretends to be a FluidNC controller so the operator can rehearse the complete
// jog interface (arming, acknowledgement, confirmation, results, cancellation, error messages)
// with the real rendering, touch and gate code while the machine cannot move.
//
// Everything it receives is checked with the same strict jog-line rule the live transport uses and
// is only ever copied into local memory. It never forwards anything, and nothing in this header (or
// in the code that owns it) refers to a transmit function. tools/check_no_sink.py enforces that.
//
// The real FluidNC read-only status and capability queries are NOT simulated: they keep running over
// the real link. Only the jog and cancel are rehearsed here, and while one is in flight the
// session is fed this controller's status instead of the real (stationary) machine's.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcJogTest.hpp"

namespace jc8048w550 {

enum class JcPreviewScenario : uint8_t {
    Normal,        // ok, motion, Idle
    Rejected,      // error:15, nothing moves
    Alarm,         // ALARM:1
    NoReply,       // the jog is never answered
    SlowAck,       // ok after 1.5 s
    IgnoreCancel,  // keeps "moving" and ignores 0x85 for a while
    Count,
};

inline const char* jc_preview_scenario_name(JcPreviewScenario s) {
    switch (s) {
        case JcPreviewScenario::Normal: return "NORMAL";
        case JcPreviewScenario::Rejected: return "REJECTS (error:15)";
        case JcPreviewScenario::Alarm: return "ALARM (ALARM:1)";
        case JcPreviewScenario::NoReply: return "NO REPLY";
        case JcPreviewScenario::SlowAck: return "SLOW ACK";
        case JcPreviewScenario::IgnoreCancel: return "IGNORES CANCEL";
        case JcPreviewScenario::Count: break;
    }
    return "?";
}

class JcPreviewController {
public:
    static constexpr uint32_t kAckMs = 60;
    static constexpr uint32_t kSlowAckMs = 1500;
    static constexpr uint32_t kIgnoreCancelRunMs = 15000;   // the stuck move ends by itself after this
    static constexpr uint32_t kAlarmMs = 1500;

    // JcJogSession::TxFn. Returns true when the SIMULATED controller accepted the bytes.
    static bool tx(void* self, const char* bytes, size_t n) { return static_cast<JcPreviewController*>(self)->on_tx(bytes, n); }

    void set_clock(uint32_t now) { _now = now; }
    // The rehearsal never moves the real machine, so the position the session sees is the REAL reported
    // position plus the displacement of the rehearsed jogs so far (cleared by reset_displacement()).
    void offset_position(float p[4]) const {
        for (int i = 0; i < 4; ++i) p[i] += _pos[i];
    }
    void reset_displacement() { memset(_pos, 0, sizeof(_pos)); }
    void set_scenario(JcPreviewScenario s) { _scenario = s; }
    JcPreviewScenario scenario() const { return _scenario; }
    void next_scenario() {
        _scenario = static_cast<JcPreviewScenario>((static_cast<uint8_t>(_scenario) + 1) % static_cast<uint8_t>(JcPreviewScenario::Count));
    }

    bool on_tx(const char* bytes, size_t n) {
        if (bytes == nullptr || n == 0) return false;
        if (n == 1 && static_cast<uint8_t>(bytes[0]) == 0x85) {
            ++_cancels;
            if (_scenario != JcPreviewScenario::IgnoreCancel) {
                _ack_pending = false;
                if (_moving) {
                    _pos[_axis] = position_now();
                    _moving = false;
                    set_state("Idle");
                }
            }
            return true;
        }
        char line[96];
        if (n < 2 || n >= sizeof(line) || bytes[n - 1] != '\n') {
            ++_refused;
            return false;
        }
        memcpy(line, bytes, n - 1);
        line[n - 1] = '\0';
        JcJogRequest r;
        if (!jc_parse_jog_line(line, r) || !jc_jog_line_allowed(line, jc_test_jog_limits())) {
            ++_refused;   // not a test jog: never accepted, even by the simulation
            return false;
        }
        ++_lines;
        strncpy(_last_line, line, sizeof(_last_line) - 1);
        _last_line[sizeof(_last_line) - 1] = '\0';
        _axis = r.axis;
        _dist = r.distance;
        _feed = r.feed;
        _start = _pos[_axis];
        _moving = false;
        set_state("Idle");
        _ack_pending = _scenario != JcPreviewScenario::NoReply;
        _ack_at = _now + (_scenario == JcPreviewScenario::SlowAck ? kSlowAckMs : kAckMs);
        return true;
    }

    // Advance the simulation (call every pass with the real clock).
    void service(uint32_t now) {
        _now = now;
        if (_ack_pending && static_cast<int32_t>(now - _ack_at) >= 0) {
            _ack_pending = false;
            switch (_scenario) {
                case JcPreviewScenario::Rejected:
                    queue_reply("error:15");
                    break;
                case JcPreviewScenario::Alarm:
                    queue_reply("ALARM:1");
                    set_state("Alarm");
                    _alarm_until = now + kAlarmMs;
                    break;
                default:
                    queue_reply("ok");
                    _moving = true;
                    _t0 = now;
                    _dur_ms = _scenario == JcPreviewScenario::IgnoreCancel
                                  ? kIgnoreCancelRunMs
                                  : static_cast<uint32_t>(fabsf(_dist) / (_feed >= 1.0f ? _feed : 1.0f) * 60000.0f);
                    set_state("Jog");
                    break;
            }
        }
        if (_moving && static_cast<uint32_t>(now - _t0) >= _dur_ms) {
            _pos[_axis] = _start + _dist;
            _moving = false;
            set_state("Idle");
        }
        if (_alarm_active() && static_cast<int32_t>(now - _alarm_until) >= 0) {
            _alarm_until = 0;
            set_state("Idle");
        }
    }

    // One reply line, if one is due (ok / error:N / ALARM:N). Returns true once per reply.
    bool take_reply(char* out, size_t size) {
        if (!_reply_pending) return false;
        _reply_pending = false;
        snprintf(out, size, "%s", _reply);
        return true;
    }

    // Something is in flight: the session must be fed this controller's status, not the real one.
    bool active() const { return _ack_pending || _moving || _reply_pending || _alarm_active(); }

    // The simulated machine's status for the session. `fresh` mirrors the real link.
    JcJogStatus status(const JcJogStatus& real, uint32_t sequence) const {
        JcJogStatus s = real;
        s.state = _state;
        s.sequence = sequence;
        for (int i = 0; i < 4; ++i) s.pos[i] = real.pos[i] + _pos[i];
        if (_moving) s.pos[_axis] = real.pos[_axis] + position_now();
        return s;
    }

    uint32_t lines() const { return _lines; }       // jog lines the simulation accepted (NOT sent anywhere)
    uint32_t cancels() const { return _cancels; }   // cancel bytes it accepted (NOT sent anywhere)
    uint32_t refused() const { return _refused; }   // lines it refused (not a restricted test jog)
    const char* last_line() const { return _last_line; }
    void reset() {
        _ack_pending = _moving = _reply_pending = false;
        _alarm_until = 0;
        set_state("Idle");
    }

private:
    float position_now() const {
        if (_dur_ms == 0) return _start + _dist;
        const uint32_t e = static_cast<uint32_t>(_now - _t0);
        const float frac = e >= _dur_ms ? 1.0f : static_cast<float>(e) / static_cast<float>(_dur_ms);
        return _start + _dist * frac;
    }
    bool _alarm_active() const { return _alarm_until != 0; }
    void set_state(const char* s) { strncpy(_state, s, sizeof(_state) - 1); _state[sizeof(_state) - 1] = '\0'; }
    void queue_reply(const char* r) {
        strncpy(_reply, r, sizeof(_reply) - 1);
        _reply[sizeof(_reply) - 1] = '\0';
        _reply_pending = true;
    }

    JcPreviewScenario _scenario = JcPreviewScenario::Normal;
    uint32_t _now = 0;
    float    _pos[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    char     _state[20] = "Idle";
    char     _reply[16] = "";
    char     _last_line[48] = "";
    uint8_t  _axis = 0;
    float    _dist = 0.0f, _feed = 100.0f, _start = 0.0f;
    uint32_t _ack_at = 0, _t0 = 0, _dur_ms = 0, _alarm_until = 0;
    bool     _ack_pending = false, _moving = false, _reply_pending = false;
    uint32_t _lines = 0, _cancels = 0, _refused = 0;
};

// Drives the session from the SIMULATED controller while a rehearsed jog is in flight, and from the
// real (read-only) status otherwise. Pure: it owns no transport and has no way to reach one.
class JcPreviewBridge {
public:
    static constexpr uint32_t kFramePeriodMs = 250;

    JcPreviewController& controller() { return _ctrl; }
    const JcPreviewController& controller() const { return _ctrl; }

    // `real` is the real controller's latest status (already freshness-qualified by the link).
    void pump(JcJogSession& session, const JcJogStatus& real, uint32_t now) {
        _ctrl.service(now);
        char reply[16];
        while (_ctrl.take_reply(reply, sizeof(reply))) session.on_line(reply, now);
        if (!_ctrl.active()) {
            if (_was_active) {
                _was_active = false;
                _next_frame = now;   // hand over promptly to the real status
            }
            JcJogStatus shown = real;   // real read-only status, plus the rehearsed displacement
            _ctrl.offset_position(shown.pos);
            session.on_status(shown, now);
            return;
        }
        _was_active = true;
        if (static_cast<int32_t>(now - _next_frame) >= 0) {
            _next_frame = now + kFramePeriodMs;
            session.on_status(_ctrl.status(real, kSimSequenceBase + (++_frames)), now);
        }
    }

    uint32_t frames() const { return _frames; }

private:
    static constexpr uint32_t kSimSequenceBase = 0x40000000u;
    JcPreviewController _ctrl;
    bool     _was_active = false;
    uint32_t _next_frame = 0;
    uint32_t _frames = 0;
};

}  // namespace jc8048w550
