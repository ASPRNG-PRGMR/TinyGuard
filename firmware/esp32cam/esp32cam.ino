/*
 * TinyGuard — ESP32-CAM Target Device Firmware
 *
 * Board: AI Thinker ESP32-CAM
 * Arduino IDE: esp32 board package by Espressif
 *
 * Brownout handling
 * -----------------
 * The AI Thinker ESP32-CAM draws up to ~300 mA during WiFi TX bursts.
 * On resistive USB cables or current-limited laptop ports the supply
 * voltage can sag below the ESP32 brownout threshold (~2.7 V), causing
 * a brownout reset. Without handling, the chip resets silently and the
 * stream dies with no log output.
 *
 * Mitigations applied:
 *   1. Brownout detector threshold lowered to level 1 (~2.43 V) via
 *      register write. This gives more headroom before reset fires.
 *      The chip is still protected — level 1 is within safe operating
 *      range for the ESP32.
 *   2. On reset, esp_reset_reason() is checked and logged so brownout
 *      events are visible in the serial monitor rather than appearing
 *      as silent reboots.
 *
 * Recommended hardware fix: solder a 100 µF electrolytic capacitor
 * across the 5 V and GND pins on the ESP-CAM board. Power via a
 * dedicated USB charger (1 A+) or USB-C breakout board rather than
 * a laptop port.
 */

#include "wifi_manager.h"
#include "camera_server.h"
#include "heartbeat_service.h"
#include "telemetry_collector.h"
#include "esp_system.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── brownout threshold ────────────────────────────────────────────────────
//
// ESP32 brownout levels (approximate trip voltages on 3.3 V rail):
//   0 = ~2.43 V   1 = ~2.48 V   2 = ~2.58 V   3 = ~2.68 V (default)
//   4 = ~2.74 V   5 = ~2.80 V   6 = ~2.92 V   7 = ~3.00 V
//
// Lowering from default (3) to 1 gives ~250 mV more headroom before
// the detector fires on a voltage sag. The chip remains safe — it will
// still reset before reaching a level that corrupts flash or RAM.
#define BROWNOUT_LEVEL  1

static void brownout_threshold_set(uint8_t level) {
    // Read-modify-write the brownout control register.
    // RTC_CNTL_BROWN_OUT_THRES_S is the field shift; mask is 7 (3 bits).
    uint32_t reg = READ_PERI_REG(RTC_CNTL_BROWN_OUT_REG);
    reg &= ~(0x7 << RTC_CNTL_DBROWN_OUT_THRES_S);
    reg |=  (level & 0x7) << RTC_CNTL_DBROWN_OUT_THRES_S;
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, reg);
    Serial.printf("[Boot] Brownout threshold set to level %d\n", level);
}

// ── reset reason logging ──────────────────────────────────────────────────

static void log_reset_reason() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:
            Serial.println("[Boot] Reset: power-on");
            break;
        case ESP_RST_BROWNOUT:
            Serial.println("[Boot] Reset: BROWNOUT DETECTED — check power supply.");
            Serial.println("[Boot]   → Use a shorter/thicker USB cable or dedicated charger.");
            Serial.println("[Boot]   → A 100 µF cap across 5V/GND is the hardware fix.");
            break;
        case ESP_RST_SW:
            Serial.println("[Boot] Reset: software reset");
            break;
        case ESP_RST_PANIC:
            Serial.println("[Boot] Reset: panic / exception");
            break;
        case ESP_RST_INT_WDT:
            Serial.println("[Boot] Reset: interrupt watchdog");
            break;
        case ESP_RST_TASK_WDT:
            Serial.println("[Boot] Reset: task watchdog");
            break;
        case ESP_RST_WDT:
            Serial.println("[Boot] Reset: other watchdog");
            break;
        case ESP_RST_DEEPSLEEP:
            Serial.println("[Boot] Reset: wake from deep sleep");
            break;
        default:
            Serial.printf("[Boot] Reset: unknown reason (%d)\n", (int)reason);
            break;
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("[TinyGuard-CAM] Booting...");

    log_reset_reason();
    brownout_threshold_set(BROWNOUT_LEVEL);

    wifi_manager_init();
    camera_server_init();
    heartbeat_service_init();

    // Heartbeat runs on core 0; HTTP server and streamer run on core 1.
    // Previously this caused lwIP conflicts because WiFiUDP (heartbeat)
    // and WiFiClient (stream) share Arduino's internal lwIP state.
    // Fixed by switching heartbeat to raw BSD sockets (lwip/sockets.h)
    // which are re-entrant and don't share state with WiFiClient.
    xTaskCreatePinnedToCore(
        [](void *) {
            for (;;) {
                heartbeat_service_tick();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        },
        "heartbeat", 4096, nullptr, 1, nullptr, 0  // core 0
    );

    Serial.println("[TinyGuard-CAM] Ready.");
    Serial.printf("[TinyGuard-CAM] View: http://%s/view\n", wifi_manager_get_ip());
}

void loop() {
    // Intentionally empty — all work is in FreeRTOS tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
