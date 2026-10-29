// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#pragma once

#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>

#include "JcTouchCalibration.hpp"
#include "JcTransport.h"
#include "JcReadOnlyFluidNc.hpp"

namespace jc8048w550 {

class PendantUi {
public:
    struct Rect {
        int16_t x;
        int16_t y;
        int16_t width;
        int16_t height;
    };

    explicit PendantUi(Arduino_GFX& display);
    void begin();
    void poll(TAMC_GT911& touch);

private:
    enum class Page : uint8_t {
        Home,
        Settings,
        Connections,
        WifiSettings,
        TouchCalibration,
    };

    enum class WifiField : uint8_t {
        Ssid,
        Password,
        Host,
    };

    enum class CalibrationPhase : uint8_t {
        Ready,
        Capturing,
        Result,
        ResetConfirm,
    };

    enum class Feedback : uint8_t {
        None,
        JogNegative,
        Stop,
        JogPositive,
    };

    static constexpr uint16_t kWidth = 800;
    static constexpr uint16_t kHeight = 480;
    static constexpr uint8_t kAxisCount = 4;
    static constexpr uint8_t kTransportCount = 3;
    static constexpr uint8_t kStepCount = 3;
    static constexpr uint8_t kCalibrationTargetCount = 5;
    static constexpr uint8_t kCalibrationSampleCount = 9;
    static constexpr uint32_t kRecoveryHoldMs = 5000;
    static constexpr uint32_t kRecoveryWindowMs = 10000;

    Arduino_GFX& _display;
    Page _page = Page::Home;
    TransportKind _selected_transport = TransportKind::WifiFluidNcTcp;
    uint8_t _selected_axis = 0;
    uint8_t _selected_step = 1;
    float _positions[kAxisCount] = { 125.450f, -42.100f, 18.250f, 90.000f };
    bool _was_touched = false;
    uint16_t _last_raw_x = UINT16_MAX;
    uint16_t _last_raw_y = UINT16_MAX;
    uint16_t _last_driver_x = UINT16_MAX;
    uint16_t _last_driver_y = UINT16_MAX;
    uint32_t _last_touch_log_ms = 0;
    JcReadOnlyFluidNc _fluidnc;
    JcWifiConfig _edit_wifi_config {};
    WifiField _wifi_field = WifiField::Ssid;
    bool _live_mode = false;
    bool _keyboard_visible = true;
    bool _show_scan_results = false;
    const char* _wifi_notice = nullptr;
    bool _keyboard_symbols = false;
    bool _keyboard_shift = false;
    uint8_t _scan_page = 0;
    uint8_t _last_displayed_scan_count = UINT8_MAX;
    bool _last_displayed_scan_running = false;
    uint32_t _last_displayed_status_sequence = 0;
    uint32_t _last_displayed_link_state = UINT32_MAX;
    uint32_t _last_wifi_refresh_ms = 0;
    char _last_displayed_machine_state[20] {};
    JcCoordinateReference _last_displayed_reference = JcCoordinateReference::Unknown;
    float _last_displayed_live_axes[kAxisCount] {};
    bool _has_displayed_live_axes = false;
    TouchMap _touch_map { 1, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    CalibrationPhase _calibration_phase = CalibrationPhase::Ready;
    CalibrationPhase _calibration_previous_phase = CalibrationPhase::Ready;
    TouchCalibrationResult _calibration_result {};
    bool _calibration_pass = false;
    bool _calibration_retry_message = false;
    bool _calibration_loaded = false;
    bool _reset_return_to_settings = false;
    TouchSample _raw_samples[kCalibrationSampleCount] {};
    TouchSample _driver_samples[kCalibrationSampleCount] {};
    TouchSample _calibration_raw_points[kCalibrationTargetCount] {};
    TouchSample _calibration_driver_points[kCalibrationTargetCount] {};
    uint8_t _calibration_target = 0;
    uint8_t _calibration_sample_count = 0;
    bool _calibration_touch_active = false;
    bool _calibration_touch_invalid = false;
    uint32_t _recovery_window_until = 0;
    uint32_t _recovery_touch_started = 0;
    bool _recovery_touch_pending = false;
    bool _recovery_triggered = false;
    uint16_t _recovery_touch_x = 0;
    uint16_t _recovery_touch_y = 0;
    Feedback _feedback = Feedback::None;
    uint32_t _feedback_until = 0;
    const char* _last_action = "Tap a control to update the demo";

    void draw();
    void draw_home();
    void draw_settings();
    void draw_connections();
    void draw_wifi_settings();
    void draw_wifi_field(uint8_t index);
    void draw_wifi_scan_results();
    void draw_touch_calibration();
    void draw_calibration_header();
    void draw_calibration_target();
    void draw_calibration_result();
    void draw_header(const char* title);
    void draw_machine_status();
    void draw_live_status_updates();
    void draw_wifi_diagnostics();
    void draw_keyboard(bool draw_panel = true);
    void draw_button(Rect bounds, const char* label, bool selected = false,
                     bool emphasized = false);
    void draw_axis_tile(uint8_t axis, Rect bounds);
    void draw_axis_selection(uint8_t axis);
    void draw_axis_selector(uint8_t axis);
    void draw_step_selector(uint8_t step);
    void draw_transport_card(uint8_t index);
    void draw_transport_status();
    void draw_home_footer();
    void draw_jog_buttons();
    void draw_jog_button(Feedback feedback);
    void draw_position(uint8_t axis);
    void handle_touch(uint16_t x, uint16_t y);
    void handle_wifi_touch(uint16_t x, uint16_t y);
    void append_keyboard_character(char character);
    void erase_keyboard_character();
    void set_live_mode(bool live);
    void begin_calibration();
    void begin_calibration_touch(uint8_t count, const TP_Point& point);
    void collect_calibration_sample(uint8_t count, const TP_Point& point);
    void finish_calibration_touch();
    void evaluate_calibration();
    void reset_calibration();
    void map_touch_for_ui(const TP_Point& point, uint16_t& ui_x,
                          uint16_t& ui_y) const;
    bool load_calibration();
    bool save_calibration();
    void draw_calibration_progress();
    void log_calibration_sample(uint8_t count, const TP_Point& point,
                                uint16_t ui_x, uint16_t ui_y);
    bool contains(Rect bounds, uint16_t x, uint16_t y) const;
    void log_touch(uint8_t count, const TP_Point& point,
                   uint16_t ui_x, uint16_t ui_y);
    void set_feedback(Feedback feedback, const char* message);
    void redraw();
    uint16_t clamp_x(uint16_t x) const;
    uint16_t clamp_y(uint16_t y) const;
    static const char* axis_name(uint8_t axis);
    static float step_value(uint8_t index);
};

} // namespace jc8048w550
