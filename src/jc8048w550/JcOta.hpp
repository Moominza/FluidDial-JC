// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

// Wi-Fi OTA update policy (Instruction 020). Pure logic: no I/O, no Arduino. The platform glue
// (JcOtaService.cpp) feeds it events from the stock ArduinoOTA library and acts on its decisions.
//
// Model (revised at the owner's request: no manual arming). The update service is available whenever an OTA
// password is set and the controller is verifiably quiet (Wi-Fi up, fresh status, exactly Idle, nothing pending);
// it closes by itself the moment that stops being true. A password is always required. Machine-changing commands
// are locked out on every transport only from the moment an authenticated transfer starts. Five wrong passwords
// pause the service for a cool-down. Nothing here sends a CNC command, and nothing here is a second motion
// controller.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace jc8048w550 {

// The marker every JC image carries (JcOtaService.cpp). An image without it - an M5Dial/CYD build, or any
// arbitrary file - is refused and the previous image stays the boot image. This is a compatibility check,
// not a signature: it does not authenticate who built the image.
#define JC_OTA_IMAGE_MARKER "FDJC-IMAGE-TARGET:JC8048W550C_I:ESP32-S3:V1"

constexpr uint32_t kJcOtaCooldownMs = 5u * 60u * 1000u;   // service pause after too many wrong passwords
constexpr uint32_t kJcOtaFailureShowMs = 60u * 1000u;     // a failed update stays on screen this long, then the service is offered again
constexpr uint32_t kJcOtaRestartDelayMs = 3000;            // lets the screen show "restarting"
constexpr uint8_t  kJcOtaMaxAuthFailures = 5;              // wrong passwords before the service pauses
constexpr uint8_t  kJcOtaMinPassword = 8;
constexpr uint8_t  kJcOtaMaxPassword = 32;

enum class JcOtaState : uint8_t { Off, Ready, Receiving, Validating, PendingRestart, Failed };

inline const char* jc_ota_state_name(JcOtaState s) {
    switch (s) {
        case JcOtaState::Off: return "UNAVAILABLE";
        case JcOtaState::Ready: return "READY - waiting for upload";
        case JcOtaState::Receiving: return "RECEIVING FIRMWARE";
        case JcOtaState::Validating: return "VALIDATING";
        case JcOtaState::PendingRestart: return "UPDATE OK - RESTARTING";
        case JcOtaState::Failed: return "FAILED";
    }
    return "?";
}

// Everything the interlock looks at, assembled by the application from live state. Nothing is assumed:
// every field defaults to the unsafe answer.
struct JcOtaInputs {
    bool        wifi_up = false;
    bool        password_set = false;
    bool        password_applied = true;     // the library holds the CURRENT password (false after a change until restart)
    bool        status_fresh = false;        // a valid, fresh status from the controller on the active transport
    const char* machine_state = "";          // as reported; "" when unknown
    bool        workflow_active = false;     // a guided positioning / probing / tool routine
    bool        jog_unresolved = false;      // a jog or jog-cancel awaiting its outcome
    bool        command_pending = false;     // a machine command awaiting its reply
};

// nullptr = OTA may be offered. Otherwise the reason, in words for the operator. Missing status is never Idle.
inline const char* jc_ota_blocker(const JcOtaInputs& in) {
    if (!in.wifi_up) return "Wi-Fi is not connected";
    if (!in.password_set) return "set an OTA password first";
    if (!in.password_applied) return "restart the pendant to apply the new password";
    if (!in.status_fresh) return "controller status is missing or stale";
    if (in.machine_state == nullptr || strcmp(in.machine_state, "Idle") != 0) return "the controller is not Idle";
    if (in.workflow_active) return "a guided routine is active";
    if (in.jog_unresolved) return "a jog is unresolved";
    if (in.command_pending) return "a machine command is awaiting its reply";
    return nullptr;
}

// 8..32 printable ASCII characters, no spaces. Never logged, never shown after entry.
inline bool jc_ota_password_valid(const char* p) {
    if (p == nullptr) return false;
    const size_t n = strlen(p);
    if (n < kJcOtaMinPassword || n > kJcOtaMaxPassword) return false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = static_cast<uint8_t>(p[i]);
        if (c <= 0x20 || c > 0x7E) return false;
    }
    return true;
}

