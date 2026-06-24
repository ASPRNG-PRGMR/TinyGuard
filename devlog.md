# TinyGuard — Development Log

---

## Milestone 0 — Environment Setup

### What was done
- Created repository structure
- `firmware/esp32cam/` — Arduino IDE project, board: AI Thinker ESP32-CAM, package: `esp32` by Espressif
- `firmware/monitor/` — ESP-IDF v5.x project with `CMakeLists.txt` at root and component level, `sdkconfig.defaults` tuning WiFi/LWIP/FreeRTOS
- Monitor `main.c` is a deliberate build-verification stub — no logic yet

### Verified
- Arduino IDE compiles esp32cam project without errors
- `idf.py build` succeeds for monitor project
- Both boards print startup messages over serial after flashing

---

## Milestone 1 — Camera Node

### What was built

**`wifi_manager`**
- Connects to laptop hotspot (static IP `192.168.137.10`)
- WiFi event handler: logs disconnect, increments reconnect counter, calls `WiFi.reconnect()`
- Blocks in `wifi_manager_init()` until connected (20s timeout → reboot)

**`camera_server`**
- Initialises OV2640; uses PSRAM if available (VGA/quality 10), else QVGA/quality 12
- HTTP server on port 80: `GET /` status page, `GET /stream` MJPEG multipart
- Viewer count tracked on connect/disconnect
- HTTP server pinned to FreeRTOS core 1 so `loop()` is never blocked

**`telemetry_collector`**
- Aggregates RSSI, uptime, stream state, viewer count, reconnect count into `telemetry_t`

**`heartbeat_service`**
- Non-blocking tick called from `loop()`
- Sends UDP JSON to `192.168.137.20:5000` every 10,000ms
- Fields: `device`, `uptime`, `rssi`, `stream_active`, `viewer_count`, `reconnects`, `timestamp`

### Verified
- `[WiFi] Connected. IP: 192.168.137.10` on serial
- `http://192.168.137.10/stream` serves live MJPEG
- Heartbeat JSON visible in `nc -ul 5000` every ~10s

### Known limitations
- `timestamp` is uptime seconds, not wall-clock (no NTP)
- Stream handler is synchronous — one viewer at a time; sufficient for MVP

---

## Milestone 2 — Monitor Node

### What was built

**`wifi_manager`** (ESP-IDF)
- Static IP `192.168.137.20`, stops DHCP, sets IP via `esp_netif_set_ip_info()`
- Blocks until connected (20s timeout → restart)

**`device_state`**
- Mutex-protected struct holding latest camera snapshot
- Fields: `valid`, `uptime_ms`, `rssi`, `stream_active`, `viewer_count`, `reconnects`, `timestamp`, `last_rx_tick`, `packet_count`
- Safe to read from any task via `device_state_get()`

**`udp_receiver`**
- Binds `SOCK_DGRAM` on `0.0.0.0:5000`
- `recvfrom()` loop in dedicated FreeRTOS task (4KB stack, priority 5)
- JSON parsed with `strstr()` + `strtol()` — no heap allocation, no library
- Blinks GPIO2 LED 80ms per heartbeat — visual confirmation without a second UART

### Verified
- `[WiFi-Monitor] Connected. IP: 192.168.137.20` on serial
- Heartbeat summaries printed every 10s
- GPIO2 LED blinks on receipt

### Errors and fixes

**`IPSTR` inside `ESP_LOGI` — parse error**
- `IPSTR` expands to `"%d.%d.%d.%d"`, which breaks `LOG_FORMAT` macro expansion
- Fix: `inet_ntoa()` into `char[16]`, pass as `%s`

**`%lu` for `uint32_t` — hard error under `-Werror=format=`**
- On Xtensa/ESP32, `uint32_t` is `unsigned int` not `unsigned long`
- Fix: `PRIu32` from `<inttypes.h>` throughout

---

## Milestone 3 — Statistics Engine

### What was built

**`stats_engine`**

Four rolling-window metrics:
- RSSI (dBm)
- Heartbeat interval (ms between arrivals)
- Reconnect rate (reconnects/hour, derived from cumulative counter delta)
- Stream viewer count

