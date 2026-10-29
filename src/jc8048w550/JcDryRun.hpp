// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Diagnostic dry run: shows, for a requested action, the FluidNC command that
// would be generated, the authorization decision the real gate would make, the
// reply that would be expected and what happens on timeout, error and loss of
// connection. It evaluates only; it records nothing, counts nothing and never
// reaches a transmit sink. Pure logic shared by the dry-run page and the tests.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcCommandExec.hpp"
#include "JcDispatch.hpp"

namespace jc8048w550 {

struct JcDryRunReport {
    bool           built = false;           // the command builder accepted the request
    char           action[48] = "";
    char           command[kJcMaxCommandLength + 16] = "";
    JcCommandClass cls = JcCommandClass::Other;
    JcAuthDecision decision = JcAuthDecision::BlockedBadState;
    bool           would_transmit = false;  // decision == Allowed (still not sent)
    uint32_t       timeout_ms = 0;
    char           expected[72] = "";
    char           on_timeout[96] = "";
    char           on_error[96] = "";
    char           on_link_loss[96] = "";
    char           rejected_reason[48] = "";
};

inline const char* jc_expected_reply(JcCommandClass cls, bool realtime) {
    if (realtime) {
        return "none (real-time byte); watch the status report";
    }
    switch (cls) {
        case JcCommandClass::Home: return "ok after the homing cycle finishes";
        case JcCommandClass::Probe: return "ok after the probe move finishes";
        case JcCommandClass::Job: return "ok when the job is accepted";
        default: return "ok";
    }
}

inline JcDryRunReport jc_dry_run(const JcDispatcher& dispatcher, const char* action, const JcCommand& command,
                                 const JcDispatchContext& ctx, const char* rejected_reason = "") {
    JcDryRunReport r;
    snprintf(r.action, sizeof(r.action), "%s", action != nullptr ? action : "");
    r.cls = command.cls;
    if (!command.ok) {
        snprintf(r.command, sizeof(r.command), "(not generated)");
        snprintf(r.rejected_reason, sizeof(r.rejected_reason), "%s", rejected_reason != nullptr ? rejected_reason : "");
        r.decision = JcAuthDecision::BlockedBadState;
        snprintf(r.expected, sizeof(r.expected), "n/a - nothing would be sent");
        return r;
    }
    r.built = true;
    if (command.realtime) {
        snprintf(r.command, sizeof(r.command), "real-time byte 0x%02X", static_cast<unsigned>(static_cast<uint8_t>(command.text[0])));
    } else {
        snprintf(r.command, sizeof(r.command), "%s", command.text);
    }
    r.decision = dispatcher.evaluate(command, ctx);
    r.would_transmit = r.decision == JcAuthDecision::Allowed;
    r.timeout_ms = command.realtime ? 0 : jc_command_timeout_ms(command.cls);
    snprintf(r.expected, sizeof(r.expected), "%s", jc_expected_reply(command.cls, command.realtime));
    if (command.realtime) {
        snprintf(r.on_timeout, sizeof(r.on_timeout), "n/a (no reply is awaited)");
        snprintf(r.on_error, sizeof(r.on_error), "n/a");
    } else {
        snprintf(r.on_timeout, sizeof(r.on_timeout), "abandoned after %lu ms; never resent; resync wait %lu ms",
                 static_cast<unsigned long>(r.timeout_ms), static_cast<unsigned long>(kJcResyncQuietMs));
        snprintf(r.on_error, sizeof(r.on_error), "error:N shown to operator; remaining sequence steps dropped");
    }
    snprintf(r.on_link_loss, sizeof(r.on_link_loss), "disarm; drop held steps; never replayed on reconnect");
    return r;
}

}  // namespace jc8048w550
