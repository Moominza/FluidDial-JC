// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Guided positioning and probing routines (Instruction 009A). FluidDial orchestrates; FluidNC plans and
// executes every motion and every probe. The engine is a one-step-at-a-time sequencer:
//
//   Reading  -> read-only $G and $# (active WCS and offsets) so the preview shows the real WCS
//   Planned  -> preview + parameters, waits for the operator's START (the single plan confirmation)
//   Running  -> submits ONE command, waits for FluidNC's reply, then (for a move) for the machine to be
//               Idle at the target (acknowledgement is NOT completion), then the next step
//   WaitOperator -> explicit pauses (the XY move after the clearance move, lowering Z beside stock)
//   Done / Failed / Cancelled
//
// Rules: a failed, rejected, alarmed, timed-out, cancelled or disconnected step ends the routine; nothing is
// retried, resumed or replayed; a centre is only computed from complete contacts; the offset is only written
// when the operator asks (request_set_origin) and is read back and compared. All commands go through the
// gate via JcWfHost::send; the engine never touches a transport.
//
// Pure: no I/O.

#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "JcCommandBuilder.hpp"
#include "JcCommandSystem.hpp"
#include "JcProbeCalc.hpp"

namespace jc8048w550 {

enum class JcWfKind : uint8_t { GoToXY, GoToX, GoToY, GoToZ, GoToXYZ, FindX, FindY, FindXY, FindRotary, ToolReference, ToolMeasure, ToolChange };

inline const char* jc_wf_title(JcWfKind k) {
    switch (k) {
        case JcWfKind::GoToXY: return "GO TO WORK X0 Y0";
        case JcWfKind::GoToX: return "GO TO WORK X0";
        case JcWfKind::GoToY: return "GO TO WORK Y0";
        case JcWfKind::GoToZ: return "GO TO WORK Z0";
        case JcWfKind::GoToXYZ: return "GO TO WORK X0 Y0 Z0";
        case JcWfKind::FindX: return "FIND X CENTRE";
        case JcWfKind::FindY: return "FIND Y CENTRE";
        case JcWfKind::FindXY: return "FIND XY CENTRE";
        case JcWfKind::FindRotary: return "FIND ROTARY A CENTRELINE";
        case JcWfKind::ToolReference: return "MEASURE REFERENCE TOOL";
        case JcWfKind::ToolMeasure: return "MEASURE TOOL LENGTH";
        case JcWfKind::ToolChange: return "MANUAL TOOL CHANGE + MEASURE";
    }
    return "?";
}

// Built-in positioning actions. All targets are WORK coordinates of the currently selected G54..G59 system (never
// machine home / G53); the A axis is never commanded.
inline bool jc_wf_is_goto(JcWfKind k) {
    return k == JcWfKind::GoToXY || k == JcWfKind::GoToX || k == JcWfKind::GoToY || k == JcWfKind::GoToZ || k == JcWfKind::GoToXYZ;
}

struct JcWfParams {
    // Operator-defined safe Z clearance in WORK coordinates. Never defaulted: Z0 is not assumed safe.
    bool  clearance_set = false;
    float clearance_z = 0.0f;
    // Probing.
    float search = 30.0f;          // maximum travel of each touch (must exceed the feature width for the second wall)
    float feed = 100.0f;           // mm/min
    float retract = 2.0f;          // mm
    bool  slow_second = false;     // repeat each touch at slow_feed for a better contact
    float slow_feed = 20.0f;
    float tip_radius = 1.0f;       // stylus tip radius (probe diameter / 2)
    float contact_timeout_s = 30.0f;
    JcFeatureKind feature = JcFeatureKind::Inside;
    float expected_size = 0.0f;    // 0 = not given; with it a wrong radius / feature is flagged
    float tolerance = 0.5f;
    float est_width = 0.0f;        // outside features: used for the traverse to the far side
    // Rotary (A parallel to X).
    float cylinder_diameter = 0.0f;
    float station_spacing = 0.0f;
    // Tool length (Instruction 010). The probed surface is a touch plate of this thickness, or a fixed reference
    // surface (thickness 0). The thickness cancels when the reference and the new tool touch the same surface; it
    // is NOT part of the tool length offset and it never changes a work offset.
    float plate_thickness = 0.0f;
    int   tool_dir = -1;           // -1: probe down (touch plate), +1: probe up (fixed sensor above the tool)
    bool  has_ref = false;         // a reference contact (machine Z) exists
    float ref_mz = 0.0f;           // machine Z at which the reference tool touched
    int   tool_number = 0;         // the operator's label for the tool being fitted (display only; T / M6 are never sent)
};

inline bool jc_wf_validate(JcWfKind kind, const JcWfParams& p, char* why, size_t size) {
    auto bad = [&](const char* m) { snprintf(why, size, "%s", m); return false; };
    const bool goto_kind = jc_wf_is_goto(kind);
    if (kind == JcWfKind::GoToXYZ && p.clearance_set && isfinite(p.clearance_z) && !(p.clearance_z > 0.0f))
        return bad("the clearance height must be ABOVE work Z0 (it is separate from Z0)");
    if (kind == JcWfKind::GoToZ) return true;   // a Z-only move needs no clearance; the operator confirms the move itself
    if (!p.clearance_set || !isfinite(p.clearance_z)) {
        if (goto_kind || p.feature == JcFeatureKind::Outside || kind == JcWfKind::FindRotary)
            return bad("set a safe Z clearance (work Z): Z0 is not assumed safe");
    }
    if (goto_kind) return true;
    const bool tool_kind = kind == JcWfKind::ToolReference || kind == JcWfKind::ToolMeasure || kind == JcWfKind::ToolChange;
    if (tool_kind) {
        if (p.tool_dir != -1 && p.tool_dir != 1) return bad("probe direction must be down (-) or up (+)");
        if (!(p.plate_thickness >= 0.0f && p.plate_thickness <= 100.0f)) return bad("plate thickness must be 0..100 mm");
        if (kind != JcWfKind::ToolReference && !p.has_ref) return bad("measure the REFERENCE tool first: no reference contact stored");
        if (kind == JcWfKind::ToolChange && (!p.clearance_set || !isfinite(p.clearance_z))) return bad("set a safe Z clearance (work Z): Z0 is not assumed safe");
        if (p.tool_number < 0 || p.tool_number > 999) return bad("tool number must be 0..999");
    }
    if (!(p.search > 0.0f && p.search <= 500.0f)) return bad("search distance must be 0..500 mm");
    if (!(p.feed >= 1.0f && p.feed <= kJcSanityMaxFeed)) return bad("probe feed not valid");
    if (!(p.retract >= 0.1f && p.retract <= 50.0f)) return bad("retract must be 0.1..50 mm");
    if (!(p.tip_radius >= 0.0f && p.tip_radius <= 25.0f) || p.tip_radius >= p.search) return bad("probe radius not valid (0..25 mm, below the search distance)");
    if (p.slow_second && !(p.slow_feed >= 1.0f && p.slow_feed <= p.feed)) return bad("slow feed must be 1..probe feed");
    if (!(p.contact_timeout_s >= 1.0f && p.contact_timeout_s <= 300.0f)) return bad("contact timeout must be 1..300 s");
    if (p.expected_size < 0.0f || !(p.tolerance > 0.0f)) return bad("expected size / tolerance not valid");
    if (tool_kind) return true;   // the feature / size rules below belong to the centre-finding routines
    if (kind == JcWfKind::FindXY && p.feature == JcFeatureKind::Outside) {
        return bad("XY centre supports inside features; probe an outside feature with FIND X then FIND Y");
    }
    if (p.feature == JcFeatureKind::Outside && kind != JcWfKind::FindXY && !(p.est_width > 0.0f)) return bad("outside feature: enter its approximate width");
    if (kind == JcWfKind::FindRotary) {
        if (!(p.cylinder_diameter > 0.0f) || !(p.station_spacing >= 1.0f)) return bad("enter the cylinder diameter and the station spacing (>= 1 mm)");
        if (!(p.est_width > 0.0f)) return bad("enter the approximate cylinder width for the traverse");
    }
    return true;
}

// ---- host and status -------------------------------------------------------------------------------------

class JcWfHost {
public:
    virtual ~JcWfHost() {}
    // Gate + transport for one command. True only when the command was handed to the transport; otherwise
    // *why says why (gate decision, busy, policy, no transport).
    virtual bool submit(const JcCommand& c, const char** why) = 0;
    // One read-only query line ($G, $#). False if it could not be sent now.
    virtual bool query(const char* line) = 0;
    // Software stop (feed hold). Best effort; never part of the sequence.
    virtual void stop() = 0;
};

struct JcWfStatus {
    bool        fresh = false;
    const char* state = "";
    bool        wpos_valid = false;
    float       wpos[4] = { 0, 0, 0, 0 };   // work coordinates
    float       spindle = 0.0f;
    const char* pins = "";                  // Pn: letters (X Y Z A P ...)
    uint32_t    sequence = 0;
    JcTri       probe_configured = JcTri::Unknown;   // probe input assigned on the controller (read of settings)
};

enum class JcWfState : uint8_t { Idle, Reading, Planned, Running, WaitOperator, Done, Failed, Cancelled };

enum class JcWfOp : uint8_t { Check, Send, Probe, WaitOperator, Compute, QueryWcs, QueryOffsets, VerifyOrigin, AwaitTouch, AwaitRelease, VerifyTlo };
enum class JcWfCheck : uint8_t { SpindleOff, ProbeReady, ClearanceNotBelow, StatusIdle, WcsUnchanged };
enum class JcWfCalc : uint8_t { AxisCentre, Station, Centreline, ToolRef, ToolLength };

struct JcWfStep {
    JcWfOp    op = JcWfOp::Check;
    JcCommand cmd;
    uint8_t   arg = 0;            // check id / slot / calc id
    uint8_t   axis = 0;           // calc axis
    uint8_t   slot_hi = 0, slot_lo = 0, slot_out = 0;
    uint8_t   dyn = 0;            // 1 / 2: target computed at run time from the measured X / Y centre
    // Motion completion (Idle at the target, work coordinates).
    bool      motion = false;
    uint8_t   mask = 0;           // axes verified
    bool      relative = false;   // target = work position at submission + delta
    float     target[4] = { 0, 0, 0, 0 };
    char      label[44] = "";
};

constexpr uint8_t kJcWfMaxSteps = 100;
constexpr uint8_t kJcWfSlots = 8;

struct JcWfResult {
    bool   valid = false;
    JcCentre x, y;                 // machine-coordinate centres (valid flags in .ok)
    JcCentreline line;
    JcStation st[2];
    int    wcs = -1;               // 0..5
    float  g5x[4] = { 0, 0, 0, 0 };
    float  g92[4] = { 0, 0, 0, 0 };
    bool   readback_done = false, readback_ok = false;
    // Tool length (Instruction 010). Machine Z of the contact, the offset it implies and the readback of an applied offset.
    bool   tool_contact_ok = false;
    float  tool_contact_mz = 0.0f;
    bool   tlo_ok = false;
    float  tlo_proposed = 0.0f;
    bool   tlo_readback_done = false, tlo_readback_ok = false;
    float  tlo_read = 0.0f;
    char   summary[3][72] = { "", "", "" };
};

class JcWorkflow {
public:
    explicit JcWorkflow(JcWfHost* host = nullptr) : _host(host) {}
    void set_host(JcWfHost* h) { _host = h; }