// ArduinoOTA's error classes (ota_error_t) without including the library.
enum class JcOtaError : uint8_t { Auth = 0, Begin = 1, Connect = 2, Receive = 3, End = 4 };

// Finds the image marker in a stream delivered in arbitrary chunks (the new image is read back from flash).
// Linear time (Knuth-Morris-Pratt): the new image is a megabyte or more and is scanned on the device.
class JcOtaMarkerScanner {
public:
    JcOtaMarkerScanner() {
        const char* m = JC_OTA_IMAGE_MARKER;
        const size_t ml = strlen(m);
        _fail[0] = 0;
        size_t k = 0;
        for (size_t i = 1; i < ml; ++i) {
            while (k > 0 && m[i] != m[k]) k = _fail[k - 1];
            if (m[i] == m[k]) ++k;
            _fail[i] = static_cast<uint8_t>(k);
        }
    }
    void reset() { _k = 0; _found = false; }
    void feed(const uint8_t* data, size_t n) {
        const char* m = JC_OTA_IMAGE_MARKER;
        const size_t ml = strlen(m);
        for (size_t i = 0; i < n && !_found; ++i) {
            const char c = static_cast<char>(data[i]);
            while (_k > 0 && c != m[_k]) _k = _fail[_k - 1];
            if (c == m[_k]) ++_k;
            if (_k == ml) _found = true;
        }
    }
    bool found() const { return _found; }

private:
    uint8_t _fail[sizeof(JC_OTA_IMAGE_MARKER)] {};
    size_t  _k = 0;
    bool    _found = false;
};

class JcOtaController {
public:
    JcOtaState state() const { return _state; }
    bool locked() const {   // machine-changing commands are refused while true (only once a transfer has begun)
        return _state == JcOtaState::Receiving || _state == JcOtaState::Validating || _state == JcOtaState::PendingRestart;
    }
    bool service_wanted() const { return _state == JcOtaState::Ready || _state == JcOtaState::Receiving; }
    const char* message() const { return _msg; }
    uint32_t progress_done() const { return _done; }
    uint32_t progress_total() const { return _total; }
    uint8_t percent() const { return _total == 0 ? 0 : static_cast<uint8_t>((static_cast<uint64_t>(_done) * 100u) / _total); }
    uint8_t auth_failures() const { return _auth_failures; }
    bool paused() const { return _paused; }
    bool boot_unsafe() const { return _boot_bad; }   // the boot slot could not be put back: do not restart

    // Every pass. Offers the service while the controller is quiet and withdraws it the moment it is not.
    void tick(const JcOtaInputs& in, uint32_t now) {
        if (_paused) {
            if (!_pause_set) { _pause_until = now + kJcOtaCooldownMs; _pause_set = true; }
            else if (static_cast<int32_t>(now - _pause_until) >= 0) {
                _paused = false;
                _pause_set = false;
                _auth_failures = 0;
                set_message("");
            }
        }
        if (_state == JcOtaState::Failed) {
            if (!_fail_seen) { _fail_seen = true; _fail_at = now; }
            else if (static_cast<int32_t>(now - _fail_at) >= static_cast<int32_t>(kJcOtaFailureShowMs)) acknowledge_failure();
        } else {
            _fail_seen = false;
        }
        if (_state == JcOtaState::Off) {
            if (!_paused && jc_ota_blocker(in) == nullptr) {
                _state = JcOtaState::Ready;
                set_message("Send the firmware from the PC (FluidDial Updater)");
            }
            return;
        }
        if (_state != JcOtaState::Ready) return;
        JcOtaInputs quiet = in;
        quiet.password_applied = true;   // the library already holds the password it was started with
        const char* why = jc_ota_blocker(quiet);
        if (why != nullptr) {
            _state = JcOtaState::Off;
            set_message("");
        }
    }

    // Operator action: nothing to arm any more; this only clears a failure message so the service is offered again.
    void acknowledge_failure() {
        if (_state == JcOtaState::Failed) {
            if (_boot_bad) return;   // never hide this one: a restart would boot the wrong image
            _state = JcOtaState::Off;
            _fail_seen = false;
            set_message("");
        }
    }

    // ---- events from the platform OTA library -------------------------------------------------------------

