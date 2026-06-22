#pragma once
#include <stdint.h>

typedef struct {
  int     rssi;
  uint32_t uptime_ms;
  bool    stream_active;
  int     viewer_count;
  int     reconnect_count;
} telemetry_t;

telemetry_t telemetry_collect();
