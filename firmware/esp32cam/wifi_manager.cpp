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
 */

#include <WiFi.h>
#include "wifi_manager.h"

// --- Configuration -----------------------------------------------------------
// Replace with your laptop hotspot credentials.
static const char* SSID     = "idk";
static const char* PASSWORD = "lol12345";

static const IPAddress STATIC_IP (192, 168, 137, 10);
static const IPAddress GATEWAY   (192, 168, 137,  1);
static const IPAddress SUBNET    (255, 255, 255,  0);
static const IPAddress DNS1      (  8,   8,   8,  8);
// -----------------------------------------------------------------------------

static int s_reconnect_count = 0;

static void wifi_event_handler(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] Disconnected — reconnecting...");
      s_reconnect_count++;
      WiFi.reconnect();
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
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

  // Static IP must be set before WiFi.begin()
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS1)) {
    Serial.println("[WiFi] Static IP config failed — check values.");
  }

  Serial.printf("[WiFi] Connecting to %s ...\n", SSID);
  WiFi.begin(SSID, PASSWORD);

  // Block until connected (with timeout)
  uint32_t timeout_ms = 20000;
  uint32_t start = millis();
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
