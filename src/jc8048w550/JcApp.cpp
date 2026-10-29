// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcApp.hpp"

#include <Preferences.h>

namespace jc8048w550 {

namespace {
constexpr uint8_t kBacklightPin = 2;       // GPIO2 backlight (see HardwareBringup.cpp)
constexpr uint8_t kBacklightChannel = 0;
constexpr const char* kPrefsNamespace = "jc_app";
constexpr const char* kPrefsKey = "settings";
}  // namespace

JcApp::JcApp(Arduino_GFX& display, JcReadOnlyFluidNc& link, JcUartLink& uart, const bool& live_mode)
    : _settings(jc_settings_defaults()),
      _profile(jc_profile_from_settings(_settings)),
      _sv { display, link, _model, _settings, _profile, _dispatcher, live_mode, *this },
      _menu(_sv),
      _jog(_sv),
      _homing(_sv),
      _probing(_sv),
      _offsets(_sv),
      _files(_sv),
      _macros(_sv),
      _tools(_sv),
      _settings_screen(_sv),
      _connections(_sv, uart),
      _display_settings(_sv),
      _jog_settings(_sv),
      _machine(_sv),
      _diagnostics(_sv, uart),
      _input(_sv) {}

void JcApp::begin(LeaveFn leave_fn, void* user) {
    _leave_fn = leave_fn;
    _leave_user = user;

    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        uint8_t buffer[sizeof(JcSettingsBlob)];
        const size_t stored = prefs.getBytesLength(kPrefsKey);
        if (stored == sizeof(buffer) && prefs.getBytes(kPrefsKey, buffer, sizeof(buffer)) == sizeof(buffer)
            && jc_settings_decode(buffer, sizeof(buffer), _settings)) {
            Serial.println("app: user settings loaded from NVS");
        } else {
            _settings = jc_settings_defaults();
            Serial.printf("app: user settings %s; defaults in use\n", stored == 0 ? "not stored" : "invalid");
        }
        prefs.end();
    } else {
        // A read-only open fails until the namespace has been written once.
        Serial.println("app: no stored user settings yet; defaults in use");
    }
    _profile = jc_profile_from_settings(_settings);

    ledcSetup(kBacklightChannel, 5000, 8);
    ledcAttachPin(kBacklightPin, kBacklightChannel);
    apply_brightness();
}

void JcApp::apply_brightness() {
    const uint32_t duty = (static_cast<uint32_t>(_settings.brightness_percent) * 255u) / 100u;
    ledcWrite(kBacklightChannel, duty);
}

void JcApp::save_settings() {
    jc_settings_sanitize(_settings);
    _profile = jc_profile_from_settings(_settings);
    const JcSettingsBlob blob = jc_settings_encode(_settings);
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
        const size_t written = prefs.putBytes(kPrefsKey, &blob, sizeof(blob));
        prefs.end();
        if (written != sizeof(blob)) {
            Serial.println("app: failed to save user settings");
        }
    } else {
        Serial.println("app: NVS unavailable; settings not saved");
    }
}

JcScreen* JcApp::screen_for(JcPage page) {
    switch (page) {
        case JcPage::Menu: return &_menu;
        case JcPage::Jog: return &_jog;
        case JcPage::Homing: return &_homing;
        case JcPage::Probing: return &_probing;
        case JcPage::Offsets: return &_offsets;
        case JcPage::Files: return &_files;
        case JcPage::Macros: return &_macros;
        case JcPage::Tools: return &_tools;
        case JcPage::Settings: return &_settings_screen;
        case JcPage::SetConnections: return &_connections;
        case JcPage::SetDisplay: return &_display_settings;
        case JcPage::SetJogging: return &_jog_settings;
        case JcPage::SetMachine: return &_machine;
        case JcPage::Diagnostics: return &_diagnostics;
        case JcPage::Input: return &_input;
        case JcPage::Count: break;
    }
    return &_menu;
}

void JcApp::open() {
    _nav.reset();
    _menu.enter();
    Serial.println("app: menu opened");
}

void JcApp::open_settings() {
    _nav.reset();
    _nav.go(JcPage::Settings);
    _settings_screen.enter();
}

void JcApp::resume() {
    screen_for(_nav.current())->draw();
}

void JcApp::go(JcPage page) {
    if (_nav.go(page)) {
        screen_for(page)->enter();
        Serial.printf("app: page=%s\n", jc_page_title(page));
    }
}

void JcApp::back() {
    if (_nav.back() == JcNav::BackResult::ExitApp) {
        leave(JcLeaveTarget::Dashboard);
        return;
    }
    screen_for(_nav.current())->draw();
}

void JcApp::request_input(const JcInputRequest& request) {
    _input.begin(request);
    if (_nav.go(JcPage::Input)) {
        _input.enter();
    }
}

void JcApp::leave(JcLeaveTarget target) {
    if (_leave_fn != nullptr) {
        _leave_fn(_leave_user, target);
    }
}

void JcApp::touch(uint16_t x, uint16_t y) {
    screen_for(_nav.current())->touch(x, y);
}

void JcApp::tick() {
    screen_for(_nav.current())->tick(millis());
}

void JcApp::note_link_fresh(bool fresh) {
    _model.set_fresh(fresh);
    if (_last_fresh && !fresh) {
        _dispatcher.on_link_lost();  // arming never survives a loss of fresh data
    }
    _last_fresh = fresh;
}

}  // namespace jc8048w550
