// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for Instruction 005 (machine-control preparation): controller
// response classification, the read-only query allowlist, the command executor
// (acknowledgement, error, timeout, loss), the capability loader against a
// scripted fake controller, the dispatcher with a fake transport, four-axis
// validation against controller-confirmed limits, and the dry run.
//
// Built twice: with the shipping policy (machine control disabled) and with the
// authorization rules compiled in. Nothing here touches hardware or a network;
// the "controller" is a table in this file.

#include <initializer_list>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcCapsLoader.hpp"
#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcCommandExec.hpp"
#include "../../src/jc8048w550/JcCommandLink.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcDryRun.hpp"
#include "../../src/jc8048w550/JcLinkTypes.hpp"
#include "../../src/jc8048w550/JcMachineCaps.hpp"
#include "../../src/jc8048w550/JcQuery.hpp"
#include "../../src/jc8048w550/JcResponse.hpp"
#include "../../src/jc8048w550/JcSettings.hpp"
#include "../../src/jc8048w550/JcStreamSplitter.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

// ---------------------------------------------------------------- responses

static void test_classify() {
    JcResponse r = jc_classify_line("ok");
    CHECK(r.kind == JcRespKind::Ok);
    r = jc_classify_line("  ok \r");
    CHECK(r.kind == JcRespKind::Ok);
    CHECK(jc_classify_line("").kind == JcRespKind::Empty);
    CHECK(jc_classify_line("   ").kind == JcRespKind::Empty);
    CHECK(jc_classify_line(nullptr).kind == JcRespKind::Empty);
    CHECK(jc_classify_line("okay").kind == JcRespKind::Other);  // not an acknowledgement

    r = jc_classify_line("error:3");
    CHECK(r.kind == JcRespKind::Error && r.code == 3);
    r = jc_classify_line("error:20");
    CHECK(r.kind == JcRespKind::Error && r.code == 20);
    r = jc_classify_line("error:");
    CHECK(r.kind == JcRespKind::Error && r.code == 0);
    r = jc_classify_line("error:abc");
    CHECK(r.kind == JcRespKind::Error && r.code == 0);
    r = jc_classify_line("error:99999");
    CHECK(r.kind == JcRespKind::Error && r.code == 0);  // implausible code is not trusted
    r = jc_classify_line("ALARM:1");
    CHECK(r.kind == JcRespKind::Alarm && r.code == 1);

    r = jc_classify_line("[MSG:INFO: Homing done]");
    CHECK(r.kind == JcRespKind::Message);
    CHECK_STR(r.value, "INFO: Homing done");
    r = jc_classify_line("[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]");
    CHECK(r.kind == JcRespKind::GCodeState);
    CHECK_STR(r.value, "G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0");
    r = jc_classify_line("[VER:4.1.1 FluidNC v4.1.1:]");
    CHECK(r.kind == JcRespKind::Info);
    CHECK_STR(r.key, "VER");
    CHECK_STR(r.value, "4.1.1 FluidNC v4.1.1:");

    r = jc_classify_line("$/axes/x/steps_per_mm=800.000");
    CHECK(r.kind == JcRespKind::Setting);
    CHECK_STR(r.key, "/axes/x/steps_per_mm");
    CHECK_STR(r.value, "800.000");
    r = jc_classify_line("$/axes/x/homing/positive_direction=false");
    CHECK(r.kind == JcRespKind::Setting);
    CHECK_STR(r.key, "/axes/x/homing/positive_direction");
    CHECK_STR(r.value, "false");
    r = jc_classify_line("$/axes/x/steps_per_mm=");
    CHECK(r.kind == JcRespKind::Setting && r.value[0] == '\0');
    CHECK(jc_classify_line("$=1").kind == JcRespKind::Other);  // no key
    CHECK(jc_classify_line("Grbl 3.0 ['$' for help]").kind == JcRespKind::Banner);
    CHECK(jc_classify_line("FluidNC v4.1.1").kind == JcRespKind::Banner);
    CHECK(jc_classify_line("garbage!!").kind == JcRespKind::Other);

    // Very long text is truncated, never overflowed.
    std::string big = "$/axes/x/" + std::string(200, 'k') + "=" + std::string(200, 'v');
    r = jc_classify_line(big.c_str());
    CHECK(r.kind == JcRespKind::Setting && strlen(r.key) < sizeof(r.key) && strlen(r.value) < sizeof(r.value));

    CHECK_STR(jc_error_text(3), "invalid $ statement");
    CHECK_STR(jc_error_text(999), "unlisted error number");
    CHECK_STR(jc_error_text(0), "no code given");
}

// ---------------------------------------------------------------- splitter

struct SplitResult {
    int frames = 0, invalid = 0, overflow = 0;
    std::vector<std::string> lines;
};

static SplitResult run_split(const std::string& stream, size_t chunk) {
    JcStreamSplitter s;
    SplitResult r;
    for (size_t i = 0; i < stream.size(); i += chunk) {
        for (size_t j = i; j < stream.size() && j < i + chunk; ++j) {
            switch (s.push(static_cast<uint8_t>(stream[j]))) {
                case JcStreamSplitter::Event::None: break;
                case JcStreamSplitter::Event::Frame: ++r.frames; break;
                case JcStreamSplitter::Event::InvalidFrame: ++r.invalid; break;
                case JcStreamSplitter::Event::Line: r.lines.push_back(s.line()); break;
                case JcStreamSplitter::Event::LineOverflow: ++r.overflow; break;
            }
        }
    }
    return r;
}

