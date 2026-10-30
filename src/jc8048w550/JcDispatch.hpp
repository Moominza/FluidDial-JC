// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Single dispatch point between UI pages and the command authorization gate.
// UI pages never touch a socket or UART: they hand a JcCommand (or sequence)
// to JcDispatcher::submit*(), which records the outcome and asks JcCommandGate.
// There is deliberately no queue and no resend API, so a command can never be
// replayed after a connection loss; arming is dropped on every link loss.

#pragma once

#include <string.h>

#include "JcCommandAuth.hpp"
#include "JcCommandBuilder.hpp"
#include "JcCommandExec.hpp"

namespace jc8048w550 {

struct JcDispatchContext {
    bool        live = false;
    bool        link_fresh = false;
    const char* machine_state = "";
    bool        operator_confirmed = false;
    uint32_t    now_ms = 0;
    uint8_t     limits_mask = 0;  // bit i set: axis i limits confirmed by the controller
    uint8_t     envelope_mask = 0;  // bit i set: axis i soft / hard limit settings were read
    bool        operation_pending = false;  // a previous motion operation is unresolved
};

struct JcDispatchRecord {
    char           text[kJcMaxCommandLength];
    JcCommandClass cls;
    JcAuthDecision decision;
    bool           valid;  // false: the builder rejected the command (not submitted)
    uint32_t       ms;
};

class JcDispatcher {
public:
    static constexpr uint8_t kHistory = 8;

    explicit JcDispatcher(JcCommandSink sink = nullptr) : _gate(sink) {}

    // The one place a decision context is built, shared by submit() and the
    // side-effect-free evaluate() used by the dry-run page.
    JcAuthContext make_auth(const JcCommand& command, const JcDispatchContext& ctx) const {
        JcAuthContext auth;
        auth.armed = _armed && (_arm_expires_ms == 0 || static_cast<int32_t>(ctx.now_ms - _arm_expires_ms) < 0);
        auth.operation_pending = ctx.operation_pending;
        auth.live = ctx.live;
        auth.link_fresh = ctx.link_fresh;
        auth.operator_confirmed = ctx.operator_confirmed;
        auth.now_ms = ctx.now_ms;
        auth.command_issued_ms = ctx.now_ms;
        const bool moves_axis = (command.cls == JcCommandClass::Jog || command.cls == JcCommandClass::Probe)
                                && command.axis != 0xFF;
        auth.limits_required = moves_axis;
        auth.limits_confirmed = moves_axis && command.axis < 4 && ((ctx.limits_mask >> command.axis) & 1u) != 0;
        auth.envelope_required = moves_axis;
        auth.envelope_confirmed = moves_axis && command.axis < 4 && ((ctx.envelope_mask >> command.axis) & 1u) != 0;
        strncpy(auth.machine_state, ctx.machine_state ? ctx.machine_state : "", sizeof(auth.machine_state) - 1);
        return auth;
    }

    // Decision only: no record, no counter, no sink. Same rules as submit().
    JcAuthDecision evaluate(const JcCommand& command, const JcDispatchContext& ctx) const {
        if (!command.ok) {
            return JcAuthDecision::BlockedBadState;
        }
        return jc_authorize(command.cls, make_auth(command, ctx));
    }

    JcAuthDecision submit(const JcCommand& command, const JcDispatchContext& ctx) {
        if (!command.ok) {
            record(command, JcAuthDecision::BlockedBadState, false, ctx.now_ms);
            return JcAuthDecision::BlockedBadState;
        }
        const JcAuthContext auth = make_auth(command, ctx);
        const JcAuthDecision decision = _gate.submit(command.cls, command.text, auth);
        record(command, decision, true, ctx.now_ms);
        return decision;
    }

    // Submits steps in order and stops at the first one that is not allowed.
    // Returns that decision (or Allowed if every step was authorized).
    //
    // With a transmit sink installed a step is only followed by the next one
    // after the controller acknowledged it: the first transmitted step is sent,
    // the remaining steps are held, and advance_sequence() releases each next
    // step with a freshly built context (so a stale link, an alarm or a lost
    // connection between steps stops the sequence). Held steps are dropped on
    // any failed outcome and on link loss; they are never replayed.
    JcAuthDecision submit_sequence(const JcCommandSequence& seq, const JcDispatchContext& ctx) {
        _pending_count = 0;
        if (!seq.ok || seq.count == 0) {
            JcCommand bad;
            record(bad, JcAuthDecision::BlockedBadState, false, ctx.now_ms);
            return JcAuthDecision::BlockedBadState;
        }
        for (uint8_t i = 0; i < seq.count; ++i) {
            const uint32_t sent_before = _gate.sent();
            const JcAuthDecision d = submit(seq.steps[i], ctx);
            if (d != JcAuthDecision::Allowed) {
                return d;
            }
            if (_gate.sent() != sent_before && i + 1 < seq.count) {
                for (uint8_t k = i + 1; k < seq.count; ++k) {
                    _pending[k - i - 1] = seq.steps[k];
                }
                _pending_count = static_cast<uint8_t>(seq.count - i - 1);
                _pending_next = 0;
                return JcAuthDecision::Allowed;
            }
        }
        return JcAuthDecision::Allowed;
    }

