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
#if JC_JOG_PREVIEW
      _cmd(&JcPreviewController::tx, &_preview.controller()),
#elif JC_FULL_CONTROL_TEST
      _cmd(&JcApp::cmd_tx_thunk, this),
#endif
#if JC_LIVE_JOG_TEST
      _jogs(&JcApp::jog_tx_thunk, this),
#elif JC_AUTHORITY
      _jogs(&JcCommandSystem::session_tx, &_cmd),
#else
      _jogs(nullptr, nullptr),
#endif
      _sv { display, link, _model, _settings, _profile, _dispatcher, _loader, _jogs, live_mode, *this, _sd, _job, _tool_reader, _tool_store, _ble, _ble_saved, _tm
#if JC_JOG_PREVIEW
             , _preview
#endif
#if JC_AUTHORITY
             , _cmd, _wf
#endif
      },
      _menu(_sv),
      _jog(_sv),
      _homing(_sv),
      _probing(_sv),
      _offsets(_sv),
      _files(_sv),
      _macros(_sv),
      _tools(_sv),
      _ble_screen(_sv),
      _settings_screen(_sv),
      _connections(_sv, uart),
      _display_settings(_sv),
      _jog_settings(_sv),
#if JC_AUTHORITY
      _workflow_screen(_sv),
#endif
      _machine(_sv),
      _caps_screen(_sv),
      _limits_screen(_sv),
      _dry_run(_sv),
#if JC_JOG_PREVIEW
      _jog_preview(_sv),
#endif
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
    // BLE UART (Instruction 010): the same parser and the same command policy as the other transports.
    _ble_radio.attach(&_ble);
    _ble.set_radio(&_ble_radio);
    _ble.set_line_listener(&JcApp::line_thunk, this);
    _sd.set_query(&JcApp::query_thunk, this);
    _tool_reader.set_query(&JcApp::query_thunk, this);
    memset(&_ble_saved, 0, sizeof(_ble_saved));
    {
        Preferences bp;
        if (bp.begin(kPrefsNamespace, true)) {
            JcBleSaved tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (bp.isKey("ble_dev") && bp.getBytesLength("ble_dev") == sizeof(tmp) && bp.getBytes("ble_dev", &tmp, sizeof(tmp)) == sizeof(tmp)
                && tmp.valid && jc_ble_addr_ok(tmp.addr)) {
                tmp.addr[sizeof(tmp.addr) - 1] = '\0';
                tmp.name[sizeof(tmp.name) - 1] = '\0';
                _ble_saved = tmp;
                Serial.printf("ble: saved device %s (%s)\n", tmp.addr, tmp.name);
            }
            bp.end();
        }
    }
    _tm.set_initial(static_cast<JcTransportKind>(_settings.transport));
    _tm.set_callbacks(&JcApp::tm_stop, &JcApp::tm_invalidate, this);
    if (_settings.transport == 2) {
        _ble.begin(_ble_saved, _ble_saved.valid, millis());   // reconnects to the SAVED device only
        Serial.println("ble: BLE UART selected at start-up");
    }
#if JC_JOG_PREVIEW
    // NON-MOTION TEST MODE: the sink is the jog session, whose only transport is the in-RAM simulated
    // controller. The gate is in preview mode, so an accepted command is counted as previewed, never sent.
    _dispatcher.set_preview(true);
    _cmd.attach_jogs(&_jogs);
    _preview.set_line_sink(&JcApp::cmd_line_thunk, this);
    _dispatcher.set_sink_ex(&JcCommandSystem::sink, &_cmd);
    Serial.println("*** PREVIEW ONLY - MOTION DISABLED: the jog interface runs against a SIMULATED controller ***");
    Serial.println("app: motion commands disabled");
#elif JC_FULL_CONTROL_TEST
    // EXPERIMENTAL FULL-CONTROL build: the unified command system is the only sink.
    _cmd.attach_jogs(&_jogs);
    _dispatcher.set_sink_ex(&JcCommandSystem::sink, &_cmd);
    Serial.println("*** FULL CONTROL TEST BUILD: the complete FluidNC operator interface can send commands ***");
    Serial.println("*** FluidNC is the authority on motion; the pendant checks command integrity only ***");
