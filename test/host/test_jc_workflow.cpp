// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the guided positioning and probing routines (Instruction 009A) against a SIMULATED
// FluidNC with a probe-able feature (bore, boss, rotary cylinder), a work-offset table and fault
// injection. Built with -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_FULL_CONTROL_TEST=1.

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCommandSystem.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcSimNc.hpp"
#include "../../src/jc8048w550/JcWorkflow.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tol) do { ++g_run; if (fabsf(static_cast<float>(a) - static_cast<float>(b)) > (tol)) { ++g_fail; printf("FAIL %s:%d  %s=%g vs %g\n", __FILE__, __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); } } while (0)

// ---- the simulated controller ---------------------------------------------------------------------------

using Feature = JcSimFeature;

// The shared simulated FluidNC plus a log of everything the transport wrote to it.
struct Nc : JcSimNc {
    std::vector<std::string> wire;
    bool always_error = false;
    static bool send(void* self, const char* bytes, size_t n) {
        Nc* s = static_cast<Nc*>(self);
        s->wire.emplace_back(bytes, n);
        return s->JcSimNc::send(bytes, n);
    }
    void command(const std::string& line) {   // read-only queries written directly
        const std::string l = line + "\n";
        JcSimNc::send(l.data(), l.size());
    }
};

struct Rig : JcWfHost {
    Nc nc;
    JcCommandSystem sys;
    JcJogSession jogs;
    JcDispatcher disp;
    JcWorkflow wf;
    uint32_t seq = 1;
    char state_buf[20] = "Idle";
    bool fresh = true;
    JcCommandResult last;
    std::vector<std::string> sent_log;
    JcTri probe_cfg = JcTri::Yes;

    Rig() : sys(&Nc::send, &nc), jogs(&JcCommandSystem::session_tx, &sys), wf(this) {
        sys.attach_jogs(&jogs);
        disp.set_sink_ex(&JcCommandSystem::sink, &sys);
        disp.arm();
    }
    bool submit(const JcCommand& c, const char** why) override {
        JcDispatchContext x;
        x.live = true; x.link_fresh = fresh; strncpy(state_buf, nc.state, sizeof(state_buf) - 1);
        x.machine_state = state_buf; x.operator_confirmed = true; x.now_ms = nc.now;
        x.operation_pending = sys.busy() || jogs.unresolved();
        const size_t before = nc.wire.size();
        const JcAuthDecision d = disp.submit(c, x);
        if (d != JcAuthDecision::Allowed) { *why = jc_auth_name(d); return false; }
        if (nc.wire.size() == before) { *why = "not transmitted (busy or refused by policy)"; return false; }
        sent_log.push_back(c.text);
        return true;
    }
    bool query(const char* line) override {
        if (!jc_query_allowed(line)) return false;
        nc.wire.push_back(line);
        sent_log.push_back(line);
        nc.command(line);
        return true;
    }
    void stop() override { const char b = '!'; Nc::send(&nc, &b, 1); sent_log.push_back("!"); }

    JcWfStatus status() const {
        JcWfStatus s;
        s.fresh = fresh;
        s.state = nc.state;
        s.wpos_valid = true;
        for (int a = 0; a < 3; ++a) s.wpos[a] = nc.work(a);
        s.spindle = nc.spindle;
        s.pins = nc.pins;
        s.sequence = seq;
        s.probe_configured = probe_cfg;
        return s;
    }
    void run(uint32_t ms) {
        const uint32_t end = nc.now + ms;
        while (nc.now < end) {
            nc.update(nc.now + 10);
            sys.set_clock(nc.now);
            jogs.set_clock(nc.now);
            char reply[96];
            while (nc.pop_line(reply, sizeof(reply))) { sys.on_line(reply); wf.on_line(reply); }
            if (nc.now % 100 == 0) { ++seq; }
            sys.on_machine_state(nc.state);
            sys.tick();
            JcCommandResult r;
            if (sys.take_result(r)) { last = r; wf.on_result(r); }
            wf.on_status(status());
            wf.service(nc.now);
        }
    }
    bool prepare(JcWfKind k, const JcWfParams& p) {
        const bool ok = wf.prepare(k, p, status(), nc.now);
        run(400);
        return ok;
    }
    void go(uint32_t timeout_ms = 60000) {   // run until the routine stops moving by itself
        const uint32_t end = nc.now + timeout_ms;
        while (nc.now < end && wf.state() == JcWfState::Running) run(50);
    }
    int count(const char* prefix) const {
        int n = 0;
        for (const std::string& s : sent_log) if (s.rfind(prefix, 0) == 0) ++n;
        return n;
    }
    int moves_sent() const { int n = 0; for (const std::string& s : sent_log) if (s.find("G0") != std::string::npos || s.find("G38") != std::string::npos) ++n; return n; }
};

