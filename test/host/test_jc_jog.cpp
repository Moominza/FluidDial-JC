// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the first-live-jog TEST configuration (Instruction 006): test
// limits, the strict transport-level jog line check, the jog session state
// machine (acknowledgement / error / alarm / timeout / motion completion /
// cancellation as distinct states), and the authorization rules.
//
// Built three ways by tools/run_host_tests.py:
//   -DJC_MACHINE_CONTROL_ENABLED=0                      ordinary firmware policy
//   -DJC_MACHINE_CONTROL_ENABLED=1                      all rules compiled in (test only)
//   -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_LIVE_JOG_TEST=1 the experimental live-jog policy
//
// The "controller" below is a fake: it replies, moves and reports status on a
// virtual clock. Nothing here talks to a real machine.

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCapsLoader.hpp"
#include "../../src/jc8048w550/JcCommandAuth.hpp"
#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcJogTest.hpp"
#include "../../src/jc8048w550/JcMachineCaps.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

// ---- fake machine ----------------------------------------------------------------

struct World {
    JcJogSession session;
    uint32_t now = 1000;
    uint32_t seq = 100;

    // transmit capture
    std::vector<std::string> lines;   // text lines (without the newline)
    int cancels = 0;                  // 0x85 bytes
    bool tx_fails = false;

    // controller behaviour
    std::string state = "Idle";
    float pos[4] = { 10.0f, 20.0f, 5.0f, 0.0f };
    bool reply_ok = true;             // false: error:reply_error instead of ok
    int reply_error = 15;
    bool silent = false;              // never replies to a jog
    bool alarm_on_jog = false;        // replies ALARM:1
    bool ignore_cancel = false;       // 0x85 has no effect
    bool late_start = false;          // 0x85 overtakes the queued $J line: the jog still starts afterwards
    bool wrong_position = false;      // ends the move 0.4 mm short
    uint32_t ack_delay = 60;
    uint32_t move_ms = 700;
    bool status_on = true;
    uint32_t status_period = 250;

    // internal schedule
    bool ack_pending = false;
    uint32_t ack_at = 0;
    bool moving = false;
    uint32_t move_start = 0, move_end = 0;
    uint8_t move_axis = 0;
    float move_dist = 0.0f;
    float move_from = 0.0f;
    uint32_t next_status = 0;

    World() : session(&World::tx, this) {
        session.set_clock(now);
        next_status = now;
    }

    static bool tx(void* user, const char* bytes, size_t n) {
        World* w = static_cast<World*>(user);
        if (w->tx_fails) return false;
        if (n == 1 && static_cast<uint8_t>(bytes[0]) == 0x85) {
            ++w->cancels;
            if (!w->ignore_cancel && w->moving) {
                const float frac = w->move_end > w->move_start
                    ? static_cast<float>(w->now - w->move_start) / static_cast<float>(w->move_end - w->move_start) : 1.0f;
                w->pos[w->move_axis] = w->move_from + w->move_dist * (frac > 1.0f ? 1.0f : frac);
                w->moving = false;
                w->state = "Idle";
            }
            if (!w->ignore_cancel && !w->late_start) w->ack_pending = false;
            return true;
        }
        std::string line(bytes, n);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        w->lines.push_back(line);
        JcJogRequest r;
        if (jc_parse_jog_line(line.c_str(), r)) {
            w->move_axis = r.axis;
            w->move_dist = r.distance;
            w->ack_pending = !w->silent;
            w->ack_at = w->now + w->ack_delay;
        }
        return true;
    }

    // Advances virtual time in 10 ms steps, running controller, status and session.
    void run(uint32_t ms) {
        const uint32_t end = now + ms;
        while (now < end) {
            now += 10;
            if (ack_pending && static_cast<int32_t>(now - ack_at) >= 0) {
                ack_pending = false;
                if (alarm_on_jog) {
                    state = "Alarm";
                    session.on_line("ALARM:1", now);
                } else if (!reply_ok) {
                    char e[24];
                    snprintf(e, sizeof(e), "error:%d", reply_error);
                    session.on_line(e, now);
                } else {
                    state = "Jog";
                    moving = true;
                    move_start = now;
                    move_end = now + move_ms;
                    move_from = pos[move_axis];
                    session.on_line("ok", now);
                }
            }
            if (moving && static_cast<int32_t>(now - move_end) >= 0) {
                moving = false;
                pos[move_axis] = move_from + move_dist + (wrong_position ? -0.4f : 0.0f);
                state = "Idle";
            }
            if (status_on && static_cast<int32_t>(now - next_status) >= 0) {
                next_status = now + status_period;
                send_status();
            }
            session.tick(now);
        }
    }

    void send_status() {
        JcJogStatus s;
        s.fresh = true;
        s.state = state.c_str();
        s.sequence = ++seq;
        s.pos_valid = true;
        float p[4];
        for (int i = 0; i < 4; ++i) p[i] = pos[i];
        if (moving) {
            const float frac = static_cast<float>(now - move_start) / static_cast<float>(move_end - move_start);
            p[move_axis] = move_from + move_dist * (frac > 1.0f ? 1.0f : frac);
        }
        memcpy(s.pos, p, sizeof(p));
        session.on_status(s, now);
    }

    // Direct sink call: the same function the gate would call for an authorized jog.
    bool jog(uint8_t axis, float distance, float feed) {
        JcMachineProfile p = jc_default_profile();
        jc_profile_apply_test_limits(p);
        const JcCommand c = jc_jog_incremental(p, axis, distance, feed);
        if (!c.ok) return false;
        session.set_clock(now);
        return JcJogSession::sink(&session, JcCommandClass::Jog, c.text);
    }
    bool jog_text(const char* text) {
        session.set_clock(now);
        return JcJogSession::sink(&session, JcCommandClass::Jog, text);
    }
    // Brings the session to "fresh Idle status seen".
    void settle() { run(600); }
};

// ---- limits, builder, strict line check --------------------------------------------

static void test_limits_and_builder() {
    const JcJogTestLimits l = jc_test_jog_limits();
    // First motion: X and Y only, 1 mm @ 100 mm/min. Z is ON HOLD, A is disabled.
    CHECK(l.max_step[0] == 1.0f && l.max_step[1] == 1.0f && l.max_step[2] == 0.0f && l.max_step[3] == 0.0f);
    CHECK(l.max_feed[0] == 100.0f && l.max_feed[1] == 100.0f && l.max_feed[2] == 0.0f && l.max_feed[3] == 0.0f);

    JcMachineProfile open = jc_default_profile();
    CHECK(!open.test_mode);
    CHECK(jc_jog_incremental(open, 0, 10.0f, 500.0f).ok);   // ordinary profile unchanged

    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    CHECK(p.test_mode);
    CHECK(p.axes[0].max_step == 1.0f && p.axes[1].max_step == 1.0f && p.axes[2].max_step == 0.0f && p.axes[3].max_step == 0.0f);

    JcCommand c = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    CHECK(c.ok && c.axis == 0);
    CHECK_STR(c.text, "$J=G91G21F100X1.000");
    c = jc_jog_incremental(p, 1, -1.0f, 100.0f);
    CHECK(c.ok);
    CHECK_STR(c.text, "$J=G91G21F100Y-1.000");

    // Z is on hold: no step, no feed, either direction, however small.
    const float z_steps[] = { 0.001f, 0.25f, 0.5f, -0.5f, 1.0f, 10.0f, 150.0f };
    for (float step : z_steps) {
        c = jc_jog_incremental(p, 2, step, 50.0f);
        CHECK(!c.ok && c.error == JcCmdError::JogDisabled);
        c = jc_jog_incremental(p, 2, step, 1.0f);
        CHECK(!c.ok && c.error == JcCmdError::JogDisabled);
    }

    // Larger steps are refused even when the UI offers 10 / 50 / 100 mm presets.
    const float presets[] = { 1.001f, 2.0f, 10.0f, 50.0f, 100.0f };
    for (float step : presets) {
        c = jc_jog_incremental(p, 0, step, 100.0f);
        CHECK(!c.ok && c.error == JcCmdError::TestLimit);
        c = jc_jog_incremental(p, 1, -step, 100.0f);
        CHECK(!c.ok && c.error == JcCmdError::TestLimit);
    }
    // Feeds
    CHECK(jc_jog_incremental(p, 0, 1.0f, 101.0f).error == JcCmdError::TestLimit);
    CHECK(jc_jog_incremental(p, 1, 1.0f, 101.0f).error == JcCmdError::TestLimit);
    CHECK(jc_jog_incremental(p, 0, 1.0f, 3000.0f).error == JcCmdError::TestLimit);
    CHECK(jc_jog_incremental(p, 0, 1.0f, 0.0f).error == JcCmdError::BadFeed);
    CHECK(jc_jog_incremental(p, 0, 0.0f, 100.0f).error == JcCmdError::BadValue);
    CHECK(jc_jog_incremental(p, 0, NAN, 100.0f).error == JcCmdError::BadValue);
    CHECK(jc_jog_incremental(p, 0, 1.0f, INFINITY).error == JcCmdError::BadFeed || !jc_jog_incremental(p, 0, 1.0f, INFINITY).ok);

    // Rotary axis blocked: any step, any feed, either direction.
    CHECK(jc_jog_incremental(p, 3, 1.0f, 100.0f).error == JcCmdError::JogDisabled);
    CHECK(jc_jog_incremental(p, 3, -45.0f, 3600.0f).error == JcCmdError::JogDisabled);
    CHECK(jc_jog_incremental(p, 3, 0.001f, 1.0f).error == JcCmdError::JogDisabled);

    // Continuous jogging stays disabled.
    for (uint8_t a = 0; a < 4; ++a) {
        CHECK(!jc_jog_continuous(p, a, 1, 50.0f).ok);
        CHECK(!jc_jog_continuous(p, a, -1, 50.0f).ok);
    }
    CHECK(jc_jog_continuous(open, 0, 1, 500.0f).ok);   // ordinary profile unchanged

    // Controller values may only LOWER the limits: apply_caps then test limits.
    JcMachineCaps caps;
    for (uint8_t a = 0; a < 4; ++a) {
        JcAxisCaps& ac = caps.axes[a];
        const JcQueryField f[4] = { JcQueryField::StepsPerMm, JcQueryField::MaxTravelMm, JcQueryField::MaxRate,
                                    JcQueryField::Accel };
        const float v[4] = { 800.0f, 2300.0f, 6000.0f, 250.0f };
        for (int k = 0; k < 4; ++k) {
            ac.state[static_cast<uint8_t>(f[k])] = JcFieldState::Valid;
            ac.value[static_cast<uint8_t>(f[k])] = v[k];
        }
    }
    JcMachineProfile q = jc_default_profile();
    jc_profile_apply_caps(q, caps);
    CHECK(q.axes[1].max_step == 2300.0f);                  // a travel of 2300 mm is NOT a safe step
    jc_profile_apply_test_limits(q);
    CHECK(q.axes[0].max_step == 1.0f && q.axes[1].max_step == 1.0f && q.axes[2].max_step == 0.0f);
    CHECK(q.axes[0].max_feed == 100.0f && q.axes[2].max_feed == 0.0f && q.axes[3].max_step == 0.0f);
    CHECK(q.axes[0].limits_confirmed && q.axes[0].travel_mm == 2300.0f);

    // A controller with SMALLER limits keeps its smaller values.
    JcMachineProfile s = jc_default_profile();
    s.axes[0].max_step = 0.25f;
    s.axes[0].max_feed = 40.0f;
    jc_profile_apply_test_limits(s);
    CHECK(s.axes[0].max_step == 0.25f && s.axes[0].max_feed == 40.0f);
}

