// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.
//
// Host UI renderer and layout checker (Instructions 006B / 006C).
//
// Compiles the REAL JC page code (JcUiKit, JcFontRenderer, JcScreen*, the real fonts) against
// small host stubs of the display and of Arduino (test/host/ui_stubs), drives the pages with
// simulated touches and controller states, and
//   * writes each frame as a PPM (tools/run_ui_preview.py turns them into PNG screenshots);
//   * checks every frame for text outside the screen, text drawn over other text, text wiped
//     out by a later filled rectangle, and, for dialogs, text outside the box or touching a button.
//
// It never touches a socket, the controller or any hardware. Build twice by
// tools/run_ui_preview.py: ordinary policy, and -DJC_MACHINE_CONTROL_ENABLED=1 -DJC_LIVE_JOG_TEST=1.
//
// Usage: ui_render [output_dir]      (no directory: checks only, no images)

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "../../src/jc8048w550/JcDashLayout.hpp"
#include "../../src/jc8048w550/JcJogLayout.hpp"
#include "../../src/jc8048w550/JcScreens.hpp"
#include "../../src/jc8048w550/JcSimNc.hpp"

// ---- stub globals ----------------------------------------------------------------------
uint32_t g_stub_millis = 100000;
bool g_stub_serial_echo = false;
StubSerial Serial;

// ---- stubs of the Wi-Fi link (the real one needs the ESP32 network stack) ------------------
namespace jc8048w550 {
JcReadOnlyFluidNc::JcReadOnlyFluidNc() {}
JcFluidState JcReadOnlyFluidNc::state() const { return JcFluidState::Connected; }
const char* JcReadOnlyFluidNc::state_name(JcFluidState s) { return s == JcFluidState::Connected ? "FluidNC connected" : "stub"; }
}  // namespace jc8048w550

using namespace jc8048w550;

static int g_problems = 0, g_frames = 0;
static const char* g_outdir = nullptr;
static const char* g_tag = JC_JOG_PREVIEW ? "preview" : (JC_LIVE_JOG_TEST ? "live" : "ordinary");

// ---- the rig: the services a page needs, with simulated controller state ----------------------------

class StubHost : public JcAppHost {
public:
    void go(JcPage) override {}
    void back() override {}
    void request_input(const JcInputRequest&) override {}
    void save_settings() override {}
    void apply_brightness() override {}
    void leave(JcLeaveTarget) override {}
    const char* start_caps_read() override { ++reads_started; return nullptr; }
#if JC_AUTHORITY
    bool (*prepare_fn)(void*, JcWfKind, const JcWfParams&) = nullptr;
    void* prepare_user = nullptr;
    bool workflow_prepare(JcWfKind k, const JcWfParams& p) override { return prepare_fn != nullptr && prepare_fn(prepare_user, k, p); }
#endif
    int reads_started = 0;
    void cancel_caps_read() override {}
    // Instruction 010 hooks: the renderer's stand-ins record what a page asked for.
    int sd_refreshes = 0, tool_reads = 0, ble_scans = 0, ble_connects = 0, ble_disconnects = 0, ble_forgets = 0, clears = 0;
    JcWfKind opened = JcWfKind::GoToXY;
    const char* (*sd_fn)(void*) = nullptr;
    void* sd_user = nullptr;
    JcBleUart* ble = nullptr;
    bool simulated_job = false;
    const char* sd_refresh() override { ++sd_refreshes; return sd_fn != nullptr ? sd_fn(sd_user) : nullptr; }
    const char* tool_read() override { ++tool_reads; return nullptr; }
    const char* select_transport(uint8_t) override { return nullptr; }
    bool ble_scan() override { ++ble_scans; return ble != nullptr && ble->scan(g_stub_millis); }
    bool ble_connect(const char* a, const char* n) override { ++ble_connects; return ble != nullptr && ble->connect_to(a, n, g_stub_millis); }
    void ble_disconnect() override { ++ble_disconnects; if (ble != nullptr) ble->disconnect(g_stub_millis); }
    void ble_forget() override { ++ble_forgets; if (ble != nullptr) ble->forget(); }
    bool job_simulated() const override { return simulated_job; }
#if JC_AUTHORITY
    void workflow_open(JcWfKind k) override { opened = k; }
    bool workflow_clear_tlo() override { ++clears; return true; }
#endif
};

struct Rig {
    Arduino_GFX gfx;
    JcReadOnlyFluidNc link;
    JcMachineModel model;
    JcUserSettings settings = jc_settings_defaults();
    JcMachineProfile profile = jc_profile_from_settings(settings);
    JcDispatcher dispatcher;
    JcCapsLoader loader;
    JcJogSession jogs;
    bool live_mode = true;
    StubHost host;
    JcSdBrowser sd;
    JcJobTracker job;
    JcToolReader tool_reader;
    JcToolStore tool_store;
    JcBleUart ble;
    JcBleSaved ble_saved {};
    JcTransportManager tm;
#if JC_JOG_PREVIEW
    JcPreviewBridge bridge;   // the SIMULATED controller: the only place a jog can go
    JcJogStatus real;         // the latest REAL read-only status
#endif
#if JC_AUTHORITY
    JcCommandSystem cmd { &Rig::tx, nullptr };
    JcSimNc nc;               // the shared simulated FluidNC for the positioning / probing routines
    struct WfH : JcWfHost {
        Rig* r = nullptr;
        bool submit(const JcCommand& c, const char** why) override;
        bool query(const char* line) override;
        void stop() override;
    } wfh;
    JcWorkflow wf { &wfh };
#endif
    JcAppServices sv;
    std::string wire;   // every byte the session would have transmitted
    bool tx_ok = true;
    uint32_t seq = 1000;

    Rig() : jogs(&Rig::tx, this), sv { gfx, link, model, settings, profile, dispatcher, loader, jogs, live_mode, host, sd, job, tool_reader, tool_store, ble, ble_saved, tm
#if JC_JOG_PREVIEW
                                       , bridge
#endif
#if JC_AUTHORITY
                                       , cmd, wf
#endif
                                     } {
        host.ble = &ble;
#if JC_AUTHORITY
        wfh.r = this;
        host.prepare_fn = &Rig::prepare_thunk;
        host.prepare_user = this;
        cmd.attach_jogs(&jogs);
        cmd.set_transport(&Rig::tx, this);
#endif
        refresh_profile();
#if JC_JOG_PREVIEW
        dispatcher.set_preview(true);   // the ordinary firmware's non-motion test mode
#endif
#if JC_AUTHORITY
        dispatcher.set_sink_ex(&JcCommandSystem::sink, &cmd);
#else
        dispatcher.set_sink_ex(&JcJogSession::sink, &jogs);   // the experimental/preview wiring; blocked by policy otherwise
#endif
    }
    // Test transport (can be made to fail). In the preview build the bytes go ONLY to the simulation.
    static bool tx(void* user, const char* bytes, size_t n) {
        Rig* r = static_cast<Rig*>(user);
        if (!r->tx_ok) return false;
        r->wire.append(bytes, n);
#if JC_AUTHORITY
        // Jogs and the jog cancel go to the jog-scenario simulation, everything else to the shared simulated FluidNC.
        const bool jog_family = (n == 1 && static_cast<uint8_t>(bytes[0]) == 0x85) || (n > 3 && strncmp(bytes, "$J=", 3) == 0);
        if (jog_family) return r->bridge.controller().on_tx(bytes, n);
        return r->nc.send(bytes, n);
#elif JC_JOG_PREVIEW
        return r->bridge.controller().on_tx(bytes, n);
#else
        return true;
#endif
    }
#if JC_AUTHORITY
    static bool prepare_thunk(void* user, JcWfKind kind, const JcWfParams& p) {
        Rig* r = static_cast<Rig*>(user);
        if (kind == JcWfKind::ToolReference || kind == JcWfKind::ToolMeasure || kind == JcWfKind::ToolChange) {
            if (r->nc.feat != JcSimFeature::Plate) r->nc.plate_z = r->nc.m[2] - 15.0f - 20.0f;   // fixed once: the reference tool is 20 mm long
            r->nc.feat = JcSimFeature::Plate;
            r->nc.tool_len = p.has_ref ? 26.5f : 20.0f;
            return r->wf.prepare(kind, p, r->wf_status(), g_stub_millis);
        }
        r->nc.centre_feature_here(kind == JcWfKind::FindRotary ? JcSimFeature::Cylinder : (p.feature == JcFeatureKind::Outside ? JcSimFeature::Boss : JcSimFeature::Bore), 10.0f, 20.0f);
        return r->wf.prepare(kind, p, r->wf_status(), g_stub_millis);
    }
    JcWfStatus wf_status() {
        JcWfStatus s;
        s.fresh = true;
        s.state = nc.state;
        s.wpos_valid = true;
        for (int a = 0; a < 3; ++a) s.wpos[a] = nc.work(a);
        s.pins = nc.pins;
        s.sequence = static_cast<uint32_t>(g_stub_millis / 100);
        s.probe_configured = JcTri::Yes;
        return s;
    }
    void wf_run(uint32_t ms) {
        for (uint32_t done = 0; done < ms; done += 10) {
            advance(10);
            nc.update(g_stub_millis);
            cmd.set_clock(g_stub_millis);
            char line[96];
            while (nc.pop_line(line, sizeof(line))) { cmd.on_line(line); wf.on_line(line); }
            cmd.tick();
            JcCommandResult res;
            if (cmd.take_result(res)) wf.on_result(res);
            wf.on_status(wf_status());
            wf.service(g_stub_millis);
        }
    }
#endif
#if JC_AUTHORITY
    // (workflow helpers are defined above)
#endif
    void set_fresh(bool fresh) {
        model.set_fresh(fresh);
#if JC_JOG_PREVIEW
        real.fresh = fresh;
#endif
    }
    void refresh_profile() {
        profile = jc_profile_from_settings(settings);
        if (loader.state() == JcLoadState::Complete) jc_profile_apply_caps(profile, loader.caps());
#if JC_LIVE_JOG_TEST
        jc_profile_apply_test_limits(profile);
#endif
    }
    void advance(uint32_t ms) {
#if JC_JOG_PREVIEW
        for (uint32_t done = 0; done < ms; done += 10) {
            g_stub_millis += (ms - done < 10 ? ms - done : 10);
            jogs.set_clock(g_stub_millis);
            bridge.pump(jogs, real, g_stub_millis);
            jogs.tick(g_stub_millis);
        }
#else
        g_stub_millis += ms;
        jogs.set_clock(g_stub_millis);
        jogs.tick(g_stub_millis);
#endif
    }
    void status(const char* state, float x, float y, float z, float a, bool fresh = true) {
        JcLiveMachineStatus s {};
        s.has_data = true;
        strlcpy(s.machine_state, state, sizeof(s.machine_state));
        s.coordinate_reference = JcCoordinateReference::Machine;
        s.axes[0] = x; s.axes[1] = y; s.axes[2] = z; s.axes[3] = a;
        s.last_valid_ms = g_stub_millis;
        s.has_wco = true;   // FluidNC reports the work offset periodically; zero here so WORK == MACHINE
        s.sequence = ++seq;
        model.on_status(s);
        model.set_fresh(fresh);
        JcJogStatus v;
        v.fresh = fresh;
        static char kept_state[24];
        strlcpy(kept_state, s.machine_state, sizeof(kept_state));
        v.state = kept_state;
        v.sequence = s.sequence;
        v.pos_valid = true;
        memcpy(v.pos, s.axes, sizeof(v.pos));
#if JC_JOG_PREVIEW
        real = v;   // the session is fed by the bridge (real status + simulated displacement)
#else
        jogs.on_status(v, g_stub_millis);
#endif
    }