static JcWfParams base_params() {
    JcWfParams p;
    p.clearance_set = true; p.clearance_z = 20.0f;
    p.search = 40.0f; p.feed = 300.0f; p.retract = 2.0f; p.tip_radius = 1.0f;
    return p;
}

// ---- positioning ------------------------------------------------------------------------------------------

static void test_goto() {
    Rig r;
    r.nc.off[0][0] = 10; r.nc.off[0][1] = 20; r.nc.off[0][2] = -5;     // G54 origin in machine coordinates
    r.nc.m[0] = 70; r.nc.m[1] = 80; r.nc.m[2] = -5;                    // work (60, 60, 0)
    JcWfParams p = base_params();
    CHECK(r.prepare(JcWfKind::GoToXY, p));
    CHECK(r.wf.state() == JcWfState::Planned && r.wf.wcs() == 0);       // active WCS read from FluidNC before the preview
    CHECK(r.count("$G") == 1 && r.count("$#") == 1);
    CHECK(r.moves_sent() == 0);                                         // nothing moves merely because it was selected
    CHECK(r.wf.start(r.nc.now));
    r.go();
    CHECK(r.wf.state() == JcWfState::WaitOperator);
    CHECK(r.count("G21 G90 G0 Z20.000") == 1);                           // clearance first
    CHECK(r.count("G21 G90 G0 X") == 0);                                 // XY never emitted before the operator continues
    NEAR(r.nc.work(2), 20.0f, 0.06f);
    r.run(2000);
    CHECK(r.count("G21 G90 G0 X") == 0);                                 // still waiting, still nothing
    r.wf.continue_after_pause(r.nc.now);
    r.go();
    CHECK(r.wf.state() == JcWfState::Done);
    CHECK(r.count("G21 G90 G0 X0.000 Y0.000") == 1);
    NEAR(r.nc.work(0), 0.0f, 0.06f); NEAR(r.nc.work(1), 0.0f, 0.06f); NEAR(r.nc.work(2), 20.0f, 0.06f);
    for (const std::string& s : r.sent_log) {                            // work coordinates only: no G53, no G92, no G10, no A
        CHECK(s.find("G53") == std::string::npos && s.find("G92") == std::string::npos && s.find("G10") == std::string::npos && s.find('A') == std::string::npos);
    }
    NEAR(r.nc.off[0][0], 10.0f, 0.0001f);                                // offsets untouched

    // X0 only / Y0 only
    Rig a; a.nc.m[0] = 70; a.nc.m[1] = 80;
    CHECK(a.prepare(JcWfKind::GoToX, p)); a.wf.start(a.nc.now); a.go(); a.wf.continue_after_pause(a.nc.now); a.go();
    CHECK(a.count("G21 G90 G0 X0.000") == 1 && a.count("G21 G90 G0 X0.000 Y") == 0);
    Rig b; b.nc.m[0] = 70; b.nc.m[1] = 80;
    CHECK(b.prepare(JcWfKind::GoToY, p)); b.wf.start(b.nc.now); b.go(); b.wf.continue_after_pause(b.nc.now); b.go();
    CHECK(b.count("G21 G90 G0 Y0.000") == 1);
}