    // ---- operator entry points ----
    // Validates, then reads the active WCS and the offset table (read-only) before the preview is shown.
    bool prepare(JcWfKind kind, const JcWfParams& p, const JcWfStatus& st, uint32_t now) {
        if (running()) { snprintf(_reason, sizeof(_reason), "a routine is already running"); return false; }
        reset_run();
        _kind = kind;
        _p = p;
        if (kind == JcWfKind::FindRotary) { _p.feature = JcFeatureKind::Outside; _p.expected_size = p.cylinder_diameter; }
        if (!jc_wf_validate(kind, p, _reason, sizeof(_reason))) { _state = JcWfState::Failed; return false; }
        if (!st.fresh) { return fail_now("no fresh controller status"); }
        _state = JcWfState::Reading;
        _read_phase = 0;
        _step_started = now;
        _status = st;
        return true;
    }

    // START: the single confirmation of the previewed plan.
    bool start(uint32_t now) {
        if (_state != JcWfState::Planned) return false;
        _state = JcWfState::Running;
        _idx = 0;
        _phase = Phase::Begin;
        _step_started = now;
        return true;
    }

    void continue_after_pause(uint32_t now) {
        if (_state != JcWfState::WaitOperator) return;
        _state = JcWfState::Running;
        _phase = Phase::Begin;
        _step_started = now;
        ++_idx;
    }

