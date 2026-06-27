# TinyGuard

> Detect suspicious IoT camera behavior using network metadata — without inspecting a single payload byte.

Lightweight communication-behavior anomaly detection for home IoT cameras.
Runs on two ESP32 boards: one simulates a camera, one monitors it.

---

## How It Works

The camera sends a UDP heartbeat every 10 seconds containing RSSI, uptime, stream state, viewer count, and reconnect count. The monitor builds rolling statistical baselines during a 5-minute learning phase, then detects deviations using z-score analysis.

Phase 2 adds long-term behavioral profiles, Pearson correlation tracking between metric pairs, session-level behavioral fingerprinting, and a Profile Divergence Score (PDS) that combines all signals into a single 0–100 behavioral health indicator.

Nothing in the application layer is inspected. Detection is based entirely on observable communication behavior.

---

## Hardware

| Device | Role | Address |
|---|---|---|
| ESP32-CAM (AI Thinker) | Simulated target camera | `tinyguard-cam.local` (DHCP) |
| ESP32 DevKit V1 | TinyGuard monitor | `tinyguard-monitor.local` (DHCP) |
| Hotspot | Network hub / gateway | Any subnet — DHCP assigns addresses |

Both devices use DHCP and advertise via mDNS. No static IPs. Works on any hotspot
(Windows, Fedora, Android) without reflashing when the network changes.

---

## Repository Layout

```
tinyguard/
│
├── firmware/
│   ├── esp32cam/                  # Arduino IDE — AI Thinker ESP32-CAM
│   │   ├── main.ino
│   │   ├── wifi_manager.h / .cpp
│   │   ├── camera_server.h / .cpp
│   │   ├── telemetry_collector.h / .cpp
│   │   └── heartbeat_service.h / .cpp
│   │
│   └── monitor/                   # ESP-IDF v5.x — ESP32 DevKit V1
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults
│       └── main/
│           ├── CMakeLists.txt
│           ├── main.c
│           ├── wifi_manager.h / .c
│           ├── device_state.h / .c
│           ├── udp_receiver.h / .c
│           ├── stats_engine.h / .c
│           ├── anomaly_engine.h / .c
│           ├── alert_manager.h / .c
│           ├── dashboard_server.h / .c
│           ├── behavior_profile.h / .c      # Phase 2
│           ├── correlation_tracker.h / .c   # Phase 2
│           ├── session_tracker.h / .c       # Phase 2
│           └── fingerprint_engine.h / .c    # Phase 2
│
├── scripts/
│   └── validate.py                # Laptop-side injection tool for validation
│
├── datasets/                      # Captured behavioral logs (gitignored)
├── devlog.md                      # Development journal
└── README.md
```

---

## Quick Start

### Camera — Arduino IDE

1. Open `firmware/esp32cam/` in Arduino IDE
2. Install board package: **`esp32` by Espressif** (Tools → Board → Boards Manager)
   > ⚠️ **Use only the official Espressif `esp32` package.** Third-party board managers (e.g. ESP32-BluePad32) use a different WiFi/lwIP stack and cause unpredictable stream failures and socket corruption.
3. Select board: `AI Thinker ESP32-CAM`
4. Set `SSID` / `PASSWORD` in `wifi_manager.cpp`
5. Flash and verify — Serial Monitor shows:
   ```
   [Boot] Reset: power-on
   [Camera] View : http://<DHCP-IP>/view
   [Camera] mDNS : http://tinyguard-cam.local/view
   [TinyGuard-CAM] View: http://<DHCP-IP>/view
   ```
   Open `/view` in any browser. `/stream` is still registered internally (used by the `/view` page) but not the intended entry point.

### Monitor — ESP-IDF

```bash
cd firmware/monitor
# Set WIFI_SSID and WIFI_PASSWORD in main/wifi_manager.c first
idf.py set-target esp32
idf.py build && idf.py flash monitor
```

Confirm on serial:
```
I (xxx) WiFi-Monitor: Connected. IP: <DHCP-IP>  (DHCP)
I (xxx) Dashboard: Dashboard : http://<DHCP-IP>/
I (xxx) Dashboard: mDNS also : http://tinyguard-monitor.local/
```
Use the IP URL directly. The `.local` hostname works on Windows and macOS; on Fedora/Linux it requires `avahi-daemon` to be running.

> **CMakeLists.txt:** ensure `main/CMakeLists.txt` lists all Phase 2 source files in `SRCS`:
> `behavior_profile.c`, `correlation_tracker.c`, `session_tracker.c`, `fingerprint_engine.c`.
> Missing entries cause linker errors that look like undefined references to `_init` functions.
>
> **mDNS component:** run `idf.py add-dependency "espressif/mdns"` from `firmware/monitor/` before building. mDNS was moved out of ESP-IDF core in v5.x and must be fetched via the component manager.
>
> **PRIV_REQUIRES:** `main/CMakeLists.txt` must declare `esp_wifi nvs_flash esp_netif esp_event esp_driver_gpio esp_http_server lwip` under `PRIV_REQUIRES`. See the Quick Start CMakeLists snippet below.

