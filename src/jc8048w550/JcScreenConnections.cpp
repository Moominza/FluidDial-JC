// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Manual transport selection and configuration. Selecting or switching a
// transport terminates any active session and always requires a deliberate
// reconnect; it is refused while the machine reports motion.

#include "JcScreens.hpp"

#include <stdlib.h>

namespace jc8048w550 {

namespace {

constexpr ui::Rect kWifiPanel { 16, 76, 768, 92 };
constexpr ui::Rect kUartPanel { 16, 176, 768, 156 };
constexpr ui::Rect kBlePanel { 16, 340, 768, 84 };
constexpr ui::Rect kWifiAuto { 388, 86, 124, 44 };   // persisted AUTO-CONNECT on / off
constexpr ui::Rect kWifiSelect { 520, 86, 118, 44 };
constexpr ui::Rect kWifiSetup { 648, 86, 126, 44 };
constexpr ui::Rect kUartRx { 28, 276, 112, 44 };
constexpr ui::Rect kUartTx { 148, 276, 112, 44 };
constexpr ui::Rect kUartBaud { 268, 276, 140, 44 };
constexpr ui::Rect kUartStart { 416, 276, 200, 44 };
constexpr ui::Rect kUartSelect { 626, 276, 148, 44 };
constexpr ui::Rect kBleSelect { 626, 350, 148, 44 };

void pin_callback(void* user, bool ok, const char* text) {
    static_cast<JcConnectionsScreen*>(user)->edit_done(ok, text);
}

}  // namespace

void JcConnectionsScreen::draw_status() {
    const JcUserSettings& s = sv.settings;
    char line[96];
    const JcReadOnlyFluidNc& link = sv.link;
    snprintf(line, sizeof(line), "%s  |  host: %s", jc_conn_label_text(link.conn_label()),
             link.config().host[0] ? link.config().host : "not set");
    _wifi.draw(sv.display, { 28, 124, 480, 34 }, line, FontId::Body16,
               link.status_is_fresh() ? ui::kGood : ui::kMuted, ui::kPanel);
    if (_uart.running()) {
        snprintf(line, sizeof(line), "RUNNING: %s, %lu frames, %lu errors", JcReadOnlyFluidNc::state_name(_uart.state()),
                 static_cast<unsigned long>(_uart.frames()), static_cast<unsigned long>(_uart.parse_errors()));
    } else {
        snprintf(line, sizeof(line), "STOPPED  (RX %d, TX %d, %lu baud)", s.uart_rx_pin, s.uart_tx_pin,
                 static_cast<unsigned long>(s.uart_baud));
    }
    _uart_state.draw(sv.display, { 28, 244, 740, 28 }, line, FontId::Body16,
                     _uart.status_is_fresh() ? ui::kGood : ui::kMuted, ui::kPanel);
}

void JcConnectionsScreen::on_draw() {
    const uint8_t t = sv.settings.transport;
    ui::panel(sv.display, kWifiPanel, t == 0 ? ui::kPanelLight : ui::kPanel);
    ui::text(sv.display, 28, 84, "Wi-Fi  (Telnet TCP, port 23)", FontId::Button20, WHITE, t == 0 ? ui::kPanelLight : ui::kPanel);
    ui::button(sv.display, kWifiAuto, sv.link.auto_pref() == JcAutoPref::Off ? "AUTO: OFF" : "AUTO: ON",
               sv.link.auto_pref() == JcAutoPref::Off ? ui::Btn::Normal : ui::Btn::Good);
    ui::button(sv.display, kWifiSelect, t == 0 ? "SELECTED" : "SELECT", t == 0 ? ui::Btn::Selected : ui::Btn::Normal);
    ui::button(sv.display, kWifiSetup, "WI-FI SETUP");

    ui::panel(sv.display, kUartPanel, t == 1 ? ui::kPanelLight : ui::kPanel);
    ui::text(sv.display, 28, 184, "Wired UART  (read-only status)", FontId::Button20, WHITE, t == 1 ? ui::kPanelLight : ui::kPanel);
    ui::text(sv.display, 28, 214, "Wiring to the controller is NOT verified or assumed. Choose pins you have wired.",
             FontId::Body16, ui::kWarn, t == 1 ? ui::kPanelLight : ui::kPanel);
    char v[24];
    snprintf(v, sizeof(v), "RX: %d", sv.settings.uart_rx_pin);
    ui::button(sv.display, kUartRx, v);
    snprintf(v, sizeof(v), "TX: %d", sv.settings.uart_tx_pin);
    ui::button(sv.display, kUartTx, v);
    snprintf(v, sizeof(v), "%lu baud", static_cast<unsigned long>(sv.settings.uart_baud));
    ui::button(sv.display, kUartBaud, v);
    ui::button(sv.display, kUartStart, _uart.running() ? "STOP UART LINK" : "START UART LINK",
               _uart.running() ? ui::Btn::Danger : ui::Btn::Normal);
    ui::button(sv.display, kUartSelect, t == 1 ? "SELECTED" : "SELECT", t == 1 ? ui::Btn::Selected : ui::Btn::Normal);

    ui::panel(sv.display, kBlePanel, t == 2 ? ui::kPanelLight : ui::kPanel);
    ui::text(sv.display, 28, 348, "Bluetooth LE  -  UNAVAILABLE", FontId::Button20, ui::kWarn, t == 2 ? ui::kPanelLight : ui::kPanel);
    ui::wrap(sv.display, { 28, 378, 580, 44 }, JcBleTransportInfo::reason(), FontId::Body16, ui::kMuted,
             t == 2 ? ui::kPanelLight : ui::kPanel);
    ui::button(sv.display, kBleSelect, "SELECT", ui::Btn::Disabled);

    ui::wrap(sv.display, { 16, 430, 768, 46 },
             "Switching transport ends the session and never reconnects by itself. Wi-Fi reconnects "
             "automatically only while AUTO is on. Refused while the machine reports motion.",
             FontId::Body16, ui::kMuted, ui::kBg);
    _wifi.invalidate();
    _uart_state.invalidate();
    draw_status();
}

void JcConnectionsScreen::select_transport(uint8_t transport) {
    if (sv.model.in_motion() && sv.model.fresh()) {
        show_message("SWITCH REFUSED", "The machine reports motion (Run / Jog / Home).",
                     "Transports are never switched while the machine is moving.");
        return;
    }
    if (sv.settings.transport == transport) {
        return;
    }
    // Terminate every active session; the operator must reconnect deliberately.
    sv.link.disconnect();
    _uart.end();
    sv.dispatcher.on_link_lost();
    sv.settings.transport = transport;
    sv.host.save_settings();
    Serial.printf("transport: selected %u; sessions closed; reconnect is manual\n", transport);
    draw();
}

void JcConnectionsScreen::edit_done(bool ok, const char* text) {
    if (!ok) {
        return;
    }
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < -1 || value > 48) {
        return;
    }
    (_edit_pin == 0 ? sv.settings.uart_rx_pin : sv.settings.uart_tx_pin) = static_cast<int8_t>(value);
    _uart.end();  // configuration changes stop a running link
    sv.host.save_settings();
    draw();
}

void JcConnectionsScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kWifiAuto, x, y)) {
        // The persisted choice: ON = connect now and after every restart or loss; OFF = disconnect and
        // stay disconnected until the operator chooses ON, CONNECT or LIVE again.
        if (sv.link.auto_pref() != JcAutoPref::Off) {
            sv.link.set_auto_pref(JcAutoPref::Off);
            sv.link.disconnect();
            sv.dispatcher.on_link_lost();
            Serial.println("net: AUTO-CONNECT turned off by the operator; link closed");
        } else if (sv.settings.transport == 0 && sv.link.config().valid) {
            sv.link.set_auto_pref(JcAutoPref::On);
            sv.link.connect();
            Serial.println("net: AUTO-CONNECT turned on by the operator; connecting");
        } else {
            show_message("CANNOT TURN AUTO-CONNECT ON",
                         sv.settings.transport != 0 ? "Wi-Fi is not the selected transport." : "No saved Wi-Fi / FluidNC configuration.",
                         "Transports are never switched automatically.");
            return;
        }
        draw();
    } else if (ui::hit(kWifiSelect, x, y)) {
        select_transport(0);
    } else if (ui::hit(kWifiSetup, x, y)) {
        sv.host.leave(JcLeaveTarget::WifiSettings);
    } else if (ui::hit(kUartSelect, x, y)) {
        select_transport(1);
    } else if (ui::hit(kBleSelect, x, y)) {
        show_message("BLUETOOTH LE UNAVAILABLE", JcBleTransportInfo::reason(),
                     "No BLE connection or discovery is attempted.");
    } else if (ui::hit(kUartRx, x, y) || ui::hit(kUartTx, x, y)) {
        _edit_pin = ui::hit(kUartRx, x, y) ? 0 : 1;
        JcInputRequest r {};
        r.prompt = _edit_pin == 0 ? "UART RX GPIO (-1 = unset)" : "UART TX GPIO (-1 = unset)";
        snprintf(r.initial, sizeof(r.initial), "%d", _edit_pin == 0 ? sv.settings.uart_rx_pin : sv.settings.uart_tx_pin);
        r.numeric = true;
        r.max_len = 3;
        r.done = pin_callback;
        r.user = this;
        sv.host.request_input(r);
    } else if (ui::hit(kUartBaud, x, y)) {
        static const uint32_t bauds[] = { 9600, 19200, 38400, 57600, 115200, 230400 };
        size_t i = 0;
        while (i < 6 && bauds[i] != sv.settings.uart_baud) ++i;
        sv.settings.uart_baud = bauds[(i + 1) % 6];
        _uart.end();
        sv.host.save_settings();
        draw();
    } else if (ui::hit(kUartStart, x, y)) {
        if (_uart.running()) {
            _uart.end();
            draw();
        } else if (sv.settings.transport != 1) {
            show_message("SELECT UART FIRST", "Choose Wired UART as the transport before starting the link.");
        } else if (sv.link.connect_requested()) {
            show_message("WI-FI LINK ACTIVE", "Disconnect the Wi-Fi link before starting UART.");
        } else if (!_uart.begin(sv.settings.uart_rx_pin, sv.settings.uart_tx_pin, sv.settings.uart_baud)) {
            show_message("UART NOT STARTED", "RX and TX pins must be set to different GPIO numbers.");
        } else {
            draw();
        }
    }
}

void JcConnectionsScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 500) {
        _last_tick = now;
        draw_status();
    }
}

}  // namespace jc8048w550