    void cancel(uint32_t now) {
        (void)now;
        if (_state == JcWfState::Idle || _state == JcWfState::Done || _state == JcWfState::Failed || _state == JcWfState::Cancelled) return;
        if (_phase == Phase::AwaitResult || _phase == Phase::AwaitMotion) { if (_host) _host->stop(); }
        snprintf(_reason, sizeof(_reason), "cancelled by the operator; nothing further will be sent");
        _state = JcWfState::Cancelled;
    }

    // Post-result actions: never automatic. They run as a short plan of their own.
    bool request_move_to_centre(uint32_t now) {
        if (_state != JcWfState::Done || !_res.valid) return false;
        _n = 0;
        const bool x = (_kind == JcWfKind::FindX || _kind == JcWfKind::FindXY) && _res.x.ok;
        const bool y = (_kind == JcWfKind::FindY || _kind == JcWfKind::FindXY) && _res.y.ok;
        if (!x && !y) return false;
        _tlo_action = 0;
        snprintf(_title, sizeof(_title), "MOVE TO THE CENTRE");
        const bool outside = _p.feature == JcFeatureKind::Outside;
        add_check(JcWfCheck::SpindleOff, "spindle stopped");
        add_check(JcWfCheck::StatusIdle, "machine Idle");
        add_check(JcWfCheck::WcsUnchanged, "active WCS unchanged");
        if (outside) add_goto_z(_p.clearance_z, "raise to clearance");
        if (outside) add_wait("probe is clear to move over the feature?");
        float wx = 0, wy = 0;
        if (x) wx = work_of(0, _res.x.centre);
        if (y) wy = work_of(1, _res.y.centre);
        JcWfStep& s = push(JcWfOp::Send, "move to the centre");
        s.cmd = jc_goto_work_xy(x, wx, y, wy);
        s.motion = true;
        s.mask = static_cast<uint8_t>((x ? 1 : 0) | (y ? 2 : 0));
        s.target[0] = wx;
        s.target[1] = wy;
        return begin_action(now, JcWfOp::QueryWcs);
    }

    // SET WORK ... : G10 L2 on the ACTIVE WCS from the measured centre (never G92), then read back and compare.
    bool request_set_origin(uint32_t now) {
        if (_state != JcWfState::Done || !_res.valid || _res.wcs < 0) return false;
        const bool x = (_kind == JcWfKind::FindX || _kind == JcWfKind::FindXY) && _res.x.ok;
        const bool y = (_kind == JcWfKind::FindY || _kind == JcWfKind::FindXY) && _res.y.ok;
        if (!x && !y) return false;
        _n = 0;
        _tlo_action = 0;
        snprintf(_title, sizeof(_title), "SET WORK ORIGIN");
        add_check(JcWfCheck::StatusIdle, "machine Idle");
        add_check(JcWfCheck::WcsUnchanged, "active WCS unchanged");
        JcWfStep& s = push(JcWfOp::Send, "set work origin (G10 L2)");
        s.cmd = jc_set_wcs_origin(static_cast<uint8_t>(_res.wcs), x, x ? _res.x.centre - _res.g92[0] : 0.0f, y, y ? _res.y.centre - _res.g92[1] : 0.0f, false, 0.0f);
        s.arg = static_cast<uint8_t>((x ? 1 : 0) | (y ? 2 : 0));
        push(JcWfOp::QueryOffsets, "read the offsets back");
        JcWfStep& v = push(JcWfOp::VerifyOrigin, "compare with the request");
        v.arg = s.arg;
        return begin_action(now, JcWfOp::QueryWcs);
    }

