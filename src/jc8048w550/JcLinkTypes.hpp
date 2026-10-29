// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Pure link-state types shared by every transport, the machine model, the UI
// and the host tests. No hardware or RTOS dependencies.

#pragma once

#include <stdint.h>

namespace jc8048w550 {

enum class JcFluidState : uint8_t {
    Disconnected,
    WifiConnecting,
    WifiConnected,
    Resolving,
    FluidNcConnecting,
    Connected,
    Lost,
    Stale,
    InvalidData,
    Error,
};

enum class JcCoordinateReference : uint8_t {
    Unknown,
    Machine,
    Work,
};

struct JcLiveMachineStatus {
    float axes[4];
    char machine_state[20];
    JcCoordinateReference coordinate_reference;
    uint32_t last_valid_ms;
    uint32_t update_interval_ms;
    uint32_t sequence;
    bool has_data;
    // Optional fields reported with the same frame (valid for this frame only).
    float feed;
    float spindle;
    bool has_wco;
    float wco[4];
    char pins[12];
    bool has_sd;
    float sd_percent;
    char sd_name[40];
};

// Freshness rule shared by all transports: data is current only while the
// link is up and the last valid frame is younger than the stale window.
inline bool jc_status_fresh(bool link_up, bool has_data, uint32_t now_ms,
                            uint32_t last_valid_ms, uint32_t stale_ms) {
    return link_up && has_data && (now_ms - last_valid_ms) <= stale_ms;
}

// Outgoing-byte rule for every read-only transport: the only byte that may be
// transmitted to the controller in this build is the '?' status query.
inline bool jc_tx_byte_allowed(uint8_t byte) {
    return byte == static_cast<uint8_t>('?');
}

}  // namespace jc8048w550
