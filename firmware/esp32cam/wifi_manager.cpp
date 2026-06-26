/*
 * wifi_manager.cpp — ESP32-CAM
 *
 * Connects to the laptop hotspot with a static IP.
 * Handles reconnects and tracks reconnect count.
 *
 * Network layout (from MVP spec):
 *   Hotspot gateway : 192.168.137.1
 *   ESP32-CAM       : 192.168.137.10  (static)
 *   ESP32 Monitor   : 192.168.137.20  (static)
 *   Subnet          : 255.255.255.0
 *
 * Fix (vs original):
 *   - s_connected flag guards the DISCONNECTED handler so transient events
 *     during initial association don't increment the reconnect counter.
 *     Only a drop from an established connection is counted.
 *   - WiFi.reconnect() is deferred 500 ms via a one-shot FreeRTOS timer to
 *     give the AP time to clean up the old association before we hammer it
 *     again. Hammering is a common cause of repeated disconnection loops on
 *     the AI Thinker module.
 */

#include <WiFi.h>
#include "wifi_manager.h"

// --- Configuration -----------------------------------------------------------
static const char* SSID     = "idk";
static const char* PASSWORD = "lol12345";

static const IPAddress STATIC_IP (192, 168, 137, 10);
static const IPAddress GATEWAY   (192, 168, 137,  1);
static const IPAddress SUBNET    (255, 255, 255,  0);
static const IPAddress DNS1      (  8,   8,   8,  8);

// Delay before calling WiFi.reconnect() after a drop.
// Gives the AP time to release the old association entry.
#define RECONNECT_DELAY_MS  500
// -----------------------------------------------------------------------------

static int           s_reconnect_count = 0;
static volatile bool s_connected       = false;  // true only after first GOT_IP

static void reconnect_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    WiFi.reconnect();
}

static void wifi_event_handler(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (s_connected) {
                // Only count drops from an established connection.
                s_connected = false;
                s_reconnect_count++;
                Serial.printf("[WiFi] Disconnected (reconnect #%d) — retrying in %d ms\n",
                              s_reconnect_count, RECONNECT_DELAY_MS);
                // One-shot timer — deferred reconnect avoids hammering the AP.
                TimerHandle_t t = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_DELAY_MS),
                                               pdFALSE, nullptr, reconnect_timer_cb);
                if (t) xTimerStart(t, 0);
                else   WiFi.reconnect();   // fallback if timer alloc fails
            }
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_connected = true;
            Serial.printf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            break;

        default:
            break;
    }
}

void wifi_manager_init() {
    WiFi.onEvent(wifi_event_handler);
    WiFi.mode(WIFI_STA);

    if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1)) {
        Serial.println("[WiFi] Static IP config failed — check values.");
    }

    Serial.printf("[WiFi] Connecting to %s ...\n", SSID);
    WiFi.begin(SSID, PASSWORD);

    uint32_t timeout_ms = 20000;
    uint32_t start      = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeout_ms) {
            Serial.println("[WiFi] Connection timed out. Restarting...");
            ESP.restart();
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();
}

const char* wifi_manager_get_ip() {
    static char ip_buf[16];
    strncpy(ip_buf, WiFi.localIP().toString().c_str(), sizeof(ip_buf));
    return ip_buf;
}

int wifi_manager_get_rssi() {
    return WiFi.RSSI();
}

int wifi_manager_get_reconnect_count() {
    return s_reconnect_count;
}