    // The real accepted baseline (2026-10-30): answered through the real loader.
    static std::string reply(const char* line) {
        struct Ax { const char *steps, *travel, *rate, *accel, *cycle, *dir, *neg; };
        static const Ax ax[4] = {
            { "40.000", "1000.000", "6000.000", "250.000", "1", "false", "gpio.41:low" },
            { "40.000", "2300.000", "6000.000", "250.000", "1", "false", "gpio.40:low" },
            { "200.000", "150.000", "1500.000", "100.000", "1", "true", "gpio.39:low" },
            { "1000.000", "3000.000", "1000.000", "25.000", nullptr, nullptr, "NO_PIN" } };
        if (strcmp(line, "$I") == 0) return "[VER:4.1 FluidNC v4.1.1 (esp32s3-wifi) :]\r\n[OPT:PH]\r\nok\r\n";
        if (strcmp(line, "$G") == 0) return "[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]\r\nok\r\n";
        if (strcmp(line, "$/probe/pin") == 0) return "$/probe/pin=gpio.48:low\r\nok\r\n";
        if (strcmp(line, "$/probe/toolsetter_pin") == 0) return "$/probe/toolsetter_pin=NO_PIN\r\nok\r\n";
        if (strcmp(line, "$/start/must_home") == 0) return "$/start/must_home=false\r\nok\r\n";
        const int a = static_cast<int>(strchr("xyza", line[7]) - "xyza");
        const std::string item = line + 9;
        const Ax& r = ax[a];
        const char* v = nullptr;
        if (item == "steps_per_mm") v = r.steps;
        else if (item == "max_travel_mm") v = r.travel;
        else if (item == "max_rate_mm_per_min") v = r.rate;
        else if (item == "acceleration_mm_per_sec2") v = r.accel;
        else if (item == "homing/cycle") v = r.cycle;
        else if (item == "homing/positive_direction") v = r.dir;
        else if (item == "soft_limits" || item == "motor0/hard_limits") v = "false";
        else if (item == "motor0/limit_neg_pin") v = r.neg;
        else if (item == "motor0/limit_pos_pin" || item == "motor0/limit_all_pin") v = "NO_PIN";
        else if (item == "homing/mpos_mm") v = a == 3 ? nullptr : "0.000";
        if (v == nullptr) return "error:3\r\n";
        return std::string(line) + "=" + v + "\r\nok\r\n";
    }
    void read_controller() {
        loader.start(g_stub_millis, nullptr);
        for (int i = 0; i < 2000 && loader.running(); ++i) {
            if (const char* q = loader.pending_line(g_stub_millis)) {
                const std::string r = reply(q);
                loader.mark_sent(g_stub_millis);
                size_t pos = 0;
                while (pos < r.size()) {
                    size_t e = r.find("\r\n", pos);
                    loader.on_line(r.substr(pos, e - pos).c_str(), g_stub_millis);
                    pos = e + 2;
                }
            }
            g_stub_millis += 20;
            loader.tick(g_stub_millis);
        }
        refresh_profile();
    }
};

#if JC_AUTHORITY
bool Rig::WfH::submit(const JcCommand& c, const char** why) {
    JcDispatchContext x = r->sv.dispatch_context(true, true);
    const size_t before = r->wire.size();
    const JcAuthDecision d = r->dispatcher.submit(c, x);
    if (d != JcAuthDecision::Allowed) { *why = jc_auth_name(d); return false; }
    if (r->wire.size() == before) { *why = "not transmitted"; return false; }
    return true;
}
bool Rig::WfH::query(const char* line) {
    std::string l = std::string(line) + "\n";
    return r->nc.send(l.data(), l.size());
}
void Rig::WfH::stop() { const char b = '!'; r->nc.send(&b, 1); }
#endif

// ---- frame output and checks ---------------------------------------------------------------------