    // APPLY TOOL LENGTH OFFSET: G43.1 Z<offset> from the measured contact, then read [TLO:] back and compare.
    // Separate, confirmed action; never automatic; never touches a work offset.
    bool request_apply_tlo(uint32_t now) {
        if (_state != JcWfState::Done || !_res.tlo_ok || (_kind != JcWfKind::ToolMeasure && _kind != JcWfKind::ToolChange)) return false;
        _n = 0;
        add_check(JcWfCheck::StatusIdle, "machine Idle");
        add_check(JcWfCheck::SpindleOff, "spindle stopped");
        _tlo_expect = _res.tlo_proposed;
        _tlo_action = 1;
        _res.tlo_readback_done = false; _res.tlo_readback_ok = false;
        snprintf(_title, sizeof(_title), "APPLY TOOL LENGTH OFFSET");
        push(JcWfOp::Send, "apply the tool length offset (G43.1)").cmd = jc_set_tlo(_res.tlo_proposed);
        push(JcWfOp::QueryOffsets, "read [TLO:] back");
        push(JcWfOp::VerifyTlo, "compare with the request");
        return begin_action(now, JcWfOp::QueryOffsets);
    }
    // CLEAR TOOL LENGTH OFFSET: G49, read back, must read 0.
    bool request_clear_tlo(uint32_t now) {
        if (running()) return false;
        _n = 0;
        add_check(JcWfCheck::StatusIdle, "machine Idle");
        _tlo_expect = 0.0f;
        _tlo_action = 2;
        _res.tlo_readback_done = false; _res.tlo_readback_ok = false;
        snprintf(_title, sizeof(_title), "CLEAR TOOL LENGTH OFFSET");
        push(JcWfOp::Send, "cancel the tool length offset (G49)").cmd = jc_clear_tlo();
        push(JcWfOp::QueryOffsets, "read [TLO:] back");
        push(JcWfOp::VerifyTlo, "compare with the request");
        _reason[0] = '\0';
        return begin_action(now, JcWfOp::QueryOffsets);
    }

    // ---- events from the application ----
    void on_status(const JcWfStatus& st) {
        _status = st;
        if (st.sequence != _last_seq) { _last_seq = st.sequence; ++_frames; }
        if (_state == JcWfState::Running || _state == JcWfState::WaitOperator) {
            if (st.fresh && st.state != nullptr && strcmp(st.state, "Alarm") == 0) fail_now("controller entered ALARM; routine stopped");
        }
    }
    void on_line(const char* line) {
        const JcResponse r = jc_classify_line(line);
        if (r.kind == JcRespKind::GCodeState) {
            const int w = jc_wcs_from_gc(r.value);
            if (w >= 0) {
                _wcs_seen = w;
                _gc_frames++;
                }
        } else if (r.kind == JcRespKind::Info) {
            JcProbeReport pr;
            if (strcmp(r.key, "PRB") == 0) {
                if (jc_parse_prb(r, pr)) { _prb = pr; _prb_seq = ++_prb_counter; } else { _prb = JcProbeReport(); }
            } else if (strcmp(r.key, "TLO") == 0) {
                float t = 0.0f;
                if (jc_parse_tlo_z(r.value, &t)) { _tlo = t; _tlo_known = true; }
            } else {
                const int idx = jc_offset_tag_index(r.key);
                if (idx >= 0 && idx <= 8) {
                    float v[4]; uint8_t n;
                    if (jc_parse_coords(r.value, v, n)) {
                        memcpy(_offsets[idx], v, sizeof(v));
                        _offset_known[idx] = true;
                    }
                }
            }
        } else if (r.kind == JcRespKind::Ok) {
            if (_query_wait) { _query_wait = false; _query_got = true; }
        } else if ((r.kind == JcRespKind::Error || r.kind == JcRespKind::Alarm) && _query_wait) {
            _query_wait = false;
            fail_now("the read-only query was refused by FluidNC");
        }
    }
    // The command link's finished request (acknowledgement, rejection, alarm, timeout, link loss).
    void on_result(const JcCommandResult& r) {
        if (_state != JcWfState::Running || _phase != Phase::AwaitResult) return;
        _last_result = r;
        char line[96];
        jc_result_line(r, line, sizeof(line));
        const JcWfStep& s = _plan[_idx];
        if (r.outcome == JcExecOutcome::Acked || r.outcome == JcExecOutcome::SentNoAck) {
            if (s.op == JcWfOp::Probe) {
                if (!_prb.valid || _prb_seq == _prb_seen) { fail_now("probe was acknowledged but no [PRB:] record arrived: no centre"); return; }
                _prb_seen = _prb_seq;
                if (!_prb.contact) { fail_now("NO CONTACT (probe reported 0)"); return; }
                _slot[s.arg] = _prb;
                _slot_ok[s.arg] = true;
                _phase = Phase::AwaitMotion;   // wait for Idle after the contact
                _ack_frame = _frames;
                return;
            }
            if (s.motion) { _phase = Phase::AwaitMotion; _ack_frame = _frames; return; }
            advance();
            return;
        }
        if (r.outcome == JcExecOutcome::Alarm && s.op == JcWfOp::Probe) {
            snprintf(_reason, sizeof(_reason), r.code == 5 ? "probe ALREADY TRIGGERED (ALARM:5): nothing probed" : "NO CONTACT within the search distance (ALARM:%d)", r.code);
            _state = JcWfState::Failed;
            return;
        }
        snprintf(_reason, sizeof(_reason), "%s", line);
        _state = JcWfState::Failed;
    }
    void on_link_lost() {
        if (_state == JcWfState::Running || _state == JcWfState::WaitOperator || _state == JcWfState::Reading || _state == JcWfState::Planned) {
            snprintf(_reason, sizeof(_reason), "connection lost; routine ended, NOT resumed");
            _state = JcWfState::Failed;
        }
    }

