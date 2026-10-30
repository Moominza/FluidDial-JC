// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the NON-MOTION jog test mode (Instruction 007): the real gate, dispatcher and jog
// session run against the SIMULATED controller, and nothing can reach a transmit path.
//
// Built by tools/run_host_tests.py with the ordinary policy (JC_MACHINE_CONTROL_ENABLED=0) and with
// the rules compiled in (=1). Never with JC_LIVE_JOG_TEST: preview and live jog are exclusive.

#include <stdio.h>
#include <string.h>

#include "../../src/jc8048w550/JcCommandAuth.hpp"
#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcJogPreview.hpp"
#include "../../src/jc8048w550/JcJogTest.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct Rig {
    JcPreviewBridge bridge;
    JcJogSession    session;
    JcDispatcher    disp;
    uint32_t        now = 5000;
    uint32_t        seq = 1;
    JcJogStatus     real;
    char            state[16] = "Idle";

    Rig() : session(&JcPreviewController::tx, &bridge.controller()) {
        disp.set_preview(true);
        disp.set_sink_ex(&JcJogSession::sink, &session);
        real.fresh = true;
        real.state = state;
        real.pos_valid = true;
    }
    void step(uint32_t ms) {
        const uint32_t end = now + ms;
        while (now < end) {
            now += 10;
            if (now % 250 == 0) real.sequence = ++seq;
            session.set_clock(now);
            bridge.pump(session, real, now);
            session.tick(now);
        }
    }
    JcDispatchContext ctx(bool confirmed) const {
        JcDispatchContext c;
        c.live = true;
        c.link_fresh = true;
        c.machine_state = "Idle";
        c.operator_confirmed = confirmed;
        c.now_ms = now;
        c.limits_mask = 0x03;
        c.envelope_mask = 0x03;
        c.protected_mask = 0;
        return c;
    }
    JcAuthDecision jog(uint8_t axis, float d, float feed, bool confirmed = true) {
        JcMachineProfile p = jc_default_profile();
        jc_profile_apply_test_limits(p);
        const JcCommand c = jc_jog_incremental(p, axis, d, feed);
        if (!c.ok) return JcAuthDecision::BlockedBadState;
        return disp.submit(c, ctx(confirmed));
    }
};