Open browser at the IP shown in serial — dashboard loads immediately.

> **One USB cable?** Flash monitor, confirm WiFi on serial, then unplug. Power camera separately. GPIO2 LED blinks every ~10s while heartbeats are arriving.

---

## Validation

After the learning phase completes (5 minutes), run attack scenarios from your laptop:

```bash
# Phase 1 scenarios (work immediately after learning phase)
python3 scripts/validate.py rssi
python3 scripts/validate.py reconnect
python3 scripts/validate.py stream
python3 scripts/validate.py heartbeat_stop
python3 scripts/validate.py all

# Phase 2 correlation scenarios
# Require ~23 min post-learning for EMA variance to mature
# Inject sustained broken metric-pair relationships

# Phase 2 fingerprint scenarios
# Require behavior_profile.all_ready (~20 min post-learning)
# Best triggered by combining multiple anomalies simultaneously
# Single-signal injection typically produces sub-threshold PDS
```

Watch `http://tinyguard-monitor.local/` during runs. Alerts appear in real-time on the dashboard.

> The script bypasses the camera entirely — sends crafted UDP packets directly to the monitor. Each scenario is reproducible and deterministic.

---

## Dashboard

Served by the monitor at `http://tinyguard-monitor.local/`. Polls `/status` every 10 seconds via JavaScript fetch — no page refresh.

### Panels

**Device Health**
Status badge (OFFLINE / LEARNING / MONITORING / TIMEOUT), PDS badge, last heartbeat age, camera uptime, RSSI, stream state, viewers, reconnects, packet count.

**Behavioral Fingerprint**
Profile Divergence Score (0–100) with colour-coded bar. Component breakdown table showing sub-score and maximum z-score for each of the three PDS components (Metric Drift, Correlation Drift, Session Drift) with readiness indicators. Hidden until `behavior_profile.all_ready`.

**Correlation Tracking**
Table showing all three correlation pairs: current Pearson r, EMA baseline r, z-score, alert indicator. Shows warming-up state for pairs not yet ready.

**Session Behavior**
Sessions completed, currently-streaming indicator, session starts in the rolling window, average session duration ± stddev, average inter-session interval ± stddev. Hidden until 5 sessions completed.

**Rolling Statistics**
Mean and stddev for RSSI and HB interval with live sparklines. Two additional sparklines show RSSI Stability (rolling stddev over time — leading indicator for link instability) and PDS History (Profile Divergence Score trend over last 60 polls). Sparklines accumulate client-side from `/status` polling — no additional storage on the ESP32.

**Alert Timeline**
Last 8 alerts, most recent first. Each alert shows level badge, source category (`[METRIC]`, `[CORR]`, `[FP]`, `[WATCH]`), and message.

### JSON Endpoint

`http://tinyguard-monitor.local/status` returns a complete JSON snapshot:

```json
{
  "status": "MONITORING",
  "rssi": -43,
  "uptime_ms": 123456,
  "last_rx_age_ms": 4200,
  "stream_active": false,
  "viewer_count": 0,
  "reconnects": 0,
  "packet_count": 847,
  "learning": false,
  "rssi_mean": -42.8,
  "rssi_stddev": 1.2,
  "fingerprint": {
    "pds": 12,
    "ready": true,
    "alert_elevated": false,
    "alert_critical": false,
    "metric_drift":      { "sub_score": 8,  "max_zscore": 0.48, "ready": true },
    "correlation_drift": { "sub_score": 15, "max_zscore": 0.90, "ready": true },
    "session_drift":     { "sub_score": 5,  "max_zscore": 0.30, "ready": true }
  },
  "correlation": {
    "ready": true,
    "pairs": [
      { "id": 0, "ready": true, "r": -0.82, "ema_r": -0.79, "zscore": -0.41, "alert": false },
      { "id": 1, "ready": true, "r": -0.71, "ema_r": -0.68, "zscore": -0.28, "alert": false },
      { "id": 2, "ready": true, "r":  0.05, "ema_r":  0.04, "zscore":  0.12, "alert": false }
    ]
  },
  "session": {
    "ready": true,
    "sessions_completed": 14,
    "in_session": false,
    "session_count_in_window": 1,
    "ema_duration_ms": 312000,
    "ema_duration_stddev": 45000,
    "ema_interval_ms": 3600000,
    "ema_interval_stddev": 480000
  },
  "alert_count": 3,
  "alerts": [...]
}
```

---

## Heartbeat Packet

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

Transport: UDP · Port: 5000 · Interval: 10s

---

## Detection Parameters

### Phase 1 — Per-Metric Anomaly

| Parameter | Value |
|---|---|
| Z-score threshold | 3.0 |
| Consecutive samples to alert | 3 |
| Minimum stddev (below = suppressed) | 0.5 |
| Heartbeat timeout | 30s |
| Learning phase duration | 300s |
| Rolling window size | 60 samples (~10 min) |

### Phase 2 — Correlation Tracking

| Parameter | Value |
|---|---|
| Minimum samples per pair | 30 |
| EMA alpha | 0.005 |
| EMA half-life | ~139 samples (~23 min) |
| Z-score threshold | 3.0 |
| Variance floor | 0.0025 |
| Consecutive samples to alert/clear | 3 |