#elif JC_LIVE_JOG_TEST
    // EXPERIMENTAL live-jog test build: the ONLY place a command sink is installed.
    _dispatcher.set_sink_ex(&JcJogSession::sink, &_jogs);
    Serial.println("*** LIVE JOG TEST BUILD: restricted incremental jog + jog cancel are ENABLED ***");
    Serial.println("*** X/Y 1 mm @100 only; Z on hold, A disabled; continuous/home/probe/macro/file/spindle blocked ***");
    Serial.println("*** UNPROTECTED MICRO-JOG: no controller travel protection is assumed; operator acknowledgement required ***");
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
        case JcPage::Ble: return &_ble_screen;
        case JcPage::Settings: return &_settings_screen;
        case JcPage::SetConnections: return &_connections;
        case JcPage::SetDisplay: return &_display_settings;
        case JcPage::SetJogging: return &_jog_settings;
#if JC_AUTHORITY
        case JcPage::Workflow: return &_workflow_screen;
#else
        case JcPage::Workflow: return &_menu;
#endif
        case JcPage::SetMachine: return &_machine;
        case JcPage::Capabilities: return &_caps_screen;
        case JcPage::LimitsHoming: return &_limits_screen;
        case JcPage::DryRun: return &_dry_run;
#if JC_JOG_PREVIEW
        case JcPage::JogPreview: return &_jog_preview;
#else
        case JcPage::JogPreview: return &_menu;
#endif
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
    if (!_job_from_sim) {   // the controller's own SD report: percentage, name, feed hold
        _job.on_status(fresh && _sv.live_mode, status.machine_state, status.has_sd, status.sd_percent, status.sd_name);
    }
    JcJogStatus view;
    view.fresh = fresh && _sv.live_mode;
    view.state = status.machine_state;
    view.sequence = status.sequence;
    view.pos_valid = status.has_data;
    memcpy(view.pos, status.axes, sizeof(view.pos));
#if JC_JOG_PREVIEW
    _real = view;   // the session is fed by the bridge: real status plus the simulated displacement
#else
    _jogs.on_status(view, millis());
#endif
}

void JcApp::note_link_fresh(bool fresh) {
    _model.set_fresh(fresh);
    if (_last_fresh && !fresh) {
        _dispatcher.on_link_lost();  // arming never survives a loss of fresh data
        // A jog that is still unresolved: try to cancel it if the socket is still open
        // (stale data), otherwise record the loss. Nothing is ever replayed.
        _jogs.on_link_lost(millis(), _tm.active() == JcTransportKind::Wifi && _link.tcp_connected());
#if JC_JOG_PREVIEW
        _preview.controller().reset();               // a rehearsal never survives a loss of the link
        _preview.controller().reset_displacement();
        _preview.disengage();
#endif
#if JC_AUTHORITY
        _cmd.on_link_lost();                         // the operation ends; nothing is replayed
        _wf.on_link_lost();
        _wf_cmd_inflight = false;
#endif
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
        drop_extra_state();
    }
    _last_fresh = fresh;
}

bool JcApp::uart_active() const {
    return _settings.transport == 1 && _uart.running();
}

bool JcApp::send_query(const char* line) {
    switch (_tm.active()) {   // exactly one control transport; a stopped one never falls through to another
        case JcTransportKind::Ble: return _ble.send_query_line(line);
        case JcTransportKind::Uart: return _uart.running() && _uart.send_query_line(line);
        default: return _link.send_query_line(line);
    }
}

bool JcApp::job_simulated() const { return _job_from_sim; }

void JcApp::drop_extra_state() {
    _sd.on_link_lost();
    _tool_reader.on_link_lost();
    _tool_store.invalidate("connection lost or transport switched");
    _job.on_status(false, nullptr, false, 0.0f, nullptr);   // a running job becomes UNKNOWN; nothing is restarted
#if JC_JOG_PREVIEW
    _job_from_sim = false;
    JcSimNc& nc = _preview.controller().nc();
    nc.job = false;
    nc.job_hold = false;
    _job.clear();
#endif
}

void JcApp::invalidate_for_switch() {
    const uint32_t now = millis();
    _dispatcher.on_link_lost();
    _jogs.on_link_lost(now, false);
#if JC_JOG_PREVIEW
    _preview.controller().reset();
    _preview.controller().reset_displacement();
    _preview.disengage();
#endif
#if JC_AUTHORITY
    _cmd.on_link_lost();
    _wf.on_link_lost();
    _wf_cmd_inflight = false;
#endif
    _loader.on_link_lost(now);
    _loader.invalidate(now);
    refresh_profile();
    drop_extra_state();
    _model.set_fresh(false);   // the new transport must deliver its own status before anything is trusted
    _last_fresh = false;
    Serial.println("transport: switched; machine status and pending operations invalidated");
}

