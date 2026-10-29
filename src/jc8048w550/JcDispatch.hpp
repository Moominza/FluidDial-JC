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

namespace jc8048w550 {

struct JcDispatchContext {
    bool        live = false;
    bool        link_fresh = false;
    const char* machine_state = "";
    bool        operator_confirmed = false;
    uint32_t    now_ms = 0;
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

    JcAuthDecision submit(const JcCommand& command, const JcDispatchContext& ctx) {
        if (!command.ok) {
            record(command, JcAuthDecision::BlockedBadState, false, ctx.now_ms);
            return JcAuthDecision::BlockedBadState;
        }
        JcAuthContext auth;
        auth.armed = _armed;
        auth.live = ctx.live;
        auth.link_fresh = ctx.link_fresh;
        auth.operator_confirmed = ctx.operator_confirmed;
        auth.now_ms = ctx.now_ms;
        auth.command_issued_ms = ctx.now_ms;
        strncpy(auth.machine_state, ctx.machine_state ? ctx.machine_state : "", sizeof(auth.machine_state) - 1);
        const JcAuthDecision decision = _gate.submit(command.cls, command.text, auth);
        record(command, decision, true, ctx.now_ms);
        return decision;
    }

    // Submits steps in order and stops at the first one that is not allowed.
    // Returns that decision (or Allowed if every step was authorized).
    JcAuthDecision submit_sequence(const JcCommandSequence& seq, const JcDispatchContext& ctx) {
        if (!seq.ok || seq.count == 0) {
            JcCommand bad;
            record(bad, JcAuthDecision::BlockedBadState, false, ctx.now_ms);
            return JcAuthDecision::BlockedBadState;
        }
        for (uint8_t i = 0; i < seq.count; ++i) {
            const JcAuthDecision d = submit(seq.steps[i], ctx);
            if (d != JcAuthDecision::Allowed) {
                return d;
            }
        }
        return JcAuthDecision::Allowed;
    }

    // Explicit operator arming. Nothing in the shipping UI calls arm().
    void arm() { _armed = true; }
    void disarm() { _armed = false; }
    bool armed() const { return _armed; }

    // Any loss of the link drops arming; reconnecting never restores it.
    void on_link_lost() { _armed = false; }

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
    bool             _armed = false;
    JcDispatchRecord _history[kHistory] {};
    uint8_t          _next = 0;
    uint8_t          _count = 0;
};

}  // namespace jc8048w550
