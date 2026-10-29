// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcPendantUi.hpp"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <cstring>
#include <stdio.h>

#include "JcFontRenderer.hpp"

namespace jc8048w550 {
namespace {

constexpr uint16_t kBackground = 0x0841;
constexpr uint16_t kPanel = 0x18E3;
constexpr uint16_t kPanelLight = 0x2965;
constexpr uint16_t kAccentDark = 0x5000;
constexpr uint16_t kMuted = 0x9CF3;
constexpr uint16_t kBorder = 0x5ACB;
constexpr uint16_t kSuccess = 0x07E0;
constexpr uint16_t kInfo = 0x051F;

constexpr PendantUi::Rect kHomeButton { 704, 12, 80, 42 };
constexpr PendantUi::Rect kSettingsButton { 600, 12, 92, 42 };
constexpr PendantUi::Rect kConnectionsButton { 702, 12, 82, 42 };
constexpr uint16_t kCalibrationTargetX[5] = { 60, 740, 740, 60, 400 };
constexpr uint16_t kCalibrationTargetY[5] = { 60, 60, 420, 420, 240 };
constexpr const char* kCalibrationTargetNames[5] = {
    "TOP-LEFT", "TOP-RIGHT", "BOTTOM-RIGHT", "BOTTOM-LEFT", "CENTRE"
};
constexpr PendantUi::Rect kCalibrationSaveButton { 40, 405, 220, 54 };
constexpr PendantUi::Rect kCalibrationRetryButton { 290, 405, 220, 54 };
constexpr PendantUi::Rect kCalibrationCancelButton { 540, 405, 220, 54 };
constexpr PendantUi::Rect kCalibrationCancelTargetButton { 40, 300, 160, 44 };
constexpr PendantUi::Rect kCalibrationResetTargetButton { 600, 300, 160, 44 };

uint16_t contrasting_text(bool emphasized) {
    return emphasized ? WHITE : LIGHTGREY;
}

void draw_text_top(Arduino_GFX& display, int16_t x, int16_t y,
                   const char* text, FontId font, uint16_t foreground,
                   uint16_t background) {
    draw_text(display, x, y + font_baseline(font), text, font, foreground, background);
}

void draw_text_centered(Arduino_GFX& display, PendantUi::Rect bounds,
                        const char* text, FontId font, uint16_t foreground,
                        uint16_t background) {
    const int16_t width = text_width(font, text);
    const int16_t line_height = font_line_height(font);
    const int16_t x = bounds.x + (bounds.width - width) / 2;
    const int16_t baseline = bounds.y + (bounds.height - line_height) / 2
                           + font_baseline(font);
    draw_text(display, x, baseline, text, font, foreground, background);
}

float median_value(const TouchSample* samples, bool y_axis) {
    float values[9];
    for (uint8_t i = 0; i < 9; ++i) {
        values[i] = y_axis ? samples[i].y : samples[i].x;
    }
    for (uint8_t i = 1; i < 9; ++i) {
        const float value = values[i];
        uint8_t index = i;
        while (index > 0 && values[index - 1] > value) {
            values[index] = values[index - 1];
            --index;
        }
        values[index] = value;
    }
    return values[4];
}

} // namespace

PendantUi::PendantUi(Arduino_GFX& display) : _display(display) {}

const char* PendantUi::axis_name(uint8_t axis) {
    static constexpr const char* names[kAxisCount] = { "X", "Y", "Z", "A" };
    return names[axis < kAxisCount ? axis : 0];
}

bool PendantUi::use_uart() const {
    return _app.settings().transport == 1 && _uart.running();
}

const JcLiveMachineStatus& PendantUi::live_status() const {
    return use_uart() ? _uart.status() : _fluidnc.status();
}

bool PendantUi::live_fresh() const {
    return use_uart() ? _uart.status_is_fresh() : _fluidnc.status_is_fresh();
}

JcFluidState PendantUi::live_state() const {
    return use_uart() ? _uart.state() : _fluidnc.state();
}

void PendantUi::update_model() {
    const JcLiveMachineStatus& status = live_status();
    if (&status != _model_source || status.sequence != _model_sequence) {
        _model_source = &status;
        _model_sequence = status.sequence;
        if (status.has_data) {
            _app.feed_status(status);
        }
    }
    _app.note_link_fresh(live_fresh());
}

void PendantUi::app_leave_thunk(void* user, JcLeaveTarget target) {
    static_cast<PendantUi*>(user)->handle_app_leave(target);
}

void PendantUi::handle_app_leave(JcLeaveTarget target) {
    switch (target) {
        case JcLeaveTarget::Dashboard:
            _page = Page::Home;
            _last_action = "Dashboard";
            break;
        case JcLeaveTarget::Connections:
            _page = Page::Connections;
            break;
        case JcLeaveTarget::WifiSettings:
            _edit_wifi_config = _fluidnc.config();
            _wifi_field = WifiField::Ssid;
            _keyboard_symbols = false;
            _keyboard_shift = false;
            _keyboard_visible = true;
            _page = Page::WifiSettings;
            Serial.println("net: Wi-Fi configuration opened; connect remains manual");
            break;
        case JcLeaveTarget::Calibration:
            Serial.println("ui: guided calibration opened from application settings");
            begin_calibration();
            return;
    }
    redraw();
}

float PendantUi::step_value(uint8_t index, uint8_t axis) {
    static constexpr float linear[kStepCount] = { 0.1f, 1.0f, 10.0f, 50.0f, 100.0f };
    static constexpr float rotary[kStepCount] = { 0.1f, 1.0f, 10.0f, 45.0f, 90.0f };
    const float* values = axis == 3 ? rotary : linear;
    return values[index < kStepCount ? index : 0];
}

void PendantUi::preview_jog(float signed_distance) {
    // Builds the command the real controller would receive and submits it to
    // the authorization gate. The gate has no sink and the build policy blocks
    // everything, so this only logs a preview; nothing is transmitted.
    const JcAxisProfile& axis = _profile.axes[_selected_axis];
    const float feed = axis.max_feed < 1000.0f ? axis.max_feed : 1000.0f;
    const JcCommand command = jc_jog_incremental(_profile, _selected_axis, signed_distance, feed);
    JcAuthContext context;
    context.live = _live_mode;
    context.now_ms = millis();
    context.command_issued_ms = context.now_ms;
    if (!command.ok) {
        Serial.printf("preview: jog rejected by validation (error %u)\n",
                      static_cast<unsigned>(command.error));
        return;
    }
    const JcAuthDecision decision = _gate.submit(command.cls, command.text, context);
    Serial.printf("preview: [%s] NOT SENT (%s)\n", command.text, jc_auth_name(decision));
}

uint16_t PendantUi::clamp_x(uint16_t x) const {
    return x < kWidth ? x : kWidth - 1;
}

uint16_t PendantUi::clamp_y(uint16_t y) const {
    return y < kHeight ? y : kHeight - 1;
}

bool PendantUi::contains(Rect bounds, uint16_t x, uint16_t y) const {
    return x >= bounds.x && y >= bounds.y
        && x < bounds.x + bounds.width && y < bounds.y + bounds.height;
}

void PendantUi::begin() {
    _fluidnc.begin();
    _app.begin(&PendantUi::app_leave_thunk, this);
    _edit_wifi_config = _fluidnc.config();
    _calibration_loaded = load_calibration();
    _recovery_window_until = millis() + kRecoveryWindowMs;
    if (_calibration_loaded) {
        _page = Page::Home;
        draw();
        Serial.println("ui: landscape demo started; saved calibration active; recovery hold available for 10s; manual read-only status available; motion disabled.");
    } else {
        Serial.println("ui: no saved calibration; opening guided calibration before dashboard.");
        begin_calibration();
    }
}

void PendantUi::poll(TAMC_GT911& touch) {
    _fluidnc.poll();
    _uart.poll();
    update_model();
    touch.read();

    if (touch.isTouched && touch.touches > 0) {
        const auto& point = touch.points[0];
        uint16_t ui_x;
        uint16_t ui_y;
        map_touch_for_ui(point, ui_x, ui_y);

        const uint32_t now = millis();
        const bool changed = point.rawX != _last_raw_x
            || point.rawY != _last_raw_y
            || point.x != _last_driver_x
            || point.y != _last_driver_y;
        if (!_was_touched || (changed && now - _last_touch_log_ms >= 100)) {
            log_touch(touch.touches, point, ui_x, ui_y);
            _last_raw_x = point.rawX;
            _last_raw_y = point.rawY;
            _last_driver_x = point.x;
            _last_driver_y = point.y;
            _last_touch_log_ms = now;
        }

        if (!_was_touched) {
            if (_page == Page::TouchCalibration
                && _calibration_phase == CalibrationPhase::Ready) {
                if (contains(kCalibrationCancelTargetButton, ui_x, ui_y)
                    || contains(kCalibrationResetTargetButton, ui_x, ui_y)) {
                    if (touch.touches == 1) {
                        handle_touch(ui_x, ui_y);
                    }
                } else {
                    begin_calibration_touch(touch.touches, point);
                }
            } else if (_page == Page::TouchCalibration
                       && _calibration_phase != CalibrationPhase::Capturing
                       && touch.touches == 1) {
                const uint32_t action_started_us = micros();
                handle_touch(ui_x, ui_y);
                Serial.printf("ui: touch_action_ms=%lu\n",
                              static_cast<unsigned long>((micros() - action_started_us) / 1000));
            } else if (_recovery_touch_pending || _recovery_triggered) {
                if (_recovery_touch_pending && _page == Page::TouchCalibration
                    && _calibration_phase == CalibrationPhase::Capturing) {
                    collect_calibration_sample(touch.touches, point);
                }
            } else if (static_cast<int32_t>(now - _recovery_window_until) < 0
                       && _page != Page::TouchCalibration) {
                if (touch.touches == 1) {
                    _recovery_touch_pending = true;
                    _recovery_touch_started = now;
                    _recovery_touch_x = ui_x;
                    _recovery_touch_y = ui_y;
                } else {
                    Serial.printf("touch: ignored startup multi-touch count=%u\n", touch.touches);
                }
            } else {
                if (touch.touches == 1) {
                    const uint32_t action_started_us = micros();
                    handle_touch(ui_x, ui_y);
                    Serial.printf("ui: touch_action_ms=%lu\n",
                                  static_cast<unsigned long>((micros() - action_started_us) / 1000));
                } else {
                    Serial.printf("ui: ignored initial multi-touch count=%u\n", touch.touches);
                }
            }
        } else if (_page == Page::TouchCalibration
                   && _calibration_phase == CalibrationPhase::Capturing
                   && _calibration_touch_active) {
            collect_calibration_sample(touch.touches, point);
        } else if (_recovery_touch_pending && !_recovery_triggered
                   && now - _recovery_touch_started >= kRecoveryHoldMs) {
            _recovery_triggered = true;
            _recovery_touch_pending = false;
            begin_calibration();
            Serial.println("touch: startup recovery hold; calibration opened; release then start target 1.");
        } else if (_recovery_touch_pending && touch.touches > 1) {
            Serial.printf("touch: recovery hold ignored multi-touch count=%u\n", touch.touches);
            _recovery_touch_pending = false;
        }
        _was_touched = true;
    } else if (_was_touched) {
        Serial.println("touch: released");
        if (_page == Page::TouchCalibration && _calibration_touch_active) {
            finish_calibration_touch();
        } else if (_recovery_touch_pending && !_recovery_triggered) {
            const uint32_t held_ms = millis() - _recovery_touch_started;
            _recovery_touch_pending = false;
            if (held_ms >= kRecoveryHoldMs) {
                _recovery_triggered = true;
                begin_calibration();
                Serial.println("touch: startup recovery hold; calibration opened.");
            } else {
                handle_touch(_recovery_touch_x, _recovery_touch_y);
            }
        }
        _was_touched = false;
        _recovery_triggered = false;
        _last_raw_x = UINT16_MAX;
        _last_raw_y = UINT16_MAX;
        _last_driver_x = UINT16_MAX;
        _last_driver_y = UINT16_MAX;
    }

    if (_feedback != Feedback::None
        && static_cast<int32_t>(millis() - _feedback_until) >= 0) {
        const Feedback expired_feedback = _feedback;
        _feedback = Feedback::None;
        if (_page == Page::Home) {
            draw_jog_button(expired_feedback);
            draw_home_footer();
            _last_displayed_status_sequence = 0;
            _last_displayed_link_state = UINT32_MAX;
            _has_displayed_live_axes = false;
        }
    }
    if (_page == Page::App) {
        _app.tick();
    }
    draw_live_status_updates();
}

void PendantUi::log_touch(uint8_t count, const TP_Point& point,
                          uint16_t ui_x, uint16_t ui_y) {
    Serial.printf(
        "touch: count=%u raw_x=%u raw_y=%u driver_x=%u driver_y=%u mapped_ui_x=%u mapped_ui_y=%u size=%u\n",
        count,
        point.rawX,
        point.rawY,
        point.x,
        point.y,
        ui_x,
        ui_y,
        point.size);
}

void PendantUi::log_calibration_sample(uint8_t count, const TP_Point& point,
                                       uint16_t ui_x, uint16_t ui_y) {
    Serial.printf(
        "cal: target=%u sample=%u touch_count=%u raw_x=%u raw_y=%u driver_x=%u driver_y=%u mapped_ui_x=%u mapped_ui_y=%u\n",
        static_cast<unsigned>(_calibration_target + 1),
        static_cast<unsigned>(_calibration_sample_count),
        count, point.rawX, point.rawY, point.x, point.y, ui_x, ui_y);
}

void PendantUi::map_touch_for_ui(const TP_Point& point, uint16_t& ui_x,
                                uint16_t& ui_y) const {
    if (_page != Page::TouchCalibration) {
        map_touch(_touch_map, point.x, point.y, ui_x, ui_y);
        return;
    }

    const bool use_candidate = _calibration_phase == CalibrationPhase::Result
        || (_calibration_phase == CalibrationPhase::ResetConfirm
            && !_reset_return_to_settings
            && _calibration_previous_phase == CalibrationPhase::Result);
    if (use_candidate && valid_touch_map(_calibration_result.map)) {
        map_touch(_calibration_result.map, point.x, point.y, ui_x, ui_y);
    } else if (_calibration_phase == CalibrationPhase::ResetConfirm
               && _reset_return_to_settings) {
        map_touch(_touch_map, point.x, point.y, ui_x, ui_y);
    } else {
        ui_x = clamp_x(point.rawX);
        ui_y = clamp_y(point.rawY);
    }
}

void PendantUi::draw_button(Rect bounds, const char* label, bool selected,
                            bool emphasized) {
    const uint16_t fill = selected ? kInfo : (emphasized ? kAccentDark : kPanelLight);
    const uint16_t border = selected ? CYAN : (emphasized ? RED : kBorder);
    const uint16_t text = contrasting_text(selected || emphasized);
    _display.fillRect(bounds.x, bounds.y, bounds.width, bounds.height, fill);
    _display.drawRect(bounds.x, bounds.y, bounds.width, bounds.height, border);
    FontId font = FontId::Button20;
    if (text_width(font, label) > bounds.width - 20) {
        font = FontId::Body16;
    }
    draw_text_centered(_display, bounds, label, font, text, fill);
}

void PendantUi::draw_axis_tile(uint8_t axis, Rect bounds) {
    const bool selected = axis == _selected_axis;
    const uint16_t fill = selected ? kPanelLight : kPanel;
    _display.fillRect(bounds.x, bounds.y, bounds.width, bounds.height,
                      fill);
    _display.drawRect(bounds.x, bounds.y, bounds.width, bounds.height,
                      selected ? CYAN : kBorder);
    draw_text_top(_display, bounds.x + 14, bounds.y + 5, axis_name(axis),
                  FontId::Button20, selected ? CYAN : kMuted, fill);
    draw_position(axis);
}

void PendantUi::draw_axis_selection(uint8_t axis) {
    const Rect bounds { static_cast<int16_t>(16 + axis * 195), 76, 183, 101 };
    draw_axis_tile(axis, bounds);
}

void PendantUi::draw_axis_selector(uint8_t axis) {
    const Rect bounds { static_cast<int16_t>(88 + axis * 82), 276, 70, 38 };
    draw_button(bounds, axis_name(axis), axis == _selected_axis);
}

void PendantUi::draw_step_selector(uint8_t step) {
    static constexpr const char* linear_steps[kStepCount] = { "0.1 mm", "1.0 mm", "10 mm", "50 mm", "100 mm" };
    static constexpr const char* rotary_steps[kStepCount] = { "0.1 deg", "1.0 deg", "10 deg", "45 deg", "90 deg" };
    const char* const* labels = _selected_axis == 3 ? rotary_steps : linear_steps;
    const Rect bounds { static_cast<int16_t>(88 + step * 90), 325, 82, 38 };
    draw_button(bounds, labels[step], step == _selected_step);
}

void PendantUi::draw_transport_card(uint8_t index) {
    static constexpr const char* titles[kTransportCount] = {
        "WI-FI / TELNET TCP", "WIRED UART", "BLUETOOTH LE"
    };
    static constexpr const char* details[kTransportCount] = {
        "FluidNC TCP adapter", "Serial byte stream", "Future transport"
    };
    static constexpr TransportKind transports[kTransportCount] = {
        TransportKind::WifiFluidNcTcp,
        TransportKind::WiredUart,
        TransportKind::BluetoothLe
    };
    if (index >= kTransportCount) {
        return;
    }

    const Rect bounds { static_cast<int16_t>(16 + index * 258), 112, 244, 154 };
    const bool selected = _selected_transport == transports[index];
    _display.fillRect(bounds.x, bounds.y, bounds.width, bounds.height,
                      selected ? kPanelLight : kPanel);
    _display.drawRect(bounds.x, bounds.y, bounds.width, bounds.height,
                      selected ? CYAN : kBorder);
    draw_text_centered(_display, { bounds.x, static_cast<int16_t>(bounds.y + 30),
                                   bounds.width, 36 },
                       titles[index], FontId::Button20,
                       selected ? CYAN : WHITE, selected ? kPanelLight : kPanel);
    draw_text_centered(_display, { bounds.x, static_cast<int16_t>(bounds.y + 72),
                                   bounds.width, 28 },
                       details[index], FontId::Body16, kMuted,
                       selected ? kPanelLight : kPanel);
    if (selected) {
        draw_text_top(_display, bounds.x + 15, bounds.y + 111, "SELECTED",
                      FontId::Body16, kSuccess, kPanelLight);
    }
}

void PendantUi::draw_transport_status() {
    _display.fillRect(28, 404, 740, 24, kAccentDark);
    char status[48];
    snprintf(status, sizeof(status), "Selected: %s", transport_name(_selected_transport));
    draw_text_top(_display, 34, 406, status, FontId::Body16, kMuted, kAccentDark);
}

void PendantUi::draw_home_footer() {
    _display.fillRect(22, 452, 756, 22, kPanel);
    draw_text_top(_display, 28, 453, _last_action, FontId::Body16, kMuted, kPanel);
}

void PendantUi::draw_jog_buttons() {
    draw_jog_button(Feedback::JogNegative);
    draw_jog_button(Feedback::Stop);
    draw_jog_button(Feedback::JogPositive);
}

void PendantUi::draw_jog_button(Feedback feedback) {
    if (_live_mode) {
        switch (feedback) {
            case Feedback::JogNegative:
            case Feedback::JogPositive:
                draw_button(feedback == Feedback::JogNegative
                                ? Rect { 30, 376, 260, 74 }
                                : Rect { 510, 376, 260, 74 },
                            "MOTION DISABLED", false, true);
                break;
            case Feedback::Stop:
                draw_button({ 300, 376, 200, 74 }, "READ ONLY", false, true);
                break;
            case Feedback::None:
                break;
        }
        return;
    }
    switch (feedback) {
        case Feedback::JogNegative:
            draw_button({ 30, 376, 260, 74 }, "JOG -", _feedback == feedback);
            break;
        case Feedback::Stop:
            draw_button({ 300, 376, 200, 74 }, "STOP / CANCEL", false,
                        _feedback == feedback);
            break;
        case Feedback::JogPositive:
            draw_button({ 510, 376, 260, 74 }, "JOG +", _feedback == feedback);
            break;
        case Feedback::None:
            break;
    }
}

void PendantUi::draw_position(uint8_t axis) {
    if (axis >= kAxisCount) {
        return;
    }
    const Rect bounds { static_cast<int16_t>(16 + axis * 195), 76, 183, 101 };
    const uint16_t fill = axis == _selected_axis ? kPanelLight : kPanel;
    constexpr int16_t kValueLeft = 8;
    constexpr int16_t kValueTop = 39;
    constexpr int16_t kValueWidth = 167;
    constexpr int16_t kValueHeight = 48;
    constexpr int16_t kValueBaseline = 76;
    _display.fillRect(bounds.x + kValueLeft, bounds.y + kValueTop,
                      kValueWidth, kValueHeight, fill);

    char value[20];
    float position = _positions[axis];
    bool has_value = true;
    if (_live_mode) {
        // Positions come from the machine model in the selected reference; an
        // unknown work offset is shown as unavailable, never as zero.
        has_value = _app.model().position(axis, _display_reference, position);
    }
    if (!has_value) {
        strlcpy(value, "---.---", sizeof(value));
    } else {
        snprintf(value, sizeof(value), "%.*f", static_cast<int>(_app.settings().coordinate_decimals),
                 static_cast<double>(position));
    }
    const char* const unit = axis == 3 ? "°" : "mm";
    static constexpr FontId dro_fonts[] = {
        FontId::Dro30, FontId::Dro28, FontId::Dro26, FontId::Dro24,
        FontId::Dro22, FontId::Dro20, FontId::Dro18
    };
    FontId value_font = FontId::Dro18;
    const int16_t available_width = kValueWidth - 4;
    for (FontId candidate : dro_fonts) {
        if (text_width(candidate, value) + 4 + text_width(FontId::Button20, unit)
            <= available_width) {
            value_font = candidate;
            break;
        }
    }

    const int16_t value_x = bounds.x + kValueLeft;
    const int16_t baseline = bounds.y + kValueBaseline;
    const uint16_t value_color = _live_mode && !live_fresh()
        ? kMuted : WHITE;
    draw_text(_display, value_x, baseline, value, value_font, value_color, fill);
    draw_text(_display, value_x + text_width(value_font, value) + 4, baseline,
              unit, FontId::Button20, kMuted, fill);
}

void PendantUi::draw_header(const char* title) {
    _display.fillRect(0, 0, kWidth, 64, kPanel);
    draw_text_top(_display, 18, 12, title, FontId::Heading24, WHITE, kPanel);

    const JcFluidState link_state = live_state();
    const bool is_fresh = live_fresh();
    const char* mode_label = _live_mode
        ? (is_fresh ? "LIVE" : "LIVE STALE")
        : "DEMO MODE";
    const uint16_t mode_fill = _live_mode
        ? (is_fresh ? 0x0320 : kAccentDark)
        : kAccentDark;
    const uint16_t mode_border = _live_mode && is_fresh ? kSuccess : RED;
    _display.fillRect(212, 13, 144, 38, mode_fill);
    _display.drawRect(212, 13, 144, 38, mode_border);
    draw_text_centered(_display, { 212, 13, 144, 38 }, mode_label,
                       FontId::Button20, WHITE, mode_fill);

    const bool connected = link_state == JcFluidState::Connected;
    const char* link_label = connected ? "FLUIDNC LINK"
        : (_live_mode && (link_state == JcFluidState::Stale
                          || link_state == JcFluidState::Lost
                          || link_state == JcFluidState::InvalidData)
            ? "DISCONNECTED" : "CNC OFFLINE");
    const uint16_t link_fill = connected ? 0x0320 : kBackground;
    _display.fillRect(368, 13, 208, 38, link_fill);
    _display.drawRect(368, 13, 208, 38, connected ? kSuccess : kBorder);
    draw_text_centered(_display, { 368, 13, 208, 38 }, link_label,
                       FontId::Button20, connected ? WHITE : kMuted, link_fill);
}

void PendantUi::draw_machine_status() {
    _display.fillRect(16, 185, 768, 48, kPanel);
    _display.drawRect(16, 185, 768, 48, kBorder);
    const JcLiveMachineStatus& status = live_status();
    if (!_live_mode) {
        draw_text_top(_display, 30, 198, "MACHINE", FontId::Button20, kMuted, kPanel);
        draw_text_top(_display, 124, 198, "IDLE", FontId::Button20, kSuccess, kPanel);
        draw_text_top(_display, 230, 198, "LINK", FontId::Button20, kMuted, kPanel);
        draw_text_top(_display, 290, 198, "DEMO", FontId::Button20, WHITE, kPanel);
        draw_text_top(_display, 405, 198, "TRANSPORT", FontId::Button20, kMuted, kPanel);
        draw_text_top(_display, 525, 198, transport_name(_selected_transport),
                      FontId::Button20, WHITE, kPanel);
        return;
    }

    const bool fresh = live_fresh();
    const JcFluidState link_state = live_state();
    const char* machine_state = fresh ? status.machine_state
        : (status.has_data ? "STALE" : "NO DATA");
    const char* link_text = JcReadOnlyFluidNc::state_name(link_state);
    uint16_t machine_color = kSuccess;
    if (!fresh || strcmp(status.machine_state, "Alarm") == 0) {
        machine_color = RED;
    } else if (strcmp(status.machine_state, "Idle") != 0) {
        machine_color = kInfo;
    }
    draw_text_top(_display, 30, 198, "MACHINE", FontId::Body16, kMuted, kPanel);
    draw_text_top(_display, 112, 198, machine_state, FontId::Button20,
                  machine_color, kPanel);
    draw_text_top(_display, 270, 198, "LINK", FontId::Body16, kMuted, kPanel);
    draw_text_top(_display, 315, 198, link_text, FontId::Body16,
                  fresh ? kSuccess : RED, kPanel);
    {
        float probe = 0.0f;
        const bool available = _app.model().position(0, _display_reference, probe);
        char reference[24];
        snprintf(reference, sizeof(reference), "%s%s",
                 _display_reference == JcRef::Work ? "WORK" : "MACHINE", available ? "" : " n/a");
        draw_button({ 600, 190, 176, 38 }, reference, false, !available);
    }
}

void PendantUi::draw() {
    switch (_page) {
        case Page::Home:
            draw_home();
            break;
        case Page::Settings:
            // The application settings page replaces the legacy settings page.
            _page = Page::App;
            _app.open_settings();
            break;
        case Page::App:
            _app.resume();
            break;
        case Page::Connections:
            draw_connections();
            break;
        case Page::WifiSettings:
            draw_wifi_settings();
            break;
        case Page::TouchCalibration:
            draw_touch_calibration();
            break;
    }
}

void PendantUi::draw_home() {
    _display.fillScreen(kBackground);
    draw_header("FLUID DIAL");
    draw_button(kSettingsButton, "MENU");
    draw_button(kConnectionsButton, "LINK");

    for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
        draw_axis_tile(axis, { static_cast<int16_t>(16 + axis * 195), 76, 183, 101 });
    }

