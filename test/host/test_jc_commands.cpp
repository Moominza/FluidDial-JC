// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the unified command interface (Instruction 009): the shared command policy, the
// controller-authoritative gate, the single-operation command system and the homing / jog readiness
// facts, every command class against a SIMULATED FluidNC (acknowledgement, rejection, alarm, timeout,
// connection loss, cancellation, reconnection, invalid input, unsupported command, four-axis formatting).
//
// Built with -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_FULL_CONTROL_TEST=1 (the full-control policy). Nothing
// here talks to a real machine.

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcCommandSystem.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcHomingState.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

// ---- simulated FluidNC ---------------------------------------------------------------------------------

struct FakeNc {
    std::vector<std::string> wire;     // every line / byte that left through the transport
    bool send_ok = true;
    uint32_t now = 1000;
    // behaviour
    enum Mode { Ok, Error, Alarm, Silent } mode = Ok;
    int error_code = 20;
    // jog motion
    float pos[4] = { 0, 0, 0, 0 };
    std::string state = "Idle";
    uint32_t ack_at = 0, end_at = 0;
    bool ack_pending = false, moving = false;
    uint8_t axis = 0;
    float dist = 0, from = 0;
    uint32_t seq = 1;
    // line to deliver to the application
    std::vector<std::string> out;

    static bool send(void* self, const char* bytes, size_t n) {
        FakeNc* f = static_cast<FakeNc*>(self);
        if (!f->send_ok) return false;
        f->wire.emplace_back(bytes, n);
        if (n == 1) {
            if (static_cast<uint8_t>(bytes[0]) == 0x85 && f->moving) {
                f->pos[f->axis] = f->from + f->dist * 0.5f;
                f->moving = false;
                f->state = "Idle";
            }
            return true;
        }
        std::string line(bytes, n - 1);
        if (f->mode == Silent) return true;
        uint8_t ax; float d, fd;
        if (jc_policy_jog_line(line.c_str(), &ax, &d, &fd)) {
            f->axis = ax; f->dist = d; f->ack_pending = true; f->ack_at = f->now + 60;
            f->from = f->pos[ax];
            return true;
        }
        f->ack_pending = true;
        f->ack_at = f->now + 40;
        return true;
    }
};

struct Rig {
    FakeNc nc;
    JcCommandSystem sys;
    JcJogSession jogs;
    JcDispatcher disp;
    JcMachineProfile profile = jc_default_profile();
    uint32_t seq = 100;
    char state_buf[20] = "Idle";
    bool fresh = true;
    JcCommandResult last;
    bool have_last = false;

    Rig() : sys(&FakeNc::send, &nc), jogs(&JcCommandSystem::session_tx, &sys) {
        sys.attach_jogs(&jogs);
        disp.set_sink_ex(&JcCommandSystem::sink, &sys);
        disp.arm();
    }
    JcDispatchContext ctx(bool confirmed = true) const {
        JcDispatchContext c;
        c.live = true;
        c.link_fresh = fresh;
        c.machine_state = state_buf;
        c.operator_confirmed = confirmed;
        c.now_ms = nc.now;
        c.operation_pending = jogs.unresolved() || sys.busy();
        return c;
    }
    JcAuthDecision submit(const JcCommand& c, bool confirmed = true) { return disp.submit(c, ctx(confirmed)); }
    void status() {
        JcJogStatus s;
        s.fresh = fresh;
        s.state = nc.state.c_str();
        s.sequence = ++seq;
        s.pos_valid = true;
        float p[4];
        for (int i = 0; i < 4; ++i) p[i] = nc.pos[i];
        if (nc.moving) {
            const float frac = static_cast<float>(nc.now - (nc.ack_at)) / static_cast<float>(nc.end_at - nc.ack_at);
            p[nc.axis] = nc.from + nc.dist * (frac > 1 ? 1 : frac);
        }
        memcpy(s.pos, p, sizeof(p));
        jogs.on_status(s, nc.now);
    }
    void run(uint32_t ms) {
        const uint32_t end = nc.now + ms;
        while (nc.now < end) {
            nc.now += 10;
            sys.set_clock(nc.now);
            jogs.set_clock(nc.now);
            if (nc.ack_pending && static_cast<int32_t>(nc.now - nc.ack_at) >= 0) {
                nc.ack_pending = false;
                char reply[16];
                if (nc.mode == FakeNc::Error) snprintf(reply, sizeof(reply), "error:%d", nc.error_code);
                else if (nc.mode == FakeNc::Alarm) { snprintf(reply, sizeof(reply), "ALARM:1"); nc.state = "Alarm"; }
                else {
                    snprintf(reply, sizeof(reply), "ok");
                    if (nc.dist != 0 && !nc.wire.empty() && nc.wire.back().rfind("$J=", 0) == 0) {
                        nc.moving = true;
                        nc.state = "Jog";
                        nc.end_at = nc.now + static_cast<uint32_t>(600);
                        nc.ack_at = nc.now;
                    }
                }
                jogs.on_line(reply, nc.now);
                sys.on_line(reply);
            }
            if (nc.moving && static_cast<int32_t>(nc.now - nc.end_at) >= 0) {
                nc.moving = false;
                nc.pos[nc.axis] = nc.from + nc.dist;
                nc.state = "Idle";
                nc.dist = 0;
            }
            if (nc.now % 250 == 0) status();
            sys.on_machine_state(nc.state.c_str());
            sys.tick();
            jogs.tick(nc.now);
            JcCommandResult r;
            if (sys.take_result(r)) { last = r; have_last = true; }
        }
    }
};

