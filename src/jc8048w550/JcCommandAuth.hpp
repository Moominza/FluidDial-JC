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

// Ordinary firmware only (PlatformIO env jc8048w550): compiles in the NON-MOTION jog test mode, an
// operator-enabled (never persisted, off after every restart) rehearsal of the experimental jog
// interface against a SIMULATED controller in RAM. It has no transmit path to the machine; the
// real gate is still JC_MACHINE_CONTROL_ENABLED=0. The two experimental options exclude each other.
#ifndef JC_JOG_PREVIEW
#define JC_JOG_PREVIEW 0
#endif
#if JC_JOG_PREVIEW && (JC_LIVE_JOG_TEST || JC_MACHINE_CONTROL_ENABLED)
#error "JC_JOG_PREVIEW (non-motion test mode) cannot be combined with live jog or enabled machine control"
#endif

// The jog page, arming, confirmation and result UI is compiled for the experimental live-jog build AND for
// the ordinary firmware's non-motion preview. Transport code (the only thing that can reach a socket) is
// compiled for JC_LIVE_JOG_TEST alone.
#define JC_JOG_UI (JC_LIVE_JOG_TEST || JC_JOG_PREVIEW)
#if JC_JOG_PREVIEW
#define JC_JOG_TAG "jogpreview"
#else
#define JC_JOG_TAG "jogtest"
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
    BlockedEnvelopeUnread,     // soft/hard-limit and limit-pin settings are not known from a current read
    BlockedUnprotectedNotAcknowledged,  // no trusted travel protection and the operator has not acknowledged that
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
    // Live-jog test policy only: when the axis has no TRUSTED controller travel protection
    // (soft/hard limits OFF, or a position that is not established) the operator must have
    // acknowledged exactly that, for this axis, in this arming. Cleared on any link loss.
    bool        unprotected_required = false;
    bool        unprotected_acked = false;
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
        case JcAuthDecision::BlockedEnvelopeUnread: return "blocked: limit settings not known from a current read";
        case JcAuthDecision::BlockedUnprotectedNotAcknowledged:
            return "blocked: no controller travel protection; not acknowledged";
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

namespace detail {

// The authorization rules after the build policy. `jog_only_rules` is the experimental jog test
// policy: only Jog and JogCancel are ever authorized, every jog needs the operator's confirmation,
// and a jog without trusted travel protection needs the per-axis acknowledgement. It is a RUNTIME
// parameter so the non-motion preview mode (jc_authorize_preview) runs exactly the rules of the
// live-jog build instead of a second copy that could drift.
inline JcAuthDecision authorize_rules(JcCommandClass cls, const JcAuthContext& ctx, bool jog_only_rules) {
    if (jog_only_rules && !jc_live_jog_class_allowed(cls)) {
        // Continuous jog is not authorizable here either: the transport only accepts the
        // strict incremental line (JcJogTest.hpp), and the builder never generates it.
        return JcAuthDecision::BlockedBuildPolicy;
    }
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
    if (jog_only_rules && ctx.unprotected_required && !ctx.unprotected_acked) {
        return JcAuthDecision::BlockedUnprotectedNotAcknowledged;
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
    const bool needs_confirmation = jc_needs_confirmation(cls) || (jog_only_rules && cls == JcCommandClass::Jog);
    if (needs_confirmation && !ctx.operator_confirmed) {
        return JcAuthDecision::BlockedUnconfirmed;
    }
    return JcAuthDecision::Allowed;
}

}  // namespace detail

inline JcAuthDecision jc_authorize(JcCommandClass cls, const JcAuthContext& ctx) {
#if !JC_MACHINE_CONTROL_ENABLED
    (void)cls;
    (void)ctx;
    return JcAuthDecision::BlockedBuildPolicy;
#else
    return detail::authorize_rules(cls, ctx, JC_LIVE_JOG_TEST != 0);
#endif
}

// NON-MOTION TEST MODE (Instruction 007). The same rules as the live-jog policy, but this decision
// is ONLY consulted when the dispatcher's gate is in preview mode, which only the ordinary firmware's
// operator-enabled test mode sets, and only together with a SIMULATED transport (JcJogPreview.hpp)
// that has no connection to any socket or UART. Allowed here means "the preview controller may
// accept this", never "a machine-changing command may be transmitted".
inline JcAuthDecision jc_authorize_preview(JcCommandClass cls, const JcAuthContext& ctx) {
    return detail::authorize_rules(cls, ctx, true);
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
        const JcAuthDecision decision = _preview ? jc_authorize_preview(cls, ctx) : jc_authorize(cls, ctx);
        _last = decision;
        if (decision != JcAuthDecision::Allowed) {
            ++_blocked;
            return decision;
        }
        const bool accepted = (_sink_ex != nullptr && _sink_ex(_sink_user, cls, text))
                              || (_sink != nullptr && _sink(text));
        if (_preview) {
            // The simulated controller is the only sink here: it either accepted the command (previewed)
            // or declined it. Nothing is ever transmitted, so `sent()` cannot move in preview mode.
            if (accepted) ++_previewed;
            else ++_declined;
        } else if (accepted) {
            ++_sent;
        } else {
            ++_previewed;  // authorized but no transmit path: preview only
        }
        return decision;
    }

    // Non-motion test mode: authorize with the preview policy and count accepted commands as
    // previewed, never as sent. Set only by the ordinary firmware's operator-enabled test mode.
    void set_preview(bool on) { _preview = on; }
    bool preview() const { return _preview; }

    void set_sink_ex(JcCommandSinkEx sink, void* user) {
        _sink_ex = sink;
        _sink_user = user;
    }

    uint32_t       sent() const { return _sent; }
    uint32_t       blocked() const { return _blocked; }
    uint32_t       previewed() const { return _previewed; }
    uint32_t       declined() const { return _declined; }   // preview mode: the simulation refused it
    JcAuthDecision last() const { return _last; }

private:
    JcCommandSink  _sink;
    JcCommandSinkEx _sink_ex = nullptr;
    void*          _sink_user = nullptr;
    uint32_t       _sent = 0;
    uint32_t       _blocked = 0;
    uint32_t       _previewed = 0;
    uint32_t       _declined = 0;
    JcAuthDecision _last = JcAuthDecision::BlockedBuildPolicy;
    bool           _preview = false;
};

}  // namespace jc8048w550
