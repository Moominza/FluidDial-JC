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
#if JC_LIVE_JOG_TEST
      _jogs(&JcApp::jog_tx_thunk, this),
#else
      _jogs(nullptr, nullptr),
#endif
      _sv { display, link, _model, _settings, _profile, _dispatcher, _loader, _jogs, live_mode, *this },
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
      _limits_screen(_sv),
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
#if JC_LIVE_JOG_TEST
    // EXPERIMENTAL live-jog test build: the ONLY place a command sink is installed.
    _dispatcher.set_sink_ex(&JcJogSession::sink, &_jogs);
    Serial.println("*** LIVE JOG TEST BUILD: restricted incremental jog + jog cancel are ENABLED ***");
    Serial.println("*** X/Y 1 mm @100, Z 0.5 mm @50, A disabled; continuous/home/probe/macro/file/spindle blocked ***");
#else
    Serial.println("app: motion commands disabled");
#endif

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
        case JcPage::LimitsHoming: return &_limits_screen;
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

void JcApp::feed_status(const JcLiveMachineStatus& status, bool fresh) {
    _model.on_status(status);
    JcJogStatus view;
    view.fresh = fresh && _sv.live_mode;
    view.state = status.machine_state;
    view.sequence = status.sequence;
    view.pos_valid = status.has_data;
    memcpy(view.pos, status.axes, sizeof(view.pos));
    _jogs.on_status(view, millis());
}

void JcApp::note_link_fresh(bool fresh) {
    _model.set_fresh(fresh);
    if (_last_fresh && !fresh) {
        _dispatcher.on_link_lost();  // arming never survives a loss of fresh data
        // A jog that is still unresolved: try to cancel it if the socket is still open
        // (stale data), otherwise record the loss. Nothing is ever replayed.
        _jogs.on_link_lost(millis(), !uart_active() && _link.tcp_connected());
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
#if JC_LIVE_JOG_TEST
    // Last, so the controller's travel can only ever lower the test limits, never widen them.
    jc_profile_apply_test_limits(_profile);
#endif
}

const char* JcApp::start_caps_read() {
    const char* blocker = jc_caps_read_blocker(_sv.live_mode, _model.fresh(), _model.state(), _loader.running());
    if (blocker == nullptr && _jogs.unresolved()) {
        blocker = "a jog operation is unresolved";
    }
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
    const uint32_t now = millis();
    _loader.on_line(line, now);
    _jogs.on_line(line, now);
}

#if JC_LIVE_JOG_TEST
bool JcApp::send_jog(const char* bytes, size_t length) {
    if (uart_active()) {
        return false;  // the wired link has no jog path in this build
    }
    return _link.send_jog_bytes(bytes, length);
}
#endif

void JcApp::service() {
    const uint32_t now = millis();
    _jogs.set_clock(now);
    _jogs.tick(now);
    if (!_sv.live_mode && _jogs.unresolved()) {
        _jogs.on_link_lost(now, false);  // DEMO mode has no controller
    }
#if JC_LIVE_JOG_TEST
    // Arming is only meaningful on the jog page (or an input dialog opened from it).
    if (_dispatcher.armed() && _nav.current() != JcPage::Jog && _nav.current() != JcPage::Input) {
        _dispatcher.disarm();
        Serial.println("jogtest: disarmed (left the jog page)");
    }
    if (_dispatcher.armed() && !_dispatcher.armed_at(now)) {
        _dispatcher.disarm();
        Serial.println("jogtest: arming lapsed");
    }
#endif
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
        char field[720];
        field[0] = '\0';
        for (uint8_t f = 0; f < kJcQueryFieldCount; ++f) {
            const size_t used = strlen(field);
            const JcQueryField qf = static_cast<JcQueryField>(f);
            if (a.state[f] == JcFieldState::Valid) {
                if (jc_query_field_kind(qf) == JcFieldKind::Pin) {
                    snprintf(field + used, sizeof(field) - used, " %s=%s", jc_query_field_key(qf),
                             a.pin[f - static_cast<uint8_t>(JcQueryField::LimitNegPin)]);
                } else {
                    snprintf(field + used, sizeof(field) - used, " %s=%g", jc_query_field_key(qf),
                             static_cast<double>(a.value[f]));
                }
            } else if (a.state[f] == JcFieldState::CtrlError) {
                snprintf(field + used, sizeof(field) - used, " %s=error:%d", jc_query_field_key(qf), a.error_code[f]);
            } else {
                snprintf(field + used, sizeof(field) - used, " %s=(%u)", jc_query_field_key(qf),
                         static_cast<unsigned>(a.state[f]));
            }
        }
        Serial.printf("caps: axis %c limits_confirmed=%d envelope_read=%d%s\n", names[i], a.limits_confirmed() ? 1 : 0,
                      a.envelope_read() ? 1 : 0, field);
    }
    Serial.printf("caps: probe_pin=[%s] toolsetter_pin=[%s] must_home=%s\n", c.probe_pin, c.toolsetter_pin,
                  c.must_home_state == JcFieldState::Valid ? (c.must_home ? "true" : "false")
                  : c.must_home_state == JcFieldState::CtrlError ? "error" : "unknown");
}

}  // namespace jc8048w550