// ---- policy: the lines the builders generate, and nothing else ---------------------------------------------

static void test_policy_accepts_builder_output() {
    JcMachineProfile p = jc_default_profile();
    p.axes[3].homing_allowed = true;
    // Four-axis jog: both directions, units per axis.
    const char letters[4] = { 'X', 'Y', 'Z', 'A' };
    for (uint8_t a = 0; a < 4; ++a) {
        for (int dir = -1; dir <= 1; dir += 2) {
            const JcCommand c = jc_jog_incremental(p, a, dir * 1.0f, 100.0f);
            CHECK(c.ok);
            char want[48];
            snprintf(want, sizeof(want), "$J=G91G21F100%c%s1.000", letters[a], dir < 0 ? "-" : "");
            CHECK_STR(c.text, want);
            CHECK(jc_command_line_allowed(JcCommandClass::Jog, c.text));
            CHECK(!jc_command_line_allowed(JcCommandClass::Home, c.text));   // class mismatch
        }
    }
    CHECK(jc_jog_incremental(p, 3, 45.0f, 600.0f).ok);   // A in degrees
    CHECK_STR(jc_jog_incremental(p, 3, -90.0f, 600.0f).text, "$J=G91G21F600A-90.000");
    // Real-time bytes.
    CHECK(jc_command_line_allowed(JcCommandClass::JogCancel, jc_jog_cancel().text));
    CHECK(jc_command_line_allowed(JcCommandClass::Hold, jc_feed_hold().text));
    CHECK(jc_command_line_allowed(JcCommandClass::Job, jc_cycle_resume().text));
    CHECK(jc_command_line_allowed(JcCommandClass::Reset, jc_soft_reset().text));
    CHECK(!jc_command_line_allowed(JcCommandClass::Hold, jc_cycle_resume().text));
    // Homing.
    CHECK(jc_command_line_allowed(JcCommandClass::Home, jc_home(p, 0xFF).text));
    for (uint8_t a = 0; a < 4; ++a) CHECK(jc_command_line_allowed(JcCommandClass::Home, jc_home(p, a).text));
    CHECK_STR(jc_home(p, 2).text, "$HZ");
    // Work offsets.
    for (uint8_t i = 0; i < 6; ++i) CHECK(jc_command_line_allowed(JcCommandClass::WorkOffset, jc_select_wcs(i).text));
    for (uint8_t a = 0; a < 4; ++a) {
        CHECK(jc_command_line_allowed(JcCommandClass::WorkOffset, jc_zero_axis(p, a).text));
        CHECK(jc_command_line_allowed(JcCommandClass::WorkOffset, jc_set_axis_value(p, a, -12.5f).text));
    }
    // Probing.
    CHECK(jc_command_line_allowed(JcCommandClass::Probe, jc_probe_move(p, 2, -10.0f, 100.0f).text));
    const JcCommandSequence z = jc_probe_z_sequence(p, 20.0f, 100.0f, 5.0f, 1.5f);
    CHECK(z.ok);
    for (uint8_t i = 0; i < z.count; ++i) {
        CHECK(jc_command_line_allowed(z.steps[i].cls, z.steps[i].text));
    }
    for (uint8_t a = 0; a < 2; ++a) {
        for (int dir = -1; dir <= 1; dir += 2) {
            const JcCommandSequence e = jc_probe_edge_sequence(p, a, dir, 20.0f, 100.0f, 3.0f, 6.0f);
            CHECK(e.ok);
            for (uint8_t i = 0; i < e.count; ++i) CHECK(jc_command_line_allowed(e.steps[i].cls, e.steps[i].text));
        }
    }
    // Jobs, macros, recovery, spindle.
    CHECK(jc_command_line_allowed(JcCommandClass::Job, jc_run_sd_file("/sd/job1.gcode").text));
    CHECK(jc_command_line_allowed(JcCommandClass::Macro, jc_run_macro_file("/macro1.nc").text));
    CHECK(jc_command_line_allowed(JcCommandClass::Unlock, jc_unlock().text));
    CHECK(jc_command_line_allowed(JcCommandClass::Spindle, jc_spindle_stop().text));
}