    draw_machine_status();

    _display.fillRect(16, 243, 768, 225, kPanel);
    _display.drawRect(16, 243, 768, 225, kBorder);
    draw_text_top(_display, 30, 248,
                  _live_mode ? "READ-ONLY LIVE STATUS" : "DEMO JOG CONTROLS",
                  FontId::Button20, WHITE, kPanel);
    draw_text_top(_display, 320, 253,
                  _live_mode ? "MOTION DISABLED - NO COMMANDS SENT"
                             : "LOCAL POSITION SIMULATION ONLY - NO COMMANDS SENT",
                  FontId::Body16, kMuted, kPanel);

    if (_live_mode) {
        draw_live_info(true);
        draw_jog_buttons();
        draw_home_footer();
        return;
    }

    draw_text_top(_display, 30, 281, "AXIS", FontId::Body16, kMuted, kPanel);
    for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
        const Rect bounds { static_cast<int16_t>(88 + axis * 82), 276, 70, 38 };
        draw_button(bounds, axis_name(axis), axis == _selected_axis);
    }

    draw_text_top(_display, 30, 331, "STEP", FontId::Body16, kMuted, kPanel);
    static constexpr const char* linear_steps[kStepCount] = { "0.1 mm", "1.0 mm", "10 mm", "50 mm", "100 mm" };
    static constexpr const char* rotary_steps[kStepCount] = { "0.1 deg", "1.0 deg", "10 deg", "45 deg", "90 deg" };
    const char* const* step_labels = _selected_axis == 3 ? rotary_steps : linear_steps;
    for (uint8_t step = 0; step < kStepCount; ++step) {
        const Rect bounds { static_cast<int16_t>(88 + step * 90), 325, 82, 38 };
        draw_button(bounds, step_labels[step], step == _selected_step);
    }

    draw_jog_buttons();

    draw_home_footer();
}

