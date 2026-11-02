// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Platform glue between JcOtaController (policy) and the stock Arduino ESP32 OTA stack (ArduinoOTA + Update +
// esp_ota_*). No custom flash writer and no custom cryptography: the espota protocol (password challenge) and the
// ESP-IDF image verification are the stock ones. See docs/OTA_UPDATES.md for the exact security properties.

#pragma once

#include <stdint.h>

#include "JcOta.hpp"

namespace jc8048w550 {

class JcOtaService {
public:
    // Called (from inside the library's receive loop) so the screen can show progress during a transfer.
    using ProgressDraw = void (*)(void* user);

    void begin(JcOtaController* controller, ProgressDraw draw, void* user);
    bool wifi_up() const;
    bool password_set() const { return _pw[0] != '\0'; }
    bool password_applied() const { return !_begun_once || !_pw_changed_since_begin; }
    // nullptr = stored. Never logged.
    const char* set_password(const char* password);
    void clear_password();
    // Starts or stops the library according to the controller policy. Call every main-loop pass.
    void service(uint32_t now);
    bool listening() const { return _listening; }
    // Boot health: after a short, healthy uptime a freshly installed image is confirmed (cancels the rollback).
    void confirm_boot_if_healthy(uint32_t now);
    const char* rollback_text() const { return _rollback_text; }
    const char* hostname() const;
    void restart();
    uint32_t boot_validated() const { return _validated; }

private:
    friend struct JcOtaCallbacks;
    void start();
    void stop();

    JcOtaController* _ctl = nullptr;
    ProgressDraw     _draw = nullptr;
    void*            _draw_user = nullptr;
    char             _pw[kJcOtaMaxPassword + 1] {};
    bool             _listening = false;
    bool             _begun_once = false;
    bool             _pw_changed_since_begin = false;
    uint32_t         _validated = 0;
    char             _rollback_text[40] = "unknown";
    bool             _confirmed = false;
};

}  // namespace jc8048w550