static void test_policy_rejects_everything_else() {
    const char* const bad[] = {
        "G0 X100", "G1 X1 F100", "$$", "$RST=*", "$Bye", "$Config/Dump", "$/axes/x/max_travel_mm=1", "$J=G91G21F100X1", "$J=G91G21F100X1.000 Y1.000",
        "$J=G90G21F100X1.000", "$J=G91G21F0X1.000", "$J=G91G21F100X0.000", "$J=G91G21F100B1.000", "$J=G91G21F99999X1.000",
        "$J=G91G21F100X99999.000", "$HX Y", "$H1", "$h", "G55 G0 X1", "G10 L20 P0 B1.000", "G38.2 X1", "G21 G91 G38.2 F100 A5.000",
        "G21 G91 G38.2 F100 X0.000", "$SD/Run=/sd/../etc", "$SD/Run=relative.gcode", "$SD/Run=/sd/a b.gcode", "$Localfs/Run=", "M3 S1000", "M5 M3", "M5\n",
        "$X\n$X", "G54\r", "G10 L2 P7 X0.000", "G10 L2 P1 Y1.000 X1.000", "G10 L2 P1 A1.000", "G10 L2 P1", "G10 L20 P7 X0.000", "G21 G90 G0 Z1.000 X1.000", "G21 G90 G0 A1.000", "G21 G90 G0 X1.000 Z1.000", "G21 G90 G0", "G53 G0 X1.000", "G21 G90 G0 X1.000 Y", "G92 X0", "", "$SD/Delete=/sd/a.gcode", "$Localfs/Delete=/a.nc", "$Erase", "$Sleep"
    };
    const JcCommandClass classes[] = { JcCommandClass::Jog, JcCommandClass::Home, JcCommandClass::Probe, JcCommandClass::WorkOffset,
                                       JcCommandClass::Spindle, JcCommandClass::Job, JcCommandClass::Macro, JcCommandClass::Unlock,
                                       JcCommandClass::Other };
    for (const char* t : bad) {
        for (JcCommandClass c : classes) {
            ++g_run;
            if (jc_command_line_allowed(c, t)) { ++g_fail; printf("FAIL accepted [%s] as class %d\n", t, static_cast<int>(c)); }
        }
    }
    CHECK(!jc_command_line_allowed(JcCommandClass::Job, "x"));
    CHECK(!jc_command_line_allowed(JcCommandClass::JogCancel, "!"));
}

