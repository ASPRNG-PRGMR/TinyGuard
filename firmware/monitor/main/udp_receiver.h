#pragma once

/*
 * udp_receiver.h — TinyGuard Monitor
 *
 * Listens on UDP port 5000 for heartbeat packets from the ESP32-CAM.
 * Parses incoming JSON, updates device_state, and logs to serial.
 *
 * Runs in its own FreeRTOS task. Call udp_receiver_start() once from app_main()
 * after WiFi is up.
 */

void udp_receiver_start(void);
