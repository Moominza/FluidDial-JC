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

#include "../../src/jc8048w550/JcScreens.hpp"

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
static const char* g_tag = JC_LIVE_JOG_TEST ? "live" : "ordinary";

// ---- the rig: the services a page needs, with simulated controller state ----------------------------

class StubHost : public JcAppHost {
public:
    void go(JcPage) override {}
    void back() override {}
    void request_input(const JcInputRequest&) override {}
    void save_settings() override {}
    void apply_brightness() override {}
    void leave(JcLeaveTarget) override {}
    const char* start_caps_read() override { return nullptr; }
    void cancel_caps_read() override {}
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
    JcAppServices sv;
    std::string wire;   // every byte the session would have transmitted
    bool tx_ok = true;
    uint32_t seq = 1000;

    Rig() : jogs(&Rig::tx, this), sv { gfx, link, model, settings, profile, dispatcher, loader, jogs, live_mode, host } {
        refresh_profile();
        dispatcher.set_sink_ex(&JcJogSession::sink, &jogs);   // the experimental build's wiring; blocked by policy otherwise
    }
    static bool tx(void* user, const char* bytes, size_t n) {
        Rig* r = static_cast<Rig*>(user);
        if (!r->tx_ok) return false;
        r->wire.append(bytes, n);
        return true;
    }
    void refresh_profile() {
        profile = jc_profile_from_settings(settings);
        if (loader.state() == JcLoadState::Complete) jc_profile_apply_caps(profile, loader.caps());
#if JC_LIVE_JOG_TEST
        jc_profile_apply_test_limits(profile);
#endif
    }
    void advance(uint32_t ms) {
        g_stub_millis += ms;
        jogs.set_clock(g_stub_millis);
        jogs.tick(g_stub_millis);
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
        v.state = s.machine_state;
        v.sequence = s.sequence;
        v.pos_valid = true;
        memcpy(v.pos, s.axes, sizeof(v.pos));
        jogs.on_status(v, g_stub_millis);
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
    check(ui::live_jog_banner("JOGGING", box, text) && strcmp(text, "LIVE JOG TEST BUILD") == 0, "the full banner text is used where it fits (JOGGING)");
    check(ui::live_jog_banner("CONTROLLER CAPABILITIES", box, text), "a shorter banner is chosen for the longest title");

    // The dashboard banner (x 584..) fits in the 800 px header.
    check(584 + text_width(FontId::Body16, "LIVE JOG TEST BUILD") + 16 <= 796, "the dashboard header banner fits");

    // Worst-case strings against their containers.
    check(text_width(FontId::Body16, "Keep E-stop / power removal in reach. Not 'safe': 1 mm only.") <= ui::kDialogBox.w - ui::kDialogTextLeft - 20,
          "the longest arming-dialog line fits the dialog width");
    check(text_width(FontId::Body16, "NO COMMUNICATION: software cancel unreliable. USE E-STOP") <= 650 - 6,
          "the no-communication message fits its 650 px slot");
    char lines[2][96];
    JcMachineCaps none;
    jc_limits_summary(none, lines, 2);
    check(text_width(FontId::Body16, lines[0]) <= 760 - 6, "the Limits summary (worst case: everything unknown) fits its slot");

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
static void scenarios() {
    Rig r;
    JcJogScreen jog(r.sv);
    JcLimitsScreen limits(r.sv);
    JcCapsScreen caps(r.sv);
    JcMenuScreen menu(r.sv);
    JcSettingsScreen settings_page(r.sv);

    const ui::Rect axis[4] = { { 16, 76, 92, 50 }, { 116, 76, 92, 50 }, { 216, 76, 92, 50 }, { 316, 76, 92, 50 } };
    const ui::Rect arm_btn { 640, 194, 144, 46 }, jog_neg { 16, 254, 250, 84 }, jog_stop { 275, 254, 250, 84 },
        jog_pos { 534, 254, 250, 84 }, ack_btn { 28, 432, 744, 36 };
    const ui::Rect step10 { 84 + 2 * 92, 138, 86, 46 };

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
#if JC_LIVE_JOG_TEST
    // Arming is refused: the read is missing.
    touch_center(jog, arm_btn);
    check_frame(r.gfx, "jog_arm_refused_unread", true);
    touch_center(jog, ui::kDialogOk);
#endif

    printf("[capabilities / limits with the real controller read]\n");
    r.read_controller();
    r.status("Idle", 10.0f, 20.0f, 5.0f, 0.0f);
    limits.enter();
    check_frame(r.gfx, "limits_real", false);
    caps.enter();
    check_frame(r.gfx, "capabilities_real", false);

#if JC_LIVE_JOG_TEST
    printf("[jog page: gate, arming dialog]\n");
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
        fj.enter();
        check_frame(f.gfx, "jog_cancel_unconfirmed", false);

        // Communication lost while a jog is unresolved: software cancel cannot be relied on.
        f.model.set_fresh(false);
        f.jogs.on_link_lost(g_stub_millis, false);
        fj.enter();
        check_frame(f.gfx, "jog_no_communication", false);
        f.tx_ok = false;
        touch_center(fj, jog_stop);
        check_frame(f.gfx, "jog_cancel_not_sent_dialog", true);
    }
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
    printf("\n%d frames checked, %d problem(s)\n", g_frames, g_problems);
    return g_problems == 0 ? 0 : 1;
}