static void test_strict_line() {
    const JcJogTestLimits l = jc_test_jog_limits();
    JcJogRequest r;
    CHECK(jc_parse_jog_line("$J=G91G21F100X1.000", r) && r.axis == 0 && r.distance == 1.0f && r.feed == 100.0f);
    CHECK(jc_parse_jog_line("$J=G91G21F50Z-0.500", r) && r.axis == 2 && r.distance == -0.5f && r.feed == 50.0f);
    CHECK(jc_parse_jog_line("$J=G91G21F3600A-45.000", r) && r.axis == 3 && r.distance == -45.0f);

    const char* const bad[] = {
        "", "$J", "$J=", "$J=G91G21F100X1", "$J=G91G21F100X1.0", "$J=G91G21F100X1.0000",
        "$J=G91G21F100X1.000 ", " $J=G91G21F100X1.000", "$J=G91G21F100X1.000\n", "$J=G91G21F100X1.000Y1.000",
        "$J=G91G21F100 X1.000", "$J=G91 G21 F100 X1.000", "$J=G90G21F100X1.000", "$J=G91G20F100X1.000",
        "$J=G91G21F100x1.000", "$J=G91G21F100B1.000", "$J=G91G21F100X+1.000", "$J=G91G21F100X1e0",
        "$J=G91G21F100X.500", "$J=G91G21FX1.000", "$J=G91G21F1000000X1.000", "$J=G91G21F100X12345.000",
        "$H", "$X", "G0 X1", "G10 L20 P0 X0", "$SD/Run=/a.nc", "$Localfs/Run=/m.nc", "M3 S1000", "$J=G91G21F100X1.000;$H",
        "$J=G91G21F100X1.000$H", "$J=G91G21F100X--1.000",
    };
    for (const char* t : bad) {
        CHECK(!jc_parse_jog_line(t, r));
        CHECK(!jc_jog_line_allowed(t, l));
    }
    CHECK(!jc_jog_line_allowed(nullptr, l));

    // Allowed: inside the limits, any direction.
    CHECK(jc_jog_line_allowed("$J=G91G21F100X1.000", l));
    CHECK(jc_jog_line_allowed("$J=G91G21F100Y-1.000", l));
    CHECK(jc_jog_line_allowed("$J=G91G21F1X0.001", l));
    // Z is on hold: a perfectly formed Z line never passes the transport check.
    CHECK(!jc_jog_line_allowed("$J=G91G21F50Z0.500", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F50Z-0.500", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F1Z0.001", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F100Z1.000", l));
    // Parsable but outside the test limits: never allowed on the transport.
    CHECK(!jc_jog_line_allowed("$J=G91G21F100X1.001", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F100X10.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F100Y-50.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F50Z0.501", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F101X1.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F51Z0.500", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F3000X1.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F0X1.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F100X0.000", l));
    // The rotary axis is never allowed on the transport.
    CHECK(!jc_jog_line_allowed("$J=G91G21F100A1.000", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F1A0.001", l));
    CHECK(!jc_jog_line_allowed("$J=G91G21F3600A-45.000", l));

    // Every line the builder can produce at the limit round-trips through the strict check.
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    for (uint8_t a = 0; a < 2; ++a) {   // X and Y only
        for (int dir = -1; dir <= 1; dir += 2) {
            const JcCommand c = jc_jog_incremental(p, a, dir * p.axes[a].max_step, p.axes[a].max_feed);
            CHECK(c.ok && jc_jog_line_allowed(c.text, l));
        }
    }
}

// ---- first jog and the distinct states -----------------------------------------------

static void test_first_jog() {
    World w;
    CHECK(w.session.can_begin(w.now) == JcJogReject::NoFreshStatus);   // no status seen yet
    CHECK(!w.jog(0, 1.0f, 100.0f));                                      // refused: nothing sent
    CHECK(w.lines.empty() && w.session.refused_busy() == 1);
    w.settle();
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);

    CHECK(w.jog(0, 1.0f, 100.0f));
    CHECK(w.lines.size() == 1);
    CHECK_STR(w.lines[0].c_str(), "$J=G91G21F100X1.000");
    CHECK(w.session.phase() == JcJogPhase::AwaitingAck);
    CHECK(w.session.unresolved());
    CHECK(w.session.result() == JcJogResult::None);
    CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);

    // 1) acknowledgement is NOT completion
    JcJogPhase seen[8];
    int n = 0;
    JcJogPhase last = w.session.phase();
    for (int i = 0; i < 150 && w.session.phase() != JcJogPhase::Idle; ++i) {
        w.run(10);
        if (w.session.phase() != last && n < 8) {
            last = w.session.phase();
            seen[n++] = last;
        }
    }
    CHECK(n >= 3);
    CHECK(seen[0] == JcJogPhase::Accepted || seen[0] == JcJogPhase::Moving);
    bool saw_accepted = false, saw_moving = false;
    for (int i = 0; i < n; ++i) {
        saw_accepted = saw_accepted || seen[i] == JcJogPhase::Accepted;
        saw_moving = saw_moving || seen[i] == JcJogPhase::Moving;
    }
    CHECK(saw_moving);
    (void)saw_accepted;
    CHECK(w.session.phase() == JcJogPhase::Idle);
    CHECK(w.session.result() == JcJogResult::Completed);
    CHECK(w.session.pos_check() == JcJogPosCheck::Match);
    CHECK(fabsf(w.session.pos_delta() - 1.0f) < 0.01f);
    CHECK(!w.session.needs_acknowledgement() && !w.session.unresolved());
    CHECK(w.session.acked() == 1 && w.session.completed() == 1 && w.session.jogs_sent() == 1);
    CHECK(w.session.saw_motion());
    CHECK(fabsf(w.pos[0] - 11.0f) < 0.001f);
    CHECK(w.cancels == 0);
}

static void test_ack_is_not_completion() {
    World w;
    w.settle();
    w.move_ms = 3000;
    CHECK(w.jog(1, -1.0f, 100.0f));
    w.run(500);   // acknowledged and moving, not finished
    CHECK(w.session.acked() == 1);
    CHECK(w.session.phase() == JcJogPhase::Moving);
    CHECK(w.session.result() == JcJogResult::None);
    CHECK(w.session.unresolved());
    CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);
    w.run(3500);
    CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::Completed);
    CHECK(fabsf(w.pos[1] - 19.0f) < 0.001f);
}

static void test_controller_rejection() {
    World w;
    w.settle();
    w.reply_ok = false;
    w.reply_error = 15;   // jog target exceeds machine travel (as a controller would report)
    CHECK(w.jog(0, 1.0f, 100.0f));
    w.run(300);
    CHECK(w.session.phase() == JcJogPhase::Idle);
    CHECK(w.session.result() == JcJogResult::Rejected && w.session.code() == 15);
    CHECK(!w.session.needs_acknowledgement());   // nothing moved: no latch
    CHECK(w.session.rejected() == 1 && w.session.acked() == 0 && w.session.completed() == 0);
    CHECK(fabsf(w.pos[0] - 10.0f) < 0.001f);
    CHECK(w.session.can_begin(w.now) == JcJogReject::Debounce);   // the settle window still applies
    w.run(300);
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);   // free again
    w.reply_ok = true;
    CHECK(w.jog(0, 0.5f, 100.0f));
    w.run(1500);
    CHECK(w.session.result() == JcJogResult::Completed);
}

static void test_alarm_conditions() {
    // ALARM:N as the reply to the jog
    {
        World w;
        w.settle();
        w.alarm_on_jog = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(300);
        CHECK(w.session.result() == JcJogResult::Alarm && w.session.code() == 1);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.needs_acknowledgement());
        CHECK(w.session.unresolved());
        CHECK(w.session.can_begin(w.now) != JcJogReject::None);
        w.run(3000);
        CHECK(w.session.can_begin(w.now) == JcJogReject::ResultUnacknowledged || w.session.can_begin(w.now) == JcJogReject::MachineNotIdle);
        CHECK(w.session.acknowledge());
        // still in Alarm: refuses until the controller is Idle again
        CHECK(w.session.can_begin(w.now) == JcJogReject::MachineNotIdle);
        w.state = "Idle";
        w.alarm_on_jog = false;
        w.run(600);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    }
    // the machine enters Alarm in the middle of a move (unexpected alarm)
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(500);
        CHECK(w.session.phase() == JcJogPhase::Moving);
        w.moving = false;
        w.state = "Alarm";
        w.run(400);
        CHECK(w.session.result() == JcJogResult::Alarm);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.needs_acknowledgement());
        CHECK(w.cancels == 0);   // the controller has already stopped
        // a late "ok" never becomes a completion
        w.session.on_line("ok", w.now);
        CHECK(w.session.result() == JcJogResult::Alarm);
    }
    // an ALARM:N line while moving
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(400);
        w.session.on_line("ALARM:3", w.now);
        CHECK(w.session.result() == JcJogResult::Alarm && w.session.code() == 3);
        CHECK(w.session.needs_acknowledgement());
    }
    // an alarm while nothing is in flight is only counted
    {
        World w;
        w.settle();
        w.session.on_line("ALARM:1", w.now);
        CHECK(w.session.unexpected_alarms() == 1 && w.session.phase() == JcJogPhase::Idle && !w.session.needs_acknowledgement());
    }
    // an error line after acceptance: stop and require acknowledgement
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(400);
        w.session.on_line("error:8", w.now);
        CHECK(w.cancels == 1 && w.session.phase() == JcJogPhase::Cancelling);
        w.run(600);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::Rejected);
        CHECK(w.session.needs_acknowledgement() && w.session.code() == 8);
    }
}

static void test_timeouts() {
    // no reply to the jog at all
    {
        World w;
        w.settle();
        w.silent = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(1900);
        CHECK(w.session.phase() == JcJogPhase::AwaitingAck && w.cancels == 0);
        for (int i = 0; i < 100 && w.session.timeouts() == 0; ++i) w.run(10);
        CHECK(w.session.phase() == JcJogPhase::Cancelling);          // timeout -> cancel
        CHECK(w.cancels == 1 && w.session.timeouts() == 1);
        CHECK(w.lines.size() == 1);                                   // never resent
        w.run(600);                                                   // controller reports Idle
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::AckTimeout);
        CHECK(w.session.needs_acknowledgement());
        // a late "ok" is never attributed to anything
        const uint32_t before = w.session.unsolicited();
        w.session.on_line("ok", w.now);
        CHECK(w.session.unsolicited() == before + 1 && w.session.phase() == JcJogPhase::Idle);
        CHECK(w.session.can_begin(w.now) == JcJogReject::ResultUnacknowledged);
        CHECK(w.session.acknowledge());
        w.run(1500);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
        CHECK(w.lines.size() == 1);   // still exactly one transmitted line
    }
    // acknowledged but never returns to Idle (and the controller ignores the cancel)
    {
        World w;
        w.settle();
        w.move_ms = 100000;
        w.ignore_cancel = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(5000);   // distance / feed = 0.6 s, plus a 5 s margin: not yet timed out
        CHECK(w.session.timeouts() == 0 && w.cancels == 0);
        for (int i = 0; i < 200 && w.session.timeouts() == 0; ++i) w.run(10);
        CHECK(w.session.timeouts() == 1 && w.cancels == 1);
        CHECK(w.session.phase() == JcJogPhase::Cancelling);
        w.run(3100);
        CHECK(w.session.cancel_unconfirmed() && w.session.result() == JcJogResult::CancelUnconfirmed);
        CHECK(w.session.needs_acknowledgement() && w.session.phase() == JcJogPhase::Cancelling);
        CHECK(!w.session.acknowledge());                              // nothing may be acknowledged while unresolved
        CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);   // new jogs refused
        CHECK(w.session.cancel(w.now) && w.cancels == 2);            // the operator can cancel again
    }
    // acknowledged, then the move overruns but the cancel works
    {
        World w;
        w.settle();
        w.move_ms = 100000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        for (int i = 0; i < 1000 && !(w.session.phase() == JcJogPhase::Idle && w.session.timeouts() == 1); ++i) w.run(10);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::MotionTimeout);
        CHECK(w.session.needs_acknowledgement());
        CHECK(w.state == "Idle");
    }
}

