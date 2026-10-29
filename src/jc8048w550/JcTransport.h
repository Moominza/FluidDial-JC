// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jc8048w550 {

enum class TransportKind : uint8_t {
    WifiWebSocket,
    WiredUart,
    BluetoothLe,
};

inline const char* transport_name(TransportKind kind) {
    switch (kind) {
        case TransportKind::WifiWebSocket:
            return "Wi-Fi / WebSocket";
        case TransportKind::WiredUart:
            return "Wired UART";
        case TransportKind::BluetoothLe:
            return "Bluetooth LE";
    }
    return "Unknown";
}

class ByteTransport {
public:
    virtual ~ByteTransport() = default;
    virtual TransportKind kind() const = 0;
    virtual bool begin() = 0;
    virtual void end() = 0;
    virtual void poll() = 0;
    virtual bool connected() const = 0;
    virtual size_t write(const uint8_t* data, size_t length) = 0;
    virtual size_t read(uint8_t* data, size_t capacity) = 0;
};

} // namespace jc8048w550
