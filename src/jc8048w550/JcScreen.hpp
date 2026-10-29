// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Application services shared by every page, the host interface the pages use
// to navigate, and the JcScreen base class. The base class owns the one staged
// command flow used by all motion-capable pages:
//
//   page builds JcCommand(s)  ->  preview + confirmation dialog
//     ->  JcDispatcher (authorization gate)  ->  result dialog
//
// Pages never write to a socket or UART; the only path out is the dispatcher,
// and in this build the gate blocks every command (JC_MACHINE_CONTROL_ENABLED=0).

#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "JcCommandBuilder.hpp"
#include "JcDispatch.hpp"
#include "JcMachineModel.hpp"
#include "JcNav.hpp"
#include "JcReadOnlyFluidNc.hpp"
#include "JcSettings.hpp"
#include "JcUiKit.hpp"

namespace jc8048w550 {

enum class JcLeaveTarget : uint8_t {
    Dashboard,
    Connections,     // legacy transport-selection page
    WifiSettings,    // legacy Wi-Fi page
    Calibration,     // guided touch calibration
};

struct JcInputRequest {
    const char* prompt;
    char        initial[64];
    bool        numeric;
    uint8_t     max_len;
    // Called after the input page has closed. ok=false on cancel.
    void (*done)(void* user, bool ok, const char* text);
    void* user;
};

class JcAppHost {
public:
    virtual ~JcAppHost() = default;
    virtual void go(JcPage page) = 0;
    virtual void back() = 0;
    virtual void request_input(const JcInputRequest& request) = 0;
    virtual void save_settings() = 0;       // persist + refresh derived state
    virtual void apply_brightness() = 0;
    virtual void leave(JcLeaveTarget target) = 0;
};

struct JcAppServices {
    Arduino_GFX&       display;
    JcReadOnlyFluidNc& link;
    JcMachineModel&    model;
    JcUserSettings&    settings;
    JcMachineProfile&  profile;
    JcDispatcher&      dispatcher;
    const bool&        live_mode;
    JcAppHost&         host;

    bool live_fresh() const { return live_mode && model.fresh(); }

    JcDispatchContext dispatch_context(bool confirmed) const {
        JcDispatchContext c;
        c.live = live_mode;
        c.link_fresh = model.fresh();
        c.machine_state = model.state();
        c.operator_confirmed = confirmed;
        c.now_ms = millis();
        return c;
    }
};

const char* jc_cmd_error_name(JcCmdError e);

class JcScreen {
public:
    explicit JcScreen(JcAppServices& services) : sv(services) {}
    virtual ~JcScreen() = default;

    virtual JcPage page() const = 0;
    void enter();
    void draw();
    void touch(uint16_t x, uint16_t y);
    void tick(uint32_t now);

protected:
    virtual void on_enter() {}
    virtual void on_draw() = 0;                 // body below the 64 px header
    virtual void on_touch(uint16_t x, uint16_t y) = 0;
    virtual void on_tick(uint32_t now) { (void)now; }

    // Stage one command / a sequence behind the preview + confirm dialog.
    void stage(const JcCommand& command, const char* title, const char* const* notes = nullptr,
               uint8_t note_count = 0);
    void stage_sequence(const JcCommandSequence& sequence, const char* title,
                        const char* const* notes = nullptr, uint8_t note_count = 0);
    // Immediately record a command with the dispatcher (no dialog); returns the decision.
    JcAuthDecision submit_now(const JcCommand& command);
    void show_message(const char* title, const char* line1, const char* line2 = nullptr,
                      const char* line3 = nullptr);

    JcAppServices& sv;
    ui::Dialog     dialog;

private:
    void open_confirm_dialog(const char* title, const char* const* notes, uint8_t note_count);
    void run_pending();

    JcCommand         _command;
    JcCommandSequence _sequence;
    bool              _pending_is_sequence = false;
    bool              _has_pending = false;
};

}  // namespace jc8048w550