Algorithm: Welford's online method for mean and variance. Numerically stable — avoids catastrophic cancellation from naive sum/count. On buffer eviction (window = 60 samples), recomputes Welford accumulators over the window in O(60).

Learning phase: 300s from first heartbeat. Detection suppressed. Logs baseline mean/stddev on completion.

Reconnect rate: `rate = (delta_reconnects / elapsed_s) * 3600`

### Verified
- `[LEARNING]` countdown visible in 30s serial summary
- `Learning complete` log with baseline values after 5 minutes
- `[ACTIVE]` summary shows stable mean/stddev under normal conditions

---

## Milestone 4 — Detection Engine

### What was built

**`alert_manager`**
- Circular buffer of 32 alerts, no heap
- Three levels: INFO, WARNING, CRITICAL
- Each alert stores: timestamp, level, type, message, observed value, mean, stddev, z-score
- Logs immediately via `ESP_LOGI` / `ESP_LOGW` / `ESP_LOGE`
- `alert_manager_get_recent()` returns N most recent — feeds dashboard in M5

**`anomaly_engine`**

Z-score: `z = (observed - mean) / stddev`

Three layers of false-positive suppression:
- Suppressed when `stddev < 0.5` (flat metric → meaningless z-scores)
- Suppressed during learning phase
- Alert fires only after 3 consecutive anomalous samples
- Cooldown: once an alert fires, further alerts suppressed until 3 consecutive normal samples

Heartbeat watchdog: separate FreeRTOS task (2KB stack, priority 3) polling every 1s. Raises CRITICAL after 30s silence. Raises INFO when heartbeat resumes. Designed as a separate task because the receive path only runs when packets arrive — it cannot detect absence.

Metrics evaluated:
- RSSI → WARNING
- Heartbeat interval → WARNING
- Reconnect rate → WARNING
- Viewer count → WARNING
- Heartbeat timeout → CRITICAL

### Verified
- `alerts: 0` in 30s summary under normal conditions after learning
- CRITICAL `HEARTBEAT_MISSING` fires within 30s of unplugging camera
- INFO resume alert fires when camera reconnects
- RSSI anomaly fires after moving camera to edge of WiFi range (3 consecutive bad samples)

---

## Milestone 5 — Dashboard

### What was built

**`dashboard_server`**
- ESP-IDF `esp_http_server` (httpd) — not a raw socket; handles connection lifecycle internally
- Two routes:
  - `GET /` — self-contained HTML dashboard, no external assets, mobile-responsive, auto-refreshes every 2s via `<meta http-equiv='refresh'>`
  - `GET /status` — JSON snapshot of all state for scripting / curl

Dashboard displays:
- Status badge: OFFLINE / LEARNING / MONITORING / TIMEOUT
- Last heartbeat age, camera uptime, RSSI, stream state, viewers, reconnects, packet count
- Statistics card: learning state, sample count, mean/stddev for all four metrics
- Alert history: last 8 alerts, most recent first, colour-coded by level (blue/orange/red)

HTML is built into a 4KB stack buffer per request. No SPIFFS, no filesystem, no external CDN. Renders correctly on mobile. Dark theme.

JSON endpoint fields mirror the heartbeat schema where applicable, plus stats and alert array.

### Verified by
- Open `http://192.168.137.20/` in browser — dashboard loads
- Status badge changes to LEARNING then MONITORING after 5 minutes
- Unplug camera — status changes to TIMEOUT, CRITICAL alert appears in history
- `curl http://192.168.137.20/status` returns valid JSON
- Page auto-refreshes every 2s without manual reload

---

## Milestone 6 — Validation

### Approach

Rather than relying on physically manipulating hardware for each scenario (moving the camera, forcing WiFi drops, opening streams), a laptop-side Python injection script was written: `scripts/validate.py`.

It sends crafted UDP packets directly to the monitor over the hotspot, bypassing the camera entirely. This makes each scenario reproducible and deterministic — no signal variability, no race conditions, no hardware dependencies during test.

The script requires no external packages (stdlib only) and runs on any Python 3 installation connected to the hotspot.

### Scenarios

**Normal baseline** (`validate.py normal`)
- Sends 30 packets with stable values: RSSI=-42, viewers=0, reconnects=0
- Expected: no alerts, `[ACTIVE] alerts: 0` in serial and dashboard
- Confirms no false positives under steady-state conditions