static void test_cancellation() {
    // cancel while moving
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        CHECK(w.session.phase() == JcJogPhase::Moving);
        w.session.set_clock(w.now);
        CHECK(JcJogSession::sink(&w.session, JcCommandClass::JogCancel, "\x85"));
        CHECK(w.cancels == 1 && w.session.phase() == JcJogPhase::Cancelling);
        CHECK(w.session.unresolved());                                // still unresolved until Idle is seen
        CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);
        w.run(400);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::Cancelled);
        CHECK(!w.session.needs_acknowledgement());
        CHECK(w.pos[0] > 10.0f && w.pos[0] < 11.0f);                  // stopped part-way, position reported
        w.run(1500);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    }
    // cancel before the controller acknowledged
    {
        World w;
        w.settle();
        w.ack_delay = 500;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(100);
        CHECK(w.session.cancel(w.now));
        CHECK(w.session.phase() == JcJogPhase::Cancelling);
        w.run(800);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::Cancelled);
        CHECK(w.lines.size() == 1);
        // the late reply (if the controller had accepted it) is not attributed to anything
        const uint32_t before = w.session.unsolicited();
        w.session.on_line("ok", w.now);
        CHECK(w.session.unsolicited() == before + 1);
    }
    // cancel with nothing in flight is harmless and is still transmitted
    {
        World w;
        w.settle();
        CHECK(w.session.cancel(w.now));
        CHECK(w.cancels == 1 && w.session.phase() == JcJogPhase::Idle && !w.session.needs_acknowledgement());
    }
    // cancel never needs the executor to be free (a jog is awaiting its reply)
    {
        World w;
        w.settle();
        w.silent = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        CHECK(w.session.cancel(w.now));
        CHECK(w.cancels == 1);
    }
    // a failing transport reports the failure of the cancel
    {
        World w;
        w.settle();
        w.tx_fails = true;
        CHECK(!w.session.cancel(w.now));
        CHECK(w.session.cancels_sent() == 0);
    }
    // cancel with no transport at all
    {
        JcJogSession none;
        CHECK(!none.cancel(5));
        CHECK(none.can_begin(5) == JcJogReject::NoTransport);
        CHECK(!JcJogSession::sink(&none, JcCommandClass::Jog, "$J=G91G21F100X1.000"));
    }
}

static void test_late_start_and_foreign_motion() {
    // The cancel overtook the queued jog line: the controller starts the jog AFTER the
    // cancel. The session must notice, stop it again, and demand acknowledgement.
    {
        World w;
        w.settle();
        w.late_start = true;
        w.ack_delay = 1500;
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(100);
        CHECK(w.session.cancel(w.now));
        for (int i = 0; i < 100 && w.session.phase() != JcJogPhase::Idle; ++i) w.run(10);
        CHECK(w.session.result() == JcJogResult::Cancelled && w.cancels == 1);
        // the queued jog now executes
        for (int i = 0; i < 400 && w.session.unexpected_motion() == 0; ++i) w.run(10);
        CHECK(w.session.unexpected_motion() == 1);
        CHECK(w.cancels == 2);                                    // stopped again
        w.run(800);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.result() == JcJogResult::UnexpectedMotion);
        CHECK(w.session.needs_acknowledgement() && w.state == "Idle");
        CHECK(w.session.can_begin(w.now) == JcJogReject::ResultUnacknowledged);
        CHECK(w.lines.size() == 1);                               // nothing was resent
    }
    // Motion we did not start (another client) is counted and never cancelled by us.
    {
        World w;
        w.settle();
        w.state = "Jog";
        w.run(1000);
        CHECK(w.session.foreign_motion() >= 1 && w.cancels == 0);
        CHECK(w.session.phase() == JcJogPhase::Idle && !w.session.needs_acknowledgement());
        // long after our own last jog, too
        w.state = "Idle";
        w.run(600);
        CHECK(w.jog(0, 0.5f, 100.0f));
        w.run(2500);
        CHECK(w.session.result() == JcJogResult::Completed);
        w.run(kJcJogLateStartWindowMs + 1000);
        const uint32_t foreign = w.session.foreign_motion();
        w.state = "Jog";
        w.run(1000);
        CHECK(w.session.foreign_motion() > foreign && w.cancels == 0 && w.session.unexpected_motion() == 0);
    }
}

static void test_connection_loss() {
    // loss while awaiting the acknowledgement
    {
        World w;
        w.settle();
        w.silent = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(200);
        const size_t lines = w.lines.size();
        w.session.on_link_lost(w.now, false);
        CHECK(w.session.phase() == JcJogPhase::Recovering && w.session.result() == JcJogResult::LinkLost);
        CHECK(w.session.needs_acknowledgement() && w.cancels == 0);   // nothing can be sent on a dead link
        CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);
        // reconnect: fresh Idle frames come back; the quiet period passes
        w.silent = false;
        w.state = "Idle";
        w.run(300);
        CHECK(w.session.phase() == JcJogPhase::Recovering);           // quiet period not over
        w.run(1500);
        CHECK(w.session.phase() == JcJogPhase::Idle);
        CHECK(w.lines.size() == lines && w.session.jogs_sent() == 1); // NOTHING was replayed
        CHECK(w.session.can_begin(w.now) == JcJogReject::ResultUnacknowledged);
        CHECK(w.session.acknowledge());
        w.run(400);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
        CHECK(w.lines.size() == lines);
    }
    // loss while moving; the controller is in an unknown state afterwards
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(500);
        CHECK(w.session.phase() == JcJogPhase::Moving);
        w.status_on = false;
        w.session.on_link_lost(w.now, false);
        CHECK(w.session.phase() == JcJogPhase::Recovering);
        w.run(4000);      // the move finishes on the controller; the pendant sees nothing
        CHECK(w.session.phase() == JcJogPhase::Recovering);           // still no fresh status
        CHECK(w.session.can_begin(w.now) != JcJogReject::None);
        w.status_on = true;
        w.run(1500);
        CHECK(w.session.phase() == JcJogPhase::Idle && w.session.needs_acknowledgement());
        CHECK(w.session.result() == JcJogResult::LinkLost);
        CHECK(w.lines.size() == 1);
    }
    // stale status with the socket still open: a cancel is attempted
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(500);
        w.session.on_link_lost(w.now, true);
        CHECK(w.cancels == 1 && w.session.result() == JcJogResult::StaleStatus);
        CHECK(w.session.phase() == JcJogPhase::Recovering);
    }
    // loss with nothing in flight: no latch, but a fresh frame is required again
    {
        World w;
        w.settle();
        w.session.on_link_lost(w.now, false);
        CHECK(w.session.phase() == JcJogPhase::Idle && !w.session.needs_acknowledgement());
        CHECK(w.session.can_begin(w.now) == JcJogReject::NoFreshStatus);
        w.run(600);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    }
    // a send failure is treated as an unusable stream, not as a retryable event
    {
        World w;
        w.settle();
        w.tx_fails = true;
        CHECK(!w.jog(0, 1.0f, 100.0f));
        CHECK(w.session.result() == JcJogResult::SendFailed && w.session.needs_acknowledgement());
        CHECK(w.session.phase() == JcJogPhase::Recovering);
        w.tx_fails = false;
        w.run(2000);
        CHECK(w.lines.empty());
        CHECK(w.session.can_begin(w.now) == JcJogReject::ResultUnacknowledged);
    }
}

static void test_stale_status() {
    World w;
    CHECK(w.session.can_begin(w.now) == JcJogReject::NoFreshStatus);
    w.settle();
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    // a stale frame (fresh=false) is never trusted
    JcJogStatus s;
    s.fresh = false;
    s.state = "Idle";
    s.sequence = 999;
    w.session.on_status(s, w.now);
    CHECK(w.session.can_begin(w.now) == JcJogReject::NoFreshStatus);
    // machine not Idle
    w.state = "Hold";
    w.run(400);
    CHECK(w.session.can_begin(w.now) == JcJogReject::MachineNotIdle);
    w.state = "Run";
    w.run(400);
    CHECK(w.session.can_begin(w.now) == JcJogReject::MachineNotIdle);
    w.state = "Alarm";
    w.run(400);
    CHECK(w.session.can_begin(w.now) == JcJogReject::MachineNotIdle);
    w.state = "Idle";
    w.run(400);
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    // the same frame twice is acted on once
    JcJogStatus f;
    f.fresh = true;
    f.state = "Idle";
    f.sequence = 5000;
    f.pos_valid = true;
    w.session.on_status(f, w.now);
    w.session.on_status(f, w.now);
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    // a jog is refused at the sink when the status went stale meanwhile
    w.session.on_link_lost(w.now, false);
    CHECK(!w.jog(0, 1.0f, 100.0f) && w.lines.empty());
}

static void test_touch_bounce() {
    World w;
    w.settle();
    // a burst of presses in 100 ms transmits exactly one jog
    int accepted = 0;
    for (int i = 0; i < 6; ++i) {
        accepted += w.jog(0, 1.0f, 100.0f) ? 1 : 0;
        w.run(20);
    }
    CHECK(accepted == 1 && w.lines.size() == 1);
    CHECK(w.session.refused_busy() == 5);
    w.run(2000);
    CHECK(w.session.result() == JcJogResult::Completed);
    // right after completion a bounce is still refused (settle window)
    w.session.set_clock(w.now);
    // run() already advanced time; force a fresh resolution then press at once
    w.run(600);
    CHECK(w.jog(1, 1.0f, 100.0f));
    w.run(1200);
    CHECK(w.session.phase() == JcJogPhase::Idle);
    CHECK(w.session.can_begin(w.now) == JcJogReject::Debounce || w.session.can_begin(w.now) == JcJogReject::None);
    // after the quiet time it works again
    w.run(1000);
    CHECK(w.session.can_begin(w.now) == JcJogReject::None);
    CHECK(w.jog(1, -1.0f, 100.0f));
    w.run(1500);
    CHECK(w.lines.size() == 3 && w.session.completed() == 3);
    // an immediate repeat after a start is Debounce when the first one already resolved quickly
    {
        World v;
        v.settle();
        v.move_ms = 20;
        v.ack_delay = 10;
        CHECK(v.jog(0, 0.5f, 100.0f));
        v.run(300);
        CHECK(v.session.phase() == JcJogPhase::Idle);
        const JcJogReject r = v.session.can_begin(v.now);
        CHECK(r == JcJogReject::Debounce || r == JcJogReject::None);
        CHECK(!v.jog(0, 0.5f, 100.0f) || r == JcJogReject::None);
    }
}