    bool sequence_active() const { return _pending_next < _pending_count; }
    uint8_t sequence_steps_left() const { return static_cast<uint8_t>(_pending_count - _pending_next); }
    void abort_sequence() { _pending_count = 0; _pending_next = 0; }

    // Called with the outcome of the step just sent. Anything but an
    // acknowledgement abandons the rest of the sequence.
    JcAuthDecision advance_sequence(JcExecOutcome last, const JcDispatchContext& ctx) {
        if (!sequence_active()) {
            return JcAuthDecision::BlockedBadState;
        }
        if (last != JcExecOutcome::Acked && last != JcExecOutcome::SentNoAck) {
            abort_sequence();
            return JcAuthDecision::BlockedBadState;
        }
        const JcCommand step = _pending[_pending_next++];
        const JcAuthDecision d = submit(step, ctx);
        if (d != JcAuthDecision::Allowed || _gate.last() != JcAuthDecision::Allowed) {
            abort_sequence();
        }
        return d;
    }

    // Test-only transmit hook; the firmware never calls this.
    void set_sink_ex(JcCommandSinkEx sink, void* user) { _gate.set_sink_ex(sink, user); }

    // Explicit operator arming. Nothing in the shipping UI calls arm().
    void arm() {
        _armed = true;
        _arm_expires_ms = 0;
    }
    // Arming that lapses by itself after ttl_ms (used by the live-jog test build).
    void arm_for(uint32_t now_ms, uint32_t ttl_ms) {
        _armed = true;
        _arm_expires_ms = now_ms + ttl_ms;
        if (_arm_expires_ms == 0) _arm_expires_ms = 1;
    }
    void disarm() {
        _armed = false;
        _arm_expires_ms = 0;
    }
    bool armed() const { return _armed; }
    bool armed_at(uint32_t now_ms) const {
        return _armed && (_arm_expires_ms == 0 || static_cast<int32_t>(now_ms - _arm_expires_ms) < 0);
    }
    uint32_t arm_remaining_ms(uint32_t now_ms) const {
        if (!armed_at(now_ms)) return 0;
        return _arm_expires_ms == 0 ? UINT32_MAX : _arm_expires_ms - now_ms;
    }

    // Any loss of the link drops arming; reconnecting never restores it.
    void on_link_lost() {
        disarm();
        abort_sequence();
    }

    uint32_t blocked() const { return _gate.blocked(); }
    uint32_t sent() const { return _gate.sent(); }
    uint32_t previewed() const { return _gate.previewed(); }
    JcAuthDecision last_decision() const { return _gate.last(); }

    uint8_t history_count() const { return _count; }
    // index 0 = newest
    const JcDispatchRecord* history(uint8_t index) const {
        if (index >= _count) {
            return nullptr;
        }
        return &_history[(_next + kHistory - 1 - index) % kHistory];
    }

private:
    void record(const JcCommand& c, JcAuthDecision d, bool valid, uint32_t ms) {
        JcDispatchRecord& r = _history[_next];
        if (valid && c.realtime) {
            snprintf(r.text, sizeof(r.text), "<realtime 0x%02X>", static_cast<unsigned>(static_cast<uint8_t>(c.text[0])));
        } else if (valid) {
            strncpy(r.text, c.text, sizeof(r.text) - 1);
            r.text[sizeof(r.text) - 1] = '\0';
        } else {
            strcpy(r.text, "(rejected by validation)");
        }
        r.cls = c.cls;
        r.decision = d;
        r.valid = valid;
        r.ms = ms;
        _next = (_next + 1) % kHistory;
        if (_count < kHistory) {
            ++_count;
        }
    }

    JcCommandGate    _gate;
    JcCommand        _pending[kJcMaxSequenceSteps];
    uint8_t          _pending_count = 0;
    uint8_t          _pending_next = 0;
    bool             _armed = false;
    uint32_t         _arm_expires_ms = 0;  // 0 = no expiry
    JcDispatchRecord _history[kHistory] {};
    uint8_t          _next = 0;
    uint8_t          _count = 0;
};

}  // namespace jc8048w550
