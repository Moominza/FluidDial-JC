// Copyright (c) 2026 FluidDial-JC contributors
// Use of this source code is governed by a GPLv3 license in the project root.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include <esp_heap_caps.h>

namespace {

constexpr int screen_width = 800;
constexpr int screen_height = 480;
constexpr int backlight_pin = 2;
constexpr int touch_sda_pin = 19;
constexpr int touch_scl_pin = 20;
constexpr int touch_reset_pin = 38;
constexpr int touch_interrupt_pin = -1;
constexpr uint8_t touch_i2c_address = 0x5D;
constexpr int status_y = 426;
constexpr int status_height = 54;

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

void draw_test_pattern() {
    display.fillScreen(BLACK);
    constexpr int swatch_y = 0;
    constexpr int swatch_height = 84;
    constexpr int swatch_width = screen_width / 5;
    const uint16_t colors[] = { RED, GREEN, BLUE, WHITE, BLACK };
    const char* names[] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };

    for (int i = 0; i < 5; ++i) {
        const int x = i * swatch_width;
        const int width = (i == 4) ? screen_width - x : swatch_width;
        display.fillRect(x, swatch_y, width, swatch_height, colors[i]);
        display.drawRect(x, swatch_y, width, swatch_height, LIGHTGREY);
        display.setTextSize(2);
        display.setTextColor((i == 3) ? BLACK : WHITE);
        display.setCursor(x + 12, 34);
        display.print(names[i]);
    }

    display.drawFastHLine(0, 85, screen_width, WHITE);
    display.drawFastHLine(0, screen_height / 2, screen_width, LIGHTGREY);
    display.drawFastVLine(400, 86, 339, LIGHTGREY);

    const auto draw_corner = [](int x, int y, const char* label) {
        display.fillRect(x, y, 132, 38, BLACK);
        display.drawRect(x, y, 132, 38, WHITE);
        display.setTextColor(WHITE);
        display.setTextSize(2);
        display.setCursor(x + 8, y + 10);
        display.print(label);
    };
    draw_corner(12, 98, "TOP LEFT");
    draw_corner(screen_width - 144, 98, "TOP RIGHT");
    draw_corner(12, 368, "BOT LEFT");
    draw_corner(screen_width - 144, 368, "BOT RIGHT");

    display.drawRect(0, 0, screen_width, screen_height, WHITE);
    display.fillRect(1, status_y, screen_width - 2, status_height - 1, BLACK);
    display.drawFastHLine(1, status_y, screen_width - 2, WHITE);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(12, status_y + 18);
    display.print("GT911 touch: place finger");
}

void draw_touch_coordinates(bool touched, uint16_t x, uint16_t y, uint8_t count) {
    display.fillRect(2, status_y + 2, screen_width - 4, status_height - 4, BLACK);
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(12, status_y + 18);
    if (touched) {
        display.printf("Touch X:%u Y:%u  points:%u", x, y, count);
    } else {
        display.print("Touch released");
    }
}

bool probe_touch_i2c() {
    Wire.beginTransmission(touch_i2c_address);
    return Wire.endTransmission() == 0;
}

void report_touch() {
    static bool was_touched = false;
    static uint16_t last_x = UINT16_MAX;
    static uint16_t last_y = UINT16_MAX;

    touch.read();
    if (touch.isTouched && touch.touches > 0) {
        const auto& point = touch.points[0];
        if (!was_touched || point.x != last_x || point.y != last_y) {
            const uint16_t ui_x = point.x < screen_width
                ? point.x
                : screen_width - 1;
            const uint16_t ui_y = point.y < screen_height
                ? point.y
                : screen_height - 1;
            Serial.printf(
                "touch: count=%u raw_x=%u raw_y=%u driver_x=%u driver_y=%u ui_x=%u ui_y=%u size=%u\n",
                touch.touches,
                point.rawX,
                point.rawY,
                point.x,
                point.y,
                ui_x,
                ui_y,
                point.size);
            draw_touch_coordinates(true, ui_x, ui_y, touch.touches);
            last_x = point.x;
            last_y = point.y;
        }
        was_touched = true;
    } else if (was_touched) {
        Serial.println("touch: released");
        draw_touch_coordinates(false, 0, 0, 0);
        was_touched = false;
        last_x = UINT16_MAX;
        last_y = UINT16_MAX;
    }
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
    draw_test_pattern();
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
    Serial.println("Ready. This image has no CNC transport or motion commands.");
}

void loop() {
    report_touch();
    delay(20);
}