### Phase 2 — Session Tracking

| Parameter | Value |
|---|---|
| EMA alpha | 0.1 |
| EMA half-life | ~7 sessions |
| Sessions required before ready | 5 |
| Minimum session duration (noise floor) | 2000 ms |
| Session count window | 60 heartbeats (~10 min) |

### Phase 2 — Profile Divergence Score

| Parameter | Value |
|---|---|
| Metric Drift weight | 40% |
| Correlation Drift weight | 35% |
| Session Drift weight | 25% |
| Z-scale (z=6 → sub-score 100) | 6.0 |
| Elevated threshold | PDS ≥ 25 |
| Critical threshold | PDS ≥ 75 |
| Consecutive evaluations to alert/clear | 3 |

---

## Alert Types

| Level | Type | Trigger |
|---|---|---|
| INFO | `DEVICE_CONNECTED` | Camera reconnects after absence |
| INFO | `LEARNING_COMPLETE` | Phase 1 baseline established |
| WARNING | `RSSI_ANOMALY` | RSSI z-score > 3.0 for 3 consecutive samples |
| WARNING | `HB_INTERVAL_ANOMALY` | Heartbeat timing z-score > 3.0 |
| WARNING | `RECONNECT_ANOMALY` | Reconnect rate z-score > 3.0 |
| WARNING | `STREAM_ANOMALY` | Viewer count z-score > 3.0 |
| WARNING | `CORRELATION_ANOMALY` | Pearson r z-score > 3.0 for a pair |
| WARNING | `PDS_ELEVATED` | PDS ≥ 25 for 3 consecutive evaluations |
| CRITICAL | `HEARTBEAT_MISSING` | No heartbeat for 30s (watchdog path) |
| CRITICAL | `PDS_CRITICAL` | PDS ≥ 75 for 3 consecutive evaluations |

Note: `HEARTBEAT_MISSING` and `HB_INTERVAL_ANOMALY` are distinct. `HEARTBEAT_MISSING` fires when packets stop arriving entirely (watchdog task). `HB_INTERVAL_ANOMALY` fires when packets arrive but inter-arrival timing is statistically anomalous (z-score path).

---

## Phase 2 Architecture

### Module Dependency Order

```
stats_engine          ← Phase 1 foundation
    │
    ├── anomaly_engine         (Phase 1 detection)
    │
    └── behavior_profile       (Phase 2 — long-term EMA baselines)
            │
            ├── correlation_tracker    (Phase 2 — Pearson r between pairs)
            │
            └── session_tracker        (Phase 2 — stream session fingerprint)
                        │
                        └── fingerprint_engine   (Phase 2 — PDS terminal consumer)
                                    │
                                    └── dashboard_server   (display)
```

### Call Order in udp_receiver (per heartbeat)

```
stats_engine_update()
stats_engine_get_snapshot()          ← one snapshot, shared downstream
anomaly_engine_heartbeat_received()
anomaly_engine_evaluate()
[learning→active edge]
  behavior_profile_seed()            ← called once only
behavior_profile_update()
behavior_profile_get_snapshot()      ← one snapshot, shared downstream
correlation_tracker_update()
session_tracker_update()
fingerprint_engine_update()          ← terminal consumer, pulls own snapshots
```

### Memory Summary

| Module | Static RAM |
|---|---|
| stats_engine | ~3840 bytes (4 × 60-sample float buffers) |
| anomaly_engine | ~48 bytes |
| alert_manager | ~3200 bytes (32 × alert_t) |
| behavior_profile | ~80 bytes |
| correlation_tracker | ~1550 bytes |
| session_tracker | ~56 bytes |
| fingerprint_engine | ~48 bytes |
| **Phase 2 total** | **~1734 bytes** |

No heap allocation in any module. No dynamic containers.

### Snapshot-Sharing Pattern

`udp_receiver` acquires snapshots once per heartbeat and distributes by pointer. No module calls `stats_engine_get_snapshot()` or `behavior_profile_get_snapshot()` independently mid-pipeline. Exception: `fingerprint_engine_update()` is the terminal consumer and calls `stats_engine_get_snapshot()` internally — acceptable since it runs after all pipeline stages have committed.

---

## Status

Phase 1 (statistical anomaly detection) and Phase 2 (behavioral fingerprinting) complete and validated on hardware. Camera stream architecture overhauled: non-blocking per-client streamer tasks, mutex-protected viewer state, deferred reconnect, HMIRROR+VFLIP for inverted mount, `/view` as the sole browser entry point (HTML-wrapped MJPEG, smoother than raw stream on all browsers), brownout logging and threshold adjustment, BSD socket heartbeat to eliminate lwIP race with TCP stream. Both nodes use DHCP with mDNS hostnames (`tinyguard-cam.local`, `tinyguard-monitor.local`). Dashboard sparklines updated: RSSI Stability and PDS History replace reconnects/hr and viewers. Phase 3 (STM32 DSP spectral analysis) planned. See `devlog.md` for full engineering history.
