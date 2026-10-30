// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Bluetooth LE UART page (Instruction 010): scan, choose a device, connect / disconnect, forget the saved device and
// read the transport diagnostics. BLE carries the same FluidNC byte stream as Wi-Fi and UART and has no command
// authority of its own. The external HM-10 / CC2541 module has NOT been verified on hardware.

#include "JcScreens.hpp"

namespace jc8048w550 {

namespace {

constexpr ui::Rect kScan { 16, 108, 188, 48 };
constexpr ui::Rect kDisconnect { 212, 108, 188, 48 };
constexpr ui::Rect kForget { 408, 108, 188, 48 };
constexpr ui::Rect kPgUp { 628, 164, 156, 58 };
constexpr ui::Rect kPgDn { 628, 228, 156, 58 };
constexpr uint8_t kRows = 3;
ui::Rect row_rect(uint8_t i) { return { 16, static_cast<int16_t>(164 + i * 42), 604, 38 }; }
constexpr ui::Rect kDiagPanel { 16, 296, 768, 176 };

}  // namespace

void JcBleScreen::on_enter() {
    _scroll = 0;
}

uint32_t JcBleScreen::list_key() const {
    uint32_t k = sv.ble.devices().count();
    k = k * 31u + static_cast<uint32_t>(sv.ble.state());
    k = k * 31u + _scroll;
    k = k * 31u + (sv.ble_saved.valid ? 1u : 0u);
    return k;
}

void JcBleScreen::draw_list() {
    sv.display.fillRect(16, 160, 768, 132, ui::kBg);
    const JcBleScanList& list = sv.ble.devices();
    const uint8_t n = list.count();
    if (n == 0) {
        const char* hint = sv.settings.transport != 2 ? "BLE is not the selected transport. Choose BLE on the Connections page first."
                         : (sv.ble.state() == JcBleState::Scanning ? "Scanning ..." : "No devices yet. Press SCAN; the saved device (if any) connects by itself.");
        ui::wrap(sv.display, { 24, 172, 592, 100 }, hint, FontId::Body16, ui::kMuted, ui::kBg);
    }
    for (uint8_t i = 0; i < kRows; ++i) {
        const uint8_t rank = static_cast<uint8_t>(_scroll + i);
        const JcBleDevice* d = list.sorted(rank);
        if (d == nullptr) break;
        const bool saved = sv.ble_saved.valid && JcBleScanList::same_addr(sv.ble_saved.addr, d->addr);
        const ui::Rect r = row_rect(i);
        ui::panel(sv.display, r, saved ? ui::kGoodDark : ui::kPanel);
        char l[100];
        snprintf(l, sizeof(l), "%s  %s", d->name[0] ? d->name : "(no name)", d->addr);
        char shown[64];
        JcSdBrowser::ellipsize(l, 46, shown, sizeof(shown));
        ui::text(sv.display, r.x + 10, r.y + 9, shown, FontId::Body16, d->serial_hint ? WHITE : ui::kMuted, saved ? ui::kGoodDark : ui::kPanel);
        char right[32];
        snprintf(right, sizeof(right), "%s%d dBm", d->serial_hint ? "serial  " : "", static_cast<int>(d->rssi));
        ui::text(sv.display, r.x + r.w - 10 - text_width(FontId::Body16, right), r.y + 9, right, FontId::Body16, ui::kMuted,
                 saved ? ui::kGoodDark : ui::kPanel);
    }
    ui::button(sv.display, kPgUp, "^", _scroll == 0 ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kPgDn, "v", _scroll + kRows >= n ? ui::Btn::Disabled : ui::Btn::Normal);
}

void JcBleScreen::draw_diag() {
    const JcBleDiag& d = sv.ble.diag();
    const uint32_t now = millis();
    char l[130];
    snprintf(l, sizeof(l), "STATE: %s   profile: %s   chunk: %u bytes", jc_ble_state_name(sv.ble.state()),
             jc_ble_profile_name(sv.ble.selection().profile), static_cast<unsigned>(sv.ble.chunk_size()));
    const uint16_t sc = sv.ble.ready() ? ui::kGood : (sv.ble.state() == JcBleState::Failed ? RED : ui::kInfo);
    _lines[0].draw(sv.display, { 28, 302, 744, 26 }, l, FontId::Body16, sc, ui::kPanel);
    snprintf(l, sizeof(l), "Saved device: %s %s", sv.ble_saved.valid ? sv.ble_saved.addr : "none", sv.ble_saved.valid ? sv.ble_saved.name : "");
    _lines[1].draw(sv.display, { 28, 330, 744, 26 }, l, FontId::Body16, WHITE, ui::kPanel);
    snprintf(l, sizeof(l), "RX %lu B  notifications %lu (partial %lu)  invalid frames %lu  dropped %lu", static_cast<unsigned long>(d.rx_bytes),
             static_cast<unsigned long>(d.notifications), static_cast<unsigned long>(d.partial_notifications),
             static_cast<unsigned long>(d.invalid_frames), static_cast<unsigned long>(d.rx_dropped));
    _lines[2].draw(sv.display, { 28, 358, 744, 26 }, l, FontId::Body16, ui::kMuted, ui::kPanel);
    snprintf(l, sizeof(l), "TX %lu B  queued %u  refused %lu  write errors %lu  blocked %lu", static_cast<unsigned long>(d.tx_bytes),
             static_cast<unsigned>(sv.ble.tx_queued()), static_cast<unsigned long>(d.tx_refused), static_cast<unsigned long>(d.write_errors),
             static_cast<unsigned long>(sv.ble.blocked()));
    _lines[3].draw(sv.display, { 28, 386, 744, 26 }, l, FontId::Body16, ui::kMuted, ui::kPanel);
    snprintf(l, sizeof(l), "connects %lu  disconnects %lu  reconnect attempts %lu  next retry %lu s", static_cast<unsigned long>(d.connects),
             static_cast<unsigned long>(d.disconnects), static_cast<unsigned long>(d.reconnect_attempts),
             static_cast<unsigned long>(sv.ble.ms_until_retry(now) / 1000u));
    _lines[4].draw(sv.display, { 28, 414, 744, 26 }, l, FontId::Body16, ui::kMuted, ui::kPanel);
    snprintf(l, sizeof(l), "Last error: %s", d.last_error[0] ? d.last_error : "none");
    _lines[5].draw(sv.display, { 28, 442, 744, 26 }, l, FontId::Body16, d.last_error[0] ? ui::kWarn : ui::kMuted, ui::kPanel);
}

void JcBleScreen::on_draw() {
    ui::text(sv.display, 16, 72, "BLE UART  -  HM-10 / CC2541 (FFE0/FFE1) or Nordic UART. NOT verified on hardware.", FontId::Body16, ui::kWarn, ui::kBg);
    ui::button(sv.display, kScan, sv.ble.state() == JcBleState::Scanning ? "SCANNING ..." : "SCAN",
               sv.settings.transport == 2 && sv.ble.state() != JcBleState::Scanning ? ui::Btn::Normal : ui::Btn::Disabled);
    ui::button(sv.display, kDisconnect, "DISCONNECT", sv.ble.state() == JcBleState::Off || sv.ble.state() == JcBleState::Idle ? ui::Btn::Disabled : ui::Btn::Normal);
    ui::button(sv.display, kForget, "FORGET SAVED", sv.ble_saved.valid ? ui::Btn::Danger : ui::Btn::Disabled);
    draw_list();
    ui::panel(sv.display, kDiagPanel);
    for (ui::Slot& s : _lines) s.invalidate();
    draw_diag();
    _drawn_key = list_key();
}

void JcBleScreen::on_touch(uint16_t x, uint16_t y) {
    if (ui::hit(kScan, x, y)) {
        if (!sv.host.ble_scan()) {
            show_message("CANNOT SCAN", sv.settings.transport != 2 ? "BLE is not the selected transport." : "A scan or connection is already in progress.",
                         "Choose BLE on the Connections page first.");
        } else {
            _scroll = 0;
            draw();
        }
        return;
    }
    if (ui::hit(kDisconnect, x, y)) {
        sv.host.ble_disconnect();
        draw();
        return;
    }
    if (ui::hit(kForget, x, y) && sv.ble_saved.valid) {
        sv.host.ble_forget();
        draw();
        return;
    }
    if (ui::hit(kPgUp, x, y)) { if (_scroll > 0) --_scroll; draw_list(); _drawn_key = list_key(); return; }
    if (ui::hit(kPgDn, x, y)) { if (_scroll + kRows < sv.ble.devices().count()) ++_scroll; draw_list(); _drawn_key = list_key(); return; }
    for (uint8_t i = 0; i < kRows; ++i) {
        if (!ui::hit(row_rect(i), x, y)) continue;
        const JcBleDevice* d = sv.ble.devices().sorted(static_cast<uint8_t>(_scroll + i));
        if (d == nullptr) return;
        if (!sv.host.ble_connect(d->addr, d->name)) show_message("CANNOT CONNECT", "BLE is not the selected transport or the address is invalid.");
        else draw();
        return;
    }
}

void JcBleScreen::on_tick(uint32_t now) {
    if (now - _last_tick < 300) return;
    _last_tick = now;
    draw_diag();
    if (list_key() != _drawn_key) {
        draw_list();
        _drawn_key = list_key();
    }
}

}  // namespace jc8048w550
