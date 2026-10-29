// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Reassembles '<...>' status frames from an arbitrary byte stream (fragmented
// TCP segments, UART bytes). Pure logic, shared by every transport and by the
// host tests. Completed frames are validated by jc_parse_status_frame().

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jc8048w550 {

class JcFrameAssembler {
public:
    enum class Result : uint8_t {
        None,     // byte consumed, frame incomplete (or ignored outside a frame)
        Frame,    // frame() / length() hold a complete '<...>' frame
        Invalid,  // frame discarded; see reason()
    };

    static constexpr size_t kCapacity = 256;

    Result push(uint8_t byte) {
        if (!_active) {
            if (byte == '<') {
                start();
            }
            return Result::None;
        }
        if (byte == '<') {
            start();
            _reason = "Nested status frame start";
            return Result::Invalid;
        }
        if (byte < 0x20 || byte > 0x7E || _length >= kCapacity - 1) {
            _active = false;
            _length = 0;
            _reason = "Status frame exceeded bounds or contained control data";
            return Result::Invalid;
        }
        _buffer[_length++] = static_cast<char>(byte);
        if (byte == '>') {
            _active = false;
            return Result::Frame;
        }
        return Result::None;
    }

    void reset() {
        _active = false;
        _length = 0;
    }

    const char* frame() const { return _buffer; }
    uint16_t length() const { return _length; }
    const char* reason() const { return _reason; }
    bool in_frame() const { return _active; }

private:
    void start() {
        _active = true;
        _length = 0;
        _buffer[_length++] = '<';
    }

    char _buffer[kCapacity] {};
    uint16_t _length = 0;
    bool _active = false;
    const char* _reason = "";
};

}  // namespace jc8048w550
