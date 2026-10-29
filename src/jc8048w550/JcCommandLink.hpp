// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Glue between the authorization gate and a transport: the sink that the gate
// calls for an AUTHORIZED command. It starts a JcCommandExec request (reply
// correlation + timeout) and hands the bytes to a raw send function.
//
// THE JC FIRMWARE DOES NOT CREATE OR INSTALL THIS in this build: with
// JC_MACHINE_CONTROL_ENABLED=0 nothing is ever authorized and no sink exists
// (tools/check_no_sink.py fails the build checks if that changes). It is
// exercised by the host tests, which supply a fake transport.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "JcCommandAuth.hpp"
#include "JcCommandExec.hpp"
#include "JcResponse.hpp"

namespace jc8048w550 {

// '!' feed hold, '~' cycle start, 0x18 soft reset, 0x85 jog cancel.
inline bool jc_is_realtime_text(const char* text) {
    if (text == nullptr || text[0] == '\0' || text[1] != '\0') return false;
    const uint8_t b = static_cast<uint8_t>(text[0]);
    return b == '!' || b == '~' || b == 0x18 || b == 0x85;
}

class JcCommandLink {
public:
    using SendFn = bool (*)(void* user, const char* bytes, size_t length);

    JcCommandLink(SendFn send, void* user) : _send(send), _user(user) {}

    void set_clock(uint32_t now) { _now = now; }

    // Matches JcCommandSinkEx: returns true only if the bytes were handed to
    // the transport.
    static bool sink(void* self, JcCommandClass cls, const char* text) {
        return static_cast<JcCommandLink*>(self)->transmit(cls, text);
    }

    void on_line(const char* line) { _exec.on_response(jc_classify_line(line), _now); }
    void on_machine_state(const char* state) { _exec.on_machine_state(state, _now); }
    void tick() { _exec.tick(_now); }
    void on_link_lost() { _exec.on_link_lost(_now); }

    JcCommandExec& exec() { return _exec; }
    const JcCommandExec& exec() const { return _exec; }
    uint32_t transmitted() const { return _transmitted; }
    uint32_t refused_busy() const { return _refused_busy; }

private:
    bool transmit(JcCommandClass cls, const char* text) {
        if (text == nullptr) return false;
        if (jc_is_realtime_text(text)) {
            if (!_exec.begin_realtime("<real-time>", _now)) {
                ++_refused_busy;
                return false;
            }
            if (!_send(_user, text, 1)) {
                _exec.on_link_lost(_now);
                return false;
            }
            ++_transmitted;
            return true;
        }
        if (!_exec.begin(text, jc_command_timeout_ms(cls), _now, true)) {
            ++_refused_busy;  // a request is still awaiting its reply
            return false;
        }
        char line[kJcLineMax];
        const size_t n = strlen(text);
        if (n + 1 >= sizeof(line)) {
            _exec.on_link_lost(_now);
            return false;
        }
        memcpy(line, text, n);
        line[n] = '\n';
        if (!_send(_user, line, n + 1)) {
            _exec.on_link_lost(_now);
            return false;
        }
        ++_transmitted;
        return true;
    }

    static constexpr size_t kJcLineMax = 160;
    SendFn         _send;
    void*          _user;
    JcCommandExec  _exec;
    uint32_t       _now = 0;
    uint32_t       _transmitted = 0;
    uint32_t       _refused_busy = 0;
};

}  // namespace jc8048w550
