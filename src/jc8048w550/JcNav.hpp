// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Application navigation state machine. Pure logic; the Dashboard itself and the
// legacy Wi-Fi / calibration pages live outside this graph and are reached via
// JcNav::back() returning ExitApp or an explicit leave request.

#pragma once

#include <stdint.h>

namespace jc8048w550 {

enum class JcPage : uint8_t {
    Menu,
    Jog,
    Homing,
    Probing,
    Offsets,
    Files,
    Macros,
    Tools,
    Settings,
    SetConnections,
    SetDisplay,
    SetJogging,
    SetMachine,
    Diagnostics,
    Capabilities,  // controller capabilities (read-only queries)
    DryRun,        // diagnostic dry run: command, authorization, expected reply
    LimitsHoming,  // soft / hard limit and homing settings as the controller reports them
    JogPreview,    // non-motion jog test mode: simulated-controller scenario and counters
    Workflow,      // guided positioning and probing routines (macros)
    Input,  // modal text / numeric entry; returns to its caller
    Count,
};

inline const char* jc_page_title(JcPage p) {
    switch (p) {
        case JcPage::Menu: return "MENU";
        case JcPage::Jog: return "JOGGING";
        case JcPage::Homing: return "HOMING";
        case JcPage::Probing: return "PROBING";
        case JcPage::Offsets: return "WORK OFFSETS";
        case JcPage::Files: return "FILES AND JOBS";
        case JcPage::Macros: return "MACROS";
        case JcPage::Tools: return "TOOLS";
        case JcPage::Settings: return "SETTINGS";
        case JcPage::SetConnections: return "CONNECTIONS";
        case JcPage::SetDisplay: return "DISPLAY";
        case JcPage::SetJogging: return "JOG PRESETS";
        case JcPage::SetMachine: return "MACHINE";
        case JcPage::Diagnostics: return "DIAGNOSTICS";
        case JcPage::Capabilities: return "CONTROLLER CAPABILITIES";
        case JcPage::DryRun: return "DRY RUN";
        case JcPage::LimitsHoming: return "LIMITS AND HOMING";
        case JcPage::JogPreview: return "JOG PREVIEW TEST";
        case JcPage::Workflow: return "POSITIONING / PROBING";
        case JcPage::Input: return "ENTER VALUE";
        case JcPage::Count: break;
    }
    return "?";
}

// Allowed forward transitions (Input is reachable from any page).
inline bool jc_nav_allowed(JcPage from, JcPage to) {
    if (to == JcPage::Input) {
        return from != JcPage::Menu && from != JcPage::Input;
    }
    switch (from) {
        case JcPage::Menu:
            return to == JcPage::Jog || to == JcPage::Homing || to == JcPage::Probing
                || to == JcPage::Offsets || to == JcPage::Files || to == JcPage::Macros
                || to == JcPage::Tools || to == JcPage::Settings;
        case JcPage::Macros:
            return to == JcPage::Workflow;
        case JcPage::Settings:
            return to == JcPage::SetConnections || to == JcPage::SetDisplay || to == JcPage::SetJogging
                || to == JcPage::SetMachine || to == JcPage::Diagnostics || to == JcPage::JogPreview;
        case JcPage::SetMachine:
            return to == JcPage::Capabilities;
        case JcPage::Capabilities:
            return to == JcPage::DryRun || to == JcPage::LimitsHoming;
        case JcPage::Tools:
            return to == JcPage::Probing;  // tool measurement configuration
        case JcPage::Files:
        case JcPage::Workflow:
        case JcPage::Jog:
        case JcPage::Homing:
        case JcPage::Probing:
        case JcPage::Offsets:
        case JcPage::SetConnections:
        case JcPage::SetDisplay:
        case JcPage::SetJogging:
        case JcPage::Diagnostics:
        case JcPage::DryRun:
        case JcPage::LimitsHoming:
        case JcPage::JogPreview:
        case JcPage::Input:
        case JcPage::Count:
            return false;
    }
    return false;
}

class JcNav {
public:
    static constexpr uint8_t kMaxDepth = 6;
    enum class BackResult : uint8_t { Moved, ExitApp };

    JcNav() { reset(); }

    void reset() {
        _depth = 1;
        _stack[0] = JcPage::Menu;
    }

    JcPage current() const { return _stack[_depth - 1]; }
    uint8_t depth() const { return _depth; }

    bool go(JcPage to) {
        if (to == current() || _depth >= kMaxDepth || !jc_nav_allowed(current(), to)) {
            return false;
        }
        _stack[_depth++] = to;
        return true;
    }

    BackResult back() {
        if (_depth <= 1) {
            return BackResult::ExitApp;
        }
        --_depth;
        return BackResult::Moved;
    }

private:
    JcPage  _stack[kMaxDepth];
    uint8_t _depth = 1;
};

}  // namespace jc8048w550
