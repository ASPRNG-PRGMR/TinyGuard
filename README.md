# TinyGuard

Lightweight IoT camera behavior monitoring using communication metadata — no packet inspection, no decryption.

Runs on two ESP32 boards. One simulates a home WiFi camera. The other monitors its behavior and detects anomalies using statistical profiling and behavioral fingerprinting.

---

## How It Works

The camera sends a UDP heartbeat every 10 seconds containing RSSI, uptime, stream state, viewer count, and reconnect count. The monitor builds rolling statistical baselines for each metric during a 5-minute learning phase, then detects deviations using z-score analysis. Phase 2 adds long-term behavioral profiles and Pearson correlation tracking between metric pairs, catching relationship-level anomalies that individual metric monitoring cannot see. Alerts are surfaced via serial and a live web dashboard.

Nothing in the application layer is inspected. Detection is based entirely on observable communication behavior.

---

## Hardware

| Device | Role | IP |
|---|---|---|
| ESP32-CAM (AI Thinker) | Simulated target camera | `192.168.137.10` |
| ESP32 DevKit V1 | TinyGuard monitor | `192.168.137.20` |
| Laptop hotspot | Network hub / gateway | `192.168.137.1` |

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
│           └── correlation_tracker.h / .c   # Phase 2
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
2. Install board package: `esp32` by Espressif
3. Select board: `AI Thinker ESP32-CAM`
4. Set `SSID` / `PASSWORD` in `wifi_manager.cpp`
5. Flash and verify:
   - Stream: `http://192.168.137.10/stream`
   - Heartbeats visible in Serial Monitor at 115200 baud

### Monitor — ESP-IDF

```bash
cd firmware/monitor
# Set WIFI_SSID and WIFI_PASSWORD in main/wifi_manager.c first
idf.py set-target esp32
idf.py build && idf.py flash monitor
```

Confirm on serial: `[WiFi-Monitor] Connected. IP: 192.168.137.20`

Open browser: `http://192.168.137.20/` — dashboard loads immediately.

> **One USB cable?** Flash monitor, confirm WiFi on serial, then unplug. Power camera separately. GPIO2 LED blinks every ~10s while heartbeats are arriving.

---

## Validation

After the learning phase completes (5 minutes), run attack scenarios from your laptop:

```bash
# Phase 1 scenarios
python3 scripts/validate.py rssi
python3 scripts/validate.py reconnect
python3 scripts/validate.py stream
python3 scripts/validate.py heartbeat_stop

# Full suite
python3 scripts/validate.py all
```

Phase 2 correlation scenarios require additional post-`all_ready` settling time (~23 minutes for EMA variance to mature). Inject sustained metric pairs that break the learned correlation relationships to trigger `CORRELATION_ANOMALY` alerts.

Watch `http://192.168.137.20/` during runs. Alerts appear in real-time.

> The script bypasses the physical camera entirely — it sends crafted UDP packets directly to the monitor. This lets you trigger each scenario deterministically without moving hardware.

---

## Dashboard

Served by the monitor at `http://192.168.137.20/`. Refreshes every 2 seconds.

- Device status: OFFLINE / LEARNING / MONITORING / TIMEOUT
- Last heartbeat age, camera uptime, RSSI, stream state, viewer count, reconnects
- Rolling statistics: mean and stddev per metric
- Alert history: last 8 alerts, colour-coded by level
- Correlation panel: per-pair Pearson r, EMA baseline, z-score, ready and alert state

JSON endpoint: `http://192.168.137.20/status`

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
| Minimum samples per pair before alerting | 30 |
| EMA alpha for r baseline | 0.005 |
| EMA half-life (approximate) | 139 samples (~23 min) |
| Z-score threshold | 3.0 |
| Variance floor (below = z-score suppressed) | 0.0025 |
| Consecutive samples to alert / clear | 3 |

---

## Alert Levels