static void test_positioning_builders() {
    const JcMachineProfile p = jc_default_profile();
    CHECK_STR(jc_goto_work_z(25.0f).text, "G21 G90 G0 Z25.000");
    CHECK_STR(jc_goto_work_xy(true, 0, true, 0).text, "G21 G90 G0 X0.000 Y0.000");
    CHECK_STR(jc_goto_work_xy(true, -12.5f, false, 0).text, "G21 G90 G0 X-12.500");
    CHECK_STR(jc_goto_work_xy(false, 0, true, 3).text, "G21 G90 G0 Y3.000");
    CHECK(!jc_goto_work_xy(false, 0, false, 0).ok && !jc_goto_work_xy(true, NAN, false, 0).ok && !jc_goto_work_z(INFINITY).ok);
    CHECK_STR(jc_set_wcs_origin(0, true, 50.0f, false, 0, false, 0).text, "G10 L2 P1 X50.000");
    CHECK_STR(jc_set_wcs_origin(2, true, 1.0f, true, -2.0f, true, 3.0f).text, "G10 L2 P3 X1.000 Y-2.000 Z3.000");
    CHECK(!jc_set_wcs_origin(6, true, 1, false, 0, false, 0).ok && !jc_set_wcs_origin(0, false, 0, false, 0, false, 0).ok);
    CHECK_STR(jc_set_axis_value_wcs(p, 4, 0, 0.0f).text, "G10 L20 P5 X0.000");
    CHECK(jc_command_line_allowed(JcCommandClass::Macro, jc_goto_work_z(5).text));
    CHECK(jc_command_line_allowed(JcCommandClass::Macro, jc_goto_work_xy(true, 1, true, 2).text));
    CHECK(jc_command_line_allowed(JcCommandClass::WorkOffset, jc_set_wcs_origin(1, true, 1, true, 2, true, 3).text));
    CHECK(jc_command_line_allowed(JcCommandClass::WorkOffset, jc_set_axis_value_wcs(p, 5, 1, 2.0f).text));
    CHECK(jc_command_line_allowed(JcCommandClass::Probe, jc_move_relative(0, -2.0f).text));
    CHECK(!jc_move_relative(3, 1.0f).ok && !jc_move_relative(0, 0.0f).ok);
    CHECK_STR(jc_restore_absolute().text, "G90");
}

static void test_builder_invalid_and_unsupported() {
    JcMachineProfile p = jc_default_profile();
    CHECK(!jc_jog_incremental(p, 4, 1.0f, 100.0f).ok);                       // no such axis
    CHECK(!jc_jog_incremental(p, 0, 0.0f, 100.0f).ok);                       // zero move
    CHECK(!jc_jog_incremental(p, 0, 1.0f, 0.0f).ok);                         // zero feed
    CHECK(!jc_jog_incremental(p, 0, NAN, 100.0f).ok);
    CHECK(!jc_home(p, 3).ok && jc_home(p, 3).error == JcCmdError::Unsupported);    // A homing unavailable unless declared
    CHECK(!jc_probe_move(p, 3, 5.0f, 100.0f).ok);                            // rotary never probed
    CHECK(!jc_probe_edge_sequence(p, 2, 1, 10.0f, 100.0f, 3.0f, 6.0f).ok);   // edge probe is X / Y only
    CHECK(!jc_select_wcs(6).ok);
    CHECK(!jc_run_sd_file("../x").ok);
    CHECK(!jc_run_macro_file("").ok);
}

// ---- gate: controller-authoritative -----------------------------------------------------------------------

static void test_authority_gate() {
    JcAuthContext a;
    a.armed = true; a.live = true; a.link_fresh = true; a.now_ms = a.command_issued_ms = 100;
    strcpy(a.machine_state, "Idle");
    // No capability, homing, envelope or acknowledgement prerequisite.
    a.limits_required = a.envelope_required = a.unprotected_required = true;   // set by an unread / unhomed machine
    CHECK(jc_authorize(JcCommandClass::Jog, a) == JcAuthDecision::Allowed);
    CHECK(jc_authorize(JcCommandClass::Home, JcAuthContext(a)) == JcAuthDecision::BlockedUnconfirmed);   // consequential: confirm
    a.operator_confirmed = true;
    CHECK(jc_authorize(JcCommandClass::Home, a) == JcAuthDecision::Allowed);
    CHECK(jc_authorize(JcCommandClass::Probe, a) == JcAuthDecision::Allowed);
    CHECK(jc_authorize(JcCommandClass::Job, a) == JcAuthDecision::Allowed);
    // Client-side integrity rules stay.
    JcAuthContext b = a;
    b.armed = false;      CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedNotArmed);
    b = a; b.live = false; CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedNotLive);
    b = a; b.link_fresh = false; CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedStaleLink);
    b = a; b.now_ms = 100 + kJcMaxCommandAgeMs + 1; CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedStaleCommand);
    b = a; strcpy(b.machine_state, "Alarm"); CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedAlarm);
    CHECK(jc_authorize(JcCommandClass::Unlock, b) == JcAuthDecision::Allowed);          // recovery still works in alarm
    CHECK(jc_authorize(JcCommandClass::Reset, b) == JcAuthDecision::Allowed);
    b = a; b.operation_pending = true; CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedOperationPending);
    b = a; strcpy(b.machine_state, "Run"); CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedBadState);
    CHECK(jc_authorize(JcCommandClass::Home, b) == JcAuthDecision::BlockedBadState);
    // Stop is always available with LIVE, armed or not.
    b = a; b.armed = false; b.link_fresh = false;
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
    CHECK(jc_authorize(JcCommandClass::Hold, b) == JcAuthDecision::Allowed);
    b.live = false; CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::BlockedNotLive);
}

