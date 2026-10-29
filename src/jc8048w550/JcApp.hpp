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
    void feed_status(const JcLiveMachineStatus& status) { _model.on_status(status); }
    void note_link_fresh(bool fresh);  // drops arming on every loss of fresh data

    JcMachineModel& model() { return _model; }
    JcUserSettings& settings() { return _settings; }
    const JcUserSettings& settings() const { return _settings; }
    JcDispatcher& dispatcher() { return _dispatcher; }

    // JcAppHost
    void go(JcPage page) override;
    void back() override;
    void request_input(const JcInputRequest& request) override;
    void save_settings() override;
    void apply_brightness() override;
    void leave(JcLeaveTarget target) override;

private:
    JcScreen* screen_for(JcPage page);

    JcUserSettings _settings;
    JcMachineProfile _profile;
    JcMachineModel _model;
    JcDispatcher _dispatcher;
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
    JcMachineScreen _machine;
    JcDiagnosticsScreen _diagnostics;
    JcInputScreen _input;
    LeaveFn _leave_fn = nullptr;
    void* _leave_user = nullptr;
    bool _last_fresh = false;
};

}  // namespace jc8048w550
