#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * device_state.h — TinyGuard Monitor
 *
 * Single shared structure representing the latest known state of the
 * monitored ESP32-CAM, populated by the UDP receiver after each packet.
 *
 * All fields are updated atomically under a mutex in device_state.c.
 * Readers call device_state_get() to get a snapshot copy.
 */

typedef struct {
    bool     valid;             /* false until first heartbeat received */
    uint32_t uptime_ms;         /* camera uptime in milliseconds */
    int      rssi;              /* camera WiFi RSSI in dBm */
    bool     stream_active;     /* stream currently being accessed */
    int      viewer_count;      /* number of active stream viewers */
    int      reconnects;        /* cumulative camera reconnect count */
    uint32_t timestamp;         /* camera-reported seconds since boot */
    uint32_t last_rx_tick;      /* esp_log_timestamp() when last packet arrived */
    uint32_t packet_count;      /* total heartbeats received since monitor boot */
} device_state_t;

void           device_state_init(void);
void           device_state_update(const device_state_t *s);
device_state_t device_state_get(void);
