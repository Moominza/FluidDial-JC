// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Plans for the guided routines (Instruction 009A): the ordered, single-command steps each routine submits.
// Included at the end of JcWorkflow.hpp. Pure: it only fills the step list.

#pragma once

namespace jc8048w550 {

namespace wf_detail {
inline JcMachineProfile probe_profile() {
    JcMachineProfile p = jc_default_profile();
    p.max_probe_travel = 500.0f;          // the operator's search distance is validated by jc_wf_validate()
    p.max_probe_feed = kJcSanityMaxFeed;
    return p;
}
}  // namespace wf_detail

// One touch: probe toward `dir` (+1 / -1) along `axis`, optional slow second touch, retract, restore G90.
inline void JcWorkflow::add_touch(uint8_t axis, int dir, uint8_t slot, const char* label) {
    const JcMachineProfile pp = wf_detail::probe_profile();
    char text[44];
    snprintf(text, sizeof(text), "probe %c%s", "XYZ"[axis], dir > 0 ? "+" : "-");
    (void)label;
    JcWfStep& s = push(JcWfOp::Probe, text);
    s.cmd = jc_probe_move(pp, axis, dir * _p.search, _p.feed);
    s.arg = slot;
    if (_p.slow_second) {
        JcWfStep& r = push(JcWfOp::Send, "back off for the slow touch");
        r.cmd = jc_move_relative(axis, -dir * _p.retract);
        r.motion = true; r.relative = true; r.mask = static_cast<uint8_t>(1u << axis); r.target[axis] = -dir * _p.retract;
        snprintf(text, sizeof(text), "slow probe %c%s", "XYZ"[axis], dir > 0 ? "+" : "-");
        JcWfStep& q = push(JcWfOp::Probe, text);
        q.cmd = jc_probe_move(pp, axis, dir * (_p.retract + 1.0f), _p.slow_feed);
        q.arg = slot;
    }
    JcWfStep& b = push(JcWfOp::Send, "retract");
    b.cmd = jc_move_relative(axis, -dir * _p.retract);
    b.motion = true; b.relative = true; b.mask = static_cast<uint8_t>(1u << axis); b.target[axis] = -dir * _p.retract;
    push(JcWfOp::Send, "restore absolute mode (G90)").cmd = jc_restore_absolute();
}

// A pair of touches on opposite faces of one feature, `hi` = contact probing +, `lo` = contact probing -.
inline void JcWorkflow::add_pair(uint8_t axis, const char* tag, uint8_t slot_hi, uint8_t slot_lo, bool outside) {
    (void)tag;
    if (!outside) {
        add_touch(axis, +1, slot_hi, "");
        add_touch(axis, -1, slot_lo, "");
        return;
    }
    // Outside: touch the near face, lift to the clearance, cross to the far side, wait for the operator to
    // confirm that lowering is clear, lower, touch the far face.
    add_touch(axis, +1, slot_lo, "");
    add_goto_z(_p.clearance_z, "raise to the safe clearance");
    JcWfStep& t = push(JcWfOp::Send, "cross to the far side");
    const float d = _p.est_width + 2.0f * (_p.retract + _p.tip_radius) + 2.0f;
    t.cmd = jc_move_relative(axis, d);
    t.motion = true; t.relative = true; t.mask = static_cast<uint8_t>(1u << axis); t.target[axis] = d;
    push(JcWfOp::Send, "restore absolute mode (G90)").cmd = jc_restore_absolute();
    add_wait("far side clear? CONTINUE lowers Z");
    add_goto_z(_probe_z, "lower to the probing height");
    add_touch(axis, -1, slot_hi, "");
}

inline bool JcWorkflow::build_plan() {
    _n = 0;
    _action = false;
    const bool goto_kind = jc_wf_is_goto(_kind);
    _probe_z = _status.wpos_valid ? _status.wpos[2] : 0.0f;
    _y_start = _status.wpos_valid ? _status.wpos[1] : 0.0f;
    add_check(JcWfCheck::StatusIdle, "machine Idle");
    add_check(JcWfCheck::SpindleOff, "spindle stopped (never started here)");
    if (_kind == JcWfKind::GoToZ) {
        // Z only: work Z0 of the active WCS (not machine home). XY and A are never commanded.
        if (!_status.wpos_valid) { snprintf(_reason, sizeof(_reason), "work position unknown"); return false; }
        add_check(JcWfCheck::WcsUnchanged, "active WCS unchanged");
        add_wait("CONFIRM: Z to work Z0 (active WCS)");
        add_check(JcWfCheck::StatusIdle, "machine still Idle");
        add_check(JcWfCheck::SpindleOff, "spindle still stopped");
        add_check(JcWfCheck::WcsUnchanged, "active WCS still unchanged");
        add_goto_z(0.0f, "move to work Z0");
        return true;
    }
    if (goto_kind) {
        add_check(JcWfCheck::ClearanceNotBelow, "clearance not below the current Z");
        if (!_status.wpos_valid) { snprintf(_reason, sizeof(_reason), "work position unknown"); return false; }
        if (fabsf(_p.clearance_z - _status.wpos[2]) > 0.001f) add_goto_z(_p.clearance_z, "raise to the safe clearance");
        add_wait("clearance reached: CONTINUE to move in X/Y");
        add_check(JcWfCheck::StatusIdle, "machine still Idle");
        add_check(JcWfCheck::SpindleOff, "spindle still stopped");
        add_check(JcWfCheck::WcsUnchanged, "active WCS unchanged");
        const bool hx = _kind != JcWfKind::GoToY, hy = _kind != JcWfKind::GoToX;
        JcWfStep& m = push(JcWfOp::Send, _kind == JcWfKind::GoToXY ? "move to work X0 Y0" : (hx ? "move to work X0" : "move to work Y0"));
        m.cmd = jc_goto_work_xy(hx, 0.0f, hy, 0.0f);
        m.motion = true;
        m.mask = static_cast<uint8_t>((hx ? 1 : 0) | (hy ? 2 : 0));
        if (_kind == JcWfKind::GoToXYZ) {
            // XY is established above the clearance height; Z0 needs its OWN confirmation.
            add_wait("XY reached. CONFIRM to lower to work Z0");
            add_check(JcWfCheck::StatusIdle, "machine still Idle");
            add_check(JcWfCheck::SpindleOff, "spindle still stopped");
            add_check(JcWfCheck::WcsUnchanged, "active WCS unchanged");
            add_goto_z(0.0f, "lower to work Z0");
        }
        return true;
    }
    const bool tool_kind = _kind == JcWfKind::ToolReference || _kind == JcWfKind::ToolMeasure || _kind == JcWfKind::ToolChange;
    if (tool_kind) {
        if (_kind == JcWfKind::ToolChange) {
            add_check(JcWfCheck::ClearanceNotBelow, "clearance not below the current Z");
            if (!_status.wpos_valid) { snprintf(_reason, sizeof(_reason), "work position unknown"); return false; }
            if (fabsf(_p.clearance_z - _status.wpos[2]) > 0.001f) add_goto_z(_p.clearance_z, "raise to the safe clearance");
            add_check(JcWfCheck::SpindleOff, "spindle stopped (stop it with its own control)");
            add_wait("CHANGE THE TOOL BY HAND, keep clear, then CONTINUE");
            add_check(JcWfCheck::StatusIdle, "machine still Idle");
            add_check(JcWfCheck::SpindleOff, "spindle still stopped");
        }
        add_check(JcWfCheck::ProbeReady, "probe input assigned and not triggered");
        // Probe-contact verification BEFORE any probing move: the operator touches the tool to the plate by hand and
        // the controller's own Pn:P must appear, then clear.
        push(JcWfOp::AwaitTouch, "TOUCH the tool to the plate by hand (probe must show contact)");
        push(JcWfOp::AwaitRelease, "release it (probe must clear)");
        add_check(JcWfCheck::StatusIdle, "machine Idle");
        add_touch(2, _p.tool_dir, 0, "");
        JcWfStep& c = push(JcWfOp::Compute, _kind == JcWfKind::ToolReference ? "record the reference contact" : "calculate the tool length offset");
        c.arg = static_cast<uint8_t>(_kind == JcWfKind::ToolReference ? JcWfCalc::ToolRef : JcWfCalc::ToolLength);
        return true;
    }
    add_check(JcWfCheck::ProbeReady, "probe input assigned and not triggered");
    const bool outside = _p.feature == JcFeatureKind::Outside;
    if (outside || _kind == JcWfKind::FindRotary) {
        add_check(JcWfCheck::ClearanceNotBelow, "clearance not below the current Z");
        if (!_status.wpos_valid) { snprintf(_reason, sizeof(_reason), "work position unknown"); return false; }
    }
    auto compute_axis = [&](uint8_t axis, uint8_t hi, uint8_t lo) {
        JcWfStep& c = push(JcWfOp::Compute, axis == 0 ? "calculate the X centre" : "calculate the Y centre");
        c.arg = static_cast<uint8_t>(JcWfCalc::AxisCentre);
        c.axis = axis; c.slot_hi = hi; c.slot_lo = lo;
    };
    switch (_kind) {
        case JcWfKind::FindX:
            add_pair(0, "X", 0, 1, outside);
            compute_axis(0, 0, 1);
            break;
        case JcWfKind::FindY:
            add_pair(1, "Y", 0, 1, outside);
            compute_axis(1, 0, 1);
            break;
        case JcWfKind::FindXY: {
            add_pair(0, "X", 0, 1, false);
            compute_axis(0, 0, 1);
            add_wait("move to the X centre inside the feature? CONTINUE");
            JcWfStep& m = push(JcWfOp::Send, "move to the measured X centre");
            m.dyn = 1;
            m.motion = true; m.mask = 1;
            add_pair(1, "Y", 2, 3, false);
            compute_axis(1, 2, 3);
            break;
        }
        case JcWfKind::FindRotary: {
            // A is parallel to X. Two stations along X; each: Y pair across the cylinder, then the top contact.
            for (uint8_t i = 0; i < 2; ++i) {
                const uint8_t base = static_cast<uint8_t>(3 * i);
                if (i == 1) {
                    add_goto_z(_p.clearance_z, "raise to the safe clearance");
                    JcWfStep& mv = push(JcWfOp::Send, "move to station 2 along the axis");
                    mv.cmd = jc_move_relative(0, _p.station_spacing);
                    mv.motion = true; mv.relative = true; mv.mask = 1; mv.target[0] = _p.station_spacing;
                    push(JcWfOp::Send, "restore absolute mode (G90)").cmd = jc_restore_absolute();
                    JcWfStep& back = push(JcWfOp::Send, "return to the start side");
                    back.cmd = jc_goto_work_xy(false, 0.0f, true, _y_start);
                    back.motion = true; back.mask = 2; back.target[1] = _y_start;
                    add_wait("beside the cylinder at station 2 and clear? CONTINUE lowers Z");
                    add_goto_z(_probe_z, "lower to the probing height");
                }
                add_pair(1, "Y", static_cast<uint8_t>(base + 1), base, true);
                JcWfStep& c = push(JcWfOp::Compute, "calculate the cylinder centre across Y");
                c.arg = static_cast<uint8_t>(JcWfCalc::AxisCentre);
                c.axis = 1; c.slot_hi = static_cast<uint8_t>(base + 1); c.slot_lo = base;
                add_goto_z(_p.clearance_z, "raise to the safe clearance");
                JcWfStep& over = push(JcWfOp::Send, "move over the cylinder centre");
                over.dyn = 2;
                over.motion = true; over.mask = 2;
                add_wait("probe above the centre-line? CONTINUE probes the top");
                const JcMachineProfile pp = wf_detail::probe_profile();
                JcWfStep& top = push(JcWfOp::Probe, "probe Z- onto the top");
                top.cmd = jc_probe_move(pp, 2, -_p.search, _p.feed);
                top.arg = static_cast<uint8_t>(base + 2);
                JcWfStep& up = push(JcWfOp::Send, "retract");
                up.cmd = jc_move_relative(2, _p.retract);
                up.motion = true; up.relative = true; up.mask = 4; up.target[2] = _p.retract;
                push(JcWfOp::Send, "restore absolute mode (G90)").cmd = jc_restore_absolute();
                JcWfStep& st = push(JcWfOp::Compute, "record the station");
                st.arg = static_cast<uint8_t>(JcWfCalc::Station);
                st.axis = i;
                st.slot_hi = static_cast<uint8_t>(base + 1); st.slot_lo = base; st.slot_out = static_cast<uint8_t>(base + 2);
                st.arg = static_cast<uint8_t>(JcWfCalc::Station);
            }
            JcWfStep& fin = push(JcWfOp::Compute, "calculate the centreline");
            fin.arg = static_cast<uint8_t>(JcWfCalc::Centreline);
            add_goto_z(_p.clearance_z, "raise to the safe clearance");
            break;
        }
        default: break;
    }
    return true;
}

}  // namespace jc8048w550
