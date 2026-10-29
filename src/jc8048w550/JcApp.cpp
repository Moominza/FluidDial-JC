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
    : _link(link),
      _uart(uart),
      _settings(jc_settings_defaults()),
      _profile(jc_profile_from_settings(_settings)),
      _sv { display, link, _model, _settings, _profile, _dispatcher, _loader, live_mode, *this },
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
      _caps_screen(_sv),
      _dry_run(_sv),
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
    refresh_profile();
    _link.set_line_listener(&JcApp::line_thunk, this);
    _uart.set_line_listener(&JcApp::line_thunk, this);

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
    refresh_profile();
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
        case JcPage::Capabilities: return &_caps_screen;
        case JcPage::DryRun: return &_dry_run;
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
        // Controller values may be stale after a loss (it may have restarted or
        // been reconfigured): abandon any read and fall back to placeholders.
        const uint32_t now = millis();
        const bool had_caps = _loader.caps().any_data();
        _loader.on_link_lost(now);
        _loader.invalidate(now);
        if (had_caps) {
            Serial.println("caps: connection lost; controller values discarded");
        }
        refresh_profile();
    }
    _last_fresh = fresh;
}

bool JcApp::uart_active() const {
    return _settings.transport == 1 && _uart.running();
}

bool JcApp::send_query(const char* line) {
    return uart_active() ? _uart.send_query_line(line) : _link.send_query_line(line);
}

void JcApp::refresh_profile() {
    _profile = jc_profile_from_settings(_settings);
    if (_loader.state() == JcLoadState::Complete) {
        jc_profile_apply_caps(_profile, _loader.caps());
    }
}

const char* JcApp::start_caps_read() {
    const char* blocker = jc_caps_read_blocker(_sv.live_mode, _model.fresh(), _model.state(), _loader.running());
    if (blocker != nullptr) {
        Serial.printf("caps: read refused: %s\n", blocker);
        return blocker;
    }
    _loader.start(millis(), nullptr);
    refresh_profile();  // a new read starts from placeholders until it completes
    Serial.println("caps: read-only capability read started");
    return nullptr;
}

void JcApp::cancel_caps_read() {
    _loader.cancel(millis());
    refresh_profile();
    Serial.println("caps: read cancelled");
}

void JcApp::on_controller_line(const char* line) {
    _loader.on_line(line, millis());
}

void JcApp::service() {
    const uint32_t now = millis();
    if (_loader.running() && !_sv.live_mode) {
        _loader.cancel(now);  // DEMO mode has no controller to ask
    }
    if (_loader.running()) {
        _loader.tick(now);
        if (const char* query = _loader.pending_line(now)) {
            if (send_query(query)) {
                _loader.mark_sent(now);
            } else {
                _loader.send_failed(now);
            }
        }
    }
    if (_loader.take_finished()) {
        on_caps_finished();
    }
}

void JcApp::on_caps_finished() {
    refresh_profile();
    const JcMachineCaps& c = _loader.caps();
    if (_loader.state() != JcLoadState::Complete) {
        Serial.printf("caps: read failed: %s\n", jc_load_failure_name(_loader.failure()));
        return;
    }
    Serial.printf("caps: complete answered=%u refused=%u ms=%lu fw=[%s] wcs=%s tool=%d\n",
                  static_cast<unsigned>(c.queries_ok), static_cast<unsigned>(c.queries_error),
                  static_cast<unsigned long>(c.read_ms), c.firmware, c.wcs[0] ? c.wcs : "--", c.tool);
    static constexpr const char* names = "XYZA";
    for (uint8_t i = 0; i < 4; ++i) {
        const JcAxisCaps& a = c.axes[i];
        char field[320];
        field[0] = '\0';
        for (uint8_t f = 0; f < kJcQueryFieldCount; ++f) {
            const size_t used = strlen(field);
            if (a.state[f] == JcFieldState::Valid) {
                snprintf(field + used, sizeof(field) - used, " %s=%g", jc_query_field_key(static_cast<JcQueryField>(f)),
                         static_cast<double>(a.value[f]));
            } else {
                snprintf(field + used, sizeof(field) - used, " %s=(%u)", jc_query_field_key(static_cast<JcQueryField>(f)),
                         static_cast<unsigned>(a.state[f]));
            }
        }
        Serial.printf("caps: axis %c limits_confirmed=%d%s\n", names[i], a.limits_confirmed() ? 1 : 0, field);
    }
    Serial.printf("caps: probe_pin=[%s] toolsetter_pin=[%s]\n", c.probe_pin, c.toolsetter_pin);
}

}  // namespace jc8048w550