**RSSI anomaly** (`validate.py rssi`)
- 10 normal packets then 5 packets with RSSI=-95 dBm
- Expected z ≈ (-95 − (−42)) / 1.2 ≈ −44, far above threshold of 3.0
- Expected: WARNING `RSSI_ANOMALY` after 3rd consecutive anomalous packet

**Reconnect anomaly** (`validate.py reconnect`)
- 10 normal packets then 5 packets incrementing reconnect counter by +20 per packet
- Injected rate ≈ 7200 reconnects/hr vs normal baseline of ~0
- Expected: WARNING `RECONNECT_ANOMALY` after 3 consecutive high-rate packets

**Stream anomaly** (`validate.py stream`)
- 10 normal packets (0 viewers) then 5 packets with viewer_count=50
- Expected: WARNING `STREAM_ANOMALY` after 3 consecutive viewer spikes

**Heartbeat timeout** (`validate.py heartbeat_stop`)
- 5 normal packets, then 35s silence, then resume
- Expected: CRITICAL `HEARTBEAT_MISSING` within 30s of last packet
- Expected: INFO `DEVICE_CONNECTED` on resume

**Full suite** (`validate.py all`)
- Runs all scenarios in sequence with 10s pauses between them
- Provides a complete end-to-end validation run

### Notes

- All scenarios must be run **after** the learning phase completes (5 minutes from first heartbeat). The script does not enforce this — running early will produce no alerts by design.
- The consecutive-sample guard means each WARNING scenario requires at least 3 anomalous packets before alerting. The scripts inject 5 anomalous packets to give clear confirmation.
- The `heartbeat_stop` scenario verifies both the watchdog timeout path and the recovery path.
- Results are visible in real-time on the dashboard at `http://192.168.137.20/` and in the monitor serial output.

### Outcome

All six detection scenarios confirmed functional. MVP complete.

---

## Phase 1 Code Review — Corrections Applied Before Phase 2

A review of the monitor firmware prior to Phase 2 development identified three issues. All were corrected before Phase 2 work began.

### Issue 1 — Alert Type Clash: HEARTBEAT_MISSING

`ALERT_TYPE_HEARTBEAT_MISSING` was being used for two distinct conditions:

- The watchdog timeout path: no heartbeat received for 30s (absence of packets)
- The z-score path: heartbeat intervals are statistically anomalous (packets arrive but timing is wrong)

These are different failure modes. Using one alert type for both made it impossible to distinguish a dead camera from a camera with irregular timing.

**Resolution:** Added `ALERT_TYPE_HB_INTERVAL_ANOMALY` (WARNING level) for the z-score path. `ALERT_TYPE_HEARTBEAT_MISSING` (CRITICAL) is now exclusively the watchdog timeout. `anomaly_engine.c` updated accordingly.

### Issue 2 — README / Implementation Mismatch: LEARNING_COMPLETE

The README specified that `LEARNING_COMPLETE` should produce an INFO alert. The implementation only logged it via `ESP_LOGI` without going through `alert_manager`.

**Resolution:** `stats_engine.c` now raises `ALERT_TYPE_LEARNING_COMPLETE` through `alert_manager` when the learning phase ends. The alert appears in the dashboard alert history and is retrievable via the JSON endpoint.

### Issue 3 — Snapshot-Sharing Architecture

Phase 2 modules would need access to `stats_snapshot_t` on every heartbeat. The naive approach — each module calling `stats_engine_get_snapshot()` independently — would acquire the stats mutex multiple times per heartbeat with no benefit.

**Resolution:** A shared snapshot architecture was adopted. `udp_receiver.c` acquires one `stats_snapshot_t` per heartbeat and passes it by pointer to all consumers. This is the standard integration pattern for all Phase 2 modules. `behavior_profile_update()` and `correlation_tracker_update()` both receive the snapshot as a parameter and never call `stats_engine_get_snapshot()` internally.

---

## Milestone 7 — Long-Term Behavioral Profiles

### Objective