void PendantUi::draw_live_info(bool force) {
    if (force) {
        for (auto& slot : _info) {
            slot.invalidate();
        }
    }
    const JcLiveMachineStatus& status = live_status();
    const bool fresh = live_fresh();
    const uint16_t ok = fresh ? WHITE : kMuted;
    char value[48];
    static constexpr const char* labels[6] = { "FEED RATE", "SPINDLE", "ACTIVE WCS", "ALARM", "JOB", "TRANSPORT" };
    for (uint8_t i = 0; i < 6; ++i) {
        const int16_t x = 30 + (i % 3) * 252;
        const int16_t y = 278 + (i / 3) * 62;
        uint16_t color = ok;
        switch (i) {
            case 0:
                if (status.has_data) {
                    snprintf(value, sizeof(value), "%g /min", static_cast<double>(status.feed));
                } else {
                    snprintf(value, sizeof(value), "--");
                }
                break;
            case 1:
                if (!status.has_data) {
                    snprintf(value, sizeof(value), "--");
                } else if (status.spindle > 0.0f) {
                    snprintf(value, sizeof(value), "%g RPM", static_cast<double>(status.spindle));
                } else {
                    snprintf(value, sizeof(value), "OFF");
                }
                break;
            case 2:
                snprintf(value, sizeof(value), "unknown (needs $G)");
                color = kMuted;
                break;
            case 3:
                if (status.has_data && strcmp(status.machine_state, "Alarm") == 0) {
                    snprintf(value, sizeof(value), "ALARM ACTIVE");
                    color = RED;
                } else {
                    snprintf(value, sizeof(value), "%s", status.has_data ? "none" : "--");
                }
                break;
            case 4:
                if (status.has_data && status.has_sd) {
                    snprintf(value, sizeof(value), "%.0f%% %s", static_cast<double>(status.sd_percent), status.sd_name);
                } else {
                    snprintf(value, sizeof(value), "%s", status.has_data ? "none" : "--");
                }
                break;
            default:
                snprintf(value, sizeof(value), "%s%s",
                         transport_name(static_cast<TransportKind>(_app.settings().transport)),
                         use_uart() ? " (UART)" : "");
                break;
        }
        draw_text_top(_display, x, y, labels[i], FontId::Body16, kMuted, kPanel);
        _info[i].draw(_display, { x, static_cast<int16_t>(y + 20), 240, 28 }, value, FontId::Button20, color, kPanel);
    }
}