static void write_ppm(const Arduino_GFX& g, const std::string& name) {
    if (g_outdir == nullptr) return;
    const std::string path = std::string(g_outdir) + "/" + g_tag + "_" + name + ".ppm";
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) { printf("cannot write %s\n", path.c_str()); ++g_problems; return; }
    fprintf(f, "P6\n%d %d\n255\n", Arduino_GFX::W, Arduino_GFX::H);
    for (int i = 0; i < Arduino_GFX::W * Arduino_GFX::H; ++i) {
        const uint16_t c = g.fb[i];
        const uint8_t rgb[3] = { static_cast<uint8_t>(((c >> 11) & 0x1F) * 255 / 31), static_cast<uint8_t>(((c >> 5) & 0x3F) * 255 / 63),
                                 static_cast<uint8_t>((c & 0x1F) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static bool intersects(const StubGlyph& a, const ui::Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}
static bool same_rect(const StubFill& f, const ui::Rect& r) { return f.x == r.x && f.y == r.y && f.w == r.w && f.h == r.h; }

// Checks the frame currently on the display. `dialog` = a modal dialog is open on top.
static void check_frame(Arduino_GFX& g, const std::string& name, bool dialog) {
    ++g_frames;
    std::vector<std::string> problems;
    char buf[200];

    if (g.oob > 0) { snprintf(buf, sizeof(buf), "%u text pixels fell outside the screen", g.oob); problems.push_back(buf); }

    int shown = 0;
    for (const StubGlyph& gl : g.glyphs) {
        if (gl.overlapped > 0 && shown < 3) {
            snprintf(buf, sizeof(buf), "text drawn over other text near x=%d y=%d (%u pixels)", gl.x, gl.y, gl.overlapped);
            problems.push_back(buf);
            ++shown;
        }
    }
    shown = 0;
    for (const StubFill& f : g.fills) {
        if (f.obscured_ink == 0) continue;
        if (dialog && same_rect(f, ui::kDialogBox)) continue;   // the dialog deliberately covers the page
        // Redrawing a control in place (new label inside the same rectangle) legitimately replaces
        // the old text. Text wiped out and NOT replaced is the defect: it is simply gone.
        bool replaced = false;
        for (size_t j = f.glyphs_before; j < g.glyphs.size() && !replaced; ++j) {
            const StubGlyph& gl = g.glyphs[j];
            replaced = gl.x >= f.x && gl.y >= f.y && gl.x + gl.w <= f.x + f.w && gl.y + gl.h <= f.y + f.h;
        }
        if (!replaced && shown++ < 3) {
            snprintf(buf, sizeof(buf), "a filled rectangle at x=%d y=%d w=%d h=%d wiped out %u text pixels without redrawing text", f.x, f.y, f.w, f.h, f.obscured_ink);
            problems.push_back(buf);
        }
    }
    if (dialog) {
        // Glyphs drawn between the box fill and the first button fill are the title and the lines.
        size_t first_button = g.glyphs.size();
        bool box_seen = false;
        for (const StubFill& f : g.fills) {
            if (same_rect(f, ui::kDialogBox)) box_seen = true;
            if (box_seen && (same_rect(f, ui::kDialogCancel) || same_rect(f, ui::kDialogConfirm) || same_rect(f, ui::kDialogOk))) {
                first_button = f.glyphs_before;
                break;
            }
        }
        size_t box_glyph_start = 0;
        for (const StubFill& f : g.fills) if (same_rect(f, ui::kDialogBox)) box_glyph_start = f.glyphs_before;
        const ui::Rect inner { static_cast<int16_t>(ui::kDialogBox.x + 4), static_cast<int16_t>(ui::kDialogBox.y + 4),
                               static_cast<int16_t>(ui::kDialogBox.w - 8), static_cast<int16_t>(ui::kDialogBox.h - 8) };
        for (size_t i = box_glyph_start; i < first_button && i < g.glyphs.size(); ++i) {
            const StubGlyph& gl = g.glyphs[i];
            if (gl.x < inner.x || gl.y < inner.y || gl.x + gl.w > inner.x + inner.w || gl.y + gl.h > inner.y + inner.h) {
                snprintf(buf, sizeof(buf), "dialog text leaves the dialog box near x=%d y=%d", gl.x, gl.y);
                problems.push_back(buf);
                break;
            }
            for (const ui::Rect& b : { ui::kDialogCancel, ui::kDialogConfirm, ui::kDialogOk }) {
                if (intersects(gl, b)) {
                    snprintf(buf, sizeof(buf), "dialog text touches a button near x=%d y=%d", gl.x, gl.y);
                    problems.push_back(buf);
                    break;
                }
            }
        }
    }

    write_ppm(g, name);
    if (problems.empty()) {
        printf("  ok       %s\n", name.c_str());
    } else {
        ++g_problems;
        printf("  PROBLEM  %s\n", name.c_str());
        for (const std::string& p : problems) printf("             - %s\n", p.c_str());
    }
}

static void touch_center(JcScreen& s, const ui::Rect& r) { s.touch(r.x + r.w / 2, r.y + r.h / 2); }

// ---- pure layout checks (no drawing) ----------------------------------------------------------------

static void check(bool cond, const char* what) {
    printf("  %s  %s\n", cond ? "ok      " : "PROBLEM ", what);
    if (!cond) ++g_problems;
}

static void pure_checks() {
    printf("[pure layout checks]\n");
    // Dialog capacity: the last of kMaxLines lines must end above the buttons, and one more would not.
    const int lh = font_line_height(FontId::Body16);
    const int last_bottom = ui::kDialogBox.y + ui::kDialogFirstLineY + ui::kDialogLineStep * (ui::Dialog::kMaxLines - 1) + lh;
    check(last_bottom <= ui::kDialogConfirm.y - 2, "the dialog's last allowed line ends above the buttons");
    check(ui::kDialogBox.y + ui::kDialogFirstLineY + ui::kDialogLineStep * ui::Dialog::kMaxLines + lh > ui::kDialogConfirm.y - 2,
          "an extra line would touch the buttons (so kMaxLines is the true capacity)");
    check(lh <= ui::kDialogLineStep, "the dialog line spacing is at least one text line high");

    // Every page title leaves room for the LIVE JOG TEST BUILD banner, which stays clear of the chip.
    bool all_fit = true, none_overlap = true;
    for (int p = 0; p < static_cast<int>(JcPage::Count); ++p) {
        ui::Rect box {};
        const char* text = nullptr;
        const char* title = jc_page_title(static_cast<JcPage>(p));
        if (!ui::live_jog_banner(title, box, text)) { all_fit = false; printf("             no banner room beside \"%s\"\n", title); continue; }
        if (box.x < 18 + text_width(FontId::Heading24, title) || box.x + box.w > ui::kHeaderChipLeft) none_overlap = false;
    }
    check(all_fit, "the banner fits beside every page title");
    check(none_overlap, "the banner never overlaps a title or the mode chip");
    ui::Rect box {};
    const char* text = nullptr;
    check(ui::live_jog_banner("JOGGING", box, text) && strcmp(text, JC_JOG_PREVIEW ? "PREVIEW ONLY - MOTION DISABLED" : "LIVE JOG TEST BUILD") == 0,
          "the full banner text is used where it fits (JOGGING)");
    check(ui::live_jog_banner("CONTROLLER CAPABILITIES", box, text), "a shorter banner is chosen for the longest title");

    // The dashboard banner (x 584..) fits in the 800 px header.
    check(584 + text_width(FontId::Body16, JC_JOG_PREVIEW ? "PREVIEW ONLY" : "LIVE JOG TEST BUILD") + 16 <= 796, "the dashboard header banner fits");
#if JC_JOG_PREVIEW
    // Simulated-result lines carry a "SIM: " prefix; the worst case must still fit the 650 px slot.
    {
        const char* const worst[] = {
            "SIM: COMPLETED (X): moved +1.000 (asked +1.000)", "SIM: POSITION MISMATCH (X): moved -10.000, asked +1.000",
            "SIM: CANCEL NOT CONFIRMED - USE THE E-STOP", "SIM: cancel sent; waiting for the controller to report Idle",
            "SIM: ACCEPTED by the controller; motion not finished yet", "SIM: NO REPLY (timeout), cancel sent",
            "SIM: UNEXPECTED MOTION, cancel sent", "SIM: REJECTED by controller (code 15)" };
        bool fit = true;
        for (const char* w : worst) if (text_width(FontId::Body16, w) > 650 - 6) { fit = false; printf("             too wide: %s\n", w); }
        check(fit, "every simulated result line fits its 650 px slot");
    }
    check(text_width(FontId::Body16, "Firmware: PREVIEW ONLY - MOTION DISABLED (jog is simulated)") <= 780,
          "the settings firmware note fits the screen width");
#endif

    // Worst-case strings against their containers.
    {   // Instruction 011A: the new GO TO wait-step labels must fit the progress line (no clipping)
        const char* const labels[] = { "Step 4 of 8: CONFIRM: Z to work Z0 (active WCS)", "Step 8 of 12: XY reached. CONFIRM to lower to work Z0" };
        bool fits = true;
        for (const char* l : labels) if (text_width(FontId::Body16, l) > 744) { fits = false; printf("             too wide: %s\n", l); }
        check(fits, "the new GO TO wait-step labels fit the progress line");
    }
    {   // the RESET dialog on the Files page (Instruction 010A): every line must fit, none may be clipped
        const char* const reset_lines[] = { kJcJobStopText, "The job will NOT continue and cannot be resumed from this pendant.",
                                            "Motion stops; an alarm, unlock or re-homing may follow.",
                                            "This is different from FEED HOLD, which only pauses.", "It is NOT a replacement for the emergency stop." };
        bool fits = true;
        for (const char* l : reset_lines) if (text_width(FontId::Body16, l) > ui::kDialogBox.w - ui::kDialogTextLeft - 20) { fits = false; printf("             too wide: %s\n", l); }
        check(fits, "every line of the RESET dialog fits the dialog width");
    }
    check(text_width(FontId::Body16, "Keep E-stop / power removal in reach. Not 'safe': 1 mm only.") <= ui::kDialogBox.w - ui::kDialogTextLeft - 20,
          "the longest arming-dialog line fits the dialog width");
    check(text_width(FontId::Body16, "NO COMMUNICATION: software cancel unreliable. USE E-STOP") <= 650 - 6,
          "the no-communication message fits its 650 px slot");
    char lines[2][96];
    JcMachineCaps none;
    jc_limits_summary(none, lines, 2);
    check(text_width(FontId::Body16, lines[0]) <= 760 - 6, "the Limits summary (worst case: everything unknown) fits its slot");

    // Dashboard geometry (JcDashLayout.hpp is what PendantUi draws AND hit-tests).
    {
        auto hit = [](const ui::Rect& a, const ui::Rect& b) { return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y; };
        const ui::Rect banner_area { dl::kBannerLeft, 13, static_cast<int16_t>(dl::kBannerRight - dl::kBannerLeft), 38 };
        const ui::Rect title_area { dl::kTitleX, 8, static_cast<int16_t>(dl::kTitleMaxRight - dl::kTitleX), 48 };
        std::vector<ui::Rect> header = { title_area, dl::kModeChip, dl::kLinkChip, banner_area, dl::kMenuButton, dl::kLinkButton };
        bool overlap = false, onscreen = true;
        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i].x < 0 || header[i].x + header[i].w > 800 || header[i].y + header[i].h > 64) onscreen = false;
            for (size_t j = i + 1; j < header.size(); ++j) if (hit(header[i], header[j])) overlap = true;
        }
        check(!overlap, "dashboard header: title, mode chip, link chip, banner, MENU and LINK never overlap");
        check(onscreen, "dashboard header controls stay inside the 800 px by 64 px header band");
        check(!hit(dl::kHomeButton, banner_area) && !hit(dl::kHomeButton, dl::kLinkChip), "the HOME button of the other pages clears the chips and banner");
        const char* const titles[] = { "FLUID DIAL", "CONNECTION", "WI-FI / FLUIDNC", "SETTINGS" };
        bool titles_fit = true;
        for (const char* ti : titles) if (dl::kTitleX + text_width(FontId::Heading24, ti) > dl::kTitleMaxRight) { titles_fit = false; printf("             title too wide: %s\n", ti); }
        check(titles_fit, "every header title ends before the mode chip");
        bool chips_fit = true;
        for (const char* s : { "LIVE", "LIVE STALE", "DEMO MODE" }) if (text_width(FontId::Body16, s) > dl::kModeChip.w - 8) { chips_fit = false; printf("             mode label too wide: %s\n", s); }
        for (const char* s : { "CNC OFFLINE", "FLUIDNC LINK", "DISCONNECTED", "WIFI CONNECTING", "FLUIDNC CONNECTING", "CONNECTED", "RECONNECTING", "STALE" })
            if (text_width(FontId::Body16, s) > dl::kLinkChip.w - 8) { chips_fit = false; printf("             link label too wide: %s\n", s); }
        check(chips_fit, "every mode and link label fits its chip");
#if JC_JOG_PREVIEW
        const char* const banners[] = { "PREVIEW ONLY - MOTION DISABLED", "PREVIEW - NO MOTION", "PREVIEW ONLY", "PREVIEW" };
#else
        const char* const banners[] = { "LIVE JOG TEST BUILD", "LIVE JOG TEST", "LIVE JOG" };
#endif
        bool banner_fits = false;
        for (const char* b : banners) if (text_width(FontId::Body16, b) + 16 <= banner_area.w) { banner_fits = true; break; }
        check(banner_fits, "a build banner wording fits beside the link chip on the dashboard");
        std::vector<ui::Rect> body;
        for (uint8_t i = 0; i < 4; ++i) body.push_back(dl::axis_card(i));
        body.push_back(dl::kStatusStrip);
        body.push_back(dl::kLowerPanel);
        bool body_overlap = false;
        for (size_t i = 0; i < body.size(); ++i) for (size_t j = i + 1; j < body.size(); ++j) if (hit(body[i], body[j])) body_overlap = true;
        check(!body_overlap, "dashboard: the four coordinate cards, status strip and lower panel do not overlap");
        bool header_clear = true;
        for (const ui::Rect& r : body) for (const ui::Rect& h : header) if (hit(r, h)) header_clear = false;
        check(header_clear, "MENU, LINK and the other header controls never overlap a coordinate card or panel");
        check(hit(dl::kStatusStrip, dl::kReferenceButton) && dl::kReferenceButton.x + dl::kReferenceButton.w <= dl::kStatusStrip.x + dl::kStatusStrip.w,
              "the MACHINE / WORK switch sits inside the status strip");
        std::vector<ui::Rect> cells;
        for (uint8_t i = 0; i < 6; ++i) { cells.push_back(dl::info_label(i)); cells.push_back(dl::info_value(i)); }
        cells.push_back(dl::kDiagLine1);
        cells.push_back(dl::kDiagLine2);
        bool cells_ok = true;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (cells[i].x < dl::kLowerPanel.x || cells[i].x + cells[i].w > dl::kLowerPanel.x + dl::kLowerPanel.w
                || cells[i].y < dl::kLowerPanel.y || cells[i].y + cells[i].h > dl::kLowerPanel.y + dl::kLowerPanel.h) cells_ok = false;
            for (size_t j = i + 1; j < cells.size(); ++j) if (hit(cells[i], cells[j])) cells_ok = false;
            if (hit(cells[i], dl::kFooter)) cells_ok = false;
        }
        check(cells_ok, "status cells and diagnostics lie inside the lower panel and overlap neither each other nor the footer");
        check(dl::kFooter.y + dl::kFooter.h <= 480, "the footer is on screen");
        check(text_width(FontId::Dro18, "-9999.999") + 4 + text_width(FontId::Button20, "mm") <= 167 - 4, "a four-digit coordinate fits its card in the smallest DRO font");
        char d[160];
        snprintf(d, sizeof(d), "Endpoint: %.28s:%u   Age: %s", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", 65535u, "4294967295 ms");
        check(text_width(FontId::Body16, d) <= dl::kDiagLine1.w - 4, "the endpoint / status-age line fits at its longest");
        check(text_width(FontId::Body16, "Jog rehearsal: MENU > JOGGING.  PREVIEW ONLY - MOTION DISABLED.") <= dl::kDiagLine2.w - 4 &&
              text_width(FontId::Body16, "Restricted X/Y jog: MENU > JOGGING.  LIVE JOG TEST BUILD.") <= dl::kDiagLine2.w - 4, "the dashboard mode line fits");
        check(dl::kMenuButton.w >= 72 && dl::kLinkButton.w >= 72 && dl::kMenuButton.h >= 40 && dl::kModeChip.h >= 36, "MENU, LINK and the mode chip are large enough to touch");
    }

    // Jog page geometry: no two interactive regions overlap; every region lies on the 800x480 screen below the header.
    {
        std::vector<ui::Rect> btn;
        for (uint8_t i = 0; i < 4; ++i) btn.push_back(jl::axis_rect(i));
        for (uint8_t i = 0; i < kJcStepPresets; ++i) btn.push_back(jl::step_rect(i));
        for (uint8_t i = 0; i < kJcFeedPresets; ++i) btn.push_back(jl::feed_rect(i));
        for (const ui::Rect& r : { jl::kIncremental, jl::kArm, jl::kJogNeg, jl::kJogStop, jl::kJogPos, jl::kDro }) btn.push_back(r);
        auto hit = [](const ui::Rect& a, const ui::Rect& b) { return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y; };
        bool overlap = false, onscreen = true;
        for (size_t i = 0; i < btn.size(); ++i) {
            if (btn[i].x < 0 || btn[i].y < 64 || btn[i].x + btn[i].w > 800 || btn[i].y + btn[i].h > 480) onscreen = false;
            for (size_t j = i + 1; j < btn.size(); ++j) if (hit(btn[i], btn[j])) overlap = true;
        }
        check(!overlap, "no two jog-page controls overlap");
        check(onscreen, "every jog-page control is on screen below the header");
        // Panel content stays inside the panel and the rows do not touch each other or the bottom region.
        const ui::Rect rows[] = { jl::kPreviewSlot, jl::kResultSlot, jl::kMotionSlot, jl::kCapsSlot, jl::kWarnRect };
        bool rows_ok = true;
        for (size_t i = 0; i < 5; ++i) {
            if (rows[i].y < jl::kPanelRect.y || rows[i].y + rows[i].h > jl::kPanelRect.y + jl::kPanelRect.h) rows_ok = false;
            for (size_t j = i + 1; j < 5; ++j) if (hit(rows[i], rows[j])) rows_ok = false;
        }
        check(rows_ok, "panel rows are inside the panel and do not overlap");
        check(!hit(jl::kAck, jl::kCapsSlot) && !hit(jl::kAck, jl::kMotionSlot), "the ACKNOWLEDGE button does not overlap panel rows");
        bool above = true;
        for (const ui::Rect& r : btn) if (hit(r, jl::kPanelRect)) above = false;
        check(above, "no control reaches into the status panel");
        // Worst-case labels fit their buttons with the same 20 px margin the button code uses.
        const struct { const char* s; ui::Rect r; } labels[] = {
            { "READ SETTINGS", jl::kArm }, { "READING 55/55", jl::kArm }, { "RETRY READ", jl::kArm }, { "ARM PREVIEW", jl::kArm },
            { "ARMED 120s", jl::kArm }, { "NOT AVAILABLE", jl::kArm }, { "INCREMENTAL", jl::kIncremental },
            { "Z  ON HOLD", jl::kJogNeg }, { "CANCEL JOG", jl::kJogStop }, { "100 /min", jl::feed_rect(0) }, { "1000 /min", jl::feed_rect(2) } };
        bool fit = true;
        for (const auto& l : labels) {
            if (text_width(FontId::Button20, l.s) > l.r.w - 20 && text_width(FontId::Body16, l.s) > l.r.w - 20) { fit = false; printf("             label too wide: %s\n", l.s); }
        }
        check(fit, "every jog-page button label fits its button");
        // The longest capability status line fits its row (single line).
        char t1[160];
        JcCapsFlow cf;
        cf.phase = JcCapsPhase::Failed;
        cf.can_read = true;
        jc_caps_status_text(cf, 55, 55, JcLoadFailure::TimedOut, 0, t1, sizeof(t1));
        char t2[200];
        snprintf(t2, sizeof(t2), "REAL controller: %s", t1);
        check(text_width(FontId::Body16, t2) <= jl::kCapsSlot.w - 4, "the capability status line fits its row");
        cf.phase = JcCapsPhase::Expired;
        jc_caps_status_text(cf, 0, 55, JcLoadFailure::None, 999 * 60000u, t1, sizeof(t1));
        snprintf(t2, sizeof(t2), "REAL controller: %s", t1);
        check(text_width(FontId::Body16, t2) <= jl::kCapsSlot.w - 4, "the capability expiry line fits its row");
    }

    // The line under the jog panel must be ONE line (a second line overflows the panel).
    check(text_width(FontId::Body16, kJcLiveJogWarning) <= 744 - 4, "the live-jog warning under the panel fits one line");
    // The capabilities note must fit its two lines (768 px wide, 22 px apart, ending above y 480).
    // 2 lines x ~94 characters; measured by wrapping the same words.
    {
        const char* note = "Read-only: nothing is changed on the controller. Values are discarded if the connection "
                           "is lost. A's rotary/degrees cannot be read; it is set in Settings. Limits: LIMITS / HOMING.";
        int lines_needed = 1, width = 0;
        const int space = text_width(FontId::Body16, " ");
        char word[64];
        const char* p = note;
        while (*p) {
            int n = 0;
            while (*p && *p != ' ' && n < 63) word[n++] = *p++;
            word[n] = '\0';
            while (*p == ' ') ++p;
            const int w = text_width(FontId::Body16, word);
            if (width > 0 && width + space + w > 768) { ++lines_needed; width = w; } else { width += (width ? space : 0) + w; }
        }
        check(lines_needed <= 2, "the capabilities page note fits two lines above the screen edge");
    }

    // The one-line jog status never gets truncated or wider than its slot, for every combination.
    {
        const char* const motions[3] = { "UNKNOWN (no live data)", "ACTIVE", "ALARM" };
        const char* const gates[3] = { "BLOCKED", "UNPROTECTED MICRO-JOG", "PROTECTED" };
        int widest = 0;
        bool truncated = false;
        for (const char* m : motions) {
            for (int ph = 0; ph <= static_cast<int>(JcJogPhase::Recovering); ++ph) {
                for (const char* gt : gates) {
                    char line[96];
                    jc_jog_motion_line(line, sizeof(line), m, "Hold:0", static_cast<JcJogPhase>(ph), gt);
                    if (strstr(line, gt) == nullptr) truncated = true;
                    const int w = text_width(FontId::Body16, line);
                    if (w > widest) widest = w;
                }
            }
        }
        check(!truncated, "the jog status line is never truncated");
        check(widest <= 650 - 6, "the jog status line fits its 650 px slot in every state");
    }
}