static void test_splitter() {
    const std::string mixed =
        "<Idle|MPos:1.000,2.000,3.000,4.000|FS:0,0>\r\n"
        "$/axes/x/steps_per_mm=800.000\r\n"
        "ok\r\n"
        "<Run|MPos:-5.5,6.5,7.5,-8.5|FS:100,0>\r\n"
        "[MSG:INFO: a<b]\r\n"
        "error:3\r\n";
    for (size_t chunk : { (size_t)1, (size_t)2, (size_t)3, (size_t)5, (size_t)64 }) {
        const SplitResult r = run_split(mixed, chunk);
        CHECK(r.frames == 2 && r.invalid == 0 && r.overflow == 0);
        CHECK(r.lines.size() == 4);
        if (r.lines.size() == 4) {
            CHECK(r.lines[0] == "$/axes/x/steps_per_mm=800.000");
            CHECK(r.lines[1] == "ok");
            CHECK(r.lines[2] == "[MSG:INFO: a<b]");  // '<' inside a line is not a frame start
            CHECK(r.lines[3] == "error:3");
        }
    }
    // Telnet negotiation / control bytes are dropped, not mistaken for text.
    {
        std::string s = "\xFF\xFB\x01ok\r\n";
        JcStreamSplitter sp;
        std::vector<std::string> lines;
        for (char c : s) {
            if (sp.push(static_cast<uint8_t>(c)) == JcStreamSplitter::Event::Line) lines.push_back(sp.line());
        }
        CHECK(lines.size() == 1 && lines[0] == "ok" && sp.dropped_bytes() == 3);
    }
    // An over-long line is discarded as a whole; the next line is intact.
    {
        const SplitResult r = run_split(std::string(500, 'x') + "\nok\n", 7);
        CHECK(r.overflow == 1 && r.lines.size() == 1 && r.lines[0] == "ok");
    }
    // A corrupt frame is reported and the stream recovers.
    {
        const SplitResult r = run_split("<Idle|MPos:1,2\r\nok\r\n<Idle|MPos:1,2,3,4|FS:0,0>\r\n", 4);
        CHECK(r.invalid == 1 && r.frames == 1 && r.lines.size() == 1 && r.lines[0] == "ok");
    }
    // Blank lines produce no events.
    CHECK(run_split("\r\n\r\n\n", 1).lines.empty());
}

// ---------------------------------------------------------------- allowlist

static void test_query_allowlist() {
    CHECK(jc_query_allowed("$I"));
    CHECK(jc_query_allowed("$G"));
    CHECK(jc_query_allowed("$/probe/pin"));
    CHECK(jc_query_allowed("$/probe/toolsetter_pin"));
    int axis_lines = 0;
    for (uint8_t a = 0; a < 4; ++a) {
        for (uint8_t f = 0; f < kJcQueryFieldCount; ++f) {
            char line[72];
            CHECK(jc_query_axis_line(line, sizeof(line), a, static_cast<JcQueryField>(f)));
            CHECK(jc_query_allowed(line));
            CHECK(line[0] == '$' && strchr(line, '=') == nullptr);
            ++axis_lines;
        }
    }
    CHECK(axis_lines == 24);

    const char* blocked[] = {
        "", "?", "$", "$H", "$HX", "$X", "$J=G91G21F100X1", "$SD/Run=/a.nc", "$Localfs/Run=/m.nc",
        "G0 X1", "G10 L20 P0 X0", "G54", "M5", "!", "~",
        "$/axes/x/steps_per_mm=1", "$/axes/x/max_travel_mm=99999", "$/axes/x/steps_per_mm ",
        "$/axes/x/steps_per_mm\n$H", "$/axes/x/steps_per_mm$H", "$/axes/b/steps_per_mm",
        "$/axes/xx/steps_per_mm", "$/axes/x/homing/cycle=0", "$/axes/x", "$/axes", "$/axes/x/",
        "$Config/Dump", "$Limits", "$#", "$$", "$RST=*", "$/probe/pin=gpio.1", "$/start_message=x",
        "$/axes/x/steps_per_mm;$H", " $I", "$I ", "$i", "$g",
    };
    for (const char* b : blocked) {
        CHECK(!jc_query_allowed(b));
    }
    CHECK(!jc_query_allowed(nullptr));
    CHECK(!jc_query_allowed(std::string(200, '$').c_str()));

    // No command the builders can generate is permitted as a "query".
    const JcMachineProfile p = jc_default_profile();
    std::vector<JcCommand> cmds = {
        jc_jog_incremental(p, 0, 1.0f, 100.0f), jc_jog_continuous(p, 0, 1, 100.0f), jc_jog_cancel(),
        jc_feed_hold(), jc_cycle_resume(), jc_soft_reset(), jc_unlock(), jc_home(p, 0xFF), jc_home(p, 0),
        jc_select_wcs(0), jc_set_axis_value(p, 0, 1.0f), jc_zero_axis(p, 1), jc_probe_move(p, 2, -5.0f, 50.0f),
        jc_run_sd_file("/a.nc"), jc_run_macro_file("/m.nc"), jc_spindle_stop(),
    };
    for (const JcCommand& c : cmds) {
        CHECK(c.ok);
        CHECK(!jc_query_allowed(c.text));
    }
    const JcCommandSequence seq = jc_probe_z_sequence(p, 20, 50, 5, 0);
    for (uint8_t i = 0; i < seq.count; ++i) CHECK(!jc_query_allowed(seq.steps[i].text));
    // And the original rule still holds: only '?' is a permitted single byte.
    for (int b = 0; b < 256; ++b) CHECK(jc_tx_byte_allowed(static_cast<uint8_t>(b)) == (b == '?'));
}

// ---------------------------------------------------------------- executor