static void test_position_reporting() {
    // position ends where it should for each axis and direction, repeatedly
    World w;
    w.settle();
    const struct { uint8_t axis; float dist; float feed; } steps[] = {
        { 0, 1.0f, 100.0f }, { 0, -1.0f, 100.0f }, { 1, -0.5f, 100.0f }, { 1, 0.5f, 50.0f }, { 0, -0.25f, 50.0f },
        { 1, 0.001f, 100.0f },
    };
    float expect[4] = { w.pos[0], w.pos[1], w.pos[2], w.pos[3] };
    for (const auto& s : steps) {
        w.run(700);
        CHECK(w.session.can_begin(w.now) == JcJogReject::None);
        CHECK(w.jog(s.axis, s.dist, s.feed));
        w.run(2500);
        expect[s.axis] += s.dist;
        CHECK(w.session.result() == JcJogResult::Completed && w.session.pos_check() == JcJogPosCheck::Match);
        for (int i = 0; i < 4; ++i) CHECK(fabsf(w.pos[i] - expect[i]) < 0.0015f);
        CHECK(fabsf(w.session.pos_delta() - s.dist) < 0.01f);
    }
    CHECK(w.session.completed() == 6 && w.lines.size() == 6);
    // Z and A never moved
    CHECK(w.pos[2] == 5.0f && w.pos[3] == 0.0f);

    // a controller that stops short is flagged and blocks further jogs
    World m;
    m.settle();
    m.wrong_position = true;
    CHECK(m.jog(0, 1.0f, 100.0f));
    m.run(2500);
    CHECK(m.session.result() == JcJogResult::PositionMismatch && m.session.pos_check() == JcJogPosCheck::Mismatch);
    CHECK(m.session.needs_acknowledgement() && m.session.completed() == 0);
    CHECK(fabsf(m.session.pos_delta() - 0.6f) < 0.01f);
    CHECK(m.session.can_begin(m.now) == JcJogReject::ResultUnacknowledged);

    // no position available: completion is still reported, but as NotChecked
    World n;
    n.settle();
    JcJogStatus s;
    s.fresh = true;
    s.state = "Idle";
    s.sequence = 7000;
    s.pos_valid = false;
    n.status_on = false;
    n.session.on_status(s, n.now);
    CHECK(n.jog(0, 1.0f, 100.0f));
    n.run(100);
    JcJogStatus j;
    j.fresh = true; j.state = "Jog"; j.sequence = 7001; j.pos_valid = false;
    n.session.on_status(j, n.now);
    JcJogStatus i2;
    i2.fresh = true; i2.state = "Idle"; i2.sequence = 7002; i2.pos_valid = false;
    n.session.on_status(i2, n.now);
    CHECK(n.session.result() == JcJogResult::Completed && n.session.pos_check() == JcJogPosCheck::NotChecked);

    // Idle seen too early (before any motion and before the position moved) is not completion
    World e;
    e.settle();
    e.move_ms = 1500;
    CHECK(e.jog(0, 1.0f, 100.0f));
    e.status_on = false;
    e.run(100);                    // acknowledged
    JcJogStatus idle;
    idle.fresh = true; idle.state = "Idle"; idle.sequence = 9000; idle.pos_valid = true;
    for (int k = 0; k < 4; ++k) idle.pos[k] = e.pos[k];
    e.session.on_status(idle, e.now);
    CHECK(e.session.phase() == JcJogPhase::Accepted && e.session.result() == JcJogResult::None);
    idle.sequence = 9001;
    e.session.on_status(idle, e.now);   // a second Idle with no change: the jog never started
    CHECK(e.session.phase() == JcJogPhase::Idle && e.session.result() == JcJogResult::PositionMismatch);
}

static void test_refusals_at_the_sink() {
    World w;
    w.settle();
    // hand-crafted lines can never get past the transport-level check
    CHECK(!w.jog_text("$J=G91G21F100X10.000"));
    CHECK(!w.jog_text("$J=G91G21F100X1.001"));
    CHECK(!w.jog_text("$J=G91G21F100A1.000"));        // rotary axis
    CHECK(!w.jog_text("$J=G91G21F3000X1.000"));
    CHECK(!w.jog_text("$H"));
    CHECK(!w.jog_text("G0 X100"));
    CHECK(!w.jog_text("$X"));
    CHECK(!w.jog_text("$J=G91G21F100X1.000\n$H"));
    CHECK(w.lines.empty() && w.cancels == 0);
    CHECK(w.session.refused_policy() == 8);
    // other classes never reach the transport
    const JcCommandClass others[] = { JcCommandClass::Home, JcCommandClass::Probe, JcCommandClass::WorkOffset,
                                      JcCommandClass::Spindle, JcCommandClass::Job, JcCommandClass::Macro,
                                      JcCommandClass::Unlock, JcCommandClass::Reset, JcCommandClass::Hold,
                                      JcCommandClass::Other };
    for (JcCommandClass k : others) CHECK(!JcJogSession::sink(&w.session, k, "$H"));
    CHECK(w.lines.empty() && w.cancels == 0 && w.session.refused_class() == 10);
    // a valid jog still works afterwards
    CHECK(w.jog(0, 1.0f, 100.0f));
    CHECK(w.lines.size() == 1);
}

// ---- authorization and dispatcher -----------------------------------------------------

static JcDispatchContext ready_context(const World& w, bool confirmed) {
    JcDispatchContext c;
    c.live = true;
    c.link_fresh = true;
    c.machine_state = "Idle";
    c.operator_confirmed = confirmed;
    c.now_ms = w.now;
    c.limits_mask = 0x0F;
    c.envelope_mask = 0x0F;
    c.operation_pending = w.session.unresolved();
    return c;
}

static void test_authorization() {
    JcAuthContext a;
    a.armed = true;
    a.live = true;
    a.link_fresh = true;
    a.operator_confirmed = true;
    a.now_ms = 1000;
    a.command_issued_ms = 1000;
    a.limits_required = a.limits_confirmed = true;
    a.envelope_required = a.envelope_confirmed = true;
    strcpy(a.machine_state, "Idle");

#if !JC_MACHINE_CONTROL_ENABLED
    const JcCommandClass all[] = { JcCommandClass::Jog, JcCommandClass::JogCancel, JcCommandClass::Home,
                                   JcCommandClass::Probe, JcCommandClass::WorkOffset, JcCommandClass::Spindle,
                                   JcCommandClass::Job, JcCommandClass::Macro, JcCommandClass::Unlock,
                                   JcCommandClass::Reset, JcCommandClass::Hold, JcCommandClass::Other };
    for (JcCommandClass k : all) CHECK(jc_authorize(k, a) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(JC_LIVE_JOG_TEST == 0);
    // nothing can be sent through the dispatcher either, whatever the session would do
    World w;
    w.settle();
    JcDispatcher d;
    d.set_sink_ex(&JcJogSession::sink, &w.session);
    d.arm_for(w.now, kJcJogArmTtlMs);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_jog_cancel(), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(w.lines.empty() && w.cancels == 0 && d.sent() == 0);
#else
    // ---- rules common to every enabled configuration --------------------------------
    JcAuthContext b = a;
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
    // stopping never depends on arming, freshness or command age
    b.armed = false;
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
    b.link_fresh = false;
    b.command_issued_ms = 0;
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
    b.live = false;
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::BlockedNotLive);   // never DEMO
    // jogging does
    b = a; b.armed = false;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedNotArmed);
    b = a; b.link_fresh = false;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedStaleLink);
    b = a; b.command_issued_ms = 100;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedStaleCommand);
    // unresolved operations block new motion but never a stop
    b = a; b.operation_pending = true;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedOperationPending);
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
    // limits and envelope must have been read
    b = a; b.limits_confirmed = false;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedLimitsUnconfirmed);
    b = a; b.envelope_confirmed = false;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedEnvelopeUnread);
    b = a; strcpy(b.machine_state, "Alarm");
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedAlarm);
    b = a; strcpy(b.machine_state, "Run");
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedBadState);
    CHECK(jc_authorize(JcCommandClass::JogCancel, b) == JcAuthDecision::Allowed);
#if JC_LIVE_JOG_TEST
    // ---- the experimental live-jog configuration --------------------------------------
    CHECK(jc_needs_confirmation(JcCommandClass::Jog));
    b = a; b.operator_confirmed = false;
    CHECK(jc_authorize(JcCommandClass::Jog, b) == JcAuthDecision::BlockedUnconfirmed);
    CHECK(jc_authorize(JcCommandClass::Jog, a) == JcAuthDecision::Allowed);
    const JcCommandClass blocked[] = { JcCommandClass::Home, JcCommandClass::Probe, JcCommandClass::WorkOffset,
                                       JcCommandClass::Spindle, JcCommandClass::Job, JcCommandClass::Macro,
                                       JcCommandClass::Unlock, JcCommandClass::Reset, JcCommandClass::Hold,
                                       JcCommandClass::Other };
    for (JcCommandClass k : blocked) {
        CHECK(jc_authorize(k, a) == JcAuthDecision::BlockedBuildPolicy);   // armed, live, fresh, confirmed: still blocked
        b = a; strcpy(b.machine_state, "Alarm");
        CHECK(jc_authorize(k, b) == JcAuthDecision::BlockedBuildPolicy);
    }
#else
    CHECK(!jc_needs_confirmation(JcCommandClass::Jog));
    CHECK(jc_authorize(JcCommandClass::Jog, a) == JcAuthDecision::Allowed);
#endif
#endif
}

#if JC_MACHINE_CONTROL_ENABLED
static void test_dispatcher_end_to_end() {
    World w;
    w.settle();
    JcDispatcher d;
    d.set_sink_ex(&JcJogSession::sink, &w.session);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);

    // not armed: nothing leaves the device
    CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true)) == JcAuthDecision::BlockedNotArmed);
    CHECK(w.lines.empty() && d.sent() == 0);

    // arming lapses by itself
    d.arm_for(w.now, 1000);
    CHECK(d.armed_at(w.now) && d.arm_remaining_ms(w.now) == 1000);
    w.run(1200);
    CHECK(!d.armed_at(w.now));
    CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true)) == JcAuthDecision::BlockedNotArmed);

    // the JOG CANCEL path works without arming
    w.session.set_clock(w.now);
    CHECK(d.submit(jc_jog_cancel(), ready_context(w, false)) == JcAuthDecision::Allowed);
    CHECK(w.cancels == 1 && d.sent() == 1);

    // armed + unprotected-micro-jog acknowledged for X and Y + confirmed: exactly one jog is
    // transmitted and a second is refused while unresolved
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);
    w.run(400);
    w.session.set_clock(w.now);
    JcAuthDecision r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
#if JC_LIVE_JOG_TEST
    CHECK(r == JcAuthDecision::Allowed && w.lines.size() == 1 && d.sent() == 2);
    CHECK_STR(w.lines[0].c_str(), "$J=G91G21F100X1.000");
    w.run(200);
    r = d.submit(jc_jog_incremental(p, 1, 1.0f, 100.0f), ready_context(w, true));
    CHECK(r == JcAuthDecision::BlockedOperationPending);   // refused by the gate itself
    CHECK(w.lines.size() == 1);
    w.run(2000);
    CHECK(w.session.result() == JcJogResult::Completed);

    // confirmation is required for every test jog
    w.run(600);
    r = d.submit(jc_jog_incremental(p, 0, -1.0f, 100.0f), ready_context(w, false));
    CHECK(r == JcAuthDecision::BlockedUnconfirmed && w.lines.size() == 1);

    // other classes never reach the transport, even armed and confirmed
    const JcCommand home = jc_home(p, 0xFF);
    CHECK(d.submit(home, ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_run_sd_file("/a.nc"), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_run_macro_file("/m.nc"), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_spindle_stop(), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_probe_move(p, 2, -5.0f, 100.0f), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_zero_axis(p, 0), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_unlock(), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_soft_reset(), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.submit(jc_feed_hold(), ready_context(w, true)) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(w.lines.size() == 1);
    // continuous jog is never generated in test mode
    CHECK(!jc_jog_continuous(p, 0, 1, 50.0f).ok);
    // oversized and rotary requests never become commands, so they never reach the gate
    CHECK(!jc_jog_incremental(p, 0, 10.0f, 100.0f).ok);
    CHECK(!jc_jog_incremental(p, 3, 1.0f, 100.0f).ok);
#else
    CHECK(r == JcAuthDecision::Allowed);   // ordinary enabled build: allowed, unconfirmed jogs permitted
#endif
}

static void test_no_replay_after_reconnect() {
    World w;
    w.settle();
    JcDispatcher d;
    d.set_sink_ex(&JcJogSession::sink, &w.session);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);
    w.session.set_clock(w.now);
    w.silent = true;
    const JcAuthDecision first = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
#if JC_LIVE_JOG_TEST
    CHECK(first == JcAuthDecision::Allowed && w.lines.size() == 1);
    w.run(300);
    // the link drops: arming is lost, the session recovers, nothing is replayed
    d.on_link_lost();
    w.session.on_link_lost(w.now, false);
    CHECK(!d.armed());
    CHECK(d.unprotected_ack_mask() == 0);   // the acknowledgement died with the arming
    w.silent = false;
    w.run(3000);
    CHECK(w.lines.size() == 1 && w.session.jogs_sent() == 1 && d.sent() == 1);
    // new jog needs re-arming, a NEW unprotected acknowledgement, an acknowledged failure and fresh status
    JcAuthDecision r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
    CHECK(r == JcAuthDecision::BlockedNotArmed);
    d.arm_for(w.now, kJcJogArmTtlMs);   // armed again, but nothing acknowledged
    r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
    CHECK(r == JcAuthDecision::BlockedOperationPending && w.lines.size() == 1);
    CHECK(w.session.acknowledge());
    w.session.set_clock(w.now);
    r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
    CHECK(r == JcAuthDecision::BlockedUnprotectedNotAcknowledged && w.lines.size() == 1);   // no persistence
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);   // the operator deliberately acknowledges again
    r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), ready_context(w, true));
    CHECK(r == JcAuthDecision::Allowed && w.lines.size() == 2);