    void service(uint32_t now) {
        _now = now;
        if (_state == JcWfState::Reading) { service_reading(now); return; }
        if (_state != JcWfState::Running) return;
        if (!_status.fresh) { fail_now("status went stale; routine stopped"); return; }
        const JcWfStep& s = _plan[_idx];
        switch (_phase) {
            case Phase::Begin: begin_step(s, now); break;
            case Phase::AwaitQuery:
                if (_query_got) {
                    if (_recheck_wcs) {
                        _recheck_wcs = false;
                        if (_wcs_seen != _wcs) {
                            char m[96];
                            snprintf(m, sizeof(m), "active WCS changed (G%d -> G%d): nothing was modified", 54 + _wcs, 54 + _wcs_seen);
                            fail_now(m);
                            break;
                        }
                    }
                    advance();
                }
                else if (now - _step_started > 4000u) fail_now("no reply to the read-only query");
                break;
            case Phase::AwaitResult: {
                const uint32_t limit = s.op == JcWfOp::Probe ? static_cast<uint32_t>(_p.contact_timeout_s * 1000.0f) : 30000u;
                if (now - _step_started > limit) {
                    if (_host) _host->stop();
                    fail_now(s.op == JcWfOp::Probe ? "no contact before the contact timeout; stopped" : "no reply before the timeout; stopped");
                }
                break;
            }
            case Phase::AwaitMotion: check_motion(s, now); break;
            case Phase::AwaitTouch:
                if (_status.pins != nullptr && strchr(_status.pins, 'P') != nullptr) { _phase = Phase::Begin; advance(); }
                else if (now - _step_started > 60000u) fail_now("NO PROBE CONTACT SEEN in 60 s: touch the tool to the plate by hand; check the probe wiring");
                break;
            case Phase::AwaitRelease:
                if (_status.pins == nullptr || strchr(_status.pins, 'P') == nullptr) advance();
                else if (now - _step_started > 60000u) fail_now("probe still shows contact after 60 s: release the tool from the plate");
                break;
        }
    }

    // ---- observers ----
    JcWfState state() const { return _state; }
    bool running() const { return _state == JcWfState::Reading || _state == JcWfState::Running || _state == JcWfState::WaitOperator; }
    JcWfKind kind() const { return _kind; }
    // 0 none, 1 apply, 2 clear: which tool-offset action ran last (its readback is in result()).
    uint8_t tlo_action() const { return _tlo_action; }
    // Title of what is running: an action's own name, else the routine's.
    const char* title() const { return _title[0] ? _title : jc_wf_title(_kind); }
    bool tlo_known() const { return _tlo_known; }
    float tlo() const { return _tlo; }
    const char* reason() const { return _reason; }
    uint8_t step_index() const { return _idx; }
    uint8_t step_count() const { return _n; }
    const char* step_label() const { return _idx < _n ? _plan[_idx].label : ""; }
    const JcWfResult& result() const { return _res; }
    int wcs() const { return _wcs; }
    float work_coord(uint8_t axis, float machine) const { return work_of(axis, machine); }
    const JcWfParams& params() const { return _p; }
    const JcWfStep& step(uint8_t i) const { return _plan[i]; }
    const float* offset(uint8_t idx) const { return _offsets[idx]; }
    bool  wpos_known() const { return _status.wpos_valid; }
    float wpos_now(uint8_t axis) const { return _status.wpos[axis < 4 ? axis : 0]; }   // work position from the last status report
    bool probe_slot_ok(uint8_t i) const { return i < kJcWfSlots && _slot_ok[i]; }
    uint32_t commands_sent() const { return _sent; }
    // One line per planned step for the preview.
    void preview_line(uint8_t i, char* out, size_t size) const {
        if (i >= _n) { out[0] = '\0'; return; }
        snprintf(out, size, "%u. %s", static_cast<unsigned>(i) + 1u, _plan[i].label);
    }

private:
    enum class Phase : uint8_t { Begin, AwaitQuery, AwaitResult, AwaitMotion, AwaitTouch, AwaitRelease };

