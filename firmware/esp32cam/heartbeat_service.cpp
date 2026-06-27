/*
 * heartbeat_service.cpp — ESP32-CAM
 *
 * Sends a UDP heartbeat packet to the monitor every HEARTBEAT_INTERVAL_MS.
 *
 * Monitor address is resolved via mDNS (tinyguard-monitor.local). If
 * resolution fails at init (monitor not yet on network), it is retried
 * automatically every RESOLVE_RETRY_MS until it succeeds. No manual
 * reboot needed if the monitor comes online after the camera.
 */

#include "heartbeat_service.h"
#include "wifi_manager.h"
#include "telemetry_collector.h"
#include <WiFiUdp.h>
#include <Arduino.h>

static const uint16_t MONITOR_UDP_PORT      = 5000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
static const uint32_t RESOLVE_RETRY_MS      = 30000;  // retry mDNS every 30s

static WiFiUDP   s_udp;
static uint32_t  s_last_beat_ms    = 0;
static uint32_t  s_last_resolve_ms = 0;

void heartbeat_service_init() {
    wifi_manager_resolve_monitor_ip();

    const char* monitor = wifi_manager_get_monitor_ip();
    if (strlen(monitor) == 0) {
        Serial.println("[Heartbeat] Monitor not found — will retry every 30s.");
    } else {
        Serial.printf("[Heartbeat] Will send to %s:%d every %lu ms\n",
                      monitor, MONITOR_UDP_PORT, HEARTBEAT_INTERVAL_MS);
    }
}

void heartbeat_service_tick() {
    uint32_t now = millis();

    // If monitor address is unknown, retry resolution periodically.
    // Covers the case where monitor boots after the camera.
    const char* monitor = wifi_manager_get_monitor_ip();
    if (strlen(monitor) == 0) {
        if (now - s_last_resolve_ms >= RESOLVE_RETRY_MS) {
            s_last_resolve_ms = now;
            Serial.println("[Heartbeat] Retrying monitor mDNS resolution...");
            wifi_manager_resolve_monitor_ip();
            monitor = wifi_manager_get_monitor_ip();
            if (strlen(monitor) > 0) {
                Serial.printf("[Heartbeat] Monitor found: %s — heartbeats starting.\n", monitor);
            }
        }
        return;  // skip send until resolved
    }

    if (now - s_last_beat_ms < HEARTBEAT_INTERVAL_MS) return;
    s_last_beat_ms = now;

    telemetry_t t = telemetry_collect();

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
        (unsigned long)(t.uptime_ms / 1000)
    );

    if (len < 0 || len >= (int)sizeof(packet)) {
        Serial.println("[Heartbeat] Packet serialisation error — skipping.");
        return;
    }

    s_udp.beginPacket(monitor, MONITOR_UDP_PORT);
    s_udp.write((const uint8_t*)packet, (size_t)len);
    int result = s_udp.endPacket();

    if (result) {
        Serial.printf("[Heartbeat] Sent (%d bytes) to %s: %s\n", len, monitor, packet);
    } else {
        Serial.println("[Heartbeat] UDP send failed — is monitor reachable?");
    }
}