#else
    (void)first;
#endif
}
#endif

// ---- the extended configuration read (soft/hard limits, homing) ----------------------

struct ConfigController {
    // Replies like a controller that has soft limits on X/Y/Z, hard limits on none, homing on X/Y/Z.
    std::vector<std::string> log;
    bool refuse_a = true;
    int bad_value_for = -1;
    const char* reply(const char* line) {
        static char out[160];
        log.push_back(line);
        if (strcmp(line, "$I") == 0) return "[VER:4.1 FluidNC v4.1.1 (esp32s3-wifi) :]\r\n[OPT:PH]\r\nok\r\n";
        if (strcmp(line, "$G") == 0) return "[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]\r\nok\r\n";
        if (strcmp(line, "$/probe/pin") == 0) return "$/probe/pin=gpio.48:low\r\nok\r\n";
        if (strcmp(line, "$/probe/toolsetter_pin") == 0) return "$/probe/toolsetter_pin=NO_PIN\r\nok\r\n";
        if (strcmp(line, "$/start/must_home") == 0) return "$/start/must_home=true\r\nok\r\n";
        int axis = -1;
        for (int a = 0; a < 4; ++a) if (line[7] == "xyza"[a]) axis = a;
        const char* item = line + 9;
        if (axis == 3 && refuse_a && (strncmp(item, "homing/", 7) == 0)) return "error:3\r\n";
        const char* value = "1";
        if (strcmp(item, "steps_per_mm") == 0) value = axis == 2 ? "200.000" : (axis == 3 ? "1000.000" : "40.000");
        else if (strcmp(item, "max_travel_mm") == 0) value = axis == 0 ? "1000.000" : axis == 1 ? "2300.000" : axis == 2 ? "150.000" : "3000.000";
        else if (strcmp(item, "max_rate_mm_per_min") == 0) value = axis == 2 ? "1500.000" : axis == 3 ? "1000.000" : "6000.000";
        else if (strcmp(item, "acceleration_mm_per_sec2") == 0) value = axis == 2 ? "100.000" : axis == 3 ? "25.000" : "250.000";
        else if (strcmp(item, "homing/cycle") == 0) value = "1";
        else if (strcmp(item, "homing/positive_direction") == 0) value = "true";
        else if (strcmp(item, "soft_limits") == 0) value = axis == 3 ? "false" : "true";
        else if (strcmp(item, "motor0/hard_limits") == 0) value = "false";
        else if (strcmp(item, "motor0/limit_neg_pin") == 0) value = "NO_PIN";
        else if (strcmp(item, "motor0/limit_pos_pin") == 0) value = axis == 2 ? "gpio.35:low:pu" : "NO_PIN";
        else if (strcmp(item, "motor0/limit_all_pin") == 0) value = axis == 0 ? "gpio.34:low:pu" : "NO_PIN";
        else if (strcmp(item, "homing/mpos_mm") == 0) value = "0.000";
        if (axis == bad_value_for && strcmp(item, "soft_limits") == 0) value = "maybe";
        snprintf(out, sizeof(out), "%s=%s\r\nok\r\n", line, value);
        return out;
    }
};

static void run_read(JcCapsLoader& loader, ConfigController& ctl, uint32_t& now) {
    for (int i = 0; i < 400 && loader.running(); ++i) {
        if (const char* q = loader.pending_line(now)) {
            CHECK(jc_query_allowed(q));
            const char* r = ctl.reply(q);
            loader.mark_sent(now);
            char buf[160];
            snprintf(buf, sizeof(buf), "%s", r);
            for (char* p = strtok(buf, "\r\n"); p; p = strtok(nullptr, "\r\n")) loader.on_line(p, now);
        }
        now += 20;
        loader.tick(now);
    }
}

static void test_config_read() {
    // allowlist: new items are reads only
    CHECK(jc_query_allowed("$/axes/x/soft_limits"));
    CHECK(jc_query_allowed("$/axes/a/motor0/hard_limits"));
    CHECK(jc_query_allowed("$/axes/z/motor0/limit_pos_pin"));
    CHECK(jc_query_allowed("$/axes/y/motor0/limit_all_pin"));
    CHECK(jc_query_allowed("$/axes/x/homing/mpos_mm"));
    CHECK(jc_query_allowed("$/start/must_home"));
    CHECK(!jc_query_allowed("$/axes/x/soft_limits=false"));     // an assignment is never a query
    CHECK(!jc_query_allowed("$/axes/x/soft_limits=true"));
    CHECK(!jc_query_allowed("$/start/must_home=false"));
    CHECK(!jc_query_allowed("$/axes/x/motor0/hard_limits=true"));
    CHECK(!jc_query_allowed("$/axes/x/motor1/hard_limits"));
    CHECK(!jc_query_allowed("$/axes/b/soft_limits"));
    CHECK(!jc_query_allowed("$/start/must_home "));
    CHECK(!jc_query_allowed("$/start/"));
    CHECK(!jc_query_allowed("$/axes/x/motor0/limit_all_pin=NO_PIN"));
    CHECK(JcCapsLoader::kSteps == 2 + 4 * 12 + 3);

    ConfigController ctl;
    JcCapsLoader loader;
    uint32_t now = 1000;
    CHECK(loader.start(now, nullptr));
    run_read(loader, ctl, now);
    CHECK(loader.state() == JcLoadState::Complete);
    CHECK(ctl.log.size() == JcCapsLoader::kSteps);
    const JcMachineCaps& c = loader.caps();
    CHECK(c.complete && c.queries_ok + c.queries_error == JcCapsLoader::kSteps);
    CHECK(c.queries_error == 3);                     // A: homing/cycle, positive_direction and mpos_mm refused
    CHECK_STR(c.firmware, "4.1 FluidNC v4.1.1 (esp32s3-wifi)");
    CHECK(c.must_home_state == JcFieldState::Valid && c.must_home && c.must_home_tri() == JcTri::Yes);
    // limits / envelope
    CHECK(c.axes[0].soft_limits() == JcTri::Yes && c.axes[1].soft_limits() == JcTri::Yes);
    CHECK(c.axes[2].soft_limits() == JcTri::Yes && c.axes[3].soft_limits() == JcTri::No);
    CHECK(c.axes[0].hard_limits() == JcTri::No);
    CHECK(c.axes[0].limit_pins_assigned() == 1 && c.axes[2].limit_pins_assigned() == 1 && c.axes[1].limit_pins_assigned() == 0);
    CHECK_STR(c.axes[2].pin[1], "gpio.35:low:pu");
    CHECK_STR(c.axes[0].pin[2], "gpio.34:low:pu");
    CHECK_STR(c.axes[1].pin[0], "NO_PIN");
    CHECK(c.axes[0].get(JcQueryField::HomingMpos) == 0.0f && c.axes[0].valid(JcQueryField::HomingMpos));
    CHECK(c.confirmed_axis_mask() == 0x0F && c.envelope_axis_mask() == 0x0F);
    // A: the homing items were refused, so homing is unknown, never "no"
    CHECK(!c.axes[3].homing_known() && c.axes[3].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] == JcFieldState::CtrlError);
    CHECK(c.axes[3].error_code[static_cast<uint8_t>(JcQueryField::HomingCycle)] == 3);
    CHECK(c.axes[0].homing_known() && c.axes[0].get(JcQueryField::HomingCycle) == 1.0f);

    // an unreadable value is "unknown", not "no"
    ConfigController bad;
    bad.bad_value_for = 1;
    JcCapsLoader l2;
    now = 5000;
    CHECK(l2.start(now, nullptr));
    run_read(l2, bad, now);
    CHECK(l2.state() == JcLoadState::Complete);
    CHECK(l2.caps().axes[1].soft_limits() == JcTri::Unknown);
    CHECK(l2.caps().axes[1].state[static_cast<uint8_t>(JcQueryField::SoftLimits)] == JcFieldState::BadValue);
    CHECK(l2.caps().axes[1].envelope_read());           // reached, though unreadable: the operator is shown "UNKNOWN"
    // a read that never reached the envelope items confirms no envelope
    JcCapsLoader l3;
    now = 9000;
    CHECK(l3.start(now, nullptr));
    CHECK(l3.pending_line(now) != nullptr);
    CHECK(l3.caps().envelope_axis_mask() == 0);
    JcMachineCaps empty;
    CHECK(empty.axes[0].soft_limits() == JcTri::Unknown && empty.must_home_tri() == JcTri::Unknown);
    CHECK(empty.axes[0].limit_pins_assigned() == -1);

    // value validators
    float v = 0.0f;
    CHECK(jc_caps_field_valid(JcQueryField::SoftLimits, "true", v) && v == 1.0f);
    CHECK(jc_caps_field_valid(JcQueryField::HardLimits, "false", v) && v == 0.0f);
    CHECK(!jc_caps_field_valid(JcQueryField::SoftLimits, "TRUE", v));
    CHECK(!jc_caps_field_valid(JcQueryField::SoftLimits, "1", v));
    CHECK(!jc_caps_field_valid(JcQueryField::SoftLimits, "", v));
    CHECK(jc_caps_field_valid(JcQueryField::LimitNegPin, "NO_PIN", v) && v == 0.0f);
    CHECK(jc_caps_field_valid(JcQueryField::LimitNegPin, "gpio.34:low:pu", v) && v == 1.0f);
    CHECK(!jc_caps_field_valid(JcQueryField::LimitNegPin, "gpio 34", v));
    CHECK(!jc_caps_field_valid(JcQueryField::LimitNegPin, "a;b", v));
    CHECK(!jc_caps_field_valid(JcQueryField::LimitNegPin, "", v));
    CHECK(jc_caps_field_valid(JcQueryField::HomingMpos, "-5.000", v) && v == -5.0f);
    CHECK(jc_caps_field_valid(JcQueryField::HomingMpos, "0", v));
    CHECK(!jc_caps_field_valid(JcQueryField::HomingMpos, "nan", v));
    CHECK(!jc_caps_field_valid(JcQueryField::HomingMpos, "1e9", v));
}

