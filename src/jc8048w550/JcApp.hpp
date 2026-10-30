// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// JcApp: the multi-page application shell. Owns navigation, the machine model,
// user settings (NVS), the command dispatcher and every page. PendantUi keeps
// the dashboard, touch calibration and the Wi-Fi page and delegates to JcApp
// while the application menu is open.

#pragma once

#include "JcScreens.hpp"

namespace jc8048w550 {

class JcApp : public JcAppHost {
public:
    using LeaveFn = void (*)(void* user, JcLeaveTarget target);

    JcApp(Arduino_GFX& display, JcReadOnlyFluidNc& link, JcUartLink& uart, const bool& live_mode);

    void begin(LeaveFn leave, void* user);
    void open();                       // reset to the menu and draw
    void open_settings();              // menu > settings (used after calibration)
    void resume();                     // redraw the current page after a legacy page
    void touch(uint16_t x, uint16_t y);
    void tick();
    // `fresh`: the status stream the frame came from is fresh right now.
    void feed_status(const JcLiveMachineStatus& status, bool fresh);
    void note_link_fresh(bool fresh);  // drops arming on every loss of fresh data
    void service();                    // capability-read pump; call every main-loop pass

    JcMachineModel& model() { return _model; }
    JcUserSettings& settings() { return _settings; }
    const JcUserSettings& settings() const { return _settings; }
    JcDispatcher& dispatcher() { return _dispatcher; }
#if JC_AUTHORITY
    // Prepares a routine: reads $G / $# (read-only), then the preview is shown. In the preview build the
    // routine runs against the shared simulated FluidNC, started from the real work position.
    bool workflow_prepare(JcWfKind kind, const JcWfParams& params) override;
#endif

    // JcAppHost
    void go(JcPage page) override;
    void back() override;
    void request_input(const JcInputRequest& request) override;
    void save_settings() override;
    void apply_brightness() override;
    void leave(JcLeaveTarget target) override;
    const char* start_caps_read() override;
    void cancel_caps_read() override;

private:
    JcScreen* screen_for(JcPage page);
    bool send_query(const char* line);
    void refresh_profile();
    void on_caps_finished();
    void on_controller_line(const char* line);
    static void line_thunk(void* user, const char* line) { static_cast<JcApp*>(user)->on_controller_line(line); }
    bool uart_active() const;
#if JC_AUTHORITY
    class WfHost : public JcWfHost {
    public:
        explicit WfHost(JcApp& app) : _app(app) {}
        bool submit(const JcCommand& c, const char** why) override;
        bool query(const char* line) override;
        void stop() override;
    private:
        JcApp& _app;
    };
    JcWfStatus wf_status(uint32_t now);
    void service_workflow(uint32_t now);
    static void cmd_line_thunk(void* user, const char* line, uint32_t now);
#if JC_FULL_CONTROL_TEST
    static bool cmd_tx_thunk(void* user, const char* bytes, size_t length) {
        return static_cast<JcApp*>(user)->send_command(bytes, length);
    }
    bool send_command(const char* bytes, size_t length);
#endif
#endif
#if JC_LIVE_JOG_TEST
    // EXPERIMENTAL live-jog build only: the transmit function handed to JcJogSession.
    static bool jog_tx_thunk(void* user, const char* bytes, size_t length) {
        return static_cast<JcApp*>(user)->send_jog(bytes, length);
    }
    bool send_jog(const char* bytes, size_t length);
#endif

    JcReadOnlyFluidNc& _link;
    JcUartLink& _uart;
    JcUserSettings _settings;
    JcMachineProfile _profile;
    JcMachineModel _model;
    JcDispatcher _dispatcher;   // a transmit sink is installed ONLY in the live-jog test build
    JcCapsLoader _loader;
#if JC_JOG_PREVIEW
    JcPreviewBridge _preview;   // SIMULATED controller: the only jog transport in the ordinary firmware
    JcJogStatus _real;          // latest REAL read-only status, as the session would see it
#endif
#if JC_AUTHORITY
    JcCommandSystem _cmd;       // ONE command interface for every transport; the policy lives in front of them
#endif
    JcJogSession _jogs;         // transport: none (ordinary), the simulation (preview), the socket (live-jog test build)
#if JC_AUTHORITY
    WfHost _wfhost { *this };
    JcWorkflow _wf { &_wfhost };
    bool _wf_cmd_inflight = false;       // the command awaiting its reply belongs to the routine
    uint32_t _wf_seq = 0, _wf_last_frame = 0;
#endif
    JcNav _nav;
    JcAppServices _sv;
    JcMenuScreen _menu;
    JcJogScreen _jog;
    JcHomingScreen _homing;
    JcProbingScreen _probing;
    JcOffsetsScreen _offsets;
    JcFilesScreen _files;
    JcMacrosScreen _macros;
    JcToolsScreen _tools;
    JcSettingsScreen _settings_screen;
    JcConnectionsScreen _connections;
    JcDisplaySettingsScreen _display_settings;
    JcJogSettingsScreen _jog_settings;
#if JC_AUTHORITY
    JcWorkflowScreen _workflow_screen;
#endif
    JcMachineScreen _machine;
    JcCapsScreen _caps_screen;
    JcLimitsScreen _limits_screen;
    JcDryRunScreen _dry_run;
#if JC_JOG_PREVIEW
    JcJogPreviewScreen _jog_preview;
#endif
    JcDiagnosticsScreen _diagnostics;
    JcInputScreen _input;
    LeaveFn _leave_fn = nullptr;
    void* _leave_user = nullptr;
    bool _last_fresh = false;
};

}  // namespace jc8048w550