static void test_goto_refusals() {
    JcWfParams p = base_params();
    {   // no clearance: not started, nothing sent
        Rig r; JcWfParams q = p; q.clearance_set = false;
        CHECK(!r.prepare(JcWfKind::GoToXY, q));
        CHECK(strstr(r.wf.reason(), "Z clearance") != nullptr && r.nc.wire.empty());
    }
    {   // clearance below the current Z: would move down
        Rig r; r.nc.m[2] = 30.0f;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "BELOW") != nullptr);
        CHECK(r.moves_sent() == 0);
    }
    {   // spindle running
        Rig r; r.nc.spindle = 12000;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "spindle") != nullptr && r.moves_sent() == 0);
    }
    {   // the clearance move is rejected: no XY follows, even after more time and a continue
        Rig r; r.nc.m[2] = -5;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.nc.error_on_cmd = r.nc.cmds + 1;
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "REJECTED") != nullptr);
        r.wf.continue_after_pause(r.nc.now);
        r.run(5000);
        CHECK(r.count("G21 G90 G0 X") == 0);
    }
    {   // active WCS changes between the clearance move and the XY move: the XY move is not sent
        Rig r; r.nc.m[2] = -5;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::WaitOperator);
        r.nc.wcs = 3;
        r.wf.continue_after_pause(r.nc.now);
        r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "WCS changed") != nullptr);
        CHECK(r.count("G21 G90 G0 X") == 0);
    }
    {   // disconnect during the sequence: ended, not resumed
        Rig r; r.nc.m[2] = -5;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.wf.start(r.nc.now); r.run(100);
        r.sys.on_link_lost(); r.wf.on_link_lost();
        r.run(200);
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "NOT resumed") != nullptr);
        const size_t n = r.nc.wire.size();
        r.fresh = true; r.run(8000);
        CHECK(r.nc.wire.size() == n);
    }
    {   // cancel during the clearance move: hold sent, nothing further
        Rig r; r.nc.m[2] = -5;
        CHECK(r.prepare(JcWfKind::GoToXY, p));
        r.wf.start(r.nc.now); r.run(60);
        r.wf.cancel(r.nc.now);
        CHECK(r.wf.state() == JcWfState::Cancelled && r.count("!") == 1);
        r.run(5000);
        CHECK(r.count("G21 G90 G0 X") == 0);
    }
}

// ---- probe centre -------------------------------------------------------------------------------------------

static void test_find_x_inside() {
    Rig r;
    r.nc.off[0][0] = 10;
    CHECK(r.prepare(JcWfKind::FindX, base_params()));
    char line[96];
    CHECK(r.wf.step_count() > 4);
    r.wf.preview_line(0, line, sizeof(line));
    CHECK(strstr(line, "1.") == line);
    r.wf.start(r.nc.now); r.go();
    CHECK(r.wf.state() == JcWfState::Done);
    const JcWfResult& res = r.wf.result();
    CHECK(res.x.ok && !res.x.suspect);
    NEAR(res.x.centre, 50.0f, 0.001f);                    // bore 40..60 -> machine centre 50
    NEAR(res.x.size, 20.0f, 0.001f);                      // compensated for the 1 mm probe radius
    CHECK(res.wcs == 0);
    CHECK(r.count("G21 G91 G38.2 F300 X40.000") == 1 && r.count("G21 G91 G38.2 F300 X-40.000") == 1);
    // Offsets are NOT touched by the measurement.
    NEAR(r.nc.off[0][0], 10.0f, 0.0001f);
    CHECK(r.count("G10") == 0);
    // Optional actions only on request. SET WORK X0: G10 L2 on the active WCS, read back.
    CHECK(r.wf.request_set_origin(r.nc.now));
    r.go();
    CHECK(r.wf.state() == JcWfState::Done && r.wf.result().readback_done && r.wf.result().readback_ok);
    CHECK(r.count("G10 L2 P1 X50.000") == 1 && r.count("G92") == 0);
    NEAR(r.nc.off[0][0], 50.0f, 0.001f);                  // work X0 is now the centre
    NEAR(r.nc.work(0) - (r.nc.m[0] - 50.0f), 0.0f, 0.001f);
    // Move to centre on request.
    CHECK(r.wf.request_move_to_centre(r.nc.now));
    r.go();
    CHECK(r.wf.state() == JcWfState::WaitOperator || r.wf.state() == JcWfState::Done);
    if (r.wf.state() == JcWfState::WaitOperator) { r.wf.continue_after_pause(r.nc.now); r.go(); }
    NEAR(r.nc.m[0], 50.0f, 0.06f);
}

