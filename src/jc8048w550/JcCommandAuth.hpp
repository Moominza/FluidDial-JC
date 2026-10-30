// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Centralized command authorization. EVERY command that could change machine
// state must be submitted through JcCommandGate. In the default build
// (JC_MACHINE_CONTROL_ENABLED == 0) nothing is ever authorized, and the
// firmware wires no transmit sink, so commands are preview-only.
//
// The only outgoing byte the current firmware may transmit is the '?' status
// query, which is sent by JcReadOnlyFluidNc and is not a machine command.

#pragma once

#include <stdint.h>
#include <string.h>

#ifndef JC_MACHINE_CONTROL_ENABLED
#define JC_MACHINE_CONTROL_ENABLED 0
#endif

// Experimental, explicitly named live-jog TEST configuration (PlatformIO env
// jc8048w550_livejog_test). Always 0 in the ordinary firmware. When 1 (together
// with JC_MACHINE_CONTROL_ENABLED=1) the gate authorizes ONLY an incremental
// jog and the jog-cancel byte; every other command class stays blocked, and the
// jog limits in JcJogTest.hpp apply. tools/check_no_sink.py enforces that the
// macro is set nowhere except that one environment.
#ifndef JC_LIVE_JOG_TEST
#define JC_LIVE_JOG_TEST 0
#endif

namespace jc8048w550 {

enum class JcCommandClass : uint8_t {
    Jog,
    JogCancel,
    Home,
    Probe,
    WorkOffset,
    Spindle,
    Job,       // run / pause / resume / cancel
    Macro,
    Unlock,
    Reset,
    Hold,
    Other,     // anything not otherwise classified is treated as machine-changing
};

enum class JcAuthDecision : uint8_t {
    Allowed,
    BlockedBuildPolicy,    // JC_MACHINE_CONTROL_ENABLED == 0
    BlockedNotArmed,       // operator has not armed machine control this session
    BlockedNotLive,        // DEMO mode or no live connection
    BlockedStaleLink,      // status data is stale
    BlockedStaleCommand,   // command too old: never replay a stale jog
    BlockedAlarm,          // alarm state blocks everything but unlock/reset/stop
    BlockedUnconfirmed,    // operator confirmation required and missing
    BlockedBadState,       // machine state does not permit this command class
    BlockedLimitsUnconfirmed,  // axis limits are still placeholders, not controller values
    BlockedOperationPending,   // a previous motion operation is not yet resolved
    BlockedEnvelopeUnread,     // soft / hard limit settings were not read for this axis
};

struct JcAuthContext {
    bool        armed = false;           // explicit per-session operator arming
    bool        live = false;            // LIVE mode, not DEMO
    bool        link_fresh = false;      // fresh valid status
    bool        operator_confirmed = false;
    // A move on an axis may only be authorized once that axis's limits were read
    // from the controller (JcMachineCaps). The dispatcher sets both fields.
    bool        limits_required = false;
    bool        limits_confirmed = false;
    // ... and once its soft / hard limit settings were read, so the operator has
    // been shown what the controller itself enforces. A configured travel
    // distance is NOT a proof of a safe movement envelope.
    bool        envelope_required = false;
    bool        envelope_confirmed = false;
    // A previous jog / motion operation has not been resolved (acknowledged,
    // completed and verified): no new motion may start.
    bool        operation_pending = false;
    char        machine_state[20] = "";  // last reported state
    uint32_t    now_ms = 0;
    uint32_t    command_issued_ms = 0;
};

constexpr uint32_t kJcMaxCommandAgeMs = 500;

inline const char* jc_auth_name(JcAuthDecision d) {
    switch (d) {
        case JcAuthDecision::Allowed: return "allowed";
        case JcAuthDecision::BlockedBuildPolicy: return "blocked: machine control disabled in this build";
        case JcAuthDecision::BlockedNotArmed: return "blocked: machine control not armed";
        case JcAuthDecision::BlockedNotLive: return "blocked: not in LIVE mode";
        case JcAuthDecision::BlockedStaleLink: return "blocked: stale or missing machine status";
        case JcAuthDecision::BlockedStaleCommand: return "blocked: stale command";
        case JcAuthDecision::BlockedAlarm: return "blocked: machine in alarm";
        case JcAuthDecision::BlockedUnconfirmed: return "blocked: operator confirmation required";
        case JcAuthDecision::BlockedBadState: return "blocked: machine state does not permit command";
        case JcAuthDecision::BlockedLimitsUnconfirmed: return "blocked: axis limits not confirmed by the controller";
        case JcAuthDecision::BlockedOperationPending: return "blocked: a previous operation is unresolved";
        case JcAuthDecision::BlockedEnvelopeUnread: return "blocked: soft/hard limit settings not read from the controller";
    }
    return "blocked";
}

inline bool jc_needs_confirmation(JcCommandClass c) {
    switch (c) {
        case JcCommandClass::Home:
        case JcCommandClass::Probe:
        case JcCommandClass::WorkOffset:
        case JcCommandClass::Spindle:
        case JcCommandClass::Job:
        case JcCommandClass::Macro:
        case JcCommandClass::Other:
            return true;
        case JcCommandClass::Jog:
            return JC_LIVE_JOG_TEST != 0;  // every test jog is confirmed by the operator
        default:
            return false;  // stops/unlock must be immediate
    }
}

// Stopping must never be blocked by bookkeeping (arming, freshness, command age):
// the jog-cancel and feed-hold bytes only need a real (LIVE) connection.
inline bool jc_is_stop_always_allowed(JcCommandClass c) {
    return c == JcCommandClass::JogCancel || c == JcCommandClass::Hold;
}

// The classes the experimental live-jog build may ever authorize.
inline bool jc_live_jog_class_allowed(JcCommandClass c) {
    return c == JcCommandClass::Jog || c == JcCommandClass::JogCancel;
}

// Commands whose purpose is to stop or recover; allowed in alarm / any state.
inline bool jc_is_stop_or_recovery(JcCommandClass c) {
    return c == JcCommandClass::JogCancel || c == JcCommandClass::Hold
        || c == JcCommandClass::Reset || c == JcCommandClass::Unlock;
}

inline JcAuthDecision jc_authorize(JcCommandClass cls, const JcAuthContext& ctx) {
#if !JC_MACHINE_CONTROL_ENABLED
    (void)cls;
    (void)ctx;
    return JcAuthDecision::BlockedBuildPolicy;
#else
#if JC_LIVE_JOG_TEST
    // Continuous jog is not authorizable here either: the transport only accepts the
    // strict incremental line (JcJogTest.hpp), and the builder never generates it.
    if (!jc_live_jog_class_allowed(cls)) {
        return JcAuthDecision::BlockedBuildPolicy;
    }
#endif
    if (jc_is_stop_always_allowed(cls)) {
        return ctx.live ? JcAuthDecision::Allowed : JcAuthDecision::BlockedNotLive;
    }
    if (!ctx.armed) {
        return JcAuthDecision::BlockedNotArmed;
    }
    if (!ctx.live) {
        return JcAuthDecision::BlockedNotLive;
    }
    if (!ctx.link_fresh) {
        return JcAuthDecision::BlockedStaleLink;
    }
    if (ctx.now_ms - ctx.command_issued_ms > kJcMaxCommandAgeMs) {
        return JcAuthDecision::BlockedStaleCommand;
    }
    if (jc_is_stop_or_recovery(cls)) {
        return JcAuthDecision::Allowed;
    }
    if (strcmp(ctx.machine_state, "Alarm") == 0) {
        return JcAuthDecision::BlockedAlarm;
    }
    if (ctx.operation_pending) {
        return JcAuthDecision::BlockedOperationPending;
    }
    if (ctx.limits_required && !ctx.limits_confirmed) {
        return JcAuthDecision::BlockedLimitsUnconfirmed;
    }
    if (ctx.envelope_required && !ctx.envelope_confirmed) {
        return JcAuthDecision::BlockedEnvelopeUnread;
    }
    const bool motion_idle_only = cls == JcCommandClass::Home || cls == JcCommandClass::Probe
                                  || cls == JcCommandClass::WorkOffset || cls == JcCommandClass::Macro
                                  || cls == JcCommandClass::Other || cls == JcCommandClass::Spindle;
    if (motion_idle_only && strcmp(ctx.machine_state, "Idle") != 0) {
        return JcAuthDecision::BlockedBadState;
    }
    if (cls == JcCommandClass::Jog
        && strcmp(ctx.machine_state, "Idle") != 0 && strcmp(ctx.machine_state, "Jog") != 0) {
        return JcAuthDecision::BlockedBadState;
    }
    if (jc_needs_confirmation(cls) && !ctx.operator_confirmed) {
        return JcAuthDecision::BlockedUnconfirmed;
    }
    return JcAuthDecision::Allowed;
#endif
}

// Sink that actually transmits an authorized command. The JC firmware does
// not provide one in this build, so even an Allowed decision transmits nothing.
using JcCommandSink = bool (*)(const char* text);
// Same, with a context pointer (used only by host tests; the firmware never
// installs a sink - tools/check_no_sink.py enforces that).
using JcCommandSinkEx = bool (*)(void* user, JcCommandClass cls, const char* text);

class JcCommandGate {
public:
    explicit JcCommandGate(JcCommandSink sink = nullptr) : _sink(sink) {}

