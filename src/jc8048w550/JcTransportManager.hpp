// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Which control transport is active (Instruction 010): Wi-Fi TCP, wired UART or BLE UART - exactly one at a time.
// Switching stops every other transport, tells the application to invalidate machine status and any pending
// operation (so nothing is replayed on the new link), and is refused while the machine reports motion.
// The manager decides; it never connects anything by itself and it holds no command policy (that is
// jc_command_bytes_allowed(), shared by all transports).
//
// Pure: no I/O.

#pragma once

#include <stdint.h>

namespace jc8048w550 {

enum class JcTransportKind : uint8_t { Wifi = 0, Uart = 1, Ble = 2 };

inline const char* jc_transport_name(JcTransportKind k) {
    switch (k) {
        case JcTransportKind::Wifi: return "Wi-Fi TCP";
        case JcTransportKind::Uart: return "Wired UART";
        case JcTransportKind::Ble: return "BLE UART";
    }
    return "?";
}

class JcTransportManager {
public:
    // `stop`: end that transport's session (disconnect / close). `invalidate`: drop machine status, pending
    // operations and job/probe state tied to the previous link.
    using StopFn = void (*)(void* user, JcTransportKind which);
    using InvalidateFn = void (*)(void* user);

    void set_callbacks(StopFn stop, InvalidateFn invalidate, void* user) { _stop = stop; _invalidate = invalidate; _user = user; }

    void set_initial(JcTransportKind k) { _active = k; }
    JcTransportKind active() const { return _active; }

    // nullptr = switched (or already active); otherwise why it was refused.
    const char* select(JcTransportKind to, bool machine_in_motion) {
        if (to == _active) return nullptr;
        if (machine_in_motion) return "the machine reports motion; transports are never switched while it moves";
        for (uint8_t i = 0; i < 3; ++i) {
            const JcTransportKind k = static_cast<JcTransportKind>(i);
            if (k != to && _stop != nullptr) _stop(_user, k);          // one active control transport only
        }
        if (_invalidate != nullptr) _invalidate(_user);                // stale status and pending work never carry over
        _active = to;
        ++_generation;
        return nullptr;
    }

    // Transports ask before they transmit anything beyond their own keep-alive.
    bool may_send(JcTransportKind k) const { return k == _active; }
    // Incremented on every switch: a result or reply captured under an older generation is discarded.
    uint32_t generation() const { return _generation; }

private:
    JcTransportKind _active = JcTransportKind::Wifi;
    uint32_t        _generation = 0;
    StopFn          _stop = nullptr;
    InvalidateFn    _invalidate = nullptr;
    void*           _user = nullptr;
};

}  // namespace jc8048w550
