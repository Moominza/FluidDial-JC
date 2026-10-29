// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Single-in-flight request tracker: correlates one transmitted line with the
// controller's "ok" / "error:N" / "ALARM:N" reply, enforces a timeout, and
// reports a lost connection. Pure logic shared by the read-only capability
// loader (today) and by any future authorized transmit sink.
//
// Rules that make it safe to build on:
//   * one request at a time (begin() refuses while one is awaiting);
//   * a request is NEVER resent: timeout and connection loss end it;
//   * after a timeout the controller's state is unknown and a late "ok" could
//     be mistaken for the next request's reply, so a quiet period must pass
//     (and the result must be released) before another request may begin;
//   * after a connection loss a new request needs a new, explicit begin().

#pragma once

#include <stdint.h>
#include <string.h>

#include "JcCommandAuth.hpp"
#include "JcResponse.hpp"

namespace jc8048w550 {

enum class JcExecState : uint8_t { Idle, Awaiting, Finished };

enum class JcExecOutcome : uint8_t {
    None,
    Acked,            // "ok"
    ControllerError,  // "error:N"
    Alarm,            // "ALARM:N" or the machine entered Alarm while waiting
    TimedOut,
    LinkLost,
    SentNoAck,        // real-time byte: the controller sends no reply
    Abandoned,        // the caller gave up; a late reply may still arrive
};

inline const char* jc_exec_outcome_name(JcExecOutcome o) {
    switch (o) {
        case JcExecOutcome::None: return "none";
        case JcExecOutcome::Acked: return "acknowledged (ok)";
        case JcExecOutcome::ControllerError: return "controller error";
        case JcExecOutcome::Alarm: return "alarm";
        case JcExecOutcome::TimedOut: return "timed out";
        case JcExecOutcome::LinkLost: return "connection lost";
        case JcExecOutcome::SentNoAck: return "sent (real-time, no reply expected)";
        case JcExecOutcome::Abandoned: return "abandoned";
    }
    return "?";
}

// How long to wait for the reply, per command class.
inline uint32_t jc_command_timeout_ms(JcCommandClass cls) {
    switch (cls) {
        case JcCommandClass::Jog: return 2000;
        case JcCommandClass::JogCancel:
        case JcCommandClass::Hold:
        case JcCommandClass::Reset: return 1000;
        case JcCommandClass::Home: return 120000;   // a homing cycle replies when finished
        case JcCommandClass::Probe: return 60000;   // a probe move replies when finished
        case JcCommandClass::WorkOffset: return 2000;
        case JcCommandClass::Spindle: return 2000;
        case JcCommandClass::Job: return 5000;
        case JcCommandClass::Macro: return 5000;
        case JcCommandClass::Unlock: return 2000;
        case JcCommandClass::Other: return 5000;
    }
    return 5000;
}

constexpr uint32_t kJcQueryTimeoutMs = 2500;
constexpr uint32_t kJcResyncQuietMs = 1000;

// Real-time bytes (! ~ 0x18 0x85) are acted on immediately and have no reply.
inline bool jc_expects_reply(bool realtime) { return !realtime; }

class JcCommandExec {
public:
    // Starts waiting for a reply. Returns false (and changes nothing) if a
    // request is already awaiting, an unreleased result exists, or the resync
    // quiet period after a timeout has not elapsed.
    bool begin(const char* text, uint32_t timeout_ms, uint32_t now, bool abort_on_alarm_state = false) {
        if (_state != JcExecState::Idle || static_cast<int32_t>(now - _quiet_until) < 0) {
            return false;
        }
        strncpy(_text, text != nullptr ? text : "", sizeof(_text) - 1);
        _text[sizeof(_text) - 1] = '\0';
        _state = JcExecState::Awaiting;
        _outcome = JcExecOutcome::None;
        _code = 0;
        _started_ms = now;
        _timeout_ms = timeout_ms;
        _abort_on_alarm_state = abort_on_alarm_state;
        _lines = 0;
        ++_begun;
        return true;
    }

    // A real-time byte has no reply: record it as sent and finish at once.
    bool begin_realtime(const char* description, uint32_t now) {
        if (!begin(description, 0, now)) {
            return false;
        }
        finish(JcExecOutcome::SentNoAck, now);
        return true;
    }

