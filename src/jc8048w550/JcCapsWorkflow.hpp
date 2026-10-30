// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Capability-read workflow shown on the jog page (Instruction 008). Pure: it only classifies what the
// operator should see and whether a read may start; it transmits nothing. The read itself is the
// existing read-only JcCapsLoader (allowlisted `$` queries). Expiry (kJcCapsMaxAgeMs) and the
// session binding (the loader is invalidated on every link loss) are NOT changed here, and UNKNOWN
// values are never turned into known ones.

#pragma once

#include <stdint.h>
#include <string.h>

#include "JcCapsLoader.hpp"
#include "JcJogTest.hpp"
#include "JcMachineCaps.hpp"

namespace jc8048w550 {

enum class JcCapsPhase : uint8_t {
    NotLive,   // DEMO mode: there is no controller to read
    NotRead,   // nothing read in this LIVE session
    Reading,   // a read is running
    Failed,    // the last read failed (operator must retry)
    Expired,   // the read is older than kJcCapsMaxAgeMs
    Current,   // complete and fresh
};

struct JcCapsFlow {
    JcCapsPhase phase = JcCapsPhase::NotLive;
    const char* blocker = nullptr;  // why a read cannot start now (nullptr = it may)
    bool can_read = false;          // READ MACHINE SETTINGS may be offered as active
    bool auto_read = false;         // a read may be started automatically (once per visit)
};

inline JcCapsFlow jc_caps_flow(bool live, bool link_fresh, const char* machine_state, bool operation_unresolved,
                               JcLoadState state, bool caps_complete, uint32_t completed_ms, uint32_t now_ms,
                               bool auto_already_tried) {
    JcCapsFlow f;
    if (!live) {
        f.phase = JcCapsPhase::NotLive;
        f.blocker = "DEMO mode: connect LIVE first";
        return f;
    }
    if (state == JcLoadState::Running) {
        f.phase = JcCapsPhase::Reading;
        f.blocker = "a read is already in progress";
        return f;
    }
    if (state == JcLoadState::Failed) {
        f.phase = JcCapsPhase::Failed;
    } else if (state == JcLoadState::Complete && caps_complete) {
        f.phase = static_cast<uint32_t>(now_ms - completed_ms) > kJcCapsMaxAgeMs ? JcCapsPhase::Expired : JcCapsPhase::Current;
    } else {
        f.phase = JcCapsPhase::NotRead;
    }
    f.blocker = jc_caps_read_blocker(live, link_fresh, machine_state, false);
    if (f.blocker == nullptr && operation_unresolved) f.blocker = "a jog operation is unresolved";
    f.can_read = f.blocker == nullptr;
    // Automatic only when a read is plainly needed, safe to start, and not already tried this visit.
    // A failed read is never retried automatically: the operator sees the reason and retries.
    f.auto_read = f.can_read && !auto_already_tried && (f.phase == JcCapsPhase::NotRead || f.phase == JcCapsPhase::Expired);
    return f;
}

// Whether the jog page may offer ARM. Never true unless the read is complete and current.
inline bool jc_caps_allow_arming(const JcCapsFlow& f) { return f.phase == JcCapsPhase::Current; }

// Text for the button that shares the ARM slot: what tapping it will do.
inline const char* jc_caps_action_label(const JcCapsFlow& f) {
    switch (f.phase) {
        case JcCapsPhase::NotLive: return "NOT LIVE";
        case JcCapsPhase::Reading: return "READING...";
        case JcCapsPhase::Failed: return "RETRY READ";
        case JcCapsPhase::NotRead:
        case JcCapsPhase::Expired: return "READ SETTINGS";
        case JcCapsPhase::Current: return "ARM";
    }
    return "?";
}

// One status line (what the operator needs to know, no jargon). `step`/`steps` = read progress.
inline void jc_caps_status_text(const JcCapsFlow& f, uint8_t step, uint8_t steps, JcLoadFailure failure, uint32_t age_ms,
                                char* out, size_t size) {
    switch (f.phase) {
        case JcCapsPhase::NotLive:
            snprintf(out, size, "DEMO mode: no controller to read");
            break;
        case JcCapsPhase::NotRead:
            snprintf(out, size, "Machine settings NOT read. %s", f.can_read ? "Tap READ SETTINGS." : f.blocker);
            break;
        case JcCapsPhase::Reading:
            snprintf(out, size, "Reading machine settings: %u of %u queries (read-only)", static_cast<unsigned>(step),
                     static_cast<unsigned>(steps));
            break;
        case JcCapsPhase::Failed:
            snprintf(out, size, "Read FAILED: %s. %s", jc_load_failure_name(failure), f.can_read ? "Tap RETRY READ." : f.blocker);
            break;
        case JcCapsPhase::Expired:
            snprintf(out, size, "Machine settings EXPIRED (%u min old). %s", static_cast<unsigned>(age_ms / 60000u),
                     f.can_read ? "Tap READ SETTINGS." : f.blocker);
            break;
        case JcCapsPhase::Current:
            snprintf(out, size, "Machine settings read %u s ago (valid %u min)", static_cast<unsigned>(age_ms / 1000u),
                     static_cast<unsigned>(kJcCapsMaxAgeMs / 60000u));
            break;
    }
}

}  // namespace jc8048w550
