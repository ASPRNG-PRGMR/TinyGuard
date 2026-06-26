/*
 * heartbeat_service.cpp — ESP32-CAM
 *
 * Sends a UDP heartbeat packet to the monitor every HEARTBEAT_INTERVAL_MS.
 *
 * Packet format:
 *   {
 *     "device":        "esp32cam",
 *     "uptime":        123456,       // milliseconds since boot
 *     "rssi":          -58,          // dBm
 *     "stream_active": 1,            // 1 = stream being accessed, 0 = idle
 *     "viewer_count":  0,
 *     "reconnects":    0,            // cumulative reconnect count
 *     "timestamp":     1710000000    // ESP uptime seconds (no NTP in MVP)
 *   }
 *
 * The monitor listens on UDP port 5000.
 * Missing heartbeat timeout (from spec): 30 seconds = 3 missed intervals.
 */

#include "heartbeat_service.h"
#include "telemetry_collector.h"
#include <WiFiUdp.h>
#include <Arduino.h>

// --- Configuration -----------------------------------------------------------
static const char*    MONITOR_IP           = "192.168.137.20";
static const uint16_t MONITOR_UDP_PORT     = 5000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;  // 10 seconds (spec)
// -----------------------------------------------------------------------------

static WiFiUDP s_udp;
static uint32_t s_last_beat_ms = 0;

void heartbeat_service_init() {
  // Nothing to bind on the sender side; WiFiUDP handles the socket
  Serial.printf("[Heartbeat] Will send to %s:%d every %lu ms\n",
                MONITOR_IP, MONITOR_UDP_PORT, HEARTBEAT_INTERVAL_MS);
}

void heartbeat_service_tick() {
  uint32_t now = millis();
  if (now - s_last_beat_ms < HEARTBEAT_INTERVAL_MS) {
    return;  // Not time yet
  }
  s_last_beat_ms = now;

  telemetry_t t = telemetry_collect();

  // Build JSON packet
  // Using a fixed-size buffer. Max packet size well within UDP limits.
  char packet[256];
  int len = snprintf(packet, sizeof(packet),
    "{"
      "\"device\":\"esp32cam\","
      "\"uptime\":%lu,"
      "\"rssi\":%d,"
      "\"stream_active\":%d,"
      "\"viewer_count\":%d,"
      "\"reconnects\":%d,"
      "\"timestamp\":%lu"
    "}",
    (unsigned long)t.uptime_ms,
    t.rssi,
    t.stream_active ? 1 : 0,
    t.viewer_count,
    t.reconnect_count,
    (unsigned long)(t.uptime_ms / 1000)  // seconds since boot
  );

  if (len < 0 || len >= (int)sizeof(packet)) {
    Serial.println("[Heartbeat] Packet serialisation error — skipping.");
    return;
  }

  s_udp.beginPacket(MONITOR_IP, MONITOR_UDP_PORT);
  s_udp.write((const uint8_t*)packet, (size_t)len);
  int result = s_udp.endPacket();

  if (result) {
    Serial.printf("[Heartbeat] Sent (%d bytes): %s\n", len, packet);
  } else {
    Serial.println("[Heartbeat] UDP send failed — is monitor reachable?");
  }
}
