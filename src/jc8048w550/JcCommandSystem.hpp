// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// The ONE command interface shared by every transport (Wi-Fi TCP, wired UART, BLE UART, the simulator).
// The gate's sink is JcCommandSystem::sink: it routes a jog to the jog session (motion verification),
// everything else to the single-operation command link, and EVERY byte leaves through checked_tx(), the
// one place the shared policy (JcCommandPolicy.hpp) is applied. A transport supplies only a SendFn that
// writes bytes with its own framing; it contains no command policy.
//
//   * single operation: a command is refused while another is unresolved (jog session or link exec);
//   * no replay: link loss ends the operation; nothing is queued or resent;
//   * results: acknowledgement, controller error, alarm, timeout, cancel and link loss are recorded and
//     handed to the UI once (take_result), so the operator is told what FluidNC answered;
//   * a Home acknowledgement is recorded in JcHomedTracker (per connection).
//
// Pure: no I/O of its own.

#pragma once

#include <stdio.h>
#include <string.h>

#include "JcCommandLink.hpp"
#include "JcCommandPolicy.hpp"
#include "JcHomingState.hpp"
#include "JcJogTest.hpp"

namespace jc8048w550 {

struct JcCommandResult {
    bool           valid = false;
    JcCommandClass cls = JcCommandClass::Other;
    JcExecOutcome  outcome = JcExecOutcome::None;
    int            code = 0;
    char           text[48] = "";
    uint32_t       ms = 0;
};

inline const char* jc_class_word(JcCommandClass c) {
    switch (c) {
        case JcCommandClass::Jog: return "JOG";
        case JcCommandClass::JogCancel: return "JOG CANCEL";
        case JcCommandClass::Home: return "HOME";
        case JcCommandClass::Probe: return "PROBE";
        case JcCommandClass::WorkOffset: return "WORK OFFSET";
        case JcCommandClass::Spindle: return "SPINDLE";
        case JcCommandClass::Job: return "JOB";
        case JcCommandClass::Macro: return "MACRO";
        case JcCommandClass::Unlock: return "UNLOCK";
        case JcCommandClass::Reset: return "RESET";
        case JcCommandClass::Hold: return "FEED HOLD";
        case JcCommandClass::Other: return "COMMAND";
    }
    return "COMMAND";
}

// One line for the operator: what FluidNC answered.
inline void jc_result_line(const JcCommandResult& r, char* out, size_t size) {
    switch (r.outcome) {
        case JcExecOutcome::Acked:
            snprintf(out, size, "%s: accepted by FluidNC (ok)", jc_class_word(r.cls));
            break;
        case JcExecOutcome::SentNoAck:
            snprintf(out, size, "%s: sent (real-time, FluidNC sends no reply)", jc_class_word(r.cls));
            break;
        case JcExecOutcome::ControllerError:
            snprintf(out, size, "%s: REJECTED by FluidNC (error:%d)", jc_class_word(r.cls), r.code);
            break;
        case JcExecOutcome::Alarm:
            snprintf(out, size, "%s: ALARM:%d", jc_class_word(r.cls), r.code);
            break;
        case JcExecOutcome::TimedOut:
            snprintf(out, size, "%s: NO REPLY (timed out); state unknown", jc_class_word(r.cls));
            break;
        case JcExecOutcome::LinkLost:
            snprintf(out, size, "%s: CONNECTION LOST; state unknown, NOT resent", jc_class_word(r.cls));
            break;
        default:
            snprintf(out, size, "%s: %s", jc_class_word(r.cls), jc_exec_outcome_name(r.outcome));
            break;
    }
}

class JcCommandSystem {
public:
    using SendFn = JcCommandLink::SendFn;

    JcCommandSystem(SendFn send, void* user) : _send(send), _user(user), _link(&JcCommandSystem::checked_tx, this) {}

    // The jog session (owned by the application) uses checked_tx as its transport.
    static bool session_tx(void* self, const char* bytes, size_t n) { return checked_tx(self, bytes, n); }
    void attach_jogs(JcJogSession* jogs) { _jogs = jogs; }
    void set_transport(SendFn send, void* user) { _send = send; _user = user; }   // tests / late binding

    // JcCommandSinkEx for the gate.
    static bool sink(void* self, JcCommandClass cls, const char* text) {
        return static_cast<JcCommandSystem*>(self)->route(cls, text);
    }