void PendantUi::draw_settings() {
    _display.fillScreen(kBackground);
    draw_header("SETTINGS");
    draw_button(kHomeButton, "HOME");

    _display.fillRect(24, 90, 752, 94, kPanel);
    _display.drawRect(24, 90, 752, 94, kBorder);
    draw_text_top(_display, 44, 104, "DISPLAY", FontId::Button20, WHITE, kPanel);
    draw_text_top(_display, 44, 141, "800 x 480 landscape  |  ST7262 RGB",
                  FontId::Body16, kMuted, kPanel);

    _display.fillRect(24, 198, 752, 94, kPanel);
    _display.drawRect(24, 198, 752, 94, kBorder);
    draw_text_top(_display, 44, 212, "TOUCH INPUT", FontId::Button20, WHITE, kPanel);
    draw_text_top(_display, 44, 249, "GT911 capacitive touch  |  polling mode",
                  FontId::Body16, kMuted, kPanel);

    _display.fillRect(24, 306, 752, 94, kPanel);
    _display.drawRect(24, 306, 752, 94, RED);
    draw_text_top(_display, 44, 320, "DEMO / MOTION SAFETY",
                  FontId::Button20, WHITE, kPanel);
    draw_text_top(_display, 44, 357,
                  "Mock data only. Transport and CNC commands are disabled.",
                  FontId::Body16, kMuted, kPanel);

    draw_button({ 24, 416, 230, 44 }, "TOUCH CALIBRATION");
    draw_button({ 280, 416, 230, 44 }, "CONNECTION OPTIONS");
    draw_button({ 536, 416, 240, 44 }, "RESET TOUCH");
}

void PendantUi::draw_touch_calibration() {
    _display.fillScreen(kBackground);
    draw_calibration_header();

    if (_calibration_phase == CalibrationPhase::ResetConfirm) {
        _display.fillRect(90, 125, 620, 215, kPanel);
        _display.drawRect(90, 125, 620, 215, RED);
        draw_text_centered(_display, { 105, 145, 590, 40 },
                           "RESTORE FACTORY TOUCH MAPPING?",
                           FontId::Button20, WHITE, kPanel);
        draw_text_centered(_display, { 105, 195, 590, 52 },
                           "This removes the saved calibration. The uncalibrated",
                           FontId::Body16, kMuted, kPanel);
        draw_text_centered(_display, { 105, 222, 590, 38 },
                           "GT911 mapping will be used after confirmation.",
                           FontId::Body16, kMuted, kPanel);
        draw_button({ 180, 285, 190, 48 }, "RESET", false, true);
        draw_button({ 430, 285, 190, 48 }, "KEEP CURRENT");
        return;
    }

    if (_calibration_phase == CalibrationPhase::Result) {
        draw_calibration_result();
        return;
    }
    draw_calibration_target();
}

void PendantUi::draw_calibration_header() {
    _display.fillRect(0, 0, kWidth, 64, kPanel);
    draw_text_centered(_display, { 200, 1, 400, 30 }, "TOUCH CAL",
                       FontId::Heading24, WHITE, kPanel);
    draw_text_centered(_display, { 200, 32, 400, 26 },
                       "DEMO MODE  |  CNC OFFLINE",
                       FontId::Body16, kMuted, kPanel);
}

void PendantUi::draw_calibration_target() {
    _display.fillRect(0, 70, kWidth, 130, kBackground);
    if (_calibration_target > 0) {
        const uint8_t previous_target = _calibration_target - 1;
        _display.fillRect(kCalibrationTargetX[previous_target] - 36,
                          kCalibrationTargetY[previous_target] - 36,
                          72, 72, kBackground);
    }
    char step[32];
    snprintf(step, sizeof(step), "Point %u of 5",
             static_cast<unsigned>(_calibration_target + 1));
    draw_text_centered(_display, { 160, 82, 480, 36 }, step,
                       FontId::Heading24, WHITE, kBackground);
    draw_text_centered(_display, { 120, 122, 560, 32 },
                       kCalibrationTargetNames[_calibration_target],
                       FontId::Button20, CYAN, kBackground);
    draw_text_centered(_display, { 100, 160, 600, 28 },
                       "Touch the target, hold steady, then release.",
                       FontId::Body16, kMuted, kBackground);

    const int16_t x = kCalibrationTargetX[_calibration_target];
    const int16_t y = kCalibrationTargetY[_calibration_target];
    _display.drawCircle(x, y, 24, WHITE);
    _display.drawCircle(x, y, 18, CYAN);
    _display.drawLine(x - 34, y, x + 34, y, WHITE);
    _display.drawLine(x, y - 34, x, y + 34, WHITE);
    _display.fillCircle(x, y, 4, RED);

    _display.fillRect(120, 350, 560, 32, kBackground);
    if (_calibration_phase == CalibrationPhase::Capturing) {
        char progress[40];
        snprintf(progress, sizeof(progress), "Sampling %u / 9 - keep touching",
                 static_cast<unsigned>(_calibration_sample_count));
        draw_text_centered(_display, { 120, 350, 560, 32 }, progress,
                           FontId::Body16, kInfo, kBackground);
    } else if (_calibration_retry_message) {
        draw_text_centered(_display, { 120, 350, 560, 32 },
                           "Unstable or multi-touch. Release and retry this point.",
                           FontId::Body16, RED, kBackground);
    } else {
        draw_text_centered(_display, { 120, 350, 560, 32 },
                           "One target at a time; multi-touch is rejected.",
                           FontId::Body16, kMuted, kBackground);
    }
    draw_button(kCalibrationCancelTargetButton, "CANCEL");
    draw_button(kCalibrationResetTargetButton, "FACTORY RESET");
}

void PendantUi::draw_calibration_result() {
    _display.fillScreen(kBackground);
    draw_calibration_header();
    draw_text_centered(_display, { 120, 78, 560, 36 },
                       _calibration_pass ? "CALIBRATION PASS" : "CALIBRATION RETRY",
                       FontId::Heading24,
                       _calibration_pass ? kSuccess : RED,
                       kBackground);

    char summary[96];
    snprintf(summary, sizeof(summary), "Corner error: max %.1f px  |  average %.1f px",
             _calibration_result.maximum_corner_error,
             _calibration_result.average_corner_error);
    draw_text_centered(_display, { 75, 125, 650, 32 }, summary,
                       FontId::Button20, WHITE, kBackground);
    snprintf(summary, sizeof(summary), "Centre-point error: %.1f px",
             _calibration_result.centre_error);
    draw_text_centered(_display, { 75, 164, 650, 30 }, summary,
                       FontId::Button20, WHITE, kBackground);
    snprintf(summary, sizeof(summary), "Axis swap: %s  |  X inverted: %s  |  Y inverted: %s",
             _calibration_result.axis_swapped ? "yes" : "no",
             _calibration_result.x_inverted ? "yes" : "no",
             _calibration_result.y_inverted ? "yes" : "no");
    draw_text_centered(_display, { 35, 205, 730, 30 }, summary,
                       FontId::Body16, kMuted, kBackground);

    draw_button(kCalibrationSaveButton, "SAVE CALIBRATION", _calibration_pass);
    draw_button(kCalibrationRetryButton, "RETRY CALIBRATION");
    draw_button(kCalibrationCancelButton, "CANCEL");
    draw_button({ 40, 343, 220, 42 }, "FACTORY RESET");
    draw_text_centered(_display, { 280, 343, 480, 42 },
                       "Mapping is not applied until saved.",
                       FontId::Body16, kMuted, kBackground);
}