| Level | Triggers |
|---|---|
| INFO | Device connected, learning complete (`LEARNING_COMPLETE`) |
| WARNING | RSSI anomaly, reconnect anomaly, stream anomaly, HB interval anomaly (`HB_INTERVAL_ANOMALY`), correlation anomaly (`CORRELATION_ANOMALY`) |
| CRITICAL | Missing heartbeat (`HEARTBEAT_MISSING`) |

Note: `HEARTBEAT_MISSING` and `HB_INTERVAL_ANOMALY` are distinct alert types. `HEARTBEAT_MISSING` fires when no packet is received for 30s (watchdog path). `HB_INTERVAL_ANOMALY` fires when packets arrive but their inter-arrival timing is statistically anomalous (z-score path).

---

## Phase 2 — Behavioral Fingerprinting

### Long-Term Behavioral Profiles (`behavior_profile`)

Phase 1 statistics use a 60-sample rolling window (~10 minutes). Behavioral profiles add a second layer: EMA means and variances with alpha=0.005, giving a half-life of ~23 minutes and a meaningful baseline after several hours of operation.

EMA means are seeded from the settled Phase 1 baseline when `learning_complete` fires, avoiding warmup bias. Variance is not seeded — it starts from zero and accumulates naturally. The profile is not considered ready (`all_ready=false`) until EMA variances have matured.

### Correlation Tracking (`correlation_tracker`)

Tracks Pearson r between three metric pairs selected for physical meaningfulness:

| Pair | What it detects |
|---|---|
| RSSI ↔ reconnect_rate | Signal-correlated reconnects decoupled — possible spoofed RSSI or forced reconnects |
| RSSI ↔ heartbeat_interval | Signal degradation without timing stress — camera may not be the signal source |
| stream_active ↔ viewer_count | Stream active with no viewers, or viewers reported with no active stream |

The `stream_active ↔ viewer_count` pair uses the raw `stream_active` boolean from the heartbeat packet, not a value derived from viewer count. This is the only way to detect `stream_active=1, viewer_count=0` — the primary threat case for unauthorized streaming with suppressed viewer reporting.

**Rejected pair:** `reconnect_rate ↔ viewer_count`. No physical relationship exists between viewer activity and reconnect behavior. The expected baseline converges toward noise, increasing false-positive risk without detection value.

### Readiness Model

Correlation output is gated on three layers:

1. Phase 1 learning complete (`snap->learning == false`)
2. Behavioral profile mature (`bp->all_ready == true`)
3. Per-pair: at least 30 samples in the rolling window and the first valid Pearson r has seeded the EMA baseline

Layers 1 and 2 are global — no pair accumulates samples until both are satisfied. Layer 3 is per-pair: each pair begins alerting independently when its own buffer is ready. Pairs do not wait for each other. `correlation_snapshot_t.ready` is true only when all pairs satisfy layer 3.

The RSSI ↔ heartbeat_interval pair will reach layer 3 slightly later than the other two pairs because it skips samples until a valid HB interval delta exists (requires at least 2 received heartbeats).

### Memory

Phase 2 adds approximately 1550 bytes of static RAM:

| Component | RAM |
|---|---|
| behavior_profile (4 EMA channels) | ~80 bytes |
| correlation_tracker (3 pairs × ~514 bytes) | ~1542 bytes |
| Mutex handles | ~16 bytes |
| **Total Phase 2 addition** | **~1638 bytes** |

No heap allocation. No dynamic containers.

### Snapshot Architecture

`udp_receiver` acquires one `stats_snapshot_t` and one `behavior_profile_snapshot_t` per heartbeat and passes them by pointer to all Phase 2 consumers. This avoids repeated mutex acquisitions inside each module and is the standard integration pattern for all future Phase 2 and Phase 3 modules.

---

## Status

Phase 1 (statistical anomaly detection) and Phase 2 (behavioral fingerprinting and correlation tracking) complete. Phase 3 (STM32 DSP analysis) planned. See `devlog.md` for full engineering history.