// ---- every class through the unified system, simulated FluidNC -----------------------------------------------

static void test_ack_and_result_for_each_class() {
    JcMachineProfile p = jc_default_profile();
    struct Case { const char* name; JcCommand c; JcCommandClass cls; };
    const Case cases[] = {
        { "home all", jc_home(p, 0xFF), JcCommandClass::Home },
        { "home x", jc_home(p, 0), JcCommandClass::Home },
        { "select g55", jc_select_wcs(1), JcCommandClass::WorkOffset },
        { "zero x", jc_zero_axis(p, 0), JcCommandClass::WorkOffset },
        { "probe z", jc_probe_move(p, 2, -10.0f, 100.0f), JcCommandClass::Probe },
        { "run file", jc_run_sd_file("/sd/a.gcode"), JcCommandClass::Job },
        { "macro", jc_run_macro_file("/m1.nc"), JcCommandClass::Macro },
        { "unlock", jc_unlock(), JcCommandClass::Unlock },
        { "spindle off", jc_spindle_stop(), JcCommandClass::Spindle },
    };
    for (const Case& k : cases) {
        Rig r;
        r.run(600);
        CHECK(k.c.ok);
        CHECK(r.submit(k.c) == JcAuthDecision::Allowed);
        CHECK(r.nc.wire.size() == 1);
        CHECK(!r.nc.wire.empty() && r.nc.wire[0] == std::string(k.c.text) + "\n");
        r.run(400);
        CHECK(r.have_last && r.last.outcome == JcExecOutcome::Acked && r.last.cls == k.cls);
        char line[96];
        jc_result_line(r.last, line, sizeof(line));
        CHECK(strstr(line, "accepted by FluidNC") != nullptr);
        CHECK(r.nc.wire.size() == 1);   // never resent
    }
    // Real-time bytes: sent, no reply expected.
    Rig r;
    r.run(600);
    CHECK(r.submit(jc_feed_hold(), false) == JcAuthDecision::Allowed);
    r.run(100);
    CHECK(r.have_last && r.last.outcome == JcExecOutcome::SentNoAck && r.last.cls == JcCommandClass::Hold);
    CHECK(r.nc.wire.size() == 1 && r.nc.wire[0] == "!");
    CHECK(r.submit(jc_cycle_resume(), true) == JcAuthDecision::Allowed);
    CHECK(r.nc.wire.back() == "~");
    CHECK(r.submit(jc_soft_reset(), true) == JcAuthDecision::Allowed);
    CHECK(r.nc.wire.back() == std::string(1, static_cast<char>(0x18)));
}

