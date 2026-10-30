// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Firmware BLE central radio for JcBleUart (Instruction 010), on the ESP32-S3's Bluetooth LE (Arduino BLE library).
// This file is only the radio: scanning, connecting, GATT discovery, notifications and GATT writes. It holds no
// command policy - every byte it writes was queued by JcBleUart after jc_query_allowed() /
// jc_command_bytes_allowed(). The BLE stack is started only when the operator scans or connects, and is released
// when the BLE transport is stopped.
//
// NOT verified on hardware: the external CC2541 / HM-10 module has not been tested with this firmware.

#pragma once

#include "JcBleUart.hpp"

namespace jc8048w550 {

class JcBleRadioEsp : public JcBleRadio {
public:
    void attach(JcBleUart* link) { _link = link; }

    bool scan_start(uint32_t duration_ms) override;
    void scan_stop() override;
    bool connect(const char* addr) override;
    void disconnect() override;
    bool discover() override;
    bool subscribe(const JcBleSelection& sel) override;
    bool write(const JcBleSelection& sel, const uint8_t* data, size_t len) override;
    uint16_t mtu() const override;

    // Main loop: delivers scan results, connection events, discovery, notifications and disconnects to the link.
    void poll(uint32_t now);
    // Releases the BLE stack (heap) when the transport is stopped.
    void shutdown();
    bool stack_started() const { return _started; }

private:
    bool ensure_stack();
    JcBleUart* _link = nullptr;
    bool _started = false;
};

}  // namespace jc8048w550
