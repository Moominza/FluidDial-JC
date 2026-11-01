// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host unit tests for the pure JC logic (no hardware, no network).
// Built twice by tools/run_host_tests.py: with the shipping policy, and with
// -DJC_MACHINE_CONTROL_ENABLED=1 to test authorization rules that are compiled
// out of the shipping firmware.

#include <stdio.h>
#include <string.h>

#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcCommandPolicy.hpp"
#include "../../src/jc8048w550/JcStatusFrame.hpp"
#include "jc_forged.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)
#define PARSE(str, out) jc_parse_status_frame((str), strlen(str), (out))

static void test_status_parsing() {
    JcStatusFrame f {};
    CHECK(PARSE("<Idle|MPos:0.000,0.000,0.000,0.000|FS:0,0>", f));
    CHECK_STR(f.state, "Idle");
    CHECK(f.machine_position);

    CHECK(PARSE("<Run|MPos:-12.345,100.500,-0.250,-359.990|FS:500,12000>", f));
    CHECK(f.pos[0] == -12.345f && f.pos[1] == 100.5f && f.pos[2] == -0.25f && f.pos[3] == -359.99f);
    CHECK(f.feed == 500.0f && f.spindle == 12000.0f);

    CHECK(PARSE("<Idle|WPos:1.000,-2.000,3.500,90.000|FS:0,0|WCO:10,20,0,0|Pn:P>", f));
    CHECK(!f.machine_position && f.has_wco && f.wco[1] == 20.0f);
    CHECK_STR(f.pins, "P");

    // Unknown extra fields (Ov, Bf, ...) are ignored.
    CHECK(PARSE("<Hold:0|MPos:1,2,3,4|Bf:15,128|FS:0,0|Ov:100,100,100>", f));

    // Rejections: three axes, five axes, non-numeric, NaN/inf, unknown state,
    // missing FS, missing position, unterminated, duplicate position.
    const char* bad[] = {
        "<Idle|MPos:1.0,2.0,3.0|FS:0,0>", "<Idle|MPos:1,2,3,4,5|FS:0,0>",
        "<Idle|MPos:1,2,x,4|FS:0,0>", "<Idle|MPos:nan,0,0,0|FS:0,0>",
        "<Idle|MPos:inf,0,0,0|FS:0,0>", "<Bogus|MPos:1,2,3,4|FS:0,0>",
        "<Idle|MPos:1,2,3,4>", "<Idle|FS:0,0>", "<Idle|MPos:1,2,3,4|FS:0,0",
        "<Idle|MPos:1,2,3,4|WPos:1,2,3,4|FS:0,0>", "<>", "<Idle>", "",
        "<Idle|MPos:1,2,3,4,|FS:0,0>", "<Idle|MPos:,1,2,3|FS:0,0>",
    };
    for (const char* b : bad) {
        JcStatusFrame g {};
        CHECK(!PARSE(b, g));
    }

    // Over-long frame is rejected without overflow.
    char big[300];
    memset(big, 'a', sizeof(big));
    big[0] = '<';
    big[299] = '>';
    JcStatusFrame g {};
    CHECK(!jc_parse_status_frame(big, 300, g));
    CHECK(!jc_parse_status_frame(nullptr, 10, g));
}

static void test_coordinate_conversion() {
    JcStatusFrame f {};
    CHECK(PARSE("<Idle|MPos:100,50,-10,45|FS:0,0|WCO:10,20,-5,5>", f));
    float out[4];
    CHECK(jc_convert_position(f, nullptr, true, out) && out[0] == 100.0f);  // as reported
    CHECK(jc_convert_position(f, nullptr, false, out));                     // MPos -> WPos
    CHECK(out[0] == 90.0f && out[1] == 30.0f && out[2] == -5.0f && out[3] == 40.0f);

    CHECK(PARSE("<Idle|WPos:1,2,3,4|FS:0,0>", f));
    CHECK(!jc_convert_position(f, nullptr, true, out));  // no WCO: refuse, never substitute
    const float wco[4] = { 10, 20, 30, 40 };
    CHECK(jc_convert_position(f, wco, true, out) && out[0] == 11 && out[3] == 44);
}

