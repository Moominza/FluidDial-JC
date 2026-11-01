// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Positioning and probing routines (Instruction 009A): choose, set parameters, PREPARE (reads the active WCS and
// offsets, read-only), preview, one confirmation, progress, cancel, result, and the optional actions
// (move to centre, set work origin). FluidNC does the motion and the probing; see JcWorkflow.hpp.

#include "JcScreens.hpp"

#if JC_AUTHORITY

namespace jc8048w550 {

namespace {

enum class P : uint8_t { Clear, Search, Feed, Retract, Tip, Feature, Expected, Timeout, Slow, SlowFeed, EstWidth, CylDia, Spacing, PlateThick, ToolDir, ToolNum };

constexpr ui::Rect kBackList { 16, 408, 170, 52 };
constexpr ui::Rect kPrepare { 198, 408, 586, 52 };
constexpr ui::Rect kCancel { 16, 408, 230, 52 };
constexpr ui::Rect kGo { 258, 408, 250, 52 };
constexpr ui::Rect kDone { 520, 408, 264, 52 };
constexpr ui::Rect kMoveCentre { 16, 340, 380, 52 };
constexpr ui::Rect kSetOrigin { 404, 340, 380, 52 };

ui::Rect list_button(uint8_t i) {
    if (i < 5) return { 16, static_cast<int16_t>(98 + i * 50), 372, 46 };
    return { 412, static_cast<int16_t>(98 + (i - 5) * 50), 372, 46 };
}
ui::Rect param_rect(uint8_t i) {
    const uint8_t col = i / 7;
    return { static_cast<int16_t>(16 + col * 392), static_cast<int16_t>(98 + (i % 7) * 36), 376, 34 };
}

const JcWfKind kKinds[9] = { JcWfKind::GoToXY, JcWfKind::GoToX, JcWfKind::GoToY, JcWfKind::GoToZ, JcWfKind::GoToXYZ, JcWfKind::FindX, JcWfKind::FindY, JcWfKind::FindXY, JcWfKind::FindRotary };

uint8_t params_for(JcWfKind k, P out[14]) {
    uint8_t n = 0;
    if (k == JcWfKind::ToolReference) { out[n++] = P::PlateThick; out[n++] = P::ToolDir; out[n++] = P::Search; out[n++] = P::Feed; out[n++] = P::Retract; return n; }
    if (k == JcWfKind::ToolMeasure) { out[n++] = P::Search; out[n++] = P::Feed; out[n++] = P::Retract; return n; }
    if (k == JcWfKind::ToolChange) { out[n++] = P::Clear; out[n++] = P::ToolNum; out[n++] = P::Search; out[n++] = P::Feed; out[n++] = P::Retract; return n; }
    out[n++] = P::Clear;
    if (k == JcWfKind::GoToZ) { n = 0; return n; }   // no parameters: Z0 of the active WCS
    if (jc_wf_is_goto(k)) return n;
    out[n++] = P::Search; out[n++] = P::Feed; out[n++] = P::Retract; out[n++] = P::Tip;
    if (k != JcWfKind::FindRotary) { out[n++] = P::Feature; out[n++] = P::Expected; }
    out[n++] = P::Timeout; out[n++] = P::Slow; out[n++] = P::SlowFeed;
    if (k == JcWfKind::FindRotary || k == JcWfKind::FindX || k == JcWfKind::FindY) out[n++] = P::EstWidth;
    if (k == JcWfKind::FindRotary) { out[n++] = P::CylDia; out[n++] = P::Spacing; }
    return n;
}

void edit_thunk(void* user, bool ok, const char* text);
struct EditCtx { JcWorkflowScreen* screen; uint8_t index; };
EditCtx g_edit { nullptr, 0 };
void edit_thunk(void*, bool ok, const char* text) {
    if (g_edit.screen != nullptr) g_edit.screen->param_done(g_edit.index, ok, text);
}

const char* state_word(JcWfState s) {
    switch (s) {
        case JcWfState::Idle: return "idle";
        case JcWfState::Reading: return "READING WCS / OFFSETS";
        case JcWfState::Planned: return "READY - preview, then START";
        case JcWfState::Running: return "RUNNING";
        case JcWfState::WaitOperator: return "WAITING FOR YOU";
        case JcWfState::Done: return "DONE";
        case JcWfState::Failed: return "FAILED - nothing further sent";
        case JcWfState::Cancelled: return "CANCELLED - nothing further sent";
    }
    return "?";
}

}  // namespace

uint8_t JcWorkflowScreen::param_count() const {
    P ids[14];
    return params_for(_kind, ids);
}

void JcWorkflowScreen::param_label(uint8_t i, char* label, char* value, size_t size) const {
    P ids[14];
    const uint8_t n = params_for(_kind, ids);
    if (i >= n) { label[0] = value[0] = '\0'; return; }
    const char* l = "?";
    char v[24] = "";
    switch (ids[i]) {
        case P::Clear: l = "SAFE Z CLEARANCE (work Z)"; if (_p.clearance_set) snprintf(v, sizeof(v), "%.3f", static_cast<double>(_p.clearance_z)); else snprintf(v, sizeof(v), "NOT SET"); break;
        case P::Search: l = "SEARCH DISTANCE"; snprintf(v, sizeof(v), "%.1f mm", static_cast<double>(_p.search)); break;
        case P::Feed: l = "PROBE FEED"; snprintf(v, sizeof(v), "%.0f /min", static_cast<double>(_p.feed)); break;
        case P::Retract: l = "RETRACT"; snprintf(v, sizeof(v), "%.1f mm", static_cast<double>(_p.retract)); break;
        case P::Tip: l = "PROBE TIP RADIUS"; snprintf(v, sizeof(v), "%.3f mm", static_cast<double>(_p.tip_radius)); break;
        case P::Feature: l = "FEATURE"; snprintf(v, sizeof(v), "%s", _p.feature == JcFeatureKind::Inside ? "INSIDE (bore)" : "OUTSIDE (boss)"); break;
        case P::Expected: l = "EXPECTED SIZE"; if (_p.expected_size > 0.0f) snprintf(v, sizeof(v), "%.3f mm", static_cast<double>(_p.expected_size)); else snprintf(v, sizeof(v), "not given"); break;
        case P::Timeout: l = "CONTACT TIMEOUT"; snprintf(v, sizeof(v), "%.0f s", static_cast<double>(_p.contact_timeout_s)); break;
        case P::Slow: l = "SLOW SECOND TOUCH"; snprintf(v, sizeof(v), "%s", _p.slow_second ? "ON" : "off"); break;
        case P::SlowFeed: l = "SLOW FEED"; snprintf(v, sizeof(v), "%.0f /min", static_cast<double>(_p.slow_feed)); break;
        case P::EstWidth: l = "APPROX WIDTH (outside)"; if (_p.est_width > 0.0f) snprintf(v, sizeof(v), "%.1f mm", static_cast<double>(_p.est_width)); else snprintf(v, sizeof(v), "NOT SET"); break;
        case P::CylDia: l = "CYLINDER DIAMETER"; if (_p.cylinder_diameter > 0.0f) snprintf(v, sizeof(v), "%.3f mm", static_cast<double>(_p.cylinder_diameter)); else snprintf(v, sizeof(v), "NOT SET"); break;
        case P::Spacing: l = "STATION SPACING (along X)"; if (_p.station_spacing > 0.0f) snprintf(v, sizeof(v), "%.1f mm", static_cast<double>(_p.station_spacing)); else snprintf(v, sizeof(v), "NOT SET"); break;
        case P::PlateThick: l = "PLATE THICKNESS (not in the offset)"; snprintf(v, sizeof(v), "%.3f mm", static_cast<double>(_p.plate_thickness)); break;
        case P::ToolDir: l = "PROBE DIRECTION"; snprintf(v, sizeof(v), "%s", _p.tool_dir < 0 ? "DOWN (plate)" : "UP (sensor)"); break;
        case P::ToolNum: l = "NEW TOOL LABEL (display only)"; snprintf(v, sizeof(v), "T%d", _p.tool_number); break;
    }
    snprintf(label, size, "%s", l);
    snprintf(value, size, "%s", v);
}

void JcWorkflowScreen::preselect(JcWfKind kind, const JcToolStore& store) {
    _kind = kind;
    _p = JcWfParams();
    if (store.has_ref) {   // measurements are made the way the reference was
        _p.has_ref = true;
        _p.ref_mz = store.ref_mz;
        _p.tool_dir = store.ref_dir;
        _p.plate_thickness = store.ref_plate_thickness;
    }
    _mode = Mode::Setup;
}

void JcWorkflowScreen::on_enter() {
    if (!sv.wf.running() && _mode == Mode::Active && sv.wf.state() != JcWfState::Planned) _mode = Mode::List;
    for (ui::Slot& s : _lines) s.invalidate();
    _drawn_state = 0xFF;
}

void JcWorkflowScreen::draw_list() {
    ui::text(sv.display, 16, 72, "POSITIONING", FontId::Button20, WHITE, ui::kBg);
    ui::text(sv.display, 412, 72, "PROBING", FontId::Button20, WHITE, ui::kBg);
    for (uint8_t i = 0; i < 9; ++i) ui::button(sv.display, list_button(i), jc_wf_title(kKinds[i]));
    ui::wrap(sv.display, { 16, 352, 768, 100 },
             "FluidNC plans and executes every move and every probe. GO TO targets are WORK zero of the active WCS, not machine "
             "home. Routines that move X/Y need a safe Z clearance that YOU enter (Z0 is not assumed safe). Each reads the "
             "active WCS first, shows a preview and asks you to confirm; X0 Y0 Z0 asks again before lowering to Z0. Nothing "
             "moves because a routine was selected.",
             FontId::Body16, ui::kMuted, ui::kBg);
}

void JcWorkflowScreen::draw_setup() {
    ui::text(sv.display, 16, 70, jc_wf_title(_kind), FontId::Button20, CYAN, ui::kBg);
    const uint8_t n = param_count();
    for (uint8_t i = 0; i < n; ++i) {
        char label[40], value[24];
        param_label(i, label, value, sizeof(label));
        const ui::Rect r = param_rect(i);
        ui::panel(sv.display, r, ui::kPanelLight);
        ui::text(sv.display, r.x + 8, r.y + 7, label, FontId::Body16, ui::kMuted, ui::kPanelLight);
        const bool unset = strstr(value, "NOT SET") != nullptr;
        ui::text(sv.display, r.x + r.w - 8 - text_width(FontId::Body16, value), r.y + 7, value, FontId::Body16,
                 unset ? RED : WHITE, ui::kPanelLight);
    }
    if (_kind == JcWfKind::ToolReference || _kind == JcWfKind::ToolMeasure || _kind == JcWfKind::ToolChange) {
        char hint[260];
        if (_kind == JcWfKind::ToolReference) {
            snprintf(hint, sizeof(hint), "Fit the REFERENCE tool and place the touch plate (or fixed surface) under it. The direction and plate thickness are stored with the reference. You touch the tool to the plate by hand first: the probe pin must react before any probing move.");
        } else {
            snprintf(hint, sizeof(hint), "Reference: machine Z %.3f, plate %.3f, probing %s. %sThe difference to the reference is PROPOSED as the tool length offset; you apply it separately and it is read back.",
                     static_cast<double>(_p.ref_mz), static_cast<double>(_p.plate_thickness), _p.tool_dir < 0 ? "down" : "up",
                     _kind == JcWfKind::ToolChange ? "Z rises to the clearance, then YOU change the tool by hand. " : "");
        }
        ui::wrap(sv.display, { 16, 290, 768, 76 }, hint, FontId::Body16, ui::kMuted, ui::kBg);
    }
    ui::button(sv.display, kBackList, _kind == JcWfKind::ToolReference || _kind == JcWfKind::ToolMeasure || _kind == JcWfKind::ToolChange ? "BACK" : "LIST");
    ui::button(sv.display, kPrepare, "PREPARE  (reads the active WCS and offsets)", ui::Btn::Selected);
    draw_live_lines();
}

void JcWorkflowScreen::draw_live_lines() {
    char line[120];
    const bool have = sv.model.has_status();
    const JcLiveMachineStatus& st = sv.model.status();
    snprintf(line, sizeof(line), "Machine: %s   Spindle: %s   Probe pin: %s%s", have ? sv.model.state() : "no data",
             st.has_data && st.spindle > 0.0f ? "RUNNING" : "off", strchr(st.pins, 'P') != nullptr ? "TRIGGERED" : "clear",
             JC_JOG_PREVIEW ? "   [simulated motion]" : "");
    _lines[0].draw(sv.display, { 16, 374, 768, 24 }, line, FontId::Body16, sv.model.fresh() ? ui::kGood : ui::kWarn, ui::kBg);
}

void JcWorkflowScreen::draw_active() {
    const JcWorkflow& w = sv.wf;
    char line[140];
    ui::text(sv.display, 16, 68, w.title(), FontId::Button20, CYAN, ui::kBg);
    ui::panel(sv.display, { 16, 98, 768, 296 });
    _lines[0].invalidate(); _lines[1].invalidate(); _lines[2].invalidate(); _lines[3].invalidate(); _lines[4].invalidate(); _lines[5].invalidate();
    snprintf(line, sizeof(line), "STATE: %s", state_word(w.state()));
    const bool bad = w.state() == JcWfState::Failed || w.state() == JcWfState::Cancelled;
    _lines[0].draw(sv.display, { 28, 104, 744, 24 }, line, FontId::Body16, bad ? RED : (w.state() == JcWfState::Done ? ui::kGood : ui::kInfo), ui::kPanel);
    if (w.wcs() >= 0) {
        const float* o = w.offset(static_cast<uint8_t>(w.wcs()));
        snprintf(line, sizeof(line), "Active WCS: G%d   origin (machine) X%.3f Y%.3f Z%.3f", 54 + w.wcs(), static_cast<double>(o[0]), static_cast<double>(o[1]), static_cast<double>(o[2]));
        _lines[1].draw(sv.display, { 28, 130, 744, 24 }, line, FontId::Body16, WHITE, ui::kPanel);
    }
    if (w.state() == JcWfState::Planned) {
        for (uint8_t i = 0; i < w.step_count() && i < 8; ++i) {
            char s[80];
            w.preview_line(i, s, sizeof(s));
            ui::text(sv.display, 28, 160 + i * 22, s, FontId::Body16, ui::kMuted, ui::kPanel);
        }
        if (w.step_count() > 8) {
            snprintf(line, sizeof(line), "... %u steps in all, each sent ONE at a time", static_cast<unsigned>(w.step_count()));
            ui::text(sv.display, 28, 160 + 8 * 22, line, FontId::Body16, ui::kMuted, ui::kPanel);
        }
    } else if (w.state() == JcWfState::Running || w.state() == JcWfState::WaitOperator) {
        snprintf(line, sizeof(line), "Step %u of %u: %s", static_cast<unsigned>(w.step_index()) + 1u, static_cast<unsigned>(w.step_count()), w.step_label());
        _lines[2].draw(sv.display, { 28, 160, 744, 24 }, line, FontId::Body16, WHITE, ui::kPanel);
    } else if (bad) {
        _lines[2].draw(sv.display, { 28, 160, 744, 24 }, w.reason(), FontId::Body16, RED, ui::kPanel);
    } else if (w.state() == JcWfState::Done) {
        const JcWfResult& r = w.result();
        const JcWfKind k = w.kind();
        int row = 0;
        auto put = [&](const char* text, uint16_t color) {
            if (row < 4) _lines[2 + row].draw(sv.display, { 28, static_cast<int16_t>(160 + row * 28), 744, 24 }, text, FontId::Body16, color, ui::kPanel);
            ++row;
        };
        if (jc_wf_is_goto(k)) {
            put("Arrived. FluidNC reported Idle at the work target.", ui::kGood);
            put("Work zero of the active WCS, not machine home. A axis and work offsets unchanged.", ui::kMuted);
        }
        if (r.x.ok) {
            snprintf(line, sizeof(line), "X centre: machine %.3f   work %.3f   size %.3f", static_cast<double>(r.x.centre), static_cast<double>(w.work_coord(0, r.x.centre)), static_cast<double>(r.x.size));
            put(line, r.x.suspect ? ui::kWarn : ui::kGood);
        }
        if (r.y.ok && k != JcWfKind::FindRotary) {
            snprintf(line, sizeof(line), "Y centre: machine %.3f   work %.3f   size %.3f", static_cast<double>(r.y.centre), static_cast<double>(w.work_coord(1, r.y.centre)), static_cast<double>(r.y.size));
            put(line, r.y.suspect ? ui::kWarn : ui::kGood);
        }
        if (k == JcWfKind::FindRotary && r.line.ok) {
            snprintf(line, sizeof(line), "Centreline: Y %.3f  Z %.3f (machine)   dia %.3f", static_cast<double>(r.line.cross), static_cast<double>(r.line.z), static_cast<double>(r.line.diameter));
            put(line, ui::kGood);
            snprintf(line, sizeof(line), "Tilt: Y %.4f  Z %.4f mm per mm along the axis", static_cast<double>(r.line.tilt_cross), static_cast<double>(r.line.tilt_z));
            put(line, ui::kInfo);
            put("A-axis calibration is NOT modified.", ui::kMuted);
        }
        if (r.x.suspect) put(r.x.why, ui::kWarn);
        if (k == JcWfKind::ToolReference && r.tool_contact_ok) {
            snprintf(line, sizeof(line), "Reference tool touched at machine Z %.3f  (stored; no offset written)", static_cast<double>(r.tool_contact_mz));
            put(line, ui::kGood);
            put("Plate thickness cancels in later measurements; it is not an offset.", ui::kMuted);
        }
        if ((k == JcWfKind::ToolMeasure || k == JcWfKind::ToolChange) && r.tlo_ok) {
            put(r.summary[0], WHITE);
            put(r.summary[1], ui::kInfo);
            if (!r.tlo_readback_done) put("NOT applied yet. APPLY sends G43.1 and reads [TLO:] back.", ui::kWarn);
        }
        if (r.tlo_readback_done) {
            snprintf(line, sizeof(line), "[TLO:] read back from FluidNC: %+.3f mm  %s", static_cast<double>(r.tlo_read), r.tlo_readback_ok ? "MATCH" : "MISMATCH");
            put(line, r.tlo_readback_ok ? ui::kGood : RED);
        }
        if (r.readback_done) put(r.readback_ok ? "Offsets read back from FluidNC: MATCH" : "Offsets read back: MISMATCH", r.readback_ok ? ui::kGood : RED);
    }
    // Buttons.
    if (w.state() == JcWfState::Planned) {
        ui::button(sv.display, kCancel, "CANCEL", ui::Btn::Danger);
        ui::button(sv.display, kGo, "START", ui::Btn::Good);
    } else if (w.state() == JcWfState::Running || w.state() == JcWfState::Reading) {
        ui::button(sv.display, kCancel, "CANCEL (HOLD)", ui::Btn::Danger);
    } else if (w.state() == JcWfState::WaitOperator) {
        ui::button(sv.display, kCancel, "CANCEL", ui::Btn::Danger);
        ui::button(sv.display, kGo, "CONTINUE", ui::Btn::Good);
    } else {
        if (w.state() == JcWfState::Done && w.result().valid) {
            const JcWfKind k = w.kind();
            const bool measured = k == JcWfKind::FindX || k == JcWfKind::FindY || k == JcWfKind::FindXY;
            if ((k == JcWfKind::ToolMeasure || k == JcWfKind::ToolChange) && w.result().tlo_ok) {
                ui::button(sv.display, kMoveCentre, "APPLY TOOL OFFSET (G43.1)", ui::Btn::Normal);
                ui::button(sv.display, kSetOrigin, "CLEAR TOOL OFFSET (G49)", ui::Btn::Danger);
            }
            if (measured) {
                ui::button(sv.display, kMoveCentre, "MOVE TO CENTRE", ui::Btn::Normal);
                ui::button(sv.display, kSetOrigin, k == JcWfKind::FindX ? "SET WORK X0 HERE" : (k == JcWfKind::FindY ? "SET WORK Y0 HERE" : "SET WORK X0 Y0 HERE"), ui::Btn::Danger);
            }
        }
        ui::button(sv.display, kDone, "BACK TO LIST");
    }
    _drawn_state = static_cast<uint8_t>(w.state());
    _drawn_step = w.step_index();
}

void JcWorkflowScreen::on_draw() {
    if (_mode == Mode::List) draw_list();
    else if (_mode == Mode::Setup) draw_setup();
    else draw_active();
}

void JcWorkflowScreen::param_done(uint8_t index, bool ok, const char* text) {
    if (!ok) return;
    P ids[14];
    const uint8_t n = params_for(_kind, ids);
    if (index >= n) return;
    char* end = nullptr;
    const float v = strtof(text, &end);
    if (end == text || !isfinite(v)) return;
    switch (ids[index]) {
        case P::Clear: _p.clearance_z = v; _p.clearance_set = true; break;
        case P::Search: _p.search = v; break;
        case P::Feed: _p.feed = v; break;
        case P::Retract: _p.retract = v; break;
        case P::Tip: _p.tip_radius = v; break;
        case P::Expected: _p.expected_size = v; break;
        case P::Timeout: _p.contact_timeout_s = v; break;
        case P::SlowFeed: _p.slow_feed = v; break;
        case P::EstWidth: _p.est_width = v; break;
        case P::CylDia: _p.cylinder_diameter = v; break;
        case P::Spacing: _p.station_spacing = v; break;
        case P::PlateThick: _p.plate_thickness = v; break;
        case P::ToolNum: _p.tool_number = static_cast<int>(v); break;
        default: break;
    }
    draw();
}

void JcWorkflowScreen::edit_param(uint8_t i) {
    P ids[14];
    const uint8_t n = params_for(_kind, ids);
    if (i >= n) return;
    if (ids[i] == P::Feature) { _p.feature = _p.feature == JcFeatureKind::Inside ? JcFeatureKind::Outside : JcFeatureKind::Inside; draw(); return; }
    if (ids[i] == P::Slow) { _p.slow_second = !_p.slow_second; draw(); return; }
    if (ids[i] == P::ToolDir) { _p.tool_dir = -_p.tool_dir; draw(); return; }
    char label[40], value[24];
    param_label(i, label, value, sizeof(label));
    JcInputRequest r {};
    r.prompt = label;
    snprintf(r.initial, sizeof(r.initial), "%s", strstr(value, "NOT SET") != nullptr || strstr(value, "not given") != nullptr ? "" : value);
    // keep only the number from "12.5 mm"
    for (char* c = r.initial; *c; ++c) if (*c == ' ') { *c = '\0'; break; }
    r.numeric = true;
    r.max_len = 10;
    r.done = edit_thunk;
    r.user = this;
    g_edit.screen = this;
    g_edit.index = i;
    sv.host.request_input(r);
}

void JcWorkflowScreen::prepare() {
    if (!sv.host.workflow_prepare(_kind, _p)) {
        show_message("CANNOT PREPARE", sv.wf.reason()[0] ? sv.wf.reason() : "check the machine status", "Nothing was sent to the machine.");
        return;
    }
    _mode = Mode::Active;
    draw();
}

void JcWorkflowScreen::request_action(uint8_t which) {
    const JcWfResult& r = sv.wf.result();
    char l1[100], l2[100], l3[100];
    const JcWfKind k = sv.wf.kind();
    const bool x = (k == JcWfKind::FindX || k == JcWfKind::FindXY) && r.x.ok;
    const bool y = (k == JcWfKind::FindY || k == JcWfKind::FindXY) && r.y.ok;
    if (which == 3) {
        snprintf(l1, sizeof(l1), "Sends G43.1 Z%+.3f (dynamic tool length offset).", static_cast<double>(r.tlo_proposed));
        snprintf(l2, sizeof(l2), "[TLO:] is read back from FluidNC and compared.");
        snprintf(l3, sizeof(l3), "Work offsets (G54..G59) and G92 are NOT touched.");
        open_custom_confirm("APPLY THE TOOL LENGTH OFFSET?", "APPLY");
    } else if (which == 4) {
        snprintf(l1, sizeof(l1), "Sends G49 (cancel the tool length offset).");
        snprintf(l2, sizeof(l2), "[TLO:] is read back and must read 0.");
        snprintf(l3, sizeof(l3), "Work offsets (G54..G59) and G92 are NOT touched.");
        open_custom_confirm("CLEAR THE TOOL LENGTH OFFSET?", "SEND G49");
    } else if (which == 2) {
        snprintf(l1, sizeof(l1), "Writes G10 L2 P%d for the ACTIVE WCS (G%d). Never G92.", r.wcs + 1, 54 + r.wcs);
        snprintf(l2, sizeof(l2), "New origin (machine): %s%.3f %s%.3f", x ? "X" : "", x ? static_cast<double>(r.x.centre - r.g92[0]) : 0.0,
                 y ? "Y" : "", y ? static_cast<double>(r.y.centre - r.g92[1]) : 0.0);
        snprintf(l3, sizeof(l3), "The offsets are read back from FluidNC and compared.");
        open_custom_confirm("SET WORK ORIGIN?", "WRITE OFFSET");
    } else {
        snprintf(l1, sizeof(l1), "Absolute move in the ACTIVE WCS (G%d), spindle stopped.", 54 + r.wcs);
        snprintf(l2, sizeof(l2), "Target work: %s%.3f %s%.3f", x ? "X" : "", x ? static_cast<double>(sv.wf.work_coord(0, r.x.centre)) : 0.0,
                 y ? "Y" : "", y ? static_cast<double>(sv.wf.work_coord(1, r.y.centre)) : 0.0);
        snprintf(l3, sizeof(l3), "%s", sv.wf.params().feature == JcFeatureKind::Outside ? "Raises to the clearance first, then pauses." : "Moves inside the feature at the current Z.");
        open_custom_confirm("MOVE TO THE CENTRE?", "MOVE");
    }
    dialog.add_line(l1);
    dialog.add_line(l2, ui::kWarn);
    dialog.add_line(l3);
    dialog.add_line("Keep the E-stop in reach. Software cancel is not an E-stop.", RED);
    draw();
}

void JcWorkflowScreen::on_custom_confirm() {
    const uint32_t now = millis();
    // The operator's confirmation arms command sending for this routine only (15 minutes at most); it is dropped
    // when the routine ends, when the page is left and on any loss of the connection.
    sv.dispatcher.arm_for(now, 15u * 60u * 1000u, 0x0F);
    const uint8_t pending = _action_pending;
    _action_pending = 0;
    if (pending == 1) {
        sv.wf.request_move_to_centre(now);
    } else if (pending == 2) {
        sv.wf.request_set_origin(now);
    } else if (pending == 3) {
        sv.wf.request_apply_tlo(now);
    } else if (pending == 4) {
        sv.wf.request_clear_tlo(now);
    } else {
        sv.wf.start(now);   // START
        Serial.printf(JC_JOG_TAG ": routine started: %s\n", jc_wf_title(sv.wf.kind()));
    }
}

void JcWorkflowScreen::on_touch(uint16_t x, uint16_t y) {
    const uint32_t now = millis();
    if (_mode == Mode::List) {
        for (uint8_t i = 0; i < 9; ++i) {
            if (ui::hit(list_button(i), x, y)) {
                _kind = kKinds[i];
                _mode = Mode::Setup;
                draw();
                return;
            }
        }
        return;
    }
    if (_mode == Mode::Setup) {
        const uint8_t n = param_count();
        for (uint8_t i = 0; i < n; ++i) {
            if (ui::hit(param_rect(i), x, y)) { edit_param(i); return; }
        }
        if (ui::hit(kBackList, x, y)) { _mode = Mode::List; draw(); return; }
        if (ui::hit(kPrepare, x, y)) prepare();
        return;
    }
    const JcWfState st = sv.wf.state();
    if (st == JcWfState::Planned) {
        if (ui::hit(kCancel, x, y)) { sv.wf.cancel(now); _mode = Mode::Setup; draw(); return; }
        if (ui::hit(kGo, x, y)) {
            _action_pending = 0;
            const JcWfResult& r = sv.wf.result();
            char l1[100], l2[100], l3[100];
            const JcWfKind sk = sv.wf.kind();
            const bool tool = sk == JcWfKind::ToolReference || sk == JcWfKind::ToolMeasure || sk == JcWfKind::ToolChange;
            if (tool) {
                snprintf(l1, sizeof(l1), "%s: probe %s up to %.1f mm at %.0f /min.", jc_wf_title(sk), sv.wf.params().tool_dir < 0 ? "DOWN" : "UP",
                         static_cast<double>(sv.wf.params().search), static_cast<double>(sv.wf.params().feed));
            } else {
                snprintf(l1, sizeof(l1), "%s in the ACTIVE WCS G%d (read from FluidNC).", jc_wf_title(sk), 54 + r.wcs);
            }
            snprintf(l2, sizeof(l2), "%u steps, sent ONE at a time; FluidNC executes each.", static_cast<unsigned>(sv.wf.step_count()));
            if (sk == JcWfKind::ToolChange) {
                snprintf(l3, sizeof(l3), "Z rises to %.3f (work), then WAITS for you to change the tool by hand.", static_cast<double>(sv.wf.params().clearance_z));
            } else if (tool) {
                snprintf(l3, sizeof(l3), "You touch the tool to the plate by hand first: the probe must react.");
            } else {
                snprintf(l3, sizeof(l3), "Safe Z clearance %.3f (work).  Spindle stays stopped.", static_cast<double>(sv.wf.params().clearance_z));
            }
            open_custom_confirm("START THIS ROUTINE?", "START");
            dialog.add_line(l1);
            dialog.add_line(l2);
            dialog.add_line(l3, ui::kWarn);
            dialog.add_line("A failed step ends the routine: nothing is retried or resumed.");
            dialog.add_line("CANCEL sends a feed hold. It is NOT an emergency stop.", RED);
            draw();
        }
        return;
    }
    if (st == JcWfState::Running || st == JcWfState::WaitOperator || st == JcWfState::Reading) {
        if (ui::hit(kCancel, x, y)) { sv.wf.cancel(now); draw(); return; }
        if (st == JcWfState::WaitOperator && ui::hit(kGo, x, y)) { sv.wf.continue_after_pause(now); draw(); }
        return;
    }
    if (st == JcWfState::Done && sv.wf.result().valid) {
        const JcWfKind k = sv.wf.kind();
        const bool measured = k == JcWfKind::FindX || k == JcWfKind::FindY || k == JcWfKind::FindXY;
        const bool tool_apply = (k == JcWfKind::ToolMeasure || k == JcWfKind::ToolChange) && sv.wf.result().tlo_ok;
        if (measured && ui::hit(kMoveCentre, x, y)) { _action_pending = 1; request_action(1); return; }
        if (measured && ui::hit(kSetOrigin, x, y)) { _action_pending = 2; request_action(2); return; }
        if (tool_apply && ui::hit(kMoveCentre, x, y)) { _action_pending = 3; request_action(3); return; }
        if (tool_apply && ui::hit(kSetOrigin, x, y)) { _action_pending = 4; request_action(4); return; }
    }
    if (ui::hit(kDone, x, y)) { _mode = Mode::List; draw(); }
}

void JcWorkflowScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 250) return;
    _last_tick = now;
    const JcWfState st = sv.wf.state();
    if ((st == JcWfState::Done || st == JcWfState::Failed || st == JcWfState::Cancelled || st == JcWfState::Planned) && sv.dispatcher.armed()) {
        sv.dispatcher.disarm();   // nothing stays armed once the routine is over
    }
    if (_mode == Mode::Active) {
        if (_drawn_state != static_cast<uint8_t>(sv.wf.state()) || _drawn_step != sv.wf.step_index()) draw();
        if (st == JcWfState::Running || st == JcWfState::WaitOperator) {   // the probe pin, live: contact verification is by hand
            const JcLiveMachineStatus& ms = sv.model.status();
            const bool trig = ms.has_data && strchr(ms.pins, 'P') != nullptr;
            char l[90];
            snprintf(l, sizeof(l), "Probe pin: %s     Machine: %s", trig ? "TRIGGERED" : "clear", sv.model.has_status() ? sv.model.state() : "no data");
            _lines[3].draw(sv.display, { 28, 192, 744, 24 }, l, FontId::Body16, trig ? ui::kWarn : ui::kMuted, ui::kPanel);
        }
    } else if (_mode == Mode::Setup) {
        draw_live_lines();
    }
}

}  // namespace jc8048w550

#endif  // JC_AUTHORITY