    void reset_run() {
        _state = JcWfState::Idle;
        _n = 0; _idx = 0;
        _reason[0] = '\0';
        _res = JcWfResult();
        memset(_slot_ok, 0, sizeof(_slot_ok));
        memset(_offset_known, 0, sizeof(_offset_known));
        memset(_offsets, 0, sizeof(_offsets));
        _prb = JcProbeReport();
        _wcs = -1; _wcs_seen = -1;
        _query_wait = _query_got = _gc_wait = false;
        _tlo = 0.0f; _tlo_known = false; _tlo_expect = 0.0f;
        _tlo_action = 0; _title[0] = '\0'; _action = false;
        _sent = 0;
        _frames = 0; _prb_seen = _prb_seq;
    }

    bool fail_now(const char* why) {
        snprintf(_reason, sizeof(_reason), "%s", why);
        _state = JcWfState::Failed;
        return false;
    }

    JcWfStep& push(JcWfOp op, const char* label) {
        JcWfStep& s = _plan[_n < kJcWfMaxSteps ? _n : kJcWfMaxSteps - 1];
        if (_n < kJcWfMaxSteps) ++_n;
        s = JcWfStep();
        s.op = op;
        snprintf(s.label, sizeof(s.label), "%s", label);
        return s;
    }
    void add_check(JcWfCheck c, const char* label) { push(JcWfOp::Check, label).arg = static_cast<uint8_t>(c); }
    void add_wait(const char* label) { push(JcWfOp::WaitOperator, label); }
    void add_goto_z(float z, const char* label) {
        JcWfStep& s = push(JcWfOp::Send, label);
        s.cmd = jc_goto_work_z(z);
        s.motion = true; s.mask = 4; s.target[2] = z;
    }
    float work_of(uint8_t axis, float machine) const {
        const float o = (_wcs >= 0 && _wcs <= 5) ? _offsets[_wcs][axis] : 0.0f;
        return machine - o - _offsets[8][axis];
    }
    float contact(uint8_t slot, uint8_t axis) const { return _slot[slot].pos[axis]; }

    // The plan is built once the active WCS and offsets are known.
    void service_reading(uint32_t now) {
        if (_read_phase == 0) {
            if (!_host || !_host->query("$G")) { fail_now("could not send the read-only $G query"); return; }
            _query_wait = true; _query_got = false; _step_started = now; _read_phase = 1;
        } else if (_read_phase == 1) {
            if (_query_got) {
                _wcs = _wcs_seen;
                if (_wcs < 0) { fail_now("active WCS not reported by FluidNC"); return; }
                _query_got = false; _read_phase = 2;
            } else if (now - _step_started > 4000u) fail_now("no reply to $G");
        } else if (_read_phase == 2) {
            if (!_host->query("$#")) { fail_now("could not send the read-only $# query"); return; }
            _query_wait = true; _query_got = false; _step_started = now; _read_phase = 3;
        } else if (_read_phase == 3) {
            if (_query_got) {
                if (!_offset_known[_wcs]) { fail_now("offsets for the active WCS were not reported"); return; }
                if (!build_plan()) { _state = JcWfState::Failed; return; }
                _res.wcs = _wcs;
                memcpy(_res.g5x, _offsets[_wcs], sizeof(_res.g5x));
                memcpy(_res.g92, _offsets[8], sizeof(_res.g92));
                _state = JcWfState::Planned;
            } else if (now - _step_started > 4000u) fail_now("no reply to $#");
        }
    }

    bool begin_action(uint32_t now, JcWfOp) {
        _state = JcWfState::Running;
        _idx = 0;
        _phase = Phase::Begin;
        _step_started = now;
        _action = true;
        return true;
    }

    void advance() {
        ++_idx;
        _phase = Phase::Begin;
        _step_started = _now;
        if (_idx >= _n) finish();
    }

    void finish() {
        if (_action) {
            _action = false;
            _state = JcWfState::Done;
            return;
        }
        _res.valid = true;
        _state = JcWfState::Done;
    }

    bool run_check(JcWfCheck c, char* why, size_t size) {
        switch (c) {
            case JcWfCheck::SpindleOff:
                if (_status.spindle > 0.0f) { snprintf(why, size, "spindle is running: stop it first (never started here)"); return false; }
                return true;
            case JcWfCheck::ProbeReady:
                if (_status.probe_configured != JcTri::Yes) { snprintf(why, size, "probe input not verified: read machine settings (probe pin)"); return false; }
                if (_status.pins != nullptr && strchr(_status.pins, 'P') != nullptr) { snprintf(why, size, "probe is ALREADY TRIGGERED (Pn:P): check the probe and wiring"); return false; }
                return true;
            case JcWfCheck::ClearanceNotBelow:
                if (!_status.wpos_valid) { snprintf(why, size, "work Z position unknown"); return false; }
                if (_p.clearance_z < _status.wpos[2] - 0.001f) {
                    snprintf(why, size, "clearance Z%.3f is BELOW the current Z%.3f: it would move down", static_cast<double>(_p.clearance_z), static_cast<double>(_status.wpos[2]));
                    return false;
                }
                return true;
            case JcWfCheck::StatusIdle:
                if (_status.state == nullptr || strcmp(_status.state, "Idle") != 0) { snprintf(why, size, "machine is not Idle"); return false; }
                return true;
            case JcWfCheck::WcsUnchanged:
                return true;   // evaluated after the re-read in begin_step
        }
        return false;
    }

