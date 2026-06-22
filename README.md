# TinyGuard

> Detect suspicious IoT camera behavior using network metadata — without inspecting a single payload byte.

Lightweight communication-behavior anomaly detection for home IoT cameras.
Runs on two ESP32 boards: one simulates a camera, one monitors it.

## Implemented

- **Milestone 0** — Repository structure, build environments configured
- **Milestone 1** — ESP32-CAM firmware: WiFi, static IP, MJPEG stream, UDP heartbeat
- **Milestone 2** — Monitor node: UDP receiver, device state, heartbeat parsing, GPIO2 LED blink
- **Milestone 3** — Statistics engine: rolling windows, Welford mean/stddev, baseline learning phase

## Pending

- Milestone 4: Detection engine (z-score anomaly detection, alerting)
- Milestone 5: Dashboard
- Milestone 6: Validation

## Hardware

| Device | Role | IP |
|---|---|---|
| ESP32-CAM | Simulated target camera | 192.168.137.10 |
| ESP32 DevKit V1 | TinyGuard monitor | 192.168.137.20 |
| Laptop hotspot | Network hub | 192.168.137.1 |

## Repository Layout

```
firmware/
├── esp32cam/          # Arduino IDE project (AI Thinker ESP32-CAM)
│   ├── main.ino
│   ├── wifi_manager.h / .cpp
│   ├── camera_server.h / .cpp
│   ├── telemetry_collector.h / .cpp
│   └── heartbeat_service.h / .cpp
└── monitor/           # ESP-IDF v5.x project (ESP32 DevKit V1)
    ├── CMakeLists.txt
    ├── sdkconfig.defaults
    └── main/
        ├── CMakeLists.txt
        ├── main.c
        ├── wifi_manager.h / .c
        ├── device_state.h / .c
        ├── udp_receiver.h / .c
        └── stats_engine.h / .c
```

## Quick Start

### Camera (ESP32-CAM) — Arduino IDE

1. Open `firmware/esp32cam/` in Arduino IDE
2. Install dependencies: `esp32` board package (Espressif)
3. Set board: `AI Thinker ESP32-CAM`
4. Set `SSID` / `PASSWORD` in `wifi_manager.cpp`
5. Flash `main.ino`
6. Verify stream at `http://192.168.137.10/stream`
7. Verify heartbeats on serial monitor

### Monitor (ESP32 DevKit) — ESP-IDF

1. Install ESP-IDF v5.x
2. Set `WIFI_SSID` / `WIFI_PASSWORD` in `firmware/monitor/main/wifi_manager.c`
3. `cd firmware/monitor`
4. `idf.py set-target esp32`
5. `idf.py build && idf.py flash monitor`
6. Confirm `[WiFi-Monitor] Connected. IP: 192.168.137.20` in serial output
7. Watch GPIO2 LED blink every ~10s once the camera is powered

> **One USB cable?** Flash the monitor first, open serial to confirm WiFi,
> then unplug and power the camera separately. The GPIO2 LED blink is your
> heartbeat indicator without needing a second serial connection.

## Heartbeat Packet Format

```json
{
  "device": "esp32cam",
  "uptime": 123456,
  "rssi": -58,
  "stream_active": 1,
  "viewer_count": 0,
  "reconnects": 0,
  "timestamp": 10
}
```

Transport: UDP · Port: 5000 · Interval: 10s · Timeout: 30s (3 missed beats)

## Monitor Serial Output

Every 10s — heartbeat receipt:
```
I (12345) UDP-RX: --- Heartbeat #3 from 192.168.137.10 ---
I (12345) UDP-RX:   uptime   : 30028 ms
I (12345) UDP-RX:   rssi     : -42 dBm
I (12345) UDP-RX:   stream   : idle  viewers: 0
I (12345) UDP-RX:   reconnects: 0  cam-ts: 30 s
```

Every 30s — statistics summary (after learning phase completes):
```
I (xxxxx) TinyGuard: [ACTIVE] samples: 42
I (xxxxx) TinyGuard:   RSSI      : mean=-42.1  stddev=1.2  latest=-43 dBm  (n=42)
I (xxxxx) TinyGuard:   HB intv   : mean=10012  stddev=45   latest=10008 ms  (n=41)
I (xxxxx) TinyGuard:   Recon/hr  : mean=0.00   stddev=0.00 latest=0.00  (n=40)
I (xxxxx) TinyGuard:   Viewers   : mean=0.00   stddev=0.00 latest=0.00  (n=42)
```

During the first 5 minutes (learning phase):
```
I (xxxxx) TinyGuard: [LEARNING] 270 s remaining | samples: 3
```
