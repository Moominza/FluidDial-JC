// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the jog page's capability-read workflow (Instruction 008).

#include <stdio.h>
#include <string.h>

#include "../../src/jc8048w550/JcCapsWorkflow.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static JcCapsFlow flow(bool live, bool fresh, const char* st, bool unresolved, JcLoadState ls, bool complete,
                       uint32_t done, uint32_t now, bool tried = false) {
    return jc_caps_flow(live, fresh, st, unresolved, ls, complete, done, now, tried);
}

int main() {
    // Nothing read, safe to start: automatic read is allowed once.
    JcCapsFlow f = flow(true, true, "Idle", false, JcLoadState::Idle, false, 0, 1000);
    CHECK(f.phase == JcCapsPhase::NotRead && f.can_read && f.auto_read && !jc_caps_allow_arming(f));
    CHECK(strcmp(jc_caps_action_label(f), "READ SETTINGS") == 0);
    // ... but not a second time in the same visit.
    f = flow(true, true, "Idle", false, JcLoadState::Idle, false, 0, 1000, true);
    CHECK(f.can_read && !f.auto_read);

    // Unsafe conditions never start an automatic read.
    CHECK(!flow(true, true, "Jog", false, JcLoadState::Idle, false, 0, 1000).can_read);
    CHECK(!flow(true, true, "Run", false, JcLoadState::Idle, false, 0, 1000).auto_read);
    CHECK(!flow(true, false, "Idle", false, JcLoadState::Idle, false, 0, 1000).can_read);   // stale status
    CHECK(!flow(true, true, "Idle", true, JcLoadState::Idle, false, 0, 1000).can_read);     // unresolved jog
    CHECK(!flow(false, true, "Idle", false, JcLoadState::Idle, false, 0, 1000).can_read);   // DEMO
    CHECK(flow(false, true, "Idle", false, JcLoadState::Idle, false, 0, 1000).phase == JcCapsPhase::NotLive);

    // Reading: no second read, no arming.
    f = flow(true, true, "Idle", false, JcLoadState::Running, false, 0, 1000);
    CHECK(f.phase == JcCapsPhase::Reading && !f.can_read && !f.auto_read && !jc_caps_allow_arming(f));

    // Failed: retry is offered but never automatic; arming stays off.
    f = flow(true, true, "Idle", false, JcLoadState::Failed, false, 0, 1000);
    CHECK(f.phase == JcCapsPhase::Failed && f.can_read && !f.auto_read && !jc_caps_allow_arming(f));
    CHECK(strcmp(jc_caps_action_label(f), "RETRY READ") == 0);

    // Complete and fresh: arming allowed; expiry boundary unchanged (5 minutes).
    f = flow(true, true, "Idle", false, JcLoadState::Complete, true, 1000, 1000 + kJcCapsMaxAgeMs);
    CHECK(f.phase == JcCapsPhase::Current && jc_caps_allow_arming(f) && !f.auto_read);
    f = flow(true, true, "Idle", false, JcLoadState::Complete, true, 1000, 1001 + kJcCapsMaxAgeMs);
    CHECK(f.phase == JcCapsPhase::Expired && !jc_caps_allow_arming(f) && f.auto_read);
    // The workflow and the gate agree on the boundary.
    JcMachineCaps caps;
    caps.complete = true;
    caps.completed_ms = 1000;
    const JcJogGate g = jc_jog_gate(caps, true, 1001 + kJcCapsMaxAgeMs, 0, JcPositionTrust::Untrusted);
    CHECK(g.reason == JcJogGateReason::Stale);
    // Complete state without complete caps is not "current".
    f = flow(true, true, "Idle", false, JcLoadState::Complete, false, 1000, 2000);
    CHECK(f.phase == JcCapsPhase::NotRead && !jc_caps_allow_arming(f));

    // Alarm: a read is permitted (read-only) but not automatic arming.
    f = flow(true, true, "Alarm", false, JcLoadState::Idle, false, 0, 1000);
    CHECK(f.can_read);

    // Text: every phase fits one line (<= 90 chars) and says what to do.
    char t[160];
    const JcCapsPhase phases[] = { JcCapsPhase::NotLive, JcCapsPhase::NotRead, JcCapsPhase::Reading, JcCapsPhase::Failed,
                                   JcCapsPhase::Expired, JcCapsPhase::Current };
    for (JcCapsPhase p : phases) {
        JcCapsFlow x;
        x.phase = p;
        x.can_read = true;
        jc_caps_status_text(x, 40, 55, JcLoadFailure::TimedOut, 400000, t, sizeof(t));
        CHECK(strlen(t) > 5 && strlen(t) < 90);
    }
    printf("%d checks, %d failed (capability workflow)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