// The envelope decision: a configured travel is never a proof of a safe movement envelope.
static void test_envelope_rules() {
    JcMachineCaps caps;
    // travel and rates read, but the soft / hard limit items never reached
    for (uint8_t a = 0; a < 4; ++a) {
        const JcQueryField f[4] = { JcQueryField::StepsPerMm, JcQueryField::MaxTravelMm, JcQueryField::MaxRate,
                                    JcQueryField::Accel };
        for (int k = 0; k < 4; ++k) {
            caps.axes[a].state[static_cast<uint8_t>(f[k])] = JcFieldState::Valid;
            caps.axes[a].value[static_cast<uint8_t>(f[k])] = 100.0f;
        }
    }
    CHECK(caps.confirmed_axis_mask() == 0x0F);   // limits confirmed...
    CHECK(caps.envelope_axis_mask() == 0x00);    // ...but the envelope settings were not read
    JcDispatcher d;
    JcDispatchContext c;
    c.live = true;
    c.link_fresh = true;
    c.machine_state = "Idle";
    c.operator_confirmed = true;
    c.now_ms = 1000;
    c.limits_mask = caps.confirmed_axis_mask();
    c.envelope_mask = caps.envelope_axis_mask();
    d.arm_for(1000, 100000);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    const JcCommand jog = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    const JcAuthDecision before = d.evaluate(jog, c);
#if JC_MACHINE_CONTROL_ENABLED
    CHECK(before == JcAuthDecision::BlockedEnvelopeUnread);
#else
    CHECK(before == JcAuthDecision::BlockedBuildPolicy);
#endif
    c.envelope_mask = 0x0F;   // read (answered or refused): the operator has been shown the facts
    const JcAuthDecision after = d.evaluate(jog, c);
#if JC_MACHINE_CONTROL_ENABLED
    CHECK(after != JcAuthDecision::BlockedEnvelopeUnread);
#else
    CHECK(after == JcAuthDecision::BlockedBuildPolicy);
#endif
    // a dry run evaluates and records nothing
    CHECK(d.history_count() == 0 && d.sent() == 0 && d.blocked() == 0);
}

// ---- Instruction 006A: the jog gate against the REAL controller state -------------------------

static void set_value(JcAxisCaps& a, JcQueryField f, float v) {
    a.state[static_cast<uint8_t>(f)] = JcFieldState::Valid;
    a.value[static_cast<uint8_t>(f)] = v;
}
static void set_pin(JcAxisCaps& a, JcQueryField f, const char* text) {
    const uint8_t slot = static_cast<uint8_t>(f) - static_cast<uint8_t>(JcQueryField::LimitNegPin);
    a.state[static_cast<uint8_t>(f)] = JcFieldState::Valid;
    a.value[static_cast<uint8_t>(f)] = strcmp(text, "NO_PIN") == 0 ? 0.0f : 1.0f;
    snprintf(a.pin[slot], sizeof(a.pin[slot]), "%s", text);
}
static void set_refused(JcAxisCaps& a, JcQueryField f, int code) {
    a.state[static_cast<uint8_t>(f)] = JcFieldState::CtrlError;
    a.error_code[static_cast<uint8_t>(f)] = code;
}

// The accepted baseline: 50 answered / 3 refused, read 2026-10-30.
static JcMachineCaps real_baseline(uint32_t completed_ms) {
    JcMachineCaps c;
    c.complete = true;
    c.completed_ms = completed_ms;
    snprintf(c.firmware, sizeof(c.firmware), "4.1 FluidNC v4.1.1 (esp32s3-wifi)");
    c.must_home_state = JcFieldState::Valid;
    c.must_home = false;
    struct Row { float steps, travel, rate, accel, cycle, dir; const char* neg; };
    const Row rows[3] = { { 40, 1000, 6000, 250, 1, 0, "gpio.41:low" }, { 40, 2300, 6000, 250, 1, 0, "gpio.40:low" },
                          { 200, 150, 1500, 100, 1, 1, "gpio.39:low" } };
    for (uint8_t i = 0; i < 3; ++i) {
        JcAxisCaps& a = c.axes[i];
        set_value(a, JcQueryField::StepsPerMm, rows[i].steps);
        set_value(a, JcQueryField::MaxTravelMm, rows[i].travel);
        set_value(a, JcQueryField::MaxRate, rows[i].rate);
        set_value(a, JcQueryField::Accel, rows[i].accel);
        set_value(a, JcQueryField::HomingCycle, rows[i].cycle);
        set_value(a, JcQueryField::HomingPositiveDir, rows[i].dir);
        set_value(a, JcQueryField::SoftLimits, 0.0f);
        set_value(a, JcQueryField::HardLimits, 0.0f);
        set_pin(a, JcQueryField::LimitNegPin, rows[i].neg);
        set_pin(a, JcQueryField::LimitPosPin, "NO_PIN");
        set_pin(a, JcQueryField::LimitAllPin, "NO_PIN");
        set_value(a, JcQueryField::HomingMpos, 0.0f);
    }
    JcAxisCaps& a = c.axes[3];
    set_value(a, JcQueryField::StepsPerMm, 1000);
    set_value(a, JcQueryField::MaxTravelMm, 3000);
    set_value(a, JcQueryField::MaxRate, 1000);
    set_value(a, JcQueryField::Accel, 25);
    set_refused(a, JcQueryField::HomingCycle, 3);
    set_refused(a, JcQueryField::HomingPositiveDir, 3);
    set_value(a, JcQueryField::SoftLimits, 0.0f);
    set_value(a, JcQueryField::HardLimits, 0.0f);
    set_pin(a, JcQueryField::LimitNegPin, "NO_PIN");
    set_pin(a, JcQueryField::LimitPosPin, "NO_PIN");
    set_pin(a, JcQueryField::LimitAllPin, "NO_PIN");
    set_refused(a, JcQueryField::HomingMpos, 3);
    return c;
}

static JcJogGate gate_for(const JcMachineCaps& c, uint8_t axis, uint32_t now = 10000) {
    return jc_jog_gate(c, true, now, axis, jc_position_trust(c));
}

static void test_jog_gate_modes() {
    const JcMachineCaps c = real_baseline(10000);

    // soft OFF + hard OFF + fresh read -> UNPROTECTED MICRO-JOG, for X and Y.
    for (uint8_t axis = 0; axis < 2; ++axis) {
        const JcJogGate g = gate_for(c, axis);
        CHECK(g.mode == JcJogGateMode::UnprotectedMicro && g.reason == JcJogGateReason::None);
    }
    // Z is excluded from the first-motion policy whatever the controller says; A stays disabled.
    CHECK(gate_for(c, 2).mode == JcJogGateMode::Blocked && gate_for(c, 2).reason == JcJogGateReason::AxisExcluded);
    CHECK(gate_for(c, 3).mode == JcJogGateMode::Blocked && gate_for(c, 3).reason == JcJogGateReason::AxisExcluded);
    CHECK(gate_for(c, 7).reason == JcJogGateReason::AxisExcluded);
    uint8_t env = 0, prot = 0;
    jc_jog_gate_masks(c, true, 10000, env, prot);
    CHECK(env == 0x03 && prot == 0x00);   // X and Y known, nothing protected

    // UNKNOWN is BLOCKED, never treated as OFF: refused, unreadable, not queried.
    for (JcFieldState bad : { JcFieldState::CtrlError, JcFieldState::BadValue, JcFieldState::NoValue, JcFieldState::Unqueried }) {
        JcMachineCaps u = real_baseline(10000);
        u.axes[0].state[static_cast<uint8_t>(JcQueryField::SoftLimits)] = bad;
        CHECK(gate_for(u, 0).mode == JcJogGateMode::Blocked && gate_for(u, 0).reason == JcJogGateReason::SoftUnknown);
        CHECK(gate_for(u, 1).mode == JcJogGateMode::UnprotectedMicro);   // another axis is unaffected
        u = real_baseline(10000);
        u.axes[1].state[static_cast<uint8_t>(JcQueryField::HardLimits)] = bad;
        CHECK(gate_for(u, 1).mode == JcJogGateMode::Blocked && gate_for(u, 1).reason == JcJogGateReason::HardUnknown);
        for (JcQueryField pin : { JcQueryField::LimitNegPin, JcQueryField::LimitPosPin, JcQueryField::LimitAllPin }) {
            u = real_baseline(10000);
            u.axes[0].state[static_cast<uint8_t>(pin)] = bad;
            CHECK(gate_for(u, 0).mode == JcJogGateMode::Blocked && gate_for(u, 0).reason == JcJogGateReason::PinUnknown);
        }
    }
    // error:3 on A's homing items does not matter for X/Y and is never converted to OFF.
    CHECK(!c.axes[3].homing_known());
    CHECK(c.axes[3].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] == JcFieldState::CtrlError);

    // Not read / incomplete / stale.
    CHECK(jc_jog_gate(c, false, 10000, 0, JcPositionTrust::Untrusted).reason == JcJogGateReason::NotRead);
    JcMachineCaps partial = real_baseline(10000);
    partial.complete = false;
    CHECK(gate_for(partial, 0).reason == JcJogGateReason::NotRead);
    CHECK(gate_for(c, 0, 10000 + kJcCapsMaxAgeMs).mode == JcJogGateMode::UnprotectedMicro);        // exactly at the limit
    CHECK(gate_for(c, 0, 10000 + kJcCapsMaxAgeMs + 1).reason == JcJogGateReason::Stale);           // one ms over
    CHECK(gate_for(c, 1, 10000 + 10u * kJcCapsMaxAgeMs).mode == JcJogGateMode::Blocked);
    jc_jog_gate_masks(c, true, 10000 + kJcCapsMaxAgeMs + 1, env, prot);
    CHECK(env == 0 && prot == 0);

    // Protected needs trusted protection AND an established position. Neither is reachable
    // from anything the firmware can read; the branch exists so that it is deliberate.
    JcMachineCaps on = real_baseline(10000);
    set_value(on.axes[0], JcQueryField::SoftLimits, 1.0f);
    set_value(on.axes[0], JcQueryField::HardLimits, 1.0f);
    CHECK(jc_jog_gate(on, true, 10000, 0, JcPositionTrust::Untrusted).mode == JcJogGateMode::UnprotectedMicro);
    CHECK(jc_jog_gate(on, true, 10000, 0, JcPositionTrust::Established).mode == JcJogGateMode::Protected);
    CHECK(jc_jog_gate(c, true, 10000, 0, JcPositionTrust::Established).mode == JcJogGateMode::UnprotectedMicro);   // soft OFF
    CHECK(jc_jog_gate(c, true, 10000, 2, JcPositionTrust::Established).mode == JcJogGateMode::Blocked);            // Z still excluded
    JcMachineCaps unk = real_baseline(10000);
    unk.axes[0].state[static_cast<uint8_t>(JcQueryField::SoftLimits)] = JcFieldState::CtrlError;
    CHECK(jc_jog_gate(unk, true, 10000, 0, JcPositionTrust::Established).mode == JcJogGateMode::Blocked);
}

