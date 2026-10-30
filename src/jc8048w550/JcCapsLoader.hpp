// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Reads the controller's capabilities with the closed set of read-only queries
// in JcQuery.hpp, one at a time, through JcCommandExec (reply correlation and
// timeout). Operator-triggered only; never started automatically; never
// resumed after a failure (start() always begins from the first query, so no
// query is replayed from a half-finished earlier run).
//
// Pure logic: the caller sends pending_line() on its transport, calls
// mark_sent(), feeds every received line to on_line(), and calls tick().

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcCommandExec.hpp"
#include "JcMachineCaps.hpp"
#include "JcQuery.hpp"
#include "JcResponse.hpp"

namespace jc8048w550 {

enum class JcLoadState : uint8_t { Idle, Running, Complete, Failed };

enum class JcLoadFailure : uint8_t {
    None,
    NotReady,    // preconditions not met (see jc_caps_read_blocker)
    SendFailed,
    TimedOut,
    LinkLost,
    Alarm,       // controller raised ALARM:N during the read
    Cancelled,
};

inline const char* jc_load_failure_name(JcLoadFailure f) {
    switch (f) {
        case JcLoadFailure::None: return "none";
        case JcLoadFailure::NotReady: return "not ready";
        case JcLoadFailure::SendFailed: return "could not send query";
        case JcLoadFailure::TimedOut: return "controller did not reply (timeout)";
        case JcLoadFailure::LinkLost: return "connection lost";
        case JcLoadFailure::Alarm: return "controller alarm during read";
        case JcLoadFailure::Cancelled: return "cancelled";
    }
    return "?";
}

class JcCapsLoader {
public:
    // $I, $G, 4 axes x 6 fields, probe pin, toolsetter pin.
    static constexpr uint8_t kSteps = 2 + kJcQueryAxes * kJcQueryFieldCount + 2;

    // Begins a fresh read. Previous results are discarded first; if the start
    // conditions are not met the loader fails with NotReady instead.
    bool start(uint32_t now, const char* blocker) {
        _caps = JcMachineCaps();
        _exec.release();  // keep the exec's resync quiet period: a late reply must not be misattributed
        _step = 0;
        _sent = false;
        _failure = JcLoadFailure::None;
        _started_ms = now;
        _finished_seen = false;
        if (blocker != nullptr) {
            _state = JcLoadState::Failed;
            _failure = JcLoadFailure::NotReady;
            return false;
        }
        _state = JcLoadState::Running;
        return true;
    }

    void cancel(uint32_t now) { fail(JcLoadFailure::Cancelled, now); }

    // Forgets everything that was read (used when the connection is lost: the
    // controller may have been restarted or reconfigured). Returns to Idle, so
    // the profile falls back to placeholders until the operator reads again.
    void invalidate(uint32_t now) {
        _exec.abandon(now);
        _exec.release();
        _caps = JcMachineCaps();
        _state = JcLoadState::Idle;
        _failure = JcLoadFailure::None;
        _sent = false;
        _step = 0;
        _finished_seen = true;
    }

    // The next line to transmit, or nullptr if none is due (a reply is awaited,
    // the exec is in its resync quiet period, or the loader is not running).
    // The text is stable until mark_sent().
    const char* pending_line(uint32_t now) {
        if (_state != JcLoadState::Running || _sent || !_exec.ready(now)) {
            return nullptr;
        }
        build_line(_step, _line, sizeof(_line));
        return _line;
    }

    // Call after the line returned by pending_line() was handed to the transport.
    void mark_sent(uint32_t now) {
        if (_state != JcLoadState::Running || _sent) return;
        _exec.begin(_line, kJcQueryTimeoutMs, now, false);
        _sent = true;
        _got_value = false;
    }

    void send_failed(uint32_t now) { fail(JcLoadFailure::SendFailed, now); }