// ---- scenarios --------------------------------------------------------------------------------------------

static void detector_self_test();

// ---- Instruction 010: SD files and jobs, tools, BLE ------------------------------------------------------------

struct MockRadio : JcBleRadio {
    JcBleUart* link = nullptr;
    bool scan_start(uint32_t) override {
        link->on_scan_result("AA:BB:CC:DD:EE:01", "HM-10", -58, true, g_stub_millis);
        link->on_scan_result("AA:BB:CC:DD:EE:02", "JC-Bridge-with-a-very-long-name", -71, true, g_stub_millis);
        link->on_scan_result("11:22:33:44:55:66", "Phone", -40, false, g_stub_millis);
        link->on_scan_result("11:22:33:44:55:67", "", -80, false, g_stub_millis);
        link->on_scan_result("11:22:33:44:55:68", "Headset", -85, false, g_stub_millis);
        return true;
    }
    void scan_stop() override {}
    bool connect(const char*) override { link->on_connected(true, g_stub_millis); return true; }
    void disconnect() override {}
    bool discover() override {
        JcBleDiscovery d; memset(&d, 0, sizeof(d));
        d.n = 1; strcpy(d.services[0].uuid, "ffe0"); d.services[0].nchars = 1;
        strcpy(d.services[0].chars[0].uuid, "ffe1"); d.services[0].chars[0].props = kBleNotify | kBleWriteNoRsp;
        link->on_discovery(d, g_stub_millis);
        return true;
    }
    bool subscribe(const JcBleSelection&) override { link->on_subscribed(true, g_stub_millis); return true; }
    bool write(const JcBleSelection&, const uint8_t*, size_t) override { return true; }
    uint16_t mtu() const override { return 185; }
};

// Hitboxes: every rectangle must lie on the 800x480 screen, be at least 38 px in both directions, not overlap
// another, and be the very rectangle the page drew a control in (a fill with exactly that rectangle).
static void check_hitboxes(const Arduino_GFX& g, const char* name, const std::vector<ui::Rect>& boxes) {
    bool inside = true, big = true, disjoint = true, drawn = true;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const ui::Rect& a = boxes[i];
        if (a.x < 0 || a.y < 0 || a.x + a.w > ui::kWidth || a.y + a.h > ui::kHeight) inside = false;
        if (a.w < 38 || a.h < 38) big = false;
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const ui::Rect& b = boxes[j];
            if (a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y) disjoint = false;
        }
        bool found = false;
        for (const StubFill& f : g.fills) if (same_rect(f, a)) { found = true; break; }
        if (!found) { drawn = false; printf("             no control drawn at x=%d y=%d w=%d h=%d\n", a.x, a.y, a.w, a.h); }
    }
    char buf[120];
    snprintf(buf, sizeof(buf), "%s: hitboxes inside 800x480", name); check(inside, buf);
    snprintf(buf, sizeof(buf), "%s: every hitbox is at least 38 px", name); check(big, buf);
    snprintf(buf, sizeof(buf), "%s: hitboxes do not overlap", name); check(disjoint, buf);
    snprintf(buf, sizeof(buf), "%s: hitboxes are the rectangles actually drawn (shared geometry)", name); check(drawn, buf);
}

static JcSimNc* g_scene = nullptr;
static bool scene_query(void*, const char* line) {
    if (g_scene == nullptr || !jc_query_allowed(line)) return false;
    const std::string l = std::string(line) + "\n";
    return g_scene->send(l.data(), l.size());
}
static void scene_run(Rig& r, JcSimNc& nc, uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 10) {
        r.advance(10);
        nc.update(g_stub_millis);
        char line[200];
        while (nc.pop_line(line, sizeof(line))) { r.sd.on_line(line, g_stub_millis); r.tool_reader.on_line(line); }
        r.sd.service(g_stub_millis);
        r.tool_reader.service(g_stub_millis);
    }
}