static void test_position_trust_and_wording() {
    // must_home, homing configuration and travel values never establish a position or a path.
    for (int must_home : { -1, 0, 1 }) {
        for (float cycle : { 0.0f, 1.0f, 3.0f }) {
            for (float travel : { 1.0f, 150.0f, 1000.0f, 100000.0f }) {
                JcMachineCaps c = real_baseline(0);
                c.must_home_state = must_home < 0 ? JcFieldState::CtrlError : JcFieldState::Valid;
                c.must_home = must_home == 1;
                for (uint8_t a = 0; a < 3; ++a) {
                    set_value(c.axes[a], JcQueryField::HomingCycle, cycle);
                    set_value(c.axes[a], JcQueryField::MaxTravelMm, travel);
                    set_value(c.axes[a], JcQueryField::HomingMpos, 0.0f);
                }
                CHECK(jc_position_trust(c) == JcPositionTrust::Untrusted);
                for (uint8_t a = 0; a < 4; ++a) CHECK(jc_path_claim(c, a, jc_position_trust(c)) == JcPathClaim::None);
                // must_home plays no part in the jog gate: the mode is the same for every value.
                CHECK(jc_jog_gate(c, true, 0, 0, jc_position_trust(c)).mode == JcJogGateMode::UnprotectedMicro);
            }
        }
    }
    CHECK(jc_path_claim(real_baseline(0), 0, JcPositionTrust::Established) == JcPathClaim::Assessed);

    // A configured limit INPUT is not hard-limit PROTECTION: the two are worded apart.
    const JcMachineCaps c = real_baseline(0);
    char input[80], protection[64];
    jc_limit_wording(c.axes[0], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(input, "LIMIT INPUT CONFIGURED (neg gpio.41:low)");
    CHECK_STR(protection, "HARD LIMIT PROTECTION OFF");
    CHECK(strstr(input, "PROTECT") == nullptr && strstr(protection, "CONFIGURED") == nullptr);
    jc_limit_wording(c.axes[1], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(input, "LIMIT INPUT CONFIGURED (neg gpio.40:low)");
    jc_limit_wording(c.axes[2], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(input, "LIMIT INPUT CONFIGURED (neg gpio.39:low)");
    CHECK_STR(protection, "HARD LIMIT PROTECTION OFF");
    jc_limit_wording(c.axes[3], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(input, "NO LIMIT INPUT ASSIGNED");
    CHECK_STR(protection, "HARD LIMIT PROTECTION OFF");
    JcMachineCaps u = real_baseline(0);
    u.axes[0].state[static_cast<uint8_t>(JcQueryField::HardLimits)] = JcFieldState::CtrlError;
    set_refused(u.axes[0], JcQueryField::LimitAllPin, 3);
    jc_limit_wording(u.axes[0], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(input, "LIMIT INPUT UNKNOWN");
    CHECK_STR(protection, "HARD LIMIT PROTECTION UNKNOWN");   // never "OFF"
    JcMachineCaps on = real_baseline(0);
    set_value(on.axes[0], JcQueryField::HardLimits, 1.0f);
    jc_limit_wording(on.axes[0], input, sizeof(input), protection, sizeof(protection));
    CHECK_STR(protection, "HARD LIMIT PROTECTION ON");
    // With hard limits OFF a configured switch is still only an input, and the gate agrees.
    CHECK(gate_for(c, 0).mode == JcJogGateMode::UnprotectedMicro);

    // No wording the gate can show claims a safe path or remaining travel.
    const char* const forbidden[] = { "safe path", "SAFE PATH", "within travel", "WITHIN TRAVEL", "remaining travel",
                                      "REMAINING TRAVEL", "enough travel", "ENOUGH TRAVEL", "path is clear", "PATH IS CLEAR" };
    for (int r = 0; r <= static_cast<int>(JcJogGateReason::PinUnknown); ++r) {
        const char* t = jc_jog_gate_reason_text(static_cast<JcJogGateReason>(r));
        for (const char* f : forbidden) CHECK(strstr(t, f) == nullptr);
    }
    for (int m = 0; m <= static_cast<int>(JcJogGateMode::Protected); ++m) {
        const char* t = jc_jog_gate_mode_name(static_cast<JcJogGateMode>(m));
        for (const char* f : forbidden) CHECK(strstr(t, f) == nullptr);
    }
    for (int d = 0; d <= static_cast<int>(JcAuthDecision::BlockedUnprotectedNotAcknowledged); ++d) {
        const char* t = jc_auth_name(static_cast<JcAuthDecision>(d));
        for (const char* f : forbidden) CHECK(strstr(t, f) == nullptr);
    }
}

static JcDispatchContext gate_context(const World& w, const JcMachineCaps& caps, bool complete, bool confirmed) {
    JcDispatchContext c = ready_context(w, confirmed);
    jc_jog_gate_masks(caps, complete, w.now, c.envelope_mask, c.protected_mask);
    return c;
}

static void test_gate_authorization() {
    World w;
    w.settle();
    const JcMachineCaps caps = real_baseline(w.now);
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    const JcCommand x = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    const JcCommand y = jc_jog_incremental(p, 1, -1.0f, 100.0f);
    CHECK(x.ok && y.ok);
    CHECK(!jc_jog_incremental(p, 2, 0.5f, 50.0f).ok && !jc_jog_incremental(p, 3, 1.0f, 100.0f).ok);   // Z hold, A disabled

    JcDispatcher d;
    d.set_sink_ex(&JcJogSession::sink, &w.session);
    d.arm_for(w.now, kJcJogArmTtlMs, 0);   // armed, but NOTHING acknowledged
    JcDispatchContext ctx = gate_context(w, caps, true, true);
    CHECK(ctx.envelope_mask == 0x03 && ctx.protected_mask == 0);
#if !JC_MACHINE_CONTROL_ENABLED
    CHECK(d.evaluate(x, ctx) == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.evaluate(jc_jog_cancel(), ctx) == JcAuthDecision::BlockedBuildPolicy);
#else
    CHECK(d.evaluate(jc_jog_cancel(), ctx) == JcAuthDecision::Allowed);   // cancel is independent of everything below
#if JC_LIVE_JOG_TEST
    // The operator has not acknowledged "no controller travel protection": blocked.
    CHECK(d.evaluate(x, ctx) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    CHECK(d.evaluate(y, ctx) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    // The acknowledgement is per axis: X only.
    d.arm_for(w.now, kJcJogArmTtlMs, 0x01);
    CHECK(d.evaluate(x, ctx) == JcAuthDecision::Allowed);
    CHECK(d.evaluate(y, ctx) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);
    CHECK(d.evaluate(y, ctx) == JcAuthDecision::Allowed);
    // Without the per-jog operator confirmation nothing is allowed either.
    CHECK(d.evaluate(x, gate_context(w, caps, true, false)) == JcAuthDecision::BlockedUnconfirmed);
    // Protected mode (when it can ever exist) needs no unprotected acknowledgement; unprotected mode never gets one for free.
    JcDispatchContext prot = ctx;
    prot.protected_mask = 0x01;
    d.arm_for(w.now, kJcJogArmTtlMs, 0);
    CHECK(d.evaluate(x, prot) == JcAuthDecision::Allowed);
    CHECK(d.evaluate(y, prot) == JcAuthDecision::BlockedUnprotectedNotAcknowledged);
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);
    // Unknown / stale / not-read capabilities block even an acknowledged, armed, confirmed jog.
    CHECK(d.evaluate(x, gate_context(w, caps, false, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    JcMachineCaps stale = caps;
    stale.completed_ms = w.now - kJcCapsMaxAgeMs - 1;
    CHECK(d.evaluate(x, gate_context(w, stale, true, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    JcMachineCaps unknown = caps;
    unknown.axes[0].state[static_cast<uint8_t>(JcQueryField::HardLimits)] = JcFieldState::CtrlError;
    CHECK(d.evaluate(x, gate_context(w, unknown, true, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    CHECK(d.evaluate(y, gate_context(w, unknown, true, true)) == JcAuthDecision::Allowed);
    // Cancel stays available in every one of those situations, armed or not.
    d.disarm();
    CHECK(d.evaluate(jc_jog_cancel(), gate_context(w, unknown, false, false)) == JcAuthDecision::Allowed);
    CHECK(d.evaluate(jc_jog_cancel(), gate_context(w, stale, true, false)) == JcAuthDecision::Allowed);
    JcDispatchContext dead = gate_context(w, caps, true, false);
    dead.link_fresh = false;
    dead.operation_pending = true;
    CHECK(d.evaluate(jc_jog_cancel(), dead) == JcAuthDecision::Allowed);
    dead.live = false;
    CHECK(d.evaluate(jc_jog_cancel(), dead) == JcAuthDecision::BlockedNotLive);
#endif
#endif
    CHECK(d.history_count() == 0);   // evaluating never records anything
}

static void test_reconnect_clears_authorization() {
    // A complete read, an arming with acknowledgement, then the connection is lost.
    ConfigController ctl;
    JcCapsLoader loader;
    uint32_t now = 1000;
    CHECK(loader.start(now, nullptr));
    run_read(loader, ctl, now);
    CHECK(loader.state() == JcLoadState::Complete);
    uint8_t env = 0, prot = 0;
    jc_jog_gate_masks(loader.caps(), true, now, env, prot);
    CHECK(env == 0x03 && prot == 0);

    JcDispatcher d;
    d.arm_for(now, kJcJogArmTtlMs, 0x03);
    CHECK(d.armed() && d.unprotected_ack_mask() == 0x03);

    // Link lost: exactly what JcApp::note_link_fresh() does.
    d.on_link_lost();
    loader.on_link_lost(now);
    loader.invalidate(now);
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0);
    CHECK(loader.state() == JcLoadState::Idle && !loader.caps().any_data());
    jc_jog_gate_masks(loader.caps(), loader.state() == JcLoadState::Complete, now + 1, env, prot);
    CHECK(env == 0 && prot == 0);   // the old read is gone: a new read is required

    // Reconnected and re-armed WITHOUT a new read: still blocked, and the old acknowledgement is gone.
    d.arm_for(now + 2, kJcJogArmTtlMs, 0);
    CHECK(d.unprotected_ack_mask() == 0);
    CHECK(loader.caps().axes[0].soft_limits() == JcTri::Unknown);
    CHECK(jc_jog_gate(loader.caps(), false, now + 2, 0, jc_position_trust(loader.caps())).reason == JcJogGateReason::NotRead);

    // Disarm / lapse also drop the acknowledgement.
    d.arm_for(now, 1000, 0x03);
    CHECK(!d.armed_at(now + 1001));   // lapsed as far as the gate is concerned
    d.disarm();
    CHECK(d.unprotected_ack_mask() == 0);
}

// ---- Instruction 006B: arming scope and the Limits summary --------------------------------

static void test_arming_scope() {
    JcDispatcher d;
    // Nothing armed: nothing to drop.
    CHECK(d.enforce_arming_scope(1000, true, true) == JcDisarmReason::None);

    // Inside the window, LIVE, on the jog page: kept, with its acknowledgement.
    d.arm_for(1000, 5000, 0x03);
    CHECK(d.enforce_arming_scope(2000, true, true) == JcDisarmReason::None);
    CHECK(d.armed() && d.unprotected_ack_mask() == 0x03);

    // Leaving the jog page drops both.
    CHECK(d.enforce_arming_scope(2100, true, false) == JcDisarmReason::LeftPage);
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0);

    // Expiry drops both (exactly at the end of the window).
    d.arm_for(1000, 5000, 0x01);
    CHECK(d.enforce_arming_scope(5999, true, true) == JcDisarmReason::None);
    CHECK(d.enforce_arming_scope(6000, true, true) == JcDisarmReason::Lapsed);
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0);

    // DEMO mode drops both, even on the jog page and inside the window (the link may stay fresh).
    d.arm_for(1000, 5000, 0x03);
    CHECK(d.enforce_arming_scope(1500, false, true) == JcDisarmReason::DemoMode);
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0);

    // After any drop, going back (LIVE, jog page) does NOT restore the arming or the acknowledgement.
    CHECK(d.enforce_arming_scope(1600, true, true) == JcDisarmReason::None);
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0 && !d.armed_at(1600));

    // Operator disarm and link loss clear the acknowledgement too.
    d.arm_for(1000, 5000, 0x03);
    d.disarm();
    CHECK(d.unprotected_ack_mask() == 0);
    d.arm_for(1000, 5000, 0x03);
    d.on_link_lost();
    CHECK(!d.armed() && d.unprotected_ack_mask() == 0);

    // Precedence when several apply at once: DEMO first, then page, then lapse.
    d.arm_for(1000, 100, 0x03);
    CHECK(d.enforce_arming_scope(9000, false, false) == JcDisarmReason::DemoMode);
    d.arm_for(1000, 100, 0x03);
    CHECK(d.enforce_arming_scope(9000, true, false) == JcDisarmReason::LeftPage);
    for (int r = 0; r <= static_cast<int>(JcDisarmReason::Lapsed); ++r) {
        CHECK(strcmp(jc_disarm_reason_text(static_cast<JcDisarmReason>(r)), "?") != 0);
    }
}

static void test_limits_summary() {
    char lines[2][96];
    // The accepted real baseline: X Y Z have a limit input but hard-limit protection is OFF; A has none.
    JcMachineCaps c = real_baseline(0);
    CHECK(jc_limits_summary(c, lines, 2) == 2);
    CHECK_STR(lines[0], "X Y Z: LIMIT INPUT CONFIGURED, HARD LIMIT PROTECTION OFF");
    CHECK_STR(lines[1], "A: NO LIMIT INPUT ASSIGNED, HARD LIMIT PROTECTION OFF");
    // Never reads as protected while hard limits are OFF.
    for (int i = 0; i < 2; ++i) CHECK(strstr(lines[i], "PROTECTION ON") == nullptr);

    // Nothing read: everything is UNKNOWN, in one group, never OFF.
    JcMachineCaps empty;
    CHECK(jc_limits_summary(empty, lines, 2) == 1);
    CHECK_STR(lines[0], "X Y Z A: LIMIT INPUT UNKNOWN, HARD LIMIT PROTECTION UNKNOWN");

    // One axis with hard limits ON: it is a different group.
    JcMachineCaps on = real_baseline(0);
    set_value(on.axes[1], JcQueryField::HardLimits, 1.0f);
    CHECK(jc_limits_summary(on, lines, 3 > 2 ? 2 : 2) >= 2);
    char three[3][96];
    CHECK(jc_limits_summary(on, three, 3) == 3);
    CHECK_STR(three[0], "X Z: LIMIT INPUT CONFIGURED, HARD LIMIT PROTECTION OFF");
    CHECK_STR(three[1], "Y: LIMIT INPUT CONFIGURED, HARD LIMIT PROTECTION ON");
    CHECK_STR(three[2], "A: NO LIMIT INPUT ASSIGNED, HARD LIMIT PROTECTION OFF");
    // Too many groups for the space: the last line says to look at the table instead of hiding any.
    CHECK(jc_limits_summary(on, lines, 2) == 2);
    CHECK_STR(lines[1], "(other axes differ: see the table)");
}

#if JC_MACHINE_CONTROL_ENABLED && JC_LIVE_JOG_TEST
// Item 9: no jog can reach the wire without a fresh capability read AND a per-jog confirmation.
static void test_no_jog_without_read_and_confirmation() {
    World w;
    w.settle();
    JcMachineProfile p = jc_default_profile();
    jc_profile_apply_test_limits(p);
    const JcCommand x = jc_jog_incremental(p, 0, 1.0f, 100.0f);
    JcDispatcher d;
    d.set_sink_ex(&JcJogSession::sink, &w.session);
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);   // armed and acknowledged: the ONLY things missing are below
    const JcMachineCaps fresh = real_baseline(w.now);

    // 1. No capability read at all: refused, nothing on the wire.
    JcDispatchContext c = gate_context(w, JcMachineCaps(), false, true);
    CHECK(d.submit(x, c) == JcAuthDecision::BlockedEnvelopeUnread);
    CHECK(w.lines.empty() && d.sent() == 0);

    // 2. A fresh read but no per-jog confirmation: refused.
    c = gate_context(w, fresh, true, false);
    CHECK(d.submit(x, c) == JcAuthDecision::BlockedUnconfirmed);
    CHECK(w.lines.empty() && d.sent() == 0);

    // 3. A read that is too old: refused even when confirmed.
    JcMachineCaps old = fresh;
    old.completed_ms = w.now - kJcCapsMaxAgeMs - 1;
    CHECK(d.submit(x, gate_context(w, old, true, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    CHECK(w.lines.empty());

    // 4. A read with an unknown limit: refused even when confirmed.
    JcMachineCaps unk = fresh;
    unk.axes[0].state[static_cast<uint8_t>(JcQueryField::SoftLimits)] = JcFieldState::CtrlError;
    CHECK(d.submit(x, gate_context(w, unk, true, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    CHECK(w.lines.empty());

    // 5. Both present: exactly one jog goes out.
    w.session.set_clock(w.now);
    CHECK(d.submit(x, gate_context(w, fresh, true, true)) == JcAuthDecision::Allowed);
    CHECK(w.lines.size() == 1 && d.sent() == 1);

    // 6. After the connection is lost the read and the arming are gone: nothing goes out until a new
    //    read, a new arming and a new confirmation all exist again.
    w.run(2500);
    CHECK(w.session.result() == JcJogResult::Completed);
    d.on_link_lost();
    w.session.on_link_lost(w.now, false);
    w.run(2500);
    CHECK(w.session.acknowledge() || !w.session.needs_acknowledgement());
    CHECK(d.submit(x, gate_context(w, fresh, true, true)) == JcAuthDecision::BlockedNotArmed);
    CHECK(d.submit(x, gate_context(w, JcMachineCaps(), false, true)) == JcAuthDecision::BlockedNotArmed);
    d.arm_for(w.now, kJcJogArmTtlMs, 0x03);
    CHECK(d.submit(x, gate_context(w, JcMachineCaps(), false, true)) == JcAuthDecision::BlockedEnvelopeUnread);
    CHECK(w.lines.size() == 1);   // still only the one jog from step 5
}
#endif

// The RESULT line the operator reads must tell the truth in every state, and must shout when the
// software cancel cannot be relied on.
static void test_result_line() {
    char t[96];
    {   // nothing yet
        World w;
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Muted);
        CHECK_STR(t, "no jog yet");
    }
    {   // sent, then accepted, then moving, then completed: four distinct messages
        World w;
        w.settle();
        w.move_ms = 2500;
        CHECK(w.jog(0, 1.0f, 100.0f));
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Info);
        CHECK(strstr(t, "waiting for the controller's ok") != nullptr);
        w.run(500);
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Warn);
        CHECK(strstr(t, "ACCEPTED") != nullptr && strstr(t, "not finished") != nullptr);   // ok is not completion
        w.run(4000);
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Good);
        CHECK(strstr(t, "COMPLETED (X)") != nullptr && strstr(t, "+1.000") != nullptr);
    }
    {   // rejected / alarm carry the controller's code and are urgent
        World w;
        w.settle();
        w.reply_ok = false;
        w.reply_error = 15;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(300);
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Alarm && strstr(t, "(code 15)") != nullptr);
    }
    {   // the cancel was never confirmed: this MUST be the message, not "waiting for Idle"
        World w;
        w.settle();
        w.move_ms = 100000;
        w.ignore_cancel = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        CHECK(w.session.cancel(w.now));
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Warn);   // still within the window
        CHECK(strstr(t, "waiting for the controller to report Idle") != nullptr);
        w.run(3200);
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Alarm);
        CHECK_STR(t, "CANCEL NOT CONFIRMED - USE THE E-STOP");
    }
    {   // communication lost while a jog is unresolved: say the cancel cannot be relied on
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        CHECK(jc_jog_result_line(w.session, false, t, sizeof(t)) == JcJogTone::Alarm);
        CHECK(strstr(t, "NO COMMUNICATION") != nullptr && strstr(t, "USE E-STOP") != nullptr);
        CHECK(strstr(t, "unreliable") != nullptr);
    }
    {   // no communication but nothing outstanding: no false alarm
        World w;
        CHECK(jc_jog_result_line(w.session, false, t, sizeof(t)) == JcJogTone::Muted);
    }
    {   // a mismatch names the axis and both distances
        World w;
        w.settle();
        w.wrong_position = true;
        CHECK(w.jog(1, 1.0f, 100.0f));
        w.run(2500);
        CHECK(jc_jog_result_line(w.session, true, t, sizeof(t)) == JcJogTone::Alarm);
        CHECK(strstr(t, "POSITION MISMATCH (Y)") != nullptr && strstr(t, "asked +1.000") != nullptr);
    }
    // every result has a non-empty line
    for (int r = 0; r <= static_cast<int>(JcJogResult::UnexpectedMotion); ++r) {
        CHECK(strlen(jc_jog_result_name(static_cast<JcJogResult>(r))) > 0);
    }
}

// Item 10: the cancel path in the four situations the owner listed. (Detailed variants live in
// test_cancellation / test_connection_loss / test_late_start_and_foreign_motion.)
static void test_cancel_matrix() {
    // (a) Connected and responsive: cancel reaches the controller, motion stops, state is Cancelled.
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        CHECK(w.session.phase() == JcJogPhase::Moving);
        CHECK(w.session.cancel(w.now) && w.cancels == 1);
        for (int i = 0; i < 200 && w.session.phase() != JcJogPhase::Idle; ++i) w.run(10);
        CHECK(w.session.result() == JcJogResult::Cancelled && !w.session.needs_acknowledgement());
        CHECK(w.state == "Idle" && w.lines.size() == 1);
    }
    // (b) Connection lost: nothing can be sent. The software must not claim otherwise.
    {
        World w;
        w.settle();
        w.move_ms = 3000;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        w.tx_fails = true;                            // the link is gone
        CHECK(!w.session.cancel(w.now));              // the cancel byte could not leave the device
        CHECK(w.cancels == 0 && w.session.cancels_sent() == 0);
        w.session.on_link_lost(w.now, false);
        CHECK(w.session.unresolved() && w.session.result() == JcJogResult::LinkLost);
        CHECK(w.session.phase() == JcJogPhase::Recovering);
        // ... and the controller, which never saw a cancel, finishes the move on its own.
        w.status_on = false;
        w.run(4000);
        CHECK(w.state == "Idle" && w.pos[0] > 10.9f);
    }
    // (c) The jog acknowledgement is delayed: a cancel sent first is not confused with the late ok.
    {
        World w;
        w.settle();
        w.ack_delay = 800;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(100);
        CHECK(w.session.cancel(w.now) && w.session.phase() == JcJogPhase::Cancelling);
        const uint32_t before = w.session.unsolicited();
        w.session.on_line("ok", w.now);               // the late acknowledgement
        CHECK(w.session.unsolicited() == before + 1 && w.session.phase() == JcJogPhase::Cancelling);
        for (int i = 0; i < 300 && w.session.phase() != JcJogPhase::Idle; ++i) w.run(10);
        CHECK(w.session.result() == JcJogResult::Cancelled && w.session.acked() == 0);
        CHECK(w.lines.size() == 1);
    }
    // (d) The controller never confirms Idle: reported, latched, still blocking, cancel can be repeated.
    {
        World w;
        w.settle();
        w.move_ms = 100000;
        w.ignore_cancel = true;
        CHECK(w.jog(0, 1.0f, 100.0f));
        w.run(600);
        CHECK(w.session.cancel(w.now));
        w.run(3200);
        CHECK(w.session.cancel_unconfirmed() && w.session.result() == JcJogResult::CancelUnconfirmed);
        CHECK(w.session.phase() == JcJogPhase::Cancelling && w.session.needs_acknowledgement());
        CHECK(!w.session.acknowledge());
        CHECK(w.session.can_begin(w.now) == JcJogReject::NotIdle);
        CHECK(w.session.cancel(w.now) && w.cancels == 2);
    }
}

int main() {
#if JC_MACHINE_CONTROL_ENABLED && JC_LIVE_JOG_TEST
    test_no_jog_without_read_and_confirmation();
#endif
    test_cancel_matrix();
    test_result_line();
    test_arming_scope();
    test_limits_summary();
    test_jog_gate_modes();
    test_position_trust_and_wording();
    test_gate_authorization();
    test_reconnect_clears_authorization();
    test_limits_and_builder();
    test_strict_line();
    test_first_jog();
    test_ack_is_not_completion();
    test_controller_rejection();
    test_alarm_conditions();
    test_timeouts();
    test_cancellation();
    test_late_start_and_foreign_motion();
    test_connection_loss();
    test_stale_status();
    test_touch_bounce();
    test_position_reporting();
    test_refusals_at_the_sink();
    test_authorization();
#if JC_MACHINE_CONTROL_ENABLED
    test_dispatcher_end_to_end();
    test_no_replay_after_reconnect();
#endif
    test_config_read();
    test_envelope_rules();
    printf("%d checks, %d failed (jog tests, JC_MACHINE_CONTROL_ENABLED=%d, JC_LIVE_JOG_TEST=%d)\n", g_run, g_fail,
           JC_MACHINE_CONTROL_ENABLED, JC_LIVE_JOG_TEST);
    return g_fail == 0 ? 0 : 1;
}