    // An authenticated transfer begins. false = refuse it (the platform glue aborts the write): the service was not
    // being offered, so the controller was not quiet.
    bool on_start() {
        if (_state != JcOtaState::Ready) return false;
        _state = JcOtaState::Receiving;
        _done = _total = 0;
        _auth_failures = 0;
        set_message("Receiving firmware...");
        return true;
    }
    void on_progress(uint32_t done, uint32_t total) {
        if (_state != JcOtaState::Receiving) return;
        _done = done;
        _total = total;
    }
    // The library finished writing and its own image verification passed; our target check follows.
    void on_transfer_complete() {
        if (_state != JcOtaState::Receiving) return;
        _state = JcOtaState::Validating;
        set_message("Checking the image target...");
    }
    void on_validated(bool target_ok, uint32_t now) {
        if (_state != JcOtaState::Validating) return;
        if (target_ok) {
            _state = JcOtaState::PendingRestart;
            _restart_at = now + kJcOtaRestartDelayMs;
            set_message("Update installed and verified. Restarting...");
        } else {
            _state = JcOtaState::Failed;
            set_message("REJECTED: not a JC8048W550C_I image. Old firmware kept.");
        }
    }
    // The image written so far is not a JC image. Called BEFORE the boot slot is switched; the transfer is aborted.
    void on_target_rejected() {
        if (_state != JcOtaState::Receiving) return;
        _state = JcOtaState::Failed;
        set_message("REJECTED: not a JC8048W550C_I image. Old firmware kept.");
    }
    // Last-resort path: an image was committed, failed the target check and the boot slot could not be restored.
    void on_boot_not_restored() {
        _state = JcOtaState::Failed;
        _boot_bad = true;
        set_message("BOOT SLOT NOT RESTORED - do not restart; reinstall by USB");
    }
    void on_error(JcOtaError e) {
        if (e == JcOtaError::Auth) {
            ++_auth_failures;
            if (_auth_failures >= kJcOtaMaxAuthFailures) {
                _paused = true;
                _pause_set = false;
                if (_state == JcOtaState::Ready) _state = JcOtaState::Off;
                set_message("Too many wrong passwords: update paused for 5 minutes");
            } else if (_state == JcOtaState::Ready) {
                set_message("Wrong password refused");
            }
            return;
        }
        if (_state == JcOtaState::Failed || _state == JcOtaState::Off) return;
        _state = JcOtaState::Failed;
        switch (e) {
            case JcOtaError::Begin: set_message("Rejected: image too large or no space. Old firmware kept."); break;
            case JcOtaError::Connect: set_message("Upload connection failed. Old firmware kept."); break;
            case JcOtaError::Receive: set_message("Transfer interrupted. Old firmware kept."); break;
            case JcOtaError::End: set_message("Image failed verification. Old firmware kept."); break;
            default: set_message("Update failed. Old firmware kept."); break;
        }
    }
    bool restart_due(uint32_t now) const {
        return _state == JcOtaState::PendingRestart && static_cast<int32_t>(now - _restart_at) >= 0;
    }

private:
    void set_message(const char* m) { strncpy(_msg, m, sizeof(_msg) - 1); _msg[sizeof(_msg) - 1] = '\0'; }
    void snprintf_msg(const char* a, const char* b) {
        set_message(a);
        const size_t used = strlen(_msg);
        strncpy(_msg + used, b, sizeof(_msg) - 1 - used);
        _msg[sizeof(_msg) - 1] = '\0';
    }

    JcOtaState _state = JcOtaState::Off;
    uint32_t   _pause_until = 0;
    bool       _paused = false, _pause_set = false;
    bool       _fail_seen = false;
    bool       _boot_bad = false;
    uint32_t   _fail_at = 0;
    uint32_t   _restart_at = 0;
    uint32_t   _done = 0, _total = 0;
    uint8_t    _auth_failures = 0;
    char       _msg[72] {};
};

// A snapshot for the touchscreen page (no pointers into live state).
struct JcOtaView {
    JcOtaState state = JcOtaState::Off;
    char       message[72] = "";
    char       blocker[64] = "";     // why the update is not offered right now; empty = offered
    char       ip[20] = "";
    char       host[24] = "";
    char       rollback[40] = "";
    uint8_t    percent = 0;
    uint8_t    auth_failures = 0;
    bool       wifi_up = false;
    bool       password_set = false;
    bool       locked = false;
};

}  // namespace jc8048w550