void JcApp::tm_stop(void* user, JcTransportKind which) {
    JcApp* app = static_cast<JcApp*>(user);
    switch (which) {
        case JcTransportKind::Wifi: app->_link.disconnect(); break;
        case JcTransportKind::Uart: app->_uart.end(); break;
        case JcTransportKind::Ble: app->_ble.end(); app->_ble_radio.shutdown(); break;
    }
}

const char* JcApp::query_blocker() const {
    if (!_sv.live_mode) return "not in LIVE mode";
    if (!_model.fresh()) return "no fresh machine status";
    if (_loader.running()) return "a capability read is running";
    if (_sd.busy()) return "an SD listing is in progress";
    if (_tool_reader.reading()) return "a tool state read is in progress";
    if (_jogs.unresolved()) return "a jog operation is unresolved";
#if JC_AUTHORITY
    if (_cmd.busy()) return "a command is awaiting its reply";
    if (_wf.running()) return "a routine is running";
#endif
    return nullptr;
}

const char* JcApp::sd_refresh() {
    const char* blocker = query_blocker();
    if (blocker != nullptr) return blocker;
    if (!_sd.refresh(millis())) return "an SD listing is in progress";
    _sd.clear_selection();
    Serial.println("sd: read-only listing started ($SD/Status, $SD/List)");
    return nullptr;
}

const char* JcApp::tool_read() {
    const char* blocker = query_blocker();
    if (blocker != nullptr) return blocker;
    if (!_tool_reader.start(millis())) return "a tool state read is in progress";
    Serial.println("tool: read-only tool state read started ($G, $#)");
    return nullptr;
}

const char* JcApp::select_transport(uint8_t transport) {
    if (transport > 2) return "unknown transport";
    const JcTransportKind to = static_cast<JcTransportKind>(transport);
    const char* why = _tm.select(to, _model.in_motion() && _model.fresh());
    if (why != nullptr) return why;
    if (_settings.transport != transport) {
        _settings.transport = transport;
        save_settings();
    }
    if (to == JcTransportKind::Ble) _ble.begin(_ble_saved, _ble_saved.valid, millis());
    Serial.printf("transport: %s selected\n", jc_transport_name(to));
    return nullptr;
}

bool JcApp::ble_scan() {
    return _tm.active() == JcTransportKind::Ble && _ble.scan(millis());
}

bool JcApp::ble_connect(const char* addr, const char* name) {
    return _tm.active() == JcTransportKind::Ble && _ble.connect_to(addr, name, millis());
}

void JcApp::ble_disconnect() {
    _ble.disconnect(millis());
}

void JcApp::ble_forget() {
    _ble.forget();
}