static void test_exec() {
    JcResponse ok = jc_classify_line("ok");

    // Acknowledgement.
    {
        JcCommandExec e;
        CHECK(e.ready(0));
        CHECK(e.begin("$X", 2000, 100));
        CHECK(e.awaiting() && !e.ready(100));
        CHECK(!e.begin("G54", 2000, 110));  // one request at a time
        CHECK(e.on_response(jc_classify_line("[MSG:Caution: Unlocked]"), 120) == false);
        CHECK(e.lines() == 1 && e.awaiting());
        CHECK(e.on_response(ok, 150));
        CHECK(e.state() == JcExecState::Finished && e.outcome() == JcExecOutcome::Acked);
        CHECK(e.elapsed_ms() == 50);
        CHECK(!e.begin("G54", 2000, 160));  // result must be released first
        e.release();
        CHECK(e.begin("G54", 2000, 160));
        CHECK(e.acked() == 1 && e.begun() == 2);
    }
    // Controller error carries its number.
    {
        JcCommandExec e;
        e.begin("$J=G91G21F100X999", 2000, 0);
        CHECK(e.on_response(jc_classify_line("error:15"), 30));
        CHECK(e.outcome() == JcExecOutcome::ControllerError && e.code() == 15 && e.errors() == 1);
        CHECK_STR(jc_error_text(e.code()), "jog target exceeds travel");
    }
    // ALARM line ends the request.
    {
        JcCommandExec e;
        e.begin("G38.2 Z-10 F50", 60000, 0);
        CHECK(e.on_response(jc_classify_line("ALARM:4"), 500));
        CHECK(e.outcome() == JcExecOutcome::Alarm && e.code() == 4 && e.alarms() == 1);
    }
    // Machine state Alarm: only aborts requests that asked for it.
    {
        JcCommandExec e;
        e.begin("$J=G91G21F100X1", 2000, 0, true);
        e.on_machine_state("Run", 10);
        CHECK(e.awaiting());
        e.on_machine_state("Alarm", 20);
        CHECK(e.outcome() == JcExecOutcome::Alarm);
        JcCommandExec q;
        q.begin("$G", 2000, 0, false);  // queries are legitimate while in Alarm
        q.on_machine_state("Alarm", 20);
        CHECK(q.awaiting());
    }
    // Timeout, never resent, late reply is not mistaken for the next request's.
    {
        JcCommandExec e;
        e.begin("$H", 120000, 1000);
        e.tick(100000);
        CHECK(e.awaiting());
        e.tick(121000);
        CHECK(e.outcome() == JcExecOutcome::TimedOut && e.timeouts() == 1);
        e.release();
        CHECK(!e.ready(121500));                // resync quiet period (1000 ms)
        CHECK(!e.begin("G54", 2000, 121500));
        // A late "ok" for the timed-out command arrives: nothing is awaiting.
        CHECK(e.on_response(ok, 121600) == false);
        CHECK(e.unsolicited() == 1);
        CHECK(e.ready(122100));
        CHECK(e.begin("G54", 2000, 122100));
    }
    // Connection loss: ends the request, no resend, new begin is explicit.
    {
        JcCommandExec e;
        e.begin("$J=G91G21F100X1", 2000, 0);
        e.on_link_lost(300);
        CHECK(e.outcome() == JcExecOutcome::LinkLost && e.losses() == 1);
        e.release();
        CHECK(e.state() == JcExecState::Idle && e.ready(300));  // fresh connection: no quiet period
        CHECK(!e.on_response(ok, 310));                         // nothing to complete
    }
    // Real-time bytes have no reply.
    {
        JcCommandExec e;
        CHECK(e.begin_realtime("<real-time>", 5));
        CHECK(e.outcome() == JcExecOutcome::SentNoAck && e.state() == JcExecState::Finished);
    }
    // Abandon behaves like a timeout for resync purposes.
    {
        JcCommandExec e;
        e.begin("$G", 2000, 0);
        e.abandon(50);
        e.release();
        CHECK(!e.ready(100) && e.ready(1100));
    }
    // Timeouts per class: long for motion that replies on completion, short otherwise.
    CHECK(jc_command_timeout_ms(JcCommandClass::Home) > jc_command_timeout_ms(JcCommandClass::Jog));
    CHECK(jc_command_timeout_ms(JcCommandClass::Probe) > jc_command_timeout_ms(JcCommandClass::Jog));
    for (int k = 0; k <= (int)JcCommandClass::Other; ++k) CHECK(jc_command_timeout_ms((JcCommandClass)k) >= 1000);
    CHECK(jc_is_realtime_text("!") && jc_is_realtime_text("~") && jc_is_realtime_text("\x18") && jc_is_realtime_text("\x85"));
    CHECK(!jc_is_realtime_text("$X") && !jc_is_realtime_text("") && !jc_is_realtime_text("?"));
}

// ---------------------------------------------------------------- fake controller

enum class Mode { Normal, ErrorOnA, BadValues, SilentFrom, NoValueLines, AlarmDuring };

struct FakeController {
    Mode mode = Mode::Normal;
    int silent_from = 9999;   // query index from which the controller stops answering
    int alarm_at = 9999;
    int received = 0;
    std::vector<std::string> log;

    static const char* val(int axis, int field) {
        static const char* table[4][6] = {
            { "800.000", "300.000", "5000.000", "200.000", "2", "false" },
            { "800.000", "250.000", "5000.000", "200.000", "3", "false" },
            { "800.000", "100.000", "2000.000", "100.000", "1", "true" },
            { "17.778", "360.000", "3600.000", "50.000", "0", "false" },
        };
        return table[axis][field];
    }

    // Returns the reply text for one query line ("" = no reply at all).
    std::string answer(const char* line) {
        log.push_back(line);
        const int index = received++;
        if (index >= silent_from) return "";
        if (index == alarm_at) return "ALARM:1\r\n";
        if (strcmp(line, "$I") == 0) return "[VER:4.1 FluidNC v4.1.1 (esp32s3-wifi) :]\r\n[OPT:PH]\r\nok\r\n";  // as sent by the real v4.1.1
        if (strcmp(line, "$G") == 0) return "[GC:G0 G55 G17 G21 G90 G94 M5 M9 T2 F0 S0]\r\nok\r\n";
        if (strcmp(line, "$/probe/pin") == 0) return "$/probe/pin=gpio.36:low\r\nok\r\n";
        if (strcmp(line, "$/probe/toolsetter_pin") == 0) return "$/probe/toolsetter_pin=NO_PIN\r\nok\r\n";
        for (int a = 0; a < 4; ++a) {
            for (int f = 0; f < kJcQueryFieldCount; ++f) {
                char q[72];
                jc_query_axis_line(q, sizeof(q), a, static_cast<JcQueryField>(f));
                if (strcmp(q, line) != 0) continue;
                if (mode == Mode::ErrorOnA && a == 3) return "error:3\r\n";
                if (mode == Mode::NoValueLines && a == 1 && f == 0) return "ok\r\n";
                std::string v = val(a, f);
                if (mode == Mode::BadValues) {
                    if (a == 0 && f == 0) v = "nan";
                    if (a == 1 && f == 1) v = "-5";
                    if (a == 2 && f == 2) v = "abc";
                    if (a == 2 && f == 3) v = "0";
                    if (a == 0 && f == 5) v = "maybe";
                    if (a == 1 && f == 4) v = "9";
                }
                return std::string(q + 1 - 1) + "=" + v + "\r\nok\r\n";
            }
        }
        return "error:3\r\n";
    }
};