static void test_gate_policy() {
    // Without the preview flag the real gate still refuses everything in the ordinary build.
    JcDispatcher plain;
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    JcDispatchContext x;
    x.live = true; x.link_fresh = true; x.machine_state = "Idle"; x.operator_confirmed = true;
    x.limits_mask = 3; x.envelope_mask = 3; x.now_ms = 100;
    plain.arm_for(100, 60000, 0x01);
    plain.set_sink_ex(&JcJogSession::sink, nullptr);
    CHECK(!plain.preview());
    const JcCommand c = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    (void)c;
#if !JC_MACHINE_CONTROL_ENABLED
    CHECK(plain.submit(c, x) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(plain.sent() == 0 && plain.previewed() == 0);
#endif

    Rig r;
    CHECK(r.disp.preview());
    r.step(600);
    // not armed
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::BlockedNotArmed);
    // armed but not acknowledged
    r.disp.arm_for(r.now, 60000, 0);
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    r.disp.arm_for(r.now, 60000, 0x01);
    // unconfirmed
    CHECK(r.jog(0, 1.0f, 100.0f, false) == JcAuthDecision::BlockedUnconfirmed);
    // oversized and Z / A never get as far as the gate
    CHECK(r.jog(0, 10.0f, 100.0f) == JcAuthDecision::BlockedBadState);
    CHECK(r.jog(2, 0.5f, 50.0f) == JcAuthDecision::BlockedBadState);
    CHECK(r.jog(3, 1.0f, 100.0f) == JcAuthDecision::BlockedBadState);
    // wrong axis acknowledged
    CHECK(r.jog(1, 1.0f, 100.0f) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    CHECK(r.bridge.controller().lines() == 0);
    // non-jog classes are never authorized in the preview either
    CHECK(jc_authorize_preview(JcCommandClass::Home, JcAuthContext()) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(jc_authorize_preview(JcCommandClass::Spindle, JcAuthContext()) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(jc_authorize_preview(JcCommandClass::Macro, JcAuthContext()) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(r.disp.sent() == 0);
}

static void test_accepted_is_previewed_never_sent() {
    Rig r;
    r.step(600);
    r.disp.arm_for(r.now, 60000, 0x01);
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::Allowed);
    CHECK(r.disp.sent() == 0);
    CHECK(r.disp.previewed() == 1);
    CHECK(r.bridge.controller().lines() == 1);
    CHECK(strcmp(r.bridge.controller().last_line(), "$J=G91G21F100X1.000") == 0);
    r.step(1500);
    CHECK(r.session.phase() == JcJogPhase::Idle || r.session.phase() == JcJogPhase::Recovering || r.session.result() == JcJogResult::Completed);
    CHECK(r.disp.sent() == 0);
}

static void test_scenarios() {
    const JcPreviewScenario all[] = { JcPreviewScenario::Normal, JcPreviewScenario::Rejected, JcPreviewScenario::Alarm,
                                      JcPreviewScenario::NoReply, JcPreviewScenario::SlowAck, JcPreviewScenario::IgnoreCancel };
    for (JcPreviewScenario s : all) {
        Rig r;
        r.bridge.controller().set_scenario(s);
        r.step(600);
        r.disp.arm_for(r.now, 600000, 0x01);
        CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::Allowed);
        r.step(300);
        switch (s) {
            case JcPreviewScenario::Rejected: r.step(1000); CHECK(r.session.result() == JcJogResult::Rejected); break;
            case JcPreviewScenario::Alarm: r.step(1000); CHECK(r.session.result() == JcJogResult::Alarm); break;
            case JcPreviewScenario::NoReply: r.step(8000); CHECK(r.session.result() != JcJogResult::None); break;
            case JcPreviewScenario::Normal: r.step(2000); CHECK(r.session.result() == JcJogResult::Completed); break;
            default: break;
        }
        // Cancel is always attempted, in every scenario, and still only reaches the simulation.
        JcCommand cancel = jc_jog_cancel();
        r.disp.submit(cancel, r.ctx(false));
        r.step(4000);
        CHECK(r.disp.sent() == 0);
        CHECK(r.bridge.controller().refused() == 0);
    }
}

static void test_cancel_while_moving() {
    Rig r;
    r.step(600);
    r.disp.arm_for(r.now, 600000, 0x01);
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::Allowed);
    r.step(400);
    const uint32_t before = r.bridge.controller().cancels();
    JcCommand cancel = jc_jog_cancel();
    CHECK(r.disp.submit(cancel, r.ctx(false)) == JcAuthDecision::Allowed);
    CHECK(r.bridge.controller().cancels() == before + 1);
    r.step(4000);
    CHECK(r.session.result() == JcJogResult::Cancelled);
    CHECK(r.disp.sent() == 0);
}

static void test_transport_refuses_foreign_lines() {
    JcPreviewController c;
    const char* bad[] = { "G0 X10\n", "$H\n", "$J=G91G21F500X10.000\n", "$J=G91G21F100Z0.500\n", "$J=G91G21F100X1.000", "M3 S1000\n", "$X\n" };
    for (const char* b : bad) CHECK(!c.on_tx(b, strlen(b)));
    CHECK(c.lines() == 0);
    CHECK(c.refused() == 7);
    const char* good = "$J=G91G21F100X1.000\n";
    CHECK(c.on_tx(good, strlen(good)));
    CHECK(c.lines() == 1);
}

static void test_declined_is_not_previewed() {
    // A simulation that declines (here: no transport at all) never counts as previewed or sent.
    Rig r;
    r.disp.set_sink_ex([](void*, JcCommandClass, const char*) { return false; }, nullptr);
    r.step(600);
    r.disp.arm_for(r.now, 600000, 0x01);
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::Allowed);
    CHECK(r.disp.previewed() == 0 && r.disp.declined() == 1 && r.disp.sent() == 0);
}

static void test_link_loss_clears() {
    Rig r;
    r.step(600);
    r.disp.arm_for(r.now, 600000, 0x03);
    CHECK(r.disp.armed() && r.disp.unprotected_ack_mask() == 0x03);
    r.disp.on_link_lost();   // Disconnect, DEMO, stale status and Wi-Fi loss all end here
    CHECK(!r.disp.armed() && r.disp.unprotected_ack_mask() == 0);
    CHECK(r.jog(0, 1.0f, 100.0f) == JcAuthDecision::BlockedNotArmed);
    // DEMO mode
    r.disp.arm_for(r.now, 600000, 0x01);
    CHECK(r.disp.enforce_arming_scope(r.now, false, true) == JcDisarmReason::DemoMode);
    CHECK(r.disp.unprotected_ack_mask() == 0);
    // not live and stale link block even when armed
    r.disp.arm_for(r.now, 600000, 0x01);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    const JcCommand c = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    JcDispatchContext x = r.ctx(true);
    x.live = false;
    CHECK(r.disp.submit(c, x) == JcAuthDecision::BlockedNotLive);
    x = r.ctx(true);
    x.link_fresh = false;
    CHECK(r.disp.submit(c, x) == JcAuthDecision::BlockedStaleLink);
    CHECK(r.bridge.controller().lines() == 0 && r.disp.sent() == 0);
}

int main() {
    test_gate_policy();
    test_accepted_is_previewed_never_sent();
    test_scenarios();
    test_cancel_while_moving();
    test_transport_refuses_foreign_lines();
    test_declined_is_not_previewed();
    test_link_loss_clears();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