static void test_rejection_alarm_timeout_loss() {
    JcMachineProfile p = jc_default_profile();
    {   // controller rejection: reported with its code, not resent
        Rig r; r.nc.mode = FakeNc::Error; r.nc.error_code = 9;
        r.run(600);
        CHECK(r.submit(jc_home(p, 0)) == JcAuthDecision::Allowed);
        r.run(400);
        CHECK(r.last.outcome == JcExecOutcome::ControllerError && r.last.code == 9);
        char line[96]; jc_result_line(r.last, line, sizeof(line));
        CHECK(strstr(line, "REJECTED by FluidNC (error:9)") != nullptr);
        CHECK(r.sys.homed().mask() == 0);   // a rejected homing homes nothing
        CHECK(r.nc.wire.size() == 1);
    }
    {   // alarm
        Rig r; r.nc.mode = FakeNc::Alarm;
        r.run(600);
        CHECK(r.submit(jc_home(p, 0xFF)) == JcAuthDecision::Allowed);
        r.run(400);
        CHECK(r.last.outcome == JcExecOutcome::Alarm && r.last.code == 1);
        CHECK(r.sys.homed().mask() == 0);
        // ... and nothing but recovery is authorized while the controller is in Alarm
        strcpy(r.state_buf, "Alarm");
        CHECK(r.submit(jc_select_wcs(0)) == JcAuthDecision::BlockedAlarm);
        CHECK(r.submit(jc_unlock(), false) == JcAuthDecision::Allowed);
    }
    {   // timeout: state unknown, NOT resent, next command waits for the resync quiet period
        Rig r; r.nc.mode = FakeNc::Silent;
        r.run(600);
        CHECK(r.submit(jc_zero_axis(p, 0)) == JcAuthDecision::Allowed);
        r.run(2500);
        CHECK(r.last.outcome == JcExecOutcome::TimedOut);
        char line[96]; jc_result_line(r.last, line, sizeof(line));
        CHECK(strstr(line, "NO REPLY") != nullptr);
        CHECK(r.nc.wire.size() == 1);
        const size_t before = r.nc.wire.size();
        r.nc.mode = FakeNc::Ok;
        CHECK(r.submit(jc_select_wcs(0)) == JcAuthDecision::Allowed);   // gate allows; the link is in its quiet period
        CHECK(r.nc.wire.size() == before);                              // refused by the single-operation link, not sent
        r.run(1200);
        CHECK(r.submit(jc_select_wcs(0)) == JcAuthDecision::Allowed);
        CHECK(r.nc.wire.size() == before + 1);
    }
    {   // connection loss: the operation ends, homing state is dropped, nothing is replayed after reconnect
        Rig r;
        r.run(600);
        CHECK(r.submit(jc_home(p, 0xFF)) == JcAuthDecision::Allowed);
        r.nc.send_ok = true;
        r.sys.on_link_lost();
        r.run(100);
        CHECK(r.last.outcome == JcExecOutcome::LinkLost);
        char line[96]; jc_result_line(r.last, line, sizeof(line));
        CHECK(strstr(line, "NOT resent") != nullptr);
        const size_t sent = r.nc.wire.size();
        r.disp.on_link_lost();       // arming never survives a loss
        CHECK(!r.disp.armed());
        r.run(5000);                 // "reconnected": time passes, status fresh again
        CHECK(r.nc.wire.size() == sent);   // nothing replayed
        CHECK(r.submit(jc_select_wcs(0)) == JcAuthDecision::BlockedNotArmed);
        CHECK(r.sys.homed().mask() == 0);
    }
    {   // transport failure: reported, not retried
        Rig r; r.nc.send_ok = false;
        r.run(600);
        r.submit(jc_select_wcs(2));
        CHECK(r.nc.wire.empty());
        r.nc.send_ok = true;
        r.run(3000);
        CHECK(r.nc.wire.empty());
    }
}

static void test_single_operation_and_no_duplicate() {
    JcMachineProfile p = jc_default_profile();
    Rig r;
    r.run(600);
    CHECK(r.submit(jc_home(p, 0)) == JcAuthDecision::Allowed);
    // second submit while the first is awaiting: the gate says operation pending (ctx) and nothing is sent
    CHECK(r.submit(jc_home(p, 1)) == JcAuthDecision::BlockedOperationPending);
    CHECK(r.nc.wire.size() == 1);
    // a double tap of the same command is not sent twice either
    CHECK(r.submit(jc_home(p, 0)) == JcAuthDecision::BlockedOperationPending);
    CHECK(r.nc.wire.size() == 1);
    // even if a caller bypassed the gate context, the link refuses
    CHECK(!JcCommandSystem::sink(&r.sys, JcCommandClass::Home, "$HY"));
    CHECK(r.nc.wire.size() == 1 && r.sys.link().refused_busy() >= 1);
    // a jog sunk directly while a command is awaiting its reply is refused too (one operation at a time)
    {
        Rig q;
        q.run(600);
        CHECK(q.submit(jc_home(p, 0)) == JcAuthDecision::Allowed);
        const size_t before = q.nc.wire.size();
        CHECK(!JcCommandSystem::sink(&q.sys, JcCommandClass::Jog, "$J=G91G21F100X1.000"));
        CHECK(q.nc.wire.size() == before);
    }
    // the shared byte policy is the last gate in front of the transport: bytes it does not allow never leave
    {
        Rig q;
        q.run(600);
        const size_t before = q.nc.wire.size();
        CHECK(!JcCommandSystem::session_tx(&q.sys, "G0 X100\n", 8));
        CHECK(!JcCommandSystem::session_tx(&q.sys, "$J=G91G21F100X1.000", 19));   // no newline
        CHECK(!JcCommandSystem::session_tx(&q.sys, "x", 1));
        CHECK(q.nc.wire.size() == before);
        CHECK(JcCommandSystem::session_tx(&q.sys, "$X\n", 3) && q.nc.wire.size() == before + 1);
    }
    // policy: a line that is not a builder line never reaches the transport, even if the gate allowed it
    r.run(400);
    CHECK(!JcCommandSystem::sink(&r.sys, JcCommandClass::Home, "$RST=*"));
    CHECK(!JcCommandSystem::sink(&r.sys, JcCommandClass::Other, "G0 X500"));
    CHECK(r.sys.refused_policy() >= 2 && r.nc.wire.size() == 1);
}