// Drives the loader the way JcApp::service() does, with a virtual clock.
static uint32_t drive(JcCapsLoader& loader, FakeController& ctl, uint32_t now, int iterations,
                      std::vector<std::string>* sent = nullptr, bool deliver = true) {
    for (int i = 0; i < iterations && loader.running(); ++i) {
        loader.tick(now);
        if (const char* q = loader.pending_line(now)) {
            if (sent) sent->push_back(q);
            const std::string reply = ctl.answer(q);
            loader.mark_sent(now);
            if (deliver) {
                JcStreamSplitter sp;
                for (char c : reply) {
                    if (sp.push(static_cast<uint8_t>(c)) == JcStreamSplitter::Event::Line) {
                        loader.on_line(sp.line(), now + 15);
                    }
                }
            }
        }
        now += 20;
    }
    return now;
}

static void test_caps_loader() {
    // Happy path.
    {
        FakeController ctl;
        JcCapsLoader loader;
        CHECK(loader.state() == JcLoadState::Idle);
        CHECK(loader.pending_line(0) == nullptr);  // never starts by itself
        CHECK(loader.start(1000, nullptr));
        std::vector<std::string> sent;
        drive(loader, ctl, 1000, 200, &sent);
        CHECK(loader.state() == JcLoadState::Complete);
        CHECK(sent.size() == JcCapsLoader::kSteps && JcCapsLoader::kSteps == 28);
        for (const std::string& q : sent) CHECK(jc_query_allowed(q.c_str()));  // only allowlisted lines
        CHECK(loader.take_finished() && !loader.take_finished());
        const JcMachineCaps& c = loader.caps();
        CHECK(c.complete && c.queries_ok == 28 && c.queries_error == 0);
        CHECK_STR(c.firmware, "4.1 FluidNC v4.1.1 (esp32s3-wifi)");  // trailing " :" removed
        CHECK_STR(c.wcs, "G55");
        CHECK(c.tool == 2);
        CHECK_STR(c.probe_pin, "gpio.36:low");
        CHECK(c.confirmed_axis_mask() == 0x0F);
        CHECK(c.axes[0].get(JcQueryField::StepsPerMm) == 800.0f);
        CHECK(c.axes[2].get(JcQueryField::MaxTravelMm) == 100.0f);
        CHECK(c.axes[3].get(JcQueryField::MaxRate) == 3600.0f);
        CHECK(c.axes[3].get(JcQueryField::HomingCycle) == 0.0f);
        CHECK(c.axes[2].get(JcQueryField::HomingPositiveDir) == 1.0f);
        CHECK(c.read_ms > 0);
    }
    // The controller refuses A: the rest is kept, A stays unconfirmed, nothing is invented.
    {
        FakeController ctl;
        ctl.mode = Mode::ErrorOnA;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 200);
        CHECK(loader.state() == JcLoadState::Complete);
        const JcMachineCaps& c = loader.caps();
        CHECK(c.queries_error == 6);
        CHECK(c.confirmed_axis_mask() == 0x07);
        CHECK(c.axes[3].state[0] == JcFieldState::CtrlError && c.axes[3].error_code[0] == 3);
        CHECK(!c.axes[3].any_valid());
        JcMachineProfile p = jc_default_profile();
        jc_profile_apply_caps(p, c);
        CHECK(p.axes[0].limits_confirmed && p.axes[2].limits_confirmed && !p.axes[3].limits_confirmed);
        CHECK(p.axes[3].max_feed == 3600.0f && p.axes[3].max_step == 360.0f);  // placeholder untouched
        CHECK(jc_profile_limits_mask(p) == 0x07);
    }
    // Implausible values are rejected, never clamped.
    {
        FakeController ctl;
        ctl.mode = Mode::BadValues;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 200);
        CHECK(loader.state() == JcLoadState::Complete);
        const JcMachineCaps& c = loader.caps();
        CHECK(c.axes[0].state[0] == JcFieldState::BadValue);   // nan
        CHECK(c.axes[1].state[1] == JcFieldState::BadValue);   // negative travel
        CHECK(c.axes[2].state[2] == JcFieldState::BadValue);   // text
        CHECK(c.axes[2].state[3] == JcFieldState::BadValue);   // zero acceleration
        CHECK(c.axes[0].state[5] == JcFieldState::BadValue);   // not true/false
        CHECK(c.axes[1].state[4] == JcFieldState::BadValue);   // homing cycle 9
        CHECK((c.confirmed_axis_mask() & 0x07) == 0);          // X, Y, Z each have a bad limit
        CHECK((c.confirmed_axis_mask() & 0x08) != 0);          // A unaffected
        JcMachineProfile p = jc_default_profile();
        jc_profile_apply_caps(p, c);
        CHECK(!p.axes[0].limits_confirmed && p.axes[0].max_feed == 3000.0f);
    }
    // "ok" with no value is recorded as such.
    {
        FakeController ctl;
        ctl.mode = Mode::NoValueLines;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 200);
        CHECK(loader.caps().axes[1].state[0] == JcFieldState::NoValue);
        CHECK(!loader.caps().axes[1].limits_confirmed());
    }
    // A silent controller: the read times out and fails; nothing is resent.
    {
        FakeController ctl;
        ctl.silent_from = 5;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        std::vector<std::string> sent;
        uint32_t now = drive(loader, ctl, 0, 1000, &sent);
        CHECK(loader.state() == JcLoadState::Failed && loader.failure() == JcLoadFailure::TimedOut);
        CHECK(sent.size() == 6);  // 5 answered + the one that timed out; no retries
        CHECK(!loader.caps().complete);
        CHECK(loader.exec().timeouts() == 1);
        CHECK(now >= kJcQueryTimeoutMs);
        CHECK(loader.take_finished());
    }
    // Connection lost mid-read.
    {
        FakeController ctl;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 3, nullptr, false);   // a query is in flight, unanswered
        CHECK(loader.running());
        loader.on_link_lost(500);
        CHECK(loader.state() == JcLoadState::Failed && loader.failure() == JcLoadFailure::LinkLost);
        CHECK(loader.pending_line(600) == nullptr);   // does not resume by itself
        CHECK(loader.exec().losses() == 1);
        // Reconnect later: only an explicit start() begins again, from the first query.
        std::vector<std::string> sent;
        FakeController ctl2;
        loader.start(10000, nullptr);
        drive(loader, ctl2, 10000, 200, &sent);
        CHECK(loader.state() == JcLoadState::Complete);
        CHECK(sent.size() == 28 && sent[0] == "$I");
    }
    // A controller alarm during the read fails it.
    {
        FakeController ctl;
        ctl.alarm_at = 4;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 200);
        CHECK(loader.state() == JcLoadState::Failed && loader.failure() == JcLoadFailure::Alarm);
    }
    // Cancel; a late reply is ignored and the next read waits out the resync period.
    {
        FakeController ctl;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 6, nullptr, false);  // sent a query, never delivered its reply
        CHECK(loader.running());
        loader.cancel(400);
        CHECK(loader.state() == JcLoadState::Failed && loader.failure() == JcLoadFailure::Cancelled);
        loader.on_line("ok", 450);                 // the late reply: ignored
        CHECK(loader.caps().queries_ok == 0);
        loader.start(500, nullptr);
        CHECK(loader.pending_line(500) == nullptr);   // inside the resync quiet period
        CHECK(loader.pending_line(1500) != nullptr);
    }
    // Not ready: the loader refuses to run and sends nothing.
    {
        JcCapsLoader loader;
        CHECK(!loader.start(0, "no fresh controller status"));
        CHECK(loader.state() == JcLoadState::Failed && loader.failure() == JcLoadFailure::NotReady);
        CHECK(loader.pending_line(0) == nullptr);
    }
    // Send failure fails the read.
    {
        JcCapsLoader loader;
        loader.start(0, nullptr);
        CHECK(loader.pending_line(0) != nullptr);
        loader.send_failed(10);
        CHECK(loader.failure() == JcLoadFailure::SendFailed);
    }
    // Lines that do not belong to the awaited query do not populate fields.
    {
        JcCapsLoader loader;
        loader.start(0, nullptr);
        loader.on_line("$/axes/x/steps_per_mm=800.000", 5);    // nothing sent yet: ignored
        CHECK(loader.caps().axes[0].state[0] == JcFieldState::Unqueried);
        const char* q = loader.pending_line(10);
        CHECK(q && strcmp(q, "$I") == 0);
        loader.mark_sent(10);
        loader.on_line("$/axes/x/steps_per_mm=800.000", 15);   // wrong echo for "$I"
        CHECK(loader.caps().axes[0].state[0] == JcFieldState::Unqueried);
    }
    // Connection loss after a good read discards the values: placeholders return.
    {
        FakeController ctl;
        JcCapsLoader loader;
        loader.start(0, nullptr);
        drive(loader, ctl, 0, 200);
        CHECK(loader.state() == JcLoadState::Complete);
        JcMachineProfile p = jc_default_profile();
        jc_profile_apply_caps(p, loader.caps());
        CHECK(jc_profile_limits_mask(p) == 0x0F);
        loader.on_link_lost(1000);            // no read in progress: no effect on the result itself
        loader.invalidate(1000);
        CHECK(loader.state() == JcLoadState::Idle && !loader.caps().any_data() && !loader.take_finished());
        JcMachineProfile q = jc_default_profile();
        if (loader.state() == JcLoadState::Complete) jc_profile_apply_caps(q, loader.caps());
        CHECK(jc_profile_limits_mask(q) == 0 && q.axes[0].max_feed == 3000.0f);
        CHECK(loader.pending_line(1000) == nullptr);   // and nothing restarts by itself
    }
    // Read preconditions.
    CHECK(jc_caps_read_blocker(true, true, "Idle", false) == nullptr);
    CHECK(jc_caps_read_blocker(true, true, "Alarm", false) == nullptr);
    CHECK(jc_caps_read_blocker(false, true, "Idle", false) != nullptr);
    CHECK(jc_caps_read_blocker(true, false, "Idle", false) != nullptr);
    CHECK(jc_caps_read_blocker(true, true, "Run", false) != nullptr);
    CHECK(jc_caps_read_blocker(true, true, "Jog", false) != nullptr);
    CHECK(jc_caps_read_blocker(true, true, "Idle", true) != nullptr);
    CHECK(jc_caps_read_blocker(true, true, nullptr, false) != nullptr);
}