    void begin_step(const JcWfStep& s, uint32_t now) {
        char why[96];
        switch (s.op) {
            case JcWfOp::Check:
                if (static_cast<JcWfCheck>(s.arg) == JcWfCheck::WcsUnchanged) {
                    // Re-read $G: an active WCS that changed since the preview ends the routine before any write.
                    if (!_host || !_host->query("$G")) { fail_now("could not re-read the active WCS"); return; }
                    _query_wait = true; _query_got = false; _step_started = now; _phase = Phase::AwaitQuery; _recheck_wcs = true;
                    return;
                }
                if (!run_check(static_cast<JcWfCheck>(s.arg), why, sizeof(why))) { fail_now(why); return; }
                advance();
                return;
            case JcWfOp::QueryWcs:
            case JcWfOp::QueryOffsets: {
                const char* q = s.op == JcWfOp::QueryWcs ? "$G" : "$#";
                if (!_host || !_host->query(q)) { fail_now("could not send the read-only query"); return; }
                _query_wait = true; _query_got = false; _step_started = now; _phase = Phase::AwaitQuery;
                return;
            }
            case JcWfOp::WaitOperator:
                _state = JcWfState::WaitOperator;
                return;
            case JcWfOp::Compute:
                do_compute(s);
                return;
            case JcWfOp::VerifyOrigin:
                do_verify(s);
                return;
            case JcWfOp::VerifyTlo:
                do_verify_tlo();
                return;
            case JcWfOp::AwaitTouch:
                _step_started = now; _phase = Phase::AwaitTouch;
                return;
            case JcWfOp::AwaitRelease:
                _step_started = now; _phase = Phase::AwaitRelease;
                return;
            case JcWfOp::Send:
            case JcWfOp::Probe: {
                if (s.dyn != 0) {
                    JcWfStep& m = _plan[_idx];
                    if (m.dyn == 1) {
                        if (!_res.x.ok) { fail_now("no measured X centre to move to"); return; }
                        const float wx = work_of(0, _res.x.centre);
                        m.cmd = jc_goto_work_xy(true, wx, false, 0.0f);
                        m.target[0] = wx;
                    } else {
                        if (!_res.y.ok) { fail_now("no measured Y centre to move to"); return; }
                        const float wy = work_of(1, _res.y.centre);
                        m.cmd = jc_goto_work_xy(false, 0.0f, true, wy);
                        m.target[1] = wy;
                    }
                }
                if (!s.cmd.ok) { fail_now("command could not be prepared"); return; }
                if (_recheck_wcs) { _recheck_wcs = false; }
                if (s.motion && s.relative == false && _status.wpos_valid) { /* absolute target given */ }
                _start_wpos_valid = _status.wpos_valid;
                memcpy(_start_wpos, _status.wpos, sizeof(_start_wpos));
                const char* refuse = "";
                if (!_host || !_host->submit(s.cmd, &refuse)) {
                    char m[96];
                    snprintf(m, sizeof(m), "command not sent: %s", refuse);
                    fail_now(m);
                    return;
                }
                ++_sent;
                _step_started = now;
                _phase = Phase::AwaitResult;
                return;
            }
        }
    }

    void check_motion(const JcWfStep& s, uint32_t now) {
        const uint32_t limit = 120000u;
        if (now - _step_started > limit) { if (_host) _host->stop(); fail_now("motion did not finish in time; stopped"); return; }
        if (_frames <= _ack_frame) return;   // a status frame newer than the acknowledgement
        if (_status.state == nullptr || strcmp(_status.state, "Idle") != 0) return;
        if (s.op == JcWfOp::Probe || !s.motion) { advance(); return; }
        if (!_status.wpos_valid) { fail_now("position unknown after the move"); return; }
        for (uint8_t a = 0; a < 3; ++a) {
            if (((s.mask >> a) & 1u) == 0) continue;
            const float want = s.relative ? (_start_wpos[a] + s.target[a]) : s.target[a];
            if (fabsf(_status.wpos[a] - want) > 0.05f) {
                char m[96];
                snprintf(m, sizeof(m), "move ended at %c%.3f, expected %.3f", "XYZ"[a], static_cast<double>(_status.wpos[a]), static_cast<double>(want));
                fail_now(m);
                return;
            }
        }
        advance();
    }