Phase 1 statistics use a 60-sample rolling window (~10 minutes). This catches acute deviations but cannot distinguish a genuine behavioral shift from a transient event. Phase 2 behavioral profiles add a second layer of learned baselines with a much longer time constant.

### What was built

**`behavior_profile`**

- EMA mean and variance per metric (RSSI, HB interval, reconnect rate, viewer count)
- Alpha = 0.005, giving half-life ≈ 139 samples ≈ 23 minutes
- `all_ready` flag: true when all four EMA variances have accumulated enough samples to be meaningful
- `behavior_profile_get_snapshot()` returns a mutex-protected copy

**Seeding design:**

The original EMA design zero-initialized means. Simulation showed severe warmup bias:
- RSSI ≈ −55 dBm, but EMA mean starts at 0
- Hundreds of samples required before the mean converged to a usable value
- During warmup, z-score-like comparisons against the EMA baseline would produce large false deviations

**Resolution:** `behavior_profile_seed()` was introduced. When Phase 1 learning completes, `udp_receiver` detects the `learning → ready` edge and calls `behavior_profile_seed()` with the Phase 1 baseline means. EMA means are seeded directly from the settled Phase 1 values. EMA variances are intentionally not seeded — they start from zero and accumulate naturally, preventing false confidence in an untested variance estimate.

### Integration changes

**`udp_receiver.c`:**
- Added `learning → ready` edge detection (comparing previous and current `snap->learning`)
- Calls `behavior_profile_seed()` on the edge
- Calls `behavior_profile_update()` on every heartbeat after that

**`main.c`:**
- Added `behavior_profile_init()`

**`main/CMakeLists.txt`:**
- Added `behavior_profile.c`

### Design decisions

- EMA alpha matches the value used in correlation_tracker for consistency across Phase 2 modules
- `all_ready` requires all four channels to be mature before any Phase 2 consumer trusts the profile — prevents partial readiness from producing misleading inputs to downstream modules

### Memory impact

Approximately 80 bytes of static RAM (4 EMA channels × ~20 bytes each). No heap allocation.

### Known limitations

- Variance warmup still takes several hours at 10s/sample before `all_ready` triggers. This is by design — immature variance produces unreliable detection. The system is simply silent during this period, which is preferable to false alerts.

---

## Milestone 8 — Correlation Tracking

### Objective

Individual metric anomaly detection catches a metric going out of range. It does not catch cases where metrics move together in unexpected ways or fail to move together when they should. Correlation tracking addresses this: it learns the expected Pearson r between metric pairs and alerts when the relationship changes significantly.

### What was built

**`correlation_tracker`**

- Rolling Pearson r computed over `STATS_WINDOW_SIZE` (60) samples per pair
- EMA baseline for r with alpha = 0.005 — same as behavioral profiles
- EMA r baseline seeded from the first valid Pearson r (avoids zero-start bias, matching the behavior_profile seeding pattern)
- Z-score over (current_r − ema_r) with a variance floor to prevent false positives when r has been extremely stable
- Per-pair alert state with 3-consecutive-sample guard and cooldown, mirroring `anomaly_engine` exactly

### Correlation pairs selected

Three pairs were selected based on physical meaningfulness — a real causal relationship must exist for the correlation baseline to be stable and the anomaly to be interpretable.

**RSSI ↔ reconnect_rate:** Signal degradation correlates with reconnect frequency under normal operation. A break in this relationship indicates either spoofed RSSI metrics or an attack forcing reconnects without affecting signal.

**RSSI ↔ heartbeat_interval:** Weak signal causes transmission stress visible in inter-packet timing irregularity. RSSI degrading without interval change suggests the camera is not the source of the signal measurements being reported.

**stream_active ↔ viewer_count:** An active stream should correlate with viewer presence. The inverse — stream active with no viewers, or viewers reported with no active stream — indicates unauthorized stream access with suppressed viewer reporting, or viewer count injection.

### Rejected pair

**reconnect_rate ↔ viewer_count:** No physical causal relationship exists. The expected correlation baseline converges toward noise. Including this pair would increase false-positive risk without providing detection value. Excluded.

### stream_active implementation correction

The initial implementation of the `stream_active ↔ viewer_count` pair contained a critical flaw: `stream_active` was derived as `(viewer_count > 0) ? 1 : 0` rather than read from the heartbeat packet directly.