    // Feed every classified non-status line. Returns true if the line completed
    // the awaited request.
    bool on_response(const JcResponse& r, uint32_t now) {
        if (_state != JcExecState::Awaiting) {
            if (r.kind == JcRespKind::Ok || r.kind == JcRespKind::Error || r.kind == JcRespKind::Alarm) {
                ++_unsolicited;  // a late or duplicate reply: never attributed to a new request
            }
            return false;
        }
        switch (r.kind) {
            case JcRespKind::Ok:
                finish(JcExecOutcome::Acked, now);
                return true;
            case JcRespKind::Error:
                _code = r.code;
                finish(JcExecOutcome::ControllerError, now);
                return true;
            case JcRespKind::Alarm:
                _code = r.code;
                finish(JcExecOutcome::Alarm, now);
                return true;
            case JcRespKind::Empty:
                return false;
            default:
                ++_lines;  // data lines belonging to this request
                return false;
        }
    }

    // The machine entered an alarm state while a command was awaiting.
    void on_machine_state(const char* state, uint32_t now) {
        if (_state == JcExecState::Awaiting && _abort_on_alarm_state && state != nullptr
            && strcmp(state, "Alarm") == 0) {
            finish(JcExecOutcome::Alarm, now);
        }
    }

    void tick(uint32_t now) {
        if (_state == JcExecState::Awaiting && static_cast<uint32_t>(now - _started_ms) >= _timeout_ms) {
            finish(JcExecOutcome::TimedOut, now);
        }
    }

    void on_link_lost(uint32_t now) {
        if (_state == JcExecState::Awaiting) {
            finish(JcExecOutcome::LinkLost, now);
        }
    }

    // The caller stops waiting (cancelled read, failed sequence). A reply may
    // still arrive, so the same resync quiet period as a timeout applies.
    void abandon(uint32_t now) {
        if (_state == JcExecState::Awaiting) {
            finish(JcExecOutcome::Abandoned, now);
        }
    }

    // True when begin() would be accepted right now.
    bool ready(uint32_t now) const {
        return _state == JcExecState::Idle && static_cast<int32_t>(now - _quiet_until) >= 0;
    }

    // Acknowledge a finished result so another request may begin.
    void release() {
        if (_state == JcExecState::Finished) {
            _state = JcExecState::Idle;
        }
    }

    JcExecState state() const { return _state; }
    bool awaiting() const { return _state == JcExecState::Awaiting; }
    JcExecOutcome outcome() const { return _outcome; }
    int code() const { return _code; }
    const char* text() const { return _text; }
    uint32_t elapsed_ms() const { return _elapsed_ms; }
    uint8_t lines() const { return _lines; }

    uint32_t begun() const { return _begun; }
    uint32_t acked() const { return _acked; }
    uint32_t errors() const { return _errors; }
    uint32_t alarms() const { return _alarms; }
    uint32_t timeouts() const { return _timeouts; }
    uint32_t losses() const { return _losses; }
    uint32_t unsolicited() const { return _unsolicited; }

private:
    void finish(JcExecOutcome outcome, uint32_t now) {
        _state = JcExecState::Finished;
        _outcome = outcome;
        _elapsed_ms = now - _started_ms;
        switch (outcome) {
            case JcExecOutcome::Acked: ++_acked; break;
            case JcExecOutcome::ControllerError: ++_errors; break;
            case JcExecOutcome::Alarm: ++_alarms; break;
            case JcExecOutcome::TimedOut:
                ++_timeouts;
                _quiet_until = now + kJcResyncQuietMs;
                break;
            case JcExecOutcome::Abandoned:
                _quiet_until = now + kJcResyncQuietMs;
                break;
            case JcExecOutcome::LinkLost: ++_losses; break;
            default: break;
        }
    }

    JcExecState   _state = JcExecState::Idle;
    JcExecOutcome _outcome = JcExecOutcome::None;
    char          _text[128] = "";
    int           _code = 0;
    uint32_t      _started_ms = 0;
    uint32_t      _timeout_ms = 0;
    uint32_t      _elapsed_ms = 0;
    uint32_t      _quiet_until = 0;
    bool          _abort_on_alarm_state = false;
    uint8_t       _lines = 0;
    uint32_t      _begun = 0, _acked = 0, _errors = 0, _alarms = 0, _timeouts = 0, _losses = 0, _unsolicited = 0;
};

}  // namespace jc8048w550