void JcApp::persist_ble(const JcBleSaved& s) {
    _ble_saved = s;
    Preferences p;
    if (p.begin(kPrefsNamespace, false)) {
        if (s.valid) p.putBytes("ble_dev", &s, sizeof(s)); else p.remove("ble_dev");
        p.end();
    }
    Serial.printf("ble: saved device preference %s\n", s.valid ? s.addr : "cleared");
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
#if !JC_JOG_PREVIEW
    _jogs.on_line(line, now);   // in the preview the session only hears the simulated controller
#endif
#if JC_FULL_CONTROL_TEST
    _cmd.on_line(line);
    _wf.on_line(line);
#endif
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
    _sd.service(now);
    _tool_reader.service(now);
    if (_tm.active() == JcTransportKind::Ble) {
        _ble_radio.poll(now);
        _ble.service(now);
        if (_ble.take_link_lost()) {
            drop_extra_state();
            Serial.println("ble: link lost; nothing queued is sent after a reconnect");
        }
        JcBleSaved saved;
        if (_ble.take_save_request(saved)) persist_ble(saved);
    }
#if JC_JOG_PREVIEW
    {   // the ordinary firmware rehearses a job on the SIMULATED controller; the percentage is still the controller's own report
        JcSimNc& nc = _preview.controller().nc();
        if (nc.job || _job_from_sim) {
            _job_from_sim = true;
            if (now - _sim_job_frame >= 200) {
                _sim_job_frame = now;
                char frame[256];
                nc.status_frame(frame, sizeof(frame));
                JcStatusFrame f;
                if (jc_parse_status_frame(frame, strlen(frame), f)) _job.on_status(true, f.state, f.has_sd, f.sd_percent, f.sd_name);
                if (!nc.job && !_job.active()) _job_from_sim = false;
            }
        }
    }
#endif
#if JC_JOG_PREVIEW
    _real.fresh = _last_fresh && _sv.live_mode;
    _preview.pump(_jogs, _real, now);
#endif
#if JC_AUTHORITY
    service_workflow(now);
#endif
    _jogs.tick(now);
    if (!_sv.live_mode && _jogs.unresolved()) {
        _jogs.on_link_lost(now, false);  // DEMO mode has no controller
    }
#if JC_JOG_UI
    // Arming (and its acknowledgement) only exists on the jog page (or an input dialog opened
    // from it), in LIVE mode and inside its time window; anything else drops both.
    const JcDisarmReason dropped = _dispatcher.enforce_arming_scope(
        now, _sv.live_mode, _nav.current() == JcPage::Jog || _nav.current() == JcPage::Input
                                   || (JC_AUTHORITY && _nav.current() == JcPage::Workflow));
    if (dropped != JcDisarmReason::None) {
        Serial.printf(JC_JOG_TAG ": disarmed (%s); acknowledgement cleared\n", jc_disarm_reason_text(dropped));
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

#if JC_AUTHORITY

void JcApp::cmd_line_thunk(void* user, const char* line, uint32_t now) {
    JcApp* app = static_cast<JcApp*>(user);
    app->_jogs.on_line(line, now);
    app->_cmd.on_line(line);
    app->_wf.on_line(line);
}

#if JC_FULL_CONTROL_TEST
bool JcApp::send_command(const char* bytes, size_t length) {
    switch (_tm.active()) {   // one control transport; the command policy sits in front of each of them
        case JcTransportKind::Ble: return _ble.send_command_bytes(bytes, length);
        case JcTransportKind::Uart: return _uart.running() && _uart.send_command_bytes(bytes, length);
        default: return _link.send_command_bytes(bytes, length);
    }
}
#endif

bool JcApp::WfHost::submit(const JcCommand& c, const char** why) {
    JcApp& a = _app;
    const JcDispatchContext ctx = a._sv.dispatch_context(true, true);   // the plan was confirmed once by the operator
#if JC_JOG_PREVIEW
    const uint32_t before = a._dispatcher.previewed();
#else
    const uint32_t before = a._dispatcher.sent();
#endif
    const JcAuthDecision d = a._dispatcher.submit(c, ctx);
    if (d != JcAuthDecision::Allowed) {
        *why = jc_auth_name(d);
        return false;
    }
#if JC_JOG_PREVIEW
    const bool accepted = a._dispatcher.previewed() != before;
#else
    const bool accepted = a._dispatcher.sent() != before;
#endif
    if (!accepted) {
        *why = "not transmitted (another operation is in progress, or the transport refused it)";
        return false;
    }
    a._wf_cmd_inflight = true;
    return true;
}

bool JcApp::WfHost::query(const char* line) {
    JcApp& a = _app;
#if JC_JOG_PREVIEW
    return a._preview.controller().query(line);   // the routine rehearses against the shared simulated FluidNC
#else
    return a.send_query(line);
#endif
}

void JcApp::WfHost::stop() {
    JcApp& a = _app;
    a._dispatcher.submit(jc_feed_hold(), a._sv.dispatch_context(false, true));   // software feed hold: always allowed when LIVE
}

JcWfStatus JcApp::wf_status(uint32_t now) {
    JcWfStatus s;
    const JcLiveMachineStatus& st = _model.status();
    s.fresh = _last_fresh && _sv.live_mode;
    s.state = _model.state();
    s.spindle = st.has_data ? st.spindle : 0.0f;
    s.pins = st.pins;
    s.sequence = st.sequence;
    bool all = true;
    for (uint8_t a = 0; a < 3; ++a) {
        float v = 0.0f;
        if (_model.position(a, JcRef::Work, v)) s.wpos[a] = v; else all = false;
    }
    s.wpos_valid = all;
    const JcMachineCaps& caps = _loader.caps();
    if (_loader.state() == JcLoadState::Complete && caps.complete) {
        s.probe_configured = (caps.probe_pin[0] != '\0' && strcmp(caps.probe_pin, "NO_PIN") != 0) ? JcTri::Yes : JcTri::No;
    }
#if JC_JOG_PREVIEW
    if (_preview.wf_engaged()) {   // the rehearsed routine moves the SIMULATED machine
        const JcSimNc& nc = _preview.controller().nc();
        for (uint8_t a = 0; a < 3; ++a) s.wpos[a] = nc.work(a);
        s.wpos_valid = true;
        s.state = nc.state;
        if (now - _wf_last_frame >= 100) { _wf_last_frame = now; ++_wf_seq; }
        s.sequence = 0x80000000u + _wf_seq;
    }
#else
    (void)now;
#endif
    return s;
}

bool JcApp::workflow_prepare(JcWfKind kind, const JcWfParams& params) {
    const uint32_t now = millis();
#if JC_JOG_PREVIEW
    const JcWfStatus real = wf_status(now);
    if (!_preview.wf_engaged() && real.wpos_valid) {
        _preview.engage(real.wpos);
        JcSimNc& nc = _preview.controller().nc();
        const JcSimFeature f = kind == JcWfKind::FindRotary ? JcSimFeature::Cylinder
                             : (params.feature == JcFeatureKind::Outside ? JcSimFeature::Boss : JcSimFeature::Bore);
        nc.centre_feature_here(f, 10.0f, params.cylinder_diameter > 0.0f ? params.cylinder_diameter : 20.0f);
    }
    if (_preview.wf_engaged() && (kind == JcWfKind::ToolReference || kind == JcWfKind::ToolMeasure || kind == JcWfKind::ToolChange)) {
        JcSimNc& nc = _preview.controller().nc();   // rehearsal: the "new" tool is 6.5 mm longer than the reference
        if (nc.feat != JcSimFeature::Plate) nc.plate_z = nc.m[2] - 15.0f - 20.0f;   // fixed once: the reference tool is 20 mm long
        nc.feat = JcSimFeature::Plate;
        nc.tool_len = params.has_ref ? 26.5f : 20.0f;
        nc.pins[0] = '\0';
    }
#endif
    return _wf.prepare(kind, params, wf_status(now), now);
}

void JcApp::service_workflow(uint32_t now) {
    _cmd.set_clock(now);
    _cmd.tick();
    JcCommandResult r;
    if (_cmd.take_result(r)) {
        char line[96];
        jc_result_line(r, line, sizeof(line));
        Serial.printf(JC_JOG_TAG ": %s\n", line);
        if (_wf_cmd_inflight) {
            _wf_cmd_inflight = false;
            _wf.on_result(r);
        } else {
            screen_for(_nav.current())->notify_result(line);
        }
    }
    _wf.on_status(wf_status(now));
    _wf.service(now);
    const JcWfState st = _wf.state();
    if (st != _wf_seen_state) {
        _wf_seen_state = st;
        if (st == JcWfState::Done) {
            const JcWfResult& r = _wf.result();
            if (_wf.kind() == JcWfKind::ToolReference && _wf.tlo_action() == 0 && r.tool_contact_ok) {
                _tool_store.set_ref(r.tool_contact_mz, _wf.params().plate_thickness, _wf.params().tool_dir, JC_JOG_PREVIEW != 0);
            }
#if !JC_JOG_PREVIEW
            if (_wf.tlo_action() != 0 && r.tlo_readback_done && r.tlo_readback_ok) {
                _tool_reader.note_tlo(r.tlo_read, _wf.tlo_action() == 1);   // FluidNC confirmed it by reading it back
                _tool_store.tlo_verified = true;
            }
#endif
        }
    }
}

void JcApp::workflow_open(JcWfKind kind) {
    if (_nav.current() != JcPage::Workflow && !_nav.go(JcPage::Workflow)) return;
    _workflow_screen.preselect(kind, _tool_store);
    _workflow_screen.enter();
}

bool JcApp::workflow_clear_tlo() {
    const uint32_t now = millis();
    if (_nav.current() != JcPage::Workflow && !_nav.go(JcPage::Workflow)) return false;
    _dispatcher.arm_for(now, 15u * 60u * 1000u, 0x0F);
    if (!_wf.request_clear_tlo(now)) {
        _dispatcher.disarm();
        return false;
    }
    _workflow_screen.show_active();
    _workflow_screen.enter();
    return true;
}

#endif  // JC_AUTHORITY

}  // namespace jc8048w550