static void pages_010() {
    printf("[010: files and jobs]\n");
    {
        Rig r;
        r.status("Idle", 50, 50, -5, 0);
        r.advance(500);
        JcSimNc nc;
        g_scene = &nc;
        r.sd.set_query(&scene_query, nullptr);
        r.tool_reader.set_query(&scene_query, nullptr);
        JcFilesScreen fs(r.sv);
        fs.enter();
        check_frame(r.gfx, "files_idle", false);
        check_hitboxes(r.gfx, "files page", { ui::Rect { 560, 68, 224, 40 }, ui::Rect { 16, 110, 86, 38 }, ui::Rect { 724, 152, 60, 78 },
                                              ui::Rect { 724, 234, 60, 78 }, ui::Rect { 16, 412, 376, 60 }, ui::Rect { 408, 412, 376, 60 } });
        // listing
        nc.add_file("/jobs/first_part_program.nc", 18432);
        nc.add_file("/jobs/a_really_long_part_program_name_for_the_rotary_axis_fixture_number_12_final_v3.nc", 5u * 1024u * 1024u);
        nc.add_file("/jobs/odd#name.nc", 999);
        nc.add_file("/jobs/deep/sub/folder/x.nc", 7);
        nc.add_file("/readme.txt", 120);
        for (int i = 0; i < 9; ++i) { char p[48]; snprintf(p, sizeof(p), "/jobs/part_%02d.gcode", i); nc.add_file(p, 1000u * static_cast<uint32_t>(i + 1)); }
        touch_center(fs, ui::Rect { 560, 68, 224, 40 });                 // REFRESH SD LIST
        check(r.host.sd_refreshes == 1, "REFRESH asks the application for the read-only listing");
        r.sd.refresh(g_stub_millis);                                     // the application would start it
        scene_run(r, nc, 120);
        fs.tick(g_stub_millis);
        check_frame(r.gfx, "files_reading", false);
        scene_run(r, nc, 3000);
        fs.tick(g_stub_millis + 400);
        fs.enter();
        check(r.sd.state() == JcSdState::Ready, "the listing is complete");
        check_frame(r.gfx, "files_root", false);
        touch_center(fs, ui::Rect { 16, 152, 700, 38 });                  // the folder row (directories first)
        check(strcmp(r.sd.cwd(), "/jobs") == 0, "tapping a folder enters it");
        check_frame(r.gfx, "files_folder", false);
        touch_center(fs, ui::Rect { 16, 192, 700, 38 });                  // second entry
        touch_center(fs, ui::Rect { 16, 232, 700, 38 });
        check_frame(r.gfx, "files_selected", false);
        touch_center(fs, ui::Rect { 724, 234, 60, 78 });                  // page down
        check_frame(r.gfx, "files_page_down", false);
        touch_center(fs, ui::Rect { 724, 234, 60, 78 });
        touch_center(fs, ui::Rect { 724, 152, 60, 78 });                  // page up
        check_frame(r.gfx, "files_page_up", false);
        touch_center(fs, ui::Rect { 16, 110, 86, 38 });                   // UP
        check(strcmp(r.sd.cwd(), "/") == 0, "UP leaves the folder");
        // 010A: selecting a file only shows it; there is no START anywhere on this page
        r.sd.enter(0);
        fs.enter();
        for (uint16_t i = 0; i < r.sd.visible_count(); ++i) if (strstr(r.sd.visible(i)->path, "odd#name") != nullptr) r.sd.select(i);
        fs.enter();
        check_frame(r.gfx, "files_odd_name_selected", false);
        for (uint16_t i = 0; i < r.sd.visible_count(); ++i) if (strstr(r.sd.visible(i)->path, "first_part") != nullptr) r.sd.select(i);
        fs.enter();
        check_frame(r.gfx, "files_selected_monitor_only", false);
        {   // sweep the whole screen with touches: nothing may ever put a program start or resume on the wire
            const size_t w0 = r.wire.size();
            for (int y = 8; y < 480; y += 20) {
                for (int x = 8; x < 800; x += 24) {
                    if (y > 66 && y < 114 && x > 556) continue;   // REFRESH is read-only and covered above
                    fs.touch(static_cast<uint16_t>(x), static_cast<uint16_t>(y));
                }
            }
            check(r.wire.find("$SD/Run") == std::string::npos && r.wire.find("~") == std::string::npos
                  && r.wire.find("$Localfs") == std::string::npos, "touching every part of the Files page never sends a start or resume");
            (void)w0;
        }
        // job states, all from the controller's own report
        const struct { const char* state; bool sd; float pct; JcJobPhase expect; const char* name; } steps[] = {
            { "Run", true, 12.5f, JcJobPhase::Running, "files_job_running" },
            { "Hold:0", true, 12.5f, JcJobPhase::Paused, "files_job_paused" },
            { "Run", true, 99.8f, JcJobPhase::Running, "files_job_end" },
            { "Idle", false, 0, JcJobPhase::Complete, "files_job_complete" },
        };
        for (const auto& st : steps) {
            r.job.on_status(true, st.state, st.sd, st.pct, "/jobs/first_part_program.nc");
            r.status(st.state, 50, 50, -5, 0);
            fs.enter();
            check(r.job.phase() == st.expect, st.name);
            check_frame(r.gfx, st.name, false);
        }
        r.job.on_status(false, nullptr, false, 0, nullptr);
        r.job.on_status(true, "Run", true, 40.0f, "/jobs/x.nc");
        r.job.on_status(false, nullptr, false, 0, nullptr);
        fs.enter();
        check(r.job.phase() == JcJobPhase::Unknown, "a lost connection leaves the job state unknown");
        check_frame(r.gfx, "files_job_unknown", false);
        r.host.simulated_job = true;
        r.job.on_status(true, "Run", true, 30.0f, "/jobs/x.nc");
        fs.enter();
        check_frame(r.gfx, "files_job_simulated", false);
        r.host.simulated_job = false;
        r.job.on_status(true, "Alarm", true, 40.0f, "/jobs/x.nc");
        fs.enter();
        check_frame(r.gfx, "files_job_alarm", false);
        // feed hold is immediate; reset needs a confirmation that spells out its consequences
        r.job.on_status(true, "Hold:0", true, 40.0f, "/jobs/x.nc");
        r.status("Hold:0", 50, 50, -5, 0);
        fs.enter();
#if JC_AUTHORITY
        const size_t wire0 = r.wire.size();
        touch_center(fs, ui::Rect { 16, 412, 376, 60 });                  // FEED HOLD
        check(r.wire.size() > wire0 && r.wire[r.wire.size() - 1] == '!', "FEED HOLD sends '!' at once, without a dialog");
        touch_center(fs, ui::Rect { 408, 412, 376, 60 });                 // RESET
        check_frame(r.gfx, "files_reset_confirm", true);
        touch_center(fs, ui::kDialogCancel);
        check(r.wire.find("~") == std::string::npos && r.wire.find("") == std::string::npos, "cancelling the dialog sent neither '~' nor reset");
#endif
        // card problems
        Rig m;
        m.status("Idle", 50, 50, -5, 0);
        m.advance(500);
        JcSimNc nc2;
        nc2.sd_present = false;
        g_scene = &nc2;
        m.sd.set_query(&scene_query, nullptr);
        JcFilesScreen ms(m.sv);
        ms.enter();
        m.sd.refresh(g_stub_millis);
        scene_run(m, nc2, 2000);
        ms.enter();
        check(m.sd.state() == JcSdState::NoCard, "a missing card is reported, not invented");
        check_frame(m.gfx, "files_no_card", false);
        nc2.sd_present = true;
        m.sd.refresh(g_stub_millis);
        scene_run(m, nc2, 2000);
        ms.enter();
        check_frame(m.gfx, "files_empty", false);
        nc2.sd_list_error = 62;
        m.sd.refresh(g_stub_millis);
        scene_run(m, nc2, 2000);
        ms.enter();
        check_frame(m.gfx, "files_error", false);
        Rig big;
        big.status("Idle", 50, 50, -5, 0);
        big.advance(500);
        JcSimNc nc3;
        for (int i = 0; i < 210; ++i) { char p[48]; snprintf(p, sizeof(p), "/f%03d.nc", i); nc3.add_file(p, static_cast<uint32_t>(i) * 100u); }
        g_scene = &nc3;
        big.sd.set_query(&scene_query, nullptr);
        JcFilesScreen bs(big.sv);
        bs.enter();
        big.sd.refresh(g_stub_millis);
        scene_run(big, nc3, 20000);
        bs.enter();
        check(big.sd.overflow(), "a very large card is bounded and flagged");
        check_frame(big.gfx, "files_large", false);
        g_scene = nullptr;
    }

    printf("[010: tools]\n");
    {
        Rig t;
        t.status("Idle", 50, 50, -5, 0);
        t.advance(500);
        JcSimNc nc;
        nc.tool = 3; nc.tlo = 2.5f; nc.tlo_active = true;
        g_scene = &nc;
        t.tool_reader.set_query(&scene_query, nullptr);
        JcToolsScreen ts(t.sv);
        ts.enter();
        check_frame(t.gfx, "tools_unread", false);
        std::vector<ui::Rect> boxes;
        for (int i = 0; i < 6; ++i) boxes.push_back(ui::Rect { 416, static_cast<int16_t>(72 + i * 66), 368, 58 });
        check_hitboxes(t.gfx, "tools page", boxes);
        touch_center(ts, boxes[1]);                                       // before the state is read
        check_frame(t.gfx, "tools_read_first", true);
        touch_center(ts, ui::kDialogOk);
        touch_center(ts, boxes[0]);
        check(t.host.tool_reads == 1, "READ TOOL STATE asks for the two read-only queries");
        t.tool_reader.start(g_stub_millis);
        scene_run(t, nc, 600);
        ts.tick(g_stub_millis + 400);
        ts.enter();
        check(t.tool_reader.state() == JcToolReadState::Done && t.tool_reader.info().tool == 3, "the T word and [TLO:] come from the controller");
        check_frame(t.gfx, "tools_read", false);
        touch_center(ts, boxes[2]);                                       // change + measure without a reference
        check_frame(t.gfx, "tools_need_reference", true);
        touch_center(ts, ui::kDialogOk);
        t.tool_store.set_ref(-20.0f, 5.0f, -1);
        ts.enter();
        check_frame(t.gfx, "tools_with_reference", false);
#if JC_AUTHORITY
        touch_center(ts, boxes[2]);
        check(t.host.opened == JcWfKind::ToolChange, "CHANGE TOOL opens the guided routine, nothing moves");
        touch_center(ts, boxes[4]);                                       // CLEAR TOOL OFFSET
        check_frame(t.gfx, "tools_clear_confirm", true);
        check(t.wire.empty(), "nothing is sent before the confirmation");
        touch_center(ts, ui::kDialogConfirm);
        check(t.host.clears == 1, "the confirmed clear is handed to the application");
#endif
        t.tool_store.invalidate("connection lost");
        ts.enter();
        check_frame(t.gfx, "tools_reference_discarded", false);
        g_scene = nullptr;
    }
#if JC_AUTHORITY
    printf("[010: tool routines]\n");
    {
        Rig w;
        w.status("Idle", 50, 50, -5, 0);
        w.advance(500);
        w.nc.m[0] = 50; w.nc.m[1] = 50; w.nc.m[2] = -5;
        JcWorkflowScreen wfs(w.sv);
        wfs.preselect(JcWfKind::ToolReference, w.tool_store);
        wfs.enter();
        check_frame(w.gfx, "toolref_setup", false);
        touch_center(wfs, ui::Rect { 198, 408, 586, 52 });                // PREPARE
        w.wf_run(600);
        check(w.wf.state() == JcWfState::Planned, "the reference routine is planned");
        wfs.tick(g_stub_millis);
        check_frame(w.gfx, "toolref_preview", false);
        touch_center(wfs, ui::Rect { 258, 408, 250, 52 });                // START
        check_frame(w.gfx, "toolref_start_confirm", true);
        touch_center(wfs, ui::kDialogConfirm);
        w.wf_run(1500);
        wfs.tick(g_stub_millis);
        check_frame(w.gfx, "toolref_wait_touch", false);
        check(w.wire.find("G38.2") == std::string::npos, "no probing move before the contact has been verified by hand");
        strcpy(w.nc.pins, "P"); w.wf_run(500);
        wfs.tick(g_stub_millis + 300);
        w.nc.pins[0] = '\0'; w.wf_run(500);
        for (int i = 0; i < 40 && w.wf.state() == JcWfState::Running; ++i) w.wf_run(500);
        wfs.tick(g_stub_millis + 300);
        check(w.wf.state() == JcWfState::Done && w.wf.result().tool_contact_ok, "the reference contact was measured on the simulated plate");
        check_frame(w.gfx, "toolref_done", false);
        w.tool_store.set_ref(w.wf.result().tool_contact_mz, 5.0f, -1);

        Rig m;
        m.status("Idle", 50, 50, -5, 0);
        m.advance(500);
        m.nc.m[0] = 50; m.nc.m[1] = 50; m.nc.m[2] = -5;
        m.tool_store.set_ref(-20.0f, 5.0f, -1);
        JcWorkflowScreen ms(m.sv);
        ms.preselect(JcWfKind::ToolMeasure, m.tool_store);
        ms.enter();
        check_frame(m.gfx, "toolmeasure_setup", false);
        touch_center(ms, ui::Rect { 198, 408, 586, 52 });
        m.wf_run(600);
        touch_center(ms, ui::Rect { 258, 408, 250, 52 });
        touch_center(ms, ui::kDialogConfirm);
        m.wf_run(1500);
        strcpy(m.nc.pins, "P"); m.wf_run(500); m.nc.pins[0] = '\0'; m.wf_run(500);
        for (int i = 0; i < 40 && m.wf.state() == JcWfState::Running; ++i) m.wf_run(500);
        ms.tick(g_stub_millis + 300);
        check(m.wf.state() == JcWfState::Done && m.wf.result().tlo_ok, "the tool length offset is proposed");
        check(m.wire.find("G43") == std::string::npos, "the offset is only proposed: no G43.1 yet");
        check_frame(m.gfx, "toolmeasure_done", false);
        check_hitboxes(m.gfx, "tool result page", { ui::Rect { 16, 340, 380, 52 }, ui::Rect { 404, 340, 380, 52 }, ui::Rect { 520, 408, 264, 52 } });
        touch_center(ms, ui::Rect { 16, 340, 380, 52 });                  // APPLY TOOL OFFSET
        check_frame(m.gfx, "toolmeasure_apply_confirm", true);
        check(m.wire.find("G43") == std::string::npos, "nothing is sent before the confirmation");
        touch_center(ms, ui::kDialogConfirm);
        for (int i = 0; i < 20 && m.wf.state() == JcWfState::Running; ++i) m.wf_run(300);
        ms.tick(g_stub_millis + 300);
        check(m.wf.result().tlo_readback_ok && m.wire.find("G43.1 Z6.500") != std::string::npos, "G43.1 Z6.500 was applied and read back");
        check(m.wire.find("G10") == std::string::npos && m.wire.find("G92") == std::string::npos, "no work offset was touched");
        check_frame(m.gfx, "toolmeasure_applied", false);

        Rig c;
        c.status("Idle", 50, 50, -5, 0);
        c.advance(500);
        c.nc.m[0] = 50; c.nc.m[1] = 50; c.nc.m[2] = -5;
        c.tool_store.set_ref(-20.0f, 5.0f, -1);
        JcWorkflowScreen cs(c.sv);
        cs.preselect(JcWfKind::ToolChange, c.tool_store);
        cs.enter();
        check_frame(c.gfx, "toolchange_setup", false);
    }
#endif

    printf("[010: bluetooth LE page]\n");
    {
        Rig b;
        b.status("Idle", 50, 50, -5, 0);
        b.advance(500);
        MockRadio radio;
        radio.link = &b.ble;
        b.ble.set_radio(&radio);
        JcBleScreen bs(b.sv);
        bs.enter();
        check_frame(b.gfx, "ble_not_selected", false);
        check_hitboxes(b.gfx, "ble page", { ui::Rect { 16, 108, 188, 48 }, ui::Rect { 212, 108, 188, 48 }, ui::Rect { 408, 108, 188, 48 },
                                            ui::Rect { 628, 164, 156, 58 }, ui::Rect { 628, 228, 156, 58 } });
        touch_center(bs, ui::Rect { 16, 108, 188, 48 });
        check_frame(b.gfx, "ble_cannot_scan", true);
        touch_center(bs, ui::kDialogOk);
        b.settings.transport = 2;
        b.ble.begin(b.ble_saved, false, g_stub_millis);
        bs.enter();
        touch_center(bs, ui::Rect { 16, 108, 188, 48 });                  // SCAN
        check(b.ble.state() == JcBleState::Scanning && b.ble.devices().count() == 5, "scanning lists the advertising devices");
        bs.tick(g_stub_millis + 400);
        check_frame(b.gfx, "ble_scan_list", false);
        touch_center(bs, ui::Rect { 628, 228, 156, 58 });                 // page down
        check_frame(b.gfx, "ble_scan_list_page2", false);
        touch_center(bs, ui::Rect { 628, 164, 156, 58 });
        touch_center(bs, ui::Rect { 16, 164, 604, 38 });                  // the strongest serial device
        for (int i = 0; i < 100; ++i) { g_stub_millis += 10; b.ble.service(g_stub_millis); }
        check(b.ble.ready() && b.ble.selection().profile == JcBleProfile::Hm10, "connecting validates the GATT table and subscribes");
        JcBleSaved sv;
        if (b.ble.take_save_request(sv)) b.ble_saved = sv;
        bs.enter();
        bs.tick(g_stub_millis + 400);
        check_frame(b.gfx, "ble_connected", false);
        touch_center(bs, ui::Rect { 408, 108, 188, 48 });                 // FORGET SAVED
        check(b.host.ble_forgets == 1, "FORGET SAVED asks the application");
        b.ble_saved = JcBleSaved {};
        bs.enter();
        check_frame(b.gfx, "ble_forgotten", false);
        touch_center(bs, ui::Rect { 212, 108, 188, 48 });                 // DISCONNECT
        check(b.host.ble_disconnects == 1 && b.ble.state() == JcBleState::Idle, "DISCONNECT ends the session and does not reconnect by itself");
        bs.enter();
        check_frame(b.gfx, "ble_disconnected", false);
    }
}