    void on_line(const char* line, uint32_t now) {
        if (_state != JcLoadState::Running || !_sent) {
            return;
        }
        const JcResponse r = jc_classify_line(line);
        switch (r.kind) {
            case JcRespKind::Setting: accept_setting(r); break;
            case JcRespKind::Info: accept_info(r); break;
            case JcRespKind::GCodeState: accept_gcode(r); break;
            default: break;
        }
        if (_exec.on_response(r, now)) {
            finish_step(now);
        }
    }

    void tick(uint32_t now) {
        if (_state != JcLoadState::Running) return;
        _exec.tick(now);
        if (_exec.outcome() == JcExecOutcome::TimedOut && _exec.state() == JcExecState::Finished) {
            fail(JcLoadFailure::TimedOut, now);
        }
    }

    void on_link_lost(uint32_t now) {
        if (_state == JcLoadState::Running) {
            _exec.on_link_lost(now);
            fail(JcLoadFailure::LinkLost, now);
        }
    }

    // True exactly once after the loader reaches Complete or Failed.
    bool take_finished() {
        if ((_state == JcLoadState::Complete || _state == JcLoadState::Failed) && !_finished_seen) {
            _finished_seen = true;
            return true;
        }
        return false;
    }

    JcLoadState state() const { return _state; }
    bool running() const { return _state == JcLoadState::Running; }
    JcLoadFailure failure() const { return _failure; }
    uint8_t step() const { return _step; }
    const JcMachineCaps& caps() const { return _caps; }
    const JcCommandExec& exec() const { return _exec; }

private:
    void build_line(uint8_t step, char* out, size_t size) const {
        if (step == 0) { snprintf(out, size, "%s", kJcQueryBuildInfo); return; }
        if (step == 1) { snprintf(out, size, "%s", kJcQueryModalState); return; }
        const uint8_t axis_steps = kJcQueryAxes * kJcQueryFieldCount;
        if (step < 2 + axis_steps) {
            const uint8_t i = static_cast<uint8_t>(step - 2);
            jc_query_axis_line(out, size, static_cast<uint8_t>(i / kJcQueryFieldCount),
                               static_cast<JcQueryField>(i % kJcQueryFieldCount));
            return;
        }
        snprintf(out, size, "%s", step == 2 + axis_steps ? kJcQueryProbePin : kJcQueryToolsetterPin);
    }

    // Maps the current step to (axis, field); false for non-axis steps.
    bool current_axis_field(uint8_t& axis, JcQueryField& field) const {
        if (_step < 2 || _step >= 2 + kJcQueryAxes * kJcQueryFieldCount) return false;
        const uint8_t i = static_cast<uint8_t>(_step - 2);
        axis = static_cast<uint8_t>(i / kJcQueryFieldCount);
        field = static_cast<JcQueryField>(i % kJcQueryFieldCount);
        return true;
    }

    void accept_setting(const JcResponse& r) {
        uint8_t axis = 0;
        JcQueryField field = JcQueryField::StepsPerMm;
        char expected[72];
        if (current_axis_field(axis, field)) {
            if (!jc_query_axis_line(expected, sizeof(expected), axis, field)
                || strcmp(r.key, expected + 1) != 0) {  // expected[0] is '$'
                return;  // an echo that does not belong to the awaited query
            }
            _got_value = true;
            JcAxisCaps& a = _caps.axes[axis];
            float v = 0.0f;
            const uint8_t f = static_cast<uint8_t>(field);
            if (jc_caps_field_valid(field, r.value, v)) {
                a.state[f] = JcFieldState::Valid;
                a.value[f] = v;
            } else {
                a.state[f] = JcFieldState::BadValue;
            }
            return;
        }
        if (_step == 2 + kJcQueryAxes * kJcQueryFieldCount && strcmp(r.key, "/probe/pin") == 0) {
            _got_value = true;
            snprintf(_caps.probe_pin, sizeof(_caps.probe_pin), "%s", r.value);
        } else if (_step == 3 + kJcQueryAxes * kJcQueryFieldCount && strcmp(r.key, "/probe/toolsetter_pin") == 0) {
            _got_value = true;
            snprintf(_caps.toolsetter_pin, sizeof(_caps.toolsetter_pin), "%s", r.value);
        }
    }

