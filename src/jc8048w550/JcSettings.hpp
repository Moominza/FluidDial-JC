// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// User settings: defaults, validation, and a versioned + checksummed binary
// encoding. Pure logic; the firmware stores the blob in NVS (JcSettingsStore).
// Wi-Fi credentials and touch calibration are stored separately and are never
// part of this blob.

#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "JcCommandBuilder.hpp"

namespace jc8048w550 {

constexpr uint8_t kJcStepPresets = 5;
constexpr uint8_t kJcFeedPresets = 3;
constexpr uint8_t kJcMacroSlots = 6;

struct JcMacroSlot {
    char name[16];
    char path[48];
    char description[40];
};

struct JcUserSettings {
    float   linear_steps[kJcStepPresets];
    float   rotary_steps[kJcStepPresets];
    float   linear_feeds[kJcFeedPresets];
    float   rotary_feeds[kJcFeedPresets];
    uint8_t brightness_percent;  // 10..100
    uint8_t coordinate_decimals; // 1..4
    uint8_t default_reference;   // 0 machine, 1 work
    uint8_t transport;           // 0 Wi-Fi, 1 UART, 2 BLE (selection only)
    uint32_t uart_baud;
    int8_t  uart_rx_pin;         // -1 = not configured
    int8_t  uart_tx_pin;
    bool    a_axis_has_homing;
    float   probe_feed;
    float   probe_travel;
    float   probe_retract;
    float   plate_thickness;
    float   tool_diameter;
    JcMacroSlot macros[kJcMacroSlots];
};

inline JcUserSettings jc_settings_defaults() {
    JcUserSettings s {};
    const float lin[kJcStepPresets] = { 0.01f, 0.1f, 1.0f, 10.0f, 100.0f };
    const float rot[kJcStepPresets] = { 0.1f, 1.0f, 5.0f, 45.0f, 90.0f };
    memcpy(s.linear_steps, lin, sizeof(lin));
    memcpy(s.rotary_steps, rot, sizeof(rot));
#if defined(JC_LIVE_JOG_TEST) && JC_LIVE_JOG_TEST
    const float lf[kJcFeedPresets] = { 100.0f, 1000.0f, 3000.0f };  // restricted live-jog test image caps jog feed at 100
#else
    const float lf[kJcFeedPresets] = { 300.0f, 1000.0f, 3000.0f };
#endif
    const float rf[kJcFeedPresets] = { 200.0f, 500.0f, 1000.0f };
    memcpy(s.linear_feeds, lf, sizeof(lf));
    memcpy(s.rotary_feeds, rf, sizeof(rf));
    s.brightness_percent = 100;
    s.coordinate_decimals = 3;
    s.default_reference = 1;
    s.transport = 0;
    s.uart_baud = 115200;
    s.uart_rx_pin = -1;
    s.uart_tx_pin = -1;
    s.a_axis_has_homing = false;
    s.probe_feed = 50.0f;
    s.probe_travel = 20.0f;
    s.probe_retract = 5.0f;
    s.plate_thickness = 0.0f;
    s.tool_diameter = 0.0f;
    return s;
}

namespace detail {

inline float clampf(float v, float lo, float hi, float fallback) {
    if (!isfinite(v)) return fallback;
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void terminate(char* s, size_t n) { s[n - 1] = '\0'; }

inline bool printable_ascii(const char* s) {
    for (; *s; ++s) {
        if (static_cast<uint8_t>(*s) < 0x20 || static_cast<uint8_t>(*s) > 0x7E) return false;
    }
    return true;
}

}  // namespace detail

// Brings every field into its valid range. Out-of-range or non-finite values
// fall back to the default; invalid macros are cleared (never executed).
inline void jc_settings_sanitize(JcUserSettings& s) {
    const JcUserSettings d = jc_settings_defaults();
    for (uint8_t i = 0; i < kJcStepPresets; ++i) {
        s.linear_steps[i] = detail::clampf(s.linear_steps[i], 0.001f, 100.0f, d.linear_steps[i]);
        s.rotary_steps[i] = detail::clampf(s.rotary_steps[i], 0.001f, 360.0f, d.rotary_steps[i]);
    }
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        s.linear_feeds[i] = detail::clampf(s.linear_feeds[i], 1.0f, 10000.0f, d.linear_feeds[i]);
        s.rotary_feeds[i] = detail::clampf(s.rotary_feeds[i], 1.0f, 36000.0f, d.rotary_feeds[i]);
    }
    if (s.brightness_percent < 10 || s.brightness_percent > 100) s.brightness_percent = d.brightness_percent;
    if (s.coordinate_decimals < 1 || s.coordinate_decimals > 4) s.coordinate_decimals = d.coordinate_decimals;
    if (s.default_reference > 1) s.default_reference = d.default_reference;
    if (s.transport > 2) s.transport = d.transport;
    static const uint32_t bauds[] = { 9600, 19200, 38400, 57600, 115200, 230400 };
    bool baud_ok = false;
    for (uint32_t b : bauds) baud_ok = baud_ok || b == s.uart_baud;
    if (!baud_ok) s.uart_baud = d.uart_baud;
    if (s.uart_rx_pin < -1 || s.uart_rx_pin > 48) s.uart_rx_pin = -1;
    if (s.uart_tx_pin < -1 || s.uart_tx_pin > 48) s.uart_tx_pin = -1;
    s.a_axis_has_homing = s.a_axis_has_homing ? true : false;
    s.probe_feed = detail::clampf(s.probe_feed, 1.0f, 200.0f, d.probe_feed);
    s.probe_travel = detail::clampf(s.probe_travel, 0.1f, 50.0f, d.probe_travel);
    s.probe_retract = detail::clampf(s.probe_retract, 0.0f, 50.0f, d.probe_retract);
    s.plate_thickness = detail::clampf(s.plate_thickness, -100.0f, 100.0f, d.plate_thickness);
    s.tool_diameter = detail::clampf(s.tool_diameter, 0.0f, 100.0f, d.tool_diameter);
    for (JcMacroSlot& m : s.macros) {
        detail::terminate(m.name, sizeof(m.name));
        detail::terminate(m.path, sizeof(m.path));
        detail::terminate(m.description, sizeof(m.description));
        const bool names_ok = detail::printable_ascii(m.name) && detail::printable_ascii(m.description);
        const bool path_ok = m.path[0] == '\0' || detail::path_ok(m.path);
        if (!names_ok || !path_ok) {
            memset(&m, 0, sizeof(m));
        }
    }
}

// Machine profile used by the command builder, derived from user settings.
inline JcMachineProfile jc_profile_from_settings(const JcUserSettings& s) {
    JcMachineProfile p = jc_default_profile();
    p.axes[3].homing_allowed = s.a_axis_has_homing;
    p.max_probe_feed = 200.0f;
    return p;
}

// ---- Versioned, checksummed encoding -------------------------------------

constexpr uint32_t kJcSettingsMagic = 0x4A435331u;  // "JCS1"
constexpr uint16_t kJcSettingsVersion = 1;

struct JcSettingsBlob {
    uint32_t       magic;
    uint16_t       version;
    uint16_t       size;
    JcUserSettings settings;
    uint32_t       crc;
};

inline uint32_t jc_crc32(const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

inline JcSettingsBlob jc_settings_encode(const JcUserSettings& s) {
    JcSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = kJcSettingsMagic;
    blob.version = kJcSettingsVersion;
    blob.size = sizeof(JcUserSettings);
    blob.settings = s;
    blob.crc = jc_crc32(reinterpret_cast<const uint8_t*>(&blob), offsetof(JcSettingsBlob, crc));
    return blob;
}

// Returns false (and defaults) for any wrong size, magic, version or checksum.
// Accepted settings are sanitized before being returned.
inline bool jc_settings_decode(const void* data, size_t length, JcUserSettings& out) {
    out = jc_settings_defaults();
    if (data == nullptr || length != sizeof(JcSettingsBlob)) return false;
    JcSettingsBlob blob;
    memcpy(&blob, data, sizeof(blob));
    if (blob.magic != kJcSettingsMagic || blob.version != kJcSettingsVersion
        || blob.size != sizeof(JcUserSettings)
        || blob.crc != jc_crc32(reinterpret_cast<const uint8_t*>(&blob), offsetof(JcSettingsBlob, crc))) {
        return false;
    }
    out = blob.settings;
    jc_settings_sanitize(out);
    return true;
}

}  // namespace jc8048w550