This created a mathematical self-correlation: a binarized version of viewer_count was being correlated against raw viewer_count. The result was a near-constant r close to 1.0 under normal conditions, and near-inability to detect the primary threat case: `stream_active=1, viewer_count=0`.

The fix was a one-line signature change. `correlation_tracker_update()` now accepts `bool stream_active` as a third parameter. `udp_receiver.c` passes the value parsed directly from the heartbeat JSON. The pair correctly measures the relationship between two independent signals.

The degenerate-window guard (suppressing output when either variable has near-zero variance in the window) correctly handles the case where `stream_active` is constant over the window — this is not a bug, it means there is no relationship to measure.

### Readiness model

The initial header documentation stated: "No correlation value is emitted until all three readiness layers are satisfied." The implementation was per-pair: each pair emits alerts independently once its own buffer is ready, without waiting for other pairs.

The per-pair model is correct. There is no technical reason to delay a ready pair because another pair is still accumulating samples — all three pairs receive the same heartbeat stream and will reach readiness within one or two heartbeats of each other in practice. The documentation was corrected to match the implementation.

`correlation_snapshot_t.ready` (the global flag) remains the AND of all pairs — for consumers like `fingerprint_engine` that need the full picture, the global flag provides the right gate.

Per-pair `ready` additionally requires `baseline_seeded == true` (the first valid Pearson r has been computed and used to seed the EMA). A pair whose buffer has 30+ samples but has only seen degenerate windows (zero variance in x or y) will not be marked ready. This is the correct behavior — a pair that has never produced a valid r has no baseline to compare against.

### Additional implementation fixes applied

**Pointer arithmetic removed from `pair_add_sample`:** The original log used `p - s_state.pairs` to derive the pair index. This is technically valid but fragile. `pair_add_sample` was updated to accept `corr_pair_id_t id` explicitly. All call sites updated.

**EMA update order corrected:** In the original, `current_r` was written before `ema_r_update()` was called, meaning `current_r` and `ema_r` could briefly be inconsistent within the mutex. The corrected order calls `ema_r_update()` first, then writes `current_r`, then calls `check_pair_alert()`. All three reads from the same pair state are now consistent.

### Memory impact

Approximately 1542 bytes of static RAM (3 pairs × ~514 bytes each). Plus mutex handle (~8 bytes). No heap allocation.

Total Phase 2 addition (behavior_profile + correlation_tracker): ~1638 bytes.

### Integration

- `correlation_tracker_init()` called from `app_main()` after `behavior_profile_init()`
- `correlation_tracker_update()` called from `udp_receiver` after `behavior_profile_update()`
- `correlation_tracker_get_snapshot()` called by `dashboard_server` for the correlation panel

### Validation

Phase 2 correlation scenarios require sustained anomalous conditions to overcome the 3-consecutive-sample guard and the EMA variance warmup. The EMA needs to be mature (half-life ~23 min) before z-scores are reliable. Injecting anomalous pairs before `all_ready` is a no-op by design.

Useful validation scenarios:
- Inject RSSI stable / reconnect_rate spike → should break `RSSI<>Reconnect` correlation
- Inject `stream_active=1` with `viewer_count=0` sustained → should break `Stream<>Viewers` correlation
- Normal steady-state → no `CORRELATION_ANOMALY` alerts

### Known limitations

- The correlation baseline takes several hours to fully mature. During this period the system is silent on correlation anomalies. No mitigation planned — premature alerting is worse than delayed alerting.
- The Pearson loop is O(60) per pair per heartbeat, giving O(180) float operations every 10 seconds. On ESP32 with hardware FPU this is negligible.
- `stream_active` is binary (0 or 1). When the stream is rarely toggled, the window may often be degenerate and output suppressed. In a camera that streams continuously or never streams, this pair will rarely produce alerts. This is a fundamental constraint of the detection approach, not an implementation issue.

### Future considerations

- Phase 3 (STM32 DSP) may add frequency-domain features from the HB interval time series, potentially feeding a fourth correlation pair.
- A lightweight struct parameter replacing the three individual parameters to `correlation_tracker_update()` would clean up the call site if more raw heartbeat fields are needed in the future.
