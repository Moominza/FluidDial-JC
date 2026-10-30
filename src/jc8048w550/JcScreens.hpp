// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Declarations of the JC application pages. Each page has its own .cpp file
// (JcScreen<Name>.cpp). Pages only read machine state from JcMachineModel and
// only issue commands through JcScreen::stage*/submit_now (-> JcDispatcher).

#pragma once

#include "JcScreen.hpp"
#include "JcTransport.h"
#include "JcUartLink.hpp"

namespace jc8048w550 {

// Shared formatting helpers (JcScreenUtil.cpp)
const char* jc_axis_name(uint8_t axis);
const char* jc_axis_unit(uint8_t axis);  // "mm" or "deg"
void jc_format_position(const JcAppServices& sv, uint8_t axis, JcRef ref, char* out, size_t size,
                        bool* available = nullptr);
void jc_format_step(char* out, size_t size, float value, uint8_t axis);
const char* jc_link_summary(const JcAppServices& sv);

class JcMenuScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Menu; }

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
};

// The line under the jog panel in the experimental build. It must fit ONE line of the 744 px
// panel text area (tests/host/ui_render.cpp checks the width); two lines overflow the panel.
#if JC_JOG_PREVIEW
constexpr const char* kJcLiveJogWarning = "PREVIEW ONLY - MOTION DISABLED. Real use: cancel is NOT an E-stop. Keep the E-stop in reach.";
#else
constexpr const char* kJcLiveJogWarning = "Cancel is NOT an E-stop and fails without communication. Keep the E-stop in reach.";
#endif

class JcJogScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Jog; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;
    void on_custom_confirm() override;

private:
    void draw_axes();
    void draw_steps();
    void draw_feeds();
    void draw_mode();
    void draw_live();
    void draw_bottom();
    void do_jog(int direction);
    JcCapsFlow caps_flow(uint32_t now) const;
    void read_settings();
    void service_caps(uint32_t now);
    float step_value() const;
    float feed_value() const;
    // Experimental live-jog build only (defined under JC_LIVE_JOG_TEST).
    void jog_live(int direction);
    void request_arm();
    void do_cancel();
    const char* arm_blocker() const;

    uint8_t _axis = 0;
    uint8_t _arm_axis = 0;   // axis the arming dialog was opened for
    uint8_t _step = 1;
    uint8_t _feed = 0;
    bool _continuous = false;
    uint32_t _last_tick = 0;
    uint32_t _live_key = 0;
    bool _auto_tried = false;     // an automatic capability read was already started during this visit
    uint8_t _drawn_step = 0xFF;   // read progress last drawn on the ARM / READ button
    ui::Slot _dro, _motion, _preview, _result, _caps;
};

class JcHomingScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Homing; }

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void draw_status();
    uint32_t _last_tick = 0;
    ui::Slot _state, _pre1, _pre2, _pre3;
};

class JcProbingScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Probing; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    enum Mode : uint8_t { ModeZ, ModeXNeg, ModeXPos, ModeYNeg, ModeYPos, ModeCount };
    void draw_modes();
    void draw_params();
    void draw_probe_status();
    void begin_edit(uint8_t field);
    float* field_ptr(uint8_t field);
    void run_sequence();

    uint8_t _mode = ModeZ;
    uint8_t _edit_field = 0;
    uint32_t _last_tick = 0;
    ui::Slot _pin;
};

class JcOffsetsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Offsets; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void draw_values();
    uint8_t _edit_axis = 0;
    uint32_t _last_tick = 0;
    ui::Slot _machine[4], _work[4], _wco;
};

class JcFilesScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Files; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void draw_job();
    char _path[64] {};
    uint32_t _last_tick = 0;
    ui::Slot _job, _path_slot;
    int16_t _last_bar = -1;
};

class JcMacrosScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Macros; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    void draw_list();
    void next_edit_stage();
    uint8_t _selected = 0;
    uint8_t _edit_stage = 0;  // 0 name, 1 path, 2 description
    JcMacroSlot _editing {};
};

class JcToolsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Tools; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    void draw_steps();
    void draw_detail();
    uint8_t _step = 0;
    bool _done[6] {};
};

class JcSettingsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Settings; }

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
};

class JcConnectionsScreen : public JcScreen {
public:
    JcConnectionsScreen(JcAppServices& services, JcUartLink& uart) : JcScreen(services), _uart(uart) {}
    JcPage page() const override { return JcPage::SetConnections; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void select_transport(uint8_t transport);
    void draw_status();
    JcUartLink& _uart;
    uint8_t _edit_pin = 0;  // 0 rx, 1 tx
    uint32_t _last_tick = 0;
    ui::Slot _wifi, _uart_state;
};

class JcDisplaySettingsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::SetDisplay; }

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    void draw_values();
};

class JcJogSettingsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::SetJogging; }
    void edit_done(bool ok, const char* text);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    float* value_ptr(uint8_t row, uint8_t index);
    void draw_values();
    uint8_t _edit_row = 0;
    uint8_t _edit_index = 0;
};

class JcMachineScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::SetMachine; }

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    void draw_toggle();
};

class JcCapsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Capabilities; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    // full: clear the area and repaint every item; otherwise only items whose
    // text changed are repainted (no flashing while the read progresses).
    void draw_values(bool full);
    ui::Slot _cells[4][5];
    ui::Slot _lines[5];
    uint32_t _last_tick = 0;
    uint32_t _drawn_key = 0;  // redraw the value area only when the loader state changes
};

class JcLimitsScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::LimitsHoming; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void draw_values(bool full);
    ui::Slot _cells[4][5];
    ui::Slot _lines[6];
    uint32_t _last_tick = 0;
    uint32_t _drawn_key = 0;
};

#if JC_JOG_PREVIEW
// Non-motion jog test mode: choose how the SIMULATED controller behaves and see what the gate and the
// simulation counted. Nothing on this page can transmit anything.
class JcJogPreviewScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::JogPreview; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;
    void on_tick(uint32_t now) override;

private:
    void draw_values();
    ui::Slot _scenario;
    ui::Slot _lines[4];
    uint32_t _last_tick = 0;
};
#endif

class JcDryRunScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::DryRun; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    static constexpr uint8_t kActions = 8;
    void draw_actions();
    void draw_report();
    void build_report(uint8_t action);
    uint8_t _selected = 0;
    JcDryRunReport _report;
};

class JcDiagnosticsScreen : public JcScreen {
public:
    JcDiagnosticsScreen(JcAppServices& services, JcUartLink& uart) : JcScreen(services), _uart(uart) {}
    JcPage page() const override { return JcPage::Diagnostics; }

protected:
    void on_enter() override;
    void on_draw() override;
    void on_touch(uint16_t, uint16_t) override {}
    void on_tick(uint32_t now) override;

private:
    static constexpr uint8_t kRows = 16;
    void row_text(uint8_t row, char* label, size_t label_size, char* value, size_t value_size) const;
    void draw_rows();
    JcUartLink& _uart;
    uint32_t _last_tick = 0;
    ui::Slot _slots[kRows];
    ui::Slot _history, _dispatch;
};

class JcInputScreen : public JcScreen {
public:
    using JcScreen::JcScreen;
    JcPage page() const override { return JcPage::Input; }
    void begin(const JcInputRequest& request);

protected:
    void on_draw() override;
    void on_touch(uint16_t x, uint16_t y) override;

private:
    void draw_value();
    void finish(bool ok);
    void press(const char* key);
    JcInputRequest _request {};
    char _value[64] {};
    uint8_t _length = 0;
    bool _shift = false;
    bool _finished = false;
};

}  // namespace jc8048w550
