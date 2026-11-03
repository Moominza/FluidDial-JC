// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host tests for the application-layer logic: frame assembly over fragmented
// streams, machine model, dispatcher, navigation, settings persistence and the
// additional command builders. No hardware or network involved.

#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "../../src/jc8048w550/JcCommandBuilder.hpp"
#include "../../src/jc8048w550/JcDispatch.hpp"
#include "../../src/jc8048w550/JcFrameAssembler.hpp"
#include "../../src/jc8048w550/JcLinkTypes.hpp"
#include "../../src/jc8048w550/JcMachineModel.hpp"
#include "../../src/jc8048w550/JcNav.hpp"
#include "../../src/jc8048w550/JcSettings.hpp"
#include "../../src/jc8048w550/JcStatusFrame.hpp"

using namespace jc8048w550;

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { ++g_run; if (strcmp((a), (b)) != 0) { ++g_fail; printf("FAIL %s:%d  '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); } } while (0)

// Feeds a byte stream through the assembler + parser in chunks of `chunk`
// bytes; returns the number of valid frames and invalid events.
struct StreamResult { int frames; int invalid; JcStatusFrame last; };
static StreamResult run_stream(const char* stream, size_t chunk) {
    JcFrameAssembler a;
    StreamResult r {};
    const size_t n = strlen(stream);
    for (size_t i = 0; i < n; i += chunk) {
        const size_t end = i + chunk < n ? i + chunk : n;
        for (size_t j = i; j < end; ++j) {
            switch (a.push(static_cast<uint8_t>(stream[j]))) {
                case JcFrameAssembler::Result::None: break;
                case JcFrameAssembler::Result::Invalid: ++r.invalid; break;
                case JcFrameAssembler::Result::Frame: {
                    JcStatusFrame f {};
                    if (jc_parse_status_frame(a.frame(), a.length(), f)) { ++r.frames; r.last = f; }
                    else ++r.invalid;
                    break;
                }
            }
        }
    }
    return r;
}

static void test_assembler_fragmentation() {
    const char* two = "<Idle|MPos:1.000,2.000,3.000,4.000|FS:0,0>\r\nok\r\n"
                      "<Run|MPos:-5.5,6.5,7.5,-8.5|FS:100,0|SD:42.5,/job.nc>\r\n";
    for (size_t chunk : { (size_t)1, (size_t)2, (size_t)3, (size_t)7, (size_t)64 }) {
        StreamResult r = run_stream(two, chunk);
        CHECK(r.frames == 2 && r.invalid == 0);
        CHECK_STR(r.last.state, "Run");
        CHECK(r.last.has_sd && r.last.sd_percent == 42.5f);
        CHECK_STR(r.last.sd_name, "/job.nc");
    }
    // Garbage / ok / messages outside frames are ignored.
    StreamResult r = run_stream("ok\r\n[MSG:Info]\r\nGrbl 4.1 ['$' for help]\r\n", 5);
    CHECK(r.frames == 0 && r.invalid == 0);
    // Nested start: first (partial) frame dropped, second one delivered.
    r = run_stream("<Idle|MPos:1,2<Idle|MPos:1,2,3,4|FS:0,0>", 4);
    CHECK(r.invalid == 1 && r.frames == 1);
    // Control character inside a frame invalidates it; next good frame recovers.
    r = run_stream("<Idle|MPos:1,2\x01,3,4|FS:0,0>\r\n<Idle|MPos:1,2,3,4|FS:0,0>", 3);
    CHECK(r.invalid >= 1 && r.frames == 1);
    // Unterminated over-long frame is bounded, does not overflow.
    char big[600];
    memset(big, 'x', sizeof(big));
    big[0] = '<';
    big[599] = 0;
    r = run_stream(big, 16);
    CHECK(r.frames == 0 && r.invalid == 1);
    // Malformed frames (three axes, bad number) are counted, not delivered.
    r = run_stream("<Idle|MPos:1,2,3|FS:0,0>\r\n<Idle|MPos:a,b,c,d|FS:0,0>\r\n", 2);
    CHECK(r.frames == 0 && r.invalid == 2);
}

static void test_sd_field_lenient() {
    JcStatusFrame f {};
    const char* s1 = "<Run|MPos:1,2,3,4|FS:0,0|SD:abc>";
    CHECK(jc_parse_status_frame(s1, strlen(s1), f) && !f.has_sd);  // bad SD never rejects position
    const char* s2 = "<Run|MPos:1,2,3,4|FS:0,0|SD:101,/x>";
    CHECK(jc_parse_status_frame(s2, strlen(s2), f) && !f.has_sd);
    const char* s3 = "<Run|MPos:1,2,3,4|FS:0,0|SD:7>";
    CHECK(jc_parse_status_frame(s3, strlen(s3), f) && f.has_sd && f.sd_percent == 7.0f && f.sd_name[0] == 0);
}

static JcLiveMachineStatus mk(const char* state, bool machine, float a, float b, float c, float d,
                              bool has_wco = false, float w0 = 0, float w1 = 0, float w2 = 0, float w3 = 0,
                              uint32_t seq = 1) {
    JcLiveMachineStatus s {};
    s.has_data = true;
    strcpy(s.machine_state, state);
    s.coordinate_reference = machine ? JcCoordinateReference::Machine : JcCoordinateReference::Work;
    s.axes[0] = a; s.axes[1] = b; s.axes[2] = c; s.axes[3] = d;
    s.has_wco = has_wco;
    s.wco[0] = w0; s.wco[1] = w1; s.wco[2] = w2; s.wco[3] = w3;
    s.sequence = seq;
    s.last_valid_ms = seq * 100;
    return s;
}

static void test_machine_model() {
    JcMachineModel m;
    float v;
    CHECK(!m.has_status() && !m.position(0, JcRef::Machine, v) && !m.alarm() && !m.in_motion());

    // Machine reference reported, no WCO yet: work is UNAVAILABLE (never zero).
    m.on_status(mk("Idle", true, 100, 50, -10, 45));
    CHECK(m.position(0, JcRef::Machine, v) && v == 100.0f);
    CHECK(!m.position(0, JcRef::Work, v));
    CHECK(!m.wco_known());

    // WCO arrives: derive work = machine - wco for all four axes (A in degrees).
    m.on_status(mk("Run", true, 100, 50, -10, 45, true, 10, 20, -5, 5, 2));
    CHECK(m.position(0, JcRef::Work, v) && v == 90.0f);
    CHECK(m.position(1, JcRef::Work, v) && v == 30.0f);
    CHECK(m.position(2, JcRef::Work, v) && v == -5.0f);
    CHECK(m.position(3, JcRef::Work, v) && v == 40.0f);
    CHECK(m.in_motion() && !m.alarm());

    // WCO is retained when later frames omit it.
    m.on_status(mk("Idle", true, 0, 0, 0, 0, false, 0, 0, 0, 0, 3));
    CHECK(m.position(2, JcRef::Work, v) && v == 5.0f);

    // Work reported: machine = work + wco.
    m.on_status(mk("Idle", false, 1, 2, 3, 4, false, 0, 0, 0, 0, 4));
    CHECK(m.position(0, JcRef::Machine, v) && v == 11.0f);
    CHECK(m.position(3, JcRef::Work, v) && v == 4.0f);
    CHECK(!m.position(4, JcRef::Work, v));  // no fifth axis

    // Alarm / homing / probe pin / job.
    JcLiveMachineStatus s = mk("Alarm", true, 0, 0, 0, 0, false, 0, 0, 0, 0, 5);
    strcpy(s.pins, "P");
    s.has_sd = true;
    m.on_status(s);
    CHECK(m.alarm() && m.probe_triggered() && m.job_running());
    m.on_status(mk("Home", true, 0, 0, 0, 0, false, 0, 0, 0, 0, 6));
    CHECK(m.homing() && m.in_motion() && !m.probe_triggered());

    // History: only state changes, newest first, bounded.
    CHECK(m.history_count() >= 3 && m.history_count() <= JcMachineModel::kHistory);
    CHECK_STR(m.history(0)->state, "Home");
    for (int i = 0; i < 20; ++i) {
        m.on_status(mk(i % 2 ? "Idle" : "Run", true, 0, 0, 0, 0, false, 0, 0, 0, 0, 10 + i));
    }
    CHECK(m.history_count() == JcMachineModel::kHistory);
    CHECK(m.history(JcMachineModel::kHistory) == nullptr);
}

static void test_dispatcher() {
    const JcMachineProfile p = jc_default_profile();
    JcDispatcher d;
    JcDispatchContext c;
    c.live = true; c.link_fresh = true; c.machine_state = "Idle"; c.operator_confirmed = true; c.now_ms = 500;

    // Shipping policy: everything is blocked, even fully "valid" context + arming.
    d.arm();
    CHECK(d.submit(jc_jog_incremental(p, 0, 1.0f, 500.0f), c) != JcAuthDecision::Allowed || JC_MACHINE_CONTROL_ENABLED);
    JcCommandSequence seq = jc_probe_z_sequence(p, 20, 50, 5, 0);
    d.submit_sequence(seq, c);
    CHECK(d.sent() == 0);
#if !JC_MACHINE_CONTROL_ENABLED
    CHECK(d.last_decision() == JcAuthDecision::BlockedBuildPolicy);
    CHECK(d.blocked() == 2);  // jog + the first sequence step (sequence stops at first block)
    CHECK(d.previewed() == 0);
#endif

    // Invalid commands are recorded as rejected and never submitted to the gate.
    JcDispatcher e;
    JcCommand bad = jc_jog_incremental(p, 9, 1.0f, 100.0f);
    CHECK(e.submit(bad, c) == JcAuthDecision::BlockedBadState);
    CHECK(e.history_count() == 1 && !e.history(0)->valid);
    CHECK(e.blocked() == 0);

    // History is bounded and newest-first; real-time commands are recorded symbolically.
    JcDispatcher h;
    for (int i = 0; i < 12; ++i) h.submit(jc_jog_incremental(p, 0, 1.0f + i, 500.0f), c);
    CHECK(h.history_count() == JcDispatcher::kHistory);
    h.submit(jc_jog_cancel(), c);
    CHECK_STR(h.history(0)->text, "<realtime 0x85>");

    // Link loss disarms; reconnecting does not re-arm.
    JcDispatcher a;
    a.arm();
    CHECK(a.armed());
    a.on_link_lost();
    CHECK(!a.armed());
    JcDispatchContext r = c;  // "reconnected": fresh again
    a.submit(jc_jog_incremental(p, 0, 1.0f, 100.0f), r);
    CHECK(!a.armed());
}

static void test_navigation() {
    JcNav nav;
    CHECK(nav.current() == JcPage::Menu && nav.depth() == 1);
    CHECK(nav.back() == JcNav::BackResult::ExitApp);  // back at root leaves the app

    const JcPage sections[] = { JcPage::Jog, JcPage::Homing, JcPage::Probing, JcPage::Offsets,
                                JcPage::Files, JcPage::Macros, JcPage::Tools, JcPage::Settings };
    for (JcPage s : sections) {
        CHECK(nav.go(s) && nav.current() == s && nav.depth() == 2);
        CHECK(nav.back() == JcNav::BackResult::Moved && nav.current() == JcPage::Menu);
    }
    // Menu cannot jump straight into subpages; same-page go is a no-op.
    CHECK(!nav.go(JcPage::SetDisplay) && !nav.go(JcPage::Diagnostics) && !nav.go(JcPage::Menu));
    CHECK(!nav.go(JcPage::Input));  // modal input needs a calling page

    // Settings -> every subpage -> back.
    CHECK(nav.go(JcPage::Settings));
    const JcPage subs[] = { JcPage::SetConnections, JcPage::SetDisplay, JcPage::SetJogging,
                            JcPage::SetMachine, JcPage::Diagnostics };
    for (JcPage s : subs) {
        CHECK(nav.go(s) && nav.depth() == 3);
        CHECK(nav.go(JcPage::Input) && nav.depth() == 4);      // input from a subpage
        CHECK(nav.back() == JcNav::BackResult::Moved && nav.current() == s);
        CHECK(nav.back() == JcNav::BackResult::Moved && nav.current() == JcPage::Settings);
    }
    // Siblings are not directly reachable.
    CHECK(nav.go(JcPage::SetDisplay) && !nav.go(JcPage::SetJogging));
    nav.reset();
    CHECK(nav.current() == JcPage::Menu && nav.depth() == 1);

    // Tools -> Probing (tool measurement configuration) and depth is bounded.
    CHECK(nav.go(JcPage::Tools) && nav.go(JcPage::Probing) && nav.go(JcPage::Input));
    CHECK(!nav.go(JcPage::Input) || nav.depth() <= JcNav::kMaxDepth);
    // Every page has a title.
    for (int i = 0; i < (int)JcPage::Count; ++i) CHECK(strcmp(jc_page_title((JcPage)i), "?") != 0);
}

static void test_settings_persistence() {
    JcUserSettings d = jc_settings_defaults();
    CHECK(d.linear_steps[0] == 0.01f && d.linear_steps[3] == 10.0f && d.linear_steps[4] == 100.0f);
    CHECK(d.rotary_steps[2] == 5.0f && d.rotary_steps[3] == 15.0f && d.rotary_steps[4] == 45.0f && d.rotary_steps[5] == 90.0f);
    CHECK(jc_step_count(0) == 5 && jc_step_count(2) == 5 && jc_step_count(3) == 6);
    {   // saved v1 settings (five presets) migrate without losing operator preferences
        JcSettingsBlobV1 v1; memset(&v1, 0, sizeof(v1));
        v1.magic = kJcSettingsMagic; v1.version = 1; v1.size = sizeof(JcUserSettingsV1);
        const float ol[5] = { 0.1f, 1.0f, 10.0f, 50.0f, 100.0f }, orot[5] = { 0.2f, 2.0f, 10.0f, 45.0f, 90.0f };
        memcpy(v1.settings.linear_steps, ol, sizeof(ol)); memcpy(v1.settings.rotary_steps, orot, sizeof(orot));
        const float lf[3] = { 100.0f, 500.0f, 1000.0f }, rf[3] = { 600.0f, 1800.0f, 3600.0f };
        memcpy(v1.settings.linear_feeds, lf, sizeof(lf)); memcpy(v1.settings.rotary_feeds, rf, sizeof(rf));
        v1.settings.brightness_percent = 55; v1.settings.coordinate_decimals = 2; v1.settings.default_reference = 0;
        v1.settings.transport = 1; v1.settings.uart_baud = 57600; v1.settings.uart_rx_pin = 5; v1.settings.uart_tx_pin = 6;
        v1.settings.probe_feed = 77.0f;
        v1.crc = jc_crc32(reinterpret_cast<const uint8_t*>(&v1), offsetof(JcSettingsBlobV1, crc));
        JcUserSettings m;
        CHECK(jc_settings_decode(&v1, sizeof(v1), m));
        CHECK(m.linear_steps[0] == 0.01f && m.linear_steps[3] == 10.0f);                 // untouched factory values -> new factory values
        CHECK(m.rotary_steps[0] == 0.2f && m.rotary_steps[1] == 2.0f && m.rotary_steps[5] == 90.0f);  // customised kept
        // v1 held three feed presets per set: the old factory sets move to the new factory sets, nothing is raised
        CHECK(m.linear_feeds[0] == 300.0f && m.linear_feeds[3] == 6000.0f && m.rotary_feeds[0] == 100.0f && m.rotary_feeds[3] == 1000.0f);
        CHECK(m.z_feeds[0] == 100.0f && m.z_feeds[3] == 1500.0f);
        CHECK(m.brightness_percent == 55 && m.transport == 1 && m.uart_baud == 57600 && m.uart_rx_pin == 5 && m.uart_tx_pin == 6 && m.probe_feed == 77.0f);
        CHECK(jc_settings_stored_length_ok(sizeof(JcSettingsBlobV1)) && jc_settings_stored_length_ok(sizeof(JcSettingsBlob)));
        CHECK(!jc_settings_stored_length_ok(0) && !jc_settings_stored_length_ok(sizeof(JcSettingsBlob) + 1));
        v1.crc ^= 1u;
        CHECK(!jc_settings_decode(&v1, sizeof(v1), m));
    }
    {   // saved v2 settings (three feed presets) migrate to v3: customised feeds are kept and never raised
        JcSettingsBlobV2 v2; memset(&v2, 0, sizeof(v2));
        v2.magic = kJcSettingsMagic; v2.version = 2; v2.size = sizeof(JcUserSettingsV2);
        const JcUserSettings d = jc_settings_defaults();
        memcpy(v2.settings.linear_steps, d.linear_steps, sizeof(d.linear_steps));
        memcpy(v2.settings.rotary_steps, d.rotary_steps, sizeof(d.rotary_steps));
        const float lf[3] = { 50.0f, 400.0f, 800.0f }, rf[3] = { 200.0f, 500.0f, 1000.0f };   // X/Y customised; A = old factory
        memcpy(v2.settings.linear_feeds, lf, sizeof(lf)); memcpy(v2.settings.rotary_feeds, rf, sizeof(rf));
        v2.settings.brightness_percent = 40; v2.settings.coordinate_decimals = 2; v2.settings.default_reference = 0;
        v2.settings.uart_baud = 115200; v2.settings.uart_rx_pin = -1; v2.settings.uart_tx_pin = -1; v2.settings.probe_feed = 60.0f;
        v2.settings.probe_travel = 10.0f; v2.settings.probe_retract = 2.0f;
        v2.crc = jc_crc32(reinterpret_cast<const uint8_t*>(&v2), offsetof(JcSettingsBlobV2, crc));
        JcUserSettings m;
        CHECK(jc_settings_stored_length_ok(sizeof(v2)));
        CHECK(jc_settings_decode(&v2, sizeof(v2), m));
        CHECK(m.linear_feeds[0] == 50.0f && m.linear_feeds[1] == 400.0f && m.linear_feeds[2] == 800.0f);   // customised: kept as set
        CHECK(m.linear_feeds[3] == 6000.0f);                                                              // the new fourth preset
        CHECK(m.rotary_feeds[0] == 100.0f && m.rotary_feeds[1] == 250.0f && m.rotary_feeds[3] == 1000.0f);  // old factory -> new factory
        CHECK(m.z_feeds[0] == 100.0f && m.z_feeds[1] == 300.0f && m.z_feeds[2] == 750.0f && m.z_feeds[3] == 1500.0f);
        CHECK(m.brightness_percent == 40 && m.coordinate_decimals == 2 && m.default_reference == 0 && m.probe_feed == 60.0f);
        CHECK(m.rotary_steps[5] == 90.0f && m.linear_steps[0] == 0.01f);
        v2.crc ^= 1u;
        CHECK(!jc_settings_decode(&v2, sizeof(v2), m));
        // a v3 blob round-trips
        JcUserSettings s3 = jc_settings_defaults(); s3.z_feeds[1] = 333.0f;
        const JcSettingsBlob b3 = jc_settings_encode(s3);
        CHECK(jc_settings_decode(&b3, sizeof(b3), m) && m.z_feeds[1] == 333.0f && m.linear_feeds[3] == 6000.0f);
        // the stored length alone identifies the format
        CHECK(sizeof(JcSettingsBlob) != sizeof(JcSettingsBlobV2) && sizeof(JcSettingsBlobV2) != sizeof(JcSettingsBlobV1));
    }
    {   // feed presets: per axis group, defaults, selection never rises above an axis limit
        const JcUserSettings d = jc_settings_defaults();
        CHECK(jc_feed_presets(d, 0) == d.linear_feeds && jc_feed_presets(d, 1) == d.linear_feeds);
        CHECK(jc_feed_presets(d, 2) == d.z_feeds && jc_feed_presets(d, 3) == d.rotary_feeds);
        CHECK(d.linear_feeds[0] == 300.0f && d.linear_feeds[1] == 1000.0f && d.linear_feeds[2] == 3000.0f && d.linear_feeds[3] == 6000.0f);
        CHECK(d.z_feeds[0] == 100.0f && d.z_feeds[1] == 300.0f && d.z_feeds[2] == 750.0f && d.z_feeds[3] == 1500.0f);
        CHECK(d.rotary_feeds[0] == 100.0f && d.rotary_feeds[1] == 250.0f && d.rotary_feeds[2] == 500.0f && d.rotary_feeds[3] == 1000.0f);
        CHECK(kJcFeedPresets == 4);
        CHECK(jc_feed_choice_within(d.linear_feeds, 3, 6000.0f) == 3);          // allowed: unchanged
        CHECK(jc_feed_choice_within(d.linear_feeds, 3, 3500.0f) == 2);          // controller says 3500: down to 3000, never up
        CHECK(jc_feed_choice_within(d.linear_feeds, 1, 3500.0f) == 1);          // a lower selection is left alone
        CHECK(jc_feed_choice_within(d.linear_feeds, 3, 1200.0f) == 1);
        CHECK(jc_feed_choice_within(d.linear_feeds, 0, 100.0f) == 0xFF);        // nothing fits: the caller keeps it and the jog is refused
        CHECK(jc_feed_choice_within(d.z_feeds, 3, 1500.0f) == 3 && jc_feed_choice_within(d.z_feeds, 3, 600.0f) == 1);
    }
    CHECK(!d.a_axis_has_homing && d.transport == 0 && d.uart_rx_pin == -1);

    // Round trip of a customized configuration.
    JcUserSettings s = d;
    s.linear_steps[0] = 0.05f; s.brightness_percent = 40; s.coordinate_decimals = 2;
    s.transport = 1; s.uart_baud = 57600; s.uart_rx_pin = 18; s.uart_tx_pin = 17;
    s.a_axis_has_homing = true; s.probe_feed = 75.0f; s.plate_thickness = 19.05f;
    strcpy(s.macros[1].name, "Park"); strcpy(s.macros[1].path, "/macros/park.nc");
    strcpy(s.macros[1].description, "Move to park position");
    JcSettingsBlob blob = jc_settings_encode(s);
    JcUserSettings out;
    CHECK(jc_settings_decode(&blob, sizeof(blob), out));
    CHECK(out.linear_steps[0] == 0.05f && out.brightness_percent == 40 && out.transport == 1);
    CHECK(out.uart_baud == 57600 && out.uart_rx_pin == 18 && out.a_axis_has_homing);
    CHECK_STR(out.macros[1].path, "/macros/park.nc");

    // Corruption, truncation, wrong magic/version: defaults returned, rejected.
    JcSettingsBlob bad = blob; bad.settings.brightness_percent ^= 0x55;
    CHECK(!jc_settings_decode(&bad, sizeof(bad), out) && out.brightness_percent == 100);
    CHECK(!jc_settings_decode(&blob, sizeof(blob) - 1, out));
    bad = blob; bad.magic = 0;
    CHECK(!jc_settings_decode(&bad, sizeof(bad), out));
    bad = blob; bad.version = 9; bad.crc = jc_crc32((const uint8_t*)&bad, offsetof(JcSettingsBlob, crc));
    CHECK(!jc_settings_decode(&bad, sizeof(bad), out));
    CHECK(!jc_settings_decode(nullptr, 0, out));

    // Invalid values are sanitized even when the checksum is valid.
    JcUserSettings evil = d;
    evil.linear_steps[2] = NAN; evil.rotary_steps[1] = 9999.0f; evil.linear_feeds[0] = -5.0f;
    evil.brightness_percent = 0; evil.coordinate_decimals = 9; evil.transport = 7; evil.uart_baud = 12345;
    evil.uart_rx_pin = 100; evil.probe_travel = 1e9f; evil.tool_diameter = -3.0f;
    strcpy(evil.macros[0].path, "/../etc/passwd"); strcpy(evil.macros[0].name, "x");
    memset(evil.macros[2].name, 'a', sizeof(evil.macros[2].name));  // not NUL terminated
    blob = jc_settings_encode(evil);
    CHECK(jc_settings_decode(&blob, sizeof(blob), out));
    CHECK(out.linear_steps[2] == 1.0f && out.rotary_steps[1] == 360.0f && out.linear_feeds[0] == 1.0f);
    CHECK(out.brightness_percent == 100 && out.coordinate_decimals == 3 && out.transport == 0);
    CHECK(out.uart_baud == 115200 && out.uart_rx_pin == -1);
    CHECK(out.probe_travel == 50.0f && out.tool_diameter == 0.0f);
    CHECK(out.macros[0].name[0] == 0 && out.macros[0].path[0] == 0);   // path traversal cleared
    CHECK(strlen(out.macros[2].name) < sizeof(out.macros[2].name));

    // Profile derives A homing from the setting only.
    CHECK(!jc_profile_from_settings(d).axes[3].homing_allowed);
    CHECK(jc_profile_from_settings(s).axes[3].homing_allowed);
}

static void test_builders_extra() {
    const JcMachineProfile p = jc_default_profile();
    CHECK_STR(jc_jog_continuous(p, 0, 1, 1000.0f).text, "$J=G91G21F1000X1000");
    CHECK_STR(jc_jog_continuous(p, 1, -1, 500.0f).text, "$J=G91G21F500Y-1000");
    CHECK(jc_jog_continuous(p, 3, 1, 600.0f).error == JcCmdError::Unsupported);  // rotary: never continuous
    CHECK(!jc_jog_continuous(p, 0, 0, 500.0f).ok && !jc_jog_continuous(p, 0, 1, 9e9f).ok);

    // Edge probing: +X touches a left edge, X becomes -radius; retract opposite.
    JcCommandSequence s = jc_probe_edge_sequence(p, 0, 1, 20, 50, 5, 6.0f);
    CHECK(s.ok && s.count == 4);
    CHECK_STR(s.steps[0].text, "G21 G91 G38.2 F50 X20.000");
    CHECK_STR(s.steps[1].text, "G10 L20 P0 X-3.000");
    CHECK_STR(s.steps[2].text, "G91 G0 X-5.000");
    CHECK_STR(s.steps[3].text, "G90");
    s = jc_probe_edge_sequence(p, 1, -1, 20, 50, 5, 6.0f);
    CHECK(s.ok);
    CHECK_STR(s.steps[0].text, "G21 G91 G38.2 F50 Y-20.000");
    CHECK_STR(s.steps[1].text, "G10 L20 P0 Y3.000");
    CHECK_STR(s.steps[2].text, "G91 G0 Y5.000");
    CHECK(!jc_probe_edge_sequence(p, 2, 1, 20, 50, 5, 6).ok);   // Z is not an edge axis
    CHECK(!jc_probe_edge_sequence(p, 3, 1, 20, 50, 5, 6).ok);   // A never uses linear probing
    CHECK(!jc_probe_edge_sequence(p, 0, 0, 20, 50, 5, 6).ok);
    CHECK(!jc_probe_edge_sequence(p, 0, 1, 20, 50, -1, 6).ok);
    CHECK(!jc_probe_edge_sequence(p, 0, 1, 20, 5000, 5, 6).ok);
}

static void test_link_rules() {
    // Freshness: link must be up, data present and younger than the stale window.
    CHECK(!jc_status_fresh(false, true, 1000, 900, 3000));
    CHECK(!jc_status_fresh(true, false, 1000, 900, 3000));
    CHECK(jc_status_fresh(true, true, 3900, 900, 3000));
    CHECK(!jc_status_fresh(true, true, 3901, 900, 3000));
    CHECK(jc_status_fresh(true, true, 5, 4294967000u, 3000));   // millis() wraparound
    // Only '?' may be transmitted.
    int allowed = 0;
    for (int b = 0; b < 256; ++b) allowed += jc_tx_byte_allowed((uint8_t)b);
    CHECK(allowed == 1 && jc_tx_byte_allowed('?'));
    CHECK(!jc_tx_byte_allowed('$') && !jc_tx_byte_allowed('!') && !jc_tx_byte_allowed('~')
          && !jc_tx_byte_allowed(0x18) && !jc_tx_byte_allowed(0x85) && !jc_tx_byte_allowed('\n'));
}

int main() {
    test_assembler_fragmentation();
    test_sd_field_lenient();
    test_machine_model();
    test_dispatcher();
    test_navigation();
    test_settings_persistence();
    test_builders_extra();
    test_link_rules();
    printf("%d checks, %d failed (app tests, JC_MACHINE_CONTROL_ENABLED=%d)\n", g_run, g_fail, JC_MACHINE_CONTROL_ENABLED);
    return g_fail ? 1 : 0;
}