    JcAuthDecision submit(JcCommandClass cls, const char* text, const JcAuthContext& ctx) {
        const JcAuthDecision decision = jc_authorize(cls, ctx);
        _last = decision;
        if (decision != JcAuthDecision::Allowed) {
            ++_blocked;
            return decision;
        }
        const bool transmitted = (_sink_ex != nullptr && _sink_ex(_sink_user, cls, text))
                                 || (_sink != nullptr && _sink(text));
        if (transmitted) {
            ++_sent;
        } else {
            ++_previewed;  // authorized but no transmit path: preview only
        }
        return decision;
    }

    void set_sink_ex(JcCommandSinkEx sink, void* user) {
        _sink_ex = sink;
        _sink_user = user;
    }

    uint32_t       sent() const { return _sent; }
    uint32_t       blocked() const { return _blocked; }
    uint32_t       previewed() const { return _previewed; }
    JcAuthDecision last() const { return _last; }

private:
    JcCommandSink  _sink;
    JcCommandSinkEx _sink_ex = nullptr;
    void*          _sink_user = nullptr;
    uint32_t       _sent = 0;
    uint32_t       _blocked = 0;
    uint32_t       _previewed = 0;
    JcAuthDecision _last = JcAuthDecision::BlockedBuildPolicy;
};

}  // namespace jc8048w550
