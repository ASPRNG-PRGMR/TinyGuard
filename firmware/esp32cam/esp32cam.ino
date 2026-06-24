/*
 * TinyGuard — ESP32-CAM Target Device Firmware
 * Milestone 1: WiFi, static IP, MJPEG stream, UDP heartbeat
 *
 * Board: AI Thinker ESP32-CAM
 * Arduino IDE: esp32 board package by Espressif
 */

#include "wifi_manager.h"
#include "camera_server.h"
#include "heartbeat_service.h"
#include "telemetry_collector.h"

void setup() {
  Serial.begin(115200);
  Serial.println("[TinyGuard-CAM] Booting...");

  wifi_manager_init();
  camera_server_init();
  heartbeat_service_init();

  Serial.println("[TinyGuard-CAM] Ready.");
  Serial.printf("[TinyGuard-CAM] Stream: http://%s/stream\n",
                wifi_manager_get_ip());
}

void loop() {
  heartbeat_service_tick();
  delay(100);
}