// Profile built from a complete, healthy read.
static JcMachineProfile confirmed_profile(bool a_has_home = false) {
    FakeController ctl;
    JcCapsLoader loader;
    loader.start(0, nullptr);
    drive(loader, ctl, 0, 200);
    JcUserSettings s = jc_settings_defaults();
    s.a_axis_has_homing = a_has_home;
    JcMachineProfile p = jc_profile_from_settings(s);
    jc_profile_apply_caps(p, loader.caps());
    return p;
}

static void test_four_axis_validation() {
    const JcMachineProfile def = jc_default_profile();
    // Placeholders before any read.
    for (uint8_t a = 0; a < 4; ++a) CHECK(!def.axes[a].limits_confirmed && def.axes[a].travel_mm == 0.0f);

    const JcMachineProfile p = confirmed_profile();
    CHECK(jc_profile_limits_mask(p) == 0x0F);
    // X: travel 300, rate 5000.
    CHECK(jc_jog_incremental(p, 0, 300.0f, 4000.0f).ok);
    CHECK(!jc_jog_incremental(p, 0, 301.0f, 4000.0f).ok);
    CHECK(jc_jog_incremental(p, 0, 1.0f, 5000.0f).ok);
    CHECK(jc_jog_incremental(p, 0, 1.0f, 5001.0f).error == JcCmdError::BadFeed);
    // Y: travel 250.
    CHECK(jc_jog_incremental(p, 1, -250.0f, 100.0f).ok && !jc_jog_incremental(p, 1, -251.0f, 100.0f).ok);
    // Z: travel 100, rate 2000.
    CHECK(jc_jog_incremental(p, 2, 100.0f, 2000.0f).ok);
    CHECK(!jc_jog_incremental(p, 2, 101.0f, 100.0f).ok);
    CHECK(jc_jog_incremental(p, 2, 1.0f, 2001.0f).error == JcCmdError::BadFeed);
    // A: rotary, travel 360 deg, rate 3600 deg/min.
    CHECK(jc_jog_incremental(p, 3, 360.0f, 3600.0f).ok && !jc_jog_incremental(p, 3, 361.0f, 100.0f).ok);
    CHECK(jc_jog_incremental(p, 3, 45.0f, 3601.0f).error == JcCmdError::BadFeed);
    // Common rejections apply to every axis.
    for (uint8_t a = 0; a < 4; ++a) {
        CHECK(jc_jog_incremental(p, a, 0.0f, 100.0f).error == JcCmdError::BadValue);
        CHECK(jc_jog_incremental(p, a, NAN, 100.0f).error == JcCmdError::BadValue);
        CHECK(jc_jog_incremental(p, a, INFINITY, 100.0f).error == JcCmdError::BadValue);
        CHECK(jc_jog_incremental(p, a, 1.0f, 0.0f).error == JcCmdError::BadFeed);
        CHECK(jc_jog_incremental(p, a, 1.0f, NAN).error == JcCmdError::BadFeed);
        const JcCommand c = jc_jog_incremental(p, a, 1.0f, 100.0f);
        CHECK(c.ok && c.axis == a && c.cls == JcCommandClass::Jog);
        CHECK(c.text[strlen("$J=G91G21F100")] == "XYZA"[a]);
    }
    CHECK(jc_jog_incremental(p, 4, 1.0f, 100.0f).error == JcCmdError::BadAxis);   // there is no fifth axis
    CHECK(jc_jog_incremental(p, 255, 1.0f, 100.0f).error == JcCmdError::BadAxis);
    // Continuous jog uses the controller-reported travel; never on the rotary axis.
    JcCommand cj = jc_jog_continuous(p, 0, 1, 1000.0f);
    CHECK(cj.ok && strstr(cj.text, "X300") != nullptr);
    cj = jc_jog_continuous(p, 2, -1, 1000.0f);
    CHECK(cj.ok && strstr(cj.text, "Z-100") != nullptr);
    CHECK(jc_jog_continuous(p, 3, 1, 1000.0f).error == JcCmdError::Unsupported);
    CHECK(strstr(jc_jog_continuous(def, 0, 1, 1000.0f).text, "X1000") != nullptr);  // placeholder rule
    // Probing: linear axes only; A never.
    CHECK(jc_probe_move(p, 2, -20.0f, 100.0f).ok && jc_probe_move(p, 2, -20.0f, 100.0f).axis == 2);
    CHECK(jc_probe_move(p, 3, -20.0f, 100.0f).error == JcCmdError::Unsupported);
    // Homing follows the controller's homing/cycle: X=2, Y=3, Z=1 homed, A=0 not.
    CHECK(jc_home(p, 0).ok && jc_home(p, 1).ok && jc_home(p, 2).ok);
    CHECK(jc_home(p, 3).error == JcCmdError::Unsupported);
    // A declared as homed by the operator is still refused when the controller says cycle 0 ...
    CHECK(confirmed_profile(true).axes[3].homing_allowed == false);
    // ... and a controller-homed A is still refused when the operator says it has no switch.
    {
        JcMachineProfile q = jc_default_profile();
        JcMachineCaps caps;
        caps.axes[3].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] = JcFieldState::Valid;
        caps.axes[3].value[static_cast<uint8_t>(JcQueryField::HomingCycle)] = 4.0f;
        q.axes[3].homing_allowed = false;
        jc_profile_apply_caps(q, caps);
        CHECK(!q.axes[3].homing_allowed);
        q.axes[3].homing_allowed = true;
        jc_profile_apply_caps(q, caps);
        CHECK(q.axes[3].homing_allowed);
    }
    // An axis the controller says is not homed (cycle 0) loses homing permission.
    {
        JcMachineProfile q = jc_default_profile();
        JcMachineCaps caps;
        caps.axes[0].state[static_cast<uint8_t>(JcQueryField::HomingCycle)] = JcFieldState::Valid;
        caps.axes[0].value[static_cast<uint8_t>(JcQueryField::HomingCycle)] = 0.0f;
        jc_profile_apply_caps(q, caps);
        CHECK(!q.axes[0].homing_allowed && q.axes[1].homing_allowed);
        CHECK(jc_home(q, 0).error == JcCmdError::Unsupported);
    }
    // Plausibility rules for individual fields.
    float v = 0;
    CHECK(jc_caps_field_valid(JcQueryField::StepsPerMm, "80", v) && v == 80.0f);
    CHECK(!jc_caps_field_valid(JcQueryField::StepsPerMm, "0", v));
    CHECK(!jc_caps_field_valid(JcQueryField::StepsPerMm, "1e9", v));
    CHECK(!jc_caps_field_valid(JcQueryField::MaxRate, "-1", v));
    CHECK(!jc_caps_field_valid(JcQueryField::MaxRate, "200001", v));
    CHECK(!jc_caps_field_valid(JcQueryField::MaxTravelMm, "12abc", v));
    CHECK(!jc_caps_field_valid(JcQueryField::MaxTravelMm, "", v));
    CHECK(!jc_caps_field_valid(JcQueryField::MaxTravelMm, "inf", v));
    CHECK(jc_caps_field_valid(JcQueryField::HomingCycle, "0", v) && v == 0.0f);
    CHECK(!jc_caps_field_valid(JcQueryField::HomingCycle, "1.5", v));
    CHECK(jc_caps_field_valid(JcQueryField::HomingPositiveDir, "true", v) && v == 1.0f);
    CHECK(!jc_caps_field_valid(JcQueryField::HomingPositiveDir, "1", v));
}