static void test_find_x_outside_and_y() {
    Rig r;
    r.nc.feat = Feature::Boss; r.nc.xlo = 40; r.nc.xhi = 60;
    r.nc.m[0] = 30.0f;                         // beside the low face
    JcWfParams p = base_params(); p.feature = JcFeatureKind::Outside; p.est_width = 20.0f; p.expected_size = 20.0f;
    CHECK(r.prepare(JcWfKind::FindX, p));
    r.wf.start(r.nc.now); r.go();
    CHECK(r.wf.state() == JcWfState::WaitOperator);      // pauses before lowering Z beside the far side
    CHECK(r.count("G21 G90 G0 Z20.000") == 1);
    r.wf.continue_after_pause(r.nc.now); r.go();
    CHECK(r.wf.state() == JcWfState::Done);
    NEAR(r.wf.result().x.centre, 50.0f, 0.001f);
    NEAR(r.wf.result().x.size, 20.0f, 0.001f);
    // Y inside.
    Rig y;
    CHECK(y.prepare(JcWfKind::FindY, base_params()));
    y.wf.start(y.nc.now); y.go();
    CHECK(y.wf.state() == JcWfState::Done);
    NEAR(y.wf.result().y.centre, 50.0f, 0.001f);
    CHECK(y.wf.request_set_origin(y.nc.now)); y.go();
    CHECK(y.count("G10 L2 P1 Y50.000") == 1);
}

static void test_find_xy_inside() {
    Rig r;
    r.nc.xlo = 30; r.nc.xhi = 70; r.nc.ylo = 44; r.nc.yhi = 56;
    r.nc.m[0] = 48; r.nc.m[1] = 49;
    CHECK(r.prepare(JcWfKind::FindXY, base_params()));
    r.wf.start(r.nc.now); r.go();
    CHECK(r.wf.state() == JcWfState::WaitOperator);                 // explicit pause before leaving the probing position
    r.wf.continue_after_pause(r.nc.now); r.go();
    CHECK(r.wf.state() == JcWfState::Done);
    const JcWfResult& res = r.wf.result();
    NEAR(res.x.centre, 50.0f, 0.001f); NEAR(res.y.centre, 50.0f, 0.001f);
    NEAR(res.x.size, 40.0f, 0.001f); NEAR(res.y.size, 12.0f, 0.001f);
    CHECK(r.wf.request_set_origin(r.nc.now)); r.go();
    CHECK(r.count("G10 L2 P1 X50.000 Y50.000") == 1 && r.wf.result().readback_ok);
    // Outside XY is refused with the reason, nothing sent.
    Rig o; JcWfParams p = base_params(); p.feature = JcFeatureKind::Outside; p.est_width = 10;
    CHECK(!o.prepare(JcWfKind::FindXY, p) && strstr(o.wf.reason(), "inside features") != nullptr && o.nc.wire.empty());
}