static void test_homed_tracker_and_state_facts() {
    JcMachineProfile p = jc_default_profile();
    Rig r;
    r.run(600);
    CHECK(r.submit(jc_home(p, 0)) == JcAuthDecision::Allowed);
    r.run(400);
    CHECK(r.sys.homed().homed(0) == JcTri::Yes && r.sys.homed().homed(1) == JcTri::Unknown);
    CHECK(r.submit(jc_home(p, 1)) == JcAuthDecision::Allowed);
    r.run(400);
    CHECK((r.sys.homed().mask() & 3u) == 3u);
    r.sys.on_link_lost();
    CHECK(r.sys.homed().mask() == 0);

    // Facts: configured / present / active / homed are separate; jog needs none of them.
    JcMachineCaps caps;
    caps.complete = true;
    for (uint8_t a = 0; a < 3; ++a) {
        caps.axes[a].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] = JcFieldState::Valid;
        caps.axes[a].value[static_cast<uint8_t>(JcQueryField::HomingCycle)] = 1.0f;
    }
    caps.axes[3].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] = JcFieldState::CtrlError;   // A: error:3
    caps.axes[0].state[static_cast<uint8_t>(JcQueryField::LimitNegPin)] = JcFieldState::Valid;
    caps.axes[0].value[static_cast<uint8_t>(JcQueryField::LimitNegPin)] = 1.0f;
    caps.must_home_state = JcFieldState::Valid;
    caps.must_home = false;
    JcHomedTracker none;
    JcHomingOwnerFacts owner;   // Z not verified, A not declared
    JcHomingView v = jc_homing_view(caps, true, "", true, "Idle", none, owner);
    CHECK(v.axis[0].configured == JcTri::Yes && v.axis[3].configured == JcTri::Unknown);
    CHECK(v.axis[0].switch_present == JcTri::Yes);
    CHECK(v.axis[0].switch_active == JcTri::No);          // fresh status, pin not in Pn: -> inactive, not an error
    CHECK(v.axis[0].homed == JcTri::Unknown);
    CHECK(v.accepts_jog && v.position_trusted == JcTri::No);
    CHECK(strstr(v.note, "Jog is available without homing") != nullptr);
    CHECK(v.axis[0].may_home && v.axis[1].may_home);
    CHECK(!v.axis[2].may_home && strstr(v.axis[2].why_not, "Z homing direction") != nullptr);   // never silently homes Z
    CHECK(!v.axis[3].may_home);
    CHECK(!v.may_home_all);
    owner.z_homing_verified = true;
    CHECK(jc_homing_view(caps, true, "", true, "Idle", none, owner).axis[2].may_home);
    // Switch active is reported from Pn:, and does not affect jogging.
    v = jc_homing_view(caps, true, "XP", true, "Idle", none, owner);
    CHECK(v.axis[0].switch_active == JcTri::Yes && v.axis[1].switch_active == JcTri::No && v.accepts_jog);
    // Stale status: unknown, jog unavailable for the honest reason.
    v = jc_homing_view(caps, true, "", false, "Idle", none, owner);
    CHECK(!v.accepts_jog && v.axis[0].switch_active == JcTri::Unknown && strstr(v.note, "No fresh status") != nullptr);
    // must_home true is shown as a controller requirement, not a communication error.
    caps.must_home = true;
    v = jc_homing_view(caps, true, "", true, "Idle", none, owner);
    CHECK(strstr(v.note, "must_home") != nullptr);
    // Unread settings: unknown, never "not configured".
    v = jc_homing_view(JcMachineCaps(), false, "", true, "Idle", none, owner);
    CHECK(v.axis[0].configured == JcTri::Unknown && v.must_home == JcTri::Unknown && v.accepts_jog);
}