static void scenarios() {
    Rig r;
    JcJogScreen jog(r.sv);
    JcLimitsScreen limits(r.sv);
    JcCapsScreen caps(r.sv);
    JcMenuScreen menu(r.sv);
    JcSettingsScreen settings_page(r.sv);

    const ui::Rect axis[4] = { jl::axis_rect(0), jl::axis_rect(1), jl::axis_rect(2), jl::axis_rect(3) };
    const ui::Rect arm_btn = jl::kArm, jog_neg = jl::kJogNeg, jog_stop = jl::kJogStop, jog_pos = jl::kJogPos, ack_btn = jl::kAck;
    const ui::Rect step10 = jl::step_rect(2);

    printf("[menu / settings]\n");
    menu.enter();
    check_frame(r.gfx, "menu", false);
    settings_page.enter();
    check_frame(r.gfx, "settings", false);

    printf("[jog page: nothing read yet]\n");
    r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    r.advance(1000);
    limits.enter();
    check_frame(r.gfx, "limits_unread", false);
    jog.enter();
    check_frame(r.gfx, "jog_x_unread", false);
#if JC_JOG_UI
    // Entering the page in LIVE, Idle, fresh: ONE automatic read was requested. Tapping the shared button
    // asks for a read instead of arming (nothing is armed without valid results).
    check(r.host.reads_started == 1, "entering the jog page starts one automatic machine-settings read");
#if JC_LIVE_JOG_TEST
    touch_center(jog, arm_btn);
    check(r.host.reads_started == 2 && !r.dispatcher.armed(), "READ SETTINGS requests a read and arms nothing");
    check_frame(r.gfx, "jog_read_settings_offered", false);
    {
        Rig m;   // moving machine: no automatic read
        JcJogScreen mj(m.sv);
        m.status("Jog", 1, 2, 3, 0);
        m.advance(1000);
        mj.enter();
        check(m.host.reads_started == 0, "no automatic read while the machine is not Idle");
        touch_center(mj, arm_btn);
        check_frame(m.gfx, "jog_read_blocked_dialog", true);
        touch_center(mj, ui::kDialogOk);
        Rig d;   // stale status: no automatic read
        JcJogScreen dj(d.sv);
        d.status("Idle", 1, 2, 3, 0, false);
        d.advance(1000);
        dj.enter();
        check(d.host.reads_started == 0, "no automatic read without fresh status");
        // A read in progress and a failed read.
        Rig p;
        JcJogScreen pj(p.sv);
        p.status("Idle", 0, 0, 0, 0);
        p.advance(1000);
        p.loader.start(g_stub_millis, nullptr);
        pj.enter();
        check_frame(p.gfx, "jog_caps_reading", false);
        touch_center(pj, arm_btn);
        check_frame(p.gfx, "jog_caps_reading_tap", true);
        touch_center(pj, ui::kDialogOk);
        p.loader.cancel(g_stub_millis);
        pj.enter();
        check_frame(p.gfx, "jog_caps_failed", false);
        check(!p.dispatcher.armed(), "a failed read leaves arming off");
    }
#endif
#endif

    printf("[capabilities / limits with the real controller read]\n");
    r.read_controller();
    r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    limits.enter();
    check_frame(r.gfx, "limits_real", false);
    caps.enter();
    check_frame(r.gfx, "capabilities_real", false);
#if JC_LIVE_JOG_TEST
    {
        // The read is only valid for kJcCapsMaxAgeMs: an old read is shown as EXPIRED and arming is off.
        const uint32_t saved = g_stub_millis;
        g_stub_millis += kJcCapsMaxAgeMs + 5000;
        r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
        jog.enter();
        check_frame(r.gfx, "jog_caps_expired", false);
        touch_center(jog, arm_btn);
        check(!r.dispatcher.armed(), "an expired read does not allow arming");
        g_stub_millis = saved;
        r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    }
#endif

#if JC_JOG_UI
    printf("[jog page: gate, arming dialog]\n");
#if JC_LIVE_JOG_TEST
    r.advance(500);
    r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    jog.enter();
    check_frame(r.gfx, "jog_x_ready", false);
    touch_center(jog, arm_btn);                                  // ARM TEST -> the arming dialog
    check_frame(r.gfx, "jog_arming_dialog_x", true);
    touch_center(jog, ui::kDialogCancel);                        // cancelling arms nothing
    check(!r.dispatcher.armed(), "cancelling the arming dialog arms nothing");
    touch_center(jog, arm_btn);
    touch_center(jog, ui::kDialogConfirm);                       // I ACKNOWLEDGE
    check(r.dispatcher.armed_at(g_stub_millis) && (r.dispatcher.unprotected_ack_mask() & 1) != 0, "acknowledging arms X");
    check_frame(r.gfx, "jog_x_armed", false);

    printf("[jog page: per-jog confirmation and result]\n");
    touch_center(jog, jog_pos);                                  // JOG + -> per-jog confirmation
    check_frame(r.gfx, "jog_confirm_dialog_x", true);
    check(r.wire.empty(), "nothing is transmitted before the per-jog confirmation");
    touch_center(jog, ui::kDialogCancel);
    check(r.wire.empty(), "cancelling the per-jog confirmation transmits nothing");
    touch_center(jog, jog_pos);
    touch_center(jog, ui::kDialogConfirm);                       // confirm -> sent
    check_frame(r.gfx, "jog_sent_dialog", true);
#if JC_JOG_PREVIEW
    check(r.wire == "$J=G91G21F100X1.000\n" && r.bridge.controller().lines() == 1 && r.dispatcher.sent() == 0,
          "exactly one restricted jog line reached the SIMULATION and nothing was sent");
    touch_center(jog, ui::kDialogOk);
    check_frame(r.gfx, "jog_awaiting_ok", false);
    r.advance(400);
    jog.enter();
    check_frame(r.gfx, "jog_accepted_moving", false);
    r.advance(1200);
    jog.enter();
    check_frame(r.gfx, "jog_completed", false);
    check(r.jogs.result() == JcJogResult::Completed, "the simulated jog completed");
#else
    check(r.wire == "$J=G91G21F100X1.000\n", "exactly one restricted jog line was transmitted");
    touch_center(jog, ui::kDialogOk);
    check_frame(r.gfx, "jog_awaiting_ok", false);
    r.advance(80);
    r.jogs.on_line("ok", g_stub_millis);
    r.advance(300);
    r.status("Jog", 10.4f, 20.0f, 5.0f, 0.0f);
    jog.enter();
    check_frame(r.gfx, "jog_accepted_moving", false);
    r.advance(700);
    r.status("Idle", 11.0f, 20.0f, 5.0f, 0.0f);
    jog.enter();
    check_frame(r.gfx, "jog_completed", false);
#endif

    printf("[jog page: Z, A and refused requests]\n");
    touch_center(jog, axis[2]);
    check_frame(r.gfx, "jog_z_hold", false);
    touch_center(jog, jog_pos);
    check_frame(r.gfx, "jog_z_hold_dialog", true);
    touch_center(jog, ui::kDialogOk);
    touch_center(jog, arm_btn);                                  // NOT AVAILABLE for Z: explained, not armed
    check_frame(r.gfx, "jog_z_arm_refused_dialog", true);
    check(!r.dispatcher.armed() || (r.dispatcher.unprotected_ack_mask() & 4) == 0, "Z can never be acknowledged / armed");
    touch_center(jog, ui::kDialogOk);
    touch_center(jog, axis[3]);
    check_frame(r.gfx, "jog_a_disabled", false);
    touch_center(jog, jog_neg);
    check_frame(r.gfx, "jog_a_disabled_dialog", true);
    touch_center(jog, ui::kDialogOk);
    touch_center(jog, axis[0]);
    touch_center(jog, step10);                                   // the 10 mm preset
    check_frame(r.gfx, "jog_x_10mm_selected", false);
    r.advance(1000);
    touch_center(jog, jog_pos);
    check_frame(r.gfx, "jog_x_10mm_refused", true);
    touch_center(jog, ui::kDialogOk);

#else
    // Controller-authoritative interface: arm once, jog straight away on all four axes, FluidNC decides.
    r.advance(500);
    r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    jog.enter();
    check_frame(r.gfx, "jog_x_ready", false);
    touch_center(jog, arm_btn);
    check_frame(r.gfx, "jog_arm_dialog", true);
    touch_center(jog, ui::kDialogCancel);
    check(!r.dispatcher.armed(), "cancelling the arming dialog arms nothing");
    touch_center(jog, jog_pos);                                  // not armed: explained, nothing sent
    check_frame(r.gfx, "jog_not_armed_dialog", true);
    touch_center(jog, ui::kDialogOk);
    check(r.wire.empty(), "nothing is sent while not armed");
    touch_center(jog, arm_btn);
    touch_center(jog, ui::kDialogConfirm);
    check(r.dispatcher.armed_at(g_stub_millis), "the confirmation arms jogging (no capability read, homing or per-axis acknowledgement)");
    check_frame(r.gfx, "jog_x_armed", false);
    touch_center(jog, jog_pos);                                  // sent at once: arming is the confirmation
    check(r.wire == "$J=G91G21F100X1.000\n" && r.dispatcher.sent() == 0, "the jog line reached the transport (the simulation) at once");
    check_frame(r.gfx, "jog_x_sent", false);
    r.advance(1500);
    jog.enter();
    check_frame(r.gfx, "jog_x_completed", false);
    check(r.jogs.result() == JcJogResult::Completed, "the simulated jog completed");
    touch_center(jog, axis[2]);
    check_frame(r.gfx, "jog_z_ready", false);
    r.advance(1000);
    touch_center(jog, jog_pos);
    check(r.wire.find("Z1.000") != std::string::npos, "Z jogs like any other axis");
    r.advance(2000);
    touch_center(jog, axis[3]);
    check_frame(r.gfx, "jog_a_ready", false);
    r.advance(1000);
    touch_center(jog, jog_neg);
    check(r.wire.find("A-") != std::string::npos, "A jogs counter-clockwise / clockwise in degrees");
    r.advance(2500);
    touch_center(jog, axis[0]);
    touch_center(jog, step10);
    check_frame(r.gfx, "jog_x_10mm_selected", false);
    r.advance(1000);
    touch_center(jog, jog_pos);
    check(r.wire.find("X10.000") != std::string::npos, "a 10 mm step is the controller's to accept or refuse");
    check_frame(r.gfx, "jog_x_10mm_sent", false);
    r.advance(4000);
    touch_center(jog, jog_stop);                                 // cancel is always available
    check(r.wire.find(std::string(1, static_cast<char>(0x85))) != std::string::npos, "CANCEL JOG sends the cancel byte");
#endif

    printf("[jog page: failure and cancel states]\n");
    {
        // A new rig per state keeps each one self-contained.
        Rig f;
        JcJogScreen fj(f.sv);
        f.status("Idle", 0, 0, 0, 0);
        f.advance(1000);
        f.read_controller();
        f.status("Idle", 0, 0, 0, 0);
        f.dispatcher.arm_for(g_stub_millis, kJcJogArmTtlMs, 0x03);
        f.advance(600);
        f.status("Idle", 0, 0, 0, 0);
        fj.enter();
        // No reply: timeout, cancel, latched failure with the ACKNOWLEDGE button.
#if JC_JOG_PREVIEW
        f.bridge.controller().set_scenario(JcPreviewScenario::NoReply);
#endif
        JcJogSession::sink(&f.jogs, JcCommandClass::Jog, "$J=G91G21F100X1.000");
        f.advance(2100);
        f.status("Idle", 0, 0, 0, 0);
        f.advance(300);
        f.status("Idle", 0, 0, 0, 0);
        fj.enter();
        check_frame(f.gfx, "jog_no_reply_latched", false);
        touch_center(fj, ack_btn);
        fj.enter();
        check_frame(f.gfx, "jog_after_acknowledge", false);

        // The controller never confirms Idle after a cancel.
        f.advance(1500);
        f.status("Idle", 0, 0, 0, 0);
#if JC_JOG_PREVIEW
        f.bridge.controller().set_scenario(JcPreviewScenario::IgnoreCancel);
        JcJogSession::sink(&f.jogs, JcCommandClass::Jog, "$J=G91G21F100X1.000");
        f.advance(400);
        f.jogs.cancel(g_stub_millis);
        f.advance(3600);
#else
        JcJogSession::sink(&f.jogs, JcCommandClass::Jog, "$J=G91G21F100X1.000");
        f.advance(60);
        f.jogs.on_line("ok", g_stub_millis);
        f.advance(300);
        f.status("Jog", 0.3f, 0, 0, 0);
        f.jogs.cancel(g_stub_millis);
        f.advance(3200);
        f.status("Jog", 0.6f, 0, 0, 0);
        f.advance(10);
        f.jogs.tick(g_stub_millis);
#endif
        fj.enter();
        check_frame(f.gfx, "jog_cancel_unconfirmed", false);

        // Communication lost while a jog is unresolved: software cancel cannot be relied on.
        f.set_fresh(false);
        f.jogs.on_link_lost(g_stub_millis, false);
        fj.enter();
        check_frame(f.gfx, "jog_no_communication", false);
        f.tx_ok = false;
        touch_center(fj, jog_stop);
        check_frame(f.gfx, "jog_cancel_not_sent_dialog", true);
    }
#if JC_JOG_PREVIEW
    {
        printf("[preview: scenario page and counters]\n");
        JcJogPreviewScreen pv(r.sv);
        pv.enter();
        check_frame(r.gfx, "preview_page", false);
        touch_center(pv, ui::Rect { 560, 188, 224, 52 });
        check_frame(r.gfx, "preview_page_next_scenario", false);
        check(r.bridge.controller().scenario() == JcPreviewScenario::Rejected, "NEXT SCENARIO advances the simulated scenario");
        check(r.dispatcher.sent() == 0, "the gate's SENT counter is still zero after the whole preview run");
    }
#endif
#if JC_AUTHORITY
    printf("[workflow page: positioning and probing routines]\n");
    {
        Rig w;
        w.status("Idle", 50, 50, -5, 0);
        w.advance(500);
        w.nc.m[0] = 50; w.nc.m[1] = 50; w.nc.m[2] = -5;
        JcWorkflowScreen wfs(w.sv);
        wfs.enter();
        check_frame(w.gfx, "workflow_list", false);
        {   // Instruction 011A: nine routines, five positioning (left) and four probing (right); no overlap, all >= 38 px
            std::vector<ui::Rect> rs;
            for (int i = 0; i < 5; ++i) rs.push_back(ui::Rect { 16, static_cast<int16_t>(98 + i * 50), 372, 46 });
            for (int i = 0; i < 4; ++i) rs.push_back(ui::Rect { 412, static_cast<int16_t>(98 + i * 50), 372, 46 });
            check_hitboxes(w.gfx, "workflow list (9 routines)", rs);
        }
        touch_center(wfs, ui::Rect { 412, 98, 372, 46 });           // FIND X CENTRE
        check_frame(w.gfx, "workflow_setup_x", false);
        touch_center(wfs, ui::Rect { 198, 408, 586, 52 });          // PREPARE
        w.wf_run(600);
        wfs.tick(g_stub_millis);
        check_frame(w.gfx, "workflow_preview", false);
        check(w.wf.state() == JcWfState::Planned && w.wf.wcs() == 0, "PREPARE read the active WCS before the preview");
        check(w.nc.cmds == 2 && w.wire.find("G21") == std::string::npos, "only the read-only $G and $# were sent: nothing moves on selection");
        touch_center(wfs, ui::Rect { 258, 408, 250, 52 });          // START -> the single confirmation
        check_frame(w.gfx, "workflow_start_confirm", true);
        touch_center(wfs, ui::kDialogConfirm);
        check(w.dispatcher.armed_at(g_stub_millis), "confirming START arms command sending for this routine");
        w.wf_run(900);
        wfs.tick(g_stub_millis);
        check_frame(w.gfx, "workflow_running", false);
        for (int i = 0; i < 80 && w.wf.state() == JcWfState::Running; ++i) { w.wf_run(500); wfs.tick(g_stub_millis); }
        wfs.tick(g_stub_millis + 300);
        check(w.wf.state() == JcWfState::Done && w.wf.result().x.ok, "the routine measured the X centre against the simulated FluidNC");
        check_frame(w.gfx, "workflow_done", false);
        touch_center(wfs, ui::Rect { 404, 340, 380, 52 });          // SET WORK X0 HERE -> confirmation, never automatic
        check_frame(w.gfx, "workflow_origin_confirm", true);
        check(w.wire.find("G10") == std::string::npos, "no work offset is written before the confirmation");
        touch_center(wfs, ui::kDialogConfirm);
        for (int i = 0; i < 20 && w.wf.state() == JcWfState::Running; ++i) { w.wf_run(300); }
        wfs.tick(g_stub_millis + 300);
        check(w.wf.result().readback_ok, "the offset was read back from the simulated FluidNC and matches");
        check_frame(w.gfx, "workflow_origin_set", false);
        {   // Instruction 011A: GO TO WORK Z0 and GO TO WORK X0 Y0 Z0
            Rig z;
            z.status("Idle", 50, 50, 15, 0);
            z.advance(500);
            z.nc.m[0] = 50; z.nc.m[1] = 50; z.nc.m[2] = 15;
            JcWorkflowScreen zs(z.sv);
            zs.enter();
            touch_center(zs, ui::Rect { 16, 248, 372, 46 });          // GO TO WORK Z0
            check_frame(z.gfx, "workflow_setup_z0", false);
            touch_center(zs, ui::Rect { 198, 408, 586, 52 });          // PREPARE
            z.wf_run(600);
            zs.tick(g_stub_millis);
            check(z.wf.state() == JcWfState::Planned && z.wf.kind() == JcWfKind::GoToZ, "GO TO WORK Z0 is planned without a clearance");
            check(z.wire.find("G21") == std::string::npos, "nothing moves on selection or preview");
            check_frame(z.gfx, "workflow_preview_z0", false);
            touch_center(zs, ui::Rect { 258, 408, 250, 52 });          // START
            check_frame(z.gfx, "workflow_start_confirm_z0", true);
            touch_center(zs, ui::kDialogConfirm);
            z.wf_run(900);
            zs.tick(g_stub_millis);
            check(z.wf.state() == JcWfState::WaitOperator && z.wire.find("G21") == std::string::npos, "Z0 waits for its own confirmation before any move");
            check_frame(z.gfx, "workflow_z0_wait", false);

            Rig x;
            x.status("Idle", 50, 50, -5, 0);
            x.advance(500);
            x.nc.m[0] = 50; x.nc.m[1] = 50; x.nc.m[2] = -5;
            JcWorkflowScreen xs(x.sv);
            xs.enter();
            touch_center(xs, ui::Rect { 16, 298, 372, 46 });          // GO TO WORK X0 Y0 Z0
            check_frame(x.gfx, "workflow_setup_xyz", false);
        }
        // a failed contact
        Rig f;
        f.status("Idle", 50, 50, -5, 0);
        f.advance(500);
        f.nc.feat = JcSimFeature::None;
        JcWorkflowScreen fs(f.sv);
        fs.enter();
        touch_center(fs, ui::Rect { 412, 100, 372, 56 });
        touch_center(fs, ui::Rect { 198, 408, 586, 52 });
        f.wf_run(600);
        f.nc.feat = JcSimFeature::None;   // nothing to touch
        touch_center(fs, ui::Rect { 258, 408, 250, 52 });
        touch_center(fs, ui::kDialogConfirm);
        for (int i = 0; i < 40 && f.wf.state() == JcWfState::Running; ++i) { f.wf_run(500); }
        fs.tick(g_stub_millis + 300);
        check(f.wf.state() == JcWfState::Failed, "no contact ends the routine");
        check_frame(f.gfx, "workflow_failed", false);
        // GO TO needs a clearance: refused in PREPARE with the reason.
        Rig g;
        g.status("Idle", 50, 50, -5, 0);
        g.advance(500);
        JcWorkflowScreen gs(g.sv);
        gs.enter();
        touch_center(gs, ui::Rect { 16, 100, 372, 56 });            // GO TO WORK X0 Y0
        check_frame(g.gfx, "workflow_setup_goto", false);
        touch_center(gs, ui::Rect { 198, 408, 586, 52 });
        check_frame(g.gfx, "workflow_prepare_refused", true);
        touch_center(gs, ui::kDialogOk);
        check(g.nc.cmds == 0, "nothing is sent when the clearance is missing");
    }
    {
        // Homing page: separate facts, Z / A not homed silently.
        Rig h;
        h.status("Idle", 0, 0, 0, 0);
        h.advance(500);
        h.read_controller();
        h.status("Idle", 0, 0, 0, 0);
        JcHomingScreen hs(h.sv);
        hs.enter();
        check_frame(h.gfx, "homing_state", false);
    }
#endif
#else
    // Ordinary firmware: the jog page is a preview only. Show it, and that nothing is queued to send.
    printf("[ordinary firmware: jog page is a preview]\n");
    r.advance(500);
    jog.enter();
    check_frame(r.gfx, "jog_ordinary", false);
    touch_center(jog, jog_pos);
    check_frame(r.gfx, "jog_ordinary_after_press", false);
    check(r.wire.empty() && r.dispatcher.sent() == 0, "the ordinary build transmits nothing when JOG is pressed");
#endif
}