static void test_probe_failures() {
    {   // never triggered: ALARM:4, explicit failure, no centre, no continuation
        Rig r; r.nc.feat = Feature::None;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "NO CONTACT") != nullptr);
        CHECK(!r.wf.result().valid && !r.wf.result().x.ok);
        const size_t n = r.nc.wire.size();
        r.run(8000);
        CHECK(r.nc.wire.size() == n);
        CHECK(!r.wf.request_set_origin(r.nc.now) && !r.wf.request_move_to_centre(r.nc.now));   // no actions on a failed routine
    }
    {   // probe already triggered (Pn:P): refused before any command
        Rig r; strcpy(r.nc.pins, "P");
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "ALREADY TRIGGERED") != nullptr);
        CHECK(r.count("G21") == 0);
    }
    {   // probe input not verified
        Rig r; r.probe_cfg = JcTri::Unknown;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.run(300);
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "probe input not verified") != nullptr);
    }
    {   // alarm on the SECOND touch: first contact exists, but no centre and nothing further
        Rig r; r.nc.alarm_on_probe = 2;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed);
        CHECK(!r.wf.result().x.ok);
        CHECK(r.count("G10") == 0);
        CHECK(r.nc.probes == 2);
    }
    {   // the controller rejects a probe: failed, no continuation
        Rig r; r.nc.error_on_cmd = 0;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.nc.error_on_cmd = r.nc.cmds + 1;
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Failed && r.nc.probes == 0);
    }
    {   // no reply: contact timeout stops the machine and ends the routine
        Rig r; JcWfParams p = base_params(); p.contact_timeout_s = 3;
        CHECK(r.prepare(JcWfKind::FindX, p));
        r.nc.silent = true;
        r.wf.start(r.nc.now); r.go(20000);
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "timeout") != nullptr && r.count("!") == 1);
        const size_t n = r.nc.wire.size();
        r.run(5000);
        CHECK(r.nc.wire.size() == n);
    }
    {   // disconnect during the probe sequence
        Rig r;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.run(300);
        r.sys.on_link_lost(); r.wf.on_link_lost();
        const size_t n = r.nc.wire.size();
        r.run(8000);
        CHECK(r.wf.state() == JcWfState::Failed && r.nc.wire.size() == n);
    }
}

static void test_radius_and_parameters() {
    JcWfParams p = base_params();
    {   // the stylus is really 2 mm but the operator entered 1 mm: the 20 mm bore measures 22 mm -> flagged suspect
        Rig r; r.nc.tip = 2.0f; p.expected_size = 20.0f; p.tolerance = 0.5f;
        CHECK(r.prepare(JcWfKind::FindX, p));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Done);
        CHECK(r.wf.result().x.ok && r.wf.result().x.suspect && strstr(r.wf.result().x.why, "probe radius") != nullptr);
        NEAR(r.wf.result().x.centre, 50.0f, 0.001f);       // the midpoint is radius independent; the SIZE shows the error
        NEAR(r.wf.result().x.size, 18.0f + 2.0f, 2.1f);
    }
    {   // correct radius entered: not suspect
        Rig r; r.nc.tip = 2.0f; JcWfParams q = base_params(); q.tip_radius = 2.0f; q.expected_size = 20.0f;
        CHECK(r.prepare(JcWfKind::FindX, q));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.result().x.ok && !r.wf.result().x.suspect);
        NEAR(r.wf.result().x.size, 20.0f, 0.001f);
    }
    // Invalid parameters are refused before anything is sent.
    char why[96];
    JcWfParams b = base_params();
    b.tip_radius = -1; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.tip_radius = 40; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.search = 0; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.feed = 0; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.retract = 0; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.slow_second = true; b.slow_feed = 500; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.contact_timeout_s = 0; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.feature = JcFeatureKind::Outside; b.est_width = 0; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    b = base_params(); b.clearance_set = false; b.feature = JcFeatureKind::Outside; b.est_width = 10; CHECK(!jc_wf_validate(JcWfKind::FindX, b, why, sizeof(why)));
    // Slow second touch: both feeds appear, and the slow one is the measurement.
    Rig s; JcWfParams q = base_params(); q.slow_second = true; q.slow_feed = 20;
    CHECK(s.prepare(JcWfKind::FindX, q));
    s.wf.start(s.nc.now); s.go();
    CHECK(s.wf.state() == JcWfState::Done);
    CHECK(s.count("G21 G91 G38.2 F300") == 2 && s.count("G21 G91 G38.2 F20") == 2);
    NEAR(s.wf.result().x.centre, 50.0f, 0.001f);
}