// ---------------------------------------------------------------- dispatcher + fake transport

struct Wire {
    std::vector<std::string> sent;
    bool fail = false;
    static bool send(void* self, const char* bytes, size_t n) {
        Wire* w = static_cast<Wire*>(self);
        if (w->fail) return false;
        w->sent.emplace_back(bytes, n);
        return true;
    }
};

static JcDispatchContext good_ctx(uint32_t now, const char* state = "Idle", uint8_t mask = 0x0F) {
    JcDispatchContext c;
    c.live = true;
    c.link_fresh = true;
    c.machine_state = state;
    c.operator_confirmed = true;
    c.now_ms = now;
    c.limits_mask = mask;
    return c;
}

static void test_dispatcher_transport() {
    const JcMachineProfile p = confirmed_profile();

    // Same wiring in both builds: with the shipping policy NOTHING reaches the wire,
    // even though a sink and a transport are installed and the operator "armed".
    {
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(1000);
        const JcAuthDecision r = d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(1000));
#if !JC_MACHINE_CONTROL_ENABLED
        CHECK(r == JcAuthDecision::BlockedBuildPolicy);
        CHECK(wire.sent.empty() && link.transmitted() == 0 && d.sent() == 0);
        const JcAuthDecision s = d.submit_sequence(jc_probe_z_sequence(p, 20, 50, 5, 0), good_ctx(1000));
        CHECK(s == JcAuthDecision::BlockedBuildPolicy && wire.sent.empty());
        for (int k = 0; k <= (int)JcCommandClass::Other; ++k) {
            JcCommand c;
            c.ok = true;
            c.cls = (JcCommandClass)k;
            strcpy(c.text, "X");
            CHECK(d.submit(c, good_ctx(1000)) == JcAuthDecision::BlockedBuildPolicy);
        }
        CHECK(wire.sent.empty());
#else
        CHECK(r == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 1);
#endif
    }

