/*
 * wifi_manager.cpp — ESP32-CAM
 *
 * Connects to a WiFi AP using DHCP. No static IP.
 *
 * The ESP announces itself via mDNS as "tinyguard-cam.local" so the
 * monitor and browser can reach it by name regardless of what subnet
 * the hotspot assigns. This works on any hotspot (Windows, Fedora,
 * Android, iOS) without reflashing when the network changes.
 *
 * The monitor's address is resolved once after connection via
 * wifi_manager_resolve_monitor_ip() — called by heartbeat_service
 * before the first send. Falls back to a compile-time literal if mDNS
 * resolution fails (useful when both ESPs are on a controlled network).
 *
 * Reconnect behaviour (unchanged from static-IP version):
 *   - s_connected flag prevents spurious counter increments during
 *     initial association.
 *   - WiFi.reconnect() deferred 500 ms via one-shot FreeRTOS timer to
 *     avoid hammering the AP before it releases the old association.
 */

#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Arduino.h>

// --- Configuration -----------------------------------------------------------
static const char* SSID     = "idk";
static const char* PASSWORD = "lol12345";

// Hostname advertised via mDNS. Browser: http://tinyguard-cam.local/stream
static const char* MDNS_HOSTNAME = "tinyguard-cam";

// Fallback monitor IP used if mDNS resolution of tinyguard-monitor.local
// fails. Set to "" to disable fallback and rely purely on mDNS.
static const char* MONITOR_FALLBACK_IP = "";

#define RECONNECT_DELAY_MS  500
// -----------------------------------------------------------------------------

static int           s_reconnect_count = 0;
static volatile bool s_connected       = false;

// Resolved monitor IP — written once by wifi_manager_resolve_monitor_ip(),
// read by heartbeat_service. No mutex needed: written before heartbeat task
// starts sending, read-only after that.
static char s_monitor_ip[16] = {0};

static void reconnect_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    WiFi.reconnect();
}

static void wifi_event_handler(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (s_connected) {
                s_connected = false;
                s_reconnect_count++;
                Serial.printf("[WiFi] Disconnected (reconnect #%d) — retrying in %d ms\n",
                              s_reconnect_count, RECONNECT_DELAY_MS);
                TimerHandle_t t = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_DELAY_MS),
                                               pdFALSE, nullptr, reconnect_timer_cb);
                if (t) xTimerStart(t, 0);
                else   WiFi.reconnect();
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
    // DHCP — no WiFi.config() call. AP assigns the address.

    Serial.printf("[WiFi] Connecting to %s (DHCP) ...\n", SSID);
    WiFi.begin(SSID, PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 20000) {
            Serial.println("[WiFi] Connection timed out. Restarting...");
            ESP.restart();
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.printf("[WiFi] DHCP address: %s\n", WiFi.localIP().toString().c_str());

    // Start mDNS responder — advertises "tinyguard-cam.local"
    if (!MDNS.begin(MDNS_HOSTNAME)) {
        Serial.println("[WiFi] mDNS responder failed to start.");
    } else {
        // Advertise HTTP service so network scanners can discover it
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WiFi] mDNS: http://%s.local/\n", MDNS_HOSTNAME);
    }
}

/*
 * Resolve the monitor's mDNS hostname to an IP string.
 * Called once by heartbeat_service_init() after wifi_manager_init().
 * Tries mDNS first; falls back to MONITOR_FALLBACK_IP if set.
 * Returns true if a usable address was found.
 */
bool wifi_manager_resolve_monitor_ip() {
    Serial.println("[WiFi] Resolving tinyguard-monitor.local ...");

    // Give mDNS a moment — the monitor may still be booting
    IPAddress addr;
    for (int attempt = 0; attempt < 5; attempt++) {
        addr = MDNS.queryHost("tinyguard-monitor");
        if (addr != INADDR_NONE && (uint32_t)addr != 0) {
            snprintf(s_monitor_ip, sizeof(s_monitor_ip), "%d.%d.%d.%d",
                     addr[0], addr[1], addr[2], addr[3]);
            Serial.printf("[WiFi] Monitor resolved: %s\n", s_monitor_ip);
            return true;
        }
        Serial.printf("[WiFi] mDNS attempt %d/5 failed — retrying...\n", attempt + 1);
        delay(1000);
    }

    // mDNS failed — use fallback if configured
    if (strlen(MONITOR_FALLBACK_IP) > 0) {
        strncpy(s_monitor_ip, MONITOR_FALLBACK_IP, sizeof(s_monitor_ip));
        Serial.printf("[WiFi] mDNS failed. Using fallback: %s\n", s_monitor_ip);
        return true;
    }

    Serial.println("[WiFi] WARNING: monitor address unknown. Heartbeats will not be sent.");
    return false;
}

const char* wifi_manager_get_monitor_ip() {
    return s_monitor_ip;
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