static void test_origin_writes() {
    {   // the active WCS changes after the measurement: nothing is written
        Rig r;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.state() == JcWfState::Done);
        r.nc.wcs = 1;
        CHECK(r.wf.request_set_origin(r.nc.now));
        r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "WCS changed") != nullptr);
        CHECK(r.count("G10") == 0);
    }
    {   // FluidNC does not apply the offset: the read-back catches it
        Rig r;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        r.nc.ignore_g10 = true;
        CHECK(r.wf.request_set_origin(r.nc.now));
        r.go();
        CHECK(r.wf.state() == JcWfState::Failed && strstr(r.wf.reason(), "READBACK MISMATCH") != nullptr);
    }
    {   // a different WCS (G56 active from the start) is the one written
        Rig r; r.nc.wcs = 2;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.request_set_origin(r.nc.now)); r.go();
        CHECK(r.count("G10 L2 P3 X50.000") == 1 && r.wf.result().readback_ok);
        NEAR(r.nc.off[2][0], 50.0f, 0.001f); NEAR(r.nc.off[0][0], 0.0f, 0.0001f);
    }
    {   // G92 active: the offset written accounts for it (work = machine - G5x - G92); G92 itself is not used or changed
        Rig r; r.nc.off[8][0] = 5.0f;
        CHECK(r.prepare(JcWfKind::FindX, base_params()));
        r.wf.start(r.nc.now); r.go();
        CHECK(r.wf.request_set_origin(r.nc.now)); r.go();
        NEAR(r.nc.off[0][0], 45.0f, 0.001f); NEAR(r.nc.work(0) - (r.nc.m[0] - 50.0f), 0.0f, 0.001f);
        NEAR(r.nc.off[8][0], 5.0f, 0.0001f);
    }
}

static void test_rotary_centreline() {
    Rig r;
    r.nc.feat = Feature::Cylinder; r.nc.yc = 55; r.nc.zc = -30; r.nc.dia = 20; r.nc.tilt_y = 0.02f; r.nc.tilt_z = -0.01f; r.nc.x0 = 50;
    r.nc.m[0] = 50; r.nc.m[1] = 30; r.nc.m[2] = -20;      // beside the cylinder on the low-Y side, at about the axis height
    JcWfParams p = base_params();
    p.cylinder_diameter = 20; p.est_width = 20; p.station_spacing = 40; p.search = 40; p.clearance_z = 20;
    CHECK(r.prepare(JcWfKind::FindRotary, p));
    r.wf.start(r.nc.now);
    for (int i = 0; i < 8 && r.wf.state() != JcWfState::Done && r.wf.state() != JcWfState::Failed; ++i) {
        r.go(120000);
        if (r.wf.state() == JcWfState::WaitOperator) r.wf.continue_after_pause(r.nc.now);
    }
    CHECK(r.wf.state() == JcWfState::Done);
    const JcCentreline& c = r.wf.result().line;
    CHECK(c.ok && !c.suspect);
    NEAR(c.tilt_cross, 0.02f, 0.0006f);                // the axis is not parallel to the machine X axis: shown, not assumed away
    NEAR(c.tilt_z, -0.01f, 0.0006f);
    NEAR(c.cross, 55.0f + 0.02f * 20.0f, 0.02f);       // mean of the two station centres (x = 50 and 90 -> mean 70, +20 from x0)
    NEAR(c.z, -30.0f + (-0.01f) * 20.0f, 0.03f);
    NEAR(c.diameter, 20.0f, 0.05f);
    CHECK(r.count("G10") == 0);                          // no work offset and no A-axis calibration is modified
    CHECK(r.count("G21 G91 G38.2 F300 Y40.000") == 2 && r.count("G21 G91 G38.2 F300 Y-40.000") == 2 && r.count("G21 G91 G38.2 F300 Z-40.000") == 2);
    // A single pass is not accepted as a centreline: a failed top contact ends the routine.
    Rig f;
    f.nc.feat = Feature::Cylinder; f.nc.m[0] = 50; f.nc.m[1] = 30; f.nc.m[2] = -20;
    CHECK(f.prepare(JcWfKind::FindRotary, p));
    f.nc.alarm_on_probe = 3;
    f.wf.start(f.nc.now);
    for (int i = 0; i < 4 && f.wf.state() == JcWfState::Running; ++i) { f.go(120000); if (f.wf.state() == JcWfState::WaitOperator) f.wf.continue_after_pause(f.nc.now); }
    CHECK(f.wf.state() == JcWfState::Failed && !f.wf.result().line.ok);
    // Invalid input.
    Rig g; JcWfParams q = p; q.cylinder_diameter = 0;
    CHECK(!g.prepare(JcWfKind::FindRotary, q) && g.nc.wire.empty());
}