#if JC_MACHINE_CONTROL_ENABLED
    // ---- acknowledgement, error, timeout, loss, replay (authorization compiled in) ----
    {   // acknowledgement
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(1000);
        CHECK(d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(1000)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 1 && wire.sent[0] == "$J=G91G21F500X10.000\n");
        CHECK(link.exec().awaiting() && d.sent() == 1);
        // A second command while one awaits its reply is not transmitted.
        CHECK(d.submit(jc_jog_incremental(p, 1, 1.0f, 100.0f), good_ctx(1010)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 1 && link.refused_busy() == 1 && d.sent() == 1);
        link.set_clock(1040);
        link.on_line("ok");
        CHECK(link.exec().outcome() == JcExecOutcome::Acked && link.exec().elapsed_ms() == 40);
        link.exec().release();
        CHECK(d.submit(jc_jog_incremental(p, 1, 1.0f, 100.0f), good_ctx(1100)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 2 && wire.sent[1] == "$J=G91G21F100Y1.000\n");
    }
    {   // controller error
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        d.submit(jc_jog_incremental(p, 2, 50.0f, 100.0f), good_ctx(0));
        link.set_clock(30);
        link.on_line("error:15");
        CHECK(link.exec().outcome() == JcExecOutcome::ControllerError && link.exec().code() == 15);
    }
    {   // timeout: abandoned, never resent
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(0));
        link.set_clock(1999);
        link.tick();
        CHECK(link.exec().awaiting());
        link.set_clock(2001);
        link.tick();
        CHECK(link.exec().outcome() == JcExecOutcome::TimedOut);
        link.set_clock(9000);
        link.tick();
        CHECK(wire.sent.size() == 1);   // never resent
        link.on_line("ok");              // late reply: not attributed to anything
        CHECK(link.exec().unsolicited() == 1);
    }
    {   // connection loss during a command; reconnect does not replay or re-arm
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(100);
        d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(100));
        CHECK(wire.sent.size() == 1);
        link.set_clock(150);
        link.on_link_lost();
        d.on_link_lost();
        CHECK(link.exec().outcome() == JcExecOutcome::LinkLost && !d.armed());
        // Reconnect: link is fresh again, the operator confirms again - still nothing is sent
        // because arming never returns on its own, and the old command is never replayed.
        link.exec().release();
        link.set_clock(5000);
        link.tick();
        CHECK(wire.sent.size() == 1 && link.transmitted() == 1);
        CHECK(d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(5000)) == JcAuthDecision::BlockedNotArmed);
        CHECK(wire.sent.size() == 1);
        // Only an explicit new arming + new command goes out.
        d.arm();
        CHECK(d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(5100)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 2);
    }
    {   // transport failure while sending
        Wire wire;
        wire.fail = true;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(0));
        CHECK(link.transmitted() == 0 && d.sent() == 0 && d.previewed() == 1);
        CHECK(link.exec().outcome() == JcExecOutcome::LinkLost);
    }
    {   // stale status, alarm, wrong state, unconfirmed limits: nothing goes out
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        JcDispatchContext stale = good_ctx(0);
        stale.link_fresh = false;
        CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), stale) == JcAuthDecision::BlockedStaleLink);
        CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(0, "Alarm")) == JcAuthDecision::BlockedAlarm);
        CHECK(d.submit(jc_home(p, 0xFF), good_ctx(0, "Run")) == JcAuthDecision::BlockedBadState);
        CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(0, "Idle", 0x00))
              == JcAuthDecision::BlockedLimitsUnconfirmed);
        CHECK(d.submit(jc_jog_incremental(p, 3, 1.0f, 100.0f), good_ctx(0, "Idle", 0x07))
              == JcAuthDecision::BlockedLimitsUnconfirmed);   // A not confirmed
        CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(0, "Idle", 0x01)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 1);
        // A stop is never gated on limits, and is allowed in Alarm.
        link.exec().on_link_lost(1);  // clear the awaiting jog
        link.exec().release();
        CHECK(d.submit(jc_jog_cancel(), good_ctx(10, "Alarm", 0x00)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 2 && wire.sent[1] == std::string("\x85", 1));   // real-time byte, no newline
        CHECK(link.exec().outcome() == JcExecOutcome::SentNoAck);
        // A command older than 500 ms is never sent late.
        JcAuthContext aged = d.make_auth(jc_jog_incremental(p, 0, 1.0f, 100.0f), good_ctx(1000));
        aged.command_issued_ms = 400;
        CHECK(jc_authorize(JcCommandClass::Jog, aged) == JcAuthDecision::BlockedStaleCommand);
    }
    {   // unexpected machine alarm while a command awaits its reply
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(0));
        link.set_clock(20);
        link.on_machine_state("Run");
        CHECK(link.exec().awaiting());
        link.on_machine_state("Alarm");
        CHECK(link.exec().outcome() == JcExecOutcome::Alarm);
        // Next jog while the alarm persists is blocked by the gate.
        link.exec().release();
        CHECK(d.submit(jc_jog_incremental(p, 0, 10.0f, 500.0f), good_ctx(30, "Alarm")) == JcAuthDecision::BlockedAlarm);
        CHECK(wire.sent.size() == 1);
    }
    {   // sequences: one step at a time, only after an acknowledgement
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        const JcCommandSequence seq = jc_probe_z_sequence(p, 20, 50, 5, 1.5f);
        CHECK(seq.ok && seq.count == 4);
        CHECK(d.submit_sequence(seq, good_ctx(0)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 1 && wire.sent[0].find("G38.2") != std::string::npos);
        CHECK(d.sequence_active() && d.sequence_steps_left() == 3);
        // Probe contact acknowledged: the offset step is released, with a fresh context.
        link.set_clock(800);
        link.on_line("ok");
        link.exec().release();
        CHECK(d.advance_sequence(link.exec().outcome(), good_ctx(800)) == JcAuthDecision::Allowed);
        CHECK(wire.sent.size() == 2 && wire.sent[1].find("G10 L20 P0 Z1.500") != std::string::npos);
        // The controller refuses that step: the rest of the sequence is dropped.
        link.set_clock(850);
        link.on_line("error:2");
        CHECK(link.exec().outcome() == JcExecOutcome::ControllerError);
        link.exec().release();
        CHECK(d.advance_sequence(JcExecOutcome::ControllerError, good_ctx(850)) == JcAuthDecision::BlockedBadState);
        CHECK(!d.sequence_active() && wire.sent.size() == 2);
    }
    {   // sequences: link loss and a stale link between steps stop the sequence
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        link.set_clock(0);
        d.submit_sequence(jc_probe_z_sequence(p, 20, 50, 5, 0), good_ctx(0));
        CHECK(d.sequence_active());
        d.on_link_lost();
        CHECK(!d.sequence_active());
        CHECK(d.advance_sequence(JcExecOutcome::Acked, good_ctx(10)) == JcAuthDecision::BlockedBadState);
        CHECK(wire.sent.size() == 1);

        Wire wire2;
        JcCommandLink link2(&Wire::send, &wire2);
        JcDispatcher d2;
        d2.set_sink_ex(&JcCommandLink::sink, &link2);
        d2.arm();
        link2.set_clock(0);
        d2.submit_sequence(jc_probe_z_sequence(p, 20, 50, 5, 0), good_ctx(0));
        link2.on_line("ok");
        link2.exec().release();
        JcDispatchContext stale = good_ctx(900);
        stale.link_fresh = false;
        CHECK(d2.advance_sequence(JcExecOutcome::Acked, stale) == JcAuthDecision::BlockedStaleLink);
        CHECK(!d2.sequence_active() && wire2.sent.size() == 1);
    }
    {   // jogging a different axis cannot borrow another axis's confirmation
        Wire wire;
        JcCommandLink link(&Wire::send, &wire);
        JcDispatcher d;
        d.set_sink_ex(&JcCommandLink::sink, &link);
        d.arm();
        for (uint8_t a = 0; a < 4; ++a) {
            for (uint8_t m = 0; m < 4; ++m) {
                JcDispatcher dd;
                dd.arm();
                const JcAuthDecision r = dd.submit(jc_jog_incremental(p, a, 1.0f, 100.0f),
                                                   good_ctx(0, "Idle", static_cast<uint8_t>(1u << m)));
                CHECK((r == JcAuthDecision::Allowed) == (a == m));
            }
        }
    }
#endif
}

