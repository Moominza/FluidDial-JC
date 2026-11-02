// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include "JcOtaService.hpp"

#include <ArduinoOTA.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace jc8048w550 {

namespace {
constexpr const char* kNs = "jc_ota";
constexpr const char* kKeyPw = "pw";
constexpr const char* kHostname = "fluiddial-jc";
constexpr uint32_t kHealthyUptimeMs = 15000;   // a new image must run this long before the rollback is cancelled

// Every JC image carries this; the OTA target check looks for it in the freshly written image.
__attribute__((used)) const char kImageMarker[] = JC_OTA_IMAGE_MARKER;

JcOtaService* g_svc = nullptr;
uint32_t g_total = 0;

// Reads the image just written back from flash and looks for the target marker.
bool new_image_has_marker(const esp_partition_t* part, uint32_t length) {
    JcOtaMarkerScanner scan;
    uint8_t buf[512];
    const uint32_t limit = (length == 0 || length > part->size) ? part->size : length;
    for (uint32_t off = 0; off < limit && !scan.found(); off += sizeof(buf)) {
        const uint32_t n = (limit - off) < sizeof(buf) ? (limit - off) : sizeof(buf);
        if (esp_partition_read(part, off, buf, n) != ESP_OK) return false;
        scan.feed(buf, n);
    }
    return scan.found();
}
}  // namespace

struct JcOtaCallbacks {
    static void on_start() {
        if (g_svc && g_svc->_ctl) g_svc->_ctl->on_start();
        Serial.println("ota: transfer started");
        if (g_svc && g_svc->_draw) g_svc->_draw(g_svc->_draw_user);
    }
    static void on_progress(unsigned int done, unsigned int total) {
        if (!g_svc || !g_svc->_ctl) return;
        g_total = total;
        const uint8_t before = g_svc->_ctl->percent();
        g_svc->_ctl->on_progress(done, total);
        if (g_svc->_ctl->percent() != before && g_svc->_draw) g_svc->_draw(g_svc->_draw_user);
    }
    static void on_end() {
        // The stock library has written the image and its own verification (esp_ota_end: image header, segment
        // checksums, SHA-256, chip id) succeeded and the boot partition was switched. Our target check follows; on
        // failure the boot partition is put back, so the old image stays the boot image.
        if (!g_svc || !g_svc->_ctl) return;
        g_svc->_ctl->on_transfer_complete();
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* boot = esp_ota_get_boot_partition();
        bool ok = boot != nullptr && running != nullptr && boot != running && new_image_has_marker(boot, g_total);
        if (!ok && boot != nullptr && running != nullptr && boot != running) {
            esp_ota_set_boot_partition(running);
        }
        g_svc->_ctl->on_validated(ok, millis());
        Serial.printf("ota: image target check %s\n", ok ? "passed" : "FAILED (old image kept)");
        if (g_svc->_draw) g_svc->_draw(g_svc->_draw_user);
    }
    static void on_error(ota_error_t e) {
        if (!g_svc || !g_svc->_ctl) return;
        g_svc->_ctl->on_error(static_cast<JcOtaError>(e));
        Serial.printf("ota: error class %d\n", static_cast<int>(e));
        if (g_svc->_draw) g_svc->_draw(g_svc->_draw_user);
    }
};

// Weak hook of the Arduino core: do NOT mark a freshly installed image valid at start-up. We confirm it ourselves
// after a healthy uptime (confirm_boot_if_healthy); a crash or reset before that rolls back to the previous image.
extern "C" bool verifyRollbackLater() { return true; }

void JcOtaService::begin(JcOtaController* controller, ProgressDraw draw, void* user) {
    _ctl = controller;
    _draw = draw;
    _draw_user = user;
    g_svc = this;
    Preferences p;
    if (p.begin(kNs, true)) {
        p.getString(kKeyPw, _pw, sizeof(_pw));
        p.end();
    }
    if (!jc_ota_password_valid(_pw)) _pw[0] = '\0';
    // Rollback status of the running image (read-only).
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr && esp_ota_get_state_partition(running, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_PENDING_VERIFY) snprintf(_rollback_text, sizeof(_rollback_text), "new image, pending confirm");
        else if (st == ESP_OTA_IMG_VALID) snprintf(_rollback_text, sizeof(_rollback_text), "confirmed");
        else snprintf(_rollback_text, sizeof(_rollback_text), "USB-installed (no rollback)");
        Serial.printf("ota: running slot %s, image state %d, OTA password %s\n", running->label, static_cast<int>(st),
                      password_set() ? "set" : "NOT set");
    }
}

bool JcOtaService::wifi_up() const { return WiFi.status() == WL_CONNECTED; }

const char* JcOtaService::hostname() const { return kHostname; }

const char* JcOtaService::set_password(const char* password) {
    if (!jc_ota_password_valid(password)) return "8-32 characters, no spaces";
    Preferences p;
    if (!p.begin(kNs, false)) return "storage unavailable";
    const size_t n = p.putString(kKeyPw, password);
    p.end();
    if (n == 0) return "could not store the password";
    if (strcmp(_pw, password) != 0 && _begun_once) _pw_changed_since_begin = true;
    strncpy(_pw, password, sizeof(_pw) - 1);
    _pw[sizeof(_pw) - 1] = '\0';
    Serial.println("ota: password changed");   // the value is never logged
    return nullptr;
}

void JcOtaService::clear_password() {
    Preferences p;
    if (p.begin(kNs, false)) {
        p.remove(kKeyPw);
        p.end();
    }
    if (_pw[0] != '\0' && _begun_once) _pw_changed_since_begin = true;
    memset(_pw, 0, sizeof(_pw));
}

void JcOtaService::start() {
    if (_listening || !password_set() || !password_applied()) return;
    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(_pw);          // the stock challenge/response (MD5); no anonymous upload is possible
    ArduinoOTA.setRebootOnSuccess(false); // our target check runs first; the restart is ours
    ArduinoOTA.onStart(&JcOtaCallbacks::on_start);
    ArduinoOTA.onProgress(&JcOtaCallbacks::on_progress);
    ArduinoOTA.onEnd(&JcOtaCallbacks::on_end);
    ArduinoOTA.onError(&JcOtaCallbacks::on_error);
    ArduinoOTA.begin();
    _begun_once = true;
    _pw_changed_since_begin = false;
    _listening = true;
    Serial.printf("ota: service listening (%s.local:3232, password required)\n", kHostname);
}

void JcOtaService::stop() {
    if (!_listening) return;
    ArduinoOTA.end();
    _listening = false;
    Serial.println("ota: service stopped");
}

void JcOtaService::service(uint32_t now) {
    (void)now;
    if (_ctl == nullptr) return;
    if (_ctl->service_wanted()) {
        start();
        if (_listening) ArduinoOTA.handle();   // blocks while a transfer runs; progress is drawn from the callback
    } else {
        stop();
    }
}

void JcOtaService::confirm_boot_if_healthy(uint32_t now) {
    if (_confirmed || now < kHealthyUptimeMs) return;
    _confirmed = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            snprintf(_rollback_text, sizeof(_rollback_text), "confirmed");
            ++_validated;
            Serial.println("ota: new image confirmed after a healthy start (rollback cancelled)");
        }
    }
}

void JcOtaService::restart() {
    Serial.println("ota: restarting");
    delay(100);
    ESP.restart();
}

}  // namespace jc8048w550