static void test_calc_functions() {
    JcCentre c = jc_axis_centre(JcFeatureKind::Inside, true, 59.0f, true, 41.0f, 1.0f, 20.0f, 0.5f);
    CHECK(c.ok && !c.suspect);
    NEAR(c.centre, 50.0f, 1e-4f); NEAR(c.size, 20.0f, 1e-4f);
    c = jc_axis_centre(JcFeatureKind::Outside, true, 61.0f, true, 39.0f, 1.0f, 20.0f, 0.5f);
    CHECK(c.ok); NEAR(c.centre, 50.0f, 1e-4f); NEAR(c.size, 20.0f, 1e-4f);
    CHECK(!jc_axis_centre(JcFeatureKind::Inside, false, 0, true, 41, 1, 0, 1).ok);          // failed contact
    CHECK(!jc_axis_centre(JcFeatureKind::Inside, false, 59.0f, true, 41.0f, 1, 0, 1).ok);   // a failed first contact, plausible numbers
    CHECK(!jc_axis_centre(JcFeatureKind::Inside, true, 59.0f, false, 41.0f, 1, 0, 1).ok);   // a failed second contact
    CHECK(!jc_axis_centre(JcFeatureKind::Inside, true, 40.0f, true, 41.0f, 1, 0, 1).ok);    // reversed contacts
    CHECK(!jc_axis_centre(JcFeatureKind::Outside, true, 41.0f, true, 39.0f, 5.0f, 0, 1).ok); // negative size
    CHECK(!jc_axis_centre(JcFeatureKind::Inside, true, 59, true, 41, -1, 0, 1).ok);
    // Reply parsing.
    JcProbeReport pr;
    CHECK(jc_parse_prb(jc_classify_line("[PRB:12.500,-3.250,-4.000:1]"), pr) && pr.contact && pr.axes == 3);
    NEAR(pr.pos[0], 12.5f, 1e-4f); NEAR(pr.pos[1], -3.25f, 1e-4f);
    CHECK(jc_parse_prb(jc_classify_line("[PRB:1.000,2.000,3.000,4.000:0]"), pr) && !pr.contact && pr.axes == 4);
    CHECK(!jc_parse_prb(jc_classify_line("[PRB:1.000,2.000:1]"), pr));
    CHECK(!jc_parse_prb(jc_classify_line("[PRB:a,b,c:1]"), pr));
    CHECK(!jc_parse_prb(jc_classify_line("[PRB:1,2,3:2]"), pr));
    CHECK(!jc_parse_prb(jc_classify_line("[MSG:INFO]"), pr));
    CHECK(jc_wcs_from_gc("G0 G55 G17 G21 G90 G94 M5 M9 T0 F0 S0") == 1);
    CHECK(jc_wcs_from_gc("G0 G17") == -1);
    CHECK(jc_offset_tag_index("G54") == 0 && jc_offset_tag_index("G59") == 5 && jc_offset_tag_index("G92") == 8 && jc_offset_tag_index("G53") == -1);
}

int main() {
    test_calc_functions();
    test_goto();
    test_goto_refusals();
    test_find_x_inside();
    test_find_x_outside_and_y();
    test_find_xy_inside();
    test_probe_failures();
    test_radius_and_parameters();
    test_origin_writes();
    test_rotary_centreline();
    printf("%d checks, %d failed (positioning and probing routines)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
