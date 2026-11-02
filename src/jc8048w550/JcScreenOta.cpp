// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// SETTINGS > FIRMWARE UPDATE. The page shows what is installed, whether the update service is available right now
// and why not, and the progress and result of a transfer. There is no arming step: the service is offered while the
// controller is Idle and a password is set. Nothing on this page can send a CNC command.

#include "JcScreens.hpp"

#include <stdio.h>
#include <string.h>

extern const char* git_info;

namespace jc8048w550 {

namespace {
constexpr ui::Rect kSetPassword { 16, 250, 250, 52 };
constexpr ui::Rect kClearPassword { 274, 250, 130, 52 };
constexpr ui::Rect kClearFailure { 16, 312, 250, 58 };
constexpr ui::Rect kRestart { 274, 312, 252, 58 };
constexpr ui::Rect kBar { 16, 380, 768, 22 };

const char* build_mode_text() {
#if JC_FULL_CONTROL_TEST
    return "Build: FULL CONTROL (this firmware can send commands)";
#elif JC_LIVE_JOG_TEST
    return "Build: LIVE JOG TEST (X/Y 1 mm jog only)";
#else
    return "Build: MOTION DISABLED (diagnostic firmware)";
#endif
}

void password_thunk(void* user, bool ok, const char* text) { static_cast<JcOtaScreen*>(user)->password_done(ok, text); }
}  // namespace

void JcOtaScreen::on_enter() {
    for (ui::Slot& s : _info) s.invalidate();
    _state.invalidate();
    _message.invalidate();
    _hint.invalidate();
    _bar = 255;
}

void JcOtaScreen::on_draw() {
    for (ui::Slot& s : _info) s.invalidate();
    _state.invalidate();
    _message.invalidate();
    _hint.invalidate();
    _bar = 255;
    ui::text(sv.display, 16, 70, "Firmware", FontId::Body16, ui::kMuted, ui::kBg);
    ui::button(sv.display, kSetPassword, "SET PASSWORD");
    ui::button(sv.display, kClearPassword, "CLEAR");
    draw_dynamic(true);
}

void JcOtaScreen::draw_dynamic(bool force) {
    JcOtaView v;
    sv.host.ota_view(v);
    char line[96];
    _info[0].draw(sv.display, { 110, 70, 674, 22 }, git_info, FontId::Body16, WHITE, ui::kBg);
#if JC_FULL_CONTROL_TEST
    const uint16_t mode_colour = RED;
#else
    const uint16_t mode_colour = ui::kMuted;
#endif
    _info[1].draw(sv.display, { 16, 94, 768, 22 }, build_mode_text(), FontId::Body16, mode_colour, ui::kBg);
    if (v.wifi_up) snprintf(line, sizeof(line), "Wi-Fi: connected   %s   host %s.local   port 3232", v.ip, v.host);
    else snprintf(line, sizeof(line), "Wi-Fi: NOT connected");
    _info[2].draw(sv.display, { 16, 118, 768, 22 }, line, FontId::Body16, v.wifi_up ? WHITE : ui::kWarn, ui::kBg);
    snprintf(line, sizeof(line), "OTA password: %s", v.password_set ? "set (never displayed)" : "NOT SET - set one before updating");
    _info[3].draw(sv.display, { 16, 142, 768, 22 }, line, FontId::Body16, v.password_set ? WHITE : ui::kWarn, ui::kBg);
    snprintf(line, sizeof(line), "Boot image: %s", v.rollback);
    _info[4].draw(sv.display, { 16, 166, 768, 22 }, line, FontId::Body16, ui::kMuted, ui::kBg);
    // The state line is the one the operator must not misread: ready / active / done / failed.
    uint16_t colour = WHITE;
    if (v.state == JcOtaState::Receiving || v.state == JcOtaState::Validating) colour = ui::kWarn;
    else if (v.state == JcOtaState::Ready || v.state == JcOtaState::PendingRestart) colour = ui::kGood;
    else if (v.state == JcOtaState::Failed) colour = RED;
    if (v.locked) snprintf(line, sizeof(line), "UPDATE: %s  COMMANDS LOCKED", jc_ota_state_name(v.state));
    else snprintf(line, sizeof(line), "UPDATE: %s", jc_ota_state_name(v.state));
    _state.draw(sv.display, { 16, 196, 768, 44 }, line, FontId::Button20, colour, ui::kBg);

    const bool ready = v.state == JcOtaState::Ready;
    const bool busy = v.state == JcOtaState::Receiving || v.state == JcOtaState::Validating || v.state == JcOtaState::PendingRestart;
    if (force || ready != _drawn_ready || v.state != _drawn_state) {
        ui::button(sv.display, kClearFailure, "CLEAR FAILURE", v.state == JcOtaState::Failed ? ui::Btn::Danger : ui::Btn::Disabled);
        ui::button(sv.display, kRestart, "RESTART PENDANT", busy ? ui::Btn::Disabled : ui::Btn::Normal);
        _drawn_ready = ready;
        _drawn_state = v.state;
    }
    // Progress bar.
    if (busy) {
        if (force || _bar != v.percent) {
            const int32_t inner = kBar.w - 2;
            const int16_t w = static_cast<int16_t>((inner * v.percent) / 100);
            if (force || _bar == 255 || v.percent < _bar) {
                // Full draw only when the bar first appears; afterwards only the newly filled part is painted, so
                // the panel is not flickered once per percent while the flash is being written.
                sv.display.drawRect(kBar.x, kBar.y, kBar.w, kBar.h, WHITE);
                sv.display.fillRect(kBar.x + 1, kBar.y + 1, kBar.w - 2, kBar.h - 2, ui::kBg);
                sv.display.fillRect(kBar.x + 1, kBar.y + 1, w, kBar.h - 2, ui::kGood);
            } else {
                const int16_t old_w = static_cast<int16_t>((inner * _bar) / 100);
                if (w > old_w) sv.display.fillRect(kBar.x + 1 + old_w, kBar.y + 1, w - old_w, kBar.h - 2, ui::kGood);
            }
            _bar = v.percent;
        }
    } else if (_bar != 255) {
        sv.display.fillRect(kBar.x, kBar.y, kBar.w, kBar.h, ui::kBg);
        _bar = 255;
    }
    if (v.message[0] != '\0') {
        snprintf(line, sizeof(line), "%s", v.message);
    } else if (v.state == JcOtaState::Off && v.blocker[0] != '\0') {
        snprintf(line, sizeof(line), "Not available: %s", v.blocker);
    } else {
        snprintf(line, sizeof(line), "Available whenever a password is set and the controller is Idle.");
    }
    _message.draw(sv.display, { 16, 408, 768, 22 }, line, FontId::Body16,
                  v.state == JcOtaState::Failed ? RED : WHITE, ui::kBg);
    _hint.draw(sv.display, { 16, 434, 768, 40 },
               "Password only: the upload is NOT encrypted or signed. Use on a trusted LAN. The E-stop is unaffected.",
               FontId::Body16, ui::kMuted, ui::kBg);
}

void JcOtaScreen::refresh() { draw_dynamic(false); }

void JcOtaScreen::on_tick(uint32_t now) {
    if (now - _last < 400) return;
    _last = now;
    draw_dynamic(false);
}

void JcOtaScreen::password_done(bool ok, const char* text) {
    if (!ok) return;
    const char* err = sv.host.ota_set_password(text);
    if (err != nullptr) {
        show_message("PASSWORD NOT SET", err);
    } else {
        draw();
    }
}

void JcOtaScreen::on_touch(uint16_t x, uint16_t y) {
    JcOtaView v;
    sv.host.ota_view(v);
    const bool busy = v.state == JcOtaState::Receiving || v.state == JcOtaState::Validating || v.state == JcOtaState::PendingRestart;
    if (ui::hit(kSetPassword, x, y)) {
        if (busy) {
            show_message("UPDATE ACTIVE", "Wait for the update to finish.");
            return;
        }
        JcInputRequest r {};
        r.prompt = "New OTA password (8-32 characters, no spaces)";
        r.max_len = 32;
        r.secret = true;
        r.done = password_thunk;
        r.user = this;
        sv.host.request_input(r);
    } else if (ui::hit(kClearPassword, x, y)) {
        if (busy) {
            show_message("UPDATE ACTIVE", "Wait for the update to finish.");
            return;
        }
        sv.host.ota_clear_password();
        draw();
    } else if (ui::hit(kClearFailure, x, y)) {
        if (v.state == JcOtaState::Failed) {
            sv.host.ota_clear_failure();
            draw();
        }
    } else if (ui::hit(kRestart, x, y)) {
        if (!busy) {
            sv.host.ota_restart();
        }
    }
}

}  // namespace jc8048w550