bool PendantUi::load_calibration() {
    _calibration_loaded = false;
    _touch_map = { 1, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    Preferences preferences;
    if (!preferences.begin("jctouch", false)) {
        Serial.println("cal: NVS open failed; using factory mapping.");
        return false;
    }

    if (!preferences.isKey("map")) {
        preferences.end();
        Serial.println("cal: no saved mapping; using factory mapping.");
        return false;
    }
    const size_t stored_size = preferences.getBytesLength("map");
    TouchMap stored {};
    const size_t read_size = preferences.getBytes("map", &stored, sizeof(stored));
    preferences.end();
    if (stored_size != sizeof(stored) || read_size != sizeof(stored)
        || !valid_touch_map(stored)) {
        Serial.println("cal: saved mapping invalid or unsupported; using factory mapping.");
        return false;
    }
    _touch_map = stored;
    _calibration_loaded = true;
    Serial.println("cal: version 1 mapping loaded from NVS.");
    return true;
}

bool PendantUi::save_calibration() {
    if (!_calibration_pass || !valid_touch_map(_calibration_result.map)) {
        Serial.println("cal: save rejected; calibration did not pass validation.");
        return false;
    }

    Preferences preferences;
    if (!preferences.begin("jctouch", false)) {
        Serial.println("cal: NVS open failed; previous mapping retained.");
        return false;
    }
    const size_t written = preferences.putBytes("map", &_calibration_result.map,
                                                sizeof(_calibration_result.map));
    preferences.end();
    if (written != sizeof(_calibration_result.map)) {
        Serial.println("cal: NVS write failed; previous active mapping retained.");
        return false;
    }

    _touch_map = _calibration_result.map;
    _calibration_loaded = true;
    Serial.printf("cal: version=1 saved; map=[%.6f %.6f %.3f; %.6f %.6f %.3f]\n",
                  _touch_map.x_from_x, _touch_map.x_from_y, _touch_map.x_offset,
                  _touch_map.y_from_x, _touch_map.y_from_y, _touch_map.y_offset);
    return true;
}

void PendantUi::reset_calibration() {
    Preferences preferences;
    if (!preferences.begin("jctouch", false)) {
        Serial.println("cal: NVS open failed; saved mapping was not changed.");
        return;
    }
    const bool removed = !preferences.isKey("map") || preferences.remove("map");
    preferences.end();
    if (!removed) {
        Serial.println("cal: NVS remove failed; saved mapping was not changed.");
        return;
    }

    _touch_map = { 1, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    _calibration_loaded = false;
    Serial.println("cal: factory mapping restored and saved mapping removed.");
    _page = Page::Settings;
    _calibration_phase = CalibrationPhase::Ready;
    draw();
}

void PendantUi::begin_calibration() {
    _page = Page::TouchCalibration;
    _calibration_phase = CalibrationPhase::Ready;
    _calibration_target = 0;
    _calibration_sample_count = 0;
    _calibration_touch_active = false;
    _calibration_touch_invalid = false;
    _calibration_retry_message = false;
    _calibration_pass = false;
    _calibration_result = {};
    memset(_calibration_raw_points, 0, sizeof(_calibration_raw_points));
    memset(_calibration_driver_points, 0, sizeof(_calibration_driver_points));
    Serial.println("cal: started; order=top-left, top-right, bottom-right, bottom-left, centre.");
    draw();
}

void PendantUi::begin_calibration_touch(uint8_t count, const TP_Point& point) {
    _calibration_sample_count = 0;
    _calibration_touch_invalid = count != 1;
    _calibration_touch_active = true;
    _calibration_phase = CalibrationPhase::Capturing;
    _calibration_retry_message = false;
    if (count != 1) {
        Serial.printf("cal: rejected touch-down with count=%u\n", count);
    }
    collect_calibration_sample(count, point);
}

void PendantUi::collect_calibration_sample(uint8_t count, const TP_Point& point) {
    if (count != 1) {
        _calibration_touch_invalid = true;
        Serial.printf("cal: multi-touch during point %u; rejecting attempt.\n",
                      static_cast<unsigned>(_calibration_target + 1));
        return;
    }
    if (_calibration_sample_count >= kCalibrationSampleCount) {
        return;
    }

    const uint8_t sample = _calibration_sample_count++;
    _raw_samples[sample] = { static_cast<float>(point.rawX),
                             static_cast<float>(point.rawY) };
    _driver_samples[sample] = { static_cast<float>(point.x),
                                static_cast<float>(point.y) };
    uint16_t mapped_x;
    uint16_t mapped_y;
    map_touch(_touch_map, point.x, point.y, mapped_x, mapped_y);
    log_calibration_sample(count, point, mapped_x, mapped_y);
    draw_calibration_progress();
}

void PendantUi::finish_calibration_touch() {
    _calibration_touch_active = false;
    const bool enough_samples = _calibration_sample_count == kCalibrationSampleCount;
    if (_calibration_touch_invalid || !enough_samples) {
        _calibration_phase = CalibrationPhase::Ready;
        _calibration_retry_message = true;
        Serial.printf("cal: target=%u rejected; samples=%u/%u multi_touch=%s. Retouch and release.\n",
                      static_cast<unsigned>(_calibration_target + 1),
                      _calibration_sample_count, kCalibrationSampleCount,
                      _calibration_touch_invalid ? "yes" : "no");
        draw_calibration_target();
        return;
    }

    const float raw_x = median_value(_raw_samples, false);
    const float raw_y = median_value(_raw_samples, true);
    const float driver_x = median_value(_driver_samples, false);
    const float driver_y = median_value(_driver_samples, true);
    for (uint8_t i = 0; i < kCalibrationSampleCount; ++i) {
        if (fabsf(_raw_samples[i].x - raw_x) > 20.0f
            || fabsf(_raw_samples[i].y - raw_y) > 20.0f
            || fabsf(_driver_samples[i].x - driver_x) > 20.0f
            || fabsf(_driver_samples[i].y - driver_y) > 20.0f) {
            _calibration_phase = CalibrationPhase::Ready;
            _calibration_retry_message = true;
            Serial.printf("cal: target=%u rejected; unstable samples exceed 20 px from median.\n",
                          static_cast<unsigned>(_calibration_target + 1));
            draw_calibration_target();
            return;
        }
    }

    const TouchSample raw_point { raw_x, raw_y };
    const TouchSample driver_point { driver_x, driver_y };
    if (_calibration_target < 4) {
        _calibration_raw_points[_calibration_target] = raw_point;
        _calibration_driver_points[_calibration_target] = driver_point;
    } else {
        _calibration_raw_points[4] = raw_point;
        _calibration_driver_points[4] = driver_point;
    }
    uint16_t mapped_x;
    uint16_t mapped_y;
    map_touch(_touch_map, static_cast<uint16_t>(driver_x),
              static_cast<uint16_t>(driver_y), mapped_x, mapped_y);
    Serial.printf(
        "cal: target=%u accepted median raw=(%.1f,%.1f) driver=(%.1f,%.1f) current_ui=(%u,%u) target_ui=(%u,%u)\n",
        static_cast<unsigned>(_calibration_target + 1), raw_x, raw_y,
        driver_x, driver_y, mapped_x, mapped_y,
        kCalibrationTargetX[_calibration_target],
        kCalibrationTargetY[_calibration_target]);

    _calibration_retry_message = false;
    if (++_calibration_target == kCalibrationTargetCount) {
        evaluate_calibration();
    } else {
        _calibration_sample_count = 0;
        _calibration_phase = CalibrationPhase::Ready;
        draw_calibration_target();
    }
}

void PendantUi::evaluate_calibration() {
    _calibration_result = {};
    _calibration_result.maximum_corner_error = 999.0f;
    _calibration_result.average_corner_error = 999.0f;
    _calibration_result.centre_error = 999.0f;
    _calibration_pass = derive_touch_map(_calibration_driver_points,
                                         _calibration_driver_points[4],
                                         _calibration_result);
    _calibration_phase = CalibrationPhase::Result;
    Serial.printf("cal: fit=%s max_corner_error=%.2f average_corner_error=%.2f centre_error=%.2f swap=%s x_inverted=%s y_inverted=%s\n",
                  _calibration_pass ? "PASS" : "RETRY",
                  _calibration_result.maximum_corner_error,
                  _calibration_result.average_corner_error,
                  _calibration_result.centre_error,
                  _calibration_result.axis_swapped ? "yes" : "no",
                  _calibration_result.x_inverted ? "yes" : "no",
                  _calibration_result.y_inverted ? "yes" : "no");
    Serial.printf("cal: candidate_map=[%.6f %.6f %.3f; %.6f %.6f %.3f]; not active until explicitly saved.\n",
                  _calibration_result.map.x_from_x,
                  _calibration_result.map.x_from_y,
                  _calibration_result.map.x_offset,
                  _calibration_result.map.y_from_x,
                  _calibration_result.map.y_from_y,
                  _calibration_result.map.y_offset);
    for (uint8_t i = 0; i < kCalibrationTargetCount; ++i) {
        const float candidate_x = _calibration_result.map.x_from_x
                                * _calibration_driver_points[i].x
                                + _calibration_result.map.x_from_y
                                * _calibration_driver_points[i].y
                                + _calibration_result.map.x_offset;
        const float candidate_y = _calibration_result.map.y_from_x
                                * _calibration_driver_points[i].x
                                + _calibration_result.map.y_from_y
                                * _calibration_driver_points[i].y
                                + _calibration_result.map.y_offset;
        Serial.printf("cal: point=%u raw=(%.1f,%.1f) driver=(%.1f,%.1f) candidate_ui=(%.1f,%.1f) target_ui=(%u,%u)\n",
                      static_cast<unsigned>(i + 1),
                      _calibration_raw_points[i].x, _calibration_raw_points[i].y,
                      _calibration_driver_points[i].x,
                      _calibration_driver_points[i].y,
                      candidate_x, candidate_y,
                      kCalibrationTargetX[i], kCalibrationTargetY[i]);
    }
    draw_calibration_result();
}

void PendantUi::draw_calibration_progress() {
    char progress[40];
    snprintf(progress, sizeof(progress), "Sampling %u / 9 - keep touching",
             static_cast<unsigned>(_calibration_sample_count));
    _display.fillRect(120, 350, 560, 32, kBackground);
    draw_text_centered(_display, { 120, 350, 560, 32 }, progress,
                       FontId::Body16, kInfo, kBackground);
}

void PendantUi::draw_connections() {
    _display.fillScreen(kBackground);
    draw_header("CONNECTION");
    draw_button(kHomeButton, "HOME");

    for (uint8_t i = 0; i < kTransportCount; ++i) {
        draw_transport_card(i);
    }

    _display.fillRect(16, 290, 768, 152, kAccentDark);
    _display.drawRect(16, 290, 768, 152, RED);
    draw_text_centered(_display, { 34, 300, 732, 34 },
                       "READ-ONLY STATUS CONNECTION",
                       FontId::Button20, WHITE, kAccentDark);
    draw_text_centered(_display, { 34, 340, 732, 26 },
                       "Wi-Fi opens network and FluidNC settings.",
                       FontId::Body16, kMuted, kAccentDark);
    draw_text_centered(_display, { 34, 368, 732, 26 },
                       "No G-code or motion commands are sent.",
                       FontId::Body16, kMuted, kAccentDark);
    draw_text_centered(_display, { 34, 394, 732, 26 },
                       "UART and Bluetooth are not implemented.",
                       FontId::Body16, kMuted, kAccentDark);
    draw_transport_status();
}

void PendantUi::draw_wifi_settings() {
    _display.fillScreen(kBackground);
    draw_header("WI-FI / FLUIDNC");
    draw_button(kHomeButton, "HOME");

    for (uint8_t i = 0; i < 3; ++i) {
        draw_wifi_field(i);
    }
    draw_button({ 20, 204, 154, 42 },
                _fluidnc.wifi_scan_running() ? "SCANNING..." : "SCAN WI-FI");
    draw_wifi_diagnostics();

    if (_show_scan_results) {
        draw_wifi_scan_results();
    } else if (_keyboard_visible) {
        draw_keyboard();
    } else {
        draw_text_centered(_display, { 90, 320, 620, 42 },
                           "Keyboard hidden - tap an SSID, password or host field.",
                           FontId::Body16, kMuted, kBackground);
    }
    draw_button({ 18, 449, 172, 27 }, "SAVE");
    draw_button({ 200, 449, 200, 27 },
                _fluidnc.connect_requested() ? "DISCONNECT" : "CONNECT");
    draw_button({ 410, 449, 180, 27 }, _live_mode ? "LIVE ACTIVE" : "USE LIVE",
                _live_mode, !_live_mode && !_fluidnc.status_is_fresh());
    draw_button({ 600, 449, 180, 27 }, "BACK");
    _last_displayed_scan_count = _fluidnc.scanned_network_count();
    _last_displayed_scan_running = _fluidnc.wifi_scan_running();
}

void PendantUi::draw_wifi_field(uint8_t index) {
    static constexpr Rect fields[] = {
        { 20, 72, 760, 40 },
        { 20, 116, 760, 40 },
        { 20, 160, 760, 40 },
    };
    static constexpr const char* labels[] = {
        "SSID", "PASSWORD", "FLUIDNC HOST / IP"
    };
    if (index >= 3) {
        return;
    }

    const char* values[] = {
        _edit_wifi_config.ssid,
        _edit_wifi_config.password,
        _edit_wifi_config.host,
    };
    const bool selected = static_cast<uint8_t>(_wifi_field) == index;
    const uint16_t fill = selected ? kPanelLight : kPanel;
    _display.fillRect(fields[index].x, fields[index].y,
                      fields[index].width, fields[index].height, fill);
    _display.drawRect(fields[index].x, fields[index].y,
                      fields[index].width, fields[index].height,
                      selected ? CYAN : kBorder);
    draw_text_top(_display, fields[index].x + 10, fields[index].y + 2,
                  labels[index], FontId::Body16, kMuted, fill);
    char masked[64];
    char display_value[48];
    const char* shown = values[index];
    if (index == static_cast<uint8_t>(WifiField::Password)) {
        const size_t count = strlen(values[index]);
        const size_t masked_count = count < sizeof(masked) - 1
            ? count : sizeof(masked) - 1;
        memset(masked, '*', masked_count);
        masked[masked_count] = '\0';
        shown = masked;
    }
    const size_t shown_length = strlen(shown);
    if (shown_length >= sizeof(display_value)) {
        const size_t suffix_length = sizeof(display_value) - 4;
        memcpy(display_value, "...", 3);
        memcpy(display_value + 3, shown + shown_length - suffix_length,
               suffix_length);
        display_value[sizeof(display_value) - 1] = '\0';
        shown = display_value;
    }
    draw_text_top(_display, fields[index].x + 245, fields[index].y + 2,
                  shown[0] ? shown : "(tap to edit)",
                  FontId::Button20, WHITE, fill);
}

void PendantUi::draw_wifi_diagnostics() {
    _display.fillRect(186, 200, 594, 54, kBackground);
    char status_line[96];
    snprintf(status_line, sizeof(status_line), "Wi-Fi: %s  |  FluidNC: %s",
             _fluidnc.wifi_connected() ? "CONNECTED" : "DISCONNECTED",
             JcReadOnlyFluidNc::state_name(_fluidnc.state()));
    draw_text_top(_display, 190, 202, status_line, FontId::Body16, kInfo, kBackground);

    const uint32_t age = _fluidnc.last_status_age_ms();
    char diagnostic_line[112];
    if (age == UINT32_MAX) {
        snprintf(diagnostic_line, sizeof(diagnostic_line),
                 "Status: none  |  attempts: %lu  |  errors: %lu",
                 static_cast<unsigned long>(_fluidnc.reconnect_attempts()),
                 static_cast<unsigned long>(_fluidnc.parse_errors()));
    } else {
        snprintf(diagnostic_line, sizeof(diagnostic_line),
                 "Age: %lu ms  |  interval: %lu ms  |  attempts: %lu  |  errors: %lu",
                 static_cast<unsigned long>(age),
                 static_cast<unsigned long>(_fluidnc.status().update_interval_ms),
                 static_cast<unsigned long>(_fluidnc.reconnect_attempts()),
                 static_cast<unsigned long>(_fluidnc.parse_errors()));
    }
    draw_text_top(_display, 190, 222, diagnostic_line, FontId::Body16,
                  kMuted, kBackground);
    const char* message = _fluidnc.last_error()[0]
        ? _fluidnc.last_error()
        : (_wifi_notice != nullptr
            ? _wifi_notice
            : "NVS is unencrypted; connect only on a trusted LAN.");
    draw_text_top(_display, 190, 242, message, FontId::Body16,
                  _fluidnc.last_error()[0] ? RED
                    : (_wifi_notice != nullptr ? RED : kMuted), kBackground);
}

void PendantUi::draw_wifi_scan_results() {
    _display.fillRect(18, 258, 764, 150, kBackground);
    if (_fluidnc.wifi_scan_running()) {
        draw_text_top(_display, 28, 264, "Scanning nearby access points...",
                      FontId::Button20, kInfo, kBackground);
        return;
    }

    const uint8_t count = _fluidnc.scanned_network_count();
    if (count == 0) {
        draw_text_top(_display, 28, 264, "No networks found. Tap SCAN WI-FI to retry.",
                      FontId::Body16, kMuted, kBackground);
        return;
    }

    draw_text_top(_display, 28, 260,
                  "Select network by SSID and BSSID (access-point MAC):",
                  FontId::Body16, kMuted, kBackground);
    constexpr uint8_t kVisibleNetworks = 4;
    const uint8_t first = static_cast<uint8_t>(_scan_page * kVisibleNetworks);
    for (uint8_t row = 0; row < kVisibleNetworks; ++row) {
        const uint8_t index = static_cast<uint8_t>(first + row);
        const JcWifiNetwork* network = _fluidnc.scanned_network(index);
        if (network == nullptr) {
            break;
        }
        char label[96];
        snprintf(label, sizeof(label),
                 "%s  %d dBm  Ch %u  %02X:%02X:%02X:%02X:%02X:%02X",
                 network->ssid, network->rssi, network->channel,
                 network->bssid[0], network->bssid[1], network->bssid[2],
                 network->bssid[3], network->bssid[4], network->bssid[5]);
        draw_button({ 18, static_cast<int16_t>(282 + row * 32), 764, 30 },
                    label, _edit_wifi_config.bssid_pinned
                        && memcmp(_edit_wifi_config.bssid, network->bssid, 6) == 0);
    }
    draw_button({ 18, 416, 128, 27 }, "PREVIOUS",
                _scan_page > 0);
    draw_button({ 152, 416, 128, 27 }, "NEXT",
                first + kVisibleNetworks < count);
    draw_button({ 286, 416, 180, 27 }, "TYPE SSID");
}

void PendantUi::draw_keyboard(bool draw_panel) {
    static constexpr const char* alpha_rows[] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.-_"
    };
    static constexpr const char* symbol_rows[] = {
        "1234567890", "!@#$%^&*()", "-_=+[]{};:", ",.?/'\"\\|~"
    };

    if (draw_panel) {
        _display.fillRect(14, 254, 772, 166, kPanel);
        _display.drawRect(14, 254, 772, 166, kBorder);
        draw_text_top(_display, 24, 254, "TOUCH KEYBOARD",
                      FontId::Body16, CYAN, kPanel);
    }
    for (uint8_t row = 0; row < 4; ++row) {
        const char* keys = _keyboard_symbols ? symbol_rows[row] : alpha_rows[row];
        const size_t count = strlen(keys);
        const int16_t cell_width = 76;
        const int16_t start_x = static_cast<int16_t>(
            18 + ((10 - static_cast<int16_t>(count)) * cell_width) / 2);
        const int16_t y = static_cast<int16_t>(270 + row * 38);
        for (size_t column = 0; column < count; ++column) {
            char label[2] = { keys[column], '\0' };
            if (!_keyboard_symbols && _keyboard_shift && row > 0) {
                if (label[0] >= 'a' && label[0] <= 'z') {
                    label[0] = static_cast<char>(label[0] - 'a' + 'A');
                }
            }
            const Rect key_bounds {
                static_cast<int16_t>(start_x + column * cell_width), y, 68, 34
            };
            const uint16_t key_fill = 0x04B2;
            _display.fillRect(key_bounds.x, key_bounds.y,
                              key_bounds.width, key_bounds.height, key_fill);
            _display.drawRect(key_bounds.x, key_bounds.y,
                              key_bounds.width, key_bounds.height, CYAN);
            draw_text_centered(_display, key_bounds, label,
                               FontId::Button20, WHITE, key_fill);
        }
    }
    if (draw_panel) {
        draw_button({ 18, 416, 116, 27 }, _keyboard_symbols ? "ABC" : "123");
        draw_button({ 140, 416, 100, 27 }, "SHIFT");
        draw_button({ 246, 416, 100, 27 }, "SPACE");
        draw_button({ 352, 416, 100, 27 }, "BACK");
        draw_button({ 458, 416, 100, 27 }, "CLEAR");
        draw_button({ 564, 416, 216, 27 }, "DONE");
    }
}

void PendantUi::draw_live_status_updates() {
    const uint32_t now = millis();
    if (_page == Page::WifiSettings && now - _last_wifi_refresh_ms >= 500) {
        _last_wifi_refresh_ms = now;
        draw_wifi_diagnostics();
        const uint8_t scan_count = _fluidnc.scanned_network_count();
        const bool scan_running = _fluidnc.wifi_scan_running();
        if (scan_count != _last_displayed_scan_count
            || scan_running != _last_displayed_scan_running) {
            draw_button({ 20, 204, 154, 42 },
                        scan_running ? "SCANNING..." : "SCAN WI-FI");
            if (_show_scan_results) {
                draw_wifi_scan_results();
            }
            _last_displayed_scan_count = scan_count;
            _last_displayed_scan_running = scan_running;
        }
    }
    if (_page != Page::Home) {
        return;
    }

    const JcFluidState link_state = live_state();
    const bool link_changed = _last_displayed_link_state
        != static_cast<uint32_t>(link_state);
    if (link_changed) {
        draw_header("FLUID DIAL");
        draw_button(kSettingsButton, "MENU");
        draw_button(kConnectionsButton, "LINK");
        draw_machine_status();
        _last_displayed_link_state = static_cast<uint32_t>(link_state);
        if (_live_mode) {
            for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
                draw_position(axis);
            }
        }
    }

    if (!_live_mode) {
        return;
    }
    const JcLiveMachineStatus& status = live_status();
    if (_last_displayed_status_sequence != status.sequence) {
        draw_live_info(false);
        bool position_changed = !_has_displayed_live_axes;
        for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
            if (!_has_displayed_live_axes
                || _last_displayed_live_axes[axis] != status.axes[axis]) {
                draw_position(axis);
                _last_displayed_live_axes[axis] = status.axes[axis];
                position_changed = true;
            }
        }
        const bool state_changed = strcmp(_last_displayed_machine_state,
                                          status.machine_state) != 0
            || _last_displayed_reference != status.coordinate_reference;
        if (state_changed || position_changed) {
            draw_machine_status();
        }
        strlcpy(_last_displayed_machine_state, status.machine_state,
                sizeof(_last_displayed_machine_state));
        _last_displayed_reference = status.coordinate_reference;
        _last_displayed_status_sequence = status.sequence;
        _has_displayed_live_axes = status.has_data;
    }
    if (millis() - _last_live_info_ms >= 500) {
        _last_live_info_ms = millis();
        draw_live_info(false);  // refreshes stale colouring and the transport label
    }
}