static void test_jog_formatting() {
    const JcMachineProfile p = jc_default_profile();
    JcCommand c = jc_jog_incremental(p, 0, 10.0f, 1000.0f);
    CHECK(c.ok);
    CHECK_STR(c.text, "$J=G91G21F1000X10.000");
    c = jc_jog_incremental(p, 1, -0.1f, 500.0f);
    CHECK_STR(c.text, "$J=G91G21F500Y-0.100");
    c = jc_jog_incremental(p, 3, -45.0f, 1800.0f);
    CHECK_STR(c.text, "$J=G91G21F1800A-45.000");  // degrees
    CHECK(!jc_jog_incremental(p, 4, 1, 100).ok);  // no 5th axis
    CHECK(jc_jog_incremental(p, 4, 1, 100).error == JcCmdError::BadAxis);
    CHECK(!jc_jog_incremental(p, 0, 0.0f, 100).ok);    // zero move
    CHECK(!jc_jog_incremental(p, 0, 101.0f, 100).ok);  // exceeds max step
    CHECK(!jc_jog_incremental(p, 0, 1.0f, 99999).ok);  // exceeds feed
    CHECK(jc_jog_incremental(p, 0, 1.0f, 0).error == JcCmdError::BadFeed);
    CHECK(!jc_jog_incremental(p, 0, NAN, 100).ok);
    CHECK(!jc_jog_incremental(p, 0, INFINITY, 100).ok);
    CHECK(!jc_jog_incremental(p, 3, 361.0f, 100).ok);  // more than one revolution
    CHECK(jc_jog_cancel().realtime && (uint8_t)jc_jog_cancel().text[0] == 0x85);
}

static void test_other_commands() {
    const JcMachineProfile p = jc_default_profile();
    CHECK_STR(jc_home(p, 0xFF).text, "$H");
    CHECK(jc_home(p, 3).error == JcCmdError::Unsupported);  // A has no assumed home switch
    JcMachineProfile withA = p;
    withA.axes[3].homing_allowed = true;
    CHECK_STR(jc_home(withA, 3).text, "$HA");
    JcMachineProfile q = p;
    q.axes[2].homing_allowed = false;
    CHECK(jc_home(q, 2).error == JcCmdError::Unsupported);
    CHECK_STR(jc_select_wcs(0).text, "G54");
    CHECK_STR(jc_select_wcs(5).text, "G59");
    CHECK(!jc_select_wcs(6).ok);
    CHECK_STR(jc_zero_axis(p, 3).text, "G10 L20 P0 A0.000");
    CHECK_STR(jc_set_axis_value(p, 0, -1.5f).text, "G10 L20 P0 X-1.500");
    CHECK(!jc_set_axis_value(p, 0, NAN).ok);
    CHECK_STR(jc_unlock().text, "$X");
    CHECK_STR(jc_spindle_stop().text, "M5");
    CHECK(jc_feed_hold().text[0] == '!');
    CHECK((uint8_t)jc_soft_reset().text[0] == 0x18);
}

static void test_probe_and_paths() {
    const JcMachineProfile p = jc_default_profile();
    CHECK_STR(jc_probe_move(p, 2, -20.0f, 100.0f).text, "G21 G91 G38.2 F100 Z-20.000");
    CHECK(jc_probe_move(p, 3, -5.0f, 50.0f).error == JcCmdError::Unsupported);  // no A probing
    CHECK(!jc_probe_move(p, 2, -500.0f, 50.0f).ok);                             // travel limit
    CHECK(!jc_probe_move(p, 2, -5.0f, 5000.0f).ok);                             // feed limit

    JcCommandSequence s = jc_probe_z_sequence(p, 25.0f, 100.0f, 5.0f, 19.05f);
    CHECK(s.ok && s.count == 4);
    CHECK_STR(s.steps[0].text, "G21 G91 G38.2 F100 Z-25.000");
    CHECK_STR(s.steps[1].text, "G10 L20 P0 Z19.050");
    CHECK_STR(s.steps[2].text, "G91 G0 Z5.000");
    CHECK_STR(s.steps[3].text, "G90");  // always restores absolute mode
    CHECK(!jc_probe_z_sequence(p, 0.0f, 100.0f, 5.0f, 1.0f).ok);

    // Instruction 010A: no builder exists for program start / resume; a forged one is refused by the one policy.
    const char* paths[] = { "/job_one.nc", "/a/b_c-1.nc", "job.nc", "/../etc", "/a;b", "/job one.nc" };
    for (const char* b : paths) {
        CHECK(!jc_command_line_allowed(JcCommandClass::Job, forged::run_sd(b).text));
        CHECK(!jc_command_line_allowed(JcCommandClass::Macro, forged::run_macro(b).text));
        CHECK(!jc_command_line_allowed(JcCommandClass::Other, forged::run_sd(b).text));
    }
}

#if JC_MACHINE_CONTROL_ENABLED
static bool g_sunk = false;
static bool sink(const char*) {
    g_sunk = true;
    return true;
}

