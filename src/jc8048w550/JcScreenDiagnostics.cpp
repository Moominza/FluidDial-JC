// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcScreens.hpp"

#include <WiFi.h>

extern const char* git_info;
extern const char* git_url;

namespace jc8048w550 {

namespace {
ui::Rect label_rect(uint8_t row) {
    const bool right = row >= 8;
    return { static_cast<int16_t>(right ? 400 : 16), static_cast<int16_t>(76 + (row % 8) * 34), 150, 30 };
}
ui::Rect value_rect(uint8_t row) {
    const bool right = row >= 8;
    return { static_cast<int16_t>(right ? 552 : 168), static_cast<int16_t>(76 + (row % 8) * 34), static_cast<int16_t>(right ? 232 : 220), 30 };
}
const char* const kLabels[16] = {
    "Firmware", "Build", "Uptime", "Heap free", "Heap min", "PSRAM free", "Wi-Fi", "Link",
    "Status age", "Interval", "Parse errors", "Blocked TX", "Reconnects", "Transport", "Cmd gate", "Last error",
};
}  // namespace

void JcDiagnosticsScreen::row_text(uint8_t row, char* label, size_t label_size, char* value, size_t value_size) const {
    snprintf(label, label_size, "%s", kLabels[row]);
    const JcReadOnlyFluidNc& link = sv.link;
    switch (row) {
        case 0: snprintf(value, value_size, "%s", git_info); break;
        case 1: snprintf(value, value_size, "%s", __DATE__ " " __TIME__); break;
        case 2: {
            const unsigned long s = millis() / 1000;
            snprintf(value, value_size, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
            break;
        }
        case 3: snprintf(value, value_size, "%lu KB", static_cast<unsigned long>(ESP.getFreeHeap() / 1024)); break;
        case 4: snprintf(value, value_size, "%lu KB", static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024)); break;
        case 5: snprintf(value, value_size, "%lu KB", static_cast<unsigned long>(ESP.getFreePsram() / 1024)); break;
        case 6:
            if (WiFi.status() == WL_CONNECTED) {
                snprintf(value, value_size, "%s %ddBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            } else {
                snprintf(value, value_size, "not connected");
            }
            break;
        case 7: snprintf(value, value_size, "%s", JcReadOnlyFluidNc::state_name(link.state())); break;
        case 8:
            if (link.status().has_data) {
                snprintf(value, value_size, "%lu ms", static_cast<unsigned long>(link.last_status_age_ms()));
            } else {
                snprintf(value, value_size, "no data");
            }
            break;
        case 9: snprintf(value, value_size, "%lu ms", static_cast<unsigned long>(link.status().update_interval_ms)); break;
        case 10: snprintf(value, value_size, "%lu", static_cast<unsigned long>(link.parse_errors())); break;
        case 11: snprintf(value, value_size, "%lu", static_cast<unsigned long>(link.blocked_transmissions())); break;
        case 12: snprintf(value, value_size, "%lu", static_cast<unsigned long>(link.reconnect_attempts())); break;
        case 13:
            snprintf(value, value_size, "%s%s", transport_name(static_cast<TransportKind>(sv.settings.transport)),
                     _uart.running() ? " (UART up)" : "");
            break;
        case 14:
            snprintf(value, value_size, "blk %lu prev %lu sent %lu", static_cast<unsigned long>(sv.dispatcher.blocked()),
                     static_cast<unsigned long>(sv.dispatcher.previewed()), static_cast<unsigned long>(sv.dispatcher.sent()));
            break;
        default: snprintf(value, value_size, "%s", link.last_error()[0] ? link.last_error() : "none"); break;
    }
}

void JcDiagnosticsScreen::on_enter() {
    for (auto& s : _slots) s.invalidate();
    _history.invalidate();
    _dispatch.invalidate();
}

void JcDiagnosticsScreen::on_draw() {
    for (uint8_t r = 0; r < kRows; ++r) {
        char label[24], value[8];
        row_text(r, label, sizeof(label), value, sizeof(value));
        const ui::Rect l = label_rect(r);
        ui::text(sv.display, l.x + 2, l.y + 5, label, FontId::Body16, ui::kMuted, ui::kBg);
        _slots[r].invalidate();
    }
    _history.invalidate();
    _dispatch.invalidate();
    ui::text(sv.display, 16, 356, "Machine state history (newest first)", FontId::Body16, ui::kMuted, ui::kBg);
    ui::text(sv.display, 16, 410, "Last command preview", FontId::Body16, ui::kMuted, ui::kBg);
    draw_rows();
}

void JcDiagnosticsScreen::draw_rows() {
    for (uint8_t r = 0; r < kRows; ++r) {
        char label[24], value[72];
        row_text(r, label, sizeof(label), value, sizeof(value));
        _slots[r].draw(sv.display, value_rect(r), value, FontId::Body16, WHITE, ui::kBg);
    }
    char line[72] = "";
    for (uint8_t i = 0; i < sv.model.history_count() && i < 6; ++i) {
        const JcHistoryEntry* e = sv.model.history(i);
        if (!e) break;
        const size_t used = strlen(line);
        snprintf(line + used, sizeof(line) - used, "%s%s", i ? " < " : "", e->state);
    }
    _history.draw(sv.display, { 16, 380, 768, 26 }, line[0] ? line : "(no status received)", FontId::Body16, WHITE, ui::kBg);
    const JcDispatchRecord* d = sv.dispatcher.history(0);
    char cmd[110];
    if (d) {
        snprintf(cmd, sizeof(cmd), "%s  ->  %s", d->text, d->valid ? jc_auth_name(d->decision) : "rejected");
    } else {
        snprintf(cmd, sizeof(cmd), "(none)");
    }
    _dispatch.draw(sv.display, { 16, 434, 768, 26 }, cmd, FontId::Body16, d ? ui::kWarn : ui::kMuted, ui::kBg);
}

void JcDiagnosticsScreen::on_tick(uint32_t now) {
    if (now - _last_tick >= 500) {
        _last_tick = now;
        draw_rows();
    }
}

}  // namespace jc8048w550