void PendantUi::append_keyboard_character(char character) {
    char* value = nullptr;
    size_t capacity = 0;
    switch (_wifi_field) {
        case WifiField::Ssid:
            value = _edit_wifi_config.ssid;
            capacity = sizeof(_edit_wifi_config.ssid);
            break;
        case WifiField::Password:
            value = _edit_wifi_config.password;
            capacity = sizeof(_edit_wifi_config.password);
            break;
        case WifiField::Host:
            value = _edit_wifi_config.host;
            capacity = sizeof(_edit_wifi_config.host);
            break;
    }
    if (value == nullptr || capacity == 0) {
        return;
    }
    if (_wifi_field == WifiField::Host
        && !((character >= 'a' && character <= 'z')
             || (character >= 'A' && character <= 'Z')
             || (character >= '0' && character <= '9')
             || character == '.' || character == '-')) {
        return;
    }
    const size_t length = strlen(value);
    if (length + 1 < capacity) {
        value[length] = character;
        value[length + 1] = '\0';
        if (_wifi_field == WifiField::Ssid) {
            _edit_wifi_config.bssid_pinned = false;
            memset(_edit_wifi_config.bssid, 0, sizeof(_edit_wifi_config.bssid));
            _edit_wifi_config.channel = 0;
            _wifi_notice = nullptr;
        }
        draw_wifi_field(static_cast<uint8_t>(_wifi_field));
    }
}

