// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Splits the byte stream coming back from FluidNC into status frames ('<...>',
// handled by JcFrameAssembler) and text lines (responses such as "ok",
// "error:3", "$/axes/x/steps_per_mm=80.000"). Fragmentation-safe. Control and
// non-ASCII bytes outside a frame (for example Telnet negotiation) are dropped
// and counted. Pure logic shared by both transports and the host tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "JcFrameAssembler.hpp"

namespace jc8048w550 {

class JcStreamSplitter {
public:
    enum class Event : uint8_t {
        None,
        Frame,          // frame() / frame_length() hold a complete '<...>' frame
        InvalidFrame,   // frame discarded; see reason()
        Line,           // line() holds one complete text line (no CR/LF)
        LineOverflow,   // an over-long line was discarded
    };

    static constexpr size_t kLineCapacity = 160;

    Event push(uint8_t byte) {
        if (_frames.in_frame()) {
            return map(_frames.push(byte));
        }
        if (_line_length == 0 && !_overflow && byte == '<') {
            _frames.push(byte);  // a frame can only start at the beginning of a line
            return Event::None;
        }
        if (byte == '\n') {
            if (_line_length == 0 && !_overflow) {
                return Event::None;
            }
            _line[_line_length] = '\0';
            const bool overflowed = _overflow;
            _line_length = 0;
            _overflow = false;
            return overflowed ? Event::LineOverflow : Event::Line;
        }
        if (byte == '\r') {
            return Event::None;
        }
        if (byte < 0x20 || byte > 0x7E) {
            ++_dropped_bytes;
            return Event::None;
        }
        if (_overflow) {
            return Event::None;
        }
        if (_line_length >= kLineCapacity - 1) {
            _overflow = true;
            _line_length = 0;
            return Event::None;
        }
        _line[_line_length++] = static_cast<char>(byte);
        return Event::None;
    }

    void reset() {
        _frames.reset();
        _line_length = 0;
        _overflow = false;
    }

    const char* frame() const { return _frames.frame(); }
    uint16_t frame_length() const { return _frames.length(); }
    const char* reason() const { return _frames.reason(); }
    // Valid until the next push().
    const char* line() const { return _line; }
    uint32_t dropped_bytes() const { return _dropped_bytes; }

private:
    static Event map(JcFrameAssembler::Result r) {
        switch (r) {
            case JcFrameAssembler::Result::Frame: return Event::Frame;
            case JcFrameAssembler::Result::Invalid: return Event::InvalidFrame;
            case JcFrameAssembler::Result::None: break;
        }
        return Event::None;
    }

    JcFrameAssembler _frames;
    char _line[kLineCapacity] {};
    size_t _line_length = 0;
    bool _overflow = false;
    uint32_t _dropped_bytes = 0;
};

}  // namespace jc8048w550
