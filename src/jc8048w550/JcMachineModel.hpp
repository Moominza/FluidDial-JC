// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Machine state model fed by validated status frames. Pure logic.
//
// FluidNC reports either MPos or WPos in every frame and the work coordinate
// offset (WCO) only periodically. The model retains the last WCO it has seen
// so the other reference can be derived. If no WCO has ever been seen the
// other reference is UNAVAILABLE; it is never assumed to be zero.

#pragma once

#include <string.h>

#include "JcLinkTypes.hpp"

namespace jc8048w550 {

enum class JcRef : uint8_t { Machine, Work };

struct JcHistoryEntry {
    uint32_t ms;
    char     state[20];
};

class JcMachineModel {
public:
    static constexpr uint8_t kHistory = 8;

    void on_status(const JcLiveMachineStatus& s) {
        if (!s.has_data) {
            return;
        }
        if (_has_status && strcmp(_status.machine_state, s.machine_state) == 0) {
            // unchanged state: no history entry
        } else {
            JcHistoryEntry& e = _history[_history_next];
            e.ms = s.last_valid_ms;
            strncpy(e.state, s.machine_state, sizeof(e.state) - 1);
            e.state[sizeof(e.state) - 1] = '\0';
            _history_next = (_history_next + 1) % kHistory;
            if (_history_count < kHistory) {
                ++_history_count;
            }
        }
        if (s.has_wco) {
            memcpy(_wco, s.wco, sizeof(_wco));
            _wco_known = true;
        }
        _status = s;
        _has_status = true;
    }

    bool has_status() const { return _has_status; }
    const JcLiveMachineStatus& status() const { return _status; }
    const char* state() const { return _has_status ? _status.machine_state : ""; }

    // Position of `axis` in the requested reference. Returns false if it cannot
    // be determined (no frame yet, or the other reference needs an unseen WCO).
    bool position(uint8_t axis, JcRef ref, float& out) const {
        if (!_has_status || axis >= 4) {
            return false;
        }
        const JcCoordinateReference reported = _status.coordinate_reference;
        if ((ref == JcRef::Machine && reported == JcCoordinateReference::Machine)
            || (ref == JcRef::Work && reported == JcCoordinateReference::Work)) {
            out = _status.axes[axis];
            return true;
        }
        if (reported == JcCoordinateReference::Unknown || !_wco_known) {
            return false;
        }
        out = ref == JcRef::Machine ? _status.axes[axis] + _wco[axis]
                                    : _status.axes[axis] - _wco[axis];
        return true;
    }

    bool wco_known() const { return _wco_known; }
    bool wco(uint8_t axis, float& out) const {
        if (!_wco_known || axis >= 4) {
            return false;
        }
        out = _wco[axis];
        return true;
    }

    bool alarm() const { return _has_status && strcmp(_status.machine_state, "Alarm") == 0; }
    bool in_motion() const {
        return _has_status
            && (strcmp(_status.machine_state, "Run") == 0 || strcmp(_status.machine_state, "Jog") == 0
                || strcmp(_status.machine_state, "Home") == 0);
    }
    bool homing() const { return _has_status && strcmp(_status.machine_state, "Home") == 0; }
    bool probe_pin_reported() const { return _has_status; }
    bool probe_triggered() const { return _has_status && strchr(_status.pins, 'P') != nullptr; }
    float feed() const { return _has_status ? _status.feed : 0.0f; }
    float spindle() const { return _has_status ? _status.spindle : 0.0f; }
    bool job_running() const { return _has_status && _status.has_sd; }

    uint8_t history_count() const { return _history_count; }
    // index 0 = newest
    const JcHistoryEntry* history(uint8_t index) const {
        if (index >= _history_count) {
            return nullptr;
        }
        return &_history[(_history_next + kHistory - 1 - index) % kHistory];
    }

private:
    JcLiveMachineStatus _status {};
    bool                _has_status = false;
    float               _wco[4] {};
    bool                _wco_known = false;
    JcHistoryEntry      _history[kHistory] {};
    uint8_t             _history_next = 0;
    uint8_t             _history_count = 0;
};

}  // namespace jc8048w550
