// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcUartLink.hpp"

#include <Arduino.h>
#include <string.h>

namespace jc8048w550 {

namespace {
constexpr uint32_t kQueryIntervalMs = 1000;
constexpr size_t kReadBudget = 256;
}  // namespace

bool JcUartLink::begin(int8_t rx_pin, int8_t tx_pin, uint32_t baud) {
    if (rx_pin < 0 || tx_pin < 0 || rx_pin == tx_pin) {
        Serial.println("uart: start refused; RX/TX pins are not configured");
        return false;
    }
    end();
    Serial1.begin(baud, SERIAL_8N1, rx_pin, tx_pin);
    _assembler.reset();
    _tracker = JcLinkTracker();
    _status = JcLiveMachineStatus {};
    _tracker.request_connect(millis());
    _tracker.on_connected();
    _next_query_ms = millis() + 200;
    _running = true;
    Serial.printf("uart: read-only status link started (RX=%d TX=%d baud=%lu); only '?' is sent\n",
                  rx_pin, tx_pin, static_cast<unsigned long>(baud));
    return true;
}

void JcUartLink::end() {
    if (_running) {
        Serial1.end();
        Serial.println("uart: status link stopped");
    }
    _running = false;
    _tracker.request_disconnect();
}

void JcUartLink::send_status_query() {
    const uint8_t byte = '?';
    if (!jc_tx_byte_allowed(byte)) {
        ++_blocked;
        return;
    }
    Serial1.write(&byte, 1);
}

bool JcUartLink::send_query_line(const char* line) {
    if (!jc_query_allowed(line)) {
        ++_rejected_queries;
        ++_blocked;
        Serial.println("SAFETY: blocked a line that is not an allowlisted read-only query");
        return false;
    }
    if (!_running) {
        return false;
    }
    Serial1.print(line);
    Serial1.print('\n');
    ++_queries_sent;
    return true;
}

void JcUartLink::accept_frame(const char* frame, uint16_t length) {
    JcStatusFrame parsed {};
    const uint32_t now = millis();
    if (!jc_parse_status_frame(frame, length, parsed)) {
        _tracker.on_invalid();
        return;
    }
    if (_status.has_data) {
        _status.update_interval_ms = now - _status.last_valid_ms;
    }
    strlcpy(_status.machine_state, parsed.state, sizeof(_status.machine_state));
    memcpy(_status.axes, parsed.pos, sizeof(_status.axes));
    _status.coordinate_reference = parsed.machine_position ? JcCoordinateReference::Machine
                                                           : JcCoordinateReference::Work;
    _status.feed = parsed.feed;
    _status.spindle = parsed.spindle;
    _status.has_wco = parsed.has_wco;
    memcpy(_status.wco, parsed.wco, sizeof(_status.wco));
    strlcpy(_status.pins, parsed.pins, sizeof(_status.pins));
    _status.has_sd = parsed.has_sd;
    _status.sd_percent = parsed.sd_percent;
    strlcpy(_status.sd_name, parsed.sd_name, sizeof(_status.sd_name));
    _status.last_valid_ms = now;
    _status.has_data = true;
    ++_status.sequence;
    _tracker.on_frame(now);
}

void JcUartLink::poll() {
    if (!_running) {
        return;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - _next_query_ms) >= 0) {
        _next_query_ms = now + kQueryIntervalMs;
        send_status_query();
    }
    size_t budget = kReadBudget;
    while (budget-- > 0 && Serial1.available() > 0) {
        const int byte = Serial1.read();
        if (byte < 0) {
            break;
        }
        switch (_assembler.push(static_cast<uint8_t>(byte))) {
            case JcStreamSplitter::Event::None: break;
            case JcStreamSplitter::Event::InvalidFrame: _tracker.on_invalid(); break;
            case JcStreamSplitter::Event::Frame: accept_frame(_assembler.frame(), _assembler.frame_length()); break;
            case JcStreamSplitter::Event::Line:
                if (_line_listener != nullptr) _line_listener(_line_user, _assembler.line());
                break;
            case JcStreamSplitter::Event::LineOverflow: _tracker.on_invalid(); break;
        }
    }
}

JcFluidState JcUartLink::state() const {
    return _tracker.state(millis());
}

bool JcUartLink::status_is_fresh() const {
    return _running && _tracker.fresh(millis());
}

uint32_t JcUartLink::last_status_age_ms() const {
    return _status.has_data ? millis() - _status.last_valid_ms : UINT32_MAX;
}

}  // namespace jc8048w550