    void accept_info(const JcResponse& r) {
        if (_step != 0) return;
        if (strcmp(r.key, "VER") == 0) {
            snprintf(_caps.firmware, sizeof(_caps.firmware), "%s", r.value);
            // FluidNC v4.1.1 sends "[VER:4.1 FluidNC v4.1.1 (esp32s3-wifi) :]"; drop the trailing " :".
            size_t n = strlen(_caps.firmware);
            while (n > 0 && (_caps.firmware[n - 1] == ' ' || _caps.firmware[n - 1] == ':')) {
                _caps.firmware[--n] = '\0';
            }
            _got_value = true;
        } else if (strcmp(r.key, "OPT") == 0) {
            snprintf(_caps.options, sizeof(_caps.options), "%s", r.value);
        }
    }

    // [GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]
    void accept_gcode(const JcResponse& r) {
        if (_step != 1) return;
        _got_value = true;
        const char* p = r.value;
        while (*p) {
            while (*p == ' ') ++p;
            const char* start = p;
            while (*p && *p != ' ') ++p;
            const size_t n = static_cast<size_t>(p - start);
            if (n == 3 && start[0] == 'G' && start[1] == '5' && start[2] >= '4' && start[2] <= '9') {
                memcpy(_caps.wcs, start, 3);
                _caps.wcs[3] = '\0';
            } else if (n >= 2 && n <= 4 && start[0] == 'T') {
                char digits[4] = "";
                memcpy(digits, start + 1, n - 1);
                digits[n - 1] = '\0';
                char* end = nullptr;
                const long t = strtol(digits, &end, 10);
                if (end != digits && *end == '\0' && t >= 0 && t < 1000) _caps.tool = static_cast<int>(t);
            }
        }
    }

    void finish_step(uint32_t now) {
        const JcExecOutcome outcome = _exec.outcome();
        uint8_t axis = 0;
        JcQueryField field = JcQueryField::StepsPerMm;
        const bool is_axis = current_axis_field(axis, field);
        if (outcome == JcExecOutcome::Alarm) {
            fail(JcLoadFailure::Alarm, now);
            return;
        }
        if (outcome == JcExecOutcome::ControllerError) {
            ++_caps.queries_error;
            if (is_axis) {
                const uint8_t f = static_cast<uint8_t>(field);
                _caps.axes[axis].state[f] = JcFieldState::CtrlError;
                _caps.axes[axis].error_code[f] = _exec.code();
            }
        } else if (outcome == JcExecOutcome::Acked) {
            ++_caps.queries_ok;
            if (is_axis && !_got_value) {
                _caps.axes[axis].state[static_cast<uint8_t>(field)] = JcFieldState::NoValue;
            }
        }
        _exec.release();
        _sent = false;
        ++_step;
        if (_step >= kSteps) {
            _caps.complete = true;
            _caps.read_ms = now - _started_ms;
            _state = JcLoadState::Complete;
        }
    }

    void fail(JcLoadFailure why, uint32_t now) {
        if (_state == JcLoadState::Failed || _state == JcLoadState::Complete) return;
        _exec.abandon(now);
        _state = JcLoadState::Failed;
        _failure = why;
        _caps.complete = false;
        _sent = false;
    }

    JcLoadState   _state = JcLoadState::Idle;
    JcLoadFailure _failure = JcLoadFailure::None;
    JcMachineCaps _caps;
    JcCommandExec _exec;
    uint8_t       _step = 0;
    bool          _sent = false;
    bool          _got_value = false;
    bool          _finished_seen = true;
    uint32_t      _started_ms = 0;
    char          _line[80] = "";
};

}  // namespace jc8048w550