    void set_clock(uint32_t now) { _now = now; _link.set_clock(now); }
    void on_line(const char* line) { _link.on_line(line); }
    void on_machine_state(const char* state) { _link.on_machine_state(state); }
    void owner_facts(const JcHomingOwnerFacts& f) { _facts = f; }

    // Every pass: ages the request, and records a finished one exactly once.
    void tick() {
        _link.tick();
        harvest();
    }

    // Records a finished request exactly once and frees the link for the next command.
    void harvest() {
        JcCommandExec& e = _link.exec();
        if (e.state() == JcExecState::Finished) {
            _result.valid = true;
            _result.cls = _in_flight_cls;
            _result.outcome = e.outcome();
            _result.code = e.code();
            snprintf(_result.text, sizeof(_result.text), "%s", e.text());
            _result.ms = e.elapsed_ms();
            _pending_result = true;
            if (e.outcome() == JcExecOutcome::Acked && _in_flight_cls == JcCommandClass::Home) {
                _homed.on_home_acked(_in_flight_axis, _configured_mask);
            }
            if (e.outcome() == JcExecOutcome::Alarm) _homed.on_invalidated();
            e.release();
        }
    }

    // True once per finished command.
    bool take_result(JcCommandResult& out) {
        if (!_pending_result) return false;
        _pending_result = false;
        out = _result;
        return true;
    }

    // Connection lost: the operation ends, nothing is replayed, nothing homed earlier is trusted.
    void on_link_lost() {
        _link.on_link_lost();
        _homed.on_invalidated();
    }
    void set_configured_homing_mask(uint8_t m) { _configured_mask = m; }

    bool busy() const { return _link.exec().state() != JcExecState::Idle; }
    JcHomedTracker& homed() { return _homed; }
    const JcHomedTracker& homed() const { return _homed; }
    JcCommandLink& link() { return _link; }
    uint32_t refused_policy() const { return _refused_policy; }
    uint32_t refused_busy() const { return _refused_busy; }
    uint32_t transmitted_bytes_calls() const { return _tx_calls; }

private:
    bool route(JcCommandClass cls, const char* text) {
        harvest();   // a finished real-time byte or reply must not block the next command
        if (text == nullptr || !jc_command_line_allowed(cls, text)) {
            ++_refused_policy;   // not a line the builders generate for this class
            return false;
        }
        const bool jog_family = cls == JcCommandClass::Jog || cls == JcCommandClass::JogCancel;
        if (jog_family) {
            if (cls == JcCommandClass::Jog && (busy())) {
                ++_refused_busy;
                return false;
            }
            return _jogs != nullptr && JcJogSession::sink(_jogs, cls, text);
        }
        const bool stop_family = cls == JcCommandClass::Hold || cls == JcCommandClass::Reset || cls == JcCommandClass::Unlock;
        if (!stop_family && (_jogs != nullptr && _jogs->unresolved())) {
            ++_refused_busy;   // one operation at a time
            return false;
        }
        _in_flight_cls = cls;
        _in_flight_axis = 0xFF;
        if (cls == JcCommandClass::Home && text[2] != '\0') {
            _in_flight_axis = static_cast<uint8_t>(strchr("XYZA", text[2]) - "XYZA");
        }
        return JcCommandLink::sink(&_link, cls, text);
    }

    // THE shared transport policy point: exactly one command line (or one real-time byte) per call.
    static bool checked_tx(void* self, const char* bytes, size_t n) {
        JcCommandSystem* s = static_cast<JcCommandSystem*>(self);
        ++s->_tx_calls;
        if (s->_send == nullptr || !jc_command_bytes_allowed(bytes, n)) { ++s->_refused_policy; return false; }
        return s->_send(s->_user, bytes, n);
    }

    SendFn          _send;
    void*           _user;
    JcCommandLink   _link;
    JcJogSession*   _jogs = nullptr;
    JcHomedTracker  _homed;
    JcHomingOwnerFacts _facts;
    JcCommandResult _result;
    bool            _pending_result = false;
    JcCommandClass  _in_flight_cls = JcCommandClass::Other;
    uint8_t         _in_flight_axis = 0xFF;
    uint8_t         _configured_mask = 0x03;
    uint32_t        _now = 0;
    uint32_t        _refused_policy = 0;
    uint32_t        _refused_busy = 0;
    uint32_t        _tx_calls = 0;
};

}  // namespace jc8048w550
