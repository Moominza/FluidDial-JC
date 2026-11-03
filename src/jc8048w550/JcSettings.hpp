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

constexpr uint8_t kJcStepPresets = 6;      // array size; linear axes use 5, the rotary axis 6
constexpr uint8_t jc_step_count(uint8_t axis) { return axis == 3 ? 6 : 5; }
constexpr uint8_t kJcFeedPresets = 4;       // jog feed presets per axis group (X/Y, Z, A)
constexpr uint8_t kJcFeedPresetsV2 = 3;     // format 1 and 2 stored three
constexpr uint8_t kJcMacroSlots = 6;

struct JcMacroSlot {
    char name[16];
    char path[48];
    char description[40];
};

struct JcUserSettings {
    float   linear_steps[kJcStepPresets];
    float   rotary_steps[kJcStepPresets];
    float   linear_feeds[kJcFeedPresets];   // X and Y
    float   z_feeds[kJcFeedPresets];        // Z (its own maximum is lower)
    float   rotary_feeds[kJcFeedPresets];   // A, degrees/min
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
    const float lin[kJcStepPresets] = { 0.01f, 0.1f, 1.0f, 10.0f, 100.0f, 100.0f };  // [5] unused for linear axes
    const float rot[kJcStepPresets] = { 0.1f, 1.0f, 5.0f, 15.0f, 45.0f, 90.0f };
    memcpy(s.linear_steps, lin, sizeof(lin));
    memcpy(s.rotary_steps, rot, sizeof(rot));
    // Maxima of the owner's machine (X/Y 6000, Z 1500 mm/min, A 1000 deg/min); jc_profile_apply_caps() lowers the
    // limit to whatever the controller reports, and a preset above an axis limit is refused when a jog is built.
#if defined(JC_LIVE_JOG_TEST) && JC_LIVE_JOG_TEST
    const float lf[kJcFeedPresets] = { 100.0f, 1000.0f, 3000.0f, 6000.0f };  // restricted live-jog test image caps jog feed at 100
#else
    const float lf[kJcFeedPresets] = { 300.0f, 1000.0f, 3000.0f, 6000.0f };
#endif
    const float zf[kJcFeedPresets] = { 100.0f, 300.0f, 750.0f, 1500.0f };
    const float rf[kJcFeedPresets] = { 100.0f, 250.0f, 500.0f, 1000.0f };
    memcpy(s.linear_feeds, lf, sizeof(lf));
    memcpy(s.z_feeds, zf, sizeof(zf));
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

// The feed presets used by an axis: X and Y share one set, Z and A have their own.
inline const float* jc_feed_presets(const JcUserSettings& s, uint8_t axis) {
    return axis == 3 ? s.rotary_feeds : (axis == 2 ? s.z_feeds : s.linear_feeds);
}

// Index of the preset to use when `wanted` is above the axis limit: the largest preset that does not exceed the
// limit (never a larger one), or 0xFF when even the smallest is above it. `wanted` itself when it is allowed.
inline uint8_t jc_feed_choice_within(const float* presets, uint8_t wanted, float axis_limit) {
    if (wanted < kJcFeedPresets && presets[wanted] <= axis_limit) return wanted;
    uint8_t best = 0xFF;
    for (uint8_t i = 0; i < kJcFeedPresets; ++i) {
        if (presets[i] <= axis_limit && (best == 0xFF || presets[i] > presets[best])) best = i;
    }
    return best;
}

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
        s.z_feeds[i] = detail::clampf(s.z_feeds[i], 1.0f, 10000.0f, d.z_feeds[i]);
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
constexpr uint16_t kJcSettingsVersion = 3;  // v3: four feed presets, own Z set; v1 and v2 blobs are migrated, not discarded

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

// Version 1 layout (five step presets per axis), kept only to migrate saved settings.
struct JcUserSettingsV1 {
    float   linear_steps[5];
    float   rotary_steps[5];
    float   linear_feeds[kJcFeedPresetsV2];
    float   rotary_feeds[kJcFeedPresetsV2];
    uint8_t brightness_percent;
    uint8_t coordinate_decimals;
    uint8_t default_reference;
    uint8_t transport;
    uint32_t uart_baud;
    int8_t  uart_rx_pin;
    int8_t  uart_tx_pin;
    bool    a_axis_has_homing;
    float   probe_feed;
    float   probe_travel;
    float   probe_retract;
    float   plate_thickness;
    float   tool_diameter;
    JcMacroSlot macros[kJcMacroSlots];
};
struct JcSettingsBlobV1 {
    uint32_t         magic;
    uint16_t         version;
    uint16_t         size;
    JcUserSettingsV1 settings;
    uint32_t         crc;
};

// Version 2 layout (six rotary step presets, three feed presets), kept only to migrate saved settings.
struct JcUserSettingsV2 {
    float   linear_steps[kJcStepPresets];
    float   rotary_steps[kJcStepPresets];
    float   linear_feeds[kJcFeedPresetsV2];
    float   rotary_feeds[kJcFeedPresetsV2];
    uint8_t brightness_percent;
    uint8_t coordinate_decimals;
    uint8_t default_reference;
    uint8_t transport;
    uint32_t uart_baud;
    int8_t  uart_rx_pin;
    int8_t  uart_tx_pin;
    bool    a_axis_has_homing;
    float   probe_feed;
    float   probe_travel;
    float   probe_retract;
    float   plate_thickness;
    float   tool_diameter;
    JcMacroSlot macros[kJcMacroSlots];
};
struct JcSettingsBlobV2 {
    uint32_t         magic;
    uint16_t         version;
    uint16_t         size;
    JcUserSettingsV2 settings;
    uint32_t         crc;
};

static_assert(sizeof(JcSettingsBlob) != sizeof(JcSettingsBlobV1) && sizeof(JcSettingsBlob) != sizeof(JcSettingsBlobV2)
              && sizeof(JcSettingsBlobV1) != sizeof(JcSettingsBlobV2), "the stored length must identify the settings format");

// The stored lengths the application accepts for decoding: the current format and the previous (migrated) one.
// Anything else is not a settings blob of ours and the defaults are used (the stored bytes are left alone).
inline bool jc_settings_stored_length_ok(size_t stored) {
    return stored == sizeof(JcSettingsBlob) || stored == sizeof(JcSettingsBlobV2) || stored == sizeof(JcSettingsBlobV1);
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

namespace detail {

// Feed presets of an older format (three per set). A set still equal to the old factory values moves to the new
// factory values; a set the operator customised is kept as it was (its fourth preset is the new factory one) and
// is never raised. The Z set did not exist: it starts from the factory values, whose maximum is the Z axis limit.
inline void migrate_feeds_v2(JcUserSettings& out, const float* old_linear, const float* old_rotary) {
    static const float old_lf[kJcFeedPresetsV2] = { 300.0f, 1000.0f, 3000.0f };
    static const float old_lf_a[kJcFeedPresetsV2] = { 100.0f, 500.0f, 1000.0f };
    static const float old_rf[kJcFeedPresetsV2] = { 200.0f, 500.0f, 1000.0f };
    static const float old_rf_a[kJcFeedPresetsV2] = { 600.0f, 1800.0f, 3600.0f };
    if (memcmp(old_linear, old_lf, sizeof(old_lf)) != 0 && memcmp(old_linear, old_lf_a, sizeof(old_lf_a)) != 0) {
        memcpy(out.linear_feeds, old_linear, sizeof(float) * kJcFeedPresetsV2);
    }
    if (memcmp(old_rotary, old_rf, sizeof(old_rf)) != 0 && memcmp(old_rotary, old_rf_a, sizeof(old_rf_a)) != 0) {
        memcpy(out.rotary_feeds, old_rotary, sizeof(float) * kJcFeedPresetsV2);
    }
}

}  // namespace detail

// Returns false (and defaults) for any wrong size, magic, version or checksum.
// Accepted settings are sanitized before being returned.
inline bool jc_settings_decode(const void* data, size_t length, JcUserSettings& out) {
    out = jc_settings_defaults();
    if (data != nullptr && length == sizeof(JcSettingsBlobV1)) {
        JcSettingsBlobV1 old;
        memcpy(&old, data, sizeof(old));
        if (old.magic != kJcSettingsMagic || old.version != 1 || old.size != sizeof(JcUserSettingsV1)
            || old.crc != jc_crc32(reinterpret_cast<const uint8_t*>(&old), offsetof(JcSettingsBlobV1, crc))) {
            return false;
        }
        // Keep every saved preference. Step presets still equal to the old factory values move to the new
        // factory values; anything the operator customised is kept.
        static const float old_lin[5] = { 0.1f, 1.0f, 10.0f, 50.0f, 100.0f };
        static const float old_rot[5] = { 0.1f, 1.0f, 10.0f, 45.0f, 90.0f };
        const JcUserSettings d = jc_settings_defaults();
        const JcUserSettingsV1& o = old.settings;
        out = d;
        if (memcmp(o.linear_steps, old_lin, sizeof(old_lin)) != 0) memcpy(out.linear_steps, o.linear_steps, sizeof(o.linear_steps));
        if (memcmp(o.rotary_steps, old_rot, sizeof(old_rot)) != 0) {
            memcpy(out.rotary_steps, o.rotary_steps, sizeof(o.rotary_steps));
            out.rotary_steps[5] = d.rotary_steps[5];
        }
        detail::migrate_feeds_v2(out, o.linear_feeds, o.rotary_feeds);
        out.brightness_percent = o.brightness_percent;
        out.coordinate_decimals = o.coordinate_decimals;
        out.default_reference = o.default_reference;
        out.transport = o.transport;
        out.uart_baud = o.uart_baud;
        out.uart_rx_pin = o.uart_rx_pin;
        out.uart_tx_pin = o.uart_tx_pin;
        out.a_axis_has_homing = o.a_axis_has_homing;
        out.probe_feed = o.probe_feed;
        out.probe_travel = o.probe_travel;
        out.probe_retract = o.probe_retract;
        out.plate_thickness = o.plate_thickness;
        out.tool_diameter = o.tool_diameter;
        memcpy(out.macros, o.macros, sizeof(out.macros));
        jc_settings_sanitize(out);
        return true;
    }
    if (data != nullptr && length == sizeof(JcSettingsBlobV2)) {
        JcSettingsBlobV2 old;
        memcpy(&old, data, sizeof(old));
        if (old.magic != kJcSettingsMagic || old.version != 2 || old.size != sizeof(JcUserSettingsV2)
            || old.crc != jc_crc32(reinterpret_cast<const uint8_t*>(&old), offsetof(JcSettingsBlobV2, crc))) {
            return false;
        }
        const JcUserSettingsV2& o = old.settings;
        out = jc_settings_defaults();
        memcpy(out.linear_steps, o.linear_steps, sizeof(o.linear_steps));
        memcpy(out.rotary_steps, o.rotary_steps, sizeof(o.rotary_steps));
        detail::migrate_feeds_v2(out, o.linear_feeds, o.rotary_feeds);
        out.brightness_percent = o.brightness_percent;
        out.coordinate_decimals = o.coordinate_decimals;
        out.default_reference = o.default_reference;
        out.transport = o.transport;
        out.uart_baud = o.uart_baud;
        out.uart_rx_pin = o.uart_rx_pin;
        out.uart_tx_pin = o.uart_tx_pin;
        out.a_axis_has_homing = o.a_axis_has_homing;
        out.probe_feed = o.probe_feed;
        out.probe_travel = o.probe_travel;
        out.probe_retract = o.probe_retract;
        out.plate_thickness = o.plate_thickness;
        out.tool_diameter = o.tool_diameter;
        memcpy(out.macros, o.macros, sizeof(out.macros));
        jc_settings_sanitize(out);
        return true;
    }
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