// A checker that cannot fire proves nothing: draw two texts on top of each other and expect it to notice.
static void detector_self_test() {
    printf("[detector self-test]\n");
    Arduino_GFX g;
    g.fillScreen(ui::kBg);
    ui::text(g, 100, 100, "OVERLAP TEST", FontId::Body16, WHITE, ui::kBg);
    ui::text(g, 104, 102, "OVERLAP TEST", FontId::Body16, WHITE, ui::kBg);
    uint32_t overlapped = 0;
    for (const StubGlyph& gl : g.glyphs) overlapped += gl.overlapped;
    check(overlapped > 0, "overlapping text is detected");
    g.fillScreen(ui::kBg);
    ui::text(g, 100, 100, "SIDE BY SIDE", FontId::Body16, WHITE, ui::kBg);
    ui::text(g, 100, 140, "SECOND LINE", FontId::Body16, WHITE, ui::kBg);
    overlapped = 0;
    for (const StubGlyph& gl : g.glyphs) overlapped += gl.overlapped;
    check(overlapped == 0, "separate text is not flagged");
    g.fillScreen(ui::kBg);
    ui::text(g, 790, 100, "OFF THE RIGHT EDGE", FontId::Body16, WHITE, ui::kBg);
    check(g.oob > 0, "text beyond the screen edge is detected");
    g.fillScreen(ui::kBg);
    ui::text(g, 100, 100, "WIPED", FontId::Body16, WHITE, ui::kBg);
    g.fillRect(90, 90, 200, 40, ui::kPanel);
    uint32_t wiped = 0;
    for (const StubFill& f : g.fills) wiped += f.obscured_ink;
    check(wiped > 0, "a fill over text is detected");
}

int main(int argc, char** argv) {
    g_outdir = argc > 1 ? argv[1] : nullptr;
    printf("UI render (%s build)\n", g_tag);
    pure_checks();
    detector_self_test();
    scenarios();
    pages_010();
    printf("\n%d frames checked, %d problem(s)\n", g_frames, g_problems);
    return g_problems == 0 ? 0 : 1;
}
