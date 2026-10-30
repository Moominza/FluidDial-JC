// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// One place that turns a parsed status frame into the shared JcLiveMachineStatus record, used by every
// transport that carries the FluidNC byte stream (wired UART, BLE UART). Pure.

#pragma once

#include <stdint.h>
#include <string.h>

#include "JcLinkTypes.hpp"
#include "JcStatusFrame.hpp"

namespace jc8048w550 {

namespace detail {
inline void copy_text(char* dst, size_t cap, const char* src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}
}  // namespace detail

inline void jc_apply_status_frame(JcLiveMachineStatus& status, const JcStatusFrame& parsed, uint32_t now) {
    if (status.has_data) {
        status.update_interval_ms = now - status.last_valid_ms;
    }
    detail::copy_text(status.machine_state, sizeof(status.machine_state), parsed.state);
    memcpy(status.axes, parsed.pos, sizeof(status.axes));
    status.coordinate_reference = parsed.machine_position ? JcCoordinateReference::Machine : JcCoordinateReference::Work;
    status.feed = parsed.feed;
    status.spindle = parsed.spindle;
    status.has_wco = parsed.has_wco;
    memcpy(status.wco, parsed.wco, sizeof(status.wco));
    detail::copy_text(status.pins, sizeof(status.pins), parsed.pins);
    status.has_sd = parsed.has_sd;
    status.sd_percent = parsed.sd_percent;
    detail::copy_text(status.sd_name, sizeof(status.sd_name), parsed.sd_name);
    status.last_valid_ms = now;
    status.has_data = true;
    ++status.sequence;
}

}  // namespace jc8048w550