    void do_compute(const JcWfStep& s) {
        const JcWfCalc calc = static_cast<JcWfCalc>(s.arg);
        if (calc == JcWfCalc::AxisCentre) {
            const bool ok_hi = _slot_ok[s.slot_hi], ok_lo = _slot_ok[s.slot_lo];
            const JcCentre c = jc_axis_centre(_p.feature, ok_hi, ok_hi ? contact(s.slot_hi, s.axis) : 0.0f, ok_lo,
                                              ok_lo ? contact(s.slot_lo, s.axis) : 0.0f, _p.tip_radius, _p.expected_size, _p.tolerance);
            if (!c.ok) { fail_now(c.why); return; }
            if (s.axis == 0) _res.x = c; else _res.y = c;
            if (c.suspect) snprintf(_res.summary[2], sizeof(_res.summary[2]), "%s", c.why);
        } else if (calc == JcWfCalc::ToolRef) {
            if (!_slot_ok[0]) { fail_now("no tool contact recorded"); return; }
            _res.tool_contact_ok = true;
            _res.tool_contact_mz = contact(0, 2);
            snprintf(_res.summary[0], sizeof(_res.summary[0]), "reference contact at machine Z%.3f", static_cast<double>(_res.tool_contact_mz));
        } else if (calc == JcWfCalc::ToolLength) {
            if (!_slot_ok[0]) { fail_now("no tool contact recorded"); return; }
            const float c = contact(0, 2);
            _res.tool_contact_ok = true;
            _res.tool_contact_mz = c;
            // A longer tool touches earlier: probing down it contacts HIGHER, probing up LOWER. Either way the
            // offset is the extra length relative to the reference: new_len - ref_len.
            const float tlo = _p.tool_dir < 0 ? (c - _p.ref_mz) : (_p.ref_mz - c);
            if (!isfinite(tlo) || fabsf(tlo) > 500.0f) { fail_now("implausible tool length difference (> 500 mm): check the reference and the plate"); return; }
            _res.tlo_ok = true;
            _res.tlo_proposed = tlo;
            snprintf(_res.summary[0], sizeof(_res.summary[0]), "contact at machine Z%.3f, reference Z%.3f", static_cast<double>(c), static_cast<double>(_p.ref_mz));
            snprintf(_res.summary[1], sizeof(_res.summary[1]), "proposed tool length offset TLO = %+.3f mm", static_cast<double>(tlo));
        } else if (calc == JcWfCalc::Station) {
            const bool ok_hi = _slot_ok[s.slot_hi], ok_lo = _slot_ok[s.slot_lo], ok_top = _slot_ok[s.slot_out];
            const JcCentre c = jc_axis_centre(JcFeatureKind::Outside, ok_hi, ok_hi ? contact(s.slot_hi, 1) : 0.0f, ok_lo,
                                              ok_lo ? contact(s.slot_lo, 1) : 0.0f, _p.tip_radius, _p.cylinder_diameter, _p.tolerance);
            if (!c.ok || !ok_top) { fail_now(c.ok ? "top contact missing: no centreline" : c.why); return; }
            JcStation& st = _res.st[s.axis == 0 ? 0 : 1];
            st.valid = true; st.along = contact(s.slot_hi, 0); st.cross = c.centre; st.diameter = c.size;
            st.top_contact = contact(s.slot_out, 2);
        } else {
            _res.line = jc_rotary_centreline(_res.st[0], _res.st[1], _p.tip_radius, _p.cylinder_diameter, _p.tolerance);
            if (!_res.line.ok) { fail_now(_res.line.why); return; }
        }
        advance();
    }

    void do_verify_tlo() {
        _res.tlo_readback_done = true;
        _res.tlo_read = _tlo;
        _res.tlo_readback_ok = _tlo_known && fabsf(_tlo - _tlo_expect) <= 0.002f;
        if (!_res.tlo_readback_ok) { fail_now("TLO READBACK MISMATCH: FluidNC's [TLO:] differs from the request"); return; }
        advance();
    }

    void do_verify(const JcWfStep& s) {
        const uint8_t m = s.arg;
        _res.readback_done = true;
        _res.readback_ok = true;
        memcpy(_res.g5x, _offsets[_wcs], sizeof(_res.g5x));
        if ((m & 1u) && fabsf(_offsets[_wcs][0] - (_res.x.centre - _res.g92[0])) > 0.002f) _res.readback_ok = false;
        if ((m & 2u) && fabsf(_offsets[_wcs][1] - (_res.y.centre - _res.g92[1])) > 0.002f) _res.readback_ok = false;
        if (!_res.readback_ok) { fail_now("READBACK MISMATCH: FluidNC's offsets differ from the request"); return; }
        advance();
    }

    bool build_plan();
    void add_pair(uint8_t axis, const char* tag, uint8_t slot_hi, uint8_t slot_lo, bool outside);
    void add_touch(uint8_t axis, int dir, uint8_t slot, const char* label);

    JcWfHost*   _host;
    JcWfKind    _kind = JcWfKind::GoToXY;
    JcWfParams  _p;
    JcWfState   _state = JcWfState::Idle;
    Phase       _phase = Phase::Begin;
    JcWfStep    _plan[kJcWfMaxSteps];
    uint8_t     _n = 0, _idx = 0;
    char        _reason[96] = "";
    JcWfResult  _res;
    JcWfStatus  _status;
    JcCommandResult _last_result;
    JcProbeReport _slot[kJcWfSlots];
    bool        _slot_ok[kJcWfSlots] = {};
    JcProbeReport _prb;
    uint32_t    _prb_seq = 0, _prb_seen = 0, _prb_counter = 0;
    float       _offsets[9][4] = {};
    bool        _offset_known[9] = {};
    int         _wcs = -1, _wcs_seen = -1;
    uint32_t    _gc_frames = 0;
    bool        _query_wait = false, _query_got = false, _gc_wait = false, _recheck_wcs = false, _action = false;
    uint8_t     _read_phase = 0;
    uint32_t    _step_started = 0, _now = 0, _frames = 0, _ack_frame = 0, _last_seq = 0, _sent = 0;
    float       _probe_z = 0.0f, _y_start = 0.0f;
    float       _tlo = 0.0f, _tlo_expect = 0.0f;
    bool        _tlo_known = false;
    uint8_t     _tlo_action = 0;
    char        _title[40] = "";
    float       _start_wpos[4] = { 0, 0, 0, 0 };
    bool        _start_wpos_valid = false;
};

}  // namespace jc8048w550

#include "JcWorkflowPlans.hpp"
