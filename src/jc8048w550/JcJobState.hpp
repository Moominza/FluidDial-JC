// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// SD job state as FluidNC reports it (Instruction 010). The percentage is ONLY the `SD:<pct>,<name>` field of the
// controller's status report; there is no timer, estimate or interpolation. The tracker never produces a command:
// a lost connection leaves the job UNKNOWN and nothing is restarted or resumed automatically.
//
//   Running   status has SD: and the machine is running
//   Paused    status has SD: and the machine is in Hold / Door (feed hold; resumable with CYCLE START)
//   Complete  the SD: field disappeared after the report had reached the end of the file (>= 99%), machine Idle
//   Ended     the SD: field disappeared before the end, machine Idle (stopped or reset)
//   Alarm     the controller went into ALARM while a job was active
//   Unknown   fresh status was lost while a job was active: its state is not known
//
// Pure: no I/O.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace jc8048w550 {

enum class JcJobPhase : uint8_t { None, Running, Paused, Complete, Ended, Alarm, Unknown };

inline const char* jc_job_phase_name(JcJobPhase p) {
    switch (p) {
        case JcJobPhase::None: return "NO JOB";
        case JcJobPhase::Running: return "RUNNING";
        case JcJobPhase::Paused: return "PAUSED (FEED HOLD)";
        case JcJobPhase::Complete: return "COMPLETE";
        case JcJobPhase::Ended: return "ENDED BEFORE THE END OF THE FILE";
        case JcJobPhase::Alarm: return "ALARM";
        case JcJobPhase::Unknown: return "UNKNOWN (no fresh status)";
    }
    return "?";
}

// The three controls, said precisely (shown on the Files page).
constexpr const char* kJcJobPauseText = "FEED HOLD ('!'): decelerates and stops; the job stays loaded and can be resumed.";
constexpr const char* kJcJobResumeText = "CYCLE START ('~'): continues a job that is in feed hold.";
constexpr const char* kJcJobStopText = "STOP / RESET (Ctrl-X): ends the job, it cannot be resumed; the position may be lost.";

class JcJobTracker {
public:
    // One call per status report (or per loss of freshness with fresh == false).
    void on_status(bool fresh, const char* state, bool has_sd, float sd_percent, const char* sd_name) {
        if (!fresh || state == nullptr) {
            if (_phase == JcJobPhase::Running || _phase == JcJobPhase::Paused) {
                _phase = JcJobPhase::Unknown;       // not restarted, not assumed finished
                ++_unknown_events;
            }
            return;
        }
        const bool hold = strncmp(state, "Hold", 4) == 0 || strncmp(state, "Door", 4) == 0;
        const bool alarm = strcmp(state, "Alarm") == 0;
        if (has_sd) {
            _pct = sd_percent;
            _have_pct = true;
            if (sd_name != nullptr) snprintf(_name, sizeof(_name), "%s", sd_name);
            if (alarm) _phase = JcJobPhase::Alarm;
            else _phase = hold ? JcJobPhase::Paused : JcJobPhase::Running;
            _seen_active = true;
            return;
        }
        if (_phase == JcJobPhase::Running || _phase == JcJobPhase::Paused || _phase == JcJobPhase::Unknown) {
            if (alarm) _phase = JcJobPhase::Alarm;
            else if (strcmp(state, "Idle") == 0) _phase = (_have_pct && _pct >= 99.0f) ? JcJobPhase::Complete : JcJobPhase::Ended;
            else if (_phase == JcJobPhase::Unknown) _phase = JcJobPhase::Ended;    // job no longer reported
        } else if (_phase == JcJobPhase::Alarm && !alarm) {
            // alarm cleared by the operator: the job is over
            _phase = JcJobPhase::Ended;
        }
    }

    void clear() { _phase = JcJobPhase::None; _have_pct = false; _pct = 0.0f; _name[0] = '\0'; _seen_active = false; }

    JcJobPhase phase() const { return _phase; }
    bool active() const { return _phase == JcJobPhase::Running || _phase == JcJobPhase::Paused; }
    bool percent_valid() const { return _have_pct; }
    float percent() const { return _pct; }
    const char* name() const { return _name; }
    bool can_resume() const { return _phase == JcJobPhase::Paused; }
    uint32_t unknown_events() const { return _unknown_events; }

private:
    JcJobPhase _phase = JcJobPhase::None;
    bool       _have_pct = false, _seen_active = false;
    float      _pct = 0.0f;
    char       _name[48] = "";
    uint32_t   _unknown_events = 0;
};

// Why a job may not be started now (nullptr = nothing prevents it). The controller still decides; these are the
// conditions the pendant itself can see.
inline const char* jc_job_start_blocker(bool live, bool fresh, const char* state, JcJobPhase phase, bool file_selected,
                                        bool file_runnable) {
    if (!live) return "not in LIVE mode";
    if (!fresh) return "no fresh machine status";
    if (!file_selected) return "no file selected";
    if (!file_runnable) return "this file name has characters the pendant will not send (use letters, digits, . _ - /)";
    if (state == nullptr || strcmp(state, "Idle") != 0) return "machine is not Idle";
    if (phase == JcJobPhase::Running || phase == JcJobPhase::Paused) return "a job is already active";
    if (phase == JcJobPhase::Unknown) return "job state unknown after a lost connection";
    return nullptr;
}

}  // namespace jc8048w550