void PendantUi::erase_keyboard_character() {
    char* value = nullptr;
    switch (_wifi_field) {
        case WifiField::Ssid: value = _edit_wifi_config.ssid; break;
        case WifiField::Password: value = _edit_wifi_config.password; break;
        case WifiField::Host: value = _edit_wifi_config.host; break;
    }
    if (value == nullptr) {
        return;
    }
    const size_t length = strlen(value);
    if (length > 0) {
        value[length - 1] = '\0';
        if (_wifi_field == WifiField::Ssid) {
            _edit_wifi_config.bssid_pinned = false;
            memset(_edit_wifi_config.bssid, 0, sizeof(_edit_wifi_config.bssid));
            _edit_wifi_config.channel = 0;
            _wifi_notice = nullptr;
        }
        draw_wifi_field(static_cast<uint8_t>(_wifi_field));
    }
}

void PendantUi::set_live_mode(bool live) {
    if (live == _live_mode) {
        return;
    }
    if (live && !live_fresh()) {
        Serial.println("live: activation denied; no fresh valid FluidNC status");
        _last_action = "LIVE unavailable - connect and receive valid 4-axis status";
        _wifi_notice = "LIVE unavailable - connect and receive valid 4-axis status.";
        if (_page == Page::Home) {
            draw_home_footer();
        } else if (_page == Page::WifiSettings) {
            draw_wifi_diagnostics();
        }
        return;
    }
    _live_mode = live;
    _wifi_notice = nullptr;
    _has_displayed_live_axes = false;
    _last_displayed_status_sequence = 0;
    strlcpy(_last_displayed_machine_state, "", sizeof(_last_displayed_machine_state));
    _last_displayed_reference = JcCoordinateReference::Unknown;
    _last_action = live ? "LIVE read-only - machine motion disabled"
                        : "DEMO mode - simulated coordinates";
    Serial.printf("ui: mode=%s\n", live ? "LIVE read-only" : "DEMO");
    if (_page == Page::Home) {
        redraw();
    } else if (_page == Page::WifiSettings) {
        draw_header("WI-FI / FLUIDNC");
        draw_button(kHomeButton, "HOME");
        draw_button({ 410, 449, 180, 27 }, _live_mode ? "LIVE ACTIVE" : "USE LIVE",
                    _live_mode, !_live_mode && !_fluidnc.status_is_fresh());
        draw_wifi_diagnostics();
    }
}

