// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <esp_heap_caps.h>

#include "JcPendantUi.hpp"

namespace {

constexpr int screen_width = 800;
constexpr int screen_height = 480;
constexpr int backlight_pin = 2;
constexpr int touch_sda_pin = 19;
constexpr int touch_scl_pin = 20;
constexpr int touch_reset_pin = 38;
constexpr int touch_interrupt_pin = -1;
constexpr uint8_t touch_i2c_address = 0x5D;

Arduino_ESP32RGBPanel rgb_bus(
    GFX_NOT_DEFINED, GFX_NOT_DEFINED, GFX_NOT_DEFINED,
    40, 41, 39, 42,
    45, 48, 47, 21, 14,
    5, 6, 7, 15, 16, 4,
    8, 3, 46, 9, 1);

Arduino_RPi_DPI_RGBPanel display(
    &rgb_bus,
    screen_width, 0, 8, 4, 8,
    screen_height, 0, 8, 4, 8,
    1, 16000000, true);

TAMC_GT911 touch(
    touch_sda_pin,
    touch_scl_pin,
    touch_interrupt_pin,
    touch_reset_pin,
    screen_width,
    screen_height);

jc8048w550::PendantUi ui(display);

bool probe_touch_i2c() {
    Wire.beginTransmission(touch_i2c_address);
    return Wire.endTransmission() == 0;
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("JC8048W550C_I isolated hardware bring-up");
    Serial.printf("chip: %s, revision: %u\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("flash: %u bytes\n", ESP.getFlashChipSize());
    const bool psram_ready = psramInit();
    Serial.printf("PSRAM initialized: %s, size: %u bytes\n",
        psram_ready ? "yes" : "no",
        ESP.getPsramSize());
    Serial.printf("PSRAM heap: %u bytes\n",
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    Serial.printf("PSRAM free heap: %u bytes\n",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.printf("internal free heap: %u bytes\n", ESP.getFreeHeap());

    if (!psram_ready || !psramFound() || ESP.getPsramSize() == 0) {
        Serial.println("ERROR: required PSRAM is unavailable; stopping bring-up.");
        while (true) {
            delay(1000);
        }
    }
    display.begin();
    pinMode(backlight_pin, OUTPUT);
    digitalWrite(backlight_pin, HIGH);
    Serial.printf("display: ST7262 RGB initialized; resolution=%ux%u\n",
        display.width(),
        display.height());
    Serial.println("backlight: GPIO2 driven HIGH (full-brightness test).");

    Wire.begin(touch_sda_pin, touch_scl_pin);
    const bool touch_ack = probe_touch_i2c();
    Serial.printf("touch: GT911 I2C 0x%02X %s\n",
        touch_i2c_address,
        touch_ack ? "ACK" : "NO ACK");
    touch.begin();
    touch.setRotation(ROTATION_NORMAL);
    Serial.println("touch: configured SDA=19, SCL=20, RST=38, INT=NC, rotation=normal.");
    ui.begin();
    Serial.println("Ready. Demo mode by default; manual read-only status only; motion commands disabled.");
}

void loop() {
    ui.poll(touch);
    delay(20);
}
