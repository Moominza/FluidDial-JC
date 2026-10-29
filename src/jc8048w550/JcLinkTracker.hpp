// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Transport-independent link state tracking: connect / frame / loss / stale /
// retry rules. Pure logic, shared by the UART link and the host integration
// harness. (The Wi-Fi link keeps its equivalent rules inside JcReadOnlyFluidNc
// and uses the same jc_status_fresh() freshness rule.)

#pragma once

#include <stdint.h>

#include "JcLinkTypes.hpp"

namespace jc8048w550 {

class JcLinkTracker {
public:
    static constexpr uint32_t kStaleMs = 3000;
    static constexpr uint32_t kRetryDelayMs = 2000;

    void request_connect(uint32_t now) {
        _requested = true;
        _next_retry_ms = now;
    }

    void request_disconnect() {
        _requested = false;
        _up = false;
        _has_data = false;
    }

    void on_connected() {
        _up = true;
        _lost = false;
        _invalid = false;
    }

    // Unexpected closure / timeout. Cached values stay but are never fresh.
    void on_closed(uint32_t now) {
        if (_up) {
            ++_lost_count;
        }
        _up = false;
        _lost = true;
        _next_retry_ms = now + kRetryDelayMs;
    }

    void on_frame(uint32_t now) {
        if (_has_data) {
            _interval_ms = now - _last_valid_ms;
        }
        _last_valid_ms = now;
        _has_data = true;
        _invalid = false;
        ++_frames;
    }

    void on_invalid() {
        _invalid = true;
        ++_invalid_count;
    }

    // True when a (re)connect attempt should start now. Never auto-starts
    // unless the operator requested a connection.
    bool should_retry(uint32_t now) {
        if (!_requested || _up || static_cast<int32_t>(now - _next_retry_ms) < 0) {
            return false;
        }
        _next_retry_ms = now + kRetryDelayMs;
        ++_retries;
        return true;
    }

    bool fresh(uint32_t now) const { return jc_status_fresh(_up, _has_data, now, _last_valid_ms, kStaleMs); }

    JcFluidState state(uint32_t now) const {
        if (!_requested) return JcFluidState::Disconnected;
        if (!_up) return _lost ? JcFluidState::Lost : JcFluidState::FluidNcConnecting;
        if (_invalid) return JcFluidState::InvalidData;
        if (!_has_data) return JcFluidState::FluidNcConnecting;
        return fresh(now) ? JcFluidState::Connected : JcFluidState::Stale;
    }

    uint32_t frames() const { return _frames; }
    uint32_t invalid_count() const { return _invalid_count; }
    uint32_t lost_count() const { return _lost_count; }
    uint32_t retries() const { return _retries; }
    uint32_t interval_ms() const { return _interval_ms; }
    uint32_t last_valid_ms() const { return _last_valid_ms; }
    bool has_data() const { return _has_data; }
    bool up() const { return _up; }

private:
    bool     _requested = false;
    bool     _up = false;
    bool     _lost = false;
    bool     _has_data = false;
    bool     _invalid = false;
    uint32_t _last_valid_ms = 0;
    uint32_t _interval_ms = 0;
    uint32_t _next_retry_ms = 0;
    uint32_t _frames = 0;
    uint32_t _invalid_count = 0;
    uint32_t _lost_count = 0;
    uint32_t _retries = 0;
};

}  // namespace jc8048w550