void PendantUi::handle_wifi_touch(uint16_t x, uint16_t y) {
    if (contains(kHomeButton, x, y)) {
        _page = Page::Home;
        redraw();
        return;
    }

    static constexpr Rect fields[] = {
        { 20, 72, 760, 40 },
        { 20, 116, 760, 40 },
        { 20, 160, 760, 40 },
    };
    for (uint8_t i = 0; i < 3; ++i) {
        if (contains(fields[i], x, y)) {
            _wifi_field = static_cast<WifiField>(i);
            _keyboard_visible = true;
            _show_scan_results = false;
            Serial.printf("ui: Wi-Fi editor opened for %s; values are not logged\n",
                          i == 0 ? "SSID" : (i == 1 ? "password" : "FluidNC host"));
            draw_wifi_field(i);
            draw_keyboard();
            return;
        }
    }

    if (contains({ 20, 204, 154, 42 }, x, y)) {
        _keyboard_visible = false;
        _show_scan_results = true;
        _scan_page = 0;
        _wifi_notice = nullptr;
        if (!_fluidnc.start_wifi_scan()) {
            Serial.println("net: Wi-Fi scan request rejected");
        }
        draw_button({ 20, 204, 154, 42 },
                    _fluidnc.wifi_scan_running() ? "SCANNING..." : "SCAN WI-FI");
        draw_wifi_scan_results();
        return;
    }

    if (contains({ 18, 449, 172, 27 }, x, y)) {
        if (_fluidnc.save_config(_edit_wifi_config)) {
            _edit_wifi_config = _fluidnc.config();
            _wifi_notice = _edit_wifi_config.bssid_pinned
                ? "Saved; connection will use the selected access-point BSSID."
                : "Local settings saved; NVS is not encrypted.";
            Serial.println("net: configuration saved; credentials were not logged");
            for (uint8_t i = 0; i < 3; ++i) {
                draw_wifi_field(i);
            }
        } else {
            _wifi_notice = "Could not save Wi-Fi settings; previous values retained.";
        }
        draw_wifi_diagnostics();
        return;
    }
    if (contains({ 200, 449, 200, 27 }, x, y)) {
        if (_fluidnc.connect_requested()) {
            _fluidnc.disconnect();
        } else if (_fluidnc.save_config(_edit_wifi_config)) {
            _edit_wifi_config = _fluidnc.config();
            _fluidnc.connect();
        } else {
            _wifi_notice = "Could not save Wi-Fi settings; connection not started.";
        }
        draw_button({ 200, 449, 200, 27 },
                    _fluidnc.connect_requested() ? "DISCONNECT" : "CONNECT");
        draw_wifi_diagnostics();
        return;
    }
    if (contains({ 410, 449, 180, 27 }, x, y)) {
        set_live_mode(!_live_mode);
        return;
    }
    if (contains({ 600, 449, 180, 27 }, x, y)) {
        _page = Page::Connections;
        redraw();
        return;
    }

    if (_show_scan_results) {
        constexpr uint8_t kVisibleNetworks = 4;
        const uint8_t first = static_cast<uint8_t>(_scan_page * kVisibleNetworks);
        for (uint8_t row = 0; row < kVisibleNetworks; ++row) {
            const uint8_t index = static_cast<uint8_t>(first + row);
            const Rect row_bounds { 18, static_cast<int16_t>(282 + row * 32),
                                    764, 30 };
            const JcWifiNetwork* network = _fluidnc.scanned_network(index);
            if (network == nullptr) {
                break;
            }
            if (contains(row_bounds, x, y)) {
                strlcpy(_edit_wifi_config.ssid, network->ssid,
                        sizeof(_edit_wifi_config.ssid));
                memcpy(_edit_wifi_config.bssid, network->bssid,
                       sizeof(_edit_wifi_config.bssid));
                _edit_wifi_config.channel = network->channel;
                _edit_wifi_config.bssid_pinned = true;
                _wifi_field = WifiField::Password;
                _keyboard_visible = true;
                _show_scan_results = false;
                char notice[64];
                snprintf(notice, sizeof(notice),
                         "Selected AP BSSID %02X:%02X:%02X:%02X:%02X:%02X.",
                         network->bssid[0], network->bssid[1], network->bssid[2],
                         network->bssid[3], network->bssid[4], network->bssid[5]);
                static char selected_ap_notice[64];
                strlcpy(selected_ap_notice, notice, sizeof(selected_ap_notice));
                _wifi_notice = selected_ap_notice;
                Serial.println("net: scanned access point selected; SSID and credentials not logged");
                draw_wifi_field(static_cast<uint8_t>(_wifi_field));
                draw_keyboard();
                return;
            }
        }
        const uint8_t count = _fluidnc.scanned_network_count();
        if (contains({ 18, 416, 128, 27 }, x, y) && _scan_page > 0) {
            --_scan_page;
            draw_wifi_scan_results();
            return;
        }
        if (contains({ 152, 416, 128, 27 }, x, y)
            && first + kVisibleNetworks < count) {
            ++_scan_page;
            draw_wifi_scan_results();
            return;
        }
        if (contains({ 286, 416, 180, 27 }, x, y)) {
            _show_scan_results = false;
            _keyboard_visible = true;
            _wifi_field = WifiField::Ssid;
            _wifi_notice = nullptr;
            redraw();
            return;
        }
        return;
    }

    if (!_keyboard_visible) {
        return;
    }

    if (contains({ 18, 416, 116, 27 }, x, y)) {
        _keyboard_symbols = !_keyboard_symbols;
        draw_keyboard(false);
        return;
    }
    if (contains({ 140, 416, 100, 27 }, x, y)) {
        _keyboard_shift = !_keyboard_shift;
        draw_keyboard(false);
        return;
    }
    if (contains({ 246, 416, 100, 27 }, x, y)) {
        append_keyboard_character(' ');
        return;
    }
    if (contains({ 352, 416, 100, 27 }, x, y)) {
        erase_keyboard_character();
        return;
    }
    if (contains({ 458, 416, 100, 27 }, x, y)) {
        switch (_wifi_field) {
            case WifiField::Ssid:
                _edit_wifi_config.ssid[0] = '\0';
                _edit_wifi_config.bssid_pinned = false;
                memset(_edit_wifi_config.bssid, 0, sizeof(_edit_wifi_config.bssid));
                _edit_wifi_config.channel = 0;
                _wifi_notice = nullptr;
                break;
            case WifiField::Password: _edit_wifi_config.password[0] = '\0'; break;
            case WifiField::Host: _edit_wifi_config.host[0] = '\0'; break;
        }
        draw_wifi_field(static_cast<uint8_t>(_wifi_field));
        return;
    }
    if (contains({ 564, 416, 216, 27 }, x, y)) {
        _keyboard_visible = false;
        _display.fillRect(14, 254, 772, 189, kBackground);
        draw_text_centered(_display, { 90, 320, 620, 42 },
                           "Keyboard hidden - tap an SSID, password or host field.",
                           FontId::Body16, kMuted, kBackground);
        return;
    }

    const int16_t row_y[] = { 270, 308, 346, 384 };
    static constexpr const char* alpha_rows[] = {
        "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm.-_"
    };
    static constexpr const char* symbol_rows[] = {
        "1234567890", "!@#$%^&*()", "-_=+[]{};:", ",.?/'\"\\|~"
    };
    for (uint8_t row = 0; row < 4; ++row) {
        const Rect row_bounds { 18, row_y[row], 764, 34 };
        if (!contains(row_bounds, x, y)) {
            continue;
        }
        const char* keys = _keyboard_symbols ? symbol_rows[row] : alpha_rows[row];
        const size_t count = strlen(keys);
        const int16_t cell_width = 76;
        const int16_t start_x = static_cast<int16_t>(
            18 + ((10 - static_cast<int16_t>(count)) * cell_width) / 2);
        const size_t column = static_cast<size_t>((x - start_x) / cell_width);
        if (column >= count) {
            return;
        }
        char character = keys[column];
        if (!_keyboard_symbols && _keyboard_shift
            && character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        append_keyboard_character(character);
        return;
    }
}

void PendantUi::set_feedback(Feedback feedback, const char* message) {
    const Feedback previous = _feedback;
    _feedback = feedback;
    _feedback_until = millis() + 240;
    _last_action = message;
    if (previous != Feedback::None && previous != feedback) {
        draw_jog_button(previous);
    }
    draw_jog_button(feedback);
    draw_home_footer();
}

void PendantUi::redraw() {
    draw();
}

void PendantUi::handle_touch(uint16_t x, uint16_t y) {
    if (_page == Page::App) {
        _app.touch(x, y);
        return;
    }
    if (_page == Page::Home) {
        if (contains({ 212, 13, 144, 38 }, x, y)) {
            set_live_mode(!_live_mode);
            return;
        }
        if (contains(kSettingsButton, x, y)) {
            _page = Page::App;
            _app.open();
            Serial.println("ui: page=application menu");
            return;
        }
        if (_live_mode && contains({ 600, 190, 176, 38 }, x, y)) {
            _display_reference = _display_reference == JcRef::Work ? JcRef::Machine : JcRef::Work;
            draw_machine_status();
            for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
                draw_position(axis);
            }
            return;
        }
        if (contains(kConnectionsButton, x, y)) {
            _page = Page::Connections;
            _last_action = "Transport selection opened";
            redraw();
            Serial.println("ui: page=connections (selection only)");
            return;
        }

        for (uint8_t axis = 0; axis < kAxisCount; ++axis) {
            const Rect tile { static_cast<int16_t>(16 + axis * 195), 76, 183, 101 };
            const Rect selector { static_cast<int16_t>(88 + axis * 82), 276, 70, 38 };
            if (!_live_mode && (contains(tile, x, y) || contains(selector, x, y))) {
                const uint8_t previous_axis = _selected_axis;
                _selected_axis = axis;
                _last_action = "Axis selected";
                draw_axis_selection(previous_axis);
                draw_axis_selection(_selected_axis);
                draw_axis_selector(previous_axis);
                draw_axis_selector(_selected_axis);
                for (uint8_t step = 0; step < kStepCount; ++step) {
                    draw_step_selector(step);
                }
                draw_home_footer();
                Serial.printf("demo: selected axis %s\n", axis_name(axis));
                return;
            }
        }

        for (uint8_t step = 0; step < kStepCount; ++step) {
            const Rect selector { static_cast<int16_t>(88 + step * 90), 325, 82, 38 };
            if (!_live_mode && contains(selector, x, y)) {
                const uint8_t previous_step = _selected_step;
                _selected_step = step;
                _last_action = "Jog increment selected";
                draw_step_selector(previous_step);
                draw_step_selector(_selected_step);
                draw_home_footer();
                Serial.printf("demo: selected jog step %.1f %s\n", step_value(step, _selected_axis),
                              _selected_axis == 3 ? "deg" : "mm");
                return;
            }
        }

        if (contains({ 30, 376, 260, 74 }, x, y)) {
            if (_live_mode) {
                Serial.println("safety: ignored jog touch in read-only LIVE mode");
                return;
            }
            _positions[_selected_axis] -= step_value(_selected_step, _selected_axis);
            char message[64];
            snprintf(message, sizeof(message), "DEMO %s-  (local only)", axis_name(_selected_axis));
            set_feedback(Feedback::JogNegative, message);
            preview_jog(-step_value(_selected_step, _selected_axis));
            draw_position(_selected_axis);
            Serial.printf("demo: local %s- position=%.3f step=%.1f %s; no command sent\n",
                          axis_name(_selected_axis), _positions[_selected_axis],
                          step_value(_selected_step, _selected_axis), _selected_axis == 3 ? "deg" : "mm");
            return;
        }
        if (contains({ 300, 376, 200, 74 }, x, y)) {
            if (_live_mode) {
                Serial.println("safety: STOP/CANCEL has no command in read-only LIVE mode");
                return;
            }
            set_feedback(Feedback::Stop, "DEMO STOP/CANCEL - no motion active");
            Serial.println("demo: STOP/CANCEL; no controller or motion active");
            return;
        }
        if (contains({ 510, 376, 260, 74 }, x, y)) {
            if (_live_mode) {
                Serial.println("safety: ignored jog touch in read-only LIVE mode");
                return;
            }
            _positions[_selected_axis] += step_value(_selected_step, _selected_axis);
            char message[64];
            snprintf(message, sizeof(message), "DEMO %s+  (local only)", axis_name(_selected_axis));
            set_feedback(Feedback::JogPositive, message);
            preview_jog(step_value(_selected_step, _selected_axis));
            draw_position(_selected_axis);
            Serial.printf("demo: local %s+ position=%.3f step=%.1f %s; no command sent\n",
                          axis_name(_selected_axis), _positions[_selected_axis],
                          step_value(_selected_step, _selected_axis), _selected_axis == 3 ? "deg" : "mm");
        }
        return;
    }

    if (_page == Page::Settings) {
        if (contains(kHomeButton, x, y)) {
            _page = Page::Home;
            redraw();
            Serial.println("ui: page=home");
        } else if (contains({ 24, 416, 230, 44 }, x, y)) {
            begin_calibration();
        } else if (contains({ 280, 416, 230, 44 }, x, y)) {
            _page = Page::Connections;
            redraw();
            Serial.println("ui: page=connections (selection only)");
        } else if (contains({ 536, 416, 240, 44 }, x, y)) {
            _page = Page::TouchCalibration;
            _calibration_previous_phase = CalibrationPhase::Ready;
            _reset_return_to_settings = true;
            _calibration_phase = CalibrationPhase::ResetConfirm;
            redraw();
        }
        return;
    }

    if (_page == Page::Connections) {
        if (contains(kHomeButton, x, y)) {
            _page = Page::Home;
            redraw();
            Serial.println("ui: page=home");
            return;
        }

        static constexpr TransportKind transports[kTransportCount] = {
            TransportKind::WifiFluidNcTcp,
            TransportKind::WiredUart,
            TransportKind::BluetoothLe
        };
        for (uint8_t i = 0; i < kTransportCount; ++i) {
            const Rect bounds { static_cast<int16_t>(16 + i * 258), 112, 244, 154 };
            if (contains(bounds, x, y)) {
                uint8_t previous = 0;
                for (; previous < kTransportCount; ++previous) {
                    if (transports[previous] == _selected_transport) {
                        break;
                    }
                }
                _selected_transport = transports[i];
                draw_transport_card(previous);
                draw_transport_card(i);
                draw_transport_status();
                if (_selected_transport == TransportKind::WifiFluidNcTcp) {
                    _edit_wifi_config = _fluidnc.config();
                    _wifi_field = WifiField::Ssid;
                    _keyboard_symbols = false;
                    _keyboard_shift = false;
                    _keyboard_visible = true;
                    _page = Page::WifiSettings;
                    Serial.println("net: Wi-Fi configuration opened; connect remains manual");
                    redraw();
                } else {
                    _last_action = "Transport selection only; no connection started";
                    Serial.printf("demo: selected transport=%s; connection not started\n",
                                  transport_name(_selected_transport));
                }
                return;
            }
        }
        return;
    }

    if (_page == Page::WifiSettings) {
        handle_wifi_touch(x, y);
        return;
    }

    if (_page == Page::TouchCalibration) {
        if (_calibration_phase == CalibrationPhase::ResetConfirm) {
            if (contains({ 180, 285, 190, 48 }, x, y)) {
                reset_calibration();
            } else if (contains({ 430, 285, 190, 48 }, x, y)) {
                _page = _reset_return_to_settings
                    ? Page::Settings : Page::TouchCalibration;
                _calibration_phase = _calibration_previous_phase;
                _reset_return_to_settings = false;
                redraw();
                Serial.println("cal: factory reset cancelled; existing mapping retained.");
            }
            return;
        }

        if (_calibration_phase == CalibrationPhase::Result) {
            if (contains(kCalibrationSaveButton, x, y)) {
                if (_calibration_pass && save_calibration()) {
                    _page = Page::Settings;
                    _calibration_phase = CalibrationPhase::Ready;
                    Serial.println("cal: accepted mapping applied; returning to settings.");
                    redraw();
                } else {
                    Serial.println("cal: result cannot be saved; previous mapping remains active.");
                }
            } else if (contains(kCalibrationRetryButton, x, y)) {
                begin_calibration();
            } else if (contains(kCalibrationCancelButton, x, y)) {
                _page = Page::Settings;
                _calibration_phase = CalibrationPhase::Ready;
                Serial.println("cal: cancelled; previous mapping retained.");
                redraw();
            } else if (contains({ 40, 343, 220, 42 }, x, y)) {
                _calibration_previous_phase = CalibrationPhase::Result;
                _reset_return_to_settings = false;
                _calibration_phase = CalibrationPhase::ResetConfirm;
                redraw();
            }
            return;
        }

        if (contains(kCalibrationCancelTargetButton, x, y)) {
            _page = Page::Settings;
            _calibration_phase = CalibrationPhase::Ready;
            _calibration_touch_active = false;
            Serial.println("cal: cancelled; previous mapping retained.");
            redraw();
        } else if (contains(kCalibrationResetTargetButton, x, y)) {
            _calibration_previous_phase = _calibration_phase;
            _reset_return_to_settings = false;
            _calibration_phase = CalibrationPhase::ResetConfirm;
            redraw();
        }
    }
}

} // namespace jc8048w550
