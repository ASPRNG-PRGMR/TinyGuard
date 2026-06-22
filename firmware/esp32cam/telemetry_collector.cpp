/*
 * telemetry_collector.cpp — ESP32-CAM
 *
 * Assembles a telemetry snapshot from the WiFi stack, camera server,
 * and system uptime. Called by heartbeat_service before each UDP send.
 */

#include "telemetry_collector.h"
#include "wifi_manager.h"
#include "camera_server.h"
#include <Arduino.h>

telemetry_t telemetry_collect() {
  telemetry_t t;
  t.rssi            = wifi_manager_get_rssi();
  t.uptime_ms       = millis();
  t.stream_active   = camera_server_is_streaming();
  t.viewer_count    = camera_server_get_viewer_count();
  t.reconnect_count = wifi_manager_get_reconnect_count();
  return t;
}