static void test_authorization_enabled() {
    JcAuthContext c;
    c.armed = true;
    c.live = true;
    c.link_fresh = true;
    c.now_ms = 1000;
    c.command_issued_ms = 900;
    strcpy(c.machine_state, "Idle");
    CHECK(jc_authorize(JcCommandClass::Jog, c) == JcAuthDecision::Allowed);

    JcAuthContext d = c;
    d.armed = false;
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedNotArmed);
    d = c;
    d.live = false;
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedNotLive);
    d = c;
    d.link_fresh = false;
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedStaleLink);
    d = c;
    d.command_issued_ms = 100;
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedStaleCommand);
    d = c;
    strcpy(d.machine_state, "Alarm");
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedAlarm);
    CHECK(jc_authorize(JcCommandClass::Unlock, d) == JcAuthDecision::Allowed);  // recovery allowed
    CHECK(jc_authorize(JcCommandClass::Hold, d) == JcAuthDecision::Allowed);
    d = c;
    strcpy(d.machine_state, "Run");
    CHECK(jc_authorize(JcCommandClass::Home, d) == JcAuthDecision::BlockedBadState);
    CHECK(jc_authorize(JcCommandClass::Jog, d) == JcAuthDecision::BlockedBadState);
    CHECK(jc_authorize(JcCommandClass::JogCancel, d) == JcAuthDecision::Allowed);

    const JcCommandClass confirm[] = { JcCommandClass::Home, JcCommandClass::Probe, JcCommandClass::WorkOffset,
                                       JcCommandClass::Spindle, JcCommandClass::Macro,
                                       JcCommandClass::Other };
    CHECK(jc_authorize(JcCommandClass::Job, c) == JcAuthDecision::BlockedProgramStart);   // never, whatever the context
    for (JcCommandClass k : confirm) {
        CHECK(jc_authorize(k, c) == JcAuthDecision::BlockedUnconfirmed);
        d = c;
        d.operator_confirmed = true;
        CHECK(jc_authorize(k, d) == JcAuthDecision::Allowed);
    }

    // An authorized command with no sink transmits nothing.
    JcCommandGate nosink;
    nosink.submit(JcCommandClass::Jog, "$J=G91G21F100X1.000", c);
    CHECK(nosink.sent() == 0 && nosink.previewed() == 1);
    g_sunk = false;
    JcCommandGate withsink(sink);
    withsink.submit(JcCommandClass::Jog, "x", c);
    CHECK(g_sunk && withsink.sent() == 1);
}
#endif

// With the shipping build policy NOTHING is authorized, in any context.
static void test_default_build_blocks_everything() {
#if !JC_MACHINE_CONTROL_ENABLED
    JcAuthContext c;
    c.armed = true;
    c.live = true;
    c.link_fresh = true;
    c.operator_confirmed = true;
    c.now_ms = 10;
    c.command_issued_ms = 10;
    strcpy(c.machine_state, "Idle");
    JcCommandGate gate;
    int n = 0;
    for (int k = 0; k <= (int)JcCommandClass::Other; ++k, ++n) {
        CHECK(gate.submit((JcCommandClass)k, "x", c) == ((JcCommandClass)k == JcCommandClass::Job ? JcAuthDecision::BlockedProgramStart : JcAuthDecision::BlockedBuildPolicy));
    }
    CHECK(gate.sent() == 0 && gate.previewed() == 0 && gate.blocked() == (uint32_t)n);
#endif
}

// Freshness model mirrors the firmware rule: data older than the stale window
// is never presented as current, and reconnecting alone does not refresh it.
struct SimLink {
    bool connected = false;
    uint32_t last_valid_ms = 0;
    bool has = false;
    void frame(uint32_t now) { last_valid_ms = now; has = true; connected = true; }
    void drop() { connected = false; }
    bool fresh(uint32_t now) const { return connected && has && now - last_valid_ms <= 3000; }
};

static void test_stale_and_loss() {
    SimLink l;
    CHECK(!l.fresh(0));
    l.frame(1000);
    CHECK(l.fresh(2000));
    CHECK(!l.fresh(4500));
    l.frame(5000);
    l.drop();
    CHECK(!l.fresh(5100));
    l.connected = true;
    CHECK(!l.fresh(9000));
}

int main() {
    test_status_parsing();
    test_coordinate_conversion();
    test_jog_formatting();
    test_other_commands();
    test_probe_and_paths();
    test_default_build_blocks_everything();
    test_stale_and_loss();
#if JC_MACHINE_CONTROL_ENABLED
    test_authorization_enabled();
#endif
    printf("%d checks, %d failed (JC_MACHINE_CONTROL_ENABLED=%d)\n", g_run, g_fail, JC_MACHINE_CONTROL_ENABLED);
    return g_fail ? 1 : 0;
}
