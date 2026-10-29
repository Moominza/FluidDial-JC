// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Read-only wired UART status link. Pins and baud rate come from user settings;
// nothing assumes the JC board is wired to the controller. Like the Wi-Fi link
// it transmits only the '?' status query (jc_tx_byte_allowed) and parses
// replies with the shared JcFrameAssembler / jc_parse_status_frame. It starts
// only when the operator presses START UART LINK on the Connections page.
//
// Bluetooth LE has no compatible FluidNC service; JcBleTransportInfo reports
// that honestly instead of pretending to connect.

#pragma once

#include <stdint.h>

#include "JcFrameAssembler.hpp"
#include "JcLinkTracker.hpp"
#include "JcLinkTypes.hpp"
#include "JcStatusFrame.hpp"

namespace jc8048w550 {

struct JcBleTransportInfo {
    static constexpr bool available() { return false; }
    static constexpr const char* reason() {
        return "No compatible FluidNC BLE GATT service exists; JC-side BLE is not implemented.";
    }
};

class JcUartLink {
public:
    // Returns false (and stays stopped) when pins are not configured.
    bool begin(int8_t rx_pin, int8_t tx_pin, uint32_t baud);
    void end();
    void poll();

    bool running() const { return _running; }
    JcFluidState state() const;
    bool status_is_fresh() const;
    const JcLiveMachineStatus& status() const { return _status; }
    uint32_t parse_errors() const { return _tracker.invalid_count(); }
    uint32_t blocked_transmissions() const { return _blocked; }
    uint32_t last_status_age_ms() const;
    uint32_t frames() const { return _tracker.frames(); }

private:
    void send_status_query();
    void accept_frame(const char* frame, uint16_t length);

    JcLinkTracker _tracker;
    JcFrameAssembler _assembler;
    JcLiveMachineStatus _status {};
    bool _running = false;
    uint32_t _next_query_ms = 0;
    uint32_t _blocked = 0;
};

}  // namespace jc8048w550