// ---------------------------------------------------------------- dry run

static void test_dry_run() {
    const JcMachineProfile p = confirmed_profile();
    Wire wire;
    JcCommandLink link(&Wire::send, &wire);
    JcDispatcher d;
    d.set_sink_ex(&JcCommandLink::sink, &link);
    d.arm();
    const JcDispatchContext ctx = good_ctx(100);

    const JcCommand jog = jc_jog_incremental(p, 0, 10.0f, 500.0f);
    const JcDryRunReport r = jc_dry_run(d, "Jog X +10", jog, ctx);
    CHECK(r.built);
    CHECK_STR(r.command, "$J=G91G21F500X10.000");
    CHECK(r.timeout_ms == 2000);
    CHECK_STR(r.expected, "ok");
    CHECK(strstr(r.on_timeout, "never resent") != nullptr);
    CHECK(strstr(r.on_link_loss, "never replayed") != nullptr);
    // Same decision the real submit() would make ...
    CHECK(r.decision == d.evaluate(jog, ctx));
    // ... but a dry run never records, counts or transmits.
    CHECK(d.history_count() == 0 && d.blocked() == 0 && d.sent() == 0 && d.previewed() == 0);
    CHECK(wire.sent.empty() && link.transmitted() == 0);
#if !JC_MACHINE_CONTROL_ENABLED
    CHECK(r.decision == JcAuthDecision::BlockedBuildPolicy && !r.would_transmit);
#else
    CHECK(r.decision == JcAuthDecision::Allowed && r.would_transmit);
    CHECK(wire.sent.empty());   // "would transmit" is not "transmitted"
#endif
    // Decision tracks the context.
    JcDispatchContext stale = ctx;
    stale.link_fresh = false;
#if JC_MACHINE_CONTROL_ENABLED
    CHECK(jc_dry_run(d, "x", jog, stale).decision == JcAuthDecision::BlockedStaleLink);
#endif
    // Real-time and long-running classes have the right expectations.
    const JcDryRunReport rt = jc_dry_run(d, "Cancel", jc_jog_cancel(), ctx);
    CHECK(rt.timeout_ms == 0 && strstr(rt.expected, "none") != nullptr && strstr(rt.command, "0x85") != nullptr);
    const JcDryRunReport home = jc_dry_run(d, "Home", jc_home(p, 0xFF), ctx);
    CHECK(home.timeout_ms == 120000 && strstr(home.expected, "homing cycle") != nullptr);
    // A request the builder rejects is reported as not generated.
    const JcDryRunReport bad = jc_dry_run(d, "Probe A", jc_probe_move(p, 3, -5.0f, 50.0f), ctx, "not supported");
    CHECK(!bad.built && !bad.would_transmit);
    CHECK_STR(bad.command, "(not generated)");
    CHECK_STR(bad.rejected_reason, "not supported");
    CHECK(d.history_count() == 0 && wire.sent.empty());
}

int main() {
    test_classify();
    test_splitter();
    test_query_allowlist();
    test_exec();
    test_caps_loader();
    test_four_axis_validation();
    test_dispatcher_transport();
    test_dry_run();
    printf("%d checks, %d failed (control tests, JC_MACHINE_CONTROL_ENABLED=%d)\n", g_run, g_fail,
           JC_MACHINE_CONTROL_ENABLED);
    return g_fail ? 1 : 0;
}
