/*
 * heartbeat_service.cpp — ESP32-CAM
 *
 * Sends a UDP heartbeat packet to the monitor every HEARTBEAT_INTERVAL_MS.
 *
 * Uses raw POSIX/lwIP BSD sockets instead of Arduino WiFiUDP.
 * WiFiUDP shares internal Arduino lwIP state with WebServer's WiFiClient
 * TCP stack — concurrent access causes write failures on the TCP side
 * exactly when the UDP send fires (every 10s). BSD sockets are re-entrant
 * and do not share state with the Arduino WiFiClient layer.
 */

#include "heartbeat_service.h"
#include "wifi_manager.h"
#include "telemetry_collector.h"
#include <Arduino.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const uint16_t MONITOR_UDP_PORT      = 5000;
static const uint32_t HEARTBEAT_INTERVAL_MS = 10000;
static const uint32_t RESOLVE_RETRY_MS      = 30000;

static int       s_sock           = -1;
static uint32_t  s_last_beat_ms   = 0;
static uint32_t  s_last_resolve_ms = 0;

// Open a UDP socket and configure the destination address.
// Called once after monitor IP is known; re-called if IP changes.
static bool socket_open(const char* ip) {
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        Serial.printf("[Heartbeat] socket() failed: %d\n", errno);
        return false;
    }

    // Connect the socket so send() works without specifying dest each time.
    struct sockaddr_in dest = {};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(MONITOR_UDP_PORT);
    dest.sin_addr.s_addr = inet_addr(ip);

    if (connect(s_sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        Serial.printf("[Heartbeat] connect() failed: %d\n", errno);
        close(s_sock);
        s_sock = -1;
        return false;
    }

    Serial.printf("[Heartbeat] UDP socket ready → %s:%d\n", ip, MONITOR_UDP_PORT);
    return true;
}

void heartbeat_service_init() {
    wifi_manager_resolve_monitor_ip();

    const char* monitor = wifi_manager_get_monitor_ip();
    if (strlen(monitor) == 0) {
        Serial.println("[Heartbeat] Monitor not found — will retry every 30s.");
        return;
    }

    socket_open(monitor);
    Serial.printf("[Heartbeat] Will send to %s:%d every %lu ms\n",
                  monitor, MONITOR_UDP_PORT, HEARTBEAT_INTERVAL_MS);
}

void heartbeat_service_tick() {
    uint32_t now = millis();

    // Retry monitor resolution if not yet found
    const char* monitor = wifi_manager_get_monitor_ip();
    if (strlen(monitor) == 0) {
        if (now - s_last_resolve_ms >= RESOLVE_RETRY_MS) {
            s_last_resolve_ms = now;
            Serial.println("[Heartbeat] Retrying monitor mDNS resolution...");
            wifi_manager_resolve_monitor_ip();
            monitor = wifi_manager_get_monitor_ip();
            if (strlen(monitor) > 0) {
                socket_open(monitor);
                Serial.printf("[Heartbeat] Monitor found: %s — heartbeats starting.\n", monitor);
            }
        }
        return;
    }

    // Open socket if not ready
    if (s_sock < 0) {
        socket_open(monitor);
        if (s_sock < 0) return;
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

    int sent = send(s_sock, packet, len, 0);
    if (sent == len) {
        Serial.printf("[Heartbeat] Sent (%d bytes) to %s: %s\n", len, monitor, packet);
    } else {
        Serial.printf("[Heartbeat] send() failed (sent=%d errno=%d) — will retry.\n", sent, errno);
        // Socket may be stale — close and reopen next tick
        close(s_sock);
        s_sock = -1;
    }
}