// ---- four-axis jog through the session, cancel, and no replay --------------------------------------------------

static void test_four_axis_jog_and_cancel() {
    JcMachineProfile p = jc_default_profile();
    const char letters[4] = { 'X', 'Y', 'Z', 'A' };
    for (uint8_t a = 0; a < 4; ++a) {
        for (int dir = -1; dir <= 1; dir += 2) {
            Rig r;
            r.run(800);
            const float step = a == 3 ? 5.0f : 1.0f;
            const JcCommand c = jc_jog_incremental(p, a, dir * step, a == 3 ? 600.0f : 100.0f);
            CHECK(c.ok);
            CHECK(r.submit(c, false) == JcAuthDecision::Allowed);   // jog: armed is the only operator step
            CHECK(r.nc.wire.size() == 1 && r.nc.wire[0].find(letters[a]) != std::string::npos);
            r.run(2500);
            CHECK(r.jogs.result() == JcJogResult::Completed);
            CHECK(fabsf(r.nc.pos[a] - dir * step) < 0.01f);
        }
    }
    {   // cancel while moving: the cancel byte goes through the same transport, motion stops, Cancelled
        Rig r;
        r.run(800);
        CHECK(r.submit(jc_jog_incremental(p, 2, 10.0f, 100.0f), false) == JcAuthDecision::Allowed);
        r.run(300);
        CHECK(r.submit(jc_jog_cancel(), false) == JcAuthDecision::Allowed);
        CHECK(r.nc.wire.back() == std::string(1, static_cast<char>(0x85)));
        r.run(3500);
        CHECK(r.jogs.result() == JcJogResult::Cancelled);
        CHECK(r.nc.pos[2] < 10.0f);
    }
    {   // rejection and alarm during a jog are reported by the session
        Rig r; r.nc.mode = FakeNc::Error; r.nc.error_code = 15;
        r.run(800);
        CHECK(r.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), false) == JcAuthDecision::Allowed);
        r.run(500);
        CHECK(r.jogs.result() == JcJogResult::Rejected && r.jogs.code() == 15);
        CHECK(r.nc.wire.size() == 1);
        Rig q; q.nc.mode = FakeNc::Alarm;
        q.run(800);
        CHECK(q.submit(jc_jog_incremental(p, 1, 1.0f, 100.0f), false) == JcAuthDecision::Allowed);
        q.run(500);
        CHECK(q.jogs.result() == JcJogResult::Alarm);
    }
    {   // timeout and link loss during a jog: unresolved, cancel attempted, nothing replayed
        Rig r; r.nc.mode = FakeNc::Silent;
        r.run(800);
        CHECK(r.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), false) == JcAuthDecision::Allowed);
        r.run(3000);
        CHECK(r.jogs.result() == JcJogResult::AckTimeout);
        const size_t n = r.nc.wire.size();
        r.run(6000);
        int jog_lines = 0;
        for (const std::string& w : r.nc.wire) if (w.rfind("$J=", 0) == 0) ++jog_lines;
        CHECK(jog_lines == 1);               // the jog line was never resent
        CHECK(r.nc.wire.size() >= n);
    }
    {   // a jog is not required to be preceded by a capability read, a homing cycle or any acknowledgement
        Rig r;
        r.run(800);
        r.disp.arm();   // arming only
        CHECK(r.submit(jc_jog_incremental(p, 3, -10.0f, 600.0f), false) == JcAuthDecision::Allowed);
        CHECK(r.sys.homed().mask() == 0);
    }
}

int main() {
    test_policy_accepts_builder_output();
    test_policy_rejects_everything_else();
    test_builder_invalid_and_unsupported();
    test_positioning_builders();
    test_authority_gate();
    test_ack_and_result_for_each_class();
    test_rejection_alarm_timeout_loss();
    test_single_operation_and_no_duplicate();
    test_homed_tracker_and_state_facts();
    test_four_axis_jog_and_cancel();
    printf("%d checks, %d failed (command interface)\n", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
